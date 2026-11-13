#!/usr/bin/env python3
"""Run the production front panel session reader on over-long words.

SessionFileGetName and SessionRestoreData are extracted verbatim from
Session.c (or from a revision named by --source-revision, to run the
negative control against the unfixed reader) and compiled with ASan/UBSan
against the stand-ins in check-session-word-width.c.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument("--source-revision", metavar="REV",
                    help="read Session.c from this git revision instead of the working tree")
parser.add_argument("source", nargs="?", type=Path, default=here.parent / "Session.c")
args = parser.parse_args()

if args.source_revision:
    top = subprocess.run(["git", "-C", str(here), "rev-parse", "--show-toplevel"],
                         check=True, capture_output=True, text=True).stdout.strip()
    rel = Path(args.source).resolve().relative_to(top)
    text = subprocess.run(["git", "-C", top, "show", f"{args.source_revision}:{rel}"],
                          check=True, capture_output=True, text=True).stdout
else:
    text = args.source.read_text()

start = text.index("static char * SESSION_FILE")
functions = [text[start:text.index(";", start) + 1]]
for signature in ("static char *\nSessionFileGetName (void)",
                  "void\nSessionRestoreData (void)"):
    start = text.index(signature)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    functions.append(text[start:end])

with tempfile.TemporaryDirectory(prefix="dtwm-session-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    reader = work / "session-reader.h"
    binary = work / "session-width"
    home = work / "home"
    home.mkdir()
    reader.write_text("\n".join(functions) + "\n")
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   shlex.split(os.environ.get("CPPFLAGS", "")) +
                   ["-std=gnu23", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-Wno-unused-variable",
                    "-Wno-unused-but-set-variable",
                    "-Wno-unused-result", "-Wno-sign-compare", "-Wno-missing-field-initializers", "-Wno-deprecated-declarations", "-Wno-format-security",
                    f'-DSESSION_READER_H="{reader}"',
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                    "-g", str(here / "check-session-word-width.c"),
                    "-o", str(binary)], check=True)
    sys.exit(subprocess.run([str(binary), str(home)]).returncode)
