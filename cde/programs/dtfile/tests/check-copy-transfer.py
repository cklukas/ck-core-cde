#!/usr/bin/env python3
"""Build the production file-copy routines and execute I/O fault fixtures."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="dtfile-copy-", dir=os.environ["TMPDIR"]) as work:
    root = Path(work)
    files = root / "files"
    files.mkdir()
    for test in ("copy-transfer", "copy-paths"):
        binary = root / test
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                       ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-parentheses", "-Wno-unused-function",
                        "-fsanitize=address,undefined", "-g", "-I", str(source),
                        str(source / ("tests/" + test + ".c")), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(files)], check=True)
