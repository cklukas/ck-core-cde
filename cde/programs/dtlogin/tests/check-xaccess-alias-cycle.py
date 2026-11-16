#!/usr/bin/env python3
"""Expand cyclic and fan-out alias sets of the Xaccess file in the production access.c.

access.c is taken whole from the working tree (or from the revision named by
--source-revision, for the negative control against the unfixed reader) and
compiled with ASan/UBSan against the stand-ins in check-xaccess-alias-cycle.c.
Every scenario runs in a process of its own with a time limit.

Default: every scenario must pass.
With --source-revision REV: negative control; the scenarios that expand a
cycle or an exponential fan-out must FAIL (they do not finish) and the
"acyclic" scenario must pass, which shows that the old and the new reader
agree on a file without cycles.  Exit status 0 means that this was observed.
Set DTLOGIN_SOURCE to test another copy of the sources.
"""
import argparse
import os
from pathlib import Path
import shlex
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


here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument("--source-revision", metavar="REV",
                    help="read access.c from this git revision instead of the working tree")
parser.add_argument("source", nargs="?", type=Path,
                    default=Path(os.environ.get("DTLOGIN_SOURCE") or here.parent))
args = parser.parse_args()

SCENARIOS = ["acyclic", "cycle", "ladder", "clique", "chooser"]
CONTROL_PASSES = {"acyclic"}

if args.source_revision:
    top = subprocess.run(["git", "-C", str(here), "rev-parse", "--show-toplevel"],
                         check=True, capture_output=True, text=True).stdout.strip()
    rel = Path(args.source).resolve().relative_to(top)
    text = subprocess.run(["git", "-C", top, "show", f"{args.source_revision}:{rel}/access.c"],
                          check=True, capture_output=True, text=True).stdout
else:
    text = (args.source / "access.c").read_text()
label = args.source_revision or "working tree"

cc = shlex.split(os.environ.get("CC", "cc"))
flags = ["-std=gnu23", "-w", "-D_DEFAULT_SOURCE", "-DUNIXCONN", "-DTCPCONN", "-DXDMCP", "-DHASXDMAUTH",
         "-DANSICPP", "-DNeedWidePrototypes=0", "-I", str(args.source.parents[1] / "include")]
libs = ["-lXdmcp", "-lXau"]
if sys.platform == "darwin":        # XQuartz or Homebrew X11
    for prefix in ("/opt/X11", "/opt/homebrew", "/usr/local"):
        if os.access(f"{prefix}/include/X11", os.R_OK):
            flags += [f"-I{prefix}/include"]
            libs = [f"-L{prefix}/lib", f"-Wl,-rpath,{prefix}/lib"] + libs
            break
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:symbolize=0",
           UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")

failed = []
with tempfile.TemporaryDirectory(prefix="dtlogin-alias-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    (work / "access.c").write_text(text)
    binary = work / "alias-cycle"
    subprocess.run(cc + flags + ["-I", str(work), "-I", str(args.source), f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
                                 "-fno-omit-frame-pointer", "-g", "-O1", str(here / "check-xaccess-alias-cycle.c")] +
                   libs + ["-o", str(binary)], check=True)
    for name in SCENARIOS:
        try:
            run = subprocess.run([str(binary), name], cwd=work, env=env, capture_output=True, text=True, timeout=30)
            code, out = run.returncode, run.stdout + run.stderr
        except subprocess.TimeoutExpired:
            code, out = -1, "did not finish"
        reason = next((l for l in out.splitlines() if l.strip()), f"exit status {code}")
        expect_fail = bool(args.source_revision) and name not in CONTROL_PASSES
        if expect_fail:
            if code == 0:
                print(f"NEGATIVE CONTROL FAILED ({name}): {label} passed", file=sys.stderr)
                failed.append(name)
            else:
                print(f"negative control {name} ({label}): fails as expected: {reason}")
        elif code:
            print(f"FAIL {name} ({label}): {reason}", file=sys.stderr)
            failed.append(name)
        else:
            print(f"ok   {name}")

if failed:
    sys.exit(1)
print(f"negative control ({label}): fails as expected" if args.source_revision else f"PASS ({label})")
