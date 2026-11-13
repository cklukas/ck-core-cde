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
 * What the unfixed code used: tmpnam() fills in a name and creates nothing.
 * The stand-in gives the first name, <dir>/AAAAAA, that an attacker who can
 * predict the name plants a link at.
 */
static char ck_tmpdir[PATH_MAX];
#undef P_tmpdir
#define P_tmpdir ck_tmpdir
static char *ck_tmpnam(char *buf)
{
    assert(strlen(ck_tmpdir) + 8 < L_tmpnam);
    snprintf(buf, L_tmpnam, "%s/AAAAAA", ck_tmpdir);
    return buf;
}
#define tmpnam ck_tmpnam

/* ---- stand-ins for the toolkit and ToolTalk calls the handler uses ---- */
typedef struct ck_widget *Widget;
typedef int Boolean;
#define True 1
#define False 0
#define MAXBUFSIZE 1024
typedef int Tt_message;
typedef int Tt_pattern;
typedef int Tt_state;
typedef int Tt_status;
typedef int Tt_callback_action;
#define TT_CALLBACK_CONTINUE 0
#define TT_CALLBACK_PROCESSED 1
#define TT_FAILED 1
#define TT_HANDLED 2
#define GETMESSAGE(set, num, str) (str)

typedef struct {
    Widget wid;
    char *pmMsgID;
    char *bmMsgID;
    char pmFileName[MAXPATHLEN];
    char bmFileName[MAXPATHLEN];
    Boolean pmDirtyBit;
    Boolean bmDirtyBit;
} IconData;

static IconData ck_icon;
static Boolean bShowPixmaps = True;
static Widget CreateActionAppShell;
static char *ck_msgid = "MSG-1";
static unsigned char *ck_buf;
static int ck_blen;
static int ck_icon_set_calls;
static char ck_icon_set_name[PATH_MAX];
static char ck_error_text[4096];
static int ck_error_calls;
static int ck_xtmalloc_live;		/* allocations not yet freed */

static void *XtMalloc(size_t n) { ck_xtmalloc_live++; return malloc(n); }
static void XtFree(void *p) { if (p) { ck_xtmalloc_live--; free(p); } }
static Widget XtParent(Widget w) { return w; }
static int tt_mark(void) { return 0; }
static void tt_release(int mark) { (void)mark; }
static char *tt_message_op(Tt_message m) { (void)m; return "Saved"; }
static Tt_state tt_message_state(Tt_message m) { (void)m; return 0; }
static void tt_message_destroy(Tt_message m) { (void)m; }
static char *tt_message_arg_val(Tt_message m, int n) { (void)m; (void)n; return ck_msgid; }
static Tt_status tt_message_arg_bval(Tt_message m, int n, unsigned char **b, int *l)
{
    (void)m; (void)n;
    *b = ck_buf;
    *l = ck_blen;
    return 0;
}
static IconData *GetIconDataFromMsgID(char *id) { (void)id; return &ck_icon; }
static void TurnOffHourGlassAllWindows(void) {}
static void XmeFlushIconFileCache(void *p) { (void)p; }
static void display_error_message(Widget w, char *msg)
{
    (void)w;
    ck_error_calls++;
    snprintf(ck_error_text, sizeof ck_error_text, "%s", msg);
}
#define SET_ICONGADGET_ICON(w, f) do { ck_icon_set_calls++; \
    snprintf(ck_icon_set_name, sizeof ck_icon_set_name, "%s", f); } while (0)

/* @@CREATE@@ */

/* @@BODY@@ */

static char ck_work[PATH_MAX];		/* the test's own directory */

static int ck_rm(const char *p, const struct stat *sb, int flag, struct FTW *w)
{
    (void)sb; (void)flag; (void)w;
    return remove(p);
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

#define VICTIM_TEXT "VICTIM-DATA\n"

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

static void ck_reset(void)
{
    ck_mk_fail = 0;
    ck_mk_calls = 0;
    ck_icon_set_calls = 0;
    ck_error_calls = 0;
    ck_error_text[0] = '\0';
}

static void ck_saved(const char *data)
{
    ck_buf = (unsigned char *)data;
    ck_blen = (int)strlen(data);
    assert(IconEdit_tt_handler(1, 0) == TT_CALLBACK_PROCESSED);
}

/* The file is ours: a plain file, mode 0600, one link, with this content. */
static void ck_check_file(const char *path, const char *text)
{
    struct stat st;
    assert(lstat(path, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 07777) == 0600);
    assert(st.st_nlink == 1 && st.st_uid == geteuid());
    assert(strcmp(ck_slurp(path), text) == 0);
}

static int ck_next_fd(void)
{
    int fd = dup(0);
    assert(fd >= 0);
    close(fd);
    return fd;
}

int main(int argc, char **argv)
{
    static const char pm1[] = "/* XPM */\nstatic char *a[] = {\"1 1 1 1\", \"a c red\", \"a\"};";
    static const char pm2[] = "/* XPM */\nstatic char *b[] = {\"b\"};";
    static const char bm[] = "#define a_width 1\n{0x01};{0x02};";
    char link[PATH_MAX], victim[PATH_MAX], first[PATH_MAX], mask[PATH_MAX];
    struct stat st;
    int before, fd0;

    assert(argc == 2);
    snprintf(ck_work, sizeof ck_work, "%s/w-XXXXXX", argv[1]);
    assert(mkdtemp(ck_work));
    snprintf(ck_tmpdir, sizeof ck_tmpdir, "%s", ck_work);
    ck_icon.pmMsgID = "MSG-1";
    ck_icon.bmMsgID = "MSG-2";

    /*
     * (b) A pixmap saved by the icon editor, a link planted at the first
     * name: the data goes to another file and the target is untouched.
     */
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_write_file(victim, VICTIM_TEXT, 0644);
    snprintf(link, sizeof link, "%s/AAAAAA", ck_work);
    assert(symlink(victim, link) == 0);
    ck_reset();
    ck_msgid = "MSG-1";
    ck_saved(pm1);
    assert(stat(victim, &st) == 0 && (st.st_mode & 07777) == 0644);
    assert(strcmp(ck_slurp(victim), VICTIM_TEXT) == 0);
    assert(strcmp(ck_icon.pmFileName, link) != 0);

    /* (a) The file exists, ours, mode 0600, with the XPM data. */
    assert(strncmp(ck_icon.pmFileName, ck_work, strlen(ck_work)) == 0);
    ck_check_file(ck_icon.pmFileName, pm1);
    assert(ck_icon.pmDirtyBit && !ck_icon.bmDirtyBit);
    assert(ck_error_calls == 0 && ck_icon_set_calls == 1);
    assert(strcmp(ck_icon_set_name, ck_icon.pmFileName) == 0);
    snprintf(first, sizeof first, "%s", ck_icon.pmFileName);

    /* Saved again: the same file is reopened and truncated. */
    ck_reset();
    ck_saved(pm2);
    assert(strcmp(ck_icon.pmFileName, first) == 0);
    ck_check_file(first, pm2);

    /* A bitmap with a mask: the bitmap file and "<name>_m". */
    ck_reset();
    ck_msgid = "MSG-2";
    ck_saved(bm);
    assert(strncmp(ck_icon.bmFileName, ck_work, strlen(ck_work)) == 0);
    assert(strcmp(ck_icon.bmFileName, first) != 0);
    assert(strcmp(ck_icon.bmFileName, link) != 0);
    ck_check_file(ck_icon.bmFileName, "#define a_width 1\n{0x01};");
    snprintf(mask, sizeof mask, "%s_m", ck_icon.bmFileName);
    assert(lstat(mask, &st) == 0 && S_ISREG(st.st_mode));
    assert(strcmp(ck_slurp(mask), "{0x02};") == 0);
    assert(ck_icon.bmDirtyBit && ck_error_calls == 0);
    unlink(mask);
    unlink(ck_icon.bmFileName);
    unlink(first);
    unlink(link);
    unlink(victim);

    /*
     * (c) mkstemp fails: the user is told, no file is made, the name stays
     * empty, the icon is not marked changed, and nothing is leaked.
     */
    ck_reset();
    memset(&ck_icon, 0, sizeof ck_icon);
    ck_icon.pmMsgID = "MSG-1";
    ck_icon.bmMsgID = "MSG-2";
    ck_msgid = "MSG-1";
    before = ck_count(ck_work);
    fd0 = ck_next_fd();
    ck_mk_fail = 1;
    ck_buf = (unsigned char *)pm1;
    ck_blen = (int)strlen(pm1);
    assert(IconEdit_tt_handler(1, 0) == TT_CALLBACK_CONTINUE);
    ck_mk_fail = 0;
    assert(ck_error_calls == 1 && strstr(ck_error_text, ck_work) != nullptr);
    assert(ck_icon.pmFileName[0] == '\0' && !ck_icon.pmDirtyBit);
    assert(ck_icon_set_calls == 0 && ck_count(ck_work) == before);
    assert(ck_xtmalloc_live == 0 && ck_next_fd() == fd0);

    puts("dtcreate icon temp files: made by mkstemp, planted link unused, failure handled");
    nftw(ck_work, ck_rm, 16, FTW_DEPTH | FTW_PHYS);
    return 0;
}
