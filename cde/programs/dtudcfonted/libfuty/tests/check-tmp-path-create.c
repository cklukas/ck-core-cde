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
#include <unistd.h>

/*
 * mkstemp stand-in.  It creates the file the way libc does (O_CREAT|O_EXCL,
 * mode 0600, a symbolic link at the name is never followed) but walks the
 * names AAAAAA, AAAAAB, ... instead of random ones, so that a test can plant
 * a file or symbolic link at the first name.  With ck_mk_fail set it fails
 * as mkstemp does when the directory cannot be written.
 */
static int ck_mk_fail;
static int ck_mk_calls;
static char ck_mk_template[PATH_MAX];

static int ck_mkstemp(char *t)
{
    size_t n = strlen(t);
    char *x;
    int i, fd;

    ++ck_mk_calls;
    snprintf(ck_mk_template, sizeof ck_mk_template, "%s", t);
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
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}
#define mkstemp ck_mkstemp

/*
 * What the unfixed code used: tmpnam() fills in a name and creates nothing.
 * The stand-in gives the first name, <dir>/AAAAAA, that an attacker who can
 * predict the name plants a link at.
 */
static char *ck_mktemp(char *t)
{
    size_t n = strlen(t);
    assert(n >= 6 && strcmp(t + n - 6, "XXXXXX") == 0);
    memcpy(t + n - 6, "AAAAAA", 6);
    return t;
}
#define mktemp ck_mktemp

#define TEMPFILEKEY "XXXXXX"
#define DEFAULT_TMPPATH "/var/tmp/"

/* Count the blocks the code under test holds. */
static int ck_live;
static void *ck_malloc(size_t n) { ck_live++; return malloc(n); }
static void ck_free(void *p) { if (p) { ck_live--; free(p); } }
#define malloc ck_malloc
#define free ck_free

/* @@CREATE@@ */


/* @@BODY@@ */

static char ck_work[PATH_MAX];		/* the test's own directory */

static int ck_rm(const char *p, const struct stat *sb, int flag, struct FTW *w)
{
    (void)sb; (void)flag; (void)w;
    return remove(p);
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

#define VICTIM_TEXT "VICTIM-DATA\n"

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

int main(int argc, char **argv)
{
    char link[PATH_MAX], victim[PATH_MAX], dir[PATH_MAX], *name, *name2;
    struct stat st;
    FILE *f;
    int fd, before;

    assert(argc == 2);
    snprintf(ck_work, sizeof ck_work, "%s/w-XXXXXX", argv[1]);
    assert(mkdtemp(ck_work));
    assert(setenv("TMPDIR", ck_work, 1) == 0);

    /*
     * (b) A link planted at the first name is not written through: the
     * callers get another file and the target is untouched.
     */
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_write_file(victim, VICTIM_TEXT, 0644);
    snprintf(link, sizeof link, "%s/AAAAAA", ck_work);
    assert(symlink(victim, link) == 0);
    name = GetTmpPath("target-file");
    assert(name);

    /* (a) The file exists when the name is returned: ours, 0600, empty. */
    assert(lstat(name, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 07777) == 0600);
    assert(st.st_nlink == 1 && st.st_uid == geteuid() && st.st_size == 0);

    /* The way dtcpftogpf opens it: create, write-only, no O_EXCL. */
    fd = open(name, O_WRONLY | O_CREAT, 0664);
    assert(fd >= 0);
    assert(write(fd, "GPF\n", 4) == 4);
    close(fd);
    assert(strcmp(ck_slurp(name), "GPF\n") == 0);
    /* The way dtbdftocpf opens it: fopen "w". */
    f = fopen(name, "w");
    assert(f);
    fputs("BDF\n", f);
    fclose(f);
    assert(strcmp(ck_slurp(name), "BDF\n") == 0);

    assert(stat(victim, &st) == 0 && (st.st_mode & 07777) == 0644);
    assert(strcmp(ck_slurp(victim), VICTIM_TEXT) == 0);
    assert(strcmp(name, link) != 0);
    assert(strncmp(name, ck_work, strlen(ck_work)) == 0 && name[strlen(ck_work)] == '/');
    assert(strlen(name) == strlen(ck_work) + 7);

    /* A second name is another file. */
    name2 = GetTmpPath("target-file");
    assert(name2 && strcmp(name, name2) != 0);
    assert(ck_count(ck_work) == 4);		/* victim, link, two files */
    free(name2);
    free(name);
    assert(ck_live == 0);

    /* A directory with a trailing slash gives no doubled slash. */
    snprintf(dir, sizeof dir, "%s/", ck_work);
    assert(setenv("TMPDIR", dir, 1) == 0);
    name = GetTmpPath("target-file");
    assert(name && strncmp(name, dir, strlen(dir)) == 0 && name[strlen(dir)] != '/');
    assert(strlen(name) == strlen(dir) + 6);
    assert(lstat(name, &st) == 0 && (st.st_mode & 07777) == 0600);
    free(name);

    /*
     * (c) mkstemp fails: no name, nothing left allocated, no file made.
     */
    before = ck_count(ck_work);
    ck_mk_fail = 1;
    assert(GetTmpPath("target-file") == nullptr);
    ck_mk_fail = 0;
    assert(ck_live == 0);
    assert(ck_count(ck_work) == before);

    /* An empty or unset TMPDIR selects the default directory. */
    ck_mk_fail = 1;
    assert(setenv("TMPDIR", "", 1) == 0);
    assert(GetTmpPath("target-file") == nullptr);
    assert(strcmp(ck_mk_template, "/var/tmp/XXXXXX") == 0);
    unsetenv("TMPDIR");
    assert(GetTmpPath("target-file") == nullptr);
    assert(strcmp(ck_mk_template, "/var/tmp/XXXXXX") == 0);
    ck_mk_fail = 0;
    assert(ck_live == 0);

    puts("libfuty GetTmpPath: file made by mkstemp, planted link unused, failure handled");
    nftw(ck_work, ck_rm, 16, FTW_DEPTH | FTW_PHYS);
    return 0;
}
