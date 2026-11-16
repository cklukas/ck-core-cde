#!/usr/bin/env python3
"""Run the production front panel session reader on hostile record counts and
on missing home directories.

SessionHomeDirectory, SessionFileGetName, SessionReadCount, SessionRestoreData
and SessionDeleteAll are extracted verbatim from Session.c (or from a revision
named by --source-revision, to run the negative control against the unfixed
reader) and compiled with ASan/UBSan against the stand-ins in
check-session-counts.c.  Every scenario runs in its own process with a time
limit; the harness itself bounds the fscanf calls and the allocation sizes.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

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

def function(signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

start = text.index("static char * SESSION_FILE")
parts = [text[start:text.index(";", start) + 1]]
start = text.index("static char * TYPES_DIR")
parts.append(text[start:text.index(";", start) + 1])
# Pieces that only the fixed source has; the unfixed source lacks them.
match = re.search(r"^#define SESSION_MAX_RECORDS.*$", text, re.M)
if match:
    parts.append(match.group(0))
for signature in ("static const char *\nSessionHomeDirectory (void)",
                  "static char *\nSessionFileGetName (void)",
                  "static int\nSessionReadCount (FILE * fd)",
                  "void\nSessionRestoreData (void)",
                  "SessionDeleteAll(void)"):
    if signature in text:
        parts.append(function(signature if signature.startswith(("static", "void")) else "void\n" + signature))

SCENARIOS = ["home-set", "home-unset", "home-empty", "home-nowhere",
             "delete-home-set", "delete-home-unset",
             "fuzz-timeout", "fuzz-oom", "huge-counts", "negative-counts",
             "limit-edge", "malformed-records", "valid-file",
             "record-alloc-fails", "list-alloc-fails"]

with tempfile.TemporaryDirectory(prefix="dtwm-counts-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    reader = work / "session-reader.h"
    binary = work / "session-counts"
    reader.write_text("\n".join(parts) + "\n")
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   shlex.split(os.environ.get("CPPFLAGS", "")) +
                   ["-std=gnu23", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-Wno-unused-variable",
                    "-Wno-unused-but-set-variable", "-Wno-unused-function",
                    "-Wno-unused-result", "-Wno-sign-compare", "-Wno-missing-field-initializers",
                    "-Wno-deprecated-declarations", "-Wno-format-security",
                    f'-DSESSION_READER_H="{reader}"',
                    f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=undefined",
                    "-g", str(here / "check-session-counts.c"),
                    "-o", str(binary)], check=True)
    failed = []
    for scenario in SCENARIOS:
        scratch = work / scenario
        scratch.mkdir()
        try:
            run = subprocess.run([str(binary), scenario, str(scratch)], timeout=60,
                                 capture_output=True, text=True)
            status, output = run.returncode, run.stdout + run.stderr
        except subprocess.TimeoutExpired:
            status, output = "timeout", ""
        if status != 0:
            failed.append(scenario)
            print(f"FAIL {scenario} ({status})\n{output[:600]}")
        else:
            print(output.strip())
    if failed:
        sys.exit("failed: " + ", ".join(failed))
    print(f"session counts: {len(SCENARIOS)} scenarios passed")
