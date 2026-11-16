#!/usr/bin/env python3
"""Run the production XDMCP ForwardQuery handler on client port arrays of any length.

forward_respond is extracted verbatim from xdmcp.c (or from a revision named
by --source-revision, to run the negative control against the unfixed
handler) and compiled with ASan/UBSan against the stand-ins in
check-forward-port.c.  Set DTLOGIN_SOURCE to test another copy of the sources.
"""
import argparse
import os
from pathlib import Path
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
                    help="read xdmcp.c from this git revision instead of the working tree")
parser.add_argument("source", nargs="?", type=Path,
                    default=Path(os.environ.get("DTLOGIN_SOURCE") or here.parent) / "xdmcp.c")
args = parser.parse_args()

if args.source_revision:
    top = subprocess.run(["git", "-C", str(here), "rev-parse", "--show-toplevel"],
                         check=True, capture_output=True, text=True).stdout.strip()
    rel = Path(args.source).resolve().relative_to(top)
    text = subprocess.run(["git", "-C", top, "show", f"{args.source_revision}:{rel}"],
                          check=True, capture_output=True, text=True).stdout
else:
    text = args.source.read_text()

start = text.index("void\nforward_respond (")
end = text.index("\n}\n", start) + 3

with tempfile.TemporaryDirectory(prefix="dtlogin-forward-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    handler = work / "forward-respond.h"
    binary = work / "forward-port"
    handler.write_text(text[start:end])
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   shlex.split(os.environ.get("CPPFLAGS", "")) +
                   ["-std=gnu23", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    "-Wno-unused-function", "-Wno-sign-compare",
                    f'-DFORWARD_RESPOND_H="{handler}"',
                    f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=undefined",
                    "-g", str(here / "check-forward-port.c"), "-o", str(binary)], check=True)
    sys.exit(subprocess.run([str(binary)], timeout=60).returncode)
