/*
 * CDE - Common Desktop Environment
 *
 * portal_remote_eis.c: Inhibit portal (dtsession / dtwm-wayland idle & sleep
 * inhibition + session state monitor), Settings portal (org.freedesktop.appearance
 * color-scheme & contrast), and RemoteDesktop portal (Notify* virtual input +
 * ConnectToEIS v1 binary wire protocol socket server).
 */

#define _GNU_SOURCE
#include "dtportal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <Xm/MessageB.h>
#include <Xm/ToggleB.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

static void mkdir_p_dir(const char *dir) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* -------------------------------------------------------------------------
 * 1. Inhibit Portal (Inhibit, CreateMonitor, QueryEndResponse)
 * ------------------------------------------------------------------------- */

void dtportal_recompute_inhibit_state(DtPortalCtx *ctx) {
    if (!ctx) return;
    uint32_t total_flags = 0;
    int count = 0;
    char apps[256] = {0};

    for (int i = 0; i < DTPORTAL_MAX_REQUESTS; ++i) {
        const DtPortalRequest *r = &ctx->requests[i];
        if (!r->in_use || r->kind != DTPORTAL_REQ_INHIBIT) continue;
        total_flags |= r->inhibit_flags;
        if (count > 0 && strlen(apps) + 2 < sizeof(apps)) strcat(apps, ",");
        if (strlen(apps) + strlen(r->app_id) + 1 < sizeof(apps)) {
            strcat(apps, r->app_id[0] ? r->app_id : "app");
        }
        count++;
    }

    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) {
        char st_path[PATH_MAX], tmp_path[PATH_MAX];
        snprintf(st_path, sizeof(st_path), "%s/cde-portal-inhibit.state", rt);
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", st_path, (int)getpid());
        FILE *fp = fopen(tmp_path, "w");
        if (fp) {
            fprintf(fp, "COUNT=%d\nFLAGS=0x%x\nAPPS=%s\n", count, total_flags, apps);
            fclose(fp);
            rename(tmp_path, st_path);
        }
    }

    /*Flags 4 = suspend, 8 = idle*/
    bool idle_or_suspend = (total_flags & (4u | 8u)) != 0;
    dtportal_send_wm_request(ctx, idle_or_suspend ? "f.set_idle_inhibit 1" : "f.set_idle_inhibit 0");
}

void dtportal_handle_inhibit(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
) {
    const char *sender = g_dbus_method_invocation_get_sender(invocation);

    if (strcmp(method_name, "Inhibit") == 0) {
        const char *handle = NULL, *app_id = NULL, *window = NULL;
        guint32 flags = 0;
        GVariant *options = NULL;
        g_variant_get(parameters, "(&o&s&su@a{sv})", &handle, &app_id, &window, &flags, &options);
        (void)window;

        /* Note: Inhibit keeps the Request object alive until Request.Close or bus disconnect,
         * while returning () immediately from the Inhibit method call! */
        DtPortalRequest *req = dtportal_alloc_request(ctx, DTPORTAL_REQ_INHIBIT, handle, app_id, NULL);
        if (req) {
            req->inhibit_flags = flags;
            const char *reason = NULL;
            if (g_variant_lookup(options, "reason", "&s", &reason) && reason) {
                snprintf(req->inhibit_reason, sizeof(req->inhibit_reason), "%s", reason);
            }
            if (sender && sender[0]) {
                snprintf(req->sender, sizeof(req->sender), "%s", sender);
            }
            dtportal_recompute_inhibit_state(ctx);
            dtportal_write_test_state(ctx);
        }
        g_variant_unref(options);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (strcmp(method_name, "CreateMonitor") == 0) {
        const char *handle = NULL, *session_handle = NULL, *app_id = NULL, *window = NULL;
        g_variant_get(parameters, "(&o&o&s&s)", &handle, &session_handle, &app_id, &window);
        (void)handle;
        (void)window;
        DtPortalSession *sess = dtportal_alloc_session(
            ctx, DTPORTAL_SESS_INHIBIT_MONITOR, session_handle, app_id, sender
        );
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", sess ? 0u : 2u));
        if (sess && ctx->bus) {
            GVariantBuilder st;
            g_variant_builder_init(&st, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&st, "{sv}", "screensaver-active", g_variant_new_boolean(FALSE));
            g_variant_builder_add(&st, "{sv}", "session-state", g_variant_new_uint32(1u));
            g_dbus_connection_emit_signal(
                ctx->bus,
                sess->sender[0] ? sess->sender : NULL,
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.impl.portal.Inhibit",
                "StateChanged",
                g_variant_new("(oa{sv})", sess->session_path, &st),
                NULL
            );
        }
        return;
    }

    if (strcmp(method_name, "QueryEndResponse") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown Inhibit method"
    );
}

/* -------------------------------------------------------------------------
 * 2. Settings Portal (org.freedesktop.appearance color-scheme & contrast)
 * ------------------------------------------------------------------------- */

void dtportal_sync_settings_from_appearance(DtPortalCtx *ctx) {
    if (!ctx || !ctx->bus) return;
    GError *err = NULL;
    GVariant *rep = g_dbus_connection_call_sync(
        ctx->bus,
        "org.cde.Appearance1",
        "/org/cde/Appearance1",
        "org.cde.Appearance1",
        "GetScheme",
        NULL,
        G_VARIANT_TYPE("(ssu)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START,
        250,
        NULL,
        &err
    );
    if (rep) {
        const char *mode = NULL, *theme = NULL;
        guint32 cs = 2u;
        g_variant_get(rep, "(&s&su)", &mode, &theme, &cs);
        ctx->color_scheme = (cs == 1u) ? 1u : 2u;
        ctx->contrast = (mode && strcmp(mode, "high-contrast") == 0) ? 1u : 0u;
        g_variant_unref(rep);
    } else if (err) {
        g_error_free(err);
    }
}

void dtportal_on_appearance_signal(DtPortalCtx *ctx, uint32_t color_scheme, uint32_t contrast) {
    if (!ctx || !ctx->bus) return;
    ctx->color_scheme = color_scheme;
    ctx->contrast = contrast;
    g_dbus_connection_emit_signal(
        ctx->bus, NULL,
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.Settings",
        "SettingChanged",
        g_variant_new("(ssv)", "org.freedesktop.appearance", "color-scheme",
                      g_variant_new_uint32(ctx->color_scheme)),
        NULL
    );
    g_dbus_connection_emit_signal(
        ctx->bus, NULL,
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.Settings",
        "SettingChanged",
        g_variant_new("(ssv)", "org.freedesktop.appearance", "contrast",
                      g_variant_new_uint32(ctx->contrast)),
        NULL
    );
}

static bool ns_matches_appearance(const char * const *namespaces) {
    if (!namespaces || !namespaces[0]) return true;
    for (int i = 0; namespaces[i]; ++i) {
        const char *ns = namespaces[i];
        if (!ns[0] || strcmp(ns, "*") == 0 ||
            strcmp(ns, "org.freedesktop.appearance") == 0 ||
            strcmp(ns, "org.freedesktop.*") == 0) {
            return true;
        }
    }
    return false;
}

void dtportal_handle_settings(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
) {
    dtportal_sync_settings_from_appearance(ctx);

    if (strcmp(method_name, "ReadAll") == 0) {
        const char **namespaces = NULL;
        g_variant_get(parameters, "(^a&s)", &namespaces);
        GVariantBuilder outer;
        g_variant_builder_init(&outer, G_VARIANT_TYPE("a{sa{sv}}"));
        if (ns_matches_appearance(namespaces)) {
            GVariantBuilder inner;
            g_variant_builder_init(&inner, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&inner, "{sv}", "color-scheme",
                                  g_variant_new_uint32(ctx->color_scheme));
            g_variant_builder_add(&inner, "{sv}", "contrast",
                                  g_variant_new_uint32(ctx->contrast));
            g_variant_builder_add(&outer, "{sa{sv}}", "org.freedesktop.appearance", &inner);
        }
        g_free(namespaces);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sa{sv}})", &outer));
        return;
    }

    if (strcmp(method_name, "Read") == 0 || strcmp(method_name, "ReadOne") == 0) {
        const char *ns = NULL, *key = NULL;
        g_variant_get(parameters, "(&s&s)", &ns, &key);
        if (strcmp(ns, "org.freedesktop.appearance") == 0 &&
            (strcmp(key, "color-scheme") == 0 || strcmp(key, "contrast") == 0)) {
            uint32_t val = (strcmp(key, "color-scheme") == 0) ? ctx->color_scheme : ctx->contrast;
            if (strcmp(method_name, "Read") == 0) {
                /* Read wraps result in an extra variant per portal spec: (v) where v contains Variant(u) */
                GVariant *inner = g_variant_new_variant(g_variant_new_uint32(val));
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", inner));
            } else {
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("(v)", g_variant_new_uint32(val))
                );
            }
            return;
        }
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.portal.Error.NotFound", "Requested setting not found"
        );
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown Settings method"
    );
}

/* -------------------------------------------------------------------------
 * 3. Wayland Virtual Pointer & Virtual Keyboard Backend (for RemoteDesktop + EIS)
 * ------------------------------------------------------------------------- */

/* Compact inline wl_interface descriptors for zwlr_virtual_pointer_v1 & zwp_virtual_keyboard_v1 */
extern const struct wl_interface dtportal_zwlr_virtual_pointer_v1_iface;
static const struct wl_interface *vptr_mgr_types[] = {
    &wl_seat_interface, &dtportal_zwlr_virtual_pointer_v1_iface
};
static const struct wl_message vptr_mgr_reqs[] = {
    { "create_virtual_pointer", "?on", vptr_mgr_types },
    { "destroy", "", NULL },
};
static const struct wl_interface dtportal_zwlr_virtual_pointer_mgr_v1_iface = {
    "zwlr_virtual_pointer_manager_v1", 1, 2, vptr_mgr_reqs, 0, NULL
};
static const struct wl_interface *vptr_null_types[5] = { NULL, NULL, NULL, NULL, NULL };
static const struct wl_message vptr_reqs[] = {
    { "motion", "uff", vptr_null_types },
    { "motion_absolute", "uuuuu", vptr_null_types },
    { "button", "uuu", vptr_null_types },
    { "axis", "uuf", vptr_null_types },
    { "frame", "", vptr_null_types },
    { "axis_source", "u", vptr_null_types },
    { "axis_stop", "uu", vptr_null_types },
    { "axis_discrete", "uufi", vptr_null_types },
    { "destroy", "", vptr_null_types },
};
const struct wl_interface dtportal_zwlr_virtual_pointer_v1_iface = {
    "zwlr_virtual_pointer_v1", 1, 9, vptr_reqs, 0, NULL
};

extern const struct wl_interface dtportal_zwp_virtual_keyboard_v1_iface;
static const struct wl_interface *vkbd_mgr_types[] = {
    &wl_seat_interface, &dtportal_zwp_virtual_keyboard_v1_iface
};
static const struct wl_message vkbd_mgr_reqs[] = {
    { "create_virtual_keyboard", "on", vkbd_mgr_types },
};
static const struct wl_interface dtportal_zwp_virtual_keyboard_mgr_v1_iface = {
    "zwp_virtual_keyboard_manager_v1", 1, 1, vkbd_mgr_reqs, 0, NULL
};
static const struct wl_message vkbd_reqs[] = {
    { "keymap", "uhu", vptr_null_types },
    { "key", "uuu", vptr_null_types },
    { "modifiers", "uuuuu", vptr_null_types },
    { "destroy", "", vptr_null_types },
};
const struct wl_interface dtportal_zwp_virtual_keyboard_v1_iface = {
    "zwp_virtual_keyboard_v1", 1, 4, vkbd_reqs, 0, NULL
};

static void wl_out_geom(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                        int32_t sub, const char *mk, const char *md, int32_t tr) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)mk; (void)md; (void)tr;
}
static void wl_out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t ref) {
    (void)o; (void)ref;
    DtPortalCtx *ctx = (DtPortalCtx *)d;
    if ((flags & WL_OUTPUT_MODE_CURRENT) && w > 0 && h > 0) {
        ctx->output_w = (uint32_t)w;
        ctx->output_h = (uint32_t)h;
    }
}
static void wl_out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void wl_out_scale(void *d, struct wl_output *o, int32_t s) { (void)d; (void)o; (void)s; }
static const struct wl_output_listener g_wl_out_l = {
    .geometry = wl_out_geom, .mode = wl_out_mode, .done = wl_out_done, .scale = wl_out_scale
};

static void rd_reg_global(void *data, struct wl_registry *reg, uint32_t name,
                          const char *iface, uint32_t ver) {
    DtPortalCtx *ctx = (DtPortalCtx *)data;
    (void)ver;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !ctx->wl_seat) {
        ctx->wl_seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, "zwlr_virtual_pointer_manager_v1") == 0 && !ctx->vptr_mgr) {
        ctx->vptr_mgr = wl_registry_bind(reg, name, &dtportal_zwlr_virtual_pointer_mgr_v1_iface, 1);
    } else if (strcmp(iface, "zwp_virtual_keyboard_manager_v1") == 0 && !ctx->vkbd_mgr) {
        ctx->vkbd_mgr = wl_registry_bind(reg, name, &dtportal_zwp_virtual_keyboard_mgr_v1_iface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        struct wl_output *out = wl_registry_bind(reg, name, &wl_output_interface, 2);
        wl_output_add_listener(out, &g_wl_out_l, ctx);
    }
}
static void rd_reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_rd_reg_l = {
    .global = rd_reg_global, .global_remove = rd_reg_rem
};

static bool ensure_virtual_input_ready(DtPortalCtx *ctx) {
    if (!ctx) return false;
    if (ctx->vptr && ctx->vkbd) return true;
    if (!getenv("WAYLAND_DISPLAY")) return false;

    if (!ctx->wl_dpy) {
        ctx->output_w = 1280;
        ctx->output_h = 900;
        ctx->wl_dpy = wl_display_connect(NULL);
        if (!ctx->wl_dpy) return false;
        ctx->wl_reg = wl_display_get_registry(ctx->wl_dpy);
        wl_registry_add_listener(ctx->wl_reg, &g_rd_reg_l, ctx);
        wl_display_roundtrip(ctx->wl_dpy);
        wl_display_roundtrip(ctx->wl_dpy);
    }
    if (ctx->wl_seat && ctx->vptr_mgr && !ctx->vptr) {
        ctx->vptr = wl_proxy_marshal_constructor(
            (struct wl_proxy *)ctx->vptr_mgr, 0,
            &dtportal_zwlr_virtual_pointer_v1_iface, ctx->wl_seat, NULL
        );
    }
    if (ctx->wl_seat && ctx->vkbd_mgr && !ctx->vkbd) {
        ctx->vkbd = wl_proxy_marshal_constructor(
            (struct wl_proxy *)ctx->vkbd_mgr, 0,
            &dtportal_zwp_virtual_keyboard_v1_iface, ctx->wl_seat, NULL
        );
        ctx->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_rule_names names = { .layout = "us" };
        ctx->xkb_keymap = xkb_keymap_new_from_names(
            ctx->xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS
        );
        if (ctx->xkb_keymap) {
            char *km_str = xkb_keymap_get_as_string(ctx->xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
            if (km_str) {
                size_t sz = strlen(km_str) + 1;
                int fd = memfd_create("dtportal-xkb", MFD_CLOEXEC);
                if (fd >= 0 && ftruncate(fd, (off_t)sz) == 0) {
                    void *map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    if (map != MAP_FAILED) {
                        memcpy(map, km_str, sz);
                        munmap(map, sz);
                        wl_proxy_marshal((struct wl_proxy *)ctx->vkbd, 0,
                                         1u /* WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 */, fd, (uint32_t)sz);
                    }
                }
                if (fd >= 0) close(fd);
                free(km_str);
            }
        }
    }
    if (ctx->wl_dpy) wl_display_roundtrip(ctx->wl_dpy);
    return ctx->vptr != NULL || ctx->vkbd != NULL;
}

void dtportal_cleanup_remote_input(DtPortalCtx *ctx) {
    if (!ctx) return;
    for (int i = 0; i < DTPORTAL_MAX_EIS_CLIENTS; ++i) {
        if (ctx->eis_clients[i].in_use) {
            if (ctx->eis_clients[i].io_watch_id) g_source_remove(ctx->eis_clients[i].io_watch_id);
            if (ctx->eis_clients[i].fd >= 0) close(ctx->eis_clients[i].fd);
            memset(&ctx->eis_clients[i], 0, sizeof(ctx->eis_clients[i]));
        }
    }
    if (ctx->xkb_keymap) { xkb_keymap_unref(ctx->xkb_keymap); ctx->xkb_keymap = NULL; }
    if (ctx->xkb_ctx) { xkb_context_unref(ctx->xkb_ctx); ctx->xkb_ctx = NULL; }
    if (ctx->wl_dpy) { wl_display_disconnect(ctx->wl_dpy); ctx->wl_dpy = NULL; }
    ctx->vptr = NULL;
    ctx->vkbd = NULL;
    ctx->vptr_mgr = NULL;
    ctx->vkbd_mgr = NULL;
    ctx->wl_seat = NULL;
}

static void inject_ptr_abs(DtPortalCtx *ctx, double x, double y) {
    if (!ensure_virtual_input_ready(ctx) || !ctx->vptr) return;
    uint32_t ext_w = ctx->output_w > 0 ? ctx->output_w : 1280u;
    uint32_t ext_h = ctx->output_h > 0 ? ctx->output_h : 900u;
    uint32_t ux = x < 0.0 ? 0u : ((uint32_t)lround(x) >= ext_w ? ext_w - 1u : (uint32_t)lround(x));
    uint32_t uy = y < 0.0 ? 0u : ((uint32_t)lround(y) >= ext_h ? ext_h - 1u : (uint32_t)lround(y));
    wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 1, monotonic_ms(), ux, uy, ext_w, ext_h);
    wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 4);
    wl_display_flush(ctx->wl_dpy);
}

static void inject_ptr_rel(DtPortalCtx *ctx, double dx, double dy) {
    if (!ensure_virtual_input_ready(ctx) || !ctx->vptr) return;
    wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 0, monotonic_ms(),
                     wl_fixed_from_double(dx), wl_fixed_from_double(dy));
    wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 4);
    wl_display_flush(ctx->wl_dpy);
}

static void inject_ptr_button(DtPortalCtx *ctx, uint32_t button, uint32_t state) {
    if (!ensure_virtual_input_ready(ctx) || !ctx->vptr) return;
    wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 2, monotonic_ms(), button, state ? 1u : 0u);
    wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 4);
    wl_display_flush(ctx->wl_dpy);
}

static void inject_kbd_keycode(DtPortalCtx *ctx, uint32_t evdev_keycode, uint32_t state) {
    if (!ensure_virtual_input_ready(ctx) || !ctx->vkbd) return;
    wl_proxy_marshal((struct wl_proxy *)ctx->vkbd, 1, monotonic_ms(), evdev_keycode, state ? 1u : 0u);
    /* Also update modifier mask for Ctrl/Alt/Shift/Super evdev keycodes */
    static uint32_t s_depressed = 0;
    uint32_t bit = 0;
    if (evdev_keycode == 42 || evdev_keycode == 54) bit = 1u;        /* Shift */
    else if (evdev_keycode == 29 || evdev_keycode == 97) bit = 4u;   /* Control */
    else if (evdev_keycode == 56 || evdev_keycode == 100) bit = 8u;  /* Mod1/Alt */
    else if (evdev_keycode == 125 || evdev_keycode == 126) bit = 64u;/* Mod4/Super */
    if (bit) {
        if (state) s_depressed |= bit;
        else s_depressed &= ~bit;
        wl_proxy_marshal((struct wl_proxy *)ctx->vkbd, 2, s_depressed, 0u, 0u, 0u);
    }
    wl_display_flush(ctx->wl_dpy);
}

static void inject_kbd_keysym(DtPortalCtx *ctx, xkb_keysym_t target_sym, uint32_t state) {
    if (!ensure_virtual_input_ready(ctx) || !ctx->vkbd || !ctx->xkb_keymap) return;
    xkb_keycode_t min_kc = xkb_keymap_min_keycode(ctx->xkb_keymap);
    xkb_keycode_t max_kc = xkb_keymap_max_keycode(ctx->xkb_keymap);
    for (xkb_keycode_t kc = min_kc; kc <= max_kc; ++kc) {
        const xkb_keysym_t *syms = NULL;
        int n = xkb_keymap_key_get_syms_by_level(ctx->xkb_keymap, kc, 0, 0, &syms);
        for (int i = 0; i < n; ++i) {
            if (syms[i] == target_sym && kc >= 8) {
                inject_kbd_keycode(ctx, (uint32_t)(kc - 8), state);
                return;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * 4. EIS v1 Binary Wire Protocol Server (ConnectToEIS)
 * ------------------------------------------------------------------------- */

#define EIS_OBJ_HANDSHAKE   0ULL
#define EIS_OBJ_CONNECTION  0xFF00000000000001ULL
#define EIS_OBJ_SEAT        0xFF00000000000002ULL
#define EIS_OBJ_DEVICE      0xFF00000000000003ULL
#define EIS_OBJ_PTR_ABS     0xFF00000000000004ULL
#define EIS_OBJ_BUTTON      0xFF00000000000005ULL
#define EIS_OBJ_KEYBOARD    0xFF00000000000006ULL

typedef struct {
    uint8_t data[1024];
    size_t len;
} EisPkt;

static void eis_pkt_begin(EisPkt *p, uint64_t obj_id, uint32_t opcode) {
    p->len = 16;
    memcpy(p->data, &obj_id, 8);
    uint32_t zero_len = 16;
    memcpy(p->data + 8, &zero_len, 4);
    memcpy(p->data + 12, &opcode, 4);
}

static void eis_pkt_u32(EisPkt *p, uint32_t v) {
    if (p->len + 4 <= sizeof(p->data)) {
        memcpy(p->data + p->len, &v, 4);
        p->len += 4;
    }
}

static void eis_pkt_u64(EisPkt *p, uint64_t v) {
    if (p->len + 8 <= sizeof(p->data)) {
        memcpy(p->data + p->len, &v, 8);
        p->len += 8;
    }
}

static void eis_pkt_f32(EisPkt *p, float v) {
    if (p->len + 4 <= sizeof(p->data)) {
        memcpy(p->data + p->len, &v, 4);
        p->len += 4;
    }
}

static void eis_pkt_str(EisPkt *p, const char *s) {
    uint32_t slen = s ? (uint32_t)strlen(s) + 1u : 0u;
    eis_pkt_u32(p, slen);
    if (slen > 0 && p->len + ((slen + 3u) & ~3u) <= sizeof(p->data)) {
        size_t padded = (slen + 3u) & ~3u;
        memset(p->data + p->len, 0, padded);
        memcpy(p->data + p->len, s, slen);
        p->len += padded;
    }
}

static void eis_pkt_send(int fd, EisPkt *p) {
    uint32_t total = (uint32_t)p->len;
    memcpy(p->data + 8, &total, 4);
    (void)send(fd, p->data, p->len, MSG_NOSIGNAL);
}

static void eis_send_initial_seat(DtPortalEisClient *ec) {
    EisPkt p;
    /* 1. ei_handshake.connection(serial=1, id=EIS_OBJ_CONNECTION, "ei_connection", ver=1) */
    eis_pkt_begin(&p, EIS_OBJ_HANDSHAKE, 1u);
    eis_pkt_u32(&p, ++ec->next_serial);
    eis_pkt_u64(&p, EIS_OBJ_CONNECTION);
    eis_pkt_str(&p, "ei_connection");
    eis_pkt_u32(&p, 1u);
    eis_pkt_send(ec->fd, &p);

    /* 2. ei_connection.seat(id=EIS_OBJ_SEAT, ver=1) */
    eis_pkt_begin(&p, EIS_OBJ_CONNECTION, 1u);
    eis_pkt_u64(&p, EIS_OBJ_SEAT);
    eis_pkt_u32(&p, 1u);
    eis_pkt_send(ec->fd, &p);

    /* 3. ei_seat.name("default"), capabilities, done() */
    eis_pkt_begin(&p, EIS_OBJ_SEAT, 1u);
    eis_pkt_str(&p, "default");
    eis_pkt_send(ec->fd, &p);

    const struct { uint64_t bit; const char *iface; } caps[] = {
        { 0x02ULL, "ei_pointer_absolute" },
        { 0x04ULL, "ei_button" },
        { 0x08ULL, "ei_keyboard" },
    };
    for (size_t i = 0; i < 3; ++i) {
        eis_pkt_begin(&p, EIS_OBJ_SEAT, 2u);
        eis_pkt_u64(&p, caps[i].bit);
        eis_pkt_str(&p, caps[i].iface);
        eis_pkt_send(ec->fd, &p);
    }
    eis_pkt_begin(&p, EIS_OBJ_SEAT, 3u); /* ei_seat.done */
    eis_pkt_send(ec->fd, &p);
}

static void eis_send_virtual_device(DtPortalEisClient *ec) {
    EisPkt p;
    uint32_t out_w = ec->ctx->output_w > 0 ? ec->ctx->output_w : 1280u;
    uint32_t out_h = ec->ctx->output_h > 0 ? ec->ctx->output_h : 900u;

    /* ei_seat.device(EIS_OBJ_DEVICE, ver=1) */
    eis_pkt_begin(&p, EIS_OBJ_SEAT, 4u);
    eis_pkt_u64(&p, EIS_OBJ_DEVICE);
    eis_pkt_u32(&p, 1u);
    eis_pkt_send(ec->fd, &p);

    /* ei_device.name("cde-eis-input"), device_type(1=VIRTUAL), region(0,0,w,h,1.0) */
    eis_pkt_begin(&p, EIS_OBJ_DEVICE, 1u);
    eis_pkt_str(&p, "cde-eis-input");
    eis_pkt_send(ec->fd, &p);

    eis_pkt_begin(&p, EIS_OBJ_DEVICE, 2u);
    eis_pkt_u32(&p, 1u);
    eis_pkt_send(ec->fd, &p);

    eis_pkt_begin(&p, EIS_OBJ_DEVICE, 4u);
    eis_pkt_u32(&p, 0u);
    eis_pkt_u32(&p, 0u);
    eis_pkt_u32(&p, out_w);
    eis_pkt_u32(&p, out_h);
    eis_pkt_f32(&p, 1.0f);
    eis_pkt_send(ec->fd, &p);

    /* Interfaces: ei_pointer_absolute, ei_button, ei_keyboard */
    const struct { uint64_t id; const char *name; } ifs[] = {
        { EIS_OBJ_PTR_ABS,  "ei_pointer_absolute" },
        { EIS_OBJ_BUTTON,   "ei_button" },
        { EIS_OBJ_KEYBOARD, "ei_keyboard" },
    };
    for (size_t i = 0; i < 3; ++i) {
        eis_pkt_begin(&p, EIS_OBJ_DEVICE, 5u);
        eis_pkt_u64(&p, ifs[i].id);
        eis_pkt_str(&p, ifs[i].name);
        eis_pkt_u32(&p, 1u);
        eis_pkt_send(ec->fd, &p);
    }

    /* ei_device.done() + ei_device.resumed(serial) */
    eis_pkt_begin(&p, EIS_OBJ_DEVICE, 6u);
    eis_pkt_send(ec->fd, &p);

    eis_pkt_begin(&p, EIS_OBJ_DEVICE, 7u);
    eis_pkt_u32(&p, ++ec->next_serial);
    eis_pkt_send(ec->fd, &p);
}

static gboolean on_eis_client_readable(GIOChannel *src, GIOCondition cond, gpointer user_data) {
    (void)src;
    DtPortalEisClient *ec = (DtPortalEisClient *)user_data;
    if (!ec || !ec->in_use) return FALSE;
    if (cond & (G_IO_ERR | G_IO_NVAL)) {
        close(ec->fd);
        ec->fd = -1;
        ec->io_watch_id = 0;
        ec->in_use = false;
        return FALSE;
    }

    ssize_t n = recv(ec->fd, ec->rx_buf + ec->rx_len, sizeof(ec->rx_buf) - ec->rx_len, MSG_DONTWAIT);
    if (n <= 0) {
        if (n < 0 && !(cond & G_IO_HUP) && (errno == EAGAIN || errno == EWOULDBLOCK)) return TRUE;
        close(ec->fd);
        ec->fd = -1;
        ec->io_watch_id = 0;
        ec->in_use = false;
        return FALSE;
    }
    ec->rx_len += (size_t)n;

    size_t off = 0;
    while (ec->rx_len - off >= 16) {
        uint64_t obj_id = 0;
        uint32_t msg_len = 0, opcode = 0;
        memcpy(&obj_id, ec->rx_buf + off, 8);
        memcpy(&msg_len, ec->rx_buf + off + 8, 4);
        memcpy(&opcode, ec->rx_buf + off + 12, 4);
        if (msg_len < 16 || msg_len > 2048) {
            /* Malformed packet -> disconnect EIS client safely */
            close(ec->fd);
            ec->fd = -1;
            ec->io_watch_id = 0;
            ec->in_use = false;
            return FALSE;
        }
        if (ec->rx_len - off < msg_len) break;

        const uint8_t *body = ec->rx_buf + off + 16;
        size_t body_len = msg_len - 16;

        if (obj_id == EIS_OBJ_HANDSHAKE && opcode == 1u && !ec->handshake_done) {
            /* ei_handshake.finish() */
            ec->handshake_done = true;
            ensure_virtual_input_ready(ec->ctx);
            eis_send_initial_seat(ec);
        } else if (obj_id == EIS_OBJ_SEAT && opcode == 1u && body_len >= 8) {
            /* ei_seat.bind(u64 capabilities) */
            uint64_t caps = 0;
            memcpy(&caps, body, 8);
            if (caps != 0) {
                eis_send_virtual_device(ec);
            }
        } else if (obj_id == EIS_OBJ_DEVICE && opcode == 1u) {
            /* ei_device.start_emulating */
            ec->device_emulating = true;
        } else if (obj_id == EIS_OBJ_DEVICE && opcode == 2u) {
            /* ei_device.stop_emulating */
            ec->device_emulating = false;
        } else if (obj_id == EIS_OBJ_PTR_ABS && opcode == 1u && body_len >= 8) {
            /* ei_pointer_absolute.motion_absolute(f32 x, f32 y) */
            float fx = 0.0f, fy = 0.0f;
            memcpy(&fx, body, 4);
            memcpy(&fy, body + 4, 4);
            inject_ptr_abs(ec->ctx, (double)fx, (double)fy);
        } else if (obj_id == EIS_OBJ_BUTTON && opcode == 1u && body_len >= 8) {
            /* ei_button.button(u32 button, u32 state) */
            uint32_t btn = 0, st = 0;
            memcpy(&btn, body, 4);
            memcpy(&st, body + 4, 4);
            inject_ptr_button(ec->ctx, btn, st);
        } else if (obj_id == EIS_OBJ_KEYBOARD && opcode == 1u && body_len >= 8) {
            /* ei_keyboard.key(u32 key, u32 state) */
            uint32_t key = 0, st = 0;
            memcpy(&key, body, 4);
            memcpy(&st, body + 4, 4);
            inject_kbd_keycode(ec->ctx, key, st);
        } else if (obj_id == EIS_OBJ_DEVICE && opcode == 3u) {
            /* ei_device.frame(u32 serial, u64 timestamp_us) */
            if (ec->ctx->wl_dpy) wl_display_flush(ec->ctx->wl_dpy);
        }

        off += msg_len;
    }
    if (off > 0) {
        memmove(ec->rx_buf, ec->rx_buf + off, ec->rx_len - off);
        ec->rx_len -= off;
    }
    return TRUE;
}

/* -------------------------------------------------------------------------
 * 5. RemoteDesktop Portal Methods & Restore Tokens
 * ------------------------------------------------------------------------- */

static void get_rd_token_file(char *out_path, size_t out_sz) {
    const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char dir[PATH_MAX];
    if (xdg_cfg && xdg_cfg[0]) snprintf(dir, sizeof(dir), "%s/ck-core", xdg_cfg);
    else if (home && home[0]) snprintf(dir, sizeof(dir), "%s/.config/ck-core", home);
    else snprintf(dir, sizeof(dir), "/var/tmp");
    mkdir_p_dir(dir);
    snprintf(out_path, out_sz, "%s/remotedesktop-tokens.conf", dir);
}

static uint32_t lookup_rd_restore_token(const char *token, const char *app_id) {
    if (!token || !token[0]) return 0;
    char path[PATH_MAX];
    get_rd_token_file(path, sizeof(path));
    GKeyFile *kf = g_key_file_new();
    uint32_t devs = 0;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *saved_app = g_key_file_get_string(kf, token, "app_id", NULL);
        if (saved_app && (!app_id || strcmp(saved_app, app_id) == 0)) {
            devs = (uint32_t)g_key_file_get_integer(kf, token, "devices", NULL);
        }
        g_free(saved_app);
    }
    g_key_file_free(kf);
    return devs;
}

static void persist_rd_restore_token(const char *token, const char *app_id, uint32_t devs) {
    if (!token || !token[0]) return;
    char path[PATH_MAX];
    get_rd_token_file(path, sizeof(path));
    GKeyFile *kf = g_key_file_new();
    (void)g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_string(kf, token, "app_id", app_id ? app_id : "");
    g_key_file_set_integer(kf, token, "devices", (gint)devs);
    (void)g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
}

static void complete_rd_start(DtPortalSession *sess, GDBusMethodInvocation *invocation,
                              uint32_t response, bool mint_token) {
    GVariantBuilder res;
    g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));
    if (response == 0 && sess) {
        sess->started = true;
        ensure_virtual_input_ready(sess->ctx);
        g_variant_builder_add(&res, "{sv}", "devices", g_variant_new_uint32(sess->device_types));
        g_variant_builder_add(&res, "{sv}", "clipboard_enabled", g_variant_new_boolean(FALSE));
        if (mint_token && sess->persist_mode_allow) {
            char *uuid = g_uuid_string_random();
            snprintf(sess->restore_token, sizeof(sess->restore_token), "%s", uuid);
            g_free(uuid);
            persist_rd_restore_token(sess->restore_token, sess->app_id, sess->device_types);
            g_variant_builder_add(&res, "{sv}", "restore_token",
                                  g_variant_new_string(sess->restore_token));
        }
    }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", response, &res));
}

static void on_rd_allow(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    DtPortalRequest *req = (DtPortalRequest *)client_data;
    if (!req || !req->in_use) return;
    DtPortalSession *sess = dtportal_find_session(req->ctx, req->session_path);
    bool remember = true;
    if (req->remember_toggle) {
        remember = XmToggleButtonGetState(req->remember_toggle) ? true : false;
    }
    if (req->invocation) {
        complete_rd_start(sess, req->invocation, 0u, remember);
        req->invocation = NULL;
    }
    DtPortalCtx *ctx = req->ctx;
    dtportal_free_request(req);
    dtportal_write_test_state(ctx);
}

static void on_rd_deny(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    DtPortalRequest *req = (DtPortalRequest *)client_data;
    if (!req || !req->in_use) return;
    if (req->invocation) {
        complete_rd_start(NULL, req->invocation, 1u, false);
        req->invocation = NULL;
    }
    DtPortalCtx *ctx = req->ctx;
    dtportal_free_request(req);
    dtportal_write_test_state(ctx);
}

void dtportal_handle_remote_desktop(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
) {
    const char *sender = g_dbus_method_invocation_get_sender(invocation);

    if (strcmp(method_name, "CreateSession") == 0) {
        const char *handle = NULL, *session_handle = NULL, *app_id = NULL;
        GVariant *options = NULL;
        g_variant_get(parameters, "(&o&o&s@a{sv})", &handle, &session_handle, &app_id, &options);
        (void)handle;
        g_variant_unref(options);

        DtPortalSession *sess = dtportal_alloc_session(
            ctx, DTPORTAL_SESS_REMOTE_DESKTOP, session_handle, app_id, sender
        );
        GVariantBuilder res;
        g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));
        if (!sess) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 2u, &res));
            return;
        }
        sess->device_types = 3u; /* default KEYBOARD | POINTER */
        g_variant_builder_add(&res, "{sv}", "session_id", g_variant_new_string(sess->session_key));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 0u, &res));
        return;
    }

    if (strcmp(method_name, "SelectDevices") == 0) {
        const char *handle = NULL, *session_handle = NULL, *app_id = NULL;
        GVariant *options = NULL;
        g_variant_get(parameters, "(&o&o&s@a{sv})", &handle, &session_handle, &app_id, &options);
        (void)handle;

        DtPortalSession *sess = dtportal_find_session(ctx, session_handle);
        GVariantBuilder res;
        g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));
        if (!sess || sess->kind != DTPORTAL_SESS_REMOTE_DESKTOP) {
            g_variant_unref(options);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 2u, &res));
            return;
        }
        guint32 types = 3u, persist_mode = 0u;
        if (g_variant_lookup(options, "types", "u", &types) && (types & 7u) != 0) {
            sess->device_types = types & 7u;
        }
        if (g_variant_lookup(options, "persist_mode", "u", &persist_mode)) {
            sess->persist_mode_allow = (persist_mode >= 1u);
        }
        const char *rtok = NULL;
        if (g_variant_lookup(options, "restore_token", "&s", &rtok) && rtok && rtok[0]) {
            snprintf(sess->restore_token, sizeof(sess->restore_token), "%s", rtok);
        }
        g_variant_unref(options);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 0u, &res));
        return;
    }

    if (strcmp(method_name, "Start") == 0) {
        const char *handle = NULL, *session_handle = NULL, *app_id = NULL, *parent_win = NULL;
        GVariant *options = NULL;
        g_variant_get(parameters, "(&o&o&s&s@a{sv})",
                      &handle, &session_handle, &app_id, &parent_win, &options);
        (void)parent_win;
        g_variant_unref(options);

        DtPortalSession *sess = dtportal_find_session(ctx, session_handle);
        if (!sess || sess->kind != DTPORTAL_SESS_REMOTE_DESKTOP) {
            GVariantBuilder empty;
            g_variant_builder_init(&empty, G_VARIANT_TYPE("a{sv}"));
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 2u, &empty));
            return;
        }

        /* Check persisted restore_token first */
        uint32_t restored_devs = lookup_rd_restore_token(sess->restore_token, sess->app_id);
        if (restored_devs != 0) {
            sess->device_types = restored_devs;
            complete_rd_start(sess, invocation, 0u, true);
            return;
        }

        /* Otherwise present native Motif Remote Desktop authorization dialog */
        DtPortalRequest *req = dtportal_alloc_request(
            ctx, DTPORTAL_REQ_REMOTEDESKTOP_START, handle, app_id, invocation
        );
        if (!req) {
            complete_rd_start(NULL, invocation, 2u, false);
            return;
        }
        snprintf(req->session_path, sizeof(req->session_path), "%s", session_handle);

        char msg_txt[512];
        snprintf(msg_txt, sizeof(msg_txt),
                 "Allow \"%s\" to inject remote pointer and keyboard input into CDE?",
                 (app_id && app_id[0]) ? app_id : "Remote Desktop Client");
        XmString xmsg = XmStringCreateLocalized(msg_txt);
        XmString xtitle = XmStringCreateLocalized("CDE Remote Desktop Access");
        XmString xok = XmStringCreateLocalized("Allow");
        XmString xcancel = XmStringCreateLocalized("Deny");
        Arg args[8];
        Cardinal n = 0;
        XtSetArg(args[n], XmNmessageString, xmsg); n++;
        XtSetArg(args[n], XmNdialogTitle, xtitle); n++;
        XtSetArg(args[n], XmNokLabelString, xok); n++;
        XtSetArg(args[n], XmNcancelLabelString, xcancel); n++;
        req->dialog = XmCreateWarningDialog(ctx->top_shell, "cdeRemoteDesktopConfirm", args, n);
        XmStringFree(xmsg);
        XmStringFree(xtitle);
        XmStringFree(xok);
        XmStringFree(xcancel);
        req->shell = XtParent(req->dialog);
        Widget hbtn = XmMessageBoxGetChild(req->dialog, XmDIALOG_HELP_BUTTON);
        if (hbtn) XtUnmanageChild(hbtn);

        if (sess->persist_mode_allow) {
            XmString xrem = XmStringCreateLocalized("Remember this remote-control permission");
            req->remember_toggle = XtVaCreateManagedWidget(
                "rememberToggle", xmToggleButtonWidgetClass, req->dialog,
                XmNlabelString, xrem,
                XmNset, True,
                NULL
            );
            XmStringFree(xrem);
        }

        XtAddCallback(req->dialog, XmNokCallback, on_rd_allow, req);
        XtAddCallback(req->dialog, XmNcancelCallback, on_rd_deny, req);
        XtManageChild(req->dialog);
        XFlush(ctx->dpy);
        dtportal_write_test_state(ctx);
        return;
    }

    if (strcmp(method_name, "ConnectToEIS") == 0) {
        const char *session_handle = NULL, *app_id = NULL;
        GVariant *options = NULL;
        g_variant_get(parameters, "(&o&s@a{sv})", &session_handle, &app_id, &options);
        (void)app_id;
        g_variant_unref(options);

        DtPortalSession *sess = dtportal_find_session(ctx, session_handle);
        if (!sess || sess->kind != DTPORTAL_SESS_REMOTE_DESKTOP || !sess->started) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.freedesktop.portal.Error.Failed", "RemoteDesktop session not started"
            );
            return;
        }

        int sv[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.freedesktop.portal.Error.Failed", "socketpair failed"
            );
            return;
        }

        DtPortalEisClient *ec = NULL;
        for (int i = 0; i < DTPORTAL_MAX_EIS_CLIENTS; ++i) {
            if (!ctx->eis_clients[i].in_use) {
                ec = &ctx->eis_clients[i];
                break;
            }
        }
        if (!ec) {
            close(sv[0]);
            close(sv[1]);
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.freedesktop.portal.Error.Failed", "Too many EIS clients"
            );
            return;
        }

        memset(ec, 0, sizeof(*ec));
        ec->in_use = true;
        ec->fd = sv[0];
        ec->ctx = ctx;
        snprintf(ec->session_path, sizeof(ec->session_path), "%s", session_handle);

        /* Immediately greet client with ei_handshake.handshake_version(1) */
        EisPkt greet;
        eis_pkt_begin(&greet, EIS_OBJ_HANDSHAKE, 0u);
        eis_pkt_u32(&greet, 1u);
        eis_pkt_send(ec->fd, &greet);

        GIOChannel *ch = g_io_channel_unix_new(ec->fd);
        g_io_channel_set_encoding(ch, NULL, NULL);
        g_io_channel_set_buffered(ch, FALSE);
        ec->io_watch_id = g_io_add_watch(
            ch, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, on_eis_client_readable, ec
        );
        g_io_channel_unref(ch);

        GUnixFDList *fd_list = g_unix_fd_list_new();
        GError *ferr = NULL;
        gint fd_idx = g_unix_fd_list_append(fd_list, sv[1], &ferr);
        close(sv[1]); /* g_unix_fd_list_append dups sv[1] */
        if (fd_idx < 0) {
            if (ferr) g_error_free(ferr);
            g_object_unref(fd_list);
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.freedesktop.portal.Error.Failed", "FD append failed"
            );
            return;
        }
        g_dbus_method_invocation_return_value_with_unix_fd_list(
            invocation, g_variant_new("(h)", fd_idx), fd_list
        );
        g_object_unref(fd_list);
        return;
    }

    if (strcmp(method_name, "NotifyPointerMotion") == 0) {
        const char *sh = NULL; GVariant *opts = NULL; gdouble dx = 0, dy = 0;
        g_variant_get(parameters, "(&o@a{sv}dd)", &sh, &opts, &dx, &dy);
        g_variant_unref(opts);
        DtPortalSession *s = dtportal_find_session(ctx, sh);
        if (s && s->started && (s->device_types & 2u)) inject_ptr_rel(ctx, dx, dy);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "NotifyPointerMotionAbsolute") == 0) {
        const char *sh = NULL; GVariant *opts = NULL; guint32 stream = 0; gdouble x = 0, y = 0;
        g_variant_get(parameters, "(&o@a{sv}udd)", &sh, &opts, &stream, &x, &y);
        (void)stream;
        g_variant_unref(opts);
        DtPortalSession *s = dtportal_find_session(ctx, sh);
        if (s && s->started && (s->device_types & 2u)) inject_ptr_abs(ctx, x, y);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "NotifyPointerButton") == 0) {
        const char *sh = NULL; GVariant *opts = NULL; gint32 btn = 0; guint32 st = 0;
        g_variant_get(parameters, "(&o@a{sv}iu)", &sh, &opts, &btn, &st);
        g_variant_unref(opts);
        DtPortalSession *s = dtportal_find_session(ctx, sh);
        if (s && s->started && (s->device_types & 2u)) inject_ptr_button(ctx, (uint32_t)btn, st);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "NotifyPointerAxis") == 0) {
        const char *sh = NULL; GVariant *opts = NULL; gdouble dx = 0, dy = 0;
        g_variant_get(parameters, "(&o@a{sv}dd)", &sh, &opts, &dx, &dy);
        g_variant_unref(opts);
        DtPortalSession *s = dtportal_find_session(ctx, sh);
        if (s && s->started && (s->device_types & 2u) && ensure_virtual_input_ready(ctx) && ctx->vptr) {
            if (fabs(dy) > 1e-4) {
                wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 3, monotonic_ms(), 0u, wl_fixed_from_double(dy));
            }
            if (fabs(dx) > 1e-4) {
                wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 3, monotonic_ms(), 1u, wl_fixed_from_double(dx));
            }
            wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 4);
            wl_display_flush(ctx->wl_dpy);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "NotifyPointerAxisDiscrete") == 0) {
        const char *sh = NULL; GVariant *opts = NULL; guint32 axis = 0; gint32 steps = 0;
        g_variant_get(parameters, "(&o@a{sv}ui)", &sh, &opts, &axis, &steps);
        g_variant_unref(opts);
        DtPortalSession *s = dtportal_find_session(ctx, sh);
        if (s && s->started && (s->device_types & 2u) && ensure_virtual_input_ready(ctx) && ctx->vptr) {
            wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 7, monotonic_ms(), axis,
                             wl_fixed_from_double((double)steps * 15.0), steps);
            wl_proxy_marshal((struct wl_proxy *)ctx->vptr, 4);
            wl_display_flush(ctx->wl_dpy);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "NotifyKeyboardKeycode") == 0) {
        const char *sh = NULL; GVariant *opts = NULL; gint32 kc = 0; guint32 st = 0;
        g_variant_get(parameters, "(&o@a{sv}iu)", &sh, &opts, &kc, &st);
        g_variant_unref(opts);
        DtPortalSession *s = dtportal_find_session(ctx, sh);
        if (s && s->started && (s->device_types & 1u) && kc >= 0) {
            inject_kbd_keycode(ctx, (uint32_t)kc, st);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "NotifyKeyboardKeysym") == 0) {
        const char *sh = NULL; GVariant *opts = NULL; gint32 sym = 0; guint32 st = 0;
        g_variant_get(parameters, "(&o@a{sv}iu)", &sh, &opts, &sym, &st);
        g_variant_unref(opts);
        DtPortalSession *s = dtportal_find_session(ctx, sh);
        if (s && s->started && (s->device_types & 1u)) {
            inject_kbd_keysym(ctx, (xkb_keysym_t)sym, st);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "NotifyTouchDown") == 0 || strcmp(method_name, "NotifyTouchMotion") == 0) {
        const char *sh = NULL; GVariant *opts = NULL; guint32 stream = 0, slot = 0; gdouble x = 0, y = 0;
        g_variant_get(parameters, "(&o@a{sv}uudd)", &sh, &opts, &stream, &slot, &x, &y);
        (void)stream; (void)slot;
        g_variant_unref(opts);
        DtPortalSession *s = dtportal_find_session(ctx, sh);
        if (s && s->started && (s->device_types & 4u)) {
            inject_ptr_abs(ctx, x, y);
            if (strcmp(method_name, "NotifyTouchDown") == 0) inject_ptr_button(ctx, 272u, 1u);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "NotifyTouchUp") == 0) {
        const char *sh = NULL; GVariant *opts = NULL; guint32 slot = 0;
        g_variant_get(parameters, "(&o@a{sv}u)", &sh, &opts, &slot);
        (void)slot;
        g_variant_unref(opts);
        DtPortalSession *s = dtportal_find_session(ctx, sh);
        if (s && s->started && (s->device_types & 4u)) inject_ptr_button(ctx, 272u, 0u);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown RemoteDesktop method"
    );
}
