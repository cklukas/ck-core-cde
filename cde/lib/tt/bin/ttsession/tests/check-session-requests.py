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
  TT_REQUESTS_OLD_XINFO optional: the same for a libtt from before Set_XInfo
                       was asked for
The test starts a process-tree ttsession of its own, which needs the
portmapper to register with and is skipped without it.  It runs a process that
joins the session with some of the requests of the process asked for (none,
each one, all of them) and a second process that sends every one of those
requests, with no handler named and addressed to the first.  A request that was
asked for is handled and does what it is for (the environment variable, the
working directory, the locale and the signal of the process change, the
questions are answered, the callback gets Do_Command); every other one fails
and leaves the process as it was.  The programs are compiled and linked with
the sanitizers when the build does not already carry them, and a sanitizer
report from either of them, from the library they use or from ttsession fails
the test.  Set_XInfo is also tried on a process with a realized shell on an
X server (Xvfb, when there is one), and as a request of a contract.
"""
import atexit
import os
from pathlib import Path
import re
import select
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
old_xinfo = bool(os.environ.get("TT_REQUESTS_OLD_XINFO"))
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="tt-requests-", dir=os.environ.get("TMPDIR")))
failures = []

# the order matters: the questions come before the requests that change what
# they ask about
REQUESTS = ["get_locale", "get_environment", "get_situation", "get_sysinfo",
            "set_locale", "set_environment", "set_situation", "signal",
            "do_command", "set_xinfo"]
OPS = {"get_locale": "Get_Locale", "get_environment": "Get_Environment",
       "get_situation": "Get_Situation", "get_sysinfo": "Get_Sysinfo",
       "set_locale": "Set_Locale", "set_environment": "Set_Environment",
       "set_situation": "Set_Situation", "signal": "Signal",
       "do_command": "Do_Command", "set_xinfo": "Set_XInfo"}


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
if old_xinfo:
    extra.append("-DTT_SESSION_REQUESTS_OLD_XINFO")

obj = work / "session-requests-test.o"
program = work / "session-requests-test"
r = subprocess.run(compile_cmd + sanitize + extra +
                   ["-c", str(srcdir / "tests/session-requests-test.c"), "-o", str(obj)],
                   cwd=builddir, text=True, errors="replace", capture_output=True)
if r.returncode == 0:
    r = subprocess.run(link_cmd + sanitize + ["-o", str(program), str(obj)] + libs,
                       cwd=builddir, text=True, errors="replace", capture_output=True)
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

# The processes that have a shell need an X server to open a display on: Xvfb,
# if the system has it, picks a free display number and says which.
XINFO_GOOD = "xinfo.example.com:7.0"
xvfb = None
xdisplay = None
if shutil.which("Xvfb"):
    read_end, write_end = os.pipe()
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_end), "-screen", "0",
                             "320x200x24", "-nolisten", "tcp"], pass_fds=(write_end,),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write_end)
    ready_fds, _, _ = select.select([read_end], [], [], 20)
    number = os.read(read_end, 16).decode().strip() if ready_fds else ""
    os.close(read_end)
    if number.isdigit():
        xdisplay = ":" + number
    else:
        xvfb.kill()
        xvfb = None


def stop_xvfb():
    """The server ends with the test, whatever ends the test."""
    if xvfb is not None and xvfb.poll() is None:
        xvfb.terminate()
        try:
            xvfb.wait(timeout=30)
        except subprocess.TimeoutExpired:
            xvfb.kill()


atexit.register(stop_xvfb)


# A sanitizer report from any of the processes fails the test, wherever in the
# library or in ttsession it comes from: the processes use libtt the way a
# client does (open, join, receive, reply, send, close), so the library is
# checked as it is shipped, with no report left out.  UBSan reports go on after
# the report, so they are looked for in what the processes print.
SANITIZER = re.compile(r"runtime error:|ERROR: AddressSanitizer|LeakSanitizer")


def reports(text):
    return [l for l in text.splitlines() if SANITIZER.search(l)]


def parse(text, key):
    return [l.split() for l in text.splitlines() if l.startswith(key + " ")]


def parse_state(text):
    """The "state" line of a process as a dictionary, None if there is not one.
    $DISPLAY, the last but one, can hold anything a process was told to set."""
    lines = [l for l in text.splitlines() if l.startswith("state ")]
    if len(lines) != 1:
        return None
    m = re.match(r"state (.*) display=(.*) connection=(\S+)$", lines[0])
    if not m:
        return {}
    seen = dict(w.split("=", 1) for w in m.group(1).split())
    seen["display"] = m.group(2)
    seen["connection"] = m.group(3)
    return seen


def handler_env(shell):
    henv = dict(env)
    if shell:
        henv["DISPLAY"] = xdisplay
    return henv


def run_case(label, asked, shell=False):
    """One process that asked for `asked`, and every request sent to it."""
    home = (work / ("process-" + label)).resolve()
    home.mkdir()
    handler = subprocess.Popen([str(program), "handle", str(home)] +
                               (["shell"] if shell else []) + sorted(asked),
                               env=handler_env(shell), cwd=work, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
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
                          text=True, errors="replace", capture_output=True, timeout=180)
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
    seen = parse_state(text)
    if seen is None:
        check("%s: the process ends and reports its state" % label, False, text)
        return
    expected = {
        "env": "changed" if "set_environment" in asked else "original",
        "cwd": str(target) if "set_situation" in asked else str(home),
        "signals": "2" if "signal" in asked else "0",
        "locale": "C.UTF-8" if "set_locale" in asked and can_set_locale else "C",
        "docommands": "2" if "do_command" in asked else "0",
        "probe2": "(unset)",
        "probe3": "(unset)",
        # $DISPLAY is the one of the X server, or what Set_XInfo set it to; the
        # connection of the shell stays where it was
        "display": ((XINFO_GOOD if "set_xinfo" in asked else xdisplay) if shell
                    else "(unset)"),
        "connection": xdisplay if shell else "(none)",
    }
    wrong = ["%s is %s, wanted %s" % (k, seen.get(k), v)
             for k, v in expected.items() if seen.get(k) != v] + reports(text)
    check("%s: the process changes only as asked" % label, not wrong, "\n".join(wrong) + "\n" + text)


# What a process that asked for all the requests still refuses: no message may
# set a variable that changes what the process loads or runs, name something
# that is no variable, hand it a locale that names a path, a directory that is
# not absolute or a signal that is none, or send it a string with a null byte
# inside or of absurd length.  Each is failed, and the process is left as it was.
HOSTILE = {
    "env-ld-preload": "eacces", "env-path": "eacces", "env-tt-prefix": "eacces",
    "env-name-digit": "einval", "env-name-equals": "einval", "env-name-empty": "einval",
    "env-name-blank": "einval", "env-name-long": "einval", "env-name-nul": "eproto",
    "env-value-long": "einval", "env-value-nul": "eproto", "env-mixed": "eacces",
    "env-many": "einval", "env-get-name-blank": "einval",
    "locale-path": "einval", "locale-dots": "einval", "locale-empty": "einval",
    "locale-category": "einval", "locale-long": "einval", "locale-nul": "eproto",
    "locale-many": "einval",
    "situation-relative": "einval", "situation-control": "einval",
    "situation-long": "einval", "situation-nul": "eproto", "situation-missing": "enoent",
    "signal-zero": "einval", "signal-huge": "einval", "signal-negative": "einval",
}


def run_hostile():
    home = (work / "process-hostile").resolve()
    home.mkdir()
    handler = subprocess.Popen([str(program), "handle", str(home)] + REQUESTS,
                               env=env, cwd=work, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    ready = handler.stdout.readline().rstrip("\n")
    if not (ready.startswith("ready ") and " locale=" in ready):
        handler.kill()
        check("hostile: the process joins the session", False, ready + handler.stdout.read())
        return
    procid = ready[len("ready "):ready.rindex(" locale=")]
    sent = subprocess.run([str(program), "hostile", procid], env=env, cwd=target,
                          text=True, errors="replace", capture_output=True, timeout=300)
    handler.stdin.close()
    try:
        text = handler.stdout.read()
        handler.wait(timeout=30)
    except subprocess.TimeoutExpired:
        handler.kill()
        text = "the process did not end"
    results = {w[1]: w[2:4] for w in parse(sent.stdout, "hostile")}
    wrong = []
    for name, status in HOSTILE.items():
        if results.get(name) != ["failed", status]:
            wrong.append("%s: %s, wanted failed %s" % (name, results.get(name), status))
    for name in ("env-remove", "env-valid"):
        if results.get(name) != ["handled", "ok"]:
            wrong.append("%s: %s, wanted handled ok" % (name, results.get(name)))
    wrong += reports(sent.stdout + sent.stderr)
    check("hostile: values no message may act on are failed", not wrong and sent.returncode == 0,
          "\n".join(wrong) + "\n" + sent.stdout + sent.stderr)
    seen = parse_state(text)
    if seen is None:
        check("hostile: the process ends and reports its state", False, text)
        return
    # the valid requests at the end took one variable away and set another
    expected = {"env": "(unset)", "cwd": str(home), "signals": "0", "locale": "C",
                "docommands": "0", "probe2": "(unset)", "probe3": "valid",
                "display": "(unset)", "connection": "(none)"}
    wrong = ["%s is %s, wanted %s" % (k, seen.get(k), v)
             for k, v in expected.items() if seen.get(k) != v] + reports(text)
    check("hostile: the process is changed by the valid requests only", not wrong,
          "\n".join(wrong) + "\n" + text)


# Set_XInfo of a process with a shell sets $DISPLAY (what the process starts and
# opens from now on uses it) and leaves the connection it has; values that are not
# a display, a visual and a depth fail.
XINFO_FAILS = {
    "garbage": "einval", "nocolon": "einval", "nul": "eproto", "long": "einval",
    "unset": "eproto", "visual": "einval", "visual-long": "einval",
    "depth-zero": "einval", "depth-big": "einval",
}


def start_handler(label, words, shell):
    home = (work / ("process-" + label)).resolve()
    home.mkdir()
    handler = subprocess.Popen([str(program), "handle", str(home)] + words,
                               env=handler_env(shell), cwd=work, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    ready = handler.stdout.readline().rstrip("\n")
    if not (ready.startswith("ready ") and " locale=" in ready):
        handler.kill()
        check("%s: the process joins the session" % label, False, ready + handler.stdout.read())
        return None, None
    return handler, ready[len("ready "):ready.rindex(" locale=")]


def finish_handler(handler):
    handler.stdin.close()
    try:
        text = handler.stdout.read()
        handler.wait(timeout=30)
    except subprocess.TimeoutExpired:
        handler.kill()
        text = "the process did not end"
    return text, (parse_state(text) or {})


def run_xinfo_values():
    handler, procid = start_handler("xinfo-values", ["shell", "set_xinfo"], True)
    if handler is None:
        return
    sent = subprocess.run([str(program), "xinfo", procid], env=env, cwd=target,
                          text=True, errors="replace", capture_output=True, timeout=300)
    text, seen = finish_handler(handler)
    results = {w[1]: w[2:4] for w in parse(sent.stdout, "xinfo")}
    wrong = []
    for name, status in XINFO_FAILS.items():
        if results.get(name) != ["failed", status]:
            wrong.append("%s: %s, wanted failed %s" % (name, results.get(name), status))
    if results.get("valid") != ["handled", "ok"]:
        wrong.append("valid: %s, wanted handled ok" % results.get("valid"))
    for key, want in (("display", XINFO_GOOD), ("connection", xdisplay)):
        if seen.get(key) != want:
            wrong.append("%s is %s, wanted %s" % (key, seen.get(key), want))
    wrong += reports(sent.stdout + sent.stderr + text)
    check("set_xinfo: a display, visual and depth that are none are failed, a good one is set",
          not wrong and sent.returncode == 0, "\n".join(wrong) + "\n" + sent.stdout + sent.stderr + text)


def run_contract(label, words, shell, accepts, display):
    """A contract the process accepts, with the Set_XInfo that follow it."""
    handler, procid = start_handler(label, ["contract"] + words, shell)
    if handler is None:
        return
    sent = subprocess.run([str(program), "contract", procid], env=env, cwd=target,
                          text=True, errors="replace", capture_output=True, timeout=300)
    text, seen = finish_handler(handler)
    results = {w[1]: w[2:4] for w in parse(sent.stdout, "contract")}
    wrong = []
    wanted = {"valid": ["handled", "ok"] if "set_xinfo" in words else ["failed", "enotsup"]}
    if shell:
        wanted["garbage"] = ["failed", "einval"] if "set_xinfo" in words else ["failed", "enotsup"]
    for name, want in wanted.items():
        if results.get(name) != want:
            wrong.append("%s: %s, wanted %s" % (name, results.get(name), " ".join(want)))
    if [w[1] for w in parse(text, "accepted")] != [accepts]:
        wrong.append("accepted %s, wanted %s" % ([w[1] for w in parse(text, "accepted")], accepts))
    if seen.get("display") != display:
        wrong.append("display is %s, wanted %s" % (seen.get("display"), display))
    wrong += reports(sent.stdout + sent.stderr + text)
    check("%s: the contract takes Set_XInfo only if asked" % label,
          not wrong and sent.returncode == 0, "\n".join(wrong) + "\n" + sent.stdout + text)


run_case("none", [])
for name in REQUESTS:
    run_case(name, [name])
run_case("all", REQUESTS)
if xdisplay:
    run_case("shell-none", [], shell=True)
    run_case("shell-set_xinfo", ["set_xinfo"], shell=True)
    run_case("shell-all", REQUESTS, shell=True)
    run_xinfo_values()
else:
    print("skip  no Xvfb: the cases with a shell are not run")
if not old_join:
    run_hostile()
run_contract("contract-none", [], False, "ok", "(unset)")
run_contract("contract-set_xinfo", ["set_xinfo"], False, "ok", "(unset)")
if xdisplay:
    run_contract("contract-shell-none", ["shell"], True, "ok", xdisplay)
    run_contract("contract-shell-set_xinfo", ["shell", "set_xinfo"], True, "ok", XINFO_GOOD)

if not old_join:
    r = subprocess.run([str(program), "arguments"], env=env, cwd=work, text=True, errors="replace",
                       capture_output=True, timeout=60)
    joins = {w[1]: w[2] for w in parse(r.stdout, "join")}
    check("a request that cannot be answered is refused, none at all is not",
          joins == {"do_command-without-callback": "pointer",
                    "unknown-request": "num",
                    "set_xinfo-without-callback-or-shell": "pointer",
                    "set_xinfo-with-callback": "ok",
                    "no-requests-without-callback": "ok"} and not reports(r.stdout + r.stderr),
          r.stdout + r.stderr)

session.terminate()
try:
    session.wait(timeout=30)
except subprocess.TimeoutExpired:
    session.kill()
    session.wait()
stop_xvfb()
served = out.read_text()
check("ttsession ends without a sanitizer report", not reports(served), served)
prog = sid.split()[2]
for vers in ("1", "4"):
    subprocess.run(["rpcinfo", "-d", prog, vers], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
finish()
