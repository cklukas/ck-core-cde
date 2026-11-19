#!/usr/bin/env python3
"""Build dtcm's ics_io RFC 5545 module with ASan/UBSan and run its unit tests."""
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


def _ck_sanitize_list() -> str:
    override = os.environ.get("CK_SANITIZE_LIST", "").strip()
    if override:
        return override
    for var in ("CC", "DIRSELECT_CC"):
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


here = Path(__file__).resolve().parent
dtcm_src = here.parent
cc = shlex.split(os.environ.get("CC", "cc"))
flags = ["-std=c23", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
         f"-fsanitize={_ck_sanitize_list()}", "-fno-sanitize-recover=all"]

with tempfile.TemporaryDirectory(prefix="dtcm-ics-", dir=os.environ.get("TMPDIR")) as tmp:
    exe = Path(tmp) / "ics-io-test"
    build = subprocess.run(
        [*cc, *flags, "-I", str(dtcm_src),
         str(dtcm_src / "ics_io.c"), str(here / "ics-io-test.c"),
         "-o", str(exe)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    if build.returncode:
        print(build.stdout, end="")
        print("check-ics-io: build failed")
        sys.exit(1)
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    run = subprocess.run(
        [str(exe)], cwd=tmp, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    print(run.stdout, end="")
    if run.returncode:
        print("check-ics-io: test failed")
        sys.exit(run.returncode)
