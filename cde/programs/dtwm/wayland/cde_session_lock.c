/*
 * CK Core CDE - Wayland Session Lock (ext-session-lock-v1), Idle Notifier
 * (ext-idle-notify-v1), and dtstyle Hardware/Input IPC (WP-06)
 */

#define _GNU_SOURCE

#include "dtwm_wayland.h"

#include <ctype.h>
#include <libinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_session_lock_surface_v1 *lock_surface;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener map;
    struct wl_listener destroy;
} DtwmLockSurface;

typedef struct {
    DtwmServer *server;
    struct wlr_session_lock_v1 *lock;
    bool unlocked_cleanly;
    struct wl_list surfaces;
    struct wl_listener new_surface;
    struct wl_listener unlock;
    struct wl_listener destroy;
} DtwmSessionLock;

typedef struct {
    struct wl_list link;
    struct wlr_input_device *dev;
    struct wl_listener destroy;
} DtwmPointerDev;

static DtwmSessionLock *g_active_lock = nullptr;
static struct wl_list g_pointer_devs;
static bool g_ptr_list_inited = false;

static xcb_atom_t intern_lock_atom(xcb_connection_t *conn, const char *name) {
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, nullptr);
    if (!rep) return XCB_ATOM_NONE;
    xcb_atom_t a = rep->atom;
    free(rep);
    return a;
}

static void set_desktop_trees_enabled(DtwmServer *server, bool enabled) {
    if (!server) return;
    if (server->bg_tree) wlr_scene_node_set_enabled(&server->bg_tree->node, enabled);
    if (server->layer_bg_tree) wlr_scene_node_set_enabled(&server->layer_bg_tree->node, enabled);
    if (server->layer_bottom_tree) wlr_scene_node_set_enabled(&server->layer_bottom_tree->node, enabled);
    if (server->desktop_icon_tree) wlr_scene_node_set_enabled(&server->desktop_icon_tree->node, enabled);
    if (server->workspace_tree) wlr_scene_node_set_enabled(&server->workspace_tree->node, enabled);
    if (server->unmanaged_xwayland_tree) wlr_scene_node_set_enabled(&server->unmanaged_xwayland_tree->node, enabled);
    if (server->layer_top_tree) wlr_scene_node_set_enabled(&server->layer_top_tree->node, enabled);
    if (server->panel_swallow_tree) wlr_scene_node_set_enabled(&server->panel_swallow_tree->node, enabled);
    if (server->popup_tree) wlr_scene_node_set_enabled(&server->popup_tree->node, enabled);
    if (server->overlay_tree) wlr_scene_node_set_enabled(&server->overlay_tree->node, enabled);
    if (server->layer_overlay_tree) wlr_scene_node_set_enabled(&server->layer_overlay_tree->node, enabled);
    if (server->lock_tree) wlr_scene_node_set_enabled(&server->lock_tree->node, !enabled);
}

void dtwm_lock_focus_surface(DtwmServer *server, struct wlr_surface *surf) {
    if (!server || !surf || !server->seat) return;
    struct wlr_xwayland_surface *xs = wlr_xwayland_surface_try_from_wlr_surface(surf);
    if (xs) {
        wlr_xwayland_surface_activate(xs, true);
    }
    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (kbd) {
        wlr_seat_keyboard_notify_enter(
            server->seat, surf, kbd->keycodes, kbd->num_keycodes, &kbd->modifiers
        );
    }
    wlr_seat_pointer_notify_enter(server->seat, surf, 8.0, 8.0);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void focus_best_lock_surface(DtwmServer *server) {
    if (!server || !server->session_locked) return;

    /* Prefer dtsession's genuine Motif PAM lock dialog if mapped */
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped && v->is_lock_dialog && v->kind == DTWM_VIEW_XWAYLAND &&
            v->xwayland_surface && v->xwayland_surface->surface) {
            dtwm_lock_focus_surface(server, v->xwayland_surface->surface);
            return;
        }
    }

    /* Otherwise focus the first mapped ext-session-lock-v1 client surface */
    if (g_active_lock) {
        DtwmLockSurface *ls;
        wl_list_for_each(ls, &g_active_lock->surfaces, link) {
            if (ls->lock_surface && ls->lock_surface->surface && ls->lock_surface->surface->mapped) {
                dtwm_lock_focus_surface(server, ls->lock_surface->surface);
                return;
            }
        }
    }

    wlr_seat_keyboard_notify_clear_focus(server->seat);
    wlr_seat_pointer_notify_clear_focus(server->seat);
}

void dtwm_lock_sync_outputs(DtwmServer *server) {
    if (!server || !server->lock_cover_tree) return;

    DtwmOutput *dout;
    wl_list_for_each(dout, &server->outputs, link) {
        struct wlr_box obox = {0};
        wlr_output_layout_get_box(server->output_layout, dout->wlr_output, &obox);
        int w = obox.width > 0 ? obox.width : 1280;
        int h = obox.height > 0 ? obox.height : 900;
        if (!dout->lock_bg_rect) {
            float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            dout->lock_bg_rect = wlr_scene_rect_create(server->lock_cover_tree, w, h, black);
            wlr_scene_node_lower_to_bottom(&dout->lock_bg_rect->node);
        } else {
            wlr_scene_rect_set_size(dout->lock_bg_rect, w, h);
        }
        wlr_scene_node_set_position(&dout->lock_bg_rect->node, obox.x, obox.y);
        wlr_scene_node_set_enabled(&dout->lock_bg_rect->node, server->session_locked);
    }

    if (g_active_lock) {
        DtwmLockSurface *ls;
        wl_list_for_each(ls, &g_active_lock->surfaces, link) {
            if (ls->lock_surface && ls->lock_surface->output) {
                struct wlr_box obox = {0};
                wlr_output_layout_get_box(server->output_layout, ls->lock_surface->output, &obox);
                if (ls->scene_tree) {
                    wlr_scene_node_set_position(&ls->scene_tree->node, obox.x, obox.y);
                }
                if (obox.width > 0 && obox.height > 0) {
                    wlr_session_lock_surface_v1_configure(
                        ls->lock_surface, (uint32_t)obox.width, (uint32_t)obox.height
                    );
                }
            }
        }
    }
}

static void lock_surface_map(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmLockSurface *ls = wl_container_of(listener, ls, map);
    focus_best_lock_surface(ls->server);
}

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmLockSurface *ls = wl_container_of(listener, ls, destroy);
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->destroy.link);
    wl_list_remove(&ls->link);
    free(ls);
    if (g_active_lock) {
        focus_best_lock_surface(g_active_lock->server);
    }
}

static void handle_lock_new_surface(struct wl_listener *listener, void *data) {
    DtwmSessionLock *sl = wl_container_of(listener, sl, new_surface);
    struct wlr_session_lock_surface_v1 *lock_surf = data;

    DtwmLockSurface *ls = calloc(1, sizeof(*ls));
    if (!ls) return;
    ls->server = sl->server;
    ls->lock_surface = lock_surf;
    ls->scene_tree = wlr_scene_subsurface_tree_create(sl->server->lock_cover_tree, lock_surf->surface);

    struct wlr_box obox = {0};
    if (lock_surf->output) {
        wlr_output_layout_get_box(sl->server->output_layout, lock_surf->output, &obox);
    }
    wlr_scene_node_set_position(&ls->scene_tree->node, obox.x, obox.y);
    uint32_t cw = obox.width > 0 ? (uint32_t)obox.width : 1280u;
    uint32_t ch = obox.height > 0 ? (uint32_t)obox.height : 900u;
    wlr_session_lock_surface_v1_configure(lock_surf, cw, ch);

    ls->map.notify = lock_surface_map;
    wl_signal_add(&lock_surf->surface->events.map, &ls->map);
    ls->destroy.notify = lock_surface_destroy;
    wl_signal_add(&lock_surf->events.destroy, &ls->destroy);
    wl_list_insert(&sl->surfaces, &ls->link);

    focus_best_lock_surface(sl->server);
}

static void handle_lock_unlock(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmSessionLock *sl = wl_container_of(listener, sl, unlock);
    DtwmServer *server = sl->server;
    sl->unlocked_cleanly = true;
    server->session_locked = false;

    set_desktop_trees_enabled(server, true);
    dtwm_lock_sync_outputs(server);
    dtwm_hw_publish_state(server);

    /* Restore focus to a visible desktop view */
    if (server->focused_view && server->focused_view->mapped) {
        dtwm_restore_view_focus(server, server->focused_view);
    }
    dtwm_ime_on_session_lock_changed(server, false);
}

static void handle_lock_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmSessionLock *sl = wl_container_of(listener, sl, destroy);
    DtwmServer *server = sl->server;
    bool was_clean = sl->unlocked_cleanly;

    DtwmLockSurface *ls, *tmp;
    wl_list_for_each_safe(ls, tmp, &sl->surfaces, link) {
        wl_list_remove(&ls->map.link);
        wl_list_remove(&ls->destroy.link);
        wl_list_remove(&ls->link);
        free(ls);
    }
    wl_list_remove(&sl->new_surface.link);
    wl_list_remove(&sl->unlock.link);
    wl_list_remove(&sl->destroy.link);
    if (g_active_lock == sl) {
        g_active_lock = nullptr;
    }
    free(sl);

    if (!was_clean && server) {
        /* Failsafe per ext-session-lock-v1: keep desktop locked & covered if locker crashed! */
        server->session_locked = true;
        set_desktop_trees_enabled(server, false);
        dtwm_lock_sync_outputs(server);
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        wlr_seat_pointer_notify_clear_focus(server->seat);
        dtwm_ime_on_session_lock_changed(server, true);
        dtwm_hw_publish_state(server);
    }
}

static void handle_new_session_lock(struct wl_listener *listener, void *data) {
    DtwmServer *server = wl_container_of(listener, server, new_session_lock);
    struct wlr_session_lock_v1 *lock = data;

    if (g_active_lock != nullptr) {
        /* Another live locker already owns the lock */
        wlr_session_lock_v1_destroy(lock);
        return;
    }

    DtwmSessionLock *sl = calloc(1, sizeof(*sl));
    if (!sl) {
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    sl->server = server;
    sl->lock = lock;
    wl_list_init(&sl->surfaces);

    sl->new_surface.notify = handle_lock_new_surface;
    wl_signal_add(&lock->events.new_surface, &sl->new_surface);
    sl->unlock.notify = handle_lock_unlock;
    wl_signal_add(&lock->events.unlock, &sl->unlock);
    sl->destroy.notify = handle_lock_destroy;
    wl_signal_add(&lock->events.destroy, &sl->destroy);

    g_active_lock = sl;
    server->session_locked = true;
    server->cursor_mode = DTWM_CURSOR_PASSTHROUGH;
    server->grabbed_view = nullptr;
    server->menu_visible = false;
    server->switcher_visible = false;

    set_desktop_trees_enabled(server, false);
    dtwm_lock_sync_outputs(server);
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    wlr_seat_pointer_notify_clear_focus(server->seat);
    dtwm_ime_on_session_lock_changed(server, true);

    wlr_session_lock_v1_send_locked(lock);
    focus_best_lock_surface(server);
    dtwm_hw_publish_state(server);
}

struct wlr_surface *dtwm_lock_surface_at(
    DtwmServer *server, double lx, double ly, double *sx, double *sy
) {
    if (!server || !server->lock_tree) return nullptr;
    struct wlr_scene_node *node = wlr_scene_node_at(&server->lock_tree->node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return nullptr;
    struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *ssurf = wlr_scene_surface_try_from_buffer(sbuf);
    return ssurf ? ssurf->surface : nullptr;
}

bool dtwm_lock_is_sm_lock_window(DtwmServer *server, xcb_window_t win) {
    if (!server || !server->xcb_conn || win == XCB_WINDOW_NONE ||
        server->atom_session_lock_dlg == XCB_ATOM_NONE) {
        return false;
    }

    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn, 0, win, server->atom_session_lock_dlg, XCB_ATOM_CARDINAL, 0, 1
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return false;
    bool marked = (rep->format == 32 && xcb_get_property_value_length(rep) >= 4 &&
                   *(const uint32_t *)xcb_get_property_value(rep) == 1u);
    free(rep);
    if (!marked) return false;

    /* Verify X11 client resource-ID base matches _DT_SM_WINDOW_INFO owner on root if present */
    if (server->atom_dt_sm_win_info != XCB_ATOM_NONE) {
        xcb_get_property_cookie_t sm_ck = xcb_get_property(
            server->xcb_conn, 0, server->xcb_root,
            server->atom_dt_sm_win_info, server->atom_dt_sm_win_info, 0, 2
        );
        xcb_get_property_reply_t *sm_rep = xcb_get_property_reply(server->xcb_conn, sm_ck, nullptr);
        if (sm_rep) {
            if (sm_rep->format == 32 && xcb_get_property_value_length(sm_rep) >= 8) {
                const uint32_t *vals = (const uint32_t *)xcb_get_property_value(sm_rep);
                xcb_window_t sm_win = vals[1];
                const xcb_setup_t *setup = xcb_get_setup(server->xcb_conn);
                uint32_t cid_mask = ~(setup->resource_id_mask);
                if (sm_win != XCB_WINDOW_NONE && (win & cid_mask) != (sm_win & cid_mask)) {
                    free(sm_rep);
                    return false;
                }
            }
            free(sm_rep);
        }
    }
    return true;
}

void dtwm_lock_notify_activity(DtwmServer *server) {
    if (server && server->idle_notifier && server->seat) {
        wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
    }
}

/* --- Hardware / Input / Bell / Saver IPC --- */
static void apply_libinput_ptr_config(DtwmServer *server, struct wlr_input_device *dev) {
    if (!wlr_input_device_is_libinput(dev)) return;
    struct libinput_device *ldev = wlr_libinput_get_device_handle(dev);
    if (!ldev) return;
    if (libinput_device_config_accel_is_available(ldev)) {
        libinput_device_config_accel_set_speed(ldev, server->ptr_accel_speed);
    }
    if (libinput_device_config_left_handed_is_available(ldev)) {
        libinput_device_config_left_handed_set(ldev, server->left_handed ? 1 : 0);
    }
}

static void handle_ptr_dev_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmPointerDev *pd = wl_container_of(listener, pd, destroy);
    wl_list_remove(&pd->destroy.link);
    wl_list_remove(&pd->link);
    free(pd);
}

void dtwm_hw_apply_input_device(DtwmServer *server, struct wlr_input_device *dev) {
    if (!server || !dev || dev->type != WLR_INPUT_DEVICE_POINTER) return;
    if (!g_ptr_list_inited) {
        wl_list_init(&g_pointer_devs);
        g_ptr_list_inited = true;
    }
    DtwmPointerDev *pd = calloc(1, sizeof(*pd));
    if (pd) {
        pd->dev = dev;
        pd->destroy.notify = handle_ptr_dev_destroy;
        wl_signal_add(&dev->events.destroy, &pd->destroy);
        wl_list_insert(&g_pointer_devs, &pd->link);
    }
    apply_libinput_ptr_config(server, dev);
}

void dtwm_hw_apply_keyboard_defaults(DtwmServer *server, struct wlr_keyboard *wlr_kbd) {
    if (!server || !wlr_kbd) return;
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = {
        .layout = server->xkb_layout[0] ? server->xkb_layout : nullptr,
    };
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(
        ctx, server->xkb_layout[0] ? &names : nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS
    );
    if (keymap) {
        wlr_keyboard_set_keymap(wlr_kbd, keymap);
        xkb_keymap_unref(keymap);
    }
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(
        wlr_kbd,
        server->key_repeat_enabled ? server->key_repeat_rate : 0,
        server->key_repeat_delay
    );
}

void dtwm_hw_publish_state(DtwmServer *server) {
    if (!server || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE ||
        server->atom_wayland_input_state == XCB_ATOM_NONE) {
        return;
    }
    char buf[512];
    int n = snprintf(
        buf, sizeof(buf),
        "locked=%d;accel=%.2f;thresh=%d;left_handed=%d;repeat=%d:%d:%d;xkb=%s;bell=%d:%d:%d;saver=%d:%d",
        server->session_locked ? 1 : 0,
        server->ptr_accel_speed,
        server->ptr_accel_thresh,
        server->left_handed ? 1 : 0,
        server->key_repeat_enabled ? 1 : 0,
        server->key_repeat_delay,
        server->key_repeat_rate,
        server->xkb_layout[0] ? server->xkb_layout : "default",
        server->bell_percent,
        server->bell_pitch,
        server->bell_duration,
        server->saver_timeout_sec,
        server->lock_timeout_sec
    );
    if (n > 0) {
        xcb_change_property(
            server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_root,
            server->atom_wayland_input_state, XCB_ATOM_STRING, 8, (uint32_t)n, buf
        );
        xcb_flush(server->xcb_conn);
    }
}

typedef struct {
    unsigned int w;
    unsigned int h;
    bool added;
} HotplugHeadlessCtx;

static void find_and_add_headless_cb(struct wlr_backend *sub, void *data) {
    HotplugHeadlessCtx *ctx = data;
    if (!ctx->added && wlr_backend_is_headless(sub)) {
        if (wlr_headless_add_output(sub, ctx->w, ctx->h) != nullptr) {
            ctx->added = true;
        }
    }
}

bool dtwm_hw_handle_wm_request(DtwmServer *server, const char *cmd) {
    if (!server || !cmd) return false;

    if (strncmp(cmd, "f.hotplug_headless_output", 25) == 0) {
        unsigned int w = 1024, h = 768;
        if (sscanf(cmd + 25, "%u %u", &w, &h) >= 2 && w >= 64 && h >= 64) {
            if (wlr_backend_is_headless(server->backend)) {
                wlr_headless_add_output(server->backend, w, h);
            } else if (wlr_backend_is_multi(server->backend)) {
                HotplugHeadlessCtx ctx = {.w = w, .h = h, .added = false};
                wlr_multi_for_each_backend(server->backend, find_and_add_headless_cb, &ctx);
            }
        }
        return true;
    }

    /* Block workspace/settings manipulation from untrusted X11 apps while locked */
    if (server->session_locked) {
        return true;
    }

    if (strncmp(cmd, "f.set_pointer_accel", 19) == 0) {
        int num = 2, den = 1, thresh = 4;
        if (sscanf(cmd + 19, "%d %d %d", &num, &den, &thresh) >= 2) {
            if (den <= 0) den = 1;
            double ratio = (double)num / (double)den;
            /* Map CDE 0..10 slider around 2.0 default onto libinput [-1.0, +1.0] */
            double speed = (ratio - 2.0) / 8.0;
            if (speed < -1.0) speed = -1.0;
            if (speed > 1.0) speed = 1.0;
            server->ptr_accel_speed = speed;
            server->ptr_accel_thresh = thresh;
            if (g_ptr_list_inited) {
                DtwmPointerDev *pd;
                wl_list_for_each(pd, &g_pointer_devs, link) {
                    apply_libinput_ptr_config(server, pd->dev);
                }
            }
            dtwm_hw_publish_state(server);
        }
        return true;
    }

    if (strncmp(cmd, "f.set_left_handed", 17) == 0) {
        int lh = 0;
        if (sscanf(cmd + 17, "%d", &lh) == 1) {
            server->left_handed = (lh != 0);
            if (g_ptr_list_inited) {
                DtwmPointerDev *pd;
                wl_list_for_each(pd, &g_pointer_devs, link) {
                    apply_libinput_ptr_config(server, pd->dev);
                }
            }
            dtwm_hw_publish_state(server);
        }
        return true;
    }

    if (strncmp(cmd, "f.set_key_repeat", 16) == 0) {
        int en = 1, delay = 600, rate = 25;
        if (sscanf(cmd + 16, "%d %d %d", &en, &delay, &rate) >= 1) {
            if (delay < 50) delay = 50;
            if (rate < 1) rate = 1;
            if (rate > 100) rate = 100;
            server->key_repeat_enabled = (en != 0);
            server->key_repeat_delay = delay;
            server->key_repeat_rate = rate;
            DtwmKeyboard *kb;
            wl_list_for_each(kb, &server->keyboards, link) {
                wlr_keyboard_set_repeat_info(
                    kb->wlr_keyboard,
                    server->key_repeat_enabled ? server->key_repeat_rate : 0,
                    server->key_repeat_delay
                );
            }
            dtwm_hw_publish_state(server);
        }
        return true;
    }

    if (strncmp(cmd, "f.set_xkb_layout", 16) == 0) {
        const char *p = cmd + 16;
        while (*p && isspace((unsigned char)*p)) p++;
        snprintf(server->xkb_layout, sizeof(server->xkb_layout), "%s", p);
        size_t l = strlen(server->xkb_layout);
        while (l > 0 && isspace((unsigned char)server->xkb_layout[l - 1])) {
            server->xkb_layout[--l] = '\0';
        }
        DtwmKeyboard *kb;
        wl_list_for_each(kb, &server->keyboards, link) {
            dtwm_hw_apply_keyboard_defaults(server, kb->wlr_keyboard);
        }
        dtwm_hw_publish_state(server);
        return true;
    }

    if (strncmp(cmd, "f.set_bell", 10) == 0) {
        int pct = 50, pitch = 400, dur = 100;
        if (sscanf(cmd + 10, "%d %d %d", &pct, &pitch, &dur) >= 1) {
            server->bell_percent = pct;
            server->bell_pitch = pitch;
            server->bell_duration = dur;
            dtwm_hw_publish_state(server);
        }
        return true;
    }

    if (strncmp(cmd, "f.set_saver_timeout", 19) == 0) {
        int saver_s = 600, lock_s = 1800;
        if (sscanf(cmd + 19, "%d %d", &saver_s, &lock_s) >= 1) {
            server->saver_timeout_sec = saver_s;
            server->lock_timeout_sec = lock_s;
            dtwm_hw_publish_state(server);
        }
        return true;
    }

    return false;
}

void dtwm_lock_on_xwayland_ready(DtwmServer *server) {
    if (!server || !server->xcb_conn) return;
    server->atom_session_lock_dlg = intern_lock_atom(server->xcb_conn, "_DT_SESSION_LOCK_DIALOG");
    server->atom_dt_sm_win_info = intern_lock_atom(server->xcb_conn, "_DT_SM_WINDOW_INFO");
    server->atom_wayland_input_state = intern_lock_atom(server->xcb_conn, "_DT_WAYLAND_INPUT_STATE");
    dtwm_hw_publish_state(server);
}

void dtwm_lock_init(DtwmServer *server) {
    if (!server) return;
    if (!g_ptr_list_inited) {
        wl_list_init(&g_pointer_devs);
        g_ptr_list_inited = true;
    }
    server->key_repeat_enabled = true;
    server->key_repeat_delay = 600;
    server->key_repeat_rate = 25;
    server->bell_percent = 50;
    server->bell_pitch = 400;
    server->bell_duration = 100;
    server->saver_timeout_sec = 600;
    server->lock_timeout_sec = 1800;

    server->lock_tree = wlr_scene_tree_create(&server->scene->tree);
    server->lock_cover_tree = wlr_scene_tree_create(server->lock_tree);
    server->lock_dialog_tree = wlr_scene_tree_create(server->lock_tree);
    wlr_scene_node_set_enabled(&server->lock_tree->node, false);

    server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
    server->session_lock_mgr = wlr_session_lock_manager_v1_create(server->wl_display);
    if (server->session_lock_mgr) {
        server->new_session_lock.notify = handle_new_session_lock;
        wl_signal_add(&server->session_lock_mgr->events.new_lock, &server->new_session_lock);
    }
}

void dtwm_lock_finish(DtwmServer *server) {
    if (server && server->session_lock_mgr) {
        wl_list_remove(&server->new_session_lock.link);
    }
    g_active_lock = nullptr;
}
