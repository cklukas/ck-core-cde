#!/usr/bin/env python3
"""Run the production build_file_list() and build_name_list() with realloc failing.

The functions (with their list helpers, struct list and srch()) are extracted
verbatim from MMDb.c (the working tree, or another revision with
--source-revision REV) and compiled together with mmdb-alloc-test.c under
AddressSanitizer and UndefinedBehaviorSanitizer with halt on error.  Every
extraction fails loudly unless its anchors occur exactly once.

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
relative = "cde/lib/DtSvc/DtUtil1/MMDb.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of MMDb.c to use as a negative control")
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


def extract(start_anchor, end_anchor="\n}\n", keep_start=0):
    if text.count(start_anchor) != 1:
        sys.exit(f"anchor {start_anchor.strip()!r} found {text.count(start_anchor)} times in {label}, expected 1")
    start = text.index(start_anchor) + keep_start
    end = text.index(end_anchor, start) + len(end_anchor)
    piece = text[start:end]
    if piece.count("{") != piece.count("}"):
        sys.exit(f"extract at {start_anchor.strip()!r} from {label} does not look complete")
    return piece


# The scan: in a revision with the helpers they precede build_file_list().
status = "\nstatic int\nbuild_file_list(" in text
first = "\n/*\n * Make room for COUNT entries in both lists" if status else "\nstatic void\nbuild_file_list("
scan_start = text.index(first) + 1 if text.count(first) == 1 else sys.exit(f"anchor {first!r} not unique in {label}")
scan_end = text.index("\n}\n", text.index("build_file_list(DtShmProtoIntList int_handle", scan_start)) + 3
scan = text[scan_start:scan_end]
if scan.count("{") != scan.count("}"):
    sys.exit(f"scan extract from {label} does not look complete")
pieces = [scan,
          extract("\nstruct  list\n{", "\n}\n", keep_start=1),  # includes srch(),
          extract("\nstatic int\nbuild_name_list(DtDtsDbDatabase *db,", "\n}\n", keep_start=1)]

parts = (here / "mmdb-alloc-test.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"mmdb-alloc-{tag}.c", scratch / f"mmdb-alloc-{tag}"
files = scratch / f"mmdb-alloc-{tag}-files"
c.write_text(prologue + "\n".join(pieces) + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) + (["-DSTATUS_RETURNED"] if status else []) +
               ["-std=gnu99", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-unused-but-set-variable", "-Wno-unused-value", "-Wno-deprecated-declarations",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)

env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
parts_to_run = ["file-list", "name-list"] if args.source_revision else [None]
for part in parts_to_run:
    run = subprocess.run([str(binary), str(files)] + ([part] if part else []),
                         env=env, capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    if args.source_revision:
        tail = "\n".join(run.stderr.strip().splitlines()[:4])
        if run.returncode == 0:
            print(f"NEGATIVE CONTROL FAILED: {label} passed the {part} injection test", file=sys.stderr)
            sys.exit(1)
        print(f"negative control ({label}, {part}): fails as expected, exit {run.returncode}\n{tail}")
    elif run.returncode:
        sys.stderr.write(run.stderr)
        print(f"FAIL ({label}): exit {run.returncode}", file=sys.stderr)
        sys.exit(1)
    else:
        print(f"PASS ({label})")
