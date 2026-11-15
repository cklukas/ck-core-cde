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
 * What the unfixed code used: mktemp() fills in a name and creates nothing.
 * The stand-in gives the first name, ...AAAAAA, that an attacker who can
 * predict the name plants a link at.
 */
static char *ck_mktemp(char *t)
{
    size_t n = strlen(t);
    assert(n >= 6 && strcmp(t + n - 6, "XXXXXX") == 0);
    memcpy(t + n - 6, "AAAAAA", 6);
    return t;
}
#define mktemp ck_mktemp

/* fdopen fails on request, as it does when memory is short. */
static int ck_fdopen_fail;
static FILE *ck_fdopen(int fd, const char *mode)
{
    if (ck_fdopen_fail) {
        errno = ENOMEM;
        return nullptr;
    }
    return fdopen(fd, mode);
}
#define fdopen ck_fdopen

/* The code under test names its files in /tmp; the test runs them in its
 * own directory instead (the working directory). */
#define CK_TMP_LITERAL "./cmXXXXXX"

/* ---- stand-ins for the toolkit types the extracted code uses ---- */
typedef void *XtPointer;
typedef struct ck_widget *Widget;
typedef struct ck_display Display;
typedef int Boolean;
#define True 1
#define False 0
static Display *ck_display;
#define XtDisplay(w) (ck_display)
#define XtNewString(s) ((s) ? strdup(s) : nullptr)
#define XtFree(p) free(p)

#define DtDND_SUCCESS 0
#define DtDND_FAILURE 1
#define DtDND_FILENAME_TRANSFER 1
#define DtDND_BUFFER_TRANSFER 2
typedef struct { char *bp; int size; } DtDndBuffer;
typedef struct {
    int numItems;
    int protocol;
    union {
        char **files;
        DtDndBuffer *buffers;
    } data;
} DtDndDropData;
typedef struct {
    int status;
    DtDndDropData *dropData;
} DtDndDropCallbackStruct;

typedef struct { int DT_catd; } Calendar;
typedef struct { int count; } CmDataList;
enum { buffer_tt, file_tt };
typedef struct {
    char *file;
    int init;
    Calendar *c;
    void *p;
} DTCM_editor;
typedef int Tt_message;
#define TT_DESKTOP_ENODATA 1
#define DATAVER_ARCHIVE 1

/* What the extracted code hands the file to, and what that saw. */
static int ck_use_calls;
static char ck_use_name[PATH_MAX];
static char ck_use_text[4096];
static struct stat ck_use_st;
static int ck_use_stat_ok;
static int ck_fail_calls;

static void ck_use(const char *name)
{
    FILE *f;
    size_t n;

    ck_use_calls++;
    snprintf(ck_use_name, sizeof ck_use_name, "%s", name);
    ck_use_stat_ok = lstat(name, &ck_use_st) == 0;
    ck_use_text[0] = '\0';
    f = fopen(name, "r");
    if (f) {
        n = fread(ck_use_text, 1, sizeof ck_use_text - 1, f);
        ck_use_text[n] = '\0';
        fclose(f);
    }
}

static void tttk_message_fail(Tt_message m, int a, int b, int c)
{
    (void)m; (void)a; (void)b; (void)c;
    ck_fail_calls++;
}
static void query_user(void) {}
static int parse_appt_from_file(int catd, char *filename, CmDataList *list,
                                void *p, void (*q)(void), Calendar *c, int ver)
{
    (void)catd; (void)list; (void)p; (void)q; (void)c; (void)ver;
    ck_use(filename);
    return 0;
}
static void load_from_file(DTCM_editor *de) { ck_use(de->file); }
static int ck_validate_result = 1;
static Boolean validate_dropped_appt(char *filename, Calendar *c)
{
    (void)c;
    ck_use(filename);
    return ck_validate_result;
}

/* load_cb() of dtcm_editor.c: the part that stores a message buffer. */
static int ck_load(DTCM_editor *de, unsigned char *contents, int len, char *file)
{
    Tt_message msg = 0;
    FILE *fp;
    int fd;
    char filename[20];
    CmDataList ck_list, *list = &ck_list;

    (void)fp; (void)fd; (void)filename; (void)list;
    /* @@LOAD_BLOCK@@ */
    return 1;
}

/* handle_drop_cb() of dtcm_editor.c */
/* @@EDITOR_DROP@@ */
#define handle_drop_cb ck_dnd_handle_drop_cb
/* handle_drop_cb() of dnd.c */
/* @@DND_DECL@@ */
/* @@DND_DROP@@ */
#undef handle_drop_cb

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
    assert(chdir(ck_work) == 0);
}

static void ck_remove_work(void)
{
    assert(chdir("/") == 0);
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

#define VICTIM_TEXT "VICTIM-DATA\n"
#define DATA_TEXT "APPOINTMENT\n"
static char ck_victim[PATH_MAX];

/* A file and a symbolic link to it, planted at the first name tried. */
static void ck_plant(void)
{
    snprintf(ck_victim, sizeof ck_victim, "%s/victim", ck_work);
    ck_write_file(ck_victim, VICTIM_TEXT, 0644);
    assert(symlink(ck_victim, "cmAAAAAA") == 0);
}

static void ck_unplant(void)
{
    unlink("cmAAAAAA");
    unlink(ck_victim);
}

/* The planted target must be neither written, truncated nor chmod'ed. */
static void ck_victim_intact(void)
{
    struct stat st;
    assert(stat(ck_victim, &st) == 0);
    assert((st.st_mode & 07777) == 0644);
    assert(strcmp(ck_slurp(ck_victim), VICTIM_TEXT) == 0);
}

static int ck_count(void)
{
    DIR *d = opendir(".");
    struct dirent *e;
    int n = 0;
    assert(d);
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            n++;
    closedir(d);
    return n;
}

/* The lowest free descriptor number: equal before and after means no leak. */
static int ck_next_fd(void)
{
    int fd = dup(0);
    assert(fd >= 0);
    close(fd);
    return fd;
}

static void ck_reset(void)
{
    ck_use_calls = ck_fail_calls = ck_mk_calls = 0;
    ck_use_name[0] = ck_use_text[0] = '\0';
    ck_use_stat_ok = 0;
    ck_mk_fail = ck_fdopen_fail = 0;
}

/* What the file looked like when the consumer got it: ours, mode 0600. */
static void ck_used_file_ok(const char *text)
{
    assert(ck_use_calls == 1 && ck_use_stat_ok);
    assert(S_ISREG(ck_use_st.st_mode));
    assert((ck_use_st.st_mode & 07777) == 0600);
    assert(ck_use_st.st_nlink == 1);
    assert(ck_use_st.st_uid == geteuid());
    assert(strcmp(ck_use_text, text) == 0);
}

static int ck_drop(DTCM_editor *de, Calendar *c, int editor, const char *text)
{
    DtDndDropCallbackStruct info;
    DtDndDropData data;
    static DtDndBuffer buf[1];

    buf[0].bp = (char *)text;
    buf[0].size = (int)strlen(text);
    data.numItems = 1;
    data.protocol = DtDND_BUFFER_TRANSFER;
    data.data.buffers = buf;
    info.dropData = &data;
    info.status = -1;
    if (editor)
        handle_drop_cb((Widget)0, (XtPointer)de, (XtPointer)&info);
    else
        ck_dnd_handle_drop_cb((Widget)0, (XtPointer)c, (XtPointer)&info);
    return info.status;
}

int main(int argc, char **argv)
{
    Calendar cal = { 1 };
    DTCM_editor de = { 0, 0, &cal, 0 };
    int fd0, before;

    assert(argc == 2);
    ck_make_work(argv[1]);

    /*
     * (b) load_cb, message buffer: a link planted at the first name is not
     * written through, and the parser gets a file of ours.
     */
    ck_reset();
    ck_plant();
    before = ck_count();
    assert(ck_load(&de, (unsigned char *)DATA_TEXT, (int)strlen(DATA_TEXT), nullptr) == 1);
    ck_victim_intact();
    assert(strcmp(ck_use_name, "./cmAAAAAA") != 0);
    ck_used_file_ok(DATA_TEXT);
    assert(ck_count() == before);		/* the temp file was removed */
    ck_unplant();

    /* (a) with no link planted: first name, created 0600, removed after. */
    ck_reset();
    assert(ck_load(&de, (unsigned char *)DATA_TEXT, (int)strlen(DATA_TEXT), nullptr) == 1);
    assert(strcmp(ck_use_name, "./cmAAAAAA") == 0);
    ck_used_file_ok(DATA_TEXT);
    assert(ck_count() == 0);

    /* A message with a file name does not touch temp files. */
    ck_reset();
    assert(ck_load(&de, nullptr, 0, "somefile") == 1);
    assert(ck_mk_calls == 0 && ck_use_calls == 1 && strcmp(ck_use_name, "somefile") == 0);
    free(de.file);
    de.file = nullptr;

    /* (c) mkstemp fails: the request fails, nothing is parsed or left. */
    ck_reset();
    ck_mk_fail = 1;
    assert(ck_load(&de, (unsigned char *)DATA_TEXT, (int)strlen(DATA_TEXT), nullptr) == 0);
    assert(ck_fail_calls == 1 && ck_use_calls == 0 && ck_count() == 0);

    /* (c) fdopen fails: same, and the descriptor and file are given up. */
    ck_reset();
    fd0 = ck_next_fd();
    ck_fdopen_fail = 1;
    assert(ck_load(&de, (unsigned char *)DATA_TEXT, (int)strlen(DATA_TEXT), nullptr) == 0);
    assert(ck_fail_calls == 1 && ck_use_calls == 0 && ck_count() == 0);
    assert(ck_next_fd() == fd0);

    /* The same three cases for the drop handler of the editor. */
    ck_reset();
    ck_plant();
    before = ck_count();
    assert(ck_drop(&de, &cal, 1, DATA_TEXT) == DtDND_SUCCESS);
    ck_victim_intact();
    assert(strcmp(ck_use_name, "./cmAAAAAA") != 0);
    ck_used_file_ok(DATA_TEXT);
    assert(ck_count() == before);
    free(de.file);
    de.file = nullptr;
    ck_unplant();

    ck_reset();
    ck_mk_fail = 1;
    assert(ck_drop(&de, &cal, 1, DATA_TEXT) == DtDND_FAILURE);
    assert(ck_use_calls == 0 && ck_count() == 0);

    ck_reset();
    fd0 = ck_next_fd();
    ck_fdopen_fail = 1;
    assert(ck_drop(&de, &cal, 1, DATA_TEXT) == DtDND_FAILURE);
    assert(ck_use_calls == 0 && ck_count() == 0);
    assert(ck_next_fd() == fd0);

    /*
     * The drop handler of the calendar keeps the file for the animate
     * callback: planted link not followed, file ours, name remembered.
     */
    ck_reset();
    ck_plant();
    assert(ck_drop(&de, &cal, 0, DATA_TEXT) == DtDND_SUCCESS);
    ck_victim_intact();
    assert(strcmp(dnd_filename, "./cmAAAAAA") != 0);
    ck_used_file_ok(DATA_TEXT);
    assert(strcmp(ck_use_name, dnd_filename) == 0);
    assert(strcmp(ck_slurp(dnd_filename), DATA_TEXT) == 0);
    unlink(dnd_filename);
    dnd_filename[0] = '\0';
    ck_unplant();

    /* A rejected appointment is removed and forgotten. */
    ck_reset();
    ck_validate_result = 0;
    assert(ck_drop(&de, &cal, 0, DATA_TEXT) == DtDND_FAILURE);
    assert(ck_use_calls == 1 && dnd_filename[0] == '\0' && ck_count() == 0);
    ck_validate_result = 1;

    ck_reset();
    ck_mk_fail = 1;
    assert(ck_drop(&de, &cal, 0, DATA_TEXT) == DtDND_FAILURE);
    assert(ck_use_calls == 0 && dnd_filename[0] == '\0' && ck_count() == 0);

    ck_reset();
    fd0 = ck_next_fd();
    ck_fdopen_fail = 1;
    assert(ck_drop(&de, &cal, 0, DATA_TEXT) == DtDND_FAILURE);
    assert(ck_use_calls == 0 && dnd_filename[0] == '\0' && ck_count() == 0);
    assert(ck_next_fd() == fd0);

    puts("dtcm temp files: made by mkstemp, planted link unused, failures handled");
    ck_remove_work();
    return 0;
}
