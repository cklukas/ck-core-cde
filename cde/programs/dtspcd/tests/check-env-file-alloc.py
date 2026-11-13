#!/usr/bin/env python3
"""Run the production SPCD_Initialize() with chosen allocations failing.

SPCD_Initialize() is extracted verbatim from main.c (the working tree, or
another revision with --source-revision REV), compiled together with
check-env-file-alloc.c under AddressSanitizer and UndefinedBehaviorSanitizer
with halt on error.

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

here = Path(__file__).resolve().parent
root = here.parents[3]
relative = "cde/programs/dtspcd/main.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of main.c to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

scratch = args.scratch or args.worktree / "tests-scratch"
scratch.mkdir(parents=True, exist_ok=True)

if args.source_revision:
    text = subprocess.run(["git", "-C", str(args.worktree), "show",
                           f"{args.source_revision}:{relative}"],
                          check=True, capture_output=True, text=True).stdout
    label = args.source_revision
else:
    text = (args.worktree / relative).read_text()
    label = "working tree"

# Exact, unique anchors; anything else is an error, never a silent skip.
head = "int SPCD_Initialize(void)\n/*----------------------------------------------------------------------+*/\n{\n"
if text.count(head) != 1:
    sys.exit(f"anchor not unique or missing in {label}: {head!r}")
start = text.index(head)
# The function ends at the first closing brace in column 0.
end = text.index("\n}\n", start)
if end < 0 or "SPC_Add_Env_File(sys_env_file" not in text[start:end]:
    sys.exit(f"SPCD_Initialize() in {label} lacks the expected environment-file code")
end += 3

parts = (here / "check-env-file-alloc.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"env-file-alloc-{tag}.c", scratch / f"env-file-alloc-{tag}"
c.write_text(prologue + text[start:end] + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-deprecated-declarations", "-Wno-unused-value",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)

# Leak detection is off: the stub environment list and the names it records
# are intentionally not freed by the harness.  Path-buffer ownership is
# checked by the test's own live-block count instead.
env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
run = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
sys.stdout.write(run.stdout)
if args.source_revision:
    tail = "\n".join(run.stderr.strip().splitlines()[:6])
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
