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

/* ---- stand-ins for the ToolTalk calls and the globals of main.c ---- */
typedef int Tt_message;
typedef int Tttk_op;
typedef int Tt_status;
typedef int Boolean;
#define True 1
#define False 0
#define MAX_FNAME 256
#define NUM_PARAMS 8
#define AUTO_FILE 0
#define TT_OK 0
#define TT_WRN_START_MESSAGE 1
#define TTME_COMPOSE 1
#define TTME_DISPLAY 2
#define TTME_EDIT 3
#define dticon_ptype "DT_Icon_Editor"

int tt_tmpfile_fd = -1;
static int undeclared = 0;
char msgID[120];
char start_file[MAX_FNAME];
Boolean param_flag[NUM_PARAMS];
Boolean argsNeedProcessed;

static int ck_message_status = TT_WRN_START_MESSAGE;
static unsigned char *ck_buf;
static int ck_blen;
static int ck_fail_calls, ck_clear_calls, ck_reject_calls;

static int tt_mark(void) { return 0; }
static void tt_release(int mark) { (void)mark; }
static int tt_message_status(Tt_message m) { (void)m; return ck_message_status; }
static void tt_message_reply(Tt_message m) { (void)m; }
static void tt_ptype_undeclare(const char *p) { (void)p; }
static char *tt_message_arg_val(Tt_message m, int n) { (void)m; (void)n; return "MSG-1"; }
static Tt_status tt_message_arg_bval(Tt_message m, int n, unsigned char **b, int *l)
{
    (void)m; (void)n;
    *b = ck_buf;
    *l = ck_blen;
    return TT_OK;
}
static void tt_message_fail(Tt_message m) { (void)m; ck_fail_calls++; }
static void tt_message_reject(Tt_message m) { (void)m; ck_reject_calls++; }
static int edit_notifier(char *fname, Tt_message msg, int clear)
{
    (void)fname; (void)msg;
    if (clear)
        ck_clear_calls++;
    return 1;
}

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
    ck_fail_calls = ck_clear_calls = ck_reject_calls = 0;
    ck_mk_fail = 0;
    ck_mk_calls = 0;
    start_file[0] = '\0';
    tt_tmpfile_fd = -1;
    argsNeedProcessed = False;
    param_flag[AUTO_FILE] = False;
}

static void ck_request(const char *data, int len)
{
    ck_buf = (unsigned char *)data;
    ck_blen = len;
    assert(ProcessToolTalkMediaMessage(1, nullptr, TTME_EDIT, TT_OK, nullptr, 0, nullptr, nullptr) == 0);
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

int main(int argc, char **argv)
{
    static const char xpm[] = "/* XPM */\nstatic char *icon[] = {\"1 1 1 1\", \"a c red\", \"a\"};";
    static const char xbm[] = "BITMAP;\nMASK;";
    char link[PATH_MAX], victim[PATH_MAX], first[PATH_MAX], mask[PATH_MAX];
    struct stat st;
    int before;

    assert(argc == 2);
    snprintf(ck_work, sizeof ck_work, "%s/w-XXXXXX", argv[1]);
    assert(mkdtemp(ck_work));
    snprintf(ck_tmpdir, sizeof ck_tmpdir, "%s", ck_work);

    /*
     * (b) Icon data in a message buffer, a link planted at the first name:
     * the data goes to another file and the target is untouched.
     */
    snprintf(victim, sizeof victim, "%s/victim", ck_work);
    ck_write_file(victim, VICTIM_TEXT, 0644);
    snprintf(link, sizeof link, "%s/AAAAAA", ck_work);
    assert(symlink(victim, link) == 0);
    ck_reset();
    ck_request(xpm, (int)strlen(xpm));
    assert(stat(victim, &st) == 0 && (st.st_mode & 07777) == 0644);
    assert(strcmp(ck_slurp(victim), VICTIM_TEXT) == 0);
    assert(strcmp(start_file, link) != 0);

    /* (a) The data file exists, ours, mode 0600, with the XPM data. */
    assert(strncmp(start_file, ck_work, strlen(ck_work)) == 0);
    ck_check_file(start_file, xpm);
    assert(param_flag[AUTO_FILE] && argsNeedProcessed);
    assert(ck_fail_calls == 0 && ck_clear_calls == 0);
    assert(fcntl(tt_tmpfile_fd, F_GETFD) == -1);	/* descriptor closed */
    unlink(start_file);

    /* An XBM buffer with a mask: the bitmap file and "<name>_m". */
    ck_reset();
    ck_request(xbm, (int)strlen(xbm));
    ck_check_file(start_file, "BITMAP;");
    snprintf(mask, sizeof mask, "%s_m", start_file);
    assert(lstat(mask, &st) == 0 && S_ISREG(st.st_mode));
    assert(strcmp(ck_slurp(mask), "MASK;") == 0);
    assert(param_flag[AUTO_FILE] && argsNeedProcessed);
    unlink(mask);
    unlink(start_file);

    /* Icon data in a file: no temp file is made. */
    ck_reset();
    ck_buf = nullptr;
    ck_blen = 0;
    assert(ProcessToolTalkMediaMessage(1, nullptr, TTME_EDIT, TT_OK, nullptr, 0, "icon.pm", nullptr) == 0);
    assert(ck_mk_calls == 0 && strcmp(start_file, "icon.pm") == 0);

    /*
     * (c) mkstemp fails: the request is failed and cleared, no file is made,
     * start_file stays empty and nothing is processed.
     */
    unlink(link);
    unlink(victim);
    ck_reset();
    before = ck_count(ck_work);
    ck_mk_fail = 1;
    ck_request(xpm, (int)strlen(xpm));
    ck_mk_fail = 0;
    assert(ck_fail_calls == 1 && ck_clear_calls == 1);
    assert(start_file[0] == '\0' && tt_tmpfile_fd == -1);
    assert(!argsNeedProcessed && !param_flag[AUTO_FILE]);
    assert(ck_count(ck_work) == before);

    puts("dticon icon data file: made by mkstemp, planted link unused, failure handled");
    nftw(ck_work, ck_rm, 16, FTW_DEPTH | FTW_PHYS);
    return 0;
}
