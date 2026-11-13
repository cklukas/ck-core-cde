#!/usr/bin/env python3
"""Check that dtfile's CheckAccess() does not change the process identity.

CheckAccess() is extracted verbatim from FileManip.c (the working tree, or
another revision with --source-revision REV) and compiled with
check-access-ids.c under AddressSanitizer and UndefinedBehaviorSanitizer.
setuid, setgid, setreuid, setregid, seteuid and setegid are replaced by
counters.  The function must give the right answer for a readable file, a
file without execute permission and a missing file, and must not have changed
the user or group ID of the process: the former way of testing (set the real
IDs to the effective ones, test, set them back) ignored every refusal and
could not restore the real ID of an unprivileged effective user.

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
relative = "cde/programs/dtfile/FileManip.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of FileManip.c to use as a negative control")
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

anchor = "\nint\nCheckAccess(\n"
if text.count(anchor) != 1:
    sys.exit(f"anchor {anchor.strip()!r} found {text.count(anchor)} times in {relative}, expected 1")
start = text.index(anchor) + 1
end = text.index("\n}\n", start) + 3
production = text[start:end]

parts = (here / "check-access-ids.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"access-ids-{tag}.c", scratch / f"access-ids-{tag}"
c.write_text(prologue + production + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)

env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
run = subprocess.run([str(binary), str(scratch)], env=env, capture_output=True, text=True)
sys.stdout.write(run.stdout)
if args.source_revision:
    tail = "\n".join(run.stderr.strip().splitlines()[:6])
    if run.returncode == 0:
        print(f"NEGATIVE CONTROL FAILED: {label} passed the test", file=sys.stderr)
        sys.exit(1)
    print(f"negative control ({label}): fails as expected, exit {run.returncode}\n{tail}")
else:
    if run.returncode:
        sys.stderr.write(run.stderr)
        print(f"FAIL ({label}): exit {run.returncode}", file=sys.stderr)
        sys.exit(1)
    print(f"PASS ({label})")
