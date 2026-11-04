#!/usr/bin/env python3
"""Run dtsession's startup guard for its identity against a fake kernel that can refuse.

dtsession is installed with the identity of the user who runs it.  Should it be
installed setuid or setgid anyway, SmPrivDropInstalledIdentity() (SmPriv.c)
gives the installed identity up for good, before anything else, and stops the
program when that fails.  The production code is compiled with stand-ins for
the process identity (real, effective and saved user and group ids) and for the
set*id() calls, including a user at a process limit (EAGAIN), a kernel that
reports success without changing anything, and one that leaves the saved id
untouched so that there is a way back.  Every scenario runs in its own process
under AddressSanitizer and UndefinedBehaviorSanitizer.

DTSESSION_SOURCE names another copy of the sources to test, for example one
with a deliberate defect.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])

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
static struct { uid_t r, e, s; gid_t gr, ge, gs; } id;
static int fail_uid_ops, fail_gid_ops, ignore_changes, keep_saved_uid, keep_saved_gid;
static int uid_calls, gid_calls, regain_attempts;
static jmp_buf escape;
static int exit_code;
static char message[256];

#define NONE ((uid_t)-1)
#define PRIVILEGED (id.e == 0)
static int one_of(uid_t v, uid_t a, uid_t b, uid_t c) { return v == a || v == b || v == c; }

static uid_t fake_getuid(void) { return id.r; }
static uid_t fake_geteuid(void) { return id.e; }
static gid_t fake_getgid(void) { return id.gr; }
static gid_t fake_getegid(void) { return id.ge; }

/* the rules of the Linux calls; the saved id follows the effective one when the real one is set or the effective one changes */
static int fake_setreuid(uid_t r, uid_t e)
{
    uid_t old_r = id.r;
    uid_calls++;
    if (fail_uid_ops) { errno = EAGAIN; return -1; }
    if (ignore_changes) return 0;
    if (!PRIVILEGED && ((r != NONE && !one_of(r, id.r, id.e, id.e)) ||
                        (e != NONE && !one_of(e, id.r, id.e, id.s)))) { errno = EPERM; return -1; }
    if (r != NONE) id.r = r;
    if (e != NONE) id.e = e;
    if ((r != NONE || (e != NONE && e != old_r)) && !keep_saved_uid) id.s = id.e;
    return 0;
}
static int fake_seteuid(uid_t e)
{
    uid_calls++;
    if (fail_uid_ops) { errno = EAGAIN; return -1; }
    if (ignore_changes) return 0;
    if (!PRIVILEGED && !one_of(e, id.r, id.s, id.s)) { errno = EPERM; return -1; }
    id.e = e;
    return 0;
}
static int fake_setuid(uid_t u)
{
    uid_calls++;
    if (fail_uid_ops) { errno = EAGAIN; return -1; }
    if (ignore_changes) return 0;
    if (PRIVILEGED) { id.r = id.e = u; if (!keep_saved_uid) id.s = u; return 0; }
    if (u == id.r || u == id.s) { id.e = u; return 0; }
    errno = EPERM;
    return -1;
}
static int fake_setregid(gid_t r, gid_t e)
{
    gid_t old_r = id.gr;
    gid_calls++;
    if (fail_gid_ops) { errno = EPERM; return -1; }
    if (ignore_changes) return 0;
    if (!PRIVILEGED && ((r != NONE && !one_of(r, id.gr, id.ge, id.ge)) ||
                        (e != NONE && !one_of(e, id.gr, id.ge, id.gs)))) { errno = EPERM; return -1; }
    if (r != NONE) id.gr = r;
    if (e != NONE) id.ge = e;
    if ((r != NONE || (e != NONE && e != old_r)) && !keep_saved_gid) id.gs = id.ge;
    return 0;
}
static int fake_setegid(gid_t e)
{
    gid_calls++;
    if (fail_gid_ops) { errno = EPERM; return -1; }
    if (ignore_changes) return 0;
    if (!PRIVILEGED && !one_of(e, id.gr, id.gs, id.gs)) { errno = EPERM; return -1; }
    id.ge = e;
    return 0;
}
static int fake_setgid(gid_t g)
{
    gid_calls++;
    if (fail_gid_ops) { errno = EPERM; return -1; }
    if (ignore_changes) return 0;
    if (PRIVILEGED) { id.gr = id.ge = g; if (!keep_saved_gid) id.gs = g; return 0; }
    if (g == id.gr || g == id.gs) { id.ge = g; return 0; }
    errno = EPERM;
    return -1;
}
static ssize_t fake_write(int fd, const void *buf, size_t n)
{
    if (fd == STDERR_FILENO) {
        size_t have = strlen(message);
        if (have + n < sizeof(message)) { memcpy(message + have, buf, n); message[have + n] = 0; }
    }
    return (ssize_t) n;
}

#define getuid fake_getuid
#define geteuid fake_geteuid
#define getgid fake_getgid
#define getegid fake_getegid
#define setreuid fake_setreuid
#define seteuid fake_seteuid
#define setuid fake_setuid
#define setregid fake_setregid
#define setegid fake_setegid
#define setgid fake_setgid
#define write fake_write
#define _exit(code) (exit_code = (code), longjmp(escape, 1))

#include "SmPriv.c"

#undef _exit
#undef write

/* A program started by user 1000, group 100, from a file owned by the given user and group. */
static void start(uid_t euid, gid_t egid)
{
    memset(&id, 0, sizeof(id));
    id.r = 1000; id.e = euid; id.s = euid;
    id.gr = 100; id.ge = egid; id.gs = egid;
    fail_uid_ops = fail_gid_ops = ignore_changes = keep_saved_uid = keep_saved_gid = 0;
    uid_calls = gid_calls = regain_attempts = 0;
    message[0] = 0;
}

/* Returns the exit code the code under test asked for, or -1 if it returned. */
static int run(void)
{
    exit_code = -1;
    if (setjmp(escape) == 0)
        SmPrivDropInstalledIdentity();
    return exit_code;
}

/* what an attacker with a memory corruption would try afterwards: take the installed identity back */
static int can_regain(uid_t user, gid_t group, uid_t installed_user, gid_t installed_group)
{
    struct { uid_t r, e, s; gid_t gr, ge, gs; } before = { id.r, id.e, id.s, id.gr, id.ge, id.gs };
    int regained = 0;
    (void) user; (void) group;
    if (installed_user != 1000 && (fake_seteuid(installed_user) == 0 || fake_setuid(installed_user) == 0 || fake_setreuid(installed_user, installed_user) == 0)) regained = 1;
    if (installed_group != 100 && (fake_setegid(installed_group) == 0 || fake_setgid(installed_group) == 0 || fake_setregid(installed_group, installed_group) == 0)) regained = 1;
    if (id.e != before.e || id.ge != before.ge) regained = 1;
    return regained;
}
#define USER_ONLY() (id.r == 1000 && id.e == 1000 && id.s == 1000 && id.gr == 100 && id.ge == 100 && id.gs == 100)

static int plain_install_does_nothing(void)
{
    start(1000, 100);
    fail_uid_ops = fail_gid_ops = 1;              /* any call at all would fail the program */
    CHECK(run() == -1);
    CHECK(uid_calls == 0 && gid_calls == 0);
    CHECK(id.r == 1000 && id.e == 1000 && id.gr == 100 && id.ge == 100);
    start(0, 0); id.r = 0; id.gr = 0; id.s = 0; id.gs = 0;     /* started by root */
    fail_uid_ops = fail_gid_ops = 1;
    CHECK(run() == -1);
    CHECK(uid_calls == 0 && gid_calls == 0);
    return 0;
}
static int setuid_root_is_dropped(void)
{
    start(0, 100);
    CHECK(run() == -1);
    CHECK(USER_ONLY());
    CHECK(gid_calls == 0);                         /* the group was the user's already */
    CHECK(!can_regain(1000, 100, 0, 100));
    return 0;
}
static int setuid_other_user_is_dropped(void)
{
    start(33, 100);                                /* installed setuid for an account other than root */
    CHECK(run() == -1);
    CHECK(USER_ONLY() && !can_regain(1000, 100, 33, 100));
    return 0;
}
static int root_running_a_setuid_program(void)
{
    start(33, 5);
    id.r = 0; id.gr = 0;                           /* root started a program that is setuid and setgid for other accounts */
    fail_uid_ops = fail_gid_ops = 1;
    CHECK(run() == -1);                            /* root can take any identity: there is nothing to give up, nothing is called */
    CHECK(uid_calls == 0 && gid_calls == 0);
    CHECK(id.r == 0 && id.e == 33 && id.ge == 5);
    return 0;
}
static int setgid_is_dropped(void)
{
    start(1000, 5);
    CHECK(run() == -1);
    CHECK(USER_ONLY());
    CHECK(uid_calls == 0);                         /* the user was the user's already */
    CHECK(!can_regain(1000, 100, 1000, 5));
    return 0;
}
static int setuid_and_setgid_are_dropped(void)
{
    start(0, 5);
    CHECK(run() == -1);
    CHECK(USER_ONLY() && !can_regain(1000, 100, 0, 5));
    start(33, 7);
    CHECK(run() == -1);
    CHECK(USER_ONLY() && !can_regain(1000, 100, 33, 7));
    return 0;
}
static int the_way_back_is_tried(void)
{
    /* the guard does not take the kernel's word for it: it tries to take the installed ids back */
    start(0, 5);
    CHECK(run() == -1);
    CHECK(uid_calls >= 3 && gid_calls >= 3);       /* the drop and at least one attempt each to get back */
    return 0;
}
static int refused_user_drop_stops(void)
{
    start(0, 100);
    fail_uid_ops = 1;                              /* the user is at a process limit */
    CHECK(run() != -1);                            /* does not return as root */
    CHECK(strstr(message, "cannot give up the user"));
    start(33, 5);
    fail_uid_ops = 1;
    CHECK(run() != -1);
    return 0;
}
static int refused_group_drop_stops(void)
{
    start(1000, 5);
    fail_gid_ops = 1;
    CHECK(run() != -1);
    CHECK(strstr(message, "cannot give up the group"));
    start(0, 5);
    fail_gid_ops = 1;
    CHECK(run() != -1);
    return 0;
}
static int ignored_drop_stops(void)
{
    start(0, 100);
    ignore_changes = 1;                            /* success is reported and nothing happens */
    CHECK(run() != -1);
    CHECK(strstr(message, "cannot give up the user"));
    start(1000, 5);
    ignore_changes = 1;
    CHECK(run() != -1);
    CHECK(strstr(message, "cannot give up the group"));
    start(0, 5);
    ignore_changes = 1;
    CHECK(run() != -1);
    return 0;
}
static int open_way_back_to_the_user_stops(void)
{
    start(0, 100);
    keep_saved_uid = 1;                            /* the saved user id stays root */
    CHECK(run() != -1);
    CHECK(strstr(message, "user of the installed program was not given up"));
    return 0;
}
static int open_way_back_to_the_group_stops(void)
{
    start(1000, 5);
    keep_saved_gid = 1;                            /* the saved group id stays the installed one */
    CHECK(run() != -1);
    CHECK(strstr(message, "group of the installed program was not given up"));
    start(0, 5);
    keep_saved_gid = 1;
    CHECK(run() != -1);
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"plain_install_does_nothing", plain_install_does_nothing},
    {"setuid_root_is_dropped", setuid_root_is_dropped},
    {"setuid_other_user_is_dropped", setuid_other_user_is_dropped},
    {"root_running_a_setuid_program", root_running_a_setuid_program},
    {"setgid_is_dropped", setgid_is_dropped},
    {"setuid_and_setgid_are_dropped", setuid_and_setgid_are_dropped},
    {"the_way_back_is_tried", the_way_back_is_tried},
    {"refused_user_drop_stops", refused_user_drop_stops},
    {"refused_group_drop_stops", refused_group_drop_stops},
    {"ignored_drop_stops", ignored_drop_stops},
    {"open_way_back_to_the_user_stops", open_way_back_to_the_user_stops},
    {"open_way_back_to_the_group_stops", open_way_back_to_the_group_stops},
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

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
count = 0
with tempfile.TemporaryDirectory(prefix="dtsession-priv-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    c = work / "privileges.c"
    c.write_text(harness)
    binary = work / "privileges"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
                  "-I", str(source), str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr)
        sys.exit("cannot compile the identity harness")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
    for scenario in names:
        count += 1
        run = subprocess.run([str(binary), scenario], capture_output=True, text=True)
        if run.returncode:
            failed.append(scenario)
            sys.stderr.write("%s failed:\n%s\n" % (scenario, run.stderr.strip().splitlines()[0] if run.stderr.strip() else "exit %d" % run.returncode))
if failed:
    sys.exit("%d of %d identity scenarios failed: %s" % (len(failed), count, ", ".join(failed)))
print("Identity: %d scenarios passed" % count)

# The identity has to be given up before main() reads the environment: the behaviour needs a setuid
# binary and a privileged tracer (the procedure is in docs/improvements/session-manager.md), so this is a
# source guard.  The other half of it: nothing in the program can take an identity back or needs one.
problems = []
main_c = (source / "SmMain.c").read_text(errors="replace")
match = re.search(r"^main \(int argc, char \*\*argv\)\n\{", main_c, re.M)
if not match:
    sys.exit("main() not found")
body = main_c[match.end():]
body = body[:body.index("\n}\n")]
drop = body.find("SmPrivDropInstalledIdentity()")
if drop < 0:
    problems.append("main() never gives up the installed identity")
else:
    early = [name for name in ("getenv(", "setlocale(", "XtSetLanguageProc(", "putenv(", "strdup(", "XtOpenDisplay(",
                               "XtToolkitInitialize(", "_DtEnvControl(", "InitNlsStrings(", "RestoreResources(")
             if 0 <= body.find(name) < drop]
    if early:
        problems.append("main() calls %s before it gives up the installed identity" % ", ".join(early))

code = {p.name: re.sub(r"/\*.*?\*/", "", p.read_text(errors="replace"), flags=re.S)
        for p in sorted(source.glob("*.c")) + sorted(source.glob("*.h"))}
for name, content in code.items():
    for pattern in (r"\bSmPrivRaise\b", r"\bSmPrivLower\b", r"\bSmPrivDropInChild\b", r"\bSmPrivUseUserGroup\b",
                    r"\bGetMemoryUtilization\b", r"\bUsesProtectedPasswordFile\b", r"\bAixEnableHftRing\b",
                    r"\bkeepPasswordAccess\b", r"\bpasswordAccess\b"):
        if re.search(pattern, content):
            problems.append("%s still has %s" % (name, pattern))
if re.search(r"\bsetreuid\s*\(|\bseteuid\s*\(|\bsetregid\s*\(|\bsetegid\s*\(|\bsetuid\s*\(|\bsetgid\s*\(",
             "".join(c for n, c in code.items() if n != "SmPriv.c")):
    problems.append("an identity is changed outside SmPriv.c")
if problems:
    sys.exit("identity source guard failed:\n  " + "\n  ".join(problems))
print("Identity source guard: main() gives up the installed identity first; nothing takes one back")
