/*
 * CDE - Common Desktop Environment
 *
 * wayland/cde_output_mgmt.c: Multi-monitor output management
 * (zwlr_output_manager_v1, zwlr_output_power_manager_v1), dtstyle Display
 * IPC (_DT_WAYLAND_OUTPUTS + f.set_output_layout), WebRTC DMA-BUF export
 * (zwlr_export_dmabuf_manager_v1), and foreign toplevel management
 * (zwlr_foreign_toplevel_manager_v1).
 */

#define _GNU_SOURCE
#include "dtwm_wayland.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>

struct DtwmForeignToplevel {
    DtwmView *view;
    struct wlr_foreign_toplevel_handle_v1 *handle;
    struct wl_listener request_maximize;
    struct wl_listener request_minimize;
    struct wl_listener request_activate;
    struct wl_listener request_close;
    struct wl_listener destroy;
};

#define IDENT_BADGE_LOGICAL_SZ 136
#define IDENT_BADGE_MARGIN     28
#define MAX_IDENT_RULES        16

typedef struct {
    char name[64];
    int number;
} DtwmIdentRule;

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_buffer *scene_buf;
    struct wl_listener output_destroy;
} DtwmIdentBadge;

static bool g_ident_active = false;
static int g_ident_selected_num = 1;
static int g_ident_rule_count = 0;
static DtwmIdentRule g_ident_rules[MAX_IDENT_RULES];
static struct wl_list g_ident_badges;
static bool g_ident_list_inited = false;
static struct wl_event_source *g_ident_watchdog = nullptr;
static bool g_ident_saw_dlg_win = false;
static int g_ident_unseen_ticks = 0;
static xcb_atom_t g_atom_ident_state = XCB_ATOM_NONE;
static xcb_atom_t g_atom_dlg_win = XCB_ATOM_NONE;

static void sync_ident_overlays(DtwmServer *server);
static void clear_ident_overlays(DtwmServer *server);

static xcb_atom_t intern_xcb_atom(xcb_connection_t *conn, const char *name) {
    if (!conn || !name || !name[0]) return XCB_ATOM_NONE;
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, nullptr);
    if (!rep) return XCB_ATOM_NONE;
    xcb_atom_t a = rep->atom;
    free(rep);
    return a;
}

static int transform_to_degrees(enum wl_output_transform tr) {
    switch (tr) {
    case WL_OUTPUT_TRANSFORM_90:
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        return 90;
    case WL_OUTPUT_TRANSFORM_180:
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        return 180;
    case WL_OUTPUT_TRANSFORM_270:
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        return 270;
    case WL_OUTPUT_TRANSFORM_NORMAL:
    case WL_OUTPUT_TRANSFORM_FLIPPED:
    default:
        return 0;
    }
}

static enum wl_output_transform degrees_to_transform(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg == 90) return WL_OUTPUT_TRANSFORM_90;
    if (deg == 180) return WL_OUTPUT_TRANSFORM_180;
    if (deg == 270) return WL_OUTPUT_TRANSFORM_270;
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

static DtwmOutput *find_dtwm_output(DtwmServer *server, struct wlr_output *wlr_out) {
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->wlr_output == wlr_out) {
            return out;
        }
    }
    return nullptr;
}

static DtwmOutput *find_dtwm_output_by_name(DtwmServer *server, const char *name) {
    if (!name || !name[0]) return nullptr;
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->wlr_output && out->wlr_output->name &&
            strcmp(out->wlr_output->name, name) == 0) {
            return out;
        }
    }
    /* Also allow numeric index fallback ("0", "1", ...) */
    char *endp = nullptr;
    unsigned long idx = strtoul(name, &endp, 10);
    if (endp && *endp == '\0') {
        wl_list_for_each(out, &server->outputs, link) {
            if (out->output_idx == (uint32_t)idx) {
                return out;
            }
        }
    }
    return nullptr;
}

static void refresh_output_scene_geometry(DtwmServer *server, DtwmOutput *out) {
    if (!out || !out->wlr_output) return;
    if (!out->wlr_output->enabled) {
        if (out->bg_rect) {
            wlr_scene_node_set_enabled(&out->bg_rect->node, false);
        }
        if (out->backdrop_buf) {
            wlr_scene_node_set_enabled(&out->backdrop_buf->node, false);
        }
        return;
    }

    struct wlr_output_layout_output *lo =
        wlr_output_layout_get(server->output_layout, out->wlr_output);
    struct wlr_scene_output *so = wlr_scene_get_scene_output(server->scene, out->wlr_output);
    if (!so) {
        so = wlr_scene_output_create(server->scene, out->wlr_output);
    }
    if (lo && so) {
        wlr_scene_output_layout_add_output(server->scene_layout, lo, so);
    }

    struct wlr_box obox = {0};
    wlr_output_layout_get_box(server->output_layout, out->wlr_output, &obox);
    if (out->bg_rect && obox.width > 0 && obox.height > 0) {
        wlr_scene_rect_set_size(out->bg_rect, obox.width, obox.height);
        wlr_scene_node_set_position(&out->bg_rect->node, obox.x, obox.y);
        wlr_scene_node_set_enabled(&out->bg_rect->node, true);
    }
    if (out->backdrop_buf) {
        wlr_scene_node_set_enabled(&out->backdrop_buf->node, true);
    }
}

static void after_outputs_reconfigured(DtwmServer *server) {
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        refresh_output_scene_geometry(server, out);
    }
    dtwm_sync_resource_manager_dpi(server);
    dtwm_reload_cursor_theme(server, server->cursor_theme, server->cursor_size);
    dtwm_update_all_backdrops(server);
    dtwm_update_front_panel(server);
    dtwm_icons_relayout(server);
    dtwm_sync_all_external_client_scales(server);
    dtwm_reflow_views_for_outputs(server);
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped && v->decorated) {
            dtwm_view_update_frame(v);
        }
    }
    dtwm_sync_xwayland_randr_dpi(server);
    dtwm_lock_sync_outputs(server);
    dtwm_output_mgmt_sync(server);
    dtwm_power_display_sync_outputs(server);
    if (g_ident_active) {
        sync_ident_overlays(server);
    }
}

void dtwm_output_mgmt_after_reconfigure(DtwmServer *server) {
    if (server) after_outputs_reconfigured(server);
}

void dtwm_output_mgmt_publish_x11(DtwmServer *server) {
    if (!server) return;

    char buf[4096];
    int pos = 0;
    buf[0] = '\0';
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        struct wlr_output *wo = out->wlr_output;
        if (!wo) continue;

        struct wlr_box obox = {0};
        if (wo->enabled) {
            wlr_output_layout_get_box(server->output_layout, wo, &obox);
        }
        /* Raw unrotated mode dimensions */
        int mode_w = wo->width > 0 ? wo->width : 1280;
        int mode_h = wo->height > 0 ? wo->height : 900;
        int rot = transform_to_degrees(wo->transform);
        int dpi = out->dpi > 0 ? out->dpi : 96;

        char modes_csv[512];
        int mp = 0;
        modes_csv[0] = '\0';
        struct wlr_output_mode *m;
        wl_list_for_each(m, &wo->modes, link) {
            char tok[32];
            snprintf(tok, sizeof(tok), "%dx%d", m->width, m->height);
            if (!strstr(modes_csv, tok) && mp + (int)strlen(tok) + 2 < (int)sizeof(modes_csv)) {
                mp += snprintf(modes_csv + mp, sizeof(modes_csv) - (size_t)mp, "%s%s",
                               mp > 0 ? "," : "", tok);
            }
        }
        /* Include current mode + standard workstation/laptop resolutions for virtual/headless outputs */
        static const char *const std_modes[] = {
            "3840x2400", "2560x1600", "2560x1440", "1920x1200", "1920x1080",
            "1600x900", "1440x900", "1280x900", "1280x800", "1024x768"
        };
        char cur_tok[32];
        snprintf(cur_tok, sizeof(cur_tok), "%dx%d", mode_w, mode_h);
        if (!strstr(modes_csv, cur_tok) && mp + (int)strlen(cur_tok) + 2 < (int)sizeof(modes_csv)) {
            mp += snprintf(modes_csv + mp, sizeof(modes_csv) - (size_t)mp, "%s%s",
                           mp > 0 ? "," : "", cur_tok);
        }
        for (size_t s = 0; s < sizeof(std_modes) / sizeof(std_modes[0]); ++s) {
            if (!strstr(modes_csv, std_modes[s]) &&
                mp + (int)strlen(std_modes[s]) + 2 < (int)sizeof(modes_csv)) {
                mp += snprintf(modes_csv + mp, sizeof(modes_csv) - (size_t)mp, "%s%s",
                               mp > 0 ? "," : "", std_modes[s]);
            }
        }

        int n = snprintf(
            buf + pos, sizeof(buf) - (size_t)pos,
            "%u|%s|%d|%d|%d|%d|%d|%d|%d|%s\n",
            out->output_idx,
            wo->name ? wo->name : "OUT",
            wo->enabled ? 1 : 0,
            obox.x,
            obox.y,
            mode_w,
            mode_h,
            rot,
            dpi,
            modes_csv
        );
        if (n > 0 && pos + n < (int)sizeof(buf)) {
            pos += n;
        }
    }

    if (pos > 0) {
        dtwm_native_ipc_publish_prop(server, "_DT_WAYLAND_OUTPUTS", buf, (size_t)pos);
    }

    if (!server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) return;
    if (server->atom_wayland_outputs == XCB_ATOM_NONE) {
        server->atom_wayland_outputs = intern_xcb_atom(server->xcb_conn, "_DT_WAYLAND_OUTPUTS");
    }
    if (server->atom_wayland_outputs == XCB_ATOM_NONE) return;

    xcb_change_property(
        server->xcb_conn,
        XCB_PROP_MODE_REPLACE,
        server->xcb_root,
        server->atom_wayland_outputs,
        XCB_ATOM_STRING,
        8,
        (uint32_t)pos,
        buf
    );
    xcb_flush(server->xcb_conn);
}

void dtwm_output_mgmt_sync(DtwmServer *server) {
    if (!server) return;
    if (server->output_mgr) {
        struct wlr_output_configuration_v1 *cfg = wlr_output_configuration_v1_create();
        if (cfg) {
            DtwmOutput *out;
            wl_list_for_each(out, &server->outputs, link) {
                if (!out->wlr_output) continue;
                struct wlr_output_configuration_head_v1 *head =
                    wlr_output_configuration_head_v1_create(cfg, out->wlr_output);
                if (!head) continue;
                struct wlr_box obox = {0};
                if (out->wlr_output->enabled) {
                    wlr_output_layout_get_box(server->output_layout, out->wlr_output, &obox);
                    head->state.x = obox.x;
                    head->state.y = obox.y;
                }
                head->state.enabled = out->wlr_output->enabled;
                /* Report logical HiDPI scale matching our per-output XLibre RandR DPI */
                head->state.scale = (out->dpi > 0) ? ((float)out->dpi / 96.0f) : out->wlr_output->scale;
            }
            wlr_output_manager_v1_set_configuration(server->output_mgr, cfg);
        }
    }
    dtwm_output_mgmt_publish_x11(server);
}

static void apply_or_test_output_config(
    DtwmServer *server,
    struct wlr_output_configuration_v1 *config,
    bool test_only
) {
    bool ok = true;
    int enabled_count = 0;

    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link) {
        if (head->state.enabled) enabled_count++;
    }
    if (enabled_count == 0 && !wl_list_empty(&server->outputs)) {
        /* Refuse configurations that would disable every display head */
        wlr_output_configuration_v1_send_failed(config);
        wlr_output_configuration_v1_destroy(config);
        return;
    }

    wl_list_for_each(head, &config->heads, link) {
        struct wlr_output *wo = head->state.output;
        if (!wo) continue;

        struct wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_enabled(&st, head->state.enabled);

        if (head->state.enabled) {
            if (head->state.mode != nullptr) {
                wlr_output_state_set_mode(&st, head->state.mode);
            } else if (head->state.custom_mode.width > 0 && head->state.custom_mode.height > 0) {
                wlr_output_state_set_custom_mode(
                    &st,
                    head->state.custom_mode.width,
                    head->state.custom_mode.height,
                    head->state.custom_mode.refresh > 0 ? head->state.custom_mode.refresh : 60000
                );
            } else if (!wo->current_mode) {
                int cw = wo->width > 0 ? wo->width : 1280;
                int ch = wo->height > 0 ? wo->height : 900;
                wlr_output_state_set_custom_mode(&st, cw, ch, 60000);
            }
            wlr_output_state_set_transform(&st, head->state.transform);
            /* Always commit 1:1 physical output scale so wlr_scene operates in exact physical pixels */
            wlr_output_state_set_scale(&st, 1.0f);
        }

        if (test_only) {
            if (!wlr_output_test_state(wo, &st)) {
                ok = false;
            }
        } else {
            if (!wlr_output_commit_state(wo, &st)) {
                ok = false;
            } else {
                DtwmOutput *dout = find_dtwm_output(server, wo);
                if (head->state.enabled) {
                    wlr_output_layout_add(server->output_layout, wo, head->state.x, head->state.y);
                    if (dout && head->state.scale > 0.25f) {
                        int new_dpi = (int)lroundf(head->state.scale * 96.0f);
                        if (new_dpi < 72) new_dpi = 72;
                        if (new_dpi > 384) new_dpi = 384;
                        dout->dpi = new_dpi;
                        if (wo->name && wo->name[0]) {
                            dtwm_power_display_record_dpi(server, wo->name, new_dpi);
                        }
                    }
                } else {
                    wlr_output_layout_remove(server->output_layout, wo);
                }
            }
        }
        wlr_output_state_finish(&st);
        if (!ok) break;
    }

    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);

    if (!test_only && ok) {
        after_outputs_reconfigured(server);
    }
}

static void handle_output_mgr_apply(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, output_mgr_apply);
    struct wlr_output_configuration_v1 *config = data;
    apply_or_test_output_config(server, config, false);
}

static void handle_output_mgr_test(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, output_mgr_test);
    struct wlr_output_configuration_v1 *config = data;
    apply_or_test_output_config(server, config, true);
}

static void handle_output_power_set_mode(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, output_power_set_mode);
    struct wlr_output_power_v1_set_mode_event *ev = data;
    if (!ev || !ev->output) return;

    bool enable = (ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, enable);
    if (enable && !ev->output->current_mode) {
        struct wlr_output_mode *pref = wlr_output_preferred_mode(ev->output);
        if (pref) {
            wlr_output_state_set_mode(&st, pref);
        } else {
            int cw = ev->output->width > 0 ? ev->output->width : 1280;
            int ch = ev->output->height > 0 ? ev->output->height : 900;
            wlr_output_state_set_custom_mode(&st, cw, ch, 60000);
        }
    }
    if (wlr_output_commit_state(ev->output, &st)) {
        if (enable && !wlr_output_layout_get(server->output_layout, ev->output)) {
            wlr_output_layout_add_auto(server->output_layout, ev->output);
        }
        after_outputs_reconfigured(server);
    }
    wlr_output_state_finish(&st);
}

/* -------------------------------------------------------------------------
 * Semi-Transparent Corner Screen-Number Identification Overlays
 * ------------------------------------------------------------------------- */
static bool ident_reject_input(struct wlr_scene_buffer *buf, double *sx, double *sy) {
    (void)buf;
    (void)sx;
    (void)sy;
    return false;
}

static void publish_ident_x11_state(DtwmServer *server, const char *state_str) {
    if (!server || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE || !state_str) return;
    if (g_atom_ident_state == XCB_ATOM_NONE) {
        g_atom_ident_state = intern_xcb_atom(server->xcb_conn, "_DT_MONITOR_IDENTIFY_STATE");
    }
    if (g_atom_ident_state == XCB_ATOM_NONE) return;
    xcb_change_property(
        server->xcb_conn,
        XCB_PROP_MODE_REPLACE,
        server->xcb_root,
        g_atom_ident_state,
        XCB_ATOM_STRING,
        8,
        (uint32_t)strlen(state_str),
        state_str
    );
    xcb_flush(server->xcb_conn);
}

static void destroy_ident_badge(DtwmIdentBadge *b) {
    if (!b) return;
    wl_list_remove(&b->link);
    wl_list_remove(&b->output_destroy.link);
    if (b->scene_buf) {
        wlr_scene_node_destroy(&b->scene_buf->node);
        b->scene_buf = nullptr;
    }
    free(b);
}

static void handle_ident_output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmIdentBadge *b = wl_container_of(listener, b, output_destroy);
    destroy_ident_badge(b);
}

static DtwmIdentBadge *find_or_create_ident_badge(DtwmServer *server, struct wlr_output *wo) {
    if (!server || !wo || !server->layer_overlay_tree) return nullptr;
    if (!g_ident_list_inited) {
        wl_list_init(&g_ident_badges);
        g_ident_list_inited = true;
    }
    DtwmIdentBadge *b;
    wl_list_for_each(b, &g_ident_badges, link) {
        if (b->wlr_output == wo) {
            return b;
        }
    }
    struct wlr_scene_buffer *sb = wlr_scene_buffer_create(server->layer_overlay_tree, nullptr);
    if (!sb) return nullptr;
    sb->point_accepts_input = ident_reject_input;
    wlr_scene_node_set_enabled(&sb->node, false);

    b = calloc(1, sizeof(*b));
    if (!b) {
        wlr_scene_node_destroy(&sb->node);
        return nullptr;
    }
    b->server = server;
    b->wlr_output = wo;
    b->scene_buf = sb;
    b->output_destroy.notify = handle_ident_output_destroy;
    wl_signal_add(&wo->events.destroy, &b->output_destroy);
    wl_list_insert(g_ident_badges.prev, &b->link);
    return b;
}

static struct wlr_buffer *render_ident_badge_buffer(
    double scale, int number, const char *out_name, bool selected
) {
    if (scale < 1.0) scale = 1.0;
    if (scale > 4.0) scale = 4.0;
    int phys_w = (int)lround((double)IDENT_BADGE_LOGICAL_SZ * scale);
    int phys_h = (int)lround((double)IDENT_BADGE_LOGICAL_SZ * scale);

    cairo_t *cr = nullptr;
    cairo_surface_t *surf = nullptr;
    struct wlr_buffer *buf = dtwm_cairo_buffer_begin(phys_w, phys_h, &cr, &surf);
    if (!buf || !cr) return nullptr;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_scale(cr, scale, scale);
    const double sz = (double)IDENT_BADGE_LOGICAL_SZ;

    /* Semi-transparent slate plate (~68% opacity) so windows/backdrop show through */
    cairo_set_source_rgba(cr, 0.09, 0.13, 0.17, 0.68);
    cairo_rectangle(cr, 0.0, 0.0, sz, sz);
    cairo_fill(cr);

    /* Semi-transparent 3D CDE mitered bevel */
    DtwmRgba tl = { 0.88, 0.92, 0.96, 0.56 };
    DtwmRgba br = { 0.03, 0.05, 0.07, 0.74 };
    dtwm_draw_bevel(cr, 0.0, 0.0, sz, sz, 2.5, tl, br);

    /* Highlight selected monitor with the gold inner rim matching Display.c's preview */
    if (selected) {
        cairo_set_source_rgba(cr, 0.98, 0.78, 0.26, 0.88);
        cairo_set_line_width(cr, 2.5);
        cairo_rectangle(cr, 4.25, 4.25, sz - 8.5, sz - 8.5);
        cairo_stroke(cr);
    }

    char num_str[16];
    snprintf(num_str, sizeof(num_str), "%d", number);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 78.0);
    cairo_text_extents_t ext = {0};
    cairo_text_extents(cr, num_str, &ext);
    double nx = (sz - ext.width) * 0.5 - ext.x_bearing;
    double ny = (out_name && out_name[0]) ? 86.0 : 94.0;

    /* Soft drop shadow + bright translucent numeral */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.62);
    cairo_move_to(cr, nx + 2.5, ny + 2.5);
    cairo_show_text(cr, num_str);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
    cairo_move_to(cr, nx, ny);
    cairo_show_text(cr, num_str);

    if (out_name && out_name[0]) {
        cairo_set_font_size(cr, 13.0);
        cairo_text_extents_t lext = {0};
        cairo_text_extents(cr, out_name, &lext);
        double lx = (sz - lext.width) * 0.5 - lext.x_bearing;
        if (lx < 8.0) lx = 8.0;
        double ly = sz - 14.0;

        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
        cairo_move_to(cr, lx + 1.0, ly + 1.0);
        cairo_show_text(cr, out_name);

        if (selected) {
            cairo_set_source_rgba(cr, 0.99, 0.85, 0.42, 0.94);
        } else {
            cairo_set_source_rgba(cr, 0.86, 0.92, 0.96, 0.88);
        }
        cairo_move_to(cr, lx, ly);
        cairo_show_text(cr, out_name);
    }

    dtwm_cairo_buffer_finish(cr, surf);
    return buf;
}

static bool check_display_dlg_win_viewable(DtwmServer *server, bool *out_has_prop) {
    *out_has_prop = false;
    if (!server) return false;
    if (!server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) {
        DtwmView *v;
        wl_list_for_each(v, &server->views, link) {
            if (!v->mapped || v->minimized || v->kind != DTWM_VIEW_XDG || !v->xdg_toplevel) {
                continue;
            }
            const char *title = v->xdg_toplevel->title ? v->xdg_toplevel->title : "";
            const char *app_id = v->xdg_toplevel->app_id ? v->xdg_toplevel->app_id : "";
            if (strstr(title, "Display & Monitors") != nullptr ||
                (strcasestr(app_id, "dtstyle") != nullptr &&
                 (strstr(title, "Monitors") != nullptr || strstr(title, "Display") != nullptr))) {
                return true;
            }
        }
        return false;
    }
    if (g_atom_dlg_win == XCB_ATOM_NONE) {
        g_atom_dlg_win = intern_xcb_atom(server->xcb_conn, "_DTSTYLE_DISPLAY_DLG_WIN");
    }
    if (g_atom_dlg_win == XCB_ATOM_NONE) return false;

    xcb_get_property_cookie_t pck = xcb_get_property(
        server->xcb_conn, 0, server->xcb_root, g_atom_dlg_win, XCB_ATOM_WINDOW, 0, 1
    );
    xcb_get_property_reply_t *prep = xcb_get_property_reply(server->xcb_conn, pck, nullptr);
    if (!prep) return false;

    xcb_window_t dlg_win = XCB_WINDOW_NONE;
    if (prep->format == 32 && xcb_get_property_value_length(prep) >= 4) {
        dlg_win = *(const xcb_window_t *)xcb_get_property_value(prep);
    }
    free(prep);
    if (dlg_win == XCB_WINDOW_NONE) return false;
    *out_has_prop = true;

    xcb_get_window_attributes_cookie_t ack = xcb_get_window_attributes(server->xcb_conn, dlg_win);
    xcb_get_window_attributes_reply_t *arep =
        xcb_get_window_attributes_reply(server->xcb_conn, ack, nullptr);
    if (!arep) return false;
    bool viewable = (arep->map_state == XCB_MAP_STATE_VIEWABLE);
    free(arep);
    return viewable;
}

static int ident_watchdog_cb(void *data) {
    DtwmServer *server = data;
    if (!g_ident_active || !server) return 0;

    bool has_prop = false;
    bool viewable = check_display_dlg_win_viewable(server, &has_prop);
    if (viewable) {
        g_ident_saw_dlg_win = true;
        g_ident_unseen_ticks = 0;
    } else {
        g_ident_unseen_ticks++;
        if (g_ident_saw_dlg_win || has_prop || g_ident_unseen_ticks >= 6) {
            clear_ident_overlays(server);
            return 0;
        }
    }

    if (g_ident_watchdog) {
        wl_event_source_timer_update(g_ident_watchdog, 800);
    }
    return 0;
}

static void clear_ident_overlays(DtwmServer *server) {
    g_ident_active = false;
    g_ident_saw_dlg_win = false;
    g_ident_unseen_ticks = 0;
    g_ident_rule_count = 0;
    if (g_ident_watchdog) {
        wl_event_source_remove(g_ident_watchdog);
        g_ident_watchdog = nullptr;
    }
    if (g_ident_list_inited) {
        DtwmIdentBadge *b, *tmp;
        wl_list_for_each_safe(b, tmp, &g_ident_badges, link) {
            destroy_ident_badge(b);
        }
    }
    publish_ident_x11_state(server, "active=0");
}

static int lookup_ident_number_for_output(const char *name, int fallback_num) {
    if (name && name[0]) {
        for (int i = 0; i < g_ident_rule_count; ++i) {
            if (strcmp(g_ident_rules[i].name, name) == 0) {
                return g_ident_rules[i].number;
            }
        }
    }
    return fallback_num;
}

static void sync_ident_overlays(DtwmServer *server) {
    if (!server) return;
    if (!g_ident_active) {
        clear_ident_overlays(server);
        return;
    }

    char state_buf[1024];
    int sp = snprintf(state_buf, sizeof(state_buf), "active=1 selected=%d", g_ident_selected_num);
    int shown = 0;
    int seq_idx = 0;

    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        seq_idx++;
        struct wlr_output *wo = out->wlr_output;
        if (!wo) continue;
        DtwmIdentBadge *badge = find_or_create_ident_badge(server, wo);
        if (!badge || !badge->scene_buf) continue;

        if (!wo->enabled) {
            wlr_scene_node_set_enabled(&badge->scene_buf->node, false);
            wlr_scene_buffer_set_buffer(badge->scene_buf, nullptr);
            continue;
        }

        struct wlr_box obox = {0};
        wlr_output_layout_get_box(server->output_layout, wo, &obox);
        if (obox.width <= 0 || obox.height <= 0) {
            wlr_scene_node_set_enabled(&badge->scene_buf->node, false);
            continue;
        }

        const char *oname = wo->name ? wo->name : "OUT";
        int num = lookup_ident_number_for_output(oname, seq_idx);
        bool sel = (num == g_ident_selected_num);
        double scale = (out->dpi > 0) ? ((double)out->dpi / 96.0) : 1.0;
        if (scale < 1.0) scale = 1.0;
        if (scale > 4.0) scale = 4.0;
        int phys_sz = (int)lround((double)IDENT_BADGE_LOGICAL_SZ * scale);
        int phys_margin = (int)lround((double)IDENT_BADGE_MARGIN * scale);

        struct wlr_buffer *buf = render_ident_badge_buffer(scale, num, oname, sel);
        if (!buf) continue;

        int bx = obox.x + obox.width - phys_sz - phys_margin;
        int by = obox.y + phys_margin;
        if (bx < obox.x + 4) bx = obox.x + 4;
        if (by + phys_sz > obox.y + obox.height - 4) {
            by = obox.y + 4;
        }

        wlr_scene_buffer_set_buffer(badge->scene_buf, buf);
        wlr_buffer_drop(buf);
        wlr_scene_buffer_set_dest_size(badge->scene_buf, phys_sz, phys_sz);
        wlr_scene_node_set_position(&badge->scene_buf->node, bx, by);
        wlr_scene_node_raise_to_top(&badge->scene_buf->node);
        wlr_scene_node_set_enabled(&badge->scene_buf->node, true);
        shown++;

        if (sp > 0 && sp + 96 < (int)sizeof(state_buf)) {
            sp += snprintf(
                state_buf + sp, sizeof(state_buf) - (size_t)sp,
                " %s=%d@%d,%d%s",
                oname, num, bx, by, sel ? ":selected" : ""
            );
        }
    }

    if (sp > 0 && sp + 32 < (int)sizeof(state_buf)) {
        snprintf(state_buf + sp, sizeof(state_buf) - (size_t)sp, " count=%d", shown);
    }
    publish_ident_x11_state(server, state_buf);

    bool has_prop = false;
    if (check_display_dlg_win_viewable(server, &has_prop)) {
        g_ident_saw_dlg_win = true;
    }
    if (!g_ident_watchdog && server->wl_display) {
        struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
        if (loop) {
            g_ident_watchdog = wl_event_loop_add_timer(loop, ident_watchdog_cb, server);
            if (g_ident_watchdog) {
                wl_event_source_timer_update(g_ident_watchdog, 800);
            }
        }
    }
}

bool dtwm_output_mgmt_handle_wm_request(DtwmServer *server, const char *cmd) {
    if (!server || !cmd) return false;
    if (strncmp(cmd, "f.identify_outputs", 18) == 0 &&
        (cmd[18] == '\0' || isspace((unsigned char)cmd[18]))) {
        const char *p = cmd + 18;
        while (*p && isspace((unsigned char)*p)) p++;
        int enable = (*p) ? atoi(p) : 0;
        if (enable <= 0) {
            clear_ident_overlays(server);
            return true;
        }

        char copy[512];
        snprintf(copy, sizeof(copy), "%s", p);
        char *savep = nullptr;
        char *tok = strtok_r(copy, " \t\r\n", &savep); /* enable */
        tok = strtok_r(nullptr, " \t\r\n", &savep);    /* selected_1based */
        g_ident_selected_num = (tok && atoi(tok) > 0) ? atoi(tok) : 1;
        g_ident_rule_count = 0;

        while ((tok = strtok_r(nullptr, " \t\r\n", &savep)) != nullptr &&
               g_ident_rule_count < MAX_IDENT_RULES) {
            char *eq = strchr(tok, '=');
            if (!eq || eq == tok) continue;
            *eq = '\0';
            int num = atoi(eq + 1);
            if (num <= 0) continue;
            snprintf(
                g_ident_rules[g_ident_rule_count].name,
                sizeof(g_ident_rules[g_ident_rule_count].name),
                "%s", tok
            );
            g_ident_rules[g_ident_rule_count].number = num;
            g_ident_rule_count++;
        }

        g_ident_active = true;
        g_ident_unseen_ticks = 0;
        sync_ident_overlays(server);
        return true;
    }
    if (strncmp(cmd, "f.set_output_layout", 19) == 0 && isspace((unsigned char)cmd[19])) {
        char name[64] = {0};
        int enabled = 1, x = 0, y = 0, w = 1280, h = 900, rot = 0, dpi = 96;
        int parsed = sscanf(
            cmd + 20, "%63s %d %d %d %dx%d %d %d",
            name, &enabled, &x, &y, &w, &h, &rot, &dpi
        );
        if (parsed < 6) return true;

        DtwmOutput *dout = find_dtwm_output_by_name(server, name);
        if (!dout || !dout->wlr_output) return true;

        /* Prevent disabling the last active output */
        if (!enabled) {
            int other_enabled = 0;
            DtwmOutput *it;
            wl_list_for_each(it, &server->outputs, link) {
                if (it != dout && it->wlr_output && it->wlr_output->enabled) {
                    other_enabled++;
                }
            }
            if (other_enabled == 0) {
                return true;
            }
        }

        struct wlr_output *wo = dout->wlr_output;
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_enabled(&st, enabled != 0);

        if (enabled) {
            if (w < 320) w = 320;
            if (h < 240) h = 240;
            if (w > 8192) w = 8192;
            if (h > 8192) h = 8192;

            struct wlr_output_mode *matched_mode = nullptr;
            struct wlr_output_mode *m;
            wl_list_for_each(m, &wo->modes, link) {
                if (m->width == w && m->height == h) {
                    matched_mode = m;
                    break;
                }
            }
            if (matched_mode) {
                wlr_output_state_set_mode(&st, matched_mode);
            } else {
                wlr_output_state_set_custom_mode(&st, w, h, 60000);
            }
            wlr_output_state_set_transform(&st, degrees_to_transform(rot));
            wlr_output_state_set_scale(&st, 1.0f);
        }

        if (wlr_output_commit_state(wo, &st)) {
            if (enabled) {
                wlr_output_layout_add(server->output_layout, wo, x, y);
                if (dpi >= 72 && dpi <= 384) {
                    dout->dpi = dpi;
                    if (wo->name && wo->name[0]) {
                        dtwm_power_display_record_dpi(server, wo->name, dpi);
                    }
                }
            } else {
                wlr_output_layout_remove(server->output_layout, wo);
            }
            after_outputs_reconfigured(server);
        }
        wlr_output_state_finish(&st);
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * zwlr_foreign_toplevel_manager_v1 Integration
 * ------------------------------------------------------------------------- */
static bool is_foreign_eligible(const DtwmView *v) {
    if (!v || !v->mapped) return false;
    if (v->is_panel || v->is_subpanel || v->is_desktop_pin || v->is_swallowed || v->is_lock_dialog) {
        return false;
    }
    if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface && v->xwayland_surface->override_redirect) {
        return false;
    }
    return true;
}

static const char *view_app_id(const DtwmView *v) {
    if (!v) return "cde-app";
    if (v->kind == DTWM_VIEW_XDG && v->xdg_toplevel && v->xdg_toplevel->app_id && v->xdg_toplevel->app_id[0]) {
        return v->xdg_toplevel->app_id;
    }
    if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface &&
        v->xwayland_surface->class && v->xwayland_surface->class[0]) {
        return v->xwayland_surface->class;
    }
    return "cde-app";
}

static void ft_handle_req_maximize(struct wl_listener *listener, void *data) {
    DtwmForeignToplevel *ft = wl_container_of(listener, ft, request_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *ev = data;
    if (ft->view && ev) {
        dtwm_view_set_maximized(ft->view, ev->maximized);
    }
}

static void ft_handle_req_minimize(struct wl_listener *listener, void *data) {
    DtwmForeignToplevel *ft = wl_container_of(listener, ft, request_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *ev = data;
    if (ft->view && ev) {
        dtwm_view_set_minimized(ft->view, ev->minimized);
    }
}

static void ft_handle_req_activate(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmForeignToplevel *ft = wl_container_of(listener, ft, request_activate);
    if (ft->view && ft->view->mapped && !ft->view->server->session_locked) {
        if (ft->view->minimized) {
            dtwm_view_set_minimized(ft->view, false);
        }
        dtwm_restore_view_focus(ft->view->server, ft->view);
    }
}

static void ft_handle_req_close(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmForeignToplevel *ft = wl_container_of(listener, ft, request_close);
    if (ft->view && !ft->view->server->session_locked) {
        dtwm_view_close(ft->view);
    }
}

static void ft_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmForeignToplevel *ft = wl_container_of(listener, ft, destroy);
    wl_list_remove(&ft->request_maximize.link);
    wl_list_remove(&ft->request_minimize.link);
    wl_list_remove(&ft->request_activate.link);
    wl_list_remove(&ft->request_close.link);
    wl_list_remove(&ft->destroy.link);
    if (ft->view && ft->view->foreign_toplevel == ft) {
        ft->view->foreign_toplevel = nullptr;
    }
    free(ft);
}

void dtwm_foreign_toplevel_sync(DtwmView *view) {
    if (!view || !view->server || !view->server->foreign_toplevel_mgr) return;

    if (!is_foreign_eligible(view)) {
        dtwm_foreign_toplevel_destroy(view);
        return;
    }

    DtwmForeignToplevel *ft = view->foreign_toplevel;
    if (!ft) {
        struct wlr_foreign_toplevel_handle_v1 *h =
            wlr_foreign_toplevel_handle_v1_create(view->server->foreign_toplevel_mgr);
        if (!h) return;
        ft = calloc(1, sizeof(*ft));
        ft->view = view;
        ft->handle = h;
        view->foreign_toplevel = ft;

        ft->request_maximize.notify = ft_handle_req_maximize;
        wl_signal_add(&h->events.request_maximize, &ft->request_maximize);
        ft->request_minimize.notify = ft_handle_req_minimize;
        wl_signal_add(&h->events.request_minimize, &ft->request_minimize);
        ft->request_activate.notify = ft_handle_req_activate;
        wl_signal_add(&h->events.request_activate, &ft->request_activate);
        ft->request_close.notify = ft_handle_req_close;
        wl_signal_add(&h->events.request_close, &ft->request_close);
        ft->destroy.notify = ft_handle_destroy;
        wl_signal_add(&h->events.destroy, &ft->destroy);

        if (!wl_list_empty(&view->server->outputs)) {
            DtwmOutput *first = wl_container_of(view->server->outputs.next, first, link);
            if (first->wlr_output) {
                wlr_foreign_toplevel_handle_v1_output_enter(h, first->wlr_output);
            }
        }
    }

    const char *title = dtwm_view_title(view);
    wlr_foreign_toplevel_handle_v1_set_title(ft->handle, title ? title : "");
    wlr_foreign_toplevel_handle_v1_set_app_id(ft->handle, view_app_id(view));
    wlr_foreign_toplevel_handle_v1_set_maximized(ft->handle, view->maximized);
    wlr_foreign_toplevel_handle_v1_set_minimized(ft->handle, view->minimized);
    wlr_foreign_toplevel_handle_v1_set_activated(ft->handle, view->active);
    dtwm_publish_client_ids(view->server);
}

void dtwm_foreign_toplevel_destroy(DtwmView *view) {
    if (!view) return;
    DtwmServer *server = view->server;
    if (!view->foreign_toplevel) {
        dtwm_publish_client_ids(server);
        return;
    }
    DtwmForeignToplevel *ft = view->foreign_toplevel;
    view->foreign_toplevel = nullptr;
    ft->view = nullptr;
    if (ft->handle) {
        wlr_foreign_toplevel_handle_v1_destroy(ft->handle);
    }
    dtwm_publish_client_ids(server);
}

void dtwm_output_mgmt_init(DtwmServer *server) {
    if (!server || !server->wl_display) return;

    server->output_mgr = wlr_output_manager_v1_create(server->wl_display);
    if (server->output_mgr) {
        server->output_mgr_apply.notify = handle_output_mgr_apply;
        wl_signal_add(&server->output_mgr->events.apply, &server->output_mgr_apply);
        server->output_mgr_test.notify = handle_output_mgr_test;
        wl_signal_add(&server->output_mgr->events.test, &server->output_mgr_test);
    }

    server->output_power_mgr = wlr_output_power_manager_v1_create(server->wl_display);
    if (server->output_power_mgr) {
        server->output_power_set_mode.notify = handle_output_power_set_mode;
        wl_signal_add(&server->output_power_mgr->events.set_mode, &server->output_power_set_mode);
    }

    server->export_dmabuf_mgr = wlr_export_dmabuf_manager_v1_create(server->wl_display);
    wlr_ext_image_copy_capture_manager_v1_create(server->wl_display, 1);
    wlr_ext_output_image_capture_source_manager_v1_create(server->wl_display, 1);
    server->foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(server->wl_display);
}

void dtwm_output_mgmt_finish(DtwmServer *server) {
    if (!server) return;
    clear_ident_overlays(server);
    if (server->output_mgr) {
        wl_list_remove(&server->output_mgr_apply.link);
        wl_list_remove(&server->output_mgr_test.link);
        server->output_mgr = nullptr;
    }
    if (server->output_power_mgr) {
        wl_list_remove(&server->output_power_set_mode.link);
        server->output_power_mgr = nullptr;
    }
}
