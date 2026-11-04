#!/usr/bin/env python3
"""Check what dtsession does to the session directories when it saves.

Saving a session archives the directory that is saved ("current" or "home") to
its ".old" version, moves an older ".old" aside, and writes the new session.
If a write fails, the old session has to come back, and pruning removes the
oldest archive.  The production SetSavePath, UndoSetSavePath, MoveDirectory of
SmGlobals.c and PrintStartupInfo and PruneSessionDirectory of SmSave.c are cut
out of the sources and compiled, under AddressSanitizer and UndefinedBehavior
Sanitizer, with stand-ins for the toolkit, and run on real directories.  The
scenarios include a Home session that saves into "current" (the way a Home
session with a Current start state is logged out), the first save of a session
(nothing to archive: the user's sessionetc script is where the archive name
would be), a file system that cannot make the temporary directory, and a
session that cannot be moved aside.  Every scenario runs in its own process.

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
globals_c = (source / "SmGlobals.c").read_text()
save_c = (source / "SmSave.c").read_text()


def function(text, pattern):
    match = re.search(pattern, text, re.M)
    assert match, pattern
    return text[match.start():text.index("\n}", match.start()) + 3] + "\n"


strdefs = (source / "SmStrDefs.c").read_text()
names = "".join(re.findall(r"^char (?:SM_CLIENT_FILE2?|SM_RESOURCE_FILE|SM_SETTING_FILE|SM_HOME_DIRECTORY|SM_CURRENT_DIRECTORY|SM_OLD_EXTENSION)\[\] = \"[^\"]*\";\n", strdefs, re.M))

code = function(globals_c, r"^(?:void|int) *\nMoveDirectory\(")
code += function(globals_c, r"^void *\nUndoSetSavePath \(void\)")
code += function(globals_c, r"^int *\nSetSavePath\(")
code += function(save_c, r"^static void *\nPrintStartupInfo\(")
code += function(save_c, r"^static void *\nPruneSessionDirectory \(void\)")
fixed = "ArchiveSessionDirectory" in globals_c
helpers = function(globals_c, r"^static Boolean *\nSetArchivePath\( void \)") + \
    function(globals_c, r"^static char \*\nCreateSessionDirs\(")
prelude = "#define SM_PATH_MARGIN 512\n#define SM_FREE(p) free(p)\n#define XtCalloc(n, m) calloc(n, m)\nstatic Boolean SetArchivePath(void);\nstatic char *CreateSessionDirs(Display *);\n"
includes = '#include "%s"\n' % (source / "SmFiles.c")
if fixed:
    helpers += function(globals_c, r"^static void *\nRememberPath\(") + function(globals_c, r"^static int *\nArchiveSessionDirectory\( void \)")
    prelude += "static int ArchiveSessionDirectory(void);\nint MoveDirectory(char *, char *, Boolean);\n"
code = helpers + code

harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>

typedef int Boolean;
#undef True
#undef False
#define True 1
#define False 0
#define FALSE 0
#define DtError 1
#define GETMESSAGE(set, n, s) (s)
#define XtMalloc(n) malloc(n)
#define XtFree(p) free(p)
#define SM_EXIT(status) (exit_status = (status), longjmp(exit_jump, 1))
#define DtSM_CURRENT_STATE 1
#define DtSM_HOME_STATE 2
#define DEFAULT_SESSION 0
#define HOME_SESSION 1
#define CURRENT_SESSION 2

''' + names + r'''
static char savedDir[MAXPATHLEN], savedOldDir[MAXPATHLEN], savedTmpDir[MAXPATHLEN];
static struct {
    char clientPath[MAXPATHLEN + 1], settingPath[MAXPATHLEN + 1], resourcePath[MAXPATHLEN + 1], etcPath[MAXPATHLEN + 1];
    char *savePath, *restoreSession;
    short sessionType;
    Display *display;
} smGD;
static struct { int startState; } smSettings;
static struct { struct { Boolean shutdown; } saveState; } smXSMP;
static struct { int numSessionsBackedup; } smRes;
static struct { char *cantMallocErrorString, *cantCreateDirsString; } smNLS = { "malloc", "dirs" };

/* the toolkit and the session database */
static int errors, exit_status = -1, discards, fail_mkdtemp, write_fails;
static jmp_buf exit_jump;
static char discard_path[MAXPATHLEN * 2];
static int discard_found_marker;
static const char *fail_rename_from;
static char fake_save_path[MAXPATHLEN * 2];
static void PrintErrnoError(int type, const char *message) { (void) type; (void) message; errors++; }
static void PrintError(int type, const char *message) { (void) type; (void) message; errors++; }
/* the real one returns a MAXPATHLEN buffer */
static char *_DtCreateDtDirs(Display *d) { char *p = calloc(1, MAXPATHLEN); (void) d; snprintf(p, MAXPATHLEN, "%s", fake_save_path); return p; }
static void ExecuteDiscardCommands(char *database)
{
    char marker[MAXPATHLEN * 2], *slash;
    discards++;
    snprintf(discard_path, sizeof discard_path, "%s", database);
    snprintf(marker, sizeof marker, "%s", database);
    slash = strrchr(marker, '/');
    snprintf(slash, 8, "/marker");
    discard_found_marker = access(marker, F_OK) == 0;
}
typedef struct { int unused; } ClientDB_;
typedef ClientDB_ *ClientDB;
typedef struct { char *clientHost; char **command; int screenNum; } ProxyClientDBRec;
static int XGetWindowAttributes_(Display *d, Window w, XWindowAttributes *a) { (void) d; (void) w; (void) a; return 1; }
#define XGetWindowAttributes XGetWindowAttributes_
static int GetStandardProperties(Window w, int screen, int *wargc, char ***wargv, char **machine, Boolean *xsmp)
{
    (void) w; (void) screen;
    *wargv = calloc(2, sizeof(char *)); (*wargv)[0] = strdup("xclock");
    *wargc = 1; *machine = NULL; *xsmp = False;
    return 1;
}
static void XFreeStringList_(char **list) { if (list) { free(list[0]); free(list); } }
#define XFreeStringList XFreeStringList_
#define XFree(p) free(p)
static int DtIsLocalHostP(char *host) { (void) host; return 1; }
static Boolean PutProxyClientDBRec(ClientDB db, ProxyClientDBRec *rec) { (void) db; (void) rec; return write_fails ? False : True; }
static Boolean CloseClientDB(ClientDB db, Boolean b) { (void) db; (void) b; return True; }

static char *fake_mkdtemp(char *t) { if (fail_mkdtemp) { errno = ENOSPC; return NULL; } return mkdtemp(t); }
static int fake_mkstemp(char *t) { if (fail_mkdtemp) { errno = ENOSPC; return -1; } return mkstemp(t); }
static int fake_rename(const char *from, const char *to)
{
    if (fail_rename_from && !strcmp(from, fail_rename_from)) { errno = EXDEV; return -1; }
    return rename(from, to);
}
#define mkdtemp fake_mkdtemp
#define mkstemp fake_mkstemp
#define rename fake_rename

''' + prelude + includes + code + r'''
#undef mkdtemp
#undef mkstemp
#undef rename

/* --- helpers --- */
static char *work;
static char *under(const char *format, ...)
{
    static char path[8][MAXPATHLEN * 2];
    static int next;
    char *p = path[next++ % 8];
    char rel[MAXPATHLEN * 2];
    va_list args;
    va_start(args, format);
    vsnprintf(rel, sizeof(rel), format, args);
    va_end(args);
    snprintf(p, sizeof(path[0]), "%s/%s", work, rel);
    return p;
}
static void make_dirs(const char *path)
{
    char *copy = strdup(path), *p;
    for (p = copy + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(copy, 0755); *p = '/'; }
    mkdir(copy, 0755);
    free(copy);
}
static void put(const char *dir, const char *name, const char *content)
{
    char path[MAXPATHLEN * 2];
    FILE *f;
    make_dirs(dir);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "w"); assert(f); fputs(content, f); fclose(f);
}
static int has(const char *dir, const char *name, const char *content)
{
    char path[MAXPATHLEN * 2], buffer[64] = "";
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buffer, sizeof buffer, f)) buffer[0] = 0;
    fclose(f);
    return !strcmp(buffer, content);
}
static int exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }
static int count_entries(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) if (!strncmp(e->d_name, prefix, strlen(prefix))) n++;
    if (d) closedir(d);
    return n;
}
static void start_session(int type, int start_state, const char *restore)
{
    snprintf(fake_save_path, sizeof fake_save_path, "%s", under(".dt/sessions"));
    make_dirs(fake_save_path);
    smGD.sessionType = type;
    smSettings.startState = start_state;
    smGD.restoreSession = (char *) restore;
    snprintf(smGD.etcPath, sizeof smGD.etcPath, "%s/sessionetc", fake_save_path);
    smGD.savePath = NULL;
    smGD.display = NULL;
    errors = 0; exit_status = 0;
}
static void fail_a_write(void)
{
    ClientDB db = NULL;
    write_fails = 1;
    if (setjmp(exit_jump) == 0)
        PrintStartupInfo(db, 0, 1);
    write_fails = 0;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* A Home session whose start state is Current is logged out by saving into "current": a write error
   must bring "current" back from "current.old" and leave the Home session alone. */
static int write_error_in_a_home_session_saved_to_current(void)
{
    char *sessions = under(".dt/sessions");
    put(under(".dt/sessions/home"), "state", "home");
    put(under(".dt/sessions/current"), "state", "current");
    start_session(HOME_SESSION, DtSM_CURRENT_STATE, "home");
    CHECK(SetSavePath(False, DtSM_HOME_STATE) == 0);
    CHECK(has(under(".dt/sessions/current.old"), "state", "current"));    /* archived */
    CHECK(!exists(under(".dt/sessions/current/state")));                  /* and a new, empty one */
    put(under(".dt/sessions/current"), "partial", "half a save");
    fail_a_write();
    CHECK(exit_status == -1);                                             /* the save stops: SM_EXIT(-1) */
    CHECK(has(under(".dt/sessions/home"), "state", "home"));              /* the Home session is untouched */
    CHECK(has(under(".dt/sessions/current"), "state", "current"));        /* the old session is back */
    CHECK(!exists(under(".dt/sessions/current/partial")));
    CHECK(!exists(under(".dt/sessions/current.old")));
    (void) sessions;
    return 0;
}
/* The first save of a session archives nothing; the path of the user's sessionetc script must not be taken
   for an archive. */
static int write_error_in_a_first_save(void)
{
    put(under(".dt/sessions"), "sessionetc", "#!/bin/sh");
    start_session(CURRENT_SESSION, DtSM_CURRENT_STATE, "current");
    CHECK(SetSavePath(False, DtSM_CURRENT_STATE) == 0);
    CHECK(exists(under(".dt/sessions/current")));
    put(under(".dt/sessions/current"), "partial", "x");
    fail_a_write();
    CHECK(has(under(".dt/sessions"), "sessionetc", "#!/bin/sh"));          /* the script is still a script */
    CHECK(!exists(under(".dt/sessions/current/sessionetc")));
    CHECK(!exists(under(".dt/sessions/current/partial")));                 /* the partial save is gone */
    return 0;
}
/* ".old" exists already: it is moved aside, and comes back with the session. */
static int write_error_with_an_older_archive(void)
{
    put(under(".dt/sessions/current"), "state", "current");
    put(under(".dt/sessions/current.old"), "state", "older");
    start_session(CURRENT_SESSION, DtSM_CURRENT_STATE, "current");
    CHECK(SetSavePath(False, DtSM_CURRENT_STATE) == 0);
    CHECK(has(under(".dt/sessions/current.old"), "state", "current"));
    CHECK(count_entries(under(".dt/sessions"), "current.") == 2);          /* current.old and current.XXXXXX */
    fail_a_write();
    CHECK(has(under(".dt/sessions/current"), "state", "current"));
    CHECK(has(under(".dt/sessions/current.old"), "state", "older"));
    CHECK(count_entries(under(".dt/sessions"), "current.") == 1);          /* nothing left over */
    return 0;
}
static int cancelled_save(void)
{
    put(under(".dt/sessions/current"), "state", "current");
    put(under(".dt/sessions/current.old"), "state", "older");
    start_session(CURRENT_SESSION, DtSM_CURRENT_STATE, "current");
    CHECK(SetSavePath(False, DtSM_CURRENT_STATE) == 0);
    UndoSetSavePath();
    CHECK(has(under(".dt/sessions/current"), "state", "current"));
    CHECK(has(under(".dt/sessions/current.old"), "state", "older"));
    UndoSetSavePath();                                                      /* nothing is undone twice */
    CHECK(has(under(".dt/sessions/current"), "state", "current"));
    return 0;
}
/* A cancelled save of a session that had no ".old": the archive goes back. */
static int cancelled_save_without_an_older_archive(void)
{
    put(under(".dt/sessions/current"), "state", "current");
    start_session(CURRENT_SESSION, DtSM_CURRENT_STATE, "current");
    CHECK(SetSavePath(False, DtSM_CURRENT_STATE) == 0);
    UndoSetSavePath();
    CHECK(has(under(".dt/sessions/current"), "state", "current"));
    CHECK(!exists(under(".dt/sessions/current.old")));
    return 0;
}
/* The file system cannot make the name for the older archive: the older archive is dropped and the save goes on. */
static int no_temporary_name_for_the_older_archive(void)
{
    put(under(".dt/sessions/current"), "state", "current");
    put(under(".dt/sessions/current.old"), "state", "older");
    start_session(CURRENT_SESSION, DtSM_CURRENT_STATE, "current");
    fail_mkdtemp = 1;
    CHECK(SetSavePath(False, DtSM_CURRENT_STATE) == 0);
    fail_mkdtemp = 0;
    CHECK(has(under(".dt/sessions/current.old"), "state", "current"));
    CHECK(exists(under(".dt/sessions/current")) && !exists(under(".dt/sessions/current/state")));
    return 0;
}
/* The session cannot be archived: the save is refused and everything is where it was. */
static int archive_refused(void)
{
    put(under(".dt/sessions/current"), "state", "current");
    put(under(".dt/sessions/current.old"), "state", "older");
    start_session(CURRENT_SESSION, DtSM_CURRENT_STATE, "current");
    fail_rename_from = under(".dt/sessions/current");
    CHECK(SetSavePath(False, DtSM_CURRENT_STATE) == -1);
    fail_rename_from = NULL;
    CHECK(has(under(".dt/sessions/current"), "state", "current"));
    CHECK(has(under(".dt/sessions/current.old"), "state", "older"));
    CHECK(count_entries(under(".dt/sessions"), "current.") == 1);
    return 0;
}
static int session_path_too_long(void)
{
    start_session(CURRENT_SESSION, DtSM_CURRENT_STATE, "current");
    memset(fake_save_path, 'p', MAXPATHLEN - 3); fake_save_path[0] = '/'; fake_save_path[MAXPATHLEN - 3] = 0;
    CHECK(SetSavePath(False, DtSM_CURRENT_STATE) == -1);
    CHECK(smGD.savePath == NULL && smGD.clientPath[0] == 0 && errors == 1);
    return 0;
}

/* --- pruning --- */
static void age(const char *path, long seconds_ago)
{
    struct timeval t[2];
    gettimeofday(&t[0], NULL);
    t[0].tv_sec -= seconds_ago; t[1] = t[0];
    utimes(path, t);
}
static void prune_setup(void)
{
    put(under(".dt/sessions/home"), "state", "home");
    put(under(".dt/sessions/home.AAAAAA"), "state", "oldest"); put(under(".dt/sessions/home.AAAAAA"), "marker", "m");
    put(under(".dt/sessions/home.BBBBBB"), "state", "newer");
    age(under(".dt/sessions/home.AAAAAA"), 5000); age(under(".dt/sessions/home.BBBBBB"), 1000);
    start_session(HOME_SESSION, DtSM_HOME_STATE, "home");
    smGD.savePath = _DtCreateDtDirs(NULL);
    smRes.numSessionsBackedup = 1;
}
static int prune_the_oldest(void)
{
    prune_setup();
    PruneSessionDirectory();
    CHECK(!exists(under(".dt/sessions/home.AAAAAA")));                       /* the oldest is gone */
    CHECK(has(under(".dt/sessions/home.BBBBBB"), "state", "newer"));
    CHECK(has(under(".dt/sessions/home"), "state", "home"));                 /* the session is back */
    CHECK(discards == 1 && discard_found_marker);                              /* its discard commands ran with it in place */
    CHECK(count_entries(under(".dt/sessions"), "home.") == 1);
    return 0;
}
static int prune_when_the_session_cannot_be_moved_aside(void)
{
    prune_setup();
    fail_rename_from = under(".dt/sessions/home");
    PruneSessionDirectory();
    fail_rename_from = NULL;
    CHECK(has(under(".dt/sessions/home"), "state", "home"));                 /* the session is untouched */
    CHECK(discards == 0);                                                      /* its own discard commands did not run */
    CHECK(has(under(".dt/sessions/home.AAAAAA"), "state", "oldest"));
    return 0;
}
static int prune_without_a_temporary_name(void)
{
    prune_setup();
    fail_mkdtemp = 1;
    PruneSessionDirectory();
    fail_mkdtemp = 0;
    CHECK(has(under(".dt/sessions/home"), "state", "home") && discards == 0);
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"write_error_in_a_home_session_saved_to_current", write_error_in_a_home_session_saved_to_current},
    {"write_error_in_a_first_save", write_error_in_a_first_save},
    {"write_error_with_an_older_archive", write_error_with_an_older_archive},
    {"cancelled_save", cancelled_save},
    {"cancelled_save_without_an_older_archive", cancelled_save_without_an_older_archive},
    {"no_temporary_name_for_the_older_archive", no_temporary_name_for_the_older_archive},
    {"archive_refused", archive_refused},
    {"session_path_too_long", session_path_too_long},
    {"prune_the_oldest", prune_the_oldest},
    {"prune_when_the_session_cannot_be_moved_aside", prune_when_the_session_cannot_be_moved_aside},
    {"prune_without_a_temporary_name", prune_without_a_temporary_name},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    if (argc < 3) { for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    work = argv[2];
    if (chdir(work) != 0) return 3;
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
with tempfile.TemporaryDirectory(prefix="dtsession-save-", dir=os.environ.get("TMPDIR")) as scratch:
    scratch = Path(scratch)
    c = scratch / "save.c"
    c.write_text(harness)
    binary = scratch / "save"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-w", "-I", str(source), str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr[:8000])
        sys.exit("cannot compile the session save harness")
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    scenario_names = subprocess.run([str(binary)], capture_output=True, text=True, check=True, env=env).stdout.split()
    for name in scenario_names:
        work = scratch / ("work-" + name)
        work.mkdir()
        run = subprocess.run([str(binary), name, str(work)], capture_output=True, text=True, timeout=120, env=env)
        if run.returncode:
            failed.append(name)
            lines = [l for l in run.stderr.strip().splitlines() if l.strip()]
            sys.stderr.write("%s failed: %s\n" % (name, lines[0] if lines else "exit %d" % run.returncode))
            if os.environ.get("DTSESSION_VERBOSE"):
                sys.stderr.write("\n".join(lines[:25]) + "\n")
        subprocess.run(["chmod", "-R", "u+rwx", str(work)])
if failed:
    sys.exit("%d of %d session save scenarios failed: %s" % (len(failed), len(scenario_names), ", ".join(failed)))
print("Session save: %d scenarios passed" % len(scenario_names))
