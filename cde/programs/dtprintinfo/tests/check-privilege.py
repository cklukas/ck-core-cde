#!/usr/bin/env python3
"""Check the changes of identity in dtprintinfo.

util/Privilege.h (the working tree, or another revision with --source-revision
REV) is compiled with check-privilege.c under AddressSanitizer and
UndefinedBehaviorSanitizer against a fake kernel that models a program
installed set-user-ID root: the real ID is the user's, the effective ID is
root.  The kernel can refuse a change (EPERM) or report success without
changing anything.  The test requires that giving up root ends the process
when it did not happen, and that the identity of a started program is
reported as failed when it was not reached.

A source check requires that the three users of the header (DtPrintinfo.C,
objects/PrintObj/ParseJobs.C, util/Invoke.C) include it and no longer call
seteuid() or setuid() themselves.

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
base = "cde/programs/dtprintinfo/"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

scratch = args.scratch or args.worktree / "tests-scratch"
scratch.mkdir(parents=True, exist_ok=True)


def source(relative):
    """Text of a file of the working tree or of the revision; None if absent."""
    if args.source_revision:
        run = subprocess.run(["git", "-C", str(args.worktree), "show",
                              f"{args.source_revision}:{base}{relative}"],
                             capture_output=True, text=True)
        return run.stdout if run.returncode == 0 else None
    path = args.worktree / (base + relative)
    return path.read_text() if path.exists() else None


label = args.source_revision or "working tree"
failures = []

users = ["DtPrintinfo.C", "objects/PrintObj/ParseJobs.C", "util/Invoke.C"]
for relative in users:
    text = source(relative)
    if text is None:
        sys.exit(f"{relative} is missing")
    code = re.sub(r"//.*|/\*.*?\*/", "", text, flags=re.S)
    if '#include "Privilege.h"' not in text:
        failures.append(f"{relative} does not include Privilege.h")
    if re.search(r"\b(?:set(?:e)?uid)\s*\(", code) or "SETEUID" in code:
        failures.append(f"{relative} changes the identity itself")

header = source("util/Privilege.h")
if header is None:
    failures.append("util/Privilege.h does not exist")
else:
    parts = (here / "check-privilege.c").read_text()
    prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
    body = parts.split("/* @@BODY@@ */")[1]
    tag = "control" if args.source_revision else "fixed"
    c, binary = scratch / f"privilege-{tag}.c", scratch / f"privilege-{tag}"
    c.write_text(prologue + header + "\n" + body)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu23", "-Wall", "-Wno-unused-function",
                    f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    run = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    if run.returncode:
        failures.append(run.stderr.strip().splitlines()[0] if run.stderr.strip() else f"exit {run.returncode}")

if args.source_revision:
    if not failures:
        print(f"NEGATIVE CONTROL FAILED: {label} passed the test", file=sys.stderr)
        sys.exit(1)
    print(f"negative control ({label}): fails as expected\n" + "\n".join(f"  {m}" for m in failures))
else:
    if failures:
        for m in failures:
            print(f"FAIL ({label}): {m}", file=sys.stderr)
        sys.exit(1)
    print(f"PASS ({label})")
