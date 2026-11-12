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
typedef char XeChar;
#define TRUE 1
#define FALSE 0
#define SPC_ERROR 0
#define XeInternalError 1
#define XeString_NULL ((XeString)0)
#define TEMPLATE_EXTENSION	  (XeString)".XXXXXX"
#define XeFree free
#define _DtSvcProcessLock()
#define _DtSvcProcessUnlock()

XeChar spc_logfile[MAXPATHLEN+1];
XeChar spc_logging = FALSE;
FILE *spc_logF = NULL;
static char *XeProgName = "test";
static char ck_temp_path[PATH_MAX];	/* what XeSBTempPath() reports */
static void _DtSimpleError(char *prog, int severity, XeString id, XeString fmt, ...) { abort(); }
static XeString XeSBTempPath(XeString suffix)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", ck_temp_path, suffix ? suffix : "");
    return strdup(path);
}
/* The log line SPC_Format_Log() writes once the log is open. */
static void SPC_Format_Log(XeString fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(spc_logF, fmt, ap);
    va_end(ap);
    fputc('\n', spc_logF);
    fflush(spc_logF);
}

/*
 * What the unfixed code used: mktemp() replaces the XXXXXX and creates
 * nothing.  The stand-in gives the first name, AAAAAA, that an attacker who
 * can predict the name plants a link at.
 */
static char *ck_mktemp(char *t)
{
    size_t n = strlen(t);
    if (n < 6 || strcmp(t + n - 6, "XXXXXX") != 0) {
        t[0] = '\0';
        return t;
    }
    memcpy(t + n - 6, "AAAAAA", 6);
    return t;
}
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

static void close_log(void)
{
    if (spc_logF)
        fclose(spc_logF);
    spc_logF = NULL;
    spc_logging = FALSE;
}

int main(int argc, char **argv)
{
    char name[PATH_MAX], link[PATH_MAX], victim[PATH_MAX], longname[MAXPATHLEN + 1];
    struct stat st;
    int before;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(ck_temp_path, sizeof ck_temp_path, "%s", ck_work);
    umask(022);		/* an unfixed fopen(..., "a+") would give mode 0644 */

    /*
     * (b) A link planted at the first name is not followed: the log opens
     * under another name and the target is not written.
     */
    snprintf(name, sizeof name, "%s/log", ck_work);
    snprintf(link, sizeof link, "%s/log.AAAAAA", ck_work);
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_plant(link, victim);
    assert(SPC_Open_Log(name, TRUE) == TRUE);
    ck_victim_intact(victim);
    assert(strcmp(spc_logfile, link) != 0);
    assert(strncmp(spc_logfile, name, strlen(name)) == 0);
    assert(strlen(spc_logfile) == strlen(name) + 7);
    ck_check_created(spc_logfile);
    close_log();

    /* (a) The file exists with mode 0600 as soon as the name is made. */
    ck_mk_calls = 0;
    assert(SPC_Make_Log_Filename(name, TRUE) == TRUE);
    assert(ck_mk_calls == 1);
    ck_check_created(spc_logfile);
    assert(stat(spc_logfile, &st) == 0 && st.st_size == 0);
    assert(strncmp(ck_mk_template, name, strlen(name)) == 0);
    assert(strcmp(ck_mk_template + strlen(name), ".XXXXXX") == 0);
    unlink(spc_logfile);

    /* The default name is made unique in the temp directory the same way. */
    assert(SPC_Open_Log(NULL, TRUE) == TRUE);
    ck_check_created(spc_logfile);
    assert(strncmp(spc_logfile, ck_work, strlen(ck_work)) == 0);
    close_log();

    /* A log that is not unique keeps the name it is given and makes no file. */
    ck_mk_calls = 0;
    snprintf(name, sizeof name, "%s/plain.log", ck_work);
    assert(SPC_Make_Log_Filename(name, FALSE) == TRUE);
    assert(ck_mk_calls == 0 && strcmp(spc_logfile, name) == 0);
    assert(lstat(name, &st) != 0);

    /*
     * (c) mkstemp fails: no name, no file, and the log does not open under
     * the predictable name instead.
     */
    snprintf(name, sizeof name, "%s/failing", ck_work);
    before = ck_count(ck_work);
    ck_mk_fail = 1;
    assert(SPC_Make_Log_Filename(name, TRUE) == FALSE);
    assert(spc_logfile[0] == '\0');
    assert(SPC_Open_Log(name, TRUE) == SPC_ERROR);
    assert(spc_logF == NULL && !spc_logging);
    assert(ck_count(ck_work) == before);
    ck_mk_fail = 0;

    /* A name too long for its extension is refused, not written past the array. */
    memset(longname, 'a', MAXPATHLEN - 3);
    longname[MAXPATHLEN - 3] = '\0';
    assert(SPC_Make_Log_Filename(longname, TRUE) == FALSE);
    assert(spc_logfile[0] == '\0');

    puts("SPC log: unique name made by mkstemp, planted link unused, failures handled");
    ck_remove_work();
    return 0;
}
