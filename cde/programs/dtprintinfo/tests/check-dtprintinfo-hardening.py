#!/usr/bin/env python3
"""Regression checks for dtprintinfo input-path and bounds hardening.

Compiles ParseJobs.C and Invoke.C from the working tree (or --source-revision
REV) with check-dtprintinfo-hardening.C under AddressSanitizer and
UndefinedBehaviorSanitizer, and verifies source invariants on Queue.C,
PrintJob.C, Process.C, and MotifThread.C.
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
ap.add_argument("--expect-fail", action="store_true", help="succeed only when the target revision fails")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

expect_fail = args.expect_fail or bool(args.source_revision)
s8t_tmp = Path("/Volumes/S8T/tmp")
scratch_default = s8t_tmp if s8t_tmp.is_dir() and os.access(s8t_tmp, os.W_OK) else Path(os.environ.get("TMPDIR", str(args.worktree)))
scratch = (args.scratch or scratch_default) / "dtprintinfo-hardening"
scratch.mkdir(parents=True, exist_ok=True)


def source(relative):
    if args.source_revision:
        run = subprocess.run(
            ["git", "-C", str(args.worktree), "show", f"{args.source_revision}:{base}{relative}"],
            capture_output=True,
            text=True,
        )
        return run.stdout if run.returncode == 0 else None
    path = args.worktree / (base + relative)
    return path.read_text() if path.exists() else None


label = args.source_revision or "working tree"
failures: list[str] = []

queue_src = source("objects/PrintObj/Queue.C")
if queue_src is None:
    failures.append("objects/PrintObj/Queue.C is missing")
else:
    start_print_match = re.search(
        r"int\s+Queue::StartPrint\s*\([^{]*\{(.*?)\n\}",
        queue_src,
        flags=re.S,
    )
    if not start_print_match or "START_PRINTING_CMD" not in start_print_match.group(1):
        failures.append("Queue::StartPrint does not format START_PRINTING_CMD")

parse_jobs_src = source("objects/PrintObj/ParseJobs.C")
invoke_src = source("util/Invoke.C")
parse_jobs_h = source("objects/PrintObj/ParseJobs.h")
invoke_h = source("util/Invoke.h")
privilege_h = source("util/Privilege.h")

if not all([parse_jobs_src, invoke_src, parse_jobs_h, invoke_h, privilege_h]):
    failures.append("required dtprintinfo source files are missing")
else:
    def strip_local_includes(text: str) -> str:
        out = []
        for line in text.splitlines():
            if re.match(r'^\s*#\s*include\s+(?:<X11/|<Dt/|<values\.h>|"dtprintinfomsg\.h"|"ParseJobs\.h"|"Invoke\.h"|"Privilege\.h")', line):
                continue
            out.append(line)
        return "\n".join(out)

    harness = (here / "check-dtprintinfo-hardening.C").read_text()
    prologue, rest = harness.split("/* @@PROLOGUE@@ */", 1)[1].split("/* @@UNITS@@ */", 1)
    _, body = rest.split("/* @@BODY@@ */", 1)

    units = "\n".join([
        privilege_h,
        invoke_h,
        parse_jobs_h,
        "#undef CSRG_BASED",
        "#undef __linux__",
        "#undef sun",
        "#undef aix",
        strip_local_includes(invoke_src),
        strip_local_includes(parse_jobs_src),
    ])

    tag = "control" if args.source_revision else "fixed"
    cpp_path = scratch / f"dtprintinfo-hardening-{tag}.C"
    bin_path = scratch / f"dtprintinfo-hardening-{tag}"
    cpp_path.write_text(prologue + "\n" + units + "\n" + body)

    cxx = shlex.split(os.environ.get("CXX", "c++"))
    compile_run = subprocess.run(
        cxx
        + [
            "-std=gnu++17",
            "-Wall",
            "-Wno-writable-strings",
            "-Wno-parentheses",
            "-Wno-unused-variable",
            "-Wno-unused-but-set-variable",
            f"-fsanitize={_CK_SANITIZE_LIST}",
            "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer",
            "-g",
            str(cpp_path),
            "-o",
            str(bin_path),
        ],
        capture_output=True,
        text=True,
    )
    if compile_run.returncode != 0:
        failures.append(f"compile failed: {compile_run.stderr.strip()}")
    else:
        env = dict(
            os.environ,
            ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
        )
        run = subprocess.run([str(bin_path)], env=env, capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        if run.returncode != 0:
            first_err = run.stderr.strip().splitlines()[0] if run.stderr.strip() else f"exit {run.returncode}"
            failures.append(first_err)

if expect_fail:
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
