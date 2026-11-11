#!/usr/bin/env python3
"""Check what ttdt_sender_imprint_on() sends, believes and changes.

check-sender-imprint.py SRCDIR TTSESSION

SRCDIR is lib/tt/bin/ttsession of the source tree, TTSESSION the built
ttsession.  Environment, as `make check` supplies it (see
lib/tt/bin/ttsession/Makefile.am):
  TT_IMPRINT_COMPILE   the compile command of the C programs of the build
  TT_IMPRINT_LINK      the libtool link command
  TT_IMPRINT_LIBS      the libraries imprint-test links, libtt first
  TT_IMPRINT_OLD_CALL  optional: build the test for the call without the
                       imprints argument of a libtt from before the changes
                       were asked for; this is how the test is shown to fail on
                       that library
  TT_IMPRINT_DETAIL    how many lines of a failure to print (default 14)
The test starts a process-tree ttsession of its own, which needs the
portmapper to register with and is skipped without it.  A handler
(imprint-test serve) answers Get_Locale, Get_Situation, Get_XInfo and
Get_Geometry the way a scenario says, and a second process
(imprint-test imprint) calls ttdt_sender_imprint_on() and reports which
requests the handler was asked, what the call returned and what it did to its
own process (directory, locale, $DISPLAY).  The process is changed only in the
ways the call is asked for, and only the requests that are needed are sent.
Replies that cannot be used (a locale that names a path, a directory that is
not absolute, a display that is shell syntax, sizes beyond what a window has, a
null byte inside a string, strings of absurd length) change nothing, not even
what the other replies carry, and are reported.  A reply that comes after the
call has stopped waiting changes nothing.  A sanitizer report from either
process, the library or ttsession fails the test.  The programs are compiled
and linked with the sanitizers when the build does not already carry them.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

srcdir = Path(sys.argv[1]).resolve()
ttsession = Path(sys.argv[2]).resolve()
compile_cmd = shlex.split(os.environ["TT_IMPRINT_COMPILE"])
link_cmd = shlex.split(os.environ["TT_IMPRINT_LINK"])
libs = shlex.split(os.environ.get("TT_IMPRINT_LIBS", ""))
old_call = bool(os.environ.get("TT_IMPRINT_OLD_CALL"))
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="tt-imprint-", dir=os.environ.get("TMPDIR")))
failures = []


def finish():
    shutil.rmtree(work, ignore_errors=True)
    print("%d checks failed" % len(failures) if failures else "all checks passed")
    sys.exit(1 if failures else 0)


def check(name, ok, detail=""):
    print("%-5s %s" % ("ok" if ok else "FAIL", name))
    if not ok:
        failures.append(name)
        for line in detail.splitlines()[:int(os.environ.get("TT_IMPRINT_DETAIL", "14"))]:
            print("      " + line)


if subprocess.run(["rpcinfo", "-p"], stdout=subprocess.DEVNULL,
                  stderr=subprocess.DEVNULL).returncode != 0:
    print("skip  no portmapper to register a session with")
    finish()

sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
if any(a.startswith("-fsanitize") for a in compile_cmd):
    sanitize = []                    # the build already carries them
extra = ["-DTT_IMPRINT_TEST_OLD_CALL"] if old_call else []

obj = work / "imprint-test.o"
program = work / "imprint-test"
r = subprocess.run(compile_cmd + sanitize + extra +
                   ["-c", str(srcdir / "tests/imprint-test.c"), "-o", str(obj)],
                   cwd=builddir, text=True, capture_output=True)
if r.returncode == 0:
    r = subprocess.run(link_cmd + sanitize + ["-o", str(program), str(obj)] + libs,
                       cwd=builddir, text=True, capture_output=True)
if r.returncode:
    check("imprint-test builds", False, r.stderr)
    finish()

start = (work / "start").resolve()
target = (work / "target").resolve()
start.mkdir()
target.mkdir()

env = dict(os.environ)
env.pop("DISPLAY", None)
env["HOME"] = str(work)
# a reply that comes late meets the stack of a call that has returned
env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0:detect_stack_use_after_return=1"
env["UBSAN_OPTIONS"] = "print_stacktrace=1"
env["IMPRINT_TEST_START"] = str(start)
env["IMPRINT_TEST_DIR"] = str(target)
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

SANITIZER = re.compile(r"runtime error:|ERROR: AddressSanitizer|LeakSanitizer")


def reports(text):
    return [l for l in text.splitlines() if SANITIZER.search(l)]


def parse(text, key):
    return [l.split() for l in text.splitlines() if l.startswith(key + " ")]


ORIGINAL = {"cwd": str(start), "locale": "C", "display": "original.example.com:0"}
GOOD_DISPLAY = "other.example.com:5.0"
NOT_ANSWERED = "-1,-1,2147483647,2147483647"     # what the geometry is left as
ALL = ["locale", "situation", "display", "want-display", "want-geometry"]


def run(scenario, words, handler_needed=True):
    """One handler with the scenario, one call of ttdt_sender_imprint_on()."""
    handler = None
    procid = "-"
    if handler_needed:
        handler = subprocess.Popen([str(program), "serve", scenario], env=env, cwd=work,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        ready = handler.stdout.readline().rstrip("\n")
        if not ready.startswith("ready "):
            handler.kill()
            return {"error": "the handler does not start: " + ready + handler.stdout.read()}
        procid = ready[len("ready "):]
    timeout = 120
    r = subprocess.run([str(program), "imprint", procid] + words, env=env, cwd=work,
                       text=True, capture_output=True, timeout=timeout)
    handled = ""
    if handler is not None:
        handler.stdin.close()
        try:
            handled = handler.stdout.read()
            handler.wait(timeout=30)
        except subprocess.TimeoutExpired:
            handler.kill()
            handled = "the handler did not end"
    result = {"asked": sorted(w[1] for w in parse(handled, "asked")),
              "reports": reports(r.stdout + r.stderr + handled),
              "text": r.stdout + r.stderr + "handler: " + handled}
    # a display or a directory that is not what it should be can hold blanks
    m = re.search(r"^imprint (\S+) display=(.*) geometry=(\S+) utf8=(yes|no)$",
                  r.stdout, re.M)
    if m:
        result["status"] = m.group(1)
        result["returned_display"] = m.group(2)
        result["geometry"] = m.group(3)
        result["utf8"] = m.group(4) == "yes"
    m = re.search(r"^state cwd=(.*) locale=(\S+) display=(.*)$", r.stdout, re.M)
    if m:
        result.update({"cwd": m.group(1), "locale": m.group(2), "display": m.group(3)})
    return result


def verdict(name, scenario, words, want_asked, want_status, want_state,
            want_display="(none)", want_geometry="7,7,7,7", handler_needed=True,
            statuses=None):
    """The requests the handler was asked, the status, what is returned and
    what the calling process is like afterwards."""
    got = run(scenario, words, handler_needed)
    if "error" in got:
        check(name, False, got["error"])
        return
    wrong = []
    if got["asked"] != sorted(want_asked):
        wrong.append("the handler was asked %s, wanted %s" % (got["asked"], sorted(want_asked)))
    if statuses is not None:
        if got.get("status") not in statuses:
            wrong.append("status %s, wanted one of %s" % (got.get("status"), statuses))
    elif got.get("status") != want_status:
        wrong.append("status %s, wanted %s" % (got.get("status"), want_status))
    for key in ("cwd", "locale", "display"):
        if got.get(key) != want_state[key]:
            wrong.append("%s of the process is %s, wanted %s" % (key, got.get(key), want_state[key]))
    if got.get("returned_display") != want_display:
        wrong.append("the display returned is %s, wanted %s" %
                     (got.get("returned_display"), want_display))
    if got.get("geometry") != want_geometry:
        wrong.append("the geometry returned is %s, wanted %s" % (got.get("geometry"), want_geometry))
    wrong += got["reports"]
    check(name, not wrong, "\n".join(wrong) + "\n" + got["text"])


def changed(**what):
    state = dict(ORIGINAL)
    state.update(what)
    return state


# the locale a handler's C.UTF-8 gives the process, if the system has it
probe = run("good", ["locale"])
utf8 = probe.get("utf8", False)
LOCALE = "C.UTF-8" if utf8 else "C"

verdict("the call without anything asked for sends nothing and changes nothing",
        "good", [], [], "ok", ORIGINAL)
verdict("locale alone: Get_Locale is asked, the locale changes, nothing else does",
        "good", ["locale"], ["Get_Locale"], "ok", changed(locale=LOCALE))
verdict("situation alone: Get_Situation is asked, the directory changes, nothing else does",
        "good", ["situation"], ["Get_Situation"], "ok", changed(cwd=str(target)))
verdict("display alone: Get_XInfo is asked, $DISPLAY changes, nothing else does",
        "good", ["display"], ["Get_XInfo"], "ok", changed(display=GOOD_DISPLAY))
verdict("everything asked for: all four requests, all three changes",
        "good", ALL, ["Get_Locale", "Get_Situation", "Get_XInfo", "Get_Geometry"], "ok",
        changed(locale=LOCALE, cwd=str(target), display=GOOD_DISPLAY),
        want_display=GOOD_DISPLAY, want_geometry="400,300,20,30")
verdict("only to be told the display and the geometry: the process is left as it was",
        "good", ["want-display", "want-geometry"], ["Get_XInfo", "Get_Geometry"], "ok",
        ORIGINAL, want_display=GOOD_DISPLAY, want_geometry="400,300,20,30")
verdict("a value that is no imprint is refused before anything is sent",
        "good", ["locale", "unknown-imprint"], [], "num", ORIGINAL)
verdict("without a handler or a contract nothing can be asked",
        "good", ["locale", "situation", "display"], [], "pointer", ORIGINAL,
        handler_needed=False)
verdict("a handler that fails everything: the failure is reported, nothing changes",
        "refuse", ALL, ["Get_Locale", "Get_Situation", "Get_XInfo", "Get_Geometry"],
        "enotsup", ORIGINAL, want_geometry=NOT_ANSWERED)
verdict("a handler that fails one request: the other replies are still used",
        "no-situation", ALL, ["Get_Locale", "Get_Situation", "Get_XInfo", "Get_Geometry"],
        "enotsup", changed(locale=LOCALE, display=GOOD_DISPLAY),
        want_display=GOOD_DISPLAY, want_geometry="400,300,20,30")

# replies that cannot be used change nothing at all
for scenario in ("locale-path", "locale-dots", "locale-nul", "locale-long", "locale-blank",
                 "cwd-relative", "cwd-nul", "cwd-control", "cwd-long",
                 "display-garbage", "display-nocolon", "display-nul", "display-long",
                 "geometry-wild"):
    verdict("a reply that cannot be used (%s) changes nothing and is reported" % scenario,
            scenario, ALL, ["Get_Locale", "Get_Situation", "Get_XInfo", "Get_Geometry"],
            None, ORIGINAL, want_geometry=NOT_ANSWERED, statuses=("eproto", "einval"))

# no reply: the wait ends, and what comes later finds nothing to write into
verdict("a handler that does not answer: the call times out and changes nothing",
        "silent", ALL, ["Get_Locale", "Get_Situation", "Get_XInfo", "Get_Geometry"],
        "timedout", ORIGINAL, want_geometry=NOT_ANSWERED)
verdict("a reply that comes after the call gave up changes nothing",
        "late", ALL + ["timeout=300", "linger=2800"],
        ["Get_Locale", "Get_Situation", "Get_XInfo", "Get_Geometry"],
        "timedout", ORIGINAL, want_geometry=NOT_ANSWERED)

session.terminate()
try:
    session.wait(timeout=30)
except subprocess.TimeoutExpired:
    session.kill()
    session.wait()
served = out.read_text()
check("ttsession ends without a sanitizer report", not reports(served), served)
prog = sid.split()[2]
for vers in ("1", "4"):
    subprocess.run(["rpcinfo", "-d", prog, vers], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
finish()
