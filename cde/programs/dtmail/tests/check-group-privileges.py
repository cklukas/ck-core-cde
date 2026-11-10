#!/usr/bin/env python3
"""Run the group handling of a set-group-id dtmail against a fake kernel that can refuse.

dtmail and dtmailpr may be installed set-group-id for the mail group.  They give
the group up at startup and take it for one spool operation at a time.
include/DtMail/MailGroup.hh is the only code that changes a group id.  It is
compiled here with stand-ins for the process identity (real, effective and
saved group id) and for getegid()/setegid(), including a kernel that refuses
the call and one that reports success without changing anything.  Every
scenario runs in its own process under AddressSanitizer and
UndefinedBehaviorSanitizer.

DTMAIL_SOURCE names another copy of the dtmail sources to test, for example one
with a deliberate defect.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTMAIL_SOURCE") or Path(__file__).resolve().parents[1])

harness = r'''
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); return 1; } } while (0)

/* the fake process identity and the faults a kernel can show */
static struct { gid_t r, e, s; } id;
static int fail_ops, ignore_changes, calls;
static jmp_buf escape;
static int exit_code;
static char message[256];

static gid_t fake_getgid(void) { return id.r; }
static gid_t fake_getegid(void) { return id.e; }
/* the rule of setegid() for a process that is not privileged */
static int fake_setegid(gid_t e)
{
    calls++;
    if (fail_ops) { errno = EPERM; return -1; }
    if (ignore_changes) return 0;
    if (e != id.r && e != id.s && e != id.e) { errno = EPERM; return -1; }
    id.e = e;
    return 0;
}
static ssize_t fake_write(int fd, const void *buf, size_t n)
{
    if (fd == STDERR_FILENO) {
        size_t have = strlen(message);
        if (have + n < sizeof(message)) { memcpy(message + have, buf, n); message[have + n] = 0; }
    }
    return (ssize_t) n;
}

#define getgid fake_getgid
#define getegid fake_getegid
#define setegid fake_setegid
#define write fake_write
#define _exit(code) (exit_code = (code), longjmp(escape, 1))

#include "include/DtMail/MailGroup.hh"

#undef _exit
#undef write

/* group 100 of the user, mail group 12 of a set-group-id installation */
#define USER 100
#define MAIL 12

static void start(gid_t egid)
{
    memset(&id, 0, sizeof(id));
    id.r = USER; id.e = egid; id.s = egid;
    fail_ops = ignore_changes = calls = 0;
    message[0] = 0;
    exit_code = -1;
}

/* the status of a call that may stop the program: its exit code, or -1 when it returns */
template <class F> static int stops(F call)
{
    exit_code = -1;
    if (setjmp(escape) == 0)
        call();
    return exit_code;
}
#define STOPS(call) stops([&] { call; })

static int plain_install_makes_no_call(void)
{
    MailGroup g;
    start(USER);
    fail_ops = 1;                                  /* any call at all would fail */
    CHECK(STOPS(g.dropOrStop()) == -1);
    CHECK(calls == 0);
    CHECK(g.enable());
    g.disableOrStop();
    CHECK(calls == 0 && id.e == USER);
    return 0;
}
static int group_is_given_up_at_startup(void)
{
    MailGroup g;
    start(MAIL);
    CHECK(STOPS(g.dropOrStop()) == -1);
    CHECK(id.e == USER && id.r == USER);
    CHECK(g.installedGid() == MAIL && g.userGid() == USER);
    CHECK(id.s == MAIL);                           /* the way back for the spool operations stays */
    return 0;
}
static int group_is_held_only_between_enable_and_disable(void)
{
    MailGroup g;
    start(MAIL);
    g.dropOrStop();
    for (int i = 0; i < 3; i++) {
        CHECK(g.enable());
        CHECK(id.e == MAIL);
        CHECK(STOPS(g.disableOrStop()) == -1);
        CHECK(id.e == USER);
    }
    return 0;
}
static int second_start_keeps_the_installed_group(void)
{
    MailGroup g;
    start(MAIL);
    g.dropOrStop();
    CHECK(g.enable());
    g.dropOrStop();                                /* the base class repeats what the derived class did */
    CHECK(g.installedGid() == MAIL && g.userGid() == USER);
    CHECK(id.e == MAIL);                           /* and does not touch the state */
    g.disableOrStop();
    CHECK(id.e == USER);
    return 0;
}
static int refused_drop_at_startup_stops(void)
{
    MailGroup g;
    start(MAIL);
    fail_ops = 1;
    CHECK(STOPS(g.dropOrStop()) == 1);
    CHECK(strstr(message, "cannot give up the group"));
    CHECK(id.e == MAIL);                           /* the program did not go on */
    return 0;
}
static int ignored_drop_at_startup_stops(void)
{
    MailGroup g;
    start(MAIL);
    ignore_changes = 1;                            /* success is reported and nothing happens */
    CHECK(STOPS(g.dropOrStop()) == 1);
    CHECK(strstr(message, "cannot give up the group"));
    return 0;
}
static int refused_drop_after_a_spool_operation_stops(void)
{
    MailGroup g;
    start(MAIL);
    g.dropOrStop();
    CHECK(g.enable());
    fail_ops = 1;
    CHECK(STOPS(g.disableOrStop()) == 1);
    CHECK(strstr(message, "cannot give up the group"));
    return 0;
}
static int ignored_drop_after_a_spool_operation_stops(void)
{
    MailGroup g;
    start(MAIL);
    g.dropOrStop();
    CHECK(g.enable());
    ignore_changes = 1;
    CHECK(STOPS(g.disableOrStop()) == 1);
    return 0;
}
static int refused_enable_leaves_the_user_group(void)
{
    MailGroup g;
    start(MAIL);
    g.dropOrStop();
    fail_ops = 1;
    CHECK(!g.enable());
    CHECK(id.e == USER);
    CHECK(STOPS(g.disableOrStop()) == -1);         /* nothing to give up, nothing to stop */
    return 0;
}
static int ignored_enable_is_not_reported_as_success(void)
{
    MailGroup g;
    start(MAIL);
    g.dropOrStop();
    ignore_changes = 1;
    CHECK(!g.enable());
    CHECK(id.e == USER);
    return 0;
}
static int enable_before_startup_does_nothing(void)
{
    MailGroup g;
    start(MAIL);
    CHECK(!g.enable());
    CHECK(calls == 0 && id.e == MAIL);             /* it is the startup that takes the group away */
    return 0;
}
static int user_group_is_never_replaced_by_the_installed_one(void)
{
    MailGroup g;
    start(MAIL);
    g.dropOrStop();
    g.enable();
    g.disableOrStop();
    CHECK(id.r == USER && id.e == USER && id.s == MAIL);
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"plain_install_makes_no_call", plain_install_makes_no_call},
    {"group_is_given_up_at_startup", group_is_given_up_at_startup},
    {"group_is_held_only_between_enable_and_disable", group_is_held_only_between_enable_and_disable},
    {"second_start_keeps_the_installed_group", second_start_keeps_the_installed_group},
    {"refused_drop_at_startup_stops", refused_drop_at_startup_stops},
    {"ignored_drop_at_startup_stops", ignored_drop_at_startup_stops},
    {"refused_drop_after_a_spool_operation_stops", refused_drop_after_a_spool_operation_stops},
    {"ignored_drop_after_a_spool_operation_stops", ignored_drop_after_a_spool_operation_stops},
    {"refused_enable_leaves_the_user_group", refused_enable_leaves_the_user_group},
    {"ignored_enable_is_not_reported_as_success", ignored_enable_is_not_reported_as_success},
    {"enable_before_startup_does_nothing", enable_before_startup_does_nothing},
    {"user_group_is_never_replaced_by_the_installed_one", user_group_is_never_replaced_by_the_installed_one},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    if (argc < 2) {
        for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name);
        return 0;
    }
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cxx = shlex.split(os.environ.get("CXX", "g++"))
failed = []
count = 0
header = source / "include" / "DtMail" / "MailGroup.hh"
if not header.exists():
    sys.exit("no %s: the group of a set-group-id installation is not handled in one place" % header)
with tempfile.TemporaryDirectory(prefix="dtmail-group-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    cc = work / "group.cc"
    cc.write_text(harness)
    binary = work / "group"
    build = cxx + ["-std=gnu++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
                   "-I", str(source), str(cc), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr)
        sys.exit("cannot compile the group harness")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
    for scenario in names:
        count += 1
        run = subprocess.run([str(binary), scenario], capture_output=True, text=True)
        if run.returncode:
            failed.append(scenario)
            sys.stderr.write("%s failed:\n%s\n" % (scenario, run.stderr.strip().splitlines()[0] if run.stderr.strip() else "exit %d" % run.returncode))
if failed:
    sys.exit("%d of %d group scenarios failed: %s" % (len(failed), count, ", ".join(failed)))
print("Group: %d scenarios passed" % count)

# The behaviour of a real set-group-id installation needs a privileged tracer (the procedure is in
# docs/improvements/mail.md), so the order of the startup and the single owner of the group calls are
# source guards.
problems = []


def body_of(text, start_pattern):
    match = re.search(start_pattern, text, re.M)
    if not match:
        return None
    opening = text.index("{", match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[opening:end]


roam = (source / "dtmail" / "RoamApp.C").read_text(errors="replace")
body = body_of(roam, r"^void RoamApp::initialize\(int \*argcp, char \*\*argv\)\n")
if body is None:
    problems.append("RoamApp::initialize not found")
else:
    drop = body.find("initializeGroupPrivileges()")
    if drop < 0:
        problems.append("RoamApp::initialize never gives up the group of the installed program")
    else:
        early = [name for name in ("getenv(", "setlocale(", "XtSetLanguageProc(", "putenv(", "_DtEnvControl(",
                                   "Application::initialize(", "signal(", "sigaction(", "getopt(")
                 if 0 <= body.find(name) < drop]
        if early:
            problems.append("RoamApp::initialize calls %s before it gives up the group" % ", ".join(early))

pr = (source / "dtmailpr" / "main.C").read_text(errors="replace")
body = body_of(pr, r"^main \(int argc, char \*\*argv\)\n")
if body is None:
    problems.append("dtmailpr main() not found")
else:
    drop = body.find("mailGroup.dropOrStop()")
    if drop < 0:
        problems.append("dtmailpr never gives up the group of the installed program")
    else:
        early = [name for name in ("getenv(", "setlocale(", "_DtEnvControl(", "Dt_nlInit(", "getopt(", "fopen(", "open(")
                 if 0 <= body.find(name) < drop]
        if early:
            problems.append("dtmailpr calls %s before it gives up the group" % ", ".join(early))

for path in sorted(source.rglob("*")):
    if path.suffix not in (".C", ".c", ".h", ".hh", ".cc") or path.name == "MailGroup.hh":
        continue
    if "tests" in path.parts:
        continue
    content = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(errors="replace"), flags=re.S)
    if re.search(r"\b(?:setgid|setegid|setregid|setresgid|setuid|seteuid|setreuid|setresuid)\s*\(", content):
        problems.append("%s changes an identity itself" % path.relative_to(source))
if problems:
    sys.exit("group source guard failed:\n  " + "\n  ".join(problems))
print("Group source guard: the group is given up first; MailGroup.hh is the only code that changes it")
