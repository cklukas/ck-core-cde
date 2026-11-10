#!/usr/bin/env python3
"""Check the termios string of the SPC protocol under the sanitizers.

check-spc-termio.py

Environment, as `make check` supplies it (see lib/DtSvc/Makefile.am):
  DTSVC_COMPILE  the compile command of libDtSvc, flags included
  DTSVC_LINK     the libtool link command of the build
  DTSVC_LIBS     the libraries to link, libDtSvc first
  SPC_SOURCE   directory with the copies of spc-termio.c and spc-error.c to
                 test (default DtEncap), e.g. an older revision; its include
                 directory, if any, comes before the headers of the build
The two production sources are compiled next to the test with the sanitizers
(when the build does not already have them); the rest of the SPC library is
the built one.  Each scenario runs in its own process.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
source = Path(os.environ.get("SPC_SOURCE") or here.parent / "DtEncap").resolve()
compile_cmd = shlex.split(os.environ["DTSVC_COMPILE"])
link_cmd = shlex.split(os.environ["DTSVC_LINK"])
libs = shlex.split(os.environ.get("DTSVC_LIBS", ""))
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="spc-termio-", dir=os.environ.get("TMPDIR")))
sanitize = [] if any(a.startswith("-fsanitize") for a in compile_cmd) else ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=builddir, text=True, capture_output=True, **kw)


def fail(message):
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(message)


old_encode = "int SPC_Encode_Termios" not in (source / "spc-termio.c").read_text()
objects = []
for path in (here / "spc-termio-test.c", source / "spc-termio.c", source / "spc-error.c"):
    obj = work / (path.stem + ".o")
    staged_headers = ["-I" + str(source / "include")] if (source / "include").is_dir() else []
    r = run(compile_cmd[:1] + staged_headers + compile_cmd[1:] + sanitize + (["-DOLD_ENCODE_RETURNS_VOID"] if old_encode else [])
            + ["-Wno-error", "-c", str(path), "-o", str(obj)])
    if r.returncode:
        fail("compile failed for %s:\n%s" % (path, "\n".join(l for l in r.stderr.splitlines() if "error" in l)[:2000]))
    objects.append(str(obj))
binary = work / "spc-termio-test"
r = run(link_cmd + sanitize + ["-o", str(binary)] + objects + libs)
if r.returncode:
    fail("link failed:\n" + r.stderr[-1500:])

env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:" + os.environ.get("ASAN_OPTIONS", ""),
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
cases = run([str(binary), "--list"], env=env).stdout.split()
failed = []
for case in cases:
    r = run([str(binary), case], env=env, timeout=120)
    print(("ok    " if r.returncode == 0 else "FAIL  ") + case)
    if r.returncode:
        failed.append(case)
        text = (r.stderr or "").splitlines()
        keep = [l for l in text if "ERROR:" in l or "SUMMARY:" in l or "runtime error" in l or "Assertion" in l]
        print("\n".join("      " + l for l in (keep or text[-4:])[:5]))
print("%d/%d cases passed" % (len(cases) - len(failed), len(cases)))
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed else 0)
