/*
 * CDE - Common Desktop Environment
 * cde-color-sync: Cross-toolkit Dark / Light / High-Contrast synchronization
 * across GSettings, GTK3/GTK4 settings.ini, X11 XSETTINGS (_XSETTINGS_S0),
 * and D-Bus XDG Settings Portal (org.freedesktop.appearance color-scheme)
 * for both CDE (Wayland) and CDE (X11 / XLibre).
 */

#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    CDE_SCHEME_LIGHT = 0,
    CDE_SCHEME_DARK = 1,
    CDE_SCHEME_HIGH_CONTRAST = 2,
} CdeScheme;

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int s) { (void)s; g_quit = 1; }

static CdeScheme parse_scheme(const char *s) {
    if (!s || !s[0]) return CDE_SCHEME_LIGHT;
    if (strcasecmp(s, "dark") == 0 || strcasecmp(s, "modern_dark") == 0 ||
        strcasecmp(s, "ModernDark") == 0 || strcasecmp(s, "ModernDark.dp") == 0 ||
        strcasecmp(s, "NordicNight") == 0 || strcasecmp(s, "NordicNight.dp") == 0 ||
        strcasecmp(s, "ObsidianTeal") == 0 || strcasecmp(s, "ObsidianTeal.dp") == 0 ||
        strcasecmp(s, "EspressoDark") == 0 || strcasecmp(s, "EspressoDark.dp") == 0 ||
        strcasecmp(s, "Black") == 0 || strcasecmp(s, "Black.dp") == 0 ||
        strcasecmp(s, "prefer-dark") == 0) {
        return CDE_SCHEME_DARK;
    }
    if (strcasecmp(s, "high_contrast") == 0 || strcasecmp(s, "high-contrast") == 0 ||
        strcasecmp(s, "HighContrast") == 0 || strcasecmp(s, "HighContrast.dp") == 0) {
        return CDE_SCHEME_HIGH_CONTRAST;
    }
    return CDE_SCHEME_LIGHT;
}

static const char *scheme_mode_str(CdeScheme sc) {
    switch (sc) {
    case CDE_SCHEME_DARK: return "prefer-dark";
    case CDE_SCHEME_HIGH_CONTRAST: return "high-contrast";
    case CDE_SCHEME_LIGHT:
    default: return "prefer-light";
    }
}

static uint32_t scheme_portal_u32(CdeScheme sc) {
    /* org.freedesktop.appearance color-scheme: 1 = prefer-dark, 2 = prefer-light */
    return (sc == CDE_SCHEME_DARK) ? 1u : 2u;
}

static uint32_t scheme_contrast_u32(CdeScheme sc) {
    /* org.freedesktop.appearance contrast: 1 = high, 0 = normal */
    return (sc == CDE_SCHEME_HIGH_CONTRAST) ? 1u : 0u;
}

static const char *resolve_gtk_theme_for_scheme(CdeScheme sc) {
    const char *override_theme = getenv("CK_CDE_GTK_THEME_BASE");
    if (override_theme && override_theme[0]) {
        if (sc == CDE_SCHEME_DARK) {
            static char dark_buf[128];
            snprintf(dark_buf, sizeof(dark_buf), "%s-dark", override_theme);
            return dark_buf;
        }
        if (sc == CDE_SCHEME_HIGH_CONTRAST) return "HighContrast";
        return override_theme;
    }
    if (sc == CDE_SCHEME_HIGH_CONTRAST) {
        return "HighContrast";
    }
    if (sc == CDE_SCHEME_DARK) {
        if (access("/usr/share/themes/Yaru-dark", F_OK) == 0) return "Yaru-dark";
        return "Adwaita-dark";
    }
    if (access("/usr/share/themes/Yaru", F_OK) == 0) return "Yaru";
    return "Adwaita";
}

static void mkdir_p(const char *dir) {
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

static void update_gtk_settings_ini(const char *cfg_subdir, CdeScheme sc, const char *gtk_theme) {
    const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
    if (xdg_cfg && xdg_cfg[0]) {
        snprintf(dir, sizeof(dir), "%s/%s", xdg_cfg, cfg_subdir);
    } else if (home && home[0]) {
        snprintf(dir, sizeof(dir), "%s/.config/%s", home, cfg_subdir);
    } else {
        return;
    }
    mkdir_p(dir);
    snprintf(path, sizeof(path), "%s/settings.ini", dir);
    snprintf(tmp, sizeof(tmp), "%s/.settings.ini.%d.tmp", dir, (int)getpid());

    /* Preserve unrelated keys in [Settings] if settings.ini already exists */
    char preserved[8192];
    size_t pres_len = 0;
    preserved[0] = '\0';

    FILE *in = fopen(path, "r");
    if (in) {
        char line[512];
        while (fgets(line, sizeof(line), in)) {
            char *s = line;
            while (*s && isspace((unsigned char)*s)) s++;
            if (strncmp(s, "[Settings]", 10) == 0 ||
                strncmp(s, "gtk-application-prefer-dark-theme", 33) == 0 ||
                strncmp(s, "gtk-theme-name", 14) == 0) {
                continue;
            }
            size_t l = strlen(line);
            if (pres_len + l + 1 < sizeof(preserved)) {
                memcpy(preserved + pres_len, line, l);
                pres_len += l;
                preserved[pres_len] = '\0';
            }
        }
        fclose(in);
    }

    FILE *out = fopen(tmp, "w");
    if (!out) return;
    fprintf(out, "[Settings]\n");
    fprintf(out, "gtk-application-prefer-dark-theme=%d\n", (sc == CDE_SCHEME_DARK) ? 1 : 0);
    fprintf(out, "gtk-theme-name=%s\n", gtk_theme);
    if (pres_len > 0) {
        fputs(preserved, out);
    }
    if (fclose(out) == 0) {
        (void)rename(tmp, path);
    } else {
        unlink(tmp);
    }
}

static void run_quiet(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > 2) close(dn);
        }
        execvp(argv[0], argv);
        _exit(127);
    } else if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
    }
}

static void apply_gsettings(CdeScheme sc, const char *gtk_theme) {
    if (access("/usr/bin/gsettings", X_OK) != 0) return;
    const char *cs = (sc == CDE_SCHEME_DARK) ? "prefer-dark" : "prefer-light";
    char *av1[] = {"/usr/bin/gsettings", "set", "org.gnome.desktop.interface", "color-scheme", (char *)cs, NULL};
    char *av2[] = {"/usr/bin/gsettings", "set", "org.gnome.desktop.interface", "gtk-theme", (char *)gtk_theme, NULL};
    run_quiet(av1);
    run_quiet(av2);
}

/* -------------------------------------------------------------------------
 * Binary X11 _XSETTINGS_S0 (_XSETTINGS_SETTINGS) builder & publisher
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t data[2048];
    size_t len;
} XSettingsBuf;

static void xs_put_u8(XSettingsBuf *b, uint8_t v) {
    if (b->len + 1 <= sizeof(b->data)) b->data[b->len++] = v;
}
static void xs_put_u16(XSettingsBuf *b, uint16_t v) {
    if (b->len + 2 <= sizeof(b->data)) {
        memcpy(b->data + b->len, &v, 2);
        b->len += 2;
    }
}
static void xs_put_u32(XSettingsBuf *b, uint32_t v) {
    if (b->len + 4 <= sizeof(b->data)) {
        memcpy(b->data + b->len, &v, 4);
        b->len += 4;
    }
}
static void xs_put_bytes_padded(XSettingsBuf *b, const char *s, size_t n) {
    size_t pad = (4u - (n & 3u)) & 3u;
    if (b->len + n + pad > sizeof(b->data)) return;
    if (n > 0) memcpy(b->data + b->len, s, n);
    b->len += n;
    for (size_t i = 0; i < pad; ++i) b->data[b->len++] = 0;
}

static void xs_add_string(XSettingsBuf *b, const char *name, const char *val, uint32_t serial) {
    uint16_t nlen = (uint16_t)strlen(name);
    uint32_t vlen = (uint32_t)strlen(val);
    xs_put_u8(b, 1); /* XSETTINGS_TYPE_STRING */
    xs_put_u8(b, 0);
    xs_put_u16(b, nlen);
    xs_put_bytes_padded(b, name, nlen);
    xs_put_u32(b, serial);
    xs_put_u32(b, vlen);
    xs_put_bytes_padded(b, val, vlen);
}

static void xs_add_int(XSettingsBuf *b, const char *name, int32_t val, uint32_t serial) {
    uint16_t nlen = (uint16_t)strlen(name);
    xs_put_u8(b, 0); /* XSETTINGS_TYPE_INT */
    xs_put_u8(b, 0);
    xs_put_u16(b, nlen);
    xs_put_bytes_padded(b, name, nlen);
    xs_put_u32(b, serial);
    xs_put_u32(b, (uint32_t)val);
}

static int x11_ignore_err(Display *dpy, XErrorEvent *ev) {
    (void)dpy; (void)ev;
    return 0;
}

typedef struct {
    Display *dpy;
    Window root;
    Window sel_win;
    Atom atom_sel;
    Atom atom_settings;
    Atom atom_mgr;
    Atom atom_cde_scheme;
    Atom atom_cde_theme;
    uint32_t serial;
} XSettingsOwner;

static void xsettings_publish(XSettingsOwner *xo, CdeScheme sc, const char *gtk_theme) {
    if (!xo || !xo->dpy) return;
    xo->serial++;

    const char *mode_s = scheme_mode_str(sc);
    XChangeProperty(xo->dpy, xo->root, xo->atom_cde_scheme, XA_STRING, 8,
                    PropModeReplace, (const unsigned char *)mode_s, (int)strlen(mode_s));
    XChangeProperty(xo->dpy, xo->root, xo->atom_cde_theme, XA_STRING, 8,
                    PropModeReplace, (const unsigned char *)gtk_theme, (int)strlen(gtk_theme));

    if (xo->sel_win != None && XGetSelectionOwner(xo->dpy, xo->atom_sel) == xo->sel_win) {
        XSettingsBuf xb;
        memset(&xb, 0, sizeof(xb));
        uint16_t endian_test = 1;
        uint8_t byte_order = (*(uint8_t *)&endian_test == 1) ? 0 : 1; /* 0 = LSBFirst */
        xs_put_u8(&xb, byte_order);
        xs_put_u8(&xb, 0);
        xs_put_u8(&xb, 0);
        xs_put_u8(&xb, 0);
        xs_put_u32(&xb, xo->serial);
        xs_put_u32(&xb, 3); /* n_settings */

        xs_add_string(&xb, "Net/ThemeName", gtk_theme, xo->serial);
        xs_add_int(&xb, "Gtk/ApplicationPreferDarkTheme", (sc == CDE_SCHEME_DARK) ? 1 : 0, xo->serial);
        xs_add_int(&xb, "Net/ApplicationPreferDarkTheme", (sc == CDE_SCHEME_DARK) ? 1 : 0, xo->serial);

        XChangeProperty(xo->dpy, xo->sel_win, xo->atom_settings, xo->atom_settings, 8,
                        PropModeReplace, xb.data, (int)xb.len);
    }
    XFlush(xo->dpy);
}

static bool xsettings_init(XSettingsOwner *xo) {
    memset(xo, 0, sizeof(*xo));
    const char *disp = getenv("DISPLAY");
    if (!disp || !disp[0]) return false;
    xo->dpy = XOpenDisplay(NULL);
    if (!xo->dpy) return false;
    XSetErrorHandler(x11_ignore_err);

    int scr = DefaultScreen(xo->dpy);
    xo->root = RootWindow(xo->dpy, scr);
    char sel_name[32];
    snprintf(sel_name, sizeof(sel_name), "_XSETTINGS_S%d", scr);
    xo->atom_sel = XInternAtom(xo->dpy, sel_name, False);
    xo->atom_settings = XInternAtom(xo->dpy, "_XSETTINGS_SETTINGS", False);
    xo->atom_mgr = XInternAtom(xo->dpy, "MANAGER", False);
    xo->atom_cde_scheme = XInternAtom(xo->dpy, "_CDE_COLOR_SCHEME", False);
    xo->atom_cde_theme = XInternAtom(xo->dpy, "_CDE_GTK_THEME", False);

    /* Only claim _XSETTINGS_S0 if no external settings daemon already owns it */
    if (XGetSelectionOwner(xo->dpy, xo->atom_sel) == None) {
        xo->sel_win = XCreateSimpleWindow(xo->dpy, xo->root, -100, -100, 1, 1, 0, 0, 0);
        XSetSelectionOwner(xo->dpy, xo->atom_sel, xo->sel_win, CurrentTime);
        if (XGetSelectionOwner(xo->dpy, xo->atom_sel) == xo->sel_win) {
            XEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.xclient.type = ClientMessage;
            ev.xclient.window = xo->root;
            ev.xclient.message_type = xo->atom_mgr;
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = CurrentTime;
            ev.xclient.data.l[1] = (long)xo->atom_sel;
            ev.xclient.data.l[2] = (long)xo->sel_win;
            XSendEvent(xo->dpy, xo->root, False, StructureNotifyMask, &ev);
        }
    }
    XFlush(xo->dpy);
    return true;
}

/* -------------------------------------------------------------------------
 * Minimal libdbus-1 dynamic bindings via dlopen("libdbus-1.so.3")
 * ------------------------------------------------------------------------- */
typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
typedef struct {
    const char *name;
    const char *message;
    unsigned int dummy1 : 1;
    unsigned int dummy2 : 1;
    unsigned int dummy3 : 1;
    unsigned int dummy4 : 1;
    unsigned int dummy5 : 1;
    void *padding1;
} DBusError;

typedef struct {
    void *dummy1;
    void *dummy2;
    uint32_t dummy3;
    int dummy4, dummy5, dummy6, dummy7, dummy8, dummy9, dummy10, dummy11;
    int pad1;
    void *pad2;
    void *pad3;
} DBusMessageIter;

typedef int (*DBusHandleMessageFunction)(DBusConnection *, DBusMessage *, void *);

#define DBUS_BUS_SESSION 0
#define DBUS_NAME_FLAG_ALLOW_REPLACEMENT 0x1
#define DBUS_NAME_FLAG_REPLACE_EXISTING 0x2
#define DBUS_NAME_FLAG_DO_NOT_QUEUE 0x4
#define DBUS_HANDLER_RESULT_HANDLED 0
#define DBUS_HANDLER_RESULT_NOT_YET_HANDLED 1
#define DBUS_TYPE_STRING ((int)'s')
#define DBUS_TYPE_UINT32 ((int)'u')
#define DBUS_TYPE_VARIANT ((int)'v')
#define DBUS_TYPE_ARRAY ((int)'a')
#define DBUS_TYPE_DICT_ENTRY ((int)'e')

typedef struct {
    void *handle;
    void (*error_init)(DBusError *);
    void (*error_free)(DBusError *);
    int (*error_is_set)(const DBusError *);
    DBusConnection *(*bus_get)(int, DBusError *);
    int (*bus_request_name)(DBusConnection *, const char *, unsigned int, DBusError *);
    int (*conn_add_filter)(DBusConnection *, DBusHandleMessageFunction, void *, void *);
    int (*conn_read_write_dispatch)(DBusConnection *, int);
    int (*conn_send)(DBusConnection *, DBusMessage *, uint32_t *);
    DBusMessage *(*conn_send_with_reply_and_block)(DBusConnection *, DBusMessage *, int, DBusError *);
    void (*conn_flush)(DBusConnection *);
    int (*msg_is_method_call)(DBusMessage *, const char *, const char *);
    DBusMessage *(*msg_new_method_call)(const char *, const char *, const char *, const char *);
    DBusMessage *(*msg_new_method_return)(DBusMessage *);
    DBusMessage *(*msg_new_error)(DBusMessage *, const char *, const char *);
    DBusMessage *(*msg_new_signal)(const char *, const char *, const char *);
    void (*msg_unref)(DBusMessage *);
    int (*msg_get_args)(DBusMessage *, DBusError *, int, ...);
    void (*iter_init_append)(DBusMessage *, DBusMessageIter *);
    int (*iter_append_basic)(DBusMessageIter *, int, const void *);
    int (*iter_open_container)(DBusMessageIter *, int, const char *, DBusMessageIter *);
    int (*iter_close_container)(DBusMessageIter *, DBusMessageIter *);
} DbusApi;

static bool load_dbus_api(DbusApi *api) {
    memset(api, 0, sizeof(*api));
    api->handle = dlopen("libdbus-1.so.3", RTLD_LAZY);
    if (!api->handle) api->handle = dlopen("libdbus-1.so", RTLD_LAZY);
    if (!api->handle) return false;

    api->error_init = dlsym(api->handle, "dbus_error_init");
    api->error_free = dlsym(api->handle, "dbus_error_free");
    api->error_is_set = dlsym(api->handle, "dbus_error_is_set");
    api->bus_get = dlsym(api->handle, "dbus_bus_get");
    api->bus_request_name = dlsym(api->handle, "dbus_bus_request_name");
    api->conn_add_filter = dlsym(api->handle, "dbus_connection_add_filter");
    api->conn_read_write_dispatch = dlsym(api->handle, "dbus_connection_read_write_dispatch");
    api->conn_send = dlsym(api->handle, "dbus_connection_send");
    api->conn_send_with_reply_and_block = dlsym(api->handle, "dbus_connection_send_with_reply_and_block");
    api->conn_flush = dlsym(api->handle, "dbus_connection_flush");
    api->msg_is_method_call = dlsym(api->handle, "dbus_message_is_method_call");
    api->msg_new_method_call = dlsym(api->handle, "dbus_message_new_method_call");
    api->msg_new_method_return = dlsym(api->handle, "dbus_message_new_method_return");
    api->msg_new_error = dlsym(api->handle, "dbus_message_new_error");
    api->msg_new_signal = dlsym(api->handle, "dbus_message_new_signal");
    api->msg_unref = dlsym(api->handle, "dbus_message_unref");
    api->msg_get_args = dlsym(api->handle, "dbus_message_get_args");
    api->iter_init_append = dlsym(api->handle, "dbus_message_iter_init_append");
    api->iter_append_basic = dlsym(api->handle, "dbus_message_iter_append_basic");
    api->iter_open_container = dlsym(api->handle, "dbus_message_iter_open_container");
    api->iter_close_container = dlsym(api->handle, "dbus_message_iter_close_container");

    return api->error_init && api->bus_get && api->bus_request_name &&
           api->conn_add_filter && api->conn_read_write_dispatch && api->conn_send &&
           api->conn_send_with_reply_and_block && api->conn_flush &&
           api->msg_is_method_call && api->msg_new_method_call &&
           api->msg_new_method_return && api->msg_new_error &&
           api->msg_new_signal && api->msg_unref && api->msg_get_args &&
           api->iter_init_append && api->iter_append_basic &&
           api->iter_open_container && api->iter_close_container;
}

typedef struct {
    DbusApi db;
    DBusConnection *conn;
    XSettingsOwner xso;
    CdeScheme scheme;
    char gtk_theme[128];
} DaemonCtx;

static void emit_portal_setting_u32(DaemonCtx *ctx, const char *iface, const char *ns, const char *key, uint32_t val) {
    DBusMessage *sig = ctx->db.msg_new_signal("/org/freedesktop/portal/desktop", iface, "SettingChanged");
    if (!sig) return;
    DBusMessageIter it, vit;
    ctx->db.iter_init_append(sig, &it);
    ctx->db.iter_append_basic(&it, DBUS_TYPE_STRING, &ns);
    ctx->db.iter_append_basic(&it, DBUS_TYPE_STRING, &key);
    ctx->db.iter_open_container(&it, DBUS_TYPE_VARIANT, "u", &vit);
    ctx->db.iter_append_basic(&vit, DBUS_TYPE_UINT32, &val);
    ctx->db.iter_close_container(&it, &vit);
    ctx->db.conn_send(ctx->conn, sig, NULL);
    ctx->db.msg_unref(sig);
}

static void daemon_set_scheme(DaemonCtx *ctx, CdeScheme sc, bool broadcast_signals) {
    ctx->scheme = sc;
    snprintf(ctx->gtk_theme, sizeof(ctx->gtk_theme), "%s", resolve_gtk_theme_for_scheme(sc));
    update_gtk_settings_ini("gtk-3.0", sc, ctx->gtk_theme);
    update_gtk_settings_ini("gtk-4.0", sc, ctx->gtk_theme);
    apply_gsettings(sc, ctx->gtk_theme);

    if (!ctx->xso.dpy && getenv("DISPLAY")) {
        (void)xsettings_init(&ctx->xso);
    }
    xsettings_publish(&ctx->xso, sc, ctx->gtk_theme);

    if (broadcast_signals && ctx->conn) {
        uint32_t cs_val = scheme_portal_u32(sc);
        uint32_t ct_val = scheme_contrast_u32(sc);
        emit_portal_setting_u32(ctx, "org.freedesktop.impl.portal.Settings", "org.freedesktop.appearance", "color-scheme", cs_val);
        emit_portal_setting_u32(ctx, "org.freedesktop.impl.portal.Settings", "org.freedesktop.appearance", "contrast", ct_val);
        emit_portal_setting_u32(ctx, "org.freedesktop.portal.Settings", "org.freedesktop.appearance", "color-scheme", cs_val);
        emit_portal_setting_u32(ctx, "org.freedesktop.portal.Settings", "org.freedesktop.appearance", "contrast", ct_val);
        DBusMessage *asig = ctx->db.msg_new_signal("/org/cde/Appearance1", "org.cde.Appearance1", "SchemeChanged");
        if (asig) {
            const char *ms = scheme_mode_str(sc);
            DBusMessageIter ait;
            ctx->db.iter_init_append(asig, &ait);
            ctx->db.iter_append_basic(&ait, DBUS_TYPE_STRING, &ms);
            ctx->db.iter_append_basic(&ait, DBUS_TYPE_UINT32, &cs_val);
            ctx->db.iter_append_basic(&ait, DBUS_TYPE_UINT32, &ct_val);
            ctx->db.conn_send(ctx->conn, asig, NULL);
            ctx->db.msg_unref(asig);
        }
        ctx->db.conn_flush(ctx->conn);
    }
}

static void append_dict_u32(DbusApi *db, DBusMessageIter *dict_it, const char *key, uint32_t val) {
    DBusMessageIter entry_it, var_it;
    db->iter_open_container(dict_it, DBUS_TYPE_DICT_ENTRY, NULL, &entry_it);
    db->iter_append_basic(&entry_it, DBUS_TYPE_STRING, &key);
    db->iter_open_container(&entry_it, DBUS_TYPE_VARIANT, "u", &var_it);
    db->iter_append_basic(&var_it, DBUS_TYPE_UINT32, &val);
    db->iter_close_container(&entry_it, &var_it);
    db->iter_close_container(dict_it, &entry_it);
}

static int dbus_filter_cb(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    DaemonCtx *ctx = (DaemonCtx *)user_data;
    DBusError err;
    ctx->db.error_init(&err);

    if (ctx->db.msg_is_method_call(msg, "org.cde.Appearance1", "SetScheme")) {
        const char *mode = NULL;
        if (ctx->db.msg_get_args(msg, &err, DBUS_TYPE_STRING, &mode, 0) && mode) {
            daemon_set_scheme(ctx, parse_scheme(mode), true);
            DBusMessage *rep = ctx->db.msg_new_method_return(msg);
            if (rep) {
                const char *ms = scheme_mode_str(ctx->scheme);
                const char *gt = ctx->gtk_theme;
                uint32_t pv = scheme_portal_u32(ctx->scheme);
                DBusMessageIter it;
                ctx->db.iter_init_append(rep, &it);
                ctx->db.iter_append_basic(&it, DBUS_TYPE_STRING, &ms);
                ctx->db.iter_append_basic(&it, DBUS_TYPE_STRING, &gt);
                ctx->db.iter_append_basic(&it, DBUS_TYPE_UINT32, &pv);
                ctx->db.conn_send(conn, rep, NULL);
                ctx->db.msg_unref(rep);
            }
            ctx->db.error_free(&err);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        ctx->db.error_free(&err);
    }

    if (ctx->db.msg_is_method_call(msg, "org.cde.Appearance1", "GetScheme")) {
        DBusMessage *rep = ctx->db.msg_new_method_return(msg);
        if (rep) {
            const char *ms = scheme_mode_str(ctx->scheme);
            const char *gt = ctx->gtk_theme;
            uint32_t pv = scheme_portal_u32(ctx->scheme);
            DBusMessageIter it;
            ctx->db.iter_init_append(rep, &it);
            ctx->db.iter_append_basic(&it, DBUS_TYPE_STRING, &ms);
            ctx->db.iter_append_basic(&it, DBUS_TYPE_STRING, &gt);
            ctx->db.iter_append_basic(&it, DBUS_TYPE_UINT32, &pv);
            ctx->db.conn_send(conn, rep, NULL);
            ctx->db.msg_unref(rep);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    bool is_impl_read = ctx->db.msg_is_method_call(msg, "org.freedesktop.impl.portal.Settings", "Read");
    bool is_front_read = ctx->db.msg_is_method_call(msg, "org.freedesktop.portal.Settings", "Read");
    if (is_impl_read || is_front_read) {
        const char *ns = NULL, *key = NULL;
        if (ctx->db.msg_get_args(msg, &err, DBUS_TYPE_STRING, &ns, DBUS_TYPE_STRING, &key, 0) && ns && key) {
            if (strcmp(ns, "org.freedesktop.appearance") == 0 &&
                (strcmp(key, "color-scheme") == 0 || strcmp(key, "contrast") == 0)) {
                uint32_t val = (strcmp(key, "color-scheme") == 0)
                                   ? scheme_portal_u32(ctx->scheme)
                                   : scheme_contrast_u32(ctx->scheme);
                DBusMessage *rep = ctx->db.msg_new_method_return(msg);
                if (rep) {
                    DBusMessageIter it, v1, v2;
                    ctx->db.iter_init_append(rep, &it);
                    if (is_front_read) {
                        /* Frontend portal Read returns variant(variant(uint32)) */
                        ctx->db.iter_open_container(&it, DBUS_TYPE_VARIANT, "v", &v1);
                        ctx->db.iter_open_container(&v1, DBUS_TYPE_VARIANT, "u", &v2);
                        ctx->db.iter_append_basic(&v2, DBUS_TYPE_UINT32, &val);
                        ctx->db.iter_close_container(&v1, &v2);
                        ctx->db.iter_close_container(&it, &v1);
                    } else {
                        /* Backend impl.portal.Settings.Read returns variant(uint32) */
                        ctx->db.iter_open_container(&it, DBUS_TYPE_VARIANT, "u", &v1);
                        ctx->db.iter_append_basic(&v1, DBUS_TYPE_UINT32, &val);
                        ctx->db.iter_close_container(&it, &v1);
                    }
                    ctx->db.conn_send(conn, rep, NULL);
                    ctx->db.msg_unref(rep);
                }
                ctx->db.error_free(&err);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            DBusMessage *erep = ctx->db.msg_new_error(
                msg, "org.freedesktop.portal.Error.NotFound", "Requested setting not found");
            if (erep) {
                ctx->db.conn_send(conn, erep, NULL);
                ctx->db.msg_unref(erep);
            }
            ctx->db.error_free(&err);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        ctx->db.error_free(&err);
    }

    if (ctx->db.msg_is_method_call(msg, "org.freedesktop.impl.portal.Settings", "ReadAll") ||
        ctx->db.msg_is_method_call(msg, "org.freedesktop.portal.Settings", "ReadAll")) {
        DBusMessage *rep = ctx->db.msg_new_method_return(msg);
        if (rep) {
            const char *ns = "org.freedesktop.appearance";
            DBusMessageIter it, outer_arr, outer_entry, inner_arr;
            ctx->db.iter_init_append(rep, &it);
            ctx->db.iter_open_container(&it, DBUS_TYPE_ARRAY, "{sa{sv}}", &outer_arr);
            ctx->db.iter_open_container(&outer_arr, DBUS_TYPE_DICT_ENTRY, NULL, &outer_entry);
            ctx->db.iter_append_basic(&outer_entry, DBUS_TYPE_STRING, &ns);
            ctx->db.iter_open_container(&outer_entry, DBUS_TYPE_ARRAY, "{sv}", &inner_arr);
            append_dict_u32(&ctx->db, &inner_arr, "color-scheme", scheme_portal_u32(ctx->scheme));
            append_dict_u32(&ctx->db, &inner_arr, "contrast", scheme_contrast_u32(ctx->scheme));
            ctx->db.iter_close_container(&outer_entry, &inner_arr);
            ctx->db.iter_close_container(&outer_arr, &outer_entry);
            ctx->db.iter_close_container(&it, &outer_arr);
            ctx->db.conn_send(conn, rep, NULL);
            ctx->db.msg_unref(rep);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (ctx->db.msg_is_method_call(msg, "org.freedesktop.DBus.Properties", "Get")) {
        const char *iface = NULL, *prop = NULL;
        if (ctx->db.msg_get_args(msg, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, 0) &&
            prop && strcmp(prop, "version") == 0) {
            uint32_t ver = 1u;
            DBusMessage *rep = ctx->db.msg_new_method_return(msg);
            if (rep) {
                DBusMessageIter it, vit;
                ctx->db.iter_init_append(rep, &it);
                ctx->db.iter_open_container(&it, DBUS_TYPE_VARIANT, "u", &vit);
                ctx->db.iter_append_basic(&vit, DBUS_TYPE_UINT32, &ver);
                ctx->db.iter_close_container(&it, &vit);
                ctx->db.conn_send(conn, rep, NULL);
                ctx->db.msg_unref(rep);
            }
            ctx->db.error_free(&err);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        ctx->db.error_free(&err);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int run_daemon(CdeScheme initial_scheme) {
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    DaemonCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    (void)xsettings_init(&ctx.xso);

    if (load_dbus_api(&ctx.db) && getenv("DBUS_SESSION_BUS_ADDRESS")) {
        DBusError err;
        ctx.db.error_init(&err);
        ctx.conn = ctx.db.bus_get(DBUS_BUS_SESSION, &err);
        ctx.db.error_free(&err);
        if (ctx.conn) {
            ctx.db.conn_add_filter(ctx.conn, dbus_filter_cb, &ctx, NULL);
            ctx.db.error_init(&err);
            ctx.db.bus_request_name(ctx.conn, "org.cde.Appearance1",
                                    DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
            ctx.db.error_free(&err);

            ctx.db.error_init(&err);
            ctx.db.bus_request_name(ctx.conn, "org.freedesktop.impl.portal.desktop.cde",
                                    DBUS_NAME_FLAG_ALLOW_REPLACEMENT | DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
            ctx.db.error_free(&err);

            /* Only claim org.freedesktop.portal.desktop when xdg-desktop-portal is not running */
            ctx.db.error_init(&err);
            ctx.db.bus_request_name(ctx.conn, "org.freedesktop.portal.desktop",
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
            ctx.db.error_free(&err);
        }
    }

    daemon_set_scheme(&ctx, initial_scheme, true);

    while (!g_quit) {
        if (ctx.conn) {
            ctx.db.conn_read_write_dispatch(ctx.conn, 150);
        } else {
            usleep(150000);
        }
        if (ctx.xso.dpy) {
            while (XPending(ctx.xso.dpy)) {
                XEvent xev;
                XNextEvent(ctx.xso.dpy, &xev);
                if (xev.type == SelectionClear && xev.xselectionclear.selection == ctx.xso.atom_sel) {
                    ctx.xso.sel_win = None;
                }
            }
        }
    }

    if (ctx.xso.dpy) XCloseDisplay(ctx.xso.dpy);
    return 0;
}

static bool send_scheme_to_daemon(const char *self_path, const char *mode_arg) {
    DbusApi db;
    if (!load_dbus_api(&db) || !getenv("DBUS_SESSION_BUS_ADDRESS")) return false;
    DBusError err;
    db.error_init(&err);
    DBusConnection *conn = db.bus_get(DBUS_BUS_SESSION, &err);
    db.error_free(&err);
    if (!conn) return false;

    for (int attempt = 0; attempt < 2; ++attempt) {
        DBusMessage *call = db.msg_new_method_call(
            "org.cde.Appearance1", "/org/cde/Appearance1", "org.cde.Appearance1", "SetScheme");
        if (!call) return false;
        DBusMessageIter it;
        db.iter_init_append(call, &it);
        db.iter_append_basic(&it, DBUS_TYPE_STRING, &mode_arg);

        db.error_init(&err);
        DBusMessage *rep = db.conn_send_with_reply_and_block(conn, call, 1200, &err);
        db.msg_unref(call);
        if (rep && !db.error_is_set(&err)) {
            db.msg_unref(rep);
            db.error_free(&err);
            return true;
        }
        db.error_free(&err);

        if (attempt == 0) {
            /* Auto-spawn cde-color-sync --daemon in background so XSETTINGS + Portal stay owned */
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                int dn = open("/dev/null", O_RDWR);
                if (dn >= 0) {
                    dup2(dn, STDIN_FILENO);
                    dup2(dn, STDOUT_FILENO);
                    dup2(dn, STDERR_FILENO);
                    if (dn > 2) close(dn);
                }
                execl(self_path, "cde-color-sync", "--daemon", mode_arg, (char *)NULL);
                execlp("cde-color-sync", "cde-color-sync", "--daemon", mode_arg, (char *)NULL);
                execl(CDE_INSTALLATION_TOP "/bin/cde-color-sync", "cde-color-sync", "--daemon", mode_arg, (char *)NULL);
                _exit(127);
            }
            usleep(220000);
        }
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--daemon") == 0) {
        CdeScheme init_sc = (argc >= 3) ? parse_scheme(argv[2]) : CDE_SCHEME_LIGHT;
        return run_daemon(init_sc);
    }

    if (argc >= 3 && strcmp(argv[1], "--apply") == 0) {
        CdeScheme sc = parse_scheme(argv[2]);
        const char *gtk_theme = resolve_gtk_theme_for_scheme(sc);
        update_gtk_settings_ini("gtk-3.0", sc, gtk_theme);
        update_gtk_settings_ini("gtk-4.0", sc, gtk_theme);
        apply_gsettings(sc, gtk_theme);

        if (!send_scheme_to_daemon(argv[0], argv[2])) {
            /* Direct one-shot X11 root property publish when D-Bus session is absent */
            XSettingsOwner xso;
            if (xsettings_init(&xso)) {
                xsettings_publish(&xso, sc, gtk_theme);
                XCloseDisplay(xso.dpy);
            }
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--query") == 0) {
        DbusApi db;
        if (load_dbus_api(&db) && getenv("DBUS_SESSION_BUS_ADDRESS")) {
            DBusError err;
            db.error_init(&err);
            DBusConnection *conn = db.bus_get(DBUS_BUS_SESSION, &err);
            db.error_free(&err);
            if (conn) {
                DBusMessage *call = db.msg_new_method_call(
                    "org.cde.Appearance1", "/org/cde/Appearance1", "org.cde.Appearance1", "GetScheme");
                if (call) {
                    db.error_init(&err);
                    DBusMessage *rep = db.conn_send_with_reply_and_block(conn, call, 800, &err);
                    db.msg_unref(call);
                    if (rep && !db.error_is_set(&err)) {
                        const char *ms = "", *gt = "";
                        uint32_t pv = 0;
                        if (db.msg_get_args(rep, &err, DBUS_TYPE_STRING, &ms, DBUS_TYPE_STRING, &gt, DBUS_TYPE_UINT32, &pv, 0)) {
                            printf("scheme=%s\ngtk_theme=%s\nportal_color_scheme=%u\n", ms, gt, pv);
                            db.msg_unref(rep);
                            db.error_free(&err);
                            return 0;
                        }
                        db.msg_unref(rep);
                    }
                    db.error_free(&err);
                }
            }
        }
        return 1;
    }

    fprintf(stderr, "Usage: cde-color-sync [--daemon [light|dark|high_contrast] | --apply <light|dark|high_contrast> | --query]\n");
    return 2;
}
