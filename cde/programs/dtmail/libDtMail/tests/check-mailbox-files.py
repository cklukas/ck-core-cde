#!/usr/bin/env python3
"""Run the libDtMail mailbox file and path tests under the sanitizers.

check-mailbox-files.py SRCDIR ARCHIVE

SRCDIR is the source directory of libDtMail, ARCHIVE the built libDtMail.a.
Environment, as `make check` supplies it (see libDtMail/Makefile.am):
  DTMAIL_COMPILE  the compile command of libDtMail, flags included
  DTMAIL_LINK     the libtool link command of the build
  DTMAIL_LIBS     the libraries dtmailpr links
  DTMAIL_STAGED   a directory with Common/Session.C and RFC/RFCMailBox.C of
                  another revision, compiled instead of the sources in SRCDIR
                  (a revision with a defect is how the test is checked itself)
  DTMAIL_TEST_CASES  run only these cases, a blank separated list of names
  DTMAIL_TEST_KEEP   keep the work directory
The production Session.C and RFCMailBox.C are compiled with the sanitizers
next to tests/mailbox-files.C and linked with the rest of the built libDtMail.
A mailbox of the test is made in an empty directory per case and each case runs
in its own process; a case that fails or crashes fails the run.
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
archive = Path(sys.argv[2]).resolve()
staged = Path(os.environ["DTMAIL_STAGED"]).resolve() if os.environ.get("DTMAIL_STAGED") else srcdir
compile_cmd = shlex.split(os.environ["DTMAIL_COMPILE"])
link_cmd = shlex.split(os.environ["DTMAIL_LINK"])
libs = shlex.split(os.environ.get("DTMAIL_LIBS", ""))
jobs = int(os.environ.get("DTMAIL_TEST_JOBS", "2"))

builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="dtmail-mailbox-", dir=os.environ.get("TMPDIR")))
sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer", "-g"]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=builddir, text=True, capture_output=True, **kw)


def fail(message):
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(message)


sources = [srcdir / "tests" / "mailbox-files.C", staged / "Common" / "Session.C", staged / "RFC" / "RFCMailBox.C"]


def compile_one(source):
    obj = work / (source.stem + ".o")
    r = run(compile_cmd + sanitize + ["-I" + str(staged / "RFC"), "-I" + str(staged / "Common"),
                                      "-c", str(source), "-o", str(obj)])
    return source, obj, r


built = []
with ThreadPoolExecutor(max_workers=jobs) as pool:
    for source, obj, r in pool.map(compile_one, sources):
        if r.returncode:
            fail("compile failed for %s:\n%s" % (source, r.stderr))
        built.append(str(obj))

binary = work / "mailbox-files"
r = run(link_cmd + sanitize + ["-o", str(binary)] + built + [str(archive)] + libs)
if r.returncode:
    fail("link failed:\n" + r.stderr)

env = dict(os.environ)
env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0"
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
cases = run([str(binary)], env=env).stdout.split()
if not cases:
    fail("the test program lists no cases")
only = os.environ.get("DTMAIL_TEST_CASES")
if only:
    cases = [c for c in cases if c in only.split()]
failed = []
for case in cases:
    case_dir = work / case
    case_dir.mkdir()
    env["DTMAIL_TEST_DIR"] = str(case_dir)
    try:
        r = run([str(binary), case], env=env, timeout=120)
        code, err = r.returncode, r.stderr or ""
    except subprocess.TimeoutExpired:
        code, err = -1, "timed out"
    status = "ok" if code == 0 else "FAIL"
    print(f"{status:5} {case}")
    if code:
        failed.append(case)
        print("\n".join("      " + line for line in err.splitlines()[:12]))
print(f"{len(cases) - len(failed)}/{len(cases)} cases passed")
if os.environ.get("DTMAIL_TEST_KEEP"):
    print("kept " + str(work))
else:
    shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed else 0)
