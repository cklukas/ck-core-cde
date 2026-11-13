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

typedef struct { int unused; } Xauth;
typedef struct {
    int        cookie_cnt;
    Xauth      **cookies;
    char       auth_filename[PATH_MAX];
    FILE       *auth_file;
} XpPdmServiceRec;

/* The directory the system default temp names are made in. */
static char ck_tmpdir[PATH_MAX];
#undef P_tmpdir
#define P_tmpdir ck_tmpdir

static int ck_writes;
static char *XauFileName(void) { return nullptr; }
static Xauth *XauReadAuth(FILE *f) { return nullptr; }
static void XauDisposeAuth(Xauth *a) { }
static int XauWriteAuth(FILE *f, Xauth *a) { ck_writes++; return fputs("COOKIE\n", f) >= 0; }

static int ck_fdopen_fail;
static FILE *ck_fdopen(int fd, const char *mode)
{
    if (ck_fdopen_fail)
        return nullptr;
    return fdopen(fd, mode);
}
#define fdopen ck_fdopen

/*
 * What the unfixed code used: tmpnam() fills in a name and creates nothing.
 * The stand-in gives the first name, <dir>/AAAAAA, that an attacker who can
 * predict the name plants a link at.
 */
static char *ck_tmpnam(char *buf)
{
    assert(strlen(ck_tmpdir) + 8 < sizeof(((XpPdmServiceRec *)0)->auth_filename));
    snprintf(buf, PATH_MAX, "%s/AAAAAA", ck_tmpdir);
    return buf;
}
#define tmpnam ck_tmpnam

/*
 * The part of mgr_launch_pdm() that writes the cookie file, with the
 * declarations it has there.
 */
static void make_cookie_file(XpPdmServiceRec *rec)
{
    int       i;
    int       original_umask;
    char      *existing_name;
    FILE      *existing_file;
    Xauth     *entry;
    int       len;
    int       auth_fd;


/* @@BODY@@ */
}

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
    char link[PATH_MAX], victim[PATH_MAX], first[PATH_MAX];
    XpPdmServiceRec rec;
    Xauth cookie, *cookies[1] = { &cookie };
    mode_t mask;
    int before;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(ck_tmpdir, sizeof ck_tmpdir, "%s", ck_work);
    umask(022);
    memset(&rec, 0, sizeof rec);
    rec.cookie_cnt = 1;
    rec.cookies = cookies;

    /*
     * (b) A link planted at the first name is not followed: the cookies go
     * to another file and the target is not touched.
     */
    snprintf(link, sizeof link, "%s/AAAAAA", ck_work);
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_plant(link, victim);
    make_cookie_file(&rec);
    assert(ck_writes == 1);
    ck_victim_intact(victim);
    assert(strcmp(rec.auth_filename, link) != 0);
    assert(strncmp(rec.auth_filename, ck_work, strlen(ck_work)) == 0);
    assert(strcmp(ck_slurp(rec.auth_filename), "COOKIE\n") == 0);
    snprintf(first, sizeof first, "%s", rec.auth_filename);
    unlink(first);

    /* (a) The file exists with mode 0600 and holds the cookies; umask is restored. */
    ck_mk_calls = ck_writes = 0;
    memset(&rec, 0, sizeof rec);
    rec.cookie_cnt = 1;
    rec.cookies = cookies;
    make_cookie_file(&rec);
    assert(ck_mk_calls == 1 && ck_writes == 1);
    ck_check_created(rec.auth_filename);
    assert(strcmp(ck_mk_template + strlen(ck_work), "/pdmXXXXXX") == 0);
    assert(strcmp(ck_slurp(rec.auth_filename), "COOKIE\n") == 0);
    mask = umask(022);
    assert(mask == 022);
    unlink(rec.auth_filename);

    /* (c) mkstemp fails: no file, no stream, an empty name (which unlink ignores). */
    before = ck_count(ck_work);
    memset(&rec, 0, sizeof rec);
    rec.cookie_cnt = 1;
    rec.cookies = cookies;
    ck_mk_fail = 1;
    ck_writes = 0;
    make_cookie_file(&rec);
    assert(rec.auth_file == nullptr && rec.auth_filename[0] == '\0' && ck_writes == 0);
    assert(ck_count(ck_work) == before);
    assert(umask(022) == 022);
    ck_mk_fail = 0;

    /* fdopen fails: the file made is removed again and the name emptied. */
    ck_fdopen_fail = 1;
    make_cookie_file(&rec);
    assert(rec.auth_file == nullptr && rec.auth_filename[0] == '\0' && ck_writes == 0);
    assert(ck_count(ck_work) == before);
    ck_fdopen_fail = 0;

    /* No cookies: no file at all. */
    rec.cookie_cnt = 0;
    ck_mk_calls = 0;
    make_cookie_file(&rec);
    assert(ck_mk_calls == 0 && ck_count(ck_work) == before);

    puts("dtpdmd cookie file: made by mkstemp, planted link unused, failures handled");
    ck_remove_work();
    return 0;
}
