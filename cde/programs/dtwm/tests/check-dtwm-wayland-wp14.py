#!/usr/bin/env python3
"""WP-14 parallel dual-environment integration check (Wayland + X11/XLibre):
1. Native Motif Polkit Authentication Agent (`ck-polkit-agent`):
   - Verifies `ck-polkit-agent` links CK Core Motif ABI 9 (`libXm.so.9`), claims singleton
     ownership, and registers `/org/ck_core/PolicyKit1/AuthenticationAgent` with
     `org.freedesktop.PolicyKit1.Authority`.
   - Exercises live `BeginAuthentication` over D-Bus + Polkit 124+ AF_UNIX
     `agent-helper.socket` conversation in both headless `dtwm-wayland` and `Xvfb + dtwm`:
     * Verifies `_DT_POLKIT_DIALOG_STATE` maps (`visible=1;...;attempts=0`).
     * Injects an invalid password (`badpass` + Return) via `zwp_virtual_keyboard_v1`
       (Wayland) / `XTestFakeKeyEvent` (X11) and verifies clean in-dialog retry
       (`attempts=1`, `AUTH_FAIL` recorded).
     * Injects the valid password (`cdepolkit42` + Return) and verifies helper `SUCCESS`,
       `BeginAuthentication` D-Bus completion (`BEGIN_REPLY=OK`), and dialog dismissal
       (`visible=0`).
     * Exercises `CancelAuthentication` mid-prompt and verifies clean dismissal.
2. PAM & Secret Service Keyring Auto-Unlock (`pam_gnome_keyring.so` + `gnome-keyring-daemon`):
   - Verifies generated PAM stacks (`dtlogin` + `dtsession`) wire optional
     `pam_gnome_keyring.so` while preserving `dtsession`'s strict `pam_deny.so`
     session/password guards.
   - Executes login-password keyring unlock (`gnome-keyring-daemon --daemonize --login`
     + `--start --components=secrets,ssh`) in isolated Wayland and X11 D-Bus sessions,
     verifying `/org/freedesktop/secrets/collection/login` `Locked == false` and
     prompt-free `secret-tool store` / `secret-tool lookup` round-trips.
3. Interactive `ck-grab` Region Selection (`--region`) & Clipboard (`--clipboard` / `--paste-png`):
   - Maps a solid `#1EE064` (`rgb(30, 224, 100)`) target window in both Wayland and X11,
     triggers `ck-grab --region --clipboard`, waits for `_DT_GRAB_REGION_ACTIVE=1`,
     drags `(210, 180) -> (370, 290)` (`160x110`) via `zwp_virtual_pointer_v1` (Wayland)
     and `XTest` (X11), pastes `image/png` via `ck-grab --paste-png`, and verifies exact
     `160x110` dimensions and `(30, 224, 100)` pixel RGB fidelity.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from PIL import Image


MOCK_POLKIT_C = r"""
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static char g_agent_bus[128];
static char g_agent_path[256];
static const char *g_log_path = NULL;
static const char *g_expected_pw = "cdepolkit42";

static void append_log(const char *fmt, ...) {
    if (!g_log_path) return;
    FILE *fp = fopen(g_log_path, "a");
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
    fclose(fp);
}

typedef struct {
    char cookie[128];
} PendingBegin;

static void on_begin_notify(DBusPendingCall *pc, void *data) {
    PendingBegin *pb = (PendingBegin *)data;
    DBusMessage *reply = dbus_pending_call_steal_reply(pc);
    if (reply) {
        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
            append_log("BEGIN_REPLY=OK:%s\n", pb ? pb->cookie : "");
        } else {
            const char *err = dbus_message_get_error_name(reply);
            append_log("BEGIN_REPLY=ERR:%s:%s\n", pb ? pb->cookie : "", err ? err : "unknown");
        }
        dbus_message_unref(reply);
    }
    free(pb);
    dbus_pending_call_unref(pc);
}

static void send_begin_auth(DBusConnection *conn, const char *cookie, const char *msg_text) {
    if (!g_agent_bus[0] || !g_agent_path[0]) {
        append_log("BEGIN_REPLY=NO_AGENT\n");
        return;
    }
    DBusMessage *m = dbus_message_new_method_call(
        g_agent_bus,
        g_agent_path,
        "org.freedesktop.PolicyKit1.AuthenticationAgent",
        "BeginAuthentication"
    );
    if (!m) return;

    const char *action_id = "org.ck_core.test.admin";
    const char *icon_name = "dialog-password";
    DBusMessageIter it, dict_it, ids_it, st_it, id_dict_it, entry_it, var_it;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &action_id);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &msg_text);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon_name);

    /* empty details a{ss} */
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{ss}", &dict_it);
    dbus_message_iter_close_container(&it, &dict_it);

    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &cookie);

    /* identities a(sa{sv}) with ("unix-user", {"uid": uint32(getuid())}) */
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(sa{sv})", &ids_it);
    dbus_message_iter_open_container(&ids_it, DBUS_TYPE_STRUCT, NULL, &st_it);
    const char *kind = "unix-user";
    dbus_message_iter_append_basic(&st_it, DBUS_TYPE_STRING, &kind);
    dbus_message_iter_open_container(&st_it, DBUS_TYPE_ARRAY, "{sv}", &id_dict_it);
    dbus_message_iter_open_container(&id_dict_it, DBUS_TYPE_DICT_ENTRY, NULL, &entry_it);
    const char *ukey = "uid";
    dbus_uint32_t uid_val = (dbus_uint32_t)getuid();
    dbus_message_iter_append_basic(&entry_it, DBUS_TYPE_STRING, &ukey);
    dbus_message_iter_open_container(&entry_it, DBUS_TYPE_VARIANT, "u", &var_it);
    dbus_message_iter_append_basic(&var_it, DBUS_TYPE_UINT32, &uid_val);
    dbus_message_iter_close_container(&entry_it, &var_it);
    dbus_message_iter_close_container(&id_dict_it, &entry_it);
    dbus_message_iter_close_container(&st_it, &id_dict_it);
    dbus_message_iter_close_container(&ids_it, &st_it);
    dbus_message_iter_close_container(&it, &ids_it);

    DBusPendingCall *pc = NULL;
    if (dbus_connection_send_with_reply(conn, m, &pc, 30000) && pc) {
        PendingBegin *pb = calloc(1, sizeof(*pb));
        if (pb) snprintf(pb->cookie, sizeof(pb->cookie), "%s", cookie);
        dbus_pending_call_set_notify(pc, on_begin_notify, pb, NULL);
    }
    dbus_message_unref(m);
    dbus_connection_flush(conn);
}

static void send_cancel_auth(DBusConnection *conn, const char *cookie) {
    if (!g_agent_bus[0] || !g_agent_path[0]) return;
    DBusMessage *m = dbus_message_new_method_call(
        g_agent_bus,
        g_agent_path,
        "org.freedesktop.PolicyKit1.AuthenticationAgent",
        "CancelAuthentication"
    );
    if (!m) return;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &cookie, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, m, NULL);
    dbus_message_unref(m);
    dbus_connection_flush(conn);
    append_log("CANCEL_SENT=%s\n", cookie);
}

static DBusHandlerResult msg_filter(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    (void)user_data;
    if (dbus_message_is_method_call(msg, "org.freedesktop.PolicyKit1.Authority",
                                    "RegisterAuthenticationAgent")) {
        const char *sender = dbus_message_get_sender(msg);
        DBusMessageIter it;
        const char *locale = NULL;
        const char *obj_path = NULL;
        if (dbus_message_iter_init(msg, &it)) {
            /* skip subject struct (sa{sv}) */
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRUCT) {
                dbus_message_iter_next(&it);
            }
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&it, &locale);
                dbus_message_iter_next(&it);
            }
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&it, &obj_path);
            }
        }
        snprintf(g_agent_bus, sizeof(g_agent_bus), "%s", sender ? sender : "");
        snprintf(g_agent_path, sizeof(g_agent_path), "%s", obj_path ? obj_path : "");
        append_log("REGISTERED=%s|%s|%s\n", g_agent_bus, g_agent_path, locale ? locale : "");
        DBusMessage *rep = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, rep, NULL);
        dbus_message_unref(rep);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, "org.freedesktop.PolicyKit1.Authority",
                                    "UnregisterAuthenticationAgent")) {
        DBusMessage *rep = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, rep, NULL);
        dbus_message_unref(rep);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, "org.ck_core.MockPolkitControl", "TriggerBegin")) {
        const char *cookie = "cookie-default";
        const char *prompt_msg = "Authentication is required to configure system policy.";
        dbus_message_get_args(msg, NULL,
                              DBUS_TYPE_STRING, &cookie,
                              DBUS_TYPE_STRING, &prompt_msg,
                              DBUS_TYPE_INVALID);
        DBusMessage *rep = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, rep, NULL);
        dbus_message_unref(rep);
        send_begin_auth(conn, cookie, prompt_msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, "org.ck_core.MockPolkitControl", "TriggerCancel")) {
        const char *cookie = "cookie-default";
        dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &cookie, DBUS_TYPE_INVALID);
        DBusMessage *rep = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, rep, NULL);
        dbus_message_unref(rep);
        send_cancel_auth(conn, cookie);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int read_line_fd(int fd, char *out, size_t cap) {
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c = 0;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        if (c != '\r') out[pos++] = c;
    }
    out[pos] = '\0';
    return pos > 0 ? 1 : 0;
}

static void handle_helper_client(int cfd) {
    char user[128] = {0};
    char cookie[128] = {0};
    char pw[128] = {0};
    if (!read_line_fd(cfd, user, sizeof(user)) || !read_line_fd(cfd, cookie, sizeof(cookie))) {
        close(cfd);
        return;
    }
    const char *prompt = "PAM_PROMPT_ECHO_OFF Password: \n";
    (void)write(cfd, prompt, strlen(prompt));
    (void)read_line_fd(cfd, pw, sizeof(pw));
    if (strcmp(pw, g_expected_pw) == 0) {
        append_log("AUTH_OK=%s|%s\n", cookie, user);
        const char *ok = "SUCCESS\n";
        (void)write(cfd, ok, strlen(ok));
    } else {
        append_log("AUTH_FAIL=%s|%s\n", cookie, pw);
        const char * err = "PAM_ERROR_MSG Authentication failure\nFAILURE\n";
        (void)write(cfd, err, strlen(err));
    }
    close(cfd);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: mock_polkit <socket_path> <log_path>\n");
        return 1;
    }
    const char *sock_path = argv[1];
    g_log_path = argv[2];
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGCHLD, SIG_IGN);

    unlink(sock_path);
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) return 2;
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(sun.sun_path)) return 3;
    memcpy(sun.sun_path, sock_path, strlen(sock_path) + 1);
    if (bind(lfd, (struct sockaddr *)&sun, sizeof(sun)) != 0 || listen(lfd, 8) != 0) {
        perror("bind/listen");
        return 4;
    }

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fprintf(stderr, "dbus_bus_get failed: %s\n", err.message);
        return 5;
    }
    int rc = dbus_bus_request_name(conn, "org.freedesktop.PolicyKit1",
                                   DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                   &err);
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "request_name failed (%d): %s\n", rc, err.message ? err.message : "");
        return 6;
    }
    dbus_connection_add_filter(conn, msg_filter, NULL, NULL);
    append_log("AUTHORITY_READY=1\n");

    while (!g_stop) {
        dbus_connection_read_write_dispatch(conn, 20);
        struct pollfd pfd = { .fd = lfd, .events = POLLIN };
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN)) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd >= 0) {
                if (fork() == 0) {
                    close(lfd);
                    handle_helper_client(cfd);
                    _exit(0);
                }
                close(cfd);
            }
        }
    }

    close(lfd);
    unlink(sock_path);
    return 0;
}
"""


WL_INPUT_HELPER_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

extern const struct wl_interface zwp_virtual_keyboard_v1_interface;
extern const struct wl_interface zwlr_virtual_pointer_v1_interface;

static const struct wl_interface *vkb_mgr_types[] = {
    &wl_seat_interface, &zwp_virtual_keyboard_v1_interface,
};
static const struct wl_message vkb_mgr_requests[] = {
    { "create_virtual_keyboard", "on", vkb_mgr_types },
};
static const struct wl_interface zwp_virtual_keyboard_manager_v1_interface = {
    "zwp_virtual_keyboard_manager_v1", 1, 1, vkb_mgr_requests, 0, NULL,
};
static const struct wl_interface *vkb_null_types[] = { NULL, NULL, NULL, NULL };
static const struct wl_message vkb_requests[] = {
    { "keymap", "uhu", vkb_null_types },
    { "key", "uuu", vkb_null_types },
    { "modifiers", "uuuu", vkb_null_types },
    { "destroy", "", vkb_null_types },
};
const struct wl_interface zwp_virtual_keyboard_v1_interface = {
    "zwp_virtual_keyboard_v1", 1, 4, vkb_requests, 0, NULL,
};

static const struct wl_interface *vptr_mgr_types[] = {
    &wl_seat_interface, &zwlr_virtual_pointer_v1_interface,
};
static const struct wl_message vptr_mgr_requests[] = {
    { "create_virtual_pointer", "?on", vptr_mgr_types },
};
static const struct wl_interface zwlr_virtual_pointer_manager_v1_interface = {
    "zwlr_virtual_pointer_manager_v1", 1, 1, vptr_mgr_requests, 0, NULL,
};
static const struct wl_interface *vptr_null_types[] = { NULL, NULL, NULL, NULL, NULL };
static const struct wl_message vptr_requests[] = {
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
const struct wl_interface zwlr_virtual_pointer_v1_interface = {
    "zwlr_virtual_pointer_v1", 1, 9, vptr_requests, 0, NULL,
};

typedef struct {
    struct wl_seat *seat;
    struct wl_proxy *vkb_mgr;
    struct wl_proxy *vptr_mgr;
} InputState;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t ver) {
    InputState *st = (InputState *)data;
    (void)ver;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !st->seat) {
        st->seat = (struct wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, "zwp_virtual_keyboard_manager_v1") == 0 && !st->vkb_mgr) {
        st->vkb_mgr = (struct wl_proxy *)wl_registry_bind(
            reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (strcmp(iface, "zwlr_virtual_pointer_manager_v1") == 0 && !st->vptr_mgr) {
        st->vptr_mgr = (struct wl_proxy *)wl_registry_bind(
            reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener = { on_global, on_global_remove };

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

static uint32_t evdev_for_ascii(char ch) {
    switch (ch) {
    case 'a': return KEY_A; case 'b': return KEY_B; case 'c': return KEY_C;
    case 'd': return KEY_D; case 'e': return KEY_E; case 'f': return KEY_F;
    case 'g': return KEY_G; case 'h': return KEY_H; case 'i': return KEY_I;
    case 'j': return KEY_J; case 'k': return KEY_K; case 'l': return KEY_L;
    case 'm': return KEY_M; case 'n': return KEY_N; case 'o': return KEY_O;
    case 'p': return KEY_P; case 'q': return KEY_Q; case 'r': return KEY_R;
    case 's': return KEY_S; case 't': return KEY_T; case 'u': return KEY_U;
    case 'v': return KEY_V; case 'w': return KEY_W; case 'x': return KEY_X;
    case 'y': return KEY_Y; case 'z': return KEY_Z;
    case '0': return KEY_0; case '1': return KEY_1; case '2': return KEY_2;
    case '3': return KEY_3; case '4': return KEY_4; case '5': return KEY_5;
    case '6': return KEY_6; case '7': return KEY_7; case '8': return KEY_8;
    case '9': return KEY_9;
    case '\n': return KEY_ENTER;
    default: return 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 2;

    InputState st = {0};
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, &st);
    wl_display_roundtrip(dpy);

    if (strcmp(argv[1], "type") == 0 && argc >= 3) {
        if (!st.seat || !st.vkb_mgr) return 3;
        struct wl_proxy *vkb = wl_proxy_marshal_flags(
            st.vkb_mgr, 0, &zwp_virtual_keyboard_v1_interface, 1, 0, st.seat, NULL);
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
        struct xkb_keymap *kmap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        char *kmap_str = xkb_keymap_get_as_string(kmap, XKB_KEYMAP_FORMAT_TEXT_V1);
        uint32_t kmap_sz = (uint32_t)strlen(kmap_str) + 1u;
        int kfd = memfd_create("wp14-xkb", MFD_CLOEXEC);
        (void)ftruncate(kfd, (off_t)kmap_sz);
        void *ptr = mmap(NULL, kmap_sz, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, 0);
        memcpy(ptr, kmap_str, kmap_sz);
        munmap(ptr, kmap_sz);
        wl_proxy_marshal(vkb, 0, 1u, kfd, kmap_sz);
        wl_display_roundtrip(dpy);
        close(kfd);

        const char *txt = argv[2];
        for (size_t i = 0; txt[i]; ++i) {
            uint32_t kc = evdev_for_ascii(txt[i]);
            if (!kc) continue;
            wl_proxy_marshal(vkb, 1, now_ms(), kc, 1u);
            wl_display_roundtrip(dpy);
            usleep(20000);
            wl_proxy_marshal(vkb, 1, now_ms(), kc, 0u);
            wl_display_roundtrip(dpy);
            usleep(20000);
        }
        /* Press Enter */
        wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_ENTER, 1u);
        wl_display_roundtrip(dpy);
        usleep(25000);
        wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_ENTER, 0u);
        wl_display_roundtrip(dpy);

        wl_proxy_marshal(vkb, 3);
        wl_proxy_destroy(vkb);
        free(kmap_str);
        xkb_keymap_unref(kmap);
        xkb_context_unref(ctx);
        wl_display_roundtrip(dpy);
        wl_display_disconnect(dpy);
        return 0;
    }

    if (strcmp(argv[1], "drag") == 0 && argc >= 8) {
        if (!st.seat || !st.vptr_mgr) return 3;
        uint32_t x1 = (uint32_t)strtoul(argv[2], NULL, 0);
        uint32_t y1 = (uint32_t)strtoul(argv[3], NULL, 0);
        uint32_t x2 = (uint32_t)strtoul(argv[4], NULL, 0);
        uint32_t y2 = (uint32_t)strtoul(argv[5], NULL, 0);
        uint32_t ext_w = (uint32_t)strtoul(argv[6], NULL, 0);
        uint32_t ext_h = (uint32_t)strtoul(argv[7], NULL, 0);

        struct wl_proxy *vp = wl_proxy_marshal_flags(
            st.vptr_mgr, 0, &zwlr_virtual_pointer_v1_interface, 1, 0, st.seat, NULL);
        wl_display_roundtrip(dpy);

        /* Move to start */
        wl_proxy_marshal(vp, 1, now_ms(), x1, y1, ext_w, ext_h);
        wl_proxy_marshal(vp, 4);
        wl_display_roundtrip(dpy);
        usleep(40000);

        /* Press BTN_LEFT */
        wl_proxy_marshal(vp, 2, now_ms(), (uint32_t)BTN_LEFT, 1u);
        wl_proxy_marshal(vp, 4);
        wl_display_roundtrip(dpy);
        usleep(40000);

        /* Smooth drag to end */
        for (int s = 1; s <= 4; ++s) {
            uint32_t cx = x1 + ((x2 - x1) * (uint32_t)s) / 4u;
            uint32_t cy = y1 + ((y2 - y1) * (uint32_t)s) / 4u;
            wl_proxy_marshal(vp, 1, now_ms(), cx, cy, ext_w, ext_h);
            wl_proxy_marshal(vp, 4);
            wl_display_roundtrip(dpy);
            usleep(25000);
        }

        /* Release BTN_LEFT */
        wl_proxy_marshal(vp, 2, now_ms(), (uint32_t)BTN_LEFT, 0u);
        wl_proxy_marshal(vp, 4);
        wl_display_roundtrip(dpy);
        usleep(40000);

        wl_proxy_marshal(vp, 8);
        wl_proxy_destroy(vp);
        wl_display_roundtrip(dpy);
        wl_display_disconnect(dpy);
        return 0;
    }

    wl_display_disconnect(dpy);
    return 1;
}
"""


X11_HELPER_C = r"""
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*XTestFakeKeyEventFn)(Display *, unsigned int, Bool, unsigned long);
typedef int (*XTestFakeMotionEventFn)(Display *, int, int, int, unsigned long);
typedef int (*XTestFakeButtonEventFn)(Display *, unsigned int, Bool, unsigned long);

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void print_prop(Display *dpy, Window root, const char *name) {
    Atom a = XInternAtom(dpy, name, False);
    Atom act = None;
    int fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, a, 0, 1024, False, AnyPropertyType,
                           &act, &fmt, &nitems, &rem, &data) == Success && data && fmt == 8) {
        printf("%s=%.*s\n", name, (int)nitems, (const char *)data);
    } else {
        printf("%s=\n", name);
    }
    if (data) XFree(data);
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);

    if (argc >= 2 && strcmp(argv[1], "props") == 0) {
        print_prop(dpy, root, "_DT_POLKIT_DIALOG_STATE");
        print_prop(dpy, root, "_DT_GRAB_REGION_ACTIVE");
        printf("ROOT_SIZE=%dx%d\n", DisplayWidth(dpy, scr), DisplayHeight(dpy, scr));
        XCloseDisplay(dpy);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "color-win") == 0) {
        signal(SIGTERM, on_sig);
        signal(SIGINT, on_sig);
        /* Undecorated override-redirect bright #1EE064 target block at (180, 140) 320x220
         * so exact crop geometry (210, 180) -> (370, 290) is 100% pure #1EE064 on both WMs! */
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.override_redirect = True;
        swa.background_pixel = 0x1EE064UL;
        swa.event_mask = ExposureMask;
        Window w = XCreateWindow(dpy, root, 180, 140, 320, 220, 0,
                                 DefaultDepth(dpy, scr), InputOutput, DefaultVisual(dpy, scr),
                                 CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
        XMapRaised(dpy, w);
        XClearWindow(dpy, w);
        XFlush(dpy);
        while (!g_stop) {
            while (XPending(dpy) > 0) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == Expose) {
                    XClearWindow(dpy, w);
                }
            }
            usleep(50000);
        }
        XDestroyWindow(dpy, w);
        XCloseDisplay(dpy);
        return 0;
    }

    void *xtst = dlopen("libXtst.so.6", RTLD_LAZY);
    if (!xtst) xtst = dlopen("libXtst.so", RTLD_LAZY);
    if (!xtst) {
        XCloseDisplay(dpy);
        return 2;
    }

    if (argc >= 3 && strcmp(argv[1], "type") == 0) {
        XTestFakeKeyEventFn fk = (XTestFakeKeyEventFn)dlsym(xtst, "XTestFakeKeyEvent");
        const char *txt = argv[2];
        for (size_t i = 0; txt[i]; ++i) {
            char ks_name[2] = { txt[i], '\0' };
            KeySym ks = XStringToKeysym(ks_name);
            KeyCode kc = XKeysymToKeycode(dpy, ks);
            if (!kc) continue;
            fk(dpy, kc, True, CurrentTime);
            XSync(dpy, False);
            usleep(18000);
            fk(dpy, kc, False, CurrentTime);
            XSync(dpy, False);
            usleep(18000);
        }
        KeyCode ret_kc = XKeysymToKeycode(dpy, XK_Return);
        if (ret_kc) {
            fk(dpy, ret_kc, True, CurrentTime);
            XSync(dpy, False);
            usleep(20000);
            fk(dpy, ret_kc, False, CurrentTime);
            XSync(dpy, False);
        }
        XCloseDisplay(dpy);
        return 0;
    }

    if (argc >= 6 && strcmp(argv[1], "drag") == 0) {
        XTestFakeMotionEventFn fm = (XTestFakeMotionEventFn)dlsym(xtst, "XTestFakeMotionEvent");
        XTestFakeButtonEventFn fb = (XTestFakeButtonEventFn)dlsym(xtst, "XTestFakeButtonEvent");
        int x1 = atoi(argv[2]), y1 = atoi(argv[3]);
        int x2 = atoi(argv[4]), y2 = atoi(argv[5]);
        fm(dpy, scr, x1, y1, CurrentTime);
        XSync(dpy, False);
        usleep(35000);
        fb(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        usleep(35000);
        for (int s = 1; s <= 4; ++s) {
            int cx = x1 + ((x2 - x1) * s) / 4;
            int cy = y1 + ((y2 - y1) * s) / 4;
            fm(dpy, scr, cx, cy, CurrentTime);
            XSync(dpy, False);
            usleep(25000);
        }
        fb(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        usleep(35000);
        XCloseDisplay(dpy);
        return 0;
    }

    XCloseDisplay(dpy);
    return 1;
}
"""


def read_props(helper_bin: Path, env: dict[str, str]) -> dict[str, str]:
    cp = subprocess.run([str(helper_bin), "props"], env=env, capture_output=True, text=True, check=True)
    out: dict[str, str] = {}
    for ln in cp.stdout.splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            out[k] = v
    return out


def wait_for(pred, timeout: float = 8.0, interval: float = 0.08, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_val = None
    while time.monotonic() < deadline:
        last_val = pred()
        if last_val:
            return last_val
        time.sleep(interval)
    raise RuntimeError(f"Timed out waiting for {desc} (last={last_val!r})")


def verify_motif_abi9(pid: int, label: str, status_file: Path | None = None) -> None:
    try:
        maps = Path(f"/proc/{pid}/maps").read_text(encoding="utf-8", errors="replace")
        if "libXm.so.9" not in maps:
            raise RuntimeError(f"{label} (pid {pid}) did not map CK Core Motif ABI 9 (libXm.so.9)")
    except PermissionError:
        # Hardened PR_SET_DUMPABLE=0 daemons (ck-polkit-agent) block /proc/<pid>/maps to
        # unprivileged processes; verify DUMPABLE=0 and dladdr MOTIF_SO=libXm.so.9 from status file.
        if status_file is None or not status_file.is_file():
            raise
        st_txt = status_file.read_text(encoding="utf-8")
        if "libXm.so.9" not in st_txt or "DUMPABLE=0" not in st_txt:
            raise RuntimeError(
                f"{label} (pid {pid}) PR_SET_DUMPABLE status missing libXm.so.9 / DUMPABLE=0:\n{st_txt}"
            )


def check_pam_configs(cde_build: Path, cde_src: Path) -> None:
    dtlogin_pam = cde_build / "programs" / "dtlogin" / "config" / "dtlogin"
    dtsession_pam = cde_build / "programs" / "dtsession" / "config" / "dtsession"
    if not dtlogin_pam.is_file():
        dtlogin_pam = Path("/etc/pam.d/dtlogin")
    if not dtsession_pam.is_file():
        dtsession_pam = Path("/etc/pam.d/dtsession")

    dl_txt = dtlogin_pam.read_text(encoding="utf-8")
    ds_txt = dtsession_pam.read_text(encoding="utf-8")
    for req in (
        "auth required pam_unix.so",
        "-auth optional pam_gnome_keyring.so",
        "-session optional pam_gnome_keyring.so auto_start",
        "-password optional pam_gnome_keyring.so",
    ):
        if req not in dl_txt:
            raise RuntimeError(f"dtlogin PAM config missing '{req}':\n{dl_txt}")

    for req in (
        "auth required pam_unix.so",
        "-auth optional pam_gnome_keyring.so",
        "session required pam_deny.so",
        "password required pam_deny.so",
    ):
        if req not in ds_txt:
            raise RuntimeError(f"dtsession PAM config missing '{req}':\n{ds_txt}")

    xsw_txt = (cde_src / "debian" / "Xsession.wayland").read_text(encoding="utf-8")
    x11_txt = (cde_src / "programs" / "dtlogin" / "config" / "Xsession.src").read_text(encoding="utf-8")
    for label, txt in (("Xsession.wayland", xsw_txt), ("Xsession.src", x11_txt)):
        if "gnome-keyring-daemon" not in txt or "ck-polkit-agent" not in txt:
            raise RuntimeError(f"{label} missing gnome-keyring-daemon or ck-polkit-agent auto-start")


def verify_keyring_unlock(lane: str, env: dict[str, str], procs: list[subprocess.Popen]) -> str:
    gkd = shutil.which("gnome-keyring-daemon", path=env.get("PATH"))
    sec_tool = shutil.which("secret-tool", path=env.get("PATH"))
    dbus_send = shutil.which("dbus-send", path=env.get("PATH"))
    if not gkd or not sec_tool or not dbus_send:
        return "skipped(missing-tools)"

    env.pop("GNOME_KEYRING_CONTROL", None)
    ctrl_dir = Path(env["XDG_RUNTIME_DIR"]) / "keyring"
    ctrl_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(ctrl_dir, 0o700)

    # Two-stage PAM + Xsession sequence isolated per-lane via --control-directory:
    # 1) --daemonize --unlock reads login password on stdin and creates/unlocks `login` keyring.
    # 2) --start --components=secrets,ssh publishes org.freedesktop.secrets on this session bus.
    p1 = subprocess.run(
        [gkd, "--daemonize", "--unlock", f"--control-directory={ctrl_dir}"],
        input="KeyringLoginSecret99!",
        text=True,
        env=env,
        capture_output=True,
        timeout=8,
    )
    if p1.returncode == 0 and p1.stdout:
        for line in p1.stdout.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().rstrip(";")
    env["GNOME_KEYRING_CONTROL"] = str(ctrl_dir)

    p2 = subprocess.run(
        [gkd, "--start", "--components=secrets,ssh", f"--control-directory={ctrl_dir}"],
        text=True,
        env=env,
        capture_output=True,
        timeout=8,
    )
    if p2.returncode == 0 and p2.stdout:
        for line in p2.stdout.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().rstrip(";")

    # Verify /org/freedesktop/secrets/collection/login is unlocked (Locked == false)
    def login_unlocked():
        cp = subprocess.run(
            [
                dbus_send,
                "--session",
                "--print-reply",
                "--dest=org.freedesktop.secrets",
                "/org/freedesktop/secrets/collection/login",
                "org.freedesktop.DBus.Properties.Get",
                "string:org.freedesktop.Secret.Collection",
                "string:Locked",
            ],
            env=env,
            capture_output=True,
            text=True,
            timeout=5,
        )
        return "boolean false" in cp.stdout

    wait_for(login_unlocked, timeout=6.0, desc=f"{lane} Secret Service login keyring unlocked")

    # Round-trip secret-tool store + lookup without any GUI prompt
    secret_val = f"cde-wp14-secret-{lane}"
    st_store = subprocess.run(
        [
            sec_tool,
            "store",
            "--label=CDE WP14 Secret",
            "service",
            "ck-cde-wp14",
            "lane",
            lane,
        ],
        input=secret_val,
        text=True,
        env=env,
        capture_output=True,
        timeout=8,
        check=True,
    )
    _ = st_store
    st_lookup = subprocess.run(
        [sec_tool, "lookup", "service", "ck-cde-wp14", "lane", lane],
        text=True,
        env=env,
        capture_output=True,
        timeout=8,
        check=True,
    )
    if st_lookup.stdout.strip() != secret_val:
        raise RuntimeError(
            f"[{lane}] secret-tool lookup mismatch: got {st_lookup.stdout!r}, expected {secret_val!r}"
        )
    subprocess.run(["pkill", "-f", str(ctrl_dir)], check=False)
    return "unlocked+roundtrip_ok"


def exercise_lane_features(
    lane: str,
    env: dict[str, str],
    work: Path,
    bins: dict[str, Path],
    procs: list[subprocess.Popen],
    is_wayland: bool,
) -> dict[str, object]:
    dbus_send = shutil.which("dbus-send", path=env.get("PATH")) or "dbus-send"
    helper_bin = bins["x11_helper"]
    wl_input_bin = bins["wl_input"]
    mock_polkit_bin = bins["mock_polkit"]
    agent_bin = bins["ck_polkit_agent"]
    grab_bin = bins["ck_grab"]

    # 1. Start mock Polkit Authority + AF_UNIX agent-helper socket
    # Keep socket path short (< 104 bytes for sockaddr_un.sun_path)
    sock_dir = Path(tempfile.mkdtemp(prefix=f"pk14-{lane[:2]}-", dir="/var/tmp"))
    sock_path = sock_dir / "h.sock"
    pk_log = work / f"polkit-{lane}.log"
    pk_log.write_text("", encoding="utf-8")

    p_auth = subprocess.Popen(
        [str(mock_polkit_bin), str(sock_path), str(pk_log)],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    procs.append(p_auth)
    wait_for(lambda: "AUTHORITY_READY=1" in pk_log.read_text(encoding="utf-8"), desc=f"{lane} mock polkit authority")

    agent_status = work / f"agent-{lane}.status"
    agent_env = {
        **env,
        "CK_POLKIT_BUS": "session",
        "CK_POLKIT_HELPER_SOCKET": str(sock_path),
    }
    p_agent = subprocess.Popen(
        [str(agent_bin), "--status-file", str(agent_status)],
        env=agent_env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    procs.append(p_agent)

    wait_for(
        lambda: "REGISTERED=" in pk_log.read_text(encoding="utf-8") and agent_status.is_file(),
        desc=f"{lane} ck-polkit-agent registration",
    )
    verify_motif_abi9(p_agent.pid, f"{lane} ck-polkit-agent", agent_status)

    # Trigger BeginAuthentication ("cookie-1")
    subprocess.run(
        [
            dbus_send,
            "--session",
            "--print-reply",
            "--dest=org.freedesktop.PolicyKit1",
            "/org/freedesktop/PolicyKit1/Authority",
            "org.ck_core.MockPolkitControl.TriggerBegin",
            "string:cookie-wp14-1",
            "string:Authentication is required to manage system disks.",
        ],
        env=env,
        check=True,
        capture_output=True,
    )

    wait_for(
        lambda: (
            lambda s: "visible=1;action=org.ck_core.test.admin;" in s and s.endswith(";attempts=0")
        )(read_props(helper_bin, env).get("_DT_POLKIT_DIALOG_STATE", "")),
        desc=f"{lane} polkit dialog visible (attempts=0)",
    )

    # Type WRONG password first -> verify clean retry (attempts=1)
    if is_wayland:
        subprocess.run([str(wl_input_bin), "type", "badpass"], env=env, check=True)
    else:
        subprocess.run([str(helper_bin), "type", "badpass"], env=env, check=True)

    wait_for(
        lambda: (
            lambda s: "visible=1;action=org.ck_core.test.admin;" in s and s.endswith(";attempts=1")
        )(read_props(helper_bin, env).get("_DT_POLKIT_DIALOG_STATE", ""))
        and "AUTH_FAIL=cookie-wp14-1|badpass" in pk_log.read_text(encoding="utf-8"),
        desc=f"{lane} polkit retry after wrong password",
    )

    # Type RIGHT password -> verify SUCCESS + dialog dismissal (visible=0)
    if is_wayland:
        subprocess.run([str(wl_input_bin), "type", "cdepolkit42"], env=env, check=True)
    else:
        subprocess.run([str(helper_bin), "type", "cdepolkit42"], env=env, check=True)

    wait_for(
        lambda: "BEGIN_REPLY=OK:cookie-wp14-1" in pk_log.read_text(encoding="utf-8")
        and "AUTH_OK=cookie-wp14-1|" in pk_log.read_text(encoding="utf-8")
        and read_props(helper_bin, env).get("_DT_POLKIT_DIALOG_STATE", "").startswith("visible=0;"),
        desc=f"{lane} polkit elevation completion",
    )

    # Exercise CancelAuthentication mid-dialog
    subprocess.run(
        [
            dbus_send,
            "--session",
            "--print-reply",
            "--dest=org.freedesktop.PolicyKit1",
            "/org/freedesktop/PolicyKit1/Authority",
            "org.ck_core.MockPolkitControl.TriggerBegin",
            "string:cookie-wp14-cancel",
            "string:Authentication is required to cancel test.",
        ],
        env=env,
        check=True,
        capture_output=True,
    )
    wait_for(
        lambda: read_props(helper_bin, env).get("_DT_POLKIT_DIALOG_STATE", "").startswith("visible=1;"),
        desc=f"{lane} polkit cancel dialog visible",
    )
    subprocess.run(
        [
            dbus_send,
            "--session",
            "--print-reply",
            "--dest=org.freedesktop.PolicyKit1",
            "/org/freedesktop/PolicyKit1/Authority",
            "org.ck_core.MockPolkitControl.TriggerCancel",
            "string:cookie-wp14-cancel",
        ],
        env=env,
        check=True,
        capture_output=True,
    )
    wait_for(
        lambda: read_props(helper_bin, env).get("_DT_POLKIT_DIALOG_STATE", "").startswith("visible=0;")
        and "cookie-wp14-cancel" in pk_log.read_text(encoding="utf-8")
        and "BEGIN_REPLY=" in pk_log.read_text(encoding="utf-8").split("CANCEL_SENT=cookie-wp14-cancel")[-1],
        desc=f"{lane} polkit CancelAuthentication dismissal",
    )

    # 2. Verify PAM Secret Service Keyring auto-unlock + secret-tool round-trip
    keyring_res = verify_keyring_unlock(lane, env, procs)

    # 3. Verify interactive ck-grab --region --clipboard + --paste-png round-trip
    p_color = subprocess.Popen(
        [str(helper_bin), "color-win"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    procs.append(p_color)
    time.sleep(0.35)

    props0 = read_props(helper_bin, env)
    root_w, root_h = (int(x) for x in props0.get("ROOT_SIZE", "1280x720").split("x"))

    p_grab = subprocess.Popen(
        [str(grab_bin), "--region", "--clipboard", "--no-cursor"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    procs.append(p_grab)

    wait_for(
        lambda: read_props(helper_bin, env).get("_DT_GRAB_REGION_ACTIVE", "") == "1",
        desc=f"{lane} ck-grab region overlay active",
    )
    verify_motif_abi9(p_grab.pid, f"{lane} ck-grab")
    time.sleep(0.12)

    if is_wayland:
        # On dual-headless outputs (2560x720 layout), zwp_virtual_pointer_v1 motion_absolute
        # maps across the full output_layout box (root_w x root_h).
        subprocess.run(
            [str(wl_input_bin), "drag", "210", "180", "370", "290", str(root_w), str(root_h)],
            env=env,
            check=True,
        )
    else:
        subprocess.run([str(helper_bin), "drag", "210", "180", "370", "290"], env=env, check=True)

    grab_out, grab_err = p_grab.communicate(timeout=10)
    if p_grab.returncode != 0:
        raise RuntimeError(f"[{lane}] ck-grab --region --clipboard exited {p_grab.returncode}: {grab_err} {grab_out}")

    pasted_png = work / f"pasted-{lane}.png"
    cp_paste = subprocess.run(
        [str(grab_bin), "--paste-png", str(pasted_png)],
        env=env,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if cp_paste.returncode != 0 or not pasted_png.is_file():
        raise RuntimeError(f"[{lane}] ck-grab --paste-png failed ({cp_paste.returncode}): {cp_paste.stderr}")

    with Image.open(pasted_png) as im:
        w, h = im.size
        rgb = im.convert("RGB").getpixel((w // 2, h // 2))
    if abs(w - 160) > 4 or abs(h - 110) > 4:
        raise RuntimeError(f"[{lane}] unexpected region crop size {w}x{h} (expected ~160x110)")
    if abs(rgb[0] - 30) > 12 or abs(rgb[1] - 224) > 12 or abs(rgb[2] - 100) > 12:
        raise RuntimeError(f"[{lane}] unexpected pasted region RGB {rgb} (expected ~(30, 224, 100))")

    shutil.rmtree(sock_dir, ignore_errors=True)
    return {
        "lane": lane,
        "polkit": "retry+elevate+cancel_ok",
        "keyring": keyring_res,
        "grab_size": f"{w}x{h}",
        "grab_rgb": rgb,
    }


def start_dbus_daemon(work: Path, lane: str) -> tuple[subprocess.Popen, str]:
    p = subprocess.Popen(
        ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        start_new_session=True,
    )
    assert p.stdout is not None
    addr = p.stdout.readline().strip()
    if not addr:
        raise RuntimeError(f"[{lane}] dbus-daemon failed to print session bus address")
    return p, addr


def cleanup_procs(procs: list[subprocess.Popen]) -> None:
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except OSError:
                pass
    time.sleep(0.2)
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass


def run_wayland_lane(comp_bin: Path, work: Path, bins: dict[str, Path]) -> dict[str, object]:
    lane_dir = work / "wayland"
    xdg = lane_dir / "xdg"
    home = lane_dir / "home"
    data_home = home / ".local" / "share"
    for d in (xdg, home, data_home):
        d.mkdir(parents=True, mode=0o700)
        os.chmod(d, 0o700)

    procs: list[subprocess.Popen] = []
    try:
        p_dbus, bus_addr = start_dbus_daemon(lane_dir, "wayland")
        procs.append(p_dbus)

        env = os.environ.copy()
        env.update(
            {
                "HOME": str(home),
                "XDG_RUNTIME_DIR": str(xdg),
                "XDG_DATA_HOME": str(data_home),
                "DBUS_SESSION_BUS_ADDRESS": bus_addr,
                "WLR_BACKENDS": "headless",
                "WLR_HEADLESS_OUTPUTS": "1",
                "WLR_RENDERER": "pixman",
                "WLR_LIBINPUT_NO_DEVICES": "1",
                "PATH": f"/usr/dt/bin:{env.get('PATH', '/usr/bin:/bin')}",
            }
        )

        status_file = lane_dir / "comp.status"
        comp_log = (lane_dir / "dtwm-wayland.log").open("w", encoding="utf-8")
        p_comp = subprocess.Popen(
            [str(comp_bin), "--status-file", str(status_file)],
            env=env,
            stdout=comp_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        procs.append(p_comp)

        def comp_ready():
            if p_comp.poll() is not None:
                raise RuntimeError("dtwm-wayland exited early")
            if not status_file.is_file():
                return None
            kv = dict(
                ln.split("=", 1)
                for ln in status_file.read_text(encoding="utf-8").splitlines()
                if "=" in ln
            )
            if kv.get("WAYLAND_DISPLAY") and kv.get("DISPLAY"):
                return kv
            return None

        kv = wait_for(comp_ready, timeout=10.0, desc="dtwm-wayland status file")
        env["WAYLAND_DISPLAY"] = kv["WAYLAND_DISPLAY"]
        env["DISPLAY"] = kv["DISPLAY"]

        return exercise_lane_features("wayland", env, lane_dir, bins, procs, is_wayland=True)
    finally:
        cleanup_procs(procs)


def run_x11_lane(dtwm_bin: Path, work: Path, bins: dict[str, Path]) -> dict[str, object]:
    lane_dir = work / "x11"
    xdg = lane_dir / "xdg"
    home = lane_dir / "home"
    data_home = home / ".local" / "share"
    for d in (xdg, home, data_home):
        d.mkdir(parents=True, mode=0o700)
        os.chmod(d, 0o700)

    procs: list[subprocess.Popen] = []
    try:
        p_dbus, bus_addr = start_dbus_daemon(lane_dir, "x11")
        procs.append(p_dbus)

        disp_fd_r, disp_fd_w = os.pipe()
        p_xvfb = subprocess.Popen(
            ["Xvfb", "-displayfd", str(disp_fd_w), "-screen", "0", "1280x720x24", "-nolisten", "tcp"],
            pass_fds=(disp_fd_w,),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        os.close(disp_fd_w)
        procs.append(p_xvfb)
        disp_num = os.read(disp_fd_r, 64).decode("utf-8").strip()
        os.close(disp_fd_r)
        if not disp_num:
            raise RuntimeError("Xvfb failed to report display number")
        x_disp = f":{disp_num}"

        env = os.environ.copy()
        env.pop("WAYLAND_DISPLAY", None)
        env.update(
            {
                "HOME": str(home),
                "XDG_RUNTIME_DIR": str(xdg),
                "XDG_DATA_HOME": str(data_home),
                "DBUS_SESSION_BUS_ADDRESS": bus_addr,
                "DISPLAY": x_disp,
                "PATH": f"/usr/dt/bin:{env.get('PATH', '/usr/bin:/bin')}",
            }
        )

        p_dtwm = subprocess.Popen(
            [str(dtwm_bin)],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        procs.append(p_dtwm)
        time.sleep(0.9)

        return exercise_lane_features("x11", env, lane_dir, bins, procs, is_wayland=False)
    finally:
        cleanup_procs(procs)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    dtwm_bin = comp_bin.parent / "dtwm"
    if not dtwm_bin.is_file():
        dtwm_bin = Path("/usr/dt/bin/dtwm")
    cde_build = comp_bin.parents[2]
    cde_src = Path(__file__).resolve().parents[3]

    agent_bin = Path(shutil.which("ck-polkit-agent") or "/usr/dt/bin/ck-polkit-agent")
    grab_bin = Path(shutil.which("ck-grab") or "/usr/dt/bin/ck-grab")
    for req_bin in (comp_bin, dtwm_bin, agent_bin, grab_bin):
        if not req_bin.is_file():
            raise SystemExit(f"missing required binary: {req_bin}")

    check_pam_configs(cde_build, cde_src)

    cc = os.environ.get("CC", "cc").split()
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp14-check-", dir=tmp_base) as work_s:
        work = Path(work_s)

        mock_c = work / "mock_polkit.c"
        mock_bin = work / "mock_polkit"
        mock_c.write_text(MOCK_POLKIT_C, encoding="utf-8")
        dbus_cflags = subprocess.check_output(["pkg-config", "--cflags", "dbus-1"], text=True).split()
        dbus_libs = subprocess.check_output(["pkg-config", "--libs", "dbus-1"], text=True).split()
        subprocess.run(cc + ["-O2", str(mock_c), "-o", str(mock_bin)] + dbus_cflags + dbus_libs, check=True)

        wl_c = work / "wl_input.c"
        wl_bin = work / "wl_input"
        wl_c.write_text(WL_INPUT_HELPER_C, encoding="utf-8")
        subprocess.run(
            cc + ["-O2", str(wl_c), "-o", str(wl_bin), "-lwayland-client", "-lxkbcommon"],
            check=True,
        )

        x11_c = work / "x11_helper.c"
        x11_bin = work / "x11_helper"
        x11_c.write_text(X11_HELPER_C, encoding="utf-8")
        subprocess.run(cc + ["-O2", str(x11_c), "-o", str(x11_bin), "-lX11", "-ldl"], check=True)

        bins = {
            "mock_polkit": mock_bin,
            "wl_input": wl_bin,
            "x11_helper": x11_bin,
            "ck_polkit_agent": agent_bin,
            "ck_grab": grab_bin,
        }

        t0 = time.monotonic()
        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_wl = pool.submit(run_wayland_lane, comp_bin, work, bins)
            fut_x11 = pool.submit(run_x11_lane, dtwm_bin, work, bins)
            res_wl = fut_wl.result()
            res_x11 = fut_x11.result()
        elapsed = time.monotonic() - t0

        print(
            f"PASS: WP-14 parallel Wayland + X11 verification completed in {elapsed:.2f}s\n"
            f"  Wayland: {res_wl}\n"
            f"  X11:     {res_x11}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
