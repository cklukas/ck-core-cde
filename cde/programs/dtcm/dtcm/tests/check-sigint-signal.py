#!/usr/bin/env python3
"""Check that dtcm runs its SIGINT logoff and exit outside signal context.

The signal state, sig_int_proc() and sig_int_handler() are extracted verbatim from calendarA.c
(the working tree, or another revision with --source-revision REV) and compiled
together with check-sigint-signal.c under AddressSanitizer and
UndefinedBehaviorSanitizer with halt on error.  csa_logoff(),
XtNoticeSignal() and exit()/_exit() are stand-ins that record whether the signal
is blocked in their calling context, which is the case only inside the signal
handler.  A source guard checks that the signal procedure is registered once,
after XtVaAppInitialize() returns and before the handler is installed.

Default: the test must pass.
With --source-revision REV: negative control; REV must FAIL the same test
(exit status 0 means the failure was observed).
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

here = Path(__file__).resolve().parent
root = here.parents[4]
relative = "cde/programs/dtcm/dtcm/calendarA.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of calendarA.c to use as a negative control")
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
handler = extract("\nstatic void\nsig_int_handler(\n", "\n}\n")
if "sig_int_proc" in text:
    production = (extract("static XtSignalId sig_int_id = 0;\n", "sig_int_requested = 0;\n") +
                  extract("\nstatic void\nsig_int_proc(\n", "\n}\n") + handler)
else:
    production = handler

parts = (here / "check-sigint-signal.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]
define = "#define CK_HAS_SIGNAL_PROC 1\n" if "sig_int_proc" in text else ""

if define:
    init = text.find("XtVaAppInitialize(&app,")
    reg = [m.start() for m in re.finditer(r"sig_int_id = XtAppAddSignal\(app,\s*sig_int_proc,", text)]
    inst = [m.start() for m in re.finditer(r"_DtSignal\(SIGINT, sig_int_handler\)", text)]
    if (text.count("XtVaAppInitialize(&app,") != 1 or len(reg) != 1 or reg[0] < init
            or len(inst) != 1 or inst[0] < reg[0]):
        sys.exit("sig_int_proc must be registered once, after XtVaAppInitialize() and before the handler is installed")

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"sigint-signal-{tag}.c", scratch / f"sigint-signal-{tag}"
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
