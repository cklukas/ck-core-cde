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

typedef void *Widget;
typedef unsigned long Atom;
typedef unsigned long Window;
typedef short Position;
typedef unsigned short Dimension;
struct Display { int unused; };
#define Success 0
#define False 0
#define IconicState 3
#define XtNx "x"
#define XtNy "y"
#define XtNwidth "width"
#define XtNheight "height"
#define XtWindow(w) ((Window)1)
#define XtName(w) "dialog"
#define LOG_DEFINEFILEPTR(x)
#define LOG_OPENFILEPTR(x)
#define LOG_FPRINTF(x, y)
#define LOG_CLOSEFILEPTR(x)
struct WmState { unsigned long state; unsigned long icon; };

static int ck_private_file(const char *path);

/* Stand-ins for the X, Xt and DtWsm calls the function makes. */
static Atom XmInternAtom(Display *, const char *, int) { return 1; }
static int XGetWindowProperty(Display *, Window, Atom, long, long, int, Atom,
                              Atom *, int *, unsigned long *, unsigned long *,
                              unsigned char **prop)
{
    *prop = (unsigned char *)calloc(1, sizeof(WmState));
    return Success;
}
static int DtWsmGetWorkspacesOccupied(Display *, Window, Atom **ws, unsigned long *n)
{
    *ws = (Atom *)malloc(sizeof(Atom));
    (*ws)[0] = 7;
    *n = 1;
    return Success;
}
static char *XGetAtomName(Display *, Atom) { return strdup("ws0"); }
static void XtFree(char *p) { free(p); }
static void XFree(void *p) { free(p); }
static void XtVaGetValues(Widget, ...)
{
    /* x, y, width, height: 10 each */
    (void)0;
}
static int SafeAccess(const char *path, int mode) { return access(path, mode); }

/* theRoamApp: the session file the function writes its line to. */
static FILE *ck_session_fp;
static struct {
    FILE *sessionFile() { return ck_session_fp; }
    Display *display() { static Display d; return &d; }
} theRoamApp;

/*
 * doAutoSave() stand-in: writes the dead letter the way RFCWriteMessage()
 * does, open(O_RDWR|O_CREAT|O_TRUNC, 0600), and records whether the file
 * existed with mode 0600 when it was called.
 */
static int ck_save_calls, ck_save_existed;
static char ck_save_name[PATH_MAX];

struct SendMsgDialog {
    Widget _w;
    void *_msgHandle;
    char *_auto_save_path;
    void doAutoSave(char *filename)
    {
        int fd;
        ++ck_save_calls;
        snprintf(ck_save_name, sizeof ck_save_name, "%s", filename);
        ck_save_existed = ck_private_file(filename);
        fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0600);
        assert(fd >= 0);
        assert(write(fd, "DEAD-LETTER\n", 12) == 12);
        close(fd);
    }
    void smpSaveSessionLocal(void);
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

#define mkstemp ck_mkstemp
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

static char ck_dead[PATH_MAX], ck_tmp[PATH_MAX], ck_sessfile[PATH_MAX];

/* The name the session file line "SendMsg <name> ..." holds. */
static char *ck_saved_name(void)
{
    static char name[PATH_MAX];
    char *text;
    fflush(ck_session_fp);
    text = ck_slurp(ck_sessfile);
    assert(strncmp(text, "SendMsg ", 8) == 0);
    assert(sscanf(text + 8, "%s", name) == 1);
    return name;
}

static void ck_reset(void)
{
    ck_save_calls = ck_save_existed = 0;
    ck_save_name[0] = '\0';
    ck_mk_fail = 0;
    ck_session_fp = freopen(ck_sessfile, "w+", ck_session_fp);
    assert(ck_session_fp);
}

int main(int argc, char **argv)
{
    SendMsgDialog dlg;
    char planted[PATH_MAX], *name;
    struct stat st;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(ck_dead, sizeof ck_dead, "%s/dead", ck_work);
    snprintf(ck_tmp, sizeof ck_tmp, "%s/tmp", ck_work);
    snprintf(ck_sessfile, sizeof ck_sessfile, "%s/session", ck_work);
    assert(mkdir(ck_dead, 0755) == 0);
    assert(mkdir(ck_tmp, 0755) == 0);
    assert(setenv("TMPDIR", ck_tmp, 1) == 0);
    ck_session_fp = fopen(ck_sessfile, "w+");
    assert(ck_session_fp);
    ck_plant(ck_dead, "sessionAAAAAA");
    snprintf(planted, sizeof planted, "%s/sessionAAAAAA", ck_dead);
    dlg._w = (Widget)&dlg;
    dlg._msgHandle = &dlg;
    dlg._auto_save_path = ck_dead;

    /*
     * (a) the dead letter file exists, mode 0600, when it is handed to
     * doAutoSave(), (b) a link planted at the previously predictable name is
     * never written through, and the session file names the file.
     */
    ck_reset();
    dlg.smpSaveSessionLocal();
    ck_victim_intact();
    assert(ck_save_calls == 1 && ck_save_existed);
    name = ck_saved_name();
    assert(strcmp(name, ck_save_name) == 0);
    assert(strcmp(name, planted) != 0);
    assert(strncmp(name, ck_dead, strlen(ck_dead)) == 0 && name[strlen(ck_dead)] == '/');
    assert(ck_private_file(name));
    assert(strcmp(ck_slurp(name), "DEAD-LETTER\n") == 0);
    assert(lstat(planted, &st) == 0 && S_ISLNK(st.st_mode));
    assert(ck_count(ck_dead) == 2);			/* the link and the file */

    /* (c) no file can be made: the window is not saved, nothing is created. */
    ck_reset();
    ck_mk_fail = 1;
    dlg.smpSaveSessionLocal();
    assert(ck_save_calls == 0);
    fflush(ck_session_fp);
    assert(strlen(ck_slurp(ck_sessfile)) == 0);
    assert(ck_count(ck_dead) == 2 && ck_count(ck_tmp) == 0);
    ck_victim_intact();

    /* Without a directory the dead letter goes to $TMPDIR. */
    ck_reset();
    dlg._auto_save_path = NULL;
    dlg.smpSaveSessionLocal();
    assert(ck_save_calls == 1 && ck_save_existed);
    name = ck_saved_name();
    assert(strncmp(name, ck_tmp, strlen(ck_tmp)) == 0 && name[strlen(ck_tmp)] == '/');
    assert(ck_private_file(name));

    ck_remove_work();
    printf("session-file-create: ok\n");
    return 0;
}
