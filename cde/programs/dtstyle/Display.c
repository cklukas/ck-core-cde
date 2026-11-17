/*
 * CDE - Common Desktop Environment
 *
 * Display.c - Graphical Multi-Monitor Arrangement, Resolution, Rotation &
 *             Per-Output HiDPI Scaling for Style Manager (Wayland & X11/XLibre).
 */

#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/MwmUtil.h>

#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>
#include <Xm/ToggleB.h>
#include <Xm/ToggleBG.h>

#include <Dt/DialogBox.h>
#include <Dt/HourGlass.h>
#include <Dt/Icon.h>
#include <Dt/TitleBox.h>

#include "Display.h"
#include "Help.h"
#include "Main.h"
#include "Protocol.h"
#include "SaveRestore.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DISP_OUTPUTS 16
#define MAX_DISP_MODES   24

#define BTN_OK     1
#define BTN_APPLY  2
#define BTN_CANCEL 3
#define BTN_HELP   4

typedef struct {
    int w;
    int h;
} DispMode;

typedef struct {
    char name[64];
    bool enabled;
    int x, y;
    int width, height;   /* Unrotated mode pixels */
    int rotation;        /* 0, 90, 180, 270 */
    int dpi;             /* 96, 120, 144, 168, 192 */
    int mode_count;
    DispMode modes[MAX_DISP_MODES];
#ifdef HAVE_XRANDR
    RROutput rr_output;
    RRCrtc rr_crtc;
#endif
} DispOutputInfo;

typedef struct {
    bool is_wayland;
    int count;
    int selected;
    DispOutputInfo outs[MAX_DISP_OUTPUTS];
    DispOutputInfo orig_outs[MAX_DISP_OUTPUTS];
    int orig_count;

    /* Canvas drag transform state */
    double view_scale;
    double view_ox;
    double view_oy;
    bool dragging;
    int drag_idx;
    int drag_start_mx, drag_start_my;
    int drag_orig_x, drag_orig_y;

    /* Widgets */
    Widget pict_icon;
    Widget canvas_tb;
    Widget canvas;
    Widget summary_lbl;
    Widget settings_tb;
    Widget mon_btn_rc;
    Widget mon_btns[MAX_DISP_OUTPUTS];
    Widget enable_toggle;
    Widget res_pulldown;
    Widget res_opt_menu;
    Widget res_btns[MAX_DISP_MODES];
    int res_btn_count;
    Widget rot_opt_menu;
    Widget rot_btns[4];
    Widget dpi_opt_menu;
    Widget dpi_btns[5];
    bool updating_widgets;
} DispDialogState;

static DispDialogState g_disp = {0};
static saveRestore g_disp_save = {FALSE, 0, };

static const int k_rot_values[4] = {0, 90, 180, 270};
static const char *const k_rot_labels[4] = {
    "Normal (0 deg)",
    "90 deg Clockwise",
    "180 deg Inverted",
    "270 deg Counter-CW",
};

static const int k_dpi_values[5] = {96, 120, 144, 168, 192};
static const char *const k_dpi_labels[5] = {
    "100% (96 DPI)",
    "125% (120 DPI)",
    "150% (144 DPI)",
    "175% (168 DPI)",
    "200% (192 DPI)",
};

static int effective_w(const DispOutputInfo *o) {
    return (o->rotation == 90 || o->rotation == 270) ? o->height : o->width;
}

static int effective_h(const DispOutputInfo *o) {
    return (o->rotation == 90 || o->rotation == 270) ? o->width : o->height;
}

static void add_unique_mode(DispOutputInfo *o, int w, int h) {
    if (!o || w < 320 || h < 240) return;
    for (int i = 0; i < o->mode_count; ++i) {
        if (o->modes[i].w == w && o->modes[i].h == h) return;
    }
    if (o->mode_count < MAX_DISP_MODES) {
        o->modes[o->mode_count].w = w;
        o->modes[o->mode_count].h = h;
        o->mode_count++;
    }
}

static void normalize_layout_origin(void) {
    int min_x = 1000000, min_y = 1000000;
    bool any = false;
    for (int i = 0; i < g_disp.count; ++i) {
        if (!g_disp.outs[i].enabled) continue;
        if (g_disp.outs[i].x < min_x) min_x = g_disp.outs[i].x;
        if (g_disp.outs[i].y < min_y) min_y = g_disp.outs[i].y;
        any = true;
    }
    if (any && (min_x != 0 || min_y != 0)) {
        for (int i = 0; i < g_disp.count; ++i) {
            if (!g_disp.outs[i].enabled) continue;
            g_disp.outs[i].x -= min_x;
            g_disp.outs[i].y -= min_y;
        }
    }
}

/* -------------------------------------------------------------------------
 * Backend Discovery & Live Apply (Wayland dtwm-wayland + X11/XLibre RandR)
 * ------------------------------------------------------------------------- */
static bool detect_wayland_outputs_prop(char *out_buf, size_t out_sz) {
    out_buf[0] = '\0';
    if (!style.display) return false;
    Atom prop = XInternAtom(style.display, "_DT_WAYLAND_OUTPUTS", True);
    if (prop == None) return false;

    Atom act_type = None;
    int act_fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(
            style.display, style.root, prop, 0, 2048, False, AnyPropertyType,
            &act_type, &act_fmt, &nitems, &rem, &data
        ) != Success || !data) {
        return false;
    }
    size_t copy_len = (nitems < out_sz - 1) ? (size_t)nitems : (out_sz - 1);
    memcpy(out_buf, data, copy_len);
    out_buf[copy_len] = '\0';
    XFree(data);
    return copy_len > 0;
}

static void send_wayland_wm_request(const char *cmd) {
    if (!style.display || !cmd || !cmd[0]) return;
    Atom mwm_info = XInternAtom(style.display, "_MOTIF_WM_INFO", True);
    if (mwm_info == None) return;
    Atom act_type = None;
    int act_fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(
            style.display, style.root, mwm_info, 0, 2, False, AnyPropertyType,
            &act_type, &act_fmt, &nitems, &rem, &prop
        ) != Success || !prop || nitems < 2) {
        if (prop) XFree(prop);
        return;
    }
    Window wm_win = (Window)((unsigned long *)prop)[1];
    XFree(prop);
    if (wm_win == None) return;

    Atom req_atom = XInternAtom(style.display, "_DT_WM_REQUEST", False);
    XChangeProperty(
        style.display, wm_win, req_atom, XA_STRING, 8, PropModeAppend,
        (const unsigned char *)cmd, (int)strlen(cmd) + 1
    );
    XSync(style.display, False);
}

static void query_outputs(void) {
    g_disp.count = 0;
    memset(g_disp.outs, 0, sizeof(g_disp.outs));

    char wl_buf[4096];
    if (detect_wayland_outputs_prop(wl_buf, sizeof(wl_buf))) {
        g_disp.is_wayland = true;
        char *savep = nullptr;
        for (char *line = strtok_r(wl_buf, "\n", &savep);
             line && g_disp.count < MAX_DISP_OUTPUTS;
             line = strtok_r(nullptr, "\n", &savep)) {
            unsigned int idx = 0;
            char name[64] = {0};
            char modes_csv[512] = {0};
            int en = 1, ox = 0, oy = 0, ow = 1280, oh = 900, rot = 0, dpi = 96;
            if (sscanf(
                    line, "%u|%63[^|]|%d|%d|%d|%d|%d|%d|%d|%511[^\n]",
                    &idx, name, &en, &ox, &oy, &ow, &oh, &rot, &dpi, modes_csv
                ) >= 9) {
                DispOutputInfo *o = &g_disp.outs[g_disp.count++];
                snprintf(o->name, sizeof(o->name), "%s", name);
                o->enabled = (en != 0);
                o->x = ox;
                o->y = oy;
                o->width = ow > 0 ? ow : 1280;
                o->height = oh > 0 ? oh : 900;
                o->rotation = rot;
                o->dpi = dpi > 0 ? dpi : 96;
                add_unique_mode(o, o->width, o->height);
                char *msave = nullptr;
                for (char *mtok = strtok_r(modes_csv, ",", &msave);
                     mtok;
                     mtok = strtok_r(nullptr, ",", &msave)) {
                    int mw = 0, mh = 0;
                    if (sscanf(mtok, "%dx%d", &mw, &mh) == 2) {
                        add_unique_mode(o, mw, mh);
                    }
                }
            }
        }
    }
#ifdef HAVE_XRANDR
    else {
        g_disp.is_wayland = false;
        int ev_base = 0, err_base = 0;
        if (XRRQueryExtension(style.display, &ev_base, &err_base)) {
            XRRScreenResources *res = XRRGetScreenResourcesCurrent(style.display, style.root);
            Atom dpi_atom = XInternAtom(style.display, "DPI", False);
            if (res) {
                for (int i = 0; i < res->noutput && g_disp.count < MAX_DISP_OUTPUTS; ++i) {
                    XRROutputInfo *oi = XRRGetOutputInfo(style.display, res, res->outputs[i]);
                    if (!oi) continue;
                    if (oi->connection == RR_Disconnected && oi->crtc == None) {
                        XRRFreeOutputInfo(oi);
                        continue;
                    }
                    DispOutputInfo *o = &g_disp.outs[g_disp.count++];
                    snprintf(o->name, sizeof(o->name), "%s", oi->name ? oi->name : "OUT");
                    o->rr_output = res->outputs[i];
                    o->rr_crtc = oi->crtc;
                    o->dpi = 96;

                    /* Read XLibre per-output DPI property if set */
                    Atom act_t = None;
                    int act_f = 0;
                    unsigned long nitems = 0, rem = 0;
                    unsigned char *pdata = nullptr;
                    if (XRRGetOutputProperty(
                            style.display, res->outputs[i], dpi_atom, 0, 1, False, False,
                            AnyPropertyType, &act_t, &act_f, &nitems, &rem, &pdata
                        ) == Success && pdata) {
                        if (act_f == 32 && nitems >= 1) {
                            int v = (int)(*(long *)pdata);
                            if (v >= 72 && v <= 384) o->dpi = v;
                        }
                        XFree(pdata);
                    }

                    if (oi->crtc != None) {
                        XRRCrtcInfo *ci = XRRGetCrtcInfo(style.display, res, oi->crtc);
                        if (ci) {
                            o->enabled = (ci->mode != None && ci->width > 0 && ci->height > 0);
                            o->x = ci->x;
                            o->y = ci->y;
                            if (ci->rotation & RR_Rotate_90) o->rotation = 90;
                            else if (ci->rotation & RR_Rotate_180) o->rotation = 180;
                            else if (ci->rotation & RR_Rotate_270) o->rotation = 270;
                            else o->rotation = 0;

                            o->width = (o->rotation == 90 || o->rotation == 270) ? (int)ci->height : (int)ci->width;
                            o->height = (o->rotation == 90 || o->rotation == 270) ? (int)ci->width : (int)ci->height;
                            XRRFreeCrtcInfo(ci);
                        }
                    }
                    if (o->width <= 0 || o->height <= 0) {
                        o->width = DisplayWidth(style.display, style.screenNum);
                        o->height = DisplayHeight(style.display, style.screenNum);
                    }
                    add_unique_mode(o, o->width, o->height);
                    for (int m = 0; m < oi->nmode; ++m) {
                        for (int rm = 0; rm < res->nmode; ++rm) {
                            if (res->modes[rm].id == oi->modes[m]) {
                                add_unique_mode(o, (int)res->modes[rm].width, (int)res->modes[rm].height);
                                break;
                            }
                        }
                    }
                    XRRFreeOutputInfo(oi);
                }
                XRRFreeScreenResources(res);
            }
        }
    }
#endif

    if (g_disp.count == 0) {
        /* Fallback single-head representation if neither Wayland nor RandR reports outputs */
        DispOutputInfo *o = &g_disp.outs[0];
        snprintf(o->name, sizeof(o->name), "Screen-0");
        o->enabled = true;
        o->x = 0;
        o->y = 0;
        o->width = DisplayWidth(style.display, style.screenNum);
        o->height = DisplayHeight(style.display, style.screenNum);
        o->rotation = 0;
        o->dpi = 96;
        add_unique_mode(o, o->width, o->height);
        g_disp.count = 1;
    }

    if (g_disp.selected >= g_disp.count || g_disp.selected < 0) {
        g_disp.selected = 0;
    }
    memcpy(g_disp.orig_outs, g_disp.outs, sizeof(g_disp.outs));
    g_disp.orig_count = g_disp.count;
}

static void apply_outputs(const DispOutputInfo *arr, int count) {
    if (!arr || count <= 0) return;

    if (g_disp.is_wayland) {
        for (int i = 0; i < count; ++i) {
            const DispOutputInfo *o = &arr[i];
            char cmd[256];
            snprintf(
                cmd, sizeof(cmd),
                "f.set_output_layout %s %d %d %d %dx%d %d %d",
                o->name,
                o->enabled ? 1 : 0,
                o->x,
                o->y,
                o->width,
                o->height,
                o->rotation,
                o->dpi
            );
            send_wayland_wm_request(cmd);
        }
        return;
    }

#ifdef HAVE_XRANDR
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(style.display, style.root);
    if (!res) return;
    Atom dpi_atom = XInternAtom(style.display, "DPI", False);
    Atom opt_in = XInternAtom(style.display, "_MOTIF_RANDR_OUTPUT_DPI", False);
    long one = 1;
    XChangeProperty(style.display, style.root, opt_in, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);

    int total_w = 320, total_h = 240;
    for (int i = 0; i < count; ++i) {
        if (!arr[i].enabled) continue;
        int r = arr[i].x + effective_w(&arr[i]);
        int b = arr[i].y + effective_h(&arr[i]);
        if (r > total_w) total_w = r;
        if (b > total_h) total_h = b;
    }

    int mm_w = (int)((double)total_w * 25.4 / 96.0);
    int mm_h = (int)((double)total_h * 25.4 / 96.0);

    for (int i = 0; i < count; ++i) {
        const DispOutputInfo *o = &arr[i];
        if (o->rr_output == None) continue;
        long dpi_val = o->dpi > 0 ? o->dpi : 96;
        XRRChangeOutputProperty(
            style.display, o->rr_output, dpi_atom, XA_INTEGER, 32,
            PropModeReplace, (const unsigned char *)&dpi_val, 1
        );

        XRROutputInfo *oi = XRRGetOutputInfo(style.display, res, o->rr_output);
        if (!oi) continue;
        RRCrtc crtc = oi->crtc;
        if (crtc == None && o->enabled && oi->ncrtc > 0) {
            crtc = oi->crtcs[0];
        }
        if (crtc != None) {
            if (!o->enabled) {
                XRRSetCrtcConfig(style.display, res, crtc, CurrentTime, 0, 0, None, RR_Rotate_0, nullptr, 0);
            } else {
                RRMode chosen = None;
                for (int m = 0; m < oi->nmode && chosen == None; ++m) {
                    for (int rm = 0; rm < res->nmode; ++rm) {
                        if (res->modes[rm].id == oi->modes[m] &&
                            (int)res->modes[rm].width == o->width &&
                            (int)res->modes[rm].height == o->height) {
                            chosen = res->modes[rm].id;
                            break;
                        }
                    }
                }
                Rotation rrot = RR_Rotate_0;
                if (o->rotation == 90) rrot = RR_Rotate_90;
                else if (o->rotation == 180) rrot = RR_Rotate_180;
                else if (o->rotation == 270) rrot = RR_Rotate_270;
                if (chosen != None) {
                    RROutput out_id = o->rr_output;
                    XRRSetCrtcConfig(
                        style.display, res, crtc, CurrentTime,
                        o->x, o->y, chosen, rrot, &out_id, 1
                    );
                }
            }
        }
        XRRFreeOutputInfo(oi);
    }
    XRRSetScreenSize(style.display, style.root, total_w, total_h, mm_w, mm_h);
    XRRFreeScreenResources(res);
    XSync(style.display, False);
#endif
}

/* -------------------------------------------------------------------------
 * Monitor Arrangement Canvas Rendering & Interactive Mouse Drag
 * ------------------------------------------------------------------------- */
static void update_controls_from_selected(void);

static void compute_canvas_transform(int cw, int ch) {
    int min_x = 0, min_y = 0, max_x = 1280, max_y = 900;
    bool first = true;
    int disabled_slot = 0;
    for (int i = 0; i < g_disp.count; ++i) {
        const DispOutputInfo *o = &g_disp.outs[i];
        int ox = o->enabled ? o->x : (disabled_slot++ * 1320);
        int oy = o->enabled ? o->y : 0;
        int ow = effective_w(o);
        int oh = effective_h(o);
        if (first) {
            min_x = ox;
            min_y = oy;
            max_x = ox + ow;
            max_y = oy + oh;
            first = false;
        } else {
            if (ox < min_x) min_x = ox;
            if (oy < min_y) min_y = oy;
            if (ox + ow > max_x) max_x = ox + ow;
            if (oy + oh > max_y) max_y = oy + oh;
        }
    }
    double span_w = (double)(max_x - min_x);
    double span_h = (double)(max_y - min_y);
    if (span_w < 640.0) span_w = 640.0;
    if (span_h < 480.0) span_h = 480.0;

    double pad = 26.0;
    double sx = ((double)cw - pad * 2.0) / span_w;
    double sy = ((double)ch - pad * 2.0) / span_h;
    g_disp.view_scale = (sx < sy) ? sx : sy;
    if (g_disp.view_scale <= 0.01) g_disp.view_scale = 0.05;

    g_disp.view_ox = ((double)cw - span_w * g_disp.view_scale) * 0.5 - (double)min_x * g_disp.view_scale;
    g_disp.view_oy = ((double)ch - span_h * g_disp.view_scale) * 0.5 - (double)min_y * g_disp.view_scale;
}

static void draw_bevel(cairo_t *cr, double x, double y, double w, double h, bool inset) {
    cairo_set_line_width(cr, 1.5);
    if (!inset) cairo_set_source_rgb(cr, 0.88, 0.90, 0.92);
    else        cairo_set_source_rgb(cr, 0.08, 0.10, 0.12);
    cairo_move_to(cr, x + 1, y + h - 1);
    cairo_line_to(cr, x + 1, y + 1);
    cairo_line_to(cr, x + w - 1, y + 1);
    cairo_stroke(cr);

    if (!inset) cairo_set_source_rgb(cr, 0.18, 0.22, 0.26);
    else        cairo_set_source_rgb(cr, 0.45, 0.50, 0.55);
    cairo_move_to(cr, x + w - 1, y + 1);
    cairo_line_to(cr, x + w - 1, y + h - 1);
    cairo_line_to(cr, x + 1, y + h - 1);
    cairo_stroke(cr);
}

static void draw_canvas_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;
    if (!w || !XtIsRealized(w)) return;

    Dimension cw = 0, ch = 0;
    XtVaGetValues(w, XmNwidth, &cw, XmNheight, &ch, NULL);
    if (cw < 40 || ch < 40) return;

    compute_canvas_transform((int)cw, (int)ch);

    Visual *vis = DefaultVisual(style.display, style.screenNum);
    cairo_surface_t *surf = cairo_xlib_surface_create(style.display, XtWindow(w), vis, cw, ch);
    cairo_t *cr = cairo_create(surf);

    /* Classic Sun/IBM recessed slate monitor-arrangement table */
    cairo_set_source_rgb(cr, 0.14, 0.18, 0.21);
    cairo_paint(cr);

    /* Subtle alignment grid */
    cairo_set_source_rgba(cr, 0.30, 0.37, 0.43, 0.35);
    cairo_set_line_width(cr, 1.0);
    for (int gx = 24; gx < (int)cw; gx += 24) {
        cairo_move_to(cr, gx + 0.5, 0);
        cairo_line_to(cr, gx + 0.5, ch);
    }
    for (int gy = 24; gy < (int)ch; gy += 24) {
        cairo_move_to(cr, 0, gy + 0.5);
        cairo_line_to(cr, cw, gy + 0.5);
    }
    cairo_stroke(cr);

    int disabled_slot = 0;
    for (int i = 0; i < g_disp.count; ++i) {
        const DispOutputInfo *o = &g_disp.outs[i];
        int ox = o->enabled ? o->x : (disabled_slot++ * 1320);
        int oy = o->enabled ? o->y : 0;
        int ow = effective_w(o);
        int oh = effective_h(o);

        double rx = floor(g_disp.view_ox + (double)ox * g_disp.view_scale);
        double ry = floor(g_disp.view_oy + (double)oy * g_disp.view_scale);
        double rw = fmax(52.0, floor((double)ow * g_disp.view_scale));
        double rh = fmax(38.0, floor((double)oh * g_disp.view_scale));

        bool sel = (i == g_disp.selected);
        if (!o->enabled) {
            cairo_set_source_rgb(cr, 0.25, 0.27, 0.29);
        } else if (sel) {
            cairo_set_source_rgb(cr, 0.24, 0.49, 0.62); /* CDE active teal-slate */
        } else {
            cairo_set_source_rgb(cr, 0.36, 0.42, 0.48);
        }
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_fill(cr);
        draw_bevel(cr, rx, ry, rw, rh, false);

        /* Top orientation bar inside monitor bezel (rotates with monitor!) */
        cairo_set_source_rgb(cr, sel ? 0.96 : 0.72, sel ? 0.76 : 0.76, sel ? 0.28 : 0.80);
        if (o->rotation == 0) {
            cairo_rectangle(cr, rx + 4, ry + 4, rw - 8, 5);
        } else if (o->rotation == 180) {
            cairo_rectangle(cr, rx + 4, ry + rh - 9, rw - 8, 5);
        } else if (o->rotation == 90) {
            cairo_rectangle(cr, rx + rw - 9, ry + 4, 5, rh - 8);
        } else {
            cairo_rectangle(cr, rx + 4, ry + 4, 5, rh - 8);
        }
        cairo_fill(cr);

        /* Monitor index badge + name + mode/DPI caption */
        char line1[96], line2[96];
        snprintf(line1, sizeof(line1), "%d: %s%s", i + 1, o->name, o->enabled ? "" : " (OFF)");
        snprintf(line2, sizeof(line2), "%dx%d @ %d DPI", ow, oh, o->dpi);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.0);
        cairo_set_source_rgb(cr, 0.96, 0.98, 0.99);
        cairo_move_to(cr, rx + 10, ry + rh * 0.46);
        cairo_show_text(cr, line1);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.5);
        cairo_set_source_rgb(cr, 0.84, 0.90, 0.94);
        cairo_move_to(cr, rx + 10, ry + rh * 0.46 + 15.0);
        cairo_show_text(cr, line2);

        if (sel) {
            cairo_set_source_rgb(cr, 0.98, 0.78, 0.26);
            cairo_set_line_width(cr, 2.0);
            cairo_rectangle(cr, rx - 1.0, ry - 1.0, rw + 2.0, rh + 2.0);
            cairo_stroke(cr);
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

static int hit_test_monitor(int mx, int my) {
    int disabled_slot = 0;
    for (int i = g_disp.count - 1; i >= 0; --i) {
        const DispOutputInfo *o = &g_disp.outs[i];
        int ox = o->enabled ? o->x : (disabled_slot++ * 1320);
        int oy = o->enabled ? o->y : 0;
        double rx = g_disp.view_ox + (double)ox * g_disp.view_scale;
        double ry = g_disp.view_oy + (double)oy * g_disp.view_scale;
        double rw = fmax(52.0, (double)effective_w(o) * g_disp.view_scale);
        double rh = fmax(38.0, (double)effective_h(o) * g_disp.view_scale);
        if (mx >= rx && mx <= rx + rw && my >= ry && my <= ry + rh) {
            return i;
        }
    }
    return -1;
}

static void snap_output_to_neighbors(int idx) {
    DispOutputInfo *me = &g_disp.outs[idx];
    int mw = effective_w(me);
    int mh = effective_h(me);
    const int snap = 48;

    if (abs(me->x) < snap) me->x = 0;
    if (abs(me->y) < snap) me->y = 0;

    for (int i = 0; i < g_disp.count; ++i) {
        if (i == idx || !g_disp.outs[i].enabled) continue;
        const DispOutputInfo *nb = &g_disp.outs[i];
        int nw = effective_w(nb);
        int nh = effective_h(nb);

        /* Snap left/right edges */
        if (abs(me->x - (nb->x + nw)) < snap) me->x = nb->x + nw;
        else if (abs((me->x + mw) - nb->x) < snap) me->x = nb->x - mw;
        else if (abs(me->x - nb->x) < snap) me->x = nb->x;

        /* Snap top/bottom edges */
        if (abs(me->y - (nb->y + nh)) < snap) me->y = nb->y + nh;
        else if (abs((me->y + mh) - nb->y) < snap) me->y = nb->y - mh;
        else if (abs(me->y - nb->y) < snap) me->y = nb->y;
    }
}

static void update_summary_label(void) {
    if (!g_disp.summary_lbl) return;
    const DispOutputInfo *o = &g_disp.outs[g_disp.selected];
    char txt[256];
    snprintf(
        txt, sizeof(txt),
        "Backend: %s  |  Selected: %s  |  Position: (%d, %d)  |  Logical Size: %dx%d",
        g_disp.is_wayland ? "Wayland (zwlr_output_manager_v1)" : "X11 / XLibre (RandR)",
        o->name, o->x, o->y, effective_w(o), effective_h(o)
    );
    XmString xs = CMPSTR(txt);
    XtVaSetValues(g_disp.summary_lbl, XmNlabelString, xs, NULL);
    XmStringFree(xs);
}

static void canvas_pointer_eh(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)client_data;
    (void)cont;
    if (!ev) return;

    if (ev->type == ButtonPress && ev->xbutton.button == Button1) {
        int hit = hit_test_monitor(ev->xbutton.x, ev->xbutton.y);
        if (hit >= 0) {
            g_disp.selected = hit;
            if (g_disp.outs[hit].enabled && g_disp.count > 1) {
                g_disp.dragging = true;
                g_disp.drag_idx = hit;
                g_disp.drag_start_mx = ev->xbutton.x;
                g_disp.drag_start_my = ev->xbutton.y;
                g_disp.drag_orig_x = g_disp.outs[hit].x;
                g_disp.drag_orig_y = g_disp.outs[hit].y;
            }
            update_controls_from_selected();
            draw_canvas_cb(w, nullptr, nullptr);
        }
    } else if (ev->type == MotionNotify && g_disp.dragging) {
        int idx = g_disp.drag_idx;
        if (idx >= 0 && idx < g_disp.count && g_disp.view_scale > 0.01) {
            int dx = (int)lround((double)(ev->xmotion.x - g_disp.drag_start_mx) / g_disp.view_scale);
            int dy = (int)lround((double)(ev->xmotion.y - g_disp.drag_start_my) / g_disp.view_scale);
            g_disp.outs[idx].x = ((g_disp.drag_orig_x + dx) / 16) * 16;
            g_disp.outs[idx].y = ((g_disp.drag_orig_y + dy) / 16) * 16;
            snap_output_to_neighbors(idx);
            update_summary_label();
            draw_canvas_cb(w, nullptr, nullptr);
        }
    } else if (ev->type == ButtonRelease && ev->xbutton.button == Button1 && g_disp.dragging) {
        g_disp.dragging = false;
        normalize_layout_origin();
        update_summary_label();
        draw_canvas_cb(w, nullptr, nullptr);
    }
}

/* -------------------------------------------------------------------------
 * UI Callbacks & Preset Layouts
 * ------------------------------------------------------------------------- */
static void select_mon_btn_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    if (g_disp.updating_widgets) return;
    int idx = (int)(intptr_t)client_data;
    if (cbs && !cbs->set) {
        XmToggleButtonSetState(w, True, False);
        return;
    }
    if (idx >= 0 && idx < g_disp.count) {
        g_disp.selected = idx;
        update_controls_from_selected();
        draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
    }
}

static void toggle_enable_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    if (g_disp.updating_widgets) return;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    bool want = (cbs && cbs->set);
    if (!want) {
        int others = 0;
        for (int i = 0; i < g_disp.count; ++i) {
            if (i != g_disp.selected && g_disp.outs[i].enabled) others++;
        }
        if (others == 0) {
            /* Keep at least one monitor enabled */
            XmToggleButtonSetState(w, True, False);
            return;
        }
    }
    g_disp.outs[g_disp.selected].enabled = want;
    normalize_layout_origin();
    update_summary_label();
    draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
}

static void select_res_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    if (g_disp.updating_widgets) return;
    int midx = (int)(intptr_t)client_data;
    DispOutputInfo *o = &g_disp.outs[g_disp.selected];
    if (midx >= 0 && midx < o->mode_count) {
        o->width = o->modes[midx].w;
        o->height = o->modes[midx].h;
        update_summary_label();
        draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
    }
}

static void select_rot_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    if (g_disp.updating_widgets) return;
    int ridx = (int)(intptr_t)client_data;
    if (ridx >= 0 && ridx < 4) {
        g_disp.outs[g_disp.selected].rotation = k_rot_values[ridx];
        update_summary_label();
        draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
    }
}

static void select_dpi_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    if (g_disp.updating_widgets) return;
    int didx = (int)(intptr_t)client_data;
    if (didx >= 0 && didx < 5) {
        g_disp.outs[g_disp.selected].dpi = k_dpi_values[didx];
        update_summary_label();
        draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
    }
}

static void preset_layout_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    int kind = (int)(intptr_t)client_data;
    if (kind == 3) { /* Refresh from compositor / RandR */
        query_outputs();
        update_controls_from_selected();
        draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
        return;
    }

    int cx = 0, cy = 0;
    for (int i = 0; i < g_disp.count; ++i) {
        if (!g_disp.outs[i].enabled) continue;
        if (kind == 0) { /* Side-by-Side */
            g_disp.outs[i].x = cx;
            g_disp.outs[i].y = 0;
            cx += effective_w(&g_disp.outs[i]);
        } else if (kind == 1) { /* Stacked Vertically */
            g_disp.outs[i].x = 0;
            g_disp.outs[i].y = cy;
            cy += effective_h(&g_disp.outs[i]);
        } else if (kind == 2) { /* Mirror at (0, 0) */
            g_disp.outs[i].x = 0;
            g_disp.outs[i].y = 0;
        }
    }
    update_summary_label();
    draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
}

static void rebuild_monitor_buttons(void) {
    if (!g_disp.mon_btn_rc) return;
    for (int i = 0; i < MAX_DISP_OUTPUTS; ++i) {
        if (g_disp.mon_btns[i]) {
            XtDestroyWidget(g_disp.mon_btns[i]);
            g_disp.mon_btns[i] = nullptr;
        }
    }
    for (int i = 0; i < g_disp.count; ++i) {
        char lbl[96];
        snprintf(lbl, sizeof(lbl), "%d: %s", i + 1, g_disp.outs[i].name);
        XmString xs = CMPSTR(lbl);
        g_disp.mon_btns[i] = XtVaCreateManagedWidget(
            "monSelectBtn", xmToggleButtonWidgetClass, g_disp.mon_btn_rc,
            XmNlabelString, xs,
            XmNindicatorOn, False,
            XmNshadowThickness, 2,
            XmNset, (i == g_disp.selected) ? True : False,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(g_disp.mon_btns[i], XmNvalueChangedCallback, select_mon_btn_cb, (XtPointer)(intptr_t)i);
    }
}

static void update_controls_from_selected(void) {
    if (!style.displayDialog || g_disp.count <= 0) return;
    g_disp.updating_widgets = true;

    rebuild_monitor_buttons();
    const DispOutputInfo *o = &g_disp.outs[g_disp.selected];

    if (g_disp.enable_toggle) {
        XmToggleButtonSetState(g_disp.enable_toggle, o->enabled ? True : False, False);
    }

    /* Update Resolution OptionMenu entries */
    for (int i = 0; i < MAX_DISP_MODES; ++i) {
        if (!g_disp.res_btns[i]) continue;
        if (i < o->mode_count) {
            char mlbl[48];
            snprintf(mlbl, sizeof(mlbl), "%d x %d", o->modes[i].w, o->modes[i].h);
            XmString xs = CMPSTR(mlbl);
            XtVaSetValues(g_disp.res_btns[i], XmNlabelString, xs, NULL);
            XmStringFree(xs);
            XtManageChild(g_disp.res_btns[i]);
            if (o->modes[i].w == o->width && o->modes[i].h == o->height && g_disp.res_opt_menu) {
                XtVaSetValues(g_disp.res_opt_menu, XmNmenuHistory, g_disp.res_btns[i], NULL);
            }
        } else {
            XtUnmanageChild(g_disp.res_btns[i]);
        }
    }

    /* Update Rotation OptionMenu */
    int rot_idx = 0;
    for (int i = 0; i < 4; ++i) {
        if (k_rot_values[i] == o->rotation) {
            rot_idx = i;
            break;
        }
    }
    if (g_disp.rot_opt_menu && g_disp.rot_btns[rot_idx]) {
        XtVaSetValues(g_disp.rot_opt_menu, XmNmenuHistory, g_disp.rot_btns[rot_idx], NULL);
    }

    /* Update Scale / DPI OptionMenu */
    int dpi_idx = 0;
    int best_dist = 9999;
    for (int i = 0; i < 5; ++i) {
        int d = abs(k_dpi_values[i] - o->dpi);
        if (d < best_dist) {
            best_dist = d;
            dpi_idx = i;
        }
    }
    if (g_disp.dpi_opt_menu && g_disp.dpi_btns[dpi_idx]) {
        XtVaSetValues(g_disp.dpi_opt_menu, XmNmenuHistory, g_disp.dpi_btns[dpi_idx], NULL);
    }

    update_summary_label();
    g_disp.updating_widgets = false;
}

static void dialog_button_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    DtDialogBoxCallbackStruct *cb = (DtDialogBoxCallbackStruct *)call_data;
    if (!cb) return;

    switch (cb->button_position) {
    case BTN_OK:
        normalize_layout_origin();
        apply_outputs(g_disp.outs, g_disp.count);
        memcpy(g_disp.orig_outs, g_disp.outs, sizeof(g_disp.outs));
        g_disp.orig_count = g_disp.count;
        XtUnmanageChild(style.displayDialog);
        break;

    case BTN_APPLY:
        normalize_layout_origin();
        apply_outputs(g_disp.outs, g_disp.count);
        memcpy(g_disp.orig_outs, g_disp.outs, sizeof(g_disp.outs));
        g_disp.orig_count = g_disp.count;
        draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
        break;

    case BTN_CANCEL:
        memcpy(g_disp.outs, g_disp.orig_outs, sizeof(g_disp.outs));
        g_disp.count = g_disp.orig_count;
        apply_outputs(g_disp.orig_outs, g_disp.orig_count);
        XtUnmanageChild(style.displayDialog);
        break;

    case BTN_HELP:
        XtCallCallbacks(style.displayDialog, XmNhelpCallback, nullptr);
        break;

    default:
        break;
    }
}

static void build_display_dialog(Widget shell) {
    Arg args[MAX_ARGS];
    int n;
    XmString button_string[4];

    button_string[0] = CMPSTR((String)_DtOkString);
    button_string[1] = CMPSTR("Apply");
    button_string[2] = CMPSTR((String)_DtCancelString);
    button_string[3] = CMPSTR((String)_DtHelpString);

    XtSetArg(g_disp_save.posArgs[g_disp_save.poscnt], XmNbuttonCount, 4); g_disp_save.poscnt++;
    XtSetArg(g_disp_save.posArgs[g_disp_save.poscnt], XmNbuttonLabelStrings, button_string); g_disp_save.poscnt++;
    XtSetArg(g_disp_save.posArgs[g_disp_save.poscnt], XmNdefaultPosition, True); g_disp_save.poscnt++;

    style.displayDialog = __DtCreateDialogBoxDialog(
        shell, "displayDialog", g_disp_save.posArgs, g_disp_save.poscnt
    );
    XtAddCallback(style.displayDialog, XmNcallback, dialog_button_cb, nullptr);
    XtAddCallback(style.displayDialog, XmNhelpCallback, HelpRequestCB, (XtPointer)HELP_SCREEN_DIALOG);

    for (int i = 0; i < 4; ++i) XmStringFree(button_string[i]);

    Widget cancel_btn = _DtDialogBoxGetButton(style.displayDialog, BTN_CANCEL);
    n = 0;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    XtSetArg(args[n], XmNcancelButton, cancel_btn); n++;
    XtSetValues(style.displayDialog, args, n);

    n = 0;
    XtSetArg(args[n], XmNtitle, "Style Manager - Display & Monitors"); n++;
    XtSetArg(args[n], XmNuseAsyncGeometry, True); n++;
    XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;
    XtSetValues(XtParent(style.displayDialog), args, n);

    n = 0;
    XtSetArg(args[n], XmNallowOverlap, False); n++;
    XtSetArg(args[n], XmNchildType, XmWORK_AREA); n++;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
    Widget form = XmCreateForm(style.displayDialog, "displayForm", args, n);

    /* Top-left Style Manager component icon */
    n = 0;
    XtSetArg(args[n], XmNfillMode, XmFILL_SELF); n++;
    XtSetArg(args[n], XmNbehavior, XmICON_LABEL); n++;
    XtSetArg(args[n], XmNpixmapForeground, style.secBSCol); n++;
    XtSetArg(args[n], XmNpixmapBackground, style.secTSCol); n++;
    XtSetArg(args[n], XmNstring, NULL); n++;
    XtSetArg(args[n], XmNshadowThickness, 0); n++;
    XtSetArg(args[n], XmNimageName, DISPLAY_ICON); n++;
    XtSetArg(args[n], XmNtraversalOn, False); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    g_disp.pict_icon = _DtCreateIcon(form, "displayPict", args, n);
    XtManageChild(g_disp.pict_icon);

    /* 1. Monitor Arrangement TitleBox */
    XmString tb1_str = CMPSTR("Monitor Arrangement (Click to Select, Drag to Position)");
    n = 0;
    XtSetArg(args[n], XmNtitleString, tb1_str); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, g_disp.pict_icon); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    g_disp.canvas_tb = _DtCreateTitleBox(form, "arrangementTB", args, n);
    XmStringFree(tb1_str);
    XtManageChild(g_disp.canvas_tb);

    Widget arr_form = XtVaCreateManagedWidget(
        "arrForm", xmFormWidgetClass, g_disp.canvas_tb,
        XmNchildType, XmWORK_AREA,
        XmNmarginWidth, 6,
        XmNmarginHeight, 6,
        NULL
    );

    Widget cframe = XtVaCreateManagedWidget(
        "canvasFrame", xmFrameWidgetClass, arr_form,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 2,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    g_disp.canvas = XtVaCreateManagedWidget(
        "monitorCanvas", xmDrawingAreaWidgetClass, cframe,
        XmNwidth, 500,
        XmNheight, 196,
        NULL
    );
    XtAddCallback(g_disp.canvas, XmNexposeCallback, draw_canvas_cb, nullptr);
    XtAddCallback(g_disp.canvas, XmNresizeCallback, draw_canvas_cb, nullptr);
    XtAddEventHandler(
        g_disp.canvas,
        ButtonPressMask | ButtonReleaseMask | Button1MotionMask,
        False, canvas_pointer_eh, nullptr
    );

    Widget preset_rc = XtVaCreateManagedWidget(
        "presetRC", xmRowColumnWidgetClass, arr_form,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 6,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, cframe,
        XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    static const char *const preset_names[4] = {
        "Side-by-Side", "Stack Vertically", "Mirror (0,0)", "Detect / Refresh"
    };
    for (int i = 0; i < 4; ++i) {
        XmString xs = CMPSTR((char *)preset_names[i]);
        Widget b = XtVaCreateManagedWidget(
            "presetBtn", xmPushButtonGadgetClass, preset_rc,
            XmNlabelString, xs,
            XmNmarginWidth, 8,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(b, XmNactivateCallback, preset_layout_cb, (XtPointer)(intptr_t)i);
    }

    g_disp.summary_lbl = XtVaCreateManagedWidget(
        "dispSummary", xmLabelGadgetClass, arr_form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, preset_rc,
        XmNtopOffset, 6,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );

    /* 2. Selected Monitor Settings TitleBox */
    XmString tb2_str = CMPSTR("Selected Monitor Settings");
    n = 0;
    XtSetArg(args[n], XmNtitleString, tb2_str); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, g_disp.canvas_tb); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    g_disp.settings_tb = _DtCreateTitleBox(form, "monitorSettingsTB", args, n);
    XmStringFree(tb2_str);
    XtManageChild(g_disp.settings_tb);

    Widget set_rc = XtVaCreateManagedWidget(
        "setRC", xmRowColumnWidgetClass, g_disp.settings_tb,
        XmNchildType, XmWORK_AREA,
        XmNorientation, XmVERTICAL,
        XmNspacing, 6,
        XmNmarginWidth, 8,
        XmNmarginHeight, 6,
        NULL
    );

    /* Row 1: Monitor selector buttons + Enable toggle */
    Widget row1 = XtVaCreateManagedWidget(
        "monRow", xmRowColumnWidgetClass, set_rc,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 10,
        NULL
    );
    XmString x_mon = CMPSTR("Monitor:");
    XtVaCreateManagedWidget("monLbl", xmLabelGadgetClass, row1, XmNlabelString, x_mon, NULL);
    XmStringFree(x_mon);

    g_disp.mon_btn_rc = XtVaCreateManagedWidget(
        "monBtnRC", xmRowColumnWidgetClass, row1,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 4,
        NULL
    );

    XmString x_en = CMPSTR("Enable Output");
    g_disp.enable_toggle = XtVaCreateManagedWidget(
        "enableOutToggle", xmToggleButtonWidgetClass, row1,
        XmNlabelString, x_en,
        NULL
    );
    XmStringFree(x_en);
    XtAddCallback(g_disp.enable_toggle, XmNvalueChangedCallback, toggle_enable_cb, nullptr);

    /* Row 2: Resolution + Rotation + Scale / HiDPI OptionMenus */
    Widget row2 = XtVaCreateManagedWidget(
        "optRow", xmRowColumnWidgetClass, set_rc,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 12,
        NULL
    );

    g_disp.res_pulldown = XmCreatePulldownMenu(row2, "resPD", nullptr, 0);
    for (int i = 0; i < MAX_DISP_MODES; ++i) {
        XmString xs = CMPSTR("1280 x 900");
        g_disp.res_btns[i] = XtVaCreateManagedWidget(
            "resItem", xmPushButtonGadgetClass, g_disp.res_pulldown,
            XmNlabelString, xs,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(g_disp.res_btns[i], XmNactivateCallback, select_res_cb, (XtPointer)(intptr_t)i);
    }
    XmString x_res = CMPSTR("Resolution:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, x_res); n++;
    XtSetArg(args[n], XmNsubMenuId, g_disp.res_pulldown); n++;
    g_disp.res_opt_menu = XmCreateOptionMenu(row2, "resOpt", args, n);
    XmStringFree(x_res);
    XtManageChild(g_disp.res_opt_menu);

    Widget rot_pd = XmCreatePulldownMenu(row2, "rotPD", nullptr, 0);
    for (int i = 0; i < 4; ++i) {
        XmString xs = CMPSTR((char *)k_rot_labels[i]);
        g_disp.rot_btns[i] = XtVaCreateManagedWidget(
            "rotItem", xmPushButtonGadgetClass, rot_pd,
            XmNlabelString, xs,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(g_disp.rot_btns[i], XmNactivateCallback, select_rot_cb, (XtPointer)(intptr_t)i);
    }
    XmString x_rot = CMPSTR("Rotation:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, x_rot); n++;
    XtSetArg(args[n], XmNsubMenuId, rot_pd); n++;
    g_disp.rot_opt_menu = XmCreateOptionMenu(row2, "rotOpt", args, n);
    XmStringFree(x_rot);
    XtManageChild(g_disp.rot_opt_menu);

    Widget dpi_pd = XmCreatePulldownMenu(row2, "dpiPD", nullptr, 0);
    for (int i = 0; i < 5; ++i) {
        XmString xs = CMPSTR((char *)k_dpi_labels[i]);
        g_disp.dpi_btns[i] = XtVaCreateManagedWidget(
            "dpiItem", xmPushButtonGadgetClass, dpi_pd,
            XmNlabelString, xs,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(g_disp.dpi_btns[i], XmNactivateCallback, select_dpi_cb, (XtPointer)(intptr_t)i);
    }
    XmString x_dpi = CMPSTR("Scale:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, x_dpi); n++;
    XtSetArg(args[n], XmNsubMenuId, dpi_pd); n++;
    g_disp.dpi_opt_menu = XmCreateOptionMenu(row2, "dpiOpt", args, n);
    XmStringFree(x_dpi);
    XtManageChild(g_disp.dpi_opt_menu);

    XtManageChild(form);
}

void popup_displayBB(Widget shell) {
    if (style.displayDialog == nullptr) {
        _DtTurnOnHourGlass(shell);
        query_outputs();
        build_display_dialog(shell);
        update_controls_from_selected();
        XtManageChild(style.displayDialog);
        _DtTurnOffHourGlass(shell);
    } else {
        query_outputs();
        update_controls_from_selected();
        XtManageChild(style.displayDialog);
        if (XtIsRealized(style.displayDialog)) {
            raiseWindow(XtWindow(XtParent(style.displayDialog)));
            draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
        }
    }
}

void restoreDisplay(Widget shell, XrmDatabase db) {
    (void)shell;
    (void)db;
}

void saveDisplay(int fd) {
    (void)fd;
}
