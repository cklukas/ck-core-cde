#!/usr/bin/env python3
"""Run the production write_rcfile() with mkstemp/fdopen failures injected.

write_rcfile() is extracted verbatim from calctool.c (the working tree, or
another revision with --source-revision REV) and compiled together with
check-rcfile-tempfile.c under AddressSanitizer and UndefinedBehaviorSanitizer
with halt on error.  The only textual change is that the fixed
"/tmp/.dtcalcrcXXXXXX" template is replaced by one inside the scratch
directory, so the test never touches /tmp or a real home directory.

Default: the test must pass.
With --source-revision REV: negative control; REV must FAIL the same test
(exit status 0 means the failure was observed).
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

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
root = here.parents[3]
relative = "cde/programs/dtcalc/calctool.c"
literal = '"/tmp/.dtcalcrcXXXXXX"'

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of calctool.c to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

scratch = args.scratch or args.worktree / "tests-scratch" / "dtcalc-tempfile"
scratch.mkdir(parents=True, exist_ok=True)

if args.source_revision:
    text = subprocess.run(["git", "-C", str(args.worktree), "show",
                           f"{args.source_revision}:{relative}"],
                          check=True, capture_output=True, text=True).stdout
    label = args.source_revision
else:
    text = (args.worktree / relative).read_text()
    label = "working tree"

start = text.index("void\nwrite_rcfile(")
end = text.index("\n}\n", start) + 3
func = text[start:end]
if literal not in func:
    sys.exit(f"template literal {literal} not found in write_rcfile()")
func = func.replace(literal, "CK_TEMPLATE")

parts = (here / "check-rcfile-tempfile.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"rcfile-tempfile-{tag}.c", scratch / f"rcfile-tempfile-{tag}"
c.write_text(prologue + func + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-deprecated-declarations", "-Wno-unused-value",
                f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)

run_dir = scratch / f"run-{tag}"
shutil.rmtree(run_dir, ignore_errors=True)
run_dir.mkdir()
env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
run = subprocess.run([str(binary), str(run_dir)], env=env, capture_output=True, text=True)
sys.stdout.write(run.stdout)
shutil.rmtree(run_dir, ignore_errors=True)
if args.source_revision:
    tail = "\n".join(run.stderr.strip().splitlines()[:12])
    if run.returncode == 0:
        print(f"NEGATIVE CONTROL FAILED: {label} passed the injection test", file=sys.stderr)
        sys.exit(1)
    print(f"negative control ({label}): fails as expected, exit {run.returncode}\n{tail}")
else:
    if run.returncode:
        sys.stderr.write(run.stderr)
        print(f"FAIL ({label}): exit {run.returncode}", file=sys.stderr)
        sys.exit(1)
    print(f"PASS ({label})")
