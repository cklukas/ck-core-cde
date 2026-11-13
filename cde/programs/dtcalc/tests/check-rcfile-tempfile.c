/*
 * Behavioural test for write_rcfile() in dtcalc's calctool.c: the temporary
 * copy of ~/.dtcalcrc must be created race-free (mkstemp + fdopen), and a
 * failure to create it must leave the user's rc file untouched and leak
 * neither a descriptor nor a temporary file.
 *
 * The runner (check-rcfile-tempfile.py) prepends this file's prologue to
 * write_rcfile(), which it extracts from calctool.c (the working tree, or
 * another revision with --source-revision REV), and appends the test body
 * (after the body marker).  Only the fixed "/tmp/.dtcalcrcXXXXXX" template
 * literal is replaced by a template inside the test's own scratch directory,
 * so nothing is created in the real /tmp and HOME/cwd are the scratch dir.
 * mkstemp, mktemp, fdopen and fopen are redirected to wrappers that can fail
 * on demand or plant a symlink at the temporary name before it is opened.
 *
 * The runner splits this file at its two @@ marker comments.
 */

/* @@PROLOGUE@@ */
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXLINE 256
#define RCNAME ".dtcalcrc"
enum menu_type { M_ACC, M_BASE, M_CON, M_EXCH, M_FUN, M_MODE };

static const char *ck_template;		/* replaces "/tmp/.dtcalcrcXXXXXX" */
static const char *ck_tmpdir;
static const char *ck_victim;		/* symlink target to plant, or NULL */
static int fail_mkstemp, fail_fdopen;
static int mkstemp_calls, mktemp_calls, fdopen_calls, tmp_fopen_calls;
static int tmp_mode_ok = 1;
static char seen_names[8][MAXLINE];

static int ck_is_tmp(const char *p)
{
    size_t n = strlen(ck_tmpdir);
    return strncmp(p, ck_tmpdir, n) == 0 && p[n] == '/';
}

static int ck_mkstemp(char *t)
{
    struct stat st;
    int fd;
    ++mkstemp_calls;
    if (fail_mkstemp) { errno = EACCES; return -1; }
    fd = mkstemp(t);
    if (fd >= 0) {
        if (mkstemp_calls <= 8) strcpy(seen_names[mkstemp_calls - 1], t);
        if (fstat(fd, &st) || (st.st_mode & 0777) != 0600) tmp_mode_ok = 0;
    }
    return fd;
}

static char *ck_mktemp(char *t)
{
    ++mktemp_calls;
    return mktemp(t);
}

static FILE *ck_fdopen(int fd, const char *mode)
{
    ++fdopen_calls;
    if (fail_fdopen) { errno = ENOMEM; return nullptr; }
    return fdopen(fd, mode);
}

static FILE *ck_fopen(const char *path, const char *mode)
{
    if (ck_is_tmp(path)) {
        ++tmp_fopen_calls;
        /* A local attacker wins the race between mktemp() and fopen(). */
        if (ck_victim) symlink(ck_victim, path);
    }
    return fopen(path, mode);
}

#define mkstemp ck_mkstemp
#define mktemp ck_mktemp
#define fdopen ck_fdopen
#define fopen ck_fopen
#define CK_TEMPLATE ck_template

/* @@BODY@@ */
#undef mkstemp
#undef mktemp
#undef fdopen
#undef fopen

static int failures;
#define CHECK(c) do { if (!(c)) { ++failures; \
    fprintf(stderr, "CHECK failed (line %d): %s\n", __LINE__, #c); } } while (0)

static char rcpath[MAXPATHLEN];

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void slurp(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t k;
    buf[0] = 0;
    if (!f) { snprintf(buf, n, "<missing>"); return; }
    k = fread(buf, 1, n - 1, f);
    buf[k] = 0;
    fclose(f);
}

static int open_fds(void)
{
    int fd, n = 0;
    for (fd = 0; fd < 256; fd++)
        if (fcntl(fd, F_GETFD) != -1) ++n;
    return n;
}

static int dir_entries(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) ++n;
    closedir(d);
    return n;
}

static void reset(void)
{
    fail_mkstemp = fail_fdopen = 0;
    ck_victim = nullptr;
    mkstemp_calls = mktemp_calls = fdopen_calls = tmp_fopen_calls = 0;
    tmp_mode_ok = 1;
    memset(seen_names, 0, sizeof seen_names);
}

int main(int argc, char **argv)
{
    static char tmpl[2 * MAXPATHLEN], tmpdir[MAXPATHLEN], victim[MAXPATHLEN];
    char got[512];
    int fds;

    if (argc != 2) { fprintf(stderr, "usage: %s SCRATCHDIR\n", argv[0]); return 2; }
    snprintf(tmpdir, sizeof tmpdir, "%s/tmp", argv[1]);
    snprintf(tmpl, sizeof tmpl, "%s/.dtcalcrcXXXXXX", tmpdir);
    snprintf(rcpath, sizeof rcpath, "%s/%s", argv[1], RCNAME);
    snprintf(victim, sizeof victim, "%s/victim", argv[1]);
    ck_tmpdir = tmpdir;
    ck_template = tmpl;
    mkdir(tmpdir, 0700);
    if (chdir(argv[1])) { perror("chdir"); return 2; }
    setenv("HOME", argv[1], 1);
    if (dir_entries(tmpdir) != 0) { fprintf(stderr, "scratch tmp dir not empty\n"); return 2; }

    /* Case 1: normal run creates a unique, private, writable temp file,
     * rewrites the rc file and leaves nothing behind. */
    reset(); unlink(rcpath);
    fds = open_fds();
    write_rcfile(M_CON, 0, 3, "1.5", "pi");
    slurp(rcpath, got, sizeof got);
    CHECK(strcmp(got, "\nC3 1.5 pi\n") == 0);
    CHECK(mkstemp_calls == 1);
    CHECK(mktemp_calls == 0);
    CHECK(tmp_fopen_calls == 0);	/* never reopened by name */
    CHECK(tmp_mode_ok);
    CHECK(open_fds() == fds);
    CHECK(dir_entries(tmpdir) == 0);
    write_rcfile(M_CON, 0, 4, "2", "e");
    CHECK(mkstemp_calls == 2);
    CHECK(seen_names[0][0] && seen_names[1][0] &&
          strcmp(seen_names[0], seen_names[1]) != 0);
    CHECK(dir_entries(tmpdir) == 0);

    /* Case 2: an existing entry is commented out when replaced. */
    reset(); put(rcpath, "C3 old x\n");
    write_rcfile(M_CON, 1, 3, "7", "seven");
    slurp(rcpath, got, sizeof got);
    CHECK(strcmp(got, "#C3 old x\n\nC3 7 seven\n") == 0);
    CHECK(dir_entries(tmpdir) == 0);

    /* Case 3: mkstemp fails.  The rc file must stay untouched, nothing is
     * opened or left behind. */
    reset(); put(rcpath, "C3 keep me\n"); fail_mkstemp = 1;
    fds = open_fds();
    write_rcfile(M_CON, 1, 3, "9", "nine");
    slurp(rcpath, got, sizeof got);
    CHECK(mkstemp_calls == 1);
    CHECK(strcmp(got, "C3 keep me\n") == 0);
    CHECK(open_fds() == fds);
    CHECK(dir_entries(tmpdir) == 0);

    /* Case 4: fdopen fails.  The descriptor is closed, the temp file is
     * unlinked and the rc file stays untouched. */
    reset(); put(rcpath, "C3 keep me\n"); fail_fdopen = 1;
    fds = open_fds();
    write_rcfile(M_CON, 1, 3, "9", "nine");
    slurp(rcpath, got, sizeof got);
    CHECK(fdopen_calls == 1);
    CHECK(strcmp(got, "C3 keep me\n") == 0);
    CHECK(open_fds() == fds);
    CHECK(dir_entries(tmpdir) == 0);

    /* Case 5: a symlink planted at the temporary name must never be
     * followed; the file it points to keeps its content. */
    reset(); put(rcpath, "C3 old x\n"); put(victim, "precious\n");
    ck_victim = victim;
    write_rcfile(M_CON, 1, 3, "7", "seven");
    slurp(victim, got, sizeof got);
    CHECK(strcmp(got, "precious\n") == 0);
    slurp(rcpath, got, sizeof got);
    CHECK(strcmp(got, "#C3 old x\n\nC3 7 seven\n") == 0);
    CHECK(dir_entries(tmpdir) == 0);

    unlink(rcpath); unlink(victim);
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("all write_rcfile tempfile checks passed");
    return 0;
}
