#!/usr/bin/env python3
"""Run the rpc.ttdbserverd request-path regression tests under the sanitizers.

check-requests.py SRCDIR DBDIR

SRCDIR is the source directory of rpc.ttdbserverd, DBDIR that of the XDR layer
(lib/tt/lib/db).  Environment, as `make check` supplies it (see
lib/tt/bin/ttdbserverd/Makefile.am):
  TTDB_COMPILE  the compile command of rpc.ttdbserverd, flags included
  TTDB_LINK     the libtool link command of the build
  TTDB_OBJECTS  the object files of the built rpc.ttdbserverd
  TTDB_LIBS     the libraries rpc.ttdbserverd links
  TTDB_TEST_JOBS    how many compilations run at once (default 2)
  TTDB_TEST_CASES   run only these cases, a blank separated list of names
  TTDB_TEST_KEEP    keep the work directory (programs, files of the cases)
The test program includes the production db_server_svc.C; tt_isam_cache.C,
db_server_functions.C and the XDR layer db_server_xdr.C are compiled next to
it, and it is linked with the rest of the built rpc.ttdbserverd and the built
libraries.  Each case runs in its own process; any case that fails or crashes
fails the run.  The sources are compiled with the sanitizers when the build
does not already have them.
"""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

srcdir = Path(sys.argv[1]).resolve()
dbdir = Path(sys.argv[2]).resolve()
tests = srcdir / "tests"
compile_cmd = shlex.split(os.environ["TTDB_COMPILE"])
link_cmd = shlex.split(os.environ["TTDB_LINK"])
objects = os.environ["TTDB_OBJECTS"].split()
libs = shlex.split(os.environ.get("TTDB_LIBS", ""))
jobs = int(os.environ.get("TTDB_TEST_JOBS", "2"))

# The flags carry include and library paths relative to the build directory.
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="ttdb-requests-", dir=os.environ.get("TMPDIR")))

sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
if any(a.startswith("-fsanitize") for a in compile_cmd):
    sanitize = []                    # the build already carries them
else:
    probe = work / "probe.cc"
    probe.write_text("int main() { return 0; }\n")
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


# The test replaces these objects with its own compilation of the same code:
# db_server_svc.C is part of the test program itself.
instrumented = ("db_server_svc", "tt_isam_cache", "db_server_functions")
kept = [o for o in objects if o.rsplit("-", 1)[-1].split(".")[0] not in instrumented]
if len(kept) != len(objects) - len(instrumented):
    fail("unexpected object list: " + " ".join(objects))

sources = [tests / "requests-test.C", srcdir / "tt_isam_cache.C",
           srcdir / "db_server_functions.C", dbdir / "db_server_xdr.C"]


def compile_one(source):
    obj = work / (source.stem + ".o")
    r = run(compile_cmd + sanitize + ["-I" + str(srcdir), "-c", str(source),
                                      "-o", str(obj)])
    return source, obj, r


built = []
with ThreadPoolExecutor(max_workers=jobs) as pool:
    for source, obj, r in pool.map(compile_one, sources):
        if r.returncode:
            fail("compile failed for %s:\n%s" % (source, r.stderr))
        built.append(str(obj))

binary = work / "requests-test"
r = run(link_cmd + sanitize + ["-o", str(binary)] + built + kept + libs)
if r.returncode:
    fail("link failed:\n" + r.stderr)

env = dict(os.environ)
# No request may make the server allocate more than a message of the limits of
# the protocol takes (16 MiB): the allocator ends the case at a larger one.
env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=0:max_allocation_size_mb=64"
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
env["TTDB_TEST_DIR"] = str(work)
cases = run([str(binary), "--list"], env=env).stdout.split()
if not cases:
    fail("the test program lists no cases")
only = os.environ.get("TTDB_TEST_CASES")
if only:
    cases = [c for c in cases if c in only.split()]
failed = []
for case in cases:
    r = run([str(binary), case], env=env, timeout=300)
    status = "ok" if r.returncode == 0 else "FAIL"
    print(f"{status:5} {case}")
    if r.returncode:
        failed.append(case)
        print("\n".join("      " + line for line in (r.stderr or "").splitlines()[:12]))
print(f"{len(cases) - len(failed)}/{len(cases)} cases passed")
if os.environ.get("TTDB_TEST_KEEP"):
    print("kept " + str(work))
else:
    shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed else 0)
