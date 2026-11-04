#!/usr/bin/env python3
"""Feed crafted palette files and palette directory lists to the color server.

dtsession's color server reads the palette named by the style manager from the
directories of the Dtstyle.paletteDirectories resource and from
$HOME/.dt/palettes.  The production functions of SrvFile_io.c (the directory
list, FindPalette, ReadPaletteFile and ParsePaletteInfo) are cut out of the
source and compiled, under AddressSanitizer and UndefinedBehaviorSanitizer,
with stand-ins for Xlib and Xrm.  The input is what a resource or a palette
file can hold: directory names longer than the buffer for them, several
directories separated by blanks, more colors than the table has, lines without
a newline and a palette with fewer colors than a low color display needs.
Every scenario runs in its own process and gets ten seconds.

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
text = (source / "SrvFile_io.c").read_text()
header = (source / "Srv.h").read_text()


def function(signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 3] + "\n"


defines = text[text.index("#define BUFSIZE"):text.index("/* Static Function Declarations */")]
defines += text[text.index("#define HOME"):text.index('#define DEFAULT_PALETTE')]
defines += '#define DEFAULT_PALETTE    "Default.dp"\nvoid SwitchAItoPS(struct _palette *);\n'
prototypes = text[text.index("/* Static Function Declarations */"):text.index("/* static variables */")]
types = re.search(r"typedef struct _colorset \{.*?\} ColorSet;", header, re.S).group(0) + "\n"
types += re.search(r"typedef struct _palette \{.*?\} palettes;", header, re.S).group(0) + "\n"
srv_defines = "".join(re.findall(r'^#define (?:DYNAMIC|BLACK|WHITE|PALETTE_SUFFIX|B_O_W|W_ONLY|W_O_B|B_ONLY) .*\n', header, re.M))
code = "".join(function(s) for s in (
    "struct _palette * \nGetPaletteDefinition(", "static Boolean\nFindPalette(", "static struct _palette * \nReadPaletteFile(",
    "static int \nParsePaletteInfo(", "static void \nInitializeBW(", "void \nSwitchAItoPS("))

harness = r'''
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <X11/Intrinsic.h>

#define XmCO_MAX_NUM_COLORS 8
enum { XmCO_BLACK_WHITE, XmCO_LOW_COLOR, XmCO_MEDIUM_COLOR, XmCO_HIGH_COLOR };
#define MAX_NUM_SCREENS 6
#define DtWarning 1
#define XmSCOLOR_SRV_NAME "colorserver"
#define GETMESSAGE(set, n, s) (s)
#define SRV_MALLOC(n) malloc(n)
#define SRV_REALLOC(p, n) realloc(p, n)
#define SRV_FREE(p) free(p)
''' + srv_defines + types + r'''
typedef struct {
   int TypeOfMonitor[MAX_NUM_SCREENS];
   int FgColor[MAX_NUM_SCREENS];
   Bool UsePixmaps[MAX_NUM_SCREENS];
} ColorSrv;
static ColorSrv colorSrv;

static char *directories_resource;
static void _DtSimpleError(const char *name, int type, const char *a, const char *message, const char *b) { (void) name; (void) type; (void) a; (void) message; (void) b; }
static XrmDatabase fake_XtDatabase(Display *d) { (void) d; return NULL; }
static Bool fake_XrmGetResource(XrmDatabase db, const char *name, const char *class_, char **type, XrmValue *value)
{
    (void) db; (void) class_;
    if (directories_resource == NULL || strcmp(name, "dtstyle.paletteDirectories")) return False;
    *type = "String";
    value->addr = directories_resource;
    value->size = strlen(directories_resource) + 1;
    return True;
}
static Status fake_XParseColor(Display *d, Colormap c, const char *spec, XColor *color)
{
    unsigned v;
    (void) d; (void) c;
    if (sscanf(spec, "#%6x", &v) != 1) return 0;
    color->red = (v >> 16 & 0xff) * 257; color->green = (v >> 8 & 0xff) * 257; color->blue = (v & 0xff) * 257;
    return 1;
}
static void calc(XColor *bg, XColor *fg, XColor *sel, XColor *ts, XColor *bs) { *fg = *sel = *ts = *bs = *bg; }
typedef void (*XmColorProc)(XColor *, XColor *, XColor *, XColor *, XColor *);
static XmColorProc fake_XmGetColorCalculation(void) { return calc; }
#define XtDatabase fake_XtDatabase
#define XrmGetResource fake_XrmGetResource
#define XParseColor fake_XParseColor
#define XmGetColorCalculation fake_XmGetColorCalculation
#undef DefaultColormap
#define DefaultColormap(d, s) 0

''' + defines + prototypes + r'''
static int error_value;

''' + code + r'''
/* --- scenarios --- */
static char *work;
static char path_buffer[8][PATH_MAX * 2];
static int path_next;
static char *under(const char *format, ...)
{
    va_list args;
    char *p = path_buffer[path_next++ % 8];
    char rel[PATH_MAX];
    va_start(args, format);
    vsnprintf(rel, sizeof rel, format, args);
    va_end(args);
    snprintf(p, PATH_MAX * 2, "%s/%s", work, rel);
    return p;
}
static void make(const char *dir)
{
    char *copy = strdup(dir), *p;
    for (p = copy + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(copy, 0755); *p = '/'; }
    mkdir(copy, 0755);
    free(copy);
}
static void put(const char *dir, const char *name, const char *content, size_t length)
{
    char file[PATH_MAX * 2];
    FILE *f;
    make(dir);
    snprintf(file, sizeof file, "%s/%s", dir, name);
    f = fopen(file, "w");
    assert(f);
    fwrite(content, 1, length, f);
    fclose(f);
}
static const char *eight = "#101010\n#202020\n#303030\n#404040\n#505050\n#606060\n#707070\n#808080\n!end\n";
static struct _palette *load(const char *name)
{
    alarm(10);
    return GetPaletteDefinition(NULL, 0, (char *) name);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int normal_palette(void)
{
    struct _palette *p;
    put(under("home/.dt/palettes"), "Mine.dp", eight, strlen(eight));
    setenv("HOME", under("home"), 1);
    colorSrv.TypeOfMonitor[0] = XmCO_HIGH_COLOR; colorSrv.FgColor[0] = DYNAMIC;
    p = load("Mine.dp");
    CHECK(p != NULL && p->num_of_colors == 8 && !strcmp(p->name, "Mine"));
    CHECK(p->color[7].bg.red == 0x80 * 257);
    return 0;
}
static int blank_separated_directories(void)
{
    struct _palette *p;
    static char resource[2000];
    put(under("dirB"), "Other.dp", eight, strlen(eight));
    make(under("dirA"));
    setenv("HOME", under("nohome"), 1);
    colorSrv.TypeOfMonitor[0] = XmCO_HIGH_COLOR;
    snprintf(resource, sizeof resource, "%s  %s ", under("dirA"), under("dirB"));
    directories_resource = resource;
    p = load("Other.dp");
    CHECK(p != NULL && p->num_of_colors == 8);
    return 0;
}
static int long_directory_names(void)
{
    struct _palette *p;
    static char resource[4000], name[600];
    memset(name, 'n', sizeof name - 1); name[sizeof name - 1] = 0;
    put(under("long"), "Found.dp", eight, strlen(eight));
    setenv("HOME", under("nohome"), 1);
    colorSrv.TypeOfMonitor[0] = XmCO_HIGH_COLOR;
    snprintf(resource, sizeof resource, "/%s %s", name, under("long"));   /* an entry that does not fit, then a good one */
    directories_resource = resource;
    p = load("Found.dp");
    CHECK(p != NULL);
    snprintf(resource, sizeof resource, "%s", under("long"));          /* a directory name longer than the system directory's */
    p = load("Found.dp");
    CHECK(p != NULL && p->num_of_colors == 8);
    return 0;
}
static int more_colors_than_the_table(void)
{
    char content[1000] = "";
    int i;
    for (i = 0; i < 40; i++) strcat(content, "#abcdef\n");
    strcat(content, "!\n");
    put(under("home/.dt/palettes"), "Many.dp", content, strlen(content));
    setenv("HOME", under("home"), 1);
    colorSrv.TypeOfMonitor[0] = XmCO_HIGH_COLOR;
    CHECK(load("Many.dp") != NULL);
    return 0;
}
static int line_without_newline(void)
{
    char content[1100];
    struct _palette *p;
    memset(content, '#', sizeof content);
    put(under("home/.dt/palettes"), "Long.dp", content, 1000);
    put(under("home/.dt/palettes"), "Hex.dp", "#ffffff", 7);
    setenv("HOME", under("home"), 1);
    colorSrv.TypeOfMonitor[0] = XmCO_HIGH_COLOR;
    CHECK(load("Long.dp") == NULL);                             /* not a color */
    p = load("Hex.dp");
    CHECK(p != NULL && p->num_of_colors == 1);                  /* the last line need not end in a newline */
    return 0;
}
static int low_color_needs_four(void)
{
    put(under("home/.dt/palettes"), "Two.dp", "#111111\n#222222\n!\n", 17);
    setenv("HOME", under("home"), 1);
    colorSrv.TypeOfMonitor[0] = XmCO_LOW_COLOR;
    CHECK(load("Two.dp") == NULL);
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"normal_palette", normal_palette}, {"blank_separated_directories", blank_separated_directories},
    {"long_directory_names", long_directory_names}, {"more_colors_than_the_table", more_colors_than_the_table},
    {"line_without_newline", line_without_newline}, {"low_color_needs_four", low_color_needs_four},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    if (argc < 3) { for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    work = argv[2];
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
with tempfile.TemporaryDirectory(prefix="dtsession-palette-", dir=os.environ.get("TMPDIR")) as scratch:
    scratch = Path(scratch)
    (scratch / "usr" / "palettes").mkdir(parents=True)
    (scratch / "etc").mkdir()
    c = scratch / "palette.c"
    c.write_text("#include <stdarg.h>\n#include <sys/stat.h>\n" + harness)
    binary = scratch / "palette"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-w", '-DCDE_INSTALLATION_TOP="%s/usr"' % scratch,
                  '-DCDE_CONFIGURATION_TOP="%s/etc"' % scratch, str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr[:6000])
        sys.exit("cannot compile the palette harness")
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
if failed:
    sys.exit("%d of %d palette scenarios failed: %s" % (len(failed), len(names), ", ".join(failed)))
print("Palette parser: %d scenarios passed" % len(names))
