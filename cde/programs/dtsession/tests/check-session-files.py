#!/usr/bin/env python3
"""Exercise how dtsession handles the files and directories of a session.

The production functions of SmGlobals.c that rotate the error log, move, copy
and remove session directories and set up the session paths are cut out of the
source and compiled, under AddressSanitizer and UndefinedBehaviorSanitizer,
with stand-ins for the toolkit.  They run on real files in a scratch directory,
with names that contain spaces and shell syntax, a home directory name that is
too long, a file system that cannot make hard links and a home directory that
cannot be created.  SmFiles.c, the library the new code uses for removing and
copying trees, is tested directly.  Every scenario runs in its own process.

DTSESSION_SOURCE names another copy of the sources to test, for example an
older revision that moved directories with mv, removed them with "rm -rf" in a
shell and rotated the error log with link().
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
text = (source / "SmGlobals.c").read_text()
fixed = (source / "SmFiles.c").exists()


def function(signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


strdefs = (source / "SmStrDefs.c").read_text()
names = "".join(re.findall(r"^char (?:SM_CLIENT_FILE2?|SM_RESOURCE_FILE|SM_SETTING_FILE|SM_HOME_DIRECTORY|SM_CURRENT_DIRECTORY|SM_OLD_EXTENSION)\[\] = \"[^\"]*\";\n", strdefs, re.M))
assert names.count("char ") == 7, names

code = function("static void\nTrimErrorlog( void )")
code += function("void \nUndoSetSavePath (void)")
code += function("static Boolean\nSetAlternateSession (")
code += function("static void\nInitializeGenericSession (")
if fixed:
    code += function("static char *\nCreateSessionDirs(")
    move = "int \nMoveDirectory(" if "int \nMoveDirectory(" in text else "void \nMoveDirectory("
    code += function(move)
    includes = '#include "%s"\n' % (source / "SmFiles.c")
    declarations = "#define SM_PATH_MARGIN 512\n" + move.split()[0] + " MoveDirectory(char *, char *, Boolean);\nstatic char *CreateSessionDirs(Display *);\n"
else:
    includes = ""
    declarations = "void MoveDirectory(char *from, char *to, Boolean remove) { (void) from; (void) to; (void) remove; }\n"

harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int Boolean;
typedef void Display;
#define True 1
#define False 0
#define DtError 1
#define DtPERSONAL_CONFIG_DIRECTORY ".dt"
#define DtSM_SESSION_DIRECTORY "sessions"
#define DtERRORLOG_FILE "errorlog"
#define DtOLD_ERRORLOG_FILE "errorlog.old"
#define DtOLDER_ERRORLOG_FILE "errorlog.older"
#define SM_MALLOC(n) malloc(n)
#define SM_REALLOC(p, n) realloc(p, n)
#define SM_FREE(p) free(p)
#define GETMESSAGE(set, n, s) (s)
#define XtMalloc(n) malloc(n)
#define XtFree(p) free(p)

''' + names + r'''
static char savedDir[MAXPATHLEN], savedOldDir[MAXPATHLEN], savedTmpDir[MAXPATHLEN];
static struct {
    char clientPath[MAXPATHLEN + 1], settingPath[MAXPATHLEN + 1], resourcePath[MAXPATHLEN + 1];
    char etcPath[MAXPATHLEN + 1];
    char *savePath, *restoreSession;
    short sessionType;
} smGD;
static struct { char *cantMallocErrorString, *cantCreateDirsString; } smNLS = { "malloc", "dirs" };
#define DEFAULT_SESSION 0
#define HOME_SESSION 1
#define CURRENT_SESSION 2

static int errors, defaults_set, fake_link_fails;
static char fake_save_path[MAXPATHLEN * 2];
static int have_save_path;
static void PrintError(int type, const char *message) { (void) type; (void) message; errors++; }
static void PrintErrnoError(int type, const char *message) { (void) type; (void) message; errors++; }
static int SetSysDefaults(void) { defaults_set++; return 0; }
static int SetResSet(void) { return 0; }
static char *_DtCreateDtDirs(Display *d) { (void) d; return have_save_path ? strdup(fake_save_path) : NULL; }
static void SystemCmd(char *command) { if (system(command)) { } }
static int fake_link(const char *from, const char *to)
{
    if (fake_link_fails) { errno = EPERM; return -1; }
    return link(from, to);
}
#define link fake_link

''' + declarations + includes + code + r'''
#undef link

/* helpers for the scenarios */
static void make_dirs(const char *path)
{
    char *copy = strdup(path), *p;
    for (p = copy + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(copy, 0755); *p = '/'; }
    mkdir(copy, 0755);
    free(copy);
}
static void put(const char *dir, const char *name, const char *content)
{
    char path[MAXPATHLEN * 2];
    FILE *f;
    make_dirs(dir);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}
static int get(const char *dir, const char *name, char *buffer, size_t size)
{
    char path[MAXPATHLEN * 2];
    FILE *f;
    size_t n;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "r");
    if (!f) return 0;
    n = fread(buffer, 1, size - 1, f);
    buffer[n] = 0;
    fclose(f);
    return 1;
}
static int exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }
static int count_dir(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    if (d) closedir(d);
    return n;
}
static char *work;
static char *under(const char *format, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
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

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* --- the error log --- */
static int trim_rotates(void)
{
    char b[64], *dot = under("h1/.dt");
    put(dot, "errorlog", "A"); put(dot, "errorlog.old", "B"); put(dot, "errorlog.older", "C");
    setenv("HOME", under("h1"), 1);
    TrimErrorlog();
    CHECK(get(dot, "errorlog.older", b, sizeof b) && !strcmp(b, "B"));
    CHECK(get(dot, "errorlog.old", b, sizeof b) && !strcmp(b, "A"));
    CHECK(!exists(under("h1/.dt/errorlog")));
    return 0;
}
static int trim_replaces_empty_old(void)
{
    char b[64], *dot = under("h2/.dt");
    put(dot, "errorlog", "A"); put(dot, "errorlog.old", "");
    setenv("HOME", under("h2"), 1);
    TrimErrorlog();
    CHECK(get(dot, "errorlog.old", b, sizeof b) && !strcmp(b, "A"));
    return 0;
}
static int trim_without_hard_links(void)
{
    char b[64], *dot = under("h3/.dt");
    put(dot, "errorlog", "A"); put(dot, "errorlog.old", "B");
    setenv("HOME", under("h3"), 1);
    fake_link_fails = 1;
    TrimErrorlog();
    CHECK(get(dot, "errorlog.older", b, sizeof b) && !strcmp(b, "B"));
    CHECK(get(dot, "errorlog.old", b, sizeof b) && !strcmp(b, "A"));
    return 0;
}
static int trim_long_home_rotates(void)
{
    /* a home directory of 4,070 characters, made of real directories, with the logs in it */
    char b[64], home[PATH_MAX * 2], dot[PATH_MAX * 2];
    size_t length, add;
    snprintf(home, sizeof home, "%s", work);
    while ((length = strlen(home)) < 4060) {
        add = 4070 - length - 1 > 200 ? 200 : 4070 - length - 1;
        home[length] = '/';
        memset(home + length + 1, 'h', add);
        home[length + 1 + add] = 0;
        CHECK(mkdir(home, 0755) == 0);
    }
    CHECK(strlen(home) > 4000);
    snprintf(dot, sizeof dot, "%s/.dt", home);
    put(dot, "errorlog", "A"); put(dot, "errorlog.old", "B"); put(dot, "errorlog.older", "C");
    setenv("HOME", home, 1);
    TrimErrorlog();
    CHECK(get(dot, "errorlog.older", b, sizeof b) && !strcmp(b, "B"));
    CHECK(get(dot, "errorlog.old", b, sizeof b) && !strcmp(b, "A"));
    return 0;
}
static int trim_home_longer_than_a_path(void)
{
    /* no such directory can exist: the logs of another directory are left alone and nothing is made */
    static char home[8200];
    char b[64], *dot = under("elsewhere/.dt");
    int length;
    put(dot, "errorlog", "A");
    for (length = 4040; length < 4110; length += 7) {
        memset(home, 'a', length);
        home[0] = '/';
        home[length] = 0;
        setenv("HOME", home, 1);
        TrimErrorlog();
    }
    memset(home, 'a', 8100); home[0] = '/'; home[8100] = 0;
    setenv("HOME", home, 1);
    TrimErrorlog();
    CHECK(get(dot, "errorlog", b, sizeof b) && !strcmp(b, "A"));
    CHECK(count_dir(work) == 1);
    return 0;
}

/* --- directories named with spaces and shell syntax --- */
static int undo_removes_the_named_path(void)
{
    char *decoy = under("save"), *dir = under("save dir; touch pwned");
    put(decoy, "keep", "x");
    put(dir, "session", "y");
    snprintf(savedDir, sizeof savedDir, "%s", dir);
    savedOldDir[0] = savedTmpDir[0] = 0;
    UndoSetSavePath();
    CHECK(!exists(dir));
    CHECK(exists(under("save/keep")));
    CHECK(!exists(under("pwned")));
    return 0;
}
static int alternate_session_copied(void)
{
    char b[64], *alt = under("home dir; touch pwned2"), *session = under("cur dir");
    put(alt, "dtsession.db", "database"); put(under("home dir; touch pwned2/sub"), "state", "s");
    CHECK(SetAlternateSession(session, alt, True) == True);
    CHECK(get(session, "dtsession.db", b, sizeof b) && !strcmp(b, "database"));
    CHECK(get(under("cur dir/sub"), "state", b, sizeof b) && !strcmp(b, "s"));
    CHECK(!strcmp(smGD.clientPath, under("cur dir/dtsession.db")));
    CHECK(!exists(under("pwned2")));
    return 0;
}
static int alternate_session_without_database(void)
{
    char *alt = under("empty alt"), *session = under("new session");
    make_dirs(alt);
    CHECK(SetAlternateSession(session, alt, False) == False);
    CHECK(!exists(session));
    CHECK(SetAlternateSession(session, alt, True) == True);
    CHECK(exists(session) && defaults_set == 1);
    return 0;
}

/* --- the session directory --- */
static int generic_session_normal(void)
{
    char *save = under("g1/.dt/sessions");
    put(under("g1/.dt/sessions/current"), "dtsession.db", "d"); put(under("g1/.dt/sessions/current"), "dt.resources", "r");
    snprintf(fake_save_path, sizeof fake_save_path, "%s", save); have_save_path = 1;
    InitializeGenericSession(NULL);
    CHECK(!strcmp(smGD.clientPath, under("g1/.dt/sessions/current/dtsession.db")));
    CHECK(smGD.sessionType == CURRENT_SESSION);
    return 0;
}
static int generic_session_without_directories(void)
{
    have_save_path = 0;
    InitializeGenericSession(NULL);
    CHECK(errors == 1 && defaults_set == 1 && smGD.savePath == NULL);
    return 0;
}
static int generic_session_long_directory(void)
{
    memset(fake_save_path, 'p', MAXPATHLEN - 3); fake_save_path[0] = '/'; fake_save_path[MAXPATHLEN - 3] = 0;
    have_save_path = 1;
    InitializeGenericSession(NULL);
    /* the directory is refused: no paths are built, the system defaults are used and the user is told */
    CHECK(smGD.savePath == NULL);
    CHECK(smGD.clientPath[0] == 0 && smGD.resourcePath[0] == 0 && smGD.settingPath[0] == 0);
    CHECK(defaults_set == 1 && errors == 1);
    return 0;
}

#ifdef FIXED
/* --- moving and removing directories --- */
static int move_without_a_search_path(void)
{
    char b[64], *from = under("mv from dir"), *to = under("mv to dir");
    put(from, "f", "moved");
    setenv("PATH", "", 1);
    MoveDirectory(from, to, False);
    CHECK(get(to, "f", b, sizeof b) && !strcmp(b, "moved"));
    CHECK(!exists(from));
    put(under("mv again"), "f", "new"); put(to, "stale", "x");
    MoveDirectory(under("mv again"), to, True);
    CHECK(get(to, "f", b, sizeof b) && !strcmp(b, "new") && !exists(under("mv to dir/stale")));
    CHECK(errors == 0);
    MoveDirectory(under("no such"), under("target"), False);
    CHECK(errors == 1);
    return 0;
}
static int join_path(void)
{
    char *p = SmJoinPath("a", "b", "c", NULL);
    CHECK(!strcmp(p, "a/b/c")); free(p);
    p = SmJoinPath("", ".dt", "errorlog", NULL);
    CHECK(!strcmp(p, "/.dt/errorlog")); free(p);
    p = SmJoinPath("only", NULL);
    CHECK(!strcmp(p, "only")); free(p);
    return 0;
}
static int remove_tree(void)
{
    char *top = under("rt top; x"), *outside = under("rt outside"), link_path[MAXPATHLEN * 2];
    put(top, "a", "1"); put(under("rt top; x/d1/d2"), "b", "2");
    put(outside, "keep", "k");
    snprintf(link_path, sizeof link_path, "%s/link", top);
    CHECK(symlink(outside, link_path) == 0);
    CHECK(SmRemoveTree(top) == 0);
    CHECK(!exists(top));
    CHECK(exists(under("rt outside/keep")));                 /* the link was not followed */
    CHECK(SmRemoveTree(under("rt missing")) == 0);
    CHECK(SmRemoveTree("") == -1 && SmRemoveTree("/") == -1 && SmRemoveTree(NULL) == -1);
    put(under("rt file"), "x", "x");
    CHECK(SmRemoveTree(under("rt file/x")) == 0 && !exists(under("rt file/x")));
    CHECK(SmRemoveTree(link_path) == 0);                     /* a link itself: nothing happens, it is gone */
    return 0;
}
static int remove_tree_failures(void)
{
    char *top = under("rf top"), *inner = under("rf top/locked");
    put(inner, "x", "1");
    chmod(inner, 0500);                                       /* cannot remove x */
    if (geteuid() != 0) {
        CHECK(SmRemoveTree(top) == -1);
        CHECK(exists(under("rf top/locked/x")));
    }
    chmod(inner, 0700);
    CHECK(SmRemoveTree(top) == 0 && !exists(top));
    return 0;
}
static int remove_tree_deep(void)
{
    char path[MAXPATHLEN * 2];
    int i, fd = open(work, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(chdir(work) == 0);
    for (i = 0; i < 150; i++) { CHECK(mkdir("d", 0755) == 0); CHECK(chdir("d") == 0); }
    CHECK(fchdir(fd) == 0);
    snprintf(path, sizeof path, "%s/d", work);
    CHECK(SmRemoveTree(path) == -1);                          /* deeper than it walks: refused, no crash */
    return 0;
}
static int remove_tree_carries_on(void)
{
    /* one directory that cannot be emptied does not keep the rest from being removed */
    char *top = under("co top"), name[64], inner[PATH_MAX * 2];
    int i, failed;
    if (geteuid() == 0) return 0;
    for (i = 0; i < 30; i++) { snprintf(name, sizeof name, "file%02d", i); put(top, name, "x"); }
    for (i = 0; i < 5; i++) {
        snprintf(inner, sizeof inner, "%s/locked%d", top, i);
        put(inner, "x", "1");
        chmod(inner, 0500);
    }
    errno = 0;
    CHECK(SmRemoveTree(top) == -1 && errno == EACCES);              /* the first error is the one reported */
    for (i = 0; i < 30; i++) { snprintf(name, sizeof name, "co top/file%02d", i); CHECK(!exists(under("%s", name))); }
    for (i = 0; i < 5; i++) { snprintf(inner, sizeof inner, "%s/locked%d", top, i); chmod(inner, 0700); }
    CHECK(SmRemoveTree(top) == 0 && !exists(top));
    return 0;
}
static int alternate_session_copy_fails(void)
{
    char *alt = under("alt home"), *session = under("half session");
    if (geteuid() == 0) return 0;
    put(alt, "dtsession.db", "database"); put(alt, "state", "s");
    chmod(under("alt home/state"), 0000);                            /* cannot be read: the copy stops */
    CHECK(SetAlternateSession(session, alt, True) == False);
    CHECK(!exists(session));                                         /* no half-copied session is left to restore */
    chmod(under("alt home/state"), 0600);
    return 0;
}
#ifdef HAS_TEMPDIR
static int temporary_directories(void)
{
    char *a = SmMakeTempDirectory(work, "current"), *b = SmMakeTempDirectory(work, "current"), *none;
    struct stat st;
    CHECK(a && b && strcmp(a, b));
    CHECK(strlen(a) == strlen(work) + 1 + strlen("current.XXXXXX") && !strncmp(a, work, strlen(work)));
    CHECK(stat(a, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
    none = SmMakeTempDirectory(under("no such directory"), "x");
    CHECK(none == NULL && errno == ENOENT);
    free(a); free(b);
    return 0;
}
#endif
static int copy_tree(void)
{
    char b[16], *from = under("ct from; x"), *to = under("ct to dir"), *big = malloc(100000);
    memset(big, 'z', 99999); big[99999] = 0;
    put(from, "small", "s"); put(from, "big", big); put(under("ct from; x/sub dir"), "n", "n");
    CHECK(symlink("small", under("ct from; x/link")) == 0);
    chmod(under("ct from; x/small"), 0640);
    CHECK(SmCopyTree(from, to) == 0);
    CHECK(get(to, "small", b, sizeof b) && !strcmp(b, "s"));
    {
        char *copy = malloc(100000);
        CHECK(get(to, "big", copy, 100000) && !strcmp(copy, big));
        free(copy);
    }
    CHECK(get(under("ct to dir/sub dir"), "n", b, sizeof b) && !strcmp(b, "n"));
    {
        char target[64]; ssize_t n = readlink(under("ct to dir/link"), target, sizeof target - 1);
        CHECK(n > 0 && (target[n] = 0, !strcmp(target, "small")));
    }
    {
        struct stat st; CHECK(stat(under("ct to dir/small"), &st) == 0 && (st.st_mode & 0777) == 0640);
    }
    CHECK(SmCopyTree(from, to) == -1);                         /* the target exists */
    CHECK(SmCopyTree(under("ct none"), under("ct x")) == -1);
    CHECK(SmCopyTree("", to) == -1);
    free(big);
    return 0;
}
#endif

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"trim_rotates", trim_rotates}, {"trim_replaces_empty_old", trim_replaces_empty_old},
    {"trim_without_hard_links", trim_without_hard_links}, {"trim_long_home_rotates", trim_long_home_rotates},
    {"trim_home_longer_than_a_path", trim_home_longer_than_a_path},
    {"undo_removes_the_named_path", undo_removes_the_named_path},
    {"alternate_session_copied", alternate_session_copied},
    {"alternate_session_without_database", alternate_session_without_database},
    {"generic_session_normal", generic_session_normal},
    {"generic_session_without_directories", generic_session_without_directories},
    {"generic_session_long_directory", generic_session_long_directory},
#ifdef FIXED
    {"move_without_a_search_path", move_without_a_search_path}, {"join_path", join_path},
    {"remove_tree", remove_tree}, {"remove_tree_failures", remove_tree_failures},
    {"remove_tree_deep", remove_tree_deep}, {"copy_tree", copy_tree}, {"remove_tree_carries_on", remove_tree_carries_on},
    {"alternate_session_copy_fails", alternate_session_copy_fails},
#ifdef HAS_TEMPDIR
    {"temporary_directories", temporary_directories},
#endif
#endif
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
with tempfile.TemporaryDirectory(prefix="dtsession-files-", dir=os.environ.get("TMPDIR")) as scratch:
    scratch = Path(scratch)
    c = scratch / "files.c"
    c.write_text(harness)
    binary = scratch / "files"
    define = (["-DFIXED"] if fixed else []) + (["-DHAS_TEMPDIR"] if fixed and "SmMakeTempDirectory" in (source / "SmFiles.c").read_text() else [])
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-Wno-unused-function", "-Wno-unused-variable",
                  "-Wno-unused-parameter", "-I", str(source)] + define + [str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr)
        sys.exit("cannot compile the session file harness")
    scenario_names = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
    for name in scenario_names:
        work = scratch / ("work-" + name)
        work.mkdir()
        run = subprocess.run([str(binary), name, str(work)], capture_output=True, text=True, timeout=120)
        if run.returncode:
            failed.append(name)
            lines = run.stderr.strip().splitlines()
            sys.stderr.write("%s failed: %s\n" % (name, lines[0] if lines else "exit %d" % run.returncode))
            if os.environ.get("DTSESSION_VERBOSE"):
                sys.stderr.write("\n".join(lines[:25]) + "\n")
        subprocess.run(["chmod", "-R", "u+rwx", str(work)])
if failed:
    sys.exit("%d of %d session file scenarios failed: %s" % (len(failed), len(scenario_names), ", ".join(failed)))
print("Session files: %d scenarios passed" % len(scenario_names))
