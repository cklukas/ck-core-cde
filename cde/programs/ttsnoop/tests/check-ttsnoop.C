/*
 * Regression tests for ttsnoop entity tracking, shell quoting, and
 * dialog dispatch / option parsing contracts.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

/* Minimal X11/Xt/Motif/Dt/ToolTalk stubs so DtTt.C and tt_c++.C compile
 * standalone without an X server or ToolTalk session. */

typedef char *String;
typedef unsigned char Boolean;
typedef void *XtPointer;
typedef unsigned long XtInputId;
typedef struct _WidgetRec *Widget;
typedef struct _Display Display;

#ifndef False
#define False 0
#endif
#ifndef True
#define True 1
#endif

typedef char *XmString;

#define XmNlabelString "labelString"
#define DtNstringData "stringData"

extern "C" {

static std::string g_last_label;
static std::string g_help_topics;
static std::vector<std::string> g_system_cmds;

char *
XtMalloc(unsigned int size)
{
    return (char *)malloc(size ? size : 1);
}

void
XtFree(char *ptr)
{
    free(ptr);
}

XmString
XmStringCreateLocalized(char *text)
{
    return strdup(text ? text : "");
}

void
XmStringFree(XmString s)
{
    free(s);
}

void
XtVaSetValues(Widget w, ...)
{
    (void)w;
    va_list ap;
    va_start(ap, w);
    const char *name = va_arg(ap, const char *);
    if (name != 0 && strcmp(name, XmNlabelString) == 0) {
        const char *val = va_arg(ap, const char *);
        g_last_label = val ? val : "";
    }
    va_end(ap);
}

void
XtVaGetValues(Widget w, ...)
{
    (void)w;
    va_list ap;
    va_start(ap, w);
    const char *name = va_arg(ap, const char *);
    if (name != 0 && strcmp(name, DtNstringData) == 0) {
        char **out = va_arg(ap, char **);
        *out = const_cast<char *>(g_help_topics.c_str());
    }
    va_end(ap);
}

} /* extern "C" */

#include "DtTt.h"

Tt_pattern snoopPat = 0;

extern "C" {

Tt_status
tt_pointer_error(void *p)
{
    if (p == (void *)-1) {
        return TT_ERR_NOMEM;
    }
    return TT_OK;
}

Tt_status
tt_int_error(int i)
{
    return (i < 0) ? TT_ERR_NOMEM : TT_OK;
}

void *
tt_error_pointer(Tt_status s)
{
    return (s == TT_OK) ? 0 : (void *)-1;
}

void
tt_free(caddr_t p)
{
    free(p);
}

char *
tt_status_message(Tt_status s)
{
    if (s == TT_OK) {
        return strdup("TT_OK\tNo error");
    }
    return strdup("TT_ERR\tError");
}

char *
tttk_op_string(Tttk_op op)
{
    switch (op) {
    case TTDT_CREATED:
        return (char *)"Created";
    case TTDT_DELETED:
        return (char *)"Deleted";
    case TTDT_MODIFIED:
        return (char *)"Modified";
    default:
        return (char *)"Op";
    }
}

Tttk_op
tttk_string_op(const char *s)
{
    if (s != 0 && strcmp(s, "Created") == 0) {
        return TTDT_CREATED;
    }
    return TTDT_OP_NONE;
}

Tt_status
tt_pattern_user_set(Tt_pattern p, int key, void *v)
{
    (void)p;
    (void)key;
    free(v);
    return TT_OK;
}

void *
tt_pattern_user(Tt_pattern p, int key)
{
    (void)p;
    (void)key;
    return 0;
}

char *
tt_message_op(Tt_message m)
{
    (void)m;
    return strdup("SampleOp");
}

char *
tt_message_id(Tt_message m)
{
    (void)m;
    return 0;
}

char *
tt_default_procid(void)
{
    return strdup("default-procid");
}

Tt_status
tt_default_procid_set(const char *procid)
{
    (void)procid;
    return TT_OK;
}

int
tt_fd(void)
{
    return 7;
}

Tt_message
tt_message_create(void)
{
    return (Tt_message)0x100;
}

Tt_status
tt_message_destroy(Tt_message m)
{
    (void)m;
    return TT_OK;
}

int
tt_message_contexts_count(Tt_message m)
{
    (void)m;
    return 0;
}

char *
tt_message_context_slotname(Tt_message m, int n)
{
    (void)m;
    (void)n;
    return 0;
}

Tt_status
tt_message_context_ival(Tt_message m, const char *s, int *v)
{
    (void)m;
    (void)s;
    (void)v;
    return TT_OK;
}

Tt_status
tt_message_icontext_set(Tt_message m, const char *s, int v)
{
    (void)m;
    (void)s;
    (void)v;
    return TT_OK;
}

Tt_status
tt_message_context_bval(Tt_message m, const char *s, unsigned char **b, int *l)
{
    (void)m;
    (void)s;
    *b = 0;
    *l = 0;
    return TT_OK;
}

Tt_status
tt_message_bcontext_set(Tt_message m, const char *s, const unsigned char *b, int l)
{
    (void)m;
    (void)s;
    (void)b;
    (void)l;
    return TT_OK;
}

int
tt_message_args_count(Tt_message m)
{
    (void)m;
    return 0;
}

Tt_mode
tt_message_arg_mode(Tt_message m, int n)
{
    (void)m;
    (void)n;
    return TT_IN;
}

char *
tt_message_arg_type(Tt_message m, int n)
{
    (void)m;
    (void)n;
    return 0;
}

Tt_status
tt_message_arg_ival(Tt_message m, int n, int *v)
{
    (void)m;
    (void)n;
    (void)v;
    return TT_OK;
}

Tt_status
tt_message_iarg_add(Tt_message m, Tt_mode mode, const char *vt, int v)
{
    (void)m;
    (void)mode;
    (void)vt;
    (void)v;
    return TT_OK;
}

Tt_status
tt_message_arg_bval(Tt_message m, int n, unsigned char **b, int *l)
{
    (void)m;
    (void)n;
    *b = 0;
    *l = 0;
    return TT_OK;
}

Tt_status
tt_message_barg_add(Tt_message m, Tt_mode mode, const char *vt, const unsigned char *b, int l)
{
    (void)m;
    (void)mode;
    (void)vt;
    (void)b;
    (void)l;
    return TT_OK;
}

Tt_status
tt_message_arg_add(Tt_message m, Tt_mode mode, const char *vt, const char *v)
{
    (void)m;
    (void)mode;
    (void)vt;
    (void)v;
    return TT_OK;
}

Tt_class
tt_message_class(Tt_message m)
{
    (void)m;
    return TT_REQUEST;
}

Tt_status
tt_message_class_set(Tt_message m, Tt_class c)
{
    (void)m;
    (void)c;
    return TT_OK;
}

char *
tt_message_file(Tt_message m)
{
    (void)m;
    return 0;
}

Tt_status
tt_message_file_set(Tt_message m, const char *f)
{
    (void)m;
    (void)f;
    return TT_OK;
}

char *
tt_message_object(Tt_message m)
{
    (void)m;
    return 0;
}

Tt_status
tt_message_object_set(Tt_message m, const char *o)
{
    (void)m;
    (void)o;
    return TT_OK;
}

Tt_status
tt_message_op_set(Tt_message m, const char *o)
{
    (void)m;
    (void)o;
    return TT_OK;
}

char *
tt_message_otype(Tt_message m)
{
    (void)m;
    return 0;
}

Tt_status
tt_message_otype_set(Tt_message m, const char *o)
{
    (void)m;
    (void)o;
    return TT_OK;
}

Tt_address
tt_message_address(Tt_message m)
{
    (void)m;
    return TT_PROCEDURE;
}

Tt_status
tt_message_address_set(Tt_message m, Tt_address a)
{
    (void)m;
    (void)a;
    return TT_OK;
}

Tt_scope
tt_message_scope(Tt_message m)
{
    (void)m;
    return TT_SESSION;
}

Tt_status
tt_message_scope_set(Tt_message m, Tt_scope s)
{
    (void)m;
    (void)s;
    return TT_OK;
}

char *
tt_message_sender_ptype(Tt_message m)
{
    (void)m;
    return 0;
}

Tt_status
tt_message_sender_ptype_set(Tt_message m, const char *p)
{
    (void)m;
    (void)p;
    return TT_OK;
}

char *
tt_message_session(Tt_message m)
{
    (void)m;
    return 0;
}

Tt_status
tt_message_session_set(Tt_message m, const char *s)
{
    (void)m;
    (void)s;
    return TT_OK;
}

int
tt_message_status(Tt_message m)
{
    (void)m;
    return 0;
}

Tt_status
tt_message_status_set(Tt_message m, int s)
{
    (void)m;
    (void)s;
    return TT_OK;
}

char *
tt_message_status_string(Tt_message m)
{
    (void)m;
    return 0;
}

Tt_status
tt_message_status_string_set(Tt_message m, const char *s)
{
    (void)m;
    (void)s;
    return TT_OK;
}

Tt_message
tttk_message_create(Tt_message ctx, Tt_class c, Tt_scope s, const char *h,
                    const char *op, Tt_message_callback cb)
{
    (void)ctx;
    (void)c;
    (void)s;
    (void)h;
    (void)op;
    (void)cb;
    return (Tt_message)0x200;
}

} /* extern "C" */

static int
fake_system(const char *cmd)
{
    g_system_cmds.push_back(cmd ? cmd : "");
    return 0;
}

#define system fake_system
#include "DtTt.C"
#include "tt_c++.C"
#undef system

static void
test_procid_null_client_datum(void)
{
    /* Regression: DtTtCreated(DTTT_PROCID, procid, 0) previously returned
     * TT_ERR_NOMEM because listAppend returned (list[n-1] = 0) == 0. */
    Tt_status st = DtTtCreated(DTTT_PROCID, "proc-zero", 0);
    assert(st == TT_OK);
    assert(DtTtIndex(DTTT_PROCID, "proc-zero") == 0);
    assert(DtTtNthClientDatum(DTTT_PROCID, 0) == 0);

    DtTtNthClientDatumSet(DTTT_PROCID, 0, (const void *)0x42);
    assert(DtTtNthClientDatum(DTTT_PROCID, 0) == (void *)0x42);

    assert(DtTtDestroyed(DTTT_PROCID, "proc-zero") == TT_OK);
    assert(DtTtIndex(DTTT_PROCID, "proc-zero") == -1);
}

static void
test_choices_and_null_labels(void)
{
    Widget dummyWidget = (Widget)0x1;

    /* Passing NULL func or string to DtTtSetLabel must not set badbit on
     * std::ostringstream or crash XmStringCreateLocalized. */
    DtTtSetLabel(dummyWidget, (const char *)0, TT_OK);
    assert(g_last_label.find("= TT_OK") != std::string::npos);

    Tt_message fakeMsg = (Tt_message)0x1234;
    assert(DtTtCreated(DTTT_MESSAGE, fakeMsg) == TT_OK);
    int count = 0;
    XmString *choices = _DtTtChoices(DTTT_MESSAGE, &count);
    assert(count == 1);
    assert(choices != 0);
    /* tt_message_id returns NULL in our stub; itemStream must still be
     * intact and contain SampleOp. */
    assert(strstr(choices[0], "SampleOp") != 0);
    XmStringFree(choices[0]);
    XtFree((char *)choices);
    assert(DtTtDestroyed(DTTT_MESSAGE, fakeMsg) == TT_OK);
}

static void
test_shell_quoting(void)
{
    Widget dummyWidget = (Widget)0x1;
    g_system_cmds.clear();

    _DtMan(dummyWidget, "tt_open; touch '/bad' $(id) `uname`");
    assert(g_system_cmds.size() == 1);
    /* Single-quoted argument must escape embedded single quotes as '\'' */
    const std::string &cmd = g_system_cmds[0];
    assert(cmd.find("'tt_open; touch '\\''/bad'\\'' $(id) `uname`'") !=
           std::string::npos);
}

int
main(int argc, char **argv)
{
    const char *part = (argc > 1) ? argv[1] : "all";

    if (strcmp(part, "all") == 0 || strcmp(part, "procid") == 0) {
        test_procid_null_client_datum();
    }
    if (strcmp(part, "all") == 0 || strcmp(part, "labels") == 0) {
        test_choices_and_null_labels();
    }
    if (strcmp(part, "all") == 0 || strcmp(part, "shell") == 0) {
        test_shell_quoting();
    }
    return 0;
}
