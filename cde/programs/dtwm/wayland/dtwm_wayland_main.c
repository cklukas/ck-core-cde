/*
 * CDE - Common Desktop Environment
 * dtwm_wayland_main.c: wlroots-0.19 compositor core with server-side CDE 3D
 * bevel decorations, rootless XWayland, window menu, snap preview, Shift-grid
 * alignment, and keyboard task switcher.
 */

#include "dtwm_wayland.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__has_include)
#if __has_include(<xf86drm.h>)
#include <xf86drm.h>
#define DTWM_HAVE_XF86DRM_H 1
#if __has_include(<virtgpu_drm.h>)
#include <virtgpu_drm.h>
#define DTWM_HAVE_VIRTGPU_DRM_H 1
#elif __has_include(<libdrm/virtgpu_drm.h>)
#include <libdrm/virtgpu_drm.h>
#define DTWM_HAVE_VIRTGPU_DRM_H 1
#endif
#endif
#endif

#include <wlr/util/log.h>
#include <xcb/xcb_icccm.h>
#include <xkbcommon/xkbcommon.h>

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    DtwmView *owner_view;
    struct wlr_xdg_popup *xdg_popup;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener commit;
    struct wl_listener reposition;
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

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_pointer *wlr_pointer;
    uint32_t held_mask;
    uint32_t held_since_ms;
    bool is_virtual;
    struct wl_listener destroy;
} DtwmPointer;

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_pointer_constraint_v1 *constraint;
    struct wl_listener set_region;
    struct wl_listener destroy;
} DtwmPointerConstraint;

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    struct wl_listener destroy;
} DtwmShortcutsInhibitor;

static uint32_t pointer_button_bit(uint32_t btn) {
    if (btn >= BTN_MOUSE && btn < BTN_MOUSE + 16u) {
        return 1u << (btn - BTN_MOUSE);
    }
    return 1u << 16u;
}

static uint32_t pointer_bit_to_button(unsigned int bit_idx) {
    if (bit_idx < 16u) {
        return BTN_MOUSE + bit_idx;
    }
    return BTN_LEFT;
}

static bool any_device_button_held(const DtwmServer *server) {
    if (!server) return false;
    const DtwmPointer *ptr;
    wl_list_for_each(ptr, &server->pointers, link) {
        if (ptr->held_mask != 0) {
            return true;
        }
    }
    return false;
}

static void close_window_menu(DtwmServer *server);
static void hide_task_switcher(DtwmServer *server, bool activate_selected);
static void hide_wireframe_outline(DtwmServer *server);
static void focus_view_ex(DtwmServer *server, DtwmView *view, struct wlr_surface *surface, bool raise_window);
static void focus_view(DtwmServer *server, DtwmView *view, struct wlr_surface *surface);
static void view_set_maximized(DtwmView *view, bool maximized);
static void view_set_minimized(DtwmView *view, bool minimized);
static void view_close(DtwmView *view);
static void view_apply_geometry(DtwmView *view, int x, int y, int client_w, int client_h);
static void process_cursor_motion(DtwmServer *server, uint32_t time_msec);
static void show_hardware_osd(
    DtwmServer *server, const char *kind, int level, bool muted, const char *label
);
static void sync_input_capture_for_focused_view(DtwmServer *server, bool show_osd);
static void deactivate_active_pointer_constraint(DtwmServer *server, bool apply_hint);
static DtwmView *find_view_owning_surface(const DtwmServer *server, struct wlr_surface *surf);
static DtwmView *surface_global_origin_and_scale(
    const DtwmServer *server,
    struct wlr_surface *surf,
    double *out_gx,
    double *out_gy,
    double *out_scale
);

static uint32_t dtwm_now_msec(void) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t ms = (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L));
    return ms ? ms : 1u;
}

static int align_to_grid(int v, int grid_sz) {
    if (grid_sz < 2) grid_sz = DTWM_GRID_SIZE;
    int rem = v % grid_sz;
    if (rem < 0) {
        rem += grid_sz;
    }
    return (rem >= grid_sz / 2) ? (v + (grid_sz - rem)) : (v - rem);
}

static bool wl_client_is_native_dpi(const DtwmServer *server, struct wl_client *client) {
    if (!server || !client) return false;
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, nullptr, nullptr);
    if (pid <= 0) return false;
    for (size_t i = 0; i < server->native_dpi_pid_count; ++i) {
        if (server->native_dpi_pids[i] == pid) {
            return true;
        }
    }
    return false;
}

static double external_view_target_scale(const DtwmView *view) {
    if (!view || !view->server || view->kind != DTWM_VIEW_XDG || view->is_native_dpi) {
        return 1.0;
    }
    double rx = view->mapped ? ((double)view->x + (double)view->client_w * 0.5)
                             : (view->server->cursor ? view->server->cursor->x : 0.0);
    double ry = view->mapped ? ((double)view->y + (double)view->client_h * 0.5)
                             : (view->server->cursor ? view->server->cursor->y : 0.0);
    int dpi = dtwm_output_dpi_at(view->server, rx, ry);
    double s = (double)(dpi > 0 ? dpi : 96) / 96.0;
    if (s < 0.5) s = 0.5;
    if (s > 4.0) s = 4.0;
    return s;
}

static void notify_one_surface_scale_iter(
    struct wlr_surface *surface, int sx, int sy, void *data
) {
    (void)sx;
    (void)sy;
    const double *sp = data;
    if (!surface || !sp) return;
    double s = *sp;
    wlr_fractional_scale_v1_notify_scale(surface, s);
    int bscale = (int)ceil(s - 1e-4);
    if (bscale < 1) bscale = 1;
    wlr_surface_set_preferred_buffer_scale(surface, bscale);
}

static void notify_surface_tree_scale(struct wlr_surface *root_surf, double scale) {
    if (!root_surf) return;
    wlr_surface_for_each_surface(root_surf, notify_one_surface_scale_iter, &scale);
}

static wlr_scene_buffer_point_accepts_input_func_t g_orig_scene_surface_accepts_input = nullptr;

static bool external_surface_point_accepts_input(
    struct wlr_scene_buffer *sbuf, double *sx, double *sy
) {
    if (!sbuf || !sx || !sy) return false;
    struct wlr_scene_surface *ssurf = wlr_scene_surface_try_from_buffer(sbuf);
    if (!ssurf || !ssurf->surface) return false;

    double s = 1.0;
    struct wlr_scene_tree *tree = sbuf->node.parent;
    while (tree && !tree->node.data) {
        tree = tree->node.parent;
    }
    if (tree && tree->node.data) {
        const DtwmView *v = tree->node.data;
        if (v->kind == DTWM_VIEW_XDG && !v->is_native_dpi && v->external_client_scale > 0.25) {
            s = v->external_client_scale;
        }
    }

    double lsx = (*sx) / s;
    double lsy = (*sy) / s;
    if (g_orig_scene_surface_accepts_input) {
        if (!g_orig_scene_surface_accepts_input(sbuf, &lsx, &lsy)) {
            return false;
        }
    } else if (!wlr_surface_point_accepts_input(ssurf->surface, lsx, lsy)) {
        return false;
    }
    *sx = lsx;
    *sy = lsy;
    return true;
}

static void sync_external_scene_tree_recursive(
    struct wlr_scene_tree *tree, double scale, int max_log_w, int max_log_h, bool is_root_toplevel
) {
    if (!tree) return;
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link) {
        if (node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
            struct wlr_scene_surface *ssurf = wlr_scene_surface_try_from_buffer(sbuf);
            if (ssurf && ssurf->surface) {
                while (!wl_list_empty(&sbuf->events.outputs_update.listener_list)) {
                    struct wl_list *elm = sbuf->events.outputs_update.listener_list.next;
                    wl_list_remove(elm);
                    wl_list_init(elm);
                }
                if (sbuf->point_accepts_input &&
                    sbuf->point_accepts_input != external_surface_point_accepts_input &&
                    !g_orig_scene_surface_accepts_input) {
                    g_orig_scene_surface_accepts_input = sbuf->point_accepts_input;
                }
                sbuf->point_accepts_input = external_surface_point_accepts_input;

                int lw = ssurf->surface->current.width;
                int lh = ssurf->surface->current.height;
                struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(ssurf->surface);
                if (sub && node->parent) {
                    wlr_scene_node_set_position(
                        &node->parent->node,
                        (int)lround((double)sub->current.x * scale),
                        (int)lround((double)sub->current.y * scale)
                    );
                } else if (is_root_toplevel) {
                    if (max_log_w > 0 && lw > max_log_w) lw = max_log_w;
                    if (max_log_h > 0 && lh > max_log_h) lh = max_log_h;
                }
                if (lw > 0 && lh > 0) {
                    int dw = (int)lround((double)lw * scale);
                    int dh = (int)lround((double)lh * scale);
                    if (dw < 1) dw = 1;
                    if (dh < 1) dh = 1;
                    wlr_scene_buffer_set_dest_size(sbuf, dw, dh);
                }
            }
        } else if (node->type == WLR_SCENE_NODE_TREE) {
            struct wlr_scene_tree *child_tree = wlr_scene_tree_from_node(node);
            sync_external_scene_tree_recursive(
                child_tree, scale, max_log_w, max_log_h, is_root_toplevel
            );
        }
    }
}

void dtwm_sync_external_view_scene(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XDG || view->is_native_dpi || !view->xdg_toplevel) {
        return;
    }
    double s = view->external_client_scale > 0.25 ? view->external_client_scale : 1.0;
    int max_log_w = 0, max_log_h = 0;
    if (view->decorated && view->client_w > 0 && view->client_h > 0) {
        max_log_w = (int)lround((double)view->client_w / s);
        max_log_h = (int)lround((double)view->client_h / s);
    }
    if (view->content_tree) {
        struct wlr_box geo = view->xdg_toplevel->base->geometry;
        if (!wl_list_empty(&view->content_tree->children)) {
            struct wlr_scene_node *first =
                wl_container_of(view->content_tree->children.next, first, link);
            if (first->type == WLR_SCENE_NODE_TREE) {
                wlr_scene_node_set_position(
                    first,
                    -(int)lround((double)geo.x * s),
                    -(int)lround((double)geo.y * s)
                );
            }
        }
        sync_external_scene_tree_recursive(view->content_tree, s, max_log_w, max_log_h, true);
    }
    if (view->server) {
        DtwmPopup *popup;
        wl_list_for_each(popup, &view->server->popups, link) {
            if (popup->owner_view == view && popup->scene_tree && popup->xdg_popup) {
                struct wlr_box pgeo = popup->xdg_popup->current.geometry;
                int px = pgeo.x;
                int py = pgeo.y;
                if (popup->xdg_popup->parent) {
                    struct wlr_xdg_surface *pxdg =
                        wlr_xdg_surface_try_from_wlr_surface(popup->xdg_popup->parent);
                    if (pxdg && pxdg->role == WLR_XDG_SURFACE_ROLE_POPUP && pxdg->popup) {
                        px += pxdg->popup->current.geometry.x - pxdg->geometry.x;
                        py += pxdg->popup->current.geometry.y - pxdg->geometry.y;
                    } else if (pxdg) {
                        px -= pxdg->geometry.x;
                        py -= pxdg->geometry.y;
                    }
                }
                wlr_scene_node_set_position(
                    &popup->scene_tree->node,
                    (int)lround((double)px * s),
                    (int)lround((double)py * s)
                );
                if (!wl_list_empty(&popup->scene_tree->children)) {
                    struct wlr_scene_node *pfirst =
                        wl_container_of(popup->scene_tree->children.next, pfirst, link);
                    if (pfirst->type == WLR_SCENE_NODE_TREE) {
                        struct wlr_box bgeo = popup->xdg_popup->base->geometry;
                        wlr_scene_node_set_position(
                            pfirst,
                            -(int)lround((double)bgeo.x * s),
                            -(int)lround((double)bgeo.y * s)
                        );
                    }
                }
                sync_external_scene_tree_recursive(popup->scene_tree, s, 0, 0, false);
            }
        }
    }
}

static uint32_t send_xdg_toplevel_size_for_view(DtwmView *view, int phys_w, int phys_h) {
    if (!view || !view->xdg_toplevel) return 0;
    int lw = phys_w;
    int lh = phys_h;
    if (!view->is_native_dpi && view->external_client_scale > 0.25) {
        if (phys_w > 0) {
            lw = (int)lround((double)phys_w / view->external_client_scale);
            if (lw < 1) lw = 1;
        }
        if (phys_h > 0) {
            lh = (int)lround((double)phys_h / view->external_client_scale);
            if (lh < 1) lh = 1;
        }
    }
    return wlr_xdg_toplevel_set_size(view->xdg_toplevel, lw, lh);
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

static void view_size_limits(
    const DtwmView *v,
    int *out_min_w,
    int *out_min_h,
    int *out_max_w,
    int *out_max_h
) {
    DtwmMetrics m = v ? dtwm_metrics_for_view(v) : dtwm_metrics_for_dpi(96);
    int min_w = (int)lround(64.0 * m.scale);
    int min_h = (int)lround(48.0 * m.scale);
    int max_w = 0;
    int max_h = 0;
    if (v && !v->is_panel && !v->is_subpanel && !v->is_swallowed && !v->is_desktop_pin) {
        if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface && v->xwayland_surface->size_hints) {
            const xcb_size_hints_t *sh = v->xwayland_surface->size_hints;
            if ((sh->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) != 0) {
                if (sh->min_width > min_w) min_w = sh->min_width;
                if (sh->min_height > min_h) min_h = sh->min_height;
            }
            if ((sh->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) != 0) {
                if (sh->max_width > 0) max_w = sh->max_width;
                if (sh->max_height > 0) max_h = sh->max_height;
            }
        } else if (v->kind == DTWM_VIEW_XDG && v->xdg_toplevel) {
            double ext_s = (!v->is_native_dpi && v->external_client_scale > 0.25)
                               ? v->external_client_scale
                               : 1.0;
            int cmin_w = (int)lround((double)v->xdg_toplevel->current.min_width * ext_s);
            int cmin_h = (int)lround((double)v->xdg_toplevel->current.min_height * ext_s);
            int cmax_w = (int)lround((double)v->xdg_toplevel->current.max_width * ext_s);
            int cmax_h = (int)lround((double)v->xdg_toplevel->current.max_height * ext_s);
            if (cmin_w > min_w) min_w = cmin_w;
            if (cmin_h > min_h) min_h = cmin_h;
            if (cmax_w > 0) max_w = cmax_w;
            if (cmax_h > 0) max_h = cmax_h;
        }
    }
    if (max_w > 0 && max_w < min_w) max_w = min_w;
    if (max_h > 0 && max_h < min_h) max_h = min_h;
    *out_min_w = min_w;
    *out_min_h = min_h;
    *out_max_w = max_w;
    *out_max_h = max_h;
}

static void view_apply_geometry(DtwmView *view, int x, int y, int client_w, int client_h) {
    int min_w = 64, min_h = 48, max_w = 0, max_h = 0;
    view_size_limits(view, &min_w, &min_h, &max_w, &max_h);
    if (client_w < min_w) client_w = min_w;
    if (client_h < min_h) client_h = min_h;
    if (!view || !view->maximized) {
        if (max_w > 0 && client_w > max_w) client_w = max_w;
        if (max_h > 0 && client_h > max_h) client_h = max_h;
    }
    if (x == view->x && y == view->y &&
        client_w == view->client_w && client_h == view->client_h) {
        return;
    }
    view->x = x;
    view->y = y;
    view->client_w = client_w;
    view->client_h = client_h;

    if (view->frame_tree) {
        wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
    }
    if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel) {
        view->pending_resize_serial =
            send_xdg_toplevel_size_for_view(view, client_w, client_h);
    } else if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        sync_xwayland_client_pos(view);
    }
    dtwm_view_update_frame(view);
    if (view == view->server->focused_view) {
        dtwm_publish_active_window_box(view->server);
        dtwm_ime_set_focus(view->server, view_wlr_surface(view));
    }
}

static void view_send_xdg_interactive_resize(DtwmView *view, int client_w, int client_h) {
    int min_w = 64, min_h = 48, max_w = 0, max_h = 0;
    view_size_limits(view, &min_w, &min_h, &max_w, &max_h);
    if (client_w < min_w) client_w = min_w;
    if (client_h < min_h) client_h = min_h;
    if (!view || !view->maximized) {
        if (max_w > 0 && client_w > max_w) client_w = max_w;
        if (max_h > 0 && client_h > max_h) client_h = max_h;
    }
    if (client_w == view->client_w && client_h == view->client_h &&
        view->pending_resize_serial == 0) {
        view->resize_inflight = false;
        return;
    }
    view->pending_resize_serial =
        send_xdg_toplevel_size_for_view(view, client_w, client_h);
}

static void view_dispatch_queued_resize(DtwmView *view, bool keep_inflight) {
    if (!view || !view->has_queued_resize) {
        return;
    }
    int qx = view->queued_x;
    int qy = view->queued_y;
    int qw = view->queued_w;
    int qh = view->queued_h;
    view->has_queued_resize = false;
    if (qx == view->x && qy == view->y && qw == view->client_w && qh == view->client_h &&
        view->pending_resize_serial == 0) {
        view->resize_inflight = false;
        return;
    }
    view->resize_inflight = keep_inflight;
    if (keep_inflight) {
        view->resize_sent_ms = dtwm_now_msec();
    }
    if (view->kind == DTWM_VIEW_XDG && view->mapped && view->xdg_toplevel &&
        view->active_resize_edges != 0) {
        view_send_xdg_interactive_resize(view, qw, qh);
    } else {
        view_apply_geometry(view, qx, qy, qw, qh);
    }
}

static int handle_resize_flush_timer(void *data) {
    DtwmServer *server = data;
    if (!server) {
        return 0;
    }
    uint32_t now_ms = dtwm_now_msec();
    uint32_t next_wait_ms = 0;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (!v->mapped) {
            continue;
        }
        if (v->has_queued_resize) {
            uint32_t elapsed = now_ms - v->resize_sent_ms;
            if (!v->resize_inflight || elapsed >= 250u) {
                view_dispatch_queued_resize(v, true);
            } else {
                uint32_t rem = 250u - elapsed;
                if (next_wait_ms == 0 || rem < next_wait_ms) {
                    next_wait_ms = rem;
                }
            }
        } else if (v->resize_inflight && (now_ms - v->resize_sent_ms) >= 250u) {
            v->resize_inflight = false;
            v->pending_resize_serial = 0;
        }
    }
    if (next_wait_ms > 0 && server->resize_flush_timer) {
        wl_event_source_timer_update(server->resize_flush_timer, (int)next_wait_ms);
    }
    return 0;
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
        dtwm_icons_refresh_focus_indicators(server);
        dtwm_foreign_toplevel_sync(prev);
    }
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    dtwm_ime_set_focus(server, nullptr);
    dtwm_publish_active_window_box(server);
    sync_input_capture_for_focused_view(server, false);
}

static uint64_t dtwm_now_nsec(void) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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
    dtwm_icon_box_deactivate(server);
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
    if (prev && prev != view && server->input_escaped_view == prev) {
        server->input_escaped_view = nullptr;
    }
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
    dtwm_icons_refresh_focus_indicators(server);
    dtwm_foreign_toplevel_sync(view);
    dtwm_publish_active_window_box(server);

    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    dtwm_a11y_notify_keyboard_enter(server, surface, kbd);
    dtwm_ime_set_focus(server, surface);
    sync_input_capture_for_focused_view(server, true);
}

static void focus_view(DtwmServer *server, DtwmView *view, struct wlr_surface *surface) {
    focus_view_ex(server, view, surface, true);
}

DtwmAnimStyle dtwm_parse_anim_style(const char *val) {
    if (!val) return DTWM_ANIM_STYLE_AUTO;
    while (*val && isspace((unsigned char)*val)) val++;
    char buf[32] = {0};
    size_t i = 0;
    while (val[i] && !isspace((unsigned char)val[i]) && i + 1 < sizeof(buf)) {
        buf[i] = (char)tolower((unsigned char)val[i]);
        i++;
    }
    buf[i] = '\0';

    if (strcmp(buf, "none") == 0 || strcmp(buf, "false") == 0 ||
        strcmp(buf, "off") == 0 || strcmp(buf, "0") == 0) {
        return DTWM_ANIM_STYLE_NONE;
    }
    if (strcmp(buf, "bevel") == 0 || strcmp(buf, "bevel_morph") == 0 ||
        strcmp(buf, "bevel_glide") == 0 || strcmp(buf, "bevelmorph") == 0 ||
        strcmp(buf, "bevelglide") == 0) {
        return DTWM_ANIM_STYLE_BEVEL;
    }
    if (strcmp(buf, "titlebar") == 0 || strcmp(buf, "titlebar_shuttle") == 0 ||
        strcmp(buf, "titlebarshuttle") == 0) {
        return DTWM_ANIM_STYLE_TITLEBAR;
    }
    if (strcmp(buf, "wireframe") == 0) {
        return DTWM_ANIM_STYLE_WIREFRAME;
    }
    return DTWM_ANIM_STYLE_AUTO;
}

DtwmAnimSpeed dtwm_parse_anim_speed(const char *val) {
    if (!val) return DTWM_ANIM_SPEED_STANDARD;
    while (*val && isspace((unsigned char)*val)) val++;
    char buf[32] = {0};
    size_t i = 0;
    while (val[i] && !isspace((unsigned char)val[i]) && i + 1 < sizeof(buf)) {
        char ch = (char)tolower((unsigned char)val[i]);
        buf[i] = (ch == '-') ? '_' : ch;
        i++;
    }
    buf[i] = '\0';

    if (strcmp(buf, "very_quick") == 0 || strcmp(buf, "veryquick") == 0 ||
        strcmp(buf, "fast") == 0 || strcmp(buf, "quick") == 0) {
        return DTWM_ANIM_SPEED_VERY_QUICK;
    }
    if (strcmp(buf, "slow") == 0 || strcmp(buf, "slower") == 0) {
        return DTWM_ANIM_SPEED_SLOW;
    }
    return DTWM_ANIM_SPEED_STANDARD;
}

static double anim_smoothstep(double edge0, double edge1, double x) {
    if (edge1 <= edge0) {
        return (x >= edge1) ? 1.0 : 0.0;
    }
    double t = (x - edge0) / (edge1 - edge0);
    if (!(t >= 0.0)) t = 0.0;
    if (t > 1.0) t = 1.0;
    double s = t * t * (3.0 - 2.0 * t);
    if (!(s >= 0.0)) return 0.0;
    if (s > 1.0) return 1.0;
    return s;
}

static float clamp_anim_opacity(double a) {
    if (!(a >= 0.0)) return 0.0f;
    if (a >= 1.0) return 1.0f;
    return (float)a;
}

/* Compute an aspect-matched center crop box guaranteed to satisfy both wlroots
 * wlr_scene_buffer_set_source_box (x,y,width,height >= 0) and renderer texture
 * bounds (x + width <= tex_w, y + height <= tex_h) without FMA 1-ULP overshoot.
 * Returns false when the full uncropped buffer should be sampled (nullptr). */
static bool compute_anim_client_source_box(
    int tex_w,
    int tex_h,
    int in_w,
    int in_h,
    double crop_blend,
    struct wlr_fbox *out_box
) {
    if (!out_box || tex_w <= 8 || tex_h <= 8 || in_w <= 0 || in_h <= 0 || !(crop_blend > 0.001)) {
        return false;
    }
    if (crop_blend > 1.0) {
        crop_blend = 1.0;
    }
    double sw = (double)tex_w;
    double sh = (double)tex_h;
    double dst_aspect = (double)in_w / (double)in_h;
    double src_aspect = sw / sh;
    double cw = sw;
    double ch = sh;
    if (src_aspect > dst_aspect) {
        double crop_w = fmin(sw, sh * dst_aspect);
        cw = sw - (sw - crop_w) * crop_blend;
    } else if (src_aspect < dst_aspect) {
        double crop_h = fmin(sh, sw / dst_aspect);
        ch = sh - (sh - crop_h) * crop_blend;
    } else {
        return false;
    }

    if (!(cw >= 1.0)) cw = 1.0;
    if (cw > sw) cw = sw;
    if (!(ch >= 1.0)) ch = 1.0;
    if (ch > sh) ch = sh;

    double sx = (sw > cw) ? ((sw - cw) * 0.5) : 0.0;
    double sy = (sh > ch) ? ((sh - ch) * 0.5) : 0.0;
    if (!(sx >= 0.0) || sx >= sw) sx = 0.0;
    if (!(sy >= 0.0) || sy >= sh) sy = 0.0;

    if (sx + cw > sw) {
        cw = sw - sx;
        if (sx + cw > sw) {
            cw = nextafter(cw, 0.0);
        }
    }
    if (sy + ch > sh) {
        ch = sh - sy;
        if (sy + ch > sh) {
            ch = nextafter(ch, 0.0);
        }
    }
    if (!(cw > 0.0) || !(ch > 0.0)) {
        return false;
    }
    *out_box = (struct wlr_fbox){
        .x = sx,
        .y = sy,
        .width = cw,
        .height = ch,
    };
    return true;
}

static int anim_latch_duration_ms(DtwmAnimSpeed speed) {
    switch (speed) {
    case DTWM_ANIM_SPEED_VERY_QUICK:
        return 32;
    case DTWM_ANIM_SPEED_SLOW:
        return 70;
    case DTWM_ANIM_SPEED_STANDARD:
    default:
        return 48;
    }
}

static int anim_flight_duration_ms(DtwmAnimSpeed speed, DtwmAnimStyle style, DtwmAnimKind kind) {
    bool is_max = (kind == DTWM_ANIM_KIND_MAXIMIZE || kind == DTWM_ANIM_KIND_UNMAXIMIZE);
    switch (speed) {
    case DTWM_ANIM_SPEED_VERY_QUICK:
        if (is_max) {
            return (style == DTWM_ANIM_STYLE_TITLEBAR) ? 90 : 95;
        }
        if (kind == DTWM_ANIM_KIND_MINIMIZE) {
            return (style == DTWM_ANIM_STYLE_BEVEL)
                       ? 125
                       : ((style == DTWM_ANIM_STYLE_TITLEBAR) ? 105 : 110);
        }
        return (style == DTWM_ANIM_STYLE_BEVEL)
                   ? 120
                   : ((style == DTWM_ANIM_STYLE_TITLEBAR) ? 105 : 110);

    case DTWM_ANIM_SPEED_SLOW:
        if (is_max) {
            return (style == DTWM_ANIM_STYLE_TITLEBAR) ? 320 : 350;
        }
        if (kind == DTWM_ANIM_KIND_MINIMIZE) {
            return (style == DTWM_ANIM_STYLE_BEVEL) ? 460 : 400;
        }
        return (style == DTWM_ANIM_STYLE_BEVEL) ? 430 : 380;

    case DTWM_ANIM_SPEED_STANDARD:
    default:
        if (is_max) {
            return (style == DTWM_ANIM_STYLE_TITLEBAR) ? 185 : 200;
        }
        if (kind == DTWM_ANIM_KIND_MINIMIZE) {
            return (style == DTWM_ANIM_STYLE_BEVEL) ? 260 : 225;
        }
        return (style == DTWM_ANIM_STYLE_BEVEL) ? 240 : 215;
    }
}

DtwmAnimStyle dtwm_resolve_anim_style(const DtwmServer *server, bool is_maximize) {
    if (!server || server->greeter_mode || server->session_locked || server->reduced_motion) {
        return DTWM_ANIM_STYLE_NONE;
    }
    DtwmAnimStyle raw = is_maximize ? server->maximize_anim_style : server->minimize_anim_style;
    if (raw != DTWM_ANIM_STYLE_AUTO) {
        return raw;
    }
    if (!is_maximize && !server->legacy_animate_minimize) {
        return DTWM_ANIM_STYLE_NONE;
    }
    /* Wayland default for both minimize/restore and maximize/unmaximize is Bevel */
    return DTWM_ANIM_STYLE_BEVEL;
}

static void schedule_all_outputs_frame(DtwmServer *server) {
    if (!server) return;
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->wlr_output && out->wlr_output->enabled) {
            wlr_output_schedule_frame(out->wlr_output);
        }
    }
}

static struct wlr_buffer *lock_view_client_buffer(DtwmView *view) {
    if (!view) return nullptr;
    struct wlr_surface *surf = view_wlr_surface(view);
    if (!surf || !surf->buffer || !surf->buffer->texture) return nullptr;
    return wlr_buffer_lock(&surf->buffer->base);
}

static void clear_anim_scene_nodes(DtwmServer *server) {
    if (!server) return;
    if (server->anim_trail_buf) {
        wlr_scene_buffer_set_buffer(server->anim_trail_buf, nullptr);
        wlr_scene_node_set_enabled(&server->anim_trail_buf->node, false);
    }
    if (server->anim_client_buf) {
        wlr_scene_buffer_set_buffer(server->anim_client_buf, nullptr);
        wlr_scene_buffer_set_source_box(server->anim_client_buf, nullptr);
        wlr_scene_buffer_set_dest_size(server->anim_client_buf, 0, 0);
        wlr_scene_buffer_set_opacity(server->anim_client_buf, 1.0f);
        wlr_scene_node_set_enabled(&server->anim_client_buf->node, false);
    }
    if (server->anim_icon_buf) {
        wlr_scene_buffer_set_buffer(server->anim_icon_buf, nullptr);
        wlr_scene_buffer_set_dest_size(server->anim_icon_buf, 0, 0);
        wlr_scene_buffer_set_opacity(server->anim_icon_buf, 1.0f);
        wlr_scene_node_set_enabled(&server->anim_icon_buf->node, false);
    }
    if (server->anim_frame_buf) {
        wlr_scene_buffer_set_buffer(server->anim_frame_buf, nullptr);
        wlr_scene_node_set_enabled(&server->anim_frame_buf->node, false);
    }
    if (server->anim_tree) {
        wlr_scene_node_set_enabled(&server->anim_tree->node, false);
    }
}

static void finish_or_cancel_anim(DtwmServer *server, bool allow_latch) {
    if (!server || server->anim_kind == DTWM_ANIM_KIND_NONE) {
        return;
    }
    DtwmView *v = server->anim_view;

    if (allow_latch && !server->anim_in_latch_phase &&
        server->anim_kind == DTWM_ANIM_KIND_MINIMIZE &&
        server->anim_active_style == DTWM_ANIM_STYLE_BEVEL &&
        v && v->mapped && v->minimized) {
        int latch_ms = anim_latch_duration_ms(server->anim_speed);
        clear_anim_scene_nodes(server);
        server->anim_in_latch_phase = true;
        server->anim_hides_view_frame = false;
        server->anim_start_ns = dtwm_now_nsec();
        server->anim_duration_ms = latch_ms;
        v->icon_latched = true;
        dtwm_icons_update_view(v);
        schedule_all_outputs_frame(server);
        if (server->anim_timer) {
            wl_event_source_timer_update(server->anim_timer, latch_ms + 2);
        }
        return;
    }

    clear_anim_scene_nodes(server);
    if (v) {
        if (v->icon_latched) {
            v->icon_latched = false;
            if (v->mapped) {
                dtwm_icons_update_view(v);
            }
        }
        if (server->anim_hides_view_frame && v->frame_tree && v->mapped) {
            wlr_scene_node_set_enabled(&v->frame_tree->node, dtwm_view_visible_on_current_ws(v));
        }
        if (v->desktop_icon_buf && v->mapped && (v->minimized || server->show_open_window_icons)) {
            wlr_scene_node_set_enabled(&v->desktop_icon_buf->node, !v->icon_clipped_out);
        }
    }

    server->anim_view = nullptr;
    server->anim_kind = DTWM_ANIM_KIND_NONE;
    server->anim_active_style = DTWM_ANIM_STYLE_NONE;
    server->anim_in_latch_phase = false;
    server->anim_hides_view_frame = false;
    server->anim_prev_box = (struct wlr_box){0};
    if (server->anim_timer) {
        wl_event_source_timer_update(server->anim_timer, 0);
    }
    schedule_all_outputs_frame(server);
}

void dtwm_anim_cancel_all(DtwmServer *server) {
    finish_or_cancel_anim(server, false);
}

void dtwm_anim_cancel_for_view(DtwmServer *server, DtwmView *view) {
    if (!server || !view) return;
    if (server->anim_view == view) {
        finish_or_cancel_anim(server, false);
    } else if (view->icon_latched) {
        view->icon_latched = false;
    }
}

static void step_window_anim(DtwmServer *server, uint64_t now_ns) {
    if (!server || server->anim_kind == DTWM_ANIM_KIND_NONE || !server->anim_tree) {
        return;
    }
    if (server->session_locked || server->greeter_mode ||
        (server->anim_view &&
         (!server->anim_view->mapped ||
          (server->anim_view->workspace_mask & (1u << server->current_workspace)) == 0))) {
        finish_or_cancel_anim(server, false);
        return;
    }

    uint64_t elapsed_ns = (now_ns >= server->anim_start_ns) ? (now_ns - server->anim_start_ns) : 0;
    double elapsed_ms = (double)elapsed_ns / 1000000.0;
    double dur_ms = (server->anim_duration_ms > 0) ? (double)server->anim_duration_ms : 1.0;

    if (server->anim_in_latch_phase) {
        if (elapsed_ms >= dur_ms) {
            finish_or_cancel_anim(server, false);
        } else {
            schedule_all_outputs_frame(server);
        }
        return;
    }

    if (elapsed_ms >= dur_ms) {
        finish_or_cancel_anim(server, true);
        return;
    }

    double raw_t = elapsed_ms / dur_ms;
    if (raw_t < 0.0) raw_t = 0.0;
    if (raw_t > 1.0) raw_t = 1.0;

    /* Balanced Hermite + cubic ease-out: responsive immediately on click while
     * keeping mid-flight window travel clearly observable across the workspace */
    bool is_minmax_ease = (server->anim_kind == DTWM_ANIM_KIND_MINIMIZE ||
                           server->anim_kind == DTWM_ANIM_KIND_RESTORE);
    double u;
    if (is_minmax_ease) {
        double herm = raw_t * raw_t * (3.0 - 2.0 * raw_t);
        double ease_out = 1.0 - pow(1.0 - raw_t, 2.3);
        u = 0.35 * herm + 0.65 * ease_out;
    } else {
        u = 1.0 - pow(1.0 - raw_t, 2.8);
    }

    int x = (int)lround(server->anim_from_box.x +
                        (server->anim_to_box.x - server->anim_from_box.x) * u);
    int y = (int)lround(server->anim_from_box.y +
                        (server->anim_to_box.y - server->anim_from_box.y) * u);
    int w = (int)lround(server->anim_from_box.width +
                        (server->anim_to_box.width - server->anim_from_box.width) * u);
    int h = (int)lround(server->anim_from_box.height +
                        (server->anim_to_box.height - server->anim_from_box.height) * u);
    if (w < 12) w = 12;
    if (h < 12) h = 12;

    struct wlr_box prev = server->anim_cur_box;
    server->anim_cur_box = (struct wlr_box){.x = x, .y = y, .width = w, .height = h};

    wlr_scene_node_set_position(&server->anim_tree->node, x, y);
    wlr_scene_node_set_enabled(&server->anim_tree->node, true);

    DtwmView *view = server->anim_view;
    DtwmAnimStyle style = server->anim_active_style;
    DtwmAnimKind kind = server->anim_kind;
    bool is_minmax_icon = (kind == DTWM_ANIM_KIND_MINIMIZE || kind == DTWM_ANIM_KIND_RESTORE);

    /* Refresh maximize/unmaximize client texture automatically once client commits new buffer */
    if (!is_minmax_icon && view && (style == DTWM_ANIM_STYLE_BEVEL || style == DTWM_ANIM_STYLE_TITLEBAR)) {
        struct wlr_surface *surf = view_wlr_surface(view);
        if (surf && surf->buffer && surf->buffer->texture && server->anim_client_buf) {
            struct wlr_buffer *live_b = &surf->buffer->base;
            if (live_b->width > 0 && live_b->height > 0 &&
                (live_b->width != server->anim_client_src_w ||
                 live_b->height != server->anim_client_src_h)) {
                wlr_scene_buffer_set_source_box(server->anim_client_buf, nullptr);
                wlr_scene_buffer_set_buffer(server->anim_client_buf, live_b);
                server->anim_client_src_w = live_b->width;
                server->anim_client_src_h = live_b->height;
            }
        }
    }

    if (style == DTWM_ANIM_STYLE_WIREFRAME) {
        const DtwmColorSet *cs =
            (view && view->active) ? &server->palette.active : &server->palette.inactive;
        if (server->anim_trail_buf) {
            if (prev.width >= 12 && prev.height >= 12 &&
                (abs(prev.x - x) > 1 || abs(prev.y - y) > 1 ||
                 abs(prev.width - w) > 1 || abs(prev.height - h) > 1)) {
                struct wlr_buffer *tbuf =
                    dtwm_render_palette_wireframe_outline(cs, prev.width, prev.height, 0.30);
                if (tbuf) {
                    wlr_scene_buffer_set_buffer(server->anim_trail_buf, tbuf);
                    wlr_buffer_drop(tbuf);
                    wlr_scene_node_set_position(&server->anim_trail_buf->node, prev.x - x, prev.y - y);
                    wlr_scene_node_set_enabled(&server->anim_trail_buf->node, true);
                }
            } else {
                wlr_scene_node_set_enabled(&server->anim_trail_buf->node, false);
            }
        }
        struct wlr_buffer *fbuf = dtwm_render_palette_wireframe_outline(cs, w, h, 1.0);
        if (fbuf && server->anim_frame_buf) {
            wlr_scene_buffer_set_buffer(server->anim_frame_buf, fbuf);
            wlr_buffer_drop(fbuf);
            wlr_scene_node_set_position(&server->anim_frame_buf->node, 0, 0);
            wlr_scene_node_set_enabled(&server->anim_frame_buf->node, true);
        }
    } else if (style == DTWM_ANIM_STYLE_TITLEBAR) {
        if (server->anim_trail_buf) {
            wlr_scene_node_set_enabled(&server->anim_trail_buf->node, false);
        }
        /* Brief client fade on minimize/restore while detached 3D titlebar shuttles */
        if (server->anim_client_buf && server->anim_client_buf->buffer &&
            server->anim_client_src_w > 0 && server->anim_client_src_h > 0) {
            if (is_minmax_icon) {
                double fade = (kind == DTWM_ANIM_KIND_MINIMIZE)
                                  ? (1.0 - raw_t / 0.40)
                                  : ((raw_t - 0.58) / 0.42);
                if (fade > 0.02) {
                    if (fade > 1.0) fade = 1.0;
                    struct wlr_box win_ref =
                        (kind == DTWM_ANIM_KIND_MINIMIZE) ? server->anim_from_box : server->anim_to_box;
                    DtwmMetrics am = view ? dtwm_metrics_for_view(view)
                                          : dtwm_metrics_for_dpi(dtwm_primary_output_dpi(server));
                    int l = am.border_w, t_in = am.border_w + am.title_h;
                    int r = am.border_w, b_in = am.border_w;
                    if (view) dtwm_view_insets(view, &l, &t_in, &r, &b_in);
                    int cw = win_ref.width - l - r;
                    int ch = win_ref.height - t_in - b_in;
                    if (cw > 8 && ch > 8) {
                        wlr_scene_node_set_position(
                            &server->anim_client_buf->node, win_ref.x - x + l, win_ref.y - y + t_in
                        );
                        wlr_scene_buffer_set_source_box(server->anim_client_buf, nullptr);
                        wlr_scene_buffer_set_dest_size(server->anim_client_buf, cw, ch);
                        wlr_scene_buffer_set_opacity(
                            server->anim_client_buf, clamp_anim_opacity(fade * 0.72)
                        );
                        wlr_scene_node_set_enabled(&server->anim_client_buf->node, true);
                    } else {
                        wlr_scene_node_set_enabled(&server->anim_client_buf->node, false);
                    }
                } else {
                    wlr_scene_node_set_enabled(&server->anim_client_buf->node, false);
                }
            } else {
                DtwmMetrics am = view ? dtwm_metrics_for_view(view)
                                      : dtwm_metrics_for_dpi(dtwm_primary_output_dpi(server));
                int l = am.border_w, t_in = am.border_w + am.title_h;
                int r = am.border_w, b_in = am.border_w;
                if (view) dtwm_view_insets(view, &l, &t_in, &r, &b_in);
                int cw = w - l - r;
                int ch = h - t_in - b_in;
                if (cw > 8 && ch > 8) {
                    wlr_scene_node_set_position(&server->anim_client_buf->node, l, t_in);
                    wlr_scene_buffer_set_source_box(server->anim_client_buf, nullptr);
                    wlr_scene_buffer_set_dest_size(server->anim_client_buf, cw, ch);
                    wlr_scene_buffer_set_opacity(server->anim_client_buf, 0.92f);
                    wlr_scene_node_set_enabled(&server->anim_client_buf->node, true);
                } else {
                    wlr_scene_node_set_enabled(&server->anim_client_buf->node, false);
                }
            }
        } else if (server->anim_client_buf) {
            wlr_scene_node_set_enabled(&server->anim_client_buf->node, false);
        }
        struct wlr_buffer *fbuf = dtwm_render_anim_bevel_frame(
            &server->palette, view, style, kind, w, h, u
        );
        if (fbuf && server->anim_frame_buf) {
            wlr_scene_buffer_set_buffer(server->anim_frame_buf, fbuf);
            wlr_buffer_drop(fbuf);
            wlr_scene_node_set_position(&server->anim_frame_buf->node, 0, 0);
            wlr_scene_node_set_enabled(&server->anim_frame_buf->node, true);
        }
    } else {
        /* DTWM_ANIM_STYLE_BEVEL ("Bevel Morph & Latch" / "Bevel Glide") */
        if (server->anim_trail_buf) {
            wlr_scene_node_set_enabled(&server->anim_trail_buf->node, false);
        }
        /* icon_prox: 0.0 at full window, 1.0 at desktop icon slot.
         * Keep the live window surface crisp for most of the flight and crossfade
         * symmetrically into/from the icon image only near the icon [0.68 .. 0.96]. */
        double icon_prox = 0.0;
        if (kind == DTWM_ANIM_KIND_MINIMIZE) {
            icon_prox = u;
        } else if (kind == DTWM_ANIM_KIND_RESTORE) {
            icon_prox = 1.0 - u;
        }
        double frame_condense = is_minmax_icon ? anim_smoothstep(0.60, 0.95, icon_prox) : 0.0;
        double crop_blend = is_minmax_icon ? anim_smoothstep(0.55, 0.95, icon_prox) : 0.0;

        DtwmMetrics am = view ? dtwm_metrics_for_view(view)
                              : dtwm_metrics_for_dpi(dtwm_primary_output_dpi(server));
        double min_b = (double)am.outer_bevel;
        int b_side = (int)lround((1.0 - frame_condense) * am.border_w + frame_condense * min_b);
        if (b_side < am.outer_bevel) b_side = am.outer_bevel;
        bool has_title = (!view || (view->decorated && view->has_title));
        int b_top = has_title
                        ? (int)lround((1.0 - frame_condense) * (am.border_w + am.title_h) +
                                      frame_condense * min_b)
                        : b_side;
        if (b_top < b_side) b_top = b_side;
        if (b_side * 2 >= w - 4) b_side = 2;
        if (b_top + b_side >= h - 4) b_top = b_side;

        int in_w = w - 2 * b_side;
        int in_h = h - b_top - b_side;

        /* 1. Client surface layer: stays 100% opaque until near the icon (icon_prox >= 0.70) */
        struct wlr_buffer *attached_cbuf =
            server->anim_client_buf ? server->anim_client_buf->buffer : nullptr;
        int tex_w = attached_cbuf ? attached_cbuf->width : 0;
        int tex_h = attached_cbuf ? attached_cbuf->height : 0;
        if (server->anim_client_buf && attached_cbuf && tex_w > 0 && tex_h > 0 &&
            in_w >= 4 && in_h >= 4) {
            double c_alpha = is_minmax_icon ? (1.0 - anim_smoothstep(0.70, 0.96, icon_prox)) : 1.0;
            if (c_alpha > 0.02) {
                struct wlr_fbox sbox = {0};
                bool use_crop = is_minmax_icon &&
                                compute_anim_client_source_box(
                                    tex_w, tex_h, in_w, in_h, crop_blend, &sbox
                                );
                wlr_scene_node_set_position(&server->anim_client_buf->node, b_side, b_top);
                wlr_scene_buffer_set_source_box(
                    server->anim_client_buf, use_crop ? &sbox : nullptr
                );
                wlr_scene_buffer_set_dest_size(server->anim_client_buf, in_w, in_h);
                wlr_scene_buffer_set_opacity(server->anim_client_buf, clamp_anim_opacity(c_alpha));
                wlr_scene_node_set_enabled(&server->anim_client_buf->node, true);
            } else {
                wlr_scene_node_set_enabled(&server->anim_client_buf->node, false);
            }
        } else if (server->anim_client_buf) {
            wlr_scene_node_set_enabled(&server->anim_client_buf->node, false);
        }

        /* 2. CDE Desktop Icon tile crossfade layer (fades in/out only near the icon [0.68 .. 0.94]) */
        if (server->anim_icon_buf && server->anim_icon_buf->buffer && is_minmax_icon && view) {
            double i_alpha = anim_smoothstep(0.68, 0.94, icon_prox);
            if (i_alpha > 0.02) {
                double iscale = (double)dtwm_primary_output_dpi(server) / 96.0;
                int img_dim = (int)lround((server->large_icons ? 96.0 : 48.0) * iscale);
                int img_box = img_dim + (int)lround(8.0 * iscale);
                int tile_w = view->icon_box_w > 0 ? view->icon_box_w : (int)lround(76.0 * iscale);
                int tile_h = view->icon_box_h > 0 ? view->icon_box_h : (img_box + (int)lround(22.0 * iscale));
                double sx = (double)w / (double)img_box;
                double sy = (double)h / (double)img_box;
                int dst_tw = (int)lround((double)tile_w * sx);
                int dst_th = (int)lround((double)tile_h * sy);
                int off_x = -(int)lround((double)((tile_w - img_box) / 2) * sx);
                wlr_scene_node_set_position(&server->anim_icon_buf->node, off_x, 0);
                wlr_scene_buffer_set_dest_size(
                    server->anim_icon_buf, dst_tw > 4 ? dst_tw : 4, dst_th > 4 ? dst_th : 4
                );
                wlr_scene_buffer_set_opacity(server->anim_icon_buf, clamp_anim_opacity(i_alpha));
                wlr_scene_node_set_enabled(&server->anim_icon_buf->node, true);
            } else {
                wlr_scene_node_set_enabled(&server->anim_icon_buf->node, false);
            }
        } else if (server->anim_icon_buf) {
            wlr_scene_node_set_enabled(&server->anim_icon_buf->node, false);
        }

        /* 3. Crisp unscaled 2px Motif bevel perimeter & HUD hairline */
        struct wlr_buffer *fbuf = dtwm_render_anim_bevel_frame(
            &server->palette, view, style, kind, w, h, u
        );
        if (fbuf && server->anim_frame_buf) {
            wlr_scene_buffer_set_buffer(server->anim_frame_buf, fbuf);
            wlr_buffer_drop(fbuf);
            wlr_scene_node_set_position(&server->anim_frame_buf->node, 0, 0);
            wlr_scene_node_set_enabled(&server->anim_frame_buf->node, true);
        }
    }

    schedule_all_outputs_frame(server);
    if (server->anim_timer) {
        wl_event_source_timer_update(server->anim_timer, 16);
    }
}

static int handle_anim_timer(void *data) {
    DtwmServer *server = data;
    if (!server || server->anim_kind == DTWM_ANIM_KIND_NONE) {
        return 0;
    }
    step_window_anim(server, dtwm_now_nsec());
    return 0;
}

static void start_window_anim(
    DtwmServer *server,
    DtwmView *view,
    DtwmAnimKind kind,
    struct wlr_box from_box,
    struct wlr_box to_box,
    struct wlr_buffer *client_buf_override,
    struct wlr_buffer *icon_buf_override
) {
    if (!server || !view || !server->anim_tree || !server->anim_timer) {
        return;
    }
    if (view->is_panel || view->is_subpanel || view->is_desktop_pin ||
        view->is_swallowed || view->is_lock_dialog) {
        return;
    }
    bool is_max = (kind == DTWM_ANIM_KIND_MAXIMIZE || kind == DTWM_ANIM_KIND_UNMAXIMIZE);
    DtwmAnimStyle style = dtwm_resolve_anim_style(server, is_max);
    if (style == DTWM_ANIM_STYLE_NONE) {
        return;
    }

    /* Smoothly reverse from current interpolated box when interrupting the same window,
     * retaining any locked client buffer across cancel if the surface dropped its buffer */
    struct wlr_buffer *retained_cbuf = nullptr;
    if (server->anim_view == view && server->anim_kind != DTWM_ANIM_KIND_NONE) {
        if (!server->anim_in_latch_phase && server->anim_cur_box.width > 0 &&
            server->anim_cur_box.height > 0) {
            from_box = server->anim_cur_box;
        }
        if (!client_buf_override && server->anim_client_buf && server->anim_client_buf->buffer) {
            retained_cbuf = wlr_buffer_lock(server->anim_client_buf->buffer);
        }
    }
    finish_or_cancel_anim(server, false);

    server->anim_view = view;
    server->anim_kind = kind;
    server->anim_active_style = style;
    server->anim_from_box = from_box;
    server->anim_to_box = to_box;
    server->anim_cur_box = from_box;
    server->anim_prev_box = from_box;
    server->anim_start_ns = dtwm_now_nsec();
    server->anim_in_latch_phase = false;

    server->anim_duration_ms = anim_flight_duration_ms(server->anim_speed, style, kind);

    server->anim_client_src_w = 0;
    server->anim_client_src_h = 0;
    if (style != DTWM_ANIM_STYLE_WIREFRAME && server->anim_client_buf) {
        struct wlr_buffer *chosen_cbuf = client_buf_override ? client_buf_override : retained_cbuf;
        if (!chosen_cbuf) {
            struct wlr_surface *surf = view_wlr_surface(view);
            if (surf && surf->buffer && surf->buffer->texture) {
                chosen_cbuf = &surf->buffer->base;
            }
        }
        if (chosen_cbuf && chosen_cbuf->width > 0 && chosen_cbuf->height > 0) {
            wlr_scene_buffer_set_source_box(server->anim_client_buf, nullptr);
            wlr_scene_buffer_set_buffer(server->anim_client_buf, chosen_cbuf);
            wlr_scene_buffer_set_filter_mode(server->anim_client_buf, WLR_SCALE_FILTER_BILINEAR);
            server->anim_client_src_w = chosen_cbuf->width;
            server->anim_client_src_h = chosen_cbuf->height;
        }
    }
    if (retained_cbuf) {
        wlr_buffer_unlock(retained_cbuf);
    }

    if (style == DTWM_ANIM_STYLE_BEVEL && !is_max && server->anim_icon_buf) {
        if (icon_buf_override) {
            wlr_scene_buffer_set_buffer(server->anim_icon_buf, icon_buf_override);
            wlr_scene_buffer_set_filter_mode(server->anim_icon_buf, WLR_SCALE_FILTER_BILINEAR);
        } else {
            struct wlr_buffer *ibuf = dtwm_icons_render_tile_buffer(view, nullptr, nullptr);
            if (ibuf) {
                wlr_scene_buffer_set_buffer(server->anim_icon_buf, ibuf);
                wlr_scene_buffer_set_filter_mode(server->anim_icon_buf, WLR_SCALE_FILTER_BILINEAR);
                wlr_buffer_drop(ibuf);
            }
        }
    }

    /* Hide live frame_tree during Restore/Maximize/Unmaximize morph so there is no double-draw */
    server->anim_hides_view_frame =
        (style != DTWM_ANIM_STYLE_WIREFRAME && kind != DTWM_ANIM_KIND_MINIMIZE);
    if (server->anim_hides_view_frame && view->frame_tree) {
        wlr_scene_node_set_enabled(&view->frame_tree->node, false);
    }
    /* When open-window icons are off, wait until arrival to reveal the newly created desktop icon */
    if (kind == DTWM_ANIM_KIND_MINIMIZE && style == DTWM_ANIM_STYLE_BEVEL &&
        !server->show_open_window_icons && view->desktop_icon_buf) {
        wlr_scene_node_set_enabled(&view->desktop_icon_buf->node, false);
    }

    step_window_anim(server, server->anim_start_ns);
}

static void view_set_maximized(DtwmView *view, bool maximized) {
    if (view->maximized == maximized) {
        return;
    }
    DtwmServer *server = view->server;
    bool can_anim = (view->mapped && !view->minimized && dtwm_view_visible_on_current_ws(view) &&
                     dtwm_resolve_anim_style(server, true) != DTWM_ANIM_STYLE_NONE);
    int pre_ow = 0, pre_oh = 0;
    dtwm_view_outer_size(view, &pre_ow, &pre_oh);
    struct wlr_box from_box = {.x = view->x, .y = view->y, .width = pre_ow, .height = pre_oh};
    struct wlr_buffer *pre_cbuf = can_anim ? lock_view_client_buffer(view) : nullptr;

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

    if (can_anim) {
        int post_ow = 0, post_oh = 0;
        dtwm_view_outer_size(view, &post_ow, &post_oh);
        struct wlr_box to_box = {.x = view->x, .y = view->y, .width = post_ow, .height = post_oh};
        if (from_box.x != to_box.x || from_box.y != to_box.y ||
            from_box.width != to_box.width || from_box.height != to_box.height) {
            start_window_anim(
                server,
                view,
                maximized ? DTWM_ANIM_KIND_MAXIMIZE : DTWM_ANIM_KIND_UNMAXIMIZE,
                from_box,
                to_box,
                pre_cbuf,
                nullptr
            );
        }
    }
    if (pre_cbuf) {
        wlr_buffer_unlock(pre_cbuf);
    }
}

static void view_set_minimized(DtwmView *view, bool minimized) {
    if (view->minimized == minimized) {
        return;
    }
    DtwmServer *server = view->server;
    DtwmAnimStyle style = dtwm_resolve_anim_style(server, false);
    int ow = 0, oh = 0;
    dtwm_view_outer_size(view, &ow, &oh);
    struct wlr_box win_box = {.x = view->x, .y = view->y, .width = ow, .height = oh};
    double iscale = (double)dtwm_primary_output_dpi(server) / 96.0;
    int ipad = (int)lround(4.0 * iscale);
    int img_box = (int)lround(((server->large_icons ? 96.0 : 48.0) + 8.0) * iscale);
    struct wlr_box pre_icon_box = {
        .x = view->icon_x - ipad,
        .y = view->icon_y - ipad,
        .width = img_box,
        .height = img_box,
    };
    struct wlr_buffer *pre_cbuf =
        (view->mapped && style != DTWM_ANIM_STYLE_NONE) ? lock_view_client_buffer(view) : nullptr;
    struct wlr_buffer *pre_ibuf =
        (view->mapped && !minimized && style == DTWM_ANIM_STYLE_BEVEL)
            ? dtwm_icons_render_tile_buffer(view, nullptr, nullptr)
            : nullptr;

    view->minimized = minimized;
    view->icon_latched = false;
    if (view->frame_tree) {
        wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
    }
    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        wlr_xwayland_surface_set_minimized(view->xwayland_surface, minimized);
        dtwm_icons_set_wm_state(view);
    }
    dtwm_icons_relayout(server);
    dtwm_foreign_toplevel_sync(view);

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

    bool on_cur_ws = (view->workspace_mask & (1u << server->current_workspace)) != 0;
    if (view->mapped && on_cur_ws && style != DTWM_ANIM_STYLE_NONE) {
        struct wlr_box icon_box = minimized
            ? (struct wlr_box){
                  .x = view->icon_x - ipad,
                  .y = view->icon_y - ipad,
                  .width = img_box,
                  .height = img_box,
              }
            : pre_icon_box;
        if (icon_box.x > -32 || icon_box.y > -32) {
            start_window_anim(
                server,
                view,
                minimized ? DTWM_ANIM_KIND_MINIMIZE : DTWM_ANIM_KIND_RESTORE,
                minimized ? win_box : icon_box,
                minimized ? icon_box : win_box,
                pre_cbuf,
                pre_ibuf
            );
        }
    }
    if (pre_cbuf) wlr_buffer_unlock(pre_cbuf);
    if (pre_ibuf) wlr_buffer_drop(pre_ibuf);
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

void dtwm_view_apply_geometry(DtwmView *view, int x, int y, int client_w, int client_h) {
    if (view) view_apply_geometry(view, x, y, client_w, client_h);
}

void dtwm_view_close(DtwmView *view) {
    view_close(view);
}

static int server_max_output_dpi(DtwmServer *server) {
    int max_dpi = dtwm_primary_output_dpi(server);
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->wlr_output && out->wlr_output->enabled && out->dpi > max_dpi) {
            max_dpi = out->dpi;
        }
    }
    return max_dpi > 0 ? max_dpi : 96;
}

static int g_loaded_cursor_phys_size = 0;

void dtwm_reload_cursor_theme(DtwmServer *server, const char *theme, int size) {
    if (!server) return;
    if (size < 12) size = 24;
    if (size > 128) size = 128;
    int max_dpi = server_max_output_dpi(server);
    int phys_size = (int)lround((double)size * (double)max_dpi / 96.0);
    if (phys_size < 12) phys_size = 12;
    if (phys_size > 256) phys_size = 256;
    const char *eff_theme = (theme && theme[0] && strcmp(theme, "default") != 0) ? theme : "";
    if (server->cursor_mgr && server->cursor_size == size &&
        g_loaded_cursor_phys_size == phys_size &&
        strcmp(server->cursor_theme, eff_theme) == 0) {
        wlr_xcursor_manager_load(server->cursor_mgr, 1.0f);
        return;
    }
    snprintf(server->cursor_theme, sizeof(server->cursor_theme), "%s", eff_theme);
    server->cursor_size = size;
    g_loaded_cursor_phys_size = phys_size;
    struct wlr_xcursor_manager *old_mgr = server->cursor_mgr;
    server->cursor_mgr = wlr_xcursor_manager_create(
        eff_theme[0] ? eff_theme : nullptr, (uint32_t)phys_size
    );
    if (server->cursor_mgr) {
        wlr_xcursor_manager_load(server->cursor_mgr, 1.0f);
        if (server->cursor) {
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        }
    }
    if (old_mgr) {
        wlr_xcursor_manager_destroy(old_mgr);
    }
}

void dtwm_sync_all_external_client_scales(DtwmServer *server) {
    if (!server) return;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->kind != DTWM_VIEW_XDG || !v->xdg_toplevel) continue;
        struct wl_client *client = wl_resource_get_client(v->xdg_toplevel->resource);
        if (wl_client_is_native_dpi(server, client)) {
            v->is_native_dpi = true;
            v->external_client_scale = 1.0;
            continue;
        }
        double new_s = external_view_target_scale(v);
        bool changed = fabs(new_s - v->external_client_scale) > 1e-4;
        v->external_client_scale = new_s;
        notify_surface_tree_scale(v->xdg_toplevel->base->surface, new_s);
        DtwmPopup *popup;
        wl_list_for_each(popup, &server->popups, link) {
            if (popup->owner_view == v && popup->xdg_popup && popup->xdg_popup->base) {
                notify_surface_tree_scale(popup->xdg_popup->base->surface, new_s);
            }
        }
        if (changed && v->mapped && !v->is_lock_dialog && v->client_w > 0 && v->client_h > 0) {
            v->pending_resize_serial =
                send_xdg_toplevel_size_for_view(v, v->client_w, v->client_h);
        }
        dtwm_sync_external_view_scene(v);
    }
}

void dtwm_reflow_views_for_outputs(DtwmServer *server) {
    if (!server) return;

    dtwm_reload_cursor_theme(server, server->cursor_theme, server->cursor_size);

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
        } else if (v->decorated) {
            dtwm_view_update_frame(v);
        }
    }
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped && v->decorated && (v->is_panel || v->is_subpanel)) {
            dtwm_view_update_frame(v);
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
    int mdpi = server->menu_target
                   ? dtwm_metrics_for_view(server->menu_target).dpi
                   : dtwm_output_dpi_at(server, server->menu_x, server->menu_y);
    struct wlr_buffer *buf = nullptr;
    if (server->menu_is_root) {
        bool has_panel = (server->panel_view != nullptr && server->panel_view->mapped);
        bool panel_min = (has_panel && server->panel_view->minimized);
        buf = dtwm_render_root_menu(
            &server->palette, panel_min, has_panel, server->menu_hover_idx, mdpi, &mw, &mh
        );
    } else {
        buf = dtwm_render_window_menu(
            &server->palette, server->menu_target, server->menu_hover_idx, mdpi, &mw, &mh
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

void dtwm_open_window_menu_for_view(DtwmServer *server, DtwmView *view, int x, int y) {
    if (!server || !view) return;
    open_window_menu(server, view, x, y);
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
    int mdpi = server->menu_target
                   ? dtwm_metrics_for_view(server->menu_target).dpi
                   : dtwm_output_dpi_at(server, server->menu_x, server->menu_y);
    if (server->menu_is_root) {
        return dtwm_root_menu_item_at(rx, ry, server->menu_w, server->menu_h, mdpi);
    }
    DtwmMetrics m = dtwm_metrics_for_dpi(mdpi);
    int mh = m.menu_pad * 2 + DTWM_MENU_ITEM_COUNT * m.menu_item_h + m.menu_sep_h;
    if (rx < 0 || ry < 0 || rx >= m.menu_w || ry >= mh) {
        return -1;
    }
    int y = m.menu_pad;
    for (int i = 0; i < DTWM_MENU_ITEM_COUNT; ++i) {
        if (i == DTWM_MENU_ITEM_COUNT - 1) {
            y += m.menu_sep_h;
        }
        if (ry >= y && ry < y + m.menu_item_h) {
            return i;
        }
        y += m.menu_item_h;
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

static bool view_is_open_xwayland_popup(const DtwmServer *server, const DtwmView *v) {
    if (!server || !v || v == server->popup_sink_view || !v->mapped ||
        v->kind != DTWM_VIEW_XWAYLAND || !v->xwayland_surface ||
        !v->xwayland_surface->override_redirect || !v->xwayland_surface->surface ||
        v->is_lock_dialog || v->is_desktop_pin || v->is_swallowed) {
        return false;
    }
    /* Ignore permanent off-screen / 1x1 X11 helper windows (e.g. _MOTIF_DRAG_WINDOW 10x10+-100+-100) */
    if (v->client_w <= 2 || v->client_h <= 2) {
        return false;
    }
    struct wlr_box vbox = {
        .x = v->x,
        .y = v->y,
        .width = v->client_w,
        .height = v->client_h,
    };
    return wlr_output_layout_intersects(server->output_layout, nullptr, &vbox);
}

static bool surface_is_open_xdg_popup(
    const DtwmServer *server, const struct wlr_surface *surface
) {
    if (!server || !surface) {
        return false;
    }
    const DtwmPopup *popup;
    wl_list_for_each(popup, &server->popups, link) {
        if (popup->xdg_popup && popup->xdg_popup->base &&
            popup->xdg_popup->base->surface == surface) {
            return true;
        }
    }
    struct wlr_xdg_surface *xdg =
        wlr_xdg_surface_try_from_wlr_surface((struct wlr_surface *)surface);
    return xdg && xdg->role == WLR_XDG_SURFACE_ROLE_POPUP;
}

static bool has_open_client_popup(const DtwmServer *server) {
    if (!server) return false;
    if (!wl_list_empty(&server->popups)) {
        return true;
    }
    const DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (view_is_open_xwayland_popup(server, v)) {
            return true;
        }
    }
    return false;
}

static void dismiss_open_client_popups_except_client(
    DtwmServer *server, uint32_t t_ms, const struct wl_client *keep_client
) {
    if (!server) {
        return;
    }
    /* Dismiss open native Wayland xdg_popups belonging to other clients */
    DtwmPopup *popup, *ptmp;
    wl_list_for_each_safe(popup, ptmp, &server->popups, link) {
        if (popup->xdg_popup) {
            if (keep_client && popup->xdg_popup->base && popup->xdg_popup->base->resource &&
                wl_resource_get_client(popup->xdg_popup->base->resource) == keep_client) {
                continue;
            }
            wlr_xdg_popup_destroy(popup->xdg_popup);
        }
    }

    /* Check whether any visible XWayland override-redirect popup/menu (e.g. Motif XmMenuShell) is open */
    DtwmView *or_view = nullptr;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (!view_is_open_xwayland_popup(server, v)) {
            continue;
        }
        or_view = v;
        break;
    }
    if (!or_view || !server->xcb_conn || server->xcb_popup_sink_win == XCB_WINDOW_NONE) {
        return;
    }

    /* Choose a 1x1 X11 root coordinate outside all open override-redirect popups */
    struct wlr_box obox = {0};
    get_primary_output_box(server, server->cursor->x, server->cursor->y, &obox);
    int cand_pts[4][2] = {
        {obox.x, obox.y},
        {obox.x + (obox.width > 2 ? obox.width - 1 : 0), obox.y},
        {obox.x, obox.y + (obox.height > 2 ? obox.height - 1 : 0)},
        {obox.x + (obox.width > 2 ? obox.width - 1 : 0),
         obox.y + (obox.height > 2 ? obox.height - 1 : 0)},
    };
    int16_t sink_x = (int16_t)cand_pts[0][0];
    int16_t sink_y = (int16_t)cand_pts[0][1];
    for (int i = 0; i < 4; ++i) {
        int px = cand_pts[i][0];
        int py = cand_pts[i][1];
        bool inside_popup = false;
        wl_list_for_each(v, &server->views, link) {
            if (!view_is_open_xwayland_popup(server, v)) {
                continue;
            }
            if (px >= v->x && px < v->x + v->client_w &&
                py >= v->y && py < v->y + v->client_h) {
                inside_popup = true;
                break;
            }
        }
        if (!inside_popup) {
            sink_x = (int16_t)px;
            sink_y = (int16_t)py;
            break;
        }
    }

    /* Position and raise the X11 sink window at (sink_x, sink_y) so XAllowEvents(ReplayPointer)
     * lands harmlessly on xcb_popup_sink_win rather than any underlying X11 client window, then
     * inject the outside click via the mapped override-redirect surface's Wayland coordinates. */
    uint32_t cfg_vals[3] = {(uint32_t)(int32_t)sink_x, (uint32_t)(int32_t)sink_y, XCB_STACK_MODE_ABOVE};
    xcb_configure_window(
        server->xcb_conn,
        server->xcb_popup_sink_win,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_STACK_MODE,
        cfg_vals
    );
    xcb_flush(server->xcb_conn);

    struct wlr_xwayland_surface *or_xs = or_view->xwayland_surface;
    struct wlr_surface *or_surf = or_xs->surface;
    double sx = (double)sink_x - (double)or_xs->x;
    double sy = (double)sink_y - (double)or_xs->y;
    uint32_t ts = t_ms ? t_ms : dtwm_now_msec();
    wlr_seat_pointer_notify_enter(server->seat, or_surf, sx, sy);
    wlr_seat_pointer_notify_frame(server->seat);
    wlr_seat_pointer_notify_motion(server->seat, ts, sx, sy);
    wlr_seat_pointer_notify_frame(server->seat);
    wlr_seat_pointer_notify_button(server->seat, ts + 1u, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_pointer_notify_frame(server->seat);
    wlr_seat_pointer_notify_button(server->seat, ts + 2u, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    wlr_seat_pointer_notify_frame(server->seat);
    wlr_seat_pointer_notify_clear_focus(server->seat);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void dismiss_open_client_popups(DtwmServer *server, uint32_t t_ms) {
    dismiss_open_client_popups_except_client(server, t_ms, nullptr);
}

static void view_init_frame_scene_nodes(
    DtwmView *view,
    struct wlr_scene_tree *parent_tree,
    struct wlr_xdg_surface *xdg_base,
    struct wlr_surface *xw_surf
) {
    view->frame_tree = wlr_scene_tree_create(parent_tree);
    view->frame_tree->node.data = view;

    const DtwmRgba *wbg = &view->server->palette.window_bg.bg;
    float bg_col[4] = { (float)wbg->r, (float)wbg->g, (float)wbg->b, 1.0f };
    view->client_bg_rect = wlr_scene_rect_create(view->frame_tree, 1, 1, bg_col);
    wlr_scene_node_set_enabled(&view->client_bg_rect->node, false);

    if (xdg_base) {
        view->content_tree = wlr_scene_xdg_surface_create(view->frame_tree, xdg_base);
    } else if (xw_surf) {
        view->content_tree = wlr_scene_subsurface_tree_create(view->frame_tree, xw_surf);
    }
    if (view->content_tree) {
        view->content_tree->node.data = view;
    }

    view->frame_top_buf = wlr_scene_buffer_create(view->frame_tree, nullptr);
    view->frame_bottom_buf = wlr_scene_buffer_create(view->frame_tree, nullptr);
    view->frame_left_buf = wlr_scene_buffer_create(view->frame_tree, nullptr);
    view->frame_right_buf = wlr_scene_buffer_create(view->frame_tree, nullptr);

    view->popup_anchor_tree = wlr_scene_tree_create(view->frame_tree);
    view->popup_anchor_tree->node.data = view;
    if (xdg_base) {
        xdg_base->data = view->popup_anchor_tree;
    }
}

static void begin_interactive(DtwmView *view, DtwmCursorMode mode, uint32_t edges) {
    DtwmServer *server = view->server;
    dtwm_anim_cancel_for_view(server, view);
    close_window_menu(server);
    dismiss_open_client_popups(server, dtwm_now_msec());
    hide_wireframe_outline(server);
    view->tile_state = 0;
    view->resize_inflight = false;
    view->has_queued_resize = false;
    view->resize_anchor_right = view->x + view->client_w;
    view->resize_anchor_bottom = view->y + view->client_h;
    view->active_resize_edges = (mode == DTWM_CURSOR_RESIZE) ? edges : 0;
    if (mode == DTWM_CURSOR_RESIZE && view->kind == DTWM_VIEW_XDG &&
        view->xdg_toplevel && view->xdg_toplevel->base->initialized) {
        wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, true);
    }
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
    DtwmMetrics m = dtwm_metrics_for_view(view);
    view->subpanel_torn_off = true;
    view->has_title = true;
    view->has_menu_btn = true;
    view->y -= m.title_h;
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

static bool view_is_normal_app(const DtwmView *v);

static void spawn_desktop_command(DtwmServer *server, const char *prog, const char *arg1) {
    if (!server || !prog || !prog[0]) {
        return;
    }
    const char *base = strrchr(prog, '/');
    base = base ? base + 1 : prog;
    if (strcmp(base, "dtaction") != 0) {
        dtwm_notify_panel_busy_start(server);
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

static xcb_atom_t intern_xcb_atom_now(xcb_connection_t *conn, const char *name, bool only_if_exists) {
    if (!conn || !name) return XCB_ATOM_NONE;
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, only_if_exists ? 1 : 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, nullptr);
    if (!rep) return XCB_ATOM_NONE;
    xcb_atom_t atom = rep->atom;
    free(rep);
    return atom;
}

static xcb_window_t find_live_dtsession_window_xcb(DtwmServer *server) {
    if (!server || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) {
        return XCB_WINDOW_NONE;
    }
    xcb_atom_t sm_info = intern_xcb_atom_now(server->xcb_conn, "_DT_SM_WINDOW_INFO", true);
    if (sm_info == XCB_ATOM_NONE) return XCB_WINDOW_NONE;
    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn, 0, server->xcb_root, sm_info, sm_info, 0, 2
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return XCB_WINDOW_NONE;
    xcb_window_t sm_win = XCB_WINDOW_NONE;
    if (rep->format == 32 && xcb_get_property_value_length(rep) >= 8) {
        const uint32_t *vals = (const uint32_t *)xcb_get_property_value(rep);
        sm_win = (xcb_window_t)vals[1];
    }
    free(rep);
    if (sm_win == XCB_WINDOW_NONE) return XCB_WINDOW_NONE;

    xcb_get_window_attributes_cookie_t ack = xcb_get_window_attributes(server->xcb_conn, sm_win);
    xcb_get_window_attributes_reply_t *arep = xcb_get_window_attributes_reply(server->xcb_conn, ack, nullptr);
    if (!arep) return XCB_WINDOW_NONE;
    free(arep);

    xcb_atom_t sm_state = intern_xcb_atom_now(server->xcb_conn, "_DT_SM_STATE_INFO", true);
    if (sm_state == XCB_ATOM_NONE) return XCB_WINDOW_NONE;
    xcb_get_property_cookie_t sck = xcb_get_property(
        server->xcb_conn, 0, sm_win, sm_state, sm_state, 0, 16
    );
    xcb_get_property_reply_t *srep = xcb_get_property_reply(server->xcb_conn, sck, nullptr);
    if (!srep) return XCB_WINDOW_NONE;
    bool has_state = xcb_get_property_value_length(srep) > 0;
    free(srep);
    return has_state ? sm_win : XCB_WINDOW_NONE;
}

static bool has_live_ttsession_prop_xcb(DtwmServer *server) {
    if (!server || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) {
        return false;
    }
    xcb_atom_t tt_atom = intern_xcb_atom_now(server->xcb_conn, "_SUN_TT_SESSION", true);
    if (tt_atom == XCB_ATOM_NONE) {
        tt_atom = intern_xcb_atom_now(server->xcb_conn, "_DT_TT_SESSION", true);
    }
    if (tt_atom == XCB_ATOM_NONE) return false;
    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn, 0, server->xcb_root, tt_atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 64
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return false;
    bool ok = xcb_get_property_value_length(rep) > 0;
    free(rep);
    return ok;
}

static void request_root_menu_logout(DtwmServer *server) {
    xcb_window_t sm_win = find_live_dtsession_window_xcb(server);
    bool tt_up = has_live_ttsession_prop_xcb(server);
    if (tt_up && (sm_win != XCB_WINDOW_NONE || server->panel_pid > 0)) {
        spawn_desktop_command(server, CDE_INSTALLATION_TOP "/bin/dtaction", "ExitSession");
        return;
    }
    if (sm_win != XCB_WINDOW_NONE && server->xcb_conn) {
        xcb_atom_t proto = intern_xcb_atom_now(server->xcb_conn, "_DT_SM_WM_PROTOCOL", false);
        xcb_atom_t exit_a = intern_xcb_atom_now(server->xcb_conn, "_DT_WM_EXIT_SESSION", false);
        if (proto != XCB_ATOM_NONE && exit_a != XCB_ATOM_NONE) {
            xcb_client_message_event_t ev = {0};
            ev.response_type = XCB_CLIENT_MESSAGE;
            ev.format = 32;
            ev.window = sm_win;
            ev.type = proto;
            ev.data.data32[0] = exit_a;
            ev.data.data32[1] = XCB_CURRENT_TIME;
            xcb_send_event(server->xcb_conn, 0, sm_win, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
            xcb_flush(server->xcb_conn);
            return;
        }
    }
    server->shutting_down = true;
    wl_display_terminate(server->wl_display);
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
        dtwm_icons_pack(server);
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
        request_root_menu_logout(server);
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
        if (v->minimized) {
            focus_view(server, v, nullptr);
        } else if (v->maximized) {
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
        if (v->minimized) {
            focus_view(server, v, nullptr);
        }
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
    DtwmMetrics m = dtwm_metrics_for_dpi(dtwm_output_dpi_at(server, cx, cy));
    bool left = (cx - obox.x) < m.snap_thresh;
    bool right = (obox.x + obox.width - cx) < m.snap_thresh;
    bool top = (cy - obox.y) < m.snap_thresh;
    bool bottom = (obox.y + obox.height - cy) < m.snap_thresh;

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
    if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface &&
        v->xwayland_surface->override_redirect) {
        return false;
    }
    return dtwm_view_visible_on_current_ws(v) &&
           !v->is_swallowed && !v->is_desktop_pin && !v->is_panel && !v->is_subpanel &&
           !v->is_lock_dialog;
}

void dtwm_restore_view_focus(DtwmServer *server, DtwmView *view) {
    if (!server || !view || !view->mapped) return;
    if ((view->workspace_mask & (1u << server->current_workspace)) == 0) {
        for (uint32_t ws = 0; ws < server->num_workspaces; ++ws) {
            if (view->workspace_mask & (1u << ws)) {
                dtwm_switch_workspace(server, ws);
                break;
            }
        }
    }
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

    /* First check scene graph for popups / unmanaged XWayland menus / layer surfaces / client surfaces.
     * Temporarily hide the cursor-attached drag_icon_tree so it never intercepts hit-testing over drop targets. */
    bool restore_drag_icon = false;
    if (server->drag_icon_tree && server->drag_icon_tree->node.enabled) {
        wlr_scene_node_set_enabled(&server->drag_icon_tree->node, false);
        restore_drag_icon = true;
    }
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (restore_drag_icon && server->drag_icon_tree) {
        wlr_scene_node_set_enabled(&server->drag_icon_tree->node, true);
    }
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
        /* Could be one of a view's server-side CDE bevel strip buffers! */
        if (node->parent && node->parent->node.data) {
            DtwmView *v = node->parent->node.data;
            *out_part = dtwm_frame_hit_test(v, (int)(lx - v->x), (int)(ly - v->y));
            return v;
        }
    } else if (node && node->type == WLR_SCENE_NODE_RECT &&
               node->parent && node->parent->node.data) {
        DtwmView *v = node->parent->node.data;
        *out_part = DTWM_PART_CLIENT;
        return v;
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
        DtwmMetrics gm = dtwm_metrics_for_view(v);
        nx = align_to_grid(nx, gm.grid_sz);
        ny = align_to_grid(ny, gm.grid_sz);
    }
    if (v->is_panel) {
        if (nx == v->x && ny == v->y) {
            return;
        }
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
    if (nx == v->x && ny == v->y) {
        return;
    }
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

    int min_w = 64, min_h = 48, max_w = 0, max_h = 0;
    view_size_limits(v, &min_w, &min_h, &max_w, &max_h);

    int new_x = server->grab_orig_box.x;
    int new_y = server->grab_orig_box.y;
    int new_w = server->grab_orig_box.width;
    int new_h = server->grab_orig_box.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_y += (int)dy;
        new_h -= (int)dy;
        if (new_h < min_h) {
            new_y = server->grab_orig_box.y + server->grab_orig_box.height - min_h;
            new_h = min_h;
        }
        if (max_h > 0 && new_h > max_h) {
            new_y = server->grab_orig_box.y + server->grab_orig_box.height - max_h;
            new_h = max_h;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_h += (int)dy;
        if (new_h < min_h) new_h = min_h;
        if (max_h > 0 && new_h > max_h) new_h = max_h;
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_x += (int)dx;
        new_w -= (int)dx;
        if (new_w < min_w) {
            new_x = server->grab_orig_box.x + server->grab_orig_box.width - min_w;
            new_w = min_w;
        }
        if (max_w > 0 && new_w > max_w) {
            new_x = server->grab_orig_box.x + server->grab_orig_box.width - max_w;
            new_w = max_w;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_w += (int)dx;
        if (new_w < min_w) new_w = min_w;
        if (max_w > 0 && new_w > max_w) new_w = max_w;
    }

    if (shift_is_held(server)) {
        DtwmMetrics gm = dtwm_metrics_for_view(v);
        new_w = align_to_grid(new_w, gm.grid_sz);
        new_h = align_to_grid(new_h, gm.grid_sz);
        if (new_w < min_w) new_w = min_w;
        if (new_h < min_h) new_h = min_h;
        if (max_w > 0 && new_w > max_w) new_w = max_w;
        if (max_h > 0 && new_h > max_h) new_h = max_h;
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
    if (new_x == v->x && new_y == v->y &&
        new_w == v->client_w && new_h == v->client_h && !v->has_queued_resize) {
        return;
    }
    uint32_t now_ms = dtwm_now_msec();
    if (v->resize_inflight && (now_ms - v->resize_sent_ms) < 250u) {
        /* Overwrite in-place so ONLY the single latest resize target is kept */
        v->has_queued_resize = true;
        v->queued_x = new_x;
        v->queued_y = new_y;
        v->queued_w = new_w;
        v->queued_h = new_h;
        if (server->resize_flush_timer) {
            wl_event_source_timer_update(server->resize_flush_timer, 250);
        }
        return;
    }
    v->resize_inflight = true;
    v->has_queued_resize = false;
    v->resize_sent_ms = now_ms;
    if (v->kind == DTWM_VIEW_XDG && v->mapped && v->xdg_toplevel) {
        view_send_xdg_interactive_resize(v, new_w, new_h);
    } else {
        view_apply_geometry(v, new_x, new_y, new_w, new_h);
    }
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
    dtwm_a11y_on_cursor_motion(server);
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

    if (dtwm_icons_handle_drag_motion(server)) {
        return;
    }
    if (dtwm_icon_box_handle_motion(server)) {
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

    if (server->pointer_buttons_down != 0 &&
        !any_device_button_held(server) &&
        server->seat->pointer_state.button_count == 0) {
        server->pointer_buttons_mask = 0;
        server->pointer_buttons_down = 0;
    }

    double sx = 0, sy = 0;
    struct wlr_surface *surface = nullptr;
    DtwmFramePart part = DTWM_PART_NONE;
    DtwmView *view = desktop_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy, &part);

    if (server->focus_follows_mouse && server->pointer_buttons_down == 0 && !server->switcher_visible &&
        !has_open_client_popup(server) &&
        view && view != server->focused_view && view_is_normal_app(view)) {
        focus_view_ex(server, view, surface, server->focus_auto_raise);
    }

    if (surface) {
        dtwm_xdnd_on_cursor_motion(server, surface, sx, sy, time_msec);
    }

    /* While a mouse button is held without a Wayland DnD session, preserve implicit pointer grab
     * on the originally pressed surface so Motif menu-bar press-drag-release across override-
     * redirect XmMenuShell popups and X11 drags never suffer a spurious wl_pointer.leave! */
    if (server->seat->drag == nullptr && server->pointer_buttons_down > 0 &&
        server->seat->pointer_state.focused_surface != nullptr) {
        struct wlr_surface *fsurf = server->seat->pointer_state.focused_surface;
        if (surface == fsurf) {
            wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
        } else {
            bool sent = false;
            DtwmView *fv;
            wl_list_for_each(fv, &server->views, link) {
                if (fv != server->popup_sink_view && fv->mapped && view_wlr_surface(fv) == fsurf) {
                    int gx = 0, gy = 0;
                    struct wlr_scene_tree *st = fv->content_tree ? fv->content_tree : fv->frame_tree;
                    if (st && wlr_scene_node_coords(&st->node, &gx, &gy)) {
                        wlr_seat_pointer_notify_motion(
                            server->seat,
                            time_msec,
                            server->cursor->x - (double)gx,
                            server->cursor->y - (double)gy
                        );
                        sent = true;
                    }
                    break;
                }
            }
            if (!sent) {
                struct wlr_xdg_surface *fxdg = wlr_xdg_surface_try_from_wlr_surface(fsurf);
                if (fxdg && fxdg->data) {
                    int gx = 0, gy = 0;
                    struct wlr_scene_tree *st = fxdg->data;
                    if (wlr_scene_node_coords(&st->node, &gx, &gy)) {
                        wlr_seat_pointer_notify_motion(
                            server->seat,
                            time_msec,
                            server->cursor->x - (double)gx,
                            server->cursor->y - (double)gy
                        );
                    }
                }
            }
        }
        dtwm_icons_sync_xwayland_pointer(server, server->cursor->x, server->cursor->y, fsurf);
        return;
    }

    if (!surface) {
        DtwmFramePart ib_part = dtwm_icon_box_is_topmost_at(server, server->cursor->x, server->cursor->y)
                                    ? dtwm_icon_box_part_at(server, server->cursor->x, server->cursor->y)
                                    : DTWM_PART_NONE;
        const char *cname = "default";
        if (view) {
            cname = cursor_name_for_part(part);
        } else if (ib_part != DTWM_PART_NONE) {
            cname = cursor_name_for_part(ib_part);
        }
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, cname);
        wlr_seat_pointer_notify_clear_focus(server->seat);
    } else {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
    }
    dtwm_icons_sync_xwayland_pointer(server, server->cursor->x, server->cursor->y, surface);
}

void dtwm_recheck_cursor_focus(DtwmServer *server) {
    if (!server || server->shutting_down || !server->cursor || !server->seat) return;
    process_cursor_motion(server, dtwm_now_msec());
    wlr_seat_pointer_notify_frame(server->seat);
}

static DtwmView *find_view_owning_surface(const DtwmServer *server, struct wlr_surface *surf) {
    if (!server || !surf) {
        return nullptr;
    }
    struct wlr_surface *root = surf;
    for (int depth = 0; depth < 16 && root; ++depth) {
        struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(root);
        if (!sub || !sub->parent) {
            break;
        }
        root = sub->parent;
    }
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped && view_wlr_surface(v) == root) {
            return v;
        }
    }
    return nullptr;
}

static DtwmView *surface_global_origin_and_scale(
    const DtwmServer *server,
    struct wlr_surface *surf,
    double *out_gx,
    double *out_gy,
    double *out_scale
) {
    DtwmView *v = find_view_owning_surface(server, surf);
    if (!v) {
        return nullptr;
    }
    int gx = 0, gy = 0;
    struct wlr_scene_tree *st = v->content_tree ? v->content_tree : v->frame_tree;
    if (st) {
        wlr_scene_node_coords(&st->node, &gx, &gy);
    }
    double s = (v->kind == DTWM_VIEW_XDG && !v->is_native_dpi && v->external_client_scale > 0.25)
                   ? v->external_client_scale
                   : 1.0;
    double off_x = 0.0, off_y = 0.0;
    struct wlr_surface *cur = surf;
    for (int depth = 0; depth < 16 && cur && cur != view_wlr_surface(v); ++depth) {
        struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(cur);
        if (!sub || !sub->parent) {
            break;
        }
        off_x += (double)sub->current.x * s;
        off_y += (double)sub->current.y * s;
        cur = sub->parent;
    }
    if (out_gx) *out_gx = (double)gx + off_x;
    if (out_gy) *out_gy = (double)gy + off_y;
    if (out_scale) *out_scale = s;
    return v;
}

bool dtwm_view_has_input_capture(const DtwmView *view) {
    if (!view || !view->server || view->server->shutting_down) {
        return false;
    }
    const DtwmServer *srv = view->server;
    if (srv->active_pointer_constraint &&
        find_view_owning_surface(srv, srv->active_pointer_constraint->surface) == view) {
        return true;
    }
    if (srv->active_shortcuts_inhibitor && srv->active_shortcuts_inhibitor->active &&
        find_view_owning_surface(srv, srv->active_shortcuts_inhibitor->surface) == view) {
        return true;
    }
    return false;
}

static void publish_input_capture_ipc(DtwmServer *server) {
    if (!server || server->shutting_down || server->native_ipc_fd < 0) {
        return;
    }
    int locked = (server->active_pointer_constraint &&
                  server->active_pointer_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
                     ? 1
                     : 0;
    int confined = (server->active_pointer_constraint &&
                    server->active_pointer_constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED)
                       ? 1
                       : 0;
    int shortcuts =
        (server->active_shortcuts_inhibitor && server->active_shortcuts_inhibitor->active) ? 1 : 0;
    int escaped = (server->input_escaped_view != nullptr) ? 1 : 0;
    char msg[128];
    int n = snprintf(
        msg,
        sizeof(msg),
        "locked=%d confined=%d shortcuts=%d escaped=%d",
        locked,
        confined,
        shortcuts,
        escaped
    );
    if (n > 0) {
        dtwm_native_ipc_publish_prop(server, "_DT_WAYLAND_INPUT_CAPTURE", msg, (size_t)n);
    }
}

static void build_pointer_constraint_region(
    struct wlr_pointer_constraint_v1 *c, pixman_region32_t *out_reg
) {
    int sw = (c && c->surface && c->surface->current.width > 0) ? c->surface->current.width : 1;
    int sh = (c && c->surface && c->surface->current.height > 0) ? c->surface->current.height : 1;
    pixman_region32_init_rect(out_reg, 0, 0, (unsigned)sw, (unsigned)sh);
    if (c && pixman_region32_not_empty(&c->region)) {
        pixman_region32_intersect(out_reg, out_reg, &c->region);
    }
}

static void clamp_point_into_region(const pixman_region32_t *reg, double *io_x, double *io_y) {
    if (!reg || !io_x || !io_y) return;
    if (pixman_region32_contains_point(reg, (int)floor(*io_x), (int)floor(*io_y), nullptr)) {
        return;
    }
    int nboxes = 0;
    const pixman_box32_t *boxes = pixman_region32_rectangles(reg, &nboxes);
    if (nboxes <= 0 || !boxes) {
        return;
    }
    double best_x = *io_x, best_y = *io_y;
    double best_d2 = 1e30;
    for (int i = 0; i < nboxes; ++i) {
        double min_x = (double)boxes[i].x1;
        double max_x = (double)boxes[i].x2 > min_x + 1.0 ? (double)boxes[i].x2 - 0.5 : min_x;
        double min_y = (double)boxes[i].y1;
        double max_y = (double)boxes[i].y2 > min_y + 1.0 ? (double)boxes[i].y2 - 0.5 : min_y;
        double cx = *io_x < min_x ? min_x : (*io_x > max_x ? max_x : *io_x);
        double cy = *io_y < min_y ? min_y : (*io_y > max_y ? max_y : *io_y);
        double dx = cx - *io_x;
        double dy = cy - *io_y;
        double d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_x = cx;
            best_y = cy;
        }
    }
    *io_x = best_x;
    *io_y = best_y;
}

static void deactivate_active_pointer_constraint(DtwmServer *server, bool apply_hint) {
    if (!server || !server->active_pointer_constraint) {
        return;
    }
    struct wlr_pointer_constraint_v1 *c = server->active_pointer_constraint;
    server->active_pointer_constraint = nullptr;
    DtwmView *owner = c->surface ? find_view_owning_surface(server, c->surface) : nullptr;
    if (apply_hint && !server->shutting_down &&
        c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
        c->current.cursor_hint.enabled && c->surface && server->cursor) {
        double gx = 0.0, gy = 0.0, s = 1.0;
        if (surface_global_origin_and_scale(server, c->surface, &gx, &gy, &s)) {
            wlr_cursor_warp_closest(
                server->cursor,
                nullptr,
                gx + c->current.cursor_hint.x * s,
                gy + c->current.cursor_hint.y * s
            );
            process_cursor_motion(server, dtwm_now_msec());
        }
    }
    wlr_pointer_constraint_v1_send_deactivated(c);
    if (owner && owner->mapped) {
        dtwm_view_update_frame(owner);
    }
    publish_input_capture_ipc(server);
}

static void activate_pointer_constraint(
    DtwmServer *server, struct wlr_pointer_constraint_v1 *c, bool show_osd
) {
    if (!server || server->active_pointer_constraint == c) {
        return;
    }
    deactivate_active_pointer_constraint(server, true);
    if (!c || !c->surface) {
        return;
    }
    double gx = 0.0, gy = 0.0, s = 1.0;
    DtwmView *owner = surface_global_origin_and_scale(server, c->surface, &gx, &gy, &s);
    if (!owner || !owner->mapped) {
        return;
    }
    double sx = (server->cursor->x - gx) / s;
    double sy = (server->cursor->y - gy) / s;
    pixman_region32_t creg;
    build_pointer_constraint_region(c, &creg);
    if (!pixman_region32_contains_point(&creg, (int)floor(sx), (int)floor(sy), nullptr)) {
        clamp_point_into_region(&creg, &sx, &sy);
        wlr_cursor_warp_closest(server->cursor, nullptr, gx + sx * s, gy + sy * s);
    }
    pixman_region32_fini(&creg);

    wlr_seat_pointer_notify_enter(server->seat, c->surface, sx, sy);
    wlr_seat_pointer_notify_frame(server->seat);

    server->active_pointer_constraint = c;
    wlr_pointer_constraint_v1_send_activated(c);
    dtwm_view_update_frame(owner);
    if (show_osd) {
        show_hardware_osd(
            server,
            "input",
            100,
            false,
            c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED
                ? "Pointer Locked \xe2\x80\x94 Ctrl+Alt to Release"
                : "Pointer Confined \xe2\x80\x94 Ctrl+Alt to Release"
        );
    }
    publish_input_capture_ipc(server);
}

static void sync_input_capture_for_focused_view(DtwmServer *server, bool show_osd) {
    if (!server || server->shutting_down) {
        return;
    }
    DtwmView *fv = (!server->session_locked && server->focused_view && server->focused_view->mapped)
                       ? server->focused_view
                       : nullptr;
    if (!fv || fv == server->input_escaped_view) {
        deactivate_active_pointer_constraint(server, true);
        if (server->active_shortcuts_inhibitor) {
            struct wlr_keyboard_shortcuts_inhibitor_v1 *inh = server->active_shortcuts_inhibitor;
            DtwmView *owner =
                inh->surface ? find_view_owning_surface(server, inh->surface) : nullptr;
            server->active_shortcuts_inhibitor = nullptr;
            if (inh->active) {
                wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inh);
            }
            if (owner && owner->mapped) {
                dtwm_view_update_frame(owner);
            }
            publish_input_capture_ipc(server);
        }
        return;
    }

    if (server->active_pointer_constraint &&
        find_view_owning_surface(server, server->active_pointer_constraint->surface) != fv) {
        deactivate_active_pointer_constraint(server, true);
    }
    if (!server->active_pointer_constraint) {
        DtwmPointerConstraint *pc;
        wl_list_for_each(pc, &server->tracked_pointer_constraints, link) {
            if (pc->constraint && find_view_owning_surface(server, pc->constraint->surface) == fv) {
                activate_pointer_constraint(server, pc->constraint, show_osd);
                break;
            }
        }
    }

    if (server->active_shortcuts_inhibitor &&
        find_view_owning_surface(server, server->active_shortcuts_inhibitor->surface) != fv) {
        struct wlr_keyboard_shortcuts_inhibitor_v1 *inh = server->active_shortcuts_inhibitor;
        DtwmView *owner = inh->surface ? find_view_owning_surface(server, inh->surface) : nullptr;
        server->active_shortcuts_inhibitor = nullptr;
        if (inh->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inh);
        }
        if (owner && owner->mapped) {
            dtwm_view_update_frame(owner);
        }
        publish_input_capture_ipc(server);
    }
    if (!server->active_shortcuts_inhibitor) {
        DtwmShortcutsInhibitor *si;
        wl_list_for_each(si, &server->tracked_shortcuts_inhibitors, link) {
            if (si->inhibitor && find_view_owning_surface(server, si->inhibitor->surface) == fv) {
                server->active_shortcuts_inhibitor = si->inhibitor;
                wlr_keyboard_shortcuts_inhibitor_v1_activate(si->inhibitor);
                dtwm_view_update_frame(fv);
                if (show_osd && !server->active_pointer_constraint) {
                    show_hardware_osd(
                        server, "input", 100, false, "Shortcuts Captured \xe2\x80\x94 Ctrl+Alt to Release"
                    );
                }
                publish_input_capture_ipc(server);
                break;
            }
        }
    }
}

static bool release_input_capture_via_escape_hatch(DtwmServer *server) {
    if (!server || (!server->active_pointer_constraint && !server->active_shortcuts_inhibitor)) {
        return false;
    }
    DtwmView *fv = server->focused_view;
    server->input_escaped_view = fv;
    server->ctrl_alt_chord_pending = false;
    deactivate_active_pointer_constraint(server, true);
    if (server->active_shortcuts_inhibitor) {
        struct wlr_keyboard_shortcuts_inhibitor_v1 *inh = server->active_shortcuts_inhibitor;
        server->active_shortcuts_inhibitor = nullptr;
        if (inh->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inh);
        }
    }
    if (fv && fv->mapped) {
        dtwm_view_update_frame(fv);
    }
    show_hardware_osd(server, "input", 0, false, "Input Released (Ctrl+Alt)");
    publish_input_capture_ipc(server);
    return true;
}

static void handle_pointer_constraint_set_region(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmPointerConstraint *pc = wl_container_of(listener, pc, set_region);
    DtwmServer *server = pc->server;
    if (!server || server->shutting_down || server->active_pointer_constraint != pc->constraint) {
        return;
    }
    if (pc->constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED && pc->constraint->surface) {
        double gx = 0.0, gy = 0.0, s = 1.0;
        if (surface_global_origin_and_scale(server, pc->constraint->surface, &gx, &gy, &s)) {
            double sx = (server->cursor->x - gx) / s;
            double sy = (server->cursor->y - gy) / s;
            pixman_region32_t creg;
            build_pointer_constraint_region(pc->constraint, &creg);
            if (!pixman_region32_contains_point(&creg, (int)floor(sx), (int)floor(sy), nullptr)) {
                clamp_point_into_region(&creg, &sx, &sy);
                wlr_cursor_warp_closest(server->cursor, nullptr, gx + sx * s, gy + sy * s);
                process_cursor_motion(server, dtwm_now_msec());
            }
            pixman_region32_fini(&creg);
        }
    }
}

static void handle_pointer_constraint_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmPointerConstraint *pc = wl_container_of(listener, pc, destroy);
    DtwmServer *server = pc->server;
    bool was_active = (server && server->active_pointer_constraint == pc->constraint);
    DtwmView *owner =
        (server && pc->constraint && pc->constraint->surface)
            ? find_view_owning_surface(server, pc->constraint->surface)
            : nullptr;
    if (was_active) {
        if (!server->shutting_down &&
            pc->constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
            pc->constraint->current.cursor_hint.enabled &&
            pc->constraint->surface && server->cursor) {
            double gx = 0.0, gy = 0.0, s = 1.0;
            if (surface_global_origin_and_scale(server, pc->constraint->surface, &gx, &gy, &s)) {
                wlr_cursor_warp_closest(
                    server->cursor,
                    nullptr,
                    gx + pc->constraint->current.cursor_hint.x * s,
                    gy + pc->constraint->current.cursor_hint.y * s
                );
            }
        }
        server->active_pointer_constraint = nullptr;
    }
    wl_list_remove(&pc->set_region.link);
    wl_list_remove(&pc->destroy.link);
    wl_list_remove(&pc->link);
    free(pc);
    if (server && !server->shutting_down) {
        if (owner && owner->mapped) {
            dtwm_view_update_frame(owner);
        }
        publish_input_capture_ipc(server);
        if (was_active) {
            dtwm_recheck_cursor_focus(server);
        }
    }
}

static void handle_new_pointer_constraint(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_pointer_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;
    DtwmPointerConstraint *pc = calloc(1, sizeof(*pc));
    if (!pc) return;
    pc->server = server;
    pc->constraint = constraint;
    pc->set_region.notify = handle_pointer_constraint_set_region;
    wl_signal_add(&constraint->events.set_region, &pc->set_region);
    pc->destroy.notify = handle_pointer_constraint_destroy;
    wl_signal_add(&constraint->events.destroy, &pc->destroy);
    wl_list_insert(&server->tracked_pointer_constraints, &pc->link);
    sync_input_capture_for_focused_view(server, true);
}

static void handle_shortcuts_inhibitor_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmShortcutsInhibitor *si = wl_container_of(listener, si, destroy);
    DtwmServer *server = si->server;
    DtwmView *owner =
        (server && si->inhibitor && si->inhibitor->surface)
            ? find_view_owning_surface(server, si->inhibitor->surface)
            : nullptr;
    if (server && server->active_shortcuts_inhibitor == si->inhibitor) {
        server->active_shortcuts_inhibitor = nullptr;
    }
    wl_list_remove(&si->destroy.link);
    wl_list_remove(&si->link);
    free(si);
    if (server && !server->shutting_down) {
        if (owner && owner->mapped) {
            dtwm_view_update_frame(owner);
        }
        publish_input_capture_ipc(server);
    }
}

static void handle_new_shortcuts_inhibitor(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_shortcuts_inhibitor);
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;
    DtwmShortcutsInhibitor *si = calloc(1, sizeof(*si));
    if (!si) return;
    si->server = server;
    si->inhibitor = inhibitor;
    si->destroy.notify = handle_shortcuts_inhibitor_destroy;
    wl_signal_add(&inhibitor->events.destroy, &si->destroy);
    wl_list_insert(&server->tracked_shortcuts_inhibitors, &si->link);
    sync_input_capture_for_focused_view(server, true);
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *ev = data;
    uint32_t t_ms = ev->time_msec ? ev->time_msec : dtwm_now_msec();
    dtwm_lock_notify_activity(server);

    if (server->relative_pointer_mgr && !server->session_locked) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            server->relative_pointer_mgr,
            server->seat,
            (uint64_t)t_ms * 1000ull,
            ev->delta_x,
            ev->delta_y,
            ev->unaccel_dx,
            ev->unaccel_dy
        );
    }

    if (server->active_pointer_constraint && !server->session_locked &&
        server->cursor_mode == DTWM_CURSOR_PASSTHROUGH &&
        server->active_pointer_constraint->surface) {
        struct wlr_pointer_constraint_v1 *c = server->active_pointer_constraint;
        if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            return;
        }
        if (c->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
            double gx = 0.0, gy = 0.0, s = 1.0;
            if (surface_global_origin_and_scale(server, c->surface, &gx, &gy, &s)) {
                double sx1 = (server->cursor->x - gx) / s;
                double sy1 = (server->cursor->y - gy) / s;
                pixman_region32_t creg;
                build_pointer_constraint_region(c, &creg);
                clamp_point_into_region(&creg, &sx1, &sy1);
                double sx2 = sx1 + ev->delta_x / s;
                double sy2 = sy1 + ev->delta_y / s;
                double cx = sx1, cy = sy1;
                if (!wlr_region_confine(&creg, sx1, sy1, sx2, sy2, &cx, &cy)) {
                    cx = sx1;
                    cy = sy1;
                }
                pixman_region32_fini(&creg);
                wlr_cursor_warp_closest(server->cursor, nullptr, gx + cx * s, gy + cy * s);
                process_cursor_motion(server, t_ms);
                return;
            }
        }
    }

    wlr_cursor_move(server->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    process_cursor_motion(server, t_ms);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *ev = data;
    uint32_t t_ms = ev->time_msec ? ev->time_msec : dtwm_now_msec();
    dtwm_lock_notify_activity(server);

    double lx = 0.0, ly = 0.0;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &ev->pointer->base, ev->x, ev->y, &lx, &ly);
    double dx = lx - server->cursor->x;
    double dy = ly - server->cursor->y;

    if (server->relative_pointer_mgr && !server->session_locked && (dx != 0.0 || dy != 0.0)) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            server->relative_pointer_mgr,
            server->seat,
            (uint64_t)t_ms * 1000ull,
            dx,
            dy,
            dx,
            dy
        );
    }

    if (server->active_pointer_constraint && !server->session_locked &&
        server->cursor_mode == DTWM_CURSOR_PASSTHROUGH &&
        server->active_pointer_constraint->surface) {
        struct wlr_pointer_constraint_v1 *c = server->active_pointer_constraint;
        if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            return;
        }
        if (c->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
            double gx = 0.0, gy = 0.0, s = 1.0;
            if (surface_global_origin_and_scale(server, c->surface, &gx, &gy, &s)) {
                double sx1 = (server->cursor->x - gx) / s;
                double sy1 = (server->cursor->y - gy) / s;
                pixman_region32_t creg;
                build_pointer_constraint_region(c, &creg);
                clamp_point_into_region(&creg, &sx1, &sy1);
                double sx2 = (lx - gx) / s;
                double sy2 = (ly - gy) / s;
                double cx = sx1, cy = sy1;
                if (!wlr_region_confine(&creg, sx1, sy1, sx2, sy2, &cx, &cy)) {
                    cx = sx1;
                    cy = sy1;
                }
                pixman_region32_fini(&creg);
                wlr_cursor_warp_closest(server->cursor, nullptr, gx + cx * s, gy + cy * s);
                process_cursor_motion(server, t_ms);
                return;
            }
        }
    }

    wlr_cursor_warp_absolute(server->cursor, &ev->pointer->base, ev->x, ev->y);
    process_cursor_motion(server, t_ms);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *ev = data;
    uint32_t t_ms = ev->time_msec ? ev->time_msec : dtwm_now_msec();
    dtwm_lock_notify_activity(server);
    server->super_tap_pending = false;

    uint32_t btn = ev->button;
    if (server->left_handed) {
        if (btn == BTN_LEFT) btn = BTN_RIGHT;
        else if (btn == BTN_RIGHT) btn = BTN_LEFT;
    }
    if (ev->pointer) {
        DtwmPointer *ptr;
        wl_list_for_each(ptr, &server->pointers, link) {
            if (ptr->wlr_pointer == ev->pointer) {
                uint32_t bbit = pointer_button_bit(btn);
                if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
                    uint32_t other_held = ptr->held_mask & ~bbit;
                    if (ptr->is_virtual && other_held != 0) {
                        /* Auto-release any orphaned button left stuck by a VNC/browser
                         * context-menu steal or client reconnect before handling a new click. */
                        uint32_t rel_ts = t_ms;
                        if (ptr->held_since_ms != 0 && (int32_t)(rel_ts - ptr->held_since_ms) < 600) {
                            rel_ts = ptr->held_since_ms + 600u;
                        }
                        ptr->held_mask &= ~other_held;
                        for (unsigned int b = 0; b <= 16u; ++b) {
                            if (other_held & (1u << b)) {
                                uint32_t obtn = pointer_bit_to_button(b);
                                dtwm_xdnd_on_button_state(server, obtn, false);
                                wlr_seat_pointer_notify_button(
                                    server->seat, rel_ts, obtn, WL_POINTER_BUTTON_STATE_RELEASED
                                );
                            }
                        }
                        wlr_seat_pointer_notify_frame(server->seat);
                        if ((int32_t)(t_ms - rel_ts) <= 0) {
                            t_ms = rel_ts + 1u;
                        }
                    }
                    ptr->held_mask |= bbit;
                    ptr->held_since_ms = t_ms;
                } else {
                    ptr->held_mask &= ~bbit;
                }
                break;
            }
        }
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
        if (dtwm_icons_handle_desktop_button(server, nullptr, btn, false)) {
            return;
        }
        if (dtwm_icon_box_handle_button(server, btn, false, t_ms)) {
            return;
        }
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
        } else if (server->cursor_mode == DTWM_CURSOR_RESIZE && server->grabbed_view) {
            DtwmView *rv = server->grabbed_view;
            if (rv->kind == DTWM_VIEW_XDG && rv->xdg_toplevel &&
                rv->xdg_toplevel->base->initialized) {
                wlr_xdg_toplevel_set_resizing(rv->xdg_toplevel, false);
            }
            if (had_wireframe) {
                rv->resize_inflight = true;
                rv->has_queued_resize = false;
                rv->resize_sent_ms = dtwm_now_msec();
                if (rv->kind == DTWM_VIEW_XDG && rv->mapped && rv->xdg_toplevel) {
                    view_send_xdg_interactive_resize(rv, pending.width, pending.height);
                } else {
                    view_apply_geometry(rv, pending.x, pending.y, pending.width, pending.height);
                }
            } else if (!rv->resize_inflight && rv->has_queued_resize) {
                view_dispatch_queued_resize(rv, true);
            } else if (!rv->resize_inflight && !rv->has_queued_resize) {
                rv->active_resize_edges = 0;
            }
            /* If rv->resize_inflight is true, leave rv->has_queued_resize and
             * rv->active_resize_edges set so xdg_toplevel_commit anchors and
             * dispatches the single final size once the in-flight frame commits. */
        }
        if (server->cursor_mode != DTWM_CURSOR_PASSTHROUGH) {
            server->cursor_mode = DTWM_CURSOR_PASSTHROUGH;
            server->grabbed_view = nullptr;
            update_snap_preview(server, DTWM_SNAP_NONE, nullptr);
            return;
        }
        wlr_seat_pointer_notify_button(server->seat, t_ms, btn, ev->state);
        wlr_seat_pointer_notify_frame(server->seat);
        if (server->pointer_buttons_down == 0 && server->seat->drag == nullptr) {
            dtwm_recheck_cursor_focus(server);
        }
        return;
    }

    /* Button press */
    if (server->active_pointer_constraint &&
        server->active_pointer_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
        server->cursor_mode == DTWM_CURSOR_PASSTHROUGH &&
        server->active_pointer_constraint->surface) {
        wlr_seat_pointer_notify_button(server->seat, t_ms, btn, ev->state);
        wlr_seat_pointer_notify_frame(server->seat);
        return;
    }
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
        if (!server->menu_is_root && server->menu_target && btn == BTN_LEFT) {
            DtwmView *mt = server->menu_target;
            DtwmFramePart mpart = dtwm_frame_hit_test(
                mt, (int)(server->cursor->x - mt->x), (int)(server->cursor->y - mt->y)
            );
            if (mpart == DTWM_PART_BTN_MENU) {
                uint32_t dc_ms = server->double_click_ms > 0 ? server->double_click_ms : 500u;
                if (server->last_menu_btn_view == mt && (t_ms - server->last_menu_btn_ms) <= dc_ms) {
                    server->last_menu_btn_view = nullptr;
                    close_window_menu(server);
                    view_close(mt);
                    return;
                }
                server->last_menu_btn_view = nullptr;
                close_window_menu(server);
                return;
            }
        }
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
        dismiss_open_client_popups(server, t_ms);
        if (dtwm_icon_box_is_topmost_at(server, server->cursor->x, server->cursor->y)) {
            (void)dtwm_icon_box_handle_button(server, btn, true, t_ms);
            if (server->icon_box_active) {
                clear_active_focus(server);
            }
            return;
        }
        dtwm_icon_box_deactivate(server);
        DtwmView *icon_v = dtwm_icons_view_at(server, server->cursor->x, server->cursor->y);
        if (icon_v) {
            if (btn == BTN_LEFT) {
                (void)dtwm_icons_handle_desktop_button(server, icon_v, btn, true);
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

    const bool clicked_xdg_popup = surface_is_open_xdg_popup(server, surface);

    if (view && !(view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface &&
                  view->xwayland_surface->override_redirect)) {
        if (part != DTWM_PART_CLIENT) {
            dismiss_open_client_popups(server, t_ms);
        } else if (view->kind == DTWM_VIEW_XDG) {
            const struct wl_client *keep_cli =
                (surface && surface->resource) ? wl_resource_get_client(surface->resource) : nullptr;
            dismiss_open_client_popups_except_client(server, t_ms, keep_cli);
            if (surface) {
                wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
            }
        }
        if (part == DTWM_PART_CLIENT && server->input_escaped_view == view) {
            server->input_escaped_view = nullptr;
        }
        if (!clicked_xdg_popup) {
            focus_view_ex(
                server,
                view,
                surface,
                (part == DTWM_PART_CLIENT) ? server->focus_auto_raise : true
            );
        } else if (server->focused_view != view) {
            focus_view_ex(server, view, view_wlr_surface(view), false);
        }

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
                    uint32_t dc_ms = server->double_click_ms > 0 ? server->double_click_ms : 500u;
                    if (server->last_menu_btn_view == view &&
                        (t_ms - server->last_menu_btn_ms) <= dc_ms) {
                        server->last_menu_btn_view = nullptr;
                        close_window_menu(server);
                        view_close(view);
                        return;
                    }
                    server->last_menu_btn_view = view;
                    server->last_menu_btn_ms = t_ms;
                    int l, t, r, b;
                    dtwm_view_insets(view, &l, &t, &r, &b);
                    open_window_menu(server, view, view->x + l, view->y + t);
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

    if (surface && server->pointer_buttons_down <= 1) {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, t_ms, sx, sy);
        wlr_seat_pointer_notify_frame(server->seat);
    }

    wlr_seat_pointer_notify_button(server->seat, t_ms, btn, ev->state);
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *ev = data;
    uint32_t t_ms = ev->time_msec ? ev->time_msec : dtwm_now_msec();
    dtwm_lock_notify_activity(server);

    double delta = ev->delta;
    int32_t delta_discrete = ev->delta_discrete;
    if (fabs(delta) < 1e-4 && delta_discrete != 0) {
        delta = (double)delta_discrete * (15.0 / 120.0);
    }
    /* Apple Virtualization.framework's Virtual USB Digitizer (05ac:8106)
     * advertises HID Resolution Multiplier = 120 to Linux hid-input while
     * forwarding unmultiplied host scroll deltas (1..15 units instead of 120),
     * attenuating wheel/trackpad scroll range by ~8x unless normalized. */
    if (dtwm_hw_pointer_needs_hires_boost(ev->pointer) &&
        ((delta_discrete > -60 && delta_discrete < 60 && delta_discrete != 0) ||
         (delta_discrete == 0 && fabs(delta) > 1e-4 && fabs(delta) < 7.5))) {
        delta *= 8.0;
        delta_discrete *= 8;
    }

    enum wl_pointer_axis_relative_direction rel_dir = ev->relative_direction;
    if (server->wheel_natural_scroll && !dtwm_hw_pointer_is_touchpad(ev->pointer)) {
        delta = -delta;
        delta_discrete = -delta_discrete;
        rel_dir = (rel_dir == WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL)
                      ? WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED
                      : WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL;
    }

    if (dtwm_a11y_handle_axis(server, ev->orientation, delta, delta_discrete)) {
        return;
    }
    if (ev->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
        dtwm_icon_box_handle_axis(server, delta, delta_discrete)) {
        return;
    }
    if (!server->session_locked && server->seat->drag == nullptr &&
        server->pointer_buttons_down == 0 &&
        server->cursor_mode == DTWM_CURSOR_PASSTHROUGH && !server->menu_visible) {
        double sx = 0.0, sy = 0.0;
        struct wlr_surface *surface = nullptr;
        DtwmFramePart part = DTWM_PART_NONE;
        (void)desktop_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy, &part);
        if (surface) {
            wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(server->seat, t_ms, sx, sy);
        }
    }
    wlr_seat_pointer_notify_axis(
        server->seat, t_ms, ev->orientation, delta,
        delta_discrete, ev->source, rel_dir
    );
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmServer *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void handle_cursor_swipe_begin(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_swipe_begin);
    struct wlr_pointer_swipe_begin_event *ev = data;
    dtwm_lock_notify_activity(server);
    if (ev->fingers == 3 && !server->session_locked && !server->greeter_mode) {
        server->swipe_active = true;
        server->swipe_fingers = 3;
        server->swipe_dx = 0.0;
        server->swipe_dy = 0.0;
        return;
    }
    server->swipe_active = false;
    if (server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_swipe_begin(server->pointer_gestures, server->seat, ev->time_msec, ev->fingers);
    }
}

static void handle_cursor_swipe_update(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_swipe_update);
    struct wlr_pointer_swipe_update_event *ev = data;
    dtwm_lock_notify_activity(server);
    if (server->swipe_active && server->swipe_fingers == 3) {
        server->swipe_dx += ev->dx;
        server->swipe_dy += ev->dy;
        return;
    }
    if (server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_swipe_update(server->pointer_gestures, server->seat, ev->time_msec, ev->dx, ev->dy);
    }
}

static void handle_cursor_swipe_end(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_swipe_end);
    struct wlr_pointer_swipe_end_event *ev = data;
    dtwm_lock_notify_activity(server);
    if (server->swipe_active && server->swipe_fingers == 3) {
        server->swipe_active = false;
        if (!ev->cancelled) {
            dtwm_handle_touchpad_swipe(server, 3, server->swipe_dx, server->swipe_dy);
        }
        return;
    }
    if (server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_swipe_end(server->pointer_gestures, server->seat, ev->time_msec, ev->cancelled);
    }
}

static void handle_cursor_pinch_begin(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_pinch_begin);
    struct wlr_pointer_pinch_begin_event *ev = data;
    dtwm_lock_notify_activity(server);
    if (server->pointer_gestures && !server->session_locked) {
        wlr_pointer_gestures_v1_send_pinch_begin(server->pointer_gestures, server->seat, ev->time_msec, ev->fingers);
    }
}

static void handle_cursor_pinch_update(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_pinch_update);
    struct wlr_pointer_pinch_update_event *ev = data;
    dtwm_lock_notify_activity(server);
    if (server->pointer_gestures && !server->session_locked) {
        wlr_pointer_gestures_v1_send_pinch_update(
            server->pointer_gestures, server->seat, ev->time_msec, ev->dx, ev->dy, ev->scale, ev->rotation
        );
    }
}

static void handle_cursor_pinch_end(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_pinch_end);
    struct wlr_pointer_pinch_end_event *ev = data;
    dtwm_lock_notify_activity(server);
    if (server->pointer_gestures && !server->session_locked) {
        wlr_pointer_gestures_v1_send_pinch_end(server->pointer_gestures, server->seat, ev->time_msec, ev->cancelled);
    }
}

static void handle_cursor_hold_begin(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_hold_begin);
    struct wlr_pointer_hold_begin_event *ev = data;
    dtwm_lock_notify_activity(server);
    if (server->pointer_gestures && !server->session_locked) {
        wlr_pointer_gestures_v1_send_hold_begin(server->pointer_gestures, server->seat, ev->time_msec, ev->fingers);
    }
}

static void handle_cursor_hold_end(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, cursor_hold_end);
    struct wlr_pointer_hold_end_event *ev = data;
    dtwm_lock_notify_activity(server);
    if (server->pointer_gestures && !server->session_locked) {
        wlr_pointer_gestures_v1_send_hold_end(server->pointer_gestures, server->seat, ev->time_msec, ev->cancelled);
    }
}

/* --- Keyboard Input & CDE Shortcuts (Alt+Tab, Alt+F4, Alt+Space) --- */
static void handle_keyboard_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmKeyboard *kb = wl_container_of(listener, kb, modifiers);
    dtwm_lock_notify_activity(kb->server);
    wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
    dtwm_a11y_notify_seat_modifiers(kb->server, kb->wlr_keyboard);

    if (!kb->server->session_locked && kb->server->switcher_visible) {
        uint32_t mods = dtwm_a11y_effective_modifiers(kb->server, kb->wlr_keyboard);
        if (!(mods & WLR_MODIFIER_ALT)) {
            hide_task_switcher(kb->server, true);
        }
    }
}

static void publish_osd_state(DtwmServer *server) {
    if (!server || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE ||
        server->atom_osd_state == XCB_ATOM_NONE) {
        return;
    }
    char buf[256];
    int n = snprintf(
        buf, sizeof(buf),
        "visible=%d;kind=%s;level=%d;muted=%d;label=%s",
        server->osd_visible ? 1 : 0,
        server->osd_kind[0] ? server->osd_kind : "none",
        server->osd_level,
        server->osd_muted ? 1 : 0,
        server->osd_label
    );
    if (n > 0) {
        xcb_change_property(
            server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_root,
            server->atom_osd_state, XCB_ATOM_STRING, 8, (uint32_t)n, buf
        );
        xcb_flush(server->xcb_conn);
    }
}

static int handle_osd_timer(void *data) {
    DtwmServer *server = data;
    if (!server || !server->osd_buf) return 0;
    server->osd_visible = false;
    wlr_scene_node_set_enabled(&server->osd_buf->node, false);
    wlr_scene_buffer_set_buffer(server->osd_buf, nullptr);
    publish_osd_state(server);
    return 0;
}

static void show_hardware_osd(
    DtwmServer *server, const char *kind, int level, bool muted, const char *label
) {
    if (!server || !server->osd_buf) return;
    snprintf(server->osd_kind, sizeof(server->osd_kind), "%s", kind ? kind : "volume");
    server->osd_level = level;
    server->osd_muted = muted;
    snprintf(server->osd_label, sizeof(server->osd_label), "%s", label ? label : "");

    int odpi = dtwm_output_dpi_at(
        server,
        server->cursor ? server->cursor->x : 0.0,
        server->cursor ? server->cursor->y : 0.0
    );
    DtwmMetrics om = dtwm_metrics_for_dpi(odpi);
    int ow = 0, oh = 0;
    struct wlr_buffer *buf = dtwm_render_osd_gauge(
        &server->palette, server->osd_kind, level, muted, server->osd_label, odpi, &ow, &oh
    );
    if (!buf) return;

    struct wlr_box usable = {0, 0, 1280, 900};
    dtwm_get_usable_output_box(server, server->cursor->x, server->cursor->y, &usable);
    int ox = usable.x + (usable.width - ow) / 2;
    int oy = usable.y + usable.height - oh - (int)lround(44.0 * om.scale);
    if (oy < usable.y + (int)lround(24.0 * om.scale)) {
        oy = usable.y + (int)lround(24.0 * om.scale);
    }

    wlr_scene_buffer_set_buffer(server->osd_buf, buf);
    wlr_buffer_drop(buf);
    wlr_scene_node_set_position(&server->osd_buf->node, ox, oy);
    wlr_scene_node_set_enabled(&server->osd_buf->node, true);
    server->osd_visible = true;
    publish_osd_state(server);

    if (server->osd_timer) {
        wl_event_source_timer_update(server->osd_timer, 1600);
    }
}

static void invoke_media_ctl_and_osd(DtwmServer *server, const char *action) {
    if (!server || server->greeter_mode) return;
    int pfd[2];
    if (pipe(pfd) != 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > 2) close(dn);
        }
        close(pfd[1]);
        if (server->exe_dir[0]) {
            char local_bin[1152];
            snprintf(local_bin, sizeof(local_bin), "%s/dtwm-media-ctl", server->exe_dir);
            execl(local_bin, "dtwm-media-ctl", action, (char *)nullptr);
        }
        execlp("dtwm-media-ctl", "dtwm-media-ctl", action, (char *)nullptr);
        execl(CDE_INSTALLATION_TOP "/bin/dtwm-media-ctl", "dtwm-media-ctl", action, (char *)nullptr);
        _exit(127);
    }
    close(pfd[1]);
    char line[256] = {0};
    size_t used = 0;
    while (used + 1 < sizeof(line)) {
        ssize_t r = read(pfd[0], line + used, sizeof(line) - 1 - used);
        if (r > 0) used += (size_t)r;
        else if (r == -1 && errno == EINTR) continue;
        else break;
    }
    close(pfd[0]);
    int st = 0;
    (void)waitpid(pid, &st, 0);

    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = '\0';
    if (strncmp(line, "OSD|", 4) == 0) {
        char *p = line + 4;
        char *save = nullptr;
        const char *kind = strtok_r(p, "|", &save);
        const char *lvl_s = strtok_r(nullptr, "|", &save);
        const char *mut_s = strtok_r(nullptr, "|", &save);
        const char *lbl = strtok_r(nullptr, "", &save);
        if (kind && lvl_s && mut_s) {
            show_hardware_osd(server, kind, atoi(lvl_s), atoi(mut_s) != 0, lbl ? lbl : "");
        }
    }
}

static void spawn_session_helper(
    DtwmServer *server, const char *prog, const char *arg1, const char *arg2
) {
    if (!server || server->session_locked || server->greeter_mode) return;
    const char *act_tok_str = nullptr;
    struct wlr_xdg_activation_token_v1 *act_tok = nullptr;
    if (server->xdg_activation) {
        act_tok = wlr_xdg_activation_token_v1_create(server->xdg_activation);
        if (act_tok) {
            act_tok_str = wlr_xdg_activation_token_v1_get_name(act_tok);
        }
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            if (server->wayland_socket) setenv("WAYLAND_DISPLAY", server->wayland_socket, 1);
            if (server->xwayland && server->xwayland->display_name) {
                setenv("DISPLAY", server->xwayland->display_name, 1);
            }
            if (act_tok_str && act_tok_str[0]) {
                setenv("XDG_ACTIVATION_TOKEN", act_tok_str, 1);
            }
            if (arg1 && arg2) {
                execlp(prog, prog, arg1, arg2, (char *)nullptr);
                char dt_path[256];
                snprintf(dt_path, sizeof(dt_path), CDE_INSTALLATION_TOP "/bin/%s", prog);
                execl(dt_path, prog, arg1, arg2, (char *)nullptr);
            } else if (arg1) {
                execlp(prog, prog, arg1, (char *)nullptr);
                char dt_path[256];
                snprintf(dt_path, sizeof(dt_path), CDE_INSTALLATION_TOP "/bin/%s", prog);
                execl(dt_path, prog, arg1, (char *)nullptr);
            } else {
                execlp(prog, prog, (char *)nullptr);
                char dt_path[256];
                snprintf(dt_path, sizeof(dt_path), CDE_INSTALLATION_TOP "/bin/%s", prog);
                execl(dt_path, prog, (char *)nullptr);
                execl("/usr/bin/ck-grab", prog, (char *)nullptr);
            }
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
    }
}

static void spawn_quick_launcher(DtwmServer *server) {
    if (!server || server->session_locked || server->greeter_mode) return;
    const char *act_tok_str = nullptr;
    if (server->xdg_activation) {
        struct wlr_xdg_activation_token_v1 *act_tok =
            wlr_xdg_activation_token_v1_create(server->xdg_activation);
        if (act_tok) {
            act_tok_str = wlr_xdg_activation_token_v1_get_name(act_tok);
        }
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            if (server->wayland_socket) setenv("WAYLAND_DISPLAY", server->wayland_socket, 1);
            if (server->xwayland && server->xwayland->display_name) {
                setenv("DISPLAY", server->xwayland->display_name, 1);
            }
            if (act_tok_str && act_tok_str[0]) {
                setenv("XDG_ACTIVATION_TOKEN", act_tok_str, 1);
            }
            execlp("ck-launch", "ck-launch", "--toggle", (char *)nullptr);
            execl("/usr/dt/bin/ck-launch", "ck-launch", "--toggle", (char *)nullptr);
            execl("/usr/bin/ck-launch", "ck-launch", "--toggle", (char *)nullptr);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
    }
}

static bool handle_compositor_keybinding(DtwmServer *server, uint32_t mods, xkb_keysym_t sym) {
    if (dtwm_a11y_handle_shortcut(server, mods, sym)) {
        return true;
    }
    /* Hardware Multimedia, Backlight, Screenshot & Lock Keys (WP-13) */
    switch (sym) {
    case XKB_KEY_XF86AudioRaiseVolume:
        invoke_media_ctl_and_osd(server, "vol-up");
        return true;
    case XKB_KEY_XF86AudioLowerVolume:
        invoke_media_ctl_and_osd(server, "vol-down");
        return true;
    case XKB_KEY_XF86AudioMute:
        invoke_media_ctl_and_osd(server, "vol-mute");
        return true;
    case XKB_KEY_XF86AudioMicMute:
        invoke_media_ctl_and_osd(server, "mic-mute");
        return true;
    case XKB_KEY_XF86MonBrightnessUp:
        invoke_media_ctl_and_osd(server, "bright-up");
        return true;
    case XKB_KEY_XF86MonBrightnessDown:
        invoke_media_ctl_and_osd(server, "bright-down");
        return true;
    case XKB_KEY_XF86KbdBrightnessUp:
        invoke_media_ctl_and_osd(server, "kbd-bright-up");
        return true;
    case XKB_KEY_XF86KbdBrightnessDown:
        invoke_media_ctl_and_osd(server, "kbd-bright-down");
        return true;
    case XKB_KEY_XF86AudioPlay:
        invoke_media_ctl_and_osd(server, "media-play");
        return true;
    case XKB_KEY_XF86AudioPause:
        invoke_media_ctl_and_osd(server, "media-pause");
        return true;
    case XKB_KEY_XF86AudioNext:
        invoke_media_ctl_and_osd(server, "media-next");
        return true;
    case XKB_KEY_XF86AudioPrev:
        invoke_media_ctl_and_osd(server, "media-prev");
        return true;
    case XKB_KEY_XF86AudioStop:
        invoke_media_ctl_and_osd(server, "media-stop");
        return true;
    case XKB_KEY_Print:
    case XKB_KEY_Sys_Req:
        if (mods & WLR_MODIFIER_SHIFT) {
            spawn_session_helper(server, "ck-grab", "--region", "--clipboard");
        } else {
            spawn_session_helper(server, "ck-grab", nullptr, nullptr);
            show_hardware_osd(server, "screenshot", 100, false, "Screenshot: ck-grab");
        }
        return true;
    default:
        break;
    }

    if ((mods & WLR_MODIFIER_LOGO) && (sym == XKB_KEY_l || sym == XKB_KEY_L)) {
        show_hardware_osd(server, "lock", 100, false, "Session: Lock Screen");
        spawn_session_helper(server, "dtaction", "LockDisplay", nullptr);
        return true;
    }
    if (dtwm_power_display_handle_keybinding(server, mods, sym)) {
        return true;
    }

    if (((mods & WLR_MODIFIER_ALT) && sym == XKB_KEY_F2) ||
        ((mods & WLR_MODIFIER_LOGO) && sym == XKB_KEY_space)) {
        spawn_quick_launcher(server);
        return true;
    }
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
                server->focused_view->x + l,
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
            if (gv) {
                gv->active_resize_edges = 0;
                gv->resize_inflight = false;
                gv->has_queued_resize = false;
                if (gv->kind == DTWM_VIEW_XDG && gv->xdg_toplevel &&
                    gv->xdg_toplevel->base->initialized) {
                    wlr_xdg_toplevel_set_resizing(gv->xdg_toplevel, false);
                }
            }
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

bool dtwm_server_dispatch_keybinding(DtwmServer *server, uint32_t mods, xkb_keysym_t sym) {
    return handle_compositor_keybinding(server, mods, sym);
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

    if (dtwm_a11y_filter_key(server, kb->wlr_keyboard, ev, t_ms)) {
        return;
    }

    uint32_t keycode = ev->keycode + 8;
    const xkb_keysym_t *syms = nullptr;
    int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
    bool is_super_sym = false;
    bool is_ctrl_or_alt_sym = false;
    bool is_escape_sym = false;
    uint32_t eff_mods = mods;
    for (int i = 0; i < nsyms; ++i) {
        xkb_keysym_t s = syms[i];
        if (s == XKB_KEY_Super_L || s == XKB_KEY_Super_R) {
            is_super_sym = true;
        }
        if (s == XKB_KEY_Control_L || s == XKB_KEY_Control_R) {
            is_ctrl_or_alt_sym = true;
            if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                eff_mods |= WLR_MODIFIER_CTRL;
            }
        } else if (s == XKB_KEY_Alt_L || s == XKB_KEY_Alt_R ||
                   s == XKB_KEY_Meta_L || s == XKB_KEY_Meta_R) {
            is_ctrl_or_alt_sym = true;
            if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                eff_mods |= WLR_MODIFIER_ALT;
            }
        }
        if (s == XKB_KEY_Escape) {
            is_escape_sym = true;
        }
    }

    bool capture_active =
        (server->active_pointer_constraint != nullptr) ||
        (server->active_shortcuts_inhibitor != nullptr &&
         server->active_shortcuts_inhibitor->active);
    if (capture_active) {
        uint32_t ca_mask = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT;
        if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            if ((eff_mods & ca_mask) == ca_mask && is_escape_sym) {
                release_input_capture_via_escape_hatch(server);
                return;
            }
            if (is_ctrl_or_alt_sym && (eff_mods & ca_mask) == ca_mask &&
                (eff_mods & ~(uint32_t)ca_mask) == 0) {
                server->ctrl_alt_chord_pending = true;
            } else if (!is_ctrl_or_alt_sym) {
                server->ctrl_alt_chord_pending = false;
            }
        } else if (ev->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            if (is_ctrl_or_alt_sym && server->ctrl_alt_chord_pending) {
                wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
                if (!dtwm_ime_forward_key(server, kb->wlr_keyboard, ev)) {
                    wlr_seat_keyboard_notify_key(server->seat, t_ms, ev->keycode, ev->state);
                }
                release_input_capture_via_escape_hatch(server);
                return;
            }
        }
    } else {
        server->ctrl_alt_chord_pending = false;
    }

    bool shortcuts_inhibited =
        (server->active_shortcuts_inhibitor != nullptr &&
         server->active_shortcuts_inhibitor->active);

    if (!shortcuts_inhibited) {
        if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            if (is_super_sym) {
                if ((mods & ~(uint32_t)WLR_MODIFIER_LOGO) == 0) {
                    server->super_tap_pending = true;
                    server->super_tap_press_ms = t_ms;
                } else {
                    server->super_tap_pending = false;
                }
            } else {
                server->super_tap_pending = false;
                for (int i = 0; i < nsyms; ++i) {
                    if (handle_compositor_keybinding(server, mods, syms[i])) {
                        handled = true;
                        break;
                    }
                }
            }
        } else if (ev->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            if (is_super_sym && server->super_tap_pending) {
                server->super_tap_pending = false;
                if ((uint32_t)(t_ms - server->super_tap_press_ms) <= 600u) {
                    spawn_quick_launcher(server);
                }
            }
        }
    } else {
        server->super_tap_pending = false;
    }
    if (!handled) {
        if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED && server->focused_view &&
            server->focused_view->mapped && !server->focused_view->is_panel &&
            !server->focused_view->is_subpanel && (mods & (WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)) == 0) {
            bool is_enter = false;
            bool is_typing = false;
            for (int i = 0; i < nsyms; ++i) {
                xkb_keysym_t s = syms[i];
                bool is_mod =
                    (s >= XKB_KEY_Shift_L && s <= XKB_KEY_Hyper_R) ||
                    (s >= XKB_KEY_ISO_Lock && s <= XKB_KEY_ISO_Last_Group_Lock) ||
                    s == XKB_KEY_Mode_switch || s == XKB_KEY_Num_Lock;
                if (s == XKB_KEY_Return || s == XKB_KEY_KP_Enter) {
                    is_enter = true;
                } else if (!is_mod && s != XKB_KEY_Escape && s != XKB_KEY_Tab) {
                    is_typing = true;
                }
            }
            if (is_enter) {
                server->last_typing_ms = 0;
                server->last_typing_view = nullptr;
            } else if (is_typing) {
                server->last_typing_ms = dtwm_now_msec();
                server->last_typing_view = server->focused_view;
            }
        }
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
            dtwm_a11y_notify_keyboard_enter(server, fs, kb->wlr_keyboard);
        }
    }
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmKeyboard *kb = wl_container_of(listener, kb, destroy);
    DtwmServer *server = kb->server;
    wl_list_remove(&kb->keymap.link);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->link);
    free(kb);
    if (server && !wl_list_empty(&server->keyboards)) {
        DtwmKeyboard *fallback = wl_container_of(server->keyboards.next, fallback, link);
        if (fallback->wlr_keyboard && fallback->wlr_keyboard->keymap) {
            wlr_seat_set_keyboard(server->seat, fallback->wlr_keyboard);
            dtwm_ime_set_keyboard(server, fallback->wlr_keyboard);
            if (!server->session_locked && server->focused_view) {
                struct wlr_surface *fs = view_wlr_surface(server->focused_view);
                if (fs) {
                    dtwm_a11y_notify_keyboard_enter(server, fs, fallback->wlr_keyboard);
                }
            }
        }
    }
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
            dtwm_a11y_notify_keyboard_enter(server, fs, wlr_kbd);
        }
    }
}

static void update_seat_capabilities(DtwmServer *server) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

static void handle_pointer_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmPointer *ptr = wl_container_of(listener, ptr, destroy);
    DtwmServer *server = ptr->server;
    uint32_t lost_mask = ptr->held_mask;
    uint32_t held_since = ptr->held_since_ms;
    wl_list_remove(&ptr->destroy.link);
    wl_list_remove(&ptr->link);
    free(ptr);

    if (!server || server->shutting_down || lost_mask == 0 || !server->seat) {
        return;
    }
    uint32_t remaining = 0;
    DtwmPointer *other;
    wl_list_for_each(other, &server->pointers, link) {
        remaining |= other->held_mask;
    }
    uint32_t orphaned = lost_mask & ~remaining;
    if (orphaned == 0) {
        return;
    }
    uint32_t ts = dtwm_now_msec();
    if (held_since != 0 && (int32_t)(ts - held_since) < 600) {
        ts = held_since + 600u;
    }
    if (server->seat->pointer_state.focused_surface != nullptr) {
        wlr_seat_pointer_notify_motion(server->seat, ts, -200.0, -200.0);
        wlr_seat_pointer_notify_frame(server->seat);
    }
    for (unsigned int b = 0; b <= 16u; ++b) {
        if (orphaned & (1u << b)) {
            uint32_t btn = pointer_bit_to_button(b);
            dtwm_xdnd_on_button_state(server, btn, false);
            wlr_seat_pointer_notify_button(
                server->seat, ts + 1u, btn, WL_POINTER_BUTTON_STATE_RELEASED
            );
        }
    }
    wlr_seat_pointer_notify_frame(server->seat);
    if (server->pointer_buttons_down == 0 && server->seat->drag == nullptr) {
        dismiss_open_client_popups(server, ts + 2u);
        dtwm_recheck_cursor_focus(server);
    }
}

static void server_add_pointer(
    DtwmServer *server, struct wlr_pointer *wlr_ptr, bool is_virtual
) {
    if (!server || !wlr_ptr) return;
    DtwmPointer *ptr = calloc(1, sizeof(*ptr));
    if (!ptr) return;
    ptr->server = server;
    ptr->wlr_pointer = wlr_ptr;
    ptr->is_virtual = is_virtual;
    ptr->destroy.notify = handle_pointer_destroy;
    wl_signal_add(&wlr_ptr->base.events.destroy, &ptr->destroy);
    wl_list_insert(&server->pointers, &ptr->link);
}

static void handle_backend_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmServer *server = wl_container_of(listener, server, backend_destroy);
    server->shutting_down = true;
    wl_list_remove(&server->new_input.link);
    wl_list_init(&server->new_input.link);
    wl_list_remove(&server->new_output.link);
    wl_list_init(&server->new_output.link);
    wl_list_remove(&server->backend_destroy.link);
    wl_list_init(&server->backend_destroy.link);
    server->backend = nullptr;
    if (server->wl_display) {
        wl_display_terminate(server->wl_display);
    }
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
        server_add_pointer(server, wlr_pointer_from_input_device(dev), false);
        dtwm_hw_apply_input_device(server, dev);
        break;
    case WLR_INPUT_DEVICE_SWITCH:
        dtwm_power_display_on_new_input(server, dev);
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
    server_add_pointer(server, &ev->new_pointer->pointer, true);
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

static void handle_request_cursor_shape(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, request_cursor_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
    if (!ev || ev->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
        return;
    }
    if (server->seat->pointer_state.focused_client == ev->seat_client && server->cursor_mgr) {
        const char *name = wlr_cursor_shape_v1_name(ev->shape);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name ? name : "default");
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

static void handle_drag_icon_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmServer *server = wl_container_of(listener, server, drag_icon_destroy);
    if (server->drag_icon_destroy_attached) {
        wl_list_remove(&server->drag_icon_destroy.link);
        server->drag_icon_destroy_attached = false;
    }
    server->drag_icon_tree = nullptr;
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmServer *server = wl_container_of(listener, server, drag_destroy);
    if (server->drag_destroy_attached) {
        wl_list_remove(&server->drag_destroy.link);
        server->drag_destroy_attached = false;
    }
    if (server->drag_icon_destroy_attached) {
        wl_list_remove(&server->drag_icon_destroy.link);
        server->drag_icon_destroy_attached = false;
    }
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
    if (server->drag_icon_destroy_attached) {
        wl_list_remove(&server->drag_icon_destroy.link);
        server->drag_icon_destroy_attached = false;
    }
    if (server->drag_destroy_attached) {
        wl_list_remove(&server->drag_destroy.link);
        server->drag_destroy_attached = false;
    }
    server->drag_icon_tree = nullptr;
    if (drag->icon) {
        server->drag_icon_tree = wlr_scene_drag_icon_create(server->overlay_tree, drag->icon);
        if (server->drag_icon_tree) {
            server->drag_icon_destroy.notify = handle_drag_icon_destroy;
            wl_signal_add(&server->drag_icon_tree->node.events.destroy, &server->drag_icon_destroy);
            server->drag_icon_destroy_attached = true;
            wlr_scene_node_set_position(
                &server->drag_icon_tree->node, (int)server->cursor->x, (int)server->cursor->y
            );
        }
    }
    server->drag_destroy.notify = handle_drag_destroy;
    wl_signal_add(&drag->events.destroy, &server->drag_destroy);
    server->drag_destroy_attached = true;
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
    DtwmServer *server = output->server;
    if (server && server->anim_kind != DTWM_ANIM_KIND_NONE) {
        step_window_anim(server, dtwm_now_nsec());
    }
    if (server) {
        DtwmView *ev;
        wl_list_for_each(ev, &server->views, link) {
            if (ev->mapped && ev->kind == DTWM_VIEW_XDG && !ev->is_native_dpi) {
                if (server->grabbed_view != ev) {
                    double target_s = external_view_target_scale(ev);
                    if (fabs(target_s - ev->external_client_scale) > 1e-4) {
                        ev->external_client_scale = target_s;
                        notify_surface_tree_scale(ev->xdg_toplevel->base->surface, target_s);
                        if (!ev->is_lock_dialog && ev->client_w > 0 && ev->client_h > 0) {
                            ev->pending_resize_serial =
                                send_xdg_toplevel_size_for_view(ev, ev->client_w, ev->client_h);
                        }
                        dtwm_view_update_frame(ev);
                        continue;
                    }
                }
                dtwm_sync_external_view_scene(ev);
            }
        }
    }
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server->scene, output->wlr_output);
    if (!scene_output) return;

    if (!dtwm_a11y_render_output_frame(output, scene_output)) {
        wlr_scene_output_commit(scene_output, nullptr);
    }

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
    dtwm_power_display_on_output_destroy(server, output);
    dtwm_a11y_on_output_destroy(output);
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

static int resolve_initial_output_dpi(const DtwmServer *server, uint32_t output_idx, const char *name) {
    if (!server) return 96;
    if (output_idx < 16 && server->initial_output_dpi[output_idx] > 0) {
        return server->initial_output_dpi[output_idx];
    }
    if (name && name[0]) {
        for (int i = 0; i < server->cli_output_dpi_count; ++i) {
            if (strcmp(server->cli_output_dpi_names[i], name) == 0 &&
                server->cli_output_dpi_vals[i] > 0) {
                return server->cli_output_dpi_vals[i];
            }
        }
        int saved = dtwm_power_display_lookup_dpi(server, name);
        if (saved > 0) {
            return saved;
        }
    }
    return 96;
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
    wlr_output_state_set_scale(&state, 1.0f);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    DtwmOutput *output = calloc(1, sizeof(*output));
    output->server = server;
    output->wlr_output = wlr_output;
    output->output_idx = server->next_output_idx++;
    output->dpi = resolve_initial_output_dpi(server, output->output_idx, wlr_output->name);

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

    dtwm_sync_resource_manager_dpi(server);
    dtwm_reload_cursor_theme(server, server->cursor_theme, server->cursor_size);
    dtwm_update_output_backdrop(output);
    dtwm_update_front_panel(server);
    dtwm_icons_relayout(server);
    dtwm_sync_xwayland_randr_dpi(server);
    dtwm_lock_sync_outputs(server);
    dtwm_output_mgmt_sync(server);
    dtwm_power_display_on_new_output(server, output);
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
    DtwmMetrics m = dtwm_metrics_for_dpi(
        dtwm_output_dpi_at(server, obox.x + obox.width * 0.5, obox.y + obox.height * 0.5)
    );
    bool left_icons = !server->use_icon_box &&
                      (server->icon_placement == DTWM_ICON_PLACE_TOP_LEFT ||
                       server->icon_placement == DTWM_ICON_PLACE_BOTTOM_LEFT);
    int left_margin = (int)lround((left_icons ? 104.0 : 48.0) * m.scale);
    int step_x = (int)lround(36.0 * m.scale);
    int step_y = (int)lround(32.0 * m.scale);
    view->x = obox.x + left_margin + (idx % 6) * step_x;
    view->y = obox.y + step_x + (idx % 6) * step_y;
    if (view->frame_tree) {
        wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
    }
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, map);
    view->mapped = true;

    struct wl_client *client = wl_resource_get_client(view->xdg_toplevel->resource);
    if (wl_client_is_native_dpi(view->server, client)) {
        view->is_native_dpi = true;
        view->external_client_scale = 1.0;
    } else {
        view->external_client_scale = external_view_target_scale(view);
        notify_surface_tree_scale(view->xdg_toplevel->base->surface, view->external_client_scale);
    }

    struct wlr_box geo = view->xdg_toplevel->base->geometry;
    int lw = geo.width > 0 ? geo.width : view->xdg_toplevel->base->surface->current.width;
    int lh = geo.height > 0 ? geo.height : view->xdg_toplevel->base->surface->current.height;
    if (!view->is_native_dpi && view->external_client_scale > 0.25) {
        view->client_w = (int)lround((double)lw * view->external_client_scale);
        view->client_h = (int)lround((double)lh * view->external_client_scale);
    } else {
        view->client_w = lw;
        view->client_h = lh;
    }

    const char *app_id = view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "";
    const char *title = view->xdg_toplevel->title ? view->xdg_toplevel->title : "";
    if (!strcmp(app_id, "coverDialog") || !strcmp(title, "coverDialog") ||
        !strcmp(app_id, "lockDialog_popup") || !strcmp(title, "lockDialog_popup")) {
        view->is_lock_dialog = true;
        view->decorated = false;
        view->has_title = false;
        view->has_resize_handles = false;
        view->has_menu_btn = false;
        view->has_min_btn = false;
        view->has_max_btn = false;
        view->workspace_mask = (1u << DTWM_MAX_WORKSPACES) - 1u;
        struct wlr_box obox = {0};
        wlr_output_layout_get_box(view->server->output_layout, nullptr, &obox);
        bool is_cover = (!strcmp(app_id, "coverDialog") || !strcmp(title, "coverDialog"));
        if (is_cover && obox.width > 0 && obox.height > 0) {
            view->x = obox.x;
            view->y = obox.y;
            view->client_w = obox.width;
            view->client_h = obox.height;
            send_xdg_toplevel_size_for_view(view, obox.width, obox.height);
        } else if (obox.width > 0 && obox.height > 0) {
            view->x = obox.x + (obox.width - view->client_w) / 2;
            view->y = obox.y + (obox.height - view->client_h) / 2;
        }
        if (view->frame_tree && view->server->lock_dialog_tree) {
            wlr_scene_node_reparent(&view->frame_tree->node, view->server->lock_dialog_tree);
            wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
            wlr_scene_node_set_enabled(&view->frame_tree->node, true);
        }
        dtwm_view_update_frame(view);
        if (view->server->session_locked) {
            dtwm_lock_focus_surface(view->server, view->xdg_toplevel->base->surface);
        }
        return;
    }
    if (!strcmp(app_id, "dtwm-front-panel") || !strcmp(app_id, "FrontPanel")) {
        view->is_panel = true;
        view->decorated = true;
        view->has_title = false;
        view->has_resize_handles = false;
        view->has_menu_btn = false;
        view->has_min_btn = false;
        view->has_max_btn = false;
        view->workspace_mask = (1u << DTWM_MAX_WORKSPACES) - 1u;
        view->server->panel_view = view;
        if (view->frame_tree && view->server->layer_top_tree) {
            wlr_scene_node_reparent(&view->frame_tree->node, view->server->layer_top_tree);
        }
        dtwm_icons_on_view_map(view);
        dtwm_update_front_panel(view->server);
        dtwm_icons_relayout(view->server);
        dtwm_notify_panel_mapped(view->server);
        return;
    }
    if (!strncmp(app_id, "dtwm-subpanel", 13) ||
        (view->server->panel_view && view->server->panel_view->xdg_toplevel &&
         view->xdg_toplevel->parent == view->server->panel_view->xdg_toplevel)) {
        view->is_subpanel = true;
        view->subpanel_torn_off = false;
        view->decorated = true;
        view->has_title = false;
        view->has_resize_handles = false;
        view->has_menu_btn = false;
        view->has_min_btn = false;
        view->has_max_btn = false;
        view->workspace_mask = (1u << DTWM_MAX_WORKSPACES) - 1u;
        if (view->frame_tree && view->server->layer_top_tree) {
            wlr_scene_node_reparent(&view->frame_tree->node, view->server->layer_top_tree);
        }
        int outer_w = 0, outer_h = 0;
        dtwm_view_outer_size(view, &outer_w, &outer_h);
        int rel_x = 0;
        bool has_rel_x = (!strncmp(app_id, "dtwm-subpanel:", 14) &&
                          sscanf(app_id + 14, "%d", &rel_x) == 1);
        int base_px = view->server->panel_x > 0 ? view->server->panel_x : 0;
        if (has_rel_x) {
            view->x = base_px + rel_x;
        } else {
            view->x = base_px > 0 ? base_px : 120;
        }
        struct wlr_box obox = {0};
        wlr_output_layout_get_box(view->server->output_layout, nullptr, &obox);
        if (obox.width > 0 && outer_w > 0) {
            if (view->x + outer_w > obox.x + obox.width) {
                view->x = obox.x + obox.width - outer_w;
            }
            if (view->x < obox.x) {
                view->x = obox.x;
            }
        }
        int py = (view->server->panel_h > 0) ? view->server->panel_y : 640;
        view->y = py - outer_h;
        if (view->y < 0) view->y = 0;
        wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
        wlr_scene_node_set_enabled(&view->frame_tree->node, true);
        dtwm_view_update_frame(view);
        focus_view(view->server, view, view->xdg_toplevel->base->surface);
        return;
    }

    view->workspace_mask = 1u << view->server->current_workspace;
    place_initial_view(view);
    wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
    dtwm_view_update_frame(view);
    dtwm_icons_on_view_map(view);
    dtwm_foreign_toplevel_sync(view);
    bool had_activation = view->pending_activation;
    view->pending_activation = false;
    bool prevent_steal = false;
    if (!had_activation && view->server->focused_view &&
        view->server->focused_view != view && view->server->focused_view->mapped &&
        !view->server->focused_view->is_panel && !view->server->focused_view->is_subpanel &&
        view->server->last_typing_view == view->server->focused_view &&
        view->server->last_typing_ms != 0 &&
        (uint32_t)(dtwm_now_msec() - view->server->last_typing_ms) <= 2500u) {
        bool is_transient_of_focused =
            (view->server->focused_view->kind == DTWM_VIEW_XDG &&
             view->xdg_toplevel->parent != nullptr &&
             view->xdg_toplevel->parent == view->server->focused_view->xdg_toplevel);
        struct wl_client *new_cli = wl_resource_get_client(view->xdg_toplevel->resource);
        struct wl_client *foc_cli =
            (view->server->focused_view->kind == DTWM_VIEW_XDG &&
             view->server->focused_view->xdg_toplevel)
                ? wl_resource_get_client(view->server->focused_view->xdg_toplevel->resource)
                : nullptr;
        if (!is_transient_of_focused && new_cli != foc_cli &&
            strcasecmp(app_id, "ck-launch") != 0) {
            prevent_steal = true;
        }
    }
    if (view->server->pending_activation_tokens == 0 || had_activation) {
        dtwm_notify_panel_busy_stop(view->server);
    }
    if (dtwm_view_visible_on_current_ws(view)) {
        if (!prevent_steal) {
            focus_view(view->server, view, view->xdg_toplevel->base->surface);
        } else if (view->frame_tree && view->server->focused_view->frame_tree) {
            wlr_scene_node_place_below(
                &view->frame_tree->node, &view->server->focused_view->frame_tree->node
            );
        }
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
    if (view == view->server->last_title_click_view) {
        view->server->last_title_click_view = nullptr;
    }
    if (view == view->server->last_menu_btn_view) {
        view->server->last_menu_btn_view = nullptr;
    }
    if (view == view->server->last_typing_view) {
        view->server->last_typing_view = nullptr;
        view->server->last_typing_ms = 0;
    }
    if (view == view->server->input_escaped_view) {
        view->server->input_escaped_view = nullptr;
    }
    view->mapped = false;
    sync_input_capture_for_focused_view(view->server, false);
    view->pending_activation = false;
    view->has_pending_geom = false;
    view->pending_pos_set = false;
    view->resize_inflight = false;
    view->has_queued_resize = false;
    view->active_resize_edges = 0;
    view->pending_resize_serial = 0;
    wlr_scene_node_set_enabled(&view->frame_tree->node, false);
    dtwm_foreign_toplevel_destroy(view);
    dtwm_icons_on_view_unmap_or_destroy(view, false);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, commit);
    struct wl_client *client = wl_resource_get_client(view->xdg_toplevel->resource);
    if (!view->is_native_dpi && wl_client_is_native_dpi(view->server, client)) {
        view->is_native_dpi = true;
        view->external_client_scale = 1.0;
    }
    if (view->xdg_toplevel->base->initial_commit) {
        if (!view->is_native_dpi) {
            view->external_client_scale = external_view_target_scale(view);
            notify_surface_tree_scale(view->xdg_toplevel->base->surface, view->external_client_scale);
            dtwm_sync_external_view_scene(view);
        }
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
        uint32_t acked = view->xdg_toplevel->base->current.configure_serial;
        bool active_resize =
            (view->server->cursor_mode == DTWM_CURSOR_RESIZE &&
             view->server->grabbed_view == view);
        bool serial_settled =
            (view->pending_resize_serial == 0 || acked >= view->pending_resize_serial);

        /* Adopt newly committed client dimensions synchronously with the buffer
         * commit so the CDE bevel frame and surface clip always match the
         * visible client buffer without overhang or gaps. */
        struct wlr_box geo = view->xdg_toplevel->base->geometry;
        int lw = geo.width > 0 ? geo.width : view->xdg_toplevel->base->surface->current.width;
        int lh = geo.height > 0 ? geo.height : view->xdg_toplevel->base->surface->current.height;
        int nw = lw;
        int nh = lh;
        if (!view->is_native_dpi && view->external_client_scale > 0.25) {
            nw = (int)lround((double)lw * view->external_client_scale);
            nh = (int)lround((double)lh * view->external_client_scale);
        }
        if (!view->is_lock_dialog && nw > 0 && nh > 0 &&
            (nw != view->client_w || nh != view->client_h) &&
            (active_resize || view->active_resize_edges != 0 || serial_settled)) {
            int nx = view->x;
            int ny = view->y;
            if (view->active_resize_edges & WLR_EDGE_LEFT) {
                nx = view->resize_anchor_right - nw;
            }
            if (view->active_resize_edges & WLR_EDGE_TOP) {
                ny = view->resize_anchor_bottom - nh;
            }
            view->x = nx;
            view->y = ny;
            view->client_w = nw;
            view->client_h = nh;
            if (view->frame_tree) {
                wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
            }
            dtwm_view_update_frame(view);
            if (view == view->server->focused_view) {
                dtwm_publish_active_window_box(view->server);
            }
        } else if (!view->is_native_dpi) {
            dtwm_sync_external_view_scene(view);
        }

        if (serial_settled && view->pending_resize_serial != 0) {
            view->pending_resize_serial = 0;
        }
        if (view->resize_inflight && serial_settled) {
            view->resize_inflight = false;
            if (view->has_queued_resize) {
                view_dispatch_queued_resize(view, true);
            } else if (!active_resize) {
                view->active_resize_edges = 0;
            }
        } else if (!active_resize && !view->resize_inflight && !view->has_queued_resize &&
                   serial_settled) {
            view->active_resize_edges = 0;
        }
        if (view->client_bg_rect) {
            wlr_scene_node_set_enabled(
                &view->client_bg_rect->node, dtwm_view_wants_opaque_client_bg(view)
            );
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

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_app_id);
    if (view->mapped) {
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
    view->mapped = false;
    dtwm_anim_cancel_for_view(server, view);
    if (view == server->focused_view) {
        server->focused_view = nullptr;
    }
    if (view == server->menu_target) {
        close_window_menu(server);
    }
    if (view == server->last_title_click_view) {
        server->last_title_click_view = nullptr;
    }
    if (view == server->last_menu_btn_view) {
        server->last_menu_btn_view = nullptr;
    }
    if (view == server->last_typing_view) {
        server->last_typing_view = nullptr;
        server->last_typing_ms = 0;
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
    wl_list_remove(&view->set_app_id.link);
    wl_list_remove(&view->link);
    dtwm_foreign_toplevel_destroy(view);
    dtwm_icons_on_view_unmap_or_destroy(view, true);
    if (view->frame_tree) {
        wlr_scene_node_destroy(&view->frame_tree->node);
    }
    free(view);
    if (server && server->pointer_buttons_down == 0 &&
        server->seat && server->seat->drag == nullptr) {
        dtwm_recheck_cursor_focus(server);
    }
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
    struct wl_client *client = wl_resource_get_client(toplevel->resource);
    view->is_native_dpi = wl_client_is_native_dpi(server, client);
    view->external_client_scale = view->is_native_dpi ? 1.0 : external_view_target_scale(view);

    view_init_frame_scene_nodes(view, server->workspace_tree, toplevel->base, nullptr);
    wlr_scene_node_set_enabled(&view->frame_tree->node, false);

    struct wlr_surface *surf = toplevel->base->surface;
    if (!view->is_native_dpi) {
        notify_surface_tree_scale(surf, view->external_client_scale);
        dtwm_sync_external_view_scene(view);
    }
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
    view->set_app_id.notify = xdg_toplevel_set_app_id;
    wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);

    wl_list_insert(&server->views, &view->link);
}

/* XDG Popups */
static void xdg_popup_unconstrain(DtwmPopup *popup) {
    if (!popup || !popup->xdg_popup || !popup->server) {
        return;
    }
    struct wlr_box obox;
    double rx = popup->server->cursor ? popup->server->cursor->x : 0.0;
    double ry = popup->server->cursor ? popup->server->cursor->y : 0.0;
    if (popup->owner_view) {
        rx = popup->owner_view->x + popup->owner_view->client_w * 0.5;
        ry = popup->owner_view->y + popup->owner_view->client_h * 0.5;
    }
    get_primary_output_box(popup->server, rx, ry, &obox);
    if (popup->owner_view) {
        int l = 0, t = 0, r = 0, b = 0;
        dtwm_view_insets(popup->owner_view, &l, &t, &r, &b);
        double s = (!popup->owner_view->is_native_dpi &&
                    popup->owner_view->external_client_scale > 0.25)
                       ? popup->owner_view->external_client_scale
                       : 1.0;
        struct wlr_box rel = {
            .x = (int)lround((double)(obox.x - (popup->owner_view->x + l)) / s),
            .y = (int)lround((double)(obox.y - (popup->owner_view->y + t)) / s),
            .width = (int)lround((double)obox.width / s),
            .height = (int)lround((double)obox.height / s),
        };
        wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &rel);
        return;
    }
    wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &obox);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmPopup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        xdg_popup_unconstrain(popup);
    }
    if (popup->owner_view && !popup->owner_view->is_native_dpi) {
        notify_surface_tree_scale(
            popup->xdg_popup->base->surface, popup->owner_view->external_client_scale
        );
        dtwm_sync_external_view_scene(popup->owner_view);
    }
}

static void xdg_popup_reposition(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmPopup *popup = wl_container_of(listener, popup, reposition);
    xdg_popup_unconstrain(popup);
    if (popup->owner_view && !popup->owner_view->is_native_dpi) {
        dtwm_sync_external_view_scene(popup->owner_view);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmPopup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->link);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->reposition.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_xdg_popup);
    struct wlr_xdg_popup *xdg_popup = data;

    DtwmPopup *popup = calloc(1, sizeof(*popup));
    popup->server = server;
    popup->xdg_popup = xdg_popup;
    wl_list_insert(&server->popups, &popup->link);

    struct wlr_scene_tree *parent_tree = server->popup_tree;
    if (xdg_popup->parent) {
        struct wlr_xdg_surface *parent_xdg = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
        if (parent_xdg && parent_xdg->data) {
            parent_tree = parent_xdg->data;
        }
    }
    struct wlr_scene_tree *walk = parent_tree;
    while (walk && !walk->node.data) {
        walk = walk->node.parent;
    }
    if (walk && walk->node.data) {
        popup->owner_view = walk->node.data;
    }
    popup->scene_tree = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    xdg_popup->base->data = popup->scene_tree;
    if (popup->owner_view && !popup->owner_view->is_native_dpi) {
        notify_surface_tree_scale(
            xdg_popup->base->surface, popup->owner_view->external_client_scale
        );
        dtwm_sync_external_view_scene(popup->owner_view);
    }

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->reposition.notify = xdg_popup_reposition;
    wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);
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

/* --- Focus-Stealing Prevention & Startup Tokens (xdg-activation-v1) --- */
typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_xdg_activation_token_v1 *token;
    struct wl_listener destroy;
} DtwmActivationToken;

static DtwmView *find_view_by_wlr_surface(DtwmServer *server, struct wlr_surface *surf) {
    if (!server || !surf) {
        return nullptr;
    }
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->kind == DTWM_VIEW_XDG && v->xdg_toplevel &&
            v->xdg_toplevel->base && v->xdg_toplevel->base->surface == surf) {
            return v;
        }
        if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface &&
            v->xwayland_surface->surface == surf) {
            return v;
        }
    }
    return nullptr;
}

static void handle_activation_token_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmActivationToken *trk = wl_container_of(listener, trk, destroy);
    DtwmServer *server = trk->server;
    wl_list_remove(&trk->destroy.link);
    wl_list_remove(&trk->link);
    free(trk);
    if (server && server->pending_activation_tokens > 0) {
        server->pending_activation_tokens--;
        if (!server->shutting_down && server->pending_activation_tokens == 0) {
            dtwm_notify_panel_busy_stop(server);
        }
    }
}

static void handle_xdg_activation_new_token(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, xdg_activation_new_token);
    struct wlr_xdg_activation_token_v1 *token = data;
    if (!server || !token || server->shutting_down) {
        return;
    }
    DtwmActivationToken *trk = calloc(1, sizeof(*trk));
    if (!trk) {
        return;
    }
    trk->server = server;
    trk->token = token;
    trk->destroy.notify = handle_activation_token_destroy;
    wl_signal_add(&token->events.destroy, &trk->destroy);
    wl_list_insert(&server->activation_tokens, &trk->link);
    server->pending_activation_tokens++;
    if (server->pending_activation_tokens == 1) {
        dtwm_notify_panel_busy_start(server);
    }
}

static void handle_xdg_activation_request_activate(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, xdg_activation_request_activate);
    struct wlr_xdg_activation_v1_request_activate_event *ev = data;
    if (!server || server->shutting_down || server->session_locked || !ev || !ev->surface || !ev->token) {
        return;
    }

    DtwmView *target = find_view_by_wlr_surface(server, ev->surface);
    if (!target || target->is_desktop_pin || target->is_swallowed) {
        return;
    }

    const char *tok_app = ev->token->app_id ? ev->token->app_id : "";
    const char *tgt_app =
        (target->kind == DTWM_VIEW_XDG && target->xdg_toplevel && target->xdg_toplevel->app_id)
            ? target->xdg_toplevel->app_id
            : "";
    bool is_quick_launcher =
        (strcasecmp(tok_app, "ck-launch") == 0 || strcasecmp(tgt_app, "ck-launch") == 0 ||
         strcasecmp(tgt_app, "CkLaunch") == 0);

    /* Reject unauthenticated self-activation from an unfocused background client
     * that did not provide a valid seat input serial when another window is focused. */
    if (!is_quick_launcher && ev->token->surface != nullptr && ev->token->seat == nullptr &&
        server->focused_view != nullptr) {
        DtwmView *src_view = find_view_by_wlr_surface(server, ev->token->surface);
        if (src_view && src_view != server->focused_view &&
            !src_view->is_panel && !src_view->is_subpanel) {
            return;
        }
    }

    if (!target->mapped) {
        target->pending_activation = true;
        return;
    }

    if (target->minimized) {
        view_set_minimized(target, false);
    }
    if ((target->workspace_mask & (1u << server->current_workspace)) == 0) {
        for (uint32_t ws = 0; ws < server->num_workspaces; ++ws) {
            if (target->workspace_mask & (1u << ws)) {
                dtwm_switch_workspace(server, ws);
                break;
            }
        }
    }
    focus_view_ex(server, target, ev->surface, true);
    dtwm_notify_panel_busy_stop(server);
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
    if (!ls->server->session_locked &&
        ls->layer_surface->current.keyboard_interactive !=
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(ls->server->seat);
        if (kb) {
            wlr_seat_keyboard_notify_enter(
                ls->server->seat,
                ls->layer_surface->surface,
                kb->keycodes,
                kb->num_keycodes,
                &kb->modifiers
            );
        } else {
            wlr_seat_keyboard_notify_enter(
                ls->server->seat, ls->layer_surface->surface, nullptr, 0, nullptr
            );
        }
    }
    dtwm_recheck_cursor_focus(ls->server);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmLayerSurface *ls = wl_container_of(listener, ls, unmap);
    if (!ls->server->session_locked &&
        ls->server->seat->keyboard_state.focused_surface == ls->layer_surface->surface) {
        wlr_seat_keyboard_notify_clear_focus(ls->server->seat);
        if (ls->server->focused_view && ls->server->focused_view->mapped) {
            focus_view_ex(ls->server, ls->server->focused_view, nullptr, false);
        }
    }
    dtwm_recheck_cursor_focus(ls->server);
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
        if (view->server->xcb_popup_sink_win != XCB_WINDOW_NONE &&
            xs->window_id == view->server->xcb_popup_sink_win) {
            view->server->popup_sink_view = view;
            view->frame_tree = nullptr;
            return;
        }
        view->x = xs->x;
        view->y = xs->y;
        view->client_w = xs->width > 0 ? xs->width : 1;
        view->client_h = xs->height > 0 ? xs->height : 1;
        wl_list_remove(&view->link);
        wl_list_insert(&view->server->views, &view->link);
        view->frame_tree = wlr_scene_subsurface_tree_create(
            view->server->unmanaged_xwayland_tree, xs->surface
        );
        view->frame_tree->node.data = view;
        wlr_scene_node_set_position(&view->frame_tree->node, xs->x, xs->y);
        wlr_scene_node_raise_to_top(&view->frame_tree->node);
        if (view->server->pointer_buttons_down == 0 && view->server->seat->drag == nullptr) {
            dtwm_recheck_cursor_focus(view->server);
        }
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

        view_init_frame_scene_nodes(view, view->server->layer_top_tree, nullptr, xs->surface);

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

        view_init_frame_scene_nodes(view, view->server->layer_top_tree, nullptr, xs->surface);

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

    view_init_frame_scene_nodes(view, view->server->workspace_tree, nullptr, xs->surface);

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
    dtwm_notify_panel_busy_stop(view->server);
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
    if (view == view->server->last_title_click_view) {
        view->server->last_title_click_view = nullptr;
    }
    if (view == view->server->last_menu_btn_view) {
        view->server->last_menu_btn_view = nullptr;
    }
    if (view == view->server->popup_sink_view) {
        view->server->popup_sink_view = nullptr;
    } else if (view->xwayland_surface && view->xwayland_surface->override_redirect &&
               view->server->xcb_conn &&
               view->server->xcb_popup_sink_win != XCB_WINDOW_NONE) {
        uint32_t stack_below = XCB_STACK_MODE_BELOW;
        xcb_configure_window(
            view->server->xcb_conn,
            view->server->xcb_popup_sink_win,
            XCB_CONFIG_WINDOW_STACK_MODE,
            &stack_below
        );
        xcb_flush(view->server->xcb_conn);
    }
    view->mapped = false;
    view->has_pending_geom = false;
    view->pending_pos_set = false;
    view->resize_inflight = false;
    view->has_queued_resize = false;
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
        view->client_bg_rect = nullptr;
        view->content_tree = nullptr;
        view->frame_top_buf = nullptr;
        view->frame_bottom_buf = nullptr;
        view->frame_left_buf = nullptr;
        view->frame_right_buf = nullptr;
        view->popup_anchor_tree = nullptr;
    }
    if (view->server->pointer_buttons_down == 0 && view->server->seat->drag == nullptr) {
        dtwm_recheck_cursor_focus(view->server);
    }
}

static void xwayland_surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, commit);
    if (!view->mapped || !view->resize_inflight) {
        return;
    }
    bool active_resize =
        (view->server->cursor_mode == DTWM_CURSOR_RESIZE &&
         view->server->grabbed_view == view);
    view->resize_inflight = false;
    if (view->has_queued_resize) {
        view_dispatch_queued_resize(view, active_resize);
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
    if (!wl_list_empty(&view->commit.link)) {
        wl_list_remove(&view->commit.link);
    }
    view->map.notify = xwayland_map;
    wl_signal_add(&view->xwayland_surface->surface->events.map, &view->map);
    view->unmap.notify = xwayland_unmap;
    wl_signal_add(&view->xwayland_surface->surface->events.unmap, &view->unmap);
    view->commit.notify = xwayland_surface_commit;
    wl_signal_add(&view->xwayland_surface->surface->events.commit, &view->commit);
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
    if (!wl_list_empty(&view->commit.link)) {
        wl_list_remove(&view->commit.link);
        wl_list_init(&view->commit.link);
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
    if (view->is_panel) {
        if (ev->mask & XCB_CONFIG_WINDOW_WIDTH) view->client_w = ev->width;
        if (ev->mask & XCB_CONFIG_WINDOW_HEIGHT) view->client_h = ev->height;
        dtwm_update_front_panel(view->server);
        dtwm_icons_relayout(view->server);
        return;
    }
    if (view->is_subpanel && !view->subpanel_torn_off) {
        int outer_w = 0, outer_h = 0;
        if (ev->mask & XCB_CONFIG_WINDOW_WIDTH) view->client_w = ev->width;
        if (ev->mask & XCB_CONFIG_WINDOW_HEIGHT) view->client_h = ev->height;
        dtwm_view_outer_size(view, &outer_w, &outer_h);
        if (ev->mask & XCB_CONFIG_WINDOW_X) view->x = ev->x;
        struct wlr_output *pout = wlr_output_layout_output_at(
            view->server->output_layout, view->server->panel_x + 8, view->server->panel_y
        );
        if (pout) {
            struct wlr_box obox = {0};
            wlr_output_layout_get_box(view->server->output_layout, pout, &obox);
            if (obox.width > 0) {
                if (view->x + outer_w > obox.x + obox.width) view->x = obox.x + obox.width - outer_w;
                if (view->x < obox.x) view->x = obox.x;
            }
        }
        int py = (view->server->panel_h > 0) ? view->server->panel_y : 640;
        view->y = py - outer_h;
        if (view->y < 0) view->y = 0;
        if (view->frame_tree) {
            wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
        }
        sync_xwayland_client_pos(view);
        dtwm_view_update_frame(view);
        return;
    }
    int l, t, r, b;
    dtwm_view_insets(view, &l, &t, &r, &b);
    int nx = (ev->mask & XCB_CONFIG_WINDOW_X) ? (ev->x - l) : view->x;
    int ny = (ev->mask & XCB_CONFIG_WINDOW_Y) ? (ev->y - t) : view->y;
    int nw = (ev->mask & XCB_CONFIG_WINDOW_WIDTH) ? ev->width : view->client_w;
    int nh = (ev->mask & XCB_CONFIG_WINDOW_HEIGHT) ? ev->height : view->client_h;
    if (ev->mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT)) {
        view_apply_geometry(view, nx, ny, nw, nh);
    } else {
        sync_xwayland_client_pos(view);
    }
}

static void xwayland_request_activate(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, request_activate);
    if (view->mapped && !view->xwayland_surface->override_redirect &&
        !view->is_panel && !view->is_subpanel && !view->is_swallowed && !view->is_desktop_pin) {
        dtwm_restore_view_focus(view->server, view);
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

static void xwayland_set_class(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmView *view = wl_container_of(listener, view, set_app_id);
    if (view->mapped) {
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
                if (view->server->pointer_buttons_down == 0 &&
                    view->server->seat->drag == nullptr) {
                    dtwm_recheck_cursor_focus(view->server);
                }
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
    view->mapped = false;
    dtwm_anim_cancel_for_view(server, view);
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
    if (view == server->last_title_click_view) {
        server->last_title_click_view = nullptr;
    }
    if (view == server->last_menu_btn_view) {
        server->last_menu_btn_view = nullptr;
    }
    if (view == server->last_typing_view) {
        server->last_typing_view = nullptr;
        server->last_typing_ms = 0;
    }
    if (view == server->popup_sink_view) {
        server->popup_sink_view = nullptr;
    }
    if (!wl_list_empty(&view->map.link)) {
        wl_list_remove(&view->map.link);
        wl_list_init(&view->map.link);
    }
    if (!wl_list_empty(&view->unmap.link)) {
        wl_list_remove(&view->unmap.link);
        wl_list_init(&view->unmap.link);
    }
    if (!wl_list_empty(&view->commit.link)) {
        wl_list_remove(&view->commit.link);
        wl_list_init(&view->commit.link);
    }
    wl_list_remove(&view->associate.link);
    wl_list_remove(&view->dissociate.link);
    wl_list_remove(&view->request_configure.link);
    wl_list_remove(&view->request_activate.link);
    wl_list_remove(&view->request_close.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_app_id.link);
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
    if (server && server->pointer_buttons_down == 0 &&
        server->seat && server->seat->drag == nullptr) {
        dtwm_recheck_cursor_focus(server);
    }
}

static bool handle_xwayland_xwm_event(struct wlr_xwayland *xwayland, xcb_generic_event_t *event) {
    DtwmServer *server = xwayland ? (DtwmServer *)xwayland->data : nullptr;
    if (!server || !event || server->session_locked) {
        return false;
    }
    uint8_t rt = event->response_type & ~0x80u;
    if (rt == XCB_CONFIGURE_REQUEST) {
        const xcb_configure_request_event_t *creq = (const xcb_configure_request_event_t *)event;
        if ((creq->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) &&
            !(creq->value_mask & XCB_CONFIG_WINDOW_SIBLING) &&
            creq->stack_mode == XCB_STACK_MODE_ABOVE) {
            DtwmView *v;
            wl_list_for_each(v, &server->views, link) {
                if (v->kind == DTWM_VIEW_XWAYLAND && v->mapped && v->xwayland_surface &&
                    v->xwayland_surface->window_id == creq->window &&
                    !v->xwayland_surface->override_redirect &&
                    !v->is_panel && !v->is_subpanel && !v->is_desktop_pin &&
                    !v->is_swallowed && !v->is_lock_dialog) {
                    dtwm_restore_view_focus(server, v);
                    break;
                }
            }
        }
    } else if (rt == XCB_MAP_REQUEST) {
        const xcb_map_request_event_t *mreq = (const xcb_map_request_event_t *)event;
        DtwmView *v;
        wl_list_for_each(v, &server->views, link) {
            if (v->kind == DTWM_VIEW_XWAYLAND && v->mapped && v->minimized &&
                v->xwayland_surface && v->xwayland_surface->window_id == mreq->window &&
                !v->xwayland_surface->override_redirect &&
                !v->is_panel && !v->is_subpanel && !v->is_desktop_pin &&
                !v->is_swallowed && !v->is_lock_dialog) {
                dtwm_restore_view_focus(server, v);
                break;
            }
        }
    }
    return false;
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
    wl_list_init(&view->commit.link);
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
    view->set_app_id.notify = xwayland_set_class;
    wl_signal_add(&xs->events.set_class, &view->set_app_id);
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
    } else if (server->no_xwayland) {
        fprintf(fp, "NO_XWAYLAND=1\n");
    }
    const char *tts_env = getenv("TT_SESSION");
    if (!tts_env || !tts_env[0]) {
        tts_env = getenv("_SUN_TT_SESSION");
    }
    if (tts_env && tts_env[0]) {
        fprintf(fp, "TT_SESSION=%s\n", tts_env);
    }
    if (server->renderer_name[0]) {
        fprintf(fp, "RENDERER=%s\n", server->renderer_name);
    }
    fprintf(fp, "DMABUF=%d\n", server->linux_dmabuf ? 1 : 0);
    fprintf(fp, "PID=%d\n", (int)getpid());
    fclose(fp);
    rename(tmp_path, server->status_file);
}

static void ensure_wayland_ttsession_env(void) {
    const char *existing = getenv("TT_SESSION");
    if (!existing || !existing[0]) {
        existing = getenv("_SUN_TT_SESSION");
    }
    if (existing && existing[0]) {
        setenv("TT_SESSION", existing, 1);
        setenv("_SUN_TT_SESSION", existing, 1);
        return;
    }

    const char *cands[2] = {
#ifdef CDE_INSTALLATION_TOP
        CDE_INSTALLATION_TOP "/bin/ttsession",
#else
        "/usr/dt/bin/ttsession",
#endif
        "/usr/dt/bin/ttsession",
    };
    const char *tt_bin = nullptr;
    for (size_t i = 0; i < 2; ++i) {
        if (cands[i] && access(cands[i], X_OK) == 0) {
            tt_bin = cands[i];
            break;
        }
    }
    if (!tt_bin) return;

    int pfd[2] = {-1, -1};
    if (pipe(pfd) != 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        pid_t daemon_pid = fork();
        if (daemon_pid < 0) {
            _exit(127);
        }
        if (daemon_pid > 0) {
            _exit(0);
        }
        setsid();
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
        dup2(pfd[1], STDOUT_FILENO);
        if (pfd[1] > STDOUT_FILENO) close(pfd[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        unsetenv("DISPLAY");
        execl(tt_bin, "ttsession", "-p", (char *)nullptr);
        _exit(127);
    }
    close(pfd[1]);
    if (pid < 0) {
        close(pfd[0]);
        return;
    }

    char line[512] = {0};
    size_t used = 0;
    struct pollfd pf = {.fd = pfd[0], .events = POLLIN, .revents = 0};
    while (used + 1 < sizeof(line) && poll(&pf, 1, 4000) > 0 &&
           (pf.revents & (POLLIN | POLLHUP))) {
        char ch = '\0';
        ssize_t nr = read(pfd[0], &ch, 1);
        if (nr <= 0 || ch == '\n' || ch == '\r') break;
        line[used++] = ch;
    }
    line[used] = '\0';
    close(pfd[0]);

    int st = 0;
    (void)waitpid(pid, &st, 0);

    if (line[0]) {
        setenv("TT_SESSION", line, 1);
        setenv("_SUN_TT_SESSION", line, 1);
    }
}

/* --- Native Session Root-Property & WM-Request IPC Bus (--no-xwayland) --- */
#define DTWM_MAX_IPC_PEERS 32
#define DTWM_MAX_IPC_PROPS 64

typedef struct {
    int fd;
    struct wl_event_source *ev_src;
    DtwmServer *server;
    pid_t client_pid;
    char *rx_buf;
    size_t rx_len;
    size_t rx_cap;
    char *tx_buf;
    size_t tx_len;
    size_t tx_cap;
} DtwmIpcPeer;

typedef struct {
    char name[128];
    uint8_t *data;
    size_t len;
    bool used;
} DtwmIpcCachedProp;

static DtwmIpcPeer g_ipc_peers[DTWM_MAX_IPC_PEERS];
static DtwmIpcCachedProp g_ipc_props[DTWM_MAX_IPC_PROPS];

static void close_ipc_peer(DtwmIpcPeer *peer);

static void peer_queue_send(DtwmIpcPeer *peer, const char *buf, size_t len) {
    if (!peer || peer->fd < 0 || !buf || len == 0) return;
    size_t sent = 0;
    if (peer->tx_len == 0) {
        while (sent < len) {
            ssize_t n = send(peer->fd, buf + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) {
                sent += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            close_ipc_peer(peer);
            return;
        }
    }
    if (sent == len) return;

    size_t rem = len - sent;
    if (peer->tx_len + rem > 524288u) {
        /* Drop unresponsive/non-draining peer instead of stalling compositor */
        close_ipc_peer(peer);
        return;
    }
    if (peer->tx_len + rem > peer->tx_cap) {
        size_t ncap = peer->tx_cap ? peer->tx_cap * 2 : 16384u;
        while (ncap < peer->tx_len + rem) ncap *= 2;
        char *nb = realloc(peer->tx_buf, ncap);
        if (!nb) {
            close_ipc_peer(peer);
            return;
        }
        peer->tx_buf = nb;
        peer->tx_cap = ncap;
    }
    memcpy(peer->tx_buf + peer->tx_len, buf + sent, rem);
    peer->tx_len += rem;
    if (peer->ev_src) {
        wl_event_source_fd_update(peer->ev_src, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
    }
}

static int ipc_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void send_ipc_frame_peer(DtwmIpcPeer *peer, const char *atom_name, const void *data, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    if (!peer || peer->fd < 0 || !atom_name || len > 262144u) return;
    size_t alen = strlen(atom_name);
    size_t total_cap = 5 + alen + 1 + len * 2 + 2;
    char *msg = malloc(total_cap);
    if (!msg) return;
    int hn = snprintf(msg, total_cap, "PROP %s ", atom_name);
    if (hn > 0 && (size_t)hn < total_cap) {
        const uint8_t *bytes = (const uint8_t *)data;
        size_t o = (size_t)hn;
        for (size_t i = 0; i < len && o + 2 < total_cap; ++i) {
            msg[o++] = hex_chars[bytes[i] >> 4];
            msg[o++] = hex_chars[bytes[i] & 0x0fu];
        }
        msg[o++] = '\n';
        peer_queue_send(peer, msg, o);
    }
    free(msg);
}

static void cache_ipc_prop(const char *atom_name, const void *data, size_t len) {
    if (!atom_name || !atom_name[0] || !strcmp(atom_name, "_DT_WM_REQUEST")) return;
    int slot = -1;
    for (int i = 0; i < DTWM_MAX_IPC_PROPS; ++i) {
        if (g_ipc_props[i].used && !strcmp(g_ipc_props[i].name, atom_name)) {
            slot = i;
            break;
        }
        if (!g_ipc_props[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return;
    uint8_t *nb = len > 0 ? malloc(len) : nullptr;
    if (len > 0 && !nb) return;
    if (nb && data) memcpy(nb, data, len);
    free(g_ipc_props[slot].data);
    snprintf(g_ipc_props[slot].name, sizeof(g_ipc_props[slot].name), "%s", atom_name);
    g_ipc_props[slot].data = nb;
    g_ipc_props[slot].len = len;
    g_ipc_props[slot].used = true;
}

void dtwm_native_ipc_publish_prop(DtwmServer *server, const char *atom_name, const void *data, size_t len) {
    (void)server;
    cache_ipc_prop(atom_name, data, len);
    for (int i = 0; i < DTWM_MAX_IPC_PEERS; ++i) {
        if (g_ipc_peers[i].fd >= 0) {
            send_ipc_frame_peer(&g_ipc_peers[i], atom_name, data, len);
        }
    }
}

static bool rewrite_resource_manager_xft_dpi(
    const char *src, size_t src_len, int dpi, char **out_buf, size_t *out_len
) {
    if (!out_buf || !out_len) return false;
    if (dpi < 1 || dpi > 4096) dpi = 96;
    size_t cap = src_len + 64;
    char *dst = malloc(cap);
    if (!dst) return false;
    size_t dpos = 0;

    const char *p = src;
    const char *end = src ? (src + src_len) : nullptr;
    while (p && p < end && *p != '\0') {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *t = p;
        size_t tlen = llen;
        while (tlen > 0 && (*t == ' ' || *t == '\t')) {
            t++;
            tlen--;
        }
        bool is_xft_dpi =
            (tlen >= 8 && strncasecmp(t, "Xft.dpi:", 8) == 0) ||
            (tlen >= 9 && strncasecmp(t, "*Xft.dpi:", 9) == 0);
        if (!is_xft_dpi && llen > 0) {
            memcpy(dst + dpos, p, llen);
            dpos += llen;
            dst[dpos++] = '\n';
        }
        if (!nl) break;
        p = nl + 1;
    }
    int n = snprintf(dst + dpos, cap - dpos, "Xft.dpi:\t%d\n", dpi);
    if (n > 0) {
        dpos += (size_t)n;
    }
    dst[dpos] = '\0';
    *out_buf = dst;
    *out_len = dpos;
    return !(src && src_len == dpos && memcmp(src, dst, dpos) == 0);
}

void dtwm_sync_resource_manager_dpi(DtwmServer *server) {
    if (!server) return;
    int dpi = dtwm_primary_output_dpi(server);
    const char *cur_data = nullptr;
    size_t cur_len = 0;
    for (int i = 0; i < DTWM_MAX_IPC_PROPS; ++i) {
        if (g_ipc_props[i].used && !strcmp(g_ipc_props[i].name, "RESOURCE_MANAGER")) {
            cur_data = (const char *)g_ipc_props[i].data;
            cur_len = g_ipc_props[i].len;
            break;
        }
    }
    char *new_buf = nullptr;
    size_t new_len = 0;
    bool changed = rewrite_resource_manager_xft_dpi(cur_data, cur_len, dpi, &new_buf, &new_len);
    if (!new_buf) return;
    if (changed || !cur_data) {
        dtwm_native_ipc_publish_prop(server, "RESOURCE_MANAGER", new_buf, new_len);
    }
    if (server->xcb_conn && server->xcb_root != XCB_WINDOW_NONE) {
        xcb_get_property_cookie_t ck = xcb_get_property(
            server->xcb_conn, 0, server->xcb_root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 65536
        );
        xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
        bool xcb_same = false;
        if (rep) {
            int rlen = xcb_get_property_value_length(rep);
            if (rlen >= 0 && (size_t)rlen == new_len &&
                memcmp(xcb_get_property_value(rep), new_buf, new_len) == 0) {
                xcb_same = true;
            }
            free(rep);
        }
        if (!xcb_same) {
            xcb_change_property(
                server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_root,
                XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 8, (uint32_t)new_len, new_buf
            );
            xcb_flush(server->xcb_conn);
        }
    }
    free(new_buf);
}

static void register_native_dpi_pid(DtwmServer *server, pid_t pid) {
    if (!server || pid <= 0) return;
    bool found = false;
    for (size_t i = 0; i < server->native_dpi_pid_count; ++i) {
        if (server->native_dpi_pids[i] == pid) {
            found = true;
            break;
        }
    }
    if (!found && server->native_dpi_pid_count < DTWM_MAX_NATIVE_DPI_PIDS) {
        server->native_dpi_pids[server->native_dpi_pid_count++] = pid;
    }
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->kind == DTWM_VIEW_XDG && v->xdg_toplevel && !v->is_native_dpi) {
            pid_t vpid = 0;
            wl_client_get_credentials(
                wl_resource_get_client(v->xdg_toplevel->resource), &vpid, nullptr, nullptr
            );
            if (vpid == pid) {
                v->is_native_dpi = true;
                v->external_client_scale = 1.0;
                notify_surface_tree_scale(v->xdg_toplevel->base->surface, 1.0);
                if (v->mapped) {
                    dtwm_view_update_frame(v);
                }
            }
        }
    }
}

static void unregister_native_dpi_pid_if_unused(DtwmServer *server, pid_t pid) {
    if (!server || pid <= 0) return;
    for (int i = 0; i < DTWM_MAX_IPC_PEERS; ++i) {
        if (g_ipc_peers[i].fd >= 0 && g_ipc_peers[i].client_pid == pid) {
            return;
        }
    }
    for (size_t i = 0; i < server->native_dpi_pid_count; ++i) {
        if (server->native_dpi_pids[i] == pid) {
            server->native_dpi_pids[i] =
                server->native_dpi_pids[server->native_dpi_pid_count - 1];
            server->native_dpi_pid_count--;
            break;
        }
    }
}

static void close_ipc_peer(DtwmIpcPeer *peer) {
    if (!peer || peer->fd < 0) return;
    pid_t closed_pid = peer->client_pid;
    DtwmServer *srv = peer->server;
    if (peer->ev_src) {
        wl_event_source_remove(peer->ev_src);
        peer->ev_src = nullptr;
    }
    close(peer->fd);
    peer->fd = -1;
    peer->client_pid = 0;
    free(peer->rx_buf);
    peer->rx_buf = nullptr;
    peer->rx_len = 0;
    peer->rx_cap = 0;
    free(peer->tx_buf);
    peer->tx_buf = nullptr;
    peer->tx_len = 0;
    peer->tx_cap = 0;
    if (closed_pid > 0 && srv) {
        unregister_native_dpi_pid_if_unused(srv, closed_pid);
    }
}

static int handle_ipc_peer_fd(int fd, uint32_t mask, void *data) {
    DtwmIpcPeer *peer = data;
    if (!peer || peer->fd < 0) return 0;
    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        close_ipc_peer(peer);
        return 0;
    }
    if ((mask & WL_EVENT_WRITABLE) && peer->tx_len > 0) {
        size_t sent = 0;
        while (sent < peer->tx_len) {
            ssize_t n = send(fd, peer->tx_buf + sent, peer->tx_len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) {
                sent += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            close_ipc_peer(peer);
            return 0;
        }
        if (sent > 0) {
            if (sent < peer->tx_len) {
                memmove(peer->tx_buf, peer->tx_buf + sent, peer->tx_len - sent);
            }
            peer->tx_len -= sent;
        }
        if (peer->tx_len == 0 && peer->ev_src) {
            wl_event_source_fd_update(peer->ev_src, WL_EVENT_READABLE);
        }
    }
    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }
    char tmp[8192];
    for (;;) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n > 0) {
            if (peer->rx_len + (size_t)n + 1 > peer->rx_cap) {
                size_t ncap = peer->rx_cap ? peer->rx_cap * 2 : 65536;
                while (ncap < peer->rx_len + (size_t)n + 1) ncap *= 2;
                if (ncap > 1048576u) { close_ipc_peer(peer); return 0; }
                char *nb = realloc(peer->rx_buf, ncap);
                if (!nb) { close_ipc_peer(peer); return 0; }
                peer->rx_buf = nb;
                peer->rx_cap = ncap;
            }
            memcpy(peer->rx_buf + peer->rx_len, tmp, (size_t)n);
            peer->rx_len += (size_t)n;
            peer->rx_buf[peer->rx_len] = '\0';
        } else if (n == 0) {
            close_ipc_peer(peer);
            return 0;
        } else if (errno == EINTR) {
            continue;
        } else {
            break;
        }
    }

    if (!peer->rx_buf || peer->rx_len == 0) return 0;

    char *start = peer->rx_buf;
    char *nl = nullptr;
    while ((nl = strchr(start, '\n')) != nullptr) {
        *nl = '\0';
        if (!strncmp(start, "CLIENT_CAP native_dpi", 21)) {
            long cpid = 0;
            if (sscanf(start + 21, "%ld", &cpid) == 1 && cpid > 0) {
                peer->client_pid = (pid_t)cpid;
                register_native_dpi_pid(peer->server, (pid_t)cpid);
            }
        } else if (!strncmp(start, "SET ", 4)) {
            char *p = start + 4;
            char *sp = strrchr(p, ' ');
            if (sp) {
                *sp = '\0';
                const char *aname = p;
                const char *hex = sp + 1;
                size_t hlen = strlen(hex);
                uint8_t *payload = malloc(hlen / 2 + 1);
                if (payload) {
                    size_t dlen = 0;
                    for (size_t i = 0; i + 1 < hlen; i += 2) {
                        int hi = ipc_hex_nibble(hex[i]);
                        int lo = ipc_hex_nibble(hex[i + 1]);
                        if (hi < 0 || lo < 0) break;
                        payload[dlen++] = (uint8_t)((hi << 4) | lo);
                    }
                    payload[dlen] = 0;
                    if (!strcmp(aname, "_DT_WM_REQUEST")) {
                        dtwm_dispatch_wm_request_bytes(peer->server, (const char *)payload, dlen);
                    } else if (!strcmp(aname, "RESOURCE_MANAGER")) {
                        dtwm_apply_resource_manager_string(peer->server, (const char *)payload, dlen);
                        cache_ipc_prop(aname, payload, dlen);
                        dtwm_sync_resource_manager_dpi(peer->server);
                    } else {
                        cache_ipc_prop(aname, payload, dlen);
                        for (int i = 0; i < DTWM_MAX_IPC_PEERS; ++i) {
                            if (g_ipc_peers[i].fd >= 0 && &g_ipc_peers[i] != peer) {
                                send_ipc_frame_peer(&g_ipc_peers[i], aname, payload, dlen);
                            }
                        }
                    }
                    free(payload);
                }
            }
        }
        start = nl + 1;
    }

    if (peer->fd >= 0 && peer->rx_buf) {
        size_t rem = strlen(start);
        if (rem > 0 && start != peer->rx_buf)
            memmove(peer->rx_buf, start, rem);
        peer->rx_len = rem;
        peer->rx_buf[rem] = '\0';
    }
    return 0;
}

static int handle_native_ipc_listen(int lfd, uint32_t mask, void *data) {
    (void)mask;
    DtwmServer *server = data;
    for (;;) {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) break;
        int fl = fcntl(cfd, F_GETFL, 0);
        if (fl >= 0) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
        fcntl(cfd, F_SETFD, FD_CLOEXEC);

        int slot = -1;
        for (int i = 0; i < DTWM_MAX_IPC_PEERS; ++i) {
            if (g_ipc_peers[i].fd < 0) { slot = i; break; }
        }
        if (slot < 0) { close(cfd); continue; }
        g_ipc_peers[slot].fd = cfd;
        g_ipc_peers[slot].server = server;
        g_ipc_peers[slot].client_pid = 0;
        g_ipc_peers[slot].rx_buf = nullptr;
        g_ipc_peers[slot].rx_len = 0;
        g_ipc_peers[slot].rx_cap = 0;
        g_ipc_peers[slot].tx_buf = nullptr;
        g_ipc_peers[slot].tx_len = 0;
        g_ipc_peers[slot].tx_cap = 0;
        g_ipc_peers[slot].ev_src = wl_event_loop_add_fd(
            wl_display_get_event_loop(server->wl_display),
            cfd, WL_EVENT_READABLE, handle_ipc_peer_fd, &g_ipc_peers[slot]
        );
        for (int p = 0; p < DTWM_MAX_IPC_PROPS; ++p) {
            if (g_ipc_props[p].used && g_ipc_peers[slot].fd >= 0) {
                send_ipc_frame_peer(&g_ipc_peers[slot], g_ipc_props[p].name, g_ipc_props[p].data, g_ipc_props[p].len);
            }
        }
        if (g_ipc_peers[slot].fd >= 0) {
            peer_queue_send(&g_ipc_peers[slot], "READY\n", 6);
        }
    }
    return 0;
}

static void start_native_ipc_server(DtwmServer *server) {
    for (int i = 0; i < DTWM_MAX_IPC_PEERS; ++i) g_ipc_peers[i].fd = -1;
    server->native_ipc_fd = -1;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || rt[0] != '/' || !server->wayland_socket) return;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    int n = snprintf(
        server->native_ipc_path, sizeof(server->native_ipc_path),
        "%s/dtwm-wayland-%s.ipc", rt, server->wayland_socket
    );
    if (n <= 0 || (size_t)n >= sizeof(addr.sun_path)) {
        server->native_ipc_path[0] = '\0';
        return;
    }
    unlink(server->native_ipc_path);
    memcpy(addr.sun_path, server->native_ipc_path, (size_t)n + 1);

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0) return;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(lfd, 16) != 0) {
        close(lfd);
        unlink(server->native_ipc_path);
        server->native_ipc_path[0] = '\0';
        return;
    }
    chmod(server->native_ipc_path, 0600);
    server->native_ipc_fd = lfd;
    server->native_ipc_source = wl_event_loop_add_fd(
        wl_display_get_event_loop(server->wl_display),
        lfd, WL_EVENT_READABLE, handle_native_ipc_listen, server
    );
}

static void stop_native_ipc_server(DtwmServer *server) {
    for (int i = 0; i < DTWM_MAX_IPC_PEERS; ++i) close_ipc_peer(&g_ipc_peers[i]);
    for (int p = 0; p < DTWM_MAX_IPC_PROPS; ++p) {
        free(g_ipc_props[p].data);
        g_ipc_props[p].data = nullptr;
        g_ipc_props[p].used = false;
    }
    if (server->native_ipc_source) {
        wl_event_source_remove(server->native_ipc_source);
        server->native_ipc_source = nullptr;
    }
    if (server->native_ipc_fd >= 0) {
        close(server->native_ipc_fd);
        server->native_ipc_fd = -1;
    }
    if (server->native_ipc_path[0]) {
        unlink(server->native_ipc_path);
        server->native_ipc_path[0] = '\0';
    }
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
    dtwm_sync_resource_manager_dpi(server);
    dtwm_power_display_on_xwayland_ready(server);
    dtwm_a11y_on_xwayland_ready(server);
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

static bool drm_fd_has_hw_3d(int fd) {
    if (fd < 0) {
        return false;
    }
#ifdef DTWM_HAVE_XF86DRM_H
    drmVersionPtr ver = drmGetVersion(fd);
    if (!ver || !ver->name) {
        if (ver) {
            drmFreeVersion(ver);
        }
        return false;
    }
    static const char *const sw_or_2d_drivers[] = {
        "vkms", "simpledrm", "efidrm", "ofdrm", "vesadrm",
        "bochs-drm", "qxl", "cirrus", "ast", "mgag200",
        "udl", "evdi", "gud", "hyperv_drm", nullptr
    };
    for (size_t i = 0; sw_or_2d_drivers[i]; ++i) {
        if (strcmp(ver->name, sw_or_2d_drivers[i]) == 0) {
            drmFreeVersion(ver);
            return false;
        }
    }
    if (strcmp(ver->name, "virtio_gpu") == 0) {
        bool ok3d = false;
#ifdef DTWM_HAVE_VIRTGPU_DRM_H
        struct drm_virtgpu_getparam gp3d = { .param = VIRTGPU_PARAM_3D_FEATURES, .value = 0 };
        struct drm_virtgpu_getparam gpcx = { .param = VIRTGPU_PARAM_CONTEXT_INIT, .value = 0 };
        if (drmIoctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp3d) == 0 && gp3d.value != 0) {
            ok3d = true;
        } else if (drmIoctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gpcx) == 0 && gpcx.value != 0) {
            ok3d = true;
        }
#endif
        drmFreeVersion(ver);
        return ok3d;
    }
    drmFreeVersion(ver);
    return true;
#else
    return true;
#endif
}

static bool probe_hw_3d_render_support(struct wlr_backend *backend) {
    const char *explicit_dev = getenv("WLR_RENDER_DRM_DEVICE");
    if (explicit_dev && explicit_dev[0]) {
        int fd = open(explicit_dev, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fd = open(explicit_dev, O_RDONLY | O_CLOEXEC);
        }
        if (fd < 0) {
            return false;
        }
        bool ok = drm_fd_has_hw_3d(fd);
        close(fd);
        return ok;
    }

    int bfd = backend ? wlr_backend_get_drm_fd(backend) : -1;
    if (bfd >= 0) {
        if (drm_fd_has_hw_3d(bfd)) {
            return true;
        }
        /* Multi-GPU split KMS/render check (e.g. DisplayLink/simpledrm + discrete/iGPU renderD*) */
    } else {
        const char *backends_env = getenv("WLR_BACKENDS");
        if (backends_env && strncasecmp(backends_env, "headless", 8) == 0 &&
            (backends_env[8] == '\0' || backends_env[8] == ',')) {
            return false;
        }
    }

    DIR *dir = opendir("/dev/dri");
    if (!dir) {
        return false;
    }
    bool found_hw = false;
    struct dirent *de;
    while ((de = readdir(dir)) != nullptr) {
        if (strncmp(de->d_name, "renderD", 7) != 0) {
            continue;
        }
        char path[256];
        snprintf(path, sizeof(path), "/dev/dri/%s", de->d_name);
        int rfd = open(path, O_RDWR | O_CLOEXEC);
        if (rfd < 0) {
            rfd = open(path, O_RDONLY | O_CLOEXEC);
        }
        if (rfd >= 0) {
            bool ok = drm_fd_has_hw_3d(rfd);
            close(rfd);
            if (ok) {
                found_hw = true;
                break;
            }
        }
    }
    closedir(dir);
    return found_hw;
}

static const char *identify_wlr_renderer_name(struct wlr_renderer *r) {
    if (!r) {
        return "none";
    }
    if (wlr_renderer_is_pixman(r)) {
        return "pixman";
    }
#ifdef DTWM_HAVE_WLR_GLES2_H
    if (wlr_renderer_is_gles2(r)) {
        return "gles2";
    }
#endif
#ifdef DTWM_HAVE_WLR_VULKAN_H
    if (wlr_renderer_is_vk(r)) {
        return "vulkan";
    }
#endif
    return "gpu";
}

static struct wlr_renderer *create_smart_wlr_renderer(
    struct wlr_backend *backend, char *out_name, size_t out_cap
) {
    struct wlr_renderer *r = nullptr;
    const char *req_env = getenv("WLR_RENDERER");
    bool auto_mode = (!req_env || req_env[0] == '\0' || strcasecmp(req_env, "auto") == 0);

    if (!auto_mode &&
        strcasecmp(req_env, "pixman") != 0 &&
        strcasecmp(req_env, "gles2") != 0 &&
        strcasecmp(req_env, "vulkan") != 0) {
        fprintf(
            stderr,
            "dtwm-wayland: unrecognized WLR_RENDERER=%s; falling back to auto selection\n",
            req_env
        );
        unsetenv("WLR_RENDERER");
        req_env = nullptr;
        auto_mode = true;
    }

    if (!auto_mode) {
        r = wlr_renderer_autocreate(backend);
        if (!r && strcasecmp(req_env, "pixman") != 0) {
            fprintf(
                stderr,
                "dtwm-wayland: WLR_RENDERER=%s unavailable; falling back to pixman\n",
                req_env
            );
            setenv("WLR_RENDERER", "pixman", 1);
            r = wlr_pixman_renderer_create();
        }
    } else {
        if (req_env && strcasecmp(req_env, "auto") == 0) {
            unsetenv("WLR_RENDERER");
        }
        if (probe_hw_3d_render_support(backend)) {
            r = wlr_renderer_autocreate(backend);
            if (r && !wlr_renderer_is_pixman(r) && wlr_renderer_get_drm_fd(r) < 0) {
                /* Reject unaccelerated software EGL/llvmpipe fallback in favor of native pixman */
                wlr_renderer_destroy(r);
                r = nullptr;
            }
        }
        if (!r) {
            setenv("WLR_RENDERER", "pixman", 1);
            r = wlr_pixman_renderer_create();
        }
    }

    const char *rname = identify_wlr_renderer_name(r);
    if (out_name && out_cap > 0) {
        snprintf(out_name, out_cap, "%s", rname);
    }
    return r;
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
    bool no_xwayland = false;
    const char *env_nox = getenv("DTWM_WAYLAND_NO_XWAYLAND");
    if (env_nox && (!strcmp(env_nox, "1") || !strcasecmp(env_nox, "true"))) {
        no_xwayland = true;
    }

    int opt_output_dpi[16] = {0};
    char opt_output_dpi_names[16][64] = {{0}};
    int opt_output_dpi_vals[16] = {0};
    int opt_output_dpi_count = 0;

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
        {"no-xwayland", no_argument, nullptr, 'X'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "s:f:w:A:D:SgPILXh", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 's': startup_cmd = optarg; break;
        case 'f': status_file = optarg; break;
        case 'w': swallow_client = optarg; break;
        case 'A': profile_arg = optarg; break;
        case 'D': {
            unsigned int oidx = 0;
            int odpi = 96;
            char eq_chk = '\0';
            if (sscanf(optarg, "%u%c%d", &oidx, &eq_chk, &odpi) == 3 &&
                eq_chk == '=' && oidx < 16 && odpi >= 1 && odpi <= 4096) {
                opt_output_dpi[oidx] = odpi;
            } else {
                const char *eq = strchr(optarg, '=');
                if (eq && eq > optarg && opt_output_dpi_count < 16) {
                    int ndpi = atoi(eq + 1);
                    size_t nlen = (size_t)(eq - optarg);
                    if (ndpi >= 1 && ndpi <= 4096 && nlen < sizeof(opt_output_dpi_names[0])) {
                        memcpy(opt_output_dpi_names[opt_output_dpi_count], optarg, nlen);
                        opt_output_dpi_names[opt_output_dpi_count][nlen] = '\0';
                        opt_output_dpi_vals[opt_output_dpi_count] = ndpi;
                        opt_output_dpi_count++;
                    }
                }
            }
            break;
        }
        case 'S': break;
        case 'g': greeter_mode = true; break;
        case 'P': no_panel = true; break;
        case 'I': no_open_icons = true; break;
        case 'L': large_icons = true; break;
        case 'X': no_xwayland = true; break;
        case 'h':
            printf(
                "Usage: %s [--session] [--greeter] [--no-xwayland] [--no-panel] [--no-open-window-icons] "
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
    server.no_xwayland = no_xwayland;
    server.native_ipc_fd = -1;
    memcpy(server.initial_output_dpi, opt_output_dpi, sizeof(opt_output_dpi));
    memcpy(server.cli_output_dpi_names, opt_output_dpi_names, sizeof(opt_output_dpi_names));
    memcpy(server.cli_output_dpi_vals, opt_output_dpi_vals, sizeof(opt_output_dpi_vals));
    server.cli_output_dpi_count = opt_output_dpi_count;
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
    wl_list_init(&server.popups);
    wl_list_init(&server.keyboards);
    wl_list_init(&server.pointers);
    wl_list_init(&server.activation_tokens);
    wl_list_init(&server.tracked_pointer_constraints);
    wl_list_init(&server.tracked_shortcuts_inhibitors);

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
    wl_list_init(&server.new_input.link);
    wl_list_init(&server.new_output.link);
    server.backend_destroy.notify = handle_backend_destroy;
    wl_signal_add(&server.backend->events.destroy, &server.backend_destroy);

    server.renderer = create_smart_wlr_renderer(
        server.backend, server.renderer_name, sizeof(server.renderer_name)
    );
    if (!server.renderer) {
        fprintf(stderr, "dtwm-wayland: failed to create wlroots renderer\n");
        return 1;
    }
    if (!wlr_renderer_init_wl_shm(server.renderer, server.wl_display)) {
        fprintf(stderr, "dtwm-wayland: failed to initialize wl_shm\n");
        return 1;
    }
    if (wlr_renderer_get_texture_formats(server.renderer, WLR_BUFFER_CAP_DMABUF) != nullptr) {
        server.linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(
            server.wl_display, 4, server.renderer
        );
        if (server.linux_dmabuf) {
            int r_drm_fd = wlr_renderer_get_drm_fd(server.renderer);
            if (r_drm_fd >= 0 &&
                server.renderer->features.timeline &&
                server.backend->features.timeline) {
                server.drm_syncobj_mgr = wlr_linux_drm_syncobj_manager_v1_create(
                    server.wl_display, 1, r_drm_fd
                );
            }
        }
    }

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator) {
        fprintf(stderr, "dtwm-wayland: failed to create wlroots allocator\n");
        return 1;
    }

    server.compositor = wlr_compositor_create(server.wl_display, 6, server.renderer);
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);
    wlr_data_control_manager_v1_create(server.wl_display);
    wlr_primary_selection_v1_device_manager_create(server.wl_display);
    wlr_viewporter_create(server.wl_display);
    wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
    wlr_screencopy_manager_v1_create(server.wl_display);
    server.presentation = wlr_presentation_create(server.wl_display, server.backend, 2);
    server.single_pixel_mgr = wlr_single_pixel_buffer_manager_v1_create(server.wl_display);
    server.alpha_modifier_mgr = wlr_alpha_modifier_v1_create(server.wl_display);

    server.output_layout = wlr_output_layout_create(server.wl_display);
    wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

    server.scene = wlr_scene_create();
    if (server.linux_dmabuf) {
        wlr_scene_set_linux_dmabuf_v1(server.scene, server.linux_dmabuf);
    }
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
    server.anim_tree = wlr_scene_tree_create(server.overlay_tree);
    wlr_scene_node_set_enabled(&server.anim_tree->node, false);
    server.anim_trail_buf = wlr_scene_buffer_create(server.anim_tree, nullptr);
    wlr_scene_node_set_enabled(&server.anim_trail_buf->node, false);
    server.anim_client_buf = wlr_scene_buffer_create(server.anim_tree, nullptr);
    wlr_scene_node_set_enabled(&server.anim_client_buf->node, false);
    server.anim_icon_buf = wlr_scene_buffer_create(server.anim_tree, nullptr);
    wlr_scene_node_set_enabled(&server.anim_icon_buf->node, false);
    server.anim_frame_buf = wlr_scene_buffer_create(server.anim_tree, nullptr);
    wlr_scene_node_set_enabled(&server.anim_frame_buf->node, false);
    server.anim_timer = wl_event_loop_add_timer(ev_loop, handle_anim_timer, &server);
    server.resize_flush_timer = wl_event_loop_add_timer(ev_loop, handle_resize_flush_timer, &server);
    server.menu_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.menu_buf->node, false);
    server.switcher_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.switcher_buf->node, false);
    server.osd_buf = wlr_scene_buffer_create(server.overlay_tree, nullptr);
    wlr_scene_node_set_enabled(&server.osd_buf->node, false);
    server.osd_timer = wl_event_loop_add_timer(ev_loop, handle_osd_timer, &server);

    server.move_opaque = true;
    server.focus_follows_mouse = false;
    server.focus_auto_raise = true;
    server.allow_primary_on_top = false;
    server.legacy_animate_minimize = true;
    server.minimize_anim_style = DTWM_ANIM_STYLE_AUTO;
    server.maximize_anim_style = DTWM_ANIM_STYLE_AUTO;
    server.anim_speed = DTWM_ANIM_SPEED_STANDARD;
    server.show_task_switcher = true;
    server.double_click_ms = 500;
    server.cursor_size = 24;

    dtwm_workspaces_init(&server);
    dtwm_icons_init(&server);
    dtwm_lock_init(&server);
    dtwm_output_mgmt_init(&server);
    dtwm_ime_init(&server);
    dtwm_power_display_init(&server);
    dtwm_a11y_init(&server);
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

    server.xdg_activation = wlr_xdg_activation_v1_create(server.wl_display);
    if (server.xdg_activation) {
        server.xdg_activation->token_timeout_msec = 15000;
        server.xdg_activation_new_token.notify = handle_xdg_activation_new_token;
        wl_signal_add(&server.xdg_activation->events.new_token, &server.xdg_activation_new_token);
        server.xdg_activation_request_activate.notify = handle_xdg_activation_request_activate;
        wl_signal_add(
            &server.xdg_activation->events.request_activate, &server.xdg_activation_request_activate
        );
    }

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

    server.pointer_gestures = wlr_pointer_gestures_v1_create(server.wl_display);
    server.relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(server.wl_display);
    server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);
    if (server.pointer_constraints) {
        server.new_pointer_constraint.notify = handle_new_pointer_constraint;
        wl_signal_add(
            &server.pointer_constraints->events.new_constraint, &server.new_pointer_constraint
        );
    }
    server.shortcuts_inhibit_mgr = wlr_keyboard_shortcuts_inhibit_v1_create(server.wl_display);
    if (server.shortcuts_inhibit_mgr) {
        server.new_shortcuts_inhibitor.notify = handle_new_shortcuts_inhibitor;
        wl_signal_add(
            &server.shortcuts_inhibit_mgr->events.new_inhibitor, &server.new_shortcuts_inhibitor
        );
    }
    server.cursor_swipe_begin.notify = handle_cursor_swipe_begin;
    wl_signal_add(&server.cursor->events.swipe_begin, &server.cursor_swipe_begin);
    server.cursor_swipe_update.notify = handle_cursor_swipe_update;
    wl_signal_add(&server.cursor->events.swipe_update, &server.cursor_swipe_update);
    server.cursor_swipe_end.notify = handle_cursor_swipe_end;
    wl_signal_add(&server.cursor->events.swipe_end, &server.cursor_swipe_end);
    server.cursor_pinch_begin.notify = handle_cursor_pinch_begin;
    wl_signal_add(&server.cursor->events.pinch_begin, &server.cursor_pinch_begin);
    server.cursor_pinch_update.notify = handle_cursor_pinch_update;
    wl_signal_add(&server.cursor->events.pinch_update, &server.cursor_pinch_update);
    server.cursor_pinch_end.notify = handle_cursor_pinch_end;
    wl_signal_add(&server.cursor->events.pinch_end, &server.cursor_pinch_end);
    server.cursor_hold_begin.notify = handle_cursor_hold_begin;
    wl_signal_add(&server.cursor->events.hold_begin, &server.cursor_hold_begin);
    server.cursor_hold_end.notify = handle_cursor_hold_end;
    wl_signal_add(&server.cursor->events.hold_end, &server.cursor_hold_end);

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
    server.cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(server.wl_display, 1);
    if (server.cursor_shape_mgr) {
        server.request_cursor_shape.notify = handle_request_cursor_shape;
        wl_signal_add(
            &server.cursor_shape_mgr->events.request_set_shape, &server.request_cursor_shape
        );
    }
    server.request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);
    server.request_set_primary_selection.notify = handle_request_set_primary_selection;
    wl_signal_add(&server.seat->events.request_set_primary_selection, &server.request_set_primary_selection);
    server.request_start_drag.notify = handle_request_start_drag;
    wl_signal_add(&server.seat->events.request_start_drag, &server.request_start_drag);
    server.start_drag.notify = handle_start_drag;
    wl_signal_add(&server.seat->events.start_drag, &server.start_drag);

    if (!server.no_xwayland) {
        server.xwayland = wlr_xwayland_create(server.wl_display, server.compositor, false);
        if (server.xwayland) {
            server.xwayland->data = &server;
            server.xwayland->user_event_handler = handle_xwayland_xwm_event;
            server.new_xwayland_surface.notify = handle_new_xwayland_surface;
            wl_signal_add(&server.xwayland->events.new_surface, &server.new_xwayland_surface);
            server.xwayland_ready.notify = handle_xwayland_ready;
            wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
            setenv("DISPLAY", server.xwayland->display_name, 1);
        }
    } else {
        unsetenv("DISPLAY");
        setenv("DTWM_WAYLAND_NO_XWAYLAND", "1", 1);
        setenv("CK_MOTIF_BACKEND", "wayland", 1);
    }
    {
        const char *rname = server.renderer_name[0] ? server.renderer_name : "pixman";
        setenv("DTWM_WAYLAND_RENDERER", rname, 1);
    }

    int exit_code = 0;
    const char *socket = wl_display_add_socket_auto(server.wl_display);
    if (!socket) {
        exit_code = 1;
        goto cleanup;
    }
    server.wayland_socket = socket;
    setenv("WAYLAND_DISPLAY", socket, 1);

    if (server.no_xwayland && !server.greeter_mode) {
        ensure_wayland_ttsession_env();
    }

    if (!wlr_backend_start(server.backend)) {
        exit_code = 1;
        goto cleanup;
    }

    start_native_ipc_server(&server);
    if (server.renderer_name[0]) {
        dtwm_native_ipc_publish_prop(
            &server, "_DT_WAYLAND_RENDERER", server.renderer_name, strlen(server.renderer_name)
        );
    }
    {
        const char *tts = getenv("TT_SESSION");
        if (!tts || !tts[0]) {
            tts = getenv("_SUN_TT_SESSION");
        }
        if (tts && tts[0]) {
            dtwm_native_ipc_publish_prop(&server, "_SUN_TT_SESSION", tts, strlen(tts));
            dtwm_native_ipc_publish_prop(&server, "TT_SESSION", tts, strlen(tts));
        }
    }
    dtwm_workspace_sync_ipc(&server);
    dtwm_output_mgmt_publish_x11(&server);
    dtwm_sync_resource_manager_dpi(&server);
    dtwm_hw_publish_state(&server);
    if (!server.xwayland) {
        dtwm_spawn_front_panel(&server);
        if (server.show_front_panel && server.panel_pid > 0 && !server.status_written) {
            server.status_fallback_timer = wl_event_loop_add_timer(
                ev_loop, handle_status_fallback_timer, &server
            );
            wl_event_source_timer_update(server.status_fallback_timer, 3500);
        } else {
            write_status_file(&server);
        }
    }
    fprintf(
        stderr,
        "dtwm-wayland: running on WAYLAND_DISPLAY=%s DISPLAY=%s RENDERER=%s DMABUF=%d%s\n",
        socket,
        server.xwayland ? server.xwayland->display_name : "(none)",
        server.renderer_name[0] ? server.renderer_name : "pixman",
        server.linux_dmabuf ? 1 : 0,
        server.no_xwayland ? " (--no-xwayland)" : ""
    );

    if (startup_cmd) {
        if (fork() == 0) {
            sigset_t empty_mask;
            sigemptyset(&empty_mask);
            sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (char *)nullptr);
            _exit(127);
        }
    }

    wl_display_run(server.wl_display);

cleanup:
    server.shutting_down = true;
    if (server.osd_timer) wl_event_source_remove(server.osd_timer);
    if (server.anim_timer) wl_event_source_remove(server.anim_timer);
    if (server.resize_flush_timer) wl_event_source_remove(server.resize_flush_timer);
    if (server.status_fallback_timer) wl_event_source_remove(server.status_fallback_timer);
    if (server.sigint_source) wl_event_source_remove(server.sigint_source);
    if (server.sigterm_source) wl_event_source_remove(server.sigterm_source);
    stop_native_ipc_server(&server);
    if (server.xwayland) {
        wl_list_remove(&server.new_xwayland_surface.link);
        wl_list_remove(&server.xwayland_ready.link);
        wlr_xwayland_destroy(server.xwayland);
        server.xwayland = nullptr;
    }
    wl_display_destroy_clients(server.wl_display);
    dtwm_a11y_finish(&server);
    dtwm_power_display_finish(&server);
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
    wl_list_remove(&server.cursor_swipe_begin.link);
    wl_list_remove(&server.cursor_swipe_update.link);
    wl_list_remove(&server.cursor_swipe_end.link);
    wl_list_remove(&server.cursor_pinch_begin.link);
    wl_list_remove(&server.cursor_pinch_update.link);
    wl_list_remove(&server.cursor_pinch_end.link);
    wl_list_remove(&server.cursor_hold_begin.link);
    wl_list_remove(&server.cursor_hold_end.link);
    wl_list_remove(&server.request_cursor.link);
    if (server.cursor_shape_mgr) {
        wl_list_remove(&server.request_cursor_shape.link);
        server.cursor_shape_mgr = nullptr;
    }
    wl_list_remove(&server.request_set_selection.link);
    wl_list_remove(&server.request_set_primary_selection.link);
    wl_list_remove(&server.request_start_drag.link);
    wl_list_remove(&server.start_drag.link);
    wl_list_remove(&server.new_virtual_pointer.link);
    wl_list_remove(&server.new_virtual_keyboard.link);
    wl_list_remove(&server.new_xdg_toplevel.link);
    wl_list_remove(&server.new_xdg_popup.link);
    wl_list_remove(&server.new_xdg_decoration.link);
    if (server.xdg_activation) {
        DtwmActivationToken *trk, *ttmp;
        wl_list_for_each_safe(trk, ttmp, &server.activation_tokens, link) {
            wl_list_remove(&trk->destroy.link);
            wl_list_remove(&trk->link);
            free(trk);
        }
        wl_list_remove(&server.xdg_activation_new_token.link);
        wl_list_remove(&server.xdg_activation_request_activate.link);
        server.xdg_activation = nullptr;
    }
    if (server.pointer_constraints) {
        DtwmPointerConstraint *pc, *pctmp;
        wl_list_for_each_safe(pc, pctmp, &server.tracked_pointer_constraints, link) {
            wl_list_remove(&pc->set_region.link);
            wl_list_remove(&pc->destroy.link);
            wl_list_remove(&pc->link);
            free(pc);
        }
        wl_list_remove(&server.new_pointer_constraint.link);
        server.pointer_constraints = nullptr;
        server.active_pointer_constraint = nullptr;
    }
    if (server.shortcuts_inhibit_mgr) {
        DtwmShortcutsInhibitor *si, *sitmp;
        wl_list_for_each_safe(si, sitmp, &server.tracked_shortcuts_inhibitors, link) {
            wl_list_remove(&si->destroy.link);
            wl_list_remove(&si->link);
            free(si);
        }
        wl_list_remove(&server.new_shortcuts_inhibitor.link);
        server.shortcuts_inhibit_mgr = nullptr;
        server.active_shortcuts_inhibitor = nullptr;
    }
    wl_list_remove(&server.new_layer_surface.link);
    {
        DtwmPointer *ptr, *ptmp;
        wl_list_for_each_safe(ptr, ptmp, &server.pointers, link) {
            wl_list_remove(&ptr->destroy.link);
            wl_list_remove(&ptr->link);
            free(ptr);
        }
    }
    if (server.backend) {
        struct wlr_backend *backend = server.backend;
        server.backend = nullptr;
        wl_list_remove(&server.new_input.link);
        wl_list_remove(&server.new_output.link);
        wl_list_remove(&server.backend_destroy.link);
        wlr_backend_destroy(backend);
    }
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wl_display_destroy(server.wl_display);
    return exit_code;
}
