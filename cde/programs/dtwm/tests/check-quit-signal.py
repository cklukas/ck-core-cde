#!/usr/bin/env python3
"""Check that dtwm quits and aborts from its signal handlers without unsafe work.

The signal state, AbortWmSignalHandler(), SetupWmSignalHandlers(),
QuitWmSignalProc(), SetupWmSignalDeferral() and QuitWmSignalHandler() are
extracted verbatim from WmSignal.c (the working tree, or another revision with
--source-revision REV) and compiled together with check-quit-signal.c under
AddressSanitizer and UndefinedBehaviorSanitizer with halt on error.  The Xlib,
Xt and window manager functions and _exit() are stand-ins that record whether
the signal is blocked in their calling context, which is the case only inside
the signal handler.  The test shows that the termination handler calls only
XtNoticeSignal() (the confirmation or Do_Quit_Mwm() run from the signal
procedure), that a second signal and the early window before the signal
procedure exists end with _exit(), and that the fatal signals are installed
with SA_RESETHAND and SA_NODEFER and the best-effort handler leaves with
_exit() when it is entered a second time.  A source guard checks that the
signal procedure is registered once, right after the application context is
created in WmInitWs.c.

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
root = here.parents[3]
relative = "cde/programs/dtwm/WmSignal.c"
init_relative = "cde/programs/dtwm/WmInitWs.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of WmSignal.c to use as a negative control")
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


def extract(start_anchor, end_anchor):
    """Return the text from start_anchor through end_anchor; the start must be unique."""
    n = text.count(start_anchor)
    if n != 1:
        sys.exit(f"anchor {start_anchor.strip()!r} found {n} times in {relative}, expected 1")
    start = text.index(start_anchor)
    end = text.index(end_anchor, start + len(start_anchor)) + len(end_anchor)
    return text[start:end] + "\n"


new = "QuitWmSignalProc" in text
production = ""
if new:
    production += extract("static XtSignalId quitSignalId = 0;\n", "abortEntered = 0;\n")
    production += extract("\nstatic void\nQuitWmSignalProc (", "END OF FUNCTION QuitWmSignalProc */\n")
    production += extract("\nvoid SetupWmSignalDeferral (", "END OF FUNCTION SetupWmSignalDeferral */\n")
production += extract("\nstatic void\nAbortWmSignalHandler (int sig)\n{",
                      "END OF FUNCTION AbortSignalHandler */\n")
production += extract("\nvoid QuitWmSignalHandler (int", "END OF FUNCTION QuitWmSignalHandler */\n")
production += extract("\nvoid SetupWmSignalHandlers (int", "END OF FUNCTION SetupWmSignalHandlers */\n")

parts = (here / "check-quit-signal.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]
define = "#define CK_NEW 1\n" if new else ""

failed = False
if new:
    init = (args.worktree / init_relative).read_text()
    ctx = init.find("wmGD.mwmAppContext = XtCreateApplicationContext();")
    reg = [m.start() for m in re.finditer(r"SetupWmSignalDeferral\s*\(\s*wmGD\.mwmAppContext\s*\)", init)]
    if ctx < 0 or len(reg) != 1 or not 0 < reg[0] - ctx < 120:
        print("SetupWmSignalDeferral() must be called once, right after XtCreateApplicationContext()",
              file=sys.stderr)
        failed = True

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"quit-signal-{tag}.c", scratch / f"quit-signal-{tag}"
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
if run.returncode:
    failed = True
if args.source_revision:
    tail = "\n".join(run.stderr.strip().splitlines()[:6])
    if not failed:
        print(f"NEGATIVE CONTROL FAILED: {label} passed the test", file=sys.stderr)
        sys.exit(1)
    print(f"negative control ({label}): fails as expected, exit {run.returncode}\n{tail}")
else:
    if failed:
        sys.stderr.write(run.stderr)
        print(f"FAIL ({label}): exit {run.returncode}", file=sys.stderr)
        sys.exit(1)
    print(f"PASS ({label})")
