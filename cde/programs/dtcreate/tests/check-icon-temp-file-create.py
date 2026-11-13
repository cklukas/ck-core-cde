#!/usr/bin/env python3
"""Check how dtcreate makes the files for the icons its icon editor sends back.

IconEdit_tt_handler() (with CreateIconTempFile(), where the revision has it)
is extracted verbatim from main.c (the working tree, or another revision with
--source-revision REV) and compiled together with
check-icon-temp-file-create.c under AddressSanitizer and
UndefinedBehaviorSanitizer with halt on error.  mkstemp and tmpnam are
replaced by stand-ins that return the same predictable first name, where the
test plants a symbolic link to a file it must not see changed.  The toolkit and
ToolTalk calls are stand-ins.

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
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[3]
TEST = "check-icon-temp-file-create.c"
TAG = "dtcreate-icon-temp-file"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

label = args.source_revision or "working tree"


def source(relative):
    """Text of a source file: the working tree, or the revision of the control."""
    if args.source_revision:
        return subprocess.run(["git", "-C", str(args.worktree), "show",
                               f"{args.source_revision}:{relative}"],
                              check=True, capture_output=True, text=True).stdout
    return (args.worktree / relative).read_text()


def extract(text, relative, start_anchor, end_anchor, required=True):
    """Return the text from start_anchor through end_anchor; start must be unique."""
    n = text.count(start_anchor)
    if n != 1:
        if n == 0 and not required:
            return ""
        sys.exit(f"anchor {start_anchor.strip()!r} found {n} times in {relative}, expected 1")
    start = text.index(start_anchor)
    end = text.index(end_anchor, start) + len(end_anchor)
    return text[start:end] + "\n"


def retarget(text, old, new, count):
    """Point a hard-coded temp directory at the test's directory."""
    if text.count(old) != count:
        sys.exit(f"{old!r} found {text.count(old)} times, expected {count}")
    return text.replace(old, new)


REL = "cde/programs/dtcreate/main.c"


def build():
    text = source(REL)
    helper = extract(text, REL, "static int CreateIconTempFile( char *name )", "\n}\n", required=False)
    handler = extract(text, REL, "Tt_callback_action IconEdit_tt_handler( Tt_message m, Tt_pattern p )",
                      "\n}\n")
    return {"CREATE": helper + handler}


def main(scratch):
    parts = (here / TEST).read_text()
    template = parts.split("/* @@PROLOGUE@@ */")[1]
    for key, value in build().items():
        template = template.replace(f"/* @@{key}@@ */", value)
    tag = TAG + ("-control" if args.source_revision else "-fixed")
    c, binary = scratch / f"{tag}.c", scratch / tag
    c.write_text(template)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                    "-Wno-deprecated-declarations", "-Wno-unused-value",
                    "-Wno-unused-but-set-variable", "-Wno-format-security",
                    "-Wno-unused-parameter",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    return subprocess.run([str(binary), str(scratch)], env=env, capture_output=True, text=True)


if args.scratch:
    args.scratch.mkdir(parents=True, exist_ok=True)
    run = main(args.scratch.resolve())
else:
    with tempfile.TemporaryDirectory(prefix=TAG + "-") as tmp:
        run = main(Path(tmp))
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
