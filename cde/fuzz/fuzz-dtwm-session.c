/*
 * Fuzz target: the dtwm front panel session file reader (dtwm/Session.c).
 *
 * run-fuzz.py extracts SessionFileGetName and SessionRestoreData verbatim.
 * The fuzz input is the content of ~/.dt/sessions/dtwmfp.session: fopen() of
 * the session file returns a memory stream over it.  The Xt widgets the reader
 * drives are stand-ins; the panel it matches the file against has two
 * subpanels (sub1, sub2) with two controls each (ctl1 ... ctl4), a main
 * control (main1, main2) owning each subpanel and one box (box1) holding them, so that the seeds can reach
 * the matching code with these names.  lstat() of a dynamic component's file
 * name answers "exists" for a name that starts with an even-valued byte.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <pwd.h>
#include <stdint.h>
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
static String unpost_arrow_image, unpost_monitor_arrow_image;

static int live;
static const uint8_t *session_data;
static size_t session_size;

/* Xt allocation: out of memory ends the program, as XtMalloc does. */
static void *XtMalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); ++live; return p; }
static void *XtRealloc(char *p, size_t n)
{ if (!p) ++live; p = realloc(p, n ? n : 1); if (!p) abort(); return p; }
static void XtFree(char *p) { if (p) { --live; free(p); } }
static char *xt_new_string(const char *s)
{ char *p = XtMalloc(strlen(s) + 1); strcpy(p, s); return p; }
static void XtManageChild(Widget w) { (void)w; }
static void XtSetMappedWhenManaged(Widget w, Boolean b) { (void)w; (void)b; }
static void XtSetValues(Widget w, Arg *a, int n) { (void)w; (void)a; (void)n; }
static void _DtWsmSetDtWmHints(void *d, int w, DtWmHints *h) { (void)d; (void)w; (void)h; }
static void XIconifyWindow(void *d, int w, int s) { (void)d; (void)w; (void)s; }
static int XScreenNumberOfScreen(void *s) { (void)s; return 0; }
static Boolean CheckOtherMonitorsOn(SubpanelData *s) { (void)s; return False; }
static void ToggleDefaultControl(ControlData *p, SubpanelData *s, ControlData *c)
{ (void)p; (void)s; (void)c; }

/* The reader locates the session file under the passwd home directory, opens
   it, and looks up the files of the dynamic records. */
#define getpwuid stub_getpwuid
static struct passwd *stub_getpwuid(uid_t u)
{ static struct passwd pw; (void)u; pw.pw_dir = (char *)"/nonexistent-home"; return &pw; }
static FILE *stub_fopen(const char *path, const char *mode)
{
    (void)path; (void)mode;
    return session_size ? fmemopen((void *)session_data, session_size, "r") : fopen("/dev/null", "r");
}
#define fopen stub_fopen

/* The record counts of the file bound the loops of the reader, not the size of
   the file.  FUZZ_WORK_LIMIT=N ends an input after N fscanf() calls so that a
   run can go on past that finding; without it the reader runs to the end. */
static unsigned long scan_calls, scan_limit;
static jmp_buf escape;
static int counted_fscanf(FILE *f, const char *format, ...)
{
    va_list args;
    int n;
    if (scan_limit && ++scan_calls > scan_limit) longjmp(escape, 1);
    va_start(args, format);
    n = vfscanf(f, format, args);
    va_end(args);
    return n;
}
#define fscanf counted_fscanf
#define lstat stub_lstat
static int stub_lstat(const char *path, struct stat *st)
{ (void)st; return path[0] % 2 == 0 ? 0 : -1; }

#include FUZZ_PRODUCTION_H
#undef fopen

/* the panel the file is matched against */
static WidgetRec widgets[16], parent_widget;
static ControlData controls[4], owners[2];
static ControlData *sub_controls[2][2], *box_list[2];
static SubpanelData subs[2];
static BoxData box;
static BoxData *boxes[1];
static WidgetRec shell_widget;

static void build_panel(void)
{
    static const char *sub_names[2] = {"sub1", "sub2"};
    static const char *ctl_names[4] = {"ctl1", "ctl2", "ctl3", "ctl4"};
    memset(controls, 0, sizeof controls); memset(owners, 0, sizeof owners);
    memset(subs, 0, sizeof subs); memset(&box, 0, sizeof box);
    memset(widgets, 0, sizeof widgets);
    parent_widget.width = 100;
    for (int i = 0; i < 16; i++) { widgets[i].width = 10 + i; widgets[i].parent = &parent_widget; }
    for (int i = 0; i < 4; i++) controls[i].element_values[CONTROL_NAME].parsed_value = (void *)ctl_names[i];
    for (int s = 0; s < 2; s++) {
        sub_controls[s][0] = &controls[2 * s];
        sub_controls[s][1] = &controls[2 * s + 1];
        subs[s].element_values[SUBPANEL_NAME].parsed_value = (void *)sub_names[s];
        subs[s].element_values[SUBPANEL_CONTAINER_NAME].parsed_value = (void *)"par";
        subs[s].control_data = sub_controls[s]; subs[s].control_data_count = 2;
        subs[s].parent_control_data = &owners[s];
        subs[s].shell = &widgets[2 * s]; subs[s].form = &widgets[2 * s + 1];
        owners[s].subpanel_data = &subs[s];
        owners[s].element_values[CONTROL_NAME].parsed_value = (void *)(s ? "main2" : "main1");
        owners[s].icon = &widgets[4 + s]; owners[s].arrow = &widgets[6 + s];
    }
    box.element_values[BOX_NAME].parsed_value = (void *)"box1";
    box_list[0] = &owners[0]; box_list[1] = &owners[1];   /* a box holds the main controls that own the subpanels */
    box.control_data = box_list; box.control_data_count = 2;
    boxes[0] = &box;
    memset(&panel, 0, sizeof panel);
    panel.box_data = boxes; panel.box_data_count = 1;
    panel.shell = &shell_widget;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    setenv("HOME", "/nonexistent-home", 1);
    if (getenv("FUZZ_WORK_LIMIT")) scan_limit = strtoul(getenv("FUZZ_WORK_LIMIT"), NULL, 10);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    build_panel();
    session_data = data;
    session_size = size;
    scan_calls = 0;
    if (setjmp(escape)) return 0;             /* work limit reached: the input is dropped */
    SessionRestoreData();

    if (panel.dynamic_data_list) {
        for (int i = 0; i < panel.dynamic_data_count; i++) {
            DynamicComponent *d = panel.dynamic_data_list[i];
            XtFree(d->file_name); XtFree(d->component_name); XtFree(d->parent_name); XtFree((char *)d);
        }
        XtFree((char *)panel.dynamic_data_list);
    }
    return 0;
}
