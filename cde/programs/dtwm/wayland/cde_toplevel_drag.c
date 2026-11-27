/*
 * CDE - Common Desktop Environment
 *
 * wayland/cde_toplevel_drag.c: Server-side xdg-toplevel-drag-v1 protocol
 * implementation enabling live window/tab tear-off and re-attachment during
 * Wayland wl_data_device drag-and-drop operations.
 */

#define _GNU_SOURCE
#include "dtwm_wayland.h"
#include "xdg-toplevel-drag-v1-protocol.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct wl_list link; /* DtwmServer.toplevel_drags */
    DtwmServer *server;
    struct wl_resource *resource;          /* xdg_toplevel_drag_v1 */
    struct wl_resource *manager_resource;  /* xdg_toplevel_drag_manager_v1 (nullable) */
    struct wl_listener manager_destroy;
    struct wl_resource *data_source_res;   /* wl_data_source */
    struct wlr_data_source *wlr_source;
    struct wl_listener data_source_destroy;

    struct wlr_drag *active_drag;
    bool drag_started;
    bool drag_ended; /* true once dnd_drop_performed or drag destroy occurs */
    struct wl_listener drag_drop;
    struct wl_listener drag_destroy;
    bool drag_listeners_attached;

    struct wlr_xdg_toplevel *attached_toplevel;
    int32_t x_offset;
    int32_t y_offset;
    struct wl_listener toplevel_unmap;
    struct wl_listener toplevel_destroy;
    bool toplevel_listeners_attached;
} DtwmToplevelDrag;

static DtwmView *find_view_for_xdg_toplevel(DtwmServer *server, struct wlr_xdg_toplevel *tl) {
    if (!server || !tl) return nullptr;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->kind == DTWM_VIEW_XDG && v->xdg_toplevel == tl) {
            return v;
        }
    }
    return nullptr;
}

static void detach_toplevel_listeners(DtwmToplevelDrag *td) {
    if (!td) return;
    if (td->toplevel_listeners_attached) {
        wl_list_remove(&td->toplevel_unmap.link);
        wl_list_remove(&td->toplevel_destroy.link);
        td->toplevel_listeners_attached = false;
    }
    td->attached_toplevel = nullptr;
}

static void detach_drag_listeners(DtwmToplevelDrag *td) {
    if (!td) return;
    if (td->drag_listeners_attached) {
        wl_list_remove(&td->drag_drop.link);
        wl_list_remove(&td->drag_destroy.link);
        td->drag_listeners_attached = false;
    }
    td->active_drag = nullptr;
}

static void position_attached_view_at_cursor(DtwmToplevelDrag *td) {
    if (!td || !td->server || !td->server->cursor || !td->attached_toplevel) {
        return;
    }
    if (!td->drag_started || td->drag_ended) {
        return;
    }
    DtwmView *view = find_view_for_xdg_toplevel(td->server, td->attached_toplevel);
    if (!view || !view->mapped) {
        return;
    }

    if (view->maximized) {
        dtwm_view_set_maximized(view, false);
    }

    int l = 0, t = 0, r = 0, b = 0;
    dtwm_view_insets(view, &l, &t, &r, &b);
    double s = (!view->is_native_dpi && view->external_client_scale > 0.25)
                   ? view->external_client_scale
                   : 1.0;
    int scaled_x_off = (int)lround((double)td->x_offset * s);
    int scaled_y_off = (int)lround((double)td->y_offset * s);

    view->x = (int)lround(td->server->cursor->x) - l - scaled_x_off;
    view->y = (int)lround(td->server->cursor->y) - t - scaled_y_off;

    if (view->frame_tree) {
        wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
        wlr_scene_node_raise_to_top(&view->frame_tree->node);
    }
    dtwm_ime_update_popups(td->server);
}

static void handle_attached_toplevel_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmToplevelDrag *td = wl_container_of(listener, td, toplevel_unmap);
    /* Spec: if the toplevel is unmapped while attached, it is automatically detached */
    detach_toplevel_listeners(td);
}

static void handle_attached_toplevel_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmToplevelDrag *td = wl_container_of(listener, td, toplevel_destroy);
    detach_toplevel_listeners(td);
}

static void handle_td_drag_drop(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmToplevelDrag *td = wl_container_of(listener, td, drag_drop);
    /* dnd_drop_performed will be emitted to source; drag move phase ends */
    td->drag_ended = true;
    if (td->server && td->attached_toplevel) {
        DtwmView *view = find_view_for_xdg_toplevel(td->server, td->attached_toplevel);
        if (view && view->mapped) {
            dtwm_restore_view_focus(td->server, view);
        }
    }
}

static void handle_td_drag_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmToplevelDrag *td = wl_container_of(listener, td, drag_destroy);
    td->drag_ended = true;
    detach_drag_listeners(td);
}

static void handle_td_manager_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmToplevelDrag *td = wl_container_of(listener, td, manager_destroy);
    wl_list_remove(&td->manager_destroy.link);
    wl_list_init(&td->manager_destroy.link);
    td->manager_resource = nullptr;
}

static void handle_td_data_source_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    DtwmToplevelDrag *td = wl_container_of(listener, td, data_source_destroy);
    wl_list_remove(&td->data_source_destroy.link);
    wl_list_init(&td->data_source_destroy.link);
    td->data_source_res = nullptr;
    td->wlr_source = nullptr;
    td->drag_ended = true;
}

static void toplevel_drag_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    DtwmToplevelDrag *td = wl_resource_get_user_data(resource);
    if (td && td->drag_started && !td->drag_ended && td->active_drag != nullptr) {
        wl_resource_post_error(
            resource,
            XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG,
            "xdg_toplevel_drag_v1 destroyed before drag ended"
        );
        return;
    }
    wl_resource_destroy(resource);
}

static void toplevel_drag_handle_attach(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *toplevel_resource,
    int32_t x_offset,
    int32_t y_offset
) {
    (void)client;
    DtwmToplevelDrag *td = wl_resource_get_user_data(resource);
    if (!td || !toplevel_resource) {
        return;
    }

    if (td->attached_toplevel != nullptr) {
        wl_resource_post_error(
            resource,
            XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED,
            "xdg_toplevel_drag_v1 already has an attached toplevel"
        );
        return;
    }

    struct wlr_xdg_toplevel *tl = wlr_xdg_toplevel_from_resource(toplevel_resource);
    if (!tl || !tl->base || !tl->base->surface) {
        return;
    }

    td->attached_toplevel = tl;
    td->x_offset = x_offset;
    td->y_offset = y_offset;

    td->toplevel_unmap.notify = handle_attached_toplevel_unmap;
    wl_signal_add(&tl->base->surface->events.unmap, &td->toplevel_unmap);
    td->toplevel_destroy.notify = handle_attached_toplevel_destroy;
    wl_signal_add(&tl->events.destroy, &td->toplevel_destroy);
    td->toplevel_listeners_attached = true;

    if (td->server) {
        td->server->toplevel_drag_attach_total++;
        dtwm_publish_presentation_state(td->server);
    }
    position_attached_view_at_cursor(td);
    if (td->server) {
        dtwm_recheck_cursor_focus(td->server);
    }
}

static const struct xdg_toplevel_drag_v1_interface toplevel_drag_impl = {
    .destroy = toplevel_drag_handle_destroy,
    .attach = toplevel_drag_handle_attach,
};

static void toplevel_drag_resource_destroy(struct wl_resource *resource) {
    DtwmToplevelDrag *td = wl_resource_get_user_data(resource);
    if (!td) return;
    detach_toplevel_listeners(td);
    detach_drag_listeners(td);
    if (!wl_list_empty(&td->manager_destroy.link)) {
        wl_list_remove(&td->manager_destroy.link);
    }
    if (!wl_list_empty(&td->data_source_destroy.link)) {
        wl_list_remove(&td->data_source_destroy.link);
    }
    wl_list_remove(&td->link);
    free(td);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_handle_get_xdg_toplevel_drag(
    struct wl_client *client,
    struct wl_resource *manager_resource,
    uint32_t id,
    struct wl_resource *data_source_resource
) {
    DtwmServer *server = wl_resource_get_user_data(manager_resource);
    if (!server || !data_source_resource) {
        return;
    }

    struct wlr_data_source *wlr_src = wl_resource_get_user_data(data_source_resource);
    if (!wlr_src) {
        wl_resource_post_error(
            manager_resource,
            XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
            "invalid wl_data_source"
        );
        return;
    }

    /* Check that source has not already been bound to a toplevel drag or active selection/drag */
    if ((server->seat && server->seat->selection_source == wlr_src) ||
        (server->seat && server->seat->drag && server->seat->drag->source == wlr_src)) {
        wl_resource_post_error(
            manager_resource,
            XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
            "wl_data_source already used for selection or active drag"
        );
        return;
    }

    DtwmToplevelDrag *existing;
    wl_list_for_each(existing, &server->toplevel_drags, link) {
        if (existing->data_source_res == data_source_resource || existing->wlr_source == wlr_src) {
            wl_resource_post_error(
                manager_resource,
                XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
                "wl_data_source already has an xdg_toplevel_drag_v1"
            );
            return;
        }
    }

    struct wl_resource *drag_res = wl_resource_create(
        client, &xdg_toplevel_drag_v1_interface, wl_resource_get_version(manager_resource), id
    );
    if (!drag_res) {
        wl_client_post_no_memory(client);
        return;
    }

    DtwmToplevelDrag *td = calloc(1, sizeof(*td));
    if (!td) {
        wl_resource_destroy(drag_res);
        wl_client_post_no_memory(client);
        return;
    }

    td->server = server;
    td->resource = drag_res;
    td->manager_resource = manager_resource;
    wl_list_init(&td->manager_destroy.link);
    td->manager_destroy.notify = handle_td_manager_destroy;
    wl_resource_add_destroy_listener(manager_resource, &td->manager_destroy);
    td->data_source_res = data_source_resource;
    td->wlr_source = wlr_src;
    wl_list_init(&td->data_source_destroy.link);
    td->data_source_destroy.notify = handle_td_data_source_destroy;
    wl_resource_add_destroy_listener(data_source_resource, &td->data_source_destroy);

    wl_list_insert(&server->toplevel_drags, &td->link);
    wl_resource_set_implementation(
        drag_res, &toplevel_drag_impl, td, toplevel_drag_resource_destroy
    );
}

static const struct xdg_toplevel_drag_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_xdg_toplevel_drag = manager_handle_get_xdg_toplevel_drag,
};

static void bind_toplevel_drag_manager(
    struct wl_client *client, void *data, uint32_t version, uint32_t id
) {
    DtwmServer *server = data;
    uint32_t ver = version < 1 ? version : 1;
    struct wl_resource *res =
        wl_resource_create(client, &xdg_toplevel_drag_manager_v1_interface, (int)ver, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &manager_impl, server, nullptr);
}

void dtwm_toplevel_drag_init(DtwmServer *server) {
    if (!server || !server->wl_display) return;
    wl_list_init(&server->toplevel_drags);
    server->toplevel_drag_global = wl_global_create(
        server->wl_display, &xdg_toplevel_drag_manager_v1_interface, 1, server, bind_toplevel_drag_manager
    );
}

void dtwm_toplevel_drag_finish(DtwmServer *server) {
    if (!server) return;
    DtwmToplevelDrag *td, *tmp;
    wl_list_for_each_safe(td, tmp, &server->toplevel_drags, link) {
        if (td->resource) {
            wl_resource_set_user_data(td->resource, nullptr);
        }
        toplevel_drag_resource_destroy(td->resource);
    }
    if (server->toplevel_drag_global) {
        wl_global_destroy(server->toplevel_drag_global);
        server->toplevel_drag_global = nullptr;
    }
}

bool dtwm_toplevel_drag_check_set_selection(DtwmServer *server, struct wlr_data_source *source) {
    if (!server || !source) return true;
    DtwmToplevelDrag *td;
    wl_list_for_each(td, &server->toplevel_drags, link) {
        if (td->wlr_source == source && (td->manager_resource || td->resource)) {
            wl_resource_post_error(
                td->manager_resource ? td->manager_resource : td->resource,
                XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
                "wl_data_source bound to xdg_toplevel_drag_v1 cannot be used for set_selection"
            );
            return false;
        }
    }
    return true;
}

void dtwm_toplevel_drag_on_start_drag(DtwmServer *server, struct wlr_drag *drag) {
    if (!server || !drag || !drag->source) return;
    DtwmToplevelDrag *td;
    wl_list_for_each(td, &server->toplevel_drags, link) {
        if (td->wlr_source == drag->source) {
            detach_drag_listeners(td);
            td->active_drag = drag;
            td->drag_started = true;
            td->drag_ended = false;
            td->drag_drop.notify = handle_td_drag_drop;
            wl_signal_add(&drag->events.drop, &td->drag_drop);
            td->drag_destroy.notify = handle_td_drag_destroy;
            wl_signal_add(&drag->events.destroy, &td->drag_destroy);
            td->drag_listeners_attached = true;
            position_attached_view_at_cursor(td);
            break;
        }
    }
}

void dtwm_toplevel_drag_on_cursor_motion(DtwmServer *server) {
    if (!server) return;
    DtwmToplevelDrag *td;
    wl_list_for_each(td, &server->toplevel_drags, link) {
        if (td->drag_started && !td->drag_ended && td->active_drag && td->attached_toplevel) {
            position_attached_view_at_cursor(td);
        }
    }
}

void dtwm_toplevel_drag_on_view_mapped(DtwmView *view) {
    if (!view || !view->server || view->kind != DTWM_VIEW_XDG || !view->xdg_toplevel) return;
    DtwmToplevelDrag *td;
    wl_list_for_each(td, &view->server->toplevel_drags, link) {
        if (td->attached_toplevel == view->xdg_toplevel && td->drag_started && !td->drag_ended) {
            position_attached_view_at_cursor(td);
            break;
        }
    }
}

DtwmView *dtwm_toplevel_drag_attached_view(const DtwmServer *server) {
    if (!server || !server->seat || !server->seat->drag) return nullptr;
    const DtwmToplevelDrag *td;
    wl_list_for_each(td, &server->toplevel_drags, link) {
        if (td->drag_started && !td->drag_ended && td->active_drag == server->seat->drag &&
            td->attached_toplevel) {
            return find_view_for_xdg_toplevel((DtwmServer *)server, td->attached_toplevel);
        }
    }
    return nullptr;
}
