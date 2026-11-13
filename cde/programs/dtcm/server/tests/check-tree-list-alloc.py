#!/usr/bin/env python3
"""Check the calendar daemon's tree and list with calloc failing.

rb_create() and rb_insert() are extracted verbatim from tree.c, and
hc_create(), hc_insert_node() and hc_insert() from list.c (the working tree,
or another revision with --source-revision REV).  They are compiled together
with check-tree-list-alloc.c under AddressSanitizer and
UndefinedBehaviorSanitizer with halt on error.  The harness makes the Nth
calloc fail and asserts the error result, the absence of a write through a
null pointer and of a leak.  Every extraction fails loudly unless its anchors
occur exactly once.

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
directory = "cde/programs/dtcm/server/"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

scratch = args.scratch or args.worktree / "tests-scratch"
scratch.mkdir(parents=True, exist_ok=True)
label = args.source_revision or "working tree"


def source(name):
    relative = directory + name
    if args.source_revision:
        return subprocess.run(["git", "-C", str(args.worktree), "show",
                               f"{args.source_revision}:{relative}"],
                              check=True, capture_output=True, text=True).stdout
    return (args.worktree / relative).read_text()


def extract(text, name, start_anchor, end_anchor):
    """Return the text from start_anchor through end_anchor; both must be unique."""
    n = text.count(start_anchor)
    if n != 1:
        sys.exit(f"anchor {start_anchor.strip()!r} found {n} times in {name}, expected 1")
    start = text.index(start_anchor)
    end = text.index(end_anchor, start) + len(end_anchor)
    return text[start:end] + "\n"


# Each function ends at its closing brace in column 0.
list_text, tree_text = source("list.c"), source("tree.c")
production = (
    "#define Private ListPrivate\n" +
    extract(list_text, "list.c", "typedef struct {\n\t_DtCmsGetKeyProc get;", "} Private;") +
    extract(list_text, "list.c", "extern Hc_list *\nhc_create(", "\n}\n") +
    extract(list_text, "list.c", "extern Rb_Status\nhc_insert_node (", "\n}\n") +
    extract(list_text, "list.c", "extern Rb_Status\nhc_insert(", "\n}\n") +
    "#undef Private\n#define Private TreePrivate\n" +
    extract(tree_text, "tree.c", "typedef struct {\n\tint size;", "\t} Private;") +
    extract(tree_text, "tree.c", "extern Rb_tree *\nrb_create (", "\n}\n") +
    extract(tree_text, "tree.c", "extern Rb_Status\nrb_insert(", "\n}\n") +
    "#undef Private\n")

parts = (here / "check-tree-list-alloc.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"tree-list-alloc-{tag}.c", scratch / f"tree-list-alloc-{tag}"
c.write_text(prologue + production + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-deprecated-declarations", "-Wno-unused-value", "-Wno-unused-parameter",
                "-Wno-unused-but-set-variable",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)

env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0:symbolize=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
run = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
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
