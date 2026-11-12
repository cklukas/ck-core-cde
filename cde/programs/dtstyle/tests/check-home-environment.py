#!/usr/bin/env python3
"""Run the production style.home initialisation of dtstyle with HOME unset/set.

The statements between the lines `style.errDialog  = NULL;` and
`style.colorDialog = NULL;` are extracted verbatim from Main.c (the working
tree, or another revision with --source-revision REV) and compiled into
check-home-environment.c under AddressSanitizer and UndefinedBehaviorSanitizer
with halt on error. Both marker lines must occur exactly once.

Default: both cases (HOME unset, HOME set) must pass.
With --source-revision REV: negative control; REV must FAIL the HOME-unset
case (exit status 0 means the failure was observed) and still pass HOME set.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

here = Path(__file__).resolve().parent
root = here.parents[3]
relative = "cde/programs/dtstyle/Main.c"
START = "style.errDialog  = NULL;"
END = "style.colorDialog = NULL;"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of Main.c to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

scratch = args.scratch or args.worktree / "tests-scratch" / "dtstyle-home"
scratch.mkdir(parents=True, exist_ok=True)

if args.source_revision:
    text = subprocess.run(["git", "-C", str(args.worktree), "show",
                           f"{args.source_revision}:{relative}"],
                          check=True, capture_output=True, text=True).stdout
    label = args.source_revision
else:
    text = (args.worktree / relative).read_text()
    label = "working tree"

for marker in (START, END):
    if text.count(marker) != 1:
        sys.exit(f"marker {marker!r} found {text.count(marker)} times in {label}, expected exactly once")
start, end = text.index(START), text.index(END)
if start >= end:
    sys.exit("markers are out of order")
# Whole lines: from the start of the START line up to the start of the END line.
block = text[text.rindex("\n", 0, start) + 1:text.rindex("\n", 0, end) + 1]

src = (here / "check-home-environment.c").read_text()
if src.count("/* @@EXTRACTED@@ */\n") != 1:
    sys.exit("template placeholder missing")
tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"home-{tag}.c", scratch / f"home-{tag}"
c.write_text(src.replace("/* @@EXTRACTED@@ */\n", block))
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu99", "-Wall", "-Wno-nonnull", "-Wno-unused-variable",
                "-Wno-unused-value", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-O0", "-g",
                str(c), "-o", str(binary)], check=True)

env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
cases = [("HOME unset", ["unset"]), ("HOME set", ["set", "/var/empty/home dir"])]
status = 0
for name, argv in cases:
    run = subprocess.run([str(binary)] + argv, env=env, capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    tail = "\n".join(run.stderr.strip().splitlines()[:6])
    expect_fail = bool(args.source_revision) and name == "HOME unset"
    if expect_fail:
        if run.returncode == 0:
            print(f"NEGATIVE CONTROL FAILED: {label} survived {name}", file=sys.stderr)
            status = 1
        else:
            print(f"negative control ({label}, {name}): fails as expected, exit {run.returncode}\n{tail}")
    elif run.returncode:
        sys.stderr.write(run.stderr)
        print(f"FAIL ({label}, {name}): exit {run.returncode}", file=sys.stderr)
        status = 1
    else:
        print(f"PASS ({label}, {name})")
sys.exit(status)
