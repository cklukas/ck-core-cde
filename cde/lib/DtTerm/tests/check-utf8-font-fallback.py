#!/usr/bin/env python3
"""Verify DtTerm multibyte/UTF-8 XFontStruct rendering and XFontSet promotion."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def _ck_sanitize_list() -> str:
    override = os.environ.get("CK_SANITIZE_LIST", "").strip()
    if override:
        return override
    cc = shlex.split(os.environ.get("CC", "cc").strip() or "cc")[:1]
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
        src, exe = os.path.join(d, "p.c"), os.path.join(d, "p")
        Path(src).write_text("int main(void) { return 0; }\n", encoding="utf-8")
        for s in ("address,undefined", "undefined"):
            r = subprocess.run(cc + [f"-fsanitize={s}", src, "-o", exe], capture_output=True)
            if r.returncode == 0 and subprocess.run([exe], capture_output=True).returncode == 0:
                return s
    return "undefined"


here = Path(__file__).resolve().parent
dtterm_dir = here.parent
render_font_src = (dtterm_dir / "TermPrim" / "TermPrimRenderFont.c").read_text(encoding="utf-8")
term_prim_src = (dtterm_dir / "TermPrim" / "TermPrim.c").read_text(encoding="utf-8")

# 1. Verify TermPrim.c promotes XmFONT_IS_FONT to XFontSet in UTF-8 locales and
#    retains usable XFontSets when XLC_LOCALE reports missing optional charsets.
assert "fontType == XmFONT_IS_FONT" in term_prim_src, "TermPrim.c missing explicit XmFONT_IS_FONT guard"
assert "if (!fontSet && MB_CUR_MAX > 1)" in term_prim_src, "TermPrim.c missing MB_CUR_MAX > 1 XFontSet promotion"
assert "PromoteFontStructToFontSet(XtDisplay(w), font)" in term_prim_src
assert "CreateUsableFontSet(" in term_prim_src
assert "fixedSet && missingCount == 0" not in term_prim_src, "TermPrim.c still discards valid fixedSet on missingCount > 0"

# 2. Extract FontRenderFunction verbatim from TermPrimRenderFont.c and run it
#    under ASan/UBSan across mbCurMax == 1, mbCurMax == 6 (8-bit XFontStruct),
#    and mbCurMax == 6 (16-bit ISO10646-1 XFontStruct) + heap >256 wide chars.
start_marker = "static void\nFontRenderFunction("
end_marker = "\nstatic void\nFontDestroyFunction("
start_idx = render_font_src.find(start_marker)
end_idx = render_font_src.find(end_marker)
assert start_idx >= 0 and end_idx > start_idx, "Could not locate FontRenderFunction in TermPrimRenderFont.c"
func_body = render_font_src[start_idx:end_idx]

harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef unsigned long Pixel;
typedef unsigned long Font;
typedef unsigned long Window;
typedef unsigned long Drawable;
typedef void *XtPointer;
typedef int Boolean;
typedef struct _Display Display;
typedef struct _GC *GC;

typedef struct {
    unsigned char byte1;
    unsigned char byte2;
} XChar2b;

typedef struct {
    Pixel foreground;
    Pixel background;
    Font font;
} XGCValues;

#define GCForeground (1UL << 2)
#define GCBackground (1UL << 3)
#define GCFont       (1UL << 14)
#define False 0

typedef struct {
    Font fid;
    unsigned int max_byte1;
    int ascent;
    int descent;
} XFontStruct;

typedef struct _TermFontRec *TermFont;
typedef void (*TermFontRenderFunction)(void *, TermFont, Pixel, Pixel, unsigned long, int, int, unsigned char *, int);
typedef struct _TermFontRec {
    XtPointer fontInfo;
} TermFontRec;

struct renderGCState {
    GC gc;
    Pixel foreground;
    Pixel background;
    Font fid;
};

struct termData {
    int mbCurMax;
    int cellWidth;
    int cellHeight;
    struct renderGCState renderGC;
};

typedef struct {
    struct {
        struct termData *tpd;
        int widthInc;
    } term;
} DtTermPrimitiveRec, *DtTermPrimitiveWidget;

typedef DtTermPrimitiveWidget Widget;

#define TermIS_SECURE(f)     (((f) & 1UL) != 0)
#define TermIS_OVERSTRIKE(f) (((f) & 2UL) != 0)
#define TermIS_UNDERLINE(f)  (((f) & 4UL) != 0)
#define isDebugFSet(c, n)    0
#define shortSleep(us)       ((void)0)

static Display *XtDisplay(Widget w) { (void)w; return (Display *)0x1; }
static Window XtWindow(Widget w) { (void)w; return (Window)0x2; }
static char *XtMalloc(size_t n) { char *p = (char *)malloc(n ? n : 1); assert(p); return p; }
static void XtFree(char *p) { free(p); }
static int XChangeGC(Display *d, GC g, unsigned long m, XGCValues *v) { (void)d; (void)g; (void)m; (void)v; return 0; }
static int XFillRectangle(Display *d, Drawable w, GC g, int x, int y, unsigned int wi, unsigned int h) {
    (void)d; (void)w; (void)g; (void)x; (void)y; (void)wi; (void)h; return 0;
}
static int XSync(Display *d, int discard) { (void)d; (void)discard; return 0; }
static int XDrawLine(Display *d, Drawable w, GC g, int x1, int y1, int x2, int y2) {
    (void)d; (void)w; (void)g; (void)x1; (void)y1; (void)x2; (void)y2; return 0;
}

static char last_img8[1024];
static int last_img8_len;
static char last_str8[1024];
static int last_str8_len;
static XChar2b last_img16[1024];
static int last_img16_len;
static XChar2b last_str16[1024];
static int last_str16_len;

static int XDrawImageString(Display *d, Drawable w, GC g, int x, int y, const char *s, int len) {
    (void)d; (void)w; (void)g; (void)x; (void)y;
    assert(len >= 0 && len <= (int)sizeof(last_img8));
    memcpy(last_img8, s, (size_t)len);
    last_img8_len = len;
    return 0;
}

static int XDrawString(Display *d, Drawable w, GC g, int x, int y, const char *s, int len) {
    (void)d; (void)w; (void)g; (void)x; (void)y;
    assert(len >= 0 && len <= (int)sizeof(last_str8));
    memcpy(last_str8, s, (size_t)len);
    last_str8_len = len;
    return 0;
}

static int XDrawImageString16(Display *d, Drawable w, GC g, int x, int y, const XChar2b *s, int len) {
    (void)d; (void)w; (void)g; (void)x; (void)y;
    assert(len >= 0 && len <= (int)(sizeof(last_img16) / sizeof(last_img16[0])));
    memcpy(last_img16, s, (size_t)len * sizeof(XChar2b));
    last_img16_len = len;
    return 0;
}

static int XDrawString16(Display *d, Drawable w, GC g, int x, int y, const XChar2b *s, int len) {
    (void)d; (void)w; (void)g; (void)x; (void)y;
    assert(len >= 0 && len <= (int)(sizeof(last_str16) / sizeof(last_str16[0])));
    memcpy(last_str16, s, (size_t)len * sizeof(XChar2b));
    last_str16_len = len;
    return 0;
}

""" + func_body + r"""

int main(void) {
    struct termData tpd;
    memset(&tpd, 0, sizeof(tpd));
    tpd.cellWidth = 9;
    tpd.cellHeight = 18;

    DtTermPrimitiveRec wrec;
    memset(&wrec, 0, sizeof(wrec));
    wrec.term.tpd = &tpd;
    wrec.term.widthInc = 9;

    XFontStruct fs8 = { .fid = 101, .max_byte1 = 0, .ascent = 14, .descent = 4 };
    TermFontRec tf8 = { .fontInfo = &fs8 };

    /* Case 1: UTF-8 locale (mbCurMax = 6) with 8-bit XFontStruct must NEVER emit NUL bytes */
    tpd.mbCurMax = 6;
    const wchar_t wprompt[] = L"klukas@lima:\x03bb$";
    int wlen = (int)(sizeof(wprompt) / sizeof(wprompt[0])) - 1;
    last_img8_len = last_str8_len = -1;
    FontRenderFunction(&wrec, &tf8, 1, 0, 2UL /* overstrike */, 4, 4,
                       (unsigned char *)wprompt, wlen);
    assert(last_img8_len == wlen);
    assert(memcmp(last_img8, "klukas@lima:?$", (size_t)wlen) == 0);
    assert(last_str8_len == wlen);
    assert(memcmp(last_str8, "klukas@lima:?$", (size_t)wlen) == 0);

    /* Case 2: UTF-8 locale (mbCurMax = 6) with 16-bit ISO10646-1 XFontStruct */
    XFontStruct fs16 = { .fid = 102, .max_byte1 = 255, .ascent = 14, .descent = 4 };
    TermFontRec tf16 = { .fontInfo = &fs16 };
    last_img16_len = last_str16_len = -1;
    FontRenderFunction(&wrec, &tf16, 1, 0, 2UL, 4, 4,
                       (unsigned char *)wprompt, wlen);
    assert(last_img16_len == wlen);
    assert(last_img16[0].byte1 == 0x00 && last_img16[0].byte2 == 'k');
    assert(last_img16[12].byte1 == 0x03 && last_img16[12].byte2 == 0xbb);
    assert(last_str16_len == wlen);

    /* Case 3: Heap allocation path (> 256 wide chars) */
    wchar_t big[320];
    for (int i = 0; i < 320; ++i)
        big[i] = L'A' + (i % 26);
    FontRenderFunction(&wrec, &tf8, 1, 0, 0UL, 0, 0, (unsigned char *)big, 320);
    assert(last_img8_len == 320 && last_img8[0] == 'A' && last_img8[25] == 'Z');
    FontRenderFunction(&wrec, &tf16, 1, 0, 0UL, 0, 0, (unsigned char *)big, 320);
    assert(last_img16_len == 320 && last_img16[0].byte2 == 'A' && last_img16[25].byte2 == 'Z');

    /* Case 4: Single-byte C locale (mbCurMax = 1) */
    tpd.mbCurMax = 1;
    const char cprompt[] = "klukas@lima:~$";
    FontRenderFunction(&wrec, &tf8, 1, 0, 0UL, 0, 0,
                       (unsigned char *)cprompt, (int)strlen(cprompt));
    assert(last_img8_len == (int)strlen(cprompt));
    assert(memcmp(last_img8, cprompt, strlen(cprompt)) == 0);

    puts("PASS dtterm UTF-8 font fallback & XFontSet promotion");
    return 0;
}
"""

cc = shlex.split(os.environ.get("CC", "cc").strip() or "cc")
san = _ck_sanitize_list()
with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as tmp:
    c_path = Path(tmp) / "test_utf8_font.c"
    bin_path = Path(tmp) / "test_utf8_font"
    c_path.write_text(harness, encoding="utf-8")
    cmd = cc + ["-std=c23", "-Wall", "-Wextra", "-Werror", f"-fsanitize={san}",
                "-fno-sanitize-recover=all", str(c_path), "-o", str(bin_path)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stdout + res.stderr)
        sys.exit(1)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
               UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    run = subprocess.run([str(bin_path)], env=env, capture_output=True, text=True)
    if run.returncode != 0:
        sys.stderr.write(run.stdout + run.stderr)
        sys.exit(1)
    sys.stdout.write(run.stdout)
