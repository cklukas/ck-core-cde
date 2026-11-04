#!/usr/bin/env python3
"""Check how dtsession writes the ICE authority file.

dtsession gives the XSMP clients of the session a cookie by merging it into the
user's ~/.ICEauthority under a lock.  The production writeIceauth() and
SetAuthentication() of SmAuth.c are cut out of the source and compiled, under
AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer, with stand-ins
for libICE that can refuse the lock.  When the lock cannot be taken the lock
files belong to someone else and must be left alone, the cookies must not be
written to a second, system-wide file, and nothing may leak; when it is taken,
the file is written for the user only and the lock is released exactly once.
Every scenario runs in its own process.

DTSESSION_SOURCE names another copy of the sources to test, for example an
older revision.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
text = (source / "SmAuth.c").read_text()


def function(pattern):
    matches = list(re.finditer(pattern, text, re.M))   # the last one is the definition, the others prototypes
    assert matches, pattern
    match = matches[-1]
    return text[match.start():text.index("\n}", match.start()) + 3] + "\n"


code = re.search(r"typedef struct _IceAuthFileEntryList\n\{.*?\} IceAuthFileEntryList;", text, re.S).group(0) + "\n"
for pattern in (r"^static void\nfreeEntryList \(", r"^static int\nfileEntryInDataEntries \(", r"^static int\naddToEntryList \(",
                r"^static int\nwriteIceauth \(", r"^Status\nSetAuthentication \("):
    code += function(pattern)

harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int Status;
typedef void *IceListenObj;
typedef struct { char *protocol_name; char *network_id; char *auth_name; unsigned short auth_data_length; char *auth_data; } IceAuthDataEntry;
typedef struct { char *protocol_name; unsigned short protocol_data_length; char *protocol_data; char *network_id; char *auth_name; unsigned short auth_data_length; char *auth_data; } IceAuthFileEntry;
enum { IceAuthLockError, IceAuthLockTimeout, IceAuthLockSuccess };
#define CDE_CONFIGURATION_TOP "/etc/dt"
#define MAGIC_COOKIE_LEN 16
#define AUTH_RETRIES 10
#define AUTH_TIMEOUT 2
#define AUTH_DEADTIME 600L
#define XtMalloc(n) malloc(n)
#define XtCalloc(n, m) calloc(n, m)
#define XtFree(p) free(p)

static char *auth_path;
static int lock_status = IceAuthLockSuccess, lock_calls, unlock_calls, written_entries, cookie_fails;
static char locked_paths[4][MAXPATHLEN], unlocked_paths[4][MAXPATHLEN];
static char *IceAuthFileName(void) { return auth_path; }
static int IceLockAuthFile(const char *path, int retries, int timeout, long dead)
{
    (void) retries; (void) timeout; (void) dead;
    if (lock_calls < 4) snprintf(locked_paths[lock_calls], MAXPATHLEN, "%s", path);
    lock_calls++;
    return lock_status;
}
static void IceUnlockAuthFile(const char *path) { if (unlock_calls < 4) snprintf(unlocked_paths[unlock_calls], MAXPATHLEN, "%s", path); unlock_calls++; }
static IceAuthFileEntry *IceReadAuthFileEntry(FILE *fp) { (void) fp; return NULL; }
static void IceFreeAuthFileEntry(IceAuthFileEntry *e) { (void) e; }
static int IceWriteAuthFileEntry(FILE *fp, IceAuthFileEntry *e) { (void) e; fputs("entry\n", fp); written_entries++; return 1; }
static char *IceGetListenConnectionString(IceListenObj o) { (void) o; return strdup("local/box:/tmp/.ICE-unix/1"); }
static char *IceGenerateMagicCookie(int length)
{
    char *c;
    if (cookie_fails) return NULL;
    c = malloc(length); memset(c, 7, length); return c;
}
static void IceSetHostBasedAuthProc(IceListenObj o, int (*proc)(char *)) { (void) o; (void) proc; }
static void IceSetPaAuthData(int n, IceAuthDataEntry *e) { (void) n; (void) e; }
static int HostBasedAuthProc(char *host) { (void) host; return 0; }

''' + code + r'''
static char *work;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static IceAuthDataEntry entry(void)
{
    IceAuthDataEntry e = { "ICE", "local/box:/tmp/.ICE-unix/1", "MIT-MAGIC-COOKIE-1", 16, "0123456789abcdef" };
    return e;
}
static void use_file(const char *name)
{
    static char path[MAXPATHLEN];
    snprintf(path, sizeof path, "%s/%s", work, name);
    auth_path = path;
}

static int lock_held_by_another_process(void)
{
    IceAuthDataEntry e = entry();
    int status;
    use_file(".ICEauthority");
    for (status = IceAuthLockError; status <= IceAuthLockTimeout; status++) {
        lock_status = status; lock_calls = unlock_calls = 0;
        CHECK(writeIceauth(1, &e, 0) == 0);
        CHECK(unlock_calls == 0);                                 /* the other process's lock files are not removed */
        CHECK(lock_calls == 1 && !strcmp(locked_paths[0], auth_path));   /* and no other file is tried */
        CHECK(access(auth_path, F_OK) != 0);                      /* nothing was written */
    }
    return 0;
}
static int cookies_are_written_for_the_user_only(void)
{
    IceAuthDataEntry e = entry();
    struct stat st;
    use_file(".ICEauthority");
    umask(0);
    CHECK(writeIceauth(1, &e, 0) == 1);
    CHECK(lock_calls == 1 && unlock_calls == 1 && !strcmp(unlocked_paths[0], auth_path));
    CHECK(written_entries == 1);
    CHECK(stat(auth_path, &st) == 0 && (st.st_mode & 077) == 0);
    return 0;
}
static int unwritable_file_releases_the_lock(void)
{
    IceAuthDataEntry e = entry();
    use_file("no/such/directory/.ICEauthority");
    CHECK(writeIceauth(1, &e, 0) == 0);
    CHECK(lock_calls == 1 && unlock_calls == 1 && !strcmp(unlocked_paths[0], auth_path));
    return 0;
}
static int restore_at_exit(void)
{
    IceAuthDataEntry e = entry();
    use_file(".ICEauthority");
    CHECK(writeIceauth(1, &e, 1) == 1 && written_entries == 0);
    return 0;
}
static int cookie_cannot_be_made(void)
{
    IceListenObj listen[1] = { NULL };
    IceAuthDataEntry *entries = NULL;
    int i;
    use_file(".ICEauthority");
    cookie_fails = 1;
    CHECK(SetAuthentication(1, listen, &entries) == 0);
    for (i = 0; i < 2; i++) { free(entries[i].network_id); free(entries[i].auth_data); }   /* what FreeAuthenticationData does */
    free(entries);
    CHECK(lock_calls == 0);
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"lock_held_by_another_process", lock_held_by_another_process},
    {"cookies_are_written_for_the_user_only", cookies_are_written_for_the_user_only},
    {"unwritable_file_releases_the_lock", unwritable_file_releases_the_lock},
    {"restore_at_exit", restore_at_exit},
    {"cookie_cannot_be_made", cookie_cannot_be_made},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    if (argc < 3) { for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    work = argv[2];
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
with tempfile.TemporaryDirectory(prefix="dtsession-ice-", dir=os.environ.get("TMPDIR")) as scratch:
    scratch = Path(scratch)
    c = scratch / "ice.c"
    c.write_text(harness)
    binary = scratch / "ice"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-w", str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr[:6000])
        sys.exit("cannot compile the ICE authority harness")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
    for name in names:
        work = scratch / ("work-" + name)
        work.mkdir()
        run = subprocess.run([str(binary), name, str(work)], capture_output=True, text=True, timeout=60)
        if run.returncode:
            failed.append(name)
            lines = [l for l in run.stderr.strip().splitlines() if l.strip()]
            sys.stderr.write("%s failed: %s\n" % (name, lines[0] if lines else "exit %d" % run.returncode))
if failed:
    sys.exit("%d of %d ICE authority scenarios failed: %s" % (len(failed), len(names), ", ".join(failed)))
print("ICE authority: %d scenarios passed" % len(names))
