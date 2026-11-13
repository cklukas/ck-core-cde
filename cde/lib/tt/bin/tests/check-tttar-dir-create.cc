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
#include <string>
using namespace std;
#include <locale.h>

typedef int bool_t;
#define TRUE 1
#define FALSE 0
enum Archive_mode { CREATE, EXTRACT, LIST };

/* The part of _Tt_string the production code uses. */
struct _Tt_string {
    string s;
    _Tt_string() {}
    _Tt_string(const char *p) : s(p ? p : "") {}
    _Tt_string &operator=(const char *p) { s = p ? p : ""; return *this; }
    _Tt_string cat(const char *p) const { return _Tt_string((s + p).c_str()); }
    operator char *() const { return (char *)s.c_str(); }
};

char our_process_name[80];
int verbosity = 0;

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

/* exit() stand-in: with ck_trap set it throws, so the test sees the status. */
static int ck_trap;
struct CkExit { int code; };
[[noreturn]] static void ck_exit(int code)
{
    if (ck_trap)
        throw CkExit{code};
    (exit)(code);
    abort();
}

/*
 * Stand-in for the archiver: records what main() hands to it.  do_tttar()
 * writes the tttarfile the way the real one does, and notes whether the
 * directory of the tttarfile was private (mode 0700) at that time.
 */
static Archive_mode ck_mode;
static int ck_tttar_calls;
static int ck_tar_calls;
static int ck_dir_private;
static char ck_tttar_dir[PATH_MAX];
static char ck_tttar_file[PATH_MAX];

static int ck_private_dir(const char *path);

struct archiver {
    archiver(const char *) {}
    void parse_args(int, char **) {}
    _Tt_string process_name() { return _Tt_string("tttar"); }
    int verbosity() { return 0; }
    void verbosity_set(int) {}
    bool_t should_tar() { return TRUE; }
    Archive_mode mode() { return ck_mode; }
    void mode_set(Archive_mode m) { ck_mode = m; }
    bool_t only_1_look_at_tarfile() { return FALSE; }
    _Tt_string tarfile_arg() { return _Tt_string("a.tar"); }
    void do_tar(_Tt_string &dir, bool_t) {
        ++ck_tar_calls;
        snprintf(ck_tttar_dir, sizeof ck_tttar_dir, "%s", (char *)dir);
    }
    bool_t do_tttar(char *file, bool_t) {
        char *slash;
        int fd;
        ++ck_tttar_calls;
        snprintf(ck_tttar_file, sizeof ck_tttar_file, "%s", file);
        snprintf(ck_tttar_dir, sizeof ck_tttar_dir, "%s", file);
        slash = strrchr(ck_tttar_dir, '/');
        assert(slash);
        *slash = '\0';
        ck_dir_private = ck_private_dir(ck_tttar_dir);
        fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(fd >= 0);
        assert(write(fd, "TTTAR\n", 6) == 6);
        close(fd);
        return TRUE;
    }
};

#define mkdtemp ck_mkdtemp
#define tempnam ck_tempnam
#define exit(c) ck_exit(c)
#define main ck_tttar_main
/* @@BODY@@ */
#undef main

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

static char ck_tmp[PATH_MAX];

static void ck_reset(void)
{
    ck_tttar_calls = ck_tar_calls = ck_dir_private = 0;
    ck_tttar_dir[0] = ck_tttar_file[0] = '\0';
    ck_mk_fail = 0;
    ck_mk_calls = 0;
}

/* Run the extracted main(); return the status it exits with, or 0. */
static int ck_run(void)
{
    char arg0[] = "tttar";
    char *argv[] = { arg0, NULL };
    ck_trap = 1;
    try {
        ck_tttar_main(1, argv);
    } catch (CkExit &e) {
        ck_trap = 0;
        return e.code;
    }
    ck_trap = 0;
    return 0;
}

int main(int argc, char **argv)
{
    char first[PATH_MAX];
    Archive_mode modes[3] = { CREATE, EXTRACT, LIST };
    int i;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(ck_tmp, sizeof ck_tmp, "%s/tmp", ck_work);
    assert(mkdir(ck_tmp, 0755) == 0);
    assert(setenv("TMPDIR", ck_tmp, 1) == 0);
    ck_plant(ck_tmp, "tttardirAAAAAA");
    snprintf(first, sizeof first, "%s/tttardirAAAAAA", ck_tmp);

    /*
     * (a) the directory of the tttarfile is made, mode 0700, before the
     * archiver sees it, (b) the link planted at the previously predictable
     * name is neither used nor replaced, and the directory is removed again.
     */
    for (i = 0; i < 3; i++) {
        struct stat st;
        ck_reset();
        ck_mode = modes[i];
        assert(ck_run() == 0);
        assert(ck_tttar_calls == 1);
        assert(ck_dir_private);
        assert(strcmp(ck_tttar_dir, first) != 0);
        assert(strncmp(ck_tttar_dir, ck_tmp, strlen(ck_tmp)) == 0 &&
               ck_tttar_dir[strlen(ck_tmp)] == '/');
        assert(strstr(ck_tttar_dir, "/tttardir") != NULL);
        assert(lstat(first, &st) == 0 && S_ISLNK(st.st_mode));
        ck_victim_intact();
        ck_gone(ck_tttar_dir);			/* removed with its tttarfile */
        assert(ck_count(ck_tmp) == 1);		/* the planted link only */
    }

    /* (c) mkdtemp fails: reported with status 1, nothing is archived. */
    ck_reset();
    ck_mode = CREATE;
    ck_mk_fail = 1;
    assert(ck_run() == 1);
    assert(ck_tttar_calls == 0 && ck_tar_calls == 0);
    assert(ck_count(ck_tmp) == 1);
    ck_victim_intact();

    /* (c) the directory name does not fit: reported with status 1. */
    {
        string longdir(ck_tmp);
        longdir += "/";
        longdir.append(PATH_MAX, 'd');
        assert(setenv("TMPDIR", longdir.c_str(), 1) == 0);
        ck_reset();
        assert(ck_run() == 1);
        assert(ck_tttar_calls == 0 && ck_tar_calls == 0 && ck_mk_calls == 0);
        assert(setenv("TMPDIR", ck_tmp, 1) == 0);
    }

    /* (c) the temporary directory does not exist: status 1, nothing made. */
    assert(setenv("TMPDIR", "/nonexistent-ck-dir", 1) == 0);
    ck_reset();
    assert(ck_run() == 1);
    assert(ck_tttar_calls == 0 && ck_tar_calls == 0);

    ck_remove_work();
    printf("tttar-dir-create: ok\n");
    return 0;
}
