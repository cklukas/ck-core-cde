/*
 * Fuzz target: the palette reader of the dtsession color server.
 *
 * run-fuzz.py extracts GetPaletteDefinition, FindPalette, ReadPaletteFile,
 * ParsePaletteInfo, InitializeBW and SwitchAItoPS verbatim from
 * dtsession/SrvFile_io.c.  The stand-ins are Xlib's colour parser and the
 * Xrm resource lookup, the colour calculation of Motif, and the palette file:
 * open() of the palette returns a descriptor over the fuzz input.
 *
 * Input layout: byte 0 selects the setup, the rest is the data.
 *   bit 7 clear  the data is the content of a palette file; bits 0-1 pick
 *                the monitor type, bits 2-3 the foreground rule, bit 4 the
 *                pixmap switch, bits 5-6 the palette name
 *   bit 7 set    the data is the Dtstyle.paletteDirectories resource and the
 *                palette file is a fixed valid one
 */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <X11/Intrinsic.h>

#define XmCO_MAX_NUM_COLORS 8
enum { XmCO_BLACK_WHITE, XmCO_LOW_COLOR, XmCO_MEDIUM_COLOR, XmCO_HIGH_COLOR };
#define MAX_NUM_SCREENS 6
#define DtWarning 1
#define XmSCOLOR_SRV_NAME "colorserver"
#define GETMESSAGE(set, n, s) (s)

/* The allocations of one input are recorded and released at its end: the
   reader does not free a palette it rejects, and the process would grow. */
static void *blocks[1024];
static int block_count;
static void forget(void *p)
{
    for (int i = 0; i < block_count; i++)
        if (blocks[i] == p) { blocks[i] = blocks[--block_count]; return; }
}
static void *srv_malloc(size_t n)
{
    void *p = malloc(n);
    if (!p || block_count == 1024) abort();
    blocks[block_count++] = p;
    return p;
}
static void *srv_realloc(void *old, size_t n)
{
    void *p;
    if (old) forget(old);
    p = realloc(old, n);
    if (!p || block_count == 1024) abort();
    blocks[block_count++] = p;
    return p;
}
static void srv_free(void *p) { forget(p); free(p); }
#define SRV_MALLOC(n) srv_malloc(n)
#define SRV_REALLOC(p, n) srv_realloc(p, n)
#define SRV_FREE(p) srv_free(p)

#include FUZZ_TYPES_H

typedef struct {
   int TypeOfMonitor[MAX_NUM_SCREENS];
   int FgColor[MAX_NUM_SCREENS];
   Bool UsePixmaps[MAX_NUM_SCREENS];
} ColorSrv;
static ColorSrv colorSrv;

static char *directories_resource;
static int palette_fd = -1;

static void _DtSimpleError(const char *name, int type, const char *a, const char *message, const char *b)
{ (void)name; (void)type; (void)a; (void)message; (void)b; }
static XrmDatabase fake_XtDatabase(Display *d) { (void)d; return NULL; }
static Bool fake_XrmGetResource(XrmDatabase db, const char *name, const char *class_, char **type, XrmValue *value)
{
    (void)db; (void)class_;
    if (directories_resource == NULL || strcmp(name, "dtstyle.paletteDirectories")) return False;
    *type = "String";
    value->addr = directories_resource;
    value->size = strlen(directories_resource) + 1;
    return True;
}
/* Xlib parses many colour notations; "#rrggbb" is the one the palettes use. */
static Status fake_XParseColor(Display *d, Colormap c, const char *spec, XColor *color)
{
    unsigned v;
    (void)d; (void)c;
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

/* The palette file: a descriptor over the input.  A reader that finds a bad
   palette unlinks a path; nothing is removed here. */
static int fuzz_open(const char *path, int flags, ...)
{
    (void)path; (void)flags;
    if (lseek(palette_fd, 0, SEEK_SET) < 0) return -1;
    return dup(palette_fd);
}
#define open fuzz_open
#define unlink(p) (0)

static int error_value;

#include FUZZ_PRODUCTION_H
#undef open
#undef unlink

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    FILE *f = tmpfile();            /* in TMPDIR, which run-fuzz.py points into the scratch directory */
    if (!f) abort();
    palette_fd = dup(fileno(f));
    setenv("HOME", "/nonexistent-home", 1);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char *names[4] = {"Mine.dp", W_O_B ".dp", B_ONLY ".dp", "x"};
    static const char valid[] = "#101010\n#202020\n#303030\n#404040\n#505050\n#606060\n#707070\n#808080\n!end\n";
    if (size == 0) return 0;
    uint8_t ctl = data[0];
    data++; size--;

    colorSrv.TypeOfMonitor[0] = ctl & 3;
    colorSrv.FgColor[0] = (ctl >> 2) & 3;
    colorSrv.UsePixmaps[0] = (ctl >> 4) & 1;

    struct _palette *p;
    if (ctl & 0x80) {
        char *resource = malloc(size + 1);
        memcpy(resource, data, size);
        resource[size] = 0;
        directories_resource = resource;
        if (ftruncate(palette_fd, 0) || pwrite(palette_fd, valid, sizeof valid - 1, 0) < 0) abort();
        p = GetPaletteDefinition(NULL, 0, (char *)"Mine.dp");
        directories_resource = NULL;
        free(resource);
    } else {
        if (ftruncate(palette_fd, 0) || (size && pwrite(palette_fd, data, size, 0) < 0)) abort();
        p = ReadPaletteFile(NULL, 0, (char *)"/nonexistent-palettes", (char *)names[(ctl >> 5) & 3]);
    }
    (void)p;
    while (block_count > 0) free(blocks[--block_count]);
    return 0;
}
