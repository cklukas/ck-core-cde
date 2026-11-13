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

#define XtMalloc(n) malloc(n)
#define XtNewString(s) strdup(s)
#define DemoDirectory 0
typedef enum { DragDirectory, DropDirectory } DragOrDrop;
static char ck_demo[PATH_MAX];
static char *fileGetDemoDirectory(void) { return ck_demo; }

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

#define mkstemp ck_mkstemp
#define tempnam ck_tempnam
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

#define DOC_TEXT "dropped data\n"

int main(int argc, char **argv)
{
    char dir[PATH_MAX], path[PATH_MAX], planted[PATH_MAX], target[PATH_MAX], link[PATH_MAX];
    char *a, *b, *c;
    struct stat st;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(ck_demo, sizeof ck_demo, "%s/demo", ck_work);
    snprintf(dir, sizeof dir, "%s/FileDropDir", ck_demo);
    assert(mkdir(ck_demo, 0755) == 0 && mkdir(dir, 0755) == 0);
    ck_plant(dir, "docAAAAAA");
    snprintf(planted, sizeof planted, "%s/docAAAAAA", dir);

    /* A new name is stored under that name. */
    a = fileStoreBuffer("doc", DOC_TEXT, (int)strlen(DOC_TEXT));
    assert(a);
    snprintf(path, sizeof path, "%s/doc", dir);
    assert(strcmp(a, path) == 0);
    assert(strcmp(ck_slurp(a), DOC_TEXT) == 0);

    /*
     * (a), (b): the name exists, so a unique one is made; the link planted at
     * the previously predictable name (docAAAAAA) is never written through,
     * and the file made is private (mode 0600).
     */
    b = fileStoreBuffer("doc", "second\n", 7);
    assert(b);
    ck_victim_intact();
    assert(strcmp(b, a) != 0 && strcmp(b, planted) != 0);
    assert(strncmp(b, dir, strlen(dir)) == 0 && b[strlen(dir)] == '/');
    assert(ck_private_file(b));
    assert(strcmp(ck_slurp(b), "second\n") == 0);
    assert(strcmp(ck_slurp(a), DOC_TEXT) == 0);		/* the first is untouched */
    assert(lstat(planted, &st) == 0 && S_ISLNK(st.st_mode));

    /*
     * (b) a dangling link at the wanted name must not be written through
     * either: the file lands under another name, the link target is not made.
     */
    snprintf(target, sizeof target, "%s/target-not-to-be-created", ck_work);
    snprintf(link, sizeof link, "%s/ghost", dir);
    assert(symlink(target, link) == 0);
    c = fileStoreBuffer("ghost", "third\n", 6);
    assert(c);
    ck_gone(target);
    assert(strcmp(c, link) != 0);
    assert(strcmp(ck_slurp(c), "third\n") == 0);
    assert(lstat(link, &st) == 0 && S_ISLNK(st.st_mode));
    free(c);

    /* (c) no unique name can be made: NULL, and the existing file is untouched. */
    ck_mk_fail = 1;
    c = fileStoreBuffer("doc", "fourth\n", 7);
    ck_mk_fail = 0;
    assert(c == NULL);
    assert(strcmp(ck_slurp(a), DOC_TEXT) == 0);
    ck_victim_intact();

    /* (c) a name that does not fit is refused. */
    {
        char longname[PATH_MAX + 8];
        memset(longname, 'n', sizeof longname - 1);
        longname[sizeof longname - 1] = '\0';
        assert(fileStoreBuffer(longname, "x", 1) == NULL);
    }

    /* No name: "unnamed". */
    c = fileStoreBuffer(NULL, "u", 1);
    assert(c);
    snprintf(path, sizeof path, "%s/unnamed", dir);
    assert(strcmp(c, path) == 0);

    free(a);
    free(b);
    free(c);
    ck_remove_work();
    printf("drop-file-create: ok\n");
    return 0;
}
