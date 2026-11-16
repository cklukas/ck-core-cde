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


src = Path(sys.argv[1]).resolve()
cc = os.environ.get("DIRSELECT_CC", "cc")
flags = ["-std=c23", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
         f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all"]

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
