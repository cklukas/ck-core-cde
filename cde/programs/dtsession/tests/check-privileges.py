#!/usr/bin/env python3
"""Run dtsession's identity handling against a fake kernel that can refuse.

dtsession is installed setuid root on systems without PAM.  The production
code (SmPriv.c) is compiled with stand-ins for the process identity (real,
effective and saved ids, user and group) and for the set*id() calls, including
a user at a process limit (EAGAIN), a kernel that reports success without
changing anything and one that leaves a way back to root.  Every scenario runs
under AddressSanitizer and UndefinedBehaviorSanitizer, in the variants of the
build with and without SVR4.

DTSESSION_SOURCE names another copy of the sources to test, for example a
revision from before SmPriv.c existed: the identity statements of main() and of
the child processes are then cut out of SmMain.c and SmGlobals.c and run the
same way.  Such a tree must fail.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
fixed = (source / "SmPriv.c").exists()

if fixed:
    code = '#include "%s"\n' % (source / "SmPriv.c")
else:
    main_c = (source / "SmMain.c").read_text()
    globals_c = (source / "SmGlobals.c").read_text()
    sm_h = (source / "Sm.h").read_text()
    macros = re.search(r"#ifndef\s+SVR4\n#define\s+SM_SETEUID.*?#endif\n", sm_h, re.S)
    assert macros, "SM_SETEUID wrappers not found"
    start = main_c.index("    smGD.runningGID = getgid();")
    stop = main_c.index("    /*\n     * Initialize LANG")
    identity = main_c[start:stop]
    identity = re.sub(r"# ifdef SECURE_SYS_PATH\n\s*status = stat\(SECURE_SYS_PATH, &buf\);\n# else\n\s*status = -1;\n# endif",
                      "status = keep ? 0 : -1;", identity)
    assert "keep ?" in identity, "secure system test not found"
    swap = re.search(r"#ifndef\s+SVR4\n\s*setregid\(smGD\.conMgmtGID, smGD\.runningGID\);\n#else.*?#endif[^\n]*\n", main_c, re.S)
    assert swap, "group swap not found"
    child = re.search(r"#ifndef\s+SVR4\n\s*setregid\(smGD\.runningGID, smGD\.runningGID\);\n#else.*?#endif\n", globals_c, re.S)
    assert child, "child group drop not found"
    code = macros.group(0) + r'''
#define True 1
#define False 0
static struct { gid_t runningGID, conMgmtGID; uid_t runningUID, unLockUID; int secureSystem; } smGD;
static int dummy_buf;
void SmPrivDropInstalledIdentity(int keep)
{
    int status; struct { int unused; } buf; (void) buf;
''' + identity + swap.group(0) + r'''
}
int SmPrivRaise(void) { if (!smGD.secureSystem) return 0; return SM_SETEUID(smGD.unLockUID) == 0; }
void SmPrivLower(void) { if (smGD.secureSystem) SM_SETEUID(smGD.runningUID); }
void SmPrivDropInChild(void) {
''' + child.group(0) + r'''
}
int SmPrivUseUserGroup(int user)
{
    if (user) {
#ifndef SVR4
        setregid(smGD.conMgmtGID, smGD.runningGID);
#else
        setgid(smGD.conMgmtGID); setegid(smGD.runningGID);
#endif
    } else {
#ifndef SVR4
        setregid(smGD.runningGID, smGD.conMgmtGID);
#else
        setgid(smGD.runningGID); setegid(smGD.conMgmtGID);
#endif
    }
    return 1;
}
'''

harness = r'''
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* the fake process identity and the faults a kernel can show */
static struct { uid_t r, e, s; gid_t gr, ge, gs; } id;
static int fail_uid_ops, ignore_changes, keep_saved_uid, fail_gid_ops, id_calls;
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

static int fake_setreuid(uid_t r, uid_t e)
{
    uid_t old_r = id.r;
    id_calls++;
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
    id_calls++;
    if (fail_uid_ops) { errno = EAGAIN; return -1; }
    if (ignore_changes) return 0;
    if (!PRIVILEGED && !one_of(e, id.r, id.s, id.s)) { errno = EPERM; return -1; }
    id.e = e;
    return 0;
}
static int fake_setuid(uid_t u)
{
    id_calls++;
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
    if (fail_gid_ops) { errno = EPERM; return -1; }
    if (ignore_changes) return 0;
    if (!PRIVILEGED && ((r != NONE && !one_of(r, id.gr, id.ge, id.ge)) ||
                        (e != NONE && !one_of(e, id.gr, id.ge, id.gs)))) { errno = EPERM; return -1; }
    if (r != NONE) id.gr = r;
    if (e != NONE) id.ge = e;
    if (r != NONE || (e != NONE && e != old_r)) id.gs = id.ge;
    return 0;
}
static int fake_setegid(gid_t e)
{
    if (fail_gid_ops) { errno = EPERM; return -1; }
    if (ignore_changes) return 0;
    if (!PRIVILEGED && !one_of(e, id.gr, id.gs, id.gs)) { errno = EPERM; return -1; }
    id.ge = e;
    return 0;
}
static int fake_setgid(gid_t g)
{
    if (fail_gid_ops) { errno = EPERM; return -1; }
    if (ignore_changes) return 0;
    if (PRIVILEGED) { id.gr = id.ge = id.gs = g; return 0; }
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

''' + code + r'''
#undef _exit
#undef write

void SmPrivDropInstalledIdentity(int);
int SmPrivRaise(void);
void SmPrivLower(void);
void SmPrivDropInChild(void);
int SmPrivUseUserGroup(int);

/* A program started by user 1000, group 100. */
static void start(uid_t euid, gid_t egid)
{
    memset(&id, 0, sizeof(id));
    id.r = 1000; id.e = euid; id.s = euid;
    id.gr = 100; id.ge = egid; id.gs = egid;
    fail_uid_ops = ignore_changes = keep_saved_uid = fail_gid_ops = id_calls = 0;
    message[0] = 0;
}
static void setuid_root(void) { start(0, 100); }

/* Returns the exit code the code under test asked for, or -1 if it returned. */
static int keep_flag;
static void do_drop(void) { SmPrivDropInstalledIdentity(keep_flag); }
static void do_child(void) { SmPrivDropInChild(); }
static void do_lower(void) { SmPrivLower(); }
static int run(void (*fn)(void))
{
    exit_code = -1;
    if (setjmp(escape) == 0)
        fn();
    return exit_code;
}
static int drop(int keep) { keep_flag = keep; return run(do_drop); }

/* once the program has exited it is no longer root: it did not go on with e == 0 */
#define MUST_STOP_OR_BE_USER(code) assert((code) != -1 || (id.r == 1000 && id.e == 1000))

static void drops_for_good(void)
{
    setuid_root();
    assert(drop(0) == -1);
    assert(id.r == 1000 && id.e == 1000 && id.s == 1000);
    assert(!SmPrivRaise());
    assert(id.e == 1000);
}
static void keeps_password_access(void)
{
    setuid_root();
    assert(drop(1) == -1);
    assert(id.r == 1000 && id.e == 1000 && id.s == 0);
    assert(SmPrivRaise() == 1 && id.e == 0);
    assert(run(do_lower) == -1 && id.e == 1000);
    assert(SmPrivRaise() == 1);
    assert(run(do_lower) == -1);
}
static void refused_drop_stops(int keep)
{
    int code;
    setuid_root();
    fail_uid_ops = 1;                /* the user is at a process limit */
    code = drop(keep);
    assert(code != -1);              /* must not return as root */
    assert(strstr(message, "cannot give up root privileges"));
}
static void ignored_drop_stops(int keep)
{
    int code;
    setuid_root();
    ignore_changes = 1;              /* success is reported, nothing happens */
    code = drop(keep);
    assert(code != -1);
}
static void way_back_stops(void)
{
    int code;
    setuid_root();
    keep_saved_uid = 1;              /* the saved id stays root */
    code = drop(0);
    assert(code != -1);
    assert(strstr(message, "not given up"));
}
static void plain_install_does_nothing(void)
{
    start(1000, 100);
    fail_uid_ops = 1;
    assert(drop(0) == -1 && drop(1) == -1);
    assert(id_calls == 0);
    assert(id.r == 1000 && id.e == 1000);
    assert(!SmPrivRaise());
    start(0, 100); id.r = 0;         /* started by root */
    fail_uid_ops = 1;
    assert(drop(0) == -1 && drop(1) == -1);
    assert(id_calls == 0);
}
static void raise_failures(void)
{
    setuid_root();
    assert(drop(1) == -1);
    fail_uid_ops = 1;
    assert(SmPrivRaise() == 0 && id.e == 1000);
    fail_uid_ops = 0; ignore_changes = 1;
    assert(SmPrivRaise() == 0 && id.e == 1000);
    ignore_changes = 0;
    assert(SmPrivRaise() == 1 && id.e == 0);
    fail_uid_ops = 1;                /* the way back to the user fails */
    assert(run(do_lower) != -1);
    assert(strstr(message, "cannot give up root privileges"));
}
static void child_drops_everything(void)
{
    setuid_root();
    assert(drop(1) == -1);
    assert(id.s == 0);
    assert(run(do_child) == -1);
    assert(id.r == 1000 && id.e == 1000 && id.s == 1000);
    assert(id.gr == 100 && id.ge == 100 && id.gs == 100);
    assert(seteuid(0) != 0 && setuid(0) != 0);
}
static void child_drop_refused(int how)
{
    int code;
    setuid_root();
    assert(drop(1) == -1);
    if (how == 0) fail_uid_ops = 1;
    if (how == 1) fail_gid_ops = 1;
    if (how == 2) ignore_changes = 1;
    code = run(do_child);
    assert(code != -1);
}
static void group_install(void)
{
    start(1000, 5);                  /* setgid for a console group, not setuid */
    assert(drop(0) == -1);
    assert(id.ge == 100);            /* the user's group is effective */
#ifndef SVR4
    assert(id.gr == 5);              /* the installed group is the real one, to swap back */
#endif
    assert(SmPrivUseUserGroup(0) == 1 && id.ge == 5);
    assert(SmPrivUseUserGroup(1) == 1 && id.ge == 100);
    assert(run(do_child) == -1);
    assert(id.ge == 100);
#ifndef SVR4
    assert(id.gr == 100 && id.gs == 100);
#endif
    start(1000, 5);
    fail_gid_ops = 1;
    assert(drop(0) != -1);           /* cannot swap: stop instead of keeping the group */
    assert(strstr(message, "group of the installed program"));
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;
    drops_for_good();
    keeps_password_access();
    refused_drop_stops(0);
    refused_drop_stops(1);
    ignored_drop_stops(0);
    ignored_drop_stops(1);
    way_back_stops();
    plain_install_does_nothing();
    raise_failures();
    child_drops_everything();
    child_drop_refused(0);
    child_drop_refused(1);
    child_drop_refused(2);
    group_install();
    puts("ok");
    return 0;
}
'''

SCENARIOS = {
    "drops_for_good": "drop(0) gives up root for good",
    "keeps_password_access": "drop(1) keeps the saved id; raise and lower work",
    "refused_drop_stops": "a refused drop stops the program",
    "ignored_drop_stops": "an ignored drop stops the program",
    "way_back_stops": "a drop that leaves a way back stops the program",
    "plain_install_does_nothing": "no identity call without setuid",
    "raise_failures": "raise and lower failures",
    "child_drops_everything": "a child drops user, saved user and group",
    "child_drop_refused": "a child that cannot drop does not exec",
    "group_install": "setgid install: swap, use, drop",
}

cc = shlex.split(os.environ.get("CC", "gcc"))
failures = []
with tempfile.TemporaryDirectory(prefix="dtsession-priv-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    for variant in ([], ["-DSVR4"]):
        name = "svr4" if variant else "bsd"
        c = work / ("harness-%s.c" % name)
        c.write_text(harness)
        binary = work / ("harness-%s" % name)
        build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                      "-fno-omit-frame-pointer", "-Wno-unused-function", "-Wno-unused-variable",
                      "-I", str(source)] + variant + [str(c), "-o", str(binary)]
        compiled = subprocess.run(build, capture_output=True, text=True)
        if compiled.returncode:
            sys.stderr.write(compiled.stderr)
            sys.exit("cannot compile the %s variant" % name)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        if run.returncode or "ok" not in run.stdout:
            failures.append(name)
            sys.stderr.write("%s variant: %s\n%s" % (name, run.stdout.strip(), run.stderr))
        else:
            print("%s variant: all identity scenarios passed" % name)
if failures:
    sys.exit("identity handling failed in the %s variant(s)" % ", ".join(failures))

# The identity has to be given up before main() reads the environment: the
# behaviour needs a setuid binary and a privileged tracer (the procedure is in
# docs/improvements/session-manager.md), so this is a source guard.
main_c = (source / "SmMain.c").read_text()
match = re.search(r"^main \(int argc, char \*\*argv\)\n\{", main_c, re.M)
assert match, "main() not found"
body = main_c[match.end():]
body = body[:body.index("\n}\n")]
drop = body.find("SmPrivDropInstalledIdentity(")
if drop < 0:
    sys.exit("main() never gives up the installed identity")
early = [name for name in ("getenv(", "setlocale(", "XtSetLanguageProc(", "putenv(", "strdup(", "XtOpenDisplay(",
                           "XtToolkitInitialize(", "_DtEnvControl(", "InitNlsStrings(", "RestoreResources(")
         if 0 <= body.find(name) < drop]
if early:
    sys.exit("main() calls %s before it gives up the installed identity" % ", ".join(early))
print("main() gives up the installed identity before it touches the environment")
