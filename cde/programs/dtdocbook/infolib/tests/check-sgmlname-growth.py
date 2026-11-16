#!/usr/bin/env python3
"""Check that the dtdocbook name table grows without a mismatched release.

programs/dtdocbook/infolib/SGMLName.C (the working tree, or another revision
with --source-revision REV) is compiled whole with check-sgmlname-growth.cc
under AddressSanitizer, with the allocation/deallocation mismatch check on,
and UndefinedBehaviorSanitizer.  The table and the upper-casing buffer grow
with new[]; releasing them with plain delete is reported by the sanitizer.

Default: the test must pass.
With --source-revision REV: negative control; REV must FAIL the same test
(exit status 0 means the failure was observed).
"""
import argparse
import os
from pathlib import Path
import shlex
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
root = here.parents[4]
relative = "cde/programs/dtdocbook/infolib/SGMLName.C"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the source to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, required=True)
args = ap.parse_args()

args.scratch.mkdir(parents=True, exist_ok=True)
if args.source_revision:
    text = subprocess.run(["git", "-C", str(args.worktree), "show",
                           f"{args.source_revision}:{relative}"],
                          check=True, capture_output=True, text=True).stdout
    label = args.source_revision
else:
    text = (args.worktree / relative).read_text()
    label = "working tree"

tag = "control" if args.source_revision else "fixed"
source = args.scratch / f"SGMLName-{tag}.C"
source.write_text(text)
binary = args.scratch / f"check-sgmlname-growth-{tag}"
compile_run = subprocess.run(
    shlex.split(os.environ.get("CXX", "c++")) +
    ["-std=gnu++23", "-O1", "-Wall", "-Wno-write-strings", "-I" + str(here.parent),
     f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
     str(source), str(here / "check-sgmlname-growth.cc"), "-o", str(binary)],
    capture_output=True, text=True)
if compile_run.returncode:
    sys.stderr.write(compile_run.stderr)
    sys.exit("FAIL: does not compile")

env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:alloc_dealloc_mismatch=1:detect_leaks=0:symbolize=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
run = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=120)
if args.source_revision:
    if run.returncode == 0:
        sys.exit(f"NEGATIVE CONTROL FAILED: {label} passed the test")
    print(f"negative control ({label}): fails as expected, exit {run.returncode}")
else:
    if run.returncode:
        sys.stderr.write(run.stderr)
        sys.exit(f"FAIL ({label}): exit {run.returncode}")
    print(f"PASS ({label})")
