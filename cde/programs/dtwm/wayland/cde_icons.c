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

#include <ctype.h>
#include <dirent.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_buffer.h>
#include <xcb/xcb.h>

/* X11 WM_HINTS bitflags & WM_STATE values */
#define DTWM_X11_STATE_HINT         (1u << 1)
#define DTWM_X11_ICON_PIXMAP_HINT   (1u << 2)
#define DTWM_X11_ICON_POSITION_HINT (1u << 3)
#define DTWM_X11_ICON_MASK_HINT     (1u << 5)
#define DTWM_X11_NORMAL_STATE       1u
#define DTWM_X11_ICONIC_STATE       3u

/* Open-window indicator bar metrics matching WmGlobal.h & WmIDecor.c */
#define DTWM_OPEN_ICON_BAR_WIDTH 10
#define DTWM_OPEN_ICON_BAR_GAP   7

/* _DT_WM_SETTINGS_V1 constants (matching <Dt/WmSettings.h>) */
#define DTWM_SETTINGS_V1_VERSION                1u
#define DTWM_SETTINGS_V1_SHOW_OPEN_WINDOW_ICONS (1u << 0)
#define DTWM_SETTINGS_V1_USE_ICON_BOX           (1u << 1)

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

static void read_xcb_wm_class_sync(
    DtwmServer *server, struct wlr_xwayland_surface *xs, char *inst, size_t inst_sz, char *cls, size_t cls_sz
);

/* --- CDE Named Icon, Dtwm.defs, .dt Action Database & _NET_WM_ICON Resolver --- */
typedef struct {
    char key[64];
    char icon[64];
} DtwmIconRule;

static DtwmIconRule g_custom_icon_rules[64];
static int g_custom_icon_rule_count = 0;

static DtwmIconRule g_action_icon_rules[256];
static int g_action_icon_rule_count = 0;
static bool g_action_icons_loaded = false;

void dtwm_icons_record_icon_image_rule(const char *client_key, const char *icon_name) {
    if (!client_key || !client_key[0] || !icon_name || !icon_name[0]) return;
    for (int i = 0; i < g_custom_icon_rule_count; ++i) {
        if (strcasecmp(g_custom_icon_rules[i].key, client_key) == 0) {
            snprintf(g_custom_icon_rules[i].icon, sizeof(g_custom_icon_rules[i].icon), "%s", icon_name);
            return;
        }
    }
    if (g_custom_icon_rule_count < (int)(sizeof(g_custom_icon_rules) / sizeof(g_custom_icon_rules[0]))) {
        snprintf(
            g_custom_icon_rules[g_custom_icon_rule_count].key,
            sizeof(g_custom_icon_rules[0].key),
            "%s",
            client_key
        );
        snprintf(
            g_custom_icon_rules[g_custom_icon_rule_count].icon,
            sizeof(g_custom_icon_rules[0].icon),
            "%s",
            icon_name
        );
        g_custom_icon_rule_count++;
    }
}

static const char *lookup_dtwm_defs_icon_image(const char *inst, const char *cls) {
    /* User / xrdb Dtwm*<Key>*iconImage overrides take top priority */
    for (int i = 0; i < g_custom_icon_rule_count; ++i) {
        if ((inst && inst[0] && strcasecmp(inst, g_custom_icon_rules[i].key) == 0) ||
            (cls && cls[0] && strcasecmp(cls, g_custom_icon_rules[i].key) == 0)) {
            return g_custom_icon_rules[i].icon;
        }
    }
    /* Canonical CDE Dtwm.defs.src client iconImage mappings */
    static const struct {
        const char *key;
        const char *icon;
    } defs[] = {
        {"FrontPanel", "Fpfpmin"},
        {"Dtmail", "DtMail"},
        {"Dtterm", "Fpterm"},
        {"Dtstyle", "Fpstyle"},
        {"Dthelpview", "Fphelp"},
        {"Dticon", "Dtpaint"},
        {"Dtksh", "Dtdtksh.m"},
        {"Dtcalc", "Dtcalc"},
        {"CkCalc", "Dtcalc"},
        {"ck-calc", "Dtcalc"},
        {"Dtaction", "Dtactn"},
        {"Dtpad", "Fppenpd"},
        {"Dtbuilder", "DtBldr"},
        {"Dtqinfo", "Fpprnt"},
        {"Printer", "Fpprnt"},
        {"Dtinfo", "Dtinfo"},
        {"Dtcm", "FpCM"},
        {"Dtcreate", "Dtcreate"},
    };
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); ++i) {
        if ((inst && inst[0] && strcasecmp(inst, defs[i].key) == 0) ||
            (cls && cls[0] && strcasecmp(cls, defs[i].key) == 0)) {
            return defs[i].icon;
        }
    }
    return nullptr;
}

static void record_action_cmd_icon(const char *cmd_raw, const char *icon_raw) {
    if (!cmd_raw || !cmd_raw[0] || !icon_raw || !icon_raw[0]) return;
    while (*cmd_raw && (isspace((unsigned char)*cmd_raw) || *cmd_raw == '"' || *cmd_raw == '\'')) {
        cmd_raw++;
    }
    char tok[128] = {0};
    size_t ti = 0;
    while (cmd_raw[ti] && !isspace((unsigned char)cmd_raw[ti]) && cmd_raw[ti] != '"' &&
           cmd_raw[ti] != '\'' && cmd_raw[ti] != ';' && ti + 1 < sizeof(tok)) {
        tok[ti] = cmd_raw[ti];
        ti++;
    }
    tok[ti] = '\0';
    const char *base = strrchr(tok, '/');
    base = base ? (base + 1) : tok;
    if (!base[0] || strcmp(base, "sh") == 0 || strcmp(base, "bash") == 0 ||
        strcmp(base, "env") == 0 || strcmp(base, "dtaction") == 0) {
        return;
    }

    while (*icon_raw && isspace((unsigned char)*icon_raw)) icon_raw++;
    char iname[64] = {0};
    size_t ii = 0;
    while (icon_raw[ii] && !isspace((unsigned char)icon_raw[ii]) && ii + 1 < sizeof(iname)) {
        iname[ii] = icon_raw[ii];
        ii++;
    }
    iname[ii] = '\0';
    if (!iname[0]) return;

    for (int i = 0; i < g_action_icon_rule_count; ++i) {
        if (strcasecmp(g_action_icon_rules[i].key, base) == 0) {
            return;
        }
    }
    if (g_action_icon_rule_count < (int)(sizeof(g_action_icon_rules) / sizeof(g_action_icon_rules[0]))) {
        snprintf(
            g_action_icon_rules[g_action_icon_rule_count].key,
            sizeof(g_action_icon_rules[0].key),
            "%s",
            base
        );
        snprintf(
            g_action_icon_rules[g_action_icon_rule_count].icon,
            sizeof(g_action_icon_rules[0].icon),
            "%s",
            iname
        );
        g_action_icon_rule_count++;
    }
}

static void parse_dt_action_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    bool in_action = false;
    int brace_depth = 0;
    char cur_icon[64] = {0};
    char cur_exec[256] = {0};
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '!' || *p == '\0') continue;
        if (!in_action) {
            if (strncmp(p, "ACTION", 6) == 0 && isspace((unsigned char)p[6])) {
                in_action = true;
                brace_depth = (strchr(p, '{') != nullptr) ? 1 : 0;
                cur_icon[0] = '\0';
                cur_exec[0] = '\0';
            }
            continue;
        }
        for (const char *c = p; *c; ++c) {
            if (*c == '{') brace_depth++;
            else if (*c == '}') brace_depth--;
        }
        if (strncmp(p, "ICON", 4) == 0 && isspace((unsigned char)p[4])) {
            const char *v = p + 4;
            while (*v && isspace((unsigned char)*v)) v++;
            snprintf(cur_icon, sizeof(cur_icon), "%s", v);
        } else if (strncmp(p, "EXEC_STRING", 11) == 0 && isspace((unsigned char)p[11])) {
            const char *v = p + 11;
            while (*v && isspace((unsigned char)*v)) v++;
            snprintf(cur_exec, sizeof(cur_exec), "%s", v);
        }
        if (brace_depth <= 0 && strchr(p, '}') != nullptr) {
            if (cur_exec[0] && cur_icon[0]) {
                record_action_cmd_icon(cur_exec, cur_icon);
            }
            in_action = false;
            brace_depth = 0;
        }
    }
    fclose(fp);
}

static void ensure_dt_action_icons_loaded(void) {
    if (g_action_icons_loaded) return;
    g_action_icons_loaded = true;

    const char *home = getenv("HOME");
    char user_dir[512] = {0};
    if (home && home[0]) {
        snprintf(user_dir, sizeof(user_dir), "%s/.dt/types", home);
    }
    const char *dirs[4] = {
        user_dir[0] ? user_dir : nullptr,
        "/etc/dt/appconfig/types/C",
#ifdef CDE_INSTALLATION_TOP
        CDE_INSTALLATION_TOP "/appconfig/types/C",
#else
        "/usr/dt/appconfig/types/C",
#endif
        "/usr/dt/appconfig/types/C",
    };
    for (size_t d = 0; d < 4; ++d) {
        if (!dirs[d] || !dirs[d][0]) continue;
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp)) != nullptr) {
            size_t nl = strlen(de->d_name);
            if (nl > 3 && strcmp(de->d_name + nl - 3, ".dt") == 0) {
                char fpath[1024];
                snprintf(fpath, sizeof(fpath), "%s/%s", dirs[d], de->d_name);
                parse_dt_action_file(fpath);
            }
        }
        closedir(dp);
    }
}

static const char *lookup_action_icon_for_token(const char *tok) {
    if (!tok || !tok[0]) return nullptr;
    ensure_dt_action_icons_loaded();
    const char *base = strrchr(tok, '/');
    base = base ? (base + 1) : tok;
    for (int i = 0; i < g_action_icon_rule_count; ++i) {
        if (strcasecmp(g_action_icon_rules[i].key, base) == 0) {
            return g_action_icon_rules[i].icon;
        }
    }
    return nullptr;
}

static uint32_t icon_rgba_to_argb32(DtwmRgba c) {
    uint32_t a = (uint32_t)fmin(255.0, fmax(0.0, round(c.a * 255.0)));
    uint32_t r = (uint32_t)fmin(255.0, fmax(0.0, round(c.r * 255.0)));
    uint32_t g = (uint32_t)fmin(255.0, fmax(0.0, round(c.g * 255.0)));
    uint32_t b = (uint32_t)fmin(255.0, fmax(0.0, round(c.b * 255.0)));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static bool parse_icon_hex_color(const char *s, DtwmRgba *out) {
    if (!s || s[0] != '#') return false;
    size_t len = strlen(s + 1);
    unsigned int r = 0, g = 0, b = 0;
    if (len == 6 && sscanf(s + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        *out = (DtwmRgba){r / 255.0, g / 255.0, b / 255.0, 1.0};
        return true;
    }
    if (len == 12 && sscanf(s + 1, "%04x%04x%04x", &r, &g, &b) == 3) {
        *out = (DtwmRgba){r / 65535.0, g / 65535.0, b / 65535.0, 1.0};
        return true;
    }
    if (len == 3 && sscanf(s + 1, "%01x%01x%01x", &r, &g, &b) == 3) {
        *out = (DtwmRgba){r / 15.0, g / 15.0, b / 15.0, 1.0};
        return true;
    }
    return false;
}

static uint32_t resolve_cde_icon_xpm_color(const char *spec, const DtwmColorSet *cs) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);

    char *save = nullptr;
    char *tok = strtok_r(buf, " \t", &save);
    const char *sym_val = nullptr;
    const char *col_val = nullptr;
    while (tok) {
        if (strcmp(tok, "s") == 0) {
            sym_val = strtok_r(nullptr, " \t", &save);
        } else if (strcmp(tok, "c") == 0) {
            col_val = strtok_r(nullptr, " \t", &save);
        } else if (strcmp(tok, "m") == 0 || strcmp(tok, "g") == 0 || strcmp(tok, "g4") == 0) {
            (void)strtok_r(nullptr, " \t", &save);
        }
        tok = strtok_r(nullptr, " \t", &save);
    }

    if ((sym_val && strcasecmp(sym_val, "none") == 0) ||
        (col_val && strcasecmp(col_val, "none") == 0)) {
        return 0x00000000u;
    }

    if (sym_val) {
        if (strcasecmp(sym_val, "background") == 0) return icon_rgba_to_argb32(cs->bg);
        if (strcasecmp(sym_val, "foreground") == 0) return icon_rgba_to_argb32(cs->fg);
        if (strcasecmp(sym_val, "topShadowColor") == 0) return icon_rgba_to_argb32(cs->top_shadow);
        if (strcasecmp(sym_val, "bottomShadowColor") == 0) return icon_rgba_to_argb32(cs->bottom_shadow);
        if (strcasecmp(sym_val, "selectColor") == 0) {
            return icon_rgba_to_argb32((DtwmRgba){cs->bg.r * 0.85, cs->bg.g * 0.85, cs->bg.b * 0.85, 1.0});
        }
        if (strcasecmp(sym_val, "iconColor1") == 0) return 0xFF000000u;
        if (strcasecmp(sym_val, "iconColor2") == 0) return 0xFFFFFFFFu;
        if (strcasecmp(sym_val, "iconColor3") == 0) return 0xFFFF0000u;
        if (strcasecmp(sym_val, "iconColor4") == 0) return 0xFF00FF00u;
        if (strcasecmp(sym_val, "iconColor5") == 0) return 0xFF0000FFu;
        if (strcasecmp(sym_val, "iconColor6") == 0) return 0xFFFFFF00u;
        if (strcasecmp(sym_val, "iconColor7") == 0) return 0xFF00FFFFu;
        if (strcasecmp(sym_val, "iconColor8") == 0) return 0xFFFF00FFu;
        if (strcasecmp(sym_val, "iconGray1") == 0) return 0xFFDEDEDEu;
        if (strcasecmp(sym_val, "iconGray2") == 0) return 0xFFBDBDBDu;
        if (strcasecmp(sym_val, "iconGray3") == 0) return 0xFFADADADu;
        if (strcasecmp(sym_val, "iconGray4") == 0) return 0xFF949494u;
        if (strcasecmp(sym_val, "iconGray5") == 0) return 0xFF737373u;
        if (strcasecmp(sym_val, "iconGray6") == 0) return 0xFF636363u;
        if (strcasecmp(sym_val, "iconGray7") == 0) return 0xFF424242u;
        if (strcasecmp(sym_val, "iconGray8") == 0) return 0xFF212121u;
    }

    if (col_val) {
        DtwmRgba parsed;
        if (parse_icon_hex_color(col_val, &parsed)) {
            return icon_rgba_to_argb32(parsed);
        }
        if (strcasecmp(col_val, "black") == 0) return 0xFF000000u;
        if (strcasecmp(col_val, "white") == 0) return 0xFFFFFFFFu;
        if (strcasecmp(col_val, "red") == 0) return 0xFFFF0000u;
        if (strcasecmp(col_val, "green") == 0) return 0xFF00FF00u;
        if (strcasecmp(col_val, "blue") == 0) return 0xFF0000FFu;
        if (strcasecmp(col_val, "yellow") == 0) return 0xFFFFFF00u;
        if (strcasecmp(col_val, "cyan") == 0) return 0xFF00FFFFu;
        if (strcasecmp(col_val, "magenta") == 0) return 0xFFFF00FFu;
        if (strcasecmp(col_val, "gray") == 0 || strcasecmp(col_val, "grey") == 0) return 0xFF808080u;
    }
    return icon_rgba_to_argb32(cs->fg);
}

static cairo_surface_t *load_cde_xpm_icon_file(const char *path, const DtwmColorSet *cs) {
    FILE *fp = fopen(path, "r");
    if (!fp) return nullptr;

    int width = 0, height = 0, ncolors = 0, cpp = 0;
    bool got_header = false;
    typedef struct {
        char key[8];
        uint32_t argb;
    } XpmEntry;
    XpmEntry *cmap = nullptr;
    int cmap_read = 0;

    cairo_surface_t *surf = nullptr;
    uint32_t *pixels = nullptr;
    int stride_u32 = 0;
    int row = 0;

    char line[4096];
    bool in_comment = false;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        if (in_comment) {
            char *endc = strstr(p, "*/");
            if (!endc) continue;
            in_comment = false;
            p = endc + 2;
        }
        while (*p && *p != '"') {
            if (p[0] == '/' && p[1] == '*') {
                char *endc = strstr(p + 2, "*/");
                if (!endc) {
                    in_comment = true;
                    break;
                }
                p = endc + 2;
                continue;
            }
            p++;
        }
        if (in_comment || *p != '"') continue;

        char *start = p + 1;
        char *end = strrchr(start, '"');
        if (!end) continue;
        *end = '\0';

        if (!got_header) {
            if (sscanf(start, "%d %d %d %d", &width, &height, &ncolors, &cpp) == 4 &&
                width > 0 && width <= 512 && height > 0 && height <= 512 &&
                ncolors > 0 && ncolors <= 1024 && cpp >= 1 && cpp <= 4) {
                got_header = true;
                cmap = calloc((size_t)ncolors, sizeof(*cmap));
                surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
                if (!cmap || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
                    free(cmap);
                    if (surf) cairo_surface_destroy(surf);
                    fclose(fp);
                    return nullptr;
                }
                pixels = (uint32_t *)cairo_image_surface_get_data(surf);
                stride_u32 = cairo_image_surface_get_stride(surf) / 4;
            }
            continue;
        }

        if (cmap_read < ncolors) {
            if ((int)strlen(start) >= cpp) {
                memcpy(cmap[cmap_read].key, start, (size_t)cpp);
                cmap[cmap_read].key[cpp] = '\0';
                cmap[cmap_read].argb = resolve_cde_icon_xpm_color(start + cpp, cs);
                cmap_read++;
            }
            continue;
        }

        if (row < height) {
            size_t slen = strlen(start);
            for (int x = 0; x < width; ++x) {
                uint32_t pix = 0x00000000u;
                if ((size_t)((x + 1) * cpp) <= slen) {
                    const char *k = start + x * cpp;
                    for (int ci = 0; ci < cmap_read; ++ci) {
                        if (memcmp(cmap[ci].key, k, (size_t)cpp) == 0) {
                            pix = cmap[ci].argb;
                            break;
                        }
                    }
                }
                pixels[row * stride_u32 + x] = pix;
            }
            row++;
            if (row >= height) break;
        }
    }

    fclose(fp);
    free(cmap);
    if (!surf || row == 0) {
        if (surf) cairo_surface_destroy(surf);
        return nullptr;
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

static bool load_xbm_bits_file(const char *path, int *out_w, int *out_h, uint8_t **out_bits) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    int width = 0, height = 0;
    bool in_bits = false;
    uint8_t *bits = nullptr;
    size_t total_bytes = 0, byte_idx = 0;
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        if (!in_bits) {
            int val = 0;
            if (strstr(line, "_width") && sscanf(line, "#define %*s %d", &val) == 1) {
                width = val;
            } else if (strstr(line, "_height") && sscanf(line, "#define %*s %d", &val) == 1) {
                height = val;
            } else if (strchr(line, '{') && width > 0 && width <= 512 && height > 0 && height <= 512) {
                int row_bytes = (width + 7) / 8;
                total_bytes = (size_t)row_bytes * (size_t)height;
                bits = calloc(total_bytes, 1);
                if (!bits) {
                    fclose(fp);
                    return false;
                }
                in_bits = true;
            }
        }
        if (in_bits) {
            const char *p = strchr(line, '{') ? (strchr(line, '{') + 1) : line;
            while (*p && byte_idx < total_bytes) {
                while (*p && *p != '0' && !isdigit((unsigned char)*p) && *p != '-') p++;
                if (!*p) break;
                char *endp = nullptr;
                long v = strtol(p, &endp, 0);
                if (endp == p) {
                    p++;
                    continue;
                }
                bits[byte_idx++] = (uint8_t)(v & 0xFF);
                p = endp;
            }
            if (byte_idx >= total_bytes) break;
        }
    }
    fclose(fp);
    if (!bits || byte_idx == 0) {
        free(bits);
        return false;
    }
    *out_w = width;
    *out_h = height;
    *out_bits = bits;
    return true;
}

static cairo_surface_t *load_cde_xbm_icon_file(
    const char *bm_path, const char *mask_path, const DtwmColorSet *cs
) {
    int w = 0, h = 0, mw = 0, mh = 0;
    uint8_t *bits = nullptr, *mbits = nullptr;
    if (!load_xbm_bits_file(bm_path, &w, &h, &bits)) return nullptr;
    if (mask_path && mask_path[0]) {
        (void)load_xbm_bits_file(mask_path, &mw, &mh, &mbits);
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        free(bits);
        free(mbits);
        cairo_surface_destroy(surf);
        return nullptr;
    }
    uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(surf);
    int stride_u32 = cairo_image_surface_get_stride(surf) / 4;
    int rbytes = (w + 7) / 8;
    int mrbytes = (mw + 7) / 8;
    uint32_t fg_argb = icon_rgba_to_argb32(cs->fg);
    uint32_t bg_argb = icon_rgba_to_argb32(cs->bg);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            bool on = ((bits[y * rbytes + (x / 8)] >> (x % 8)) & 1u) != 0;
            bool vis = true;
            if (mbits && x < mw && y < mh) {
                vis = ((mbits[y * mrbytes + (x / 8)] >> (x % 8)) & 1u) != 0;
            }
            dst[y * stride_u32 + x] = vis ? (on ? fg_argb : bg_argb) : 0x00000000u;
        }
    }
    free(bits);
    free(mbits);
    cairo_surface_mark_dirty(surf);
    return surf;
}

static cairo_surface_t *load_named_icon_surface(DtwmServer *server, const char *icon_name) {
    if (!server || !icon_name || !icon_name[0]) return nullptr;
    const DtwmColorSet *cs = &server->palette.inactive;

    /* Direct path */
    if (strchr(icon_name, '/')) {
        size_t pl = strlen(icon_name);
        if (pl > 4 && strcasecmp(icon_name + pl - 4, ".png") == 0) {
            cairo_surface_t *ps = cairo_image_surface_create_from_png(icon_name);
            if (ps && cairo_surface_status(ps) == CAIRO_STATUS_SUCCESS) return ps;
            if (ps) cairo_surface_destroy(ps);
        }
        cairo_surface_t *xs = load_cde_xpm_icon_file(icon_name, cs);
        if (xs) return xs;
        return load_cde_xbm_icon_file(icon_name, nullptr, cs);
    }

    /* Strip trailing .pm / .bm if included in resource value */
    char stem[128];
    snprintf(stem, sizeof(stem), "%s", icon_name);
    size_t sl = strlen(stem);
    if (sl > 3 && (strcasecmp(stem + sl - 3, ".pm") == 0 || strcasecmp(stem + sl - 3, ".bm") == 0)) {
        stem[sl - 3] = '\0';
    }

    const char *home = getenv("HOME");
    char user_icons[512] = {0};
    if (home && home[0]) {
        snprintf(user_icons, sizeof(user_icons), "%s/.dt/icons", home);
    }
    const char *cde_dirs[5] = {
        user_icons[0] ? user_icons : nullptr,
        "/etc/dt/appconfig/icons/C",
#ifdef CDE_INSTALLATION_TOP
        CDE_INSTALLATION_TOP "/appconfig/icons/C",
#else
        "/usr/dt/appconfig/icons/C",
#endif
        "/usr/dt/appconfig/icons/C",
        "cde/programs/icons",
    };

    static const char *const pm_exts[] = {".l.pm", ".pm", ".m.pm", ".s.pm", ".t.pm"};
    for (size_t d = 0; d < 5; ++d) {
        if (!cde_dirs[d] || !cde_dirs[d][0]) continue;
        for (size_t e = 0; e < sizeof(pm_exts) / sizeof(pm_exts[0]); ++e) {
            char cand[1024];
            snprintf(cand, sizeof(cand), "%s/%s%s", cde_dirs[d], stem, pm_exts[e]);
            if (access(cand, R_OK) == 0) {
                cairo_surface_t *s = load_cde_xpm_icon_file(cand, cs);
                if (s) return s;
            }
        }
        static const struct {
            const char *bm;
            const char *mask;
        } bm_exts[] = {
            {".l.bm", ".l_m.bm"},
            {".bm", "_m.bm"},
            {".m.bm", ".m_m.bm"},
        };
        for (size_t e = 0; e < sizeof(bm_exts) / sizeof(bm_exts[0]); ++e) {
            char cand[1024], mcand[1024];
            snprintf(cand, sizeof(cand), "%s/%s%s", cde_dirs[d], stem, bm_exts[e].bm);
            snprintf(mcand, sizeof(mcand), "%s/%s%s", cde_dirs[d], stem, bm_exts[e].mask);
            if (access(cand, R_OK) == 0) {
                cairo_surface_t *s = load_cde_xbm_icon_file(
                    cand, access(mcand, R_OK) == 0 ? mcand : nullptr, cs
                );
                if (s) return s;
            }
        }
    }

    /* Freedesktop hicolor / pixmaps fallback for modern X11 & native Wayland apps */
    const char *png_dirs[] = {
        "/usr/share/icons/hicolor/48x48/apps",
        "/usr/share/icons/hicolor/64x64/apps",
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/icons/hicolor/32x32/apps",
        "/usr/share/pixmaps",
    };
    for (size_t d = 0; d < sizeof(png_dirs) / sizeof(png_dirs[0]); ++d) {
        char cand[1024];
        snprintf(cand, sizeof(cand), "%s/%s.png", png_dirs[d], stem);
        if (access(cand, R_OK) == 0) {
            cairo_surface_t *ps = cairo_image_surface_create_from_png(cand);
            if (ps && cairo_surface_status(ps) == CAIRO_STATUS_SUCCESS) return ps;
            if (ps) cairo_surface_destroy(ps);
        }
        snprintf(cand, sizeof(cand), "%s/%s.xpm", png_dirs[d], stem);
        if (access(cand, R_OK) == 0) {
            cairo_surface_t *xs = load_cde_xpm_icon_file(cand, cs);
            if (xs) return xs;
        }
    }
    return nullptr;
}

static cairo_surface_t *fetch_x11_net_wm_icon(DtwmServer *server, xcb_window_t win) {
    if (!server->xcb_conn || server->atom_net_wm_icon == XCB_ATOM_NONE) return nullptr;
    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn, 0, win, server->atom_net_wm_icon, XCB_ATOM_CARDINAL, 0, 65536
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return nullptr;

    cairo_surface_t *best_surf = nullptr;
    if (rep->format == 32) {
        int nwords = xcb_get_property_value_length(rep) / 4;
        const uint32_t *data = (const uint32_t *)xcb_get_property_value(rep);
        int target = server->large_icons ? 96 : 48;
        int best_off = -1, best_w = 0, best_h = 0, best_diff = 100000;
        int idx = 0;
        while (idx + 2 <= nwords) {
            uint32_t iw = data[idx];
            uint32_t ih = data[idx + 1];
            if (iw == 0 || ih == 0 || iw > 512 || ih > 512) break;
            int pixels = (int)(iw * ih);
            if (idx + 2 + pixels > nwords) break;
            int diff = abs((int)iw - target);
            if (diff < best_diff) {
                best_diff = diff;
                best_off = idx + 2;
                best_w = (int)iw;
                best_h = (int)ih;
            }
            idx += 2 + pixels;
        }
        if (best_off >= 0 && best_w > 0 && best_h > 0) {
            best_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_w, best_h);
            if (cairo_surface_status(best_surf) == CAIRO_STATUS_SUCCESS) {
                uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(best_surf);
                int stride_u32 = cairo_image_surface_get_stride(best_surf) / 4;
                const uint32_t *src = data + best_off;
                for (int y = 0; y < best_h; ++y) {
                    for (int x = 0; x < best_w; ++x) {
                        uint32_t px = src[y * best_w + x];
                        uint32_t a = (px >> 24) & 0xFFu;
                        uint32_t r = (((px >> 16) & 0xFFu) * a + 127u) / 255u;
                        uint32_t g = (((px >> 8) & 0xFFu) * a + 127u) / 255u;
                        uint32_t b = ((px & 0xFFu) * a + 127u) / 255u;
                        dst[y * stride_u32 + x] = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                }
                cairo_surface_mark_dirty(best_surf);
            } else {
                cairo_surface_destroy(best_surf);
                best_surf = nullptr;
            }
        }
    }
    free(rep);
    return best_surf;
}

static void refresh_xwayland_icon_title(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return;
    DtwmServer *server = view->server;
    if (!server->xcb_conn) return;
    xcb_window_t win = view->xwayland_surface->window_id;

    view->icon_title[0] = '\0';

    /* 1. Prefer UTF-8 _NET_WM_ICON_NAME */
    if (server->atom_net_wm_icon_name != XCB_ATOM_NONE) {
        xcb_get_property_cookie_t ck = xcb_get_property(
            server->xcb_conn, 0, win, server->atom_net_wm_icon_name, XCB_GET_PROPERTY_TYPE_ANY, 0, 128
        );
        xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
        if (rep) {
            int len = xcb_get_property_value_length(rep);
            if (rep->format == 8 && len > 0) {
                const char *val = (const char *)xcb_get_property_value(rep);
                size_t n = strnlen(val, (size_t)len);
                if (n > 0) {
                    snprintf(view->icon_title, sizeof(view->icon_title), "%.*s", (int)n, val);
                }
            }
            free(rep);
        }
    }

    /* 2. Fall back to ICCCM WM_ICON_NAME */
    if (!view->icon_title[0]) {
        xcb_get_property_cookie_t ck = xcb_get_property(
            server->xcb_conn, 0, win, XCB_ATOM_WM_ICON_NAME, XCB_GET_PROPERTY_TYPE_ANY, 0, 128
        );
        xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
        if (rep) {
            int len = xcb_get_property_value_length(rep);
            if (rep->format == 8 && len > 0) {
                const char *val = (const char *)xcb_get_property_value(rep);
                size_t n = strnlen(val, (size_t)len);
                if (n > 0) {
                    snprintf(view->icon_title, sizeof(view->icon_title), "%.*s", (int)n, val);
                }
            }
            free(rep);
        }
    }
}

static const char *lookup_xwayland_proc_action_icon(DtwmServer *server, xcb_window_t win) {
    if (!server->xcb_conn) return nullptr;

    /* 1. Try WM_COMMAND */
    if (server->atom_wm_command != XCB_ATOM_NONE) {
        xcb_get_property_cookie_t ck = xcb_get_property(
            server->xcb_conn, 0, win, server->atom_wm_command, XCB_GET_PROPERTY_TYPE_ANY, 0, 128
        );
        xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
        if (rep) {
            int len = xcb_get_property_value_length(rep);
            if (rep->format == 8 && len > 0) {
                const char *val = (const char *)xcb_get_property_value(rep);
                size_t n = strnlen(val, (size_t)len);
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "%.*s", (int)n, val);
                const char *ic = lookup_action_icon_for_token(cmd);
                if (ic) {
                    free(rep);
                    return ic;
                }
            }
            free(rep);
        }
    }

    /* 2. Try _NET_WM_PID -> /proc/<pid>/cmdline */
    if (server->atom_net_wm_pid != XCB_ATOM_NONE) {
        xcb_get_property_cookie_t ck = xcb_get_property(
            server->xcb_conn, 0, win, server->atom_net_wm_pid, XCB_ATOM_CARDINAL, 0, 1
        );
        xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
        if (rep) {
            uint32_t pid = 0;
            if (rep->format == 32 && xcb_get_property_value_length(rep) >= 4) {
                pid = *(const uint32_t *)xcb_get_property_value(rep);
            }
            free(rep);
            if (pid > 1) {
                char ppath[64], cmdline[512];
                snprintf(ppath, sizeof(ppath), "/proc/%u/cmdline", pid);
                int fd = open(ppath, O_RDONLY);
                if (fd >= 0) {
                    ssize_t nr = read(fd, cmdline, sizeof(cmdline) - 1);
                    close(fd);
                    if (nr > 0) {
                        cmdline[nr] = '\0';
                        const char *ic = lookup_action_icon_for_token(cmdline);
                        if (ic) return ic;
                    }
                }
            }
        }
    }
    return nullptr;
}
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
        uint32_t flags = 0u;
        if (server->show_open_window_icons) {
            flags |= DTWM_SETTINGS_V1_SHOW_OPEN_WINDOW_ICONS;
        }
        if (server->use_icon_box) {
            flags |= DTWM_SETTINGS_V1_USE_ICON_BOX;
        }
        uint32_t settings[2] = {DTWM_SETTINGS_V1_VERSION, flags};
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
        uint32_t fg_1bit = icon_rgba_to_argb32(server->palette.inactive.fg);
        uint32_t bg_1bit = icon_rgba_to_argb32(server->palette.inactive.bg);
        if (stride_bytes * h <= data_len) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    bool set = extract_1bit_xy_bit(bits, stride_bytes, bitmap_msb, x, y);
                    dst[y * dst_stride_u32 + x] = set ? fg_1bit : bg_1bit;
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

static void resolve_xdg_view_named_icon_fallback(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XDG || !view->xdg_toplevel || view->icon_surface) return;
    const char *app_id = view->xdg_toplevel->app_id;
    if (!app_id || !app_id[0]) return;

    const char *dot = strrchr(app_id, '.');
    const char *tail = dot ? (dot + 1) : app_id;

    const char *ic = lookup_dtwm_defs_icon_image(app_id, tail);
    if (!ic) ic = lookup_action_icon_for_token(app_id);
    if (!ic) ic = lookup_action_icon_for_token(tail);

    cairo_surface_t *s = ic ? load_named_icon_surface(view->server, ic) : nullptr;
    if (!s) s = load_named_icon_surface(view->server, app_id);
    if (!s && tail != app_id) s = load_named_icon_surface(view->server, tail);
    if (s) {
        view->icon_surface = s;
        view->icon_from_named_image = true;
    }
}

static void refresh_xwayland_wm_hints(DtwmView *view, bool check_initial_iconic) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return;
    DtwmServer *server = view->server;
    if (!server->xcb_conn) return;

    char inst[128] = {0}, cls[128] = {0};
    read_xcb_wm_class_sync(server, view->xwayland_surface, inst, sizeof(inst), cls, sizeof(cls));
    const char *defs_icon = lookup_dtwm_defs_icon_image(
        view->is_panel ? "FrontPanel" : inst,
        view->is_panel ? "FrontPanel" : cls
    );

    xcb_pixmap_t icon_pix = XCB_NONE;
    xcb_pixmap_t icon_mask = XCB_NONE;

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
    if (rep) {
        int len = xcb_get_property_value_length(rep);
        if (rep->format == 32 && len >= 16) {
            const uint32_t *vals = (const uint32_t *)xcb_get_property_value(rep);
            int nwords = len / 4;
            uint32_t flags = vals[0];
            uint32_t initial_state = (nwords > 2) ? vals[2] : 1u;
            icon_pix = ((flags & DTWM_X11_ICON_PIXMAP_HINT) && nwords > 3) ? vals[3] : XCB_NONE;
            icon_mask = ((flags & DTWM_X11_ICON_MASK_HINT) && nwords > 7) ? vals[7] : XCB_NONE;

            if ((flags & DTWM_X11_ICON_POSITION_HINT) && nwords > 6) {
                view->has_icon_pos_hint = true;
                view->icon_hint_x = (int32_t)vals[5];
                view->icon_hint_y = (int32_t)vals[6];
            }

            if (check_initial_iconic && (flags & DTWM_X11_STATE_HINT) &&
                initial_state == DTWM_X11_ICONIC_STATE && !view->is_swallowed && !view->is_desktop_pin) {
                view->minimized = true;
                wlr_xwayland_surface_set_minimized(view->xwayland_surface, true);
            }
        }
        free(rep);
    }

    /* Precedence matching X11 dtwm WmWinInfo.c (useClientIcon defaults to False):
     * 1. Explicit Dtwm.defs / xrdb Dtwm*<Class>*iconImage (e.g. FrontPanel->Fpfpmin, Dtterm->Fpterm)
     * 2. Client WM_HINTS.icon_pixmap (+ icon_mask) or _NET_WM_ICON (e.g. dtfile dynamic folder icon)
     * 3. CDE .dt ACTION ICON / class / pixmap lookup (e.g. ck-calc->Dtcalc, Fphome, etc.)
     */
    cairo_surface_t *new_surf = nullptr;
    bool from_named = false;
    if (defs_icon) {
        new_surf = load_named_icon_surface(server, defs_icon);
        if (new_surf) from_named = true;
    }
    if (!new_surf && icon_pix != XCB_NONE) {
        new_surf = fetch_x11_icon_pixmap(server, icon_pix, icon_mask);
    }
    if (!new_surf) {
        new_surf = fetch_x11_net_wm_icon(server, view->xwayland_surface->window_id);
    }
    if (!new_surf) {
        const char *act_ic = lookup_xwayland_proc_action_icon(server, view->xwayland_surface->window_id);
        if (!act_ic && inst[0]) act_ic = lookup_action_icon_for_token(inst);
        if (!act_ic && cls[0]) act_ic = lookup_action_icon_for_token(cls);
        if (act_ic) {
            new_surf = load_named_icon_surface(server, act_ic);
            if (new_surf) from_named = true;
        }
    }
    if (!new_surf && cls[0] && strcasecmp(cls, "Dtfile") == 0) {
        new_surf = load_named_icon_surface(server, "Fphome");
        if (new_surf) from_named = true;
    }
    if (!new_surf && inst[0]) {
        new_surf = load_named_icon_surface(server, inst);
        if (new_surf) from_named = true;
    }
    if (!new_surf && cls[0]) {
        new_surf = load_named_icon_surface(server, cls);
        if (new_surf) from_named = true;
    }

    if (new_surf) {
        if (view->icon_surface) {
            cairo_surface_destroy(view->icon_surface);
        }
        view->icon_surface = new_surf;
        view->icon_from_named_image = from_named;
    }
}

/* --- Desktop Icon Classification, Layout & Rendering --- */
static bool should_show_desktop_icon(const DtwmView *view) {
    if (!view || !view->mapped) return false;
    if (view->server->greeter_mode) return false;
    if (view->is_desktop_pin || view->is_swallowed || view->is_subpanel || view->is_lock_dialog) return false;
    if (view->is_panel) {
        return view->minimized || (view->server->show_open_window_icons && !view->server->use_icon_box);
    }
    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface &&
        view->xwayland_surface->override_redirect) {
        return false;
    }
    if ((view->workspace_mask & (1u << view->server->current_workspace)) == 0) {
        return false;
    }
    if (!view->minimized) {
        if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface &&
            view->xwayland_surface->parent != nullptr) {
            return false;
        }
        if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel &&
            view->xdg_toplevel->parent != nullptr) {
            return false;
        }
    }
    return view->minimized || view->server->use_icon_box || view->server->show_open_window_icons;
}

static bool view_or_transient_has_focus(const DtwmView *view) {
    if (!view || !view->server || !view->server->focused_view) return false;
    const DtwmView *fv = view->server->focused_view;
    if (fv == view) return true;
    if (view->kind == DTWM_VIEW_XWAYLAND && fv->kind == DTWM_VIEW_XWAYLAND &&
        view->xwayland_surface && fv->xwayland_surface) {
        const struct wlr_xwayland_surface *cur = fv->xwayland_surface->parent;
        for (int depth = 0; cur && depth < 8; ++depth, cur = cur->parent) {
            if (cur == view->xwayland_surface) return true;
        }
    } else if (view->kind == DTWM_VIEW_XDG && fv->kind == DTWM_VIEW_XDG &&
               view->xdg_toplevel && fv->xdg_toplevel) {
        const struct wlr_xdg_toplevel *cur = fv->xdg_toplevel->parent;
        for (int depth = 0; cur && depth < 8; ++depth, cur = cur->parent) {
            if (cur == view->xdg_toplevel) return true;
        }
    }
    return false;
}

static void draw_asym_bevel(
    cairo_t *cr,
    double x,
    double y,
    double w,
    double h,
    double top_t,
    double right_t,
    double bot_t,
    double left_t,
    DtwmRgba tl_col,
    DtwmRgba br_col
) {
    if (w <= 0.0 || h <= 0.0) return;
    cairo_set_source_rgba(cr, tl_col.r, tl_col.g, tl_col.b, tl_col.a);
    cairo_new_path(cr);
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + w, y);
    cairo_line_to(cr, x + w - right_t, y + top_t);
    cairo_line_to(cr, x + left_t, y + top_t);
    cairo_line_to(cr, x + left_t, y + h - bot_t);
    cairo_line_to(cr, x, y + h);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, br_col.r, br_col.g, br_col.b, br_col.a);
    cairo_new_path(cr);
    cairo_move_to(cr, x + w, y);
    cairo_line_to(cr, x + w, y + h);
    cairo_line_to(cr, x, y + h);
    cairo_line_to(cr, x + left_t, y + h - bot_t);
    cairo_line_to(cr, x + w - right_t, y + h - bot_t);
    cairo_line_to(cr, x + w - right_t, y + top_t);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_cde_icon_title_box(
    cairo_t *cr,
    const DtwmServer *server,
    const char *text,
    double x,
    double y,
    double w,
    double h,
    DtwmRgba fg
) {
    if (!text || !text[0] || w <= 2.0 || h <= 2.0) return;
    const char *fspec = (server->palette.profile == DTWM_PROFILE_CLASSIC) ? "Serif 9" : "Sans 9";
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(fspec);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text, -1);

    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    /* Matches WmIDecor.c DrawIconTitle: center if text fits, else left-align & clip cleanly */
    double tx = (tw >= (int)w) ? x : (x + floor((w - tw) * 0.5));
    double ty = y + floor((h - th) * 0.5);

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, fg.r, fg.g, fg.b, fg.a);
    cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
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

struct wlr_buffer *dtwm_icons_render_tile_buffer(DtwmView *view, int *out_w, int *out_h) {
    if (!view || !view->server) return nullptr;
    DtwmServer *server = view->server;

    /* Exact X11 dtwm WmIDecor.c GetIconDimensions (WmRECESSED) metrics:
     * frame_w = img_dim + 2*pad(2) + 2*ext_shadow(2) + 4*int_shadow(1) = img_dim + 12 (60 for 48px)
     * img_frame_h = img_dim + 11 (59 for 48px), label_frame_h = 21 -> frame_h = 80
     * Open-window indicator bar sits 7px (OPEN_ICON_BAR_GAP) to the right, 10px wide x 3/8*frame_h tall.
     */
    int img_dim = server->large_icons ? 96 : 48;
    int frame_w = img_dim + 12;
    int img_frame_h = img_dim + 11;
    int label_frame_h = server->large_icons ? 24 : 21;
    int frame_h = img_frame_h + label_frame_h;

    bool in_icon_box = server->use_icon_box && !view->is_panel;
    bool show_bar = !view->minimized && server->show_open_window_icons && !in_icon_box;
    int bar_gap = DTWM_OPEN_ICON_BAR_GAP;
    int bar_w = DTWM_OPEN_ICON_BAR_WIDTH;
    int bar_h = (frame_h * 3) / 8;
    int bar_x = frame_w + bar_gap;
    int bar_y = (frame_h - bar_h) / 2;

    int total_w = show_bar ? (frame_w + bar_gap + bar_w) : frame_w;
    int total_h = frame_h;

    view->icon_box_w = total_w;
    view->icon_box_h = total_h;
    view->icon_w = frame_w;
    view->icon_h = frame_h;
    if (out_w) *out_w = total_w;
    if (out_h) *out_h = total_h;

    cairo_t *cr = nullptr;
    cairo_surface_t *surf = nullptr;
    struct wlr_buffer *buf = dtwm_cairo_buffer_begin(total_w, total_h, &cr, &surf);
    if (!buf) return nullptr;

    /* Clear full buffer so the 7px gap between icon frame and open-window indicator bar is transparent */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    /* WmIDecor.c:927: only focused minimized icons use active bevels; open-window icons use inactive
     * appearance with sunken (pressed) bevels + darker select_color fill, while the separate 10px
     * indicator bar to the right reflects active vs inactive window focus! */
    const DtwmColorSet *ics =
        (view->minimized && view->active) ? &server->palette.active : &server->palette.inactive;

    if (view->minimized) {
        cairo_set_source_rgba(cr, ics->bg.r, ics->bg.g, ics->bg.b, 1.0);
        cairo_rectangle(cr, 0, 0, frame_w, frame_h);
        cairo_fill(cr);

        draw_asym_bevel(cr, 0, 0, frame_w, img_frame_h, 2, 2, 1, 2, ics->top_shadow, ics->bottom_shadow);
        draw_asym_bevel(cr, 4, 4, img_dim + 4, img_dim + 4, 1, 1, 1, 1, ics->bottom_shadow, ics->top_shadow);
        draw_asym_bevel(cr, 5, 5, img_dim + 2, img_dim + 2, 1, 1, 1, 1, ics->top_shadow, ics->bottom_shadow);
        draw_asym_bevel(cr, 0, img_frame_h, frame_w, label_frame_h, 1, 2, 2, 2, ics->top_shadow, ics->bottom_shadow);
    } else {
        DtwmRgba open_fill = {ics->bg.r * 0.85, ics->bg.g * 0.85, ics->bg.b * 0.85, 1.0};
        cairo_set_source_rgba(cr, open_fill.r, open_fill.g, open_fill.b, 1.0);
        cairo_rectangle(cr, 0, 0, frame_w, frame_h);
        cairo_fill(cr);

        draw_asym_bevel(cr, 0, 0, frame_w, img_frame_h, 2, 2, 1, 2, ics->bottom_shadow, ics->top_shadow);
        draw_asym_bevel(cr, 4, 4, img_dim + 4, img_dim + 4, 1, 1, 1, 1, ics->top_shadow, ics->bottom_shadow);
        draw_asym_bevel(cr, 5, 5, img_dim + 2, img_dim + 2, 1, 1, 1, 1, ics->bottom_shadow, ics->top_shadow);
        draw_asym_bevel(cr, 0, img_frame_h, frame_w, label_frame_h, 1, 2, 2, 2, ics->bottom_shadow, ics->top_shadow);
    }

    /* Composite icon image centered inside (6, 6, img_dim, img_dim) */
    if (view->icon_surface) {
        int sw = cairo_image_surface_get_width(view->icon_surface);
        int sh = cairo_image_surface_get_height(view->icon_surface);
        if (sw > 0 && sh > 0) {
            double scale = 1.0;
            if (sw > img_dim || sh > img_dim) {
                scale = fmin((double)img_dim / sw, (double)img_dim / sh);
            } else if (server->large_icons && sw * 2 <= img_dim && sh * 2 <= img_dim) {
                scale = 2.0;
            }
            double dw = sw * scale;
            double dh = sh * scale;
            double dx = 6.0 + floor((img_dim - dw) * 0.5);
            double dy = 6.0 + floor((img_dim - dh) * 0.5);
            cairo_save(cr);
            cairo_rectangle(cr, 6.0, 6.0, img_dim, img_dim);
            cairo_clip(cr);
            cairo_translate(cr, dx, dy);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, view->icon_surface, 0, 0);
            cairo_pattern_set_filter(
                cairo_get_source(cr),
                (scale == 2.0 || scale == 1.0) ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD
            );
            cairo_paint(cr);
            cairo_restore(cr);
        }
    } else {
        draw_default_cde_app_emblem(cr, 6.0, 6.0, img_dim, view, ics);
    }

    /* Icon label from WM_ICON_NAME / _NET_WM_ICON_NAME (or active workspace name for FrontPanel) */
    const char *label = nullptr;
    if (view->is_panel) {
        label = (server->current_workspace < server->num_workspaces &&
                 server->workspaces[server->current_workspace].title[0])
                    ? server->workspaces[server->current_workspace].title
                    : (view->icon_title[0] ? view->icon_title : "One");
    } else if (view->icon_title[0]) {
        label = view->icon_title;
    } else {
        label = dtwm_view_title(view);
    }
    draw_cde_icon_title_box(
        cr,
        server,
        label,
        4.0,
        img_frame_h + 2.0,
        frame_w - 8.0,
        label_frame_h - 4.0,
        server->palette.inactive.fg
    );

    /* Separate 3D open-window indicator bar beside the icon frame (UpdateOpenIconIndicator) */
    if (show_bar) {
        bool ind_active = view_or_transient_has_focus(view);
        const DtwmColorSet *bcs = ind_active ? &server->palette.active : &server->palette.inactive;
        cairo_set_source_rgba(cr, bcs->bg.r, bcs->bg.g, bcs->bg.b, 1.0);
        cairo_rectangle(cr, bar_x, bar_y, bar_w, bar_h);
        cairo_fill(cr);
        dtwm_draw_bevel(cr, bar_x, bar_y, bar_w, bar_h, 1.0, bcs->top_shadow, bcs->bottom_shadow);
    }

    dtwm_cairo_buffer_finish(cr, surf);
    return buf;
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

    struct wlr_buffer *buf = dtwm_icons_render_tile_buffer(view, nullptr, nullptr);
    if (!buf) return;
    wlr_scene_buffer_set_buffer(view->desktop_icon_buf, buf);
    wlr_scene_node_set_enabled(&view->desktop_icon_buf->node, !view->icon_clipped_out);
    wlr_buffer_drop(buf);
}

static void ensure_icon_box_scene_nodes(DtwmServer *server) {
    if (!server || !server->workspace_tree) return;
    if (!server->icon_box_tree) {
        server->icon_box_tree = wlr_scene_tree_create(server->workspace_tree);
        wlr_scene_node_set_enabled(&server->icon_box_tree->node, false);
    }
    if (!server->icon_box_bg_buf && server->icon_box_tree) {
        server->icon_box_bg_buf = wlr_scene_buffer_create(server->icon_box_tree, nullptr);
    }
}

static void draw_ib_groove_v(cairo_t *cr, double x, double y0, double y1, DtwmRgba bs, DtwmRgba ts) {
    cairo_set_source_rgba(cr, bs.r, bs.g, bs.b, bs.a);
    cairo_rectangle(cr, x, y0, 1.0, y1 - y0);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, ts.r, ts.g, ts.b, ts.a);
    cairo_rectangle(cr, x + 1.0, y0, 1.0, y1 - y0);
    cairo_fill(cr);
}

static void draw_ib_groove_h(cairo_t *cr, double x0, double x1, double y, DtwmRgba bs, DtwmRgba ts) {
    cairo_set_source_rgba(cr, bs.r, bs.g, bs.b, bs.a);
    cairo_rectangle(cr, x0, y, x1 - x0, 1.0);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, ts.r, ts.g, ts.b, ts.a);
    cairo_rectangle(cr, x0, y + 1.0, x1 - x0, 1.0);
    cairo_fill(cr);
}

static void render_icon_box_frame(DtwmServer *server, int total_content_h, int well_h) {
    if (!server || !server->icon_box_bg_buf) return;
    int outer_w = server->icon_box_w;
    int outer_h = server->icon_box_h;
    if (outer_w < 180) outer_w = 180;
    if (outer_h < 120) outer_h = 120;

    cairo_t *cr = nullptr;
    cairo_surface_t *surf = nullptr;
    struct wlr_buffer *buf = dtwm_cairo_buffer_begin(outer_w, outer_h, &cr, &surf);
    if (!buf) return;

    const DtwmColorSet *fcs =
        server->icon_box_active ? &server->palette.active : &server->palette.inactive;
    const DtwmColorSet *ics = &server->palette.menu;

    /* 1. Outer 3D CDE window frame + resize corner grooves */
    cairo_set_source_rgba(cr, fcs->bg.r, fcs->bg.g, fcs->bg.b, 1.0);
    cairo_rectangle(cr, 0, 0, outer_w, outer_h);
    cairo_fill(cr);
    dtwm_draw_bevel(cr, 0, 0, outer_w, outer_h, 2.0, fcs->top_shadow, fcs->bottom_shadow);
    dtwm_draw_bevel(
        cr,
        DTWM_BORDER_WIDTH - 1,
        DTWM_BORDER_WIDTH - 1,
        outer_w - 2 * (DTWM_BORDER_WIDTH - 1),
        outer_h - 2 * (DTWM_BORDER_WIDTH - 1),
        1.0,
        fcs->bottom_shadow,
        fcs->top_shadow
    );

    int c = DTWM_CORNER_HANDLE_LEN;
    if (outer_w > 2 * c + 16) {
        draw_ib_groove_v(cr, c, 1, DTWM_BORDER_WIDTH - 1, fcs->bottom_shadow, fcs->top_shadow);
        draw_ib_groove_v(cr, outer_w - c - 2, 1, DTWM_BORDER_WIDTH - 1, fcs->bottom_shadow, fcs->top_shadow);
        draw_ib_groove_v(cr, c, outer_h - DTWM_BORDER_WIDTH + 1, outer_h - 1, fcs->bottom_shadow, fcs->top_shadow);
        draw_ib_groove_v(cr, outer_w - c - 2, outer_h - DTWM_BORDER_WIDTH + 1, outer_h - 1, fcs->bottom_shadow, fcs->top_shadow);
    }
    if (outer_h > 2 * c + 16) {
        draw_ib_groove_h(cr, 1, DTWM_BORDER_WIDTH - 1, c, fcs->bottom_shadow, fcs->top_shadow);
        draw_ib_groove_h(cr, outer_w - DTWM_BORDER_WIDTH + 1, outer_w - 1, c, fcs->bottom_shadow, fcs->top_shadow);
        draw_ib_groove_h(cr, 1, DTWM_BORDER_WIDTH - 1, outer_h - c - 2, fcs->bottom_shadow, fcs->top_shadow);
        draw_ib_groove_h(cr, outer_w - DTWM_BORDER_WIDTH + 1, outer_w - 1, outer_h - c - 2, fcs->bottom_shadow, fcs->top_shadow);
    }

    /* 2. Titlebar ("Icons") with MWM menu bar button and Maximize button */
    double tx = DTWM_BORDER_WIDTH;
    double ty = DTWM_BORDER_WIDTH;
    double tw = outer_w - 2 * DTWM_BORDER_WIDTH;
    double th = DTWM_TITLE_HEIGHT;
    double bw = DTWM_TITLE_HEIGHT;

    /* Left menu button */
    dtwm_draw_bevel(cr, tx, ty, bw, th, 1.0, fcs->top_shadow, fcs->bottom_shadow);
    dtwm_draw_bevel(cr, tx + 4, ty + th * 0.5 - 2, bw - 8, 4, 1.0, fcs->top_shadow, fcs->bottom_shadow);

    /* Right maximize button */
    double mx = tx + tw - bw;
    dtwm_draw_bevel(cr, mx, ty, bw, th, 1.0, fcs->top_shadow, fcs->bottom_shadow);
    if (server->icon_box_maximized) {
        dtwm_draw_bevel(cr, mx + 4, ty + 4, bw - 8, th - 8, 1.0, fcs->bottom_shadow, fcs->top_shadow);
    } else {
        dtwm_draw_bevel(cr, mx + 4, ty + 4, bw - 8, th - 8, 1.0, fcs->top_shadow, fcs->bottom_shadow);
    }

    /* Center title plate */
    double title_x = tx + bw;
    double title_w = tw - 2 * bw;
    dtwm_draw_bevel(cr, title_x, ty, title_w, th, 1.0, fcs->top_shadow, fcs->bottom_shadow);
    dtwm_draw_label(cr, "Icons", title_x, ty, title_w, th, fcs->fg, true, true);

    /* 3. Client interior + sunken scrolled well + vertical CDE scrollbar */
    double cx = DTWM_BORDER_WIDTH;
    double cy = DTWM_BORDER_WIDTH + DTWM_TITLE_HEIGHT;
    double cw = outer_w - 2 * DTWM_BORDER_WIDTH;
    double ch = outer_h - 2 * DTWM_BORDER_WIDTH - DTWM_TITLE_HEIGHT;

    cairo_set_source_rgba(cr, ics->bg.r, ics->bg.g, ics->bg.b, 1.0);
    cairo_rectangle(cr, cx, cy, cw, ch);
    cairo_fill(cr);

    double sb_w = 16.0;
    double well_x = cx + 5.0;
    double well_y = cy + 5.0;
    double well_w = cw - 10.0 - sb_w - 4.0;
    double well_box_h = ch - 10.0;
    if (well_w < 64.0) well_w = 64.0;
    if (well_box_h < 40.0) well_box_h = 40.0;

    cairo_set_source_rgba(cr, ics->bg.r * 0.86, ics->bg.g * 0.86, ics->bg.b * 0.86, 1.0);
    cairo_rectangle(cr, well_x, well_y, well_w, well_box_h);
    cairo_fill(cr);
    dtwm_draw_bevel(cr, well_x, well_y, well_w, well_box_h, 2.0, ics->bottom_shadow, ics->top_shadow);

    /* Vertical CDE scrollbar */
    double sb_x = well_x + well_w + 4.0;
    double sb_y = well_y;
    double sb_h = well_box_h;
    cairo_set_source_rgba(cr, ics->bg.r * 0.82, ics->bg.g * 0.82, ics->bg.b * 0.82, 1.0);
    cairo_rectangle(cr, sb_x, sb_y, sb_w, sb_h);
    cairo_fill(cr);
    dtwm_draw_bevel(cr, sb_x, sb_y, sb_w, sb_h, 1.5, ics->bottom_shadow, ics->top_shadow);

    double arr_h = fmin(15.0, sb_h * 0.25);
    /* Up arrow */
    cairo_move_to(cr, sb_x + sb_w * 0.5, sb_y + 3.0);
    cairo_line_to(cr, sb_x + sb_w - 3.0, sb_y + arr_h);
    cairo_line_to(cr, sb_x + 3.0, sb_y + arr_h);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, ics->bg.r, ics->bg.g, ics->bg.b, 1.0);
    cairo_fill(cr);

    /* Down arrow */
    cairo_move_to(cr, sb_x + sb_w * 0.5, sb_y + sb_h - 3.0);
    cairo_line_to(cr, sb_x + sb_w - 3.0, sb_y + sb_h - arr_h);
    cairo_line_to(cr, sb_x + 3.0, sb_y + sb_h - arr_h);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, ics->bg.r, ics->bg.g, ics->bg.b, 1.0);
    cairo_fill(cr);

    /* Proportional 3D thumb */
    double track_y = sb_y + arr_h + 2.0;
    double track_h = sb_h - 2.0 * (arr_h + 2.0);
    if (track_h > 12.0) {
        double ratio = (total_content_h > well_h && total_content_h > 0)
                           ? fmax(0.18, fmin(1.0, (double)well_h / (double)total_content_h))
                           : 1.0;
        double thumb_h = fmax(14.0, track_h * ratio);
        double frac = (server->icon_box_max_scroll_y > 0)
                          ? ((double)server->icon_box_scroll_y / (double)server->icon_box_max_scroll_y)
                          : 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        double thumb_y = track_y + (track_h - thumb_h) * frac;
        cairo_set_source_rgba(cr, ics->bg.r, ics->bg.g, ics->bg.b, 1.0);
        cairo_rectangle(cr, sb_x + 2.0, thumb_y, sb_w - 4.0, thumb_h);
        cairo_fill(cr);
        dtwm_draw_bevel(cr, sb_x + 2.0, thumb_y, sb_w - 4.0, thumb_h, 1.5, ics->top_shadow, ics->bottom_shadow);
    }

    dtwm_cairo_buffer_finish(cr, surf);
    wlr_scene_buffer_set_buffer(server->icon_box_bg_buf, buf);
    wlr_scene_node_set_enabled(&server->icon_box_bg_buf->node, true);
    wlr_buffer_drop(buf);
}

void dtwm_icons_relayout(DtwmServer *server) {
    if (!server) return;
    ensure_icon_box_scene_nodes(server);

    struct wlr_box ubox;
    dtwm_get_usable_output_box(server, 1, 1, &ubox);

    int img_dim = server->large_icons ? 96 : 48;

    if (server->use_icon_box && !server->greeter_mode) {
        int min_h = server->large_icons ? 214 : 158;
        if (!server->icon_box_pos_initialized) {
            server->icon_box_w = 524;
            server->icon_box_h = min_h;
            server->icon_box_x = ubox.x + 20;
            server->icon_box_y = ubox.y + ubox.height - server->icon_box_h - 14;
            if (server->icon_box_y < ubox.y + 8) server->icon_box_y = ubox.y + 8;
            server->icon_box_pos_initialized = true;
        } else if (!server->icon_box_maximized && server->icon_box_h < min_h && !server->icon_box_resizing) {
            int dy = min_h - server->icon_box_h;
            server->icon_box_h = min_h;
            if (server->icon_box_y + server->icon_box_h > ubox.y + ubox.height - 4) {
                server->icon_box_y -= dy;
                if (server->icon_box_y < ubox.y) server->icon_box_y = ubox.y;
            }
        }
        if (server->icon_box_w < 220) server->icon_box_w = 220;
        if (server->icon_box_h < 124) server->icon_box_h = 124;

        if (server->icon_box_tree) {
            wlr_scene_node_set_position(
                &server->icon_box_tree->node, server->icon_box_x, server->icon_box_y
            );
            wlr_scene_node_set_enabled(&server->icon_box_tree->node, true);
        }

        int cw = server->icon_box_w - 2 * DTWM_BORDER_WIDTH;
        int ch = server->icon_box_h - 2 * DTWM_BORDER_WIDTH - DTWM_TITLE_HEIGHT;
        int well_x = DTWM_BORDER_WIDTH + 5;
        int well_y = DTWM_BORDER_WIDTH + DTWM_TITLE_HEIGHT + 5;
        int well_w = cw - 10 - 20;
        int well_h = ch - 10;
        if (well_w < 80) well_w = 80;
        if (well_h < 48) well_h = 48;

        int frame_w = img_dim + 12;
        int frame_h = (img_dim + 11) + (server->large_icons ? 24 : 21);
        int col_w = frame_w + 12;
        int row_h = frame_h + 10;
        int cols = (well_w - 12) / col_w;
        if (cols < 1) cols = 1;

        int count = 0;
        DtwmView *v;
        wl_list_for_each_reverse(v, &server->views, link) {
            if (should_show_desktop_icon(v) && !v->is_panel) {
                count++;
            }
        }
        int total_rows = (count + cols - 1) / cols;
        if (total_rows < 1) total_rows = 1;
        int content_h = 10 + total_rows * row_h;
        int max_scroll = content_h > well_h ? (content_h - well_h) : 0;
        server->icon_box_max_scroll_y = max_scroll;
        if (server->icon_box_scroll_y > max_scroll) server->icon_box_scroll_y = max_scroll;
        if (server->icon_box_scroll_y < 0) server->icon_box_scroll_y = 0;

        render_icon_box_frame(server, content_h, well_h);

        int slot = 0;
        wl_list_for_each_reverse(v, &server->views, link) {
            if (!should_show_desktop_icon(v) || v->is_panel) {
                dtwm_icons_update_view(v);
                continue;
            }
            dtwm_icons_update_view(v);
            if (v->desktop_icon_buf && server->icon_box_tree &&
                v->desktop_icon_buf->node.parent != server->icon_box_tree) {
                wlr_scene_node_reparent(&v->desktop_icon_buf->node, server->icon_box_tree);
            }

            int row = slot / cols;
            int col = slot % cols;
            int lx = well_x + 8 + col * col_w;
            int ly = well_y + 6 + row * row_h - server->icon_box_scroll_y;

            v->icon_x = server->icon_box_x + lx;
            v->icon_y = server->icon_box_y + ly;

            bool in_bounds = (ly >= well_y + 2) &&
                             (ly + v->icon_box_h <= well_y + well_h + 2 || row == 0);
            v->icon_clipped_out = !in_bounds;
            if (v->desktop_icon_buf) {
                wlr_scene_node_set_position(&v->desktop_icon_buf->node, lx, ly);
                wlr_scene_node_set_enabled(&v->desktop_icon_buf->node, in_bounds);
            }
            publish_client_icon_geometry(v);
            slot++;
        }
        return;
    }

    if (server->icon_box_tree) {
        wlr_scene_node_set_enabled(&server->icon_box_tree->node, false);
        server->icon_box_active = false;
        server->icon_box_dragging = false;
        server->icon_box_resizing = false;
    }

    struct wlr_box fbox = ubox;
    wlr_output_layout_get_box(server->output_layout, nullptr, &fbox);
    if (fbox.width <= 0 || fbox.height <= 0) {
        fbox = ubox;
    }

    int frame_w = img_dim + 12;
    int frame_h = (img_dim + 11) + (server->large_icons ? 24 : 21);
    int indicator_extra =
        server->show_open_window_icons ? (DTWM_OPEN_ICON_BAR_GAP + DTWM_OPEN_ICON_BAR_WIDTH) : 0;

    int gap = (server->icon_placement_margin >= 0 && server->icon_placement_margin <= 128)
                  ? server->icon_placement_margin
                  : 8;
    int edge_pad_x = gap > 8 ? gap : 12;
    int edge_pad_y = gap > 8 ? gap : 6;
    int col_w = frame_w + indicator_extra + (gap > 6 ? gap : 10);
    int row_h = frame_h + (gap > 4 ? gap : 6);

    /* Use full monitor height for left/right edge columns when Front Panel doesn't reach corners */
    bool side_clears_panel =
        !server->show_front_panel || (fbox.width >= DTWM_PANEL_WIDTH + 2 * (col_w + edge_pad_x + 8));
    struct wlr_box gbox = side_clears_panel ? fbox : ubox;

    int max_rows = (gbox.height - edge_pad_y * 2) / row_h;
    if (max_rows < 1) max_rows = 1;
    int max_cols = (gbox.width - edge_pad_x * 2) / col_w;
    if (max_cols < 1) max_cols = 1;

    /* Check whether FrontPanel occupies the bottom-left corner slot (matching X11 UI.c XmNiconY) */
    bool panel_in_bottom_left = false;
    DtwmView *v;
    wl_list_for_each_reverse(v, &server->views, link) {
        if (v->is_panel && should_show_desktop_icon(v)) {
            panel_in_bottom_left = true;
            break;
        }
    }

    DtwmIconPlacement place = server->icon_placement;
    bool horiz_primary =
        (place == DTWM_ICON_PLACE_LEFT_TOP || place == DTWM_ICON_PLACE_LEFT_BOTTOM ||
         place == DTWM_ICON_PLACE_RIGHT_TOP || place == DTWM_ICON_PLACE_RIGHT_BOTTOM);
    bool right_side =
        (place == DTWM_ICON_PLACE_TOP_RIGHT || place == DTWM_ICON_PLACE_RIGHT_TOP ||
         place == DTWM_ICON_PLACE_BOTTOM_RIGHT || place == DTWM_ICON_PLACE_RIGHT_BOTTOM);
    bool bottom_side =
        (place == DTWM_ICON_PLACE_BOTTOM_LEFT || place == DTWM_ICON_PLACE_LEFT_BOTTOM ||
         place == DTWM_ICON_PLACE_BOTTOM_RIGHT || place == DTWM_ICON_PLACE_RIGHT_BOTTOM);

    int col0_rows = max_rows;
    if (panel_in_bottom_left && !horiz_primary && !right_side && !bottom_side && col0_rows > 1) {
        col0_rows--; /* Reserve bottom-left slot for FrontPanel ("One") */
    }

    int slot = 0;
    wl_list_for_each_reverse(v, &server->views, link) {
        if (!should_show_desktop_icon(v)) {
            dtwm_icons_update_view(v);
            continue;
        }
        dtwm_icons_update_view(v);
        if (v->desktop_icon_buf && server->desktop_icon_tree &&
            v->desktop_icon_buf->node.parent != server->desktop_icon_tree) {
            wlr_scene_node_reparent(&v->desktop_icon_buf->node, server->desktop_icon_tree);
        }

        int tile_x = 0, tile_y = 0;
        if (v->is_panel || (v->has_icon_pos_hint && v->icon_hint_y > fbox.height * 3 / 4 && v->icon_hint_x <= 64)) {
            /* FrontPanel (UI.c XmNiconY = display_height): bottom-left corner beside Front Panel */
            tile_x = gbox.x + edge_pad_x;
            tile_y = gbox.y + gbox.height - frame_h - 4;
            if (tile_y < gbox.y + edge_pad_y) tile_y = gbox.y + edge_pad_y;
        } else {
            int col = 0, row = 0;
            if (horiz_primary) {
                col = slot % max_cols;
                row = slot / max_cols;
            } else if (!right_side && !bottom_side && slot < col0_rows) {
                col = 0;
                row = slot;
            } else if (!right_side && !bottom_side) {
                int rem = slot - col0_rows;
                col = 1 + (rem / max_rows);
                row = rem % max_rows;
            } else {
                col = slot / max_rows;
                row = slot % max_rows;
            }

            if (right_side) {
                tile_x = gbox.x + gbox.width - edge_pad_x - (col + 1) * col_w +
                         (col_w - (frame_w + indicator_extra));
            } else {
                tile_x = gbox.x + edge_pad_x + col * col_w;
            }
            if (bottom_side) {
                tile_y = gbox.y + gbox.height - edge_pad_y - frame_h - row * row_h;
            } else {
                tile_y = gbox.y + edge_pad_y + row * row_h;
            }
            slot++;
        }

        v->icon_x = tile_x;
        v->icon_y = tile_y;
        v->icon_clipped_out = false;
        if (v->desktop_icon_buf) {
            wlr_scene_node_set_position(&v->desktop_icon_buf->node, tile_x, tile_y);
            wlr_scene_node_set_enabled(&v->desktop_icon_buf->node, true);
        }
        publish_client_icon_geometry(v);
    }
}

DtwmView *dtwm_icons_view_at(DtwmServer *server, double lx, double ly) {
    if (!server) return nullptr;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (!should_show_desktop_icon(v) || !v->desktop_icon_buf ||
            !v->desktop_icon_buf->node.enabled) {
            continue;
        }
        bool in_box = (server->use_icon_box && !server->greeter_mode && !v->is_panel);
        int base_x = in_box ? server->icon_box_x : 0;
        int base_y = in_box ? server->icon_box_y : 0;
        int tx = base_x + v->desktop_icon_buf->node.x;
        int ty = base_y + v->desktop_icon_buf->node.y;
        /* Hit-test main icon frame */
        if (lx >= tx && lx < tx + v->icon_w &&
            ly >= ty && ly < ty + v->icon_h) {
            return v;
        }
        /* Hit-test separate open-window indicator bar beside icon frame (if visible) */
        if (v->icon_box_w > v->icon_w) {
            int bar_x = tx + v->icon_w + DTWM_OPEN_ICON_BAR_GAP;
            int bar_h = (v->icon_h * 3) / 8;
            int bar_y = ty + (v->icon_h - bar_h) / 2;
            if (lx >= bar_x && lx < bar_x + DTWM_OPEN_ICON_BAR_WIDTH &&
                ly >= bar_y && ly < bar_y + bar_h) {
                return v;
            }
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

static bool matches_panel_embedded_client_prop(DtwmServer *server, const char *inst, const char *cls) {
    if (!server->panel_view || !server->panel_view->xwayland_surface || !server->xcb_conn) {
        return false;
    }
    xcb_atom_t prop = intern_atom_icons(server->xcb_conn, "_DT_PANEL_EMBEDDED_CLIENTS");
    if (prop == XCB_ATOM_NONE) return false;
    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn, 0, server->panel_view->xwayland_surface->window_id,
        prop, XCB_ATOM_STRING, 0, 256
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return false;
    bool hit = false;
    int len = xcb_get_property_value_length(rep);
    if (len > 0) {
        const char *p = (const char *)xcb_get_property_value(rep);
        int off = 0;
        while (off < len) {
            size_t sl = strnlen(p + off, (size_t)(len - off));
            if (sl > 0) {
                if ((inst && strncasecmp(inst, p + off, sl) == 0 && inst[sl] == '\0') ||
                    (cls && strncasecmp(cls, p + off, sl) == 0 && cls[sl] == '\0')) {
                    hit = true;
                    break;
                }
            }
            off += (int)sl + 1;
        }
    }
    free(rep);
    return hit;
}

bool dtwm_icons_classify_xwayland_map(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return false;
    DtwmServer *server = view->server;
    struct wlr_xwayland_surface *xs = view->xwayland_surface;

    char inst[128] = {0}, cls[128] = {0};
    read_xcb_wm_class_sync(server, xs, inst, sizeof(inst), cls, sizeof(cls));

    if (matches_panel_embedded_client_prop(server, inst, cls)) {
        /* Genuine dtwm-panel will XReparentWindow this TYPE client into its DtControl slot */
        view->is_swallowed = true;
        view->decorated = false;
        view->has_title = false;
        view->has_resize_handles = false;
        return true;
    }

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
                view->icon_from_named_image = false;
                wl_list_remove(&pend->link);
                free(pend);
                break;
            }
        }
        resolve_xdg_view_named_icon_fallback(view);
    } else if (view->kind == DTWM_VIEW_XWAYLAND) {
        refresh_xwayland_icon_title(view);
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
    dtwm_anim_cancel_for_view(server, view);
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
    if (!server) return;
    bool is_hints = (atom == XCB_ATOM_WM_HINTS || atom == server->atom_net_wm_icon ||
                     atom == XCB_ATOM_WM_CLASS || atom == server->atom_wm_command);
    bool is_icon_name = (atom == XCB_ATOM_WM_ICON_NAME || atom == server->atom_net_wm_icon_name ||
                         atom == XCB_ATOM_WM_NAME);
    if (!is_hints && !is_icon_name) return;

    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface &&
            v->xwayland_surface->window_id == win) {
            if (is_icon_name) {
                refresh_xwayland_icon_title(v);
            }
            if (is_hints) {
                refresh_xwayland_wm_hints(v, false);
            }
            dtwm_icons_update_view(v);
            break;
        }
    }
}

void dtwm_icons_set_placement(DtwmServer *server, const char *spec) {
    if (!server || !spec) return;
    while (*spec && isspace((unsigned char)*spec)) spec++;
    char tok1[32] = {0}, tok2[32] = {0};
    size_t i = 0;
    while (*spec && !isspace((unsigned char)*spec) && i + 1 < sizeof(tok1)) {
        tok1[i++] = *spec++;
    }
    while (*spec && isspace((unsigned char)*spec)) spec++;
    i = 0;
    while (*spec && !isspace((unsigned char)*spec) && i + 1 < sizeof(tok2)) {
        tok2[i++] = *spec++;
    }
    if (strcasecmp(tok2, "tight") == 0) tok2[0] = '\0';
    if (!tok1[0]) return;

    DtwmIconPlacement next = server->icon_placement;
    if (strcasecmp(tok1, "top") == 0) {
        next = (strcasecmp(tok2, "right") == 0) ? DTWM_ICON_PLACE_TOP_RIGHT : DTWM_ICON_PLACE_TOP_LEFT;
    } else if (strcasecmp(tok1, "bottom") == 0) {
        next = (strcasecmp(tok2, "right") == 0) ? DTWM_ICON_PLACE_BOTTOM_RIGHT : DTWM_ICON_PLACE_BOTTOM_LEFT;
    } else if (strcasecmp(tok1, "left") == 0) {
        next = (strcasecmp(tok2, "top") == 0) ? DTWM_ICON_PLACE_LEFT_TOP : DTWM_ICON_PLACE_LEFT_BOTTOM;
    } else if (strcasecmp(tok1, "right") == 0) {
        next = (strcasecmp(tok2, "top") == 0) ? DTWM_ICON_PLACE_RIGHT_TOP : DTWM_ICON_PLACE_RIGHT_BOTTOM;
    }
    if (next != server->icon_placement) {
        server->icon_placement = next;
        dtwm_icons_relayout(server);
    }
}

void dtwm_icons_refresh_focus_indicators(DtwmServer *server) {
    if (!server) return;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped && should_show_desktop_icon(v)) {
            dtwm_icons_update_view(v);
        }
    }
}

void dtwm_icons_invalidate_theme_icons(DtwmServer *server) {
    if (!server) return;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (!v->mapped) continue;
        if (v->kind == DTWM_VIEW_XWAYLAND) {
            if (v->icon_from_named_image && v->icon_surface) {
                cairo_surface_destroy(v->icon_surface);
                v->icon_surface = nullptr;
            }
            refresh_xwayland_wm_hints(v, false);
        } else if (v->kind == DTWM_VIEW_XDG && v->icon_from_named_image) {
            if (v->icon_surface) {
                cairo_surface_destroy(v->icon_surface);
                v->icon_surface = nullptr;
            }
            resolve_xdg_view_named_icon_fallback(v);
        }
    }
}

void dtwm_icons_set_open_window_icons(DtwmServer *server, bool enabled) {
    if (!server || server->show_open_window_icons == enabled) return;
    server->show_open_window_icons = enabled;
    publish_icon_sizes_and_settings(server);
    dtwm_icons_relayout(server);
}

void dtwm_icons_set_use_icon_box(DtwmServer *server, bool enabled) {
    if (!server || server->use_icon_box == enabled) return;
    server->use_icon_box = enabled;
    server->icon_box_scroll_y = 0;
    publish_icon_sizes_and_settings(server);
    dtwm_icons_relayout(server);
    if (enabled && server->icon_box_tree) {
        wlr_scene_node_raise_to_top(&server->icon_box_tree->node);
    }
}

void dtwm_icons_set_placement_margin(DtwmServer *server, int margin) {
    if (!server) return;
    if (margin < -1) margin = -1;
    if (margin > 128) margin = 128;
    if (server->icon_placement_margin == margin) return;
    server->icon_placement_margin = margin;
    dtwm_icons_relayout(server);
}

void dtwm_icons_set_large_icons(DtwmServer *server, bool large) {
    if (!server || server->large_icons == large) return;
    server->large_icons = large;
    publish_icon_sizes_and_settings(server);
    dtwm_icons_relayout(server);
}

bool dtwm_icon_box_is_topmost_at(DtwmServer *server, double lx, double ly) {
    if (!server || !server->use_icon_box || server->greeter_mode || !server->icon_box_tree ||
        !server->icon_box_tree->node.enabled) {
        return false;
    }
    double nx = 0, ny = 0;
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, &nx, &ny);
    return node && node->parent == server->icon_box_tree;
}

DtwmFramePart dtwm_icon_box_part_at(DtwmServer *server, double lx, double ly) {
    if (!server || !server->use_icon_box || !server->icon_box_tree ||
        !server->icon_box_tree->node.enabled) {
        return DTWM_PART_NONE;
    }
    int x = (int)lx - server->icon_box_x;
    int y = (int)ly - server->icon_box_y;
    int w = server->icon_box_w;
    int h = server->icon_box_h;
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return DTWM_PART_NONE;
    }
    int b = DTWM_BORDER_WIDTH;
    int c = DTWM_CORNER_HANDLE_LEN;
    bool top_e = (y < b);
    bool bot_e = (y >= h - b);
    bool left_e = (x < b);
    bool right_e = (x >= w - b);
    if (top_e || bot_e || left_e || right_e) {
        if ((top_e && x < c) || (left_e && y < c)) return DTWM_PART_RESIZE_NW;
        if ((top_e && x >= w - c) || (right_e && y < c)) return DTWM_PART_RESIZE_NE;
        if ((bot_e && x < c) || (left_e && y >= h - c)) return DTWM_PART_RESIZE_SW;
        if ((bot_e && x >= w - c) || (right_e && y >= h - c)) return DTWM_PART_RESIZE_SE;
        if (top_e) return DTWM_PART_RESIZE_N;
        if (bot_e) return DTWM_PART_RESIZE_S;
        if (left_e) return DTWM_PART_RESIZE_W;
        return DTWM_PART_RESIZE_E;
    }
    if (y >= b && y < b + DTWM_TITLE_HEIGHT) {
        int bw = DTWM_TITLE_HEIGHT;
        if (x < b + bw) return DTWM_PART_BTN_MENU;
        if (x >= w - b - bw) return DTWM_PART_BTN_MAXIMIZE;
        return DTWM_PART_TITLE;
    }
    return DTWM_PART_CLIENT;
}

void dtwm_icon_box_deactivate(DtwmServer *server) {
    if (!server || !server->icon_box_active) return;
    server->icon_box_active = false;
    if (server->use_icon_box && server->icon_box_tree && server->icon_box_tree->node.enabled) {
        dtwm_icons_relayout(server);
    }
}

static void toggle_icon_box_maximize(DtwmServer *server) {
    struct wlr_box ubox;
    dtwm_get_usable_output_box(server, server->icon_box_x + 32, server->icon_box_y + 32, &ubox);
    if (!server->icon_box_maximized) {
        server->icon_box_saved_x = server->icon_box_x;
        server->icon_box_saved_y = server->icon_box_y;
        server->icon_box_saved_w = server->icon_box_w;
        server->icon_box_saved_h = server->icon_box_h;
        server->icon_box_maximized = true;
        server->icon_box_x = ubox.x;
        server->icon_box_y = ubox.y;
        server->icon_box_w = ubox.width;
        server->icon_box_h = ubox.height;
    } else {
        server->icon_box_maximized = false;
        server->icon_box_x = server->icon_box_saved_x;
        server->icon_box_y = server->icon_box_saved_y;
        server->icon_box_w = server->icon_box_saved_w > 220 ? server->icon_box_saved_w : 524;
        server->icon_box_h = server->icon_box_saved_h > 124 ? server->icon_box_saved_h : 158;
    }
    dtwm_icons_relayout(server);
}

bool dtwm_icon_box_handle_button(DtwmServer *server, uint32_t btn, bool pressed, uint32_t t_ms) {
    if (!server) return false;
    if (!pressed) {
        if (server->icon_box_dragging || server->icon_box_resizing) {
            server->icon_box_dragging = false;
            server->icon_box_resizing = false;
            dtwm_icons_relayout(server);
            return true;
        }
        return false;
    }
    if (!dtwm_icon_box_is_topmost_at(server, server->cursor->x, server->cursor->y)) {
        return false;
    }

    wlr_scene_node_raise_to_top(&server->icon_box_tree->node);
    if (!server->icon_box_active) {
        server->icon_box_active = true;
        dtwm_icons_relayout(server);
    }

    DtwmFramePart part = dtwm_icon_box_part_at(server, server->cursor->x, server->cursor->y);
    if (part == DTWM_PART_CLIENT) {
        DtwmView *iv = dtwm_icons_view_at(server, server->cursor->x, server->cursor->y);
        if (iv) {
            if (btn == BTN_LEFT) {
                server->icon_box_active = false;
                if (iv->minimized) {
                    dtwm_view_set_minimized(iv, false);
                }
                dtwm_restore_view_focus(server, iv);
            } else if (btn == BTN_RIGHT) {
                dtwm_open_window_menu_for_view(
                    server, iv, (int)server->cursor->x, (int)server->cursor->y
                );
            }
            return true;
        }
        /* Check scrollbar up/down arrow clicks on right edge of Icon Box */
        double rel_x = server->cursor->x - server->icon_box_x;
        double rel_y = server->cursor->y - server->icon_box_y;
        double sb_x0 = server->icon_box_w - DTWM_BORDER_WIDTH - 21.0;
        if (btn == BTN_LEFT && rel_x >= sb_x0) {
            double mid_y = DTWM_BORDER_WIDTH + DTWM_TITLE_HEIGHT +
                           (server->icon_box_h - 2 * DTWM_BORDER_WIDTH - DTWM_TITLE_HEIGHT) * 0.5;
            dtwm_icon_box_handle_axis(server, (rel_y < mid_y) ? -15.0 : 15.0, (rel_y < mid_y) ? -1 : 1);
        }
        return true;
    }

    if (btn == BTN_LEFT) {
        if (part == DTWM_PART_BTN_MAXIMIZE) {
            toggle_icon_box_maximize(server);
            return true;
        }
        if (part == DTWM_PART_BTN_MENU) {
            wlr_scene_node_lower_to_bottom(&server->icon_box_tree->node);
            server->icon_box_active = false;
            dtwm_icons_relayout(server);
            return true;
        }
        if (part == DTWM_PART_TITLE) {
            uint32_t dc = server->double_click_ms > 0 ? server->double_click_ms : 500u;
            if (server->icon_box_last_title_ms > 0 && (t_ms - server->icon_box_last_title_ms) <= dc) {
                server->icon_box_last_title_ms = 0;
                toggle_icon_box_maximize(server);
                return true;
            }
            server->icon_box_last_title_ms = t_ms;
            if (server->icon_box_maximized) {
                toggle_icon_box_maximize(server);
            }
            server->icon_box_dragging = true;
            server->icon_box_grab_x = server->cursor->x;
            server->icon_box_grab_y = server->cursor->y;
            server->icon_box_orig_box = (struct wlr_box){
                server->icon_box_x, server->icon_box_y, server->icon_box_w, server->icon_box_h
            };
            return true;
        }
        uint32_t edges = dtwm_part_to_wlr_edges(part);
        if (edges != WLR_EDGE_NONE) {
            server->icon_box_resizing = true;
            server->icon_box_resize_edges = edges;
            server->icon_box_grab_x = server->cursor->x;
            server->icon_box_grab_y = server->cursor->y;
            server->icon_box_orig_box = (struct wlr_box){
                server->icon_box_x, server->icon_box_y, server->icon_box_w, server->icon_box_h
            };
            return true;
        }
    }
    return true;
}

bool dtwm_icon_box_handle_motion(DtwmServer *server) {
    if (!server) return false;
    if (server->icon_box_dragging) {
        int nx = server->icon_box_orig_box.x + (int)(server->cursor->x - server->icon_box_grab_x);
        int ny = server->icon_box_orig_box.y + (int)(server->cursor->y - server->icon_box_grab_y);
        if (nx != server->icon_box_x || ny != server->icon_box_y) {
            server->icon_box_x = nx;
            server->icon_box_y = ny;
            if (server->icon_box_tree) {
                wlr_scene_node_set_position(&server->icon_box_tree->node, nx, ny);
            }
        }
        return true;
    }
    if (server->icon_box_resizing) {
        int dx = (int)(server->cursor->x - server->icon_box_grab_x);
        int dy = (int)(server->cursor->y - server->icon_box_grab_y);
        int nx = server->icon_box_orig_box.x;
        int ny = server->icon_box_orig_box.y;
        int nw = server->icon_box_orig_box.width;
        int nh = server->icon_box_orig_box.height;
        if (server->icon_box_resize_edges & WLR_EDGE_LEFT) {
            nx += dx; nw -= dx;
            if (nw < 220) { nx = server->icon_box_orig_box.x + server->icon_box_orig_box.width - 220; nw = 220; }
        } else if (server->icon_box_resize_edges & WLR_EDGE_RIGHT) {
            nw += dx; if (nw < 220) nw = 220;
        }
        if (server->icon_box_resize_edges & WLR_EDGE_TOP) {
            ny += dy; nh -= dy;
            if (nh < 124) { ny = server->icon_box_orig_box.y + server->icon_box_orig_box.height - 124; nh = 124; }
        } else if (server->icon_box_resize_edges & WLR_EDGE_BOTTOM) {
            nh += dy; if (nh < 124) nh = 124;
        }
        if (nx != server->icon_box_x || ny != server->icon_box_y ||
            nw != server->icon_box_w || nh != server->icon_box_h) {
            server->icon_box_x = nx;
            server->icon_box_y = ny;
            server->icon_box_w = nw;
            server->icon_box_h = nh;
            dtwm_icons_relayout(server);
        }
        return true;
    }
    return false;
}

bool dtwm_icon_box_handle_axis(DtwmServer *server, double delta, int32_t discrete) {
    if (!server || !dtwm_icon_box_is_topmost_at(server, server->cursor->x, server->cursor->y)) {
        return false;
    }
    if (server->icon_box_max_scroll_y <= 0) {
        return true;
    }
    int step = discrete != 0 ? (discrete * 36) : (int)(delta * 2.4);
    int next = server->icon_box_scroll_y + step;
    if (next < 0) next = 0;
    if (next > server->icon_box_max_scroll_y) next = server->icon_box_max_scroll_y;
    if (next != server->icon_box_scroll_y) {
        server->icon_box_scroll_y = next;
        dtwm_icons_relayout(server);
    }
    return true;
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
    server->atom_net_wm_icon = intern_atom_icons(server->xcb_conn, "_NET_WM_ICON");
    server->atom_net_wm_icon_name = intern_atom_icons(server->xcb_conn, "_NET_WM_ICON_NAME");
    server->atom_net_wm_pid = intern_atom_icons(server->xcb_conn, "_NET_WM_PID");
    server->atom_wm_command = intern_atom_icons(server->xcb_conn, "WM_COMMAND");
    publish_icon_sizes_and_settings(server);
}

void dtwm_icons_init(DtwmServer *server) {
    wl_list_init(&g_pending_xdg_icons);
    server->show_open_window_icons = true;
    server->icon_placement = DTWM_ICON_PLACE_TOP_LEFT;
    server->icon_placement_margin = -1;
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
