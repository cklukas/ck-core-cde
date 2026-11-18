/*
 * CK Core CDE - X11/Motif XDND v5 -> Native Wayland wl_data_device Bridge
 *
 * wlroots-0.19 natively bridges Wayland -> X11 XDND v5 drags, but does not
 * initiate Wayland wlr_drag sessions when an X11/Motif client starts an XDND v5
 * drag (owning XdndSelection). This module watches XdndSelection via XFixes on
 * dtwm-wayland's XCB connection, preserves native X11-to-X11 Motif drags while
 * hovering XWayland windows, and seamlessly promotes the drag to a Wayland
 * wlr_drag whenever the pointer enters a native Wayland surface.
 */

#define _GNU_SOURCE

#include "dtwm_wayland.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/xwayland/server.h>
#include <xcb/xfixes.h>

#define DTWM_XDND_MAX_MIMES 16
#define DTWM_XDND_MAX_BYTES (256 * 1024)

typedef struct {
    struct wlr_data_source base;
    DtwmServer *server;
    uint8_t *payload;
    size_t payload_len;
    bool had_wayland_drop;
    uint32_t release_button;
} DtwmX11DragSource;

static struct {
    uint8_t xfixes_first_event;
    bool xfixes_ready;

    xcb_atom_t atom_xdnd_sel;
    xcb_atom_t atom_xdnd_typelist;
    xcb_atom_t atom_targets;
    xcb_atom_t atom_utf8;
    xcb_atom_t atom_uri_list;
    xcb_atom_t atom_text_plain_utf8;
    xcb_atom_t atom_prop_targets;
    xcb_atom_t atom_prop_data;

    xcb_window_t owner;
    bool button_session_active;
    bool targets_ready;
    bool payload_ready;

    char *mimes[DTWM_XDND_MAX_MIMES];
    size_t mime_count;
    xcb_atom_t preferred_target;

    uint8_t *cached_bytes;
    size_t cached_len;
} g_xdnd;

static xcb_atom_t xdnd_intern_atom(xcb_connection_t *conn, const char *name) {
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, nullptr);
    if (!rep) return XCB_ATOM_NONE;
    xcb_atom_t a = rep->atom;
    free(rep);
    return a;
}

static void xdnd_clear_mimes_and_cache(void) {
    for (size_t i = 0; i < g_xdnd.mime_count; i++) {
        free(g_xdnd.mimes[i]);
        g_xdnd.mimes[i] = nullptr;
    }
    g_xdnd.mime_count = 0;
    g_xdnd.preferred_target = XCB_ATOM_NONE;
    g_xdnd.targets_ready = false;
    g_xdnd.payload_ready = false;
    free(g_xdnd.cached_bytes);
    g_xdnd.cached_bytes = nullptr;
    g_xdnd.cached_len = 0;
}

static void xdnd_add_mime(const char *mime) {
    if (!mime || !*mime || g_xdnd.mime_count >= DTWM_XDND_MAX_MIMES) return;
    for (size_t i = 0; i < g_xdnd.mime_count; i++) {
        if (strcmp(g_xdnd.mimes[i], mime) == 0) return;
    }
    g_xdnd.mimes[g_xdnd.mime_count++] = strdup(mime);
}

static bool is_ignored_target_name(const char *name) {
    if (!name || !*name) return true;
    if (strncmp(name, "_MOTIF_", 7) == 0) return true;
    return strcmp(name, "TARGETS") == 0 ||
           strcmp(name, "MULTIPLE") == 0 ||
           strcmp(name, "TIMESTAMP") == 0 ||
           strcmp(name, "DELETE") == 0 ||
           strcmp(name, "INSERT_SELECTION") == 0 ||
           strcmp(name, "INSERT_PROPERTY") == 0 ||
           strcmp(name, "TRANSFER_SUCCESS") == 0 ||
           strcmp(name, "TRANSFER_FAILURE") == 0;
}

static void x11_drag_source_send(struct wlr_data_source *wlr_src, const char *mime_type, int32_t fd) {
    (void)mime_type;
    DtwmX11DragSource *src = wl_container_of(wlr_src, src, base);
    if (src->payload && src->payload_len > 0) {
        ssize_t off = 0;
        while ((size_t)off < src->payload_len) {
            ssize_t n = write(fd, src->payload + off, src->payload_len - (size_t)off);
            if (n <= 0) break;
            off += n;
        }
    }
    close(fd);
}

static void x11_drag_source_accept(struct wlr_data_source *wlr_src, uint32_t serial, const char *mime_type) {
    (void)serial;
    wlr_src->accepted = (mime_type != nullptr);
}

static void x11_drag_source_dnd_drop(struct wlr_data_source *wlr_src) {
    DtwmX11DragSource *src = wl_container_of(wlr_src, src, base);
    src->had_wayland_drop = true;
}

static void x11_drag_source_dnd_finish(struct wlr_data_source *wlr_src) {
    DtwmX11DragSource *src = wl_container_of(wlr_src, src, base);
    src->had_wayland_drop = true;
}

typedef struct {
    DtwmServer *server;
    uint32_t button;
} ReleaseXwaylandCtx;

static void release_xwayland_grab_idle(void *data) {
    ReleaseXwaylandCtx *ctx = data;
    DtwmServer *server = ctx->server;
    uint32_t btn = ctx->button;
    free(ctx);

    if (!server || !server->seat || server->seat->drag) return;

    struct wlr_surface *xw_surf = nullptr;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->kind == DTWM_VIEW_XWAYLAND && v->mapped &&
            v->xwayland_surface && v->xwayland_surface->surface) {
            xw_surf = v->xwayland_surface->surface;
            break;
        }
    }
    if (xw_surf && btn != 0) {
        struct wlr_surface *prev = server->seat->pointer_state.focused_surface;
        double prev_sx = server->seat->pointer_state.sx;
        double prev_sy = server->seat->pointer_state.sy;

        wlr_seat_pointer_notify_enter(server->seat, xw_surf, 1.0, 1.0);
        wlr_seat_pointer_notify_button(server->seat, 0, btn, WL_POINTER_BUTTON_STATE_RELEASED);
        wlr_seat_pointer_notify_frame(server->seat);

        if (prev && prev != xw_surf) {
            wlr_seat_pointer_notify_enter(server->seat, prev, prev_sx, prev_sy);
            wlr_seat_pointer_notify_frame(server->seat);
        }
    }
}

static void x11_drag_source_destroy(struct wlr_data_source *wlr_src) {
    DtwmX11DragSource *src = wl_container_of(wlr_src, src, base);
    DtwmServer *server = src->server;
    uint32_t rel_btn = src->release_button;
    free(src->payload);
    free(src);

    if (server && server->wl_display && rel_btn != 0) {
        ReleaseXwaylandCtx *ctx = calloc(1, sizeof(*ctx));
        if (ctx) {
            ctx->server = server;
            ctx->button = rel_btn;
            wl_event_loop_add_idle(wl_display_get_event_loop(server->wl_display), release_xwayland_grab_idle, ctx);
        }
    }
}

static const struct wlr_data_source_impl x11_drag_source_impl = {
    .send = x11_drag_source_send,
    .accept = x11_drag_source_accept,
    .destroy = x11_drag_source_destroy,
    .dnd_drop = x11_drag_source_dnd_drop,
    .dnd_finish = x11_drag_source_dnd_finish,
};

void dtwm_xdnd_on_cursor_motion(
    DtwmServer *server, struct wlr_surface *surface, double sx, double sy, uint32_t time_msec
) {
    if (!server || !surface) return;
    if (!g_xdnd.button_session_active || server->pointer_buttons_down <= 0) return;
    if (server->seat->drag != nullptr) return;
    if (!g_xdnd.targets_ready || g_xdnd.mime_count == 0 || !g_xdnd.payload_ready) return;

    /* Preserve native X11 drags while hovering XWayland surfaces */
    if (wlr_xwayland_surface_try_from_wlr_surface(surface) != nullptr) {
        return;
    }

    if (!server->xwayland || !server->xwayland->server || !server->xwayland->server->client) {
        return;
    }
    struct wlr_seat_client *sc =
        wlr_seat_client_for_wl_client(server->seat, server->xwayland->server->client);
    if (!sc) {
        return;
    }

    DtwmX11DragSource *xsrc = calloc(1, sizeof(*xsrc));
    if (!xsrc) return;
    wlr_data_source_init(&xsrc->base, &x11_drag_source_impl);
    xsrc->server = server;
    xsrc->release_button = server->last_pointer_button;
    xsrc->base.actions =
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

    for (size_t i = 0; i < g_xdnd.mime_count; i++) {
        char **slot = wl_array_add(&xsrc->base.mime_types, sizeof(char *));
        if (slot) *slot = strdup(g_xdnd.mimes[i]);
    }
    if (g_xdnd.cached_bytes && g_xdnd.cached_len > 0) {
        xsrc->payload = malloc(g_xdnd.cached_len);
        if (xsrc->payload) {
            memcpy(xsrc->payload, g_xdnd.cached_bytes, g_xdnd.cached_len);
            xsrc->payload_len = g_xdnd.cached_len;
        }
    }

    struct wlr_drag *drag = wlr_drag_create(sc, &xsrc->base, nullptr);
    if (!drag) {
        wlr_data_source_destroy(&xsrc->base);
        return;
    }

    g_xdnd.button_session_active = false;
    if (server->seat->pointer_state.button_count == 0 && server->last_pointer_button != 0) {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_button(
            server->seat, time_msec, server->last_pointer_button, WL_POINTER_BUTTON_STATE_PRESSED
        );
    }
    uint32_t serial = wl_display_next_serial(server->wl_display);
    wlr_seat_start_pointer_drag(server->seat, drag, serial);
    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void handle_targets_reply(DtwmServer *server) {
    xcb_connection_t *conn = server->xcb_conn;
    xcb_get_property_cookie_t ck = xcb_get_property(
        conn, 1, server->xcb_wm_win, g_xdnd.atom_prop_targets, XCB_ATOM_ANY, 0, 64
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(conn, ck, nullptr);
    if (!rep) return;

    xdnd_clear_mimes_and_cache();
    int n_atoms = xcb_get_property_value_length(rep) / 4;
    const xcb_atom_t *atoms = (const xcb_atom_t *)xcb_get_property_value(rep);

    xcb_atom_t best_atom = XCB_ATOM_NONE;
    int best_prio = 0;

    for (int i = 0; i < n_atoms; i++) {
        xcb_atom_t a = atoms[i];
        if (a == XCB_ATOM_NONE) continue;
        xcb_get_atom_name_cookie_t nck = xcb_get_atom_name(conn, a);
        xcb_get_atom_name_reply_t *nrep = xcb_get_atom_name_reply(conn, nck, nullptr);
        if (!nrep) continue;
        int nlen = xcb_get_atom_name_name_length(nrep);
        char name[128];
        if (nlen > 0 && nlen < (int)sizeof(name)) {
            memcpy(name, xcb_get_atom_name_name(nrep), (size_t)nlen);
            name[nlen] = '\0';
            if (!is_ignored_target_name(name)) {
                xdnd_add_mime(name);
                if (strcmp(name, "UTF8_STRING") == 0 || strcmp(name, "STRING") == 0) {
                    xdnd_add_mime("text/plain;charset=utf-8");
                }
                int prio = 1;
                if (a == g_xdnd.atom_uri_list || strcmp(name, "text/uri-list") == 0) prio = 4;
                else if (a == g_xdnd.atom_text_plain_utf8) prio = 3;
                else if (a == g_xdnd.atom_utf8 || a == XCB_ATOM_STRING) prio = 2;
                if (prio > best_prio) {
                    best_prio = prio;
                    best_atom = a;
                }
            }
        }
        free(nrep);
    }
    free(rep);

    if (g_xdnd.mime_count > 0 && best_atom != XCB_ATOM_NONE) {
        g_xdnd.targets_ready = true;
        g_xdnd.preferred_target = best_atom;
        xcb_delete_property(conn, server->xcb_wm_win, g_xdnd.atom_prop_data);
        xcb_convert_selection(
            conn, server->xcb_wm_win, g_xdnd.atom_xdnd_sel,
            best_atom, g_xdnd.atom_prop_data, XCB_CURRENT_TIME
        );
        xcb_flush(conn);
    }
}

static void handle_payload_reply(DtwmServer *server) {
    xcb_connection_t *conn = server->xcb_conn;
    xcb_get_property_cookie_t ck = xcb_get_property(
        conn, 1, server->xcb_wm_win, g_xdnd.atom_prop_data, XCB_ATOM_ANY, 0, DTWM_XDND_MAX_BYTES / 4
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(conn, ck, nullptr);
    if (!rep) return;

    int len = xcb_get_property_value_length(rep);
    if (len > 0) {
        free(g_xdnd.cached_bytes);
        g_xdnd.cached_bytes = malloc((size_t)len);
        if (g_xdnd.cached_bytes) {
            memcpy(g_xdnd.cached_bytes, xcb_get_property_value(rep), (size_t)len);
            g_xdnd.cached_len = (size_t)len;
            g_xdnd.payload_ready = true;
        }
    }
    free(rep);
    if (g_xdnd.payload_ready) {
        dtwm_recheck_cursor_focus(server);
    }
}

bool dtwm_xdnd_handle_xcb_event(DtwmServer *server, xcb_generic_event_t *ev) {
    if (!server || !ev || !g_xdnd.xfixes_ready) return false;
    uint8_t rt = ev->response_type & ~0x80u;

    if (rt == (uint8_t)(g_xdnd.xfixes_first_event + XCB_XFIXES_SELECTION_NOTIFY)) {
        xcb_xfixes_selection_notify_event_t *sev = (xcb_xfixes_selection_notify_event_t *)ev;
        if (sev->selection == g_xdnd.atom_xdnd_sel) {
            g_xdnd.owner = sev->owner;
            if (sev->owner != XCB_WINDOW_NONE && server->seat->drag == nullptr &&
                server->pointer_buttons_down > 0) {
                g_xdnd.button_session_active = true;
                xdnd_clear_mimes_and_cache();
                xcb_delete_property(server->xcb_conn, server->xcb_wm_win, g_xdnd.atom_prop_targets);
                xcb_convert_selection(
                    server->xcb_conn, server->xcb_wm_win, g_xdnd.atom_xdnd_sel,
                    g_xdnd.atom_targets, g_xdnd.atom_prop_targets, XCB_CURRENT_TIME
                );
                xcb_flush(server->xcb_conn);
            } else if (sev->owner == XCB_WINDOW_NONE && server->pointer_buttons_down <= 0) {
                g_xdnd.button_session_active = false;
                xdnd_clear_mimes_and_cache();
            }
            return true;
        }
    } else if (rt == XCB_SELECTION_NOTIFY) {
        xcb_selection_notify_event_t *snev = (xcb_selection_notify_event_t *)ev;
        if (snev->requestor == server->xcb_wm_win && snev->selection == g_xdnd.atom_xdnd_sel) {
            if (snev->property == g_xdnd.atom_prop_targets) {
                handle_targets_reply(server);
            } else if (snev->property == g_xdnd.atom_prop_data) {
                handle_payload_reply(server);
            }
            return true;
        }
    }
    return false;
}

void dtwm_xdnd_on_button_state(DtwmServer *server, uint32_t button, bool pressed) {
    if (!server) return;
    uint32_t bit = (button >= 0x110u && button < 0x120u)
                       ? (1u << (button - 0x110u))
                       : (1u << 16u);
    if (pressed) {
        server->pointer_buttons_mask |= bit;
        server->last_pointer_button = button;
    } else {
        server->pointer_buttons_mask &= ~bit;
    }
    server->pointer_buttons_down = __builtin_popcount(server->pointer_buttons_mask);
    if (server->pointer_buttons_down == 0) {
        g_xdnd.button_session_active = false;
    }
}

void dtwm_xdnd_on_xwayland_ready(DtwmServer *server) {
    if (!server || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) return;
    xcb_connection_t *conn = server->xcb_conn;

    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(conn, &xcb_xfixes_id);
    if (!ext || !ext->present) return;
    g_xdnd.xfixes_first_event = ext->first_event;

    xcb_xfixes_query_version_cookie_t vck = xcb_xfixes_query_version(conn, 5, 0);
    free(xcb_xfixes_query_version_reply(conn, vck, nullptr));

    g_xdnd.atom_xdnd_sel = xdnd_intern_atom(conn, "XdndSelection");
    g_xdnd.atom_xdnd_typelist = xdnd_intern_atom(conn, "XdndTypeList");
    g_xdnd.atom_targets = xdnd_intern_atom(conn, "TARGETS");
    g_xdnd.atom_utf8 = xdnd_intern_atom(conn, "UTF8_STRING");
    g_xdnd.atom_uri_list = xdnd_intern_atom(conn, "text/uri-list");
    g_xdnd.atom_text_plain_utf8 = xdnd_intern_atom(conn, "text/plain;charset=utf-8");
    g_xdnd.atom_prop_targets = xdnd_intern_atom(conn, "_DTWM_XDND_TARGETS");
    g_xdnd.atom_prop_data = xdnd_intern_atom(conn, "_DTWM_XDND_DATA");

    xcb_xfixes_select_selection_input(
        conn,
        server->xcb_root,
        g_xdnd.atom_xdnd_sel,
        XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
            XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
            XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE
    );
    xcb_flush(conn);
    g_xdnd.xfixes_ready = true;
}

void dtwm_xdnd_finish(void) {
    xdnd_clear_mimes_and_cache();
    g_xdnd.xfixes_ready = false;
}
