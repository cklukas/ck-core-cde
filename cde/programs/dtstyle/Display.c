/*
 * CDE - Common Desktop Environment
 *
 * Display.c - Graphical Multi-Monitor Arrangement, Resolution, Rotation &
 *             Per-Output HiDPI Scaling for Style Manager (Wayland & X11/XLibre).
 */

#define _GNU_SOURCE
#include <X11/Shell.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <Xm/MwmUtil.h>
#include <dlfcn.h>

#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <Xm/DrawingA.h>
#include <Xm/FileSB.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/Scale.h>
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

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
    bool vrr_enabled;    /* Per-output Variable Refresh Rate / Adaptive Sync */
    char color_profile[256]; /* "default", "srgb", "display-p3", "hdr10", or .icc path */
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

    /* Night Light & Ergonomics state */
    bool nl_enabled;
    int nl_mode;         /* 0=always, 1=schedule (20:00-06:00), 2=sunset */
    int nl_temp_k;       /* 2700 .. 6500 */
    int nl_start_min;
    int nl_end_min;
    bool orig_nl_enabled;
    int orig_nl_mode;
    int orig_nl_temp_k;
    int orig_nl_start_min;
    int orig_nl_end_min;

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
    Widget vrr_toggle;
    Widget res_pulldown;
    Widget res_opt_menu;
    Widget res_btns[MAX_DISP_MODES];
    int res_btn_count;
    Widget rot_opt_menu;
    Widget rot_btns[4];
    Widget dpi_opt_menu;
    Widget dpi_btns[5];
    Widget color_opt_menu;
    Widget color_btns[5];
    Widget color_browse_btn;
    Widget color_path_lbl;
    Widget icc_fsb_dlg;
    Widget ergo_tb;
    Widget nl_toggle;
    Widget nl_mode_opt_menu;
    Widget nl_mode_btns[3];
    Widget nl_temp_scale;
    bool updating_widgets;

    /* On-screen monitor identification overlay badges (X11/XLibre & Wayland) */
    Widget ident_shells[MAX_DISP_OUTPUTS];
    Widget ident_canvases[MAX_DISP_OUTPUTS];
    cairo_surface_t *ident_bg_surfs[MAX_DISP_OUTPUTS];
    bool ident_popped[MAX_DISP_OUTPUTS];
    int ident_last_x[MAX_DISP_OUTPUTS];
    int ident_last_y[MAX_DISP_OUTPUTS];
    bool ident_atexit_registered;
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

static const char *const k_nl_mode_ids[3] = { "always", "schedule", "sunset" };
static const char *const k_nl_mode_labels[3] = {
    "Always On",
    "Night Schedule (20:00-06:00)",
    "Sunset to Sunrise",
};

static const char *const k_color_profile_ids[5] = {
    "default",
    "srgb",
    "display-p3",
    "hdr10",
    "custom",
};
static const char *const k_color_profile_labels[5] = {
    "Native",
    "sRGB Clamp",
    "Display P3",
    "HDR10 (PQ)",
    "Custom ICC...",
};

static int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double clamp_d(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void kelvin_to_rgb_mult(int temp_k, double *out_r, double *out_g, double *out_b) {
    temp_k = clamp_i(temp_k, 2700, 6500);
    if (temp_k >= 6500) {
        *out_r = 1.0;
        *out_g = 1.0;
        *out_b = 1.0;
        return;
    }
    double t = (double)temp_k / 100.0;
    double ref_t = 65.0;
    double g_raw = (99.4708025861 * log(t) - 161.1195681661) / 255.0;
    double g_ref = (99.4708025861 * log(ref_t) - 161.1195681661) / 255.0;
    double b_raw = (t <= 19.0) ? 0.0 : (138.5177312231 * log(t - 10.0) - 305.0447927307) / 255.0;
    double b_ref = (138.5177312231 * log(ref_t - 10.0) - 305.0447927307) / 255.0;
    double g = (g_ref > 0.01) ? clamp_d(g_raw / g_ref, 0.50, 1.0) : 1.0;
    double b = (b_ref > 0.01) ? clamp_d(b_raw / b_ref, 0.20, 1.0) : 1.0;
    if (b >= g) b = g - 0.02;
    *out_r = 1.0;
    *out_g = g;
    *out_b = b;
}

static bool is_night_light_active_now(bool enabled, int mode_idx, int start_min, int end_min) {
    if (!enabled) return false;
    if (mode_idx == 0) return true; /* Always On */
    int now_min = 0;
    const char *ov = getenv("CK_NIGHT_LIGHT_TEST_MINUTES");
    if (ov && ov[0]) {
        now_min = clamp_i(atoi(ov), 0, 1439);
    } else {
        time_t now = time(nullptr);
        struct tm ltm;
        if (localtime_r(&now, &ltm)) now_min = ltm.tm_hour * 60 + ltm.tm_min;
    }
    int s = (mode_idx == 2) ? (19 * 60 + 30) : start_min;
    int e = (mode_idx == 2) ? (6 * 60 + 30) : end_min;
    s = ((s % 1440) + 1440) % 1440;
    e = ((e % 1440) + 1440) % 1440;
    if (s == e) return true;
    if (s > e) return (now_min >= s || now_min < e);
    return (now_min >= s && now_min < e);
}

static void ergonomics_file_path(char *out, size_t out_sz) {
    const char *home = (style.home && style.home[0]) ? style.home : getenv("HOME");
    if (!home || !home[0]) home = "/var/tmp";
    char dt_dir[PATH_MAX];
    snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", home);
    mkdir(dt_dir, 0755);
    snprintf(out, out_sz, "%s/display_ergonomics.conf", dt_dir);
}

static void save_ergonomics_file(
    bool nl_enabled, int nl_mode, int nl_temp_k, int nl_start_min, int nl_end_min,
    const DispOutputInfo *arr, int count
) {
    char path[PATH_MAX], tmp[PATH_MAX + 32];
    ergonomics_file_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    int m = clamp_i(nl_mode, 0, 2);
    fprintf(fp, "night_light_enabled=%d\n", nl_enabled ? 1 : 0);
    fprintf(fp, "night_light_mode=%s\n", nl_enabled ? k_nl_mode_ids[m] : "off");
    fprintf(fp, "night_light_schedule=%s\n", k_nl_mode_ids[m]);
    fprintf(fp, "night_light_temp=%d\n", clamp_i(nl_temp_k, 2700, 6500));
    fprintf(fp, "night_light_start_min=%d\n", nl_start_min);
    fprintf(fp, "night_light_end_min=%d\n", nl_end_min);
    for (int i = 0; i < count; ++i) {
        if (arr[i].name[0]) {
            fprintf(fp, "vrr.%s=%d\n", arr[i].name, arr[i].vrr_enabled ? 1 : 0);
            const char *cp = arr[i].color_profile[0] ? arr[i].color_profile : "default";
            fprintf(fp, "color_profile.%s=%s\n", arr[i].name, cp);
        }
    }
    if (fclose(fp) == 0) {
        if (rename(tmp, path) != 0) unlink(tmp);
    } else {
        unlink(tmp);
    }
}

static void load_ergonomics_into_state(void) {
    g_disp.nl_enabled = false;
    g_disp.nl_mode = 1; /* default Night Schedule (20:00-06:00) */
    g_disp.nl_temp_k = 4000;
    g_disp.nl_start_min = 20 * 60;
    g_disp.nl_end_min = 6 * 60;

    char path[PATH_MAX];
    ergonomics_file_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            char *p = line;
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p || *p == '#') continue;
            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *k = p;
            const char *v = eq + 1;
            if (strcmp(k, "night_light_enabled") == 0) {
                g_disp.nl_enabled = (atoi(v) != 0);
            } else if (strcmp(k, "night_light_mode") == 0 || strcmp(k, "night_light_schedule") == 0) {
                if (strcmp(v, "always") == 0) g_disp.nl_mode = 0;
                else if (strcmp(v, "schedule") == 0) g_disp.nl_mode = 1;
                else if (strcmp(v, "sunset") == 0) g_disp.nl_mode = 2;
            } else if (strcmp(k, "night_light_temp") == 0) {
                g_disp.nl_temp_k = clamp_i(atoi(v), 2700, 6500);
            } else if (strcmp(k, "night_light_start_min") == 0) {
                g_disp.nl_start_min = clamp_i(atoi(v), 0, 1439);
            } else if (strcmp(k, "night_light_end_min") == 0) {
                g_disp.nl_end_min = clamp_i(atoi(v), 0, 1439);
            } else if (strncmp(k, "vrr.", 4) == 0) {
                for (int i = 0; i < g_disp.count; ++i) {
                    if (strcmp(g_disp.outs[i].name, k + 4) == 0) {
                        g_disp.outs[i].vrr_enabled = (atoi(v) != 0);
                    }
                }
            } else if (strncmp(k, "color_profile.", 14) == 0) {
                for (int i = 0; i < g_disp.count; ++i) {
                    if (strcmp(g_disp.outs[i].name, k + 14) == 0) {
                        snprintf(
                            g_disp.outs[i].color_profile,
                            sizeof(g_disp.outs[i].color_profile),
                            "%s",
                            v[0] ? v : "default"
                        );
                    }
                }
            }
        }
        fclose(fp);
    }

    /* Also sync live Wayland _DT_NIGHT_LIGHT_STATE / _DT_VRR_STATE if published */
    if (style.display) {
        Atom nl_a = XInternAtom(style.display, "_DT_NIGHT_LIGHT_STATE", True);
        if (nl_a != None) {
            Atom at = None;
            int af = 0;
            unsigned long ni = 0, rem = 0;
            unsigned char *data = nullptr;
            if (XGetWindowProperty(style.display, style.root, nl_a, 0, 512, False,
                                   AnyPropertyType, &at, &af, &ni, &rem, &data) == Success && data) {
                char buf[512];
                size_t cl = ni < sizeof(buf) - 1 ? (size_t)ni : sizeof(buf) - 1;
                memcpy(buf, data, cl);
                buf[cl] = '\0';
                XFree(data);
                int en = 0, tk = 4000;
                char mode[32] = {0};
                if (sscanf(buf, "enabled=%d", &en) == 1) g_disp.nl_enabled = (en != 0);
                const char *mp = strstr(buf, "mode=");
                if (mp && sscanf(mp + 5, "%31s", mode) == 1) {
                    if (strcmp(mode, "always") == 0) g_disp.nl_mode = 0;
                    else if (strcmp(mode, "schedule") == 0) g_disp.nl_mode = 1;
                    else if (strcmp(mode, "sunset") == 0) g_disp.nl_mode = 2;
                }
                const char *tp = strstr(buf, "temp=");
                if (tp && sscanf(tp + 5, "%d", &tk) == 1) {
                    g_disp.nl_temp_k = clamp_i(tk, 2700, 6500);
                }
            }
        }
        Atom vrr_a = XInternAtom(style.display, "_DT_VRR_STATE", True);
        if (vrr_a != None) {
            Atom at = None;
            int af = 0;
            unsigned long ni = 0, rem = 0;
            unsigned char *data = nullptr;
            if (XGetWindowProperty(style.display, style.root, vrr_a, 0, 512, False,
                                   AnyPropertyType, &at, &af, &ni, &rem, &data) == Success && data) {
                char buf[512];
                size_t cl = ni < sizeof(buf) - 1 ? (size_t)ni : sizeof(buf) - 1;
                memcpy(buf, data, cl);
                buf[cl] = '\0';
                XFree(data);
                char *savep = nullptr;
                for (char *tok = strtok_r(buf, ",", &savep); tok; tok = strtok_r(nullptr, ",", &savep)) {
                    char *eq = strchr(tok, '=');
                    if (!eq) continue;
                    *eq = '\0';
                    for (int i = 0; i < g_disp.count; ++i) {
                        if (strcmp(g_disp.outs[i].name, tok) == 0) {
                            g_disp.outs[i].vrr_enabled = (atoi(eq + 1) != 0);
                        }
                    }
                }
            }
        }
        Atom cp_a = XInternAtom(style.display, "_DT_COLOR_PROFILE_STATE", True);
        if (cp_a != None) {
            Atom at = None;
            int af = 0;
            unsigned long ni = 0, rem = 0;
            unsigned char *data = nullptr;
            if (XGetWindowProperty(style.display, style.root, cp_a, 0, 1024, False,
                                   AnyPropertyType, &at, &af, &ni, &rem, &data) == Success && data) {
                char buf[1024];
                size_t cl = ni < sizeof(buf) - 1 ? (size_t)ni : sizeof(buf) - 1;
                memcpy(buf, data, cl);
                buf[cl] = '\0';
                XFree(data);
                char *savep = nullptr;
                for (char *tok = strtok_r(buf, ",", &savep); tok; tok = strtok_r(nullptr, ",", &savep)) {
                    char *eq = strchr(tok, '=');
                    if (!eq) continue;
                    *eq = '\0';
                    for (int i = 0; i < g_disp.count; ++i) {
                        if (strcmp(g_disp.outs[i].name, tok) == 0) {
                            snprintf(
                                g_disp.outs[i].color_profile,
                                sizeof(g_disp.outs[i].color_profile),
                                "%s",
                                (eq[1] != '\0') ? (eq + 1) : "default"
                            );
                        }
                    }
                }
            }
        }
    }
    for (int i = 0; i < g_disp.count; ++i) {
        if (!g_disp.outs[i].color_profile[0]) {
            snprintf(g_disp.outs[i].color_profile, sizeof(g_disp.outs[i].color_profile), "default");
        }
    }

    g_disp.orig_nl_enabled = g_disp.nl_enabled;
    g_disp.orig_nl_mode = g_disp.nl_mode;
    g_disp.orig_nl_temp_k = g_disp.nl_temp_k;
    g_disp.orig_nl_start_min = g_disp.nl_start_min;
    g_disp.orig_nl_end_min = g_disp.nl_end_min;
    memcpy(g_disp.orig_outs, g_disp.outs, sizeof(g_disp.outs));
}

static int effective_w(const DispOutputInfo *o) {
    int rw = (o->rotation == 90 || o->rotation == 270) ? o->height : o->width;
    if (g_disp.is_wayland && o->dpi > 0) {
        double sc = (double)o->dpi / 96.0;
        if (sc > 0.25) rw = (int)lround((double)rw / sc);
    }
    return rw > 0 ? rw : 1;
}

static int effective_h(const DispOutputInfo *o) {
    int rh = (o->rotation == 90 || o->rotation == 270) ? o->width : o->height;
    if (g_disp.is_wayland && o->dpi > 0) {
        double sc = (double)o->dpi / 96.0;
        if (sc > 0.25) rh = (int)lround((double)rh / sc);
    }
    return rh > 0 ? rh : 1;
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

                    /* Read XLibre per-output DPI & VariableRefresh properties if set */
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
                    Atom vrr_prop = XInternAtom(style.display, "VariableRefresh", True);
                    if (vrr_prop == None) vrr_prop = XInternAtom(style.display, "_DT_VRR_ENABLED", True);
                    if (vrr_prop != None &&
                        XRRGetOutputProperty(
                            style.display, res->outputs[i], vrr_prop, 0, 1, False, False,
                            AnyPropertyType, &act_t, &act_f, &nitems, &rem, &pdata
                        ) == Success && pdata) {
                        if ((act_f == 32 && *(long *)pdata != 0) ||
                            (act_f == 8 && pdata[0] != 0)) {
                            o->vrr_enabled = true;
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
    load_ergonomics_into_state();
}

static void apply_ergonomics(
    bool nl_enabled, int nl_mode, int nl_temp_k, int nl_start_min, int nl_end_min,
    const DispOutputInfo *arr, int count, bool persist_file
) {
    int m = clamp_i(nl_mode, 0, 2);
    int temp_k = clamp_i(nl_temp_k, 2700, 6500);
    const char *mode_str = nl_enabled ? k_nl_mode_ids[m] : "off";

    if (persist_file) {
        save_ergonomics_file(nl_enabled, m, temp_k, nl_start_min, nl_end_min, arr, count);
    }

    if (g_disp.is_wayland) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "f.set_night_light %d %s %d %d %d",
                 nl_enabled ? 1 : 0, mode_str, temp_k, nl_start_min, nl_end_min);
        send_wayland_wm_request(cmd);
        for (int i = 0; i < count; ++i) {
            if (!arr[i].name[0]) continue;
            snprintf(cmd, sizeof(cmd), "f.set_vrr %s %d",
                     arr[i].name, arr[i].vrr_enabled ? 1 : 0);
            send_wayland_wm_request(cmd);
            const char *cp = arr[i].color_profile[0] ? arr[i].color_profile : "default";
            snprintf(cmd, sizeof(cmd), "f.set_output_color_profile %s %s",
                     arr[i].name, cp);
            send_wayland_wm_request(cmd);
        }
        return;
    }

#ifdef HAVE_XRANDR
    bool active = is_night_light_active_now(nl_enabled, m, nl_start_min, nl_end_min);
    double r_mult = 1.0, g_mult = 1.0, b_mult = 1.0;
    if (active) {
        kelvin_to_rgb_mult(temp_k, &r_mult, &g_mult, &b_mult);
    }
    int r_pm = (int)lround(r_mult * 1000.0);
    int g_pm = (int)lround(g_mult * 1000.0);
    int b_pm = (int)lround(b_mult * 1000.0);
    bool any_vrr = false;

    XRRScreenResources *res = XRRGetScreenResourcesCurrent(style.display, style.root);
    if (res) {
        for (int c = 0; c < res->ncrtc; ++c) {
            int gsz = XRRGetCrtcGammaSize(style.display, res->crtcs[c]);
            if (gsz > 1) {
                XRRCrtcGamma *gam = XRRAllocGamma(gsz);
                if (gam) {
                    for (int gi = 0; gi < gsz; ++gi) {
                        double base = ((double)gi / (double)(gsz - 1)) * 65535.0;
                        gam->red[gi] = (unsigned short)clamp_i((int)lround(base * r_mult), 0, 65535);
                        gam->green[gi] = (unsigned short)clamp_i((int)lround(base * g_mult), 0, 65535);
                        gam->blue[gi] = (unsigned short)clamp_i((int)lround(base * b_mult), 0, 65535);
                    }
                    XRRSetCrtcGamma(style.display, res->crtcs[c], gam);
                    XRRFreeGamma(gam);
                }
            }
        }

        Atom vrr_hw_atom = XInternAtom(style.display, "VariableRefresh", False);
        Atom vrr_dt_atom = XInternAtom(style.display, "_DT_VRR_ENABLED", False);
        for (int i = 0; i < count; ++i) {
            if (arr[i].vrr_enabled) any_vrr = true;
            if (arr[i].rr_output == None) continue;
            long val = arr[i].vrr_enabled ? 1 : 0;
            XRRChangeOutputProperty(
                style.display, arr[i].rr_output, vrr_dt_atom, XA_INTEGER, 32,
                PropModeReplace, (const unsigned char *)&val, 1
            );
            /* Also set driver VariableRefresh output property (XLibre / modesetting / amdgpu) */
            int nprops = 0;
            Atom *props = XRRListOutputProperties(style.display, arr[i].rr_output, &nprops);
            bool has_hw_vrr = false;
            if (props) {
                for (int pi = 0; pi < nprops; ++pi) {
                    if (props[pi] == vrr_hw_atom) {
                        has_hw_vrr = true;
                        break;
                    }
                }
                XFree(props);
            }
            if (has_hw_vrr) {
                unsigned char bval = (unsigned char)(arr[i].vrr_enabled ? 1 : 0);
                XRRChangeOutputProperty(
                    style.display, arr[i].rr_output, vrr_hw_atom, XA_INTEGER, 8,
                    PropModeReplace, &bval, 1
                );
            } else {
                XRRChangeOutputProperty(
                    style.display, arr[i].rr_output, vrr_hw_atom, XA_INTEGER, 32,
                    PropModeReplace, (const unsigned char *)&val, 1
                );
            }
        }
        XRRFreeScreenResources(res);
    }

    Atom nl_state_atom = XInternAtom(style.display, "_DT_NIGHT_LIGHT_STATE", False);
    char state_buf[320];
    int sn = snprintf(
        state_buf, sizeof(state_buf),
        "enabled=%d active=%d mode=%s temp=%d r_permille=%d g_permille=%d b_permille=%d vrr=%d",
        nl_enabled ? 1 : 0, active ? 1 : 0, mode_str, temp_k, r_pm, g_pm, b_pm, any_vrr ? 1 : 0
    );
    if (sn > 0) {
        XChangeProperty(style.display, style.root, nl_state_atom, XA_STRING, 8,
                        PropModeReplace, (const unsigned char *)state_buf, sn);
    }

    Atom vrr_state_atom = XInternAtom(style.display, "_DT_VRR_STATE", False);
    char vrr_buf[512];
    int vp = 0;
    vrr_buf[0] = '\0';
    for (int i = 0; i < count; ++i) {
        int n = snprintf(vrr_buf + vp, sizeof(vrr_buf) - (size_t)vp, "%s%s=%d",
                         vp > 0 ? "," : "", arr[i].name, arr[i].vrr_enabled ? 1 : 0);
        if (n > 0 && vp + n < (int)sizeof(vrr_buf)) vp += n;
    }
    XChangeProperty(style.display, style.root, vrr_state_atom, XA_STRING, 8,
                    PropModeReplace, (const unsigned char *)vrr_buf, vp);

    Atom cp_state_atom = XInternAtom(style.display, "_DT_COLOR_PROFILE_STATE", False);
    Atom icc_atom = XInternAtom(style.display, "_ICC_PROFILE", False);
    char cp_buf[1024];
    int cpp = 0;
    cp_buf[0] = '\0';
    bool uploaded_icc = false;
    for (int i = 0; i < count; ++i) {
        const char *cp = arr[i].color_profile[0] ? arr[i].color_profile : "default";
        int n = snprintf(cp_buf + cpp, sizeof(cp_buf) - (size_t)cpp, "%s%s=%s",
                         cpp > 0 ? "," : "", arr[i].name, cp);
        if (n > 0 && cpp + n < (int)sizeof(cp_buf)) cpp += n;
        if (!uploaded_icc && cp[0] == '/') {
            FILE *ifp = fopen(cp, "rb");
            if (ifp) {
                struct stat st;
                if (fstat(fileno(ifp), &st) == 0 && st.st_size > 128 &&
                    st.st_size <= (4 * 1024 * 1024)) {
                    unsigned char *ibuf = malloc((size_t)st.st_size);
                    if (ibuf) {
                        if (fread(ibuf, 1, (size_t)st.st_size, ifp) == (size_t)st.st_size) {
                            XChangeProperty(
                                style.display, style.root, icc_atom, XA_CARDINAL, 8,
                                PropModeReplace, ibuf, (int)st.st_size
                            );
                            uploaded_icc = true;
                        }
                        free(ibuf);
                    }
                }
                fclose(ifp);
            }
        }
    }
    if (!uploaded_icc) {
        XDeleteProperty(style.display, style.root, icc_atom);
    }
    XChangeProperty(style.display, style.root, cp_state_atom, XA_STRING, 8,
                    PropModeReplace, (const unsigned char *)cp_buf, cpp);
    XSync(style.display, False);
#endif
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
 * On-Screen Monitor Identification Corner Number Overlays (Wayland & X11)
 * ------------------------------------------------------------------------- */
#define IDENT_BADGE_SZ     136
#define IDENT_BADGE_MARGIN  28

static bool g_x11_getimage_err = false;
static int ignore_getimage_xerr(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    g_x11_getimage_err = true;
    return 0;
}

static unsigned int extract_mask_byte(unsigned long px, unsigned long mask) {
    if (mask == 0) return 0;
    int shift = 0;
    unsigned long m = mask;
    while ((m & 1UL) == 0UL) {
        m >>= 1;
        shift++;
    }
    unsigned long v = (px & mask) >> shift;
    if (m >= 255UL) return (unsigned int)(v & 0xffUL);
    if (m == 0UL) return 0;
    return (unsigned int)((v * 255UL) / m);
}

static cairo_surface_t *capture_x11_root_patch(int rx, int ry, int rw, int rh) {
    if (!style.display || rw <= 0 || rh <= 0) return nullptr;
    int scr_w = DisplayWidth(style.display, style.screenNum);
    int scr_h = DisplayHeight(style.display, style.screenNum);
    if (rx < 0 || ry < 0 || rx + rw > scr_w || ry + rh > scr_h) return nullptr;

    XSync(style.display, False);
    g_x11_getimage_err = false;
    XErrorHandler prev = XSetErrorHandler(ignore_getimage_xerr);
    XImage *img = XGetImage(style.display, style.root, rx, ry, (unsigned int)rw, (unsigned int)rh, AllPlanes, ZPixmap);
    XSync(style.display, False);
    XSetErrorHandler(prev);
    if (!img || g_x11_getimage_err) {
        if (img) XDestroyImage(img);
        return nullptr;
    }

    Visual *vis = DefaultVisual(style.display, style.screenNum);
    unsigned long rmask = img->red_mask ? img->red_mask : (vis ? vis->red_mask : 0xff0000UL);
    unsigned long gmask = img->green_mask ? img->green_mask : (vis ? vis->green_mask : 0x00ff00UL);
    unsigned long bmask = img->blue_mask ? img->blue_mask : (vis ? vis->blue_mask : 0x0000ffUL);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, rw, rh);
    if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
        unsigned char *dst = cairo_image_surface_get_data(surf);
        int stride = cairo_image_surface_get_stride(surf);
        for (int y = 0; y < rh; ++y) {
            uint32_t *row = (uint32_t *)(dst + y * stride);
            for (int x = 0; x < rw; ++x) {
                unsigned long px = XGetPixel(img, x, y);
                unsigned int r = extract_mask_byte(px, rmask);
                unsigned int g = extract_mask_byte(px, gmask);
                unsigned int b = extract_mask_byte(px, bmask);
                row[x] = (0xffU << 24) | (r << 16) | (g << 8) | b;
            }
        }
        cairo_surface_mark_dirty(surf);
    }
    XDestroyImage(img);
    return surf;
}

static void apply_x11_click_through_shape(Window win) {
    if (!style.display || win == None) return;
    typedef void (*XShapeCombineRectanglesFn)(
        Display *, Window, int, int, int, XRectangle *, int, int, int
    );
    static bool s_resolved = false;
    static XShapeCombineRectanglesFn s_shape_fn = nullptr;
    if (!s_resolved) {
        s_resolved = true;
        s_shape_fn = (XShapeCombineRectanglesFn)dlsym(RTLD_DEFAULT, "XShapeCombineRectangles");
        if (!s_shape_fn) {
            void *h = dlopen("libXext.so.6", RTLD_LAZY | RTLD_LOCAL);
            if (h) s_shape_fn = (XShapeCombineRectanglesFn)dlsym(h, "XShapeCombineRectangles");
        }
    }
    if (s_shape_fn) {
        /* ShapeInput = 2, ShapeSet = 0, Unsorted = 0: empty input region -> 100% click-through */
        s_shape_fn(style.display, win, 2, 0, 0, nullptr, 0, 0, 0);
    }
}

static void draw_x11_ident_badge_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)call_data;
    if (!w || !XtIsRealized(w) || !style.display) return;
    int idx = (int)(intptr_t)client_data;
    if (idx < 0 || idx >= MAX_DISP_OUTPUTS) return;

    const int sz = IDENT_BADGE_SZ;
    Visual *vis = DefaultVisual(style.display, style.screenNum);
    cairo_surface_t *xsurf = cairo_xlib_surface_create(style.display, XtWindow(w), vis, sz, sz);
    cairo_t *cr = cairo_create(xsurf);

    /* Composite over captured underlying desktop/window pixels for genuine translucency on X11 */
    if (g_disp.ident_bg_surfs[idx]) {
        cairo_set_source_surface(cr, g_disp.ident_bg_surfs[idx], 0.0, 0.0);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgb(cr, 0.22, 0.28, 0.34);
        cairo_paint(cr);
    }

    cairo_set_source_rgba(cr, 0.09, 0.13, 0.17, 0.68);
    cairo_rectangle(cr, 0.0, 0.0, sz, sz);
    cairo_fill(cr);

    /* 3D CDE mitered bevel */
    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgba(cr, 0.88, 0.92, 0.96, 0.56);
    cairo_move_to(cr, 1.0, sz - 1.0);
    cairo_line_to(cr, 1.0, 1.0);
    cairo_line_to(cr, sz - 1.0, 1.0);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.03, 0.05, 0.07, 0.74);
    cairo_move_to(cr, sz - 1.0, 1.0);
    cairo_line_to(cr, sz - 1.0, sz - 1.0);
    cairo_line_to(cr, 1.0, sz - 1.0);
    cairo_stroke(cr);

    bool sel = (idx == g_disp.selected);
    if (sel) {
        cairo_set_source_rgba(cr, 0.98, 0.78, 0.26, 0.88);
        cairo_set_line_width(cr, 2.5);
        cairo_rectangle(cr, 4.25, 4.25, sz - 8.5, sz - 8.5);
        cairo_stroke(cr);
    }

    char num_str[16];
    snprintf(num_str, sizeof(num_str), "%d", idx + 1);
    const char *oname = (idx < g_disp.orig_count) ? g_disp.orig_outs[idx].name : "";

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 78.0);
    cairo_text_extents_t ext = {0};
    cairo_text_extents(cr, num_str, &ext);
    double nx = (sz - ext.width) * 0.5 - ext.x_bearing;
    double ny = (oname && oname[0]) ? 86.0 : 94.0;

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.62);
    cairo_move_to(cr, nx + 2.5, ny + 2.5);
    cairo_show_text(cr, num_str);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
    cairo_move_to(cr, nx, ny);
    cairo_show_text(cr, num_str);

    if (oname && oname[0]) {
        cairo_set_font_size(cr, 13.0);
        cairo_text_extents_t lext = {0};
        cairo_text_extents(cr, oname, &lext);
        double lx = (sz - lext.width) * 0.5 - lext.x_bearing;
        if (lx < 8.0) lx = 8.0;
        double ly = sz - 14.0;

        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
        cairo_move_to(cr, lx + 1.0, ly + 1.0);
        cairo_show_text(cr, oname);

        if (sel) cairo_set_source_rgba(cr, 0.99, 0.85, 0.42, 0.94);
        else     cairo_set_source_rgba(cr, 0.86, 0.92, 0.96, 0.88);
        cairo_move_to(cr, lx, ly);
        cairo_show_text(cr, oname);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(xsurf);
}

static void publish_x11_ident_state(const char *state_str) {
    if (!style.display || !state_str) return;
    Atom a = XInternAtom(style.display, "_DT_MONITOR_IDENTIFY_STATE", False);
    XChangeProperty(
        style.display, style.root, a, XA_STRING, 8, PropModeReplace,
        (const unsigned char *)state_str, (int)strlen(state_str)
    );
    XFlush(style.display);
}

static void sync_monitor_identify_overlays(bool show) {
    if (!style.display) return;

    if (g_disp.is_wayland) {
        if (!show || g_disp.count <= 0) {
            send_wayland_wm_request("f.identify_outputs 0");
            return;
        }
        char cmd[512];
        int pos = snprintf(cmd, sizeof(cmd), "f.identify_outputs 1 %d", g_disp.selected + 1);
        for (int i = 0; i < g_disp.count; ++i) {
            if (!g_disp.outs[i].name[0]) continue;
            int n = snprintf(
                cmd + pos, sizeof(cmd) - (size_t)pos, " %s=%d",
                g_disp.outs[i].name, i + 1
            );
            if (n > 0 && pos + n < (int)sizeof(cmd)) pos += n;
        }
        send_wayland_wm_request(cmd);
        return;
    }

    if (!show || g_disp.orig_count <= 0) {
        for (int i = 0; i < MAX_DISP_OUTPUTS; ++i) {
            if (g_disp.ident_shells[i] && g_disp.ident_popped[i]) {
                XtPopdown(g_disp.ident_shells[i]);
                g_disp.ident_popped[i] = false;
            }
            if (g_disp.ident_bg_surfs[i]) {
                cairo_surface_destroy(g_disp.ident_bg_surfs[i]);
                g_disp.ident_bg_surfs[i] = nullptr;
            }
        }
        publish_x11_ident_state("active=0");
        return;
    }

    char state_buf[1024];
    int sp = snprintf(state_buf, sizeof(state_buf), "active=1 selected=%d", g_disp.selected + 1);
    int shown = 0;

    for (int i = 0; i < MAX_DISP_OUTPUTS; ++i) {
        if (i >= g_disp.orig_count || !g_disp.orig_outs[i].enabled) {
            if (g_disp.ident_shells[i] && g_disp.ident_popped[i]) {
                XtPopdown(g_disp.ident_shells[i]);
                g_disp.ident_popped[i] = false;
            }
            continue;
        }

        const DispOutputInfo *o = &g_disp.orig_outs[i];
        int ow = effective_w(o);
        int oh = effective_h(o);
        int bx = o->x + ow - IDENT_BADGE_SZ - IDENT_BADGE_MARGIN;
        int by = o->y + IDENT_BADGE_MARGIN;
        if (bx < o->x + 4) bx = o->x + 4;
        if (by + IDENT_BADGE_SZ > o->y + oh - 4) by = o->y + 4;

        bool moved = (!g_disp.ident_popped[i] ||
                      g_disp.ident_last_x[i] != bx ||
                      g_disp.ident_last_y[i] != by);
        if (moved) {
            if (g_disp.ident_shells[i] && g_disp.ident_popped[i]) {
                XtPopdown(g_disp.ident_shells[i]);
                g_disp.ident_popped[i] = false;
                XSync(style.display, False);
            }
            if (g_disp.ident_bg_surfs[i]) {
                cairo_surface_destroy(g_disp.ident_bg_surfs[i]);
                g_disp.ident_bg_surfs[i] = nullptr;
            }
            g_disp.ident_bg_surfs[i] = capture_x11_root_patch(bx, by, IDENT_BADGE_SZ, IDENT_BADGE_SZ);
        }

        if (!g_disp.ident_shells[i] && style.displayDialog) {
            char sname[32];
            snprintf(sname, sizeof(sname), "monIdentShell%d", i + 1);
            g_disp.ident_shells[i] = XtVaCreatePopupShell(
                sname, overrideShellWidgetClass, XtParent(style.displayDialog),
                XmNx, (Position)bx,
                XmNy, (Position)by,
                XmNwidth, (Dimension)IDENT_BADGE_SZ,
                XmNheight, (Dimension)IDENT_BADGE_SZ,
                XmNborderWidth, 0,
                XmNsaveUnder, True,
                NULL
            );
            g_disp.ident_canvases[i] = XtVaCreateManagedWidget(
                "monIdentCanvas", xmDrawingAreaWidgetClass, g_disp.ident_shells[i],
                XmNwidth, (Dimension)IDENT_BADGE_SZ,
                XmNheight, (Dimension)IDENT_BADGE_SZ,
                XmNmarginWidth, 0,
                XmNmarginHeight, 0,
                NULL
            );
            XtAddCallback(
                g_disp.ident_canvases[i], XmNexposeCallback,
                draw_x11_ident_badge_cb, (XtPointer)(intptr_t)i
            );
            XtRealizeWidget(g_disp.ident_shells[i]);
            apply_x11_click_through_shape(XtWindow(g_disp.ident_shells[i]));
        } else if (g_disp.ident_shells[i] && moved) {
            XtVaSetValues(
                g_disp.ident_shells[i],
                XmNx, (Position)bx,
                XmNy, (Position)by,
                NULL
            );
        }

        if (g_disp.ident_shells[i]) {
            if (!g_disp.ident_popped[i]) {
                XtPopup(g_disp.ident_shells[i], XtGrabNone);
                g_disp.ident_popped[i] = true;
            }
            g_disp.ident_last_x[i] = bx;
            g_disp.ident_last_y[i] = by;
            if (XtIsRealized(g_disp.ident_shells[i])) {
                XRaiseWindow(style.display, XtWindow(g_disp.ident_shells[i]));
                draw_x11_ident_badge_cb(g_disp.ident_canvases[i], (XtPointer)(intptr_t)i, nullptr);
            }
            shown++;

            if (sp > 0 && sp + 96 < (int)sizeof(state_buf)) {
                sp += snprintf(
                    state_buf + sp, sizeof(state_buf) - (size_t)sp,
                    " %s=%d@%d,%d%s",
                    o->name[0] ? o->name : "OUT",
                    i + 1, bx, by,
                    (i == g_disp.selected) ? ":selected" : ""
                );
            }
        }
    }

    if (sp > 0 && sp + 32 < (int)sizeof(state_buf)) {
        snprintf(state_buf + sp, sizeof(state_buf) - (size_t)sp, " count=%d", shown);
    }
    publish_x11_ident_state(state_buf);
}

static void cleanup_ident_on_exit(void) {
    if (style.display) {
        sync_monitor_identify_overlays(false);
        if (style.root != None) {
            Atom ready_a = XInternAtom(style.display, "_DT_STYLE_DISPLAY_READY", False);
            XChangeProperty(style.display, style.root, ready_a, XA_STRING, 8,
                            PropModeReplace, (const unsigned char *)"0", 1);
            XFlush(style.display);
        }
    }
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

        /* Matching top-right semi-transparent corner numeral badge inside preview */
        if (rw >= 64.0 && rh >= 44.0) {
            double bsz = fmin(28.0, fmax(18.0, rh * 0.30));
            double bx = rx + rw - bsz - 6.0;
            double by = ry + 11.0;
            cairo_set_source_rgba(cr, 0.09, 0.13, 0.17, 0.62);
            cairo_rectangle(cr, bx, by, bsz, bsz);
            cairo_fill(cr);
            if (sel) {
                cairo_set_source_rgba(cr, 0.98, 0.78, 0.26, 0.85);
            } else {
                cairo_set_source_rgba(cr, 0.88, 0.92, 0.96, 0.45);
            }
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, bx + 0.5, by + 0.5, bsz - 1.0, bsz - 1.0);
            cairo_stroke(cr);

            char cnum[16];
            snprintf(cnum, sizeof(cnum), "%d", i + 1);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, bsz * 0.68);
            cairo_text_extents_t cext = {0};
            cairo_text_extents(cr, cnum, &cext);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
            cairo_move_to(cr, bx + (bsz - cext.width) * 0.5 - cext.x_bearing, by + bsz * 0.76);
            cairo_show_text(cr, cnum);
        }

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
            sync_monitor_identify_overlays(true);
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
        sync_monitor_identify_overlays(true);
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

static void update_controls_from_selected(void);

static void icc_fsb_cancel_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;
    XtUnmanageChild(w);
    update_controls_from_selected();
}

static void icc_fsb_ok_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    XmFileSelectionBoxCallbackStruct *cbs = (XmFileSelectionBoxCallbackStruct *)call_data;
    char *path = nullptr;
    if (cbs && cbs->value && XmStringGetLtoR(cbs->value, XmFONTLIST_DEFAULT_TAG, &path) && path) {
        if (path[0] && g_disp.selected >= 0 && g_disp.selected < g_disp.count) {
            snprintf(
                g_disp.outs[g_disp.selected].color_profile,
                sizeof(g_disp.outs[g_disp.selected].color_profile),
                "%s",
                path
            );
            apply_ergonomics(
                g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
                g_disp.nl_start_min, g_disp.nl_end_min,
                g_disp.outs, g_disp.count, false
            );
        }
        XtFree(path);
    }
    XtUnmanageChild(w);
    update_controls_from_selected();
}

static void open_icc_file_picker(void) {
    if (!style.displayDialog) return;
    if (!g_disp.icc_fsb_dlg) {
        Arg args[8];
        int n = 0;
        XmString pat = CMPSTR("*.ic*");
        XmString dtitle = CMPSTR("Select Monitor ICC Color Profile");
        XtSetArg(args[n], XmNpattern, pat); n++;
        XtSetArg(args[n], XmNdialogTitle, dtitle); n++;
        g_disp.icc_fsb_dlg = XmCreateFileSelectionDialog(style.displayDialog, "iccFileSB", args, n);
        XmStringFree(pat);
        XmStringFree(dtitle);
        Widget help_btn = XmFileSelectionBoxGetChild(g_disp.icc_fsb_dlg, XmDIALOG_HELP_BUTTON);
        if (help_btn) XtUnmanageChild(help_btn);
        XtAddCallback(g_disp.icc_fsb_dlg, XmNokCallback, icc_fsb_ok_cb, nullptr);
        XtAddCallback(g_disp.icc_fsb_dlg, XmNcancelCallback, icc_fsb_cancel_cb, nullptr);
    }
    XtManageChild(g_disp.icc_fsb_dlg);
}

static void browse_icc_profile_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    (void)call_data;
    if (g_disp.updating_widgets) return;
    open_icc_file_picker();
}

static void select_color_profile_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    if (g_disp.updating_widgets) return;
    int cidx = (int)(intptr_t)client_data;
    if (cidx < 0 || cidx >= 5 || g_disp.selected < 0 || g_disp.selected >= g_disp.count) {
        return;
    }
    if (cidx == 4) {
        open_icc_file_picker();
        return;
    }
    snprintf(
        g_disp.outs[g_disp.selected].color_profile,
        sizeof(g_disp.outs[g_disp.selected].color_profile),
        "%s",
        k_color_profile_ids[cidx]
    );
    update_controls_from_selected();
    apply_ergonomics(
        g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
        g_disp.nl_start_min, g_disp.nl_end_min,
        g_disp.outs, g_disp.count, false
    );
}

static void toggle_vrr_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    if (g_disp.updating_widgets) return;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    g_disp.outs[g_disp.selected].vrr_enabled = (cbs && cbs->set);
    apply_ergonomics(
        g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
        g_disp.nl_start_min, g_disp.nl_end_min,
        g_disp.outs, g_disp.count, false
    );
}

static void toggle_night_light_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    if (g_disp.updating_widgets) return;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    g_disp.nl_enabled = (cbs && cbs->set);
    apply_ergonomics(
        g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
        g_disp.nl_start_min, g_disp.nl_end_min,
        g_disp.outs, g_disp.count, false
    );
}

static void select_nl_mode_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    if (g_disp.updating_widgets) return;
    int midx = (int)(intptr_t)client_data;
    if (midx >= 0 && midx < 3) {
        g_disp.nl_mode = midx;
        apply_ergonomics(
            g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
            g_disp.nl_start_min, g_disp.nl_end_min,
            g_disp.outs, g_disp.count, false
        );
    }
}

static void change_nl_temp_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    if (g_disp.updating_widgets) return;
    XmScaleCallbackStruct *cbs = (XmScaleCallbackStruct *)call_data;
    if (cbs) {
        g_disp.nl_temp_k = clamp_i(cbs->value, 2700, 6500);
        apply_ergonomics(
            g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
            g_disp.nl_start_min, g_disp.nl_end_min,
            g_disp.outs, g_disp.count, false
        );
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
        sync_monitor_identify_overlays(true);
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

    int cp_idx = 0;
    const char *cp = o->color_profile[0] ? o->color_profile : "default";
    if (strcasecmp(cp, "default") == 0 || strcasecmp(cp, "none") == 0) cp_idx = 0;
    else if (strcasecmp(cp, "srgb") == 0) cp_idx = 1;
    else if (strcasecmp(cp, "display-p3") == 0 || strcasecmp(cp, "p3") == 0) cp_idx = 2;
    else if (strcasecmp(cp, "hdr10") == 0 || strcasecmp(cp, "bt2020-pq") == 0) cp_idx = 3;
    else cp_idx = 4;

    if (g_disp.color_opt_menu && g_disp.color_btns[cp_idx]) {
        XtVaSetValues(g_disp.color_opt_menu, XmNmenuHistory, g_disp.color_btns[cp_idx], NULL);
    }
    if (g_disp.color_path_lbl) {
        char plbl[192];
        if (cp_idx == 4) {
            const char *base = strrchr(cp, '/');
            snprintf(plbl, sizeof(plbl), "ICC: %s", base ? (base + 1) : cp);
        } else {
            snprintf(plbl, sizeof(plbl), "Active: %s", k_color_profile_labels[cp_idx]);
        }
        XmString xs = CMPSTR(plbl);
        XtVaSetValues(g_disp.color_path_lbl, XmNlabelString, xs, NULL);
        XmStringFree(xs);
    }

    if (g_disp.vrr_toggle) {
        XmToggleButtonSetState(g_disp.vrr_toggle, o->vrr_enabled ? True : False, False);
    }
    if (g_disp.nl_toggle) {
        XmToggleButtonSetState(g_disp.nl_toggle, g_disp.nl_enabled ? True : False, False);
    }
    int m_idx = clamp_i(g_disp.nl_mode, 0, 2);
    if (g_disp.nl_mode_opt_menu && g_disp.nl_mode_btns[m_idx]) {
        XtVaSetValues(g_disp.nl_mode_opt_menu, XmNmenuHistory, g_disp.nl_mode_btns[m_idx], NULL);
    }
    if (g_disp.nl_temp_scale) {
        XmScaleSetValue(g_disp.nl_temp_scale, clamp_i(g_disp.nl_temp_k, 2700, 6500));
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
        apply_ergonomics(
            g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
            g_disp.nl_start_min, g_disp.nl_end_min,
            g_disp.outs, g_disp.count, true
        );
        memcpy(g_disp.orig_outs, g_disp.outs, sizeof(g_disp.outs));
        g_disp.orig_count = g_disp.count;
        g_disp.orig_nl_enabled = g_disp.nl_enabled;
        g_disp.orig_nl_mode = g_disp.nl_mode;
        g_disp.orig_nl_temp_k = g_disp.nl_temp_k;
        g_disp.orig_nl_start_min = g_disp.nl_start_min;
        g_disp.orig_nl_end_min = g_disp.nl_end_min;
        sync_monitor_identify_overlays(false);
        XtUnmanageChild(style.displayDialog);
        break;

    case BTN_APPLY:
        normalize_layout_origin();
        apply_outputs(g_disp.outs, g_disp.count);
        apply_ergonomics(
            g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
            g_disp.nl_start_min, g_disp.nl_end_min,
            g_disp.outs, g_disp.count, true
        );
        memcpy(g_disp.orig_outs, g_disp.outs, sizeof(g_disp.outs));
        g_disp.orig_count = g_disp.count;
        g_disp.orig_nl_enabled = g_disp.nl_enabled;
        g_disp.orig_nl_mode = g_disp.nl_mode;
        g_disp.orig_nl_temp_k = g_disp.nl_temp_k;
        g_disp.orig_nl_start_min = g_disp.nl_start_min;
        g_disp.orig_nl_end_min = g_disp.nl_end_min;
        draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
        sync_monitor_identify_overlays(true);
        break;

    case BTN_CANCEL:
        memcpy(g_disp.outs, g_disp.orig_outs, sizeof(g_disp.outs));
        g_disp.count = g_disp.orig_count;
        g_disp.nl_enabled = g_disp.orig_nl_enabled;
        g_disp.nl_mode = g_disp.orig_nl_mode;
        g_disp.nl_temp_k = g_disp.orig_nl_temp_k;
        g_disp.nl_start_min = g_disp.orig_nl_start_min;
        g_disp.nl_end_min = g_disp.orig_nl_end_min;
        apply_outputs(g_disp.orig_outs, g_disp.orig_count);
        apply_ergonomics(
            g_disp.orig_nl_enabled, g_disp.orig_nl_mode, g_disp.orig_nl_temp_k,
            g_disp.orig_nl_start_min, g_disp.orig_nl_end_min,
            g_disp.orig_outs, g_disp.orig_count, true
        );
        sync_monitor_identify_overlays(false);
        XtUnmanageChild(style.displayDialog);
        break;

    case BTN_HELP:
        XtCallCallbacks(style.displayDialog, XmNhelpCallback, nullptr);
        break;

    default:
        break;
    }
}

static void handle_display_ipc_one(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    if (strncmp(cmd, "set-ergonomics", 14) == 0) {
        int en = 1, tk = 3400, vrr = 1;
        char mode[32] = "always";
        if (sscanf(cmd + 14, "%d %31s %d %d", &en, mode, &tk, &vrr) >= 3) {
            g_disp.nl_enabled = (en != 0);
            if (strcmp(mode, "always") == 0) g_disp.nl_mode = 0;
            else if (strcmp(mode, "schedule") == 0) g_disp.nl_mode = 1;
            else if (strcmp(mode, "sunset") == 0) g_disp.nl_mode = 2;
            g_disp.nl_temp_k = clamp_i(tk, 2700, 6500);
            for (int i = 0; i < g_disp.count; ++i) {
                g_disp.outs[i].vrr_enabled = (vrr != 0);
            }
            update_controls_from_selected();
            apply_ergonomics(
                g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
                g_disp.nl_start_min, g_disp.nl_end_min,
                g_disp.outs, g_disp.count, false
            );
        }
    } else if (strncmp(cmd, "set-color-profile", 17) == 0 && isspace((unsigned char)cmd[17])) {
        const char *p = cmd + 18;
        while (*p && isspace((unsigned char)*p)) p++;
        char target[64] = {0};
        size_t ti = 0;
        while (*p && !isspace((unsigned char)*p) && ti + 1 < sizeof(target)) {
            target[ti++] = *p++;
        }
        target[ti] = '\0';
        while (*p && isspace((unsigned char)*p)) p++;
        if (target[0] && *p) {
            bool all = (strcasecmp(target, "all") == 0);
            for (int i = 0; i < g_disp.count; ++i) {
                if (all || strcmp(g_disp.outs[i].name, target) == 0 ||
                    (isdigit((unsigned char)target[0]) && atoi(target) == i)) {
                    snprintf(
                        g_disp.outs[i].color_profile,
                        sizeof(g_disp.outs[i].color_profile),
                        "%s",
                        p
                    );
                }
            }
            update_controls_from_selected();
            apply_ergonomics(
                g_disp.nl_enabled, g_disp.nl_mode, g_disp.nl_temp_k,
                g_disp.nl_start_min, g_disp.nl_end_min,
                g_disp.outs, g_disp.count, false
            );
        }
    } else if (strcmp(cmd, "apply") == 0) {
        DtDialogBoxCallbackStruct cbs = {0};
        cbs.button_position = BTN_APPLY;
        dialog_button_cb(style.displayDialog, nullptr, &cbs);
    } else if (strcmp(cmd, "ok") == 0) {
        DtDialogBoxCallbackStruct cbs = {0};
        cbs.button_position = BTN_OK;
        dialog_button_cb(style.displayDialog, nullptr, &cbs);
    } else if (strcmp(cmd, "cancel") == 0) {
        DtDialogBoxCallbackStruct cbs = {0};
        cbs.button_position = BTN_CANCEL;
        dialog_button_cb(style.displayDialog, nullptr, &cbs);
    }
}

static void handle_display_ipc_cmd(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    const char *cur = cmd;
    while (*cur) {
        const char *nl = strchr(cur, '\n');
        size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
        const char *seg = cur;
        while (len > 0 && (seg[0] == ' ' || seg[0] == '\t' || seg[0] == '\r')) {
            seg++;
            len--;
        }
        while (len > 0 && (seg[len - 1] == ' ' || seg[len - 1] == '\t' || seg[len - 1] == '\r')) {
            len--;
        }
        if (len > 0) {
            char line[1024];
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, seg, len);
            line[len] = '\0';
            handle_display_ipc_one(line);
        }
        if (!nl) break;
        cur = nl + 1;
    }
}

static void display_ipc_prop_eh(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)client_data;
    (void)cont;
    if (!ev || ev->type != PropertyNotify || !style.display) return;
    Atom ipc_atom = XInternAtom(style.display, "_DTSTYLE_DISPLAY_IPC", False);
    Atom wl_ipc_atom = XInternAtom(style.display, "_DT_STYLE_DISPLAY_IPC", False);
    if ((ev->xproperty.atom != ipc_atom && ev->xproperty.atom != wl_ipc_atom) ||
        ev->xproperty.state != PropertyNewValue) {
        return;
    }

    Atom act = None;
    int fmt = 0;
    unsigned long ni = 0, rem = 0;
    unsigned char *data = nullptr;
    Window src_win = ev->xproperty.window ? ev->xproperty.window : XtWindow(w);
    if (XGetWindowProperty(style.display, src_win, ev->xproperty.atom, 0, 1024, True,
                           AnyPropertyType, &act, &fmt, &ni, &rem, &data) != Success || !data) {
        return;
    }
    if (act == None || ni == 0) {
        XFree(data);
        return;
    }
    char cmd[4096];
    size_t cl = ni < sizeof(cmd) - 1 ? (size_t)ni : sizeof(cmd) - 1;
    memcpy(cmd, data, cl);
    for (size_t i = 0; i < cl; ++i) {
        if (cmd[i] == '\0') cmd[i] = '\n';
    }
    cmd[cl] = '\0';
    XFree(data);
    handle_display_ipc_cmd(cmd);
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
    XtSetArg(g_disp_save.posArgs[g_disp_save.poscnt], XmNdefaultPosition, False); g_disp_save.poscnt++;

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

    XmString x_vrr = CMPSTR("Variable Refresh Rate (VRR)");
    g_disp.vrr_toggle = XtVaCreateManagedWidget(
        "vrrToggle", xmToggleButtonWidgetClass, row1,
        XmNlabelString, x_vrr,
        NULL
    );
    XmStringFree(x_vrr);
    XtAddCallback(g_disp.vrr_toggle, XmNvalueChangedCallback, toggle_vrr_cb, nullptr);

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

    /* Row 3: Per-Monitor Color Profile / HDR / Custom ICC Picker */
    Widget row3 = XtVaCreateManagedWidget(
        "colorRow", xmRowColumnWidgetClass, set_rc,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 10,
        NULL
    );

    Widget cp_pd = XmCreatePulldownMenu(row3, "colorPD", nullptr, 0);
    for (int i = 0; i < 5; ++i) {
        XmString xs = CMPSTR((char *)k_color_profile_labels[i]);
        g_disp.color_btns[i] = XtVaCreateManagedWidget(
            "colorItem", xmPushButtonGadgetClass, cp_pd,
            XmNlabelString, xs,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(
            g_disp.color_btns[i], XmNactivateCallback, select_color_profile_cb, (XtPointer)(intptr_t)i
        );
    }
    XmString x_cp = CMPSTR("Color:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, x_cp); n++;
    XtSetArg(args[n], XmNsubMenuId, cp_pd); n++;
    g_disp.color_opt_menu = XmCreateOptionMenu(row3, "colorOpt", args, n);
    XmStringFree(x_cp);
    XtManageChild(g_disp.color_opt_menu);

    XmString x_browse = CMPSTR("Browse ICC...");
    g_disp.color_browse_btn = XtVaCreateManagedWidget(
        "browseIccBtn", xmPushButtonGadgetClass, row3,
        XmNlabelString, x_browse,
        XmNmarginWidth, 6,
        NULL
    );
    XmStringFree(x_browse);
    XtAddCallback(g_disp.color_browse_btn, XmNactivateCallback, browse_icc_profile_cb, nullptr);

    XmString x_cp_lbl = CMPSTR("Active: Native");
    g_disp.color_path_lbl = XtVaCreateManagedWidget(
        "colorPathLbl", xmLabelGadgetClass, row3,
        XmNlabelString, x_cp_lbl,
        NULL
    );
    XmStringFree(x_cp_lbl);

    /* 3. Night Light & Adaptive Sync TitleBox */
    XmString tb3_str = CMPSTR("Night Light & Eye Comfort");
    n = 0;
    XtSetArg(args[n], XmNtitleString, tb3_str); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, g_disp.settings_tb); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    g_disp.ergo_tb = _DtCreateTitleBox(form, "nightLightTB", args, n);
    XmStringFree(tb3_str);
    XtManageChild(g_disp.ergo_tb);

    Widget ergo_rc = XtVaCreateManagedWidget(
        "ergoRC", xmRowColumnWidgetClass, g_disp.ergo_tb,
        XmNchildType, XmWORK_AREA,
        XmNorientation, XmVERTICAL,
        XmNspacing, 6,
        XmNmarginWidth, 8,
        XmNmarginHeight, 6,
        NULL
    );

    Widget nl_row1 = XtVaCreateManagedWidget(
        "nlRow1", xmRowColumnWidgetClass, ergo_rc,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 14,
        NULL
    );

    XmString x_nl = CMPSTR("Enable Night Light");
    g_disp.nl_toggle = XtVaCreateManagedWidget(
        "nightLightToggle", xmToggleButtonWidgetClass, nl_row1,
        XmNlabelString, x_nl,
        NULL
    );
    XmStringFree(x_nl);
    XtAddCallback(g_disp.nl_toggle, XmNvalueChangedCallback, toggle_night_light_cb, nullptr);

    Widget nl_pd = XmCreatePulldownMenu(nl_row1, "nlModePD", nullptr, 0);
    for (int i = 0; i < 3; ++i) {
        XmString xs = CMPSTR((char *)k_nl_mode_labels[i]);
        g_disp.nl_mode_btns[i] = XtVaCreateManagedWidget(
            "nlModeItem", xmPushButtonGadgetClass, nl_pd,
            XmNlabelString, xs,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(g_disp.nl_mode_btns[i], XmNactivateCallback, select_nl_mode_cb, (XtPointer)(intptr_t)i);
    }
    XmString x_nlm = CMPSTR("Schedule:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, x_nlm); n++;
    XtSetArg(args[n], XmNsubMenuId, nl_pd); n++;
    g_disp.nl_mode_opt_menu = XmCreateOptionMenu(nl_row1, "nlModeOpt", args, n);
    XmStringFree(x_nlm);
    XtManageChild(g_disp.nl_mode_opt_menu);

    XmString x_tk = CMPSTR("Color Temperature (2700 K Warm - 6500 K Daylight):");
    g_disp.nl_temp_scale = XtVaCreateManagedWidget(
        "nlTempScale", xmScaleWidgetClass, ergo_rc,
        XmNtitleString, x_tk,
        XmNorientation, XmHORIZONTAL,
        XmNminimum, 2700,
        XmNmaximum, 6500,
        XmNvalue, 4000,
        XmNshowValue, True,
        XmNscaleMultiple, 100,
        NULL
    );
    XmStringFree(x_tk);
    XtAddCallback(g_disp.nl_temp_scale, XmNvalueChangedCallback, change_nl_temp_cb, nullptr);
    XtAddCallback(g_disp.nl_temp_scale, XmNdragCallback, change_nl_temp_cb, nullptr);

    XtAddEventHandler(
        XtParent(style.displayDialog), PropertyChangeMask, False,
        display_ipc_prop_eh, nullptr
    );
    XtAddEventHandler(
        form, PropertyChangeMask, False,
        display_ipc_prop_eh, nullptr
    );
    if (g_disp.is_wayland && style.display && style.root != None) {
        Atom stale_ipc = XInternAtom(style.display, "_DT_STYLE_DISPLAY_IPC", False);
        XDeleteProperty(style.display, style.root, stale_ipc);
        XWindowAttributes wa;
        if (XGetWindowAttributes(style.display, style.root, &wa)) {
            XSelectInput(style.display, style.root, wa.your_event_mask | PropertyChangeMask);
        }
        Widget root_w = XtWindowToWidget(style.display, style.root);
        if (root_w) {
            XtAddEventHandler(root_w, PropertyChangeMask, False, display_ipc_prop_eh, nullptr);
        } else {
            XtRegisterDrawable(style.display, style.root, form);
        }
    }

    XtManageChild(form);
}

static void publish_display_ipc_ready(bool ready) {
    if (!style.display || style.root == None) return;
    Atom ready_a = XInternAtom(style.display, "_DT_STYLE_DISPLAY_READY", False);
    const char *val = ready ? "1" : "0";
    XChangeProperty(style.display, style.root, ready_a, XA_STRING, 8,
                    PropModeReplace, (const unsigned char *)val, 1);
    XSync(style.display, False);
}

static void display_unmap_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    (void)call_data;
    sync_monitor_identify_overlays(false);
    publish_display_ipc_ready(false);
}

static void display_shell_structure_eh(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w;
    (void)client_data;
    (void)cont;
    if (ev && (ev->type == UnmapNotify || ev->type == DestroyNotify)) {
        sync_monitor_identify_overlays(false);
        publish_display_ipc_ready(false);
    }
}

static void publish_display_dlg_window(void) {
    if (!style.display || !style.displayDialog) return;
    Widget sh = XtParent(style.displayDialog);
    if (!sh || !XtIsRealized(sh)) return;
    Window w = XtWindow(sh);
    if (w == None) return;
    Atom a = XInternAtom(style.display, "_DTSTYLE_DISPLAY_DLG_WIN", False);
    XChangeProperty(style.display, style.root, a, XA_WINDOW, 32,
                    PropModeReplace, (const unsigned char *)&w, 1);
    XFlush(style.display);
}

void popup_displayBB(Widget shell) {
    if (!g_disp.ident_atexit_registered) {
        atexit(cleanup_ident_on_exit);
        g_disp.ident_atexit_registered = true;
    }
    if (style.displayDialog == nullptr) {
        _DtTurnOnHourGlass(shell);
        query_outputs();
        build_display_dialog(shell);
        XtAddCallback(style.displayDialog, XmNunmapCallback, display_unmap_cb, nullptr);
        XtAddEventHandler(
            XtParent(style.displayDialog), StructureNotifyMask, False,
            display_shell_structure_eh, nullptr
        );
        update_controls_from_selected();
        XtManageChild(style.displayDialog);
        {
            Widget dsh = XtParent(style.displayDialog);
            int scr_w = WidthOfScreen(style.screen);
            int scr_h = HeightOfScreen(style.screen);
            int dw = (int)XtWidth(style.displayDialog);
            int dh = (int)XtHeight(style.displayDialog);
            if (dw < 400) dw = 620;
            if (dh < 300) dh = 520;
            Position nx = (Position)((scr_w - dw) / 2);
            Position ny = (Position)((scr_h - dh) / 2);
            if (scr_w >= 1200 && nx < 360) nx = (Position)(scr_w - dw - 48);
            if (nx < 24) nx = 24;
            if (ny < 32) ny = 32;
            XtVaSetValues(dsh, XmNx, nx, XmNy, ny, NULL);
        }
        XtRealizeWidget(XtParent(style.displayDialog));
        publish_display_dlg_window();
        sync_monitor_identify_overlays(true);
        _DtTurnOffHourGlass(shell);
        publish_display_ipc_ready(true);
    } else {
        query_outputs();
        update_controls_from_selected();
        XtManageChild(style.displayDialog);
        if (XtIsRealized(style.displayDialog)) {
            publish_display_dlg_window();
            raiseWindow(XtWindow(XtParent(style.displayDialog)));
            draw_canvas_cb(g_disp.canvas, nullptr, nullptr);
            sync_monitor_identify_overlays(true);
            publish_display_ipc_ready(true);
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
