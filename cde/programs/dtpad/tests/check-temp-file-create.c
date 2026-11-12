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
static char ck_tmpdir[PATH_MAX];
#undef P_tmpdir
#define P_tmpdir ck_tmpdir
static char *ck_tmpnam(char *buf)
{
    assert(strlen(ck_tmpdir) + 8 < L_tmpnam);
    snprintf(buf, L_tmpnam, "%s/AAAAAA", ck_tmpdir);
    return buf;
}
#define tmpnam ck_tmpnam

static int ck_xtmalloc_live;		/* allocations not yet freed */
static void *XtMalloc(size_t n) { ck_xtmalloc_live++; return malloc(n); }
static void XtFree(void *p) { if (p) { ck_xtmalloc_live--; free(p); } }

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
    char link[PATH_MAX], victim[PATH_MAX], own[64], *name;
    struct stat st;
    FILE *f;
    int before;

    assert(argc == 2);
    snprintf(ck_work, sizeof ck_work, "%s/w-XXXXXX", argv[1]);
    assert(mkdtemp(ck_work));
    snprintf(ck_tmpdir, sizeof ck_tmpdir, "%s", ck_work);

    /*
     * (b) A link planted at the first name is not written through: the
     * caller gets another file and the target is untouched.
     */
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_write_file(victim, VICTIM_TEXT, 0644);
    snprintf(link, sizeof link, "%s/AAAAAA", ck_work);
    assert(symlink(victim, link) == 0);
    name = GetTempFile();
    assert(name);
    assert(stat(victim, &st) == 0 && (st.st_mode & 07777) == 0644);
    assert(strcmp(ck_slurp(victim), VICTIM_TEXT) == 0);
    assert(strcmp(name, link) != 0);

    /* (a) The file exists when the name is returned: ours, 0600, empty. */
    assert(strncmp(name, ck_work, strlen(ck_work)) == 0);
    assert(strlen(name) == strlen(ck_work) + 1 + 9);
    assert(strncmp(name + strlen(ck_work), "/dtp", 4) == 0);
    assert(lstat(name, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 07777) == 0600);
    assert(st.st_nlink == 1 && st.st_uid == geteuid() && st.st_size == 0);

    /* The caller writes it by name, overwriting, as the editor save does. */
    f = fopen(name, "w");
    assert(f);
    fputs("TEXT\n", f);
    fclose(f);
    assert(strcmp(ck_slurp(name), "TEXT\n") == 0);
    assert(ck_count(ck_work) == 3);		/* victim, link, new file */
    XtFree(name);
    assert(ck_xtmalloc_live == 0);

    /*
     * (c) mkstemp fails: no file name, nothing left allocated, no file made,
     * and no fallback to a predictable name of our own.
     */
    before = ck_count(ck_work);
    ck_mk_fail = 1;
    assert(GetTempFile() == NULL);
    ck_mk_fail = 0;
    assert(ck_xtmalloc_live == 0);
    assert(ck_count(ck_work) == before);
    snprintf(own, sizeof own, "/usr/tmp/editor%ld", (long)getpid());
    assert(access(own, F_OK) != 0);
    snprintf(own, sizeof own, "/tmp/editor%ld", (long)getpid());
    assert(access(own, F_OK) != 0);

    puts("dtpad temp file: made by mkstemp, planted link unused, failure handled");
    nftw(ck_work, ck_rm, 16, FTW_DEPTH | FTW_PHYS);
    return 0;
}
