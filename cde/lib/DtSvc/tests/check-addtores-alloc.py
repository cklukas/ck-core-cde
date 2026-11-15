#!/usr/bin/env python3
"""Run the production _DtAddEntry() with realloc failing.

The Entry/Entries types and the functions _DtAllocEntries(), _DtFreeEntries()
and _DtAddEntry() are extracted verbatim from addToRes.c (the working tree,
or another revision with --source-revision REV) and compiled together with
addtores-alloc-test.c under AddressSanitizer and UndefinedBehaviorSanitizer
with halt on error.  Every extraction fails loudly unless its anchors occur
exactly once.

Default: the test must pass.
With --source-revision REV: negative control; REV must FAIL the same test
(exit status 0 means the failure was observed).
"""
import argparse
import atexit
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[3]
relative = "cde/lib/DtSvc/DtUtil2/addToRes.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of addToRes.c to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

if args.scratch:
    scratch = args.scratch
    scratch.mkdir(parents=True, exist_ok=True)
else:
    scratch = Path(tempfile.mkdtemp(prefix="addtores-alloc-", dir=os.environ.get("TMPDIR")))
    atexit.register(shutil.rmtree, scratch, True)

if args.source_revision:
    text = subprocess.run(["git", "-C", str(args.worktree), "show",
                           f"{args.source_revision}:{relative}"],
                          check=True, capture_output=True, text=True).stdout
    label = args.source_revision
else:
    text = (args.worktree / relative).read_text()
    label = "working tree"


def extract(start_anchor, end_anchor, keep_start=0):
    if text.count(start_anchor) != 1:
        sys.exit(f"anchor {start_anchor.strip()!r} found {text.count(start_anchor)} times in {label}, expected 1")
    start = text.index(start_anchor) + keep_start
    end = text.index(end_anchor, start) + len(end_anchor)
    piece = text[start:end]
    if piece.count("{") != piece.count("}"):
        sys.exit(f"extract at {start_anchor.strip()!r} from {label} does not look complete")
    return piece


add_anchor = ("\nstatic int\n_DtAddEntry(\n"
              if "\nstatic int\n_DtAddEntry(\n" in text
              else "\nstatic void \n_DtAddEntry(\n")
pieces = [extract("\ntypedef struct _Entry {", "} Entries;\n", keep_start=1),
          extract("\n#define INIT_ENTRY_SIZE", "\n", keep_start=1),
          extract("\nstatic Entries *\n_DtAllocEntries(", "\n}\n", keep_start=1),
          extract("\nstatic void\n_DtFreeEntries(", "\n}\n", keep_start=1),
          extract(add_anchor, "\n}\n", keep_start=1)]

parts = (here / "addtores-alloc-test.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"addtores-alloc-{tag}.c", scratch / f"addtores-alloc-{tag}"
c.write_text(prologue + "\n".join(pieces) + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
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
