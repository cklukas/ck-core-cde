#!/usr/bin/env python3
"""Compile and execute the production Trash parser and its C regression."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="trash-metadata-", dir=os.environ["TMPDIR"]) as work:
    binary = Path(work) / "metadata"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", str(source),
                    str(source / "TrashMetadata.c"), str(source / "tests/trash-metadata.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
