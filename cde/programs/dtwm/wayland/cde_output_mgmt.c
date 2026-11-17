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
    dtwm_update_all_backdrops(server);
    dtwm_update_front_panel(server);
    dtwm_icons_relayout(server);
    dtwm_reflow_views_for_outputs(server);
    dtwm_sync_xwayland_randr_dpi(server);
    dtwm_lock_sync_outputs(server);
    dtwm_output_mgmt_sync(server);
}

void dtwm_output_mgmt_publish_x11(DtwmServer *server) {
    if (!server || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) return;
    if (server->atom_wayland_outputs == XCB_ATOM_NONE) {
        server->atom_wayland_outputs = intern_xcb_atom(server->xcb_conn, "_DT_WAYLAND_OUTPUTS");
    }
    if (server->atom_wayland_outputs == XCB_ATOM_NONE) return;

    char buf[4096];
    int pos = 0;
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
            float req_scale = (head->state.scale >= 0.5f && head->state.scale <= 4.0f)
                                  ? head->state.scale
                                  : 1.0f;
            wlr_output_state_set_scale(&st, req_scale);
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

bool dtwm_output_mgmt_handle_wm_request(DtwmServer *server, const char *cmd) {
    if (!server || !cmd) return false;
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
            float req_scale = (dpi >= 72 && dpi <= 384) ? ((float)dpi / 96.0f) : 1.0f;
            if (req_scale < 0.5f) req_scale = 0.5f;
            if (req_scale > 4.0f) req_scale = 4.0f;
            wlr_output_state_set_scale(&st, req_scale);
        }

        if (wlr_output_commit_state(wo, &st)) {
            if (enabled) {
                wlr_output_layout_add(server->output_layout, wo, x, y);
                if (dpi >= 72 && dpi <= 384) {
                    dout->dpi = dpi;
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
}

void dtwm_foreign_toplevel_destroy(DtwmView *view) {
    if (!view || !view->foreign_toplevel) return;
    DtwmForeignToplevel *ft = view->foreign_toplevel;
    view->foreign_toplevel = nullptr;
    ft->view = nullptr;
    if (ft->handle) {
        wlr_foreign_toplevel_handle_v1_destroy(ft->handle);
    }
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
