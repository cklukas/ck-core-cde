#!/usr/bin/env python3
"""Build dtpad's directory-selection module with the sanitizers and run its test.

    check-dirselect.py SRCDIR

DIRSELECT_CC names the compiler (default: cc).  The module and its test are
compiled from SRCDIR without Motif or Xt headers, so the check needs no display
and no built tree.  The test runs in a fresh temporary directory.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

src = Path(sys.argv[1]).resolve()
cc = os.environ.get("DIRSELECT_CC", "cc")
flags = ["-std=c23", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]

with tempfile.TemporaryDirectory(prefix="dirselect-") as tmp:
    exe = Path(tmp) / "dirselect-test"
    build = subprocess.run(
        [cc, *flags, "-I", str(src),
         str(src / "dirselect.c"), str(src / "tests" / "dirselect-test.c"),
         "-o", str(exe)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    if build.returncode:
        print(build.stdout, end="")
        print("check-dirselect: the module does not build with the sanitizers")
        sys.exit(1)
    run = subprocess.run(
        [str(exe)], cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False)
    print(run.stdout, end="")
    if run.returncode:
        print("check-dirselect: the directory selection test failed")
        sys.exit(run.returncode)
