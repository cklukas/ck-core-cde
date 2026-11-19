#include "padTabs.h"
#include "padFeatures.h"
#include "padSyntax.h"

#include <Dt/DtPStrings.h>
#include <Dt/SmCreateDirs.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <Xm/CascadeBG.h>
#include <Xm/DrawingA.h>
#include <Xm/FileSB.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/ScrollBar.h>
#include <Xm/SeparatoG.h>
#include <Xm/Text.h>
#include <Xm/ToggleB.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DTPAD_MAX_TABS 16

typedef struct {
    int tab_id;
    char *file_name;
    char *saved_text;
    Boolean file_exists;
    Boolean read_only;
    Boolean unsaved;
    Boolean recovered;
    XmTextPosition cursor_pos;
    char autosave_path[PATH_MAX];
} DtpadDocTab;

typedef struct _DtpadUiExt {
    Editor *pad;
    Widget work_form;
    Widget tab_bar_frame;
    Widget tab_row;
    Widget tab_buttons[DTPAD_MAX_TABS];
    Widget plus_btn;

    Widget body_form;
    Widget gutter_frame;
    Widget gutter_canvas;
    Widget text_widget;
    Widget vscroll_widget;

    Widget recent_menu;
    Widget recent_items[DTPAD_MAX_RECENT];
    char recent_paths[DTPAD_MAX_RECENT][PATH_MAX];
    int recent_count;

    Widget line_numbers_toggle;
    Widget syntax_toggle;
    Widget open_tab_dialog;

    DtpadDocTab tabs[DTPAD_MAX_TABS];
    int tab_count;
    int active_tab;
    int next_tab_id;
    Boolean switching_tabs;
    Boolean show_line_numbers;
    Boolean syntax_highlighting;
    DtpadSyntaxLang active_lang;
    Pixel syntax_pixels[2][DTPAD_TOK_COUNT];
    Boolean syntax_pixels_init[2];
    XtIntervalId syntax_timer;
    XtIntervalId gutter_timer;
    XmTextPosition last_gutter_top_pos;
    Boolean recovered_last_load;

    XtIntervalId autosave_timer;
    Atom state_atom;
    Atom ipc_atom;
    Atom utf8_atom;

    struct _DtpadUiExt *next;
} DtpadUiExt;

static DtpadUiExt *g_ext_head = NULL;

static DtpadUiExt *find_ext(Editor *pPad)
{
    for (DtpadUiExt *e = g_ext_head; e != NULL; e = e->next) {
        if (e->pad == pPad) return e;
    }
    return NULL;
}

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    size_t j = 0;
    if (src) {
        for (size_t i = 0; src[i] && j + 2 < dst_len; ++i) {
            unsigned char c = (unsigned char)src[i];
            if (c == '"' || c == '\\') {
                if (j + 3 >= dst_len) break;
                dst[j++] = '\\';
                dst[j++] = (char)c;
            } else if (c >= 32) {
                dst[j++] = (char)c;
            } else {
                dst[j++] = ' ';
            }
        }
    }
    dst[j] = '\0';
}

static char *get_editor_string_copy(Widget editor)
{
    if (!editor) return strdup("");
    Widget tw = XtNameToWidget(editor, "*text");
    if (tw && XmIsText(tw)) {
        char *xm_str = XmTextGetString(tw);
        if (xm_str) {
            char *res = strdup(xm_str);
            XtFree(xm_str);
            return res ? res : strdup("");
        }
    }
    DtEditorContentRec rec;
    memset(&rec, 0, sizeof(rec));
    rec.type = DtEDITOR_TEXT;
    if (DtEditorGetContents(editor, &rec, False, False) == DtEDITOR_NO_ERRORS && rec.value.string) {
        char *res = strdup(rec.value.string);
        XtFree(rec.value.string);
        return res ? res : strdup("");
    }
    return strdup("");
}

static void set_editor_string_with_dirty_state(Widget editor, const char *text, Boolean mark_dirty)
{
    if (!editor) return;
    DtEditorContentRec empty_rec;
    memset(&empty_rec, 0, sizeof(empty_rec));
    empty_rec.type = DtEDITOR_TEXT;
    empty_rec.value.string = (char *)(mark_dirty ? "" : (text ? text : ""));
    DtEditorSetContents(editor, &empty_rec);

    if (mark_dirty) {
        DtEditorContentRec dirty_rec;
        memset(&dirty_rec, 0, sizeof(dirty_rec));
        dirty_rec.type = DtEDITOR_TEXT;
        dirty_rec.value.string = (char *)(text ? text : "");
        DtEditorReplace(editor, 0, DtEditorGetLastPosition(editor), &dirty_rec);
    }
}

static void save_line_numbers_pref(Editor *pPad, Boolean enabled)
{
    if (!pPad || !pPad->display) return;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    _DtCreateDtDirs(pPad->display);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/dtpadrc", home, DtPERSONAL_CONFIG_DIRECTORY);
    XrmDatabase db = XrmGetFileDatabase(path);
    if (!db) db = XrmGetStringDatabase("");
    XrmPutStringResource(&db, "Dtpad*showLineNumbers", enabled ? "True" : "False");
    XrmPutFileDatabase(db, path);
}

static Boolean load_line_numbers_pref(Editor *pPad)
{
    if (!pPad || !pPad->display) return False;
    XrmDatabase db = XtDatabase(pPad->display);
    if (!db) return False;
    char *str_type = NULL;
    XrmValue val = {0, NULL};
    if (XrmGetResource(db, "dtpad.showLineNumbers", "Dtpad.ShowLineNumbers", &str_type, &val) &&
        val.addr) {
        if (strcasecmp((char *)val.addr, "true") == 0 || strcmp((char *)val.addr, "1") == 0) {
            return True;
        }
    }
    return False;
}

static void save_syntax_pref(Editor *pPad, Boolean enabled)
{
    if (!pPad || !pPad->display) return;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    _DtCreateDtDirs(pPad->display);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/dtpadrc", home, DtPERSONAL_CONFIG_DIRECTORY);
    XrmDatabase db = XrmGetFileDatabase(path);
    if (!db) db = XrmGetStringDatabase("");
    XrmPutStringResource(&db, "Dtpad*syntaxHighlighting", enabled ? "True" : "False");
    XrmPutFileDatabase(db, path);
}

static Boolean load_syntax_pref(Editor *pPad)
{
    if (!pPad || !pPad->display) return True;
    XrmDatabase db = XtDatabase(pPad->display);
    if (!db) return True;
    char *str_type = NULL;
    XrmValue val = {0, NULL};
    if (XrmGetResource(db, "dtpad.syntaxHighlighting", "Dtpad.SyntaxHighlighting", &str_type, &val) &&
        val.addr) {
        if (strcasecmp((char *)val.addr, "false") == 0 || strcmp((char *)val.addr, "0") == 0) {
            return False;
        }
        return True;
    }
    return True;
}

static Pixel alloc_hex_color(Display *dpy, Colormap cmap,
                             unsigned short r8, unsigned short g8, unsigned short b8,
                             Pixel fallback)
{
    XColor xc;
    memset(&xc, 0, sizeof(xc));
    xc.red = (unsigned short)(r8 * 257U);
    xc.green = (unsigned short)(g8 * 257U);
    xc.blue = (unsigned short)(b8 * 257U);
    xc.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(dpy, cmap, &xc))
        return xc.pixel;
    return fallback;
}

static void ensure_syntax_palette(DtpadUiExt *ext, int dark_bg, Pixel fallback_fg)
{
    Display *dpy = XtDisplay(ext->text_widget);
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    int slot = dark_bg ? 1 : 0;
    if (ext->syntax_pixels_init[slot])
        return;

    if (dark_bg) {
        /* Crisp high-contrast workstation syntax palette on dark slate / ModernDark */
        ext->syntax_pixels[slot][DTPAD_TOK_KEYWORD]  = alloc_hex_color(dpy, cmap, 0xFF, 0xD1, 0x66, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_TYPE]     = alloc_hex_color(dpy, cmap, 0x7E, 0xE8, 0xFA, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_STRING]   = alloc_hex_color(dpy, cmap, 0xA8, 0xF0, 0xA2, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_NUMBER]   = alloc_hex_color(dpy, cmap, 0xFF, 0xB4, 0xA2, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_COMMENT]  = alloc_hex_color(dpy, cmap, 0xC6, 0xD2, 0xE1, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_PREPROC]  = alloc_hex_color(dpy, cmap, 0xF0, 0xA6, 0xFF, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_FUNCTION] = alloc_hex_color(dpy, cmap, 0x9B, 0xF6, 0xFF, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_KEY]      = alloc_hex_color(dpy, cmap, 0xFF, 0xE5, 0x99, fallback_fg);
    } else {
        /* High-contrast syntax palette on light / paper backgrounds */
        ext->syntax_pixels[slot][DTPAD_TOK_KEYWORD]  = alloc_hex_color(dpy, cmap, 0x00, 0x33, 0xB3, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_TYPE]     = alloc_hex_color(dpy, cmap, 0x00, 0x62, 0x7A, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_STRING]   = alloc_hex_color(dpy, cmap, 0x06, 0x7D, 0x17, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_NUMBER]   = alloc_hex_color(dpy, cmap, 0x9E, 0x2A, 0x2B, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_COMMENT]  = alloc_hex_color(dpy, cmap, 0x5C, 0x67, 0x73, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_PREPROC]  = alloc_hex_color(dpy, cmap, 0x7B, 0x2C, 0xBF, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_FUNCTION] = alloc_hex_color(dpy, cmap, 0x00, 0x50, 0x9D, fallback_fg);
        ext->syntax_pixels[slot][DTPAD_TOK_KEY]      = alloc_hex_color(dpy, cmap, 0x7F, 0x4F, 0x24, fallback_fg);
    }
    ext->syntax_pixels_init[slot] = True;
}

static void apply_syntax_highlighting(DtpadUiExt *ext)
{
    if (!ext || !ext->text_widget || !XmIsText(ext->text_widget))
        return;

    if (!ext->syntax_highlighting) {
        ext->active_lang = DTPAD_SYNTAX_NONE;
        XmTextClearColorSpans(ext->text_widget);
        return;
    }

    char *full = XmTextGetString(ext->text_widget);
    if (!full) {
        ext->active_lang = DTPAD_SYNTAX_NONE;
        XmTextClearColorSpans(ext->text_widget);
        return;
    }

    const char *fname = (ext->pad && ext->pad->fileStuff.fileName)
                            ? ext->pad->fileStuff.fileName
                            : NULL;
    DtpadSyntaxLang lang = dtpad_syntax_detect(fname, full);
    ext->active_lang = lang;
    if (lang == DTPAD_SYNTAX_NONE) {
        XmTextClearColorSpans(ext->text_widget);
        XtFree(full);
        return;
    }

    Display *dpy = XtDisplay(ext->text_widget);
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    Pixel bg = 0, fg = 0;
    XtVaGetValues(ext->text_widget, XmNbackground, &bg, XmNforeground, &fg, NULL);
    XColor qbg;
    memset(&qbg, 0, sizeof(qbg));
    qbg.pixel = bg;
    XQueryColor(dpy, cmap, &qbg);
    double lum = (0.2126 * (double)qbg.red +
                  0.7152 * (double)qbg.green +
                  0.0722 * (double)qbg.blue) / 65535.0;
    int dark_bg = lum < 0.50 ? 1 : 0;
    ensure_syntax_palette(ext, dark_bg, fg);

    DtpadSyntaxSpan *lex_spans = NULL;
    size_t nspans = dtpad_syntax_lex(lang, full, strlen(full), &lex_spans);
    XtFree(full);

    if (nspans == 0 || !lex_spans) {
        free(lex_spans);
        XmTextClearColorSpans(ext->text_widget);
        return;
    }

    XmTextColorSpan *xm_spans = (XmTextColorSpan *)malloc(nspans * sizeof(*xm_spans));
    if (!xm_spans) {
        free(lex_spans);
        return;
    }
    for (size_t i = 0; i < nspans; ++i) {
        int kind = (int)lex_spans[i].kind;
        if (kind < 0 || kind >= DTPAD_TOK_COUNT)
            kind = 0;
        xm_spans[i].left = (XmTextPosition)lex_spans[i].start_pos;
        xm_spans[i].right = (XmTextPosition)lex_spans[i].end_pos;
        xm_spans[i].foreground = ext->syntax_pixels[dark_bg][kind];
    }
    XmTextSetColorSpans(ext->text_widget, xm_spans, (Cardinal)nspans);
    free(xm_spans);
    free(lex_spans);
}

static void on_syntax_debounce_timeout(XtPointer client, XtIntervalId *id)
{
    (void)id;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext) return;
    ext->syntax_timer = 0;
    apply_syntax_highlighting(ext);
}

static void schedule_syntax_refresh(DtpadUiExt *ext)
{
    if (!ext || !ext->pad || !ext->pad->app_context) return;
    if (ext->syntax_timer != 0)
        XtRemoveTimeOut(ext->syntax_timer);
    ext->syntax_timer = XtAppAddTimeOut(ext->pad->app_context, 35,
                                        on_syntax_debounce_timeout, ext);
}

static void draw_line_gutter(DtpadUiExt *ext)
{
    if (!ext || !ext->show_line_numbers || !ext->gutter_canvas ||
        !XtIsRealized(ext->gutter_canvas) || !ext->text_widget) {
        return;
    }

    Display *dpy = XtDisplay(ext->gutter_canvas);
    Window win = XtWindow(ext->gutter_canvas);
    if (!dpy || !win) return;

    Dimension gw = 0, gh = 0;
    Pixel bg = 0, fg = 0, bs = 0;
    XtVaGetValues(ext->gutter_canvas,
                  XmNwidth, &gw,
                  XmNheight, &gh,
                  XmNbackground, &bg,
                  XmNforeground, &fg,
                  XmNbottomShadowColor, &bs,
                  NULL);
    if (gw < 8 || gh < 8) return;

    XmRenderTable rt = NULL;
    XtVaGetValues(ext->text_widget, XmNrenderTable, &rt, NULL);

    int depth = DefaultDepthOfScreen(XtScreen(ext->gutter_canvas));
    Pixmap back_pm = XCreatePixmap(dpy, win, gw, gh, (unsigned int)depth);
    Drawable target = (back_pm != None) ? (Drawable)back_pm : (Drawable)win;

    GC gc = XCreateGC(dpy, target, 0, NULL);
    if (back_pm != None) {
        XSetForeground(dpy, gc, bg);
        XFillRectangle(dpy, back_pm, gc, 0, 0, gw, gh);
    } else {
        XSetWindowBackground(dpy, win, bg);
        XClearWindow(dpy, win);
    }

    /* Divider line on right edge */
    XSetForeground(dpy, gc, bs);
    XDrawLine(dpy, target, gc, (int)gw - 1, 0, (int)gw - 1, (int)gh);
    XSetForeground(dpy, gc, fg);
    XSetBackground(dpy, gc, bg);

    XmTextPosition top_pos = XmTextGetTopCharacter(ext->text_widget);
    if (top_pos < 0) top_pos = 0;
    ext->last_gutter_top_pos = top_pos;

    char *full = XmTextGetString(ext->text_widget);
    if (full) {
        size_t total_bytes = strlen(full);

        /* Advance through UTF-8 scalars up to top_pos and find the 1-based line number
         * and starting scalar position of the line containing top_pos. */
        int line_num = 1;
        long cur_scalar = 0;
        long line_start_scalar = 0;
        size_t line_start_byte = 0;
        size_t b = 0;
        while (b < total_bytes && cur_scalar < (long)top_pos) {
            unsigned char c = (unsigned char)full[b++];
            if (c >= 0x80U) {
                while (b < total_bytes && (((unsigned char)full[b] & 0xC0U) == 0x80U))
                    ++b;
            }
            ++cur_scalar;
            if (c == '\n') {
                ++line_num;
                line_start_scalar = cur_scalar;
                line_start_byte = b;
            }
        }

        int text_y_offset = 0;
        if (XtIsRealized(ext->text_widget)) {
            int dx = 0, dy = 0;
            Window child_ret = None;
            if (XTranslateCoordinates(dpy, XtWindow(ext->text_widget), win,
                                      0, 0, &dx, &dy, &child_ret)) {
                text_y_offset = dy;
            }
        }

        Position tx = 0, ty = 0;
        size_t pos_byte = line_start_byte;
        long pos_scalar = line_start_scalar;
        int prev_baseline_y = -10000;

        while (pos_byte <= total_bytes) {
            /* XmTextPosToXY returns *ty = exact glyph baseline in text_widget coords! */
            if (XmTextPosToXY(ext->text_widget, (XmTextPosition)pos_scalar, &tx, &ty)) {
                int baseline_y = (int)ty + text_y_offset;
                if (baseline_y > (int)gh + 24)
                    break;
                if (baseline_y >= 2 && baseline_y != prev_baseline_y) {
                    char num_buf[24];
                    snprintf(num_buf, sizeof(num_buf), "%d", line_num);
                    if (rt != NULL) {
                        XmString num_xms = XmStringCreateLocalized(num_buf);
                        Dimension str_bs = XmStringBaseline(rt, num_xms);
                        int top_y = baseline_y - (int)str_bs;
                        XmStringDraw(dpy, target, rt, num_xms, gc,
                                     2, (Position)top_y,
                                     (Dimension)(gw > 8 ? gw - 7 : gw),
                                     XmALIGNMENT_END,
                                     XmSTRING_DIRECTION_L_TO_R,
                                     NULL);
                        XmStringFree(num_xms);
                    } else {
                        XDrawString(dpy, target, gc, 4, baseline_y, num_buf, (int)strlen(num_buf));
                    }
                    prev_baseline_y = baseline_y;
                }
            }
            if (pos_byte >= total_bytes)
                break;
            /* Advance to next '\n' counting exact UTF-8 scalars */
            int found_nl = 0;
            while (pos_byte < total_bytes) {
                unsigned char c = (unsigned char)full[pos_byte++];
                if (c >= 0x80U) {
                    while (pos_byte < total_bytes && (((unsigned char)full[pos_byte] & 0xC0U) == 0x80U))
                        ++pos_byte;
                }
                ++pos_scalar;
                if (c == '\n') {
                    found_nl = 1;
                    ++line_num;
                    break;
                }
            }
            if (!found_nl)
                break;
        }
        XtFree(full);
    }

    if (back_pm != None) {
        XCopyArea(dpy, back_pm, win, gc, 0, 0, gw, gh, 0, 0);
        XFreePixmap(dpy, back_pm);
    }
    XFreeGC(dpy, gc);
    XFlush(dpy);
}

static void on_gutter_sync_timeout(XtPointer client, XtIntervalId *id)
{
    (void)id;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext) return;
    ext->gutter_timer = 0;
    if (!ext->switching_tabs && ext->show_line_numbers) {
        draw_line_gutter(ext);
    }
}

static void schedule_gutter_sync(DtpadUiExt *ext)
{
    if (!ext || !ext->show_line_numbers || !ext->pad || !ext->pad->app_context)
        return;
    if (ext->gutter_timer != 0)
        return;
    ext->gutter_timer = XtAppAddTimeOut(ext->pad->app_context, 0,
                                        on_gutter_sync_timeout, ext);
}

static void on_gutter_expose(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)call;
    draw_line_gutter((DtpadUiExt *)client);
}

static void on_text_changed(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)call;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext || ext->switching_tabs) return;
    draw_line_gutter(ext);
    schedule_syntax_refresh(ext);
    DtpadSyncActiveTabTitle(ext->pad);
}

static void on_text_scrolled(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)call;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext || ext->switching_tabs) return;
    draw_line_gutter(ext);
    schedule_gutter_sync(ext);
}

static void on_text_viewport_event(Widget w, XtPointer client, XEvent *ev, Boolean *cont)
{
    (void)w;
    (void)cont;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext || ext->switching_tabs || !ext->show_line_numbers || !ev)
        return;

    if (ev->type == ConfigureNotify || ev->type == MapNotify) {
        draw_line_gutter(ext);
        schedule_gutter_sync(ext);
        return;
    }

    if (ev->type == Expose || ev->type == GraphicsExpose ||
        ev->type == ButtonPress || ev->type == ButtonRelease ||
        ev->type == MotionNotify || ev->type == KeyRelease) {
        if (ext->text_widget) {
            XmTextPosition cur_top = XmTextGetTopCharacter(ext->text_widget);
            if (cur_top < 0) cur_top = 0;
            if (cur_top != ext->last_gutter_top_pos) {
                draw_line_gutter(ext);
            }
        }
        schedule_gutter_sync(ext);
    }
}

static void snapshot_active_tab(DtpadUiExt *ext)
{
    if (!ext || ext->active_tab < 0 || ext->active_tab >= ext->tab_count) return;
    Editor *pPad = ext->pad;
    DtpadDocTab *tab = &ext->tabs[ext->active_tab];

    free(tab->file_name);
    tab->file_name = (pPad->fileStuff.fileName && pPad->fileStuff.fileName[0])
                         ? strdup(pPad->fileStuff.fileName)
                         : NULL;
    tab->file_exists = pPad->fileStuff.fileExists;
    tab->read_only = pPad->fileStuff.readOnly;
    tab->unsaved = DtEditorCheckForUnsavedChanges(pPad->editor);
    tab->cursor_pos = DtEditorGetInsertionPosition(pPad->editor);
    free(tab->saved_text);
    tab->saved_text = get_editor_string_copy(pPad->editor);
}

static void on_tab_btn_clicked(Widget w, XtPointer client, XtPointer call)
{
    (void)call;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext) return;
    for (int i = 0; i < ext->tab_count; ++i) {
        if (ext->tab_buttons[i] == w) {
            DtpadSelectTab(ext->pad, i);
            return;
        }
    }
}

static void refresh_tab_bar_ui(DtpadUiExt *ext)
{
    if (!ext || !ext->tab_bar_frame || !ext->tab_row) return;

    if (ext->tab_count <= 1) {
        if (XtIsManaged(ext->tab_bar_frame)) {
            XtUnmanageChild(ext->tab_bar_frame);
        }
        XtVaSetValues(ext->body_form,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNtopOffset, 0,
                      NULL);
    } else {
        if (!XtIsManaged(ext->tab_bar_frame)) {
            XtManageChild(ext->tab_bar_frame);
        }
        XtVaSetValues(ext->body_form,
                      XmNtopAttachment, XmATTACH_WIDGET,
                      XmNtopWidget, ext->tab_bar_frame,
                      XmNtopOffset, 2,
                      NULL);
    }

    for (int i = 0; i < DTPAD_MAX_TABS; ++i) {
        if (i < ext->tab_count) {
            if (!ext->tab_buttons[i]) {
                ext->tab_buttons[i] = XtVaCreateWidget(
                    "docTabBtn", xmPushButtonWidgetClass, ext->tab_row,
                    XmNmarginWidth, 8,
                    XmNmarginHeight, 3,
                    NULL);
                XtAddCallback(ext->tab_buttons[i], XmNactivateCallback, on_tab_btn_clicked, ext);
            }
            const char *fname = (i == ext->active_tab)
                                    ? ext->pad->fileStuff.fileName
                                    : ext->tabs[i].file_name;
            Boolean dirty = (i == ext->active_tab)
                                ? DtEditorCheckForUnsavedChanges(ext->pad->editor)
                                : ext->tabs[i].unsaved;
            const char *base = "(Untitled)";
            if (fname && fname[0]) {
                const char *slash = strrchr(fname, '/');
                base = slash ? slash + 1 : fname;
            }
            char lbl[160];
            snprintf(lbl, sizeof(lbl), "%s%s", base, dirty ? " *" : "");
            XmString xms = XmStringCreateLocalized(lbl);
            XtVaSetValues(ext->tab_buttons[i],
                          XmNlabelString, xms,
                          XmNshadowThickness, (i == ext->active_tab) ? 2 : 1,
                          NULL);
            XmStringFree(xms);
            if (!XtIsManaged(ext->tab_buttons[i])) XtManageChild(ext->tab_buttons[i]);
        } else if (ext->tab_buttons[i] && XtIsManaged(ext->tab_buttons[i])) {
            XtUnmanageChild(ext->tab_buttons[i]);
        }
    }
}

void DtpadSyncActiveTabTitle(Editor *pPad)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) return;
    if (ext->tab_count > 1) {
        refresh_tab_bar_ui(ext);
    }
    DtpadPublishState(pPad);
}

void DtpadSelectTab(Editor *pPad, int index)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext || index < 0 || index >= ext->tab_count) return;
    if (index == ext->active_tab) {
        refresh_tab_bar_ui(ext);
        return;
    }

    snapshot_active_tab(ext);
    ext->switching_tabs = True;
    ext->active_tab = index;
    DtpadDocTab *tab = &ext->tabs[index];

    if (pPad->fileStuff.fileName) {
        XtFree(pPad->fileStuff.fileName);
        pPad->fileStuff.fileName = NULL;
    }
    if (tab->file_name && tab->file_name[0]) {
        pPad->fileStuff.fileName = XtNewString(tab->file_name);
    }
    pPad->fileStuff.fileExists = tab->file_exists;
    pPad->fileStuff.readOnly = tab->read_only;

    set_editor_string_with_dirty_state(pPad->editor,
                                       tab->saved_text ? tab->saved_text : "",
                                       tab->unsaved);
    DtEditorSetInsertionPosition(pPad->editor, tab->cursor_pos);

    ext->recovered_last_load = tab->recovered;
    ext->switching_tabs = False;

    ChangeMainWindowTitle(pPad);
    SetStatusLineMsg(pPad);
    refresh_tab_bar_ui(ext);
    apply_syntax_highlighting(ext);
    draw_line_gutter(ext);
    DtpadPublishState(pPad);
}

void DtpadNewTab(Editor *pPad, const char *initial_path)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext || ext->tab_count >= DTPAD_MAX_TABS) return;

    snapshot_active_tab(ext);

    int idx = ext->tab_count++;
    DtpadDocTab *tab = &ext->tabs[idx];
    memset(tab, 0, sizeof(*tab));
    tab->tab_id = ++ext->next_tab_id;
    tab->saved_text = strdup("");

    ext->switching_tabs = True;
    ext->active_tab = idx;

    if (pPad->fileStuff.fileName) {
        XtFree(pPad->fileStuff.fileName);
        pPad->fileStuff.fileName = NULL;
    }
    if (initial_path && initial_path[0]) {
        pPad->fileStuff.fileName = XtNewString((String)initial_path);
    }
    pPad->fileStuff.fileExists = False;
    pPad->fileStuff.readOnly = False;
    ext->switching_tabs = False;

    LoadFile(pPad, NULL);
    ChangeMainWindowTitle(pPad);
    refresh_tab_bar_ui(ext);
    apply_syntax_highlighting(ext);
    draw_line_gutter(ext);
    DtpadPublishState(pPad);
}

void DtpadCloseActiveTab(Editor *pPad)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) return;
    if (ext->tab_count <= 1) {
        FileExitCB(pPad->ExitWid, (XtPointer)pPad, NULL);
        return;
    }

    int closing = ext->active_tab;
    DtpadDocTab *ctab = &ext->tabs[closing];
    if (ctab->autosave_path[0]) {
        dtpad_autosave_remove_file(ctab->autosave_path);
    }
    if (pPad->fileStuff.fileName && pPad->fileStuff.fileName[0]) {
        dtpad_autosave_remove_for_path(NULL, pPad->fileStuff.fileName);
    }
    free(ctab->file_name);
    free(ctab->saved_text);

    for (int i = closing; i + 1 < ext->tab_count; ++i) {
        ext->tabs[i] = ext->tabs[i + 1];
    }
    memset(&ext->tabs[ext->tab_count - 1], 0, sizeof(DtpadDocTab));
    ext->tab_count--;

    int next_idx = closing < ext->tab_count ? closing : (ext->tab_count - 1);
    ext->active_tab = -1; /* force DtpadSelectTab to load next_idx without snapshotting closed tab */
    DtpadSelectTab(pPad, next_idx);
}

void DtpadSetLineNumbers(Editor *pPad, Boolean enabled)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) return;
    ext->show_line_numbers = enabled ? True : False;
    if (ext->line_numbers_toggle) {
        XmToggleButtonSetState(ext->line_numbers_toggle, ext->show_line_numbers, False);
    }
    if (ext->show_line_numbers) {
        XtVaSetValues(pPad->editor,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNleftOffset, 50,
                      NULL);
        XtVaSetValues(ext->gutter_frame,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_WIDGET,
                      XmNrightWidget, pPad->editor,
                      XmNrightOffset, 2,
                      NULL);
        if (!XtIsManaged(ext->gutter_frame)) XtManageChild(ext->gutter_frame);
        draw_line_gutter(ext);
    } else {
        if (XtIsManaged(ext->gutter_frame)) XtUnmanageChild(ext->gutter_frame);
        XtVaSetValues(pPad->editor,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNleftOffset, 0,
                      NULL);
    }
    save_line_numbers_pref(pPad, ext->show_line_numbers);
    DtpadPublishState(pPad);
}

void DtpadSetSyntaxHighlighting(Editor *pPad, Boolean enabled)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) return;
    ext->syntax_highlighting = enabled ? True : False;
    if (ext->syntax_toggle) {
        XmToggleButtonSetState(ext->syntax_toggle, ext->syntax_highlighting, False);
    }
    apply_syntax_highlighting(ext);
    save_syntax_pref(pPad, ext->syntax_highlighting);
    DtpadPublishState(pPad);
}

void DtpadFlushAutosaveNow(Editor *pPad)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) return;
    snapshot_active_tab(ext);
    int pid = (int)getpid();

    for (int i = 0; i < ext->tab_count; ++i) {
        DtpadDocTab *tab = &ext->tabs[i];
        if (tab->unsaved && tab->saved_text) {
            dtpad_autosave_write(NULL,
                                 tab->file_name,
                                 pid,
                                 tab->tab_id,
                                 tab->saved_text,
                                 strlen(tab->saved_text),
                                 tab->autosave_path,
                                 sizeof(tab->autosave_path));
        }
    }
}

static void on_autosave_timeout(XtPointer client, XtIntervalId *id)
{
    (void)id;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext || !ext->pad) return;
    ext->autosave_timer = 0;
    if (ext->pad->inUse) {
        DtpadFlushAutosaveNow(ext->pad);
    }
    int ms = 5000;
    const char *env = getenv("DTPAD_AUTOSAVE_MS");
    if (env && atoi(env) >= 100) ms = atoi(env);
    ext->autosave_timer = XtAppAddTimeOut(ext->pad->app_context, (unsigned long)ms,
                                          on_autosave_timeout, ext);
}

void DtpadNotifyFileLoaded(Editor *pPad)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) return;

    const char *fname = (pPad->fileStuff.fileName && pPad->fileStuff.fileName[0])
                            ? pPad->fileStuff.fileName
                            : NULL;
    if (fname && pPad->fileStuff.fileExists) {
        dtpad_recent_record(NULL, fname);
    }

    /* Check crash-recovery autosave */
    char *recovered = NULL;
    size_t rec_len = 0;
    char swap_used[PATH_MAX] = "";
    ext->recovered_last_load = False;

    if (dtpad_autosave_find_recovery(NULL, fname, &recovered, &rec_len,
                                     swap_used, sizeof(swap_used)) == 0 && recovered) {
        set_editor_string_with_dirty_state(pPad->editor, recovered, True);
        free(recovered);
        /* Adopt the swap slot into our active tab and remove old dead-PID file if untitled */
        if (!fname && swap_used[0]) {
            dtpad_autosave_remove_file(swap_used);
        }
        ext->recovered_last_load = True;
        if (ext->active_tab >= 0 && ext->active_tab < ext->tab_count) {
            ext->tabs[ext->active_tab].recovered = True;
        }
        if (pPad->statusLineMsg) {
            SetStatusMessage(pPad, "Recovered unsaved changes from crash-autosave.");
        }
        DtpadFlushAutosaveNow(pPad);
    } else if (ext->active_tab >= 0 && ext->active_tab < ext->tab_count) {
        ext->tabs[ext->active_tab].recovered = False;
    }

    snapshot_active_tab(ext);
    refresh_tab_bar_ui(ext);
    apply_syntax_highlighting(ext);
    draw_line_gutter(ext);
    DtpadPublishState(pPad);
}

void DtpadNotifyFileSaved(Editor *pPad, const char *saved_path)
{
    DtpadUiExt *ext = find_ext(pPad);
    const char *path = (saved_path && saved_path[0]) ? saved_path : pPad->fileStuff.fileName;
    if (path && path[0]) {
        dtpad_recent_record(NULL, path);
        dtpad_autosave_remove_for_path(NULL, path);
    }
    if (ext && ext->active_tab >= 0 && ext->active_tab < ext->tab_count) {
        DtpadDocTab *tab = &ext->tabs[ext->active_tab];
        if (tab->autosave_path[0]) {
            dtpad_autosave_remove_file(tab->autosave_path);
            tab->autosave_path[0] = '\0';
        }
        tab->recovered = False;
        ext->recovered_last_load = False;
        snapshot_active_tab(ext);
        refresh_tab_bar_ui(ext);
        apply_syntax_highlighting(ext);
        DtpadPublishState(pPad);
    }
}

void DtpadNotifyPadClosing(Editor *pPad, Boolean discard_autosave)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) return;
    if (ext->gutter_timer != 0) {
        XtRemoveTimeOut(ext->gutter_timer);
        ext->gutter_timer = 0;
    }
    if (ext->syntax_timer != 0) {
        XtRemoveTimeOut(ext->syntax_timer);
        ext->syntax_timer = 0;
    }
    if (discard_autosave) {
        for (int i = 0; i < ext->tab_count; ++i) {
            if (ext->tabs[i].autosave_path[0]) {
                dtpad_autosave_remove_file(ext->tabs[i].autosave_path);
                ext->tabs[i].autosave_path[0] = '\0';
            }
            if (ext->tabs[i].file_name && ext->tabs[i].file_name[0]) {
                dtpad_autosave_remove_for_path(NULL, ext->tabs[i].file_name);
            }
        }
        if (pPad->fileStuff.fileName && pPad->fileStuff.fileName[0]) {
            dtpad_autosave_remove_for_path(NULL, pPad->fileStuff.fileName);
        }
    }
}

void DtpadPublishState(Editor *pPad)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext || !pPad || !pPad->app_shell || !XtIsRealized(pPad->app_shell)) return;
    Display *dpy = XtDisplay(pPad->app_shell);
    Window win = XtWindow(pPad->app_shell);
    if (!dpy || !win) return;

    if (ext->state_atom == None) {
        ext->state_atom = XInternAtom(dpy, "_DTPAD_STATE", False);
        ext->ipc_atom = XInternAtom(dpy, "_DTPAD_IPC", False);
        ext->utf8_atom = XInternAtom(dpy, "UTF8_STRING", False);
    }

    char recent_tmp[DTPAD_MAX_RECENT][PATH_MAX];
    int rcnt = dtpad_recent_load(NULL, recent_tmp);

    char *cur_text = get_editor_string_copy(pPad->editor);
    char preview[128] = "";
    if (cur_text) {
        snprintf(preview, sizeof(preview), "%.120s", cur_text);
        free(cur_text);
    }

    char esc_file[PATH_MAX], esc_prev[256];
    json_escape(pPad->fileStuff.fileName ? pPad->fileStuff.fileName : "", esc_file, sizeof(esc_file));
    json_escape(preview, esc_prev, sizeof(esc_prev));

    char json[PATH_MAX + 1024];
    snprintf(json, sizeof(json),
             "{\"tab_count\":%d,\"active_tab\":%d,\"file\":\"%s\","
             "\"line_numbers\":%s,\"syntax_highlighting\":%s,\"syntax_lang\":\"%s\","
             "\"recovered\":%s,\"recent_count\":%d,"
             "\"unsaved\":%s,\"preview\":\"%s\"}",
             ext->tab_count,
             ext->active_tab,
             esc_file,
             ext->show_line_numbers ? "true" : "false",
             ext->syntax_highlighting ? "true" : "false",
             dtpad_syntax_lang_id(ext->active_lang),
             ext->recovered_last_load ? "true" : "false",
             rcnt,
             DtEditorCheckForUnsavedChanges(pPad->editor) ? "true" : "false",
             esc_prev);

    XChangeProperty(dpy, win, ext->state_atom, ext->utf8_atom, 8, PropModeReplace,
                    (const unsigned char *)json, (int)strlen(json));
    XFlush(dpy);
}

/* --- Menu & Keyboard Callbacks --- */
static void on_new_tab_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)call;
    DtpadNewTab((Editor *)client, NULL);
}

static void on_open_tab_ok(Widget w, XtPointer client, XtPointer call)
{
    DtpadUiExt *ext = (DtpadUiExt *)client;
    XmFileSelectionBoxCallbackStruct *cbs = (XmFileSelectionBoxCallbackStruct *)call;
    char *path = NULL;
    if (cbs && XmStringGetLtoR(cbs->value, XmFONTLIST_DEFAULT_TAG, &path) && path) {
        DtpadNewTab(ext->pad, path);
        XtFree(path);
    }
    XtUnmanageChild(w);
}

static void on_open_tab_cancel(Widget w, XtPointer client, XtPointer call)
{
    (void)client;
    (void)call;
    XtUnmanageChild(w);
}

static void on_open_in_tab_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)call;
    Editor *pPad = (Editor *)client;
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) return;
    if (!ext->open_tab_dialog) {
        ext->open_tab_dialog = XmCreateFileSelectionDialog(pPad->app_shell, "openInTabDialog", NULL, 0);
        XmString title = XmStringCreateLocalized("Open File in New Tab");
        XtVaSetValues(ext->open_tab_dialog, XmNdialogTitle, title, NULL);
        XmStringFree(title);
        Widget help = XmFileSelectionBoxGetChild(ext->open_tab_dialog, XmDIALOG_HELP_BUTTON);
        if (help) XtUnmanageChild(help);
        XtAddCallback(ext->open_tab_dialog, XmNokCallback, on_open_tab_ok, ext);
        XtAddCallback(ext->open_tab_dialog, XmNcancelCallback, on_open_tab_cancel, ext);
    }
    XtManageChild(ext->open_tab_dialog);
}

static void on_close_tab_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)call;
    DtpadCloseActiveTab((Editor *)client);
}

static void on_recent_item_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)call;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext) return;
    for (int i = 0; i < ext->recent_count; ++i) {
        if (ext->recent_items[i] == w && ext->recent_paths[i][0]) {
            if (!ext->pad->fileStuff.fileName && !DtEditorCheckForUnsavedChanges(ext->pad->editor)) {
                ext->pad->fileStuff.fileName = XtNewString(ext->recent_paths[i]);
                LoadFile(ext->pad, NULL);
                ChangeMainWindowTitle(ext->pad);
            } else {
                DtpadNewTab(ext->pad, ext->recent_paths[i]);
            }
            return;
        }
    }
}

void DtpadRefreshRecentMenu(Editor *pPad)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext || !ext->recent_menu) return;

    ext->recent_count = dtpad_recent_load(NULL, ext->recent_paths);
    for (int i = 0; i < DTPAD_MAX_RECENT; ++i) {
        if (!ext->recent_items[i]) continue;
        if (i < ext->recent_count) {
            char label[PATH_MAX + 16];
            snprintf(label, sizeof(label), "%d. %s", i + 1, ext->recent_paths[i]);
            XmString xms = XmStringCreateLocalized(label);
            XtVaSetValues(ext->recent_items[i], XmNlabelString, xms, XmNsensitive, True, NULL);
            XmStringFree(xms);
            XtManageChild(ext->recent_items[i]);
        } else if (i == 0 && ext->recent_count == 0) {
            XmString xms = XmStringCreateLocalized("(No recent files)");
            XtVaSetValues(ext->recent_items[0], XmNlabelString, xms, XmNsensitive, False, NULL);
            XmStringFree(xms);
            XtManageChild(ext->recent_items[0]);
        } else {
            XtUnmanageChild(ext->recent_items[i]);
        }
    }
}

static void on_line_numbers_toggled(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    Editor *pPad = (Editor *)client;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;
    if (!pPad || !cbs) return;
    DtpadSetLineNumbers(pPad, cbs->set ? True : False);
}

static void on_syntax_toggled(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    Editor *pPad = (Editor *)client;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;
    if (!pPad || !cbs) return;
    DtpadSetSyntaxHighlighting(pPad, cbs->set ? True : False);
}

static void on_key_shortcuts(Widget w, XtPointer client, XEvent *ev, Boolean *cont)
{
    (void)w;
    if (!ev || ev->type != KeyPress) return;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext) return;
    if (!(ev->xkey.state & ControlMask)) return;
    KeySym ks = XLookupKeysym(&ev->xkey, 0);
    if (ks == XK_Tab || ks == XK_ISO_Left_Tab) {
        if (ext->tab_count > 1) {
            int delta = (ev->xkey.state & ShiftMask) ? -1 : 1;
            int next = (ext->active_tab + delta + ext->tab_count) % ext->tab_count;
            DtpadSelectTab(ext->pad, next);
            if (cont) *cont = False;
        }
    }
}

static void on_shell_property_notify(Widget w, XtPointer client, XEvent *ev, Boolean *cont)
{
    (void)cont;
    if (!ev || ev->type != PropertyNotify || ev->xproperty.state != PropertyNewValue) return;
    DtpadUiExt *ext = (DtpadUiExt *)client;
    if (!ext || !ext->pad) return;
    Display *dpy = XtDisplay(w);
    if (ext->ipc_atom == None) ext->ipc_atom = XInternAtom(dpy, "_DTPAD_IPC", False);
    if (ev->xproperty.atom != ext->ipc_atom) return;

    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, XtWindow(w), ext->ipc_atom, 0, 1024, True,
                           AnyPropertyType, &actual, &fmt, &nitems, &after, &prop) != Success || !prop) {
        return;
    }
    char cmd[PATH_MAX + 256];
    size_t clen = nitems < sizeof(cmd) - 1 ? (size_t)nitems : sizeof(cmd) - 1;
    memcpy(cmd, prop, clen);
    cmd[clen] = '\0';
    XFree(prop);

    Editor *pPad = ext->pad;
    if (strcmp(cmd, "NEW_TAB") == 0) {
        DtpadNewTab(pPad, NULL);
    } else if (strncmp(cmd, "OPEN_TAB ", 9) == 0) {
        DtpadNewTab(pPad, cmd + 9);
    } else if (strncmp(cmd, "SELECT_TAB ", 11) == 0) {
        DtpadSelectTab(pPad, atoi(cmd + 11));
    } else if (strcmp(cmd, "CLOSE_TAB") == 0) {
        DtpadCloseActiveTab(pPad);
    } else if (strncmp(cmd, "SET_TEXT ", 9) == 0) {
        set_editor_string_with_dirty_state(pPad->editor, cmd + 9, True);
        apply_syntax_highlighting(ext);
        draw_line_gutter(ext);
        DtpadSyncActiveTabTitle(pPad);
    } else if (strcmp(cmd, "SAVE") == 0) {
        FileSaveCB(pPad->fileStuff.fileWidgets.saveBtn, (XtPointer)pPad, NULL);
    } else if (strcmp(cmd, "TOGGLE_LINE_NUMBERS") == 0) {
        DtpadSetLineNumbers(pPad, ext->show_line_numbers ? False : True);
    } else if (strncmp(cmd, "SET_LINE_NUMBERS ", 17) == 0) {
        DtpadSetLineNumbers(pPad, atoi(cmd + 17) != 0 ? True : False);
    } else if (strcmp(cmd, "TOGGLE_SYNTAX_HIGHLIGHT") == 0) {
        DtpadSetSyntaxHighlighting(pPad, ext->syntax_highlighting ? False : True);
    } else if (strncmp(cmd, "SET_SYNTAX_HIGHLIGHT ", 21) == 0) {
        DtpadSetSyntaxHighlighting(pPad, atoi(cmd + 21) != 0 ? True : False);
    } else if (strcmp(cmd, "FLUSH_AUTOSAVE") == 0) {
        DtpadFlushAutosaveNow(pPad);
        DtpadPublishState(pPad);
    }
}

void DtpadPopulateFileMenuExtras(Editor *pPad, Widget file_pane)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) {
        ext = (DtpadUiExt *)calloc(1, sizeof(DtpadUiExt));
        if (!ext) return;
        ext->pad = pPad;
        ext->next = g_ext_head;
        g_ext_head = ext;
    }

    XtVaCreateManagedWidget("tabSep", xmSeparatorGadgetClass, file_pane, NULL);

    XmString s_nt = XmStringCreateLocalized("New Tab");
    XmString a_nt = XmStringCreateLocalized("Ctrl+T");
    Widget b_nt = XtVaCreateManagedWidget("newTab", xmPushButtonGadgetClass, file_pane,
                                          XmNlabelString, s_nt,
                                          XmNaccelerator, "Ctrl<Key>t",
                                          XmNacceleratorText, a_nt,
                                          NULL);
    XmStringFree(s_nt);
    XmStringFree(a_nt);
    XtAddCallback(b_nt, XmNactivateCallback, on_new_tab_cb, pPad);

    XmString s_ot = XmStringCreateLocalized("Open in New Tab...");
    Widget b_ot = XtVaCreateManagedWidget("openInTab", xmPushButtonGadgetClass, file_pane,
                                          XmNlabelString, s_ot, NULL);
    XmStringFree(s_ot);
    XtAddCallback(b_ot, XmNactivateCallback, on_open_in_tab_cb, pPad);

    XmString s_ct = XmStringCreateLocalized("Close Tab");
    XmString a_ct = XmStringCreateLocalized("Ctrl+W");
    Widget b_ct = XtVaCreateManagedWidget("closeTab", xmPushButtonGadgetClass, file_pane,
                                          XmNlabelString, s_ct,
                                          XmNaccelerator, "Ctrl<Key>w",
                                          XmNacceleratorText, a_ct,
                                          NULL);
    XmStringFree(s_ct);
    XmStringFree(a_ct);
    XtAddCallback(b_ct, XmNactivateCallback, on_close_tab_cb, pPad);

    ext->recent_menu = XmCreatePulldownMenu(file_pane, "openRecentMenu", NULL, 0);
    XmString s_rec = XmStringCreateLocalized("Open Recent");
    XtVaCreateManagedWidget("openRecentCascade", xmCascadeButtonGadgetClass, file_pane,
                            XmNlabelString, s_rec,
                            XmNsubMenuId, ext->recent_menu,
                            NULL);
    XmStringFree(s_rec);

    for (int i = 0; i < DTPAD_MAX_RECENT; ++i) {
        char iname[32];
        snprintf(iname, sizeof(iname), "recentItem%d", i);
        ext->recent_items[i] = XtVaCreateWidget(iname, xmPushButtonGadgetClass,
                                                ext->recent_menu, NULL);
        XtAddCallback(ext->recent_items[i], XmNactivateCallback, on_recent_item_cb, ext);
    }
    DtpadRefreshRecentMenu(pPad);
}

void DtpadPopulateOptionsMenuExtras(Editor *pPad, Widget options_pane)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) return;

    Boolean init_ln = load_line_numbers_pref(pPad);
    ext->show_line_numbers = init_ln;

    XmString s_ln = XmStringCreateLocalized("Show Line Numbers");
    ext->line_numbers_toggle = XtVaCreateManagedWidget(
        "showLineNumbers", xmToggleButtonWidgetClass, options_pane,
        XmNlabelString, s_ln,
        XmNvisibleWhenOff, True,
        XmNset, init_ln,
        NULL);
    XmStringFree(s_ln);
    XtAddCallback(ext->line_numbers_toggle, XmNvalueChangedCallback,
                  on_line_numbers_toggled, pPad);

    Boolean init_syn = load_syntax_pref(pPad);
    ext->syntax_highlighting = init_syn;

    XmString s_syn = XmStringCreateLocalized("Syntax Highlighting");
    ext->syntax_toggle = XtVaCreateManagedWidget(
        "syntaxHighlighting", xmToggleButtonWidgetClass, options_pane,
        XmNlabelString, s_syn,
        XmNvisibleWhenOff, True,
        XmNset, init_syn,
        NULL);
    XmStringFree(s_syn);
    XtAddCallback(ext->syntax_toggle, XmNvalueChangedCallback,
                  on_syntax_toggled, pPad);
}

static int dtpad_select_toolbar_px(Display *dpy, int screen)
{
    const char *env = getenv("CK_TOOLBAR_ICON_SCALE");
    double scale = 1.0;
    if (env && env[0]) {
        double v = atof(env);
        if (v >= 0.75 && v <= 3.0) scale = v;
    } else if (dpy) {
        int w_px = DisplayWidth(dpy, screen);
        int w_mm = DisplayWidthMM(dpy, screen);
        if (w_px > 0 && w_mm > 10) {
            double dpi = ((double)w_px * 25.4) / (double)w_mm;
            if (dpi >= 168.0) scale = 2.0;
            else if (dpi >= 120.0) scale = 1.5;
        }
    }
    if (scale >= 1.75) return 32;
    if (scale >= 1.25) return 24;
    return 16;
}

static void dtpad_try_apply_icon(Widget btn, const char *icon_name, const char *tooltip)
{
    if (!btn || !icon_name || !icon_name[0]) return;
    Display *dpy = XtDisplay(btn);
    int screen = dpy ? DefaultScreen(dpy) : 0;
    int target_px = dtpad_select_toolbar_px(dpy, screen);

    const char *env_dir = getenv("CK_TOOLBAR_ICON_DIR");
    const char *roots[4];
    int nroots = 0;
    if (env_dir && env_dir[0]) roots[nroots++] = env_dir;
    roots[nroots++] = "/usr/dt/appconfig/icons/C/toolbar";
    roots[nroots++] = "/usr/local/share/ck-core/icons/toolbar";
    roots[nroots++] = "/usr/share/ck-core/icons/toolbar";

    int sizes[5];
    int nsizes = 0;
    sizes[nsizes++] = target_px;
    const int std_sizes[] = { 16, 24, 32, 48 };
    for (size_t i = 0; i < sizeof(std_sizes) / sizeof(std_sizes[0]); ++i) {
        if (std_sizes[i] != target_px) sizes[nsizes++] = std_sizes[i];
    }

    char found[PATH_MAX] = "";
    int picked_px = target_px;
    for (int r = 0; r < nroots && !found[0]; ++r) {
        for (int s = 0; s < nsizes; ++s) {
            char cand[PATH_MAX];
            snprintf(cand, sizeof(cand), "%s/%dx%d/%s.png",
                     roots[r], sizes[s], sizes[s], icon_name);
            if (access(cand, R_OK) == 0) {
                snprintf(found, sizeof(found), "%s", cand);
                picked_px = sizes[s];
                break;
            }
        }
    }
    if (!found[0]) return;

    Pixel fg = 0, bg = 0, arm = 0;
    XtVaGetValues(btn, XmNforeground, &fg, XmNbackground, &bg, XmNarmColor, &arm, NULL);
    int depth = DefaultDepthOfScreen(XtScreen(btn));
    double ratio = picked_px > 0 ? ((double)target_px / (double)picked_px) : 1.0;
    Pixmap p_norm = XmGetScaledPixmap(btn, found, fg, bg, depth, ratio);
    if (p_norm != XmUNSPECIFIED_PIXMAP) {
        XtVaSetValues(btn, XmNlabelType, XmPIXMAP, XmNlabelPixmap, p_norm,
                      XmNmarginWidth, 3, XmNmarginHeight, 2, NULL);
        Pixmap p_arm = XmGetScaledPixmap(btn, found, fg, arm, depth, ratio);
        if (p_arm != XmUNSPECIFIED_PIXMAP) {
            XtVaSetValues(btn, XmNarmPixmap, p_arm, NULL);
        }
        if (tooltip && tooltip[0]) {
            XmString tip = XmStringCreateLocalized((String)tooltip);
            XtVaSetValues(btn, XmNtoolTipString, tip, NULL);
            XmStringFree(tip);
        }
    }
}

Widget DtpadCreateWorkArea(Editor *pPad)
{
    DtpadUiExt *ext = find_ext(pPad);
    if (!ext) {
        ext = (DtpadUiExt *)calloc(1, sizeof(DtpadUiExt));
        ext->pad = pPad;
        ext->next = g_ext_head;
        g_ext_head = ext;
    }

    ext->work_form = XmCreateForm(pPad->mainWindow, "dtpadWorkForm", NULL, 0);
    XtManageChild(ext->work_form);

    ext->tab_bar_frame = XmCreateFrame(ext->work_form, "dtpadTabBarFrame", NULL, 0);
    XtVaSetValues(ext->tab_bar_frame,
                  XmNshadowType, XmSHADOW_ETCHED_OUT,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);

    ext->tab_row = XmCreateRowColumn(ext->tab_bar_frame, "dtpadTabRow", NULL, 0);
    XtVaSetValues(ext->tab_row,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 4,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 2,
                  NULL);
    XtManageChild(ext->tab_row);

    XmString s_plus = XmStringCreateLocalized("+");
    ext->plus_btn = XtVaCreateManagedWidget("newTabPlus", xmPushButtonWidgetClass, ext->tab_row,
                                            XmNlabelString, s_plus,
                                            XmNmarginWidth, 6,
                                            XmNmarginHeight, 2,
                                            NULL);
    XmStringFree(s_plus);
    dtpad_try_apply_icon(ext->plus_btn, "tab-new", "New Tab (Ctrl+T)");
    XtAddCallback(ext->plus_btn, XmNactivateCallback, on_new_tab_cb, pPad);

    ext->body_form = XmCreateForm(ext->work_form, "dtpadBodyForm", NULL, 0);
    XtVaSetValues(ext->body_form,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(ext->body_form);

    ext->gutter_frame = XmCreateFrame(ext->body_form, "lineGutterFrame", NULL, 0);
    XtVaSetValues(ext->gutter_frame,
                  XmNshadowType, XmSHADOW_ETCHED_IN,
                  XmNshadowThickness, 1,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNwidth, 46,
                  NULL);
    ext->gutter_canvas = XtVaCreateManagedWidget("lineGutterCanvas",
                                                 xmDrawingAreaWidgetClass, ext->gutter_frame,
                                                 XmNresizePolicy, XmRESIZE_NONE,
                                                 XmNwidth, 46,
                                                 XmNtraversalOn, False,
                                                 NULL);
    XtAddCallback(ext->gutter_canvas, XmNexposeCallback, on_gutter_expose, ext);
    XtAddCallback(ext->gutter_canvas, XmNresizeCallback, on_gutter_expose, ext);

    /* Create the DtEditor widget inside body_form */
    Arg al[12];
    int ac = 0;
    XmString dialogTitleStr = XmStringCreateLocalized(DialogTitle(pPad));
    XtSetArg(al[ac], DtNrows, DEFAULT_ROWS); ac++;
    XtSetArg(al[ac], DtNcolumns, DEFAULT_COLS); ac++;
    XtSetArg(al[ac], DtNdialogTitle, dialogTitleStr); ac++;
    XtSetArg(al[ac], DtNdroppedFilesMode, pPad->xrdb.droppedFilesMode); ac++;
    XtSetArg(al[ac], XmNtopAttachment, XmATTACH_FORM); ac++;
    XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_FORM); ac++;
    XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
    XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
    pPad->editor = DtCreateEditor(ext->body_form, "editor", al, ac);
    XmStringFree(dialogTitleStr);
    XtManageChild(pPad->editor);

    XtAddCallback(pPad->editor, XmNhelpCallback, HelpEditorWidgetCB, (XtPointer)pPad);
    XtAddCallback(pPad->editor, DtNtextSelectCallback, TextSelectedCB, (XtPointer)pPad);
    XtAddCallback(pPad->editor, DtNtextDeselectCallback, TextDeselectedCB, (XtPointer)pPad);
    pPad->statusLineMsg = DtEditorGetMessageTextFieldID(pPad->editor);

    ext->text_widget = XtNameToWidget(pPad->editor, "*text");
    if (ext->text_widget) {
        XtAddCallback(ext->text_widget, XmNvalueChangedCallback, on_text_changed, ext);
        XtAddCallback(ext->text_widget, XmNmotionVerifyCallback, on_text_scrolled, ext);
        XtAddEventHandler(ext->text_widget, KeyPressMask, False, on_key_shortcuts, ext);
        XtAddEventHandler(ext->text_widget,
                          StructureNotifyMask | ExposureMask | ButtonPressMask |
                              ButtonReleaseMask | KeyReleaseMask,
                          True, on_text_viewport_event, ext);

        Widget sw = XtParent(ext->text_widget);
        if (sw) {
            XtVaGetValues(sw, XmNverticalScrollBar, &ext->vscroll_widget, NULL);
            if (ext->vscroll_widget) {
                static const char *const sb_cbs[] = {
                    XmNvalueChangedCallback,
                    XmNdragCallback,
                    XmNincrementCallback,
                    XmNdecrementCallback,
                    XmNpageIncrementCallback,
                    XmNpageDecrementCallback,
                    XmNtoTopCallback,
                    XmNtoBottomCallback,
                };
                for (size_t i = 0; i < sizeof(sb_cbs) / sizeof(sb_cbs[0]); ++i) {
                    XtAddCallback(ext->vscroll_widget, (String)sb_cbs[i], on_text_scrolled, ext);
                }
                XtAddEventHandler(ext->vscroll_widget,
                                  ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
                                  False, on_text_viewport_event, ext);
            }
        }
    }

    /* Initialize Tab 0 */
    ext->tab_count = 1;
    ext->active_tab = 0;
    ext->next_tab_id = 1;
    ext->syntax_highlighting = load_syntax_pref(pPad);
    memset(&ext->tabs[0], 0, sizeof(DtpadDocTab));
    ext->tabs[0].tab_id = 1;
    ext->tabs[0].saved_text = strdup("");

    if (ext->show_line_numbers) {
        DtpadSetLineNumbers(pPad, True);
    }

    XtAddEventHandler(pPad->app_shell, PropertyChangeMask, False, on_shell_property_notify, ext);

    int ms = 5000;
    const char *env = getenv("DTPAD_AUTOSAVE_MS");
    if (env && atoi(env) >= 100) ms = atoi(env);
    ext->autosave_timer = XtAppAddTimeOut(pPad->app_context, (unsigned long)ms,
                                          on_autosave_timeout, ext);

    return ext->work_form;
}
