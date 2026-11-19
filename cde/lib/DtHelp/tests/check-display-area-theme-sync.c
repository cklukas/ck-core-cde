/*
 * Verify DtHelp display-area live color/decor sync and child-resize handling.
 */
/* @@PROLOGUE@@ */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long Pixel;
typedef unsigned long Pixmap;
typedef unsigned long Window;
typedef unsigned short Dimension;
typedef void *XtPointer;
typedef char Boolean;

#define True 1
#define False 0
#define None 0UL
#define XmUNSPECIFIED_PIXMAP 2UL
#define Expose 12
#define VisibilityFullyObscured 2
#define XmCR_EXPOSE 38
#define XmCR_RESIZE 39

#define XmNforeground "foreground"
#define XmNbackground "background"
#define XmNhighlightColor "highlightColor"
#define XmNshadowThickness "shadowThickness"
#define XmNhighlightThickness "highlightThickness"
#define XmNwidth "width"
#define XmNheight "height"

typedef struct _Display {
    int dummy;
} Display;

typedef struct _Screen {
    int dummy;
} Screen;

typedef struct _GC {
    Pixel foreground;
    Pixel background;
    Pixmap tile;
} GCRec, *GC;

typedef struct {
    Pixel foreground;
    Pixel background;
    Pixmap tile;
} XGCValues;

#define GCForeground (1UL << 2)
#define GCBackground (1UL << 3)
#define GCTile       (1UL << 10)

typedef struct {
    const char *name;
    uintptr_t value;
} Arg;

typedef unsigned int Cardinal;

#define XtSetArg(arg, n, d) \
    do { \
        (arg).name = (n); \
        (arg).value = (uintptr_t)(d); \
    } while (0)

typedef struct _WidgetRec {
    struct _WidgetRec *parent;
    Display *dpy;
    Screen *scr;
    Window win;
    Boolean realized;
    Dimension width;
    Dimension height;
    Dimension shadow_thickness;
    Dimension highlight_thickness;
    Pixel fg;
    Pixel bg;
    Pixel hl;
} WidgetRec, *Widget;

typedef struct {
    int type;
    struct {
        int count;
    } xexpose;
} XEvent;

typedef struct {
    int reason;
    XEvent *event;
    Window window;
} XmDrawnButtonCallbackStruct;

typedef int _DtCvUnit;
typedef int _DtCvRenderType;
typedef void *_DtCvHandle;

#define _DtCvTRUE 1
#define _DtCvRENDER_PARTIAL 1
#define _DtCvTRAVERSAL_OFF 0
#define _DtCvTRAVERSAL_ON 1

#define _DT_HELP_TOC_ON (1 << 5)
#define _DT_HELP_FOCUS_FLAG 0x04
#define _DT_HELP_IN_SET_SCROLLBARS (1 << 8)

typedef struct _dtHelpDispAreaStruct {
    Widget dispWid;
    short neededFlags;
    Dimension formWidth;
    Dimension formHeight;
    Dimension dispWidth;
    Dimension dispHeight;
    Dimension dispUseHeight;
    Dimension dispUseWidth;
    short decorThickness;
    Pixel traversalColor;
    Pixel foregroundColor;
    Pixel backgroundColor;
    GC pixmapGC;
    GC normalGC;
    GC invertGC;
    Pixmap def_pix;
    int firstVisible;
    int nextNonVisible;
    int virtualX;
    _DtCvUnit toc_height;
    _DtCvUnit toc_y;
    short toc_flag;
    _DtCvHandle canvas;
    _DtCvRenderType render_type;
} DtHelpDispAreaStruct;

static int g_clear_area_calls;
static int g_canvas_render_calls;
static int g_set_scrollbars_calls;
static int g_destroyed_pixmaps;
static int g_nested_resize_during_set_scrollbars;
static Widget g_nested_resize_widget;

static Display *XtDisplay(Widget w) { return w->dpy; }
static Screen *XtScreen(Widget w) { return w->scr; }
static Window XtWindow(Widget w) { return w->win; }
static Widget XtParent(Widget w) { return w->parent; }
static Boolean XtIsRealized(Widget w) { return w->realized; }

static void
XtGetValues(Widget w, Arg *args, Cardinal count)
{
    for (Cardinal i = 0; i < count; ++i) {
        if (strcmp(args[i].name, XmNforeground) == 0)
            *(Pixel *)args[i].value = w->fg;
        else if (strcmp(args[i].name, XmNbackground) == 0)
            *(Pixel *)args[i].value = w->bg;
        else if (strcmp(args[i].name, XmNhighlightColor) == 0)
            *(Pixel *)args[i].value = w->hl;
        else if (strcmp(args[i].name, XmNshadowThickness) == 0)
            *(Dimension *)args[i].value = w->shadow_thickness;
        else if (strcmp(args[i].name, XmNhighlightThickness) == 0)
            *(Dimension *)args[i].value = w->highlight_thickness;
        else if (strcmp(args[i].name, XmNwidth) == 0)
            *(Dimension *)args[i].value = w->width;
        else if (strcmp(args[i].name, XmNheight) == 0)
            *(Dimension *)args[i].value = w->height;
    }
}

static Pixmap
XmGetPixmap(Screen *scr, const char *name, Pixel fg, Pixel bg)
{
    (void)scr;
    (void)name;
    return ((fg & 0xffffUL) << 16) | (bg & 0xffffUL) | 0x10000000UL;
}

static Boolean
XmDestroyPixmap(Screen *scr, Pixmap pix)
{
    (void)scr;
    (void)pix;
    ++g_destroyed_pixmaps;
    return True;
}

static void
XFreePixmap(Display *dpy, Pixmap pix)
{
    (void)dpy;
    (void)pix;
}

static void
XChangeGC(Display *dpy, GC gc, unsigned long mask, XGCValues *v)
{
    (void)dpy;
    if (mask & GCForeground)
        gc->foreground = v->foreground;
    if (mask & GCBackground)
        gc->background = v->background;
    if (mask & GCTile)
        gc->tile = v->tile;
}

static void
XClearArea(Display *dpy, Window win, int x, int y,
           unsigned int w, unsigned int h, Boolean exposures)
{
    (void)dpy;
    (void)win;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)exposures;
    ++g_clear_area_calls;
}

static void
_DtCanvasMoveTraversal(_DtCvHandle canvas, int cmd, Boolean wrap, Boolean draw,
                       void *a, void *b, void *c, void *d, void *e)
{
    (void)canvas;
    (void)cmd;
    (void)wrap;
    (void)draw;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
}

static void
_DtCanvasRender(_DtCvHandle canvas, _DtCvUnit x1, _DtCvUnit y1,
                _DtCvUnit x2, _DtCvUnit y2, _DtCvRenderType rtype,
                int flag, void *a, _DtCvUnit *next_y)
{
    (void)canvas;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)rtype;
    (void)flag;
    (void)a;
    ++g_canvas_render_calls;
    if (next_y)
        *next_y = y2;
}

static void
_DtHelpDATocMarker(XtPointer client_data, Boolean draw)
{
    (void)client_data;
    (void)draw;
}

static void
UpdateLinkCursorFromPointer(Widget w, DtHelpDispAreaStruct *pDAS)
{
    (void)w;
    (void)pDAS;
}

void _DtHelpResizeCB(Widget widget, XtPointer client_data, XtPointer call_data);

static Boolean
_DtHelpSetScrollBars(XtPointer client_data, Dimension new_w, Dimension new_h)
{
    DtHelpDispAreaStruct *pDAS = (DtHelpDispAreaStruct *)client_data;
    Boolean was_in = (pDAS->neededFlags & _DT_HELP_IN_SET_SCROLLBARS) ? True : False;

    ++g_set_scrollbars_calls;
    pDAS->neededFlags |= _DT_HELP_IN_SET_SCROLLBARS;

    if (g_nested_resize_during_set_scrollbars && g_nested_resize_widget != NULL) {
        XmDrawnButtonCallbackStruct nested_cb = {XmCR_RESIZE, NULL, g_nested_resize_widget->win};
        g_nested_resize_during_set_scrollbars = 0;
        _DtHelpResizeCB(g_nested_resize_widget, client_data, &nested_cb);
    }

    pDAS->dispWidth = new_w;
    pDAS->dispHeight = new_h;
    pDAS->dispUseWidth = (Dimension)(new_w - 2 * pDAS->decorThickness);
    pDAS->dispUseHeight = (Dimension)(new_h - 2 * pDAS->decorThickness);

    if (!was_in)
        pDAS->neededFlags &= ~(_DT_HELP_IN_SET_SCROLLBARS);
    return True;
}

/* @@EXTRACTED@@ */

int
main(void)
{
    Display dpy = {0};
    Screen scr = {0};
    WidgetRec form = {
        .parent = NULL, .dpy = &dpy, .scr = &scr, .win = 100, .realized = True,
        .width = 500, .height = 380
    };
    WidgetRec disp = {
        .parent = &form, .dpy = &dpy, .scr = &scr, .win = 101, .realized = True,
        .width = 485, .height = 380, .shadow_thickness = 2, .highlight_thickness = 1,
        .fg = 0x111111UL, .bg = 0xeeeeeeUL, .hl = 0xff8800UL
    };
    GCRec normal_gc = {disp.fg, disp.bg, 0};
    GCRec pixmap_gc = {disp.fg, disp.bg, 0};
    GCRec invert_gc = {disp.bg, disp.fg, 0};
    DtHelpDispAreaStruct das;
    memset(&das, 0, sizeof(das));
    das.dispWid = &disp;
    das.formWidth = 500;
    das.formHeight = 380;
    das.dispWidth = 485;
    das.dispHeight = 380;
    das.decorThickness = 3;
    das.dispUseWidth = 479;
    das.dispUseHeight = 374;
    das.foregroundColor = disp.fg;
    das.backgroundColor = disp.bg;
    das.traversalColor = disp.hl;
    das.normalGC = &normal_gc;
    das.pixmapGC = &pixmap_gc;
    das.invertGC = &invert_gc;
    das.def_pix = 99UL;
    das.render_type = _DtCvRENDER_PARTIAL;

    /* 1. Child dispWid resize inside unchanged parent form (scrollbar width change)
     *    plus nested resize re-entrancy guard. */
    disp.width = 482;
    g_nested_resize_during_set_scrollbars = 1;
    g_nested_resize_widget = &disp;
    XmDrawnButtonCallbackStruct resize_cb = {XmCR_RESIZE, NULL, disp.win};
    _DtHelpResizeCB(&disp, &das, &resize_cb);
    assert(g_set_scrollbars_calls == 1);
    assert(das.dispWidth == 482);
    assert(das.dispUseWidth == 476);
    assert(g_canvas_render_calls == 1);

    /* Subsequent Expose must render instead of being dropped due to stale dispWidth. */
    XEvent expose_ev = {.type = Expose, .xexpose = {.count = 0}};
    XmDrawnButtonCallbackStruct expose_cb = {XmCR_EXPOSE, &expose_ev, disp.win};
    _DtHelpExposeCB(&disp, &das, &expose_cb);
    assert(g_canvas_render_calls == 2);

    /* 2. Live color theme change (foreground/background/highlightColor SetValues)
     *    without geometry change updates cached pixels, GCs, tile, and def_pix. */
    disp.fg = 0xf8f9faUL;
    disp.bg = 0x283952UL;
    disp.hl = 0x527a94UL;
    _DtHelpExposeCB(&disp, &das, &expose_cb);
    assert(g_canvas_render_calls == 3);
    assert(das.foregroundColor == 0xf8f9faUL);
    assert(das.backgroundColor == 0x283952UL);
    assert(das.traversalColor == 0x527a94UL);
    assert(normal_gc.foreground == 0xf8f9faUL && normal_gc.background == 0x283952UL);
    assert(pixmap_gc.foreground == 0xf8f9faUL && pixmap_gc.background == 0x283952UL);
    assert(invert_gc.foreground == 0x283952UL && invert_gc.background == 0xf8f9faUL);
    assert(normal_gc.tile != 0);
    assert(das.def_pix == 0 && g_destroyed_pixmaps == 1);

    /* 3. Border thickness (shadowThickness + highlightThickness) change with same
     *    outer dispWid width/height recomputes dispUseWidth/dispUseHeight via SetScrollBars. */
    disp.shadow_thickness = 1;
    disp.highlight_thickness = 1;
    _DtHelpExposeCB(&disp, &das, &expose_cb);
    assert(das.decorThickness == 2);
    assert(g_set_scrollbars_calls == 2);
    assert(das.dispUseWidth == 478);
    assert(das.dispUseHeight == 376);
    assert(g_canvas_render_calls == 4);

    /* 4. Synthetic expose (callback.event == NULL) also redraws cleanly. */
    XmDrawnButtonCallbackStruct synth_expose_cb = {XmCR_EXPOSE, NULL, disp.win};
    _DtHelpExposeCB(&disp, &das, &synth_expose_cb);
    assert(g_canvas_render_calls == 5);

    puts("PASS DtHelp display-area theme sync and child-resize regression");
    return 0;
}
