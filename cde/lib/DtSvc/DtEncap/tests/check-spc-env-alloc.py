#!/usr/bin/env python3
"""Run the production SPC_Create_Default_Envp() with allocation failing.

The function is extracted verbatim from spc-env.c (the working tree, or
another revision with --source-revision REV), compiled together with
check-spc-env-alloc.c under AddressSanitizer and UndefinedBehaviorSanitizer
with halt on error.  The extraction fails loudly unless both anchors occur
exactly once.

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
root = here.parents[4]
relative = "cde/lib/DtSvc/DtEncap/spc-env.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of spc-env.c to use as a negative control")
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

start_anchor = "\nXeString *SPC_Create_Default_Envp(XeString *old_envp)\n"
end_anchor = "\n}\n"
if text.count(start_anchor) != 1:
    sys.exit(f"anchor {start_anchor.strip()!r} found {text.count(start_anchor)} times in {label}, expected 1")
start = text.index(start_anchor) + 1
end = text.index(end_anchor, start) + len(end_anchor)
func = text[start:end]
# Sanity: the extract must be the whole function, ending at its return.
if func.count("{") != func.count("}") or "return(envp);" not in func:
    sys.exit(f"extracted function from {label} does not look complete")

parts = (here / "check-spc-env-alloc.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"spc-env-alloc-{tag}.c", scratch / f"spc-env-alloc-{tag}"
c.write_text(prologue + func + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-deprecated-declarations", "-Wno-unused-value",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)

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
