#!/usr/bin/env python3
"""Run the dtspcd request-path regression tests under the sanitizers.

check-requests.py SRCDIR

Environment, as `make check` supplies it (see programs/dtspcd/Makefile.am):
  SPC_COMPILE  the compile command of dtspcd, flags included
  SPC_LINK     the libtool link command of the build
  SPC_LIBS     the libraries dtspcd links
The test program includes the production main.c and links the built libDtSvc.
Each case runs in its own process; any case that fails or crashes fails the
run.  Compiled without sanitizers when the build was not configured with them,
the cases still check results; the sanitizer cases are most useful with
`configure --enable-sanitizers`, and the driver adds the sanitizers itself
when the compiler accepts them.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

srcdir = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
tests = srcdir / "tests"
compile_cmd = shlex.split(os.environ["SPC_COMPILE"])
link_cmd = shlex.split(os.environ["SPC_LINK"])
libs = shlex.split(os.environ.get("SPC_LIBS", ""))

defines = []
if "Become_User" in (srcdir / "main.c").read_text():
    defines.append("-DNEW_REGISTER_HELPERS")

sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
if any(a.startswith("-fsanitize") for a in compile_cmd):
    sanitize = []                    # the build already carries them

# The flags carry include and library paths relative to the build directory.
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="spcd-requests-", dir=os.environ.get("TMPDIR")))


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=builddir, text=True, capture_output=True, **kw)


objects = []
for source in (tests / "requests-test.c", srcdir / "spcd_event.c"):
    obj = work / (source.stem + ".o")
    r = run(compile_cmd + defines + sanitize + ["-I" + str(srcdir), "-c", str(source), "-o", str(obj)])
    if r.returncode:
        sys.exit("compile failed:\n" + r.stderr)
    objects.append(str(obj))

binary = work / "requests-test"
r = run(link_cmd + sanitize + ["-o", str(binary)] + objects + libs)
if r.returncode:
    sys.exit("link failed:\n" + r.stderr)

env = dict(os.environ)
env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0"
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
cases = run([str(binary), "--list"], env=env).stdout.split()
failed = []
for case in cases:
    r = run([str(binary), case], env=env, timeout=60)
    status = "ok" if r.returncode == 0 else "FAIL"
    print(f"{status:5} {case}")
    if r.returncode:
        failed.append(case)
        print("\n".join("      " + line for line in (r.stderr or "").splitlines()[:12]))
print(f"{len(cases) - len(failed)}/{len(cases)} cases passed")
sys.exit(1 if failed else 0)
