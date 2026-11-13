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
typedef void *XtPointer;
typedef void (*XtCallbackProc)(Widget, XtPointer, XtPointer);
#define XtNdestroyCallback "destroyCallback"
#define TRUE 1
#define FALSE 0
#define DTM_TRUE 1
#define DTM_FALSE 0
#define DtPERSONAL_TMP_DIRECTORY ".dt/tmp"
#define DebugPrintf(...) ((void)0)
#define CATGETS(c, s, n, d) (d)
#define DTMAILHELPNOCREATE ((char *)"nocreate")
static int DT_catd;

static int ck_private_file(const char *path);

/*
 * Stand-ins for the classes the two functions use.  copySelected() creates
 * the destination the way the mailbox code does for a name that exists or
 * not: open(O_WRONLY|O_CREAT|O_APPEND, 0600), no O_EXCL, no truncation; it
 * records whether the file existed with mode 0600 when it was called.
 */
#define MSG_TEXT "From a@b Thu Nov 12 00:00:00 2026\nSubject: x\n\nbody\n"
static int ck_cs_calls, ck_cs_existed, ck_cs_error, ck_cs_status;
static char ck_cs_dest[PATH_MAX];
static int ck_posted, ck_jobs;
static char ck_job_file[PATH_MAX], ck_dialog[512];
static XtCallbackProc ck_cb;
static XtPointer ck_cb_data;

struct DtMailEnv {
    int err;
    DtMailEnv() : err(0) {}
    void clear() { err = 0; }
    int isSet() { return err; }
};

struct MsgScrollingList {
    int copySelected(DtMailEnv &e, char *dest, int, int)
    {
        int fd;
        ++ck_cs_calls;
        snprintf(ck_cs_dest, sizeof ck_cs_dest, "%s", dest);
        ck_cs_existed = ck_private_file(dest);
        if (ck_cs_error) {
            e.err = 1;
            return 0;
        }
        if (ck_cs_status)
            return ck_cs_status;
        fd = open(dest, O_WRONLY | O_CREAT | O_APPEND, 0600);
        assert(fd >= 0);
        assert(write(fd, MSG_TEXT, strlen(MSG_TEXT)) == (ssize_t)strlen(MSG_TEXT));
        close(fd);
        return 0;
    }
};

static MsgScrollingList ck_list;

struct ck_Window {
    MsgScrollingList *list() { return &ck_list; }
    void postErrorDialog(DtMailEnv &) { ++ck_posted; }
    int fullHeader() { return 0; }
};

struct DtMailGenDialog {
    void setToErrorDialog(const char *, const char *text)
    {
        snprintf(ck_dialog, sizeof ck_dialog, "%s", text);
    }
    void post_and_return(char *) {}
};

struct DmxPrintJob {
    DmxPrintJob(char *file, int, ck_Window *)
    {
        ++ck_jobs;
        snprintf(ck_job_file, sizeof ck_job_file, "%s", file);
    }
    Widget baseWidget() { return (Widget)this; }
    void execute() {}
};

static void XtRemoveCallback(Widget, const char *, XtCallbackProc, XtPointer) {}
static void XtAddCallback(Widget, const char *, XtCallbackProc cb, XtPointer data)
{
    ck_cb = cb;
    ck_cb_data = data;
}

struct PrintCmd {
    ck_Window *_parent;
    const char *name() { return "print"; }
    void printit(int silent);
    static void printjobcb(Widget w, XtPointer client_data, XtPointer);
};

enum DmxPrintHeadersEnum { DMX_PRINT_HEADERS_ALL, DMX_PRINT_HEADERS_ABBREV };

struct DmxMsg {
    string text;
    void display(DmxPrintHeadersEnum, void (*write_fn)(XtPointer, char *), XtPointer fd)
    {
        write_fn(fd, (char *)text.c_str());
    }
};

struct DmxMailbox {
    string path;
    DmxMsg msg;
    DmxMailbox(char *p) : path(p) {}
    void loadMessages()
    {
        FILE *f = fopen(path.c_str(), "r");
        char buf[1024];
        size_t n;
        assert(f);
        n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
        msg.text = buf;
    }
    DmxMsg *firstMessage() { return &msg; }
    DmxMsg *nextMessage() { return NULL; }
};

struct SaveAsTextCmd {
    ck_Window *_roam_menu_window;
    DtMailGenDialog *_genDialog;
    static void writeText(XtPointer filedes, char *text_buf)
    {
        long fdl = (long)filedes;
        assert(write((int)fdl, text_buf, strlen(text_buf)) == (ssize_t)strlen(text_buf));
    }
    void writeTextFromScrolledList(int fd);
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

static char ck_home[PATH_MAX], ck_tmp[PATH_MAX], ck_personal[PATH_MAX], ck_out[PATH_MAX];

static void ck_reset(void)
{
    ck_cs_calls = ck_cs_existed = ck_cs_error = ck_cs_status = 0;
    ck_posted = ck_jobs = 0;
    ck_cs_dest[0] = ck_job_file[0] = ck_dialog[0] = '\0';
    ck_cb = NULL;
    ck_cb_data = NULL;
    ck_mk_fail = 0;
}

/* (a), (b): the file the name denotes is ours, private, and not the planted link. */
static void ck_check_dest(void)
{
    char planted[PATH_MAX];
    struct stat st;
    snprintf(planted, sizeof planted, "%s/dtmailAAAAAA", ck_personal);
    ck_victim_intact();
    assert(ck_cs_calls == 1);
    assert(ck_cs_existed);			/* made before copySelected() ran */
    assert(strcmp(ck_cs_dest, planted) != 0);
    assert(strncmp(ck_cs_dest, ck_personal, strlen(ck_personal)) == 0 &&
           ck_cs_dest[strlen(ck_personal)] == '/');
    assert(lstat(planted, &st) == 0 && S_ISLNK(st.st_mode));
}

int main(int argc, char **argv)
{
    PrintCmd print;
    SaveAsTextCmd save;
    ck_Window window;
    DtMailGenDialog dialog;
    char path[PATH_MAX];
    int base, fd;

    assert(argc == 2);
    ck_make_work(argv[1]);
    snprintf(ck_home, sizeof ck_home, "%s/home", ck_work);
    snprintf(ck_tmp, sizeof ck_tmp, "%s/tmp", ck_work);
    snprintf(ck_personal, sizeof ck_personal, "%s/.dt/tmp", ck_home);
    snprintf(ck_out, sizeof ck_out, "%s/out", ck_work);
    assert(mkdir(ck_home, 0755) == 0);
    assert(mkdir(ck_tmp, 0755) == 0);
    snprintf(path, sizeof path, "%s/.dt", ck_home);
    assert(mkdir(path, 0755) == 0);
    assert(mkdir(ck_personal, 0755) == 0);
    assert(setenv("HOME", ck_home, 1) == 0);
    assert(setenv("TMPDIR", ck_tmp, 1) == 0);
    ck_plant(ck_personal, "dtmailAAAAAA");
    print._parent = &window;
    save._roam_menu_window = &window;
    save._genDialog = &dialog;
    base = ck_count(ck_personal);			/* the planted link */

    /* ---- PrintCmd::printit ---- */

    /* (a), (b): the print file is private, and the print job removes it. */
    ck_reset();
    print.printit(0);
    ck_check_dest();
    assert(ck_jobs == 1 && strcmp(ck_job_file, ck_cs_dest) == 0);
    assert(ck_private_file(ck_job_file));
    assert(strcmp(ck_slurp(ck_job_file), MSG_TEXT) == 0);
    assert(ck_cb == &PrintCmd::printjobcb && ck_cb_data);
    assert(ck_count(ck_personal) == base + 1);
    ck_cb(NULL, ck_cb_data, NULL);			/* the job widget is destroyed */
    ck_gone(ck_job_file);
    assert(ck_count(ck_personal) == base);

    /* (c) no file can be made: nothing is copied or printed. */
    ck_reset();
    ck_mk_fail = 1;
    print.printit(0);
    assert(ck_cs_calls == 0 && ck_jobs == 0);
    assert(ck_count(ck_personal) == base && ck_count(ck_tmp) == 0);

    /* The copy fails: the error is posted and the empty file is removed. */
    ck_reset();
    ck_cs_error = 1;
    print.printit(0);
    assert(ck_cs_calls == 1 && ck_posted == 1 && ck_jobs == 0);
    ck_gone(ck_cs_dest);
    assert(ck_count(ck_personal) == base);

    /* Nothing is copied (status 1): the file is removed. */
    ck_reset();
    ck_cs_status = 1;
    print.printit(0);
    assert(ck_cs_calls == 1 && ck_jobs == 0);
    ck_gone(ck_cs_dest);
    assert(ck_count(ck_personal) == base);
    ck_victim_intact();

    /* ---- SaveAsTextCmd::writeTextFromScrolledList ---- */

    /* (a), (b): the messages come out through the private file, which is removed. */
    ck_reset();
    fd = open(ck_out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    save.writeTextFromScrolledList(fd);
    close(fd);
    ck_check_dest();
    assert(strncmp(ck_slurp(ck_out), MSG_TEXT, strlen(MSG_TEXT)) == 0);
    ck_gone(ck_cs_dest);
    assert(ck_count(ck_personal) == base);

    /* (c) no file can be made: the user gets the error dialog, nothing is copied. */
    ck_reset();
    ck_mk_fail = 1;
    save.writeTextFromScrolledList(1);
    assert(ck_cs_calls == 0);
    assert(strncmp(ck_dialog, "Unable to create", 16) == 0);
    assert(ck_count(ck_personal) == base && ck_count(ck_tmp) == 0);

    /* The copy fails: the error is posted and the empty file is removed. */
    ck_reset();
    ck_cs_error = 1;
    save.writeTextFromScrolledList(1);
    assert(ck_cs_calls == 1 && ck_posted == 1);
    ck_gone(ck_cs_dest);
    assert(ck_count(ck_personal) == base);

    ck_reset();
    ck_cs_status = 1;
    save.writeTextFromScrolledList(1);
    assert(ck_cs_calls == 1);
    ck_gone(ck_cs_dest);
    assert(ck_count(ck_personal) == base);
    ck_victim_intact();

    ck_remove_work();
    printf("roam-temp-file-create: ok\n");
    return 0;
}
