#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char Boolean;
typedef unsigned int Cardinal;
typedef void *XtPointer;
typedef char *String;
typedef char *caddr_t;
typedef void *Display;
typedef void *Screen;
typedef void *Widget;
typedef unsigned long Window;
typedef unsigned long Atom;

#ifndef True
#define True 1
#endif
#ifndef False
#define False 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef None
#define None 0L
#endif

typedef struct {
    unsigned int size;
    caddr_t addr;
} XrmValue, *XrmValuePtr;

typedef void (*XtCallbackProc)(Widget, XtPointer, XtPointer);

typedef struct {
    XtCallbackProc callback;
    XtPointer closure;
} XtCallbackRec, *XtCallbackList;

typedef struct {
    XtPointer ksh_cmd;
} dtksh_client_data_t;

static void
stdCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
}

typedef struct {
    const char *text;
    Boolean separator;
} FakeSegment;

typedef struct {
    const FakeSegment *segs;
    int count;
} FakeXmString;

typedef FakeXmString *XmString;
typedef XmString *XmStringTable;
typedef struct {
    XmString str;
    int idx;
} FakeContext, *XmStringContext;
typedef char *XmStringCharSet;
typedef int XmStringDirection;

static char *
XtMalloc(size_t size)
{
    return (char *)malloc(size ? size : 1);
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
DtkStrDup(const char *s)
{
    return s ? strdup(s) : NULL;
}

static void
XtWarning(const char *msg)
{
    (void)msg;
}

#define GETMESSAGE(msg) (msg)
#define DT_CONV_BUF_OVFL 1
#define DT_WARNING 2
#define DT_TK_NOT_INIT 3

static char *
GetSharedMsg(int id)
{
    (void)id;
    return "shared message";
}

static void
printerr(const char *cmd, const char *msg1, const char *msg2)
{
    (void)cmd;
    (void)msg1;
    (void)msg2;
}

static Boolean
XmStringInitContext(XmStringContext *ctx, XmString str)
{
    *ctx = (XmStringContext)malloc(sizeof(FakeContext));
    (*ctx)->str = str;
    (*ctx)->idx = 0;
    return TRUE;
}

static Boolean
XmStringGetNextSegment(
    XmStringContext ctx,
    char **text,
    XmStringCharSet *charset,
    XmStringDirection *dir,
    Boolean *separator)
{
    (void)charset;
    (void)dir;
    if (!ctx || !ctx->str || ctx->idx >= ctx->str->count)
        return FALSE;
    const FakeSegment *seg = &ctx->str->segs[ctx->idx++];
    *text = strdup(seg->text);
    *separator = seg->separator;
    return TRUE;
}

static void
XmStringFreeContext(XmStringContext ctx)
{
    free(ctx);
}

struct named_integer {
    const char *name;
    int value;
};

#define TT_SCOPE_NONE 0
#define TT_SESSION 1
#define TT_FILE 2
#define TT_BOTH 3
#define TT_FILE_IN_SESSION 4

static struct named_integer NI_TT_SCOPE[] = {
    {"TT_SCOPE_NONE", TT_SCOPE_NONE},
    {"TT_SESSION", TT_SESSION},
    {"TT_FILE", TT_FILE},
    {"TT_BOTH", TT_BOTH},
    {"TT_FILE_IN_SESSION", TT_FILE_IN_SESSION},
};

static struct named_integer NI_TT_OP[] = {
    {"TTDT_CREATED", 100},
};

static int
DtCompareISOLatin1(const char *s1, const char *s2)
{
    if (!s1 || !s2)
        return 0;
    return strcasecmp(s1, s2) == 0;
}

static void
DtkshCvtNamedValueToString(
    XrmValuePtr args,
    Cardinal *nargs,
    XrmValuePtr fval,
    XrmValuePtr toval)
{
    (void)args;
    (void)nargs;
    (void)fval;
    toval->addr = NULL;
    toval->size = 0;
}

#define TAB_EMPTY 0
typedef struct wtab {
    int type;
    char *wname;
    Widget w;
    struct wtab *parent;
} wtab_t;

static wtab_t **W = NULL;
static Widget Toplevel = (Widget)0x1234;

static Widget
XtNameToWidget(Widget ref, const char *name)
{
    (void)ref;
    (void)name;
    return (Widget)0x5678;
}

#include "dtksh_extracted.inc"

static void
test_xmstring_table_to_string(void)
{
    /*
     * 1. Five empty XmStrings: previously overflowed XtMalloc(3) because
     *    strcat(buf, ",") ran without growing buf.
     */
    FakeXmString empty_item = {NULL, 0};
    XmString empty_table[5] = {
        &empty_item, &empty_item, &empty_item, &empty_item, &empty_item
    };
    char *res = _CvtXmStringTableToString(empty_table, 5);
    assert(res != NULL);
    assert(strcmp(res, ",,,,") == 0);

    /*
     * 2. Item 0 has separator=TRUE, followed by item 1 ("beta") and item 2 ("gamma").
     *    Previously separator stayed TRUE after item 0 so items 1 and 2 were skipped!
     */
    FakeSegment seg0[] = {{"alpha", TRUE}};
    FakeSegment seg1[] = {{"beta,one", FALSE}};
    FakeSegment seg2[] = {{"gamma", FALSE}};
    FakeXmString item0 = {seg0, 1};
    FakeXmString item1 = {seg1, 1};
    FakeXmString item2 = {seg2, 1};
    XmString table[3] = {&item0, &item1, &item2};

    res = _CvtXmStringTableToString(table, 3);
    assert(res != NULL);
    assert(strcmp(res, "alpha,beta\\,one,gamma") == 0);
}

static void
test_cvt_string_to_tt_scope(void)
{
    XrmValue toval = {0, NULL};
    DtkshCvtStringToTtScope("TT_SESSION", &toval);
    assert(toval.addr != NULL);
    assert(toval.size == sizeof(int));
    assert(*(int *)toval.addr == TT_SESSION);
}

static void
test_64bit_numeric_and_pointer_converters(void)
{
    /* 1. DtkshCvtScreenToString with a 64-bit pointer */
    Screen fake_screen = (Screen)(uintptr_t)0x7ffeffffffff1234ULL;
    XrmValue fval = {sizeof(Screen *), (caddr_t)&fake_screen};
    XrmValue toval = {0, NULL};
    Cardinal nargs = 0;
    DtkshCvtScreenToString(NULL, &nargs, &fval, &toval);
    assert(toval.addr != NULL);

    /* 2. DtkshCvtHexIntToString with a 64-bit long (-1L) */
    long neg_one = -1L;
    fval.size = sizeof(long);
    fval.addr = (caddr_t)&neg_one;
    toval.addr = NULL;
    toval.size = 0;
    DtkshCvtHexIntToString(NULL, &nargs, &fval, &toval);
    assert(toval.addr != NULL);

    /* 3. DtkshCvtIntToString with a 64-bit long (XmTextPosition / XEvent.serial) */
    long pos = 123456789L;
    fval.size = sizeof(long);
    fval.addr = (caddr_t)&pos;
    toval.addr = NULL;
    toval.size = 0;
    DtkshCvtIntToString(NULL, &nargs, &fval, &toval);
    assert(toval.addr != NULL);
    assert(strcmp((char *)toval.addr, "123456789") == 0);
}

static void
test_callback_to_string_null_and_bounds(void)
{
    XtCallbackList null_list = NULL;
    XrmValue fval = {sizeof(XtCallbackList), (caddr_t)&null_list};
    XrmValue toval = {0, NULL};
    Cardinal nargs = 0;

    DtkshCvtCallbackToString(NULL, NULL, &nargs, &fval, &toval, NULL);
    assert(toval.addr != NULL);
    assert(strcmp((char *)toval.addr, ": ;") == 0);
}

static void
test_widget_name_lookup_and_getname(void)
{
    wtab_t empty_slot;
    memset(&empty_slot, 0, sizeof(empty_slot));
    empty_slot.type = TAB_EMPTY;
    empty_slot.wname = NULL;
    wtab_t *table[1] = {&empty_slot};
    W = table;

    /* Must not dereference W[0]->wname when TAB_EMPTY */
    assert(DtkshNameToWidget("top.child") == NULL);

    /* getname overflow handling must NUL-terminate */
    wtab_t long_w;
    memset(&long_w, 0, sizeof(long_w));
    long_w.wname = "very_long_widget_name_exceeding_small_buffer";
    long_w.parent = NULL;
    char small_buf[16];
    memset(small_buf, 'Z', sizeof(small_buf));
    char *res = getname(&long_w, small_buf, sizeof(small_buf));
    assert(res != NULL);
    assert(res[0] == '*');
    assert(res[1] == '\0');
}

int
main(void)
{
    test_xmstring_table_to_string();
    test_cvt_string_to_tt_scope();
    test_64bit_numeric_and_pointer_converters();
    test_callback_to_string_null_and_bounds();
    test_widget_name_lookup_and_getname();
    printf("check-dtksh-hardening: all tests passed\n");
    return 0;
}
