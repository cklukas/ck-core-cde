/*
 * CDE - Common Desktop Environment
 *
 * dtportal_main.c: Entry point, XtAppMainLoop <-> GMainContext pump, D-Bus
 * introspection/object registration (org.freedesktop.impl.portal.desktop.cde),
 * Request & Session object lifecycles, and dtwm-wayland .ipc stream watcher.
 */

#define _GNU_SOURCE
#include "dtportal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <Dt/Action.h>
#include <Dt/Dt.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/Display.h>
#include <Xm/Wayland.h>

static volatile sig_atomic_t g_stop_portal = 0;
static void on_portal_sig(int s) {
    (void)s;
    g_stop_portal = 1;
}

static const gchar PORTAL_DESKTOP_INTROSPECTION_XML[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.FileChooser'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='OpenFile'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SaveFile'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SaveFiles'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "  <interface name='org.freedesktop.impl.portal.AppChooser'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='ChooseApplication'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='as' name='choices' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='UpdateChoices'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='as' name='choices' direction='in'/>"
    "    </method>"
    "  </interface>"
    "  <interface name='org.freedesktop.impl.portal.Inhibit'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='Inhibit'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='window' direction='in'/>"
    "      <arg type='u' name='flags' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "    </method>"
    "    <method name='CreateMonitor'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='window' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "    </method>"
    "    <method name='QueryEndResponse'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "    </method>"
    "    <signal name='StateChanged'>"
    "      <arg type='o' name='session_handle'/>"
    "      <arg type='a{sv}' name='state'/>"
    "    </signal>"
    "  </interface>"
    "  <interface name='org.freedesktop.impl.portal.Wallpaper'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='SetWallpaperURI'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='uri' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "    </method>"
    "  </interface>"
    "  <interface name='org.freedesktop.impl.portal.GlobalShortcuts'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='CreateSession'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='BindShortcuts'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a(sa{sv})' name='shortcuts' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='ListShortcuts'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='ConfigureShortcuts'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "    </method>"
    "    <signal name='Activated'>"
    "      <arg type='o' name='session_handle'/>"
    "      <arg type='s' name='shortcut_id'/>"
    "      <arg type='t' name='timestamp'/>"
    "      <arg type='a{sv}' name='options'/>"
    "    </signal>"
    "    <signal name='Deactivated'>"
    "      <arg type='o' name='session_handle'/>"
    "      <arg type='s' name='shortcut_id'/>"
    "      <arg type='t' name='timestamp'/>"
    "      <arg type='a{sv}' name='options'/>"
    "    </signal>"
    "    <signal name='ShortcutsChanged'>"
    "      <arg type='o' name='session_handle'/>"
    "      <arg type='a(sa{sv})' name='shortcuts'/>"
    "    </signal>"
    "  </interface>"
    "  <interface name='org.freedesktop.impl.portal.RemoteDesktop'>"
    "    <property name='version' type='u' access='read'/>"
    "    <property name='AvailableDeviceTypes' type='u' access='read'/>"
    "    <method name='CreateSession'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SelectDevices'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='Start'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='NotifyPointerMotion'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='d' name='dx' direction='in'/>"
    "      <arg type='d' name='dy' direction='in'/>"
    "    </method>"
    "    <method name='NotifyPointerMotionAbsolute'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='stream' direction='in'/>"
    "      <arg type='d' name='x' direction='in'/>"
    "      <arg type='d' name='y' direction='in'/>"
    "    </method>"
    "    <method name='NotifyPointerButton'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='i' name='button' direction='in'/>"
    "      <arg type='u' name='state' direction='in'/>"
    "    </method>"
    "    <method name='NotifyPointerAxis'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='d' name='dx' direction='in'/>"
    "      <arg type='d' name='dy' direction='in'/>"
    "    </method>"
    "    <method name='NotifyPointerAxisDiscrete'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='axis' direction='in'/>"
    "      <arg type='i' name='steps' direction='in'/>"
    "    </method>"
    "    <method name='NotifyKeyboardKeycode'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='i' name='keycode' direction='in'/>"
    "      <arg type='u' name='state' direction='in'/>"
    "    </method>"
    "    <method name='NotifyKeyboardKeysym'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='i' name='keysym' direction='in'/>"
    "      <arg type='u' name='state' direction='in'/>"
    "    </method>"
    "    <method name='NotifyTouchDown'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='stream' direction='in'/>"
    "      <arg type='u' name='slot' direction='in'/>"
    "      <arg type='d' name='x' direction='in'/>"
    "      <arg type='d' name='y' direction='in'/>"
    "    </method>"
    "    <method name='NotifyTouchMotion'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='stream' direction='in'/>"
    "      <arg type='u' name='slot' direction='in'/>"
    "      <arg type='d' name='x' direction='in'/>"
    "      <arg type='d' name='y' direction='in'/>"
    "    </method>"
    "    <method name='NotifyTouchUp'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='slot' direction='in'/>"
    "    </method>"
    "    <method name='ConnectToEIS'>"
    "      <annotation name='org.gtk.GDBus.C.UnixFD' value='true'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='h' name='fd' direction='out'/>"
    "    </method>"
    "  </interface>"
    "  <interface name='org.freedesktop.impl.portal.Settings'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='ReadAll'>"
    "      <arg type='as' name='namespaces' direction='in'/>"
    "      <arg type='a{sa{sv}}' name='value' direction='out'/>"
    "    </method>"
    "    <method name='Read'>"
    "      <arg type='s' name='namespace' direction='in'/>"
    "      <arg type='s' name='key' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "    <method name='ReadOne'>"
    "      <arg type='s' name='namespace' direction='in'/>"
    "      <arg type='s' name='key' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "    <signal name='SettingChanged'>"
    "      <arg type='s' name='namespace'/>"
    "      <arg type='s' name='key'/>"
    "      <arg type='v' name='value'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static const gchar REQUEST_INTROSPECTION_XML[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Request'>"
    "    <method name='Close'/>"
    "  </interface>"
    "</node>";

static const gchar SESSION_INTROSPECTION_XML[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Session'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='Close'/>"
    "    <signal name='Closed'/>"
    "  </interface>"
    "</node>";

static GDBusNodeInfo *g_desktop_node = NULL;
static GDBusNodeInfo *g_request_node = NULL;
static GDBusNodeInfo *g_session_node = NULL;

/* -------------------------------------------------------------------------
 * Request & Session Object Lifecycles
 * ------------------------------------------------------------------------- */

static void on_request_method_call(
    GDBusConnection *conn, const gchar *sender, const gchar *object_path,
    const gchar *interface_name, const gchar *method_name,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data
) {
    (void)conn; (void)sender; (void)object_path; (void)interface_name; (void)parameters;
    DtPortalRequest *req = (DtPortalRequest *)user_data;
    if (strcmp(method_name, "Close") == 0 && req && req->in_use) {
        DtPortalCtx *ctx = req->ctx;
        DtPortalReqKind kind = req->kind;
        if (req->invocation) {
            if (kind == DTPORTAL_REQ_WALLPAPER) {
                g_dbus_method_invocation_return_value(req->invocation, g_variant_new("(u)", 2u));
            } else {
                GVariantBuilder empty;
                g_variant_builder_init(&empty, G_VARIANT_TYPE("a{sv}"));
                g_dbus_method_invocation_return_value(req->invocation, g_variant_new("(ua{sv})", 2u, &empty));
            }
            req->invocation = NULL;
        }
        dtportal_free_request(req);
        if (kind == DTPORTAL_REQ_INHIBIT) {
            dtportal_recompute_inhibit_state(ctx);
        }
        dtportal_write_test_state(ctx);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable g_request_vtable = {
    .method_call = on_request_method_call,
    .get_property = NULL,
    .set_property = NULL,
};

static void on_req_sender_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    (void)conn; (void)name;
    DtPortalRequest *req = (DtPortalRequest *)user_data;
    if (!req || !req->in_use) return;
    DtPortalCtx *ctx = req->ctx;
    DtPortalReqKind kind = req->kind;
    dtportal_free_request(req);
    if (kind == DTPORTAL_REQ_INHIBIT) {
        dtportal_recompute_inhibit_state(ctx);
    }
    dtportal_write_test_state(ctx);
}

DtPortalRequest *dtportal_alloc_request(
    DtPortalCtx *ctx,
    DtPortalReqKind kind,
    const char *handle_path,
    const char *app_id,
    GDBusMethodInvocation *invocation
) {
    if (!ctx || !handle_path || !handle_path[0]) return NULL;
    for (int i = 0; i < DTPORTAL_MAX_REQUESTS; ++i) {
        DtPortalRequest *req = &ctx->requests[i];
        if (req->in_use) continue;
        memset(req, 0, sizeof(*req));
        req->in_use = true;
        req->ctx = ctx;
        req->kind = kind;
        snprintf(req->handle_path, sizeof(req->handle_path), "%s", handle_path);
        snprintf(req->app_id, sizeof(req->app_id), "%s", app_id ? app_id : "");
        req->invocation = invocation;
        const char *sender = invocation ? g_dbus_method_invocation_get_sender(invocation) : NULL;
        if (sender && sender[0]) {
            snprintf(req->sender, sizeof(req->sender), "%s", sender);
            req->watch_id = g_bus_watch_name_on_connection(
                ctx->bus, sender, G_BUS_NAME_WATCHER_FLAGS_NONE,
                NULL, on_req_sender_vanished, req, NULL
            );
        }
        if (ctx->bus && g_request_node) {
            req->reg_id = g_dbus_connection_register_object(
                ctx->bus, handle_path, g_request_node->interfaces[0],
                &g_request_vtable, req, NULL, NULL
            );
        }
        return req;
    }
    return NULL;
}

void dtportal_free_request(DtPortalRequest *req) {
    if (!req || !req->in_use) return;
    DtPortalCtx *ctx = req->ctx;
    if (req->watch_id) {
        g_bus_unwatch_name(req->watch_id);
        req->watch_id = 0;
    }
    if (req->reg_id && ctx && ctx->bus) {
        g_dbus_connection_unregister_object(ctx->bus, req->reg_id);
        req->reg_id = 0;
    }
    for (int i = 0; i < req->filter_count; ++i) {
        if (req->filters[i].raw_tuple) {
            g_variant_unref(req->filters[i].raw_tuple);
            req->filters[i].raw_tuple = NULL;
        }
    }
    if (req->dialog) {
        XtUnmanageChild(req->dialog);
        if (req->shell) XtDestroyWidget(req->shell);
        else XtDestroyWidget(req->dialog);
        req->dialog = NULL;
        req->shell = NULL;
        if (ctx && ctx->dpy) XFlush(ctx->dpy);
    }
    memset(req, 0, sizeof(*req));
}

DtPortalRequest *dtportal_find_request(DtPortalCtx *ctx, const char *handle_path) {
    if (!ctx || !handle_path) return NULL;
    for (int i = 0; i < DTPORTAL_MAX_REQUESTS; ++i) {
        if (ctx->requests[i].in_use && strcmp(ctx->requests[i].handle_path, handle_path) == 0) {
            return &ctx->requests[i];
        }
    }
    return NULL;
}

static void on_session_method_call(
    GDBusConnection *conn, const gchar *sender, const gchar *object_path,
    const gchar *interface_name, const gchar *method_name,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data
) {
    (void)conn; (void)sender; (void)object_path; (void)interface_name; (void)parameters;
    DtPortalSession *sess = (DtPortalSession *)user_data;
    if (strcmp(method_name, "Close") == 0 && sess && sess->in_use) {
        dtportal_close_session(sess, false);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *on_session_get_prop(
    GDBusConnection *conn, const gchar *sender, const gchar *object_path,
    const gchar *interface_name, const gchar *property_name,
    GError **error, gpointer user_data
) {
    (void)conn; (void)sender; (void)object_path; (void)interface_name; (void)error; (void)user_data;
    if (strcmp(property_name, "version") == 0) return g_variant_new_uint32(1u);
    return NULL;
}

static const GDBusInterfaceVTable g_session_vtable = {
    .method_call = on_session_method_call,
    .get_property = on_session_get_prop,
    .set_property = NULL,
};

static void on_sess_sender_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    (void)conn; (void)name;
    DtPortalSession *sess = (DtPortalSession *)user_data;
    if (sess && sess->in_use) {
        dtportal_close_session(sess, false);
    }
}

DtPortalSession *dtportal_alloc_session(
    DtPortalCtx *ctx,
    DtPortalSessionKind kind,
    const char *session_path,
    const char *app_id,
    const char *sender
) {
    if (!ctx || !session_path || !session_path[0]) return NULL;
    static unsigned int s_seq = 0;
    for (int i = 0; i < DTPORTAL_MAX_SESSIONS; ++i) {
        DtPortalSession *sess = &ctx->sessions[i];
        if (sess->in_use) continue;
        memset(sess, 0, sizeof(*sess));
        sess->in_use = true;
        sess->ctx = ctx;
        sess->kind = kind;
        snprintf(sess->session_path, sizeof(sess->session_path), "%s", session_path);
        snprintf(sess->session_key, sizeof(sess->session_key), "s%u", ++s_seq);
        snprintf(sess->app_id, sizeof(sess->app_id), "%s", app_id ? app_id : "");
        if (sender && sender[0]) {
            snprintf(sess->sender, sizeof(sess->sender), "%s", sender);
            sess->watch_id = g_bus_watch_name_on_connection(
                ctx->bus, sender, G_BUS_NAME_WATCHER_FLAGS_NONE,
                NULL, on_sess_sender_vanished, sess, NULL
            );
        }
        if (ctx->bus && g_session_node) {
            sess->reg_id = g_dbus_connection_register_object(
                ctx->bus, session_path, g_session_node->interfaces[0],
                &g_session_vtable, sess, NULL, NULL
            );
        }
        return sess;
    }
    return NULL;
}

void dtportal_close_session(DtPortalSession *sess, bool emit_closed_signal) {
    if (!sess || !sess->in_use) return;
    DtPortalCtx *ctx = sess->ctx;
    if (sess->kind == DTPORTAL_SESS_GLOBAL_SHORTCUTS) {
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "f.unregister_portal_session %s", sess->session_key);
        dtportal_send_wm_request(ctx, cmd);
    }
    if (emit_closed_signal && ctx && ctx->bus) {
        g_dbus_connection_emit_signal(
            ctx->bus,
            sess->sender[0] ? sess->sender : NULL,
            sess->session_path,
            "org.freedesktop.impl.portal.Session",
            "Closed",
            NULL,
            NULL
        );
    }
    if (sess->watch_id) {
        g_bus_unwatch_name(sess->watch_id);
        sess->watch_id = 0;
    }
    if (sess->reg_id && ctx && ctx->bus) {
        g_dbus_connection_unregister_object(ctx->bus, sess->reg_id);
        sess->reg_id = 0;
    }
    memset(sess, 0, sizeof(*sess));
}

DtPortalSession *dtportal_find_session(DtPortalCtx *ctx, const char *session_path) {
    if (!ctx || !session_path) return NULL;
    for (int i = 0; i < DTPORTAL_MAX_SESSIONS; ++i) {
        if (ctx->sessions[i].in_use && strcmp(ctx->sessions[i].session_path, session_path) == 0) {
            return &ctx->sessions[i];
        }
    }
    return NULL;
}

DtPortalSession *dtportal_find_session_by_key(DtPortalCtx *ctx, const char *session_key) {
    if (!ctx || !session_key) return NULL;
    for (int i = 0; i < DTPORTAL_MAX_SESSIONS; ++i) {
        if (ctx->sessions[i].in_use && strcmp(ctx->sessions[i].session_key, session_key) == 0) {
            return &ctx->sessions[i];
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Compositor IPC (_DT_WM_REQUEST & .ipc watcher) + Activation Token Helper
 * ------------------------------------------------------------------------- */

void dtportal_send_wm_request(DtPortalCtx *ctx, const char *cmd) {
    if (!ctx || !cmd || !cmd[0]) return;
    size_t len = strlen(cmd) + 1;

    /* 1. Native Wayland dtwm-wayland .ipc socket if connected */
    if (ctx->wl_ipc_fd >= 0) {
        char *hex = g_malloc(len * 2 + 1);
        for (size_t i = 0; i < len; ++i) {
            sprintf(hex + i * 2, "%02x", (unsigned char)cmd[i]);
        }
        char *line = g_strdup_printf("SET _DT_WM_REQUEST %s\n", hex);
        ssize_t wr = send(ctx->wl_ipc_fd, line, strlen(line), MSG_NOSIGNAL);
        g_free(line);
        g_free(hex);
        if (wr > 0) return;
    }

    /* 2. X11 / Motif virtual-root _DT_WM_REQUEST property fallback */
    if (ctx->dpy) {
        Window root = DefaultRootWindow(ctx->dpy);
        Atom a_info = XInternAtom(ctx->dpy, "_MOTIF_WM_INFO", False);
        Atom a_req = XInternAtom(ctx->dpy, "_DT_WM_REQUEST", False);
        Window target = root;
        Atom act = None;
        int fmt = 0;
        unsigned long nitems = 0, rem = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(ctx->dpy, root, a_info, 0, 2, False, AnyPropertyType,
                               &act, &fmt, &nitems, &rem, &prop) == Success && prop) {
            if (fmt == 32 && nitems >= 2) {
                Window wm_win = (Window)((unsigned long *)prop)[1];
                if (wm_win != None) target = wm_win;
            }
            XFree(prop);
        }
        XChangeProperty(ctx->dpy, target, a_req, XA_STRING, 8, PropModeAppend,
                        (const unsigned char *)cmd, (int)len);
        XFlush(ctx->dpy);
    }
}

char *dtportal_mint_activation_token(DtPortalCtx *ctx, const char *app_id) {
    if (!ctx || !ctx->dpy || !ctx->is_wayland) return NULL;
    char *xt_tok = NULL;
    if (XmWaylandRequestActivationToken(ctx->dpy, None, app_id, &xt_tok) && xt_tok) {
        char *dup = g_strdup(xt_tok);
        XtFree(xt_tok);
        return dup;
    }
    return NULL;
}

static void connect_wayland_ipc_if_present(DtPortalCtx *ctx) {
    ctx->wl_ipc_fd = -1;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    const char *wd = getenv("WAYLAND_DISPLAY");
    if (!rt || !rt[0] || !wd || !wd[0]) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    int n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/dtwm-wayland-%s.ipc", rt, wd);
    if (n <= 0 || (size_t)n >= sizeof(addr.sun_path)) return;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    ctx->wl_ipc_fd = fd;
}

static void poll_wayland_ipc(DtPortalCtx *ctx) {
    if (!ctx || ctx->wl_ipc_fd < 0) return;
    for (;;) {
        ssize_t n = recv(ctx->wl_ipc_fd,
                         ctx->wl_ipc_rx + ctx->wl_ipc_rx_len,
                         sizeof(ctx->wl_ipc_rx) - 1 - ctx->wl_ipc_rx_len,
                         MSG_DONTWAIT);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(ctx->wl_ipc_fd);
                ctx->wl_ipc_fd = -1;
            }
            break;
        }
        ctx->wl_ipc_rx_len += (size_t)n;
        ctx->wl_ipc_rx[ctx->wl_ipc_rx_len] = '\0';

        char *start = ctx->wl_ipc_rx;
        char *nl = NULL;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            if (strncmp(start, "PROP _DT_PORTAL_SHORTCUT_EVENT ", 31) == 0) {
                const char *hex = start + 31;
                size_t hlen = strlen(hex);
                char decoded[512] = {0};
                size_t di = 0;
                for (size_t i = 0; i + 1 < hlen && di + 1 < sizeof(decoded); i += 2) {
                    unsigned int byte = 0;
                    if (sscanf(hex + i, "%2x", &byte) == 1) {
                        decoded[di++] = (char)byte;
                    }
                }
                decoded[di] = '\0';
                dtportal_on_shortcut_ipc_event(ctx, decoded);
            }
            start = nl + 1;
        }
        size_t rem = strlen(start);
        if (start != ctx->wl_ipc_rx) {
            memmove(ctx->wl_ipc_rx, start, rem + 1);
        }
        ctx->wl_ipc_rx_len = rem;
        if (ctx->wl_ipc_rx_len >= sizeof(ctx->wl_ipc_rx) - 2) {
            ctx->wl_ipc_rx_len = 0;
        }
    }
}

/* -------------------------------------------------------------------------
 * Test State & Command File Introspection (DTPORTAL_TEST_STATE / DTPORTAL_TEST_CMD)
 * ------------------------------------------------------------------------- */

void dtportal_write_test_state(DtPortalCtx *ctx) {
    if (!ctx || !ctx->test_state_path || !ctx->test_state_path[0]) return;
    int open_dialogs = 0;
    int inhibit_count = 0;
    const char *active_kind = "none";
    int app_items = 0;

    for (int i = 0; i < DTPORTAL_MAX_REQUESTS; ++i) {
        const DtPortalRequest *r = &ctx->requests[i];
        if (!r->in_use) continue;
        if (r->kind == DTPORTAL_REQ_INHIBIT) {
            inhibit_count++;
        } else if (r->dialog) {
            open_dialogs++;
            switch (r->kind) {
            case DTPORTAL_REQ_FILE_OPEN: active_kind = "file_open"; break;
            case DTPORTAL_REQ_FILE_SAVE: active_kind = "file_save"; break;
            case DTPORTAL_REQ_FILE_SAVE_MULTI: active_kind = "file_save_multi"; break;
            case DTPORTAL_REQ_APP_CHOOSER:
                active_kind = "app_chooser";
                app_items = r->app_count;
                break;
            case DTPORTAL_REQ_WALLPAPER: active_kind = "wallpaper"; break;
            case DTPORTAL_REQ_REMOTEDESKTOP_START: active_kind = "remotedesktop_start"; break;
            default: break;
            }
        }
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", ctx->test_state_path, (int)getpid());
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    fprintf(fp,
            "READY=1\nWAYLAND=%d\nOPEN_DIALOGS=%d\nACTIVE_KIND=%s\n"
            "APP_ITEMS=%d\nINHIBIT_COUNT=%d\n",
            ctx->is_wayland ? 1 : 0,
            open_dialogs,
            active_kind,
            app_items,
            inhibit_count);
    fclose(fp);
    rename(tmp, ctx->test_state_path);
}

static void poll_test_cmd_file(DtPortalCtx *ctx) {
    if (!ctx || !ctx->test_cmd_path || !ctx->test_cmd_path[0]) return;
    if (access(ctx->test_cmd_path, R_OK) != 0) return;
    char line[1024] = {0};
    FILE *fp = fopen(ctx->test_cmd_path, "r");
    if (fp) {
        if (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
        }
        fclose(fp);
    }
    unlink(ctx->test_cmd_path);

    if (strncmp(line, "ACCEPT", 6) == 0) {
        const char *arg = line + 6;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        dtportal_test_accept_active_dialog(ctx, arg);
    } else if (strcmp(line, "CANCEL") == 0) {
        dtportal_test_cancel_active_dialog(ctx);
    } else if (strcmp(line, "QUIT") == 0) {
        g_stop_portal = 1;
    }
}

/* -------------------------------------------------------------------------
 * Top-Level D-Bus Dispatch (/org/freedesktop/portal/desktop)
 * ------------------------------------------------------------------------- */

static void on_portal_method_call(
    GDBusConnection *conn, const gchar *sender, const gchar *object_path,
    const gchar *interface_name, const gchar *method_name,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data
) {
    (void)conn; (void)sender; (void)object_path;
    DtPortalCtx *ctx = (DtPortalCtx *)user_data;

    if (strcmp(interface_name, "org.freedesktop.impl.portal.FileChooser") == 0) {
        dtportal_handle_file_chooser(ctx, method_name, parameters, invocation);
    } else if (strcmp(interface_name, "org.freedesktop.impl.portal.AppChooser") == 0) {
        dtportal_handle_app_chooser(ctx, method_name, parameters, invocation);
    } else if (strcmp(interface_name, "org.freedesktop.impl.portal.Inhibit") == 0) {
        dtportal_handle_inhibit(ctx, method_name, parameters, invocation);
    } else if (strcmp(interface_name, "org.freedesktop.impl.portal.Wallpaper") == 0) {
        dtportal_handle_wallpaper(ctx, method_name, parameters, invocation);
    } else if (strcmp(interface_name, "org.freedesktop.impl.portal.GlobalShortcuts") == 0) {
        dtportal_handle_global_shortcuts(ctx, method_name, parameters, invocation);
    } else if (strcmp(interface_name, "org.freedesktop.impl.portal.RemoteDesktop") == 0) {
        dtportal_handle_remote_desktop(ctx, method_name, parameters, invocation);
    } else if (strcmp(interface_name, "org.freedesktop.impl.portal.Settings") == 0) {
        dtportal_handle_settings(ctx, method_name, parameters, invocation);
    } else {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.DBus.Error.UnknownInterface", "Unknown portal interface"
        );
    }
}

static GVariant *on_portal_get_property(
    GDBusConnection *conn, const gchar *sender, const gchar *object_path,
    const gchar *interface_name, const gchar *property_name,
    GError **error, gpointer user_data
) {
    (void)conn; (void)sender; (void)object_path; (void)error; (void)user_data;
    if (strcmp(property_name, "version") == 0) {
        if (strcmp(interface_name, "org.freedesktop.impl.portal.FileChooser") == 0)
            return g_variant_new_uint32(4u);
        if (strcmp(interface_name, "org.freedesktop.impl.portal.AppChooser") == 0)
            return g_variant_new_uint32(2u);
        if (strcmp(interface_name, "org.freedesktop.impl.portal.Inhibit") == 0)
            return g_variant_new_uint32(3u);
        if (strcmp(interface_name, "org.freedesktop.impl.portal.RemoteDesktop") == 0)
            return g_variant_new_uint32(2u);
        return g_variant_new_uint32(1u);
    }
    if (strcmp(interface_name, "org.freedesktop.impl.portal.RemoteDesktop") == 0 &&
        strcmp(property_name, "AvailableDeviceTypes") == 0) {
        return g_variant_new_uint32(7u); /* KEYBOARD | POINTER | TOUCHSCREEN */
    }
    return NULL;
}

static const GDBusInterfaceVTable g_portal_vtable = {
    .method_call = on_portal_method_call,
    .get_property = on_portal_get_property,
    .set_property = NULL,
};

static void on_appearance_signal_cb(
    GDBusConnection *conn, const gchar *sender, const gchar *obj_path,
    const gchar *iface, const gchar *sig_name, GVariant *params, gpointer user_data
) {
    (void)conn; (void)sender; (void)obj_path; (void)iface; (void)sig_name;
    DtPortalCtx *ctx = (DtPortalCtx *)user_data;
    const char *mode = NULL;
    guint32 cs = 2u, ct = 0u;
    g_variant_get(params, "(&suu)", &mode, &cs, &ct);
    dtportal_on_appearance_signal(ctx, cs, ct);
}

static void on_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    (void)name;
    DtPortalCtx *ctx = (DtPortalCtx *)user_data;
    ctx->bus = conn;
    for (int i = 0; g_desktop_node && g_desktop_node->interfaces[i]; ++i) {
        g_dbus_connection_register_object(
            conn, "/org/freedesktop/portal/desktop",
            g_desktop_node->interfaces[i], &g_portal_vtable, ctx, NULL, NULL
        );
    }
    g_dbus_connection_signal_subscribe(
        conn, NULL, "org.cde.Appearance1", "SchemeChanged", "/org/cde/Appearance1",
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_appearance_signal_cb, ctx, NULL
    );
    dtportal_sync_settings_from_appearance(ctx);
}

static void on_name_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    (void)conn; (void)name;
    DtPortalCtx *ctx = (DtPortalCtx *)user_data;
    dtportal_write_test_state(ctx);
}

static void on_pump_timer(XtPointer client_data, XtIntervalId *id) {
    (void)id;
    DtPortalCtx *ctx = (DtPortalCtx *)client_data;
    if (g_stop_portal) {
        XtAppSetExitFlag(ctx->app);
        return;
    }
    while (g_main_context_iteration(NULL, FALSE)) {}
    poll_wayland_ipc(ctx);
    poll_test_cmd_file(ctx);
    XtAppAddTimeOut(ctx->app, 15, on_pump_timer, ctx);
}

int main(int argc, char **argv) {
    signal(SIGTERM, on_portal_sig);
    signal(SIGINT, on_portal_sig);

    DtPortalCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.wl_ipc_fd = -1;
    ctx.color_scheme = 2u; /* default prefer-light */
    ctx.contrast = 0u;
    ctx.test_state_path = getenv("DTPORTAL_TEST_STATE");
    ctx.test_cmd_path = getenv("DTPORTAL_TEST_CMD");

    ctx.top_shell = XtVaAppInitialize(
        &ctx.app, "DtPortal", NULL, 0, &argc, argv, NULL,
        XmNmappedWhenManaged, False,
        XmNwidth, 1,
        XmNheight, 1,
        NULL
    );
    ctx.dpy = XtDisplay(ctx.top_shell);
    ctx.is_wayland = (ctx.dpy && XmWaylandIsActive(ctx.dpy)) ? true : false;
    if (ctx.dpy) {
        Widget xm_dpy = XmGetXmDisplay(ctx.dpy);
        if (xm_dpy) {
            XtVaSetValues(
                xm_dpy,
                XmNdragInitiatorProtocolStyle, XmDRAG_NONE,
                XmNdragReceiverProtocolStyle, XmDRAG_NONE,
                NULL
            );
        }
    }
    XtRealizeWidget(ctx.top_shell);

    /* Load CDE DtAction / datatypes database so AppChooser resolves .dt actions */
    if (DtAppInitialize(ctx.app, ctx.dpy, ctx.top_shell, argv[0], "DtPortal")) {
        DtDbLoad();
    }

    connect_wayland_ipc_if_present(&ctx);

    GError *err = NULL;
    g_desktop_node = g_dbus_node_info_new_for_xml(PORTAL_DESKTOP_INTROSPECTION_XML, &err);
    if (!g_desktop_node) {
        fprintf(stderr, "xdg-desktop-portal-cde: XML parse error: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        return 1;
    }
    g_request_node = g_dbus_node_info_new_for_xml(REQUEST_INTROSPECTION_XML, NULL);
    g_session_node = g_dbus_node_info_new_for_xml(SESSION_INTROSPECTION_XML, NULL);

    ctx.bus_owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        "org.freedesktop.impl.portal.desktop.cde",
        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_bus_acquired,
        on_name_acquired,
        NULL,
        &ctx,
        NULL
    );

    /* Pump GMainContext initially so bus ownership completes promptly */
    for (int i = 0; i < 10; ++i) {
        g_main_context_iteration(NULL, FALSE);
    }

    XtAppAddTimeOut(ctx.app, 15, on_pump_timer, &ctx);
    XtAppMainLoop(ctx.app);

    for (int i = 0; i < DTPORTAL_MAX_REQUESTS; ++i) {
        if (ctx.requests[i].in_use) dtportal_free_request(&ctx.requests[i]);
    }
    for (int i = 0; i < DTPORTAL_MAX_SESSIONS; ++i) {
        if (ctx.sessions[i].in_use) dtportal_close_session(&ctx.sessions[i], false);
    }
    dtportal_cleanup_remote_input(&ctx);
    if (ctx.wl_ipc_fd >= 0) close(ctx.wl_ipc_fd);
    if (ctx.bus_owner_id) g_bus_unown_name(ctx.bus_owner_id);
    if (g_desktop_node) g_dbus_node_info_unref(g_desktop_node);
    if (g_request_node) g_dbus_node_info_unref(g_request_node);
    if (g_session_node) g_dbus_node_info_unref(g_session_node);
    return 0;
}
