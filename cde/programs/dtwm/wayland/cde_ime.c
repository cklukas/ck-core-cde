/*
 * CDE - Common Desktop Environment
 *
 * wayland/cde_ime.c: Native Wayland CJK Input Method relay bridging
 * zwp_text_input_v3 clients, zwp_input_method_v2 daemons (fcitx5 / ibus),
 * zwp_input_method_keyboard_grab_v2 key routing (isolated during session lock),
 * and zwp_input_popup_surface_v2 CJK candidate popup positioning.
 */

#define _GNU_SOURCE
#include "dtwm_wayland.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_text_input_v3 *wlr_ti;
    struct wl_listener enable;
    struct wl_listener commit;
    struct wl_listener disable;
    struct wl_listener destroy;
} DtwmTextInput;

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_input_popup_surface_v2 *wlr_popup;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener surface_commit;
    struct wl_listener surface_map;
    struct wl_listener surface_unmap;
    struct wl_listener destroy;
} DtwmImePopup;

struct DtwmImeState {
    DtwmServer *server;
    struct wlr_text_input_manager_v3 *ti_mgr;
    struct wlr_input_method_manager_v2 *im_mgr;
    struct wl_listener new_text_input;
    struct wl_listener new_input_method;

    struct wl_list text_inputs; /* DtwmTextInput.link */
    DtwmTextInput *active_ti;

    struct wlr_input_method_v2 *active_im;
    struct wl_listener im_commit;
    struct wl_listener im_new_popup;
    struct wl_listener im_grab_keyboard;
    struct wl_listener im_destroy;

    struct wlr_input_method_keyboard_grab_v2 *kb_grab;
    struct wl_listener kb_grab_destroy;

    struct wl_list popups; /* DtwmImePopup.link */
    struct wlr_surface *focused_surface;
};

static void update_ime_popup_positions(DtwmImeState *ime) {
    if (!ime || wl_list_empty(&ime->popups)) return;
    DtwmServer *server = ime->server;

    bool visible = (!server->session_locked && ime->active_ti &&
                    ime->active_ti->wlr_ti && ime->active_ti->wlr_ti->current_enabled &&
                    ime->active_ti->wlr_ti->focused_surface != nullptr);

    int base_x = 80, base_y = 80;
    struct wlr_box crect = {0, 0, 16, 20};
    if (visible) {
        if (ime->active_ti->wlr_ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE) {
            crect = ime->active_ti->wlr_ti->current.cursor_rectangle;
        }
        if (server->focused_view && server->focused_view->mapped) {
            int l = 0, t = 0, r = 0, b = 0;
            dtwm_view_insets(server->focused_view, &l, &t, &r, &b);
            base_x = server->focused_view->x + l;
            base_y = server->focused_view->y + t;
        }
    }

    struct wlr_box obox = {0, 0, 1280, 820};
    dtwm_get_usable_output_box(server, base_x + crect.x, base_y + crect.y, &obox);

    DtwmImePopup *pop;
    wl_list_for_each(pop, &ime->popups, link) {
        if (!pop->scene_tree) continue;
        if (!visible || !pop->wlr_popup->surface->mapped) {
            wlr_scene_node_set_enabled(&pop->scene_tree->node, false);
            continue;
        }

        int pw = pop->wlr_popup->surface->current.width;
        int ph = pop->wlr_popup->surface->current.height;
        if (pw <= 0) pw = 180;
        if (ph <= 0) ph = 40;

        int px = base_x + crect.x;
        int py = base_y + crect.y + (crect.height > 0 ? crect.height : 20) + 2;

        /* Clamp inside usable monitor workarea so candidate popups never clip */
        if (px + pw > obox.x + obox.width) {
            px = obox.x + obox.width - pw;
        }
        if (px < obox.x) px = obox.x;
        if (py + ph > obox.y + obox.height) {
            py = base_y + crect.y - ph - 2;
            if (py < obox.y) py = obox.y;
        }

        wlr_scene_node_set_position(&pop->scene_tree->node, px, py);
        wlr_scene_node_set_enabled(&pop->scene_tree->node, true);
        wlr_scene_node_raise_to_top(&pop->scene_tree->node);

        struct wlr_box rel_rect = {
            .x = (base_x + crect.x) - px,
            .y = (base_y + crect.y) - py,
            .width = crect.width > 0 ? crect.width : 2,
            .height = crect.height > 0 ? crect.height : 18,
        };
        wlr_input_popup_surface_v2_send_text_input_rectangle(pop->wlr_popup, &rel_rect);
    }
}

static void sync_ti_state_to_im(DtwmImeState *ime, struct wlr_text_input_v3 *ti, bool send_activate) {
    if (!ime || !ime->active_im || !ti || ime->server->session_locked) return;
    struct wlr_input_method_v2 *im = ime->active_im;

    if (send_activate) {
        wlr_input_method_v2_send_activate(im);
    }
    if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
        wlr_input_method_v2_send_surrounding_text(
            im,
            ti->current.surrounding.text ? ti->current.surrounding.text : "",
            ti->current.surrounding.cursor,
            ti->current.surrounding.anchor
        );
    }
    wlr_input_method_v2_send_text_change_cause(im, ti->current.text_change_cause);
    if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
        wlr_input_method_v2_send_content_type(
            im, ti->current.content_type.hint, ti->current.content_type.purpose
        );
    }
    wlr_input_method_v2_send_done(im);
    update_ime_popup_positions(ime);
}

static void deactivate_im_if_needed(DtwmImeState *ime) {
    if (!ime) return;
    if (ime->active_im) {
        wlr_input_method_v2_send_deactivate(ime->active_im);
        wlr_input_method_v2_send_done(ime->active_im);
    }
    update_ime_popup_positions(ime);
}

/* -------------------------------------------------------------------------
 * wlr_text_input_v3 Listeners
 * ------------------------------------------------------------------------- */
static void ti_handle_enable(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmTextInput *ti = wl_container_of(listener, ti, enable);
    DtwmImeState *ime = ti->server->ime;
    if (!ime || ti->server->session_locked) return;
    if (ti->wlr_ti->focused_surface == nullptr) return;

    ime->active_ti = ti;
    sync_ti_state_to_im(ime, ti->wlr_ti, true);
}

static void ti_handle_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmTextInput *ti = wl_container_of(listener, ti, commit);
    DtwmImeState *ime = ti->server->ime;
    if (!ime || ti->server->session_locked) return;
    if (ime->active_ti == ti && ti->wlr_ti->current_enabled) {
        sync_ti_state_to_im(ime, ti->wlr_ti, false);
    }
}

static void ti_handle_disable(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmTextInput *ti = wl_container_of(listener, ti, disable);
    DtwmImeState *ime = ti->server->ime;
    if (!ime) return;
    if (ime->active_ti == ti) {
        ime->active_ti = nullptr;
        deactivate_im_if_needed(ime);
    }
}

static void ti_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmTextInput *ti = wl_container_of(listener, ti, destroy);
    DtwmImeState *ime = ti->server->ime;
    if (ime && ime->active_ti == ti) {
        ime->active_ti = nullptr;
        deactivate_im_if_needed(ime);
    }
    wl_list_remove(&ti->enable.link);
    wl_list_remove(&ti->commit.link);
    wl_list_remove(&ti->disable.link);
    wl_list_remove(&ti->destroy.link);
    wl_list_remove(&ti->link);
    free(ti);
}

static void handle_new_text_input(struct wl_listener *listener, void *data) {
    DtwmImeState *ime = wl_container_of(listener, ime, new_text_input);
    struct wlr_text_input_v3 *wlr_ti = data;

    DtwmTextInput *ti = calloc(1, sizeof(*ti));
    ti->server = ime->server;
    ti->wlr_ti = wlr_ti;

    ti->enable.notify = ti_handle_enable;
    wl_signal_add(&wlr_ti->events.enable, &ti->enable);
    ti->commit.notify = ti_handle_commit;
    wl_signal_add(&wlr_ti->events.commit, &ti->commit);
    ti->disable.notify = ti_handle_disable;
    wl_signal_add(&wlr_ti->events.disable, &ti->disable);
    ti->destroy.notify = ti_handle_destroy;
    wl_signal_add(&wlr_ti->events.destroy, &ti->destroy);

    wl_list_insert(&ime->text_inputs, &ti->link);

    /* If this client already holds keyboard focus, send enter immediately */
    if (!ime->server->session_locked && ime->focused_surface &&
        wl_resource_get_client(wlr_ti->resource) == wl_resource_get_client(ime->focused_surface->resource)) {
        wlr_text_input_v3_send_enter(wlr_ti, ime->focused_surface);
    }
}

/* -------------------------------------------------------------------------
 * wlr_input_method_v2, Keyboard Grab & Candidate Popup Listeners
 * ------------------------------------------------------------------------- */
static void im_handle_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmImeState *ime = wl_container_of(listener, ime, im_commit);
    struct wlr_input_method_v2 *im = ime->active_im;
    if (!im || !ime->active_ti || ime->server->session_locked) return;

    struct wlr_text_input_v3 *ti = ime->active_ti->wlr_ti;
    if (!ti || !ti->current_enabled || !ti->focused_surface) return;

    if (im->current.preedit.text) {
        wlr_text_input_v3_send_preedit_string(
            ti,
            im->current.preedit.text,
            im->current.preedit.cursor_begin,
            im->current.preedit.cursor_end
        );
    } else {
        wlr_text_input_v3_send_preedit_string(ti, nullptr, 0, 0);
    }

    if (im->current.commit_text) {
        wlr_text_input_v3_send_commit_string(ti, im->current.commit_text);
    }

    if (im->current.delete.before_length || im->current.delete.after_length) {
        wlr_text_input_v3_send_delete_surrounding_text(
            ti, im->current.delete.before_length, im->current.delete.after_length
        );
    }

    wlr_text_input_v3_send_done(ti);
    update_ime_popup_positions(ime);
}

static void popup_surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmImePopup *pop = wl_container_of(listener, pop, surface_commit);
    if (pop->server && pop->server->ime) {
        update_ime_popup_positions(pop->server->ime);
    }
}

static void popup_surface_map(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmImePopup *pop = wl_container_of(listener, pop, surface_map);
    if (pop->server && pop->server->ime) {
        update_ime_popup_positions(pop->server->ime);
    }
}

static void popup_surface_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmImePopup *pop = wl_container_of(listener, pop, surface_unmap);
    if (pop->scene_tree) {
        wlr_scene_node_set_enabled(&pop->scene_tree->node, false);
    }
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmImePopup *pop = wl_container_of(listener, pop, destroy);
    wl_list_remove(&pop->surface_commit.link);
    wl_list_remove(&pop->surface_map.link);
    wl_list_remove(&pop->surface_unmap.link);
    wl_list_remove(&pop->destroy.link);
    wl_list_remove(&pop->link);
    if (pop->scene_tree) {
        wlr_scene_node_destroy(&pop->scene_tree->node);
        pop->scene_tree = nullptr;
    }
    free(pop);
}

static void im_handle_new_popup(struct wl_listener *listener, void *data) {
    DtwmImeState *ime = wl_container_of(listener, ime, im_new_popup);
    struct wlr_input_popup_surface_v2 *wlr_popup = data;

    DtwmImePopup *pop = calloc(1, sizeof(*pop));
    pop->server = ime->server;
    pop->wlr_popup = wlr_popup;
    pop->scene_tree = wlr_scene_subsurface_tree_create(ime->server->popup_tree, wlr_popup->surface);
    wlr_scene_node_set_enabled(&pop->scene_tree->node, false);

    pop->surface_commit.notify = popup_surface_commit;
    wl_signal_add(&wlr_popup->surface->events.commit, &pop->surface_commit);
    pop->surface_map.notify = popup_surface_map;
    wl_signal_add(&wlr_popup->surface->events.map, &pop->surface_map);
    pop->surface_unmap.notify = popup_surface_unmap;
    wl_signal_add(&wlr_popup->surface->events.unmap, &pop->surface_unmap);
    pop->destroy.notify = popup_handle_destroy;
    wl_signal_add(&wlr_popup->events.destroy, &pop->destroy);

    wl_list_insert(&ime->popups, &pop->link);
    update_ime_popup_positions(ime);
}

static void kb_grab_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmImeState *ime = wl_container_of(listener, ime, kb_grab_destroy);
    wl_list_remove(&ime->kb_grab_destroy.link);
    ime->kb_grab = nullptr;
}

static void im_handle_grab_keyboard(struct wl_listener *listener, void *data) {
    DtwmImeState *ime = wl_container_of(listener, ime, im_grab_keyboard);
    struct wlr_input_method_keyboard_grab_v2 *grab = data;

    if (ime->kb_grab) {
        wl_list_remove(&ime->kb_grab_destroy.link);
        ime->kb_grab = nullptr;
    }
    ime->kb_grab = grab;
    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(ime->server->seat);
    if (kbd) {
        wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kbd);
    }
    ime->kb_grab_destroy.notify = kb_grab_handle_destroy;
    wl_signal_add(&grab->events.destroy, &ime->kb_grab_destroy);
}

static void im_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmImeState *ime = wl_container_of(listener, ime, im_destroy);
    if (ime->kb_grab) {
        wl_list_remove(&ime->kb_grab_destroy.link);
        ime->kb_grab = nullptr;
    }
    wl_list_remove(&ime->im_commit.link);
    wl_list_remove(&ime->im_new_popup.link);
    wl_list_remove(&ime->im_grab_keyboard.link);
    wl_list_remove(&ime->im_destroy.link);
    ime->active_im = nullptr;
    update_ime_popup_positions(ime);
}

static void handle_new_input_method(struct wl_listener *listener, void *data) {
    DtwmImeState *ime = wl_container_of(listener, ime, new_input_method);
    struct wlr_input_method_v2 *wlr_im = data;

    if (ime->active_im != nullptr || wlr_im->seat != ime->server->seat) {
        wlr_input_method_v2_send_unavailable(wlr_im);
        return;
    }

    ime->active_im = wlr_im;
    ime->im_commit.notify = im_handle_commit;
    wl_signal_add(&wlr_im->events.commit, &ime->im_commit);
    ime->im_new_popup.notify = im_handle_new_popup;
    wl_signal_add(&wlr_im->events.new_popup_surface, &ime->im_new_popup);
    ime->im_grab_keyboard.notify = im_handle_grab_keyboard;
    wl_signal_add(&wlr_im->events.grab_keyboard, &ime->im_grab_keyboard);
    ime->im_destroy.notify = im_handle_destroy;
    wl_signal_add(&wlr_im->events.destroy, &ime->im_destroy);

    if (!ime->server->session_locked && ime->active_ti &&
        ime->active_ti->wlr_ti->current_enabled &&
        ime->active_ti->wlr_ti->focused_surface != nullptr) {
        sync_ti_state_to_im(ime, ime->active_ti->wlr_ti, true);
    }
}

/* -------------------------------------------------------------------------
 * Public Compositor Entry Points
 * ------------------------------------------------------------------------- */
void dtwm_ime_init(DtwmServer *server) {
    if (!server || !server->wl_display) return;
    DtwmImeState *ime = calloc(1, sizeof(*ime));
    ime->server = server;
    wl_list_init(&ime->text_inputs);
    wl_list_init(&ime->popups);
    server->ime = ime;

    ime->ti_mgr = wlr_text_input_manager_v3_create(server->wl_display);
    if (ime->ti_mgr) {
        ime->new_text_input.notify = handle_new_text_input;
        wl_signal_add(&ime->ti_mgr->events.text_input, &ime->new_text_input);
    }

    ime->im_mgr = wlr_input_method_manager_v2_create(server->wl_display);
    if (ime->im_mgr) {
        ime->new_input_method.notify = handle_new_input_method;
        wl_signal_add(&ime->im_mgr->events.input_method, &ime->new_input_method);
    }
}

void dtwm_ime_finish(DtwmServer *server) {
    if (!server || !server->ime) return;
    DtwmImeState *ime = server->ime;
    server->ime = nullptr;

    if (ime->kb_grab) {
        wl_list_remove(&ime->kb_grab_destroy.link);
        ime->kb_grab = nullptr;
    }
    if (ime->active_im) {
        wl_list_remove(&ime->im_commit.link);
        wl_list_remove(&ime->im_new_popup.link);
        wl_list_remove(&ime->im_grab_keyboard.link);
        wl_list_remove(&ime->im_destroy.link);
        ime->active_im = nullptr;
    }
    DtwmImePopup *pop, *ptmp;
    wl_list_for_each_safe(pop, ptmp, &ime->popups, link) {
        wl_list_remove(&pop->surface_commit.link);
        wl_list_remove(&pop->surface_map.link);
        wl_list_remove(&pop->surface_unmap.link);
        wl_list_remove(&pop->destroy.link);
        wl_list_remove(&pop->link);
        free(pop);
    }
    DtwmTextInput *ti, *ttmp;
    wl_list_for_each_safe(ti, ttmp, &ime->text_inputs, link) {
        wl_list_remove(&ti->enable.link);
        wl_list_remove(&ti->commit.link);
        wl_list_remove(&ti->disable.link);
        wl_list_remove(&ti->destroy.link);
        wl_list_remove(&ti->link);
        free(ti);
    }
    if (ime->ti_mgr) {
        wl_list_remove(&ime->new_text_input.link);
    }
    if (ime->im_mgr) {
        wl_list_remove(&ime->new_input_method.link);
    }
    free(ime);
}

void dtwm_ime_set_focus(DtwmServer *server, struct wlr_surface *surface) {
    if (!server || !server->ime) return;
    DtwmImeState *ime = server->ime;

    if (server->session_locked || server->greeter_mode) {
        surface = nullptr;
    }
    ime->focused_surface = surface;

    DtwmTextInput *ti;
    wl_list_for_each(ti, &ime->text_inputs, link) {
        if (ti->wlr_ti->focused_surface && ti->wlr_ti->focused_surface != surface) {
            if (ime->active_ti == ti) {
                ime->active_ti = nullptr;
                deactivate_im_if_needed(ime);
            }
            wlr_text_input_v3_send_leave(ti->wlr_ti);
        }
    }

    if (surface) {
        struct wl_client *target_client = wl_resource_get_client(surface->resource);
        wl_list_for_each(ti, &ime->text_inputs, link) {
            if (wl_resource_get_client(ti->wlr_ti->resource) == target_client &&
                ti->wlr_ti->focused_surface != surface) {
                wlr_text_input_v3_send_enter(ti->wlr_ti, surface);
            }
        }
    }
    update_ime_popup_positions(ime);
}

void dtwm_ime_on_session_lock_changed(DtwmServer *server, bool locked) {
    if (!server || !server->ime) return;
    if (locked) {
        dtwm_ime_set_focus(server, nullptr);
    } else if (server->focused_view && server->focused_view->mapped) {
        struct wlr_surface *surf = nullptr;
        if (server->focused_view->kind == DTWM_VIEW_XDG && server->focused_view->xdg_toplevel) {
            surf = server->focused_view->xdg_toplevel->base->surface;
        } else if (server->focused_view->kind == DTWM_VIEW_XWAYLAND && server->focused_view->xwayland_surface) {
            surf = server->focused_view->xwayland_surface->surface;
        }
        dtwm_ime_set_focus(server, surf);
    }
}

static bool is_im_own_virtual_keyboard(DtwmImeState *ime, struct wlr_keyboard *wlr_kbd) {
    struct wlr_virtual_keyboard_v1 *vkbd = wlr_input_device_get_virtual_keyboard(&wlr_kbd->base);
    if (!vkbd || !vkbd->resource || !ime->active_im || !ime->active_im->resource) {
        return false;
    }
    return wl_resource_get_client(vkbd->resource) ==
           wl_resource_get_client(ime->active_im->resource);
}

void dtwm_ime_set_keyboard(DtwmServer *server, struct wlr_keyboard *wlr_kbd) {
    if (!server || !server->ime || !wlr_kbd || !wlr_kbd->keymap) return;
    DtwmImeState *ime = server->ime;
    if (!ime->kb_grab || !ime->active_im) return;
    if (is_im_own_virtual_keyboard(ime, wlr_kbd)) return;
    if (ime->kb_grab->keyboard != wlr_kbd) {
        wlr_input_method_keyboard_grab_v2_set_keyboard(ime->kb_grab, wlr_kbd);
    }
}

bool dtwm_ime_forward_key(
    DtwmServer *server,
    struct wlr_keyboard *wlr_kbd,
    const struct wlr_keyboard_key_event *ev
) {
    if (!server || !server->ime || !wlr_kbd || !ev) return false;
    if (server->session_locked || server->greeter_mode) return false;
    DtwmImeState *ime = server->ime;
    if (!ime->kb_grab || !ime->active_im) return false;
    if (is_im_own_virtual_keyboard(ime, wlr_kbd)) return false;

    if (ime->kb_grab->keyboard != wlr_kbd) {
        wlr_input_method_keyboard_grab_v2_set_keyboard(ime->kb_grab, wlr_kbd);
    }
    wlr_input_method_keyboard_grab_v2_send_key(
        ime->kb_grab, ev->time_msec, ev->keycode, ev->state
    );
    return true;
}

bool dtwm_ime_forward_modifiers(DtwmServer *server, struct wlr_keyboard *wlr_kbd) {
    if (!server || !server->ime || !wlr_kbd) return false;
    if (server->session_locked || server->greeter_mode) return false;
    DtwmImeState *ime = server->ime;
    if (!ime->kb_grab || !ime->active_im) return false;
    if (is_im_own_virtual_keyboard(ime, wlr_kbd)) return false;

    if (ime->kb_grab->keyboard != wlr_kbd) {
        wlr_input_method_keyboard_grab_v2_set_keyboard(ime->kb_grab, wlr_kbd);
    }
    wlr_input_method_keyboard_grab_v2_send_modifiers(ime->kb_grab, &wlr_kbd->modifiers);
    return true;
}
