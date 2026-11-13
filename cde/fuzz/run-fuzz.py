#!/usr/bin/env python3
"""Build and run the libFuzzer targets of the CK Core CDE parsers.

run-fuzz.py --list
run-fuzz.py --target NAME --seconds N --scratch DIR [--worktree DIR]

Every target fuzzes production parser code without the X11, Motif and
daemon environment it normally runs in.  Like the regression tests under
programs/*/tests, the runner cuts the parser functions out of the production
source text (the working tree of --worktree), writes them into a generated
header under DIR/build/NAME/, and compiles that header together with the
fuzz-NAME.c harness, which holds the small stand-ins for the surroundings.
The production files are only read, never changed.

The target is built with the Homebrew clang (environment variable CLANG,
default /opt/homebrew/opt/llvm/bin/clang) and
-fsanitize=fuzzer,address,undefined.  Undefined behaviour stops the run like
an address error does (-fno-sanitize-recover=undefined).  The corpus starts
from cde/fuzz/corpus/NAME/; the grown corpus, the logs and the crash inputs
stay below DIR.  The exit status is 1 when a crash, a timeout or an
out-of-memory input was found.

Only the first process of libFuzzer runs, on one CPU.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
DEFAULT_CLANG = "/opt/homebrew/opt/llvm/bin/clang"
# Headers of XQuartz: Xlib, Xt, Xdmcp.  No Motif is needed by any target.
DEFAULT_XINCLUDE = "/opt/X11/include"
DEFAULT_XLIB = "/opt/X11/lib"


# ---- extraction helpers ------------------------------------------------------

def function(text, name, source="source"):
    """Return the definition of the function NAME (not a prototype or a call):
    from its return type line to the closing brace in column 0.  The
    definition must be unique."""
    found = []
    for m in re.finditer(r"(?<![\w])" + re.escape(name) + r"[ \t]*\(", text):
        line_start = text.rfind("\n", 0, m.start()) + 1
        before = text[line_start:m.start()]
        if not re.fullmatch(r"[\w \t\*]*", before):
            continue                                   # a call or an expression
        if before.strip().startswith(("*", "/")):
            continue                                   # inside a comment
        if before.split() and before.split()[0] in ("return", "else", "case", "typedef"):
            continue
        brace = text.find("{", m.end())
        semi = text.find(";", m.end())
        if brace < 0 or (0 <= semi < brace):
            continue                                   # a prototype
        start = line_start
        if not before.strip():                         # the return type sits on the line above
            prev = text.rfind("\n", 0, start - 1) + 1
            above = text[prev:start - 1]
            if re.fullmatch(r"(?:static\s+)?[\w \t\*]+", above) and above.strip():
                start = prev
        found.append(start)
    found = sorted(set(found))
    if len(found) != 1:
        sys.exit(f"{source}: definition of {name} found {len(found)} times, expected 1")
    start = found[0]
    end = text.index("\n}", start)
    end = text.find("\n", end + 1)
    return text[start:end if end >= 0 else len(text)] + "\n"


def functions(text, names, source):
    return "\n".join(function(text, n, source) for n in names)


def defines(text, names, source="source"):
    """Return the #define lines (with backslash continuations) of NAMES."""
    out = []
    for name in names:
        m = re.search(r"^[ \t]*#[ \t]*define[ \t]+" + re.escape(name) + r"\b(?:\([^)]*\))?(?:.*\\\n)*.*$", text, re.M)
        if not m:
            sys.exit(f"{source}: #define {name} not found")
        line = m.group(0)
        line = re.sub(r"/\*(?:(?!\*/).)*$", "", line)    # a comment that continues on the next line
        out.append(line.rstrip())
    return "\n".join(out) + "\n"


def between(text, start, end, source="source"):
    if text.count(start) < 1:
        sys.exit(f"{source}: anchor {start!r} not found")
    first = text.index(start)
    return text[first:text.index(end, first)]


def typedef(text, pattern, source="source"):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit(f"{source}: typedef /{pattern}/ not found")
    return m.group(0) + "\n"


# ---- targets -----------------------------------------------------------------

def build_spc_proto(wt):
    base = wt / "cde"
    proto_c = (base / "lib/DtSvc/DtEncap/spc-proto.c").read_text()
    src = "spc-proto.c"
    spcp = (base / "include/SPC/spcP.h").read_text()
    protoh = (base / "include/SPC/spc-proto.h").read_text()
    spch = (base / "include/bms/spc.h").read_text()
    types = defines(spch, ["SPC_BUFSIZ"], "spc.h")
    types += defines(spcp, ["REQUEST_HEADER_LENGTH", "MAXREQLEN", "MAXPAYLOADLEN"], "spcP.h")
    types += defines(protoh, ["NREQS", "PDRP", "PDEND"], "spc-proto.h")
    types += typedef(spcp, r"typedef struct _buffered_data \{.*?\} buffered_data, \*buffered_data_ptr;", "spcP.h")
    types += typedef(protoh, r"typedef struct _prot_request \{.*?\} protocol_request, \*protocol_request_ptr;", "spc-proto.h")
    types += '#define NULL_STR     "\\001\\001\\001"\n#define EMPTY_STR    "\\001\\002\\003"\n'
    types += defines(proto_c, ["SPC_MAX_COUNTED_STRINGS", "SPC_MAX_MULTI_STRINGS"], src)
    prod = functions(proto_c, [
        "SPC_New_Buffered_Data_Ptr", "SPC_Reset_Protocol_Ptr", "SPC_New_Protocol_Ptr", "SPC_Free_Protocol_Ptr",
        "SPC_Free_Vector", "SPC_Parse_Hex_Field", "SPC_Parse_Header", "SPC_Scan_Decimal", "SPC_Reject_Request",
        "SPC_Read_Chars", "SPC_Read_Protocol", "SPC_Get_Multi_Packet",
        "sscan_counted_string", "sscan_application_data", "sscan_device_data", "sscan_logfile_data",
        "sscan_register_data", "SPC_Read_Termios_Request"], src)
    return {"types.h": types, "production.h": prod}, {}


def build_dtwm_session(wt):
    text = (wt / "cde/programs/dtwm/Session.c").read_text()
    src = "Session.c"
    start = text.index("static char * SESSION_FILE")
    prod = text[start:text.index(";", start) + 1] + "\n"
    prod += functions(text, ["SessionFileGetName", "SessionRestoreData"], src)
    return {"types.h": "", "production.h": prod}, {}


def build_dtsession_client(wt):
    root = wt / "cde/programs/dtsession"
    text = (root / "SmRestore.c").read_text()
    src = "SmRestore.c"
    prod = functions(text, ["PeekAhead", "GetSmartString", "ScanWhitespace", "GetNextLine", "EnsureBufferRoom",
                            "FillHintBuf", "FillCmdBuf", "FillRemoteBuf", "ParseClientFile"], src)
    return {"types.h": "", "production.h": prod}, {}


def build_dtsession_palette(wt):
    root = wt / "cde/programs/dtsession"
    text = (root / "SrvFile_io.c").read_text()
    header = (root / "Srv.h").read_text()
    src = "SrvFile_io.c"
    defs = text[text.index("#define BUFSIZE"):text.index("/* Static Function Declarations */")]
    defs += text[text.index("#define HOME"):text.index("#define DEFAULT_PALETTE")]
    defs += '#define DEFAULT_PALETTE    "Default.dp"\nvoid SwitchAItoPS(struct _palette *);\n'
    protos = text[text.index("/* Static Function Declarations */"):text.index("/* static variables */")]
    types = typedef(header, r"typedef struct _colorset \{.*?\} ColorSet;", "Srv.h")
    types += typedef(header, r"typedef struct _palette \{.*?\} palettes;", "Srv.h")
    types += "".join(re.findall(r"^#define (?:DYNAMIC|BLACK|WHITE|PALETTE_SUFFIX|B_O_W|W_ONLY|W_O_B|B_ONLY) .*\n",
                                header, re.M))
    prod = defs + protos + functions(text, [
        "GetPaletteDefinition", "FindPalette", "ReadPaletteFile", "ParsePaletteInfo", "InitializeBW",
        "SwitchAItoPS"], src)
    return {"types.h": types, "production.h": prod}, {
        "define": ['CDE_INSTALLATION_TOP="/nonexistent-cde"', 'CDE_CONFIGURATION_TOP="/nonexistent-cde-etc"']}


def build_dtlogin_xdmcp(wt):
    root = wt / "cde/programs/dtlogin"
    read = lambda name: (root / name).read_text()
    xdmcp, auth = read("xdmcp.c"), read("auth.c")
    # whole files of the display manager's policy layer, as check-xdmcp.py compiles them
    prod = "\n".join(read(name) for name in ("netaddr.c", "policy.c", "protodpy.c", "xdmauth.c"))
    prod += between(auth, "struct AuthProtocol {", "static Xauth *\nGenerateAuthorization", "auth.c")
    prod += between(auth, "#if NeedWidePrototypes\nvoid\nSetProtoDisplayAuthorization",
                    "\n}\n", "auth.c") + "\n}\n"
    # the request handlers of xdmcp.c
    prod += "\n".join(re.findall(r"^void (?:query|broadcast|forward|request)_respond \(.*;$|"
                                   r"^void (?:send_\w+|manage) \(.*;$", xdmcp, re.M)) + "\n"
    prod += "#define getString(name,len)\t((name = malloc (len + 1)) ? 1 : 0)\n#define pS(s)\t((s) ? ((char *) (s)) : \"empty string\")\n"
    prod += "static XdmcpBuffer\tbuffer;\nstatic ARRAY8\tHostname;\nstatic unsigned long\tglobalSessionID;\n"
    prod += "#define NextSessionID()    (++globalSessionID)\n"
    prod += "\n".join(re.findall(r"^static ARRAY8 (?:outOfMemory|noValidAddr|noValidAuth|noAuthentic) = .*;$", xdmcp, re.M)) + "\n"
    prod += functions(xdmcp, ["sendForward", "ClientAddress", "all_query_respond", "indirect_respond",
                              "direct_query_respond", "query_respond", "broadcast_respond", "forward_respond",
                              "send_willing", "send_unwilling", "request_respond", "send_accept", "send_decline",
                              "SameHost", "manage", "send_failed", "send_refuse", "send_alive",
                              "ProcessRequestSocket"], "xdmcp.c")
    return {"types.h": "", "production.h": prod}, {
        "define": ["UNIXCONN", "TCPCONN", "XDMCP", "HASXDMAUTH", "ANSICPP", "NeedWidePrototypes=0"],
        "include": [str(root), str(wt / "cde/include")],
        "link": ["-L" + os.environ.get("XLIB", DEFAULT_XLIB), "-lXdmcp", "-lXau",
                 "-Wl,-rpath," + os.environ.get("XLIB", DEFAULT_XLIB)]}


def build_dtwm_property(wt):
    root = wt / "cde/programs/dtwm"
    text = (root / "WmProperty.c").read_text()
    src = "WmProperty.c"
    prod = functions(text, ["GetNormalHints", "ProcessWmProtocols", "ProcessMwmMessages", "GetWMState",
                            "GetMwmHints", "GetMwmInfo", "ProcessWmColormapWindows", "GetUtf8String"], src)
    return {"types.h": "", "production.h": prod}, {"include": [str(root)]}


TARGETS = {
    "spc-proto": ("dtspcd request wire layer: header decode, request read, multi-packet and counted-string "
                  "decoders (DtEncap/spc-proto.c)", build_spc_proto),
    "dtwm-session": ("dtwm front panel session file reader (dtwm/Session.c)", build_dtwm_session),
    "dtsession-client": ("dtsession client file parser and quoting parser (dtsession/SmRestore.c)",
                         build_dtsession_client),
    "dtsession-palette": ("dtsession color server palette reader (dtsession/SrvFile_io.c)",
                          build_dtsession_palette),
    "dtlogin-xdmcp": ("dtlogin XDMCP request handlers over libXdmcp: query, forward, request, manage, keep-alive "
                      "(dtlogin/xdmcp.c with policy.c, protodpy.c, xdmauth.c, auth.c)", build_dtlogin_xdmcp),
    "dtwm-property": ("dtwm window property readers (dtwm/WmProperty.c)", build_dtwm_property),
}


# ---- build and run -------------------------------------------------------------

def find_worktree(given):
    if given:
        return Path(given).resolve()
    return HERE.parents[1]


def build(name, wt, scratch, clang, recover_ub=False):
    description, builder = TARGETS[name]
    gen, extra = builder(wt)
    gendir = scratch / "build" / name
    gendir.mkdir(parents=True, exist_ok=True)
    for fname, content in gen.items():
        (gendir / fname).write_text(content)
    binary = gendir / (f"fuzz-{name}-recover" if recover_ub else f"fuzz-{name}")
    xinclude = os.environ.get("XINCLUDE", DEFAULT_XINCLUDE)
    cmd = [clang, "-fsanitize=fuzzer,address,undefined",
           "-fsanitize-recover=undefined" if recover_ub else "-fno-sanitize-recover=undefined",
           "-fno-omit-frame-pointer", "-g", "-O1", "-std=gnu23", "-w",
           "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE",
           f'-DFUZZ_TYPES_H="{gendir / "types.h"}"', f'-DFUZZ_PRODUCTION_H="{gendir / "production.h"}"',
           "-I", str(gendir), "-I", xinclude]
    for d in extra.get("define", []):
        cmd += ["-D" + d]
    for inc in extra.get("include", []):
        cmd += ["-I", inc]
    cmd += [str(HERE / f"fuzz-{name}.c"), "-o", str(binary)]
    cmd += extra.get("link", [])
    print("+ " + " ".join(shlex.quote(c) for c in cmd), flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit(f"build of {name} failed")
    return binary


def run(name, seconds, scratch, wt, clang, work_limit=0, recover_ub=False):
    binary = build(name, wt, scratch, clang, recover_ub)
    seeds = HERE / "corpus" / name
    work = scratch / "corpus" / name
    crashes = scratch / "crashes" / name
    logs = scratch / "logs"
    for d in (work, crashes, logs):
        d.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ,
               ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:symbolize=1",
               UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    tmp = scratch / "tmp"
    tmp.mkdir(exist_ok=True)
    env["TMPDIR"] = str(tmp)
    if recover_ub:
        env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=0"
    if work_limit:
        env["FUZZ_WORK_LIMIT"] = str(work_limit)
    before = set(p.name for p in crashes.iterdir())
    cmd = [str(binary), "-max_len=4096", "-rss_limit_mb=2048", "-timeout=5", f"-max_total_time={seconds}",
           f"-artifact_prefix={crashes}/", "-print_final_stats=1", str(work), str(seeds)]
    print("+ " + " ".join(shlex.quote(c) for c in cmd), flush=True)
    log = logs / f"{name}.log"
    with open(log, "w") as out:
        proc = subprocess.run(cmd, env=env, stdout=out, stderr=subprocess.STDOUT, cwd=scratch)
    text = log.read_text(errors="replace")
    tail = [l for l in text.splitlines() if l.startswith("#") or l.startswith("stat::")]
    print("\n".join(tail[-12:]))
    new = sorted(p for p in crashes.iterdir() if p.name not in before and not p.name.startswith("min-"))
    for crash in new:
        minimized = crashes / ("min-" + crash.name)
        mcmd = [str(binary), "-minimize_crash=1", "-max_total_time=30", "-timeout=5", "-rss_limit_mb=2048",
                f"-exact_artifact_path={minimized}", str(crash)]
        subprocess.run(mcmd, env=env, stdout=open(logs / f"{name}-minimize.log", "a"),
                       stderr=subprocess.STDOUT, cwd=scratch)
        print(f"crash input: {crash}" + (f" (minimized: {minimized})" if minimized.exists() else ""))
    if recover_ub:
        seen = []
        for l in text.splitlines():
            if "runtime error:" in l and l not in seen:
                seen.append(l)
        for l in seen:
            print("undefined behaviour: " + l)
    print(f"log: {log}")
    if new or proc.returncode or (recover_ub and seen):
        print(f"{name}: FOUND {len(new)} crash input(s)" + (f", {len(seen)} undefined-behaviour location(s)" if recover_ub else "")
              + f", libFuzzer exit status {proc.returncode}")
        return 1
    print(f"{name}: no crash in {seconds} s")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list the targets")
    ap.add_argument("--target", choices=sorted(TARGETS))
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--scratch", type=Path, help="directory for the build, corpus, logs and crashes")
    ap.add_argument("--recover-ub", action="store_true",
                    help="report undefined behaviour and go on (the first report of each location is listed at the "
                         "end) instead of stopping at it; addresses errors still stop the run")
    ap.add_argument("--work-limit", type=int, default=0,
                    help="dtwm-session: end an input after this many fscanf calls (0: no limit)")
    ap.add_argument("--worktree", type=Path, default=None, help="checkout to read the production sources from")
    args = ap.parse_args()
    if args.list:
        for name, (description, _) in sorted(TARGETS.items()):
            print(f"{name:20} {description}")
        return 0
    if not args.target or not args.scratch:
        ap.error("--target and --scratch are required")
    clang = os.environ.get("CLANG", DEFAULT_CLANG)
    if not Path(clang).exists():
        sys.exit(f"clang not found: {clang} (set CLANG)")
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    return run(args.target, args.seconds, scratch, find_worktree(args.worktree), clang, args.work_limit, args.recover_ub)


if __name__ == "__main__":
    sys.exit(main())
