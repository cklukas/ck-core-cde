#!/usr/bin/env python3
"""Build the production file-copy routines and execute I/O fault fixtures."""
import os
from pathlib import Path
import shlex
import subprocess
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


source = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="dtfile-copy-", dir=os.environ["TMPDIR"]) as work:
    root = Path(work)
    files = root / "files"
    files.mkdir()
    for test in ("copy-transfer", "copy-paths"):
        binary = root / test
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                       ["-std=gnu23", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-parentheses", "-Wno-unused-function",
                        f"-fsanitize={_CK_SANITIZE_LIST}", "-g", "-I", str(source),
                        str(source / ("tests/" + test + ".c")), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(files)], check=True)
