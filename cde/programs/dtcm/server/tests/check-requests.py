#!/usr/bin/env python3
"""Run the rpc.cmsd request-path regression tests under the sanitizers.

check-requests.py SRCDIR CSA_SRCDIR CSA_BUILDDIR

Environment, as `make check` supplies it (see programs/dtcm/server/Makefile.am):
  CMSD_COMPILE  the compile command of rpc.cmsd, flags included
  CMSD_LINK     the libtool link command of the build
  CMSD_OBJECTS  the object files of the built rpc.cmsd
  CMSD_LIBS     the libraries rpc.cmsd links
The test program includes the production log.c and cmsfunc.c; the other sources
the tests exercise (rtable4.c, cmsconvert.c, cmscalendar.c, lexit.c, lookup.c,
reminder.c, garbage.c, reutil.c, rerule.c, access.c of rpc.cmsd; cmxdr.c, lutil.c, attr.c, match.c, iso8601.c, rescan.c,
updateattrs.c and the generated reparser.c of libcsa) are compiled next to it, and it is linked with
the rest of the built rpc.cmsd and libcsa.  Each case runs in
its own process; any case that fails or crashes fails the run.  The sources
are compiled with the sanitizers when the build does not already have them.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

srcdir = Path(sys.argv[1]).resolve()
csa_src = Path(sys.argv[2]).resolve()
csa_build = Path(sys.argv[3]).resolve()
tests = srcdir / "tests"
compile_cmd = shlex.split(os.environ["CMSD_COMPILE"])
link_cmd = shlex.split(os.environ["CMSD_LINK"])
objects = os.environ["CMSD_OBJECTS"].split()
libs = shlex.split(os.environ.get("CMSD_LIBS", ""))

# The flags carry include and library paths relative to the build directory.
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="cmsd-requests-", dir=os.environ.get("TMPDIR")))

sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
if any(a.startswith("-fsanitize") for a in compile_cmd):
    sanitize = []                    # the build already carries them
else:
    probe = work / "probe.c"
    probe.write_text("int main(void) { return 0; }\n")
    r = subprocess.run(compile_cmd + sanitize + ["-o", str(work / "probe"), str(probe)],
                       cwd=builddir, text=True, capture_output=True)
    if r.returncode:
        print("the compiler has no sanitizers: checking results only")
        sanitize = []


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=builddir, text=True, capture_output=True, **kw)


def fail(message):
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(message)


# The test replaces these objects with its own compilation of the same code.
instrumented = ("rtable4", "cmsconvert", "cmscalendar", "lexit", "lookup",
                "reminder", "garbage", "reutil", "rerule", "access")
replaced = {"log", "cmsfunc", "svcmain", *instrumented}
kept = [o for o in objects if o.rsplit("-", 1)[-1].split(".")[0] not in replaced]
if len(kept) != len(objects) - len(replaced):
    fail("unexpected object list: " + " ".join(objects))

built = []
for source in (tests / "requests-test.c",
               *(srcdir / (name + ".c") for name in instrumented),
               csa_src / "cmxdr.c", csa_src / "lutil.c", csa_src / "attr.c",
               csa_src / "match.c", csa_src / "iso8601.c", csa_src / "rescan.c",
               csa_src / "updateattrs.c", csa_build / "reparser.c"):
    obj = work / (source.stem + ".o")
    r = run(compile_cmd + sanitize + ["-I" + str(srcdir), "-I" + str(csa_src),
                                      "-I" + str(csa_build), "-c", str(source),
                                      "-o", str(obj)])
    if r.returncode:
        fail("compile failed for %s:\n%s" % (source, r.stderr))
    built.append(str(obj))

binary = work / "requests-test"
r = run(link_cmd + sanitize + ["-o", str(binary)] + built + kept + libs)
if r.returncode:
    fail("link failed:\n" + r.stderr)

env = dict(os.environ)
env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0"
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
env["CMSD_TEST_DIR"] = str(work)
cases = run([str(binary), "--list"], env=env).stdout.split()
failed = []
for case in cases:
    r = run([str(binary), case], env=env, timeout=120)
    status = "ok" if r.returncode == 0 else "FAIL"
    print(f"{status:5} {case}")
    if r.returncode:
        failed.append(case)
        print("\n".join("      " + line for line in (r.stderr or "").splitlines()[:12]))
print(f"{len(cases) - len(failed)}/{len(cases)} cases passed")
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed else 0)
