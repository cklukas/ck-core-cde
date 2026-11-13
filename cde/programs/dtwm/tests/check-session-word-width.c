/*
 * Behavioural check of the front panel session reader (Session.c).
 *
 * The runner extracts SessionFileGetName and SessionRestoreData verbatim from
 * Session.c into a generated header (SESSION_READER_H) and compiles it here
 * against minimal stand-ins for the Xt/Dt types and globals the reader
 * touches. Each scenario writes a real session file containing a 300
 * character word and runs the production reader on it.
 *
 * A word of 256 or more characters must be truncated to 255 characters by
 * the "%255s" conversions; the unread tail is then parsed as the next token.
 * Built with ASan/UBSan, an unbounded "%s" overruns the 256 byte name buffers.
 */
#include <assert.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int Boolean;
typedef unsigned short Dimension;
#define True 1
#define False 0
#define MAX_PATH 1024

typedef struct WidgetRec { int width; struct WidgetRec *parent; } WidgetRec;
typedef WidgetRec *Widget;
typedef struct { int name; long value; } Arg;
typedef char *String;
enum { XmNx = 1, XmNy, XmNwidth, XmNimageName };
#define XtSetArg(a, n, v) ((a).name = (n), (a).value = (long)(v))
#define XtWidth(w) ((w)->width)
#define XtParent(w) ((w)->parent)
#define XtNewString(s) xt_new_string(s)
#define XtDisplay(w) ((void *)0)
#define XtWindow(w) 0
#define XtScreen(w) ((void *)0)

enum { SUBPANEL_NAME, SUBPANEL_CONTAINER_NAME, CONTROL_NAME, BOX_NAME, ELEMENTS };
enum { BOX = 1, SUBPANEL = 2 };
typedef struct { void *parsed_value; } ElementValue;

typedef struct ControlData ControlData;
typedef struct SubpanelData SubpanelData;
typedef struct { ElementValue element_values[ELEMENTS];
                 ControlData **control_data; int control_data_count; } BoxData;
struct ControlData { ElementValue element_values[ELEMENTS];
                     SubpanelData *subpanel_data; Widget arrow, icon; };
struct SubpanelData { ElementValue element_values[ELEMENTS];
                      Boolean torn; Widget shell, form;
                      ControlData *parent_control_data;
                      ControlData **control_data; int control_data_count; };
typedef struct { char *file_name, *component_name, *parent_name;
                 int component_type, parent_type; Boolean delete; } DynamicComponent;

typedef struct { unsigned long flags, behaviors, attachWindow; } DtWmHints;
#define DtWM_HINTS_BEHAVIORS 1
#define DtWM_HINTS_ATTACH_WINDOW 2
#define DtWM_BEHAVIOR_PANEL 1
#define DtWM_BEHAVIOR_SUBPANEL 2
#define DtWM_BEHAVIOR_SUB_RESTORED 4

static struct {
    Widget shell;
    int box_data_count;
    BoxData **box_data;
    int dynamic_data_count, max_dynamic_data_count;
    DynamicComponent **dynamic_data_list;
} panel;
static int subpanel_x, subpanel_y, subpanel_torn;
static int dynamic_data_count;
static Boolean have_it;
static DtWmHints vHints;
static String unpost_arrow_image, unpost_monitor_arrow_image;

static int live, manage_calls, toggle_calls, iconify_calls;
static SubpanelData *toggled_subpanel;
static ControlData *toggled_control;
static const char *home_dir;

static void *XtMalloc(size_t n) { void *p = malloc(n); assert(p); ++live; return p; }
static void *XtRealloc(char *p, size_t n)
{ if (!p) ++live; p = realloc(p, n); assert(p); return p; }
static void XtFree(char *p) { if (p) { --live; free(p); } }
static char *xt_new_string(const char *s)
{ char *p = XtMalloc(strlen(s) + 1); strcpy(p, s); return p; }
static void XtManageChild(Widget w) { (void)w; ++manage_calls; }
static void XtSetMappedWhenManaged(Widget w, Boolean b) { (void)w; (void)b; }
static void XtSetValues(Widget w, Arg *a, int n) { (void)w; (void)a; (void)n; }
static void _DtWsmSetDtWmHints(void *d, int w, DtWmHints *h) { (void)d; (void)w; (void)h; }
static void XIconifyWindow(void *d, int w, int s) { (void)d; (void)w; (void)s; ++iconify_calls; }
static int XScreenNumberOfScreen(void *s) { (void)s; return 0; }
static Boolean CheckOtherMonitorsOn(SubpanelData *s) { (void)s; return False; }
static void ToggleDefaultControl(ControlData *p, SubpanelData *s, ControlData *c)
{ (void)p; ++toggle_calls; toggled_subpanel = s; toggled_control = c; }

/* HOME names the home directory; the password database is the fallback. */
#define getpwuid stub_getpwuid
static struct passwd *stub_getpwuid(uid_t u)
{ static struct passwd pw; (void)u; pw.pw_dir = (char *)home_dir; return &pw; }

#include SESSION_READER_H

#define WORD 300
#define KEPT 255

static char *letters(size_t n)
{ char *s = malloc(n + 1); assert(s); memset(s, 'a', n); s[n] = 0; return s; }

static void write_session(const char *dir, const char *head, const char *tail)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/.dt", dir); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.dt/sessions", dir); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.dt/sessions/dtwmfp.session", dir);
    FILE *f = fopen(path, "w"); assert(f);
    char *w = letters(WORD);
    fprintf(f, "%s%s%s", head, w, tail);
    free(w);
    fclose(f);
}

static void reset(void)
{
    manage_calls = toggle_calls = iconify_calls = 0;
    toggled_subpanel = nullptr; toggled_control = nullptr;
    subpanel_x = subpanel_y = subpanel_torn = 0;
    panel.dynamic_data_count = panel.max_dynamic_data_count = 0;
    panel.dynamic_data_list = nullptr;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    home_dir = argv[1];
    assert(setenv("HOME", home_dir, 1) == 0);
    assert(chdir(home_dir) == 0);

    /* One subpanel "a*255" with one control "a*45" (the tail of a 300 word). */
    char *sub_name = letters(KEPT), *ctl_name = letters(WORD - KEPT);
    ControlData control = {{{0}}};
    control.element_values[CONTROL_NAME].parsed_value = ctl_name;
    ControlData *controls[1] = { &control };
    WidgetRec icon = { 10, nullptr }, parent = { 100, nullptr };
    icon.parent = &parent;
    ControlData owner = {{{0}}};
    owner.icon = &icon;
    SubpanelData sub = {{{0}}};
    sub.element_values[SUBPANEL_NAME].parsed_value = sub_name;
    sub.element_values[SUBPANEL_CONTAINER_NAME].parsed_value = "par";
    sub.control_data = controls; sub.control_data_count = 1;
    sub.parent_control_data = &owner;
    sub.shell = &icon; sub.form = &icon;
    owner.subpanel_data = &sub;
    ControlData *box_controls[1] = { &owner };
    BoxData box = {{{0}}};
    box.control_data = box_controls; box.control_data_count = 1;
    BoxData *boxes[1] = { &box };
    panel.box_data = boxes; panel.box_data_count = 1;
    WidgetRec shell = { 0, nullptr };
    panel.shell = &shell;

    /* 1. Posted-subpanel record: the name is cut at 255 characters and the
     *    45 character tail is read where the x coordinate belongs, so the
     *    record is malformed.  It must be dropped (no subpanel is posted and
     *    the rest of the section is not read), not overrun the buffer. */
    reset();
    write_session(home_dir, "1\n", " 5 6 0\n0\n0\n1\n");
    SessionRestoreData();
    assert(live == 0);
    assert(manage_calls == 0);

    /* 2. Default-control record: the first word is cut at 255 characters and
     *    its 45 character tail is read as the control name. */
    reset();
    write_session(home_dir, "0\n1\n", "\n0\n1\n");
    SessionRestoreData();
    assert(live == 0);
    assert(toggle_calls == 1);
    assert(toggled_subpanel == &sub && toggled_control == &control);
    assert(iconify_calls == 0);  /* the trailing "1" after the records parsed */

    /* 3. Dynamic record: file name cut at 255 characters (such a file exists
     *    in the working directory), tail 45 characters become the component
     *    name, and the remaining fields of the record still parse. */
    reset();
    char *file = letters(KEPT);
    FILE *f = fopen(file, "w"); assert(f); fclose(f);
    write_session(home_dir, "0\n0\n1\n", " 2 par 1 1\n0\n");
    SessionRestoreData();
    assert(panel.dynamic_data_count == 1);
    DynamicComponent *d = panel.dynamic_data_list[0];
    assert(strlen(d->file_name) == KEPT && strcmp(d->file_name, file) == 0);
    assert(strlen(d->component_name) == WORD - KEPT);
    assert(strcmp(d->component_name, ctl_name) == 0);
    assert(d->component_type == 2 && d->parent_type == 1 && d->delete);
    assert(strcmp(d->parent_name, "par") == 0);
    assert(iconify_calls == 1);  /* mapped == 0 after the records */
    XtFree(d->file_name); XtFree(d->component_name); XtFree(d->parent_name);
    XtFree((char *)d); XtFree((char *)panel.dynamic_data_list);
    assert(live == 0);
    unlink(file);
    free(file); free(sub_name); free(ctl_name);
    puts("session word width: 3 scenarios passed");
    return 0;
}
