#!/usr/bin/env python3
"""Run dtterm's setuid-root handling against a fake kernel that can refuse.

The production functions are cut out of TermPrim/TermPrimUtil.c and compiled
with stubs that model the process identity (real, effective and saved ids) and
the set*uid() family, including a user at a process limit (EAGAIN), a kernel
that reports success without changing anything and one that leaves a way back
to root.  The build variants of the platform macros HAS_SETRESUID, HAS_SETREUID
and HAS_SETEUID are each compiled.  TERMPRIM_UTIL names another copy of the
source to test, e.g. an older revision.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(__file__).resolve().parents[1]
path = Path(os.environ.get("TERMPRIM_UTIL", source / "TermPrim" / "TermPrimUtil.c"))
text = path.read_text()


def function(name, required=True):
    match = re.search(r"^(?:static\s+)?void\n" + re.escape(name) + r"\s*\(", text, re.M)
    if not match:
        assert not required, name
        return ""
    opening = text.index("{", match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end] + "\n"


state = "".join(re.findall(r"^static (?:Boolean first|uid_t \w+|gid_t \w+)[^;\n]*;\n", text, re.M))
assert "uid_user" in state and "gid_root" in state
code = state + function("suidInit") + function("suidFatal", required=False) \
    + function("_DtTermPrimRemoveSuidRoot") + function("_DtTermPrimToggleSuidRoot")

harness = r'''
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

typedef int Boolean;
#define True 1
#define False 0
#define _DtTermProcessLock() ((void)0)
#define _DtTermProcessUnlock() ((void)0)

/* the fake process identity and the faults a kernel can show */
static struct { uid_t r, e, s; gid_t gr, ge, gs; } id;
static int fail_uid_ops, ignore_changes, keep_saved_uid;
static jmp_buf escape;
static int exit_code;

static uid_t fake_getuid(void) { return id.r; }
static uid_t fake_geteuid(void) { return id.e; }
static gid_t fake_getgid(void) { return id.gr; }
static gid_t fake_getegid(void) { return id.ge; }
#define PRIVILEGED (id.e == 0)
#define NONE ((uid_t)-1)
static int one_of(uid_t v, uid_t a, uid_t b, uid_t c) { return v == a || v == b || v == c; }

static int fake_setresuid(uid_t r, uid_t e, uid_t s)
{
    if (fail_uid_ops) { errno = EAGAIN; return -1; }
    if (ignore_changes) return 0;
    if (!PRIVILEGED && ((r != NONE && !one_of(r, id.r, id.e, id.s)) ||
                        (e != NONE && !one_of(e, id.r, id.e, id.s)) ||
                        (s != NONE && !one_of(s, id.r, id.e, id.s)))) { errno = EPERM; return -1; }
    if (r != NONE) id.r = r;
    if (e != NONE) id.e = e;
    if (s != NONE) id.s = s;
    return 0;
}
static int fake_setreuid(uid_t r, uid_t e)
{
    uid_t old_r = id.r;
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
    if (fail_uid_ops) { errno = EAGAIN; return -1; }
    if (ignore_changes) return 0;
    if (!PRIVILEGED && !one_of(e, id.r, id.s, id.s)) { errno = EPERM; return -1; }
    id.e = e;
    return 0;
}
static int fake_setuid(uid_t u)
{
    if (fail_uid_ops) { errno = EAGAIN; return -1; }
    if (ignore_changes) return 0;
    if (PRIVILEGED) { id.r = id.e = u; if (!keep_saved_uid) id.s = u; return 0; }
    if (u == id.r || u == id.s) { id.e = u; return 0; }
    errno = EPERM;
    return -1;
}
static int fake_setresgid(gid_t r, gid_t e, gid_t s)
{
    if (ignore_changes) return 0;
    if (!PRIVILEGED && ((r != NONE && !one_of(r, id.gr, id.ge, id.gs)) ||
                        (e != NONE && !one_of(e, id.gr, id.ge, id.gs)) ||
                        (s != NONE && !one_of(s, id.gr, id.ge, id.gs)))) { errno = EPERM; return -1; }
    if (r != NONE) id.gr = r;
    if (e != NONE) id.ge = e;
    if (s != NONE) id.gs = s;
    return 0;
}
static int fake_setregid(gid_t r, gid_t e)
{
    if (ignore_changes) return 0;
    if (!PRIVILEGED && ((r != NONE && !one_of(r, id.gr, id.ge, id.ge)) ||
                        (e != NONE && !one_of(e, id.gr, id.ge, id.gs)))) { errno = EPERM; return -1; }
    if (r != NONE) id.gr = r;
    if (e != NONE) id.ge = e;
    return 0;
}
static int fake_setegid(gid_t e)
{
    if (ignore_changes) return 0;
    if (!PRIVILEGED && !one_of(e, id.gr, id.gs, id.gs)) { errno = EPERM; return -1; }
    id.ge = e;
    return 0;
}
static int fake_setgid(gid_t g)
{
    if (ignore_changes) return 0;
    if (PRIVILEGED) { id.gr = id.ge = id.gs = g; return 0; }
    if (g == id.gr || g == id.gs) { id.ge = g; return 0; }
    errno = EPERM;
    return -1;
}

#define getuid fake_getuid
#define geteuid fake_geteuid
#define getgid fake_getgid
#define getegid fake_getegid
#define setresuid fake_setresuid
#define setreuid fake_setreuid
#define seteuid fake_seteuid
#define setuid fake_setuid
#define setresgid fake_setresgid
#define setregid fake_setregid
#define setegid fake_setegid
#define setgid fake_setgid
#define _exit(code) (exit_code = (code), longjmp(escape, 1))

''' + code + r'''
#undef _exit

/* An installed setuid-root program started by user 1000, group 100. */
static void setuid_root_install(void)
{
    id.r = 1000; id.e = 0; id.s = 0;
    id.gr = id.ge = id.gs = 100;
    fail_uid_ops = ignore_changes = keep_saved_uid = 0;
    first = True;
}

/* Returns the exit code the code under test asked for, or -1 if it returned. */
static int run(void (*fn)(void))
{
    exit_code = -1;
    if (setjmp(escape) == 0)
        fn();
    return exit_code;
}
static void do_remove(void) { _DtTermPrimRemoveSuidRoot(); }
static void do_user(void) { _DtTermPrimToggleSuidRoot(False); }
static void do_root(void) { _DtTermPrimToggleSuidRoot(True); }

static void scenario_remove_works(void)
{
    setuid_root_install();
    assert(run(do_remove) == -1);
    assert(id.r == 1000 && id.e == 1000 && id.s == 1000);
    assert(id.gr == 100 && id.ge == 100 && id.gs == 100);
}
static void scenario_remove_refused(void)
{
    setuid_root_install();
    do_root();                       /* suidInit() has seen euid 0 */
    fail_uid_ops = 1;                /* the user is at a process limit */
    assert(run(do_remove) != -1);    /* must not return as root */
    assert(id.e == 0 || exit_code != -1);
}
static void scenario_remove_ignored(void)
{
    setuid_root_install();
    ignore_changes = 1;              /* success reported, nothing changed */
    assert(run(do_remove) != -1);
}
static void scenario_remove_leaves_way_back(void)
{
    setuid_root_install();
    keep_saved_uid = 1;              /* saved uid still 0: root can be regained */
#ifdef HAS_SETRESUID
    /* setresuid() sets the saved id itself, so there is no way back to leave */
    assert(run(do_remove) == -1 && id.s == 1000);
#else
    assert(run(do_remove) != -1);
#endif
}
static void scenario_remove_not_installed(void)
{
    id.r = id.e = id.s = 1000;
    id.gr = id.ge = id.gs = 100;
    fail_uid_ops = ignore_changes = keep_saved_uid = 0;
    first = True;
    assert(run(do_remove) == -1);    /* nothing to give up, nothing to fail */
    assert(id.e == 1000);
}
static void scenario_remove_as_root(void)
{
    id.r = id.e = id.s = 0;
    id.gr = id.ge = id.gs = 0;
    fail_uid_ops = ignore_changes = keep_saved_uid = 0;
    first = True;
    assert(run(do_remove) == -1);    /* root has no way back to lose */
}
static void scenario_toggle_works(void)
{
    setuid_root_install();
    assert(run(do_user) == -1 && id.e == 1000);
    assert(run(do_root) == -1 && id.e == 0);
    assert(run(do_user) == -1 && id.e == 1000);
}
static void scenario_toggle_down_refused(void)
{
    setuid_root_install();
    assert(run(do_root) == -1);      /* the euid is 0 and stays 0 */
    fail_uid_ops = 1;
    assert(run(do_user) != -1);      /* must not carry on as root */
}
static void scenario_toggle_up_refused(void)
{
    setuid_root_install();
    assert(run(do_user) == -1);
    fail_uid_ops = 1;
    assert(run(do_root) == -1);      /* only the privileged step is lost */
    assert(id.e == 1000);
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    { "remove-works", scenario_remove_works },
    { "remove-refused", scenario_remove_refused },
    { "remove-ignored", scenario_remove_ignored },
    { "remove-leaves-way-back", scenario_remove_leaves_way_back },
    { "remove-not-installed", scenario_remove_not_installed },
    { "remove-as-root", scenario_remove_as_root },
    { "toggle-works", scenario_toggle_works },
    { "toggle-down-refused", scenario_toggle_down_refused },
    { "toggle-up-refused", scenario_toggle_up_refused },
};

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
            puts(scenarios[i].name);
        return 0;
    }
    assert(argc == 2);
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        if (strcmp(scenarios[i].name, argv[1]) == 0) { scenarios[i].run(); return 0; }
    return 2;
}
'''

variants = {
    "setresuid": ["-DHAS_SETRESUID", "-DHAS_SETREUID", "-DHAS_SETEUID"],
    "setreuid+seteuid": ["-DHAS_SETREUID", "-DHAS_SETEUID"],
    "seteuid": ["-DHAS_SETEUID"],
}
cc = shlex.split(os.environ.get("CC", "cc"))
failed, total = [], 0
with tempfile.TemporaryDirectory(prefix="suid-root-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    (work / "test.c").write_text(harness)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:" + os.environ.get("ASAN_OPTIONS", ""))
    for variant, defines in variants.items():
        binary = work / ("test-" + variant.replace("+", "-"))
        subprocess.run(cc + ["-std=gnu11", "-UNDEBUG", "-g", "-O1", "-fno-omit-frame-pointer",
                             "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                             "-Wno-unused-function", "-Wno-unused-variable"] + defines
                       + [str(work / "test.c"), "-o", str(binary)], check=True)
        names = subprocess.run([str(binary), "--list"], env=env, stdout=subprocess.PIPE, text=True).stdout.split()
        for name in names:
            total += 1
            run = subprocess.run([str(binary), name], env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
            if run.returncode:
                failed.append(variant + ": " + name)
                report = [l for l in run.stdout.splitlines() if "ssertion" in l or "ERROR" in l]
                print("FAIL %s: %s (exit %d) %s" % (variant, name, run.returncode, (report or [""])[0][:110]))
if failed:
    sys.exit("dtterm setuid handling failed: %d of %d scenarios" % (len(failed), total))
print("dtterm setuid-root handling: %d scenarios passed in %d platform variants" % (total, len(variants)))
