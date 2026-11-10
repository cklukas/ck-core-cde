#!/usr/bin/env python3
"""Run the tests of the client of dtmailspool and of the files around a mailbox under the sanitizers.

check-spool-client.py SRCDIR ARCHIVE

SRCDIR is the source directory of libDtMail, ARCHIVE the built libDtMail.a.
Environment, as `make check` supplies it (see libDtMail/Makefile.am):
  DTMAIL_COMPILE      the compile command of libDtMail, flags included
  DTMAIL_LINK         the libtool link command of the build
  DTMAIL_LIBS         the libraries dtmailpr links
  DTMAIL_CC           the C compile command of the build, flags included
  DTMAIL_STAGED       a directory with Common/SpoolHelper.C and RFC/MailboxFiles.C
                      (and RFC/MailboxFiles.hh) of another revision or a changed
                      copy, compiled instead of the sources in SRCDIR
  DTMAIL_SPOOL_STAGED a directory with dtmailspool/{main.c,spool.c,spool.h} and
                      include/DtMail/SpoolProtocol.h to build the helper from
  DTMAIL_TEST_CASES   run only these cases, a blank separated list of names
  DTMAIL_TEST_KEEP    keep the work directory
The production SpoolHelper.C and MailboxFiles.C are compiled with the sanitizers
next to tests/spool-client.C for a spool directory of the test and linked with the
rest of the built libDtMail.  The helper they start is a script that records each
start and then either becomes the real dtmailspool (built from the production sources
for the same spool directory) or plays a helper that answers wrongly, as the case
says in a file; every case runs in its own process.
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
dtmail_dir = srcdir.parent
spool_staged = (Path(os.environ["DTMAIL_SPOOL_STAGED"]).resolve() if os.environ.get("DTMAIL_SPOOL_STAGED")
                else dtmail_dir)
compile_cmd = shlex.split(os.environ["DTMAIL_COMPILE"])
link_cmd = shlex.split(os.environ["DTMAIL_LINK"])
libs = shlex.split(os.environ.get("DTMAIL_LIBS", ""))
cc = shlex.split(os.environ.get("DTMAIL_CC") or os.environ.get("CC") or "cc")
jobs = int(os.environ.get("DTMAIL_TEST_JOBS", "2"))

builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="dtmail-spool-client-", dir=os.environ.get("TMPDIR")))
sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer", "-g"]
spool = work / "spool"
real = work / "dtmailspool-real"
wrapper = work / "dtmailspool"
mode_file = work / "mode"
wrapper_log = work / "wrapper.log"


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=builddir, text=True, capture_output=True, **kw)


def fail(message):
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(message)


# the real helper, for the spool directory of the test
r = run(cc + ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
              "-Wall", "-Wextra", "-Werror", "-I", str(spool_staged / "include"),
              '-DDTMAIL_SPOOL_DIR="%s"' % spool,
              str(spool_staged / "dtmailspool" / "main.c"), str(spool_staged / "dtmailspool" / "spool.c"),
              "-o", str(real)])
if r.returncode:
    fail("compile of the helper failed:\n" + r.stderr)

# the helper the client starts: it records the start, then is the real one or plays a wrong one
wrapper.write_text('''#!%(python)s -I
import binascii, os, socket, struct, sys
with open(%(log)r, "a") as f:
    f.write("started\\n")
try:
    lines = open(%(mode)r).read().split("\\n")
except FileNotFoundError:
    lines = ["real"]
if lines[0] == "real":
    os.execv(%(real)r, [%(real)r])
if lines[0] == "exit":
    sys.exit(int(lines[1]))
if lines[0] == "reply":
    data = binascii.unhexlify(lines[1])
    count = int(lines[2])
    sock = socket.socket(fileno=3)
    def read_exactly(n):
        got = b""
        while len(got) < n:
            chunk = sock.recv(n - len(got))
            if not chunk:
                sys.exit(0)
            got += chunk
        return got
    length = struct.unpack("=III", read_exactly(12))[2]
    read_exactly(length)
    fds = [os.open("/dev/null", os.O_RDONLY) for _ in range(count)]
    if fds and data:
        socket.send_fds(sock, [data], fds)
    elif data:
        sock.sendall(data)
    if len(lines) > 3 and lines[3] == "hold":     # stay until the client closes the connection
        while sock.recv(4096):
            pass
    sock.close()
    sys.exit(0)
sys.exit(99)
''' % {"python": sys.executable, "log": str(wrapper_log), "mode": str(mode_file), "real": str(real)})
wrapper.chmod(0o755)

sources = [srcdir / "tests" / "spool-client.C", staged / "Common" / "SpoolHelper.C", staged / "RFC" / "MailboxFiles.C"]
extra = {"SpoolHelper": ['-DDTMAIL_SPOOL_HELPER="%s"' % wrapper, '-DDTMAIL_SPOOL_DIR="%s"' % spool]}


def compile_one(source):
    obj = work / (source.stem + ".o")
    r = run(compile_cmd + sanitize + extra.get(source.stem, []) + ["-I" + str(staged / "RFC"), "-I" + str(staged / "Common"),
                                                                  "-c", str(source), "-o", str(obj)])
    return source, obj, r


built = []
with ThreadPoolExecutor(max_workers=jobs) as pool:
    for source, obj, r in pool.map(compile_one, sources):
        if r.returncode:
            fail("compile failed for %s:\n%s" % (source, r.stderr))
        built.append(str(obj))

binary = work / "spool-client"
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
    shutil.rmtree(spool, ignore_errors=True)
    spool.mkdir(mode=0o770)
    os.chmod(spool, 0o770)
    for leftover in (mode_file, wrapper_log):
        leftover.unlink(missing_ok=True)
    env["DTMAIL_TEST_DIR"] = str(case_dir)
    env["DTMAIL_TEST_SPOOL"] = str(spool)
    env["DTMAIL_TEST_WRAPPER"] = str(wrapper)
    env["DTMAIL_TEST_WRAPPER_LOG"] = str(wrapper_log)
    env["DTMAIL_TEST_MODE_FILE"] = str(mode_file)
    env["DTMAIL_TEST_REAL"] = str(real)
    try:
        r = run([str(binary), case], env=env, timeout=180)
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
