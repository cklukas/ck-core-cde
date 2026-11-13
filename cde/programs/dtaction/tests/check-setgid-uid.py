#!/usr/bin/env python3
"""Check that dtaction stops when a change of group or user ID is refused.

SetGidUid() is extracted verbatim from Main.c (the working tree, or another
revision with --source-revision REV) and compiled with check-setgid-uid.c
under AddressSanitizer and UndefinedBehaviorSanitizer.  The set*gid, set*uid
and initgroups calls are replaced by a fake kernel that records the order of
the calls and refuses a chosen one.  The test requires: every step in the
right order (group, supplementary groups, user), a failure reported to the
caller, and no later step after a refused one.  Both build variants of the
function (Linux/BSD set-re-ids and System V setgid/setuid) are compiled.

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
relative = "cde/programs/dtaction/Main.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of Main.c to use as a negative control")
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

anchor = "\nSetGidUid ( "
if text.count(anchor) != 1:
    sys.exit(f"anchor {anchor.strip()!r} found {text.count(anchor)} times in {relative}, expected 1")
start = text.rindex("\n", 0, text.index(anchor) - 1) + 1     # the line with the return type
end = text.index("\n}\n", start) + 3
production = text[start:end]

parts = (here / "check-setgid-uid.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
failures = []
for variant in ("linux", "svr4"):
    c = scratch / f"setgid-uid-{tag}-{variant}.c"
    binary = scratch / f"setgid-uid-{tag}-{variant}"
    c.write_text(prologue + production + "\n" + body)
    cmd = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-fno-omit-frame-pointer", "-g"]
    if variant == "svr4":
        cmd.append("-DTEST_VARIANT_SVR4")
    cmd += [str(c), "-o", str(binary)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode:
        # the revision does not offer the contract the test checks (it has no
        # result to report): that is a failure of the revision
        if args.source_revision:
            failures.append((variant, "does not build: " + build.stderr.strip().splitlines()[0]))
            continue
        sys.stderr.write(build.stderr)
        sys.exit(f"FAIL ({label}): the {variant} variant does not build")
    run = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    if run.returncode:
        failures.append((variant, run.stderr.strip().splitlines()[0] if run.stderr.strip() else f"exit {run.returncode}"))

if args.source_revision:
    if not failures:
        print(f"NEGATIVE CONTROL FAILED: {label} passed the test", file=sys.stderr)
        sys.exit(1)
    print(f"negative control ({label}): fails as expected\n" +
          "\n".join(f"  {v}: {m}" for v, m in failures))
else:
    if failures:
        for v, m in failures:
            print(f"FAIL ({label}) {v}: {m}", file=sys.stderr)
        sys.exit(1)
    print(f"PASS ({label})")
