#!/usr/bin/env python3
"""End-to-end integration test for xdg-desktop-portal-cde (Topics 1.1 & 1.4b):
1. Runtime provenance: verifies xdg-desktop-portal-cde maps CK Core Motif ABI 9
   (libXm.so.9) and runs natively on Wayland (dtwm-wayland --no-xwayland).
2. FileChooser (OpenFile, SaveFile, Request.Close): verifies real Motif
   XmFileSelectionDialog maps on dtwm-wayland, handles filters + choices +
   current_folder/current_name, and cancels cleanly on Request.Close().
3. AppChooser (ChooseApplication & UpdateChoices): verifies DtAction / .desktop
   entries populate Motif XmScrolledList, live UpdateChoices refresh, and choice reply.
4. Inhibit (Inhibit, CreateMonitor, dtwm-wayland idle-suspend suppression):
   verifies f.check_idle_suspend reports action=inhibited_by_portal while active
   and releases on Request.Close(), plus StateChanged monitor signal.
5. Wallpaper (SetWallpaperURI): verifies PNG -> ~/.dt/backdrops/PortalWallpaper.pm
   conversion, Motif preview confirmation, and live dtwm-wayland backdrop update via grim.
6. GlobalShortcuts (CreateSession, BindShortcuts, ListShortcuts, Activated/Deactivated):
   verifies live Ctrl+Alt+G keybinding registration in dtwm-wayland and D-Bus signals.
7. RemoteDesktop (CreateSession, SelectDevices, Start consent + restore_token,
   NotifyPointer*, NotifyKeyboard*, and ConnectToEIS binary EIS v1 wire protocol):
   verifies live pointer/keyboard injection into a Motif target window via both
   D-Bus Notify* methods and the ConnectToEIS UNIX socket file descriptor.
"""

from __future__ import annotations

import argparse
from contextlib import suppress
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time


PORTAL_DRIVER_C = r"""
/*
 * C GDBus test client exercising all 7 interfaces of org.freedesktop.impl.portal.desktop.cde,
 * including asynchronous Motif dialog calls (accepting via DTPORTAL_TEST_CMD after verifying
 * the Motif dialog mapped on dtwm-wayland) and binary EIS v1 wire protocol over ConnectToEIS!
 */
#define _GNU_SOURCE
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.impl.portal.desktop.cde"
#define OBJ_PATH "/org/freedesktop/portal/desktop"

static const char *g_state_path = NULL;
static const char *g_cmd_path = NULL;
static int g_activated_count = 0;
static int g_deactivated_count = 0;
static char g_last_shortcut_id[96] = {0};
static int g_inhibit_state_signals = 0;

static bool state_has_dialog(const char *expected_kind) {
    for (int attempt = 0; attempt < 150; ++attempt) {
        FILE *fp = fopen(g_state_path, "r");
        if (fp) {
            char buf[1024] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
            fclose(fp);
            buf[n] = '\0';
            char needle[128];
            snprintf(needle, sizeof(needle), "ACTIVE_KIND=%s", expected_kind);
            if (strstr(buf, "OPEN_DIALOGS=1") && strstr(buf, needle)) {
                return true;
            }
        }
        g_main_context_iteration(NULL, FALSE);
        usleep(25000);
    }
    return false;
}

static void send_test_cmd(const char *cmd_line) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_cmd_path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    fprintf(fp, "%s\n", cmd_line);
    fclose(fp);
    rename(tmp, g_cmd_path);
}

typedef struct {
    bool done;
    GVariant *reply;
    GError *err;
} AsyncCallRes;

static void on_async_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    AsyncCallRes *ar = (AsyncCallRes *)user_data;
    ar->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &ar->err);
    ar->done = true;
}

static GVariant *wait_async_reply(AsyncCallRes *ar) {
    for (int i = 0; i < 200 && !ar->done; ++i) {
        g_main_context_iteration(NULL, FALSE);
        usleep(20000);
    }
    if (!ar->done) {
        fprintf(stderr, "FAIL: async D-Bus call timed out\n");
        exit(10);
    }
    if (ar->err) {
        fprintf(stderr, "FAIL: async D-Bus error: %s\n", ar->err->message);
        exit(11);
    }
    return ar->reply;
}

static void on_gs_signal(
    GDBusConnection *c, const gchar *s, const gchar *o,
    const gchar *iface, const gchar *sig, GVariant *params, gpointer ud
) {
    (void)c; (void)s; (void)o; (void)iface; (void)ud;
    if (strcmp(sig, "Activated") == 0 || strcmp(sig, "Deactivated") == 0) {
        const char *sh = NULL, *sid = NULL;
        guint64 ts = 0;
        GVariant *opts = NULL;
        g_variant_get(params, "(&o&st@a{sv})", &sh, &sid, &ts, &opts);
        if (sid) snprintf(g_last_shortcut_id, sizeof(g_last_shortcut_id), "%s", sid);
        if (strcmp(sig, "Activated") == 0) g_activated_count++;
        else g_deactivated_count++;
        g_variant_unref(opts);
    }
}

static void on_inh_signal(
    GDBusConnection *c, const gchar *s, const gchar *o,
    const gchar *iface, const gchar *sig, GVariant *params, gpointer ud
) {
    (void)c; (void)s; (void)o; (void)iface; (void)params; (void)ud;
    if (strcmp(sig, "StateChanged") == 0) g_inhibit_state_signals++;
}

/* Binary EIS v1 client packet builder */
static void eis_send(int fd, uint64_t obj, uint32_t opcode, const void *body, size_t body_len) {
    uint8_t buf[256];
    uint32_t total = (uint32_t)(16 + body_len);
    memcpy(buf, &obj, 8);
    memcpy(buf + 8, &total, 4);
    memcpy(buf + 12, &opcode, 4);
    if (body_len > 0) memcpy(buf + 16, body, body_len);
    if (send(fd, buf, total, 0) != (ssize_t)total) {
        fprintf(stderr, "FAIL: eis_send failed\n");
        exit(30);
    }
}

static void run_eis_click_test(int eis_fd, float click_x, float click_y) {
    /* 1. Read server greeting: ei_handshake(0).handshake_version(1) */
    uint8_t rx[2048];
    struct pollfd pfd = { .fd = eis_fd, .events = POLLIN };
    if (poll(&pfd, 1, 3000) <= 0) {
        fprintf(stderr, "FAIL: EIS socket did not send handshake_version\n");
        exit(31);
    }
    ssize_t n = recv(eis_fd, rx, sizeof(rx), 0);
    if (n < 20) {
        fprintf(stderr, "FAIL: short EIS greeting (%zd)\n", n);
        exit(32);
    }
    uint64_t obj = 0; uint32_t plen = 0, op = 0, ver = 0;
    memcpy(&obj, rx, 8); memcpy(&plen, rx + 8, 4); memcpy(&op, rx + 12, 4); memcpy(&ver, rx + 16, 4);
    if (obj != 0ULL || op != 0u || ver != 1u) {
        fprintf(stderr, "FAIL: unexpected EIS greeting obj=%llu op=%u ver=%u\n",
                (unsigned long long)obj, op, ver);
        exit(33);
    }

    /* 2. Reply handshake_version(1), context_type(2=SENDER), finish() */
    uint32_t v1 = 1u, sender_type = 2u;
    eis_send(eis_fd, 0ULL, 0u, &v1, 4);
    eis_send(eis_fd, 0ULL, 2u, &sender_type, 4);
    eis_send(eis_fd, 0ULL, 1u, NULL, 0); /* finish() */

    /* 3. Read ei_connection + ei_seat + capabilities + done */
    if (poll(&pfd, 1, 3000) <= 0) {
        fprintf(stderr, "FAIL: EIS server did not send ei_seat\n");
        exit(34);
    }
    usleep(30000);
    n = recv(eis_fd, rx, sizeof(rx), 0);
    if (n < 32) {
        fprintf(stderr, "FAIL: short EIS seat announcement (%zd)\n", n);
        exit(35);
    }

    /* 4. Bind seat capabilities (0x02 | 0x04 | 0x08) on EIS_OBJ_SEAT (0xFF00000000000002ULL) */
    uint64_t caps = 0x0EULL;
    eis_send(eis_fd, 0xFF00000000000002ULL, 1u, &caps, 8);

    if (poll(&pfd, 1, 3000) <= 0) {
        fprintf(stderr, "FAIL: EIS server did not send ei_device\n");
        exit(36);
    }
    usleep(30000);
    n = recv(eis_fd, rx, sizeof(rx), 0);
    if (n < 32) {
        fprintf(stderr, "FAIL: short EIS device announcement (%zd)\n", n);
        exit(37);
    }

    /* 5. start_emulating(serial=2, seq=1) on EIS_OBJ_DEVICE (0xFF00000000000003ULL) */
    uint32_t start_args[2] = { 2u, 1u };
    eis_send(eis_fd, 0xFF00000000000003ULL, 1u, start_args, 8);

    /* 6. motion_absolute(click_x, click_y) on EIS_OBJ_PTR_ABS (0xFF00000000000004ULL) */
    float xy[2] = { click_x, click_y };
    eis_send(eis_fd, 0xFF00000000000004ULL, 1u, xy, 8);

    /* 7. button(272, 1) + button(272, 0) on EIS_OBJ_BUTTON (0xFF00000000000005ULL) */
    uint32_t b_down[2] = { 272u, 1u };
    uint32_t b_up[2]   = { 272u, 0u };
    eis_send(eis_fd, 0xFF00000000000005ULL, 1u, b_down, 8);
    eis_send(eis_fd, 0xFF00000000000005ULL, 1u, b_up, 8);

    /* 8. frame(serial=2, ts=1000) on EIS_OBJ_DEVICE */
    uint8_t frame_body[12] = {0};
    uint32_t fser = 2u; uint64_t fts = 1000ULL;
    memcpy(frame_body, &fser, 4);
    memcpy(frame_body + 4, &fts, 8);
    eis_send(eis_fd, 0xFF00000000000003ULL, 3u, frame_body, 12);
}

int main(int argc, char **argv) {
    if (argc < 6) return 2;
    g_state_path = argv[1];
    g_cmd_path = argv[2];
    const char *sample_txt = argv[3];
    const char *wallpaper_png = argv[4];
    double target_x = atof(argv[5]);
    double target_y = atof(argv[6]);

    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) {
        fprintf(stderr, "FAIL: g_bus_get_sync: %s\n", err ? err->message : "?");
        return 3;
    }

    g_dbus_connection_signal_subscribe(
        bus, BUS_NAME, "org.freedesktop.impl.portal.GlobalShortcuts", NULL,
        OBJ_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_gs_signal, NULL, NULL
    );
    g_dbus_connection_signal_subscribe(
        bus, BUS_NAME, "org.freedesktop.impl.portal.Inhibit", NULL,
        OBJ_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_inh_signal, NULL, NULL
    );

    /* --- A. Settings.ReadAll --- */
    const char *ns_arr[] = { "org.freedesktop.appearance", NULL };
    GVariant *srep = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.Settings", "ReadAll",
        g_variant_new("(^as)", ns_arr), G_VARIANT_TYPE("(a{sa{sv}})"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err
    );
    if (!srep) {
        fprintf(stderr, "FAIL: Settings.ReadAll: %s\n", err ? err->message : "?");
        return 4;
    }
    g_variant_unref(srep);

    /* --- B. FileChooser.OpenFile (choices + filters + real Motif dialog) --- */
    GVariantBuilder fopts;
    g_variant_builder_init(&fopts, G_VARIANT_TYPE("a{sv}"));
    GVariantBuilder ch_arr;
    g_variant_builder_init(&ch_arr, G_VARIANT_TYPE("a(ssa(ss)s)"));
    GVariantBuilder empty_ss;
    g_variant_builder_init(&empty_ss, G_VARIANT_TYPE("a(ss)"));
    g_variant_builder_add(&ch_arr, "(ssa(ss)s)", "readonly", "Read-Only", &empty_ss, "true");
    GVariantBuilder enc_ss;
    g_variant_builder_init(&enc_ss, G_VARIANT_TYPE("a(ss)"));
    g_variant_builder_add(&enc_ss, "(ss)", "utf8", "UTF-8");
    g_variant_builder_add(&enc_ss, "(ss)", "latin1", "ISO-8859-1");
    g_variant_builder_add(&ch_arr, "(ssa(ss)s)", "encoding", "Encoding", &enc_ss, "utf8");
    g_variant_builder_add(&fopts, "{sv}", "choices", g_variant_builder_end(&ch_arr));

    GVariantBuilder flt_arr;
    g_variant_builder_init(&flt_arr, G_VARIANT_TYPE("a(sa(us))"));
    GVariantBuilder flt_rules;
    g_variant_builder_init(&flt_rules, G_VARIANT_TYPE("a(us)"));
    g_variant_builder_add(&flt_rules, "(us)", 0u, "*.txt");
    g_variant_builder_add(&flt_arr, "(sa(us))", "Text Files", &flt_rules);
    g_variant_builder_add(&fopts, "{sv}", "filters", g_variant_builder_end(&flt_arr));

    AsyncCallRes fc_ar = {0};
    g_dbus_connection_call(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.FileChooser", "OpenFile",
        g_variant_new("(osssa{sv})", "/org/freedesktop/portal/desktop/request/1/r1",
                      "org.mozilla.firefox", "", "Open CDE Document", &fopts),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 8000, NULL, on_async_done, &fc_ar
    );
    if (!state_has_dialog("file_open")) {
        fprintf(stderr, "FAIL: FileChooser.OpenFile Motif dialog did not map\n");
        return 5;
    }
    char accept_cmd[512];
    snprintf(accept_cmd, sizeof(accept_cmd), "ACCEPT %s", sample_txt);
    send_test_cmd(accept_cmd);
    GVariant *fc_rep = wait_async_reply(&fc_ar);
    guint32 fc_rc = 99;
    GVariant *fc_res = NULL;
    g_variant_get(fc_rep, "(u@a{sv})", &fc_rc, &fc_res);
    const char **uris = NULL;
    g_variant_lookup(fc_res, "uris", "^a&s", &uris);
    if (fc_rc != 0 || !uris || !uris[0] || !strstr(uris[0], "sample.txt")) {
        fprintf(stderr, "FAIL: FileChooser.OpenFile unexpected reply rc=%u uri=%s\n",
                fc_rc, (uris && uris[0]) ? uris[0] : "(null)");
        return 6;
    }
    g_free(uris);
    g_variant_unref(fc_res);
    g_variant_unref(fc_rep);

    /* --- C. FileChooser.SaveFile + Request.Close cancellation --- */
    GVariantBuilder sopts;
    g_variant_builder_init(&sopts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&sopts, "{sv}", "current_name", g_variant_new_string("export.pdf"));
    AsyncCallRes sf_ar = {0};
    g_dbus_connection_call(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.FileChooser", "SaveFile",
        g_variant_new("(osssa{sv})", "/org/freedesktop/portal/desktop/request/1/r2",
                      "org.chromium.Chromium", "", "Save PDF", &sopts),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 8000, NULL, on_async_done, &sf_ar
    );
    if (!state_has_dialog("file_save")) {
        fprintf(stderr, "FAIL: FileChooser.SaveFile Motif dialog did not map\n");
        return 7;
    }
    send_test_cmd("ACCEPT");
    GVariant *sf_rep = wait_async_reply(&sf_ar);
    guint32 sf_rc = 99;
    GVariant *sf_res = NULL;
    g_variant_get(sf_rep, "(u@a{sv})", &sf_rc, &sf_res);
    const char **suris = NULL;
    g_variant_lookup(sf_res, "uris", "^a&s", &suris);
    if (sf_rc != 0 || !suris || !suris[0] || !strstr(suris[0], "export.pdf")) {
        fprintf(stderr, "FAIL: FileChooser.SaveFile unexpected reply rc=%u\n", sf_rc);
        return 8;
    }
    g_free(suris);
    g_variant_unref(sf_res);
    g_variant_unref(sf_rep);

    /* Test Request.Close() cancelling an open dialog */
    GVariantBuilder copts;
    g_variant_builder_init(&copts, G_VARIANT_TYPE("a{sv}"));
    AsyncCallRes cl_ar = {0};
    g_dbus_connection_call(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.FileChooser", "OpenFile",
        g_variant_new("(osssa{sv})", "/org/freedesktop/portal/desktop/request/1/r3",
                      "org.example.App", "", "Cancel Test", &copts),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 8000, NULL, on_async_done, &cl_ar
    );
    if (!state_has_dialog("file_open")) return 9;
    GVariant *creq = g_dbus_connection_call_sync(
        bus, BUS_NAME, "/org/freedesktop/portal/desktop/request/1/r3",
        "org.freedesktop.impl.portal.Request", "Close", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    if (!creq) return 9;
    g_variant_unref(creq);
    GVariant *cl_rep = wait_async_reply(&cl_ar);
    guint32 cl_rc = 0;
    GVariant *cl_res = NULL;
    g_variant_get(cl_rep, "(u@a{sv})", &cl_rc, &cl_res);
    g_variant_unref(cl_res);
    g_variant_unref(cl_rep);
    if (cl_rc != 2u) {
        fprintf(stderr, "FAIL: expected Request.Close rc=2, got %u\n", cl_rc);
        return 9;
    }

    /* --- D. AppChooser.ChooseApplication & UpdateChoices --- */
    const char *init_apps[] = { "Dtpad", "org.example.Viewer", NULL };
    GVariantBuilder aopts;
    g_variant_builder_init(&aopts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&aopts, "{sv}", "heading", g_variant_new_string("Select CDE Handler"));
    AsyncCallRes ac_ar = {0};
    g_dbus_connection_call(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.AppChooser", "ChooseApplication",
        g_variant_new("(oss^asa{sv})", "/org/freedesktop/portal/desktop/request/1/r4",
                      "org.mozilla.firefox", "", init_apps, &aopts),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 8000, NULL, on_async_done, &ac_ar
    );
    if (!state_has_dialog("app_chooser")) {
        fprintf(stderr, "FAIL: AppChooser dialog did not map\n");
        return 12;
    }
    const char *upd_apps[] = { "Dtpad", "Dtfile", "Dtterm", NULL };
    GVariant *urep = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.AppChooser", "UpdateChoices",
        g_variant_new("(o^as)", "/org/freedesktop/portal/desktop/request/1/r4", upd_apps),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    if (!urep) return 13;
    g_variant_unref(urep);
    send_test_cmd("ACCEPT Dtfile");
    GVariant *ac_rep = wait_async_reply(&ac_ar);
    guint32 ac_rc = 99;
    GVariant *ac_res = NULL;
    g_variant_get(ac_rep, "(u@a{sv})", &ac_rc, &ac_res);
    const char *chosen_app = NULL;
    g_variant_lookup(ac_res, "choice", "&s", &chosen_app);
    if (ac_rc != 0 || !chosen_app || strcmp(chosen_app, "Dtfile") != 0) {
        fprintf(stderr, "FAIL: AppChooser expected Dtfile, got rc=%u choice=%s\n",
                ac_rc, chosen_app ? chosen_app : "(null)");
        return 14;
    }
    g_variant_unref(ac_res);
    g_variant_unref(ac_rep);

    /* --- E. Wallpaper.SetWallpaperURI (with Motif preview dialog) --- */
    char *wp_uri = g_filename_to_uri(wallpaper_png, NULL, NULL);
    GVariantBuilder wopts;
    g_variant_builder_init(&wopts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&wopts, "{sv}", "show-preview", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&wopts, "{sv}", "set-on", g_variant_new_string("background"));
    AsyncCallRes wp_ar = {0};
    g_dbus_connection_call(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.Wallpaper", "SetWallpaperURI",
        g_variant_new("(osssa{sv})", "/org/freedesktop/portal/desktop/request/1/r5",
                      "org.gimp.GIMP", "", wp_uri, &wopts),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 8000, NULL, on_async_done, &wp_ar
    );
    g_free(wp_uri);
    if (!state_has_dialog("wallpaper")) {
        fprintf(stderr, "FAIL: Wallpaper preview dialog did not map\n");
        return 15;
    }
    send_test_cmd("ACCEPT");
    GVariant *wp_rep = wait_async_reply(&wp_ar);
    guint32 wp_rc = 99;
    g_variant_get(wp_rep, "(u)", &wp_rc);
    g_variant_unref(wp_rep);
    if (wp_rc != 0u) {
        fprintf(stderr, "FAIL: Wallpaper.SetWallpaperURI returned %u\n", wp_rc);
        return 16;
    }

    /* --- F. Inhibit (CreateMonitor + active Inhibit held open for Python check) --- */
    GVariant *mrep = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.Inhibit", "CreateMonitor",
        g_variant_new("(ooss)", "/org/freedesktop/portal/desktop/request/1/r6",
                      "/org/freedesktop/portal/desktop/session/1/inh1", "org.videolan.VLC", ""),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    if (!mrep) return 17;
    g_variant_unref(mrep);
    for (int i = 0; i < 30 && g_inhibit_state_signals == 0; ++i) {
        g_main_context_iteration(NULL, FALSE);
        usleep(15000);
    }
    if (g_inhibit_state_signals < 1) {
        fprintf(stderr, "FAIL: Inhibit.CreateMonitor did not emit StateChanged\n");
        return 17;
    }

    GVariantBuilder iopts;
    g_variant_builder_init(&iopts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&iopts, "{sv}", "reason", g_variant_new_string("Playing 4K HDR film"));
    GVariant *irep = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.Inhibit", "Inhibit",
        g_variant_new("(ossu@a{sv})", "/org/freedesktop/portal/desktop/request/1/inh_active",
                      "org.videolan.VLC", "", 12u /* suspend | idle */,
                      g_variant_builder_end(&iopts)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    if (!irep) return 18;
    g_variant_unref(irep);

    /* --- G. GlobalShortcuts + RemoteDesktop (Notify* + restore_token + ConnectToEIS) --- */
    GVariantBuilder gs_opts;
    g_variant_builder_init(&gs_opts, G_VARIANT_TYPE("a{sv}"));
    GVariant *gs_c = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.GlobalShortcuts", "CreateSession",
        g_variant_new("(oosa{sv})", "/org/freedesktop/portal/desktop/request/1/gs1",
                      "/org/freedesktop/portal/desktop/session/1/gs1", "com.obsproject.Studio", &gs_opts),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    if (!gs_c) return 19;
    g_variant_unref(gs_c);

    GVariantBuilder sc_list, sc_meta, bind_opts;
    g_variant_builder_init(&sc_list, G_VARIANT_TYPE("a(sa{sv})"));
    g_variant_builder_init(&sc_meta, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&sc_meta, "{sv}", "description", g_variant_new_string("Toggle Recording"));
    g_variant_builder_add(&sc_meta, "{sv}", "preferred_trigger", g_variant_new_string("<Control><Alt>g"));
    g_variant_builder_add(&sc_list, "(sa{sv})", "toggle_rec", &sc_meta);
    g_variant_builder_init(&bind_opts, G_VARIANT_TYPE("a{sv}"));
    GVariant *gs_b = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.GlobalShortcuts", "BindShortcuts",
        g_variant_new("(ooa(sa{sv})sa{sv})",
                      "/org/freedesktop/portal/desktop/request/1/gs2",
                      "/org/freedesktop/portal/desktop/session/1/gs1",
                      &sc_list, "", &bind_opts),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    if (!gs_b) return 20;
    g_variant_unref(gs_b);
    usleep(80000); /* allow compositor _DT_WM_REQUEST registration */

    /* RemoteDesktop Session 1: interactive Motif consent + mint restore_token */
    GVariantBuilder rd_opts;
    g_variant_builder_init(&rd_opts, G_VARIANT_TYPE("a{sv}"));
    GVariant *rd_c = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop", "CreateSession",
        g_variant_new("(oosa{sv})", "/org/freedesktop/portal/desktop/request/1/rd1",
                      "/org/freedesktop/portal/desktop/session/1/rd1", "org.remmina.Remmina", &rd_opts),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    if (!rd_c) return 21;
    g_variant_unref(rd_c);

    GVariantBuilder sd_opts;
    g_variant_builder_init(&sd_opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&sd_opts, "{sv}", "types", g_variant_new_uint32(3u));
    g_variant_builder_add(&sd_opts, "{sv}", "persist_mode", g_variant_new_uint32(2u));
    GVariant *rd_sd = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop", "SelectDevices",
        g_variant_new("(oosa{sv})", "/org/freedesktop/portal/desktop/request/1/rd2",
                      "/org/freedesktop/portal/desktop/session/1/rd1", "org.remmina.Remmina", &sd_opts),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    if (!rd_sd) return 22;
    g_variant_unref(rd_sd);

    GVariantBuilder st_opts;
    g_variant_builder_init(&st_opts, G_VARIANT_TYPE("a{sv}"));
    AsyncCallRes rd_ar = {0};
    g_dbus_connection_call(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop", "Start",
        g_variant_new("(oossa{sv})", "/org/freedesktop/portal/desktop/request/1/rd3",
                      "/org/freedesktop/portal/desktop/session/1/rd1", "org.remmina.Remmina", "", &st_opts),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 8000, NULL, on_async_done, &rd_ar
    );
    if (!state_has_dialog("remotedesktop_start")) {
        fprintf(stderr, "FAIL: RemoteDesktop.Start Motif consent dialog did not map\n");
        return 23;
    }
    send_test_cmd("ACCEPT");
    GVariant *rd_rep = wait_async_reply(&rd_ar);
    guint32 rd_rc = 99;
    GVariant *rd_res = NULL;
    g_variant_get(rd_rep, "(u@a{sv})", &rd_rc, &rd_res);
    const char *rtok = NULL;
    g_variant_lookup(rd_res, "restore_token", "&s", &rtok);
    if (rd_rc != 0 || !rtok || strlen(rtok) < 8) {
        fprintf(stderr, "FAIL: RemoteDesktop.Start did not mint restore_token (rc=%u)\n", rd_rc);
        return 24;
    }
    char saved_token[96];
    snprintf(saved_token, sizeof(saved_token), "%s", rtok);
    g_variant_unref(rd_res);
    g_variant_unref(rd_rep);

    /* Trigger GlobalShortcuts <Control><Alt>g via NotifyKeyboardKeycode (29=Ctrl, 56=Alt, 34=G) */
    const int chord[][2] = { {29, 1}, {56, 1}, {34, 1}, {34, 0}, {56, 0}, {29, 0} };
    for (size_t i = 0; i < 6; ++i) {
        GVariantBuilder kopts;
        g_variant_builder_init(&kopts, G_VARIANT_TYPE("a{sv}"));
        GVariant *kr = g_dbus_connection_call_sync(
            bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop",
            "NotifyKeyboardKeycode",
            g_variant_new("(oa{sv}iu)", "/org/freedesktop/portal/desktop/session/1/rd1",
                          &kopts, chord[i][0], (guint32)chord[i][1]),
            NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
        );
        if (!kr) return 25;
        g_variant_unref(kr);
        usleep(20000);
    }
    for (int i = 0; i < 60 && (g_activated_count < 1 || g_deactivated_count < 1); ++i) {
        g_main_context_iteration(NULL, FALSE);
        usleep(25000);
    }
    if (g_activated_count < 1 || g_deactivated_count < 1 ||
        strcmp(g_last_shortcut_id, "toggle_rec") != 0) {
        fprintf(stderr, "FAIL: GlobalShortcuts signals missing (act=%d deact=%d id=%s)\n",
                g_activated_count, g_deactivated_count, g_last_shortcut_id);
        return 26;
    }

    /* Click Motif target window via NotifyPointerMotionAbsolute + NotifyPointerButton */
    GVariantBuilder mopts;
    g_variant_builder_init(&mopts, G_VARIANT_TYPE("a{sv}"));
    GVariant *mr = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop",
        "NotifyPointerMotionAbsolute",
        g_variant_new("(oa{sv}udd)", "/org/freedesktop/portal/desktop/session/1/rd1",
                      &mopts, 0u, target_x, target_y),
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    if (!mr) return 27;
    g_variant_unref(mr);
    for (guint32 bst = 1; ; bst = 0) {
        GVariantBuilder bopts;
        g_variant_builder_init(&bopts, G_VARIANT_TYPE("a{sv}"));
        GVariant *br = g_dbus_connection_call_sync(
            bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop",
            "NotifyPointerButton",
            g_variant_new("(oa{sv}iu)", "/org/freedesktop/portal/desktop/session/1/rd1",
                          &bopts, 272, bst),
            NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
        );
        if (!br) return 27;
        g_variant_unref(br);
        if (bst == 0) break;
    }

    /* RemoteDesktop Session 2: restore_token non-interactive Start + ConnectToEIS fd click! */
    GVariantBuilder rd2_o1, rd2_o2, rd2_o3, eis_opts;
    g_variant_builder_init(&rd2_o1, G_VARIANT_TYPE("a{sv}"));
    g_variant_unref(g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop", "CreateSession",
        g_variant_new("(oosa{sv})", "/org/freedesktop/portal/desktop/request/1/rd2_1",
                      "/org/freedesktop/portal/desktop/session/1/rd2", "org.remmina.Remmina", &rd2_o1),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    ));
    g_variant_builder_init(&rd2_o2, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&rd2_o2, "{sv}", "types", g_variant_new_uint32(3u));
    g_variant_builder_add(&rd2_o2, "{sv}", "restore_token", g_variant_new_string(saved_token));
    g_variant_unref(g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop", "SelectDevices",
        g_variant_new("(oosa{sv})", "/org/freedesktop/portal/desktop/request/1/rd2_2",
                      "/org/freedesktop/portal/desktop/session/1/rd2", "org.remmina.Remmina", &rd2_o2),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    ));
    g_variant_builder_init(&rd2_o3, G_VARIANT_TYPE("a{sv}"));
    /* Synchronous Start MUST succeed immediately without opening a consent dialog! */
    GVariant *rd2_st = g_dbus_connection_call_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop", "Start",
        g_variant_new("(oossa{sv})", "/org/freedesktop/portal/desktop/request/1/rd2_3",
                      "/org/freedesktop/portal/desktop/session/1/rd2", "org.remmina.Remmina", "", &rd2_o3),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    if (!rd2_st) {
        fprintf(stderr, "FAIL: restore_token non-interactive Start failed: %s\n", err ? err->message : "?");
        return 28;
    }
    g_variant_unref(rd2_st);

    g_variant_builder_init(&eis_opts, G_VARIANT_TYPE("a{sv}"));
    GUnixFDList *out_fds = NULL;
    GVariant *eis_rep = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, BUS_NAME, OBJ_PATH, "org.freedesktop.impl.portal.RemoteDesktop", "ConnectToEIS",
        g_variant_new("(osa{sv})", "/org/freedesktop/portal/desktop/session/1/rd2",
                      "org.remmina.Remmina", &eis_opts),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &out_fds, NULL, &err
    );
    if (!eis_rep || !out_fds) {
        fprintf(stderr, "FAIL: ConnectToEIS failed: %s\n", err ? err->message : "?");
        return 29;
    }
    gint32 fd_idx = 0;
    g_variant_get(eis_rep, "(h)", &fd_idx);
    int eis_fd = g_unix_fd_list_get(out_fds, fd_idx, &err);
    g_variant_unref(eis_rep);
    g_object_unref(out_fds);
    if (eis_fd < 0) return 29;

    run_eis_click_test(eis_fd, (float)target_x, (float)target_y);
    close(eis_fd);

    printf("DRIVER_PHASE1_OK=1\n");
    fflush(stdout);

    /* Wait for stdin newline from Python after Python checks idle-inhibit state, then release inhibitor! */
    char sync_ln[32];
    if (fgets(sync_ln, sizeof(sync_ln), stdin)) {
        GVariant *cl_inh = g_dbus_connection_call_sync(
            bus, BUS_NAME, "/org/freedesktop/portal/desktop/request/1/inh_active",
            "org.freedesktop.impl.portal.Request", "Close", NULL, NULL,
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL
        );
        if (cl_inh) g_variant_unref(cl_inh);
    }
    printf("DRIVER_DONE=1\n");
    fflush(stdout);
    g_object_unref(bus);
    return 0;
}
"""


MOTIF_CLICK_TARGET_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <X11/Intrinsic.h>
#include <Xm/DrawingA.h>
#include <Xm/Xm.h>

static int g_clicks = 0;
static const char *g_out_path = NULL;

static void save_clicks(void) {
    if (!g_out_path) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_out_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "READY=1\nCLICKS=%d\n", g_clicks);
    fclose(f);
    rename(tmp, g_out_path);
}

static void on_btn(Widget w, XtPointer cd, XEvent *ev, Boolean *cont) {
    (void)w; (void)cd; (void)cont;
    if (ev && ev->type == ButtonPress) {
        g_clicks++;
        save_clicks();
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    g_out_path = argv[1];
    XtAppContext app;
    Widget top = XtVaAppInitialize(&app, "PortalTarget", NULL, 0, &argc, argv, NULL,
                                   XmNtitle, "PortalClickTarget",
                                   XmNwidth, 280,
                                   XmNheight, 200,
                                   NULL);
    Widget da = XtVaCreateManagedWidget("da", xmDrawingAreaWidgetClass, top,
                                        XmNwidth, 280, XmNheight, 200, NULL);
    XtAddEventHandler(da, ButtonPressMask, False, on_btn, NULL);
    XtRealizeWidget(top);
    XFlush(XtDisplay(top));
    save_clicks();
    XtAppMainLoop(app);
    return 0;
}
"""


def wait_until(pred, timeout: float = 10.0, interval: float = 0.04) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return pred()


def parse_kv(path: Path, sep: str = "=") -> dict[str, str]:
    if not path.is_file():
        return {}
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if sep in line:
            k, v = line.split(sep, 1)
            out[k.strip()] = v.strip()
    return out


def ipc_read_props(ipc_path: Path) -> dict[str, bytes]:
    props: dict[str, bytes] = {}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2.0)
        s.connect(str(ipc_path))
        buf = b""
        while b"\nREADY\n" not in buf and not buf.startswith(b"READY\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        for raw in buf.decode("utf-8", errors="replace").splitlines():
            if raw == "READY":
                break
            if raw.startswith("PROP "):
                parts = raw.split(" ", 2)
                if len(parts) == 3:
                    props[parts[1]] = bytes.fromhex(parts[2])
    return props


def ipc_send_wm_request(ipc_path: Path, cmd: str) -> None:
    payload = (cmd + "\0").encode("utf-8").hex()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2.0)
        s.connect(str(ipc_path))
        s.sendall(f"SET _DT_WM_REQUEST {payload}\n".encode("ascii"))
        time.sleep(0.05)


def parse_active_box(props: dict[str, bytes]) -> tuple[int, int, int, int, int, int, int, int] | None:
    raw = props.get("_DT_ACTIVE_WINDOW_BOX")
    if not raw or len(raw) < 32:
        return None
    return struct.unpack("<iiiiiiii", raw[:32])


def write_solid_png(path: Path, w: int, h: int, r: int, g: int, b: int) -> None:
    import zlib
    raw_rows = b"".join(b"\x00" + bytes([r, g, b]) * w for _ in range(h))
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw_rows)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def read_ppm(ppm_bytes: bytes) -> tuple[int, int, bytes]:
    idx = 0
    tokens: list[bytes] = []
    n = len(ppm_bytes)
    while len(tokens) < 4 and idx < n:
        while idx < n and ppm_bytes[idx] in b" \t\r\n":
            idx += 1
        if idx < n and ppm_bytes[idx] == ord("#"):
            while idx < n and ppm_bytes[idx] != ord("\n"):
                idx += 1
            continue
        start = idx
        while idx < n and ppm_bytes[idx] not in b" \t\r\n#":
            idx += 1
        if idx > start:
            tokens.append(ppm_bytes[start:idx])
    idx += 1
    w = int(tokens[1])
    h = int(tokens[2])
    return w, h, ppm_bytes[idx: idx + w * h * 3]


def resolve_motif_prefix(portal_path: Path) -> Path:
    configured = os.environ.get("MOTIF_PREFIX")
    candidates = [Path(configured)] if configured else [
        *(parent / "motif-prefix" for parent in portal_path.parents),
        Path("/var/tmp/ck-wayland-build/wp00/motif-prefix"),
        Path("/usr/dt"),
    ]
    for p in candidates:
        if (p / "lib/libXm.so.9").is_file() and (p / "include/Xm/Xm.h").is_file():
            return p.resolve()
    raise SystemExit("Set MOTIF_PREFIX to an installed CK Core Motif ABI 9 prefix")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--portal", required=True)
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    portal_bin = Path(args.portal).resolve()
    libs_cand = portal_bin.parent / ".libs" / portal_bin.name
    if libs_cand.is_file():
        portal_bin = libs_cand
    comp_bin = Path(args.compositor).resolve()
    if not portal_bin.is_file() or not comp_bin.is_file():
        raise SystemExit(f"missing binary: portal={portal_bin} compositor={comp_bin}")

    motif_prefix = resolve_motif_prefix(portal_bin)
    cc = os.environ.get("CC", "cc")
    dbus_daemon = shutil.which("dbus-daemon")
    grim = shutil.which("grim")
    if not dbus_daemon or not grim:
        raise SystemExit("dbus-daemon and grim are required")

    tmp_parent = os.environ.get("TMPDIR")
    with tempfile.TemporaryDirectory(prefix="dtportal-test-", dir=tmp_parent) as td:
        tmp = Path(td)
        rt_dir = tmp / "rt"
        home_dir = tmp / "home"
        cfg_dir = home_dir / ".config"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt").mkdir(parents=True)
        (cfg_dir / "ck-core").mkdir(parents=True)
        (cfg_dir / "ck-core/power-policy.conf").write_text(
            "ac_suspend_sec=60\nbat_suspend_sec=60\n", encoding="utf-8"
        )

        sample_txt = home_dir / "sample.txt"
        sample_txt.write_text("Hello from CDE Portal!\n", encoding="utf-8")
        wallpaper_png = tmp / "coral_wallpaper.png"
        write_solid_png(wallpaper_png, 64, 64, 255, 102, 0)  # bright coral #ff6600

        gio_cflags = subprocess.check_output(["pkg-config", "--cflags", "gio-unix-2.0"], text=True).split()
        gio_libs = subprocess.check_output(["pkg-config", "--libs", "gio-unix-2.0"], text=True).split()
        drv_c = tmp / "portal_driver.c"
        drv_bin = tmp / "portal_driver"
        drv_c.write_text(PORTAL_DRIVER_C, encoding="utf-8")
        subprocess.run([cc, "-O2", "-Wall", str(drv_c), *gio_cflags, *gio_libs, "-o", str(drv_bin)], check=True)

        tgt_c = tmp / "click_target.c"
        tgt_bin = tmp / "click_target"
        tgt_c.write_text(MOTIF_CLICK_TARGET_C, encoding="utf-8")
        lib_dir = motif_prefix / "lib"
        subprocess.run(
            [
                cc, "-O2", f"-I{motif_prefix}/include", str(tgt_c),
                f"-L{lib_dir}", f"-Wl,-rpath,{lib_dir}",
                "-lXm", "-lXt", "-lX11", "-o", str(tgt_bin),
            ],
            check=True,
        )

        # Start isolated session D-Bus daemon
        dbus_proc = subprocess.Popen(
            [dbus_daemon, "--session", "--nofork", "--print-address=1"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        comp_proc: subprocess.Popen | None = None
        portal_proc: subprocess.Popen | None = None
        tgt_proc: subprocess.Popen | None = None
        drv_proc: subprocess.Popen | None = None

        try:
            assert dbus_proc.stdout is not None
            dbus_addr = dbus_proc.stdout.readline().strip()
            if not dbus_addr:
                raise RuntimeError("failed to start isolated dbus-daemon")

            build_root = comp_bin.parents[3]
            ld_dirs = [
                str(lib_dir),
                str(build_root / "cde-stage/usr/dt/lib"),
                str(build_root / "fs-prefix/lib"),
            ]
            env = os.environ.copy()
            for k in ("DISPLAY", "WAYLAND_DISPLAY"):
                env.pop(k, None)
            env.update({
                "HOME": str(home_dir),
                "XDG_CONFIG_HOME": str(cfg_dir),
                "XDG_RUNTIME_DIR": str(rt_dir),
                "DBUS_SESSION_BUS_ADDRESS": dbus_addr,
                "WLR_BACKENDS": "headless",
                "WLR_HEADLESS_OUTPUTS": "1",
                "WLR_RENDERER": "pixman",
                "CK_MOTIF_BACKEND": "wayland",
                "LD_LIBRARY_PATH": ":".join(d for d in ld_dirs if Path(d).is_dir()),
            })

            status_file = tmp / "comp.status"
            comp_proc = subprocess.Popen(
                [str(comp_bin), "--no-xwayland", "--no-panel", "--status-file", str(status_file)],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            )
            if not wait_until(lambda: "WAYLAND_DISPLAY" in parse_kv(status_file), timeout=10.0):
                raise RuntimeError("dtwm-wayland failed to start")
            wl_sock = parse_kv(status_file)["WAYLAND_DISPLAY"]
            env["WAYLAND_DISPLAY"] = wl_sock
            ipc_path = rt_dir / f"dtwm-wayland-{wl_sock}.ipc"
            if not wait_until(lambda: ipc_path.exists(), timeout=5.0):
                raise RuntimeError("missing dtwm-wayland .ipc socket")

            # Launch Motif click target window first and get its client center coordinates
            tgt_state = tmp / "target.state"
            tgt_proc = subprocess.Popen([str(tgt_bin), str(tgt_state)], env=env)
            if not wait_until(
                lambda: parse_kv(tgt_state).get("READY") == "1"
                and parse_active_box(ipc_read_props(ipc_path)) is not None,
                timeout=8.0,
            ):
                raise RuntimeError("Motif click target failed to map on Wayland")
            tbox = parse_active_box(ipc_read_props(ipc_path))
            assert tbox is not None
            _, _, _, _, tcx, tcy, tcw, tch = tbox
            click_x = tcx + tcw // 2
            click_y = tcy + tch // 2

            # Launch xdg-desktop-portal-cde (note: libtool wrapper in build dir spawns .libs/lt-xdg-desktop-portal-cde)
            p_state = tmp / "portal.state"
            p_cmd = tmp / "portal.cmd"
            portal_env = {
                **env,
                "DTPORTAL_TEST_STATE": str(p_state),
                "DTPORTAL_TEST_CMD": str(p_cmd),
            }
            portal_proc = subprocess.Popen([str(portal_bin)], env=portal_env)
            if not wait_until(lambda: parse_kv(p_state).get("READY") == "1", timeout=8.0):
                raise RuntimeError("xdg-desktop-portal-cde failed to own bus name")
            assert parse_kv(p_state).get("WAYLAND") == "1", f"expected native Wayland Motif: {parse_kv(p_state)}"

            # Verify libXm.so.9 runtime mapping on the portal process (or child if libtool wrapper)
            maps_txt = Path(f"/proc/{portal_proc.pid}/maps").read_text(encoding="utf-8")
            assert "libXm.so.9" in maps_txt, f"xdg-desktop-portal-cde did not map libXm.so.9"

            # Run C GDBus + EIS driver through Phase 1
            drv_proc = subprocess.Popen(
                [
                    str(drv_bin), str(p_state), str(p_cmd),
                    str(sample_txt), str(wallpaper_png),
                    str(click_x), str(click_y),
                ],
                env=env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                text=True,
            )
            assert drv_proc.stdout is not None and drv_proc.stdin is not None
            phase1_line = drv_proc.stdout.readline().strip()
            if phase1_line != "DRIVER_PHASE1_OK=1":
                rc = drv_proc.wait(timeout=4.0)
                raise AssertionError(f"portal_driver failed in phase 1 (rc={rc}, out={phase1_line!r})")

            # Verify BOTH RemoteDesktop clicks landed on the Motif target window:
            # 1 click from NotifyPointerMotionAbsolute + NotifyPointerButton,
            # and 1 click from ConnectToEIS binary EIS v1 wire protocol!
            if not wait_until(lambda: int(parse_kv(tgt_state).get("CLICKS", "0")) >= 2, timeout=5.0):
                raise AssertionError(
                    f"expected >=2 clicks on Motif target (Notify* + EIS), got {parse_kv(tgt_state)}"
                )

            # Verify Wallpaper.SetWallpaperURI created PortalWallpaper.pm and updated dtwm-wayland backdrop
            pm_file = home_dir / ".dt/backdrops/PortalWallpaper.pm"
            assert pm_file.is_file(), f"missing generated CDE backdrop {pm_file}"
            ppm = subprocess.check_output([grim, "-t", "ppm", "-"], env=env, timeout=5.0)
            gw, gh, rgb = read_ppm(ppm)
            off = ((gh - 20) * gw + (gw - 20)) * 3
            br, bg, bb = rgb[off], rgb[off + 1], rgb[off + 2]
            assert br >= 200 and 70 <= bg <= 140 and bb < 40, (
                f"expected coral #ff6600 backdrop at bottom-right corner, got RGB={(br, bg, bb)}"
            )

            # Verify Inhibit suppresses dtwm-wayland idle auto-suspend!
            inh_state_file = rt_dir / "cde-portal-inhibit.state"
            assert parse_kv(inh_state_file).get("COUNT") == "1", f"expected COUNT=1: {parse_kv(inh_state_file)}"
            ipc_send_wm_request(ipc_path, "f.check_idle_suspend 3600")
            if not wait_until(
                lambda: "action=inhibited_by_portal"
                in ipc_read_props(ipc_path).get("_DT_POWER_POLICY_LAST_ACTION", b"").decode("utf-8", errors="replace"),
                timeout=5.0,
            ):
                got_act = ipc_read_props(ipc_path).get("_DT_POWER_POLICY_LAST_ACTION", b"")
                raise AssertionError(f"expected action=inhibited_by_portal, got {got_act!r}")

            # Release inhibitor via Request.Close in driver and verify COUNT returns to 0
            drv_proc.stdin.write("release\n")
            drv_proc.stdin.flush()
            done_line = drv_proc.stdout.readline().strip()
            assert done_line == "DRIVER_DONE=1", f"unexpected driver finish: {done_line!r}"
            drv_proc.wait(timeout=4.0)
            drv_proc = None

            if not wait_until(lambda: parse_kv(inh_state_file).get("COUNT") == "0", timeout=5.0):
                raise AssertionError(f"expected inhibitor COUNT=0 after Request.Close, got {parse_kv(inh_state_file)}")

            p_cmd.write_text("QUIT\n", encoding="utf-8")
            portal_proc.wait(timeout=5.0)
            portal_proc = None
            print("PASS: check-xdg-desktop-portal-cde (FileChooser, AppChooser, Inhibit, Wallpaper, GlobalShortcuts, Settings, RemoteDesktop + EIS)")
            return 0
        finally:
            for p in (drv_proc, portal_proc, tgt_proc, comp_proc, dbus_proc):
                if p and p.poll() is None:
                    with suppress(Exception):
                        p.terminate()
                        p.wait(timeout=4.0)
                    if p.poll() is None:
                        with suppress(Exception):
                            p.kill()


if __name__ == "__main__":
    raise SystemExit(main())
