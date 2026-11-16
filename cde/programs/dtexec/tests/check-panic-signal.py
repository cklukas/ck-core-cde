#!/usr/bin/env python3
"""Check that dtexec shuts down after SIGINT outside signal context.

The signal state, PanicShutdown() and PanicSignal() are extracted verbatim
from Main.c (the working tree, or another revision with --source-revision REV)
and compiled together with check-panic-signal.c under AddressSanitizer and
UndefinedBehaviorSanitizer with halt on error.  DoneRequest(), chmod(),
unlink() and _exit() are stand-ins that record whether the signal is blocked
in their calling context, which is the case only inside the signal handler.
A source guard checks that the handler is installed after the self-pipe exists
and that the select loop acts on the request.

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

def _ck_sanitize_list() -> str:
    import os, shlex, subprocess, tempfile
    override = os.environ.get("CK_SANITIZE_LIST", "").strip()
    if override:
        return override
    for var in ("CC", "CXX", "CLANG", "TT_COMPILE", "CSA_COMPILE", "CMSD_COMPILE",
                "SPC_COMPILE", "DTSVC_COMPILE", "DTSR_COMPILE", "DTMAIL_CC",
                "DTMAIL_COMPILE", "DIRSELECT_CC"):
        val = os.environ.get(var, "").strip()
        if val:
            cc = shlex.split(val)[:1]
            break
    else:
        cc = ["cc"]
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
        src, exe = os.path.join(d, "p.c"), os.path.join(d, "p")
        with open(src, "w", encoding="utf-8") as f:
            f.write("int main(void) { return 0; }\n")
        for s in ("address,undefined", "undefined"):
            r = subprocess.run(cc + [f"-fsanitize={s}", src, "-o", exe], capture_output=True)
            if r.returncode == 0 and subprocess.run([exe], capture_output=True).returncode == 0:
                return s
    return "undefined"


_CK_SANITIZE_LIST = _ck_sanitize_list()


here = Path(__file__).resolve().parent
root = here.parents[3]
relative = "cde/programs/dtexec/Main.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of Main.c to use as a negative control")
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


def extract(start_pattern, end_anchor="\n}\n"):
    """Return the text from the unique match of start_pattern through end_anchor."""
    found = list(re.finditer(start_pattern, text))
    if len(found) != 1:
        sys.exit(f"pattern {start_pattern!r} found {len(found)} times in {relative}, expected 1")
    start = found[0].start()
    end = text.index(end_anchor, found[0].end()) + len(end_anchor)
    return text[start:end] + "\n"


new = "PanicShutdown" in text
if new:
    production = (extract(r"static volatile sig_atomic_t panicRequestedG = 0;\n", "panicPipeG[2] = { -1, -1 };\n") +
                  extract(r"static void\nPanicShutdown\(void\)\n") +
                  extract(r"static void\nPanicSignal\(int s\)\n"))
else:
    production = extract(r"static void\n(?:#if[^\n]*\n)?PanicSignal\(int s\)\n")

parts = (here / "check-panic-signal.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]
define = "#define CK_HAS_PANIC_SHUTDOWN 1\n" if new else ""

if new:
    pipe = text.find("pipe(panicPipeG)")
    install = text.find("InitializeSignalHandling ();")
    loop = text.find("for (;;) {")
    check = text.find("if (panicRequestedG) {", loop)
    if min(pipe, install, loop, check) < 0 or text.count("pipe(panicPipeG)") != 1 \
            or text.count("InitializeSignalHandling ();") != 1 or not pipe < install < loop < check \
            or "BITSET(allactivefdsG, panicPipeG[0])" not in text:
        sys.exit("the self-pipe must be created before the handlers, watched by select() "
                 "and the loop must call PanicShutdown() when panicRequestedG is set")

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"panic-signal-{tag}.c", scratch / f"panic-signal-{tag}"
c.write_text(prologue + production + "\n" + define + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-unused-value", "-Wno-unused-but-set-variable",
                f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
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
