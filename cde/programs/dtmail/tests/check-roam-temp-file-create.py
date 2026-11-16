#!/usr/bin/env python3
"""Check how the mailer creates the temporary files for printing and for saving as text.

PrintCmd::printjobcb(), PrintCmd::printit() and
SaveAsTextCmd::writeTextFromScrolledList() (with the helper create_temp_file(),
where the revision has it) are extracted verbatim from RoamCmds.C (the working
tree, or another revision with --source-revision REV) and compiled together
with check-roam-temp-file-create.cc under AddressSanitizer and
UndefinedBehaviorSanitizer with halt on error.  The list, windows, dialog,
print job and mailbox are small stand-ins; the stand-in for copySelected()
opens the name it is given with O_CREAT and without O_EXCL.  mkstemp and
tempnam are replaced by stand-ins that return the same predictable first name,
where the test plants a symbolic link to a file it must not see changed.

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
root = here.parents[3]
relative = "cde/programs/dtmail/dtmail/RoamCmds.C"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of RoamCmds.C to use as a negative control")
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
    """Return the text from start_anchor through end_anchor; start must be unique."""
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
    ("static char *\ncreate_temp_file(", "\n}\n", False),
    ("void\nPrintCmd::printjobcb( Widget w, XtPointer client_data, XtPointer )\n{", "\n}\n"),
    ("void\nPrintCmd::printit( int silent )\n{", "\n}\n"),
    ("void\nSaveAsTextCmd::writeTextFromScrolledList(int fd)\n{", "\n}\n"),
]

production = "".join(extract(*p) for p in pieces)

parts = (here / "check-roam-temp-file-create.cc").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
src, binary = scratch / f"roam-temp-file-create-{tag}.cc", scratch / f"roam-temp-file-create-{tag}"
src.write_text(prologue + production + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CXX", "c++")) +
               ["-std=gnu++17", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-deprecated-declarations", "-Wno-unused-value", "-Wno-writable-strings",
                "-Wno-write-strings", "-Wno-sometimes-uninitialized", "-Wno-unused-but-set-variable", "-Wno-format-security",
                f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(src), "-o", str(binary)], check=True)

env = dict(os.environ,
           ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0:alloc_dealloc_mismatch=0",
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
