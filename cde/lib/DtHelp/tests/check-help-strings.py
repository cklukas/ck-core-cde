#!/usr/bin/env python3
"""Compile the production canvas string implementation with its regression."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="help-strings-", dir=os.environ["TMPDIR"]) as work:
    binary = Path(work) / "strings"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", str(source),
                    str(source / "CvString.c"), str(source / "tests/help-strings.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
