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

typedef int Boolean;
#define True 1
#define False 0
typedef struct { int unused; } Display;
typedef struct { int unused; } *Widget;
typedef unsigned long Atom;

static char *ck_dir;			/* what _DtCreateDtDirs() reports */
static int ck_xtmalloc_fail;

static void *XtMalloc(size_t n)
{
    if (ck_xtmalloc_fail)
        return NULL;		/* the real one aborts; the code tests the result */
    return malloc(n);
}
static void XtFree(void *p) { free(p); }
static void XFree(void *p) { free(p); }
static Display *XtDisplay(Widget w) { return NULL; }
static Atom XInternAtom(Display *d, const char *name, int only) { return 1; }

/* Like the real one: a buffer of MAXPATHLEN bytes holding the dt directory. */
static char *_DtCreateDtDirs(Display *d)
{
    char *p = malloc(MAXPATHLEN);
    snprintf(p, MAXPATHLEN, "%s", ck_dir);
    return p;
}
static char *getSessionName(Display *d, Atom a) { return strdup("session"); }

/*
 * What the unfixed code used: mktemp() replaces the XXXXXX and creates
 * nothing.  The stand-in gives the first name, AAAAAA, that an attacker who
 * can predict the name plants a link at.
 */
static char *ck_mktemp(char *t)
{
    size_t n = strlen(t);
    if (n < 6 || strcmp(t + n - 6, "XXXXXX") != 0) {
        t[0] = '\0';
        return t;
    }
    memcpy(t + n - 6, "AAAAAA", 6);
    return t;
}
#define mktemp ck_mktemp

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

/* A file and a symbolic link to it, planted at the first name mkstemp tries. */
#define VICTIM_TEXT "VICTIM-DATA\n"
static void ck_plant(const char *link, const char *victim)
{
    ck_write_file(victim, VICTIM_TEXT, 0644);
    assert(symlink(victim, link) == 0);
}

/* The planted target must be neither written, truncated nor chmod'ed. */
static void ck_victim_intact(const char *victim)
{
    struct stat st;
    assert(stat(victim, &st) == 0);
    assert((st.st_mode & 07777) == 0644);
    assert(strcmp(ck_slurp(victim), VICTIM_TEXT) == 0);
}

/* The name denotes a plain file the process created: not a link, mode 0600. */
static void ck_check_created(const char *path)
{
    struct stat st;
    assert(lstat(path, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 07777) == 0600);
    assert(st.st_nlink == 1);
    assert(st.st_uid == geteuid());
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

int main(int argc, char **argv)
{
    char sdir[PATH_MAX], link[PATH_MAX], victim[PATH_MAX];
    char *path = NULL, *file = NULL;
    struct stat st;
    FILE *f;
    int before;

    assert(argc == 2);
    ck_make_work(argv[1]);
    ck_dir = ck_work;
    snprintf(sdir, sizeof sdir, "%s/session", ck_work);
    assert(mkdir(sdir, 0755) == 0);

    /*
     * (b) A link planted at the first name is not followed: the path names
     * another file, and the application's fopen("w") does not reach the
     * target.
     */
    snprintf(link, sizeof link, "%s/dtAAAAAA", sdir);
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_plant(link, victim);
    assert(getSessionPath(NULL, "p", &path, &file) == True);
    f = fopen(path, "w");
    assert(f);
    fputs("SESSION\n", f);
    fclose(f);
    ck_victim_intact(victim);
    assert(strcmp(path, link) != 0);
    assert(strcmp(ck_slurp(path), "SESSION\n") == 0);
    XtFree(path);
    XtFree(file);
    path = file = NULL;

    /* (a) The file exists with mode 0600 and the returned names match. */
    assert(getSessionPath(NULL, "p", &path, &file) == True);
    ck_check_created(path);
    assert(stat(path, &st) == 0 && st.st_size == 0);
    assert(strlen(file) == 8 && strncmp(file, "dt", 2) == 0);
    assert(strcmp(path + strlen(path) - 8, file) == 0);
    assert(strncmp(path, sdir, strlen(sdir)) == 0 && path[strlen(sdir)] == '/');
    XtFree(path);
    XtFree(file);
    path = file = NULL;

    /* A name that is given is used as it is, and no file is made. */
    before = ck_count(sdir);
    ck_mk_calls = 0;
    file = strdup("given");
    assert(getSessionPath(NULL, "p", &path, &file) == True);
    assert(ck_mk_calls == 0 && ck_count(sdir) == before);
    assert(strcmp(path + strlen(path) - 5, "given") == 0);
    XtFree(path);
    free(file);
    path = file = NULL;

    /* (c) mkstemp fails: no path, no file. */
    ck_mk_fail = 1;
    assert(getSessionPath(NULL, "p", &path, &file) == False);
    assert(path == NULL && file == NULL);
    assert(ck_count(sdir) == before);
    ck_mk_fail = 0;

    /* The name cannot be returned: the file just made is removed again. */
    ck_xtmalloc_fail = 1;
    assert(getSessionPath(NULL, "p", &path, &file) == False);
    assert(path == NULL && file == NULL);
    assert(ck_count(sdir) == before);
    ck_xtmalloc_fail = 0;

    puts("session save path: file made by mkstemp, planted link unused, failures handled");
    ck_remove_work();
    return 0;
}
