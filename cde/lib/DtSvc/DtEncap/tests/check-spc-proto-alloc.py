#!/usr/bin/env python3
"""Check the parsing of the SPC register and logfile data with allocations failing.

sscan_counted_string(), sscan_logfile_data() and sscan_register_data() are
extracted verbatim from spc-proto.c (the working tree, or another revision
with --source-revision REV) and compiled together with check-spc-proto-alloc.c
under AddressSanitizer and UndefinedBehaviorSanitizer with halt on error.
The harness makes each allocation fail in turn and asserts the error result,
the strings that are handed out and the absence of a leak.  Every extraction
fails loudly unless its anchors occur exactly once.

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
relative = "cde/lib/DtSvc/DtEncap/spc-proto.c"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of spc-proto.c to use as a negative control")
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
    """Return the text from start_anchor through end_anchor; both must be unique."""
    n = text.count(start_anchor)
    if n != 1:
        sys.exit(f"anchor {start_anchor.strip()!r} found {n} times in {relative}, expected 1")
    start = text.index(start_anchor)
    end = text.index(end_anchor, start) + len(end_anchor)
    return text[start:end] + "\n"


# Each piece ends at its closing brace in column 0 (braces in character
# literals make brace counting unreliable).
pieces = [
    ("#define SPC_MAX_COUNTED_STRINGS", "\n"),
    ("static void SPC_Free_Vector(", "\n}\n"),
    ("XeString *sscan_counted_string(", "\n}\n"),
    ("int\nsscan_logfile_data(", "\n}\n"),
    ("int\nsscan_register_data(", "\n}\n"),
]
production = "".join(extract(*p) for p in pieces)

parts = (here / "check-spc-proto-alloc.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"spc-proto-alloc-{tag}.c", scratch / f"spc-proto-alloc-{tag}"
c.write_text(prologue + production + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-deprecated-declarations", "-Wno-unused-value",
                "-Wno-unused-but-set-variable", "-Wno-format-security", "-Wno-pointer-sign",
                f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
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
