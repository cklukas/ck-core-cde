#!/usr/bin/env python3
"""WP-18 parallel dual-environment integration check (Wayland + X11/XLibre):
1. Runtime Motif ABI 9 (`libXm.so.9`) Linkage:
   - Verifies `dtstyle`, `dtwm` / `dtwm-panel`, `dtprintinfo`, `ck-status`, and `ck-admin`
     all map CK Core Motif ABI 9 (`libXm.so.9`) via `/proc/<pid>/maps`.
2. Night Light, VRR & Backlight Persistence:
   - Launches `dtstyle -display_dialog`, sets Night Light `3400 K` (`always`) + VRR ON via
     `_DTSTYLE_DISPLAY_IPC`, verifies `~/.dt/display_ergonomics.conf` + `_DT_NIGHT_LIGHT_STATE`,
     verifies real warm pixel delta via `grim` on headless Wayland AND `XRRGetCrtcGamma`
     (`blue < green < red`) + `_DT_VRR_ENABLED` + Cancel revert on X11, plus `dtwm-media-ctl`
     `~/.dt/backlight.conf` save & `bright-restore`.
3. Keyboard Window Tiling (`Super+Left/Right/Up/Down`):
   - Exercises Left Half, Top-Left Quadrant, Top-Right Quadrant, Pre-tile Geometry Restore,
     and Bottom Half (verifying Front Panel strut avoidance) in both `dtwm-wayland` and X11 `dtwm`.
4. Power Profiles, Idle Auto-Suspend & Laptop Lid Policy (`ck-status`):
   - Switches `power-saver` / `balanced` / `performance` against mocked
     `org.freedesktop.UPower.PowerProfiles` + `$CK_POWER_PROFILE_SYSFS`.
   - Verifies dual-monitor docked clamshell lid-close disables internal panel with 0 suspend calls,
     single-monitor battery lid-close invokes `org.freedesktop.login1.Manager.Suspend`, and idle
     auto-suspend invokes `Suspend` + updates `_DT_POWER_POLICY_LAST_ACTION` in both sessions.
5. CUPS / IPP Everywhere (`dtprintinfo`) & System Administration (`ck-admin`):
   - Verifies `dtprintinfo --cups-summary` + GUI `libXm.so.9` linkage with multi-queue IPP jobs.
   - Verifies `ck-admin` `XmTabStack` Default Applications (`mimeapps.list`), Date/Time & NTP
     (`org.freedesktop.timedate1`), Firmware Updates (`org.freedesktop.fwupd`), and `-session`
     save/restore.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import zlib


X11_WP18_HELPER_C = r"""
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int win_has_prop(Display *dpy, Window w, Atom state_atom) {
    Atom act = None;
    int fmt = 0;
    unsigned long n = 0, rem = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, state_atom, 0, 4, False, AnyPropertyType,
                           &act, &fmt, &n, &rem, &data) == Success && data) {
        XFree(data);
        return act != None && n > 0;
    }
    return 0;
}

static Window find_by_state_atom(Display *dpy, Window w, Atom state_atom) {
    if (win_has_prop(dpy, w, state_atom)) return w;
    Window root = None, parent = None, *kids = NULL;
    unsigned int nkids = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &kids, &nkids)) return None;
    Window found = None;
    for (unsigned int i = 0; i < nkids && !found; ++i) {
        found = find_by_state_atom(dpy, kids[i], state_atom);
    }
    if (kids) XFree(kids);
    return found;
}

static void outer_frame_geom(Display *dpy, Window client, int *ox, int *oy, int *ow, int *oh) {
    Window root = DefaultRootWindow(dpy);
    Window cur = client, parent = None, r = None, *kids = NULL;
    unsigned int nkids = 0;
    while (cur != None && cur != root) {
        if (!XQueryTree(dpy, cur, &r, &parent, &kids, &nkids)) break;
        if (kids) XFree(kids);
        if (parent == root || parent == None) break;
        cur = parent;
    }
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, cur, &wa)) {
        int rx = wa.x, ry = wa.y;
        Window child = None;
        if (cur == client) {
            XTranslateCoordinates(dpy, client, root, 0, 0, &rx, &ry, &child);
        }
        *ox = rx;
        *oy = ry;
        *ow = wa.width;
        *oh = wa.height;
    }
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 2;
    Window root = DefaultRootWindow(dpy);

    if (argc < 2 || strcmp(argv[1], "ping") == 0) {
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "get") == 0) {
        Atom prop = XInternAtom(dpy, argv[2], False);
        Window w = find_by_state_atom(dpy, root, prop);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom act = None; int fmt = 0; unsigned long n = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, w, prop, 0, 65536, False, AnyPropertyType,
                               &act, &fmt, &n, &rem, &data) == Success && data) {
            fwrite(data, 1, n, stdout);
            XFree(data);
            XCloseDisplay(dpy);
            return 0;
        }
        XCloseDisplay(dpy);
        return 4;
    }
    if (argc >= 3 && strcmp(argv[1], "get_root") == 0) {
        Atom prop = XInternAtom(dpy, argv[2], False);
        Atom act = None; int fmt = 0; unsigned long n = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, prop, 0, 65536, False, AnyPropertyType,
                               &act, &fmt, &n, &rem, &data) == Success && data && act != None) {
            if (fmt == 8) {
                fwrite(data, 1, n, stdout);
            } else if (fmt == 32 && n >= 1) {
                printf("0x%lx", *(unsigned long *)data);
            }
            XFree(data);
            XCloseDisplay(dpy);
            return 0;
        }
        if (data) XFree(data);
        XCloseDisplay(dpy);
        return 3;
    }
    if (argc >= 4 && strcmp(argv[1], "set_root") == 0) {
        Atom prop = XInternAtom(dpy, argv[2], False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        XChangeProperty(dpy, root, prop, utf8, 8, PropModeReplace,
                        (const unsigned char *)argv[3], (int)strlen(argv[3]));
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 5 && strcmp(argv[1], "set") == 0) {
        Atom state_atom = XInternAtom(dpy, argv[2], False);
        Window w = find_by_state_atom(dpy, root, state_atom);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom prop = XInternAtom(dpy, argv[3], False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        XChangeProperty(dpy, w, prop, utf8, 8, PropModeReplace,
                        (const unsigned char *)argv[4], (int)strlen(argv[4]));
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 5 && strcmp(argv[1], "set_win") == 0) {
        Window w = (Window)strtoul(argv[2], NULL, 0);
        Atom prop = XInternAtom(dpy, argv[3], False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        XChangeProperty(dpy, w, prop, utf8, 8, PropModeReplace,
                        (const unsigned char *)argv[4], (int)strlen(argv[4]));
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "randr_check") == 0) {
        XRRScreenResources *sr = XRRGetScreenResourcesCurrent(dpy, root);
        if (!sr || sr->ncrtc < 1) { XCloseDisplay(dpy); return 3; }
        XRRCrtcGamma *gm = XRRGetCrtcGamma(dpy, sr->crtcs[0]);
        int r_last = -1, g_last = -1, b_last = -1;
        if (gm && gm->size > 0) {
            r_last = gm->red[gm->size - 1];
            g_last = gm->green[gm->size - 1];
            b_last = gm->blue[gm->size - 1];
            XRRFreeGamma(gm);
        }
        int vrr_val = -1;
        Atom a_vrr = XInternAtom(dpy, "_DT_VRR_ENABLED", False);
        for (int o = 0; o < sr->noutput; ++o) {
            Atom act = None; int fmt = 0; unsigned long n = 0, rem = 0;
            unsigned char *data = NULL;
            if (XRRGetOutputProperty(dpy, sr->outputs[o], a_vrr, 0, 1, False, False,
                                     AnyPropertyType, &act, &fmt, &n, &rem, &data) == Success && data) {
                if (fmt == 32 && n >= 1) vrr_val = (int)(*(long *)data);
                XFree(data);
                if (vrr_val >= 0) break;
            }
        }
        XRRFreeScreenResources(sr);
        printf("{\"r\":%d,\"g\":%d,\"b\":%d,\"vrr\":%d}\n", r_last, g_last, b_last, vrr_val);
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "test_client") == 0) {
        /* Persistent white resizable window that publishes _WP18_TEST_WIN */
        int x = 120, y = 90, w = 420, h = 300;
        int scr = DefaultScreen(dpy);
        Window win = XCreateSimpleWindow(dpy, root, x, y, (unsigned)w, (unsigned)h, 0,
                                         BlackPixel(dpy, scr), WhitePixel(dpy, scr));
        XStoreName(dpy, win, argv[2]);
        XSizeHints sh;
        memset(&sh, 0, sizeof(sh));
        sh.flags = USPosition | USSize | PPosition | PSize | PMinSize;
        sh.x = x; sh.y = y; sh.width = w; sh.height = h;
        sh.min_width = 120; sh.min_height = 90;
        XSetWMNormalHints(dpy, win, &sh);
        GC gc = XCreateGC(dpy, win, 0, NULL);
        XSetForeground(dpy, gc, WhitePixel(dpy, scr));
        Atom marker = XInternAtom(dpy, "_WP18_TEST_WIN", False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        XChangeProperty(dpy, win, marker, utf8, 8, PropModeReplace,
                        (const unsigned char *)"ready", 5);
        XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
        XMapRaised(dpy, win);
        XFillRectangle(dpy, win, gc, 0, 0, 2000, 2000);
        XFlush(dpy);
        for (;;) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose || ev.type == ConfigureNotify || ev.type == MapNotify) {
                XFillRectangle(dpy, win, gc, 0, 0, 2000, 2000);
                XFlush(dpy);
            }
        }
    }
    if (argc >= 2 && strcmp(argv[1], "client_geom") == 0) {
        Atom marker = XInternAtom(dpy, "_WP18_TEST_WIN", False);
        Window w = find_by_state_atom(dpy, root, marker);
        if (!w) { XCloseDisplay(dpy); return 3; }
        int ox = 0, oy = 0, ow = 0, oh = 0;
        outer_frame_geom(dpy, w, &ox, &oy, &ow, &oh);
        XWindowAttributes cwa;
        XGetWindowAttributes(dpy, w, &cwa);
        int cx = 0, cy = 0;
        Window ch = None;
        XTranslateCoordinates(dpy, w, root, 0, 0, &cx, &cy, &ch);
        printf("{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"cx\":%d,\"cy\":%d,\"cw\":%d,\"ch\":%d}\n",
               ox, oy, ow, oh, cx, cy, cwa.width, cwa.height);
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "focus_test_client") == 0) {
        Atom marker = XInternAtom(dpy, "_WP18_TEST_WIN", False);
        Window w = find_by_state_atom(dpy, root, marker);
        if (!w) { XCloseDisplay(dpy); return 3; }
        XMapRaised(dpy, w);
        XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
        int cx = 200, cy = 200;
        Window ch = None;
        XTranslateCoordinates(dpy, w, root, 40, 40, &cx, &cy, &ch);
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, cx, cy);
        XFlush(dpy);
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "super_arrow") == 0) {
        KeySym ks_super = XK_Super_L;
        KeySym ks_arrow = NoSymbol;
        if (strcasecmp(argv[2], "left") == 0) ks_arrow = XK_Left;
        else if (strcasecmp(argv[2], "right") == 0) ks_arrow = XK_Right;
        else if (strcasecmp(argv[2], "up") == 0) ks_arrow = XK_Up;
        else if (strcasecmp(argv[2], "down") == 0) ks_arrow = XK_Down;
        KeyCode kc_super = XKeysymToKeycode(dpy, ks_super);
        KeyCode kc_arrow = XKeysymToKeycode(dpy, ks_arrow);
        if (!kc_super || !kc_arrow) { XCloseDisplay(dpy); return 4; }
        XTestFakeKeyEvent(dpy, kc_super, True, CurrentTime);
        XFlush(dpy);
        usleep(20000);
        XTestFakeKeyEvent(dpy, kc_arrow, True, CurrentTime);
        XFlush(dpy);
        usleep(20000);
        XTestFakeKeyEvent(dpy, kc_arrow, False, CurrentTime);
        XTestFakeKeyEvent(dpy, kc_super, False, CurrentTime);
        XFlush(dpy);
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "wm_save_yourself") == 0) {
        Atom a_save_mode = XInternAtom(dpy, "_DT_SAVE_MODE", False);
        Atom a_rest_mode = XInternAtom(dpy, "_DT_RESTORE_MODE", False);
        XChangeProperty(dpy, root, a_save_mode, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)"current", 7);
        XChangeProperty(dpy, root, a_rest_mode, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)"current", 7);
        XFlush(dpy);

        Atom state_atom = XInternAtom(dpy, argv[2], False);
        Window w = find_by_state_atom(dpy, root, state_atom);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom wm_proto = XInternAtom(dpy, "WM_PROTOCOLS", False);
        Atom wm_save = XInternAtom(dpy, "WM_SAVE_YOURSELF", False);
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = w;
        ev.xclient.message_type = wm_proto;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)wm_save;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, w, False, NoEventMask, &ev);
        XFlush(dpy);
        for (int attempt = 0; attempt < 40; ++attempt) {
            usleep(50000);
            char **cmd_argv = NULL;
            int cmd_argc = 0;
            if (XGetCommand(dpy, w, &cmd_argv, &cmd_argc) && cmd_argc >= 3 && cmd_argv) {
                for (int i = 0; i + 1 < cmd_argc; ++i) {
                    if (strcmp(cmd_argv[i], "-session") == 0) {
                        printf("%s", cmd_argv[i + 1]);
                        XFreeStringList(cmd_argv);
                        XCloseDisplay(dpy);
                        return 0;
                    }
                }
                XFreeStringList(cmd_argv);
            }
        }
        XCloseDisplay(dpy);
        return 5;
    }
    XCloseDisplay(dpy);
    return 1;
}
"""


MOCK_WP18_DBUS_C = r"""
/*
 * Lightweight session-bus mock daemon for WP-18 integration tests:
 *   - org.freedesktop.UPower.PowerProfiles (ActiveProfile Get/Set)
 *   - org.freedesktop.login1.Manager (Suspend counter)
 *   - org.freedesktop.timedate1 (Timezone, NTP, SetTimezone, SetNTP)
 *   - org.freedesktop.fwupd (DaemonVersion, GetDevices, GetReleases, RefreshMetadata)
 */
#define _GNU_SOURCE
#include <assert.h>
#include <dbus/dbus.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void write_state(const char *path, const char *prof, int suspends,
                        const char *tz, int ntp, int fw_refreshes) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    fprintf(fp,
            "{\"active_profile\":\"%s\",\"suspend_calls\":%d,"
            "\"timezone\":\"%s\",\"ntp\":%s,\"fw_refreshes\":%d}\n",
            prof, suspends, tz, ntp ? "true" : "false", fw_refreshes);
    fclose(fp);
    rename(tmp, path);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *state_file = argv[1];
    const char *ready_file = argv[2];
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    assert(conn != NULL);

    const char *names[] = {
        "org.freedesktop.UPower.PowerProfiles",
        "org.freedesktop.login1",
        "org.freedesktop.timedate1",
        "org.freedesktop.fwupd",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(dbus_bus_request_name(conn, names[i], DBUS_NAME_FLAG_REPLACE_EXISTING, &err) > 0);
    }

    char active_prof[64] = "balanced";
    int suspend_calls = 0;
    char cur_tz[128] = "Europe/Berlin";
    dbus_bool_t cur_ntp = TRUE;
    int fw_refreshes = 0;

    write_state(state_file, active_prof, suspend_calls, cur_tz, cur_ntp != FALSE, fw_refreshes);
    FILE *rf = fopen(ready_file, "w");
    if (rf) { fputs("ready\n", rf); fclose(rf); }

    while (!g_stop) {
        if (!dbus_connection_read_write(conn, 40)) break;
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(conn)) != NULL) {
            if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Get")) {
                const char *iface = NULL, *prop = NULL;
                if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &iface,
                                          DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
                    DBusMessage *rep = dbus_message_new_method_return(msg);
                    DBusMessageIter it, var;
                    dbus_message_iter_init_append(rep, &it);
                    if ((strcmp(iface, "org.freedesktop.UPower.PowerProfiles") == 0 ||
                         strcmp(iface, "net.hadess.PowerProfiles") == 0) &&
                        strcmp(prop, "ActiveProfile") == 0) {
                        const char *v = active_prof;
                        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &var);
                        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
                        dbus_message_iter_close_container(&it, &var);
                    } else if (strcmp(iface, "org.freedesktop.timedate1") == 0 &&
                               strcmp(prop, "Timezone") == 0) {
                        const char *v = cur_tz;
                        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &var);
                        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
                        dbus_message_iter_close_container(&it, &var);
                    } else if (strcmp(iface, "org.freedesktop.timedate1") == 0 &&
                               (strcmp(prop, "NTP") == 0 || strcmp(prop, "NTPSynchronized") == 0 ||
                                strcmp(prop, "CanNTP") == 0)) {
                        dbus_bool_t b = (strcmp(prop, "CanNTP") == 0) ? TRUE : cur_ntp;
                        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var);
                        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
                        dbus_message_iter_close_container(&it, &var);
                    } else if (strcmp(iface, "org.freedesktop.fwupd") == 0 &&
                               strcmp(prop, "DaemonVersion") == 0) {
                        const char *v = "2.0.5-wp18";
                        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &var);
                        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
                        dbus_message_iter_close_container(&it, &var);
                    } else {
                        dbus_message_unref(rep);
                        rep = dbus_message_new_error(msg, "org.freedesktop.DBus.Error.UnknownProperty", "unknown");
                    }
                    dbus_connection_send(conn, rep, NULL);
                    dbus_message_unref(rep);
                }
            } else if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Set")) {
                DBusMessageIter it, sub;
                if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                    const char *iface = NULL, *prop = NULL;
                    dbus_message_iter_get_basic(&it, &iface);
                    dbus_message_iter_next(&it);
                    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                        dbus_message_iter_get_basic(&it, &prop);
                        dbus_message_iter_next(&it);
                        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
                            dbus_message_iter_recurse(&it, &sub);
                            if (prop && strcmp(prop, "ActiveProfile") == 0 &&
                                dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
                                const char *nv = NULL;
                                dbus_message_iter_get_basic(&sub, &nv);
                                if (nv) snprintf(active_prof, sizeof(active_prof), "%s", nv);
                            }
                        }
                    }
                }
                write_state(state_file, active_prof, suspend_calls, cur_tz, cur_ntp != FALSE, fw_refreshes);
                DBusMessage *rep = dbus_message_new_method_return(msg);
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
            } else if (dbus_message_is_method_call(msg, "org.freedesktop.login1.Manager", "Suspend")) {
                suspend_calls++;
                write_state(state_file, active_prof, suspend_calls, cur_tz, cur_ntp != FALSE, fw_refreshes);
                DBusMessage *rep = dbus_message_new_method_return(msg);
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
            } else if (dbus_message_is_method_call(msg, "org.freedesktop.timedate1", "SetTimezone")) {
                const char *ntz = NULL;
                dbus_bool_t inter = FALSE;
                if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &ntz,
                                          DBUS_TYPE_BOOLEAN, &inter, DBUS_TYPE_INVALID) && ntz) {
                    snprintf(cur_tz, sizeof(cur_tz), "%s", ntz);
                }
                write_state(state_file, active_prof, suspend_calls, cur_tz, cur_ntp != FALSE, fw_refreshes);
                DBusMessage *rep = dbus_message_new_method_return(msg);
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
            } else if (dbus_message_is_method_call(msg, "org.freedesktop.timedate1", "SetNTP")) {
                dbus_bool_t en = FALSE, inter = FALSE;
                if (dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &en,
                                          DBUS_TYPE_BOOLEAN, &inter, DBUS_TYPE_INVALID)) {
                    cur_ntp = en;
                }
                write_state(state_file, active_prof, suspend_calls, cur_tz, cur_ntp != FALSE, fw_refreshes);
                DBusMessage *rep = dbus_message_new_method_return(msg);
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
            } else if (dbus_message_is_method_call(msg, "org.freedesktop.fwupd", "GetDevices")) {
                DBusMessage *rep = dbus_message_new_method_return(msg);
                DBusMessageIter root, arr, d0, ent, var;
                dbus_message_iter_init_append(rep, &root);
                dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "a{sv}", &arr);
                dbus_message_iter_open_container(&arr, DBUS_TYPE_ARRAY, "{sv}", &d0);
                struct { const char *k; const char *v; } kvs[] = {
                    {"DeviceId", "uefi-sys-fw0"},
                    {"Name", "Workstation System UEFI"},
                    {"Vendor", "CK-Core Hardware"},
                    {"Version", "1.14.0"},
                    {"Summary", "Mainboard System Firmware"},
                };
                for (size_t i = 0; i < sizeof(kvs) / sizeof(kvs[0]); ++i) {
                    dbus_message_iter_open_container(&d0, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                    dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &kvs[i].k);
                    dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "s", &var);
                    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &kvs[i].v);
                    dbus_message_iter_close_container(&ent, &var);
                    dbus_message_iter_close_container(&d0, &ent);
                }
                const char *kf = "Flags";
                dbus_uint64_t fl = (1ULL << 1);
                dbus_message_iter_open_container(&d0, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &kf);
                dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "t", &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT64, &fl);
                dbus_message_iter_close_container(&ent, &var);
                dbus_message_iter_close_container(&d0, &ent);
                dbus_message_iter_close_container(&arr, &d0);
                dbus_message_iter_close_container(&root, &arr);
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
            } else if (dbus_message_is_method_call(msg, "org.freedesktop.fwupd", "GetReleases")) {
                DBusMessage *rep = dbus_message_new_method_return(msg);
                DBusMessageIter root, arr, d0, ent, var;
                dbus_message_iter_init_append(rep, &root);
                dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "a{sv}", &arr);
                dbus_message_iter_open_container(&arr, DBUS_TYPE_ARRAY, "{sv}", &d0);
                const char *kv = "Version";
                const char *vv = fw_refreshes > 0 ? "1.16.2" : "1.15.0";
                dbus_message_iter_open_container(&d0, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &kv);
                dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "s", &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &vv);
                dbus_message_iter_close_container(&ent, &var);
                dbus_message_iter_close_container(&d0, &ent);
                dbus_message_iter_close_container(&arr, &d0);
                dbus_message_iter_close_container(&root, &arr);
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
            } else if (dbus_message_is_method_call(msg, "org.freedesktop.fwupd", "RefreshMetadata")) {
                fw_refreshes++;
                write_state(state_file, active_prof, suspend_calls, cur_tz, cur_ntp != FALSE, fw_refreshes);
                DBusMessage *rep = dbus_message_new_method_return(msg);
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
            } else if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_METHOD_CALL &&
                       !dbus_message_get_no_reply(msg)) {
                DBusMessage *rep = dbus_message_new_error(
                    msg, "org.freedesktop.DBus.Error.UnknownMethod", "unsupported in mock"
                );
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
            }
            dbus_connection_flush(conn);
            dbus_message_unref(msg);
        }
    }
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return 0;
}
"""


def wait_for(pred, timeout: float = 8.0, interval: float = 0.08, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_exc = None
    while time.monotonic() < deadline:
        try:
            val = pred()
            if val:
                return val
        except Exception as exc:  # noqa: BLE001
            last_exc = exc
        time.sleep(interval)
    raise AssertionError(f"Timed out waiting for {desc} (last_exc={last_exc})")


def verify_proc_motif9(pid: int, label: str) -> None:
    maps_path = Path(f"/proc/{pid}/maps")
    wait_for(
        lambda: (
            "libXm.so.9" in maps_path.read_text(encoding="utf-8", errors="ignore")
            if maps_path.is_file() else False
        ),
        timeout=5.0,
        desc=f"{label} /proc/{pid}/maps -> libXm.so.9",
    )


def sample_png_rgb(png_path: Path, x: int, y: int) -> tuple[int, int, int]:
    raw = png_path.read_bytes()
    assert raw[:8] == b"\x89PNG\r\n\x1a\n", "Not a PNG file"
    off = 8
    w = h = bit_depth = color_type = 0
    idat = bytearray()
    while off + 8 <= len(raw):
        length, ctype = struct.unpack(">I4s", raw[off:off + 8])
        cdata = raw[off + 8:off + 8 + length]
        off += 12 + length
        if ctype == b"IHDR":
            w, h, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", cdata)
        elif ctype == b"IDAT":
            idat.extend(cdata)
        elif ctype == b"IEND":
            break
    assert bit_depth == 8 and color_type in (2, 6), f"Unsupported PNG format depth={bit_depth} type={color_type}"
    bpp = 4 if color_type == 6 else 3
    stride = w * bpp
    decomp = zlib.decompress(bytes(idat))
    prev = bytearray(stride)
    pos = 0
    for row_idx in range(h):
        ftype = decomp[pos]
        pos += 1
        cur = bytearray(decomp[pos:pos + stride])
        pos += stride
        if ftype == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 0xFF
        if row_idx == y:
            px_off = x * bpp
            return int(cur[px_off]), int(cur[px_off + 1]), int(cur[px_off + 2])
        prev = cur
    raise AssertionError(f"Coordinate ({x},{y}) out of range ({w}x{h})")


def kill_procs(procs: list[subprocess.Popen]) -> None:
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
    time.sleep(0.15)
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                p.wait(timeout=2.0)
            except Exception:  # noqa: BLE001
                pass


def resolve_real_elf(path: Path) -> Path:
    libs_cand = path.parent / ".libs" / path.name
    return libs_cand.resolve() if libs_cand.is_file() else path.resolve()


def run_lane(
    lane: str,
    disp_idx: int,
    work_root: Path,
    comp_bin: Path,
    dtwm_x11_bin: Path,
    panel_bin: Path,
    media_ctl_bin: Path,
    dtstyle_bin: Path,
    dtprintinfo_bin: Path,
    status_bin: Path,
    admin_bin: Path,
    x11_helper: Path,
    mock_dbus_bin: Path,
) -> str:
    lane_root = work_root / lane
    home = lane_root / "home"
    xdg_rt = lane_root / "rt"
    sysfs = lane_root / "sys_class"
    apps_dir = lane_root / "xdg_apps"
    for d in (
        home, xdg_rt, home / ".config", home / ".cache", home / ".local/share",
        home / ".dt/sessions", home / ".dt/types", sysfs / "power_supply/BAT0",
        sysfs / "power_supply/AC0", sysfs / "backlight/intel_backlight", apps_dir,
    ):
        d.mkdir(parents=True, exist_ok=True)
    os.chmod(xdg_rt, 0o700)
    (home / ".Xdefaults").write_text("*fontList: fixed\n", encoding="utf-8")

    # Seed mock laptop battery (on battery) + backlight + platform_profile
    (sysfs / "power_supply/BAT0/type").write_text("Battery\n", encoding="utf-8")
    (sysfs / "power_supply/BAT0/present").write_text("1\n", encoding="utf-8")
    (sysfs / "power_supply/BAT0/status").write_text("Discharging\n", encoding="utf-8")
    (sysfs / "power_supply/BAT0/capacity").write_text("82\n", encoding="utf-8")
    (sysfs / "power_supply/BAT0/energy_now").write_text("41000000\n", encoding="utf-8")
    (sysfs / "power_supply/BAT0/energy_full").write_text("50000000\n", encoding="utf-8")
    (sysfs / "power_supply/BAT0/power_now").write_text("10000000\n", encoding="utf-8")
    (sysfs / "power_supply/AC0/type").write_text("Mains\n", encoding="utf-8")
    (sysfs / "power_supply/AC0/online").write_text("0\n", encoding="utf-8")

    bl_dir = sysfs / "backlight/intel_backlight"
    (bl_dir / "max_brightness").write_text("200\n", encoding="utf-8")
    (bl_dir / "brightness").write_text("100\n", encoding="utf-8")

    pprof_file = lane_root / "platform_profile"
    pprof_file.write_text("balanced\n", encoding="utf-8")

    # Seed sample XDG .desktop files for ck-admin Default Applications
    (apps_dir / "firefox.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=Firefox Browser\nExec=firefox %u\n"
        "MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;\n",
        encoding="utf-8",
    )
    (apps_dir / "ck-pdf.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=CK-Core PDF Viewer\nExec=ck-pdf %f\n"
        "MimeType=application/pdf;\n",
        encoding="utf-8",
    )

    # Seed mock CUPS IPP Everywhere queues & jobs for dtprintinfo
    cups_mock = lane_root / "cups_mock.conf"
    cups_mock.write_text(
        "PRINTER|Office_Color_IPP|ipp://print.office.lan/ipp/print|IPP Everywhere Color MFP|Main Floor|idle|1|1|1\n"
        "PRINTER|Lab_LaserJet_IPP|ipps://lab-prn.lan/ipp/print|IPP Everywhere Mono Laser|Lab 3B|stopped|0|0|1\n"
        "JOB|401|Office_Color_IPP|Quarterly_Spec.pdf|klukas|processing|1480|2026-11-19|16:00:00\n"
        "JOB|402|Office_Color_IPP|Schematic_A3.pdf|klukas|pending|620|2026-11-19|16:02:00\n"
        "JOB|403|Lab_LaserJet_IPP|Kernel_Notes.ps|alice|held|210|2026-11-19|16:05:00\n",
        encoding="utf-8",
    )

    cde_build_root = comp_bin.parent.parent.parent
    if (cde_build_root / "programs").is_dir() is False:
        cde_build_root = comp_bin.parent.parent.parent.parent

    env = os.environ.copy()
    motif_pfx = os.environ.get("MOTIF_PREFIX", "/var/tmp/ck-wayland-build/wp00/motif-prefix")
    fs_pfx = os.environ.get("FS_PREFIX", "/var/tmp/ck-wayland-build/wp00/fs-prefix")
    ld_parts = [
        f"{fs_pfx}/lib",
        f"{motif_pfx}/lib",
        str(cde_build_root / "lib/DtSvc/.libs"),
        str(cde_build_root / "lib/DtWidget/.libs"),
        str(cde_build_root / "lib/DtSearch/.libs"),
        str(cde_build_root / "lib/DtHelp/.libs"),
        str(cde_build_root / "lib/DtPrint/.libs"),
        str(cde_build_root / "lib/DtTerm/.libs"),
        str(cde_build_root / "lib/csa/.libs"),
        str(cde_build_root / "lib/tt/lib/.libs"),
        str(cde_build_root / "lib/DtXinerama/.libs"),
        "/usr/dt/lib",
        "/usr/local/lib",
    ]
    if env.get("LD_LIBRARY_PATH"):
        ld_parts.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
    env.update({
        "HOME": str(home),
        "DTDATABASESEARCHPATH": f"{home / '.dt/types'},/usr/dt/appconfig/types/C",
        "XDG_RUNTIME_DIR": str(xdg_rt),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "CK_STATUS_SYSFS_ROOT": str(sysfs),
        "CK_CDE_BACKLIGHT_DIR": str(bl_dir),
        "CK_POWER_PROFILE_SYSFS": str(pprof_file),
        "CK_STATUS_NO_AUDIO": "1",
        "CK_STATUS_SYSTEM_BUS": "session",
        "CK_ADMIN_SESSION_BUS": "1",
        "CK_ADMIN_APPLICATIONS_DIRS": str(apps_dir),
        "CK_CUPS_MOCK_FILE": str(cups_mock),
        "LC_ALL": "C.UTF-8",
        "LANG": "C.UTF-8",
    })

    procs: list[subprocess.Popen] = []
    try:
        # 1. Start isolated session dbus-daemon + WP-18 mock D-Bus services
        dbus_addr_file = lane_root / "dbus.addr"
        with dbus_addr_file.open("w", encoding="utf-8") as dfp:
            dbus_proc = subprocess.Popen(
                ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                env=env, stdout=dfp, stderr=subprocess.DEVNULL, start_new_session=True,
            )
        procs.append(dbus_proc)
        wait_for(lambda: dbus_addr_file.is_file() and dbus_addr_file.read_text(encoding="utf-8").strip(),
                 timeout=6.0, desc=f"{lane} dbus-daemon")
        env["DBUS_SESSION_BUS_ADDRESS"] = dbus_addr_file.read_text(encoding="utf-8").splitlines()[0].strip()

        dbus_state_file = lane_root / "dbus_state.json"
        dbus_ready_file = lane_root / "dbus.ready"
        mock_proc = subprocess.Popen(
            [str(mock_dbus_bin), str(dbus_state_file), str(dbus_ready_file)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(mock_proc)
        wait_for(lambda: dbus_ready_file.is_file(), timeout=6.0, desc=f"{lane} mock D-Bus services")

        # 2. Launch Wayland (`dtwm-wayland` + `dtwm-panel`) or X11 (`Xvfb` + `dtwm`)
        if lane == "wayland":
            status_file = lane_root / "comp.status"
            wenv = dict(env, WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1",
                        WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1")
            wenv.pop("WAYLAND_DISPLAY", None)
            wenv.pop("DISPLAY", None)
            comp_proc = subprocess.Popen(
                [str(comp_bin), "--status-file", str(status_file)],
                env=wenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(comp_proc)

            def wl_up():
                if not status_file.is_file():
                    return None
                kv = dict(ln.split("=", 1) for ln in status_file.read_text(encoding="utf-8").splitlines() if "=" in ln)
                return kv if kv.get("WAYLAND_DISPLAY") and kv.get("DISPLAY") else None

            kv = wait_for(wl_up, timeout=10.0, desc=f"{lane} dtwm-wayland")
            env["WAYLAND_DISPLAY"] = kv["WAYLAND_DISPLAY"]
            env["DISPLAY"] = kv["DISPLAY"]

            def find_spawned_panel_pid() -> int:
                r = subprocess.run(["pgrep", "-P", str(comp_proc.pid), "-f", "dtwm-panel"],
                                   capture_output=True, text=True)
                if r.returncode == 0 and r.stdout.strip():
                    for tok in r.stdout.split():
                        p = int(tok)
                        if "libXm.so.9" in Path(f"/proc/{p}/maps").read_text(encoding="utf-8", errors="ignore"):
                            return p
                return 0

            panel_pid = wait_for(find_spawned_panel_pid, timeout=8.0, desc=f"{lane} dtwm-panel libXm.so.9")
            verify_proc_motif9(panel_pid, f"{lane} dtwm-panel")
            time.sleep(0.4)
        else:
            env.pop("WAYLAND_DISPLAY", None)
            chosen_disp = disp_idx
            for cand_d in range(disp_idx, disp_idx + 40):
                if not Path(f"/tmp/.X{cand_d}-lock").exists() and not Path(f"/tmp/.X11-unix/X{cand_d}").exists():
                    chosen_disp = cand_d
                    break
            xdisp = f":{chosen_disp}"
            xvfb_proc = subprocess.Popen(
                ["Xvfb", xdisp, "-screen", "0", "1280x900x24", "-nolisten", "tcp", "-ac"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(xvfb_proc)
            env["DISPLAY"] = xdisp
            wait_for(lambda: subprocess.run([str(x11_helper), "ping"], env=env, capture_output=True).returncode == 0,
                     timeout=8.0, desc=f"{lane} Xvfb")
            dtwm_err = lane_root / "dtwm.stderr"
            dtwm_proc = None
            last_dtwm_exc = None
            for _attempt in range(2):
                with dtwm_err.open("w", encoding="utf-8") as der:
                    dtwm_proc = subprocess.Popen(
                        [str(dtwm_x11_bin)], env=env, stdout=subprocess.DEVNULL, stderr=der, start_new_session=True,
                    )
                procs.append(dtwm_proc)
                try:
                    verify_proc_motif9(dtwm_proc.pid, f"{lane} dtwm")
                    wait_for(
                        lambda: subprocess.run([str(x11_helper), "get_root", "_NET_SUPPORTING_WM_CHECK"],
                                               env=env, capture_output=True).returncode == 0,
                        timeout=8.0, desc=f"{lane} dtwm _NET_SUPPORTING_WM_CHECK",
                    )
                    last_dtwm_exc = None
                    break
                except Exception as exc:
                    last_dtwm_exc = exc
                    time.sleep(0.3)
            if last_dtwm_exc is not None:
                errlog = (home / ".dt/errorlog").read_text(encoding="utf-8", errors="ignore") if (home / ".dt/errorlog").is_file() else ""
                raise AssertionError(
                    f"dtwm (rc={dtwm_proc.poll() if dtwm_proc else None}) failed on {xdisp}: {last_dtwm_exc}\n"
                    f"stderr={dtwm_err.read_text(encoding='utf-8', errors='ignore')}\nerrorlog={errlog}"
                ) from last_dtwm_exc
            time.sleep(0.4)

        def get_root_prop(prop: str) -> str:
            r = subprocess.run([str(x11_helper), "get_root", prop], env=env, capture_output=True, text=True)
            return r.stdout.strip() if r.returncode == 0 else ""

        def set_root_prop(prop: str, val: str) -> None:
            subprocess.run([str(x11_helper), "set_root", prop, val], env=env, check=True)

        def get_json(state_prop: str) -> dict:
            r = subprocess.run([str(x11_helper), "get", state_prop], env=env, capture_output=True, text=True)
            if r.returncode != 0 or not r.stdout.strip():
                return {}
            return json.loads(r.stdout)

        def send_ipc(state_prop: str, ipc_prop: str, cmd: str) -> None:
            subprocess.run([str(x11_helper), "set", state_prop, ipc_prop, cmd], env=env, check=True)
            time.sleep(0.05)

        def read_dbus_mock() -> dict:
            return json.loads(dbus_state_file.read_text(encoding="utf-8"))

        # ---- PART 1: White Test Window + Night Light (`3400K`), VRR & Backlight Persistence ----
        client_proc = subprocess.Popen(
            [str(x11_helper), "test_client", f"WP18Client-{lane}"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(client_proc)
        wait_for(
            lambda: (r := subprocess.run([str(x11_helper), "client_geom"], env=env, capture_output=True, text=True)).returncode == 0
            and json.loads(r.stdout),
            timeout=6.0, desc=f"{lane} test client mapped",
        )
        time.sleep(0.25)
        orig_geom = json.loads(subprocess.run([str(x11_helper), "client_geom"], env=env, capture_output=True, text=True, check=True).stdout)

        if lane == "wayland":
            shot_before = lane_root / "nl_before.png"
            def wait_white_client():
                nonlocal orig_geom
                orig_geom = json.loads(
                    subprocess.run([str(x11_helper), "client_geom"], env=env, capture_output=True, text=True, check=True).stdout
                )
                subprocess.run(["grim", str(shot_before)], env=env, check=True)
                rgb = sample_png_rgb(shot_before, int(orig_geom["cx"]) + 100, int(orig_geom["cy"]) + 100)
                return rgb if (rgb[0] > 235 and rgb[1] > 235 and rgb[2] > 235) else None
            r0, g0, b0 = wait_for(wait_white_client, timeout=6.0, interval=0.12, desc=f"{lane} white client pixel committed")

        dtstyle_proc = subprocess.Popen(
            [str(dtstyle_bin), "-display_dialog"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(dtstyle_proc)
        dlg_win = wait_for(lambda: get_root_prop("_DTSTYLE_DISPLAY_DLG_WIN"),
                           timeout=10.0, desc=f"{lane} dtstyle _DTSTYLE_DISPLAY_DLG_WIN")
        verify_proc_motif9(dtstyle_proc.pid, f"{lane} dtstyle")

        subprocess.run([str(x11_helper), "set_win", dlg_win, "_DTSTYLE_DISPLAY_IPC", "set-ergonomics 1 always 3400 1"],
                       env=env, check=True)
        wait_for(
            lambda: "temp=3400" in get_root_prop("_DT_NIGHT_LIGHT_STATE"),
            timeout=6.0, desc=f"{lane} dtstyle set-ergonomics preview",
        )
        subprocess.run([str(x11_helper), "set_win", dlg_win, "_DTSTYLE_DISPLAY_IPC", "apply"],
                       env=env, check=True)

        ergo_conf = home / ".dt/display_ergonomics.conf"
        wait_for(
            lambda: ergo_conf.is_file()
            and "night_light_temp=3400" in (t := ergo_conf.read_text(encoding="utf-8"))
            and "=1" in [ln[ln.find("="):] for ln in t.splitlines() if ln.startswith("vrr.")],
            timeout=6.0, desc=f"{lane} display_ergonomics.conf saved with VRR",
        )
        ergo_txt = ergo_conf.read_text(encoding="utf-8")
        assert "night_light_enabled=1" in ergo_txt and "night_light_mode=always" in ergo_txt

        nl_state = wait_for(
            lambda: s if "temp=3400" in (s := get_root_prop("_DT_NIGHT_LIGHT_STATE")) and "vrr=1" in s else "",
            timeout=6.0, desc=f"{lane} _DT_NIGHT_LIGHT_STATE",
        )

        if lane == "wayland":
            time.sleep(0.25)
            shot_after = lane_root / "nl_after.png"
            subprocess.run(["grim", str(shot_after)], env=env, check=True)
            r1, g1, b1 = sample_png_rgb(shot_after, int(orig_geom["cx"]) + 100, int(orig_geom["cy"]) + 100)
            assert b1 < b0 - 15 and (r1 - b1) > 15, (
                f"{lane}: expected warm pixel shift at 3400K, before={(r0,g0,b0)} after={(r1,g1,b1)}"
            )
            # Disable Night Light before tiling so clean state continues
            subprocess.run([str(x11_helper), "set_win", dlg_win, "_DTSTYLE_DISPLAY_IPC", "set-ergonomics 0 always 6500 1"],
                           env=env, check=True)
            wait_for(lambda: "enabled=0" in get_root_prop("_DT_NIGHT_LIGHT_STATE"), timeout=5.0, desc="Wayland NL reset")
            subprocess.run([str(x11_helper), "set_win", dlg_win, "_DTSTYLE_DISPLAY_IPC", "ok"],
                           env=env, check=True)
        else:
            def read_randr() -> dict:
                return json.loads(subprocess.run([str(x11_helper), "randr_check"], env=env, capture_output=True, text=True, check=True).stdout)

            rr = read_randr()
            assert rr["b"] < rr["g"] < rr["r"] and rr["r"] >= 65000 and rr["b"] < 46000, f"X11 gamma not warm: {rr}"
            assert rr["vrr"] == 1, f"X11 _DT_VRR_ENABLED not 1: {rr}"

            # Verify Cancel reverts unsaved preview changes on X11!
            subprocess.run([str(x11_helper), "set_win", dlg_win, "_DTSTYLE_DISPLAY_IPC", "set-ergonomics 0 always 6500 0"],
                           env=env, check=True)
            wait_for(lambda: read_randr().get("vrr") == 0, timeout=5.0, desc="X11 preview vrr=0 before Cancel")
            subprocess.run([str(x11_helper), "set_win", dlg_win, "_DTSTYLE_DISPLAY_IPC", "cancel"],
                           env=env, check=True)
            rr_reverted = wait_for(
                lambda: r if (r := read_randr()).get("vrr") == 1 and r["b"] < r["g"] < r["r"] else None,
                timeout=5.0, desc="X11 Cancel revert gamma/VRR",
            )
            assert rr_reverted["b"] < 46000, f"X11 Cancel failed to revert gamma/VRR: {rr_reverted}"

        dtstyle_proc.terminate()
        dtstyle_proc.wait(timeout=4.0)

        # Backlight persistence via dtwm-media-ctl
        subprocess.run([str(media_ctl_bin), "bright-up"], env=env, check=True)
        bl_conf = home / ".dt/backlight.conf"
        assert bl_conf.is_file() and "brightness_pct=60" in bl_conf.read_text(encoding="utf-8"), (
            f"backlight.conf not saved: {bl_conf.read_text(encoding='utf-8') if bl_conf.is_file() else 'missing'}"
        )
        (bl_dir / "brightness").write_text("20\n", encoding="utf-8")
        subprocess.run([str(media_ctl_bin), "bright-restore"], env=env, check=True)
        restored_bl = int((bl_dir / "brightness").read_text(encoding="utf-8").strip())
        assert restored_bl == 120, f"Expected bright-restore -> 120 (60% of 200), got {restored_bl}"

        # ---- PART 2: Keyboard Window Tiling (`Super+Left/Right/Up/Down`) ----
        subprocess.run([str(x11_helper), "focus_test_client"], env=env, check=True)
        time.sleep(0.15)
        pre_tile = json.loads(subprocess.run([str(x11_helper), "client_geom"], env=env, capture_output=True, text=True, check=True).stdout)

        def trigger_tile(direction: str) -> None:
            if lane == "wayland":
                set_root_prop("_DT_WM_REQUEST", f"f.tile_window {direction}")
            else:
                subprocess.run([str(x11_helper), "super_arrow", direction], env=env, check=True)

        def client_geom() -> dict:
            return json.loads(subprocess.run([str(x11_helper), "client_geom"], env=env, capture_output=True, text=True, check=True).stdout)

        # 1) Super+Left -> Left Half (honoring Front Panel strut: h < 885 on 1280x900)
        trigger_tile("left")
        g_left = wait_for(
            lambda: g if 0 <= (g := client_geom())["x"] <= 12 and 600 <= g["w"] <= 650 else None,
            timeout=5.0, desc=f"{lane} Super+Left half tile",
        )
        usable_h = g_left["y"] + g_left["h"]
        assert 740 <= usable_h <= 885, f"{lane}: Left half should honor Front Panel bottom strut on 1280x900, got {g_left}"

        # 2) Super+Up from Left Half -> Top-Left Quadrant
        trigger_tile("up")
        g_tl = wait_for(
            lambda: g if 0 <= (g := client_geom())["x"] <= 12 and (g["y"] + g["h"]) <= (usable_h // 2) + 20 else None,
            timeout=5.0, desc=f"{lane} Super+Up Top-Left quadrant",
        )
        assert 0 <= g_tl["y"] <= 36 and 600 <= g_tl["w"] <= 650, f"{lane}: unexpected Top-Left quadrant {g_tl}"

        # 3) Super+Right from Top-Left -> Top-Right Quadrant
        trigger_tile("right")
        g_tr = wait_for(
            lambda: g if (g := client_geom())["x"] >= 620 else None,
            timeout=5.0, desc=f"{lane} Super+Right Top-Right quadrant",
        )
        assert 0 <= g_tr["y"] <= 36 and (g_tr["y"] + g_tr["h"]) <= (usable_h // 2) + 20, f"{lane}: unexpected Top-Right quadrant {g_tr}"

        # 4) Super+Down from Quadrant -> Restore Pre-Tile Geometry!
        trigger_tile("down")
        g_rest = wait_for(
            lambda: g if abs((g := client_geom())["w"] - pre_tile["w"]) <= 28 and g["x"] > 40 else None,
            timeout=5.0, desc=f"{lane} Super+Down pre-tile geometry restore",
        )
        assert abs(g_rest["h"] - pre_tile["h"]) <= 28, f"{lane}: restore mismatch pre={pre_tile} post={g_rest}"

        # 5) Super+Down from Untiled -> Bottom Half (must not overlap Front Panel!)
        trigger_tile("down")
        g_bot = wait_for(
            lambda: g if (g := client_geom())["w"] >= 1220 and g["y"] >= (usable_h // 2) - 20 else None,
            timeout=5.0, desc=f"{lane} Super+Down Bottom Half",
        )
        assert g_bot["y"] + g_bot["h"] <= usable_h + 16, f"{lane}: Bottom Half overlapped Front Panel! {g_bot}"

        # ---- PART 3: Power Profiles, Docked Clamshell vs. Battery Lid Suspend & Idle Auto-Suspend ----
        status_proc = subprocess.Popen(
            [str(status_bin), "--tab", "power", "--standalone"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(status_proc)
        wait_for(lambda: get_json("_CK_STATUS_STATE").get("tab") == "power",
                 timeout=8.0, desc=f"{lane} ck-status _CK_STATUS_STATE")
        verify_proc_motif9(status_proc.pid, f"{lane} ck-status")

        send_ipc("_CK_STATUS_STATE", "_CK_STATUS_IPC", "set-power-profile:performance")
        wait_for(
            lambda: get_json("_CK_STATUS_STATE").get("power_profile") == "performance"
            and read_dbus_mock().get("active_profile") == "performance",
            timeout=5.0, desc=f"{lane} set-power-profile:performance",
        )
        assert pprof_file.read_text(encoding="utf-8").strip() == "performance"

        send_ipc("_CK_STATUS_STATE", "_CK_STATUS_IPC", "set-power-profile:power-saver")
        wait_for(
            lambda: get_json("_CK_STATUS_STATE").get("power_profile") == "power-saver"
            and read_dbus_mock().get("active_profile") == "power-saver",
            timeout=5.0,
            desc=f"{lane} set-power-profile:power-saver",
        )

        send_ipc("_CK_STATUS_STATE", "_CK_STATUS_IPC", "set-power-policy:1800:300:docked-clamshell")
        pol_conf = home / ".config/ck-core/power-policy.conf"
        wait_for(lambda: pol_conf.is_file() and "bat_suspend_sec=300" in pol_conf.read_text(encoding="utf-8"),
                 timeout=5.0, desc=f"{lane} power-policy.conf written")

        def count_wl_enabled_outputs() -> int:
            cnt = 0
            for ln in get_root_prop("_DT_WAYLAND_OUTPUTS").splitlines():
                parts = ln.split("|")
                if len(parts) >= 3 and parts[2] == "1":
                    cnt += 1
            return cnt

        def second_wl_output_name() -> str:
            for ln in get_root_prop("_DT_WAYLAND_OUTPUTS").splitlines():
                parts = ln.split("|")
                if len(parts) >= 3 and parts[0] != "0":
                    return parts[1]
            return "HEADLESS-2"

        # Dual-output Docked Clamshell test -> internal panel disabled, 0 Suspend calls!
        if lane == "wayland":
            set_root_prop("_DT_WM_REQUEST", "f.hotplug_headless_output 1280 900")
            wait_for(
                lambda: count_wl_enabled_outputs() == 2,
                timeout=6.0, desc="Wayland 2 active outputs before clamshell",
            )
        else:
            set_root_prop("_DT_X11_OUTPUT_TOPOLOGY", "eDP-1:1,HDMI-1:1")

        assert read_dbus_mock()["suspend_calls"] == 0
        send_ipc("_CK_STATUS_STATE", "_CK_STATUS_IPC", "simulate-lid:closed")
        act_clam = wait_for(
            lambda: s if "action=clamshell" in (s := get_root_prop("_DT_POWER_POLICY_LAST_ACTION")) else "",
            timeout=6.0, desc=f"{lane} docked clamshell lid-close",
        )
        assert read_dbus_mock()["suspend_calls"] == 0, f"{lane}: clamshell should NOT suspend! {act_clam}"
        if lane == "wayland":
            wait_for(
                lambda: count_wl_enabled_outputs() == 1,
                timeout=5.0, desc="Wayland internal panel disabled in clamshell",
            )
        else:
            assert get_root_prop("_DT_X11_OUTPUT_TOPOLOGY") == "eDP-1:0,HDMI-1:1"

        send_ipc("_CK_STATUS_STATE", "_CK_STATUS_IPC", "simulate-lid:open")
        wait_for(lambda: "action=restore-panel" in get_root_prop("_DT_POWER_POLICY_LAST_ACTION"),
                 timeout=6.0, desc=f"{lane} lid-open restore-panel")

        # Undock external monitor -> single-output on battery -> lid-close MUST invoke Suspend!
        if lane == "wayland":
            ext_name = second_wl_output_name()
            set_root_prop("_DT_WM_REQUEST", f"f.set_output_layout {ext_name} 0 0 0 1280x900 0 96")
            wait_for(
                lambda: count_wl_enabled_outputs() == 1,
                timeout=6.0, desc="Wayland undocked to single output",
            )
        else:
            set_root_prop("_DT_X11_OUTPUT_TOPOLOGY", "eDP-1:1")

        send_ipc("_CK_STATUS_STATE", "_CK_STATUS_IPC", "simulate-lid:closed")
        wait_for(
            lambda: "reason=lid-close action=suspend dbus=1" in get_root_prop("_DT_POWER_POLICY_LAST_ACTION")
            and read_dbus_mock().get("suspend_calls") == 1,
            timeout=6.0, desc=f"{lane} single-output battery lid-close Suspend",
        )
        send_ipc("_CK_STATUS_STATE", "_CK_STATUS_IPC", "simulate-lid:open")

        # Idle auto-suspend check (310s >= 300s battery threshold) -> Suspend call #2!
        send_ipc("_CK_STATUS_STATE", "_CK_STATUS_IPC", "check-idle-suspend:310")
        wait_for(
            lambda: "reason=idle action=suspend dbus=1" in get_root_prop("_DT_POWER_POLICY_LAST_ACTION")
            and read_dbus_mock().get("suspend_calls") == 2,
            timeout=6.0, desc=f"{lane} idle auto-suspend trigger",
        )
        status_proc.terminate()
        status_proc.wait(timeout=4.0)

        # ---- PART 4: `dtprintinfo` CUPS / IPP Everywhere & `ck-admin` ----
        cups_res = subprocess.run([str(dtprintinfo_bin), "--cups-summary"], env=env, capture_output=True, text=True, check=True)
        cups_out = cups_res.stdout
        assert "CUPS_BACKEND=active" in cups_out, f"Expected active CUPS backend:\n{cups_out}"
        assert "PRINTER_COUNT=2" in cups_out, f"Expected 2 IPP printers:\n{cups_out}"
        assert "PRINTER|Office_Color_IPP|" in cups_out and "default=1|ipp_everywhere=1" in cups_out, cups_out
        assert "PRINTER|Lab_LaserJet_IPP|" in cups_out and "state=stopped" in cups_out, cups_out
        assert "JOB_COUNT=3" in cups_out and "JOB|401|dest=Office_Color_IPP|" in cups_out, cups_out

        # Cancel job 401 via dtprintinfo --cups-cancel and verify JOB_COUNT drops to 2
        subprocess.run([str(dtprintinfo_bin), "--cups-cancel", "Office_Color_IPP", "401"], env=env, check=True)
        cups_after = subprocess.run([str(dtprintinfo_bin), "--cups-summary"], env=env, capture_output=True, text=True, check=True).stdout
        assert "JOB_COUNT=2" in cups_after and "JOB|401|" not in cups_after, cups_after

        dtp_gui = subprocess.Popen(
            [str(dtprintinfo_bin)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(dtp_gui)
        verify_proc_motif9(dtp_gui.pid, f"{lane} dtprintinfo")
        dtp_gui.terminate()
        dtp_gui.wait(timeout=4.0)

        # Launch live Motif `ck-admin`
        admin_proc = subprocess.Popen(
            [str(admin_bin), "--tab", "default-apps"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(admin_proc)
        wait_for(lambda: get_json("_CK_ADMIN_STATE").get("tab") == "default-apps",
                 timeout=8.0, desc=f"{lane} ck-admin _CK_ADMIN_STATE")
        verify_proc_motif9(admin_proc.pid, f"{lane} ck-admin")

        send_ipc("_CK_ADMIN_STATE", "_CK_ADMIN_IPC", "set-default:web:firefox.desktop")
        wait_for(
            lambda: get_json("_CK_ADMIN_STATE").get("defaults", {}).get("web") == "firefox.desktop",
            timeout=5.0, desc=f"{lane} ck-admin set-default web",
        )
        send_ipc("_CK_ADMIN_STATE", "_CK_ADMIN_IPC", "set-default:pdf:ck-pdf.desktop")
        wait_for(
            lambda: (s := get_json("_CK_ADMIN_STATE").get("defaults", {})).get("web") == "firefox.desktop"
            and s.get("pdf") == "ck-pdf.desktop",
            timeout=5.0, desc=f"{lane} ck-admin mimeapps defaults",
        )
        mimeapps_txt = (home / ".config/mimeapps.list").read_text(encoding="utf-8")
        assert "x-scheme-handler/https=firefox.desktop" in mimeapps_txt
        assert "application/pdf=ck-pdf.desktop" in mimeapps_txt

        send_ipc("_CK_ADMIN_STATE", "_CK_ADMIN_IPC", "tab:datetime")
        wait_for(lambda: get_json("_CK_ADMIN_STATE").get("tab") == "datetime",
                 timeout=5.0, desc=f"{lane} ck-admin tab:datetime")
        send_ipc("_CK_ADMIN_STATE", "_CK_ADMIN_IPC", "set-timezone:America/New_York")
        wait_for(
            lambda: get_json("_CK_ADMIN_STATE").get("datetime", {}).get("timezone") == "America/New_York",
            timeout=6.0, desc=f"{lane} ck-admin timedate1 SetTimezone",
        )
        send_ipc("_CK_ADMIN_STATE", "_CK_ADMIN_IPC", "set-ntp:0")
        wait_for(
            lambda: (dt := get_json("_CK_ADMIN_STATE").get("datetime", {})).get("timezone") == "America/New_York"
            and dt.get("ntp") is False,
            timeout=6.0, desc=f"{lane} ck-admin timedate1 SetNTP",
        )

        send_ipc("_CK_ADMIN_STATE", "_CK_ADMIN_IPC", "tab:firmware")
        wait_for(lambda: get_json("_CK_ADMIN_STATE").get("tab") == "firmware",
                 timeout=5.0, desc=f"{lane} ck-admin tab:firmware")
        send_ipc("_CK_ADMIN_STATE", "_CK_ADMIN_IPC", "fwupd-refresh")
        wait_for(
            lambda: (devs := get_json("_CK_ADMIN_STATE").get("firmware", {}).get("devices", []))
            and devs[0].get("available_version") == "1.16.2",
            timeout=6.0, desc=f"{lane} ck-admin fwupd RefreshMetadata -> v1.16.2",
        )

        # Verify CDE WM_SAVE_YOURSELF -> -session restore on `firmware` tab!
        sess_res = subprocess.run(
            [str(x11_helper), "wm_save_yourself", "_CK_ADMIN_STATE"],
            env=env, capture_output=True, text=True, check=True,
        )
        sess_id = sess_res.stdout.strip()
        assert sess_id, f"{lane}: ck-admin did not publish -session id on WM_SAVE_YOURSELF"
        admin_proc.terminate()
        admin_proc.wait(timeout=4.0)

        admin_restored = subprocess.Popen(
            [str(admin_bin), "-session", sess_id],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(admin_restored)
        wait_for(lambda: get_json("_CK_ADMIN_STATE").get("tab") == "firmware",
                 timeout=8.0, desc=f"{lane} ck-admin -session restored firmware tab")
        admin_restored.terminate()
        admin_restored.wait(timeout=4.0)

        return (
            f"{lane}: PASS (Night Light 3400K+VRR+Backlight, Super+Arrow Half/Quadrant/Restore Tiling, "
            f"PowerProfiles+Clamshell/Suspend+Idle, dtprintinfo CUPS IPP, ck-admin XmTabStack+Session)"
        )
    finally:
        kill_procs(procs)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", type=Path, default=Path("./dtwm-wayland"))
    ap.add_argument("--apps-bin-dir", type=Path, default=None)
    args = ap.parse_args()

    comp_bin = resolve_real_elf(args.compositor.resolve())
    dtwm_dir = args.compositor.resolve().parent
    if dtwm_dir.name == ".libs":
        dtwm_dir = dtwm_dir.parent
    dtwm_x11_bin = resolve_real_elf(dtwm_dir / "dtwm")
    panel_bin = resolve_real_elf(dtwm_dir / "dtwm-panel")
    media_ctl_bin = resolve_real_elf(dtwm_dir / "dtwm-media-ctl")

    cde_programs = dtwm_dir.parent
    dtstyle_bin = resolve_real_elf(cde_programs / "dtstyle/dtstyle")
    dtprintinfo_bin = resolve_real_elf(cde_programs / "dtprintinfo/dtprintinfo")

    cand_app_dirs: list[Path] = []
    if args.apps_bin_dir is not None:
        cand_app_dirs.append(args.apps_bin_dir)
    if os.environ.get("CK_APPS_BIN_DIR"):
        cand_app_dirs.append(Path(os.environ["CK_APPS_BIN_DIR"]))
    cand_app_dirs.extend([
        dtwm_dir.parents[2] / "apps-src/build/bin",
        Path("/var/tmp/ck-wayland-build/wp18/apps-src/build/bin"),
        Path("/var/tmp/ck-wayland-build/wp00/apps-src/build/bin"),
        Path("/var/tmp/ck-wayland-build/wp00/apps-build/bin"),
    ])

    def find_companion(name: str) -> Path:
        for d in cand_app_dirs:
            p = d / name
            if p.is_file():
                return p.resolve()
        w = shutil.which(name)
        if w:
            return Path(w).resolve()
        for sys_d in (Path("/usr/dt/bin"), Path("/usr/local/bin")):
            p = sys_d / name
            if p.is_file():
                return p.resolve()
        return Path(f"/usr/dt/bin/{name}")

    status_bin = find_companion("ck-status")
    admin_bin = find_companion("ck-admin")

    for b in (comp_bin, dtwm_x11_bin, panel_bin, media_ctl_bin, dtstyle_bin, dtprintinfo_bin, status_bin, admin_bin):
        if not b.is_file():
            print(f"SKIP/FAIL: required binary not found: {b}", file=sys.stderr)
            return 1

    cc = shlex.split(os.environ.get("CC", "cc"))
    dbus_cflags = subprocess.run(["pkg-config", "--cflags", "dbus-1"], capture_output=True, text=True, check=True).stdout.split()
    dbus_libs = subprocess.run(["pkg-config", "--libs", "dbus-1"], capture_output=True, text=True, check=True).stdout.split()

    base_tmp = os.environ.get("TMPDIR") or "/var/tmp"
    with tempfile.TemporaryDirectory(prefix="wp18-dual-", dir=base_tmp) as td:
        work_root = Path(td)
        x11_src = work_root / "x11_wp18_helper.c"
        x11_bin = work_root / "x11_wp18_helper"
        x11_src.write_text(X11_WP18_HELPER_C, encoding="utf-8")
        subprocess.run([*cc, "-O2", str(x11_src), "-o", str(x11_bin), "-lX11", "-lXtst", "-lXrandr"], check=True)

        dbus_src = work_root / "mock_wp18_dbus.c"
        dbus_bin = work_root / "mock_wp18_dbus"
        dbus_src.write_text(MOCK_WP18_DBUS_C, encoding="utf-8")
        subprocess.run([*cc, "-O2", *dbus_cflags, str(dbus_src), "-o", str(dbus_bin), *dbus_libs], check=True)

        x11_base_disp = 110 + (os.getpid() % 60)
        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_wl = pool.submit(
                run_lane, "wayland", x11_base_disp, work_root, comp_bin, dtwm_x11_bin, panel_bin,
                media_ctl_bin, dtstyle_bin, dtprintinfo_bin, status_bin, admin_bin, x11_bin, dbus_bin,
            )
            fut_x11 = pool.submit(
                run_lane, "x11", x11_base_disp + 1, work_root, comp_bin, dtwm_x11_bin, panel_bin,
                media_ctl_bin, dtstyle_bin, dtprintinfo_bin, status_bin, admin_bin, x11_bin, dbus_bin,
            )
            errs = []
            for name, fut in (("wayland", fut_wl), ("x11", fut_x11)):
                try:
                    print(fut.result())
                except Exception:  # noqa: BLE001
                    errs.append(f"=== {name} FAILED ===\n{traceback.format_exc()}")
            if errs:
                for e in errs:
                    print(e, file=sys.stderr)
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
