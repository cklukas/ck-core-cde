/*
 * CDE - Common Desktop Environment
 *
 * wayland/cde_power_display.c: WP-18 Night Light (Planckian Kelvin-to-RGB
 * hardware gamma LUTs via wlr-gamma-control-unstable-v1 + headless scene tint),
 * per-output Variable Refresh Rate (adaptive sync), laptop lid-switch clamshell
 * vs. systemd-logind suspend policy, idle auto-suspend, and Super+Arrow
 * half/quadrant keyboard window tiling for dtwm-wayland.
 */

#define _GNU_SOURCE
#include "dtwm_wayland.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_switch.h>
#include <xkbcommon/xkbcommon.h>

typedef enum {
    DTWM_TILE_NONE = 0,
    DTWM_TILE_LEFT,
    DTWM_TILE_RIGHT,
    DTWM_TILE_TOP,
    DTWM_TILE_BOTTOM,
    DTWM_TILE_TOP_LEFT,
    DTWM_TILE_TOP_RIGHT,
    DTWM_TILE_BOTTOM_LEFT,
    DTWM_TILE_BOTTOM_RIGHT,
} DtwmTileState;

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_switch *wlr_switch;
    struct wl_listener toggle;
    struct wl_listener destroy;
} DtwmSwitchDev;

typedef struct {
    char name[64];
    bool vrr_enabled;
} DtwmVrrRule;

typedef struct {
    char name[64];
    int dpi;
} DtwmDpiRule;

typedef struct {
    char name[64];
    char spec[1024];
} DtwmColorProfileRule;

struct DtwmPowerDisplayState {
    DtwmServer *server;
    struct wlr_gamma_control_manager_v1 *gamma_mgr;
    struct wl_event_source *periodic_timer;
    struct wl_list switches;

    /* Night Light & VRR state */
    bool nl_enabled;
    char nl_mode[24]; /* "off", "always", "schedule", "sunset" */
    int nl_temp_k;
    int nl_start_min; /* default 20:00 = 1200 */
    int nl_end_min;   /* default 06:00 = 360 */
    bool nl_active;
    int r_permille;
    int g_permille;
    int b_permille;

    int vrr_rule_count;
    DtwmVrrRule vrr_rules[16];
    int dpi_rule_count;
    DtwmDpiRule dpi_rules[16];
    int color_rule_count;
    DtwmColorProfileRule color_rules[16];

    /* Power / Lid / Idle Suspend policy (~/.config/ck-core/power-policy.conf) */
    time_t policy_mtime;
    int ac_suspend_sec;
    int bat_suspend_sec;
    char lid_action[32]; /* "docked-clamshell", "suspend", "lock", "nothing" */

    uint64_t last_activity_sec;
    bool idle_suspend_fired;
    bool lid_closed;
    char clamshell_output_name[64];
    int clamshell_saved_x;
    int clamshell_saved_y;
    int clamshell_saved_w;
    int clamshell_saved_h;

    xcb_atom_t atom_night_light_state;
    xcb_atom_t atom_vrr_state;
    xcb_atom_t atom_power_last_action;
    char last_power_action[256];
};

static uint64_t mono_seconds_now(void) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double clamp_dbl(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static xcb_atom_t intern_xcb_atom(xcb_connection_t *conn, const char *name) {
    if (!conn || !name || !name[0]) return XCB_ATOM_NONE;
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, nullptr);
    if (!rep) return XCB_ATOM_NONE;
    xcb_atom_t a = rep->atom;
    free(rep);
    return a;
}

static bool night_light_reject_input(struct wlr_scene_buffer *buf, double *sx, double *sy) {
    (void)buf;
    (void)sx;
    (void)sy;
    return false;
}

/* -------------------------------------------------------------------------
 * 1. Planckian / Tanner Helland Kelvin-to-RGB Curve & Schedule Evaluation
 * ------------------------------------------------------------------------- */
void dtwm_kelvin_to_rgb(int temp_k, double *out_r, double *out_g, double *out_b) {
    temp_k = clamp_int(temp_k, 2700, 6500);
    if (temp_k >= 6500) {
        if (out_r) *out_r = 1.0;
        if (out_g) *out_g = 1.0;
        if (out_b) *out_b = 1.0;
        return;
    }

    double t = (double)temp_k / 100.0;
    double ref_t = 65.0;

    double g_raw = (99.4708025861 * log(t) - 161.1195681661) / 255.0;
    double g_ref = (99.4708025861 * log(ref_t) - 161.1195681661) / 255.0;

    double b_raw = (t <= 19.0) ? 0.0 : (138.5177312231 * log(t - 10.0) - 305.0447927307) / 255.0;
    double b_ref = (138.5177312231 * log(ref_t - 10.0) - 305.0447927307) / 255.0;

    double r = 1.0;
    double g = (g_ref > 0.01) ? clamp_dbl(g_raw / g_ref, 0.50, 1.0) : 1.0;
    double b = (b_ref > 0.01) ? clamp_dbl(b_raw / b_ref, 0.20, 1.0) : 1.0;
    if (b >= g) b = g - 0.02;

    if (out_r) *out_r = r;
    if (out_g) *out_g = g;
    if (out_b) *out_b = b;
}

static bool evaluate_night_light_schedule(const DtwmPowerDisplayState *st) {
    if (!st || !st->nl_enabled || strcmp(st->nl_mode, "off") == 0) {
        return false;
    }
    if (strcmp(st->nl_mode, "always") == 0) {
        return true;
    }

    int now_min = -1;
    const char *override_min = getenv("CK_NIGHT_LIGHT_TEST_MINUTES");
    if (override_min && override_min[0]) {
        now_min = clamp_int(atoi(override_min), 0, 1439);
    } else {
        time_t now = time(nullptr);
        struct tm ltm;
        if (localtime_r(&now, &ltm)) {
            now_min = ltm.tm_hour * 60 + ltm.tm_min;
        } else {
            now_min = 0;
        }
    }

    int start_m = st->nl_start_min;
    int end_m = st->nl_end_min;
    if (strcmp(st->nl_mode, "sunset") == 0) {
        /* Seasonal sunset-to-sunrise approximation when geolocation is offline */
        start_m = 19 * 60 + 30;
        end_m = 6 * 60 + 30;
    }
    start_m = ((start_m % 1440) + 1440) % 1440;
    end_m = ((end_m % 1440) + 1440) % 1440;

    if (start_m == end_m) return true;
    if (start_m > end_m) {
        return (now_min >= start_m || now_min < end_m);
    }
    return (now_min >= start_m && now_min < end_m);
}

/* -------------------------------------------------------------------------
 * 2. Display Ergonomics Persistence (~/.dt/display_ergonomics.conf)
 * ------------------------------------------------------------------------- */
static void ergonomics_conf_path(char *out, size_t out_sz) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/var/tmp";
    char dt_dir[PATH_MAX];
    snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", home);
    mkdir(dt_dir, 0755);
    snprintf(out, out_sz, "%s/display_ergonomics.conf", dt_dir);
}

static bool lookup_vrr_rule(const DtwmPowerDisplayState *st, const char *name) {
    if (!st || !name || !name[0]) return false;
    for (int i = 0; i < st->vrr_rule_count; ++i) {
        if (strcmp(st->vrr_rules[i].name, name) == 0) {
            return st->vrr_rules[i].vrr_enabled;
        }
    }
    return false;
}

static void record_vrr_rule(DtwmPowerDisplayState *st, const char *name, bool enabled) {
    if (!st || !name || !name[0]) return;
    for (int i = 0; i < st->vrr_rule_count; ++i) {
        if (strcmp(st->vrr_rules[i].name, name) == 0) {
            st->vrr_rules[i].vrr_enabled = enabled;
            return;
        }
    }
    if (st->vrr_rule_count < 16) {
        snprintf(st->vrr_rules[st->vrr_rule_count].name,
                 sizeof(st->vrr_rules[0].name), "%s", name);
        st->vrr_rules[st->vrr_rule_count].vrr_enabled = enabled;
        st->vrr_rule_count++;
    }
}

static void record_dpi_rule(DtwmPowerDisplayState *st, const char *name, int dpi) {
    if (!st || !name || !name[0] || dpi < 48 || dpi > 576) return;
    for (int i = 0; i < st->dpi_rule_count; ++i) {
        if (strcmp(st->dpi_rules[i].name, name) == 0) {
            st->dpi_rules[i].dpi = dpi;
            return;
        }
    }
    if (st->dpi_rule_count < 16) {
        snprintf(st->dpi_rules[st->dpi_rule_count].name,
                 sizeof(st->dpi_rules[0].name), "%s", name);
        st->dpi_rules[st->dpi_rule_count].dpi = dpi;
        st->dpi_rule_count++;
    }
}

static void record_color_rule(DtwmPowerDisplayState *st, const char *name, const char *spec) {
    if (!st || !name || !name[0] || !spec) return;
    while (*spec && isspace((unsigned char)*spec)) spec++;
    const char *norm = (*spec && strcasecmp(spec, "none") != 0) ? spec : "default";
    for (int i = 0; i < st->color_rule_count; ++i) {
        if (strcmp(st->color_rules[i].name, name) == 0) {
            snprintf(st->color_rules[i].spec, sizeof(st->color_rules[0].spec), "%s", norm);
            return;
        }
    }
    if (st->color_rule_count < 16) {
        snprintf(st->color_rules[st->color_rule_count].name,
                 sizeof(st->color_rules[0].name), "%s", name);
        snprintf(st->color_rules[st->color_rule_count].spec,
                 sizeof(st->color_rules[0].spec), "%s", norm);
        st->color_rule_count++;
    }
}

static void save_ergonomics_conf(const DtwmPowerDisplayState *st) {
    if (!st) return;
    char path[PATH_MAX], tmp[PATH_MAX + 32];
    ergonomics_conf_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    fprintf(fp, "night_light_enabled=%d\n", st->nl_enabled ? 1 : 0);
    fprintf(fp, "night_light_mode=%s\n", st->nl_mode);
    fprintf(fp, "night_light_temp=%d\n", st->nl_temp_k);
    fprintf(fp, "night_light_start_min=%d\n", st->nl_start_min);
    fprintf(fp, "night_light_end_min=%d\n", st->nl_end_min);
    for (int i = 0; i < st->vrr_rule_count; ++i) {
        fprintf(fp, "vrr.%s=%d\n", st->vrr_rules[i].name,
                st->vrr_rules[i].vrr_enabled ? 1 : 0);
    }
    for (int i = 0; i < st->dpi_rule_count; ++i) {
        fprintf(fp, "dpi.%s=%d\n", st->dpi_rules[i].name, st->dpi_rules[i].dpi);
    }
    for (int i = 0; i < st->color_rule_count; ++i) {
        fprintf(fp, "color_profile.%s=%s\n", st->color_rules[i].name, st->color_rules[i].spec);
    }
    if (fclose(fp) == 0) {
        if (rename(tmp, path) != 0) unlink(tmp);
    } else {
        unlink(tmp);
    }
}

static void load_ergonomics_conf(DtwmPowerDisplayState *st) {
    if (!st) return;
    char path[PATH_MAX];
    ergonomics_conf_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = p;
        const char *v = eq + 1;
        if (strcmp(k, "night_light_enabled") == 0) {
            st->nl_enabled = (atoi(v) != 0);
        } else if (strcmp(k, "night_light_mode") == 0) {
            if (strcmp(v, "off") == 0 || strcmp(v, "always") == 0 ||
                strcmp(v, "schedule") == 0 || strcmp(v, "sunset") == 0) {
                snprintf(st->nl_mode, sizeof(st->nl_mode), "%s", v);
            }
        } else if (strcmp(k, "night_light_temp") == 0) {
            st->nl_temp_k = clamp_int(atoi(v), 2700, 6500);
        } else if (strcmp(k, "night_light_start_min") == 0) {
            st->nl_start_min = clamp_int(atoi(v), 0, 1439);
        } else if (strcmp(k, "night_light_end_min") == 0) {
            st->nl_end_min = clamp_int(atoi(v), 0, 1439);
        } else if (strncmp(k, "vrr.", 4) == 0 && k[4] != '\0') {
            record_vrr_rule(st, k + 4, atoi(v) != 0);
        } else if (strncmp(k, "dpi.", 4) == 0 && k[4] != '\0') {
            int dpi = atoi(v);
            if (dpi >= 48 && dpi <= 576) {
                record_dpi_rule(st, k + 4, dpi);
            }
        } else if (strncmp(k, "color_profile.", 14) == 0 && k[14] != '\0') {
            record_color_rule(st, k + 14, v);
        }
    }
    fclose(fp);
}

void dtwm_power_display_record_dpi(DtwmServer *server, const char *name, int dpi) {
    if (!server || !server->power_display || !name || !name[0]) return;
    record_dpi_rule(server->power_display, name, dpi);
    save_ergonomics_conf(server->power_display);
}

int dtwm_power_display_lookup_dpi(const DtwmServer *server, const char *name) {
    if (!server || !server->power_display || !name || !name[0]) return 0;
    const DtwmPowerDisplayState *st = server->power_display;
    for (int i = 0; i < st->dpi_rule_count; ++i) {
        if (strcmp(st->dpi_rules[i].name, name) == 0) {
            return st->dpi_rules[i].dpi;
        }
    }
    return 0;
}

void dtwm_power_display_record_color_profile(
    DtwmServer *server, const char *name, const char *spec
) {
    if (!server || !server->power_display || !name || !name[0]) return;
    record_color_rule(server->power_display, name, spec ? spec : "default");
    save_ergonomics_conf(server->power_display);
}

const char *dtwm_power_display_lookup_color_profile(
    const DtwmServer *server, const char *name
) {
    if (!server || !server->power_display || !name || !name[0]) return nullptr;
    const DtwmPowerDisplayState *st = server->power_display;
    for (int i = 0; i < st->color_rule_count; ++i) {
        if (strcmp(st->color_rules[i].name, name) == 0) {
            return st->color_rules[i].spec;
        }
    }
    return nullptr;
}

/* -------------------------------------------------------------------------
 * 3. Publish Night Light, VRR & Power Policy State on X11 Root
 * ------------------------------------------------------------------------- */
static void publish_night_light_and_vrr_x11(DtwmServer *server) {
    if (!server || !server->power_display || !server->xcb_conn ||
        server->xcb_root == XCB_WINDOW_NONE) {
        return;
    }
    DtwmPowerDisplayState *st = server->power_display;
    if (st->atom_night_light_state == XCB_ATOM_NONE) {
        st->atom_night_light_state = intern_xcb_atom(server->xcb_conn, "_DT_NIGHT_LIGHT_STATE");
    }
    if (st->atom_vrr_state == XCB_ATOM_NONE) {
        st->atom_vrr_state = intern_xcb_atom(server->xcb_conn, "_DT_VRR_STATE");
    }

    bool any_vrr = false;
    char vrr_buf[1024];
    int vpos = 0;
    vrr_buf[0] = '\0';
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (!out->wlr_output) continue;
        if (out->vrr_enabled) any_vrr = true;
        int n = snprintf(
            vrr_buf + vpos, sizeof(vrr_buf) - (size_t)vpos,
            "%s%s=%d", vpos > 0 ? "," : "",
            out->wlr_output->name ? out->wlr_output->name : "OUT",
            out->vrr_enabled ? 1 : 0
        );
        if (n > 0 && vpos + n < (int)sizeof(vrr_buf)) vpos += n;
    }

    if (st->atom_night_light_state != XCB_ATOM_NONE) {
        char buf[320];
        int n = snprintf(
            buf, sizeof(buf),
            "enabled=%d active=%d mode=%s temp=%d r_permille=%d g_permille=%d b_permille=%d vrr=%d",
            st->nl_enabled ? 1 : 0,
            st->nl_active ? 1 : 0,
            st->nl_mode,
            st->nl_temp_k,
            st->r_permille,
            st->g_permille,
            st->b_permille,
            any_vrr ? 1 : 0
        );
        if (n > 0) {
            xcb_change_property(
                server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_root,
                st->atom_night_light_state, XCB_ATOM_STRING, 8, (uint32_t)n, buf
            );
        }
    }
    if (st->atom_vrr_state != XCB_ATOM_NONE) {
        xcb_change_property(
            server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_root,
            st->atom_vrr_state, XCB_ATOM_STRING, 8, (uint32_t)vpos, vrr_buf
        );
    }
    xcb_flush(server->xcb_conn);
}

static void publish_power_policy_action(DtwmServer *server, const char *action_str) {
    if (!server || !server->power_display || !action_str) return;
    DtwmPowerDisplayState *st = server->power_display;
    snprintf(st->last_power_action, sizeof(st->last_power_action), "%s", action_str);
    dtwm_native_ipc_publish_prop(
        server, "_DT_POWER_POLICY_LAST_ACTION",
        st->last_power_action, strlen(st->last_power_action)
    );
    if (!server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) return;
    if (st->atom_power_last_action == XCB_ATOM_NONE) {
        st->atom_power_last_action = intern_xcb_atom(server->xcb_conn, "_DT_POWER_POLICY_LAST_ACTION");
    }
    if (st->atom_power_last_action != XCB_ATOM_NONE) {
        xcb_change_property(
            server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_root,
            st->atom_power_last_action, XCB_ATOM_STRING, 8,
            (uint32_t)strlen(st->last_power_action), st->last_power_action
        );
        xcb_flush(server->xcb_conn);
    }
}

/* -------------------------------------------------------------------------
 * 4. Apply Night Light Gamma LUT / Scene Tint & Adaptive Sync to Outputs
 * ------------------------------------------------------------------------- */
static void apply_output_vrr_hw(DtwmOutput *out) {
    if (!out || !out->wlr_output || !out->wlr_output->enabled) return;
    struct wlr_output *wo = out->wlr_output;
    if (!wo->adaptive_sync_supported) return;

    bool cur_en = (wo->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
    if (cur_en == out->vrr_enabled) return;

    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_adaptive_sync_enabled(&st, out->vrr_enabled);
    if (wlr_output_test_state(wo, &st)) {
        wlr_output_commit_state(wo, &st);
    }
    wlr_output_state_finish(&st);
}

void dtwm_power_display_sync_outputs(DtwmServer *server) {
    if (!server || !server->power_display) return;
    DtwmPowerDisplayState *st = server->power_display;

    st->nl_active = evaluate_night_light_schedule(st);
    double r_mult = 1.0, g_mult = 1.0, b_mult = 1.0;
    if (st->nl_active) {
        dtwm_kelvin_to_rgb(st->nl_temp_k, &r_mult, &g_mult, &b_mult);
    }
    st->r_permille = (int)lround(r_mult * 1000.0);
    st->g_permille = (int)lround(g_mult * 1000.0);
    st->b_permille = (int)lround(b_mult * 1000.0);

    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        struct wlr_output *wo = out->wlr_output;
        if (!wo) continue;
        if (!wo->enabled) {
            if (out->night_light_buf) {
                wlr_scene_node_set_enabled(&out->night_light_buf->node, false);
            }
            continue;
        }

        apply_output_vrr_hw(out);

        /* Respect any external wlr_gamma_control_v1 client on this output */
        if (st->gamma_mgr &&
            wlr_gamma_control_manager_v1_get_control(st->gamma_mgr, wo) != nullptr) {
            if (out->night_light_buf) {
                wlr_scene_node_set_enabled(&out->night_light_buf->node, false);
            }
            continue;
        }

        size_t gsz = wlr_output_get_gamma_size(wo);
        if (gsz > 1) {
            if (out->night_light_buf) {
                wlr_scene_node_set_enabled(&out->night_light_buf->node, false);
                wlr_scene_buffer_set_buffer(out->night_light_buf, nullptr);
            }
            uint16_t *rl = calloc(gsz, sizeof(uint16_t));
            uint16_t *gl = calloc(gsz, sizeof(uint16_t));
            uint16_t *bl = calloc(gsz, sizeof(uint16_t));
            if (rl && gl && bl) {
                for (size_t i = 0; i < gsz; ++i) {
                    double base = ((double)i / (double)(gsz - 1)) * 65535.0;
                    rl[i] = (uint16_t)clamp_int((int)lround(base * r_mult), 0, 65535);
                    gl[i] = (uint16_t)clamp_int((int)lround(base * g_mult), 0, 65535);
                    bl[i] = (uint16_t)clamp_int((int)lround(base * b_mult), 0, 65535);
                }
                struct wlr_output_state ost;
                wlr_output_state_init(&ost);
                if (wlr_output_state_set_gamma_lut(&ost, gsz, rl, gl, bl) &&
                    wlr_output_test_state(wo, &ost)) {
                    wlr_output_commit_state(wo, &ost);
                }
                wlr_output_state_finish(&ost);
            }
            free(rl);
            free(gl);
            free(bl);
        } else {
            /* Software warm scene tint fallback when backend gamma_size == 0 (e.g. headless) */
            if (!out->night_light_buf && server->layer_overlay_tree) {
                out->night_light_buf = wlr_scene_buffer_create(server->layer_overlay_tree, nullptr);
                if (out->night_light_buf) {
                    out->night_light_buf->point_accepts_input = night_light_reject_input;
                    wlr_scene_node_set_enabled(&out->night_light_buf->node, false);
                }
            }
            if (out->night_light_buf) {
                if (st->nl_active && st->nl_temp_k < 6500) {
                    struct wlr_box obox = {0};
                    wlr_output_layout_get_box(server->output_layout, wo, &obox);
                    if (obox.width > 0 && obox.height > 0) {
                        cairo_t *cr = nullptr;
                        cairo_surface_t *surf = nullptr;
                        struct wlr_buffer *buf = dtwm_cairo_buffer_begin(4, 4, &cr, &surf);
                        if (buf) {
                            double strength = clamp_dbl(1.0 - b_mult, 0.0, 0.80);
                            double alpha = clamp_dbl(strength * 0.48, 0.04, 0.34);
                            cairo_set_source_rgba(cr, 1.0, g_mult * 0.68, 0.04, alpha);
                            cairo_paint(cr);
                            dtwm_cairo_buffer_finish(cr, surf);
                            wlr_scene_buffer_set_buffer(out->night_light_buf, buf);
                            wlr_buffer_drop(buf);
                            wlr_scene_node_set_position(&out->night_light_buf->node, obox.x, obox.y);
                            wlr_scene_buffer_set_dest_size(out->night_light_buf, obox.width, obox.height);
                            wlr_scene_node_set_enabled(&out->night_light_buf->node, true);
                        }
                    }
                } else {
                    wlr_scene_node_set_enabled(&out->night_light_buf->node, false);
                    wlr_scene_buffer_set_buffer(out->night_light_buf, nullptr);
                }
                wlr_output_schedule_frame(wo);
            }
        }
    }

    publish_night_light_and_vrr_x11(server);
}

/* -------------------------------------------------------------------------
 * 5. Power Policy, Idle Auto-Suspend & Laptop Lid Clamshell / Suspend
 * ------------------------------------------------------------------------- */
static void power_policy_conf_path(char *out, size_t out_sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, out_sz, "%s/ck-core/power-policy.conf", xdg);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, out_sz, "%s/.config/ck-core/power-policy.conf",
             (home && home[0]) ? home : "/var/tmp");
}

static void reload_power_policy_if_changed(DtwmPowerDisplayState *st, bool force) {
    if (!st) return;
    char path[PATH_MAX];
    power_policy_conf_path(path, sizeof(path));
    struct stat sb;
    if (stat(path, &sb) != 0) return;
    if (!force && sb.st_mtime == st->policy_mtime) return;
    st->policy_mtime = sb.st_mtime;

    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[2048];
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
        if (strcmp(k, "ac_suspend_sec") == 0) {
            st->ac_suspend_sec = clamp_int(atoi(v), 0, 86400);
        } else if (strcmp(k, "bat_suspend_sec") == 0) {
            st->bat_suspend_sec = clamp_int(atoi(v), 0, 86400);
        } else if (strcmp(k, "lid_action") == 0) {
            if (strcmp(v, "docked-clamshell") == 0 || strcmp(v, "suspend") == 0 ||
                strcmp(v, "lock") == 0 || strcmp(v, "nothing") == 0) {
                snprintf(st->lid_action, sizeof(st->lid_action), "%s", v);
            }
        }
    }
    fclose(fp);
}

static bool detect_on_battery_power(void) {
    const char *sysfs = getenv("CK_STATUS_SYSFS_ROOT");
    char ps_dir[PATH_MAX];
    snprintf(ps_dir, sizeof(ps_dir), "%s/power_supply",
             (sysfs && sysfs[0]) ? sysfs : "/sys/class");
    DIR *dp = opendir(ps_dir);
    if (!dp) return false;

    bool has_bat = false;
    bool bat_discharging = false;
    bool saw_mains = false;
    bool mains_online = false;

    struct dirent *de;
    while ((de = readdir(dp)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        char tpath[PATH_MAX], tbuf[64] = {0};
        snprintf(tpath, sizeof(tpath), "%s/%s/type", ps_dir, de->d_name);
        FILE *fp = fopen(tpath, "r");
        if (fp) {
            if (fgets(tbuf, sizeof(tbuf), fp)) {
                char *nl = strpbrk(tbuf, "\r\n ");
                if (nl) *nl = '\0';
            }
            fclose(fp);
        } else if (strncmp(de->d_name, "BAT", 3) == 0) {
            strcpy(tbuf, "Battery");
        } else if (strncmp(de->d_name, "AC", 2) == 0 || strncmp(de->d_name, "ADP", 3) == 0) {
            strcpy(tbuf, "Mains");
        }

        if (strcasecmp(tbuf, "Mains") == 0 || strcasecmp(tbuf, "USB") == 0) {
            saw_mains = true;
            char opath[PATH_MAX], obuf[32] = {0};
            snprintf(opath, sizeof(opath), "%s/%s/online", ps_dir, de->d_name);
            FILE *of = fopen(opath, "r");
            if (of) {
                if (fgets(obuf, sizeof(obuf), of) && atoi(obuf) > 0) mains_online = true;
                fclose(of);
            }
        } else if (strcasecmp(tbuf, "Battery") == 0) {
            has_bat = true;
            char spath[PATH_MAX], sbuf[32] = {0};
            snprintf(spath, sizeof(spath), "%s/%s/status", ps_dir, de->d_name);
            FILE *sf = fopen(spath, "r");
            if (sf) {
                if (fgets(sbuf, sizeof(sbuf), sf) && strncasecmp(sbuf, "Discharging", 11) == 0) {
                    bat_discharging = true;
                }
                fclose(sf);
            }
        }
    }
    closedir(dp);

    if (!has_bat) return false;
    if (saw_mains) return !mains_online;
    return bat_discharging;
}

static bool invoke_logind_suspend_dbus(void) {
    /* Use libdbus-1 dynamically so dtwm-wayland links cleanly everywhere */
    typedef struct DBusConnection DBusConnection;
    typedef struct DBusMessage DBusMessage;
    typedef struct {
        const char *name;
        const char *message;
        unsigned int dummy1;
        void *padding1;
    } LocalDBusError;

    void *lib = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) lib = dlopen("libdbus-1.so", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return false;

    void (*p_err_init)(LocalDBusError *) = dlsym(lib, "dbus_error_init");
    void (*p_err_free)(LocalDBusError *) = dlsym(lib, "dbus_error_free");
    DBusConnection *(*p_bus_get)(int, LocalDBusError *) = dlsym(lib, "dbus_bus_get");
    void (*p_conn_unref)(DBusConnection *) = dlsym(lib, "dbus_connection_unref");
    DBusMessage *(*p_new_call)(const char *, const char *, const char *, const char *) =
        dlsym(lib, "dbus_message_new_method_call");
    int (*p_append_args)(DBusMessage *, int, ...) = dlsym(lib, "dbus_message_append_args");
    DBusMessage *(*p_send_block)(DBusConnection *, DBusMessage *, int, LocalDBusError *) =
        dlsym(lib, "dbus_connection_send_with_reply_and_block");
    void (*p_msg_unref)(DBusMessage *) = dlsym(lib, "dbus_message_unref");

    bool ok = false;
    if (p_err_init && p_err_free && p_bus_get && p_conn_unref &&
        p_new_call && p_append_args && p_send_block && p_msg_unref) {
        const char *bus_env = getenv("CK_STATUS_SYSTEM_BUS");
        int bus_type = (bus_env && strcmp(bus_env, "session") == 0) ? 0 /* SESSION */ : 1 /* SYSTEM */;
        LocalDBusError err;
        p_err_init(&err);
        DBusConnection *conn = p_bus_get(bus_type, &err);
        p_err_free(&err);
        if (conn) {
            DBusMessage *msg = p_new_call(
                "org.freedesktop.login1",
                "/org/freedesktop/login1",
                "org.freedesktop.login1.Manager",
                "Suspend"
            );
            if (msg) {
                unsigned int interactive = 1u;
                /* DBUS_TYPE_BOOLEAN = 'b' (98), DBUS_TYPE_INVALID = 0 */
                p_append_args(msg, (int)'b', &interactive, 0);
                p_err_init(&err);
                DBusMessage *rep = p_send_block(conn, msg, 1500, &err);
                p_err_free(&err);
                p_msg_unref(msg);
                if (rep) {
                    p_msg_unref(rep);
                    ok = true;
                }
            }
            p_conn_unref(conn);
        }
    }
    dlclose(lib);
    return ok;
}

static void trigger_screen_lock_helper(DtwmServer *server) {
    if (!server || server->session_locked || server->greeter_mode) return;
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            if (server->wayland_socket) setenv("WAYLAND_DISPLAY", server->wayland_socket, 1);
            if (server->xwayland && server->xwayland->display_name) {
                setenv("DISPLAY", server->xwayland->display_name, 1);
            }
            execlp("dtaction", "dtaction", "LockDisplay", (char *)nullptr);
            execl(CDE_INSTALLATION_TOP "/bin/dtaction", "dtaction", "LockDisplay", (char *)nullptr);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
    }
}

static bool is_internal_panel_name(const char *name) {
    if (!name) return false;
    return strncasecmp(name, "eDP", 3) == 0 ||
           strncasecmp(name, "LVDS", 4) == 0 ||
           strncasecmp(name, "DSI", 3) == 0;
}

static DtwmOutput *pick_internal_output_for_clamshell(DtwmServer *server, int *out_enabled_count) {
    int enabled_cnt = 0;
    DtwmOutput *internal = nullptr;
    DtwmOutput *first_enabled = nullptr;
    DtwmOutput *it;
    wl_list_for_each(it, &server->outputs, link) {
        if (!it->wlr_output || !it->wlr_output->enabled) continue;
        enabled_cnt++;
        if (!first_enabled) first_enabled = it;
        if (!internal && is_internal_panel_name(it->wlr_output->name)) {
            internal = it;
        }
    }
    if (out_enabled_count) *out_enabled_count = enabled_cnt;
    return internal ? internal : first_enabled;
}

static void handle_lid_state_transition(DtwmServer *server, bool closed) {
    if (!server || !server->power_display || server->greeter_mode) return;
    DtwmPowerDisplayState *st = server->power_display;
    reload_power_policy_if_changed(st, true);
    st->lid_closed = closed;

    bool on_battery = detect_on_battery_power();

    if (!closed) {
        /* Lid opened: re-enable internal panel if it was disabled in clamshell mode */
        if (st->clamshell_output_name[0]) {
            DtwmOutput *it;
            wl_list_for_each(it, &server->outputs, link) {
                if (it->wlr_output && it->wlr_output->name &&
                    strcmp(it->wlr_output->name, st->clamshell_output_name) == 0) {
                    struct wlr_output *wo = it->wlr_output;
                    struct wlr_output_state ost;
                    wlr_output_state_init(&ost);
                    wlr_output_state_set_enabled(&ost, true);
                    if (!wo->current_mode) {
                        struct wlr_output_mode *pref = wlr_output_preferred_mode(wo);
                        if (pref) {
                            wlr_output_state_set_mode(&ost, pref);
                        } else {
                            int cw = st->clamshell_saved_w > 0 ? st->clamshell_saved_w : 1280;
                            int ch = st->clamshell_saved_h > 0 ? st->clamshell_saved_h : 900;
                            wlr_output_state_set_custom_mode(&ost, cw, ch, 60000);
                        }
                    }
                    if (wlr_output_commit_state(wo, &ost)) {
                        wlr_output_layout_add(server->output_layout, wo,
                                              st->clamshell_saved_x, st->clamshell_saved_y);
                        dtwm_output_mgmt_after_reconfigure(server);
                    }
                    wlr_output_state_finish(&ost);
                    break;
                }
            }
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "reason=lid-open action=restore-panel enabled_output=%s on_battery=%d",
                     st->clamshell_output_name, on_battery ? 1 : 0);
            st->clamshell_output_name[0] = '\0';
            publish_power_policy_action(server, msg);
        }
        return;
    }

    int enabled_cnt = 0;
    DtwmOutput *internal = pick_internal_output_for_clamshell(server, &enabled_cnt);

    if (strcmp(st->lid_action, "nothing") == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "reason=lid-close action=none outputs=%d on_battery=%d",
                 enabled_cnt, on_battery ? 1 : 0);
        publish_power_policy_action(server, msg);
        return;
    }

    if (strcmp(st->lid_action, "lock") == 0) {
        trigger_screen_lock_helper(server);
        char msg[256];
        snprintf(msg, sizeof(msg), "reason=lid-close action=lock outputs=%d on_battery=%d",
                 enabled_cnt, on_battery ? 1 : 0);
        publish_power_policy_action(server, msg);
        return;
    }

    if (strcmp(st->lid_action, "suspend") == 0) {
        bool dbus_ok = invoke_logind_suspend_dbus();
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "reason=lid-close action=suspend dbus=%d outputs=%d on_battery=%d",
                 dbus_ok ? 1 : 0, enabled_cnt, on_battery ? 1 : 0);
        publish_power_policy_action(server, msg);
        return;
    }

    /* Default: "docked-clamshell" (Suspend on Battery / Clamshell when Docked) */
    if (enabled_cnt >= 2 && internal && internal->wlr_output) {
        struct wlr_output *wo = internal->wlr_output;
        struct wlr_box obox = {0};
        wlr_output_layout_get_box(server->output_layout, wo, &obox);
        snprintf(st->clamshell_output_name, sizeof(st->clamshell_output_name),
                 "%s", wo->name ? wo->name : "OUT-0");
        st->clamshell_saved_x = obox.x;
        st->clamshell_saved_y = obox.y;
        st->clamshell_saved_w = wo->width > 0 ? wo->width : 1280;
        st->clamshell_saved_h = wo->height > 0 ? wo->height : 900;

        struct wlr_output_state ost;
        wlr_output_state_init(&ost);
        wlr_output_state_set_enabled(&ost, false);
        if (wlr_output_commit_state(wo, &ost)) {
            wlr_output_layout_remove(server->output_layout, wo);
            /* Normalize remaining external output layout origin if needed */
            int min_x = 1000000, min_y = 1000000;
            bool any_ext = false;
            DtwmOutput *ext;
            wl_list_for_each(ext, &server->outputs, link) {
                if (ext->wlr_output && ext->wlr_output->enabled) {
                    struct wlr_box eb = {0};
                    wlr_output_layout_get_box(server->output_layout, ext->wlr_output, &eb);
                    if (eb.x < min_x) min_x = eb.x;
                    if (eb.y < min_y) min_y = eb.y;
                    any_ext = true;
                }
            }
            if (any_ext && (min_x != 0 || min_y != 0)) {
                wl_list_for_each(ext, &server->outputs, link) {
                    if (ext->wlr_output && ext->wlr_output->enabled) {
                        struct wlr_box eb = {0};
                        wlr_output_layout_get_box(server->output_layout, ext->wlr_output, &eb);
                        wlr_output_layout_add(server->output_layout, ext->wlr_output,
                                              eb.x - min_x, eb.y - min_y);
                    }
                }
            }
            dtwm_output_mgmt_after_reconfigure(server);
        }
        wlr_output_state_finish(&ost);

        char msg[256];
        snprintf(msg, sizeof(msg),
                 "reason=lid-close action=clamshell disabled_output=%s outputs=%d on_battery=%d",
                 st->clamshell_output_name, enabled_cnt, on_battery ? 1 : 0);
        publish_power_policy_action(server, msg);
        return;
    }

    if (on_battery) {
        bool dbus_ok = invoke_logind_suspend_dbus();
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "reason=lid-close action=suspend dbus=%d outputs=%d on_battery=1",
                 dbus_ok ? 1 : 0, enabled_cnt);
        publish_power_policy_action(server, msg);
    } else {
        trigger_screen_lock_helper(server);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "reason=lid-close action=lock outputs=%d on_battery=0", enabled_cnt);
        publish_power_policy_action(server, msg);
    }
}

static void check_idle_suspend_with_elapsed(DtwmServer *server, int idle_sec, bool force_recheck) {
    if (!server || !server->power_display || server->greeter_mode) return;
    DtwmPowerDisplayState *st = server->power_display;
    reload_power_policy_if_changed(st, force_recheck);

    bool on_battery = detect_on_battery_power();
    int limit_sec = on_battery ? st->bat_suspend_sec : st->ac_suspend_sec;
    if (limit_sec <= 0) return;

    if (idle_sec >= limit_sec && (!st->idle_suspend_fired || force_recheck)) {
        if (server->portal_idle_inhibited) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "reason=idle action=inhibited_by_portal idle_sec=%d limit_sec=%d on_battery=%d",
                     idle_sec, limit_sec, on_battery ? 1 : 0);
            publish_power_policy_action(server, msg);
            return;
        }
        st->idle_suspend_fired = true;
        bool dbus_ok = invoke_logind_suspend_dbus();
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "reason=idle action=suspend dbus=%d idle_sec=%d limit_sec=%d on_battery=%d",
                 dbus_ok ? 1 : 0, idle_sec, limit_sec, on_battery ? 1 : 0);
        publish_power_policy_action(server, msg);
    }
}

static void handle_switch_toggle(struct wl_listener *listener, void *data) {
    DtwmSwitchDev *sw = wl_container_of(listener, sw, toggle);
    struct wlr_switch_toggle_event *ev = data;
    if (!sw || !sw->server || !ev) return;
    if (ev->switch_type == WLR_SWITCH_TYPE_LID) {
        handle_lid_state_transition(sw->server, ev->switch_state == WLR_SWITCH_STATE_ON);
    }
}

static void handle_switch_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmSwitchDev *sw = wl_container_of(listener, sw, destroy);
    wl_list_remove(&sw->toggle.link);
    wl_list_remove(&sw->destroy.link);
    wl_list_remove(&sw->link);
    free(sw);
}

void dtwm_power_display_on_new_input(DtwmServer *server, struct wlr_input_device *dev) {
    if (!server || !server->power_display || !dev || dev->type != WLR_INPUT_DEVICE_SWITCH) {
        return;
    }
    struct wlr_switch *wlr_sw = wlr_switch_from_input_device(dev);
    if (!wlr_sw) return;

    DtwmSwitchDev *sw = calloc(1, sizeof(*sw));
    if (!sw) return;
    sw->server = server;
    sw->wlr_switch = wlr_sw;
    sw->toggle.notify = handle_switch_toggle;
    wl_signal_add(&wlr_sw->events.toggle, &sw->toggle);
    sw->destroy.notify = handle_switch_destroy;
    wl_signal_add(&dev->events.destroy, &sw->destroy);
    wl_list_insert(&server->power_display->switches, &sw->link);
}

void dtwm_power_display_notify_activity(DtwmServer *server) {
    if (!server || !server->power_display) return;
    server->power_display->last_activity_sec = mono_seconds_now();
    server->power_display->idle_suspend_fired = false;
}

static int handle_periodic_power_timer(void *data) {
    DtwmServer *server = data;
    if (!server || server->shutting_down || !server->power_display) return 0;
    DtwmPowerDisplayState *st = server->power_display;

    bool want_nl = evaluate_night_light_schedule(st);
    if (want_nl != st->nl_active) {
        dtwm_power_display_sync_outputs(server);
    }

    uint64_t now_s = mono_seconds_now();
    int idle_s = (now_s >= st->last_activity_sec) ? (int)(now_s - st->last_activity_sec) : 0;
    check_idle_suspend_with_elapsed(server, idle_s, false);

    if (st->periodic_timer) {
        wl_event_source_timer_update(st->periodic_timer, 15000);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * 6. Super+Arrow Half & Quadrant Keyboard Window Tiling
 * ------------------------------------------------------------------------- */
static bool view_eligible_for_tiling(const DtwmView *v) {
    if (!v || !v->mapped || v->minimized || v->is_panel || v->is_subpanel ||
        v->is_desktop_pin || v->is_swallowed || v->is_lock_dialog) {
        return false;
    }
    if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface &&
        v->xwayland_surface->override_redirect) {
        return false;
    }
    return v->has_resize_handles;
}

static void tile_active_window(DtwmServer *server, const char *dir) {
    if (!server || !dir || server->session_locked || server->greeter_mode) return;
    DtwmView *v = server->focused_view;
    if (!view_eligible_for_tiling(v)) {
        DtwmView *cand;
        wl_list_for_each(cand, &server->views, link) {
            if (dtwm_view_visible_on_current_ws(cand) &&
                view_eligible_for_tiling(cand)) {
                v = cand;
                dtwm_restore_view_focus(server, v);
                break;
            }
        }
    }
    if (!view_eligible_for_tiling(v)) return;

    int outer_w = 0, outer_h = 0, l = 0, t = 0, r = 0, b = 0;
    dtwm_view_outer_size(v, &outer_w, &outer_h);
    dtwm_view_insets(v, &l, &t, &r, &b);

    struct wlr_box ubox;
    dtwm_get_usable_output_box(server, v->x + outer_w * 0.5, v->y + outer_h * 0.5, &ubox);
    if (ubox.width < 160 || ubox.height < 120) return;

    /* Save pre-tile geometry before first tile */
    if (v->tile_state == DTWM_TILE_NONE && !v->maximized) {
        v->tile_saved_x = v->x;
        v->tile_saved_y = v->y;
        v->tile_saved_w = v->client_w;
        v->tile_saved_h = v->client_h;
        v->has_tile_saved_geom = true;
    } else if (!v->has_tile_saved_geom) {
        v->tile_saved_x = v->has_saved_geom ? v->saved_x : (ubox.x + 64);
        v->tile_saved_y = v->has_saved_geom ? v->saved_y : (ubox.y + 48);
        v->tile_saved_w = v->has_saved_geom ? v->saved_w : 640;
        v->tile_saved_h = v->has_saved_geom ? v->saved_h : 440;
        v->has_tile_saved_geom = true;
    }

    DtwmTileState cur = (DtwmTileState)v->tile_state;
    DtwmTileState next = DTWM_TILE_NONE;

    if (strcasecmp(dir, "left") == 0) {
        if (v->maximized) {
            dtwm_view_set_maximized(v, false);
            next = DTWM_TILE_LEFT;
        } else if (cur == DTWM_TILE_TOP || cur == DTWM_TILE_TOP_RIGHT) {
            next = DTWM_TILE_TOP_LEFT;
        } else if (cur == DTWM_TILE_BOTTOM || cur == DTWM_TILE_BOTTOM_RIGHT) {
            next = DTWM_TILE_BOTTOM_LEFT;
        } else {
            next = DTWM_TILE_LEFT;
        }
    } else if (strcasecmp(dir, "right") == 0) {
        if (v->maximized) {
            dtwm_view_set_maximized(v, false);
            next = DTWM_TILE_RIGHT;
        } else if (cur == DTWM_TILE_TOP || cur == DTWM_TILE_TOP_LEFT) {
            next = DTWM_TILE_TOP_RIGHT;
        } else if (cur == DTWM_TILE_BOTTOM || cur == DTWM_TILE_BOTTOM_LEFT) {
            next = DTWM_TILE_BOTTOM_RIGHT;
        } else {
            next = DTWM_TILE_RIGHT;
        }
    } else if (strcasecmp(dir, "up") == 0) {
        if (v->maximized) {
            return;
        }
        if (cur == DTWM_TILE_TOP) {
            /* Pressing Super+Up again while Top Half maximizes */
            v->tile_state = DTWM_TILE_NONE;
            if (v->has_tile_saved_geom) {
                v->saved_x = v->tile_saved_x;
                v->saved_y = v->tile_saved_y;
                v->saved_w = v->tile_saved_w;
                v->saved_h = v->tile_saved_h;
                v->has_saved_geom = true;
            }
            dtwm_view_set_maximized(v, true);
            /* Preserve pre-tile geometry across the maximize call */
            if (v->has_tile_saved_geom) {
                v->saved_x = v->tile_saved_x;
                v->saved_y = v->tile_saved_y;
                v->saved_w = v->tile_saved_w;
                v->saved_h = v->tile_saved_h;
                v->has_saved_geom = true;
            }
            return;
        }
        if (cur == DTWM_TILE_LEFT || cur == DTWM_TILE_BOTTOM_LEFT) {
            next = DTWM_TILE_TOP_LEFT;
        } else if (cur == DTWM_TILE_RIGHT || cur == DTWM_TILE_BOTTOM_RIGHT) {
            next = DTWM_TILE_TOP_RIGHT;
        } else {
            next = DTWM_TILE_TOP;
        }
    } else if (strcasecmp(dir, "down") == 0) {
        if (v->maximized) {
            dtwm_view_set_maximized(v, false);
            v->tile_state = DTWM_TILE_NONE;
            return;
        }
        if (cur == DTWM_TILE_LEFT) {
            next = DTWM_TILE_BOTTOM_LEFT;
        } else if (cur == DTWM_TILE_RIGHT) {
            next = DTWM_TILE_BOTTOM_RIGHT;
        } else if (cur == DTWM_TILE_BOTTOM ||
                   cur == DTWM_TILE_TOP_LEFT || cur == DTWM_TILE_TOP_RIGHT ||
                   cur == DTWM_TILE_BOTTOM_LEFT || cur == DTWM_TILE_BOTTOM_RIGHT) {
            /* Restore saved pre-tile geometry */
            v->tile_state = DTWM_TILE_NONE;
            if (v->has_tile_saved_geom) {
                dtwm_view_apply_geometry(
                    v, v->tile_saved_x, v->tile_saved_y, v->tile_saved_w, v->tile_saved_h
                );
            }
            dtwm_publish_active_window_box(server);
            return;
        } else {
            next = DTWM_TILE_BOTTOM;
        }
    } else {
        return;
    }

    int hw = ubox.width / 2;
    int hh = ubox.height / 2;
    struct wlr_box tbox = ubox;
    switch (next) {
    case DTWM_TILE_LEFT:
        tbox = (struct wlr_box){ubox.x, ubox.y, hw, ubox.height};
        break;
    case DTWM_TILE_RIGHT:
        tbox = (struct wlr_box){ubox.x + hw, ubox.y, ubox.width - hw, ubox.height};
        break;
    case DTWM_TILE_TOP:
        tbox = (struct wlr_box){ubox.x, ubox.y, ubox.width, hh};
        break;
    case DTWM_TILE_BOTTOM:
        tbox = (struct wlr_box){ubox.x, ubox.y + hh, ubox.width, ubox.height - hh};
        break;
    case DTWM_TILE_TOP_LEFT:
        tbox = (struct wlr_box){ubox.x, ubox.y, hw, hh};
        break;
    case DTWM_TILE_TOP_RIGHT:
        tbox = (struct wlr_box){ubox.x + hw, ubox.y, ubox.width - hw, hh};
        break;
    case DTWM_TILE_BOTTOM_LEFT:
        tbox = (struct wlr_box){ubox.x, ubox.y + hh, hw, ubox.height - hh};
        break;
    case DTWM_TILE_BOTTOM_RIGHT:
        tbox = (struct wlr_box){ubox.x + hw, ubox.y + hh, ubox.width - hw, ubox.height - hh};
        break;
    default:
        return;
    }

    v->tile_state = (int)next;
    int cw = tbox.width - l - r;
    int ch = tbox.height - t - b;
    dtwm_view_apply_geometry(v, tbox.x, tbox.y, cw, ch);
    dtwm_publish_active_window_box(server);
}

bool dtwm_power_display_handle_keybinding(DtwmServer *server, uint32_t mods, uint32_t keysym) {
    if (!server) return false;
    uint32_t clean = mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT);
    if (clean == WLR_MODIFIER_LOGO) {
        if (keysym == XKB_KEY_Left || keysym == XKB_KEY_KP_Left) {
            tile_active_window(server, "left");
            return true;
        }
        if (keysym == XKB_KEY_Right || keysym == XKB_KEY_KP_Right) {
            tile_active_window(server, "right");
            return true;
        }
        if (keysym == XKB_KEY_Up || keysym == XKB_KEY_KP_Up) {
            tile_active_window(server, "up");
            return true;
        }
        if (keysym == XKB_KEY_Down || keysym == XKB_KEY_KP_Down) {
            tile_active_window(server, "down");
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------
 * 7. _DT_WM_REQUEST IPC Dispatch & Lifecycle Hooks
 * ------------------------------------------------------------------------- */
bool dtwm_power_display_handle_wm_request(DtwmServer *server, const char *cmd) {
    if (!server || !server->power_display || !cmd || server->session_locked) return false;
    DtwmPowerDisplayState *st = server->power_display;

    if (strncmp(cmd, "f.set_night_light", 17) == 0 &&
        (cmd[17] == '\0' || isspace((unsigned char)cmd[17]))) {
        int en = 0, temp = 4000, smin = st->nl_start_min, emin = st->nl_end_min;
        char mode[24] = "always";
        int parsed = sscanf(cmd + 17, "%d %23s %d %d %d", &en, mode, &temp, &smin, &emin);
        if (parsed >= 3) {
            st->nl_enabled = (en != 0);
            if (strcmp(mode, "off") == 0 || strcmp(mode, "always") == 0 ||
                strcmp(mode, "schedule") == 0 || strcmp(mode, "sunset") == 0) {
                snprintf(st->nl_mode, sizeof(st->nl_mode), "%s", mode);
            }
            st->nl_temp_k = clamp_int(temp, 2700, 6500);
            if (parsed >= 5) {
                st->nl_start_min = clamp_int(smin, 0, 1439);
                st->nl_end_min = clamp_int(emin, 0, 1439);
            }
            save_ergonomics_conf(st);
            dtwm_power_display_sync_outputs(server);
        }
        return true;
    }

    if (strncmp(cmd, "f.set_vrr", 9) == 0 && isspace((unsigned char)cmd[9])) {
        char out_name[64] = {0};
        int en = 0;
        if (sscanf(cmd + 10, "%63s %d", out_name, &en) == 2) {
            bool want = (en != 0);
            DtwmOutput *out;
            wl_list_for_each(out, &server->outputs, link) {
                if (!out->wlr_output || !out->wlr_output->name) continue;
                if (strcmp(out_name, "all") == 0 || strcmp(out_name, "*") == 0 ||
                    strcmp(out->wlr_output->name, out_name) == 0) {
                    out->vrr_enabled = want;
                    record_vrr_rule(st, out->wlr_output->name, want);
                }
            }
            if (strcmp(out_name, "all") != 0 && strcmp(out_name, "*") != 0) {
                record_vrr_rule(st, out_name, want);
            }
            save_ergonomics_conf(st);
            dtwm_power_display_sync_outputs(server);
        }
        return true;
    }

    if (strncmp(cmd, "f.tile_window", 13) == 0 && isspace((unsigned char)cmd[13])) {
        char dir[32] = {0};
        if (sscanf(cmd + 14, "%31s", dir) == 1) {
            tile_active_window(server, dir);
        }
        return true;
    }

    if (strncmp(cmd, "f.reload_power_policy", 21) == 0) {
        reload_power_policy_if_changed(st, true);
        return true;
    }

    if (strncmp(cmd, "f.simulate_lid", 14) == 0 && isspace((unsigned char)cmd[14])) {
        char arg[32] = {0};
        if (sscanf(cmd + 15, "%31s", arg) == 1) {
            bool closed = (strcasecmp(arg, "closed") == 0 || strcasecmp(arg, "close") == 0 ||
                           strcmp(arg, "1") == 0);
            handle_lid_state_transition(server, closed);
        }
        return true;
    }

    if (strncmp(cmd, "f.check_idle_suspend", 20) == 0 && isspace((unsigned char)cmd[20])) {
        int idle_sec = 0;
        if (sscanf(cmd + 21, "%d", &idle_sec) == 1) {
            check_idle_suspend_with_elapsed(server, idle_sec, true);
        }
        return true;
    }

    return false;
}

static void restore_saved_backlight_async(DtwmServer *server) {
    if (!server || server->greeter_mode) return;
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) {
                dup2(dn, STDIN_FILENO);
                dup2(dn, STDOUT_FILENO);
                dup2(dn, STDERR_FILENO);
                if (dn > 2) close(dn);
            }
            if (server->exe_dir[0]) {
                char local_bin[1152];
                snprintf(local_bin, sizeof(local_bin), "%s/dtwm-media-ctl", server->exe_dir);
                execl(local_bin, "dtwm-media-ctl", "bright-restore", (char *)nullptr);
            }
            execlp("dtwm-media-ctl", "dtwm-media-ctl", "bright-restore", (char *)nullptr);
            execl(CDE_INSTALLATION_TOP "/bin/dtwm-media-ctl", "dtwm-media-ctl", "bright-restore", (char *)nullptr);
            _exit(0);
        }
        _exit(0);
    } else if (pid > 0) {
        int wst = 0;
        (void)waitpid(pid, &wst, 0);
    }
}

void dtwm_power_display_init(DtwmServer *server) {
    if (!server) return;
    DtwmPowerDisplayState *st = calloc(1, sizeof(*st));
    if (!st) return;
    st->server = server;
    wl_list_init(&st->switches);

    st->nl_enabled = false;
    snprintf(st->nl_mode, sizeof(st->nl_mode), "schedule");
    st->nl_temp_k = 4000;
    st->nl_start_min = 20 * 60;
    st->nl_end_min = 6 * 60;
    st->r_permille = 1000;
    st->g_permille = 1000;
    st->b_permille = 1000;

    st->ac_suspend_sec = 0;
    st->bat_suspend_sec = 900;
    snprintf(st->lid_action, sizeof(st->lid_action), "docked-clamshell");
    st->last_activity_sec = mono_seconds_now();

    server->power_display = st;

    st->gamma_mgr = wlr_gamma_control_manager_v1_create(server->wl_display);
    if (st->gamma_mgr && server->scene) {
        wlr_scene_set_gamma_control_manager_v1(server->scene, st->gamma_mgr);
    }

    load_ergonomics_conf(st);
    reload_power_policy_if_changed(st, true);

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    if (loop) {
        st->periodic_timer = wl_event_loop_add_timer(loop, handle_periodic_power_timer, server);
        if (st->periodic_timer) {
            wl_event_source_timer_update(st->periodic_timer, 15000);
        }
    }
}

void dtwm_power_display_on_xwayland_ready(DtwmServer *server) {
    if (!server || !server->power_display) return;
    if (!server->greeter_mode) {
        restore_saved_backlight_async(server);
    }
    dtwm_power_display_sync_outputs(server);
    if (server->power_display->last_power_action[0]) {
        publish_power_policy_action(server, server->power_display->last_power_action);
    }
}

void dtwm_power_display_on_new_output(DtwmServer *server, DtwmOutput *output) {
    if (!server || !server->power_display || !output || !output->wlr_output) return;
    if (output->wlr_output->name) {
        output->vrr_enabled = lookup_vrr_rule(server->power_display, output->wlr_output->name);
    }
    dtwm_power_display_sync_outputs(server);
}

void dtwm_power_display_on_output_destroy(DtwmServer *server, DtwmOutput *output) {
    (void)server;
    if (!output) return;
    if (output->night_light_buf) {
        wlr_scene_node_destroy(&output->night_light_buf->node);
        output->night_light_buf = nullptr;
    }
}

void dtwm_power_display_finish(DtwmServer *server) {
    if (!server || !server->power_display) return;
    DtwmPowerDisplayState *st = server->power_display;
    server->power_display = nullptr;

    if (st->periodic_timer) {
        wl_event_source_remove(st->periodic_timer);
    }
    DtwmSwitchDev *sw, *tmp;
    wl_list_for_each_safe(sw, tmp, &st->switches, link) {
        wl_list_remove(&sw->toggle.link);
        wl_list_remove(&sw->destroy.link);
        wl_list_remove(&sw->link);
        free(sw);
    }
    free(st);
}
