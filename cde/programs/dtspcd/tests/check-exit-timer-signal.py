#!/usr/bin/env python3
"""Check that the exit timer of dtspcd does its work outside signal context.

SPCD_Alarm_Handler(), SPCD_Alarm_Pipe_Open() and SPCD_Alarm_Work() (with the
pipe descriptors) are extracted verbatim from main.c (the working tree, or
another revision with --source-revision REV) and compiled together with
check-exit-timer-signal.c under AddressSanitizer and UndefinedBehaviorSanitizer
with halt on error.  The logging and exit functions are stand-ins that record
whether SIGALRM is blocked in their calling context, which is the case only
inside the signal handler.

Default: the test must pass.
With --source-revision REV: negative control; REV must FAIL the same test
(exit status 0 means the failure was observed).  A revision without the pipe
code is tested with the handler alone.
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


def extract(start_anchor, end_anchor, required=True):
    """Return the text from start_anchor through end_anchor; the start must be unique."""
    n = text.count(start_anchor)
    if n != 1:
        if n == 0 and not required:
            return ""
        sys.exit(f"anchor {start_anchor.strip()!r} found {n} times in {relative}, expected 1")
    start = text.index(start_anchor)
    end = text.index(end_anchor, start + len(start_anchor)) + len(end_anchor)
    return text[start:end] + "\n"


# Functions end at their closing brace in column 0.
pipe_pieces = [
    ("\nstatic int SPCD_alarm_pipe[2] = {-1, -1};", "\n"),
    ("\nint SPCD_Alarm_Pipe_Open(void)\n", "\n}\n"),
    ("\nvoid SPCD_Alarm_Work(void *data, int *fd, SbInputId *id)\n", "\n}\n"),
]
has_pipe = text.count("SPCD_Alarm_Pipe_Open") > 0
# A revision that still has the old handler guards its signature with
# SA_HANDLER_INT_ARG; the stand-ins define it.
handler = extract("\nvoid SPCD_Alarm_Handler(int not_used)\n" if has_pipe else
                  "\n#ifdef SA_HANDLER_INT_ARG\nvoid SPCD_Alarm_Handler(int not_used)\n",
                  "\n}\n")
if args.source_revision and not has_pipe:
    production = handler
else:
    # the pipe declaration must precede the handler that uses it
    production = (extract(*pipe_pieces[0]) + handler +
                  "".join(extract(*p) for p in pipe_pieces[1:]))

parts = (here / "check-exit-timer-signal.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]
define = "#define CK_HAS_ALARM_PIPE 1\n" if "SPCD_Alarm_Pipe_Open" in production else ""

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"exit-timer-signal-{tag}.c", scratch / f"exit-timer-signal-{tag}"
c.write_text(prologue + production + "\n" + define + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-unused-value", "-Wno-unused-but-set-variable",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)

env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
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
