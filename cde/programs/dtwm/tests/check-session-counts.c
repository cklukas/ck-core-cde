/*
 * Behavioural check of the home directory lookup and the record counts of the
 * front panel session reader (Session.c).
 *
 * The runner extracts the reader verbatim from Session.c into a generated
 * header (SESSION_READER_H) and compiles it here against stand-ins for the
 * Xt/Dt types, the password database and the allocators.  One scenario runs per
 * process (argv[1]), in the scratch directory argv[2], which holds two home
 * directories: "home-hit" (named by HOME) and "home-pw" (named by the fake
 * password database).
 *
 * Bounds enforced here: the reader may call fscanf only a few times for a file
 * of a few bytes, and may never ask the allocator for more than a documented
 * limit of records needs.  A reader that spins on end of file or requests
 * gigabytes ends the process with a message instead of running on.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
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
static long scan_calls;
static size_t largest_request;
static int fail_next_components;       /* fail the Nth DynamicComponent allocation */
static size_t fail_size;               /* fail allocations of exactly this size */
static const char *passwd_dir;         /* nullptr: the password database has no entry */

#define SCAN_LIMIT 100000L
#define REQUEST_LIMIT ((size_t)64 << 20)

static void *XtMalloc(size_t n)
{
    if (n > largest_request) largest_request = n;
    if (n > REQUEST_LIMIT) {
        printf("FAIL: allocation of %zu bytes requested\n", n);
        exit(4);
    }
    if (fail_size && n == fail_size) return NULL;
    if (n == sizeof(DynamicComponent) && fail_next_components > 0 &&
        --fail_next_components == 0) return NULL;
    void *p = malloc(n); assert(p); ++live; return p;
}
static void *XtRealloc(char *p, size_t n)
{
    if (n > largest_request) largest_request = n;
    if (n > REQUEST_LIMIT) {
        printf("FAIL: allocation of %zu bytes requested\n", n);
        exit(4);
    }
    if (!p) ++live;
    p = realloc(p, n); assert(p); return p;
}
static void XtFree(char *p) { if (p) { --live; free(p); } }
static char *xt_new_string(const char *s)
{ char *p = XtMalloc(strlen(s) + 1); if (p) strcpy(p, s); return p; }
static void XtManageChild(Widget w) { (void)w; ++manage_calls; }
static void XtSetMappedWhenManaged(Widget w, Boolean b) { (void)w; (void)b; }
static void XtSetValues(Widget w, Arg *a, int n) { (void)w; (void)a; (void)n; }
static void _DtWsmSetDtWmHints(void *d, int w, DtWmHints *h) { (void)d; (void)w; (void)h; }
static void XIconifyWindow(void *d, int w, int s) { (void)d; (void)w; (void)s; ++iconify_calls; }
static int XScreenNumberOfScreen(void *s) { (void)s; return 0; }
static Boolean CheckOtherMonitorsOn(SubpanelData *s) { (void)s; return False; }
static void ToggleDefaultControl(ControlData *p, SubpanelData *s, ControlData *c)
{ (void)p; (void)s; (void)c; ++toggle_calls; }

#define getpwuid stub_getpwuid
static struct passwd *stub_getpwuid(uid_t u)
{ static struct passwd pw; (void)u;
  if (!passwd_dir) return NULL;
  pw.pw_dir = (char *)passwd_dir; return &pw; }

/* Every fscanf of the reader is counted; a reader that does not stop at the
 * end of the file is cut off here. */
#define fscanf counted_fscanf
static int counted_fscanf(FILE *f, const char *format, ...)
{
    va_list args; int n;
    if (++scan_calls > SCAN_LIMIT) {
        printf("FAIL: more than %ld fscanf calls\n", SCAN_LIMIT);
        exit(3);
    }
    va_start(args, format); n = vfscanf(f, format, args); va_end(args);
    return n;
}

#include SESSION_READER_H

static char work[MAX_PATH], home_hit[MAX_PATH], home_pw[MAX_PATH];

static void put(const char *home, const char *name, const char *content)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/.dt", home); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.dt/sessions", home); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.dt/types", home); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.dt/types/fp_dynamic", home); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/%s", home, name);
    FILE *f = fopen(path, "w"); assert(f);
    fputs(content, f); fclose(f);
}
#define SESSION ".dt/sessions/dtwmfp.session"

static void reset(void)
{
    manage_calls = toggle_calls = iconify_calls = 0;
    scan_calls = 0; largest_request = 0;
    panel.dynamic_data_count = panel.max_dynamic_data_count = 0;
    panel.dynamic_data_list = nullptr;
}

static void free_list(void)
{
    for (int i = 0; i < panel.dynamic_data_count; i++) {
        DynamicComponent *d = panel.dynamic_data_list[i];
        XtFree(d->file_name); XtFree(d->component_name); XtFree(d->parent_name);
        XtFree((char *)d);
    }
    XtFree((char *)panel.dynamic_data_list);
    panel.dynamic_data_list = nullptr; panel.dynamic_data_count = 0;
}

/* Run the reader on CONTENT (written to the session file under home_hit). */
static void restore(const char *content)
{
    put(home_hit, SESSION, content);
    reset();
    SessionRestoreData();
}

/* A file of a few bytes: a handful of fscanf calls, a small allocation. */
static void bounded(long calls, size_t bytes)
{
    if (scan_calls > calls || largest_request > bytes) {
        printf("FAIL: %ld fscanf calls (limit %ld), largest request %zu (limit %zu)\n",
               scan_calls, calls, largest_request, bytes);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    const char *scenario = argv[1];
    snprintf(work, sizeof work, "%s", argv[2]);
    snprintf(home_hit, sizeof home_hit, "%s/home-hit", work);
    snprintf(home_pw, sizeof home_pw, "%s/home-pw", work);
    mkdir(home_hit, 0700); mkdir(home_pw, 0700);
    assert(chdir(work) == 0);

    /* One subpanel "sub1" with one control "ctl1" in a box. */
    ControlData control = {{{0}}};
    control.element_values[CONTROL_NAME].parsed_value = "ctl1";
    ControlData *controls[1] = { &control };
    WidgetRec icon = { 10, nullptr }, parent = { 100, nullptr };
    icon.parent = &parent;
    ControlData owner = {{{0}}};
    owner.icon = &icon;
    SubpanelData sub = {{{0}}};
    sub.element_values[SUBPANEL_NAME].parsed_value = "sub1";
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

    /* The posted-subpanel record that proves which file was read. */
    const char *posted = "1\nsub1 5 6 0\n0\n0\n1\n";

    if (!strcmp(scenario, "home-set")) {
        /* HOME names the directory even when the password database differs. */
        setenv("HOME", home_hit, 1); passwd_dir = home_pw;
        put(home_pw, SESSION, "0\n0\n0\n1\n");
        restore(posted);
        assert(manage_calls == 2);
        assert(live == 0);
    } else if (!strcmp(scenario, "home-unset") || !strcmp(scenario, "home-empty")) {
        /* No usable HOME: the password database names the directory. */
        if (!strcmp(scenario, "home-unset")) unsetenv("HOME"); else setenv("HOME", "", 1);
        passwd_dir = home_pw;
        put(home_hit, SESSION, "0\n0\n0\n1\n");
        put(home_pw, SESSION, posted);
        reset();
        SessionRestoreData();
        assert(manage_calls == 2);
        assert(live == 0);
    } else if (!strcmp(scenario, "home-nowhere")) {
        /* No HOME and no password entry: nothing to read or write, no crash. */
        unsetenv("HOME"); passwd_dir = nullptr;
        reset();
        SessionRestoreData();
        assert(manage_calls == 0 && scan_calls == 0 && live == 0);
        SessionDeleteAll();
    } else if (!strcmp(scenario, "delete-home-set")) {
        /* SessionDeleteAll empties fp_dynamic of HOME, not of the passwd home. */
        setenv("HOME", home_hit, 1); passwd_dir = home_pw;
        put(home_hit, ".dt/types/fp_dynamic/a", "x");
        put(home_pw, ".dt/types/fp_dynamic/b", "x");
        SessionDeleteAll();
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s/.dt/types/fp_dynamic/a", home_hit);
        assert(access(path, F_OK) != 0);
        snprintf(path, sizeof path, "%s/.dt/types/fp_dynamic/b", home_pw);
        assert(access(path, F_OK) == 0);
    } else if (!strcmp(scenario, "delete-home-unset")) {
        unsetenv("HOME"); passwd_dir = home_pw;
        put(home_pw, ".dt/types/fp_dynamic/b", "x");
        SessionDeleteAll();
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s/.dt/types/fp_dynamic/b", home_pw);
        assert(access(path, F_OK) != 0);
    } else {
        /* Both lookups name the same directory: these scenarios test the
         * counts, whatever the home directory rules are. */
        setenv("HOME", home_hit, 1); passwd_dir = home_hit;
        FILE *f = fopen("exists", "w"); assert(f); fclose(f);

        if (!strcmp(scenario, "fuzz-timeout")) {
            restore("88888888t");                  /* min-timeout of the fuzzer */
            bounded(6, 1024);
        } else if (!strcmp(scenario, "fuzz-oom")) {
            restore("0 0 606888800");              /* min-oom of the fuzzer */
            bounded(6, 1024);
            assert(panel.dynamic_data_count == 0 && live == 0);
        } else if (!strcmp(scenario, "huge-counts")) {
            restore("2144444444444444448\n2147483647\n2147483647\n");
            bounded(8, 1024);
            assert(panel.dynamic_data_count == 0 && live == 0);
        } else if (!strcmp(scenario, "negative-counts")) {
            restore("-1\n-2147483647\n-5\n0\n");
            bounded(8, 1024);
            assert(panel.dynamic_data_count == 0 && live == 0);
        } else if (!strcmp(scenario, "limit-edge")) {
            /* 10000 records are accepted (allocated, then the file ends);
             * 10001 are not. */
            restore("0\n0\n10000\n");
            bounded(8, 10000 * sizeof(void *) + 1024);
            assert(largest_request >= 10000 * sizeof(void *));
            assert(panel.dynamic_data_count == 0);
            XtFree((char *)panel.dynamic_data_list);
            assert(live == 0);
            restore("0\n0\n10001\n");
            bounded(8, 1024);
            assert(panel.dynamic_data_list == nullptr && live == 0);
        } else if (!strcmp(scenario, "malformed-records")) {
            /* Counts that promise more records than the file holds, and
             * records cut short: the reader stops at the first one. */
            restore("5\nsub1 5\n");
            bounded(6, 1024);
            restore("0\n5\nsub1\n");
            bounded(6, 1024);
            assert(toggle_calls == 0);
            restore("0\n0\n3\nexists sub1 2 par\n");
            bounded(6, 1024);
            free_list();
            restore("0\n0\n9999\nexists ctl1 2 par 1 1\nexists ctl1 2\n");
            bounded(8, 9999 * sizeof(void *) + 1024);
            assert(panel.dynamic_data_count == 1);   /* the complete record is kept */
            free_list();
            assert(live == 0);
        } else if (!strcmp(scenario, "valid-file")) {
            /* A well-formed file still restores everything. */
            restore("1\nsub1 5 6 0\n1\nsub1 ctl1\n1\nexists ctl1 2 par 1 1\n0\n");
            assert(manage_calls == 2 && toggle_calls == 1);
            assert(panel.dynamic_data_count == 1);
            assert(strcmp(panel.dynamic_data_list[0]->file_name, "exists") == 0);
            assert(iconify_calls == 1);
            free_list();
            assert(live == 0);
        } else if (!strcmp(scenario, "record-alloc-fails")) {
            /* The second record allocation fails: the first record is kept,
             * the rest of the file is dropped, nothing leaks. */
            fail_next_components = 2;
            restore("0\n0\n3\nexists ctl1 2 par 1 1\nexists ctl2 2 par 1 1\n"
                    "exists ctl3 2 par 1 1\n");
            assert(panel.dynamic_data_count == 1);
            assert(panel.dynamic_data_list[0] != nullptr);
            free_list();
            assert(live == 0);
        } else if (!strcmp(scenario, "list-alloc-fails")) {
            /* The list itself cannot be allocated: no records, no crash. */
            fail_size = 3 * sizeof(DynamicComponent *);
            restore("0\n0\n3\nexists ctl1 2 par 1 1\n");
            assert(panel.dynamic_data_count == 0 && panel.dynamic_data_list == nullptr);
            assert(live == 0);
        } else {
            printf("unknown scenario %s\n", scenario);
            return 2;
        }
    }
    printf("scenario %s: ok\n", scenario);
    return 0;
}
