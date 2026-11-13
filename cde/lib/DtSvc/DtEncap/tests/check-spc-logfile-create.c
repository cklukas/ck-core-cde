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

typedef char *XeString;
typedef struct { char *logfile; int IOMode; } SPC_Channel, *SPC_Channel_Ptr;
#define SPC_ERROR 0
#define TRUE 1
#define ERROR (-1)
#define STDIN 0
#define STDOUT 1
#define STDERR 2
#define SPC_Out_Of_Memory 100
#define SPC_Cannot_Open_Log 149
#define SPCIO_USE_LOGFILE 1
#define IS_SPCIO_USE_LOGFILE(a) ((a) & SPCIO_USE_LOGFILE)
#define call_parent_method(channel, func, args, result) ((result) = (__typeof__(result))1)
#define _DtSvcProcessLock()
#define _DtSvcProcessUnlock()

char **SPC_logfile_list = nullptr;
XeString SPCD_Authentication_Dir = nullptr;
static char *ck_tmp_dir_name;		/* what get_tmp_dir() reports */
static int last_error;
static void SPC_Error(int code, ...) { last_error = code; }
static char *get_tmp_dir(void) { return ck_tmp_dir_name ? strdup(ck_tmp_dir_name) : nullptr; }
static void SPC_Close_Unused(void) { }
static int ck_fd[3];
static void ck_dup2(int fd, int target) { ck_fd[target] = fd; }
#define spc_dup2 ck_dup2

static int ck_realloc_fail;
static void *ck_realloc(void *p, size_t n)
{
    if (ck_realloc_fail)
        return nullptr;		/* the old block stays valid, as in libc */
    return realloc(p, n);
}
#define realloc ck_realloc

/*
 * What the unfixed code used: tempnam() returns a name and creates nothing.
 * The stand-in returns the first name, <dir>/<prefix>AAAAAA, that an
 * attacker who can predict the name plants a link at.
 */
static char *ck_tempnam(const char *dir, const char *prefix)
{
    char path[PATH_MAX];
    if (dir == nullptr)
        dir = getenv("TMPDIR");
    if (dir == nullptr)
        dir = "/tmp";
    snprintf(path, sizeof path, "%s/%sAAAAAA", dir, prefix);
    return strdup(path);
}
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

static SPC_Channel_Ptr open_channel(SPC_Channel *c)
{
    c->logfile = nullptr;
    c->IOMode = SPCIO_USE_LOGFILE;
    last_error = 0;
    return open_noio_channel_object(c, SPCIO_USE_LOGFILE, nullptr);
}

/* What the forked child does with the name: open it, log through it. */
static void child_logs(SPC_Channel *c)
{
    assert(post_fork_noio_channel_object(c, 0) == TRUE);
    assert(ck_fd[STDOUT] >= 0 && ck_fd[STDOUT] == ck_fd[STDERR]);
    assert(write(ck_fd[STDOUT], "LOG\n", 4) == 4);
    close(ck_fd[STDOUT]);
}

static void check_in_dir(const SPC_Channel *c, const char *dir)
{
    size_t n = strlen(dir);
    assert(strncmp(c->logfile, dir, n) == 0 && c->logfile[n] == '/');
    assert(strncmp(c->logfile + n + 1, "SPC", 3) == 0);
    assert(strlen(c->logfile + n + 1) == 9);
}

int main(int argc, char **argv)
{
    char dir[PATH_MAX], dir2[PATH_MAX], other[PATH_MAX], link[PATH_MAX], victim[PATH_MAX];
    SPC_Channel a, b, c, d, e, f;
    int before;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(dir, sizeof dir, "%s/auth", ck_work);
    snprintf(other, sizeof other, "%s/tmp", ck_work);
    snprintf(dir2, sizeof dir2, "%s/auth2", ck_work);
    assert(mkdir(dir, 0755) == 0 && mkdir(dir2, 0755) == 0 && mkdir(other, 0755) == 0);

    /*
     * (b) A link planted at the first name is not used: the channel gets
     * another name, and the child's logging does not reach the target.
     */
    SPCD_Authentication_Dir = dir2;
    snprintf(link, sizeof link, "%s/SPCAAAAAA", dir2);
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_plant(link, victim);
    assert(open_channel(&b) == &b && b.logfile);
    child_logs(&b);
    ck_victim_intact(victim);
    check_in_dir(&b, dir2);
    assert(strcmp(b.logfile, link) != 0);
    ck_check_created(b.logfile);
    assert(strcmp(ck_slurp(b.logfile), "LOG\n") == 0);
    assert(strcmp(SPC_logfile_list[0], b.logfile) == 0);

    /* (a) The file exists, with mode 0600, when the channel is returned. */
    SPCD_Authentication_Dir = dir;
    ck_mk_calls = 0;
    assert(open_channel(&a) == &a && a.logfile);
    check_in_dir(&a, dir);
    ck_check_created(a.logfile);
    assert(ck_mk_calls == 1);
    assert(strcmp(ck_mk_template, "XXXXXX") != 0);
    assert(SPC_logfile_list[1] && strcmp(SPC_logfile_list[1], a.logfile) == 0);
    assert(SPC_logfile_list[2] == nullptr);
    child_logs(&a);
    assert(strcmp(ck_slurp(a.logfile), "LOG\n") == 0);

    /* The directory comes from get_tmp_dir() when no authentication dir is set. */
    SPCD_Authentication_Dir = nullptr;
    ck_tmp_dir_name = other;
    assert(open_channel(&c) == &c && c.logfile);
    check_in_dir(&c, other);
    ck_check_created(c.logfile);

    /* ... and from $TMPDIR or the system default when that finds none. */
    ck_tmp_dir_name = nullptr;
    assert(setenv("TMPDIR", other, 1) == 0);
    assert(open_channel(&d) == &d && d.logfile);
    check_in_dir(&d, other);
    ck_check_created(d.logfile);

    /*
     * (c) mkstemp fails: no channel, the error says the log cannot be
     * opened, no name is kept and nothing is left behind.
     */
    before = ck_count(dir) + ck_count(dir2) + ck_count(other);
    ck_mk_fail = 1;
    assert(open_channel(&e) == SPC_ERROR);
    assert(last_error == SPC_Cannot_Open_Log);
    assert(e.logfile == nullptr);
    assert(ck_count(dir) + ck_count(dir2) + ck_count(other) == before);
    SPCD_Authentication_Dir = dir;
    assert(open_channel(&e) == SPC_ERROR);
    assert(e.logfile == nullptr);
    ck_mk_fail = 0;

    /* A list that cannot grow gives the new file up again. */
    ck_realloc_fail = 1;
    assert(open_channel(&f) == SPC_ERROR);
    assert(last_error == SPC_Out_Of_Memory);
    assert(f.logfile == nullptr);
    assert(ck_count(dir) + ck_count(dir2) + ck_count(other) == before);
    ck_realloc_fail = 0;

    puts("SPC logfile: created by mkstemp, planted link unused, failures handled");
    ck_remove_work();
    return 0;
}
