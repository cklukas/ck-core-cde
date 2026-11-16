/*
 * CDE - Common Desktop Environment
 * cde_icons.c: Desktop open-window & minimized icons (Dtwm*showOpenWindowIcons),
 * active-focus icon markers, WM_ICON_SIZE (48/96), live WM_HINTS.icon_pixmap +
 * 1-bit icon_mask streaming, native Wayland xdg-toplevel-icon-v1 server,
 * XWayland virtual root pointer sync for ck-eyes, dtfile pinned desktop
 * windows, and Front Panel TYPE client window swallowing.
 */

#include "dtwm_wayland.h"
#include "xdg-toplevel-icon-v1-protocol.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_buffer.h>
#include <xcb/xcb.h>

/* X11 WM_HINTS bitflags & WM_STATE values */
#define DTWM_X11_STATE_HINT       (1u << 1)
#define DTWM_X11_ICON_PIXMAP_HINT (1u << 2)
#define DTWM_X11_ICON_MASK_HINT   (1u << 5)
#define DTWM_X11_NORMAL_STATE     1u
#define DTWM_X11_ICONIC_STATE     3u

/* _DT_WM_SETTINGS_V1 constants (matching <Dt/WmSettings.h>) */
#define DTWM_SETTINGS_V1_VERSION                1u
#define DTWM_SETTINGS_V1_SHOW_OPEN_WINDOW_ICONS (1u << 0)

/* --- Native Wayland xdg-toplevel-icon-v1 State --- */
typedef struct {
    cairo_surface_t *surface;
    char icon_name[128];
    bool assigned;
} DtwmWlIcon;

typedef struct {
    struct wl_list link;
    struct wlr_xdg_toplevel *toplevel;
    cairo_surface_t *surface;
} DtwmPendingXdgIcon;

static struct wl_list g_pending_xdg_icons;

static DtwmView *find_xdg_view(DtwmServer *server, struct wlr_xdg_toplevel *toplevel) {
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->kind == DTWM_VIEW_XDG && v->xdg_toplevel == toplevel) {
            return v;
        }
    }
    return nullptr;
}

static cairo_surface_t *copy_shm_buffer_to_cairo(struct wl_resource *buf_res) {
    struct wlr_buffer *wbuf = wlr_buffer_try_from_resource(buf_res);
    if (!wbuf) return nullptr;
    int w = wbuf->width;
    int h = wbuf->height;
    if (w <= 0 || h <= 0 || w != h || w > 512) {
        return nullptr;
    }

    void *raw_data = nullptr;
    uint32_t drm_fmt = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(
            wbuf, WLR_BUFFER_DATA_PTR_ACCESS_READ, &raw_data, &drm_fmt, &stride
        )) {
        return nullptr;
    }
    if (drm_fmt != DRM_FORMAT_ARGB8888 && drm_fmt != DRM_FORMAT_XRGB8888) {
        wlr_buffer_end_data_ptr_access(wbuf);
        return nullptr;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        wlr_buffer_end_data_ptr_access(wbuf);
        cairo_surface_destroy(surf);
        return nullptr;
    }

    uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(surf);
    int dst_stride_u32 = cairo_image_surface_get_stride(surf) / 4;
    const uint8_t *src_bytes = (const uint8_t *)raw_data;
    for (int y = 0; y < h; ++y) {
        const uint32_t *srow = (const uint32_t *)(src_bytes + (size_t)y * stride);
        uint32_t *drow = dst + y * dst_stride_u32;
        for (int x = 0; x < w; ++x) {
            uint32_t px = srow[x];
            if (drm_fmt == DRM_FORMAT_XRGB8888) {
                px |= 0xFF000000u;
            }
            drow[x] = px;
        }
    }
    wlr_buffer_end_data_ptr_access(wbuf);
    cairo_surface_mark_dirty(surf);
    return surf;
}

static void wl_icon_destroy_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void wl_icon_set_name_req(struct wl_client *client, struct wl_resource *resource, const char *icon_name) {
    (void)client;
    DtwmWlIcon *ic = wl_resource_get_user_data(resource);
    if (!ic) return;
    if (ic->assigned) {
        wl_resource_post_error(
            resource, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE, "xdg_toplevel_icon_v1 already assigned"
        );
        return;
    }
    snprintf(ic->icon_name, sizeof(ic->icon_name), "%s", icon_name ? icon_name : "");
}

static void wl_icon_add_buffer_req(
    struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer, int32_t scale
) {
    (void)client;
    (void)scale;
    DtwmWlIcon *ic = wl_resource_get_user_data(resource);
    if (!ic) return;
    if (ic->assigned) {
        wl_resource_post_error(
            resource, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE, "xdg_toplevel_icon_v1 already assigned"
        );
        return;
    }
    cairo_surface_t *copied = copy_shm_buffer_to_cairo(buffer);
    if (!copied) {
        wl_resource_post_error(
            resource, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER, "expected square ARGB8888/XRGB8888 wl_shm buffer"
        );
        return;
    }
    /* Keep highest-resolution buffer supplied */
    if (ic->surface) {
        int old_w = cairo_image_surface_get_width(ic->surface);
        int new_w = cairo_image_surface_get_width(copied);
        if (new_w >= old_w) {
            cairo_surface_destroy(ic->surface);
            ic->surface = copied;
        } else {
            cairo_surface_destroy(copied);
        }
    } else {
        ic->surface = copied;
    }
}

static const struct xdg_toplevel_icon_v1_interface wl_icon_impl = {
    .destroy = wl_icon_destroy_req,
    .set_name = wl_icon_set_name_req,
    .add_buffer = wl_icon_add_buffer_req,
};

static void wl_icon_res_destroy(struct wl_resource *resource) {
    DtwmWlIcon *ic = wl_resource_get_user_data(resource);
    if (!ic) return;
    if (ic->surface) {
        cairo_surface_destroy(ic->surface);
    }
    free(ic);
}

static void wl_icon_mgr_destroy_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void wl_icon_mgr_create_icon_req(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *ires = wl_resource_create(
        client, &xdg_toplevel_icon_v1_interface, wl_resource_get_version(resource), id
    );
    if (!ires) {
        wl_client_post_no_memory(client);
        return;
    }
    DtwmWlIcon *ic = calloc(1, sizeof(*ic));
    wl_resource_set_implementation(ires, &wl_icon_impl, ic, wl_icon_res_destroy);
}

static void wl_icon_mgr_set_icon_req(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *toplevel_res,
    struct wl_resource *icon_res
) {
    (void)client;
    DtwmServer *server = wl_resource_get_user_data(resource);
    if (!server || !toplevel_res) return;

    struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_from_resource(toplevel_res);
    if (!toplevel) return;

    cairo_surface_t *clone = nullptr;
    if (icon_res) {
        DtwmWlIcon *ic = wl_resource_get_user_data(icon_res);
        if (ic) {
            ic->assigned = true;
            if (ic->surface) {
                int w = cairo_image_surface_get_width(ic->surface);
                int h = cairo_image_surface_get_height(ic->surface);
                clone = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
                cairo_t *cr = cairo_create(clone);
                cairo_set_source_surface(cr, ic->surface, 0, 0);
                cairo_paint(cr);
                cairo_destroy(cr);
            }
        }
    }

    DtwmView *view = find_xdg_view(server, toplevel);
    if (!view) {
        /* Client called set_icon before initial wl_surface.commit; stage until map */
        DtwmPendingXdgIcon *pend, *tmp;
        wl_list_for_each_safe(pend, tmp, &g_pending_xdg_icons, link) {
            if (pend->toplevel == toplevel) {
                if (pend->surface) cairo_surface_destroy(pend->surface);
                wl_list_remove(&pend->link);
                free(pend);
            }
        }
        if (clone) {
            pend = calloc(1, sizeof(*pend));
            pend->toplevel = toplevel;
            pend->surface = clone;
            wl_list_insert(&g_pending_xdg_icons, &pend->link);
        }
        return;
    }

    if (view->icon_surface) {
        cairo_surface_destroy(view->icon_surface);
        view->icon_surface = nullptr;
    }
    view->icon_surface = clone;
    dtwm_icons_update_view(view);
}

static const struct xdg_toplevel_icon_manager_v1_interface wl_icon_mgr_impl = {
    .destroy = wl_icon_mgr_destroy_req,
    .create_icon = wl_icon_mgr_create_icon_req,
    .set_icon = wl_icon_mgr_set_icon_req,
};

static void bind_wl_icon_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    DtwmServer *server = data;
    struct wl_resource *res = wl_resource_create(
        client, &xdg_toplevel_icon_manager_v1_interface, (int)version, id
    );
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &wl_icon_mgr_impl, server, nullptr);
    xdg_toplevel_icon_manager_v1_send_icon_size(res, server->large_icons ? 96 : 48);
    xdg_toplevel_icon_manager_v1_send_icon_size(res, 96);
    xdg_toplevel_icon_manager_v1_send_done(res);
}

/* --- XWayland WM_ICON_SIZE, _DT_WM_SETTINGS_V1, WM_STATE & WM_HINTS.icon_pixmap --- */
static xcb_atom_t intern_atom_icons(xcb_connection_t *conn, const char *name) {
    if (!conn || !name || !name[0]) return XCB_ATOM_NONE;
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, nullptr);
    if (!rep) return XCB_ATOM_NONE;
    xcb_atom_t a = rep->atom;
    free(rep);
    return a;
}

static void publish_icon_sizes_and_settings(DtwmServer *server) {
    if (!server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) return;

    uint32_t max_dim = server->large_icons ? 96u : 48u;
    uint32_t min_dim = server->large_icons ? 48u : 32u;
    uint32_t icon_sizes[6] = {min_dim, min_dim, max_dim, max_dim, 16u, 16u};
    xcb_change_property(
        server->xcb_conn,
        XCB_PROP_MODE_REPLACE,
        server->xcb_root,
        XCB_ATOM_WM_ICON_SIZE,
        XCB_ATOM_WM_ICON_SIZE,
        32,
        6,
        icon_sizes
    );

    if (server->atom_wm_settings_v1 != XCB_ATOM_NONE) {
        uint32_t settings[2] = {
            DTWM_SETTINGS_V1_VERSION,
            server->show_open_window_icons ? DTWM_SETTINGS_V1_SHOW_OPEN_WINDOW_ICONS : 0u,
        };
        xcb_change_property(
            server->xcb_conn,
            XCB_PROP_MODE_REPLACE,
            server->xcb_root,
            server->atom_wm_settings_v1,
            XCB_ATOM_CARDINAL,
            32,
            2,
            settings
        );
        if (server->xcb_wm_win != XCB_WINDOW_NONE) {
            xcb_change_property(
                server->xcb_conn,
                XCB_PROP_MODE_REPLACE,
                server->xcb_wm_win,
                server->atom_wm_settings_v1,
                XCB_ATOM_CARDINAL,
                32,
                2,
                settings
            );
        }
    }
    xcb_flush(server->xcb_conn);
}

void dtwm_icons_set_wm_state(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return;
    DtwmServer *server = view->server;
    if (!server->xcb_conn || server->atom_wm_state == XCB_ATOM_NONE) return;

    uint32_t state[2] = {
        view->minimized ? DTWM_X11_ICONIC_STATE : DTWM_X11_NORMAL_STATE,
        XCB_WINDOW_NONE,
    };
    xcb_change_property(
        server->xcb_conn,
        XCB_PROP_MODE_REPLACE,
        view->xwayland_surface->window_id,
        server->atom_wm_state,
        server->atom_wm_state,
        32,
        2,
        state
    );
    xcb_flush(server->xcb_conn);
}

static void publish_client_icon_geometry(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return;
    DtwmServer *server = view->server;
    if (!server->xcb_conn || server->atom_net_wm_icon_geom == XCB_ATOM_NONE) return;

    if (view->icon_w > 0 && view->icon_h > 0) {
        uint32_t geom[4] = {
            (uint32_t)(view->icon_x >= 0 ? view->icon_x : 0),
            (uint32_t)(view->icon_y >= 0 ? view->icon_y : 0),
            (uint32_t)view->icon_w,
            (uint32_t)view->icon_h,
        };
        xcb_change_property(
            server->xcb_conn,
            XCB_PROP_MODE_REPLACE,
            view->xwayland_surface->window_id,
            server->atom_net_wm_icon_geom,
            XCB_ATOM_CARDINAL,
            32,
            4,
            geom
        );
    } else {
        xcb_delete_property(
            server->xcb_conn, view->xwayland_surface->window_id, server->atom_net_wm_icon_geom
        );
    }
    xcb_flush(server->xcb_conn);
}

static bool extract_1bit_xy_bit(
    const uint8_t *data, int stride_bytes, bool msb_first, int x, int y
) {
    const uint8_t *row = data + y * stride_bytes;
    uint8_t byte = row[x >> 3];
    int bit = msb_first ? (7 - (x & 7)) : (x & 7);
    return ((byte >> bit) & 1u) != 0;
}

static cairo_surface_t *fetch_x11_icon_pixmap(
    DtwmServer *server, xcb_pixmap_t pixmap, xcb_pixmap_t mask_pixmap
) {
    if (!server->xcb_conn || pixmap == XCB_NONE) return nullptr;

    xcb_get_geometry_cookie_t gck = xcb_get_geometry(server->xcb_conn, pixmap);
    xcb_get_geometry_reply_t *grep = xcb_get_geometry_reply(server->xcb_conn, gck, nullptr);
    if (!grep) return nullptr;

    uint16_t w = grep->width;
    uint16_t h = grep->height;
    uint8_t depth = grep->depth;
    free(grep);

    if (w == 0 || h == 0 || w > 512 || h > 512) {
        return nullptr;
    }

    const xcb_setup_t *setup = xcb_get_setup(server->xcb_conn);
    bool bitmap_msb = (setup->bitmap_format_bit_order == XCB_IMAGE_ORDER_MSB_FIRST);
    int scanline_pad_bits = setup->bitmap_format_scanline_pad;
    if (scanline_pad_bits < 8) scanline_pad_bits = 32;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return nullptr;
    }
    uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(surf);
    int dst_stride_u32 = cairo_image_surface_get_stride(surf) / 4;

    if (depth == 1) {
        xcb_get_image_cookie_t ick = xcb_get_image(
            server->xcb_conn, XCB_IMAGE_FORMAT_XY_PIXMAP, pixmap, 0, 0, w, h, 1u
        );
        xcb_get_image_reply_t *irep = xcb_get_image_reply(server->xcb_conn, ick, nullptr);
        if (!irep) {
            cairo_surface_destroy(surf);
            return nullptr;
        }
        int row_bits = ((w + scanline_pad_bits - 1) / scanline_pad_bits) * scanline_pad_bits;
        int stride_bytes = row_bits / 8;
        const uint8_t *bits = xcb_get_image_data(irep);
        int data_len = xcb_get_image_data_length(irep);
        if (stride_bytes * h <= data_len) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    bool set = extract_1bit_xy_bit(bits, stride_bytes, bitmap_msb, x, y);
                    dst[y * dst_stride_u32 + x] = set ? 0xFF181C24u : 0xFFE6E9F0u;
                }
            }
        }
        free(irep);
    } else {
        xcb_get_image_cookie_t ick = xcb_get_image(
            server->xcb_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, 0, 0, w, h, ~0u
        );
        xcb_get_image_reply_t *irep = xcb_get_image_reply(server->xcb_conn, ick, nullptr);
        if (!irep) {
            cairo_surface_destroy(surf);
            return nullptr;
        }
        const uint8_t *raw = xcb_get_image_data(irep);
        int raw_len = xcb_get_image_data_length(irep);
        if (raw_len >= (int)w * (int)h * 4) {
            for (int y = 0; y < h; ++y) {
                const uint32_t *srow = (const uint32_t *)(raw + y * w * 4);
                for (int x = 0; x < w; ++x) {
                    uint32_t px = srow[x];
                    if (depth == 24) {
                        px |= 0xFF000000u;
                    } else if (depth == 32) {
                        uint8_t a = (uint8_t)(px >> 24);
                        if (a == 0 && (px & 0x00FFFFFFu) != 0) {
                            /* Many X11 clients draw with XFillRectangle into depth-32 without setting alpha */
                            px |= 0xFF000000u;
                        }
                    }
                    dst[y * dst_stride_u32 + x] = px;
                }
            }
        } else if (raw_len >= (int)w * (int)h * 2) {
            /* RGB565 fallback */
            for (int y = 0; y < h; ++y) {
                const uint16_t *srow = (const uint16_t *)(raw + y * w * 2);
                for (int x = 0; x < w; ++x) {
                    uint16_t p = srow[x];
                    uint32_t r = ((p >> 11) & 0x1Fu) * 255u / 31u;
                    uint32_t g = ((p >> 5) & 0x3Fu) * 255u / 63u;
                    uint32_t b = (p & 0x1Fu) * 255u / 31u;
                    dst[y * dst_stride_u32 + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
        free(irep);
    }

    /* Apply optional 1-bit IconMaskHint (used by ck-eyes & ck-browser for shaped desktop icons!) */
    if (mask_pixmap != XCB_NONE) {
        xcb_get_geometry_cookie_t mck = xcb_get_geometry(server->xcb_conn, mask_pixmap);
        xcb_get_geometry_reply_t *mrep = xcb_get_geometry_reply(server->xcb_conn, mck, nullptr);
        if (mrep) {
            uint16_t mw = mrep->width;
            uint16_t mh = mrep->height;
            uint8_t md = mrep->depth;
            free(mrep);
            if (md == 1 && mw > 0 && mh > 0 && mw <= 512 && mh <= 512) {
                xcb_get_image_cookie_t mick = xcb_get_image(
                    server->xcb_conn, XCB_IMAGE_FORMAT_XY_PIXMAP, mask_pixmap, 0, 0, mw, mh, 1u
                );
                xcb_get_image_reply_t *mirep = xcb_get_image_reply(server->xcb_conn, mick, nullptr);
                if (mirep) {
                    int row_bits = ((mw + scanline_pad_bits - 1) / scanline_pad_bits) * scanline_pad_bits;
                    int stride_bytes = row_bits / 8;
                    const uint8_t *mbits = xcb_get_image_data(mirep);
                    int mlen = xcb_get_image_data_length(mirep);
                    if (stride_bytes * mh <= mlen) {
                        int cw = w < mw ? w : mw;
                        int ch = h < mh ? h : mh;
                        for (int y = 0; y < h; ++y) {
                            for (int x = 0; x < w; ++x) {
                                bool opaque = (x < cw && y < ch) &&
                                              extract_1bit_xy_bit(mbits, stride_bytes, bitmap_msb, x, y);
                                if (!opaque) {
                                    dst[y * dst_stride_u32 + x] = 0x00000000u;
                                }
                            }
                        }
                    }
                    free(mirep);
                }
            }
        }
    }

    cairo_surface_mark_dirty(surf);
    return surf;
}

static void refresh_xwayland_wm_hints(DtwmView *view, bool check_initial_iconic) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return;
    DtwmServer *server = view->server;
    if (!server->xcb_conn) return;

    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn,
        0,
        view->xwayland_surface->window_id,
        XCB_ATOM_WM_HINTS,
        XCB_ATOM_WM_HINTS,
        0,
        9
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return;

    int len = xcb_get_property_value_length(rep);
    if (rep->format == 32 && len >= 16) {
        const uint32_t *vals = (const uint32_t *)xcb_get_property_value(rep);
        int nwords = len / 4;
        uint32_t flags = vals[0];
        uint32_t initial_state = (nwords > 2) ? vals[2] : 1u;
        xcb_pixmap_t icon_pix = ((flags & DTWM_X11_ICON_PIXMAP_HINT) && nwords > 3) ? vals[3] : XCB_NONE;
        xcb_pixmap_t icon_mask = ((flags & DTWM_X11_ICON_MASK_HINT) && nwords > 7) ? vals[7] : XCB_NONE;

        if (check_initial_iconic && (flags & DTWM_X11_STATE_HINT) &&
            initial_state == DTWM_X11_ICONIC_STATE && !view->is_swallowed && !view->is_desktop_pin) {
            view->minimized = true;
            wlr_xwayland_surface_set_minimized(view->xwayland_surface, true);
        }

        if (icon_pix != XCB_NONE) {
            cairo_surface_t *new_surf = fetch_x11_icon_pixmap(server, icon_pix, icon_mask);
            if (new_surf) {
                if (view->icon_surface) {
                    cairo_surface_destroy(view->icon_surface);
                }
                view->icon_surface = new_surf;
            }
        }
    }
    free(rep);
}

/* --- Desktop Icon Classification, Layout & Rendering --- */
static bool should_show_desktop_icon(const DtwmView *view) {
    if (!view || !view->mapped) return false;
    if (view->is_desktop_pin || view->is_swallowed) return false;
    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface &&
        view->xwayland_surface->override_redirect) {
        return false;
    }
    if ((view->workspace_mask & (1u << view->server->current_workspace)) == 0) {
        return false;
    }
    return view->minimized || view->server->show_open_window_icons;
}

static bool icon_surface_has_transparency(cairo_surface_t *surf) {
    if (!surf) return false;
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    int stride_u32 = cairo_image_surface_get_stride(surf) / 4;
    const uint32_t *px = (const uint32_t *)cairo_image_surface_get_data(surf);
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            if ((px[y * stride_u32 + x] >> 24) < 250u) {
                return true;
            }
        }
    }
    return false;
}

static void draw_default_cde_app_emblem(
    cairo_t *cr, double x, double y, double size, const DtwmView *view, const DtwmColorSet *cs
) {
    /* Miniature 3D CDE window glyph with terminal/document accent */
    double pad = floor(size * 0.12);
    double wx = x + pad;
    double wy = y + pad;
    double ww = size - 2 * pad;
    double wh = size - 2 * pad;

    cairo_set_source_rgba(cr, 0.92, 0.93, 0.96, 1.0);
    cairo_rectangle(cr, wx, wy, ww, wh);
    cairo_fill(cr);
    dtwm_draw_bevel(cr, wx, wy, ww, wh, 1.5, cs->top_shadow, cs->bottom_shadow);

    /* Mini titlebar */
    double th = fmax(6.0, floor(wh * 0.24));
    cairo_set_source_rgba(cr, 0.26, 0.44, 0.56, 1.0);
    cairo_rectangle(cr, wx + 2, wy + 2, ww - 4, th);
    cairo_fill(cr);

    const char *title = dtwm_view_title(view);
    char initial[4] = {0};
    initial[0] = (title && title[0]) ? title[0] : 'X';
    dtwm_draw_label(
        cr, initial, wx + 2, wy + th + 1, ww - 4, wh - th - 3,
        (DtwmRgba){0.14, 0.18, 0.26, 1.0}, true, true
    );
}

void dtwm_icons_update_view(DtwmView *view) {
    if (!view || !view->server) return;
    DtwmServer *server = view->server;

    if (!should_show_desktop_icon(view)) {
        if (view->desktop_icon_buf) {
            wlr_scene_buffer_set_buffer(view->desktop_icon_buf, nullptr);
            wlr_scene_node_set_enabled(&view->desktop_icon_buf->node, false);
        }
        view->icon_w = 0;
        view->icon_h = 0;
        publish_client_icon_geometry(view);
        return;
    }

    if (!view->desktop_icon_buf) {
        view->desktop_icon_buf = wlr_scene_buffer_create(server->desktop_icon_tree, nullptr);
    }

    int img_dim = server->large_icons ? 96 : 48;
    int frame_pad = 4;
    int img_box = img_dim + frame_pad * 2;
    int label_w = server->large_icons ? 112 : 76;
    if (label_w < img_box) label_w = img_box;
    int label_h = 20;
    int total_w = label_w;
    int total_h = img_box + 2 + label_h;

    view->icon_box_w = total_w;
    view->icon_box_h = total_h;
    int img_x = (total_w - img_box) / 2;
    view->icon_w = img_dim;
    view->icon_h = img_dim;

    cairo_t *cr = nullptr;
    cairo_surface_t *surf = nullptr;
    struct wlr_buffer *buf = dtwm_cairo_buffer_begin(total_w, total_h, &cr, &surf);
    if (!buf) return;

    /* Clear full tile to transparent so narrow icon box sits cleanly above wider label plate */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    /* Active-focus marker uses CDE active Crimson bevels; inactive uses CDE Slate */
    const DtwmColorSet *cs = view->active ? &server->palette.active : &server->palette.inactive;
    bool shaped = icon_surface_has_transparency(view->icon_surface);

    if (!shaped || view->active) {
        cairo_set_source_rgba(cr, cs->bg.r, cs->bg.g, cs->bg.b, shaped ? 0.35 : 1.0);
        cairo_rectangle(cr, img_x, 0, img_box, img_box);
        cairo_fill(cr);
        if (view->minimized && !view->active) {
            dtwm_draw_bevel(cr, img_x, 0, img_box, img_box, 2.0, cs->bottom_shadow, cs->top_shadow);
        } else {
            dtwm_draw_bevel(cr, img_x, 0, img_box, img_box, 2.0, cs->top_shadow, cs->bottom_shadow);
        }
    }

    /* Icon image (client dynamic pixmap or fallback CDE emblem) */
    if (view->icon_surface) {
        int sw = cairo_image_surface_get_width(view->icon_surface);
        int sh = cairo_image_surface_get_height(view->icon_surface);
        if (sw > 0 && sh > 0) {
            cairo_save(cr);
            cairo_translate(cr, img_x + frame_pad, frame_pad);
            cairo_scale(cr, (double)img_dim / sw, (double)img_dim / sh);
            cairo_set_source_surface(cr, view->icon_surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    } else {
        draw_default_cde_app_emblem(cr, img_x + frame_pad, frame_pad, img_dim, view, cs);
    }

    /* Bottom 3D CDE icon label plate */
    double ly = img_box + 2;
    cairo_set_source_rgba(cr, cs->bg.r, cs->bg.g, cs->bg.b, 1.0);
    cairo_rectangle(cr, 0, ly, label_w, label_h);
    cairo_fill(cr);
    dtwm_draw_bevel(cr, 0, ly, label_w, label_h, 1.5, cs->top_shadow, cs->bottom_shadow);

    const char *label = nullptr;
    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        label = dtwm_view_title(view);
    } else {
        label = dtwm_view_title(view);
    }
    dtwm_draw_label(cr, label, 2, ly, label_w - 4, label_h, cs->fg, true, view->active);

    dtwm_cairo_buffer_finish(cr, surf);
    wlr_scene_buffer_set_buffer(view->desktop_icon_buf, buf);
    wlr_scene_node_set_enabled(&view->desktop_icon_buf->node, true);
    wlr_buffer_drop(buf);
}

void dtwm_icons_relayout(DtwmServer *server) {
    if (!server) return;
    struct wlr_box ubox;
    dtwm_get_usable_output_box(server, 1, 1, &ubox);

    int img_dim = server->large_icons ? 96 : 48;
    int col_w = (server->large_icons ? 112 : 76) + 12;
    int row_h = (img_dim + 8 + 22) + 12;
    int max_rows = (ubox.height - 24) / row_h;
    if (max_rows < 1) max_rows = 1;

    /* Layout in stable creation order (reverse of MRU server->views) along top-right column */
    int slot = 0;
    DtwmView *v;
    wl_list_for_each_reverse(v, &server->views, link) {
        if (!should_show_desktop_icon(v)) {
            dtwm_icons_update_view(v);
            continue;
        }
        dtwm_icons_update_view(v);
        int col = slot / max_rows;
        int row = slot % max_rows;
        int tile_x = ubox.x + ubox.width - col_w * (col + 1) + 4;
        int tile_y = ubox.y + 14 + row * row_h;

        int img_box = img_dim + 8;
        int img_off_x = (v->icon_box_w - img_box) / 2 + 4;
        v->icon_x = tile_x + img_off_x;
        v->icon_y = tile_y + 4;
        if (v->desktop_icon_buf) {
            wlr_scene_node_set_position(&v->desktop_icon_buf->node, tile_x, tile_y);
        }
        publish_client_icon_geometry(v);
        slot++;
    }
}

DtwmView *dtwm_icons_view_at(DtwmServer *server, double lx, double ly) {
    if (!server) return nullptr;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (!should_show_desktop_icon(v) || !v->desktop_icon_buf) continue;
        int tx = v->desktop_icon_buf->node.x;
        int ty = v->desktop_icon_buf->node.y;
        if (lx >= tx && lx < tx + v->icon_box_w &&
            ly >= ty && ly < ty + v->icon_box_h) {
            return v;
        }
    }
    return nullptr;
}

static void read_xcb_wm_class_sync(
    DtwmServer *server, struct wlr_xwayland_surface *xs, char *inst, size_t inst_sz, char *cls, size_t cls_sz
) {
    snprintf(inst, inst_sz, "%s", xs->instance ? xs->instance : "");
    snprintf(cls, cls_sz, "%s", xs->class ? xs->class : "");
    if ((inst[0] != '\0' || cls[0] != '\0') || !server->xcb_conn) {
        return;
    }
    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn, 0, xs->window_id, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 64
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return;
    int len = xcb_get_property_value_length(rep);
    if (len > 0) {
        const char *val = (const char *)xcb_get_property_value(rep);
        size_t n1 = strnlen(val, (size_t)len);
        snprintf(inst, inst_sz, "%.*s", (int)n1, val);
        if ((int)(n1 + 1) < len) {
            size_t n2 = strnlen(val + n1 + 1, (size_t)len - (n1 + 1));
            snprintf(cls, cls_sz, "%.*s", (int)n2, val + n1 + 1);
        }
    }
    free(rep);
}

/* --- Front Panel TYPE client Swallowing & dtfile Pinned Desktop Objects --- */
static bool matches_swallow_clock(
    DtwmServer *server, struct wlr_xwayland_surface *xs, const char *inst, const char *cls
) {
    (void)xs;
    if (!server->show_front_panel || server->swallowed_clock_view != nullptr) {
        return false;
    }
    if (strcasecmp(inst, "ck-clock") == 0 || strcasecmp(inst, "fpclock") == 0 ||
        strcasecmp(inst, "dtclock") == 0) {
        return true;
    }
    if (server->swallow_client_name[0] != '\0') {
        if (strcasecmp(inst, server->swallow_client_name) == 0 ||
            strcasecmp(cls, server->swallow_client_name) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_dtfile_desktop_pin_surface(
    DtwmServer *server, struct wlr_xwayland_surface *xs, const char *inst, const char *cls
) {
    if (!xs || xs->override_redirect) return false;
    bool undecorated = (xs->decorations & WLR_XWAYLAND_SURFACE_DECORATIONS_NO_BORDER) != 0 &&
                       (xs->decorations & WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE) != 0;
    if (!undecorated && server->xcb_conn && server->atom_mwm_info != XCB_ATOM_NONE) {
        xcb_atom_t mwm_hints_a = intern_atom_icons(server->xcb_conn, "_MOTIF_WM_HINTS");
        xcb_get_property_cookie_t ck = xcb_get_property(
            server->xcb_conn, 0, xs->window_id, mwm_hints_a, mwm_hints_a, 0, 5
        );
        xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
        if (rep) {
            if (rep->format == 32 && xcb_get_property_value_length(rep) >= 12) {
                const uint32_t *v = (const uint32_t *)xcb_get_property_value(rep);
                if ((v[0] & 2u) && v[2] == 0u) undecorated = true;
            }
            free(rep);
        }
    }
    if (!undecorated) return false;
    if (strncmp(inst, "popup_name", 10) == 0) return true;
    if (strcasecmp(cls, "Dtfile") == 0 && strncmp(inst, "desktop", 7) == 0) return true;
    return false;
}

void dtwm_icons_reposition_swallowed(DtwmServer *server) {
    if (!server) return;
    /* Compute left-most Front Panel cell ("Clock") geometry matching cde_workspace.c */
    int well_w = 248;
    int well_x = (server->panel_w - well_w) / 2;
    int left_area_x = 24;
    int left_area_w = well_x - left_area_x - 38;
    int cell_w = left_area_w / 3;
    server->clock_slot_x = server->panel_x + left_area_x + 2 + 3;
    server->clock_slot_y = server->panel_y + 8 + 3;
    server->clock_slot_w = cell_w - 4 - 6;
    server->clock_slot_h = server->panel_h - 16 - 6;

    DtwmView *v = server->swallowed_clock_view;
    if (!v || !v->mapped || !v->xwayland_surface) return;
    if (!server->show_front_panel) {
        if (v->frame_tree) wlr_scene_node_set_enabled(&v->frame_tree->node, false);
        return;
    }
    v->x = server->clock_slot_x;
    v->y = server->clock_slot_y;
    v->client_w = server->clock_slot_w > 16 ? server->clock_slot_w : 48;
    v->client_h = server->clock_slot_h > 16 ? server->clock_slot_h : 48;
    if (v->frame_tree) {
        wlr_scene_node_set_position(&v->frame_tree->node, v->x, v->y);
        wlr_scene_node_set_enabled(&v->frame_tree->node, true);
    }
    wlr_xwayland_surface_configure(
        v->xwayland_surface,
        (int16_t)v->x,
        (int16_t)v->y,
        (uint16_t)v->client_w,
        (uint16_t)v->client_h
    );
}

bool dtwm_icons_classify_xwayland_map(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return false;
    DtwmServer *server = view->server;
    struct wlr_xwayland_surface *xs = view->xwayland_surface;

    char inst[128] = {0}, cls[128] = {0};
    read_xcb_wm_class_sync(server, xs, inst, sizeof(inst), cls, sizeof(cls));

    if (matches_swallow_clock(server, xs, inst, cls)) {
        view->is_swallowed = true;
        view->decorated = false;
        view->has_title = false;
        view->has_resize_handles = false;
        view->workspace_mask = 0xFFu;
        server->swallowed_clock_view = view;

        view->frame_tree = wlr_scene_tree_create(server->panel_swallow_tree);
        view->frame_tree->node.data = view;
        view->content_tree = wlr_scene_subsurface_tree_create(view->frame_tree, xs->surface);
        view->content_tree->node.data = view;

        dtwm_workspaces_on_xwayland_view_map(view);
        dtwm_icons_set_wm_state(view);
        dtwm_update_front_panel(server);
        return true;
    }

    if (is_dtfile_desktop_pin_surface(server, xs, inst, cls)) {
        view->is_desktop_pin = true;
        view->decorated = false;
        view->has_title = false;
        view->has_resize_handles = false;

        view->frame_tree = wlr_scene_tree_create(server->desktop_icon_tree);
        view->frame_tree->node.data = view;
        view->content_tree = wlr_scene_subsurface_tree_create(view->frame_tree, xs->surface);
        view->content_tree->node.data = view;

        view->x = xs->x;
        view->y = xs->y;
        wlr_scene_node_set_position(&view->frame_tree->node, view->x, view->y);
        dtwm_workspaces_on_xwayland_view_map(view);
        dtwm_icons_set_wm_state(view);
        wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
        wlr_xwayland_surface_configure(
            xs, (int16_t)view->x, (int16_t)view->y, (uint16_t)view->client_w, (uint16_t)view->client_h
        );
        return true;
    }

    return false;
}

void dtwm_icons_on_view_map(DtwmView *view) {
    if (!view || !view->server) return;
    if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel) {
        DtwmPendingXdgIcon *pend, *tmp;
        wl_list_for_each_safe(pend, tmp, &g_pending_xdg_icons, link) {
            if (pend->toplevel == view->xdg_toplevel) {
                if (view->icon_surface) cairo_surface_destroy(view->icon_surface);
                view->icon_surface = pend->surface;
                wl_list_remove(&pend->link);
                free(pend);
                break;
            }
        }
    } else if (view->kind == DTWM_VIEW_XWAYLAND) {
        refresh_xwayland_wm_hints(view, true);
        dtwm_icons_set_wm_state(view);
    }
    if (view->frame_tree) {
        wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
    }
    dtwm_icons_relayout(view->server);
}

void dtwm_icons_on_view_unmap_or_destroy(DtwmView *view, bool destroying) {
    if (!view || !view->server) return;
    DtwmServer *server = view->server;
    if (server->swallowed_clock_view == view) {
        server->swallowed_clock_view = nullptr;
        dtwm_update_front_panel(server);
    }
    if (view->desktop_icon_buf) {
        if (destroying) {
            wlr_scene_node_destroy(&view->desktop_icon_buf->node);
            view->desktop_icon_buf = nullptr;
        } else {
            wlr_scene_buffer_set_buffer(view->desktop_icon_buf, nullptr);
            wlr_scene_node_set_enabled(&view->desktop_icon_buf->node, false);
        }
    }
    if (destroying && view->icon_surface) {
        cairo_surface_destroy(view->icon_surface);
        view->icon_surface = nullptr;
    }
    dtwm_icons_relayout(server);
}

void dtwm_icons_on_xcb_property(DtwmServer *server, xcb_window_t win, xcb_atom_t atom) {
    if (!server || atom != XCB_ATOM_WM_HINTS) return;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface &&
            v->xwayland_surface->window_id == win) {
            refresh_xwayland_wm_hints(v, false);
            dtwm_icons_update_view(v);
            break;
        }
    }
}

void dtwm_icons_set_open_window_icons(DtwmServer *server, bool enabled) {
    if (!server || server->show_open_window_icons == enabled) return;
    server->show_open_window_icons = enabled;
    publish_icon_sizes_and_settings(server);
    dtwm_icons_relayout(server);
}

void dtwm_icons_set_large_icons(DtwmServer *server, bool large) {
    if (!server || server->large_icons == large) return;
    server->large_icons = large;
    publish_icon_sizes_and_settings(server);
    dtwm_icons_relayout(server);
}

void dtwm_icons_sync_xwayland_pointer(
    DtwmServer *server, double lx, double ly, struct wlr_surface *focused_surf
) {
    if (!server || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) return;
    if (focused_surf && wlr_xwayland_surface_try_from_wlr_surface(focused_surf) != nullptr) {
        /* XWayland updates its own pointer coordinates when focused on an X11 surface */
        return;
    }
    xcb_warp_pointer(
        server->xcb_conn, XCB_NONE, server->xcb_root, 0, 0, 0, 0, (int16_t)lx, (int16_t)ly
    );
    xcb_flush(server->xcb_conn);
}

void dtwm_icons_on_xwayland_ready(DtwmServer *server) {
    if (!server || !server->xcb_conn) return;
    server->atom_wm_settings_v1 = intern_atom_icons(server->xcb_conn, "_DT_WM_SETTINGS_V1");
    server->atom_wm_state = intern_atom_icons(server->xcb_conn, "WM_STATE");
    server->atom_net_wm_icon_geom = intern_atom_icons(server->xcb_conn, "_NET_WM_ICON_GEOMETRY");
    publish_icon_sizes_and_settings(server);
}

void dtwm_icons_init(DtwmServer *server) {
    wl_list_init(&g_pending_xdg_icons);
    server->show_open_window_icons = true;
    server->toplevel_icon_global = wl_global_create(
        server->wl_display,
        &xdg_toplevel_icon_manager_v1_interface,
        1,
        server,
        bind_wl_icon_manager
    );
}

void dtwm_icons_finish(DtwmServer *server) {
    DtwmPendingXdgIcon *pend, *tmp;
    wl_list_for_each_safe(pend, tmp, &g_pending_xdg_icons, link) {
        if (pend->surface) cairo_surface_destroy(pend->surface);
        wl_list_remove(&pend->link);
        free(pend);
    }
    if (server->toplevel_icon_global) {
        wl_global_destroy(server->toplevel_icon_global);
        server->toplevel_icon_global = nullptr;
    }
}
