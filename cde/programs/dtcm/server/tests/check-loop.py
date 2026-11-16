#!/usr/bin/env python3
"""Run the service loop tests of rpc.cmsd under the sanitizers.

check-loop.py SRCDIR

Environment, as `make check` supplies it (see programs/dtcm/server/Makefile.am):
  CMSD_COMPILE  the compile command of rpc.cmsd, flags included
  CMSD_LINK     the libtool link command of the build
  CMSD_LIBS     the libraries rpc.cmsd links
  CMSD_LOOP_SOURCE  directory to take cmsloop.c from (default SRCDIR)
The loop is compiled next to the test with the sanitizers (when the build
does not already have them); each case runs in its own process.
"""
import os
from pathlib import Path
import shlex
import shutil
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


srcdir = Path(sys.argv[1]).resolve()
loopdir = Path(os.environ.get("CMSD_LOOP_SOURCE", srcdir)).resolve()
compile_cmd = shlex.split(os.environ["CMSD_COMPILE"])
link_cmd = shlex.split(os.environ["CMSD_LINK"])
libs = shlex.split(os.environ.get("CMSD_LIBS", ""))
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="cmsd-loop-", dir=os.environ.get("TMPDIR")))

sanitize = [f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-omit-frame-pointer"]
if "clang" in Path(compile_cmd[0]).name:
    sanitize.append("-fno-sanitize=function")
if any(a.startswith("-fsanitize") for a in compile_cmd):
    sanitize = []


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=builddir, text=True, capture_output=True, **kw)


def fail(message):
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(message)


objects = []
for source in (srcdir / "tests" / "loop-test.c", loopdir / "cmsloop.c"):
    obj = work / (source.stem + ".o")
    r = run(compile_cmd + sanitize + ["-I" + str(srcdir), "-I" + str(loopdir),
                                      "-c", str(source), "-o", str(obj)])
    if r.returncode:
        fail("compile failed for %s:\n%s" % (source, r.stderr))
    objects.append(str(obj))

binary = work / "loop-test"
r = run(link_cmd + sanitize + ["-o", str(binary)] + objects + libs)
if r.returncode:
    fail("link failed:\n" + r.stderr)

env = dict(os.environ)
env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0"
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
cases = run([str(binary), "--list"], env=env).stdout.split()
failed = []
for case in cases:
    try:
        r = run([str(binary), case], env=env, timeout=60)
        rc, err = r.returncode, r.stderr
    except subprocess.TimeoutExpired:
        rc, err = 1, "timed out"
    status = "ok" if rc == 0 else "FAIL"
    print(f"{status:5} {case}")
    if rc:
        failed.append(case)
        print("\n".join("      " + line for line in (err or "").splitlines()[:10]))
print(f"{len(cases) - len(failed)}/{len(cases)} cases passed")
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed else 0)
