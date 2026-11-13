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
 * a link at the first name.  With ck_mk_fail set it fails as mkstemp does
 * when the directory cannot be written.
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
 * What the unfixed code used: tempnam() returns a predictable name and
 * creates nothing.  The stand-in returns <dir>/AAAAAA, where an attacker
 * who can predict the name plants a link.
 */
static char *ck_tempnam(const char *dir, const char *prefix)
{
    char path[PATH_MAX];
    (void)prefix;
    if (dir == nullptr)
        dir = getenv("TMPDIR");
    if (dir == nullptr)
        dir = "/tmp";
    snprintf(path, sizeof path, "%s/AAAAAA", dir);
    return strdup(path);
}
#define tempnam ck_tempnam

/*
 * Stand-ins for the pipe reader of the canvas engine and for popen(): the
 * interpreter "runs" by checking, at the moment popen() is called, that the
 * file named in the command exists, can be read and holds the data; its
 * output is the text OUT.
 */
typedef void *BufFilePtr;
typedef void *_DtCvPointer;
#define True 1
#define FileStream(b) ((FILE *)(b))

static char *ck_slurp_early(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f)
        return "";
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int ck_popen_fail;
static int ck_popen_calls;
static char ck_cmd[PATH_MAX * 2];
static char ck_popen_file[PATH_MAX];
static int ck_popen_ok;			/* readable, 0400, holds the data */
static const char *ck_expect;
static int ck_reads;

static FILE *ck_popen(const char *cmd, const char *mode)
{
    struct stat st;
    char *sp;
    FILE *f;

    ++ck_popen_calls;
    snprintf(ck_cmd, sizeof ck_cmd, "%s", cmd);
    sp = strrchr(ck_cmd, ' ');
    assert(sp);
    snprintf(ck_popen_file, sizeof ck_popen_file, "%s", sp + 1);
    ck_popen_ok = lstat(ck_popen_file, &st) == 0 && S_ISREG(st.st_mode) &&
                  (st.st_mode & 07777) == 0400 && st.st_uid == geteuid() &&
                  access(ck_popen_file, R_OK) == 0 &&
                  strcmp(ck_slurp_early(ck_popen_file), ck_expect) == 0;
    if (ck_popen_fail)
        return nullptr;
    (void)mode;
    f = fopen("/dev/null", "r");
    assert(f);
    return f;
}
#define popen ck_popen
static int ck_pclose(FILE *f) { return fclose(f); }
#define pclose ck_pclose

static BufFilePtr _DtHelpCeCreatePipeBufFile(FILE *f) { return (BufFilePtr)f; }
static int _DtHelpCeGetNxtBuf(BufFilePtr b, char *buf, char **ptr, int size)
{
    (void)ptr; (void)size;
    if (ck_reads++ == 0) {
        strcpy(buf, "OUT\n");
        return 1;
    }
    (void)fgetc(FileStream(b));		/* reach end of file */
    buf[0] = '\0';
    return 0;
}
static void _DtHelpCeBufFileClose(BufFilePtr b, int flag)
{
    (void)flag;
    fclose(FileStream(b));
}

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

#define DATA_TEXT "script body\n"
#define INTERP "sh"
#define LONG_INTERP "/usr/local/lib/some-long-interpreter-name"
#define VICTIM_TEXT "VICTIM-DATA\n"
static char ck_victim[PATH_MAX];

/* Links planted at the first name of the unfixed code (AAAAAA) and of the fixed one (dtAAAAAA). */
static void ck_plant(const char *dir)
{
    char link[PATH_MAX];
    snprintf(ck_victim, sizeof ck_victim, "%s/victim", ck_work);
    ck_write_file(ck_victim, VICTIM_TEXT, 0644);
    snprintf(link, sizeof link, "%s/AAAAAA", dir);
    assert(symlink(ck_victim, link) == 0);
    snprintf(link, sizeof link, "%s/dtAAAAAA", dir);
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

static void ck_reset(void)
{
    ck_mk_calls = 0;
    ck_mk_fail = 0;
    ck_popen_calls = 0;
    ck_popen_fail = 0;
    ck_popen_ok = 0;
    ck_reads = 0;
    ck_cmd[0] = ck_popen_file[0] = '\0';
}

static void ck_gone(const char *path)
{
    struct stat st;
    assert(lstat(path, &st) == -1 && errno == ENOENT);
}

static int ck_filter_calls;
static int ck_filter(_DtCvPointer client, const char *data, char **newdata)
{
    (void)client;
    ++ck_filter_calls;
    *newdata = strdup(data);		/* the caller must free this */
    return 0;
}

int main(int argc, char **argv)
{
    char dir[PATH_MAX], link[PATH_MAX], *out;
    char data[] = DATA_TEXT;
    int base, rc;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(dir, sizeof dir, "%s/tmp", ck_work);
    assert(mkdir(dir, 0755) == 0);
    assert(setenv("TMPDIR", dir, 1) == 0);
    ck_plant(dir);
    base = ck_count(dir);			/* the two planted links */
    ck_expect = DATA_TEXT;

    /*
     * (b) A link planted at the previously predictable name is never
     * written through; (a) the interpreter finds a plain read-only file of
     * ours with the data (it was created by mkstemp with mode 0600 and made
     * read-only after the data was written).
     */
    ck_reset();
    out = nullptr;
    rc = _DtCvRunInterp(nullptr, nullptr, INTERP, data, &out);
    assert(rc == 0 && out && strcmp(out, "OUT\n") == 0);
    free(out);
    assert(ck_mk_calls == 1 && ck_popen_calls == 1);
    assert(ck_popen_ok);
    ck_victim_intact();
    snprintf(link, sizeof link, "%s/dtAAAAAA", dir);
    assert(strcmp(ck_popen_file, link) != 0);
    assert(strncmp(ck_popen_file, dir, strlen(dir)) == 0 &&
           ck_popen_file[strlen(dir)] == '/' &&
           strncmp(ck_popen_file + strlen(dir) + 1, "dt", 2) == 0);
    assert(strcmp(ck_cmd, INTERP " ") != 0 && strncmp(ck_cmd, INTERP " ", strlen(INTERP) + 1) == 0);
    ck_gone(ck_popen_file);			/* removed afterwards */
    assert(ck_count(dir) == base);

    /*
     * A filter that supplies new data: that data is used and freed.  The
     * command line holds the whole interpreter name (it is longer than a
     * pointer).
     */
    ck_reset();
    ck_filter_calls = 0;
    out = nullptr;
    assert(_DtCvRunInterp(ck_filter, nullptr, LONG_INTERP, data, &out) == 0);
    assert(ck_filter_calls == 1 && ck_popen_ok);
    assert(strncmp(ck_cmd, LONG_INTERP " ", strlen(LONG_INTERP) + 1) == 0);
    free(out);
    assert(ck_count(dir) == base);

    /* (c) mkstemp fails: -1, the interpreter is not run, nothing is left. */
    ck_reset();
    ck_mk_fail = 1;
    ck_filter_calls = 0;
    out = nullptr;
    assert(_DtCvRunInterp(ck_filter, nullptr, INTERP, data, &out) == -1);
    assert(ck_mk_calls == 1 && ck_popen_calls == 0 && out == nullptr);
    assert(ck_count(dir) == base);
    ck_victim_intact();

    /* popen fails: -1 and the file is removed again. */
    ck_reset();
    ck_popen_fail = 1;
    out = nullptr;
    assert(_DtCvRunInterp(nullptr, nullptr, INTERP, data, &out) == -1);
    assert(ck_popen_calls == 1);
    assert(ck_count(dir) == base);
    ck_victim_intact();

    puts("DtHelp interpreter file: created by mkstemp, planted link unused, failures handled");
    ck_remove_work();
    return 0;
}
