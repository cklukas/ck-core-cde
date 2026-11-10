#!/usr/bin/env python3
"""Check which Desktop requests ttdt_session_join() answers for a process.

check-session-requests.py SRCDIR TTSESSION

SRCDIR is lib/tt/bin/ttsession of the source tree, TTSESSION the built
ttsession.  Environment, as `make check` supplies it (see
lib/tt/bin/ttsession/Makefile.am):
  TT_REQUESTS_COMPILE  the compile command of the C programs of the build
  TT_REQUESTS_LINK     the libtool link command
  TT_REQUESTS_LIBS     the libraries session-requests-test links, libtt first
  TT_REQUESTS_OLD_JOIN optional: build the test for the five argument
                       ttdt_session_join() of a libtt from before the requests
                       were asked for; this is how the test is shown to fail
                       on that library
The test starts a process-tree ttsession of its own, which needs the
portmapper to register with and is skipped without it.  It runs a process that
joins the session with some of the requests of the process asked for (none,
each one, all of them) and a second process that sends every one of those
requests, with no handler named and addressed to the first.  A request that was
asked for is handled and does what it is for (the environment variable, the
working directory, the locale and the signal of the process change, the
questions are answered, the callback gets Do_Command); every other one fails
and leaves the process as it was.  The programs are compiled and linked with
the sanitizers when the build does not already carry them.
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
compile_cmd = shlex.split(os.environ["TT_REQUESTS_COMPILE"])
link_cmd = shlex.split(os.environ["TT_REQUESTS_LINK"])
libs = shlex.split(os.environ.get("TT_REQUESTS_LIBS", ""))
old_join = bool(os.environ.get("TT_REQUESTS_OLD_JOIN"))
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="tt-requests-", dir=os.environ.get("TMPDIR")))
failures = []

# the order matters: the questions come before the requests that change what
# they ask about
REQUESTS = ["get_locale", "get_environment", "get_situation", "get_sysinfo",
            "set_locale", "set_environment", "set_situation", "signal",
            "do_command"]
OPS = {"get_locale": "Get_Locale", "get_environment": "Get_Environment",
       "get_situation": "Get_Situation", "get_sysinfo": "Get_Sysinfo",
       "set_locale": "Set_Locale", "set_environment": "Set_Environment",
       "set_situation": "Set_Situation", "signal": "Signal",
       "do_command": "Do_Command"}


def finish():
    shutil.rmtree(work, ignore_errors=True)
    print("%d checks failed" % len(failures) if failures else "all checks passed")
    sys.exit(1 if failures else 0)


def check(name, ok, detail=""):
    print("%-5s %s" % ("ok" if ok else "FAIL", name))
    if not ok:
        failures.append(name)
        for line in detail.splitlines()[:int(os.environ.get("TT_REQUESTS_DETAIL", "14"))]:
            print("      " + line)


if subprocess.run(["rpcinfo", "-p"], stdout=subprocess.DEVNULL,
                  stderr=subprocess.DEVNULL).returncode != 0:
    print("skip  no portmapper to register a session with")
    finish()

sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
if any(a.startswith("-fsanitize") for a in compile_cmd):
    sanitize = []                    # the build already carries them
extra = ["-DTT_SESSION_REQUESTS_OLD_JOIN"] if old_join else []

obj = work / "session-requests-test.o"
program = work / "session-requests-test"
r = subprocess.run(compile_cmd + sanitize + extra +
                   ["-c", str(srcdir / "tests/session-requests-test.c"), "-o", str(obj)],
                   cwd=builddir, text=True, capture_output=True)
if r.returncode == 0:
    r = subprocess.run(link_cmd + sanitize + ["-o", str(program), str(obj)] + libs,
                       cwd=builddir, text=True, capture_output=True)
if r.returncode:
    check("session-requests-test builds", False, r.stderr)
    finish()

env = dict(os.environ)
env.pop("DISPLAY", None)
env["HOME"] = str(work)
env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0"
env["UBSAN_OPTIONS"] = "print_stacktrace=1"
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

target = (work / "target").resolve()
target.mkdir()


# UBSan reports go on after the report, so they are looked for in what the
# programs print.  This test is about the code of tttk and the test program,
# so a UBSan report in one of their files fails it; an AddressSanitizer or
# leak report fails it wherever it is.
tttk_dir = srcdir / ".." / ".." / "lib" / "tttk"
here = sorted({p.name for p in tttk_dir.iterdir() if p.suffix in (".C", ".h")}
              | {"session-requests-test.c"})
UBSAN_HERE = re.compile(r"(^|/)(%s):\d+:\d+: runtime error:" %
                        "|".join(re.escape(n) for n in here))


def reports(text):
    return [l for l in text.splitlines()
            if UBSAN_HERE.search(l)
            or "ERROR: AddressSanitizer" in l or "LeakSanitizer" in l]


def parse(text, key):
    return [l.split() for l in text.splitlines() if l.startswith(key + " ")]


def run_case(label, asked):
    """One process that asked for `asked`, and every request sent to it."""
    home = (work / ("process-" + label)).resolve()
    home.mkdir()
    handler = subprocess.Popen([str(program), "handle", str(home)] + sorted(asked),
                               env=env, cwd=work, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # a process id holds blanks: "ready PROCID locale=yes|no"
    ready = handler.stdout.readline().rstrip("\n")
    if not (ready.startswith("ready ") and " locale=" in ready):
        handler.kill()
        check("%s: the process joins the session" % label, False,
              ready + handler.stdout.read())
        return
    procid = ready[len("ready "):ready.rindex(" locale=")]
    can_set_locale = ready.endswith("locale=yes")
    sent = subprocess.run([str(program), "send", procid] + REQUESTS, env=env, cwd=target,
                          text=True, capture_output=True, timeout=180)
    handler.stdin.close()
    try:
        text = handler.stdout.read()
        handler.wait(timeout=30)
    except subprocess.TimeoutExpired:
        handler.kill()
        text = "the process did not end"

    # the replies
    wrong = []
    results = {}
    for words in parse(sent.stdout, "result"):
        op, how, state, status = words[1:5]
        results[(op, how)] = (state, status, words[5:])
    for name in REQUESTS:
        op = OPS[name]
        for how in ("unaddressed", "addressed"):
            if name in asked:
                want = ("handled", "ok")
            elif how == "unaddressed":
                want = ("failed", "nomatch")     # nobody registered for it
            else:
                want = ("failed", "enotsup")     # the process refuses it
            got = results.get((op, how))
            if got is None or got[:2] != want:
                wrong.append("%s %s: %s, wanted %s" %
                             (op, how, got and "%s %s" % got[:2], "%s %s" % want))
    # what the answers say
    if "get_locale" in asked:
        got = results.get(("Get_Locale", "unaddressed"))
        if got and got[2] != ["C"]:
            wrong.append("Get_Locale answered %r" % got[2])
    if "get_environment" in asked:
        got = results.get(("Get_Environment", "unaddressed"))
        if got and got[2] != ["original"]:
            wrong.append("Get_Environment answered %r" % got[2])
    if "get_situation" in asked:
        got = results.get(("Get_Situation", "unaddressed"))
        if got and got[2] != [str(home)]:
            wrong.append("Get_Situation answered %r, the directory is %s" % (got[2], home))
    if "get_sysinfo" in asked:
        got = results.get(("Get_Sysinfo", "unaddressed"))
        if got and got[2][:1] != [os.uname().nodename]:
            wrong.append("Get_Sysinfo answered %r" % got[2])
    wrong += reports(sent.stdout + sent.stderr)
    check("%s: the requests asked for are handled, the others fail" % label,
          not wrong and sent.returncode == 0,
          "\n".join(wrong) + ("\n%s" % sent.stderr if sent.returncode else "") +
          "\nsender: " + sent.stdout.replace("\n", "\n        "))

    # what happened to the process
    state = parse(text, "state")
    if len(state) != 1:
        check("%s: the process ends and reports its state" % label, False, text)
        return
    seen = dict(w.split("=", 1) for w in state[0][1:])
    expected = {
        "env": "changed" if "set_environment" in asked else "original",
        "cwd": str(target) if "set_situation" in asked else str(home),
        "signals": "2" if "signal" in asked else "0",
        "locale": "C.UTF-8" if "set_locale" in asked and can_set_locale else "C",
        "docommands": "2" if "do_command" in asked else "0",
    }
    wrong = ["%s is %s, wanted %s" % (k, seen.get(k), v)
             for k, v in expected.items() if seen.get(k) != v] + reports(text)
    check("%s: the process changes only as asked" % label, not wrong, "\n".join(wrong) + "\n" + text)


run_case("none", [])
for name in REQUESTS:
    run_case(name, [name])
run_case("all", REQUESTS)

if not old_join:
    r = subprocess.run([str(program), "arguments"], env=env, cwd=work, text=True,
                       capture_output=True, timeout=60)
    joins = {w[1]: w[2] for w in parse(r.stdout, "join")}
    check("a request that cannot be answered is refused, none at all is not",
          joins == {"do_command-without-callback": "pointer",
                    "unknown-request": "num",
                    "no-requests-without-callback": "ok"},
          r.stdout + r.stderr)

session.terminate()
try:
    session.wait(timeout=30)
except subprocess.TimeoutExpired:
    session.kill()
    session.wait()
prog = sid.split()[2]
for vers in ("1", "4"):
    subprocess.run(["rpcinfo", "-d", prog, vers], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
finish()
