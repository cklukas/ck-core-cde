/*
 * Regression harness for dtstyle input-path, palette, and I18n hardening.
 * Extracted production functions from ColorFile.c, ColorMain.c, I18nEnv.c,
 * and I18nMain.c are spliced at the @@EXTRACTED@@ marker.
 */
/* @@PROLOGUE@@ */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int Boolean;
typedef void *Widget;
typedef void *XtPointer;
typedef unsigned long XtInputId;
typedef unsigned long XtIntervalId;
typedef void *XtAppContext;
typedef void *Display;
typedef unsigned long Colormap;
typedef unsigned long XrmDatabase;

#define True 1
#define False 0
#define TRUE 1
#define FALSE 0

#define XmCO_MAX_NUM_COLORS 8
#define XmCO_BLACK_WHITE 0
#define XmCO_LOW_COLOR 1
#define XmCO_MEDIUM_COLOR 2
#define XmCO_HIGH_COLOR 3

#define PALETTE_SUFFIX ".dp"
#define DEFAULT_PALETTE "Default.dp"
#define XtInputReadMask 1

#define NoError 0
#define ErrNoHome 1
#define ErrNoLocale 2
#define ErrNoCDELocale 3
#define ErrFileCreate 4
#define ErrNoSelectionFile 5
#define ErrSaveSelection 6
#define ErrUnknownHost 7
#define ErrNoDtimsstart 8
#define ErrRemoteFailed 9
#define ErrNoPopen 10
#define ErrTimeOut 11

#define GETMESSAGE(set, num, dflt) (dflt)

typedef struct {
    unsigned short red;
    unsigned short green;
    unsigned short blue;
    unsigned long pixel;
    char flags;
    char pad;
} XColor;

typedef struct {
    XColor bg;
    XColor fg;
    XColor ts;
    XColor bs;
    XColor sc;
} ColorSet;

typedef struct _palette {
    char *name;
    char *desc;
    char *directory;
    int item_position;
    int num_of_colors;
    int primary;
    int secondary;
    int active;
    int inactive;
    ColorSet color[XmCO_MAX_NUM_COLORS];
    struct _palette *next;
} palette;

typedef void (*ColorCalcProc)(XColor *, XColor *, XColor *, XColor *, XColor *);

static struct {
    Display display;
    Colormap colormap;
    int screenNum;
    Widget i18nDialog;
    struct {
        int pipeTimeOut;
    } xrdb;
} style;

static struct {
    ColorCalcProc calcRGB;
} edit;

static palette *pHeadPalette = nullptr;
static int NumOfPalettes = 0;
static int TypeOfMonitor = XmCO_HIGH_COLOR;
static char *defaultName = nullptr;

typedef struct {
    char *im_name;
    char *im_label;
    Boolean inactive;
    Boolean im_default;
} ImsEnt;

typedef struct {
    int status;
    ImsEnt *ims_list;
    int ims_list_size;
    char *host_name;
} ImsSel;

typedef struct {
    FILE *pipe;
    XtInputId input_id;
    XtIntervalId timer_id;
} PipeRec;

typedef struct {
    Widget shell;
    ImsSel *ims_sel;
    PipeRec *pipe_info;
} I18nEnv;

typedef struct {
    unsigned int size;
    char *addr;
} XrmValue;

static char *
XtMalloc(size_t size)
{
    return (char *)malloc(size ? size : 1);
}

static char *
XtCalloc(size_t num, size_t size)
{
    return (char *)calloc(num ? num : 1, size ? size : 1);
}

static char *
XtRealloc(char *ptr, size_t size)
{
    return (char *)realloc(ptr, size ? size : 1);
}

static void
XtFree(char *ptr)
{
    free(ptr);
}

static char *
XtNewString(const char *str)
{
    if (!str)
        return nullptr;
    size_t len = strlen(str) + 1;
    char *copy = XtMalloc(len);
    memcpy(copy, str, len);
    return copy;
}

static int
XParseColor(Display dpy, Colormap cmap, const char *spec, XColor *out)
{
    (void)dpy;
    (void)cmap;
    if (!spec || spec[0] != '#')
        return 0;
    memset(out, 0, sizeof(*out));
    out->red = 0x1111;
    out->green = 0x2222;
    out->blue = 0x3333;
    return 1;
}

static void
stub_calc_rgb(XColor *bg, XColor *fg, XColor *sc, XColor *ts, XColor *bs)
{
    *fg = *bg;
    *sc = *bg;
    *ts = *bg;
    *bs = *bg;
}

static ColorCalcProc
XmGetColorCalculation(void)
{
    return stub_calc_rgb;
}

static void
RGBtoHSV(unsigned short r, unsigned short g, unsigned short b,
         int *h, int *s, int *v)
{
    (void)r;
    (void)g;
    (void)b;
    *h = *s = *v = 0;
}

static void
SetColors(int num_of_colors, palette *new_palette)
{
    (void)num_of_colors;
    (void)new_palette;
}

static void
InitializeBW(unsigned long color, int num_of_colors, palette *new_palette)
{
    (void)color;
    (void)num_of_colors;
    (void)new_palette;
}

static void
SwitchAItoPS(palette *new_palette)
{
    (void)new_palette;
}

static XrmDatabase
XtDatabase(Display dpy)
{
    (void)dpy;
    return 0;
}

static Boolean
XrmGetResource(XrmDatabase db, const char *name, const char *cls,
               char **type_ret, XrmValue *val_ret)
{
    (void)db;
    (void)name;
    (void)cls;
    (void)type_ret;
    (void)val_ret;
    return False;
}

static int ck_stat_override = 0;
static int
ck_stat(const char *path, struct stat *st)
{
    (void)path;
    memset(st, 0, sizeof(*st));
    return ck_stat_override;
}
#define stat(path, buf) ck_stat(path, buf)

static int ck_popen_called = 0;
static FILE *
ck_popen(const char *cmd, const char *mode)
{
    (void)cmd;
    (void)mode;
    ck_popen_called++;
    return nullptr;
}
#define popen(cmd, mode) ck_popen(cmd, mode)

static int
ck_pclose(FILE *fp)
{
    (void)fp;
    return 0;
}
#define pclose(fp) ck_pclose(fp)

static void
_DtI18nSetSensitiveImTB(I18nEnv *env, Boolean sensitivity)
{
    (void)env;
    (void)sensitivity;
}

static XtAppContext
XtWidgetToApplicationContext(Widget w)
{
    (void)w;
    return nullptr;
}

static XtInputId
XtAppAddInput(XtAppContext app, int fd, XtPointer mask,
              void (*proc)(XtPointer, int *, XtInputId *), XtPointer closure)
{
    (void)app;
    (void)fd;
    (void)mask;
    (void)proc;
    (void)closure;
    return 1;
}

static XtIntervalId
XtAppAddTimeOut(XtAppContext app, unsigned long interval,
                void (*proc)(XtPointer, XtIntervalId *), XtPointer closure)
{
    (void)app;
    (void)interval;
    (void)proc;
    (void)closure;
    return 2;
}

static void
XtRemoveInput(XtInputId id)
{
    (void)id;
}

static void
XtRemoveTimeOut(XtIntervalId id)
{
    (void)id;
}

static const char *last_err_dialog_msg = nullptr;
static void
ErrDialog(char *err_str, Widget parent)
{
    (void)parent;
    last_err_dialog_msg = err_str;
}

void _DtI18nErrorDialog(int err);
static int ProcessBuf(char *savebuf, I18nEnv *env);

/* @@EXTRACTED@@ */

int
main(void)
{
    /* 1. RemovePaletteFromList when deleting the sole remaining palette */
    pHeadPalette = (palette *)XtCalloc(1, sizeof(palette));
    pHeadPalette->name = XtNewString("SolePalette");
    pHeadPalette->directory = XtNewString("/var/empty");
    pHeadPalette->item_position = 1;
    pHeadPalette->next = nullptr;
    NumOfPalettes = 1;

    palette *after = RemovePaletteFromList(pHeadPalette);
    if (after != nullptr || pHeadPalette != nullptr || NumOfPalettes != 0) {
        fprintf(stderr, "FAIL: RemovePaletteFromList sole palette failed\n");
        return 1;
    }

    /* 2. CheckForDeletedFile when deleting the sole palette & short d_name */
    pHeadPalette = (palette *)XtCalloc(1, sizeof(palette));
    pHeadPalette->name = XtNewString(" Crimson");
    XtFree(pHeadPalette->name);
    pHeadPalette->name = XtNewString("Crimson");
    pHeadPalette->directory = XtNewString("/var/empty");
    pHeadPalette->item_position = 1;
    pHeadPalette->next = nullptr;
    NumOfPalettes = 1;

    struct dirent de;
    memset(&de, 0, sizeof(de));
    strcpy(de.d_name, "~Crimson.dp");
    CheckForDeletedFile(&de);
    if (pHeadPalette != nullptr || NumOfPalettes != 0) {
        fprintf(stderr, "FAIL: CheckForDeletedFile sole palette failed\n");
        return 1;
    }

    /* 3. ReadInInfo on non-NUL-terminated exact-sized buffer & overlong line */
    const char *raw_line =
        "#111122223333:extra_colon_comment_exceeding_fifty_bytes_on_single_line_"
        "01234567890123456789\n"
        "#444455556666_no_trailing_newline_and_over_fifty_chars_"
        "01234567890123456789";
    size_t raw_len = strlen(raw_line);
    char *exact_buf = (char *)malloc(raw_len);
    memcpy(exact_buf, raw_line, raw_len);
    palette pal;
    memset(&pal, 0, sizeof(pal));
    ReadInInfo(exact_buf, (int)raw_len, &pal);
    free(exact_buf);
    if (pal.num_of_colors < 2) {
        fprintf(stderr, "FAIL: ReadInInfo parsed %d colors\n", pal.num_of_colors);
        return 1;
    }

    /* 4. GetDefaultPal fallback allocation must include NUL terminator */
    defaultName = nullptr;
    GetDefaultPal(nullptr);
    if (!defaultName || strcmp(defaultName, DEFAULT_PALETTE) != 0) {
        fprintf(stderr, "FAIL: GetDefaultPal failed\n");
        return 1;
    }
    XtFree(defaultName);
    defaultName = nullptr;

    /* 5. _DtI18nErrorDialog(ErrRemoteFailed) must not fall through to ErrNoPopen */
    last_err_dialog_msg = nullptr;
    _DtI18nErrorDialog(ErrRemoteFailed);
    if (!last_err_dialog_msg ||
        strstr(last_err_dialog_msg, "Remote execution failed") == nullptr) {
        fprintf(stderr, "FAIL: _DtI18nErrorDialog(ErrRemoteFailed) fell through: %s\n",
                last_err_dialog_msg ? last_err_dialog_msg : "(null)");
        return 1;
    }

    /* 6. ReadPipe + ProcessBuf with pipe input and malformed line without newline */
    int pfd[2];
    if (pipe(pfd) != 0) {
        perror("pipe");
        return 1;
    }
    const char *ims_payload = "#xim XIM\n(kinput2) Kinput2\nmalformed_no_space_or_nl";
    if (write(pfd[1], ims_payload, strlen(ims_payload)) != (ssize_t)strlen(ims_payload)) {
        perror("write");
        return 1;
    }
    close(pfd[1]);

    ImsSel ims_sel;
    memset(&ims_sel, 0, sizeof(ims_sel));
    PipeRec pipe_rec;
    memset(&pipe_rec, 0, sizeof(pipe_rec));
    I18nEnv env;
    memset(&env, 0, sizeof(env));
    env.ims_sel = &ims_sel;
    env.pipe_info = &pipe_rec;
    XtInputId input_id = 1;

    /* First call reads payload; second call reads EOF (0 bytes) and parses */
    ReadPipe((XtPointer)&env, &pfd[0], &input_id);
    ReadPipe((XtPointer)&env, &pfd[0], &input_id);
    close(pfd[0]);

    if (ims_sel.ims_list_size != 3 ||
        strcmp(ims_sel.ims_list[0].im_name, "xim") != 0 ||
        strcmp(ims_sel.ims_list[0].im_label, "XIM") != 0 ||
        strcmp(ims_sel.ims_list[1].im_name, "kinput2") != 0 ||
        strcmp(ims_sel.ims_list[1].im_label, "Kinput2") != 0) {
        fprintf(stderr, "FAIL: ReadPipe/ProcessBuf unexpected list size=%d\n",
                ims_sel.ims_list_size);
        return 1;
    }
    for (int i = 0; i < ims_sel.ims_list_size; i++) {
        XtFree(ims_sel.ims_list[i].im_name);
        XtFree(ims_sel.ims_list[i].im_label);
    }
    XtFree((char *)ims_sel.ims_list);

    /* 7. GetImsList rejects shell metacharacters in hostname before popen */
    ck_stat_override = NoError;
    ck_popen_called = 0;
    int ims_ret = GetImsList(&env, "badhost;echo_injected");
    if (ims_ret != ErrUnknownHost || ck_popen_called != 0) {
        fprintf(stderr, "FAIL: GetImsList allowed unsafe hostname (ret=%d, popen=%d)\n",
                ims_ret, ck_popen_called);
        return 1;
    }

    return 0;
}
