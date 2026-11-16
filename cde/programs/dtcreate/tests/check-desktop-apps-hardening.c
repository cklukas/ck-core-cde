/*
 * Sanitizer regression harness for desktop apps hardening fixes:
 *  - dtcreate/FileCharacteristics.c: ParseAndUpdateID 1-byte heap overflow
 *  - dtcreate/parser.c: ProcessContents stack buffer overflow on buf[10]
 *  - dtpad/fileIo.c: AddPound 1-byte buffer overflow on 512-byte returnBuf
 *  - dtpad/fileDlg.c: ExtractAndStoreDir buffer overflow on pathDir
 *  - dticon/main.c: send_tt_saved 1-byte heap overflow on buffer[len + 1]
 *  - dtcalc/calctool.c: process_item out-of-bounds access on buttons[n]
 *    when n < 0 || n >= TITEMS
 *  - dtimsstart/env.c: expand_string bounds check on ~/ expansion
 */
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef True
#define True 1
#endif
#ifndef False
#define False 0
#endif

typedef int Boolean;
typedef void *Widget;

static void *
XtMalloc(size_t sz)
{
    return malloc(sz);
}

static void
XtFree(void *p)
{
    free(p);
}

static char *
XtNewString(const char *s)
{
    return s ? strdup(s) : nullptr;
}

#define GETMESSAGE(set, num, dflt) (dflt)

/* --- dtcreate stubs --- */
#define CA_FT_CNTBYTE   0x01
#define CA_FT_CNTSTRING 0x02
#define CA_FT_CNTLONG   0x04
#define CA_FT_CNTSHORT  0x08

typedef struct {
    char *pszPattern;
    char *pszPermissions;
    char *pszContents;
    int sStart;
    unsigned int fsFlags;
} FiletypeData;

static Widget AF_IdCharacteristicsText = nullptr;
static char last_xm_text[1024];

static void
XmTextSetString(Widget w, char *str)
{
    (void)w;
    snprintf(last_xm_text, sizeof(last_xm_text), "%s", str ? str : "");
}

/* --- dtpad stubs --- */
#define OPEN 1
#define INCLUDE 2

typedef struct {
    struct {
        char pathDir[64];
        char includePathDir[64];
        char *lastIncludeName;
    } fileStuff;
} Editor;

static char *
MbStrchr(char *s, int c)
{
    return strchr(s, c);
}

static char *
MbStrrchr(char *s, int c)
{
    return strrchr(s, c);
}

/* --- dticon stubs --- */
typedef int Tt_status;
typedef void *Tt_message;
#define TT_HANDLER 1
#define TT_NOTICE 2
#define TT_SESSION 3
#define TT_IN 4
#define FORMAT_XPM 0
#define FORMAT_XBM 1

static int fileFormat = FORMAT_XPM;
static char last_fname[1024];
static char dummy[1028];
static char msgID[120] = "msg-1";
static Tt_message local_msg = nullptr;
static size_t last_barg_len = 0;

static int tt_mark(void) { return 1; }
static void tt_release(int m) { (void)m; }
static Tt_message tt_message_create(void) { return (Tt_message)0x1; }
static void tt_message_address_set(Tt_message m, int a) { (void)m; (void)a; }
static char *tt_message_sender(Tt_message m) { (void)m; return "sender"; }
static void tt_message_handler_set(Tt_message m, char *h) { (void)m; (void)h; }
static void tt_message_class_set(Tt_message m, int c) { (void)m; (void)c; }
static void tt_message_scope_set(Tt_message m, int s) { (void)m; (void)s; }
static char *tt_default_session(void) { return "sess"; }
static void tt_message_session_set(Tt_message m, char *s) { (void)m; (void)s; }
static void tt_message_op_set(Tt_message m, const char *op) { (void)m; (void)op; }
static char *tt_message_arg_type(Tt_message m, int i) { (void)m; (void)i; return "XPM"; }
static void tt_message_barg_add(Tt_message m, int mode, const char *type, void *buf, int len)
{
    (void)m; (void)mode; (void)type; (void)buf;
    last_barg_len = (size_t)len;
}
static void tt_message_arg_add(Tt_message m, int mode, const char *type, const char *val)
{
    (void)m; (void)mode; (void)type; (void)val;
}
static Tt_status tt_message_send(Tt_message m) { (void)m; return 0; }
static void tt_message_destroy(Tt_message m) { (void)m; }

/* --- dtcalc stubs --- */
#define TITEMS 4
#define MAXVKEYS 2
#define FCP_KEY 1
#define MODEKEYS 4
#define SCIENTIFIC 0
#define OPITEM 1
#define V_CLR 0

enum opdisp_type { OP_NONE = 0, OP_SET = 1, OP_CLEAR = 2 };

struct button {
    char value;
    enum opdisp_type opdisp;
    char *str;
    void (*func)(void);
    char *resname;
};

static int button_called = 0;
static void dummy_btn_func(void) { button_called++; }

static struct button buttons[TITEMS] = {
    { '0', OP_NONE, "0", dummy_btn_func, "zero" },
    { '1', OP_SET,  "1", dummy_btn_func, "one" },
    { '+', OP_SET,  "+", dummy_btn_func, "add" },
    { 'C', OP_CLEAR, "C", dummy_btn_func, "clr" },
};

static struct button mode_buttons[MODEKEYS] = {
    { 'm', OP_NONE, "m", dummy_btn_func, "m0" },
    { 'm', OP_NONE, "m", dummy_btn_func, "m1" },
    { 'm', OP_NONE, "m", dummy_btn_func, "m2" },
    { 'm', OP_NONE, "m", dummy_btn_func, "m3" },
};

static char validkeys[MAXVKEYS] = { 'C', 'q' };
static char *vstrs[1] = { "CLR" };

struct calc_vars {
    int error;
    char current;
    char pending;
    int pending_win;
    int pending_n;
    int pending_mode;
    int modetype;
    int defState;
};

static struct calc_vars calc_v;
static struct calc_vars *v = &calc_v;
static int beep_count = 0;
static void beep(void) { beep_count++; }
static void set_item(int item, char *str) { (void)item; (void)str; }

/* --- dtimsstart stubs --- */
typedef struct {
    char *imsConfDir;
    char *imsAppDir;
    char *imsLogDir;
    char *imsLogFile;
    char *imsDir;
    char *userImsDir;
    char *userTmpDir;
    char *userAltDir;
    struct {
        char *confDir;
        char *userDir;
    } *dt;
} CmdConf;

typedef struct {
    char *real_locale;
    char *locale;
    char *homedir;
    char *username;
    char *hostname;
    char *displayname;
    char *displaydir;
    long screen_num;
} UserEnv;

typedef struct {
    char *servername;
    char *servername2;
    char *classname;
} ImsConf;

static struct {
    char *hostname;
} userSel = { "localhost" };

static UserEnv userEnv;
static CmdConf Conf;

#define DPR(x) ((void)0)
#define DPR2(x) ((void)0)
#define DPR3(x) ((void)0)

static char *
strcpyx(char *dst, const char *src)
{
    while (*src)
        *dst++ = *src++;
    *dst = '\0';
    return dst;
}

/* @@EXTRACTED_FUNCTIONS@@ */

static void
test_dtcreate_parse_and_update_id(void)
{
    FiletypeData ftd;
    memset(&ftd, 0, sizeof(ftd));
    ftd.pszPattern = "*.c";
    ftd.pszPermissions = "d!r";
    ftd.pszContents = "0 string /*";
    /* Previously allocated `size` without +1 for NUL, causing 1-byte heap overflow */
    ParseAndUpdateID(&ftd);
    assert(strstr(last_xm_text, "*.c") != nullptr);
}

static void
test_dtcreate_process_contents_long_token(void)
{
    FiletypeData ftd;
    memset(&ftd, 0, sizeof(ftd));
    /* Token longer than buf[10] previously overflowed stack buffer buf */
    ftd.pszContents = strdup("123456789012345 string hello");
    assert(ProcessContents(&ftd) == 0);
    assert(ftd.pszContents != nullptr);
    assert(strcmp(ftd.pszContents, "hello") == 0);
    free(ftd.pszContents);
}

static void
test_dtpad_add_pound_and_extract_dir(void)
{
    char longName[600];
    memset(longName, 'a', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    char *res = AddPound(longName);
    assert(res != nullptr);
    assert(strlen(res) < 512);

    Editor pad;
    memset(&pad, 0, sizeof(pad));
    char deepPath[200];
    memset(deepPath, 'd', sizeof(deepPath) - 1);
    deepPath[0] = '/';
    deepPath[150] = '/';
    deepPath[sizeof(deepPath) - 1] = '\0';
    ExtractAndStoreDir(&pad, deepPath, OPEN);
    assert(strlen(pad.fileStuff.pathDir) < sizeof(pad.fileStuff.pathDir));
}

static void
test_dticon_send_tt_saved(void)
{
    char tmp_pm[] = "/tmp/dticon-test-XXXXXX";
    int fd = mkstemp(tmp_pm);
    assert(fd >= 0);
    const char *payload = "/* XPM */\nstatic char * icon[] = {};\n";
    assert(write(fd, payload, strlen(payload)) == (ssize_t)strlen(payload));
    close(fd);

    snprintf(last_fname, sizeof(last_fname), "%s", tmp_pm);
    fileFormat = FORMAT_XPM;
    /* Previously wrote buffer[len + 1] = '\0' (1 byte past heap allocation!) */
    send_tt_saved();
    unlink(tmp_pm);
    assert(last_barg_len == strlen(payload) + 1);
}

static void
test_dtcalc_process_item_bounds(void)
{
    memset(&calc_v, 0, sizeof(calc_v));
    beep_count = 0;
    button_called = 0;
    /* Out-of-range index (-1 from get_index on unknown key) must not index buttons[-1] */
    process_item(-1);
    assert(calc_v.error == TRUE);
    assert(beep_count == 1);
    assert(button_called == 0);
}

static void
test_dtimsstart_expand_string_bounds(void)
{
    memset(&userEnv, 0, sizeof(userEnv));
    userEnv.homedir = "/very/long/home/directory/path/that/exceeds/small/out";
    char out[16];
    memset(out, 0, sizeof(out));
    int len = expand_string("~/file", out, (int)sizeof(out), nullptr);
    assert(len >= 0 && len < (int)sizeof(out));
}

int
main(void)
{
    test_dtcreate_parse_and_update_id();
    test_dtcreate_process_contents_long_token();
    test_dtpad_add_pound_and_extract_dir();
    test_dticon_send_tt_saved();
    test_dtcalc_process_item_bounds();
    test_dtimsstart_expand_string_bounds();
    printf("check-desktop-apps-hardening: all tests passed\n");
    return 0;
}
