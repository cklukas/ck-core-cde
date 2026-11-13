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
#include <fstream>
#include <iostream>

#define CASTEXCEPT
struct Exception {};

/* The part of UAS_String the constructor and destructor use. */
struct UAS_String {
    string s;
    UAS_String() {}
    UAS_String &operator=(const char *p) { s = p ? p : ""; return *this; }
    operator char *() { return (char *)s.c_str(); }
};

struct DtSR_Parser {};

struct DtSR_SearchEngine {
    char **f_dbnames;
    int f_dbcount;
    unsigned int f_valid_bc_map;
    UAS_String f_config_path;
    DtSR_Parser *f_oql_parser;
    DtSR_SearchEngine();
    ~DtSR_SearchEngine();
};

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

/*
 * The production code makes its file in /tmp; the driver rewrites that
 * directory to CK_DIR, a directory of the test.
 */
#define mkstemp ck_mkstemp
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

int main(int argc, char **argv)
{
    char planted[PATH_MAX];
    const char *dir = CK_DIR;
    struct stat st;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(planted, sizeof planted, "%s/.DtSR_AAAAAA", dir);
    ck_plant(dir, ".DtSR_AAAAAA");
    assert(ck_count(dir) == 1);

    /*
     * (a) the configuration file exists, mode 0600, when the constructor
     * returns, (b) a link planted at the previously predictable name is never
     * written, truncated or reused, and the destructor removes the file.
     */
    {
        char name[PATH_MAX];
        DtSR_SearchEngine *e = new DtSR_SearchEngine();
        snprintf(name, sizeof name, "%s", (char *)e->f_config_path);
        ck_victim_intact();
        assert(strcmp(name, planted) != 0);
        assert(strncmp(name, dir, strlen(dir)) == 0 && name[strlen(dir)] == '/');
        assert(ck_private_file(name));
        assert(lstat(planted, &st) == 0 && S_ISLNK(st.st_mode));
        delete e;
        ck_gone(name);
        ck_victim_intact();
        assert(ck_count(dir) == 1);		/* the planted link only */
    }

    /* (c) mkstemp fails: the constructor throws and nothing is created. */
    {
        int thrown = 0;
        ck_mk_fail = 1;
        try {
            DtSR_SearchEngine e;
        } catch (Exception &) {
            thrown = 1;
        }
        ck_mk_fail = 0;
        assert(thrown);
        assert(ck_count(dir) == 1);
        ck_victim_intact();
    }

    /*
     * (c) the file made cannot be opened for writing: the constructor throws
     * and removes the file.  Skipped for root, which opens any file.
     */
    if (geteuid() != 0) {
        int thrown = 0;
        ck_mk_unwritable = 1;
        try {
            DtSR_SearchEngine e;
        } catch (Exception &) {
            thrown = 1;
        }
        ck_mk_unwritable = 0;
        assert(thrown);
        assert(ck_count(dir) == 1);
        ck_victim_intact();
    }

    ck_remove_work();
    printf("search-engine-config-create: ok\n");
    return 0;
}
