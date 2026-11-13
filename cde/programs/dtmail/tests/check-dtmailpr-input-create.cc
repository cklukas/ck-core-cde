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

#define DtPERSONAL_TMP_DIRECTORY ".dt/tmp"

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

#define mkstemp ck_mkstemp
#define tempnam ck_tempnam
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

#define INPUT_TEXT "From a@b Thu Nov 12 00:00:00 2026\nSubject: x\n\nbody\n"
static char ck_home[PATH_MAX], ck_tmp[PATH_MAX], ck_input[PATH_MAX];

/* Run the extracted code with ck_input as standard input; NULL if it exits. */
static char *ck_do(int *status)
{
    int fd = open(ck_input, O_RDONLY);
    char *name = NULL;
    assert(fd >= 0 && dup2(fd, 0) == 0);
    if (fd != 0)
        close(fd);
    *status = 0;
    ck_trap = 1;
    try {
        name = ck_region(NULL);
    } catch (CkExit &e) {
        *status = e.code;
    }
    ck_trap = 0;
    return name;
}

int main(int argc, char **argv)
{
    char personal[PATH_MAX], planted[PATH_MAX], *name;
    struct stat st;
    int status;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(ck_home, sizeof ck_home, "%s/home", ck_work);
    snprintf(ck_tmp, sizeof ck_tmp, "%s/tmp", ck_work);
    snprintf(ck_input, sizeof ck_input, "%s/input", ck_work);
    snprintf(personal, sizeof personal, "%s/.dt/tmp", ck_home);
    assert(mkdir(ck_home, 0755) == 0);
    assert(mkdir(ck_tmp, 0755) == 0);
    snprintf(planted, sizeof planted, "%s/.dt", ck_home);
    assert(mkdir(planted, 0755) == 0);
    assert(mkdir(personal, 0755) == 0);
    ck_write_file(ck_input, INPUT_TEXT, 0644);
    assert(setenv("HOME", ck_home, 1) == 0);
    assert(setenv("TMPDIR", ck_tmp, 1) == 0);
    ck_plant(personal, "dtmprAAAAAA");
    snprintf(planted, sizeof planted, "%s/dtmprAAAAAA", personal);

    /*
     * (a) the file exists with mode 0600 and holds the input, (b) a link
     * planted at the previously predictable name is never written through.
     */
    name = ck_do(&status);
    assert(status == 0 && name);
    ck_victim_intact();
    assert(strcmp(name, planted) != 0);
    assert(strncmp(name, personal, strlen(personal)) == 0 && name[strlen(personal)] == '/');
    assert(ck_private_file(name));
    assert(strcmp(ck_slurp(name), INPUT_TEXT) == 0);
    assert(lstat(planted, &st) == 0 && S_ISLNK(st.st_mode));
    assert(ck_count(personal) == 2);			/* the link and the file */
    free(name);

    /* The personal directory is missing: the file goes to $TMPDIR. */
    assert(setenv("HOME", ck_tmp, 1) == 0);
    name = ck_do(&status);
    assert(status == 0 && name);
    assert(strncmp(name, ck_tmp, strlen(ck_tmp)) == 0 && name[strlen(ck_tmp)] == '/');
    assert(ck_private_file(name));
    assert(strcmp(ck_slurp(name), INPUT_TEXT) == 0);
    free(name);
    assert(setenv("HOME", ck_home, 1) == 0);

    /* (c) no file can be made: reported with status 1, nothing is created. */
    {
        int before = ck_count(personal), before_tmp = ck_count(ck_tmp);
        ck_mk_fail = 1;
        name = ck_do(&status);
        ck_mk_fail = 0;
        assert(name == NULL && status == 1);
        assert(ck_count(personal) == before && ck_count(ck_tmp) == before_tmp);
        ck_victim_intact();
    }

    ck_remove_work();
    printf("dtmailpr-input-create: ok\n");
    return 0;
}
