/*
 * CDE - Common Desktop Environment
 * dtwm_wayland_main.c: wlroots-0.19 compositor core with server-side CDE 3D
 * bevel decorations, rootless XWayland, window menu, snap preview, Shift-grid
 * alignment, and keyboard task switcher.
 */

#include "dtwm_wayland.h"

#include <getopt.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wlr/util/log.h>
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
static void focus_view(DtwmServer *server, DtwmView *view, struct wlr_surface *surface);
static void view_set_maximized(DtwmView *view, bool maximized);
static void view_set_minimized(DtwmView *view, bool minimized);
static void view_close(DtwmView *view);
static void view_apply_geometry(DtwmView *view, int x, int y, int client_w, int client_h);

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
    }
    wlr_seat_keyboard_notify_clear_focus(server->seat);
}

static void focus_view(DtwmServer *server, DtwmView *view, struct wlr_surface *surface) {
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

    DtwmView *prev = server->focused_view;
    if (prev && prev != view) {
        prev->active = false;
        if (prev->kind == DTWM_VIEW_XDG && prev->xdg_toplevel && prev->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
        } else if (prev->kind == DTWM_VIEW_XWAYLAND && prev->xwayland_surface) {
            wlr_xwayland_surface_activate(prev->xwayland_surface, false);
        }
        dtwm_view_update_frame(prev);
    }

    /* Raise in Z-order and move to head of server->views MRU list */
    if (view->frame_tree) {
        wlr_scene_node_raise_to_top(&view->frame_tree->node);
    }
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);

    server->focused_view = view;
    view->active = true;
    if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel && view->xdg_toplevel->base->initialized) {
        wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
    } else if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        wlr_xwayland_surface_activate(view->xwayland_surface, true);
        wlr_xwayland_surface_restack(view->xwayland_surface, nullptr, XCB_STACK_MODE_ABOVE);
    }
    dtwm_view_update_frame(view);

    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (kbd) {
        wlr_seat_keyboard_notify_enter(
            server->seat, surface, kbd->keycodes, kbd->num_keycodes, &kbd->modifiers
        );
    } else {
        wlr_seat_keyboard_notify_enter(server->seat, surface, nullptr, 0, nullptr);
    }
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
}

static void view_set_minimized(DtwmView *view, bool minimized) {
    if (view->minimized == minimized) {
        return;
    }
    view->minimized = minimized;
    if (view->frame_tree) {
        wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
    }
    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        wlr_xwayland_surface_set_minimized(view->xwayland_surface, minimized);
    }
    if (minimized && view->server->focused_view == view) {
        clear_active_focus(view->server);
        /* Focus next visible view on current workspace if any */
        DtwmView *cand;
        wl_list_for_each(cand, &view->server->views, link) {
            if (cand != view && dtwm_view_visible_on_current_ws(cand)) {
                focus_view(view->server, cand, nullptr);
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

/* --- CDE Window Menu Overlay --- */
static void update_window_menu_buffer(DtwmServer *server) {
    if (!server->menu_visible || !server->menu_buf) {
        return;
    }
    int mw = 0, mh = 0;
    struct wlr_buffer *buf = dtwm_render_window_menu(
        &server->palette, server->menu_target, server->menu_hover_idx, &mw, &mh
    );
    if (buf) {
        wlr_scene_buffer_set_buffer(server->menu_buf, buf);
        wlr_buffer_drop(buf);
    }
    wlr_scene_node_set_position(&server->menu_buf->node, server->menu_x, server->menu_y);
    wlr_scene_node_set_enabled(&server->menu_buf->node, true);
}

static void open_window_menu(DtwmServer *server, DtwmView *view, int x, int y) {
    server->menu_visible = true;
    server->menu_target = view;
    server->menu_x = x;
    server->menu_y = y;
    server->menu_hover_idx = -1;
    update_window_menu_buffer(server);
}

static void close_window_menu(DtwmServer *server) {
    server->menu_visible = false;
    server->menu_target = nullptr;
    server->menu_hover_idx = -1;
    if (server->menu_buf) {
        wlr_scene_buffer_set_buffer(server->menu_buf, nullptr);
        wlr_scene_node_set_enabled(&server->menu_buf->node, false);
    }
}

static int menu_item_at(DtwmServer *server, double lx, double ly) {
    int rx = (int)lx - server->menu_x;
    int ry = (int)ly - server->menu_y;
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

static void begin_interactive(DtwmView *view, DtwmCursorMode mode, uint32_t edges) {
    DtwmServer *server = view->server;
    close_window_menu(server);
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
    server->resize_edges = edges;
}

static void execute_menu_action(DtwmServer *server, int idx) {
    DtwmView *v = server->menu_target;
    close_window_menu(server);
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
        break;
    case 2: /* Size */
        begin_interactive(v, DTWM_CURSOR_RESIZE, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
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

static void show_or_cycle_task_switcher(DtwmServer *server) {
    int count = 0;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (dtwm_view_visible_on_current_ws(v)) count++;
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
            if (dtwm_view_visible_on_current_ws(v)) count++;
        }
        if (count > 0) {
            int target = server->switcher_index % count;
            int idx = 0;
            wl_list_for_each(v, &server->views, link) {
                if (!dtwm_view_visible_on_current_ws(v)) continue;
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
        server->grab_x = server->cursor->x;
        server->grab_y = server->cursor->y;
    }
    int nx = server->grab_orig_box.x + (int)(server->cursor->x - server->grab_x);
    int ny = server->grab_orig_box.y + (int)(server->cursor->y - server->grab_y);
    if (shift_is_held(server)) {
        nx = align_to_grid(nx);
        ny = align_to_grid(ny);
    }
    v->x = nx;
    v->y = ny;
    wlr_scene_node_set_position(&v->frame_tree->node, v->x, v->y);
    sync_xwayland_client_pos(v);

    struct wlr_box sbox = {0};
    DtwmSnapZone zone = compute_snap_zone(server, server->cursor->x, server->cursor->y, &sbox);
    update_snap_preview(server, zone, &sbox);
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
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_h += (int)dy;
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_x += (int)dx;
        new_w -= (int)dx;
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_w += (int)dx;
    }

    if (shift_is_held(server)) {
        new_w = align_to_grid(new_w);
        new_h = align_to_grid(new_h);
    }
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
    if (server->menu_visible) {
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

    double sx = 0, sy = 0;
    struct wlr_surface *surface = nullptr;
    DtwmFramePart part = DTWM_PART_NONE;
    DtwmView *view = desktop_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy, &part);

    if (!surface) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, view ? cursor_name_for_part(part) : "default");
        wlr_seat_pointer_notify_clear_focus(server->seat);
    } else {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
    }
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *ev = data;
    wlr_cursor_move(server->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    process_cursor_motion(server, ev->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *ev = data;
    wlr_cursor_warp_absolute(server->cursor, &ev->pointer->base, ev->x, ev->y);
    process_cursor_motion(server, ev->time_msec);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *ev = data;

    if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (server->cursor_mode == DTWM_CURSOR_MOVE && server->grabbed_view && server->active_snap != DTWM_SNAP_NONE) {
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
        }
        if (server->cursor_mode != DTWM_CURSOR_PASSTHROUGH) {
            server->cursor_mode = DTWM_CURSOR_PASSTHROUGH;
            server->grabbed_view = nullptr;
            update_snap_preview(server, DTWM_SNAP_NONE, nullptr);
            return;
        }
        wlr_seat_pointer_notify_button(server->seat, ev->time_msec, ev->button, ev->state);
        return;
    }

    /* Button press */
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

    if (dtwm_front_panel_click(server, server->cursor->x, server->cursor->y, ev->button)) {
        return;
    }

    double sx = 0, sy = 0;
    struct wlr_surface *surface = nullptr;
    DtwmFramePart part = DTWM_PART_NONE;
    DtwmView *view = desktop_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy, &part);

    if (!view && !surface) {
        /* Clicking empty CDE desktop clears active window focus */
        clear_active_focus(server);
        return;
    }

    if (view) {
        focus_view(server, view, surface);

        if (part != DTWM_PART_CLIENT) {
            if (ev->button == BTN_RIGHT && (part == DTWM_PART_TITLE || part == DTWM_PART_BTN_MENU)) {
                open_window_menu(server, view, (int)server->cursor->x, (int)server->cursor->y);
                return;
            }
            if (ev->button == BTN_LEFT) {
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
                    if (server->last_title_click_view == view &&
                        (ev->time_msec - server->last_title_click_ms) <= 400) {
                        server->last_title_click_view = nullptr;
                        view_set_maximized(view, !view->maximized);
                    } else {
                        server->last_title_click_view = view;
                        server->last_title_click_ms = ev->time_msec;
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

    wlr_seat_pointer_notify_button(server->seat, ev->time_msec, ev->button, ev->state);
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *ev = data;
    wlr_seat_pointer_notify_axis(
        server->seat, ev->time_msec, ev->orientation, ev->delta,
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
    wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);

    if (kb->server->switcher_visible) {
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
        show_or_cycle_task_switcher(server);
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
        if (server->menu_visible) {
            close_window_menu(server);
            return true;
        }
        if (server->switcher_visible) {
            hide_task_switcher(server, false);
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
        wlr_seat_keyboard_notify_key(server->seat, ev->time_msec, ev->keycode, ev->state);
    }
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmKeyboard *kb = wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->link);
    free(kb);
}

static void server_add_keyboard(DtwmServer *server, struct wlr_keyboard *wlr_kbd) {
    DtwmKeyboard *kb = calloc(1, sizeof(*kb));
    if (!kb) return;
    kb->server = server;
    kb->wlr_keyboard = wlr_kbd;

    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap) {
        wlr_keyboard_set_keymap(wlr_kbd, keymap);
        xkb_keymap_unref(keymap);
    }
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(wlr_kbd, 25, 600);

    kb->modifiers.notify = handle_keyboard_modifiers;
    wl_signal_add(&wlr_kbd->events.modifiers, &kb->modifiers);
    kb->key.notify = handle_keyboard_key;
    wl_signal_add(&wlr_kbd->events.key, &kb->key);
    kb->destroy.notify = handle_keyboard_destroy;
    wl_signal_add(&wlr_kbd->base.events.destroy, &kb->destroy);

    wlr_seat_set_keyboard(server->seat, wlr_kbd);
    wl_list_insert(&server->keyboards, &kb->link);
}

static void update_seat_capabilities(DtwmServer *server) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *dev = data;
    switch (dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_add_keyboard(server, wlr_keyboard_from_input_device(dev));
        break;
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, dev);
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
    server_add_keyboard(server, &vkbd->keyboard);
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
    free(output);
    dtwm_update_front_panel(server);
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

    output->frame.notify = handle_output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = handle_output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = handle_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(server->outputs.prev, &output->link);

    struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
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

    wlr_xcursor_manager_load(server->cursor_mgr, wlr_output->scale);
}

/* --- XDG Toplevels & Decorations --- */
static void place_initial_view(DtwmView *view) {
    DtwmServer *server = view->server;
    int idx = 0;
    DtwmView *it;
    wl_list_for_each(it, &server->views, link) {
        if (it != view && dtwm_view_visible_on_current_ws(it)) idx++;
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
    if (dtwm_view_visible_on_current_ws(view)) {
        focus_view(view->server, view, view->xdg_toplevel->base->surface);
    }
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, unmap);
    if (view == view->server->grabbed_view) {
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
    wlr_scene_node_set_enabled(&view->frame_tree->node, false);
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
        }
    }
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_title);
    if (view->mapped) {
        dtwm_view_update_frame(view);
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
    if (view == view->server->focused_view) {
        view->server->focused_view = nullptr;
    }
    if (view == view->server->menu_target) {
        close_window_menu(view->server);
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

/* --- Rootless XWayland Integration (Honoring _MOTIF_WM_HINTS) --- */
static void apply_xwayland_decorations(DtwmView *view) {
    struct wlr_xwayland_surface *xs = view->xwayland_surface;
    if (!xs || xs->override_redirect) {
        view->decorated = false;
        view->has_title = false;
        view->has_resize_handles = false;
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
}

static void xwayland_map(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, map);
    struct wlr_xwayland_surface *xs = view->xwayland_surface;
    view->mapped = true;

    apply_xwayland_decorations(view);
    view->client_w = xs->width > 0 ? xs->width : 320;
    view->client_h = xs->height > 0 ? xs->height : 240;

    if (xs->override_redirect) {
        view->frame_tree = wlr_scene_subsurface_tree_create(
            view->server->unmanaged_xwayland_tree, xs->surface
        );
        view->frame_tree->node.data = view;
        wlr_scene_node_set_position(&view->frame_tree->node, xs->x, xs->y);
        return;
    }

    view->frame_tree = wlr_scene_tree_create(view->server->workspace_tree);
    view->frame_tree->node.data = view;
    view->frame_buf = wlr_scene_buffer_create(view->frame_tree, nullptr);
    view->content_tree = wlr_scene_subsurface_tree_create(view->frame_tree, xs->surface);
    view->content_tree->node.data = view;

    if (xs->x != 0 || xs->y != 0) {
        view->x = xs->x;
        view->y = xs->y;
    } else {
        place_initial_view(view);
    }
    wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
    dtwm_workspaces_on_xwayland_view_map(view);
    wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
    sync_xwayland_client_pos(view);
    dtwm_view_update_frame(view);
    if (dtwm_view_visible_on_current_ws(view)) {
        focus_view(view->server, view, xs->surface);
    }
}

static void xwayland_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, unmap);
    if (view == view->server->grabbed_view) {
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
    view->map.notify = xwayland_map;
    wl_signal_add(&view->xwayland_surface->surface->events.map, &view->map);
    view->unmap.notify = xwayland_unmap;
    wl_signal_add(&view->xwayland_surface->surface->events.unmap, &view->unmap);
}

static void xwayland_dissociate(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, dissociate);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
}

static void xwayland_request_configure(struct wl_listener *listener, void *data) {
    DtwmView *view = wl_container_of(listener, view, request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    struct wlr_xwayland_surface *xs = view->xwayland_surface;

    if (!view->mapped || xs->override_redirect) {
        wlr_xwayland_surface_configure(xs, ev->x, ev->y, ev->width, ev->height);
        if (view->mapped && xs->override_redirect && view->frame_tree) {
            wlr_scene_node_set_position(&view->frame_tree->node, ev->x, ev->y);
        }
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
    }
}

static void xwayland_set_decorations(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_decorations);
    apply_xwayland_decorations(view);
    if (view->mapped) {
        sync_xwayland_client_pos(view);
        dtwm_view_update_frame(view);
    }
}

static void xwayland_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, destroy);
    if (view == view->server->focused_view) {
        view->server->focused_view = nullptr;
    }
    if (view == view->server->menu_target) {
        close_window_menu(view->server);
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
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->link);
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
    view->destroy.notify = xwayland_destroy;
    wl_signal_add(&xs->events.destroy, &view->destroy);

    wl_list_insert(&server->views, &view->link);
}

static void write_status_file(DtwmServer *server) {
    if (!server->status_file || !server->wayland_socket) {
        return;
    }
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
    write_status_file(server);
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
    bool greeter_mode = false;
    bool no_panel = false;

    static const struct option long_opts[] = {
        {"startup", required_argument, nullptr, 's'},
        {"status-file", required_argument, nullptr, 'f'},
        {"session", no_argument, nullptr, 'S'},
        {"greeter", no_argument, nullptr, 'g'},
        {"no-panel", no_argument, nullptr, 'P'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "s:f:SgPh", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 's': startup_cmd = optarg; break;
        case 'f': status_file = optarg; break;
        case 'S': break;
        case 'g': greeter_mode = true; break;
        case 'P': no_panel = true; break;
        case 'h':
            printf("Usage: %s [--session] [--greeter] [--no-panel] [--status-file PATH] [--startup CMD]\n", argv[0]);
            return 0;
        default:
            return 2;
        }
    }

    wlr_log_init(WLR_INFO, nullptr);

    DtwmServer server = {0};
    server.status_file = status_file;
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
    server.workspace_tree = wlr_scene_tree_create(&server.scene->tree);
    server.unmanaged_xwayland_tree = wlr_scene_tree_create(&server.scene->tree);
    server.layer_top_tree = wlr_scene_tree_create(&server.scene->tree);
    server.popup_tree = wlr_scene_tree_create(&server.scene->tree);
    server.overlay_tree = wlr_scene_tree_create(&server.scene->tree);
    server.layer_overlay_tree = wlr_scene_tree_create(&server.scene->tree);

    server.snap_preview_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.snap_preview_buf->node, false);
    server.menu_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.menu_buf->node, false);
    server.switcher_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.switcher_buf->node, false);

    dtwm_workspaces_init(&server);
    if (greeter_mode || no_panel) {
        server.show_front_panel = false;
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
    wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER);

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

    server.xwayland = wlr_xwayland_create(server.wl_display, server.compositor, false);
    if (server.xwayland) {
        server.new_xwayland_surface.notify = handle_new_xwayland_surface;
        wl_signal_add(&server.xwayland->events.new_surface, &server.new_xwayland_surface);
        server.xwayland_ready.notify = handle_xwayland_ready;
        wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
        setenv("DISPLAY", server.xwayland->display_name, 1);
    }

    const char *socket = wl_display_add_socket_auto(server.wl_display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        return 1;
    }
    server.wayland_socket = socket;

    if (!wlr_backend_start(server.backend)) {
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.wl_display);
        return 1;
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

    if (server.sigint_source) wl_event_source_remove(server.sigint_source);
    if (server.sigterm_source) wl_event_source_remove(server.sigterm_source);
    dtwm_workspaces_finish(&server);
    if (server.xwayland) wlr_xwayland_destroy(server.xwayland);
    wl_display_destroy_clients(server.wl_display);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 0;
}
