#!/usr/bin/env python3
"""Check what the observer of a notice with a disposition receives from ttsession.

check-notice.py SRCDIR TTSESSION

SRCDIR is lib/tt/bin/ttsession of the source tree, TTSESSION the built
ttsession.  Environment, as `make check` supplies it (see
lib/tt/bin/ttsession/Makefile.am):
  TT_NOTICE_COMPILE  the compile command of the C programs of the build
  TT_NOTICE_LINK     the libtool link command
  TT_NOTICE_LIBS     the libraries notice-test links, libtt first
The test starts a process-tree ttsession of its own, which needs the
portmapper to register with and is skipped without it.  One process observes
the operation, another sends a notice with the disposition TT_QUEUE (and then
TT_START): nobody handles a notice, so the observer has to receive it once, in
the state TT_SENT, and not again as a failed message.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

srcdir = Path(sys.argv[1]).resolve()
ttsession = Path(sys.argv[2]).resolve()
compile_cmd = shlex.split(os.environ["TT_NOTICE_COMPILE"])
link_cmd = shlex.split(os.environ["TT_NOTICE_LINK"])
libs = shlex.split(os.environ.get("TT_NOTICE_LIBS", ""))
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="tt-notice-", dir=os.environ.get("TMPDIR")))
failures = []


def finish():
    shutil.rmtree(work, ignore_errors=True)
    print("%d checks failed" % len(failures) if failures else "all checks passed")
    sys.exit(1 if failures else 0)


def check(name, ok, detail=""):
    print("%-5s %s" % ("ok" if ok else "FAIL", name))
    if not ok:
        failures.append(name)
        for line in detail.splitlines()[:8]:
            print("      " + line)


if subprocess.run(["rpcinfo", "-p"], stdout=subprocess.DEVNULL,
                  stderr=subprocess.DEVNULL).returncode != 0:
    print("skip  no portmapper to register a session with")
    finish()

obj = work / "notice-test.o"
program = work / "notice-test"
r = subprocess.run(compile_cmd + ["-c", str(srcdir / "tests/notice-test.c"), "-o", str(obj)],
                   cwd=builddir, text=True, capture_output=True)
if r.returncode == 0:
    r = subprocess.run(link_cmd + ["-o", str(program), str(obj)] + libs,
                       cwd=builddir, text=True, capture_output=True)
if r.returncode:
    check("notice-test builds", False, r.stderr)
    finish()

env = dict(os.environ)
env.pop("DISPLAY", None)
env["HOME"] = str(work)
out = work / "ttsession.out"
session = subprocess.Popen([str(ttsession), "-S", "-p"], stdout=out.open("w"),
                           stderr=subprocess.STDOUT, env=env, cwd=work)
sid = None
for _ in range(100):
    lines = out.read_text().splitlines()
    if lines and len(lines[0].split()) >= 8:
        sid = lines[0]
        break
    time.sleep(0.1)
if sid is None:
    session.kill()
    check("a session starts", False, out.read_text())
    finish()
env["TT_SESSION"] = sid


def run_case(label, op):
    observer = subprocess.Popen([str(program), "observe", op, "3"], env=env, cwd=work,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    ready = observer.stdout.readline().strip()
    sent = subprocess.run([str(program), "send", op, "1"], env=env, cwd=work, text=True,
                          capture_output=True, timeout=30)
    text = observer.communicate(timeout=30)[0]
    observed = [l.split()[1] for l in text.splitlines() if l.startswith("observed ")]
    check("%s: the observer gets the notice once, as sent" % label,
          ready == "ready" and sent.returncode == 0 and observed == ["sent"],
          "ready=%r sender rc=%d %s observer: %r" % (ready, sent.returncode, sent.stderr, text))


run_case("queue", "dleft_queue_notice")

# A session that is told to end leaves the portmapper: the main loop does it,
# the signal handler only notes the signal.
prog = sid.split()[2]


def registered():
    r = subprocess.run(["rpcinfo", "-p"], text=True, capture_output=True)
    return [l for l in r.stdout.splitlines() if l.split()[:1] == [prog]]


check("the session is registered", len(registered()) > 0)
session.terminate()
try:
    code = session.wait(timeout=30)
except subprocess.TimeoutExpired:
    session.kill()
    session.wait()
    code = None
left = registered()
check("a session that is told to end exits", code == 1, "exit status %r" % code)
check("... and takes itself out of the portmapper", not left, "\n".join(left))
for vers in ("1", "4"):
    subprocess.run(["rpcinfo", "-d", prog, vers], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
finish()
