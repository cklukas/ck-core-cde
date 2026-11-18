/*
 * CDE - Common Desktop Environment
 * dtwm_wayland_main.c: wlroots-0.19 compositor core with server-side CDE 3D
 * bevel decorations, rootless XWayland, window menu, snap preview, Shift-grid
 * alignment, and keyboard task switcher.
 */

#include "dtwm_wayland.h"

#include <getopt.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wlr/util/log.h>
#include <xcb/xcb_icccm.h>
#include <xkbcommon/xkbcommon.h>

typedef struct {
    DtwmServer *server;
    struct wlr_xdg_popup *xdg_popup;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener commit;
    struct wl_listener destroy;
} DtwmPopup;

typedef struct {
    DtwmServer *server;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener request_mode;
    struct wl_listener surface_commit;
    struct wl_listener destroy;
} DtwmDecoration;

typedef struct {
    DtwmServer *server;
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_layer_surface_v1 *scene_layer;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
} DtwmLayerSurface;

static void close_window_menu(DtwmServer *server);
static void hide_task_switcher(DtwmServer *server, bool activate_selected);
static void hide_wireframe_outline(DtwmServer *server);
static void focus_view_ex(DtwmServer *server, DtwmView *view, struct wlr_surface *surface, bool raise_window);
static void focus_view(DtwmServer *server, DtwmView *view, struct wlr_surface *surface);
static void view_set_maximized(DtwmView *view, bool maximized);
static void view_set_minimized(DtwmView *view, bool minimized);
static void view_close(DtwmView *view);
static void view_apply_geometry(DtwmView *view, int x, int y, int client_w, int client_h);

static uint32_t dtwm_now_msec(void) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t ms = (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L));
    return ms ? ms : 1u;
}

static int align_to_grid(int v) {
    int rem = v % DTWM_GRID_SIZE;
    if (rem < 0) {
        rem += DTWM_GRID_SIZE;
    }
    return (rem >= DTWM_GRID_SIZE / 2) ? (v + (DTWM_GRID_SIZE - rem)) : (v - rem);
}

static bool shift_is_held(DtwmServer *server) {
    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (!kbd) {
        return false;
    }
    return (wlr_keyboard_get_modifiers(kbd) & WLR_MODIFIER_SHIFT) != 0;
}

static struct wlr_surface *view_wlr_surface(const DtwmView *view) {
    if (!view) {
        return nullptr;
    }
    if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel && view->xdg_toplevel->base) {
        return view->xdg_toplevel->base->surface;
    }
    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        return view->xwayland_surface->surface;
    }
    return nullptr;
}

static void get_primary_output_box(DtwmServer *server, double ref_x, double ref_y, struct wlr_box *out_box) {
    struct wlr_output *out = wlr_output_layout_output_at(server->output_layout, ref_x, ref_y);
    if (!out && !wl_list_empty(&server->outputs)) {
        DtwmOutput *first = wl_container_of(server->outputs.next, first, link);
        out = first->wlr_output;
    }
    if (out) {
        wlr_output_layout_get_box(server->output_layout, out, out_box);
    } else {
        *out_box = (struct wlr_box){.x = 0, .y = 0, .width = 1280, .height = 720};
    }
}

static void sync_xwayland_client_pos(DtwmView *view) {
    if (view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) {
        return;
    }
    if (view->xwayland_surface->override_redirect) {
        return;
    }
    int l, t, r, b;
    dtwm_view_insets(view, &l, &t, &r, &b);
    uint16_t cw = (uint16_t)(view->client_w > 1 ? view->client_w : 1);
    uint16_t ch = (uint16_t)(view->client_h > 1 ? view->client_h : 1);
    wlr_xwayland_surface_configure(
        view->xwayland_surface,
        (int16_t)(view->x + l),
        (int16_t)(view->y + t),
        cw,
        ch
    );
}

static void view_apply_geometry(DtwmView *view, int x, int y, int client_w, int client_h) {
    if (client_w < 64) client_w = 64;
    if (client_h < 48) client_h = 48;
    view->x = x;
    view->y = y;
    view->client_w = client_w;
    view->client_h = client_h;

    if (view->frame_tree) {
        wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
    }
    if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, client_w, client_h);
    } else if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        sync_xwayland_client_pos(view);
    }
    dtwm_view_update_frame(view);
    if (view == view->server->focused_view) {
        dtwm_publish_active_window_box(view->server);
        dtwm_ime_set_focus(view->server, view_wlr_surface(view));
    }
}

static void clear_active_focus(DtwmServer *server) {
    if (server->focused_view) {
        DtwmView *prev = server->focused_view;
        server->focused_view = nullptr;
        prev->active = false;
        if (prev->kind == DTWM_VIEW_XDG && prev->xdg_toplevel && prev->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
        } else if (prev->kind == DTWM_VIEW_XWAYLAND && prev->xwayland_surface) {
            wlr_xwayland_surface_activate(prev->xwayland_surface, false);
        }
        dtwm_view_update_frame(prev);
        dtwm_icons_update_view(prev);
        dtwm_foreign_toplevel_sync(prev);
    }
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    dtwm_ime_set_focus(server, nullptr);
    dtwm_publish_active_window_box(server);
}

static void raise_transient_children_above(DtwmServer *server, DtwmView *parent) {
    if (!server || !parent || server->allow_primary_on_top) {
        return;
    }
    DtwmView *cand;
    wl_list_for_each(cand, &server->views, link) {
        if (cand == parent || !cand->mapped || !dtwm_view_visible_on_current_ws(cand)) {
            continue;
        }
        bool is_child = false;
        if (parent->kind == DTWM_VIEW_XWAYLAND && cand->kind == DTWM_VIEW_XWAYLAND &&
            parent->xwayland_surface && cand->xwayland_surface &&
            cand->xwayland_surface->parent == parent->xwayland_surface) {
            is_child = true;
        } else if (parent->kind == DTWM_VIEW_XDG && cand->kind == DTWM_VIEW_XDG &&
                   parent->xdg_toplevel && cand->xdg_toplevel &&
                   cand->xdg_toplevel->parent == parent->xdg_toplevel) {
            is_child = true;
        }
        if (is_child && cand->frame_tree) {
            wlr_scene_node_raise_to_top(&cand->frame_tree->node);
            if (cand->kind == DTWM_VIEW_XWAYLAND && cand->xwayland_surface) {
                wlr_xwayland_surface_restack(cand->xwayland_surface, nullptr, XCB_STACK_MODE_ABOVE);
            }
        }
    }
}

static void focus_view_ex(
    DtwmServer *server, DtwmView *view, struct wlr_surface *surface, bool raise_window
) {
    if (server->session_locked && (!view || !view->is_lock_dialog)) {
        return;
    }
    if (!view) {
        clear_active_focus(server);
        return;
    }
    if (!surface) {
        surface = view_wlr_surface(view);
    }
    if (!surface) {
        return;
    }
    if (view->minimized) {
        view_set_minimized(view, false);
    }
    if (view->is_swallowed) {
        return;
    }
    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface &&
        view->xwayland_surface->override_redirect) {
        return;
    }

    DtwmView *prev = server->focused_view;
    if (prev && prev != view) {
        prev->active = false;
        if (prev->kind == DTWM_VIEW_XDG && prev->xdg_toplevel && prev->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
        } else if (prev->kind == DTWM_VIEW_XWAYLAND && prev->xwayland_surface) {
            wlr_xwayland_surface_activate(prev->xwayland_surface, false);
        }
        dtwm_view_update_frame(prev);
        dtwm_icons_update_view(prev);
        dtwm_foreign_toplevel_sync(prev);
    }

    /* Raise in Z-order when requested and move to head of server->views MRU list */
    if (raise_window && view->frame_tree && !view->is_desktop_pin) {
        wlr_scene_node_raise_to_top(&view->frame_tree->node);
    }
    if (!view->is_desktop_pin) {
        wl_list_remove(&view->link);
        wl_list_insert(&server->views, &view->link);
    }

    server->focused_view = view;
    view->active = true;
    if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel && view->xdg_toplevel->base->initialized) {
        wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
    } else if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        wlr_xwayland_surface_activate(view->xwayland_surface, true);
        if (raise_window && !view->is_desktop_pin) {
            wlr_xwayland_surface_restack(view->xwayland_surface, nullptr, XCB_STACK_MODE_ABOVE);
        }
        if (server->greeter_mode && server->xcb_conn) {
            xcb_set_input_focus(
                server->xcb_conn,
                XCB_INPUT_FOCUS_POINTER_ROOT,
                view->xwayland_surface->window_id,
                XCB_CURRENT_TIME
            );
            xcb_flush(server->xcb_conn);
        }
    }
    if (raise_window && !view->is_desktop_pin) {
        raise_transient_children_above(server, view);
    }
    dtwm_view_update_frame(view);
    dtwm_icons_update_view(view);
    dtwm_foreign_toplevel_sync(view);
    dtwm_publish_active_window_box(server);

    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (kbd) {
        wlr_seat_keyboard_notify_enter(
            server->seat, surface, kbd->keycodes, kbd->num_keycodes, &kbd->modifiers
        );
    } else {
        wlr_seat_keyboard_notify_enter(server->seat, surface, nullptr, 0, nullptr);
    }
    dtwm_ime_set_focus(server, surface);
}

static void focus_view(DtwmServer *server, DtwmView *view, struct wlr_surface *surface) {
    focus_view_ex(server, view, surface, true);
}

static void view_set_maximized(DtwmView *view, bool maximized) {
    if (view->maximized == maximized) {
        return;
    }
    if (maximized) {
        view->saved_x = view->x;
        view->saved_y = view->y;
        view->saved_w = view->client_w;
        view->saved_h = view->client_h;
        view->has_saved_geom = true;

        struct wlr_box obox;
        dtwm_get_usable_output_box(view->server, view->x + 32, view->y + 32, &obox);
        view->maximized = true;
        int l, t, r, b;
        dtwm_view_insets(view, &l, &t, &r, &b);
        int cw = obox.width - l - r;
        int ch = obox.height - t - b;
        if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel) {
            wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, true);
        } else if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
            wlr_xwayland_surface_set_maximized(view->xwayland_surface, true, true);
        }
        view_apply_geometry(view, obox.x, obox.y, cw, ch);
    } else {
        view->maximized = false;
        if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel) {
            wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
        } else if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
            wlr_xwayland_surface_set_maximized(view->xwayland_surface, false, false);
        }
        if (view->has_saved_geom) {
            view_apply_geometry(view, view->saved_x, view->saved_y, view->saved_w, view->saved_h);
        } else {
            dtwm_view_update_frame(view);
        }
    }
    dtwm_foreign_toplevel_sync(view);
}

static int handle_anim_timer(void *data) {
    DtwmServer *server = data;
    if (!server || !server->anim_buf) {
        return 0;
    }
    const int total_steps = 4;
    if (server->anim_step <= 0 || server->anim_step > total_steps) {
        wlr_scene_buffer_set_buffer(server->anim_buf, nullptr);
        wlr_scene_node_set_enabled(&server->anim_buf->node, false);
        server->anim_step = 0;
        return 0;
    }
    double t = (double)server->anim_step / (double)(total_steps + 1);
    int x = (int)(server->anim_from_box.x + (server->anim_to_box.x - server->anim_from_box.x) * t);
    int y = (int)(server->anim_from_box.y + (server->anim_to_box.y - server->anim_from_box.y) * t);
    int w = (int)(server->anim_from_box.width + (server->anim_to_box.width - server->anim_from_box.width) * t);
    int h = (int)(server->anim_from_box.height + (server->anim_to_box.height - server->anim_from_box.height) * t);
    struct wlr_buffer *buf = dtwm_render_wireframe_outline(w, h);
    if (buf) {
        wlr_scene_buffer_set_buffer(server->anim_buf, buf);
        wlr_buffer_drop(buf);
        wlr_scene_node_set_position(&server->anim_buf->node, x, y);
        wlr_scene_node_set_enabled(&server->anim_buf->node, true);
    }
    server->anim_step++;
    if (server->anim_timer) {
        wl_event_source_timer_update(server->anim_timer, 18);
    }
    return 0;
}

static void start_minimize_restore_anim(
    DtwmServer *server, struct wlr_box from_box, struct wlr_box to_box
) {
    if (!server || !server->animate_minimize || server->greeter_mode || !server->anim_timer ||
        !server->anim_buf) {
        return;
    }
    server->anim_from_box = from_box;
    server->anim_to_box = to_box;
    server->anim_step = 1;
    handle_anim_timer(server);
}

static void view_set_minimized(DtwmView *view, bool minimized) {
    if (view->minimized == minimized) {
        return;
    }
    DtwmServer *server = view->server;
    int ow = 0, oh = 0;
    dtwm_view_outer_size(view, &ow, &oh);
    struct wlr_box win_box = {.x = view->x, .y = view->y, .width = ow, .height = oh};
    struct wlr_box pre_icon_box = {
        .x = view->icon_x,
        .y = view->icon_y,
        .width = view->icon_box_w > 0 ? view->icon_box_w : 56,
        .height = view->icon_box_h > 0 ? view->icon_box_h : 56,
    };

    view->minimized = minimized;
    if (view->frame_tree) {
        wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
    }
    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        wlr_xwayland_surface_set_minimized(view->xwayland_surface, minimized);
        dtwm_icons_set_wm_state(view);
    }
    dtwm_icons_relayout(server);
    dtwm_foreign_toplevel_sync(view);

    if (view->mapped && server->animate_minimize) {
        struct wlr_box icon_box = minimized
            ? (struct wlr_box){
                  .x = view->icon_x,
                  .y = view->icon_y,
                  .width = view->icon_box_w > 0 ? view->icon_box_w : 56,
                  .height = view->icon_box_h > 0 ? view->icon_box_h : 56,
              }
            : pre_icon_box;
        if (icon_box.x > 0 || icon_box.y > 0) {
            start_minimize_restore_anim(
                server, minimized ? win_box : icon_box, minimized ? icon_box : win_box
            );
        }
    }

    if (minimized && server->focused_view == view) {
        clear_active_focus(server);
        /* Focus next visible view on current workspace if any */
        DtwmView *cand;
        wl_list_for_each(cand, &server->views, link) {
            if (cand != view && dtwm_view_visible_on_current_ws(cand) &&
                !cand->is_swallowed && !cand->is_desktop_pin) {
                focus_view(server, cand, nullptr);
                break;
            }
        }
    }
}

static void view_close(DtwmView *view) {
    if (!view) {
        return;
    }
    if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel) {
        wlr_xdg_toplevel_send_close(view->xdg_toplevel);
    } else if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        wlr_xwayland_surface_close(view->xwayland_surface);
    }
}

void dtwm_view_set_maximized(DtwmView *view, bool maximized) {
    if (view) view_set_maximized(view, maximized);
}

void dtwm_view_set_minimized(DtwmView *view, bool minimized) {
    if (view) view_set_minimized(view, minimized);
}

void dtwm_view_close(DtwmView *view) {
    view_close(view);
}

static float server_max_output_scale(DtwmServer *server) {
    float max_scale = 1.0f;
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->wlr_output && out->wlr_output->enabled && out->wlr_output->scale > max_scale) {
            max_scale = out->wlr_output->scale;
        }
    }
    return max_scale;
}

void dtwm_reload_cursor_theme(DtwmServer *server, const char *theme, int size) {
    if (!server) return;
    if (size < 12) size = 24;
    if (size > 128) size = 128;
    const char *eff_theme = (theme && theme[0] && strcmp(theme, "default") != 0) ? theme : "";
    if (server->cursor_mgr && server->cursor_size == size &&
        strcmp(server->cursor_theme, eff_theme) == 0) {
        wlr_xcursor_manager_load(server->cursor_mgr, ceilf(server_max_output_scale(server)));
        return;
    }
    snprintf(server->cursor_theme, sizeof(server->cursor_theme), "%s", eff_theme);
    server->cursor_size = size;
    struct wlr_xcursor_manager *old_mgr = server->cursor_mgr;
    server->cursor_mgr = wlr_xcursor_manager_create(
        eff_theme[0] ? eff_theme : nullptr, (uint32_t)size
    );
    if (server->cursor_mgr) {
        wlr_xcursor_manager_load(server->cursor_mgr, ceilf(server_max_output_scale(server)));
        if (server->cursor) {
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        }
    }
    if (old_mgr) {
        wlr_xcursor_manager_destroy(old_mgr);
    }
}

void dtwm_reflow_views_for_outputs(DtwmServer *server) {
    if (!server) return;

    float max_scale = server_max_output_scale(server);
    if (server->cursor_mgr) {
        wlr_xcursor_manager_load(server->cursor_mgr, ceilf(max_scale));
    }

    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (!v->mapped || v->is_panel || v->is_subpanel || v->is_desktop_pin ||
            v->is_swallowed || v->is_lock_dialog) {
            continue;
        }
        if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface && v->xwayland_surface->override_redirect) {
            continue;
        }
        int outer_w = 0, outer_h = 0, l = 0, t = 0, r = 0, b = 0;
        dtwm_view_outer_size(v, &outer_w, &outer_h);
        dtwm_view_insets(v, &l, &t, &r, &b);
        struct wlr_box ubox;
        dtwm_get_usable_output_box(server, (double)v->x + outer_w * 0.5, (double)v->y + outer_h * 0.5, &ubox);
        if (ubox.width <= 0 || ubox.height <= 0) continue;

        if (v->maximized) {
            int cw = ubox.width - l - r;
            int ch = ubox.height - t - b;
            if (cw < 64) cw = 64;
            if (ch < 64) ch = 64;
            view_apply_geometry(v, ubox.x, ubox.y, cw, ch);
            continue;
        }

        int nx = v->x;
        int ny = v->y;
        if (nx + outer_w > ubox.x + ubox.width) {
            nx = ubox.x + ubox.width - outer_w;
        }
        if (nx < ubox.x) {
            nx = ubox.x;
        }
        if (ny + outer_h > ubox.y + ubox.height) {
            ny = ubox.y + ubox.height - outer_h;
        }
        if (ny < ubox.y) {
            ny = ubox.y;
        }
        if (nx != v->x || ny != v->y) {
            view_apply_geometry(v, nx, ny, v->client_w, v->client_h);
        }
    }
    dtwm_publish_active_window_box(server);
}

/* --- CDE Window Menu & Desktop Workspace Menu Overlay --- */
static void clamp_menu_to_output(DtwmServer *server, int mw, int mh) {
    struct wlr_box obox = {0};
    get_primary_output_box(server, server->menu_x, server->menu_y, &obox);
    if (obox.width > 0 && obox.height > 0) {
        if (server->menu_x + mw > obox.x + obox.width - 2) {
            server->menu_x = obox.x + obox.width - mw - 2;
        }
        if (server->menu_x < obox.x + 2) {
            server->menu_x = obox.x + 2;
        }
        if (server->menu_y + mh > obox.y + obox.height - 2) {
            server->menu_y = obox.y + obox.height - mh - 2;
        }
        if (server->menu_y < obox.y + 2) {
            server->menu_y = obox.y + 2;
        }
    }
}

static void update_window_menu_buffer(DtwmServer *server) {
    if (!server->menu_visible || !server->menu_buf) {
        return;
    }
    int mw = 0, mh = 0;
    struct wlr_buffer *buf = nullptr;
    if (server->menu_is_root) {
        bool has_panel = (server->panel_view != nullptr && server->panel_view->mapped);
        bool panel_min = (has_panel && server->panel_view->minimized);
        buf = dtwm_render_root_menu(
            &server->palette, panel_min, has_panel, server->menu_hover_idx, &mw, &mh
        );
    } else {
        buf = dtwm_render_window_menu(
            &server->palette, server->menu_target, server->menu_hover_idx, &mw, &mh
        );
    }
    server->menu_w = mw;
    server->menu_h = mh;
    clamp_menu_to_output(server, mw, mh);
    if (buf) {
        wlr_scene_buffer_set_buffer(server->menu_buf, buf);
        wlr_buffer_drop(buf);
    }
    wlr_scene_node_set_position(&server->menu_buf->node, server->menu_x, server->menu_y);
    wlr_scene_node_set_enabled(&server->menu_buf->node, true);
}

static void open_window_menu(DtwmServer *server, DtwmView *view, int x, int y) {
    server->menu_visible = true;
    server->menu_is_root = false;
    server->menu_target = view;
    server->menu_x = x;
    server->menu_y = y;
    server->menu_hover_idx = -1;
    server->menu_open_ms = 0;
    server->menu_open_x = server->cursor ? server->cursor->x : x;
    server->menu_open_y = server->cursor ? server->cursor->y : y;
    server->menu_drag_moved = false;
    update_window_menu_buffer(server);
}

static void open_root_menu(DtwmServer *server, int x, int y, uint32_t t_ms) {
    server->menu_visible = true;
    server->menu_is_root = true;
    server->menu_target = nullptr;
    server->menu_x = x;
    server->menu_y = y;
    server->menu_hover_idx = -1;
    server->menu_open_ms = t_ms;
    server->menu_open_x = server->cursor ? server->cursor->x : x;
    server->menu_open_y = server->cursor ? server->cursor->y : y;
    server->menu_drag_moved = false;
    update_window_menu_buffer(server);
}

static void close_window_menu(DtwmServer *server) {
    server->menu_visible = false;
    server->menu_is_root = false;
    server->menu_target = nullptr;
    server->menu_hover_idx = -1;
    server->menu_drag_moved = false;
    if (server->menu_buf) {
        wlr_scene_buffer_set_buffer(server->menu_buf, nullptr);
        wlr_scene_node_set_enabled(&server->menu_buf->node, false);
    }
}

static int menu_item_at(DtwmServer *server, double lx, double ly) {
    int rx = (int)lx - server->menu_x;
    int ry = (int)ly - server->menu_y;
    if (server->menu_is_root) {
        return dtwm_root_menu_item_at(rx, ry, server->menu_w, server->menu_h);
    }
    int mh = DTWM_MENU_PAD * 2 + DTWM_MENU_ITEM_COUNT * DTWM_MENU_ITEM_H + 6;
    if (rx < 0 || ry < 0 || rx >= DTWM_MENU_WIDTH || ry >= mh) {
        return -1;
    }
    int y = DTWM_MENU_PAD;
    for (int i = 0; i < DTWM_MENU_ITEM_COUNT; ++i) {
        if (i == DTWM_MENU_ITEM_COUNT - 1) {
            y += 6;
        }
        if (ry >= y && ry < y + DTWM_MENU_ITEM_H) {
            return i;
        }
        y += DTWM_MENU_ITEM_H;
    }
    return -1;
}

static void show_wireframe_outline(DtwmServer *server, int x, int y, int outer_w, int outer_h) {
    if (!server || !server->wireframe_buf) {
        return;
    }
    if (outer_w < 16) outer_w = 16;
    if (outer_h < 16) outer_h = 16;
    if (!server->wireframe_active || outer_w != server->wireframe_w || outer_h != server->wireframe_h) {
        struct wlr_buffer *buf = dtwm_render_wireframe_outline(outer_w, outer_h);
        if (!buf) {
            return;
        }
        wlr_scene_buffer_set_buffer(server->wireframe_buf, buf);
        wlr_buffer_drop(buf);
        server->wireframe_w = outer_w;
        server->wireframe_h = outer_h;
    }
    server->wireframe_active = true;
    wlr_scene_node_set_position(&server->wireframe_buf->node, x, y);
    wlr_scene_node_set_enabled(&server->wireframe_buf->node, true);
}

static void hide_wireframe_outline(DtwmServer *server) {
    if (!server) {
        return;
    }
    server->wireframe_active = false;
    server->wireframe_w = 0;
    server->wireframe_h = 0;
    if (server->wireframe_buf) {
        wlr_scene_buffer_set_buffer(server->wireframe_buf, nullptr);
        wlr_scene_node_set_enabled(&server->wireframe_buf->node, false);
    }
}

static void begin_interactive(DtwmView *view, DtwmCursorMode mode, uint32_t edges) {
    DtwmServer *server = view->server;
    close_window_menu(server);
    hide_wireframe_outline(server);
    server->grabbed_view = view;
    server->cursor_mode = mode;
    server->grab_x = server->cursor->x;
    server->grab_y = server->cursor->y;
    server->grab_orig_box = (struct wlr_box){
        .x = view->x,
        .y = view->y,
        .width = view->client_w,
        .height = view->client_h,
    };
    server->pending_grab_box = server->grab_orig_box;
    server->grab_ignore_next_release = false;
    server->resize_edges = edges;
}

static void sync_xwayland_client_pos(DtwmView *view);

static void promote_subpanel_to_tool_window(DtwmView *view) {
    if (!view || !view->is_subpanel || view->subpanel_torn_off) {
        return;
    }
    view->subpanel_torn_off = true;
    view->has_title = true;
    view->has_menu_btn = true;
    view->y -= DTWM_TITLE_HEIGHT;
    if (view->y < 0) view->y = 0;
    if (view->frame_tree) {
        wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
    }
    sync_xwayland_client_pos(view);
    dtwm_view_update_frame(view);

    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface && view->server->xcb_conn) {
        xcb_connection_t *conn = view->server->xcb_conn;
        const char *aname = "_DT_SUBPANEL_TORN_OFF";
        xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(aname), aname);
        xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, nullptr);
        if (rep && rep->atom != XCB_ATOM_NONE) {
            uint32_t one = 1;
            xcb_change_property(
                conn, XCB_PROP_MODE_REPLACE, view->xwayland_surface->window_id,
                rep->atom, XCB_ATOM_CARDINAL, 32, 1, &one
            );
            xcb_flush(conn);
        }
        free(rep);
    }
}

static void close_docked_subpanels(DtwmServer *server) {
    if (!server) return;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped && v->is_subpanel && !v->subpanel_torn_off) {
            view_close(v);
        }
    }
}

static bool view_is_normal_app(const DtwmView *v);

static void spawn_desktop_command(DtwmServer *server, const char *prog, const char *arg1) {
    if (!server || !prog || !prog[0]) {
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        /* Double-fork so init reaps the grandchild without interfering with wlr_xwayland */
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
        setsid();
        pid_t gpid = fork();
        if (gpid != 0) {
            _exit(0);
        }
        if (server->xwayland && server->xwayland->display_name) {
            setenv("DISPLAY", server->xwayland->display_name, 1);
        }
        if (server->wayland_socket) {
            setenv("WAYLAND_DISPLAY", server->wayland_socket, 1);
        }
        const char *home = getenv("HOME");
        if (home && home[0]) {
            (void)chdir(home);
        }
        char *args[3] = {(char *)prog, (char *)arg1, nullptr};
        execv(prog, args);
        _exit(127);
    }
    (void)waitpid(pid, nullptr, 0);
}

static void shuffle_workspace_up(DtwmServer *server) {
    if (!server || !server->workspace_tree) {
        return;
    }
    struct wlr_scene_node *node;
    wl_list_for_each(node, &server->workspace_tree->children, link) {
        DtwmView *v = node->data;
        if (v && view_is_normal_app(v)) {
            focus_view(server, v, nullptr);
            return;
        }
    }
}

static void shuffle_workspace_down(DtwmServer *server) {
    if (!server || !server->workspace_tree) {
        return;
    }
    DtwmView *top_v = nullptr;
    struct wlr_scene_node *node;
    wl_list_for_each_reverse(node, &server->workspace_tree->children, link) {
        DtwmView *v = node->data;
        if (v && view_is_normal_app(v)) {
            top_v = v;
            break;
        }
    }
    if (!top_v) {
        return;
    }
    if (top_v->frame_tree) {
        wlr_scene_node_lower_to_bottom(&top_v->frame_tree->node);
    }
    if (top_v->kind == DTWM_VIEW_XWAYLAND && top_v->xwayland_surface) {
        wlr_xwayland_surface_restack(top_v->xwayland_surface, nullptr, XCB_STACK_MODE_BELOW);
    }
    DtwmView *next_top = nullptr;
    wl_list_for_each_reverse(node, &server->workspace_tree->children, link) {
        DtwmView *v = node->data;
        if (v && view_is_normal_app(v)) {
            next_top = v;
            break;
        }
    }
    if (next_top && next_top != top_v) {
        focus_view_ex(server, next_top, nullptr, false);
    }
}

static void refresh_workspace_desktop(DtwmServer *server) {
    if (!server) {
        return;
    }
    dtwm_update_all_backdrops(server);
    dtwm_icons_relayout(server);
    dtwm_update_front_panel(server);
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped && v->decorated) {
            dtwm_view_update_frame(v);
        }
    }
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->wlr_output) {
            wlr_output_schedule_frame(out->wlr_output);
        }
    }
}

static void execute_root_menu_action(DtwmServer *server, int idx) {
    switch (idx) {
    case 0: /* Open Terminal */
        spawn_desktop_command(server, CDE_INSTALLATION_TOP "/bin/dtterm", nullptr);
        break;
    case 1: /* Open File Manager */
        spawn_desktop_command(server, CDE_INSTALLATION_TOP "/bin/dtfile", nullptr);
        break;
    case 2: /* Application Manager */
        spawn_desktop_command(server, CDE_INSTALLATION_TOP "/bin/dtaction", "Dtappmgr");
        break;
    case 3: /* Change Backdrop... */
        spawn_desktop_command(server, CDE_INSTALLATION_TOP "/bin/dtstyle", "-backdrop");
        break;
    case 4: /* Display Settings... */
        spawn_desktop_command(server, CDE_INSTALLATION_TOP "/bin/dtstyle", "-monitors");
        break;
    case 5: /* Style Manager... */
        spawn_desktop_command(server, CDE_INSTALLATION_TOP "/bin/dtstyle", nullptr);
        break;
    case 6: /* Clean Up Icons */
        dtwm_icons_relayout(server);
        break;
    case 7: /* Shuffle Up */
        shuffle_workspace_up(server);
        break;
    case 8: /* Shuffle Down */
        shuffle_workspace_down(server);
        break;
    case 9: /* Minimize / Restore Front Panel */
        if (server->panel_view && server->panel_view->mapped) {
            view_set_minimized(server->panel_view, !server->panel_view->minimized);
            dtwm_update_front_panel(server);
        }
        break;
    case 10: /* Refresh */
        refresh_workspace_desktop(server);
        break;
    case 11: /* Lock Screen */
        spawn_desktop_command(server, CDE_INSTALLATION_TOP "/bin/dtaction", "LockDisplay");
        break;
    case 12: /* Log Out... */
        spawn_desktop_command(server, CDE_INSTALLATION_TOP "/bin/dtaction", "ExitSession");
        break;
    default:
        break;
    }
}

static void execute_menu_action(DtwmServer *server, int idx) {
    bool is_root = server->menu_is_root;
    DtwmView *v = server->menu_target;
    close_window_menu(server);
    if (is_root) {
        execute_root_menu_action(server, idx);
        return;
    }
    if (!v) {
        return;
    }
    switch (idx) {
    case 0: /* Restore */
        if (v->maximized) {
            view_set_maximized(v, false);
        }
        break;
    case 1: /* Move */
        begin_interactive(v, DTWM_CURSOR_MOVE, 0);
        server->grab_ignore_next_release = (server->pointer_buttons_down > 0);
        if (!server->move_opaque && !v->is_panel) {
            int ow = 0, oh = 0;
            dtwm_view_outer_size(v, &ow, &oh);
            show_wireframe_outline(server, v->x, v->y, ow, oh);
        }
        break;
    case 2: /* Size */
        begin_interactive(v, DTWM_CURSOR_RESIZE, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
        server->grab_ignore_next_release = (server->pointer_buttons_down > 0);
        if (!server->move_opaque) {
            int ow = 0, oh = 0;
            dtwm_view_outer_size(v, &ow, &oh);
            show_wireframe_outline(server, v->x, v->y, ow, oh);
        }
        break;
    case 3: /* Minimize */
        view_set_minimized(v, true);
        break;
    case 4: /* Maximize */
        if (!v->maximized) {
            view_set_maximized(v, true);
        }
        break;
    case 5: /* Lower */
        if (v->frame_tree) {
            wlr_scene_node_lower_to_bottom(&v->frame_tree->node);
        }
        break;
    case 6: { /* Occupy All Workspaces / Unoccupy Other Workspaces */
        uint32_t all_mask = (1u << server->num_workspaces) - 1u;
        if ((v->workspace_mask & all_mask) == all_mask) {
            dtwm_view_set_workspace_mask(v, 1u << server->current_workspace);
        } else {
            dtwm_view_set_workspace_mask(v, all_mask);
        }
        break;
    }
    case 7: /* Close */
        view_close(v);
        break;
    default:
        break;
    }
}

/* --- Snap Preview & Task Switcher --- */
static DtwmSnapZone compute_snap_zone(DtwmServer *server, double cx, double cy, struct wlr_box *snap_box) {
    struct wlr_box obox;
    dtwm_get_usable_output_box(server, cx, cy, &obox);
    bool left = (cx - obox.x) < DTWM_SNAP_THRESHOLD;
    bool right = (obox.x + obox.width - cx) < DTWM_SNAP_THRESHOLD;
    bool top = (cy - obox.y) < DTWM_SNAP_THRESHOLD;
    bool bottom = (obox.y + obox.height - cy) < DTWM_SNAP_THRESHOLD;

    int hw = obox.width / 2;
    int hh = obox.height / 2;
    if (left && top) {
        *snap_box = (struct wlr_box){obox.x, obox.y, hw, hh};
        return DTWM_SNAP_NW;
    }
    if (right && top) {
        *snap_box = (struct wlr_box){obox.x + hw, obox.y, obox.width - hw, hh};
        return DTWM_SNAP_NE;
    }
    if (left && bottom) {
        *snap_box = (struct wlr_box){obox.x, obox.y + hh, hw, obox.height - hh};
        return DTWM_SNAP_SW;
    }
    if (right && bottom) {
        *snap_box = (struct wlr_box){obox.x + hw, obox.y + hh, obox.width - hw, obox.height - hh};
        return DTWM_SNAP_SE;
    }
    if (left) {
        *snap_box = (struct wlr_box){obox.x, obox.y, hw, obox.height};
        return DTWM_SNAP_LEFT;
    }
    if (right) {
        *snap_box = (struct wlr_box){obox.x + hw, obox.y, obox.width - hw, obox.height};
        return DTWM_SNAP_RIGHT;
    }
    return DTWM_SNAP_NONE;
}

static void update_snap_preview(DtwmServer *server, DtwmSnapZone zone, const struct wlr_box *box) {
    server->active_snap = zone;
    if (zone == DTWM_SNAP_NONE) {
        wlr_scene_buffer_set_buffer(server->snap_preview_buf, nullptr);
        wlr_scene_node_set_enabled(&server->snap_preview_buf->node, false);
        return;
    }
    struct wlr_buffer *buf = dtwm_render_snap_preview(&server->palette, box->width, box->height);
    if (buf) {
        wlr_scene_buffer_set_buffer(server->snap_preview_buf, buf);
        wlr_buffer_drop(buf);
        wlr_scene_node_set_position(&server->snap_preview_buf->node, box->x, box->y);
        wlr_scene_node_set_enabled(&server->snap_preview_buf->node, true);
    }
}

static bool view_is_normal_app(const DtwmView *v) {
    return dtwm_view_visible_on_current_ws(v) &&
           !v->is_swallowed && !v->is_desktop_pin && !v->is_panel && !v->is_subpanel &&
           !v->is_lock_dialog;
}

void dtwm_restore_view_focus(DtwmServer *server, DtwmView *view) {
    if (!server || !view || !view->mapped) return;
    focus_view(server, view, nullptr);
}

static void cycle_focus_without_osd(DtwmServer *server) {
    DtwmView *first = nullptr;
    DtwmView *after_focused = nullptr;
    bool seen_focused = false;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (!view_is_normal_app(v)) {
            continue;
        }
        if (!first) {
            first = v;
        }
        if (v == server->focused_view) {
            seen_focused = true;
            continue;
        }
        if (seen_focused) {
            after_focused = v;
            break;
        }
    }
    DtwmView *chosen = after_focused ? after_focused : first;
    if (chosen) {
        focus_view_ex(server, chosen, nullptr, server->focus_auto_raise);
    }
}

static void show_or_cycle_task_switcher(DtwmServer *server) {
    int count = 0;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (view_is_normal_app(v)) count++;
    }
    if (count == 0) {
        return;
    }
    if (!server->switcher_visible) {
        server->switcher_visible = true;
        server->switcher_index = (count > 1) ? 1 : 0;
    } else {
        server->switcher_index = (server->switcher_index + 1) % count;
    }
    int sw = 0, sh = 0;
    struct wlr_buffer *buf = dtwm_render_task_switcher(server, &sw, &sh);
    if (buf) {
        struct wlr_box obox;
        get_primary_output_box(server, server->cursor->x, server->cursor->y, &obox);
        wlr_scene_buffer_set_buffer(server->switcher_buf, buf);
        wlr_buffer_drop(buf);
        wlr_scene_node_set_position(
            &server->switcher_buf->node,
            obox.x + (obox.width - sw) / 2,
            obox.y + (obox.height - sh) / 2
        );
        wlr_scene_node_set_enabled(&server->switcher_buf->node, true);
    }
}

static void hide_task_switcher(DtwmServer *server, bool activate_selected) {
    if (!server->switcher_visible) {
        return;
    }
    DtwmView *chosen = nullptr;
    if (activate_selected) {
        int count = 0;
        DtwmView *v;
        wl_list_for_each(v, &server->views, link) {
            if (view_is_normal_app(v)) count++;
        }
        if (count > 0) {
            int target = server->switcher_index % count;
            int idx = 0;
            wl_list_for_each(v, &server->views, link) {
                if (!view_is_normal_app(v)) continue;
                if (idx == target) {
                    chosen = v;
                    break;
                }
                idx++;
            }
        }
    }
    server->switcher_visible = false;
    wlr_scene_buffer_set_buffer(server->switcher_buf, nullptr);
    wlr_scene_node_set_enabled(&server->switcher_buf->node, false);
    if (chosen) {
        focus_view(server, chosen, nullptr);
    }
}

/* --- Hit Testing Across Managed Views & Unmanaged Surfaces --- */
static DtwmView *desktop_view_at(
    DtwmServer *server,
    double lx,
    double ly,
    struct wlr_surface **out_surface,
    double *sx,
    double *sy,
    DtwmFramePart *out_part
) {
    *out_surface = nullptr;
    *out_part = DTWM_PART_NONE;

    /* First check scene graph for popups / unmanaged XWayland menus / layer surfaces / client surfaces */
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ssurf = wlr_scene_surface_try_from_buffer(sbuf);
        if (ssurf) {
            *out_surface = ssurf->surface;
            /* Walk ancestor scene_trees to see if owned by a DtwmView */
            struct wlr_scene_tree *tree = node->parent;
            while (tree && !tree->node.data) {
                tree = tree->node.parent;
            }
            if (tree && tree->node.data) {
                DtwmView *v = tree->node.data;
                *out_part = DTWM_PART_CLIENT;
                return v;
            }
            return nullptr;
        }
        /* Could be a view's server-side CDE bevel frame_buf! */
        if (node->parent && node->parent->node.data) {
            DtwmView *v = node->parent->node.data;
            *out_part = dtwm_frame_hit_test(v, (int)(lx - v->x), (int)(ly - v->y));
            return v;
        }
    }
    return nullptr;
}

/* --- Pointer Motion / Button / Axis --- */
static void process_cursor_move(DtwmServer *server) {
    DtwmView *v = server->grabbed_view;
    if (!v) return;
    if (v->maximized) {
        view_set_maximized(v, false);
        server->grab_orig_box.x = (int)server->cursor->x - v->client_w / 2;
        server->grab_orig_box.y = (int)server->cursor->y - 12;
        server->grab_orig_box.width = v->client_w;
        server->grab_orig_box.height = v->client_h;
        server->grab_x = server->cursor->x;
        server->grab_y = server->cursor->y;
    }
    int nx = server->grab_orig_box.x + (int)(server->cursor->x - server->grab_x);
    int ny = server->grab_orig_box.y + (int)(server->cursor->y - server->grab_y);
    if (shift_is_held(server)) {
        nx = align_to_grid(nx);
        ny = align_to_grid(ny);
    }
    if (v->is_panel) {
        v->x = nx;
        v->y = ny;
        wlr_scene_node_set_position(&v->frame_tree->node, v->x, v->y);
        sync_xwayland_client_pos(v);
        dtwm_publish_active_window_box(server);
        server->panel_user_moved = true;
        server->panel_x = v->x;
        server->panel_y = v->y;
        return;
    }

    struct wlr_box sbox = {0};
    DtwmSnapZone zone = compute_snap_zone(server, server->cursor->x, server->cursor->y, &sbox);
    update_snap_preview(server, zone, &sbox);

    if (!server->move_opaque) {
        int ow = 0, oh = 0;
        dtwm_view_outer_size(v, &ow, &oh);
        server->pending_grab_box = (struct wlr_box){
            .x = nx,
            .y = ny,
            .width = v->client_w,
            .height = v->client_h,
        };
        show_wireframe_outline(server, nx, ny, ow, oh);
        return;
    }

    hide_wireframe_outline(server);
    v->x = nx;
    v->y = ny;
    wlr_scene_node_set_position(&v->frame_tree->node, v->x, v->y);
    sync_xwayland_client_pos(v);
    dtwm_publish_active_window_box(server);
}

static void process_cursor_resize(DtwmServer *server) {
    DtwmView *v = server->grabbed_view;
    if (!v) return;
    double dx = server->cursor->x - server->grab_x;
    double dy = server->cursor->y - server->grab_y;

    int new_x = server->grab_orig_box.x;
    int new_y = server->grab_orig_box.y;
    int new_w = server->grab_orig_box.width;
    int new_h = server->grab_orig_box.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_y += (int)dy;
        new_h -= (int)dy;
        if (new_h < 48) {
            new_y = server->grab_orig_box.y + server->grab_orig_box.height - 48;
            new_h = 48;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_h += (int)dy;
        if (new_h < 48) new_h = 48;
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_x += (int)dx;
        new_w -= (int)dx;
        if (new_w < 64) {
            new_x = server->grab_orig_box.x + server->grab_orig_box.width - 64;
            new_w = 64;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_w += (int)dx;
        if (new_w < 64) new_w = 64;
    }

    if (shift_is_held(server)) {
        new_w = align_to_grid(new_w);
        new_h = align_to_grid(new_h);
        if (new_w < 64) new_w = 64;
        if (new_h < 48) new_h = 48;
    }

    if (!server->move_opaque) {
        int l = 0, t = 0, r = 0, b = 0;
        dtwm_view_insets(v, &l, &t, &r, &b);
        server->pending_grab_box = (struct wlr_box){
            .x = new_x,
            .y = new_y,
            .width = new_w,
            .height = new_h,
        };
        show_wireframe_outline(server, new_x, new_y, new_w + l + r, new_h + t + b);
        return;
    }

    hide_wireframe_outline(server);
    view_apply_geometry(v, new_x, new_y, new_w, new_h);
}

static const char *cursor_name_for_part(DtwmFramePart part) {
    switch (part) {
    case DTWM_PART_RESIZE_NW: return "nw-resize";
    case DTWM_PART_RESIZE_N:  return "n-resize";
    case DTWM_PART_RESIZE_NE: return "ne-resize";
    case DTWM_PART_RESIZE_E:  return "e-resize";
    case DTWM_PART_RESIZE_SE: return "se-resize";
    case DTWM_PART_RESIZE_S:  return "s-resize";
    case DTWM_PART_RESIZE_SW: return "sw-resize";
    case DTWM_PART_RESIZE_W:  return "w-resize";
    default:                  return "default";
    }
}

static void process_cursor_motion(DtwmServer *server, uint32_t time_msec) {
    dtwm_lock_notify_activity(server);
    if (server->session_locked) {
        double lsx = 0, lsy = 0;
        struct wlr_surface *lsurf =
            dtwm_lock_surface_at(server, server->cursor->x, server->cursor->y, &lsx, &lsy);
        if (lsurf) {
            wlr_seat_pointer_notify_enter(server->seat, lsurf, lsx, lsy);
            wlr_seat_pointer_notify_motion(server->seat, time_msec, lsx, lsy);
        } else {
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
            wlr_seat_pointer_notify_clear_focus(server->seat);
        }
        dtwm_icons_sync_xwayland_pointer(server, server->cursor->x, server->cursor->y, lsurf);
        return;
    }

    if (server->drag_icon_tree) {
        wlr_scene_node_set_position(
            &server->drag_icon_tree->node, (int)server->cursor->x, (int)server->cursor->y
        );
    }
    if (server->menu_visible) {
        double dx = server->cursor->x - server->menu_open_x;
        double dy = server->cursor->y - server->menu_open_y;
        if (dx * dx + dy * dy >= 16.0) {
            server->menu_drag_moved = true;
        }
        int idx = menu_item_at(server, server->cursor->x, server->cursor->y);
        if (idx != server->menu_hover_idx) {
            server->menu_hover_idx = idx;
            update_window_menu_buffer(server);
        }
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        return;
    }

    if (server->cursor_mode == DTWM_CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    }
    if (server->cursor_mode == DTWM_CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    if (server->subpanel_drag_candidate) {
        DtwmView *sp = server->subpanel_drag_candidate;
        if (!sp->mapped || !sp->is_subpanel || sp->subpanel_torn_off) {
            server->subpanel_drag_candidate = nullptr;
        } else {
            double dx = server->cursor->x - server->subpanel_drag_start_x;
            double dy = server->cursor->y - server->subpanel_drag_start_y;
            if (dx * dx + dy * dy >= 64.0) {
                server->subpanel_drag_candidate = nullptr;
                /* Disarm any armed gadget inside the subpanel before tearing off */
                wlr_seat_pointer_notify_motion(server->seat, time_msec, -200.0, -200.0);
                wlr_seat_pointer_notify_button(
                    server->seat, time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED
                );
                wlr_seat_pointer_notify_clear_focus(server->seat);
                promote_subpanel_to_tool_window(sp);
                begin_interactive(sp, DTWM_CURSOR_MOVE, 0);
                process_cursor_move(server);
                return;
            }
        }
    }

    double sx = 0, sy = 0;
    struct wlr_surface *surface = nullptr;
    DtwmFramePart part = DTWM_PART_NONE;
    DtwmView *view = desktop_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy, &part);

    if (server->focus_follows_mouse && server->pointer_buttons_down == 0 && !server->switcher_visible &&
        view && view != server->focused_view && view_is_normal_app(view)) {
        focus_view_ex(server, view, surface, server->focus_auto_raise);
    }

    if (!surface) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, view ? cursor_name_for_part(part) : "default");
        if (server->seat->drag != nullptr || server->pointer_buttons_down == 0) {
            wlr_seat_pointer_notify_clear_focus(server->seat);
        } else if (server->seat->pointer_state.focused_surface) {
            struct wlr_surface *fsurf = server->seat->pointer_state.focused_surface;
            DtwmView *fv;
            wl_list_for_each(fv, &server->views, link) {
                if (fv->mapped && view_wlr_surface(fv) == fsurf) {
                    int fl = 0, ft = 0, fr = 0, fb = 0;
                    dtwm_view_insets(fv, &fl, &ft, &fr, &fb);
                    double fsx = server->cursor->x - (double)(fv->x + fl);
                    double fsy = server->cursor->y - (double)(fv->y + ft);
                    wlr_seat_pointer_notify_motion(server->seat, time_msec, fsx, fsy);
                    break;
                }
            }
        }
    } else {
        dtwm_xdnd_on_cursor_motion(server, surface, sx, sy, time_msec);
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
    }
    dtwm_icons_sync_xwayland_pointer(server, server->cursor->x, server->cursor->y, surface);
}

void dtwm_recheck_cursor_focus(DtwmServer *server) {
    if (!server) return;
    process_cursor_motion(server, dtwm_now_msec());
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *ev = data;
    uint32_t t_ms = ev->time_msec ? ev->time_msec : dtwm_now_msec();
    wlr_cursor_move(server->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    process_cursor_motion(server, t_ms);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *ev = data;
    uint32_t t_ms = ev->time_msec ? ev->time_msec : dtwm_now_msec();
    wlr_cursor_warp_absolute(server->cursor, &ev->pointer->base, ev->x, ev->y);
    process_cursor_motion(server, t_ms);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *ev = data;
    uint32_t t_ms = ev->time_msec ? ev->time_msec : dtwm_now_msec();
    dtwm_lock_notify_activity(server);

    uint32_t btn = ev->button;
    if (server->left_handed) {
        if (btn == BTN_LEFT) btn = BTN_RIGHT;
        else if (btn == BTN_RIGHT) btn = BTN_LEFT;
    }
    dtwm_xdnd_on_button_state(server, btn, ev->state == WL_POINTER_BUTTON_STATE_PRESSED);

    if (server->session_locked) {
        double lsx = 0, lsy = 0;
        struct wlr_surface *lsurf =
            dtwm_lock_surface_at(server, server->cursor->x, server->cursor->y, &lsx, &lsy);
        if (lsurf && ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
            dtwm_lock_focus_surface(server, lsurf);
            wlr_seat_pointer_notify_enter(server->seat, lsurf, lsx, lsy);
        }
        wlr_seat_pointer_notify_button(server->seat, t_ms, btn, ev->state);
        return;
    }

    if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        server->subpanel_drag_candidate = nullptr;
        if (server->menu_visible) {
            /* Support classic Motif/CDE press-drag-release on popup menus while keeping
             * a stationary right-click posted for click-to-select interaction. */
            if (server->menu_drag_moved) {
                int idx = menu_item_at(server, server->cursor->x, server->cursor->y);
                if (idx >= 0) {
                    execute_menu_action(server, idx);
                }
                server->menu_drag_moved = false;
            }
            return;
        }
        if (server->cursor_mode != DTWM_CURSOR_PASSTHROUGH && server->grab_ignore_next_release) {
            server->grab_ignore_next_release = false;
            return;
        }
        bool had_wireframe = server->wireframe_active;
        struct wlr_box pending = server->pending_grab_box;
        hide_wireframe_outline(server);

        if (server->cursor_mode == DTWM_CURSOR_MOVE && server->grabbed_view) {
            if (server->active_snap != DTWM_SNAP_NONE) {
                struct wlr_box sbox = {0};
                DtwmSnapZone z = compute_snap_zone(server, server->cursor->x, server->cursor->y, &sbox);
                update_snap_preview(server, DTWM_SNAP_NONE, nullptr);
                if (z != DTWM_SNAP_NONE) {
                    DtwmView *v = server->grabbed_view;
                    v->saved_x = server->grab_orig_box.x;
                    v->saved_y = server->grab_orig_box.y;
                    v->saved_w = server->grab_orig_box.width;
                    v->saved_h = server->grab_orig_box.height;
                    v->has_saved_geom = true;
                    int l, t, r, b;
                    dtwm_view_insets(v, &l, &t, &r, &b);
                    view_apply_geometry(v, sbox.x, sbox.y, sbox.width - l - r, sbox.height - t - b);
                }
            } else if (had_wireframe) {
                DtwmView *v = server->grabbed_view;
                v->x = pending.x;
                v->y = pending.y;
                if (v->frame_tree) {
                    wlr_scene_node_set_position(&v->frame_tree->node, v->x, v->y);
                }
                sync_xwayland_client_pos(v);
                dtwm_publish_active_window_box(server);
            }
        } else if (server->cursor_mode == DTWM_CURSOR_RESIZE && server->grabbed_view && had_wireframe) {
            view_apply_geometry(
                server->grabbed_view, pending.x, pending.y, pending.width, pending.height
            );
        }
        if (server->cursor_mode != DTWM_CURSOR_PASSTHROUGH) {
            server->cursor_mode = DTWM_CURSOR_PASSTHROUGH;
            server->grabbed_view = nullptr;
            update_snap_preview(server, DTWM_SNAP_NONE, nullptr);
            return;
        }
        wlr_seat_pointer_notify_button(server->seat, t_ms, btn, ev->state);
        return;
    }

    /* Button press */
    if (server->cursor_mode != DTWM_CURSOR_PASSTHROUGH) {
        /* Clicking while in Window Menu Move/Size mode commits on the subsequent release */
        server->grab_ignore_next_release = false;
        return;
    }
    if (server->switcher_visible) {
        hide_task_switcher(server, true);
        return;
    }

    if (server->menu_visible) {
        int idx = menu_item_at(server, server->cursor->x, server->cursor->y);
        if (idx >= 0) {
            execute_menu_action(server, idx);
        } else {
            close_window_menu(server);
        }
        return;
    }

    double sx = 0, sy = 0;
    struct wlr_surface *surface = nullptr;
    DtwmFramePart part = DTWM_PART_NONE;
    DtwmView *view = desktop_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy, &part);

    if (!view && !surface) {
        close_docked_subpanels(server);
        DtwmView *icon_v = dtwm_icons_view_at(server, server->cursor->x, server->cursor->y);
        if (icon_v) {
            if (btn == BTN_LEFT) {
                focus_view(server, icon_v, nullptr);
            } else if (btn == BTN_RIGHT) {
                open_window_menu(server, icon_v, (int)server->cursor->x, (int)server->cursor->y);
            }
            return;
        }
        /* Clicking empty CDE desktop clears active window focus; right-click opens Workspace Menu */
        clear_active_focus(server);
        if (btn == BTN_RIGHT && !server->greeter_mode && !server->session_locked) {
            open_root_menu(server, (int)server->cursor->x, (int)server->cursor->y, t_ms);
        }
        return;
    }

    if (view && !(view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface &&
                  view->xwayland_surface->override_redirect)) {
        if (!view->is_panel && !view->is_subpanel) {
            close_docked_subpanels(server);
        }
        focus_view_ex(
            server,
            view,
            surface,
            (part == DTWM_PART_CLIENT) ? server->focus_auto_raise : true
        );

        if (view->is_subpanel && !view->subpanel_torn_off && btn == BTN_LEFT) {
            if (part != DTWM_PART_CLIENT) {
                promote_subpanel_to_tool_window(view);
                begin_interactive(view, DTWM_CURSOR_MOVE, 0);
                return;
            }
            server->subpanel_drag_candidate = view;
            server->subpanel_drag_start_x = server->cursor->x;
            server->subpanel_drag_start_y = server->cursor->y;
        }

        if (part != DTWM_PART_CLIENT) {
            if (btn == BTN_RIGHT && !view->is_panel && !server->greeter_mode) {
                open_window_menu(server, view, (int)server->cursor->x, (int)server->cursor->y);
                return;
            }
            if (btn == BTN_LEFT) {
                switch (part) {
                case DTWM_PART_BTN_MENU: {
                    int l, t, r, b;
                    dtwm_view_insets(view, &l, &t, &r, &b);
                    open_window_menu(server, view, view->x + DTWM_BORDER_WIDTH, view->y + t);
                    return;
                }
                case DTWM_PART_BTN_MINIMIZE:
                    view_set_minimized(view, true);
                    return;
                case DTWM_PART_BTN_MAXIMIZE:
                    view_set_maximized(view, !view->maximized);
                    return;
                case DTWM_PART_TITLE:
                    if (view->has_max_btn && server->last_title_click_view == view &&
                        (t_ms - server->last_title_click_ms) <=
                            (server->double_click_ms > 0 ? server->double_click_ms : 500u)) {
                        server->last_title_click_view = nullptr;
                        view_set_maximized(view, !view->maximized);
                    } else {
                        server->last_title_click_view = view;
                        server->last_title_click_ms = t_ms;
                        begin_interactive(view, DTWM_CURSOR_MOVE, 0);
                    }
                    return;
                default: {
                    uint32_t edges = dtwm_part_to_wlr_edges(part);
                    if (edges != WLR_EDGE_NONE) {
                        begin_interactive(view, DTWM_CURSOR_RESIZE, edges);
                    }
                    return;
                }
                }
            }
        }
    }

    wlr_seat_pointer_notify_button(server->seat, t_ms, btn, ev->state);
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *ev = data;
    uint32_t t_ms = ev->time_msec ? ev->time_msec : dtwm_now_msec();
    dtwm_lock_notify_activity(server);
    wlr_seat_pointer_notify_axis(
        server->seat, t_ms, ev->orientation, ev->delta,
        ev->delta_discrete, ev->source, ev->relative_direction
    );
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmServer *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

/* --- Keyboard Input & CDE Shortcuts (Alt+Tab, Alt+F4, Alt+Space) --- */
static void handle_keyboard_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmKeyboard *kb = wl_container_of(listener, kb, modifiers);
    dtwm_lock_notify_activity(kb->server);
    wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
    if (!dtwm_ime_forward_modifiers(kb->server, kb->wlr_keyboard)) {
        wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
    }

    if (!kb->server->session_locked && kb->server->switcher_visible) {
        uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
        if (!(mods & WLR_MODIFIER_ALT)) {
            hide_task_switcher(kb->server, true);
        }
    }
}

static bool handle_compositor_keybinding(DtwmServer *server, uint32_t mods, xkb_keysym_t sym) {
    if ((mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_ALT)) {
        if (sym >= XKB_KEY_1 && sym <= XKB_KEY_8) {
            uint32_t ws = (uint32_t)(sym - XKB_KEY_1);
            if (ws < server->num_workspaces) {
                dtwm_switch_workspace(server, ws);
            }
            return true;
        }
        if (sym == XKB_KEY_Right && server->num_workspaces > 1) {
            dtwm_switch_workspace(server, (server->current_workspace + 1) % server->num_workspaces);
            return true;
        }
        if (sym == XKB_KEY_Left && server->num_workspaces > 1) {
            dtwm_switch_workspace(
                server, (server->current_workspace + server->num_workspaces - 1) % server->num_workspaces
            );
            return true;
        }
    }
    if ((mods & WLR_MODIFIER_ALT) && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
        if (server->show_task_switcher) {
            show_or_cycle_task_switcher(server);
        } else {
            cycle_focus_without_osd(server);
        }
        return true;
    }
    if ((mods & WLR_MODIFIER_ALT) && sym == XKB_KEY_F4) {
        if (server->focused_view) {
            view_close(server->focused_view);
        }
        return true;
    }
    if ((mods & WLR_MODIFIER_ALT) && sym == XKB_KEY_space) {
        if (server->focused_view) {
            int l, t, r, b;
            dtwm_view_insets(server->focused_view, &l, &t, &r, &b);
            open_window_menu(
                server,
                server->focused_view,
                server->focused_view->x + DTWM_BORDER_WIDTH,
                server->focused_view->y + t
            );
        }
        return true;
    }
    if (sym == XKB_KEY_Escape) {
        if (server->cursor_mode != DTWM_CURSOR_PASSTHROUGH) {
            DtwmView *gv = server->grabbed_view;
            bool was_opaque = server->move_opaque || (gv && gv->is_panel);
            hide_wireframe_outline(server);
            update_snap_preview(server, DTWM_SNAP_NONE, nullptr);
            if (gv && was_opaque) {
                view_apply_geometry(
                    gv,
                    server->grab_orig_box.x,
                    server->grab_orig_box.y,
                    server->grab_orig_box.width,
                    server->grab_orig_box.height
                );
            }
            server->cursor_mode = DTWM_CURSOR_PASSTHROUGH;
            server->grabbed_view = nullptr;
            return true;
        }
        if (server->menu_visible) {
            close_window_menu(server);
            return true;
        }
        if (server->switcher_visible) {
            hide_task_switcher(server, false);
            return true;
        }
    }
    if (server->menu_visible) {
        int max_items = server->menu_is_root ? DTWM_ROOT_MENU_ITEM_COUNT : DTWM_MENU_ITEM_COUNT;
        if (sym == XKB_KEY_Down) {
            server->menu_hover_idx = (server->menu_hover_idx + 1) % max_items;
            update_window_menu_buffer(server);
            return true;
        }
        if (sym == XKB_KEY_Up) {
            server->menu_hover_idx =
                (server->menu_hover_idx <= 0) ? (max_items - 1) : (server->menu_hover_idx - 1);
            update_window_menu_buffer(server);
            return true;
        }
        if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && server->menu_hover_idx >= 0) {
            execute_menu_action(server, server->menu_hover_idx);
            return true;
        }
    }
    if (sym == XKB_KEY_Return && server->switcher_visible) {
        hide_task_switcher(server, true);
        return true;
    }
    return false;
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
    DtwmKeyboard *kb = wl_container_of(listener, kb, key);
    DtwmServer *server = kb->server;
    struct wlr_keyboard_key_event *ev = data;
    uint32_t t_ms = ev->time_msec ? ev->time_msec : dtwm_now_msec();
    dtwm_lock_notify_activity(server);

    if (server->session_locked || server->greeter_mode) {
        wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, t_ms, ev->keycode, ev->state);
        return;
    }

    uint32_t keycode = ev->keycode + 8;
    const xkb_keysym_t *syms = nullptr;
    int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; ++i) {
            if (handle_compositor_keybinding(server, mods, syms[i])) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        if (!dtwm_ime_forward_key(server, kb->wlr_keyboard, ev)) {
            wlr_seat_keyboard_notify_key(server->seat, t_ms, ev->keycode, ev->state);
        }
    }
}

static void handle_keyboard_keymap(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmKeyboard *kb = wl_container_of(listener, kb, keymap);
    DtwmServer *server = kb->server;
    wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
    dtwm_ime_set_keyboard(server, kb->wlr_keyboard);
    if (!server->session_locked && server->focused_view) {
        struct wlr_surface *fs = view_wlr_surface(server->focused_view);
        if (fs) {
            wlr_seat_keyboard_notify_enter(
                server->seat,
                fs,
                kb->wlr_keyboard->keycodes,
                kb->wlr_keyboard->num_keycodes,
                &kb->wlr_keyboard->modifiers
            );
        }
    }
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmKeyboard *kb = wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->keymap.link);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->link);
    free(kb);
}

static void server_add_keyboard(DtwmServer *server, struct wlr_keyboard *wlr_kbd, bool is_virtual) {
    DtwmKeyboard *kb = calloc(1, sizeof(*kb));
    if (!kb) return;
    kb->server = server;
    kb->wlr_keyboard = wlr_kbd;
    if (!is_virtual) {
        dtwm_hw_apply_keyboard_defaults(server, wlr_kbd);
    }

    kb->keymap.notify = handle_keyboard_keymap;
    wl_signal_add(&wlr_kbd->events.keymap, &kb->keymap);
    kb->modifiers.notify = handle_keyboard_modifiers;
    wl_signal_add(&wlr_kbd->events.modifiers, &kb->modifiers);
    kb->key.notify = handle_keyboard_key;
    wl_signal_add(&wlr_kbd->events.key, &kb->key);
    kb->destroy.notify = handle_keyboard_destroy;
    wl_signal_add(&wlr_kbd->base.events.destroy, &kb->destroy);

    if (wlr_kbd->keymap) {
        wlr_seat_set_keyboard(server->seat, wlr_kbd);
        dtwm_ime_set_keyboard(server, wlr_kbd);
    }
    wl_list_insert(&server->keyboards, &kb->link);
    if (!server->session_locked && server->focused_view && wlr_kbd->keymap) {
        struct wlr_surface *fs = view_wlr_surface(server->focused_view);
        if (fs) {
            wlr_seat_keyboard_notify_enter(
                server->seat, fs, wlr_kbd->keycodes, wlr_kbd->num_keycodes, &wlr_kbd->modifiers
            );
        }
    }
}

static void update_seat_capabilities(DtwmServer *server) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *dev = data;
    switch (dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_add_keyboard(server, wlr_keyboard_from_input_device(dev), false);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, dev);
        dtwm_hw_apply_input_device(server, dev);
        break;
    default:
        break;
    }
    update_seat_capabilities(server);
}

static void handle_new_virtual_pointer(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_virtual_pointer);
    struct wlr_virtual_pointer_v1_new_pointer_event *ev = data;
    wlr_cursor_attach_input_device(server->cursor, &ev->new_pointer->pointer.base);
    if (ev->suggested_output) {
        wlr_cursor_map_input_to_output(server->cursor, &ev->new_pointer->pointer.base, ev->suggested_output);
    }
    update_seat_capabilities(server);
}

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *vkbd = data;
    server_add_keyboard(server, &vkbd->keyboard, true);
    update_seat_capabilities(server);
}

static void handle_request_cursor(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *ev = data;
    if (server->seat->pointer_state.focused_client == ev->seat_client) {
        wlr_cursor_set_surface(server->cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
    }
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *ev = data;
    wlr_seat_set_selection(server->seat, ev->source, ev->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *ev = data;
    wlr_seat_set_primary_selection(server->seat, ev->source, ev->serial);
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmServer *server = wl_container_of(listener, server, drag_destroy);
    wl_list_remove(&server->drag_destroy.link);
    server->drag_icon_tree = nullptr;
    if (server->focused_view) {
        struct wlr_surface *s = view_wlr_surface(server->focused_view);
        if (s) {
            focus_view(server, server->focused_view, s);
        }
    }
}

static void handle_start_drag(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, start_drag);
    struct wlr_drag *drag = data;
    if (drag->icon) {
        server->drag_icon_tree = wlr_scene_drag_icon_create(server->overlay_tree, drag->icon);
        if (server->drag_icon_tree) {
            wlr_scene_node_set_position(
                &server->drag_icon_tree->node, (int)server->cursor->x, (int)server->cursor->y
            );
        }
    }
    server->drag_destroy.notify = handle_drag_destroy;
    wl_signal_add(&drag->events.destroy, &server->drag_destroy);
}

static void handle_request_start_drag(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, request_start_drag);
    struct wlr_seat_request_start_drag_event *ev = data;
    if (wlr_seat_validate_pointer_grab_serial(server->seat, ev->origin, ev->serial)) {
        wlr_seat_start_pointer_drag(server->seat, ev->drag, ev->serial);
    } else {
        wlr_data_source_destroy(ev->drag->source);
    }
}

/* --- Outputs --- */
static void handle_output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmOutput *output = wl_container_of(listener, output, frame);
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
    if (!scene_output) return;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (wlr_scene_output_build_state(scene_output, &state, nullptr)) {
        wlr_output_commit_state(output->wlr_output, &state);
    }
    wlr_output_state_finish(&state);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void handle_output_request_state(struct wl_listener *listener, void *data) {
    DtwmOutput *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *ev = data;
    wlr_output_commit_state(output->wlr_output, ev->state);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmOutput *output = wl_container_of(listener, output, destroy);
    DtwmServer *server = output->server;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    if (output->backdrop_buf) {
        wlr_scene_node_destroy(&output->backdrop_buf->node);
    }
    if (output->bg_rect) {
        wlr_scene_node_destroy(&output->bg_rect->node);
    }
    if (output->lock_bg_rect) {
        wlr_scene_node_destroy(&output->lock_bg_rect->node);
    }
    free(output);
    dtwm_update_front_panel(server);
    dtwm_output_mgmt_sync(server);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    } else if (!wlr_output->current_mode) {
        wlr_output_state_set_custom_mode(&state, 1280, 900, 60000);
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    DtwmOutput *output = calloc(1, sizeof(*output));
    output->server = server;
    output->wlr_output = wlr_output;
    output->output_idx = server->next_output_idx++;
    output->dpi = (output->output_idx < 16 && server->initial_output_dpi[output->output_idx] > 0)
                      ? server->initial_output_dpi[output->output_idx]
                      : 96;

    output->frame.notify = handle_output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = handle_output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = handle_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    int next_x = 0;
    {
        DtwmOutput *prev_out;
        wl_list_for_each(prev_out, &server->outputs, link) {
            if (prev_out->wlr_output && prev_out->wlr_output->enabled) {
                struct wlr_box pbox = {0};
                wlr_output_layout_get_box(server->output_layout, prev_out->wlr_output, &pbox);
                if (pbox.x + pbox.width > next_x) {
                    next_x = pbox.x + pbox.width;
                }
            }
        }
    }

    wl_list_insert(server->outputs.prev, &output->link);

    struct wlr_output_layout_output *l_output =
        wlr_output_layout_add(server->output_layout, wlr_output, next_x, 0);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

    struct wlr_box obox;
    wlr_output_layout_get_box(server->output_layout, wlr_output, &obox);
    float bg[4] = {
        (float)server->palette.desktop_bg.r,
        (float)server->palette.desktop_bg.g,
        (float)server->palette.desktop_bg.b,
        1.0f,
    };
    output->bg_rect = wlr_scene_rect_create(server->bg_tree, obox.width, obox.height, bg);
    wlr_scene_node_set_position(&output->bg_rect->node, obox.x, obox.y);

    dtwm_update_output_backdrop(output);
    dtwm_update_front_panel(server);
    dtwm_icons_relayout(server);
    dtwm_sync_xwayland_randr_dpi(server);
    dtwm_lock_sync_outputs(server);
    dtwm_output_mgmt_sync(server);

    wlr_xcursor_manager_load(server->cursor_mgr, wlr_output->scale);
}

/* --- XDG Toplevels & Decorations --- */
static void place_initial_view(DtwmView *view) {
    DtwmServer *server = view->server;
    int idx = 0;
    DtwmView *it;
    wl_list_for_each(it, &server->views, link) {
        if (it != view && view_is_normal_app(it)) {
            idx++;
        }
    }
    struct wlr_box obox;
    dtwm_get_usable_output_box(server, server->cursor->x, server->cursor->y, &obox);
    view->x = obox.x + 48 + (idx % 6) * 36;
    view->y = obox.y + 40 + (idx % 6) * 32;
    if (view->frame_tree) {
        wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
    }
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, map);
    view->mapped = true;

    struct wlr_box geo = view->xdg_toplevel->base->geometry;
    view->client_w = geo.width > 0 ? geo.width : view->xdg_toplevel->base->surface->current.width;
    view->client_h = geo.height > 0 ? geo.height : view->xdg_toplevel->base->surface->current.height;
    place_initial_view(view);
    wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
    dtwm_view_update_frame(view);
    dtwm_icons_on_view_map(view);
    dtwm_foreign_toplevel_sync(view);
    if (dtwm_view_visible_on_current_ws(view)) {
        focus_view(view->server, view, view->xdg_toplevel->base->surface);
    }
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, unmap);
    if (view == view->server->grabbed_view) {
        hide_wireframe_outline(view->server);
        update_snap_preview(view->server, DTWM_SNAP_NONE, nullptr);
        view->server->cursor_mode = DTWM_CURSOR_PASSTHROUGH;
        view->server->grabbed_view = nullptr;
    }
    if (view == view->server->menu_target) {
        close_window_menu(view->server);
    }
    if (view == view->server->focused_view) {
        clear_active_focus(view->server);
    }
    view->mapped = false;
    view->has_pending_geom = false;
    view->pending_pos_set = false;
    wlr_scene_node_set_enabled(&view->frame_tree->node, false);
    dtwm_foreign_toplevel_destroy(view);
    dtwm_icons_on_view_unmap_or_destroy(view, false);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, commit);
    if (view->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_wm_capabilities(
            view->xdg_toplevel,
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU |
                WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
                WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE |
                WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN
        );
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
        return;
    }
    if (view->mapped) {
        struct wlr_box geo = view->xdg_toplevel->base->geometry;
        int nw = geo.width > 0 ? geo.width : view->xdg_toplevel->base->surface->current.width;
        int nh = geo.height > 0 ? geo.height : view->xdg_toplevel->base->surface->current.height;
        if (nw > 0 && nh > 0 && (nw != view->client_w || nh != view->client_h)) {
            view->client_w = nw;
            view->client_h = nh;
            dtwm_view_update_frame(view);
            if (view == view->server->focused_view) {
                dtwm_publish_active_window_box(view->server);
            }
        }
    }
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_title);
    if (view->mapped) {
        dtwm_view_update_frame(view);
        dtwm_icons_update_view(view);
        dtwm_foreign_toplevel_sync(view);
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, request_move);
    begin_interactive(view, DTWM_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    DtwmView *view = wl_container_of(listener, view, request_resize);
    struct wlr_xdg_toplevel_resize_event *ev = data;
    begin_interactive(view, DTWM_CURSOR_RESIZE, ev->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, request_maximize);
    if (view->xdg_toplevel->base->initialized) {
        view_set_maximized(view, view->xdg_toplevel->requested.maximized);
        wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
    }
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, request_minimize);
    view_set_minimized(view, true);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, destroy);
    DtwmServer *server = view->server;
    if (view == server->focused_view) {
        server->focused_view = nullptr;
    }
    if (view == server->menu_target) {
        close_window_menu(server);
    }
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->link);
    dtwm_foreign_toplevel_destroy(view);
    dtwm_icons_on_view_unmap_or_destroy(view, true);
    if (view->frame_tree) {
        wlr_scene_node_destroy(&view->frame_tree->node);
    }
    free(view);
}

static void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;

    DtwmView *view = calloc(1, sizeof(*view));
    view->server = server;
    view->kind = DTWM_VIEW_XDG;
    view->xdg_toplevel = toplevel;
    view->decorated = true;
    view->has_title = true;
    view->has_resize_handles = true;
    view->has_menu_btn = true;
    view->has_min_btn = true;
    view->has_max_btn = true;
    view->workspace_mask = 1u << server->current_workspace;

    view->frame_tree = wlr_scene_tree_create(server->workspace_tree);
    view->frame_tree->node.data = view;
    wlr_scene_node_set_enabled(&view->frame_tree->node, false);

    view->frame_buf = wlr_scene_buffer_create(view->frame_tree, nullptr);

    view->content_tree = wlr_scene_xdg_surface_create(view->frame_tree, toplevel->base);
    view->content_tree->node.data = view;
    toplevel->base->data = view->content_tree;

    struct wlr_surface *surf = toplevel->base->surface;
    view->map.notify = xdg_toplevel_map;
    wl_signal_add(&surf->events.map, &view->map);
    view->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&surf->events.unmap, &view->unmap);
    view->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&surf->events.commit, &view->commit);
    view->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&toplevel->events.destroy, &view->destroy);

    view->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&toplevel->events.request_move, &view->request_move);
    view->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
    view->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
    view->request_minimize.notify = xdg_toplevel_request_minimize;
    wl_signal_add(&toplevel->events.request_minimize, &view->request_minimize);
    view->set_title.notify = xdg_toplevel_set_title;
    wl_signal_add(&toplevel->events.set_title, &view->set_title);

    wl_list_insert(&server->views, &view->link);
}

/* XDG Popups */
static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmPopup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        struct wlr_box obox;
        get_primary_output_box(popup->server, popup->server->cursor->x, popup->server->cursor->y, &obox);
        wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &obox);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmPopup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_xdg_popup);
    struct wlr_xdg_popup *xdg_popup = data;

    DtwmPopup *popup = calloc(1, sizeof(*popup));
    popup->server = server;
    popup->xdg_popup = xdg_popup;

    struct wlr_scene_tree *parent_tree = server->popup_tree;
    if (xdg_popup->parent) {
        struct wlr_xdg_surface *parent_xdg = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
        if (parent_xdg && parent_xdg->data) {
            parent_tree = parent_xdg->data;
        }
    }
    popup->scene_tree = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    xdg_popup->base->data = popup->scene_tree;

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* XDG Server-Side Decorations (Negotiate authentic CDE 3D bevels for Wayland clients) */
static void decoration_request_mode(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmDecoration *dec = wl_container_of(listener, dec, request_mode);
    if (dec->decoration->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            dec->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
        );
    }
}

static void decoration_surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmDecoration *dec = wl_container_of(listener, dec, surface_commit);
    if (dec->decoration->toplevel->base->initial_commit) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            dec->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
        );
    }
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmDecoration *dec = wl_container_of(listener, dec, destroy);
    wl_list_remove(&dec->request_mode.link);
    wl_list_remove(&dec->surface_commit.link);
    wl_list_remove(&dec->destroy.link);
    free(dec);
}

static void handle_new_xdg_decoration(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_xdg_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *wlr_dec = data;

    DtwmDecoration *dec = calloc(1, sizeof(*dec));
    dec->server = server;
    dec->decoration = wlr_dec;
    dec->request_mode.notify = decoration_request_mode;
    wl_signal_add(&wlr_dec->events.request_mode, &dec->request_mode);
    dec->surface_commit.notify = decoration_surface_commit;
    wl_signal_add(&wlr_dec->toplevel->base->surface->events.commit, &dec->surface_commit);
    dec->destroy.notify = decoration_destroy;
    wl_signal_add(&wlr_dec->events.destroy, &dec->destroy);

    if (wlr_dec->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            wlr_dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
        );
    }
}

/* --- Layer Shell v1 --- */
static void arrange_layer_surface(DtwmLayerSurface *ls) {
    if (!ls->layer_surface->output) {
        return;
    }
    struct wlr_box full = {0};
    wlr_output_layout_get_box(ls->server->output_layout, ls->layer_surface->output, &full);
    struct wlr_box usable = full;
    wlr_scene_layer_surface_v1_configure(ls->scene_layer, &full, &usable);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmLayerSurface *ls = wl_container_of(listener, ls, commit);
    arrange_layer_surface(ls);
}

static void layer_surface_map(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmLayerSurface *ls = wl_container_of(listener, ls, map);
    arrange_layer_surface(ls);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    (void)listener;
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmLayerSurface *ls = wl_container_of(listener, ls, destroy);
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->unmap.link);
    wl_list_remove(&ls->commit.link);
    wl_list_remove(&ls->destroy.link);
    free(ls);
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *wlr_ls = data;

    if (!wlr_ls->output && !wl_list_empty(&server->outputs)) {
        DtwmOutput *first = wl_container_of(server->outputs.next, first, link);
        wlr_ls->output = first->wlr_output;
    }

    struct wlr_scene_tree *parent = server->layer_top_tree;
    switch (wlr_ls->pending.layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: parent = server->layer_bg_tree; break;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     parent = server->layer_bottom_tree; break;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        parent = server->layer_top_tree; break;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    parent = server->layer_overlay_tree; break;
    }

    DtwmLayerSurface *ls = calloc(1, sizeof(*ls));
    ls->server = server;
    ls->layer_surface = wlr_ls;
    ls->scene_layer = wlr_scene_layer_surface_v1_create(parent, wlr_ls);

    ls->map.notify = layer_surface_map;
    wl_signal_add(&wlr_ls->surface->events.map, &ls->map);
    ls->unmap.notify = layer_surface_unmap;
    wl_signal_add(&wlr_ls->surface->events.unmap, &ls->unmap);
    ls->commit.notify = layer_surface_commit;
    wl_signal_add(&wlr_ls->surface->events.commit, &ls->commit);
    ls->destroy.notify = layer_surface_destroy;
    wl_signal_add(&wlr_ls->events.destroy, &ls->destroy);
}

/* --- Rootless XWayland Integration (Honoring _MOTIF_WM_HINTS & WM_NORMAL_HINTS) --- */
static void apply_xwayland_decorations(DtwmView *view) {
    struct wlr_xwayland_surface *xs = view->xwayland_surface;
    if (!xs || xs->override_redirect) {
        view->decorated = false;
        view->has_title = false;
        view->has_resize_handles = false;
        view->has_menu_btn = false;
        view->has_min_btn = false;
        view->has_max_btn = false;
        return;
    }
    bool no_border = (xs->decorations & WLR_XWAYLAND_SURFACE_DECORATIONS_NO_BORDER) != 0;
    bool no_title = (xs->decorations & WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE) != 0;
    view->decorated = !(no_border && no_title);
    view->has_title = !no_title;
    view->has_resize_handles = !no_border;
    view->has_menu_btn = !no_title;
    view->has_min_btn = !no_title;
    view->has_max_btn = !no_title;

    /* Honor full 5-field _MOTIF_WM_HINTS (MWM_DECOR_ALL / MWM_FUNC_ALL subtraction semantics) */
    DtwmServer *server = view->server;
    if (server && server->xcb_conn && server->atom_mwm_hints != XCB_ATOM_NONE &&
        xs->window_id != XCB_WINDOW_NONE) {
        xcb_get_property_cookie_t ck = xcb_get_property(
            server->xcb_conn, 0, xs->window_id, server->atom_mwm_hints, XCB_GET_PROPERTY_TYPE_ANY, 0, 5
        );
        xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
        if (rep) {
            if (rep->format == 32 && xcb_get_property_value_length(rep) >= 12) {
                const uint32_t *h = (const uint32_t *)xcb_get_property_value(rep);
                uint32_t flags = h[0];
                uint32_t funcs = h[1];
                uint32_t decor = h[2];

                if (flags & 0x02u /* MWM_HINTS_DECORATIONS */) {
                    const uint32_t all_d = 0x02u | 0x04u | 0x08u | 0x10u | 0x20u | 0x40u;
                    uint32_t eff_d = (decor & 0x01u /* MWM_DECOR_ALL */)
                                         ? (all_d & ~(decor & ~0x01u))
                                         : decor;
                    view->decorated = (eff_d != 0);
                    view->has_title = (eff_d & 0x08u /* MWM_DECOR_TITLE */) != 0;
                    view->has_resize_handles = (eff_d & 0x04u /* MWM_DECOR_RESIZEH */) != 0;
                    view->has_menu_btn = view->has_title && ((eff_d & 0x10u /* MWM_DECOR_MENU */) != 0);
                    view->has_min_btn = view->has_title && ((eff_d & 0x20u /* MWM_DECOR_MINIMIZE */) != 0);
                    view->has_max_btn = view->has_title && ((eff_d & 0x40u /* MWM_DECOR_MAXIMIZE */) != 0);
                }
                if (flags & 0x01u /* MWM_HINTS_FUNCTIONS */) {
                    const uint32_t all_f = 0x02u | 0x04u | 0x08u | 0x10u | 0x20u;
                    uint32_t eff_f = (funcs & 0x01u /* MWM_FUNC_ALL */)
                                         ? (all_f & ~(funcs & ~0x01u))
                                         : funcs;
                    if (!(eff_f & 0x02u /* MWM_FUNC_RESIZE */)) {
                        view->has_resize_handles = false;
                    }
                    if (!(eff_f & 0x08u /* MWM_FUNC_MINIMIZE */)) {
                        view->has_min_btn = false;
                    }
                    if (!(eff_f & 0x10u /* MWM_FUNC_MAXIMIZE */)) {
                        view->has_max_btn = false;
                    }
                }
            }
            free(rep);
        }
    }

    /* Also suppress resize handles & maximize button on fixed-size WM_NORMAL_HINTS windows */
    if (xs->size_hints) {
        const uint32_t min_max = (uint32_t)(XCB_ICCCM_SIZE_HINT_P_MIN_SIZE | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE);
        if ((xs->size_hints->flags & min_max) == min_max &&
            xs->size_hints->min_width > 0 && xs->size_hints->min_height > 0 &&
            xs->size_hints->min_width == xs->size_hints->max_width &&
            xs->size_hints->min_height == xs->size_hints->max_height) {
            view->has_resize_handles = false;
            view->has_max_btn = false;
        }
    }
}

void dtwm_begin_move_panel(DtwmServer *server) {
    if (!server || !server->panel_view || !server->panel_view->mapped) return;
    begin_interactive(server->panel_view, DTWM_CURSOR_MOVE, 0);
}

static uint32_t read_dt_wm_behaviors_sync(DtwmServer *server, xcb_window_t win) {
    if (!server->xcb_conn || server->atom_dt_wm_hints == XCB_ATOM_NONE) return 0;
    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn, 0, win, server->atom_dt_wm_hints, server->atom_dt_wm_hints, 0, 8
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return 0;
    uint32_t behaviors = 0;
    if (rep->format == 32 && xcb_get_property_value_length(rep) >= 12) {
        const uint32_t *vals = (const uint32_t *)xcb_get_property_value(rep);
        /* DtWmHints: [0]=flags (DtWM_HINTS_BEHAVIORS=2), [1]=functions, [2]=behaviors */
        if (vals[0] & 2u) {
            behaviors = vals[2];
        }
    }
    free(rep);
    return behaviors;
}

static void xwayland_map(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, map);
    struct wlr_xwayland_surface *xs = view->xwayland_surface;
    view->mapped = true;

    bool had_pending_pos = view->pending_pos_set;
    apply_xwayland_decorations(view);
    if (!view->has_pending_geom || view->client_w <= 0 || view->client_h <= 0) {
        view->client_w = xs->width > 0 ? xs->width : 320;
        view->client_h = xs->height > 0 ? xs->height : 240;
    }
    view->has_pending_geom = false;
    view->pending_pos_set = false;

    if (dtwm_lock_is_sm_lock_window(view->server, xs->window_id)) {
        view->is_lock_dialog = true;
        view->decorated = !xs->override_redirect;
        view->has_title = false;
        view->has_resize_handles = false;
        view->has_menu_btn = false;
        view->has_min_btn = false;
        view->has_max_btn = false;
        view->x = xs->x;
        view->y = xs->y;
        view->frame_tree = wlr_scene_subsurface_tree_create(
            view->server->lock_dialog_tree, xs->surface
        );
        view->frame_tree->node.data = view;
        wlr_scene_node_set_position(&view->frame_tree->node, xs->x, xs->y);
        if (view->server->session_locked) {
            dtwm_lock_focus_surface(view->server, xs->surface);
        }
        return;
    }

    if (xs->override_redirect) {
        view->x = xs->x;
        view->y = xs->y;
        view->client_w = xs->width > 0 ? xs->width : 1;
        view->client_h = xs->height > 0 ? xs->height : 1;
        view->frame_tree = wlr_scene_subsurface_tree_create(
            view->server->unmanaged_xwayland_tree, xs->surface
        );
        view->frame_tree->node.data = view;
        wlr_scene_node_set_position(&view->frame_tree->node, xs->x, xs->y);
        wlr_scene_node_raise_to_top(&view->frame_tree->node);
        return;
    }

    uint32_t dt_behaviors = read_dt_wm_behaviors_sync(view->server, xs->window_id);
    if (dt_behaviors & (1u << 2)) {
        /* Genuine CDE Subpanel (DtWM_BEHAVIOR_SUBPANEL): starts docked & frameless
         * flush above the Front Panel, promoting to a Tool Window only when torn off! */
        view->is_subpanel = true;
        view->subpanel_torn_off = false;
        view->decorated = true;
        view->has_title = false;
        view->has_resize_handles = false;
        view->has_menu_btn = false;
        view->has_min_btn = false;
        view->has_max_btn = false;
        view->workspace_mask = (1u << DTWM_MAX_WORKSPACES) - 1u;

        view->frame_tree = wlr_scene_tree_create(view->server->layer_top_tree);
        view->frame_tree->node.data = view;
        view->frame_buf = wlr_scene_buffer_create(view->frame_tree, nullptr);
        view->content_tree = wlr_scene_subsurface_tree_create(view->frame_tree, xs->surface);
        view->content_tree->node.data = view;

        int outer_w = 0, outer_h = 0;
        dtwm_view_outer_size(view, &outer_w, &outer_h);
        view->x = xs->x;
        int py = (view->server->panel_h > 0) ? view->server->panel_y : 640;
        view->y = py - outer_h;
        if (view->y < 0) view->y = 0;
        wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
        dtwm_workspaces_on_xwayland_view_map(view);
        dtwm_icons_on_view_map(view);
        sync_xwayland_client_pos(view);
        dtwm_view_update_frame(view);
        focus_view(view->server, view, xs->surface);
        return;
    }
    if (dt_behaviors & (1u << 1)) {
        /* Genuine CDE Main Front Panel (DtWM_BEHAVIOR_PANEL) */
        view->is_panel = true;
        view->decorated = true;
        view->has_title = false;
        view->has_resize_handles = false;
        view->has_menu_btn = false;
        view->has_min_btn = false;
        view->has_max_btn = false;
        view->workspace_mask = (1u << DTWM_MAX_WORKSPACES) - 1u;
        view->server->panel_view = view;

        view->frame_tree = wlr_scene_tree_create(view->server->layer_top_tree);
        view->frame_tree->node.data = view;
        view->frame_buf = wlr_scene_buffer_create(view->frame_tree, nullptr);
        view->content_tree = wlr_scene_subsurface_tree_create(view->frame_tree, xs->surface);
        view->content_tree->node.data = view;

        dtwm_workspaces_on_xwayland_view_map(view);
        dtwm_icons_on_view_map(view);
        dtwm_update_front_panel(view->server);
        dtwm_icons_relayout(view->server);
        dtwm_notify_panel_mapped(view->server);
        return;
    }

    if (dtwm_icons_classify_xwayland_map(view)) {
        return;
    }

    view->frame_tree = wlr_scene_tree_create(view->server->workspace_tree);
    view->frame_tree->node.data = view;
    view->frame_buf = wlr_scene_buffer_create(view->frame_tree, nullptr);
    view->content_tree = wlr_scene_subsurface_tree_create(view->frame_tree, xs->surface);
    view->content_tree->node.data = view;

    if (view->server->greeter_mode) {
        struct wlr_box obox = {0};
        wlr_output_layout_get_box(view->server->output_layout, nullptr, &obox);
        if (obox.width > 0 && obox.height > 0 &&
            view->client_w >= obox.width && view->client_h >= obox.height) {
            /* dtgreet's full-screen login_shell maps frameless at (0, 0) */
            view->decorated = false;
            view->has_title = false;
            view->has_resize_handles = false;
            view->has_menu_btn = false;
            view->has_min_btn = false;
            view->has_max_btn = false;
            view->x = 0;
            view->y = 0;
        } else {
            view->x = xs->x;
            view->y = xs->y;
        }
    } else if (had_pending_pos) {
        /* Keep view->x / view->y recorded from pre-map ConfigureRequest */
    } else if (xs->x != 0 || xs->y != 0) {
        view->x = xs->x;
        view->y = xs->y;
    } else {
        place_initial_view(view);
    }
    wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
    dtwm_workspaces_on_xwayland_view_map(view);
    dtwm_icons_on_view_map(view);
    wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
    sync_xwayland_client_pos(view);
    dtwm_view_update_frame(view);
    dtwm_foreign_toplevel_sync(view);
    if (dtwm_view_visible_on_current_ws(view)) {
        focus_view(view->server, view, xs->surface);
    }
}

static void xwayland_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, unmap);
    if (view == view->server->grabbed_view) {
        hide_wireframe_outline(view->server);
        update_snap_preview(view->server, DTWM_SNAP_NONE, nullptr);
        view->server->cursor_mode = DTWM_CURSOR_PASSTHROUGH;
        view->server->grabbed_view = nullptr;
    }
    if (view == view->server->menu_target) {
        close_window_menu(view->server);
    }
    if (view == view->server->focused_view) {
        clear_active_focus(view->server);
    }
    view->mapped = false;
    view->has_pending_geom = false;
    view->pending_pos_set = false;
    if (view == view->server->panel_view) {
        view->server->panel_view = nullptr;
    }
    if (view == view->server->subpanel_drag_candidate) {
        view->server->subpanel_drag_candidate = nullptr;
    }
    view->is_panel = false;
    view->is_subpanel = false;
    view->subpanel_torn_off = false;
    view->is_desktop_pin = false;
    view->is_swallowed = false;
    dtwm_foreign_toplevel_destroy(view);
    dtwm_icons_on_view_unmap_or_destroy(view, false);
    if (view->frame_tree) {
        wlr_scene_node_destroy(&view->frame_tree->node);
        view->frame_tree = nullptr;
        view->content_tree = nullptr;
        view->frame_buf = nullptr;
    }
}

static void xwayland_associate(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, associate);
    if (!wl_list_empty(&view->map.link)) {
        wl_list_remove(&view->map.link);
    }
    if (!wl_list_empty(&view->unmap.link)) {
        wl_list_remove(&view->unmap.link);
    }
    view->map.notify = xwayland_map;
    wl_signal_add(&view->xwayland_surface->surface->events.map, &view->map);
    view->unmap.notify = xwayland_unmap;
    wl_signal_add(&view->xwayland_surface->surface->events.unmap, &view->unmap);
}

static void xwayland_dissociate(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, dissociate);
    if (!wl_list_empty(&view->map.link)) {
        wl_list_remove(&view->map.link);
        wl_list_init(&view->map.link);
    }
    if (!wl_list_empty(&view->unmap.link)) {
        wl_list_remove(&view->unmap.link);
        wl_list_init(&view->unmap.link);
    }
}

static void xwayland_request_configure(struct wl_listener *listener, void *data) {
    DtwmView *view = wl_container_of(listener, view, request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    struct wlr_xwayland_surface *xs = view->xwayland_surface;

    if (!view->mapped || xs->override_redirect || view->is_swallowed) {
        if (view->is_swallowed) {
            dtwm_icons_reposition_swallowed(view->server);
            return;
        }
        if (xs->override_redirect) {
            int16_t nx = (ev->mask & XCB_CONFIG_WINDOW_X) ? ev->x : xs->x;
            int16_t ny = (ev->mask & XCB_CONFIG_WINDOW_Y) ? ev->y : xs->y;
            uint16_t nw = (ev->mask & XCB_CONFIG_WINDOW_WIDTH)
                              ? ev->width
                              : (xs->width > 0 ? xs->width : 1);
            uint16_t nh = (ev->mask & XCB_CONFIG_WINDOW_HEIGHT)
                              ? ev->height
                              : (xs->height > 0 ? xs->height : 1);
            wlr_xwayland_surface_configure(xs, nx, ny, nw, nh);
            if (view->mapped && view->frame_tree) {
                wlr_scene_node_set_position(&view->frame_tree->node, nx, ny);
            }
            return;
        }
        int nx = (ev->mask & XCB_CONFIG_WINDOW_X)
                     ? ev->x
                     : (view->pending_pos_set ? view->x : xs->x);
        int ny = (ev->mask & XCB_CONFIG_WINDOW_Y)
                     ? ev->y
                     : (view->pending_pos_set ? view->y : xs->y);
        uint16_t nw = (ev->mask & XCB_CONFIG_WINDOW_WIDTH)
                          ? ev->width
                          : (uint16_t)((view->has_pending_geom && view->client_w > 0)
                                           ? view->client_w
                                           : (xs->width > 0 ? xs->width : 1));
        uint16_t nh = (ev->mask & XCB_CONFIG_WINDOW_HEIGHT)
                          ? ev->height
                          : (uint16_t)((view->has_pending_geom && view->client_h > 0)
                                           ? view->client_h
                                           : (xs->height > 0 ? xs->height : 1));
        if (ev->mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)) {
            view->x = nx;
            view->y = ny;
            view->pending_pos_set = true;
        }
        if (ev->mask & (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT)) {
            view->client_w = nw;
            view->client_h = nh;
            view->has_pending_geom = true;
        }
        wlr_xwayland_surface_configure(xs, (int16_t)nx, (int16_t)ny, nw, nh);
        return;
    }
    int l, t, r, b;
    dtwm_view_insets(view, &l, &t, &r, &b);
    int nx = (ev->mask & XCB_CONFIG_WINDOW_X) ? (ev->x - l) : view->x;
    int ny = (ev->mask & XCB_CONFIG_WINDOW_Y) ? (ev->y - t) : view->y;
    int nw = (ev->mask & XCB_CONFIG_WINDOW_WIDTH) ? ev->width : view->client_w;
    int nh = (ev->mask & XCB_CONFIG_WINDOW_HEIGHT) ? ev->height : view->client_h;
    view_apply_geometry(view, nx, ny, nw, nh);
}

static void xwayland_request_activate(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, request_activate);
    if (view->mapped && !view->xwayland_surface->override_redirect) {
        focus_view(view->server, view, view->xwayland_surface->surface);
    }
}

static void xwayland_request_close(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, request_close);
    view_close(view);
}

static void xwayland_request_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, request_maximize);
    bool want = view->xwayland_surface->maximized_horz && view->xwayland_surface->maximized_vert;
    view_set_maximized(view, want);
}

static void xwayland_request_minimize(struct wl_listener *listener, void *data) {
    DtwmView *view = wl_container_of(listener, view, request_minimize);
    struct wlr_xwayland_minimize_event *ev = data;
    view_set_minimized(view, ev->minimize);
}

static void xwayland_set_title(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_title);
    if (view->mapped) {
        dtwm_view_update_frame(view);
        dtwm_icons_update_view(view);
        dtwm_foreign_toplevel_sync(view);
    }
}

static void xwayland_set_decorations(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_decorations);
    if (view->is_swallowed || view->is_desktop_pin || view->is_panel || view->is_subpanel) return;
    apply_xwayland_decorations(view);
    if (view->mapped) {
        sync_xwayland_client_pos(view);
        dtwm_view_update_frame(view);
    }
}

static void xwayland_set_hints(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_hints);
    if (view->is_swallowed || view->is_desktop_pin || view->is_panel || view->is_subpanel) return;
    apply_xwayland_decorations(view);
    if (view->mapped) {
        sync_xwayland_client_pos(view);
        dtwm_view_update_frame(view);
        dtwm_icons_update_view(view);
    }
}

static void xwayland_set_geometry(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_geometry);
    struct wlr_xwayland_surface *xs = view->xwayland_surface;
    if (!xs) {
        return;
    }
    if (xs->override_redirect || view->is_lock_dialog) {
        view->x = xs->x;
        view->y = xs->y;
        if (xs->width > 0) view->client_w = xs->width;
        if (xs->height > 0) view->client_h = xs->height;
        if (view->mapped && view->frame_tree) {
            wlr_scene_node_set_position(&view->frame_tree->node, xs->x, xs->y);
            if (xs->override_redirect && !view->is_lock_dialog) {
                wlr_scene_node_raise_to_top(&view->frame_tree->node);
            }
        }
    }
}

static void xwayland_set_override_redirect(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_override_redirect);
    struct wlr_xwayland_surface *xs = view->xwayland_surface;
    if (!xs) {
        return;
    }
    apply_xwayland_decorations(view);
    if (view->mapped && xs->surface) {
        xwayland_unmap(&view->unmap, nullptr);
        xwayland_map(&view->map, nullptr);
    }
}

static void xwayland_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, destroy);
    DtwmServer *server = view->server;
    if (view == server->grabbed_view) {
        server->cursor_mode = DTWM_CURSOR_PASSTHROUGH;
        server->grabbed_view = nullptr;
    }
    if (view == server->focused_view) {
        server->focused_view = nullptr;
    }
    if (view == server->panel_view) {
        server->panel_view = nullptr;
    }
    if (view == server->menu_target) {
        close_window_menu(server);
    }
    if (!wl_list_empty(&view->map.link)) {
        wl_list_remove(&view->map.link);
        wl_list_init(&view->map.link);
    }
    if (!wl_list_empty(&view->unmap.link)) {
        wl_list_remove(&view->unmap.link);
        wl_list_init(&view->unmap.link);
    }
    wl_list_remove(&view->associate.link);
    wl_list_remove(&view->dissociate.link);
    wl_list_remove(&view->request_configure.link);
    wl_list_remove(&view->request_activate.link);
    wl_list_remove(&view->request_close.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_decorations.link);
    wl_list_remove(&view->set_hints.link);
    wl_list_remove(&view->set_geometry.link);
    wl_list_remove(&view->set_override_redirect.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->link);
    dtwm_foreign_toplevel_destroy(view);
    dtwm_icons_on_view_unmap_or_destroy(view, true);
    if (view->frame_tree) {
        wlr_scene_node_destroy(&view->frame_tree->node);
    }
    free(view);
}

static void handle_new_xwayland_surface(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface *xs = data;

    DtwmView *view = calloc(1, sizeof(*view));
    view->server = server;
    view->kind = DTWM_VIEW_XWAYLAND;
    view->xwayland_surface = xs;
    view->workspace_mask = 1u << server->current_workspace;
    wl_list_init(&view->map.link);
    wl_list_init(&view->unmap.link);
    apply_xwayland_decorations(view);

    view->associate.notify = xwayland_associate;
    wl_signal_add(&xs->events.associate, &view->associate);
    view->dissociate.notify = xwayland_dissociate;
    wl_signal_add(&xs->events.dissociate, &view->dissociate);
    view->request_configure.notify = xwayland_request_configure;
    wl_signal_add(&xs->events.request_configure, &view->request_configure);
    view->request_activate.notify = xwayland_request_activate;
    wl_signal_add(&xs->events.request_activate, &view->request_activate);
    view->request_close.notify = xwayland_request_close;
    wl_signal_add(&xs->events.request_close, &view->request_close);
    view->request_maximize.notify = xwayland_request_maximize;
    wl_signal_add(&xs->events.request_maximize, &view->request_maximize);
    view->request_minimize.notify = xwayland_request_minimize;
    wl_signal_add(&xs->events.request_minimize, &view->request_minimize);
    view->set_title.notify = xwayland_set_title;
    wl_signal_add(&xs->events.set_title, &view->set_title);
    view->set_decorations.notify = xwayland_set_decorations;
    wl_signal_add(&xs->events.set_decorations, &view->set_decorations);
    view->set_hints.notify = xwayland_set_hints;
    wl_signal_add(&xs->events.set_hints, &view->set_hints);
    view->set_geometry.notify = xwayland_set_geometry;
    wl_signal_add(&xs->events.set_geometry, &view->set_geometry);
    view->set_override_redirect.notify = xwayland_set_override_redirect;
    wl_signal_add(&xs->events.set_override_redirect, &view->set_override_redirect);
    view->destroy.notify = xwayland_destroy;
    wl_signal_add(&xs->events.destroy, &view->destroy);

    wl_list_insert(&server->views, &view->link);
}

static void write_status_file(DtwmServer *server) {
    if (server->status_written) {
        return;
    }
    if (!server->status_file || !server->wayland_socket) {
        server->status_written = true;
        return;
    }
    server->status_written = true;
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", server->status_file, (int)getpid());
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        return;
    }
    fprintf(fp, "WAYLAND_DISPLAY=%s\n", server->wayland_socket);
    if (server->xwayland && server->xwayland->display_name) {
        fprintf(fp, "DISPLAY=%s\n", server->xwayland->display_name);
    }
    fprintf(fp, "PID=%d\n", (int)getpid());
    fclose(fp);
    rename(tmp_path, server->status_file);
}

static int handle_status_fallback_timer(void *data) {
    DtwmServer *server = data;
    write_status_file(server);
    return 0;
}

void dtwm_notify_panel_mapped(DtwmServer *server) {
    if (!server) return;
    if (server->status_fallback_timer) {
        wl_event_source_remove(server->status_fallback_timer);
        server->status_fallback_timer = nullptr;
    }
    write_status_file(server);
}

static void register_cde_xwayland_font_path(DtwmServer *server) {
    if (!server || !server->xcb_conn) return;
    const char *alias_dir = CDE_INSTALLATION_TOP "/share/cde/fontaliases";
    char dir_file[512];
    snprintf(dir_file, sizeof(dir_file), "%s/fonts.dir", alias_dir);
    if (access(dir_file, R_OK) != 0) return;

    xcb_get_font_path_cookie_t gck = xcb_get_font_path(server->xcb_conn);
    xcb_get_font_path_reply_t *rep = xcb_get_font_path_reply(server->xcb_conn, gck, nullptr);
    if (!rep) return;

    size_t add_len = strlen(alias_dir);
    if (add_len == 0 || add_len > 255) {
        free(rep);
        return;
    }

    int qty = xcb_get_font_path_path_length(rep);
    xcb_str_iterator_t it = xcb_get_font_path_path_iterator(rep);
    size_t total_bytes = 0;
    for (; it.rem > 0; xcb_str_next(&it)) {
        int slen = xcb_str_name_length(it.data);
        const char *sname = xcb_str_name(it.data);
        if ((size_t)slen == add_len && memcmp(sname, alias_dir, add_len) == 0) {
            free(rep);
            return;
        }
        total_bytes += 1u + (size_t)slen;
    }

    size_t new_bytes = total_bytes + 1u + add_len;
    uint8_t *buf = calloc(1, new_bytes);
    if (buf) {
        size_t pos = 0;
        it = xcb_get_font_path_path_iterator(rep);
        for (; it.rem > 0; xcb_str_next(&it)) {
            uint8_t slen = (uint8_t)xcb_str_name_length(it.data);
            buf[pos++] = slen;
            memcpy(buf + pos, xcb_str_name(it.data), slen);
            pos += slen;
        }
        buf[pos++] = (uint8_t)add_len;
        memcpy(buf + pos, alias_dir, add_len);
        pos += add_len;

        xcb_void_cookie_t sck = xcb_set_font_path_checked(
            server->xcb_conn, (uint16_t)(qty + 1), (const xcb_str_t *)buf
        );
        free(xcb_request_check(server->xcb_conn, sck));
        free(buf);
    }
    free(rep);
}

static void handle_xwayland_ready(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmServer *server = wl_container_of(listener, server, xwayland_ready);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
    struct wlr_xcursor *xcur = wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1.0f);
    if (xcur && xcur->image_count > 0) {
        wlr_xwayland_set_cursor(
            server->xwayland,
            xcur->images[0]->buffer,
            xcur->images[0]->width * 4,
            xcur->images[0]->width,
            xcur->images[0]->height,
            (int32_t)xcur->images[0]->hotspot_x,
            (int32_t)xcur->images[0]->hotspot_y
        );
    }
    dtwm_workspaces_on_xwayland_ready(server);
    register_cde_xwayland_font_path(server);
    dtwm_icons_on_xwayland_ready(server);
    dtwm_xdnd_on_xwayland_ready(server);
    dtwm_lock_on_xwayland_ready(server);
    dtwm_output_mgmt_publish_x11(server);
    if (server->greeter_mode && server->xcb_conn) {
        /* Grant local root (dtlogin session manager, xrdb, dtgreet) access via
         * ServerInterpreted SI:localuser:root without opening XWayland to other users. */
        static const char si_root[] = "localuser\0root";
        xcb_void_cookie_t hck = xcb_change_hosts_checked(
            server->xcb_conn,
            XCB_HOST_MODE_INSERT,
            5 /* XCB_FAMILY_SERVER_INTERPRETED */,
            (uint16_t)(sizeof(si_root) - 1),
            (const uint8_t *)si_root
        );
        free(xcb_request_check(server->xcb_conn, hck));
    }
    if (server->show_front_panel && server->panel_pid > 0 && !server->status_written) {
        struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
        server->status_fallback_timer = wl_event_loop_add_timer(
            loop, handle_status_fallback_timer, server
        );
        wl_event_source_timer_update(server->status_fallback_timer, 3500);
    } else {
        write_status_file(server);
    }
}

static int handle_signal_quit(int sig_num, void *data) {
    (void)sig_num;
    struct wl_display *display = data;
    wl_display_terminate(display);
    return 0;
}

int main(int argc, char **argv) {
    const char *startup_cmd = nullptr;
    const char *status_file = nullptr;
    const char *swallow_client = nullptr;
    const char *profile_arg = nullptr;
    bool greeter_mode = false;
    bool no_panel = false;
    bool no_open_icons = false;
    bool large_icons = false;

    int opt_output_dpi[16] = {0};

    static const struct option long_opts[] = {
        {"startup", required_argument, nullptr, 's'},
        {"status-file", required_argument, nullptr, 'f'},
        {"swallow", required_argument, nullptr, 'w'},
        {"appearance-profile", required_argument, nullptr, 'A'},
        {"output-dpi", required_argument, nullptr, 'D'},
        {"session", no_argument, nullptr, 'S'},
        {"greeter", no_argument, nullptr, 'g'},
        {"no-panel", no_argument, nullptr, 'P'},
        {"no-open-window-icons", no_argument, nullptr, 'I'},
        {"large-icons", no_argument, nullptr, 'L'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "s:f:w:A:D:SgPILh", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 's': startup_cmd = optarg; break;
        case 'f': status_file = optarg; break;
        case 'w': swallow_client = optarg; break;
        case 'A': profile_arg = optarg; break;
        case 'D': {
            unsigned int oidx = 0;
            int odpi = 96;
            if (sscanf(optarg, "%u=%d", &oidx, &odpi) == 2 && oidx < 16 && odpi >= 1 && odpi <= 4096) {
                opt_output_dpi[oidx] = odpi;
            }
            break;
        }
        case 'S': break;
        case 'g': greeter_mode = true; break;
        case 'P': no_panel = true; break;
        case 'I': no_open_icons = true; break;
        case 'L': large_icons = true; break;
        case 'h':
            printf(
                "Usage: %s [--session] [--greeter] [--no-panel] [--no-open-window-icons] "
                "[--large-icons] [--appearance-profile NAME] [--swallow NAME] "
                "[--status-file PATH] [--startup CMD]\n",
                argv[0]
            );
            return 0;
        default:
            return 2;
        }
    }

    wlr_log_init(WLR_INFO, nullptr);

    DtwmServer server = {0};
    memcpy(server.initial_output_dpi, opt_output_dpi, sizeof(opt_output_dpi));
    server.status_file = status_file;
    {
        char resolved[PATH_MAX];
        if (argv[0] && realpath(argv[0], resolved)) {
            char *slash = strrchr(resolved, '/');
            if (slash) {
                *slash = '\0';
                snprintf(server.exe_dir, sizeof(server.exe_dir), "%s", resolved);
            }
        }
    }
    dtwm_palette_init_default(&server.palette);
    wl_list_init(&server.outputs);
    wl_list_init(&server.views);
    wl_list_init(&server.keyboards);

    server.wl_display = wl_display_create();
    if (!server.wl_display) {
        return 1;
    }

    struct wl_event_loop *ev_loop = wl_display_get_event_loop(server.wl_display);
    server.sigint_source = wl_event_loop_add_signal(ev_loop, SIGINT, handle_signal_quit, server.wl_display);
    server.sigterm_source = wl_event_loop_add_signal(ev_loop, SIGTERM, handle_signal_quit, server.wl_display);

    server.backend = wlr_backend_autocreate(ev_loop, &server.wlr_session);
    if (!server.backend) {
        fprintf(stderr, "dtwm-wayland: failed to create wlroots backend\n");
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        fprintf(stderr, "dtwm-wayland: failed to create wlroots renderer\n");
        return 1;
    }
    wlr_renderer_init_wl_display(server.renderer, server.wl_display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator) {
        fprintf(stderr, "dtwm-wayland: failed to create wlroots allocator\n");
        return 1;
    }

    server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);
    wlr_primary_selection_v1_device_manager_create(server.wl_display);
    wlr_viewporter_create(server.wl_display);
    wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
    wlr_screencopy_manager_v1_create(server.wl_display);

    server.output_layout = wlr_output_layout_create(server.wl_display);
    wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    /* Ordered scene-graph layers */
    server.bg_tree = wlr_scene_tree_create(&server.scene->tree);
    server.layer_bg_tree = wlr_scene_tree_create(&server.scene->tree);
    server.layer_bottom_tree = wlr_scene_tree_create(&server.scene->tree);
    server.desktop_icon_tree = wlr_scene_tree_create(&server.scene->tree);
    server.workspace_tree = wlr_scene_tree_create(&server.scene->tree);
    server.layer_top_tree = wlr_scene_tree_create(&server.scene->tree);
    server.panel_swallow_tree = wlr_scene_tree_create(&server.scene->tree);
    server.unmanaged_xwayland_tree = wlr_scene_tree_create(&server.scene->tree);
    server.popup_tree = wlr_scene_tree_create(&server.scene->tree);
    server.overlay_tree = wlr_scene_tree_create(&server.scene->tree);
    server.layer_overlay_tree = wlr_scene_tree_create(&server.scene->tree);

    server.snap_preview_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.snap_preview_buf->node, false);
    server.wireframe_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.wireframe_buf->node, false);
    server.anim_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.anim_buf->node, false);
    server.anim_timer = wl_event_loop_add_timer(ev_loop, handle_anim_timer, &server);
    server.menu_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.menu_buf->node, false);
    server.switcher_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.switcher_buf->node, false);

    server.move_opaque = true;
    server.focus_follows_mouse = false;
    server.focus_auto_raise = true;
    server.allow_primary_on_top = false;
    server.animate_minimize = true;
    server.show_task_switcher = true;
    server.double_click_ms = 500;
    server.cursor_size = 24;

    dtwm_workspaces_init(&server);
    dtwm_icons_init(&server);
    dtwm_lock_init(&server);
    dtwm_output_mgmt_init(&server);
    dtwm_ime_init(&server);
    server.greeter_mode = greeter_mode;
    if (greeter_mode || no_panel) {
        server.show_front_panel = false;
    }
    if (greeter_mode || no_open_icons) {
        server.show_open_window_icons = false;
    }
    if (large_icons) {
        server.large_icons = true;
    }
    if (swallow_client && swallow_client[0]) {
        snprintf(server.swallow_client_name, sizeof(server.swallow_client_name), "%s", swallow_client);
    }
    if (profile_arg && profile_arg[0]) {
        dtwm_server_apply_appearance_profile(&server, dtwm_parse_appearance_profile(profile_arg));
    }

    server.new_output.notify = handle_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 5);
    server.new_xdg_toplevel.notify = handle_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = handle_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    server.xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(server.wl_display);
    server.new_xdg_decoration.notify = handle_new_xdg_decoration;
    wl_signal_add(&server.xdg_decoration_mgr->events.new_toplevel_decoration, &server.new_xdg_decoration);

    server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
    server.new_layer_surface.notify = handle_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);
    wlr_xcursor_manager_load(server.cursor_mgr, 1.0f);

    server.cursor_motion.notify = handle_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = handle_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = handle_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = handle_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = handle_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    server.seat = wlr_seat_create(server.wl_display, "seat0");
    wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    server.new_input.notify = handle_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    server.virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(server.wl_display);
    server.new_virtual_pointer.notify = handle_new_virtual_pointer;
    wl_signal_add(&server.virtual_pointer_mgr->events.new_virtual_pointer, &server.new_virtual_pointer);

    server.virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(server.wl_display);
    server.new_virtual_keyboard.notify = handle_new_virtual_keyboard;
    wl_signal_add(&server.virtual_keyboard_mgr->events.new_virtual_keyboard, &server.new_virtual_keyboard);

    server.request_cursor.notify = handle_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);
    server.request_set_primary_selection.notify = handle_request_set_primary_selection;
    wl_signal_add(&server.seat->events.request_set_primary_selection, &server.request_set_primary_selection);
    server.request_start_drag.notify = handle_request_start_drag;
    wl_signal_add(&server.seat->events.request_start_drag, &server.request_start_drag);
    server.start_drag.notify = handle_start_drag;
    wl_signal_add(&server.seat->events.start_drag, &server.start_drag);

    server.xwayland = wlr_xwayland_create(server.wl_display, server.compositor, false);
    if (server.xwayland) {
        server.new_xwayland_surface.notify = handle_new_xwayland_surface;
        wl_signal_add(&server.xwayland->events.new_surface, &server.new_xwayland_surface);
        server.xwayland_ready.notify = handle_xwayland_ready;
        wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
        setenv("DISPLAY", server.xwayland->display_name, 1);
    }

    int exit_code = 0;
    const char *socket = wl_display_add_socket_auto(server.wl_display);
    if (!socket) {
        exit_code = 1;
        goto cleanup;
    }
    server.wayland_socket = socket;

    if (!wlr_backend_start(server.backend)) {
        exit_code = 1;
        goto cleanup;
    }

    setenv("WAYLAND_DISPLAY", socket, 1);
    if (!server.xwayland) {
        write_status_file(&server);
    }
    fprintf(
        stderr,
        "dtwm-wayland: running on WAYLAND_DISPLAY=%s DISPLAY=%s\n",
        socket,
        server.xwayland ? server.xwayland->display_name : "(none)"
    );

    if (startup_cmd) {
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (char *)nullptr);
            _exit(127);
        }
    }

    wl_display_run(server.wl_display);

cleanup:
    if (server.anim_timer) wl_event_source_remove(server.anim_timer);
    if (server.status_fallback_timer) wl_event_source_remove(server.status_fallback_timer);
    if (server.sigint_source) wl_event_source_remove(server.sigint_source);
    if (server.sigterm_source) wl_event_source_remove(server.sigterm_source);
    if (server.xwayland) {
        wl_list_remove(&server.new_xwayland_surface.link);
        wl_list_remove(&server.xwayland_ready.link);
        wlr_xwayland_destroy(server.xwayland);
        server.xwayland = nullptr;
    }
    wl_display_destroy_clients(server.wl_display);
    dtwm_ime_finish(&server);
    dtwm_output_mgmt_finish(&server);
    dtwm_lock_finish(&server);
    dtwm_xdnd_finish();
    dtwm_icons_finish(&server);
    dtwm_workspaces_finish(&server);
    wl_list_remove(&server.cursor_motion.link);
    wl_list_remove(&server.cursor_motion_absolute.link);
    wl_list_remove(&server.cursor_button.link);
    wl_list_remove(&server.cursor_axis.link);
    wl_list_remove(&server.cursor_frame.link);
    wl_list_remove(&server.request_cursor.link);
    wl_list_remove(&server.request_set_selection.link);
    wl_list_remove(&server.request_set_primary_selection.link);
    wl_list_remove(&server.request_start_drag.link);
    wl_list_remove(&server.start_drag.link);
    wl_list_remove(&server.new_virtual_pointer.link);
    wl_list_remove(&server.new_virtual_keyboard.link);
    wl_list_remove(&server.new_input.link);
    wl_list_remove(&server.new_output.link);
    wl_list_remove(&server.new_xdg_toplevel.link);
    wl_list_remove(&server.new_xdg_popup.link);
    wl_list_remove(&server.new_xdg_decoration.link);
    wl_list_remove(&server.new_layer_surface.link);
    wlr_backend_destroy(server.backend);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wl_display_destroy(server.wl_display);
    return exit_code;
}
