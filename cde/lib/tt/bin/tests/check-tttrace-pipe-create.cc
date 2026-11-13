/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
using namespace std;

/* The part of _Tt_string the production code uses. */
struct _Tt_string {
    string s;
    _Tt_string() {}
    _Tt_string &operator=(const char *p) { s = p ? p : ""; return *this; }
    int operator==(const char *p) const { return s == p; }
    int len() const { return (int)s.size(); }
    operator char *() const { return (char *)s.c_str(); }
};

/*
 * mkstemp and mkdtemp stand-ins.  They create the file or directory the way
 * libc does (O_CREAT|O_EXCL, mode 0600, or mkdir with mode 0700; a symbolic
 * link at the name is never followed) but walk the names AAAAAA, AAAAAB, ...
 * instead of random ones, so that a test can plant a link at the first name.
 * With ck_mk_fail set they fail as libc does when the directory cannot be
 * written.
 */
static int ck_mk_fail;
static int ck_mk_calls;
static int ck_mk_unwritable;	/* mkstemp leaves a file nobody can open */

static int ck_mkstemp(char *t)
{
    size_t n = strlen(t);
    char *x;
    int i, fd;

    ++ck_mk_calls;
    if (ck_mk_fail) {
        errno = EACCES;
        return -1;
    }
    assert(n >= 6 && strcmp(t + n - 6, "XXXXXX") == 0);
    x = t + n - 6;
    for (i = 0; i < 26; i++) {
        memcpy(x, "AAAAA", 5);
        x[5] = (char)('A' + i);
        fd = open(t, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            if (ck_mk_unwritable)
                fchmod(fd, 0);
            return fd;
        }
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

static char *ck_mkdtemp(char *t)
{
    size_t n = strlen(t);
    char *x;
    int i;

    ++ck_mk_calls;
    if (ck_mk_fail) {
        errno = EACCES;
        return NULL;
    }
    assert(n >= 6 && strcmp(t + n - 6, "XXXXXX") == 0);
    x = t + n - 6;
    for (i = 0; i < 26; i++) {
        memcpy(x, "AAAAA", 5);
        x[5] = (char)('A' + i);
        if (mkdir(t, 0700) == 0)
            return t;
        if (errno != EEXIST)
            return NULL;
    }
    errno = EEXIST;
    return NULL;
}

/*
 * What the unfixed code used: tempnam() and mktemp() return a predictable
 * name and create nothing.  The stand-ins return <dir>/<prefix>AAAAAA, where
 * an attacker who can predict the name plants a link.
 */
static char *ck_tempnam(const char *dir, const char *prefix)
{
    char path[PATH_MAX];
    if (dir == NULL)
        dir = getenv("TMPDIR");
    if (dir == NULL)
        dir = "/tmp";
    snprintf(path, sizeof path, "%s/%sAAAAAA", dir, prefix);
    return strdup(path);
}

static char *ck_mktemp(char *t)
{
    size_t n = strlen(t);
    assert(n >= 6 && strcmp(t + n - 6, "XXXXXX") == 0);
    memcpy(t + n - 6, "AAAAAA", 6);
    return t;
}

/* mkfifo stand-in: fails when ck_fifo_fail is set. */
static int ck_fifo_fail;
static int ck_mkfifo(const char *path, mode_t mode)
{
    if (ck_fifo_fail) {
        errno = EACCES;
        return -1;
    }
    return mkfifo(path, mode);
}

/* exit() stand-in: with ck_trap set it throws, so the test sees the status. */
static int ck_trap;
struct CkExit { int code; };
[[noreturn]] static void ck_exit(int code)
{
    if (ck_trap)
        throw CkExit{code};
    (exit)(code);
    abort();
}

#define mkdtemp ck_mkdtemp
#define tempnam ck_tempnam
#define mkfifo ck_mkfifo
#define exit(c) ck_exit(c)
/* @@BODY@@ */

/* ---- helpers of the test ---- */
static char ck_work[PATH_MAX];		/* the test's own directory */

static int ck_rm(const char *p, const struct stat *sb, int flag, struct FTW *w)
{
    (void)sb; (void)flag; (void)w;
    return remove(p);
}

static void ck_make_work(const char *scratch)
{
    snprintf(ck_work, sizeof ck_work, "%s/w-XXXXXX", scratch);
    assert(mkdtemp(ck_work));
}

static void ck_remove_work(void)
{
    nftw(ck_work, ck_rm, 16, FTW_DEPTH | FTW_PHYS);
}

static void ck_write_file(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
    assert(chmod(path, mode) == 0);
}

static char *ck_slurp(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n;
    assert(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int ck_count(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    assert(d);
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            n++;
    closedir(d);
    return n;
}

static void ck_gone(const char *path)
{
    struct stat st;
    assert(lstat(path, &st) != 0 && errno == ENOENT);
}

/* A plain file of ours, mode 0600, one link: what mkstemp makes. */
static int ck_private_file(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           (st.st_mode & 07777) == 0600 && st.st_uid == geteuid() &&
           st.st_nlink == 1;
}

static int ck_private_dir(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           (st.st_mode & 07777) == 0700 && st.st_uid == geteuid();
}

/*
 * A link planted at the first name (<dir>/<leaf>) the unfixed code uses and
 * the fixed code tries first, pointing to a victim file.
 */
#define VICTIM_TEXT "VICTIM-DATA\n"
static char ck_victim[PATH_MAX];

static void ck_plant(const char *dir, const char *leaf)
{
    char link[PATH_MAX];
    snprintf(ck_victim, sizeof ck_victim, "%s/victim", ck_work);
    ck_write_file(ck_victim, VICTIM_TEXT, 0644);
    snprintf(link, sizeof link, "%s/%s", dir, leaf);
    assert(symlink(ck_victim, link) == 0);
}

/* The planted target must be neither written, truncated nor chmod'ed. */
static void ck_victim_intact(void)
{
    struct stat st;
    assert(stat(ck_victim, &st) == 0);
    assert((st.st_mode & 07777) == 0644);
    assert(strcmp(ck_slurp(ck_victim), VICTIM_TEXT) == 0);
}

static char ck_tmp[PATH_MAX];

/* Run the extracted option code; return the status it exits with, or 0. */
static int ck_run(_Tt_string &outfile, _Tt_string &pipenm)
{
    ck_trap = 1;
    try {
        ck_region(0, outfile, pipenm);
    } catch (CkExit &e) {
        ck_trap = 0;
        return e.code;
    }
    ck_trap = 0;
    return 0;
}

static void ck_check_pipe(const _Tt_string &pipenm)
{
    struct stat st;
    char dir[PATH_MAX], first[PATH_MAX], *slash;

    assert(pipenm.len() > 0);
    snprintf(dir, sizeof dir, "%s", (char *)pipenm);
    slash = strrchr(dir, '/');
    assert(slash);
    *slash = '\0';
    /* (a) a FIFO, mode 0600, inside a private directory, mode 0700 */
    assert(lstat(pipenm, &st) == 0 && S_ISFIFO(st.st_mode));
    assert((st.st_mode & 07777) == 0600 && st.st_uid == geteuid());
    assert(ck_private_dir(dir));
    snprintf(first, sizeof first, "%s/traceAAAAAA", ck_tmp);
    /* (b) the planted link is not the name, and is neither followed nor replaced */
    assert(strcmp(dir, first) != 0);
    assert(strncmp(dir, ck_tmp, strlen(ck_tmp)) == 0 && dir[strlen(ck_tmp)] == '/');
    assert(lstat(first, &st) == 0 && S_ISLNK(st.st_mode));
    ck_victim_intact();
}

static int ck_child(void (*scenario)(void))
{
    int status = 0;
    pid_t pid;
    fflush(NULL);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        scenario();
        (exit)(0);		/* runs the atexit handler of the program */
    }
    assert(waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void ck_s_create(void)
{
    _Tt_string outfile, pipenm;
    assert(ck_run(outfile, pipenm) == 0);
    ck_check_pipe(pipenm);
}

static void ck_s_mkdtemp_fails(void)
{
    _Tt_string outfile, pipenm;
    ck_mk_fail = 1;
    assert(ck_run(outfile, pipenm) == 2);	/* (c) reported, not hidden */
    assert(pipenm.len() == 0);
    assert(ck_count(ck_tmp) == 1);		/* only the planted link */
}

static void ck_s_mkfifo_fails(void)
{
    _Tt_string outfile, pipenm;
    ck_fifo_fail = 1;
    assert(ck_run(outfile, pipenm) == 2);	/* (c) */
    assert(pipenm.len() == 0);
}

static void ck_s_forked_exit(void)
{
    _Tt_string outfile, pipenm;
    struct stat st;
    int status;
    pid_t pid;

    assert(ck_run(outfile, pipenm) == 0);
    fflush(NULL);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0)
        (exit)(1);		/* a command whose exec failed exits like this */
    assert(waitpid(pid, &status, 0) == pid);
    /* the forked process must not have removed the parent's FIFO */
    assert(lstat(pipenm, &st) == 0 && S_ISFIFO(st.st_mode));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(ck_tmp, sizeof ck_tmp, "%s/tmp", ck_work);
    assert(mkdir(ck_tmp, 0755) == 0);
    assert(setenv("TMPDIR", ck_tmp, 1) == 0);
    ck_plant(ck_tmp, "traceAAAAAA");

    /* (a), (b): FIFO in a private directory; both are gone after exit. */
    assert(ck_child(ck_s_create) == 0);
    assert(ck_count(ck_tmp) == 1);
    ck_victim_intact();

    /* (c): mkdtemp fails. */
    assert(ck_child(ck_s_mkdtemp_fails) == 0);
    assert(ck_count(ck_tmp) == 1);

    /* (c): mkfifo fails; the directory made before is removed at exit. */
    assert(ck_child(ck_s_mkfifo_fails) == 0);
    assert(ck_count(ck_tmp) == 1);

    /* The forked command's exit does not remove the parent's FIFO. */
    assert(ck_child(ck_s_forked_exit) == 0);
    assert(ck_count(ck_tmp) == 1);

    /* An unusable TMPDIR is reported, nothing is made. */
    assert(setenv("TMPDIR", "/nonexistent-ck-dir", 1) == 0);
    {
        _Tt_string outfile, pipenm;
        ck_mk_fail = 0;
        ck_mk_calls = 0;
        assert(ck_run(outfile, pipenm) == 2);
        assert(pipenm.len() == 0);
    }
    ck_victim_intact();

    ck_remove_work();
    printf("tttrace-pipe-create: ok\n");
    return 0;
}
