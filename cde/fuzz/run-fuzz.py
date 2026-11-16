#!/usr/bin/env python3
"""Build and run the libFuzzer targets of the CK Core CDE parsers.

run-fuzz.py --list
run-fuzz.py --target NAME --seconds N --scratch DIR [--worktree DIR]
run-fuzz.py --all --replay [--scratch DIR] [--worktree DIR]

Every target fuzzes production parser code without the X11, Motif and
daemon environment it normally runs in.  Like the regression tests under
programs/*/tests, the runner cuts the parser functions out of the production
source text (the working tree of --worktree), writes them into a generated
header under DIR/build/NAME/, and compiles that header together with the
fuzz-NAME.c harness, which holds the small stand-ins for the surroundings.
The production files are only read, never changed.

The target is built with the clang of the environment variable CLANG (default:
the Homebrew clang, /opt/homebrew/opt/llvm/bin/clang, on macOS; the first
`clang` in PATH elsewhere) and -fsanitize=fuzzer,address,undefined.  Undefined behaviour stops the run like
an address error does (-fno-sanitize-recover=undefined).  The corpus starts
from cde/fuzz/corpus/NAME/; the grown corpus, the logs and the crash inputs
stay below DIR.  The exit status is 1 when a crash, a timeout or an
out-of-memory input was found.

--replay runs each target once over every file of its corpus directory
(libFuzzer -runs=0: no mutation, no corpus growth) and prints one result line
per target; the exit status is 1 when any input crashes or a target does not
build.  The corpora hold the regression input of every fixed defect, so a replay
is the fast check that no fixed defect has come back.  --all applies the action
(a fuzzing run of --seconds, or --replay) to every target.  A target whose
prerequisites are missing on the host (see TARGET_LIBS) is reported as skipped
with the reason; it is not a failure.

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
import tempfile

def _ck_sanitize_list() -> str:
    import os, shlex, subprocess, tempfile
    override = os.environ.get("CK_SANITIZE_LIST", "").strip()
    if override:
        return override
    for var in ("CC", "CXX", "CLANG", "TT_COMPILE", "CSA_COMPILE", "CMSD_COMPILE",
                "SPC_COMPILE", "DTSVC_COMPILE", "DTSR_COMPILE", "DTMAIL_CC",
                "DTMAIL_COMPILE", "DIRSELECT_CC"):
        val = os.environ.get(var, "").strip()
        if val:
            cc = shlex.split(val)[:1]
            break
    else:
        cc = ["cc"]
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
        src, exe = os.path.join(d, "p.c"), os.path.join(d, "p")
        with open(src, "w", encoding="utf-8") as f:
            f.write("int main(void) { return 0; }\n")
        for s in ("address,undefined", "undefined"):
            r = subprocess.run(cc + [f"-fsanitize={s}", src, "-o", exe], capture_output=True)
            if r.returncode == 0 and subprocess.run([exe], capture_output=True).returncode == 0:
                return s
    return "undefined"


_CK_SANITIZE_LIST = _ck_sanitize_list()


HERE = Path(__file__).resolve().parent
IS_MACOS = sys.platform == "darwin"
MACOS_CLANG = "/opt/homebrew/opt/llvm/bin/clang"
# Headers of XQuartz: Xlib, Xt, Xdmcp.  No Motif is needed by any target.  Other
# hosts use the system paths (pkg-config, then the compiler's default
# directories).  The environment variables XINCLUDE and XLIB override both.
MACOS_XPREFIX = "/opt/X11" if os.access("/opt/X11/include/X11", os.R_OK) else "/opt/homebrew"
MACOS_XINCLUDE = f"{MACOS_XPREFIX}/include"
MACOS_XLIB = f"{MACOS_XPREFIX}/lib"
# pkg-config modules that a target needs besides Xlib and Xt; a host without
# them skips the target.
TARGET_LIBS = {
    "dtlogin-xdmcp": ("xdmcp", "xau"),
    "dtlogin-xaccess": ("xdmcp", "xau"),
}


def find_clang():
    """The compiler: $CLANG, else the Homebrew clang on macOS, else clang in PATH."""
    wanted = os.environ.get("CLANG") or (MACOS_CLANG if IS_MACOS else "clang")
    found = shutil.which(wanted)
    if not found:
        sys.exit(f"clang not found: {wanted} (set CLANG)")
    return found


def pkg_config(*args):
    """The words pkg-config prints, or None when it is missing or refuses."""
    try:
        r = subprocess.run(["pkg-config", *args], capture_output=True, text=True)
    except OSError:
        return None
    return shlex.split(r.stdout) if r.returncode == 0 else None


def x_include_flags():
    given = os.environ.get("XINCLUDE")
    if given:
        return ["-I", given]
    if IS_MACOS:
        return ["-I", MACOS_XINCLUDE]
    return pkg_config("--cflags", "x11", "xt") or []


def x_link_flags(libs):
    xlib = os.environ.get("XLIB") or (MACOS_XLIB if IS_MACOS else "")
    flags = ["-L" + xlib, "-Wl,-rpath," + xlib] if xlib else []
    return flags + ["-l" + lib for lib in libs]


def unavailable_reason(name):
    """Why the target cannot be built on this host, or None."""
    if IS_MACOS or name not in TARGET_LIBS or os.environ.get("XINCLUDE"):
        return None
    missing = [m for m in TARGET_LIBS[name] if pkg_config("--exists", m) is None]
    return ("pkg-config module(s) not installed: " + ", ".join(missing)) if missing else None


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


def c_function(text, name, source="source"):
    """Return the definition of the function NAME for files in the old K&R-era
    layout (return type, qualifiers and name on separate lines, arguments over
    many lines): the type lines above the name, the argument list, and the
    body up to its matching brace.  Strings, characters and comments are
    skipped when braces are counted.  The definition must be unique."""
    def skip(i):
        if text.startswith("/*", i):
            return text.index("*/", i + 2) + 2
        if text.startswith("//", i):
            return text.index("\n", i)
        quote = text[i]
        i += 1
        while text[i] != quote:
            i += 2 if text[i] == "\\" else 1
        return i + 1

    def close(i, opener, closer):
        depth = 1
        while depth:
            if text[i] in "\"'" or text.startswith(("/*", "//"), i):
                i = skip(i)
                continue
            depth += (text[i] == opener) - (text[i] == closer)
            i += 1
        return i

    found = []
    for m in re.finditer(r"^(?:[\w \t\*]*[ \t\*])?" + re.escape(name) + r"\s*\(", text, re.M):
        i = close(m.end(), "(", ")")
        if not re.match(r"\s*\{", text[i:]):
            continue                                   # a prototype or a call
        start = m.start()
        while True:                                    # the type lines above the name
            previous = text.rfind("\n", 0, start - 1) + 1
            line = text[previous:start - 1]
            if (not line.strip() or line.lstrip().startswith(("*", "/*", "#", "}"))
                    or line.rstrip().endswith((";", "*/", "}"))):
                break
            start = previous
        end = close(text.index("{", i) + 1, "{", "}")
        found.append(text[start:end] + "\n")
    if len(found) != 1:
        sys.exit(f"{source}: definition of {name} found {len(found)} times, expected 1")
    return found[0]


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
    prod += defines(text, ["SESSION_MAX_RECORDS"], src)
    prod += functions(text, ["SessionHomeDirectory", "SessionFileGetName", "SessionReadCount",
                             "SessionRestoreData"], src)
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
        "link": x_link_flags(["Xdmcp", "Xau"])}


def build_dtwm_property(wt):
    root = wt / "cde/programs/dtwm"
    text = (root / "WmProperty.c").read_text()
    src = "WmProperty.c"
    prod = functions(text, ["GetNormalHints", "ProcessWmProtocols", "ProcessMwmMessages", "GetWMState",
                            "GetMwmHints", "GetMwmInfo", "ProcessWmColormapWindows", "GetUtf8String"], src)
    return {"types.h": "", "production.h": prod}, {"include": [str(root)]}


def build_dtsvc_dbreader(wt):
    base = wt / "cde"
    text = (base / "lib/DtSvc/DtUtil1/DbReader.c").read_text()
    reader_h = (base / "include/Dt/DbReader.h").read_text()
    src = "DbReader.c"
    types = typedef(reader_h, r"typedef\s+struct\s*\{[^{}]*\}\s*DtDtsDbField;", "DbReader.h")
    types += typedef(reader_h, r"typedef\s+Boolean\s*\(\*DtDbConverter\)[^;]*;", "DbReader.h")
    types += typedef(reader_h, r"typedef\s+struct\s*\{[^{}]*DtDbConverter\s*\*\s*converters;[^{}]*\}\s*DtDbRecordDesc;",
                     "DbReader.h")
    for pattern in (r"^#define\s+MAX_LINE_LENGTH\s+\d+\s*$", r"^#define\s+NUMBER_FIELDS_DEFAULT\s+\d+\s*$",
                    r"^#define\s+NUMBER_FIELDS_INCREMENT\s+\d+\s*$", r"^#define\s+DTUNLIMITEDFIELDS\s+\d+\s*$",
                    r"^#define\s+AT_START\s+\d+", r"^#define\s+NO_START_SYMBOL\s+\d+",
                    r"^#define\s+START_SYMBOL\s+\d+", r"^#define\s+VERSION_\w+\s+.*$",
                    r"^#define\s+MAX_VARIABLE_REPLACEMENTS\s+\d+\s*$",
                    r"^#define\s+MAX_EXPANDED_LINE_LENGTH\s+.*$"):
        found = [m.group(0) + "\n" for m in re.finditer(pattern, text + reader_h, re.M)]
        if not found:
            sys.exit(f"{src}: definition /{pattern}/ not found")
        types += "".join(found)
    types += typedef(text, r"typedef struct \{\s*char \* varName;\s*char \* value;\s*\} VariableEntry;", src)
    types += "static int variableCount;\nstatic int variableCapacity;\nstatic VariableEntry * variableSet;\n"
    decl = between(text, "/********    Static Function Declarations", "/********    End Static Function Declarations", src)
    names = ["report_error", "read_line", "clean_line", "get_variable", "ResolveVariableReference",
             "_DtDbFillVariables", "DefineVariable", "ClearVariables", "SplitField", "MatchKeyword",
             "ReadNextEntry", "FreeDbField"]
    prod = decl + "\n".join(c_function(text, n, src) for n in names)
    return {"types.h": types, "production.h": prod}, {}


def build_dtsvc_mmcache(wt):
    base = wt / "cde"
    root = base / "lib/DtSvc/DtUtil1"
    mm = (root / "DtsMM.c").read_text()
    strtab = (root / "strtab.c").read_text()
    inttab = (root / "inttab.c").read_text()
    mm_h = (base / "include/Dt/DtsMM.h").read_text()
    shm_h = (base / "include/Dt/DtShmDb.h").read_text()
    types = between(shm_h, "typedef const void * \tDtShmStrtab;", "/*\n  routines used while building", "DtShmDb.h")
    types += between(mm_h, "typedef\tint\tDtDtsMMSeqNo;", "/* Db Internal pointers */", "DtsMM.h")
    types += typedef(strtab, r"typedef struct strtab \{.*?\} strtab_t;", "strtab.c")
    types += typedef(strtab, r"typedef struct strtab_entry \{.*?\} strtab_entry_t;", "strtab.c")
    types += defines(strtab, ["NOT_AN_INDEX"], "strtab.c")
    types += typedef(inttab, r"typedef struct inttab \{.*?\} inttab_t;", "inttab.c")
    types += '#define DtDTS_DC_NAME "DATA_CRITERIA"\n'
    types += typedef(mm, r"struct cache_view\s*\{.*?\n\};", "DtsMM.c")
    prod = between(mm, "static\tDtDtsMMDatabase\t*db_list;", "int _DtDtsMMUnLoad(void);", "DtsMM.c")
    prod += c_function(strtab, "_DtShmBosonToString", "strtab.c")
    prod += c_function(strtab, "_DtShmStringToBoson", "strtab.c")
    prod += c_function(strtab, "_DtShmStrtabHasString", "strtab.c")
    prod += c_function(strtab, "_DtShmValidateStrtab", "strtab.c")
    prod += c_function(inttab, "_DtShmFindIntTabEntry", "inttab.c")
    prod += c_function(inttab, "_DtShmValidateInttab", "inttab.c")
    prod += "\n".join(c_function(mm, n, "DtsMM.c") for n in [
        "_DtDtsMMGetPtr", "_DtDtsMMGetPtrSize", "_DtDtsMMGetDCNameIndex", "_DtDtsMMGetDbName",
        "_DtDtsMMGetNoNameIndex", "_DtDtsMMGetBufferIndex", "_DtDtsMMGetFileList", "_DtDtsMMBosonToString",
        "_DtDtsMMStringToBoson", "_DtDtsMMGet", "_DtDtsMMGetField", "_DtDtsMMGetFieldByName",
        "_DtDtsMMGetRecordByName", "cache_size_fits", "index_inside", "cache_header_valid",
        "cache_block", "cache_ints", "cache_string", "cache_records_below", "cache_record_entry",
        "cache_name_entry", "cache_inttab", "cache_database_valid", "cache_tables_valid", "_DtDtsMMapDB",
        "_DtDtsMMUnLoad"])
    return {"types.h": types, "production.h": prod}, {}


def build_dthelp_sdl(wt):
    base = wt / "cde"
    root = base / "lib/DtHelp"
    src = "UtilSDL.c"
    util = (root / src).read_text()
    for name in ("SkipWhiteSpace", "GetSDLElement", "_DtHelpCeMatchSdlElement", "_DtHelpCeGetSdlAttribute",
                 "_DtHelpCeReturnSdlElement", "_DtHelpCeGetSdlCdata", "_DtHelpCeGetSdlAttributeCdata",
                 "_DtHelpCeSkipCdata", "_DtHelpCeGetSdlId", "_DtHelpCeGetSdlNumber",
                 "_DtHelpCeFindSkipSdlElementEnd"):
        c_function(util, name, src)                       # the anchors must exist exactly once
    # whole production files: the SDL token layer, the buffered reader it reads through, and the
    # two buffer-append helpers and the case-insensitive compare it calls (over CvString.c)
    prod = f'#include "{root / src}"\n#include "{root / "bufio.c"}"\n#include "{root / "CvString.c"}"\n'
    fmt = (root / "FormatUtil.c").read_text()
    strf = (root / "StringFuncs.c").read_text()
    prod += c_function(fmt, "_DtHelpCeAddCharToBuf", "FormatUtil.c")
    prod += c_function(fmt, "_DtHelpCeAddStrToBuf", "FormatUtil.c")
    prod += c_function(strf, "_DtHelpCeStrNCaseCmpLatin1", "StringFuncs.c")
    return {"types.h": "", "production.h": prod}, {
        "define": ["DTLIB"],
        "include": [str(root), str(base / "include/Dt"), str(base / "include/DtI"), str(base / "include")]}


def build_dtlogin_xaccess(wt):
    root = wt / "cde/programs/dtlogin"
    text = (root / "access.c").read_text()
    for name in ("ReadWord", "ReadHostEntry", "ReadDisplayEntry", "ReadAccessDatabase", "ScanAccessDatabase",
                 "patternMatch", "scanHostlist", "indirectAlias", "ForEachMatchingIndirectHost",
                 "ForEachChooserHost", "AcceptableDisplayAddress"):
        c_function(text, name, "access.c")                  # the anchors must exist exactly once
    # the whole production file, as check-config.py compiles it; gethostbyname goes to the harness
    prod = '#define gethostbyname fuzz_gethostbyname\n#include "%s"\n' % (root / "access.c")
    return {"types.h": "", "production.h": prod}, {
        "define": ["UNIXCONN", "TCPCONN", "XDMCP", "HASXDMAUTH", "ANSICPP", "NeedWidePrototypes=0"],
        "include": [str(root), str(wt / "cde/include")],
        "link": x_link_flags(["Xdmcp", "Xau"])}


TARGETS = {
    "dtlogin-xaccess": ("dtlogin Xaccess file reader and the XDMCP access checks over it: hosts, patterns, "
                        "aliases, chooser and broadcast entries (dtlogin/access.c)", build_dtlogin_xaccess),
    "dthelp-sdl": ("DtHelp SDL token layer: elements, attributes, cdata with entities and multibyte "
                   "characters, skipped declarations (DtHelp/UtilSDL.c over bufio.c)", build_dthelp_sdl),
    "dtsvc-mmcache": ("libDtSvc memory-mapped type database cache: header check, string and integer tables, "
                      "record and field lookup (DtUtil1/DtsMM.c, strtab.c, inttab.c)", build_dtsvc_mmcache),
    "dtsvc-dbreader": ("libDtSvc data-type and action database (.dt) file reader: lines, variables, fields, "
                       "records (DtUtil1/DbReader.c)", build_dtsvc_dbreader),
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
    cmd = [clang, f"-fsanitize=fuzzer,{_CK_SANITIZE_LIST}",
           "-fsanitize-recover=undefined" if recover_ub else "-fno-sanitize-recover=undefined",
           "-fno-omit-frame-pointer", "-g", "-O1", "-std=gnu23", "-w",
           "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE",
           f'-DFUZZ_TYPES_H="{gendir / "types.h"}"', f'-DFUZZ_PRODUCTION_H="{gendir / "production.h"}"',
           "-I", str(gendir), *x_include_flags()]
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


def replay(name, scratch, wt, clang):
    """Run the target once over every file of its corpus directory, without
    mutation (libFuzzer -runs=0).  Prints one result line; returns 1 on a crash."""
    binary = build(name, wt, scratch, clang)
    seeds = HERE / "corpus" / name
    inputs = [p for p in seeds.rglob("*") if p.is_file()]
    if not inputs:
        print(f"{name}: replay FAILED: no corpus files in {seeds}")
        return 1
    crashes = scratch / "crashes" / name
    logs = scratch / "logs"
    tmp = scratch / "tmp"
    for d in (crashes, logs, tmp):
        d.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(tmp),
               ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:symbolize=1",
               UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    # an input longer than -max_len would be cut, so the cap follows the largest file
    max_len = max(4096, max(p.stat().st_size for p in inputs))
    cmd = [str(binary), "-runs=0", f"-max_len={max_len}", "-rss_limit_mb=2048", "-timeout=5",
           f"-artifact_prefix={crashes}/", str(seeds)]
    print("+ " + " ".join(shlex.quote(c) for c in cmd), flush=True)
    log = logs / f"{name}-replay.log"
    with open(log, "w") as out:
        proc = subprocess.run(cmd, env=env, stdout=out, stderr=subprocess.STDOUT, cwd=scratch)
    if proc.returncode:
        text = log.read_text(errors="replace")
        sys.stdout.write("\n".join(text.splitlines()[-40:]) + "\n")
        print(f"{name}: replay FAILED: libFuzzer exit status {proc.returncode} over {len(inputs)} input(s), "
              f"log {log}")
        return 1
    print(f"{name}: replay ok, {len(inputs)} input(s)")
    return 0


def run_target(args, name, scratch, wt, clang):
    """Run one target for --all or --target; a missing prerequisite skips it, a
    failed build or extraction is a failure of that target only."""
    reason = unavailable_reason(name)
    if reason:
        print(f"{name}: skipped: {reason}")
        return 0
    try:
        if args.replay:
            return replay(name, scratch, wt, clang)
        return run(name, args.seconds, scratch, wt, clang, args.work_limit, args.recover_ub)
    except SystemExit as e:
        print(f"{name}: {'replay ' if args.replay else ''}FAILED: {e.code}")
        return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list the targets")
    ap.add_argument("--target", choices=sorted(TARGETS))
    ap.add_argument("--all", action="store_true", help="apply the action to every target")
    ap.add_argument("--replay", action="store_true",
                    help="run each target once over every file of its corpus directory (libFuzzer -runs=0, no "
                         "mutation) and exit 1 if any input crashes; --scratch defaults to a temporary directory")
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
    if bool(args.target) == args.all:
        ap.error("exactly one of --target and --all is required")
    if not args.scratch and not args.replay:
        ap.error("--scratch is required")
    clang = find_clang()
    own_scratch = None
    if args.scratch:
        scratch = args.scratch.resolve()
        scratch.mkdir(parents=True, exist_ok=True)
    else:
        own_scratch = scratch = Path(tempfile.mkdtemp(prefix="ck-fuzz-replay-")).resolve()
    wt = find_worktree(args.worktree)
    try:
        status = 0
        for name in ([args.target] if args.target else sorted(TARGETS)):
            status |= run_target(args, name, scratch, wt, clang)
        return status
    finally:
        if own_scratch:
            shutil.rmtree(own_scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
