#!/usr/bin/env python3
"""Check how dtpad saves its edit sessions when it gets a signal.

The signal state, handlers and setPanicConditions() are extracted verbatim from
main.c, and the emergency save from fileIo.c (the working tree, or another
revision with --source-revision REV), and compiled together with
check-panic-signal.c under AddressSanitizer and UndefinedBehaviorSanitizer
with halt on error.  The toolkit, the editor widget, sigaction() for the
installation checks and exit()/_exit() are stand-ins that record whether
SIGUSR1 is blocked in their calling context, which is the case only inside the
handlers under test.  Termination signals must only wake the event loop; the
fatal signals keep a best-effort save that leaves at once when it is entered a
second time.

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
base = "cde/programs/dtpad/"

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
main_c, fileio_c = source("main.c"), source("fileIo.c")


def extract(text, name, start_pattern, end_anchor="\n}\n"):
    """Return the text from the unique match of start_pattern through end_anchor
    (end_anchor None: the match itself)."""
    found = list(re.finditer(start_pattern, text))
    if len(found) != 1:
        sys.exit(f"pattern {start_pattern!r} found {len(found)} times in {name}, expected 1")
    if end_anchor is None:
        return found[0].group(0) + "\n"
    end = text.index(end_anchor, found[0].end()) + len(end_anchor)
    return text[found[0].start():end] + "\n"


new = "panicFatalHandler" in main_c
if new:
    production = (extract(fileio_c, "fileIo.c", r"static volatile sig_atomic_t panicSaving = 0;\n", None) +
                  extract(fileio_c, "fileIo.c", r"void \nPanicSaveEditSessions\(void\)\n\{") +
                  extract(fileio_c, "fileIo.c", r"void \nPanicSave\(void\)\n\{") +
                  extract(main_c, "main.c", r"static XtSignalId panicSignalId = 0;\n"
                          r"static volatile sig_atomic_t panicRequested = 0;\n", None) +
                  extract(main_c, "main.c", r"static void\npanicSignalProc\(") +
                  extract(main_c, "main.c", r"static void\npanicTerminateHandler\(") +
                  extract(main_c, "main.c", r"static void\npanicFatalHandler\(") +
                  extract(main_c, "main.c", r"static const int fatalSignals\[\] = \{", "};\n") +
                  extract(main_c, "main.c", r"void\nsetPanicConditions\(XtAppContext appContext\)\n\{"))
else:
    production = (extract(fileio_c, "fileIo.c", r"void \nPanicSave\(void\)\n\{") +
                  extract(main_c, "main.c", r"static void\npanicSignalHandler\(int signalNumber\)\n\{"))

parts = (here / "check-panic-signal.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]
define = "#define CK_HAS_FATAL_SPLIT 1\n" if new else ""

if new and main_c.count("setPanicConditions(pPad->app_context);") != 1:
    sys.exit("setPanicConditions() must be called once, with the application context of the pad")

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"dtpad-panic-signal-{tag}.c", scratch / f"dtpad-panic-signal-{tag}"
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
