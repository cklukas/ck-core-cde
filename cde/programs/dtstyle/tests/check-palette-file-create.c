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
 * mkstemps stand-in.  It creates the file the way libc does (O_CREAT|O_EXCL,
 * mode 0600, a symbolic link at the name is never followed) but walks the
 * names AAAAAA, AAAAAB, ... in front of the suffix instead of random ones,
 * so that a test can plant a file or symbolic link at the first name.  With
 * ck_mk_fail set it fails as mkstemps does when the directory cannot be
 * written.
 */
static int ck_mk_fail;
static int ck_mk_calls;

static int ck_mkstemps(char *t, int suffixlen)
{
    size_t n = strlen(t);
    char *x;
    int i, fd;

    ++ck_mk_calls;
    if (ck_mk_fail) {
        errno = EACCES;
        return -1;
    }
    assert(n >= 6 + (size_t)suffixlen);
    x = t + n - suffixlen - 6;
    assert(memcmp(x, "XXXXXX", 6) == 0);
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
#define mkstemps ck_mkstemps

/*
 * What the unfixed code used: mktemp() fills in a name and creates nothing.
 * The stand-in gives the first name, ...AAAAAA, that an attacker who can
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

#define PALETTE_SUFFIX ".dp"
#define DT_PAL_DIR "/.dt/palettes/"

static int ck_xtmalloc_live;		/* allocations not yet freed */
static void *XtMalloc(size_t n) { ck_xtmalloc_live++; return malloc(n); }
static void XtFree(void *p) { if (p) { ck_xtmalloc_live--; free(p); } }

/* @@CREATE@@ */

/* @@BODY@@ */

/* ---- helpers of the test ---- */
static char ck_work[PATH_MAX];		/* the test's own directory */
static char ck_dir[PATH_MAX];		/* its .dt/palettes directory, with the slash */

static int ck_rm(const char *p, const struct stat *sb, int flag, struct FTW *w)
{
    (void)sb; (void)flag; (void)w;
    return remove(p);
}

static void ck_make_work(const char *scratch)
{
    char sub[PATH_MAX];

    snprintf(ck_work, sizeof ck_work, "%s/w-XXXXXX", scratch);
    assert(mkdtemp(ck_work));
    snprintf(sub, sizeof sub, "%s/.dt", ck_work);
    assert(mkdir(sub, 0700) == 0);
    snprintf(sub, sizeof sub, "%s/.dt/palettes", ck_work);
    assert(mkdir(sub, 0700) == 0);
    snprintf(ck_dir, sizeof ck_dir, "%s/", sub);
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

#define VICTIM_TEXT "VICTIM-DATA\n"
#define PALETTE_TEXT "#ffff00000000\n"

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

/*
 * What WriteOutPalette() does with the name: open "<dir><name>.dp" for
 * reading and writing, create it if missing, no truncation, no O_EXCL.
 */
static void ck_write_palette(const char *name)
{
    char path[PATH_MAX];
    int fd;

    snprintf(path, sizeof path, "%s%s%s", ck_dir, name, PALETTE_SUFFIX);
    fd = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    assert(fd >= 0);
    assert(write(fd, PALETTE_TEXT, strlen(PALETTE_TEXT)) == (ssize_t)strlen(PALETTE_TEXT));
    close(fd);
}

int main(int argc, char **argv)
{
    char link[PATH_MAX], victim[PATH_MAX], path[PATH_MAX], *name;
    struct stat st;
    int before;

    assert(argc == 2);
    ck_make_work(argv[1]);
    CK_SETUP();

    /*
     * (b) A link planted at the first name is not written through: the
     * palette file is another one, and the target is untouched.
     */
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_write_file(victim, VICTIM_TEXT, 0644);
    snprintf(link, sizeof link, "%sdtAAAAAA.dp", ck_dir);
    assert(symlink(victim, link) == 0);
    name = CK_CREATE(ck_dir);
    assert(name);
    assert(strlen(name) == 8 && strncmp(name, "dt", 2) == 0);

    /* (a) The file exists before anything opens it: ours, 0600, one name. */
    snprintf(path, sizeof path, "%s%s.dp", ck_dir, name);
    assert(lstat(path, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 07777) == 0600);
    assert(st.st_nlink == 1 && st.st_uid == geteuid() && st.st_size == 0);
    snprintf(path, sizeof path, "%s%s", ck_dir, name);
    assert(lstat(path, &st) != 0);		/* no stray file without the suffix */
    ck_write_palette(name);
    assert(stat(victim, &st) == 0 && (st.st_mode & 07777) == 0644);
    assert(strcmp(ck_slurp(victim), VICTIM_TEXT) == 0);
    assert(strcmp(name, "dtAAAAAA") != 0);
    snprintf(path, sizeof path, "%s%s.dp", ck_dir, name);
    assert(strcmp(ck_slurp(path), PALETTE_TEXT) == 0);
    XtFree(name);
    assert(ck_xtmalloc_live == 0);
    assert(ck_count(ck_dir) == 2);		/* the link and the palette */

    /* (c) mkstemps fails: no name, no file, nothing left allocated. */
    before = ck_count(ck_dir);
    ck_mk_fail = 1;
    assert(CK_CREATE(ck_dir) == NULL);
    ck_mk_fail = 0;
    assert(ck_xtmalloc_live == 0);
    assert(ck_count(ck_dir) == before);

    puts("dtstyle palette files: made by mkstemps, planted link unused, failure handled");
    ck_remove_work();
    return 0;
}
