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

#define MAXWMPATH 1023
typedef char *String;

/* The directory the system default temp names are made in. */
static char ck_tmpdir[PATH_MAX];
#undef P_tmpdir
#define P_tmpdir ck_tmpdir

static struct { char *cppCommand; } wmGD;
static struct { char *fileName; char *cppName; } ck_top, *pConfigStackTop = &ck_top;

static int ck_xtmalloc_live;		/* allocations not yet freed */
static void *XtMalloc(size_t n) { ck_xtmalloc_live++; return malloc(n); }
static void XtFree(void *p) { if (p) { ck_xtmalloc_live--; free(p); } }

/*
 * The converter run: "<cppCommand> <in> <out>".  Like cpp it opens the
 * output file for writing and writes the result into it; whatever file or
 * link is at the name receives it.  The file must already be in the shape
 * mkstemp leaves it in.
 */
static int ck_cpp_calls;
static char ck_cpp_out[PATH_MAX];
static void SystemCmd(char *cmd)
{
    char *out = strrchr(cmd, ' ');
    FILE *f;

    assert(out);
    out++;
    ck_cpp_calls++;
    snprintf(ck_cpp_out, sizeof ck_cpp_out, "%s", out);
    f = fopen(out, "w");
    assert(f);
    fputs("PREPROCESSED\n", f);
    fclose(f);
}

/*
 * What the unfixed code used: tmpnam() fills in a name and creates nothing.
 * The stand-in gives the first name, <dir>/AAAAAA, that an attacker who can
 * predict the name plants a link at.
 */
static char *ck_tmpnam(char *buf)
{
    assert(strlen(ck_tmpdir) + 8 < L_tmpnam);
    snprintf(buf, L_tmpnam, "%s/AAAAAA", ck_tmpdir);
    return buf;
}
#define tmpnam ck_tmpnam

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
    char link[PATH_MAX], victim[PATH_MAX], first[PATH_MAX];
    struct stat st;
    int before;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(ck_tmpdir, sizeof ck_tmpdir, "%s", ck_work);
    ck_top.fileName = "dtwmrc";
    wmGD.cppCommand = "cpp";

    /*
     * (b) A link planted at the first name is not followed: the converter
     * writes to another file and the target is not touched.
     */
    snprintf(link, sizeof link, "%s/AAAAAA", ck_work);
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_plant(link, victim);
    PreprocessConfigFile();
    assert(ck_top.cppName && ck_cpp_calls == 1);
    ck_victim_intact(victim);
    assert(strcmp(ck_top.cppName, link) != 0);
    assert(strncmp(ck_top.cppName, ck_work, strlen(ck_work)) == 0);
    assert(strcmp(ck_slurp(ck_top.cppName), "PREPROCESSED\n") == 0);
    assert(strcmp(ck_cpp_out, ck_top.cppName) == 0);
    snprintf(first, sizeof first, "%s", ck_top.cppName);
    XtFree(ck_top.cppName);
    ck_top.cppName = nullptr;
    unlink(first);

    /* (a) The file exists, mode 0600, when the converter is started. */
    ck_mk_calls = 0;
    ck_cpp_calls = 0;
    PreprocessConfigFile();
    assert(ck_mk_calls == 1 && ck_cpp_calls == 1);
    ck_check_created(ck_top.cppName);
    assert(strcmp(ck_mk_template + strlen(ck_work), "/dwmXXXXXX") == 0);
    assert(strlen(ck_top.cppName) == strlen(ck_work) + 10);
    unlink(ck_top.cppName);
    XtFree(ck_top.cppName);
    ck_top.cppName = nullptr;

    /* No converter configured: nothing is made. */
    wmGD.cppCommand = "";
    ck_mk_calls = ck_cpp_calls = 0;
    PreprocessConfigFile();
    assert(ck_top.cppName == nullptr && ck_mk_calls == 0 && ck_cpp_calls == 0);
    wmGD.cppCommand = "cpp";

    /*
     * (c) mkstemp fails: no temp file, no converter run, the name is given
     * back, and the window manager reads the original configuration file.
     */
    before = ck_count(ck_work);
    ck_mk_fail = 1;
    ck_cpp_calls = 0;
    PreprocessConfigFile();
    assert(ck_top.cppName == nullptr && ck_cpp_calls == 0);
    assert(ck_xtmalloc_live == 0);
    assert(ck_count(ck_work) == before);
    ck_mk_fail = 0;

    puts("dtwm config preprocessing: temp file made by mkstemp, planted link unused, failures handled");
    ck_remove_work();
    return 0;
}
