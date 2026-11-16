#!/usr/bin/env python3
"""Run the production growth of the SPC daemon's lists with realloc failing.

open_noio_channel_object() is extracted verbatim from noio.c, and
reserve_pid_slot() with its SPC_PID_RESERVED definition from spc-exec.c (the
working tree, or another revision with --source-revision REV).  They are
compiled together with check-spc-list-alloc.c under AddressSanitizer and
UndefinedBehaviorSanitizer with halt on error.  Every extraction fails
loudly unless its anchors occur exactly once.

Default: the test must pass.
With --source-revision REV: negative control for noio.c; REV must FAIL the
same test (exit status 0 means the failure was observed).  The pid list
growth was inline in the fork and exec function in such a revision and
cannot be extracted, so the control covers the logfile list only.
"""
import argparse
import os
from pathlib import Path
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
root = here.parents[4]

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of noio.c to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

scratch = args.scratch or args.worktree / "tests-scratch"
scratch.mkdir(parents=True, exist_ok=True)


def source(relative):
    if args.source_revision:
        return subprocess.run(["git", "-C", str(args.worktree), "show",
                               f"{args.source_revision}:{relative}"],
                              check=True, capture_output=True, text=True).stdout
    return (args.worktree / relative).read_text()


def extract(text, label, start_anchor, end_anchor="\n}\n", keep_start=0):
    if text.count(start_anchor) != 1:
        sys.exit(f"anchor {start_anchor.strip()!r} found {text.count(start_anchor)} times in {label}, expected 1")
    start = text.index(start_anchor) + keep_start
    end = text.index(end_anchor, start) + len(end_anchor)
    piece = text[start:end]
    if piece.count("{") != piece.count("}"):
        sys.exit(f"extract at {start_anchor.strip()!r} from {label} does not look complete")
    return piece


label = args.source_revision or "working tree"
noio_text = source("cde/lib/DtSvc/DtEncap/noio.c")
# The logfile is created by make_logfile() in a revision that has it.
noio = ""
if "static char * make_logfile (" in noio_text:
    noio = extract(noio_text, "noio.c", "static char * make_logfile (") + "\n"
noio += extract(noio_text, "noio.c",
                "\nSPC_Channel_Ptr open_noio_channel_object(", keep_start=1)
pid_part = ""
defines = []
if not args.source_revision:
    text = source("cde/lib/DtSvc/DtEncap/spc-exec.c")
    pid_part = (extract(text, "spc-exec.c", "\n#define SPC_PID_RESERVED", "\n\n", keep_start=1) +
                extract(text, "spc-exec.c", "\nstatic int\nreserve_pid_slot(", keep_start=1) + "\n")
    defines = ["-DTEST_PID_LIST"]

parts = (here / "check-spc-list-alloc.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"spc-list-alloc-{tag}.c", scratch / f"spc-list-alloc-{tag}"
c.write_text(prologue + pid_part + noio + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) + defines +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-deprecated-declarations", "-Wno-unused-value", "-Wno-unused-but-set-variable",
                f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)

env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
run = subprocess.run([str(binary), str(scratch)], env=env, capture_output=True, text=True)
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
