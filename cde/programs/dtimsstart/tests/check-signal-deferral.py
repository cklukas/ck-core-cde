#!/usr/bin/env python3
"""Check that dtimsstart ends and reaps outside signal context.

The signal state and handlers are extracted verbatim from main.c (sigExit(),
ximsHandleDeferredSignals()) and start.c (on_sig_chld(),
ximsHandleChildExit()) of the working tree, or of another revision with
--source-revision REV, and compiled together with check-signal-deferral.c
under AddressSanitizer and UndefinedBehaviorSanitizer with halt on error.
Exit(), put_xims_log(), ximsWaitDone() and the wake-up of the event loop are
stand-ins that record whether the signal is blocked in their calling context,
which is the case only inside a signal handler.  The child handler is run for
a real forked child.  A source guard checks that the Xt signal procedure is
registered after the application context exists.

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
base = "cde/programs/dtimsstart/"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

scratch = args.scratch or args.worktree / "tests-scratch"
scratch.mkdir(parents=True, exist_ok=True)


def source(name):
    if args.source_revision:
        return subprocess.run(["git", "-C", str(args.worktree), "show",
                               f"{args.source_revision}:{base}{name}"],
                              check=True, capture_output=True, text=True).stdout
    return (args.worktree / base / name).read_text()


label = args.source_revision or "working tree"
main_c, start_c, win_c = source("main.c"), source("start.c"), source("win.c")


def extract(text, name, start_pattern, end_anchor="\n}\n"):
    # end_anchor None: the match itself is the whole text
    """Return the text from the unique match of start_pattern through end_anchor."""
    found = list(re.finditer(start_pattern, text))
    if len(found) != 1:
        sys.exit(f"pattern {start_pattern!r} found {len(found)} times in {name}, expected 1")
    if end_anchor is None:
        return found[0].group(0)
    end = text.index(end_anchor, found[0].end()) + len(end_anchor)
    return text[found[0].start():end] + "\n"


new = "ximsHandleDeferredSignals" in main_c
if new:
    production = (extract(start_c, "start.c", r"static volatile sig_atomic_t\tchildExitPending = 0;\n"
                          r"static volatile pid_t\tchildExitPid = 0;\n"
                          r"static volatile int\tchildExitStatus = 0;\n", None) +
                  extract(start_c, "start.c", r"static void\ton_sig_chld\(int sig\)\n\{") +
                  extract(start_c, "start.c", r"void\tximsHandleChildExit\(void\)\n\{") +
                  extract(main_c, "main.c", r"static volatile sig_atomic_t\texitSignalNumber = 0;\n", None) +
                  extract(main_c, "main.c", r"void\tsigExit\(int sig\)\n\{") +
                  extract(main_c, "main.c", r"void\tximsHandleDeferredSignals\(void\)\n\{"))
    appc = win_c.find("Dpy = dpy; TopW = topW; appC = app;")
    reg = [m.start() for m in re.finditer(r"XtAppAddSignal\(app,\s*deferred_signal_proc,", win_c)]
    if appc < 0 or len(reg) != 1 or reg[0] < appc:
        sys.exit("deferred_signal_proc must be registered once, after the application context exists")
else:
    production = (extract(main_c, "main.c", r"static int\texitSignalNumber = 0;\n", None) +
                  extract(start_c, "start.c", r"static void\ton_sig_chld\(int sig\)\n\{") +
                  extract(main_c, "main.c", r"void\tsigExit\(int sig\)\n\{"))

parts = (here / "check-signal-deferral.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]
define = "#define CK_HAS_DEFERRAL 1\n" if new else ""

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"signal-deferral-{tag}.c", scratch / f"signal-deferral-{tag}"
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
