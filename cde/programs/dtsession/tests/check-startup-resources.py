#!/usr/bin/env python3
"""Check how dtsession loads the language and resolution independent resources.

At startup dtsession compares the language (LANG) and display resolution of the
saved session with the current ones and, if they differ, looks for a font
resource file in the session directory and in the system directories and merges
it into the resources of the display.  The production
RestoreIndependentResources() of SmRestore.c is cut out of the source and
compiled, under AddressSanitizer and UndefinedBehaviorSanitizer, with stand-ins
for Xlib's property access and the toolkit, and run with what the environment
and the session can hold: no LANG, no saved language, a LANG as long as it can
be, no session directory (the directories could not be created), font resources
that are missing, long or have a name only. Every scenario runs in its own
process.

DTSESSION_SOURCE names another copy of the sources to test, for example an
older revision.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
text = (source / "SmRestore.c").read_text()
strdefs = (source / "SmStrDefs.c").read_text()

start = text.index("void \nRestoreIndependentResources( void )")
function = text[start:text.index("\n}\n", start) + 3]
function = function.replace("&(smGD.display->db)", "&fake_db")
arrays = re.search(r"^char \*fonttype\[\] = \{.*?\};\n", text, re.M | re.S).group(0) + \
    re.search(r"^static char \*fontclass\[\] = \{.*?\};\n", text, re.M | re.S).group(0)
names = "".join(re.findall(r"^char SM_(?:HOME_FONT_DIRECTORY|CURRENT_FONT_DIRECTORY|FONT_FILE|SYSTEM_FONT_FILE|LOW_RES_EXT|MED_RES_EXT|HIGH_RES_EXT)\[\] *= *\"[^\"]*\";\n", strdefs, re.M))

harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <X11/Intrinsic.h>
#include <X11/Xatom.h>

#define MED_RES_Y_RES 3150
#define HIGH_RES_Y_RES 3583
#define LOW_RES_Y_RES 1
#define HOME_SESSION 1
#define DtError 1
#define _DT_ATR_PREFS 1
#define SM_MALLOC(n) malloc(n)
#define SM_FREE(p) free(p)
#define GETMESSAGE(set, n, s) (s)
#define XmNdefaultFontList "defaultFontList"
typedef void *XmFontList;

''' + names + arrays + r'''
static struct {
    char *savePath, *sessionLang;
    char fontPath[MAXPATHLEN + 1];
    short sessionType;
    Display *display;
    void *topLevelWid;
} smGD;
static struct { int displayResolution; } smRes;
static Arg uiArgs[8];
static XrmDatabase fake_db;
static char *resource_manager;
static int errors, merges, font_lists, added;
static char merged_file[MAXPATHLEN * 2], added_resources[16][128], seen_font_list[64];

#define DisplayHeight(d, s) 1000
#define DisplayHeightMM(d, s) 250
#define RootWindow(d, s) 1
static void FixPath(char *p) { (void) p; }
static void PrintError(int type, const char *m) { (void) type; (void) m; errors++; }
static int RestoreResources(Boolean handler, ...)
{
    va_list args; const char *a;
    merges++;
    va_start(args, handler);
    while ((a = va_arg(args, const char *)) != NULL)
        if (!strcmp(a, "-file")) snprintf(merged_file, sizeof merged_file, "%s", va_arg(args, const char *));
    va_end(args);
    return 0;
}
static int fake_XGetWindowProperty(Display *d, Window w, Atom a, long off, long len, Bool del, Atom req, Atom *type, int *format, unsigned long *n, unsigned long *left, unsigned char **data)
{
    (void) d; (void) w; (void) a; (void) off; (void) len; (void) del; (void) req; (void) format; (void) left;
    *type = XA_STRING;
    *data = resource_manager ? (unsigned char *) strdup(resource_manager) : NULL;
    *n = resource_manager ? strlen(resource_manager) : 0;
    return Success;
}
static void fake_XtSetValues(Widget w, ArgList args, Cardinal n) { (void) w; (void) n; font_lists++; snprintf(seen_font_list, sizeof seen_font_list, "%s", (char *) args[0].value); }
static void fake_DtAddResString(Display *d, char *string, int where) { (void) d; (void) where; if (added < 16) snprintf(added_resources[added], 128, "%.127s", string); added++; assert(strlen(string) > 0); }
#define XGetWindowProperty fake_XGetWindowProperty
#define XtSetValues fake_XtSetValues
#define _DtAddResString fake_DtAddResString

''' + function + r'''
static char *work;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static void session(const char *session_lang, int resolution)
{
    static char save[MAXPATHLEN];
    snprintf(save, sizeof save, "%s/sessions", work);
    mkdir(save, 0755);
    smGD.savePath = save;
    smGD.sessionLang = session_lang ? strdup(session_lang) : NULL;
    smGD.sessionType = HOME_SESSION;
    smRes.displayResolution = resolution;
    resource_manager = NULL;
}
static void write_file(const char *path, const char *content)
{
    char *copy = strdup(path), *p;
    FILE *f;
    for (p = copy + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(copy, 0755); *p = '/'; }
    f = fopen(path, "w"); assert(f); fputs(content, f); fclose(f);
    free(copy);
}
static char *font_file(const char *lang)
{
    static char path[MAXPATHLEN * 2];
    snprintf(path, sizeof path, "%s/sessions/%s/%s/%s.%s", work, SM_HOME_FONT_DIRECTORY, lang, SM_FONT_FILE, SM_HIGH_RES_EXT);
    return path;
}

static int no_language_in_the_environment(void)
{
    session("de_DE.UTF-8", 4000);
    unsetenv("LANG");
    RestoreIndependentResources();
    CHECK(merges == 0);                      /* nothing to find: no file for "no language", and no crash */
    return 0;
}
static int no_saved_language(void)
{
    session(NULL, 5000);                      /* a saved resolution that is not this display's, and no language */
    setenv("LANG", "C", 1);
    RestoreIndependentResources();
    CHECK(merges == 0);
    session(NULL, 0);
    RestoreIndependentResources();            /* nothing saved at all: nothing to do */
    return 0;
}
static int language_as_long_as_it_can_be(void)
{
    static char lang[9000];
    memset(lang, 'l', sizeof lang - 1); lang[sizeof lang - 1] = 0;
    session("de_DE.UTF-8", 4000);
    setenv("LANG", lang, 1);
    RestoreIndependentResources();
    CHECK(merges == 0);
    lang[MAXPATHLEN - 12] = 0;                /* and just short of what fits */
    setenv("LANG", lang, 1);
    RestoreIndependentResources();
    return 0;
}
static int no_session_directory(void)
{
    /* the session directories could not be made: there is no path to look in */
    session("de_DE.UTF-8", 4000);
    smGD.savePath = NULL;
    setenv("LANG", "C", 1);
    {
        char planted[MAXPATHLEN];                 /* where a null directory printed as text would be looked for */
        snprintf(planted, sizeof planted, "(null)/%s/C/%s.%s", SM_HOME_FONT_DIRECTORY, SM_FONT_FILE, SM_HIGH_RES_EXT);
        write_file(planted, "x");
    }
    RestoreIndependentResources();
    CHECK(merges == 0);
    return 0;
}
static int user_font_file_is_merged(void)
{
    setenv("LANG", "de_DE.UTF-8", 1);
    session("C", 4000);
    write_file(font_file("de_DE.UTF-8"), "*fontList: x\n");
    resource_manager = "*fontList: -adobe-helvetica\n*systemFont: -misc-fixed\n*userFont: -misc-user\n";
    RestoreIndependentResources();
    CHECK(merges == 1 && !strcmp(merged_file, smGD.fontPath) && !strcmp(merged_file, font_file("de_DE.UTF-8")));
    CHECK(font_lists == 1 && !strcmp(seen_font_list, "-adobe-helvetica"));
    CHECK(added >= 2);                                                /* the font resources go into the preferences */
    return 0;
}
static int no_font_list_resource(void)
{
    setenv("LANG", "de_DE.UTF-8", 1);
    session("C", 4000);
    write_file(font_file("de_DE.UTF-8"), "x\n");
    resource_manager = "*systemFont: -misc-fixed\n";                  /* there is no *fontList */
    RestoreIndependentResources();
    CHECK(merges == 1);
    CHECK(font_lists == 0);                                           /* no garbage is set as the default font list */
    CHECK(added == 1 && !strcmp(added_resources[0], "*systemFont: -misc-fixed\n"));
    return 0;
}
static int very_long_font_resource(void)
{
    static char big[100000], resources[100100];
    memset(big, 'f', sizeof big - 1); big[sizeof big - 1] = 0;
    setenv("LANG", "de_DE.UTF-8", 1);
    session("C", 4000);
    write_file(font_file("de_DE.UTF-8"), "x\n");
    snprintf(resources, sizeof resources, "*fontList: short\n*systemFont: %s\n", big);
    resource_manager = resources;
    RestoreIndependentResources();
    CHECK(merges == 1 && added == 1);
    return 0;
}
static int no_resource_manager_property(void)
{
    setenv("LANG", "de_DE.UTF-8", 1);
    session("C", 4000);
    write_file(font_file("de_DE.UTF-8"), "x\n");
    resource_manager = NULL;                                          /* the property is not there */
    RestoreIndependentResources();
    CHECK(merges == 1 && font_lists == 0 && errors == 1);
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"no_language_in_the_environment", no_language_in_the_environment},
    {"no_saved_language", no_saved_language},
    {"language_as_long_as_it_can_be", language_as_long_as_it_can_be},
    {"no_session_directory", no_session_directory},
    {"user_font_file_is_merged", user_font_file_is_merged},
    {"no_font_list_resource", no_font_list_resource},
    {"very_long_font_resource", very_long_font_resource},
    {"no_resource_manager_property", no_resource_manager_property},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    if (argc < 3) { for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    work = argv[2];
    if (chdir(work) != 0) return 3;
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
with tempfile.TemporaryDirectory(prefix="dtsession-startup-", dir=os.environ.get("TMPDIR")) as scratch:
    scratch = Path(scratch)
    c = scratch / "startup.c"
    c.write_text(harness)
    binary = scratch / "startup"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-w", str(c), "-o", str(binary), "-lXt", "-lX11"]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr[:6000])
        sys.exit("cannot compile the startup resources harness")
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True, env=env).stdout.split()
    for name in names:
        work = scratch / ("work-" + name)
        work.mkdir()
        run = subprocess.run([str(binary), name, str(work)], capture_output=True, text=True, timeout=60, env=env)
        if run.returncode:
            failed.append(name)
            lines = [l for l in run.stderr.strip().splitlines() if l.strip()]
            sys.stderr.write("%s failed: %s\n" % (name, lines[0] if lines else "exit %d (signal %d)" % (run.returncode, -run.returncode)))
            if os.environ.get("DTSESSION_VERBOSE"):
                sys.stderr.write("\n".join(lines[:25]) + "\n")
if failed:
    sys.exit("%d of %d startup resource scenarios failed: %s" % (len(failed), len(names), ", ".join(failed)))
print("Startup resources: %d scenarios passed" % len(names))
