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

#define CEErrorMalloc 1001

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
 * What the unfixed code used: tmpnam() returns a predictable name and
 * creates nothing.  The stand-in returns <dir>/ckAAAAAA, where an attacker
 * who can predict the name plants a link.
 */
static char *ck_tmpnam(char *buf)
{
    const char *dir = getenv("TMPDIR");
    static char own[PATH_MAX];
    if (buf == NULL)
        buf = own;
    snprintf(buf, PATH_MAX, "%s/ckAAAAAA", dir ? dir : "/tmp");
    return buf;
}
#define tmpnam ck_tmpnam

/*
 * Stand-in for _DtHelpCeUncompressFile().  Like the real one it opens the
 * output file by name with O_CREAT|O_WRONLY (mode 0666, no O_TRUNC) and
 * writes into it.  It records what the output name denoted when it was
 * called.
 */
#define UNCOMPRESSED_TEXT "UNCOMPRESSED\n"
static int ck_unc_calls;
static int ck_unc_result;
static int ck_unc_existed;		/* a plain 0600 file of ours existed */
static char ck_unc_out[PATH_MAX];

static int _DtHelpCeUncompressFile(char *infile, char *outfile)
{
    struct stat st;
    int fd;

    (void)infile;
    ++ck_unc_calls;
    snprintf(ck_unc_out, sizeof ck_unc_out, "%s", outfile);
    ck_unc_existed = lstat(outfile, &st) == 0 && S_ISREG(st.st_mode) &&
                     (st.st_mode & 07777) == 0600 && st.st_uid == geteuid() &&
                     st.st_nlink == 1;
    if (ck_unc_result != 0)
        return ck_unc_result;
    fd = open(outfile, O_CREAT | O_WRONLY, 0666);
    if (fd < 0)
        return -1;
    assert(write(fd, UNCOMPRESSED_TEXT, strlen(UNCOMPRESSED_TEXT)) ==
           (ssize_t)strlen(UNCOMPRESSED_TEXT));
    close(fd);
    return 0;
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

/*
 * Links planted at the first name the unfixed code (ckAAAAAA) and the fixed
 * code (dthAAAAAA) would use, both to one victim file.
 */
#define VICTIM_TEXT "VICTIM-DATA\n"
static char ck_victim[PATH_MAX];

static void ck_plant(const char *dir)
{
    char link[PATH_MAX];
    snprintf(ck_victim, sizeof ck_victim, "%s/victim", ck_work);
    ck_write_file(ck_victim, VICTIM_TEXT, 0644);
    snprintf(link, sizeof link, "%s/ckAAAAAA", dir);
    assert(symlink(ck_victim, link) == 0);
    snprintf(link, sizeof link, "%s/dthAAAAAA", dir);
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
    ck_unc_calls = 0;
    ck_unc_result = 0;
    ck_unc_existed = 0;
    ck_unc_out[0] = '\0';
}

static void ck_gone(const char *path)
{
    struct stat st;
    assert(lstat(path, &st) == -1 && errno == ENOENT);
}

int main(int argc, char **argv)
{
    char dir[PATH_MAX], name[PATH_MAX], link[PATH_MAX], *ret;
    char tmpcopy[PATH_MAX];
    struct stat st;
    int fd, rc, base;
    char buf[64];
    ssize_t n;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(dir, sizeof dir, "%s/tmp", ck_work);
    assert(mkdir(dir, 0755) == 0);
    assert(setenv("TMPDIR", dir, 1) == 0);
    ck_plant(dir);
    base = ck_count(dir);			/* the two planted links */
    snprintf(name, sizeof name, "%s/doc", ck_work);	/* absent; "doc.Z" is stubbed */

    /*
     * FileOpenRtnFd, (b): a link planted at the previously predictable name
     * is never written through, and (a) the file existed with mode 0600 when
     * the uncompressor was handed the name.
     */
    ck_reset();
    errno = 0;
    fd = -2;
    rc = FileOpenRtnFd(name, &fd);
    assert(rc == 0 && fd >= 0);
    assert(ck_unc_calls == 1 && ck_unc_existed);
    assert(ck_mk_calls == 1);
    ck_victim_intact();
    snprintf(link, sizeof link, "%s/dthAAAAAA", dir);
    assert(strcmp(ck_unc_out, link) != 0);
    assert(strncmp(ck_unc_out, dir, strlen(dir)) == 0 &&
           ck_unc_out[strlen(dir)] == '/' &&
           strncmp(ck_unc_out + strlen(dir) + 1, "dth", 3) == 0);
    n = read(fd, buf, sizeof buf - 1);
    assert(n > 0);
    buf[n] = '\0';
    assert(strcmp(buf, UNCOMPRESSED_TEXT) == 0);
    close(fd);
    ck_gone(ck_unc_out);			/* unlinked after it was opened */
    assert(ck_count(dir) == base);

    /* The uncompressor fails: -1, ENOENT, no file left behind. */
    ck_reset();
    ck_unc_result = -1;
    errno = 0;
    fd = -2;
    assert(FileOpenRtnFd(name, &fd) == -1 && errno == ENOENT);
    assert(ck_unc_calls == 1 && ck_unc_existed);
    ck_gone(ck_unc_out);
    assert(ck_count(dir) == base);
    ck_victim_intact();

    /* (c) mkstemp fails: -1, nothing is uncompressed to a guessable name. */
    ck_reset();
    ck_mk_fail = 1;
    errno = 0;
    fd = -2;
    assert(FileOpenRtnFd(name, &fd) == -1);
    assert(errno != 0);
    assert(ck_mk_calls == 1 && ck_unc_calls == 0);
    assert(ck_count(dir) == base);
    ck_victim_intact();

    /* An existing file is opened as it is: no temporary file. */
    ck_reset();
    ck_write_file(name, "PLAIN\n", 0644);
    fd = -2;
    assert(FileOpenRtnFd(name, &fd) == 1 && fd >= 0);
    close(fd);
    assert(ck_mk_calls == 0 && ck_unc_calls == 0);
    assert(unlink(name) == 0);

    /*
     * _DtHelpCeGetUncompressedFileName, (b) and (a): the returned name
     * denotes the plain 0600 file the uncompressor wrote, not the link.
     */
    ck_reset();
    ret = NULL;
    rc = _DtHelpCeGetUncompressedFileName(name, &ret);
    assert(rc == 0 && ret && ret != name);
    assert(ck_unc_calls == 1 && ck_unc_existed && ck_mk_calls == 1);
    ck_victim_intact();
    assert(strcmp(ret, ck_unc_out) == 0);
    snprintf(link, sizeof link, "%s/dthAAAAAA", dir);
    assert(strcmp(ret, link) != 0);
    assert(lstat(ret, &st) == 0 && S_ISREG(st.st_mode));
    assert((st.st_mode & 07777) == 0600 && st.st_uid == geteuid());
    assert(strcmp(ck_slurp(ret), UNCOMPRESSED_TEXT) == 0);
    assert(ck_count(dir) == base + 1);
    snprintf(tmpcopy, sizeof tmpcopy, "%s", ret);
    assert(unlink(ret) == 0);			/* the caller removes it */
    free(ret);
    assert(ck_count(dir) == base);

    /* The uncompressor fails: -1, ENOENT, no file left behind. */
    ck_reset();
    ck_unc_result = -1;
    errno = 0;
    ret = NULL;
    assert(_DtHelpCeGetUncompressedFileName(name, &ret) == -1 && errno == ENOENT);
    assert(ck_unc_calls == 1 && ck_unc_existed);
    ck_gone(ck_unc_out);
    assert(ck_count(dir) == base);

    /* (c) mkstemp fails. */
    ck_reset();
    ck_mk_fail = 1;
    errno = 0;
    ret = NULL;
    assert(_DtHelpCeGetUncompressedFileName(name, &ret) == -1);
    assert(errno != 0 && ck_mk_calls == 1 && ck_unc_calls == 0);
    assert(ck_count(dir) == base);
    ck_victim_intact();

    /* An existing file is returned as it is. */
    ck_reset();
    ck_write_file(name, "PLAIN\n", 0644);
    ret = NULL;
    assert(_DtHelpCeGetUncompressedFileName(name, &ret) == 1 && ret == name);
    assert(ck_mk_calls == 0 && ck_unc_calls == 0);

    puts("DtHelp uncompress: temporary file created by mkstemp, planted link unused, failures handled");
    ck_remove_work();
    return 0;
}
