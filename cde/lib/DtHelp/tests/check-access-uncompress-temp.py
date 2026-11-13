#!/usr/bin/env python3
"""Check how DtHelp creates the temporary file of an uncompressed help file.

FileOpenRtnFd() and _DtHelpCeGetUncompressedFileName() (with the helper
CreateTempFileName(), where the revision has it) are extracted verbatim from
Access.c (the working tree, or another revision with --source-revision REV)
and compiled together with check-access-uncompress-temp.c under
AddressSanitizer and UndefinedBehaviorSanitizer with halt on error.  mkstemp
and tmpnam are replaced by stand-ins that return the same predictable first
name, where the test plants a symbolic link to a file it must not see
changed.  The uncompressor is replaced by a stand-in that opens the output
name the way _DtHelpCeUncompressFile() does.

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
relative = "cde/lib/DtHelp/Access.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of Access.c to use as a negative control")
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


def extract(start_anchor, end_anchor, required=True):
    """Return the text from start_anchor through end_anchor; both must be unique."""
    n = text.count(start_anchor)
    if n != 1:
        if n == 0 and not required:
            return ""
        sys.exit(f"anchor {start_anchor.strip()!r} found {n} times in {relative}, expected 1")
    start = text.index(start_anchor)
    end = text.index(end_anchor, start) + len(end_anchor)
    return text[start:end] + "\n"


# Each piece ends at its closing brace in column 0.
pieces = [
    ("static int\nCreateTempFileName (", "\n}\n", False),
    ("static int\nFileOpenRtnFd (", "\n} /* End FileOpenRtnFd */\n"),
    ("\nint\n_DtHelpCeGetUncompressedFileName (", "\n}\n"),
]
production = "".join(extract(*p) for p in pieces)

parts = (here / "check-access-uncompress-temp.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"access-uncompress-temp-{tag}.c", scratch / f"access-uncompress-temp-{tag}"
c.write_text(prologue + production + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-deprecated-declarations", "-Wno-unused-value",
                "-Wno-unused-but-set-variable", "-Wno-format-security",
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
