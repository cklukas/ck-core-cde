#!/usr/bin/env python3
"""WP-20 parallel dual-environment integration check (Wayland + X11/XLibre):
1. Runtime Toolkit Provenance:
   - Verifies all tested Motif/CDE processes (`dtwm`, `dtstyle`, `ck-tasks`,
     `ck-magnifier`, and the Motif AccessX test client) map CK Core Motif ABI 9 (`libXm.so.9`).
2. Keyboard Accessibility (`AccessX`: Sticky Keys, Slow Keys, Bounce Keys) & `dtstyle` Persistence:
   - Applies preferences via `dtstyle -apply-accessibility`, verifying `~/.dt/accessibility.conf`
     and `_DT_ACCESSIBILITY_STATE`.
   - Sticky Keys: single tap `Shift_L` latches next key (`a` -> `'A'`, then `b` -> `'b'`);
     double tap `Shift_L` locks (`c`, `d` -> `'CD'`); third tap unlocks (`e` -> `'e'`).
   - Slow Keys (`slow_ms=260`): 50 ms tap rejected; 360 ms hold accepted.
   - Bounce Keys (`bounce_ms=260`): two rapid taps 45 ms apart produce a single character;
     subsequent tap after 320 ms produces second character.
3. Visual Bell & Reduced Motion:
   - Enables `vbell=window` / `vbell=screen` and `reduced_motion=1`.
   - Triggers X11 `XkbBell` and `_DT_WM_REQUEST` `f.visual_bell`, verifying `vbell_count`
     increments and visible bright flash pixels render on screen.
4. Cursor-Tracking Screen Magnification (`Super+Equal`, `Super+Minus`, `Super+0`):
   - Positions cursor over a crisp 40x40 `#ff00cc` reference square, verifies 100% pixel bbox,
     steps `Super+Equal` x4 to `zoom_pct=200`, verifies magnified pixel bbox scales ~2x in
     both `dtwm-wayland` (`grim`) and X11 (`ck-magnifier`), steps `Super+Minus` (`175%`), and
     resets via `Super+0` (`100%`).
5. Motif AT-SPI2 Screen Reader Bridge (`XM_ACCESSIBILITY=1`, `org.a11y.atspi`):
   - Launches unmodified `dtstyle -accessibility` and `ck-tasks` with `XM_ACCESSIBILITY=1`
     against an isolated session + AT-SPI2 D-Bus registry, walks live `org.a11y.atspi.Accessible`
     trees (`dtstyle` Accessibility controls + `ck-tasks` `CkTable` semantic `Table` / `Selection`
     / `Action`), and invokes `org.a11y.atspi.Action.DoAction(0)` over D-Bus.
"""

from __future__ import annotations

import argparse
from collections import deque
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from PIL import Image


WL_INPUT_HELPER_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
    &wl_seat_interface,
    &zwp_virtual_keyboard_v1_interface,
};
static const struct wl_message vkb_mgr_requests[] = {
    { "create_virtual_keyboard", "on", vkb_mgr_types },
};
static const struct wl_interface zwp_virtual_keyboard_manager_v1_interface = {
    "zwp_virtual_keyboard_manager_v1", 1, 1, vkb_mgr_requests, 0, NULL,
};
static const struct wl_interface *vkb_null_types[] = { NULL, NULL, NULL, NULL, NULL };
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
    &wl_seat_interface,
    &zwlr_virtual_pointer_v1_interface,
};
static const struct wl_message vptr_mgr_requests[] = {
    { "create_virtual_pointer", "?on", vptr_mgr_types },
    { "destroy", "", vkb_null_types },
};
static const struct wl_interface zwlr_virtual_pointer_manager_v1_interface = {
    "zwlr_virtual_pointer_manager_v1", 1, 2, vptr_mgr_requests, 0, NULL,
};
static const struct wl_message vptr_requests[] = {
    { "motion", "uff", vkb_null_types },
    { "motion_absolute", "uuuuu", vkb_null_types },
    { "button", "uuu", vkb_null_types },
    { "axis", "uuf", vkb_null_types },
    { "frame", "", vkb_null_types },
    { "axis_source", "u", vkb_null_types },
    { "axis_stop", "uu", vkb_null_types },
    { "axis_discrete", "uufi", vkb_null_types },
    { "destroy", "", vkb_null_types },
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
static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener reg_listener = { on_global, on_global_remove };

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

static struct wl_proxy *setup_vkb(struct wl_display *dpy, InputState *st) {
    struct wl_proxy *vkb = wl_proxy_marshal_flags(
        st->vkb_mgr, 0, &zwp_virtual_keyboard_v1_interface, 1, 0, st->seat, NULL);
    if (!vkb) return NULL;
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
    struct xkb_keymap *kmap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *kmap_str = xkb_keymap_get_as_string(kmap, XKB_KEYMAP_FORMAT_TEXT_V1);
    uint32_t kmap_sz = (uint32_t)strlen(kmap_str) + 1u;
    int kfd = memfd_create("wp20-xkb", MFD_CLOEXEC);
    if (kfd < 0 || ftruncate(kfd, (off_t)kmap_sz) != 0) return NULL;
    void *ptr = mmap(NULL, kmap_sz, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, 0);
    memcpy(ptr, kmap_str, kmap_sz);
    munmap(ptr, kmap_sz);
    wl_proxy_marshal_flags(vkb, 0, NULL, 1, 0, 1u, kfd, kmap_sz);
    wl_display_roundtrip(dpy);
    close(kfd);
    free(kmap_str);
    xkb_keymap_unref(kmap);
    xkb_context_unref(ctx);
    return vkb;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 2;
    InputState st = { NULL, NULL, NULL };
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, &st);
    wl_display_roundtrip(dpy);

    if (strcmp(argv[1], "warp") == 0 && argc >= 6) {
        if (!st.seat || !st.vptr_mgr) return 3;
        uint32_t x = (uint32_t)strtoul(argv[2], NULL, 0);
        uint32_t y = (uint32_t)strtoul(argv[3], NULL, 0);
        uint32_t w = (uint32_t)strtoul(argv[4], NULL, 0);
        uint32_t h = (uint32_t)strtoul(argv[5], NULL, 0);
        struct wl_proxy *vptr = wl_proxy_marshal_flags(
            st.vptr_mgr, 0, &zwlr_virtual_pointer_v1_interface, 1, 0, st.seat, NULL);
        wl_proxy_marshal_flags(vptr, 1, NULL, 1, 0, now_ms(), x, y, w, h);
        wl_proxy_marshal_flags(vptr, 4, NULL, 1, 0);
        wl_display_roundtrip(dpy);
        wl_proxy_marshal_flags(vptr, 8, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
        wl_display_flush(dpy);
        wl_display_disconnect(dpy);
        return 0;
    }

    if (!st.seat || !st.vkb_mgr) return 3;
    struct wl_proxy *vkb = setup_vkb(dpy, &st);
    if (!vkb) return 4;

    if (strcmp(argv[1], "tap") == 0 && argc >= 3) {
        uint32_t kc = (uint32_t)strtoul(argv[2], NULL, 0);
        int hold_ms = (argc >= 4) ? atoi(argv[3]) : 35;
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), kc, 1u);
        wl_display_roundtrip(dpy);
        usleep((useconds_t)hold_ms * 1000u);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), kc, 0u);
        wl_display_roundtrip(dpy);
    } else if (strcmp(argv[1], "bounce-tap") == 0 && argc >= 5) {
        uint32_t kc = (uint32_t)strtoul(argv[2], NULL, 0);
        int hold_ms = atoi(argv[3]);
        int gap_ms = atoi(argv[4]);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), kc, 1u);
        wl_display_roundtrip(dpy);
        usleep((useconds_t)hold_ms * 1000u);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), kc, 0u);
        wl_display_roundtrip(dpy);
        usleep((useconds_t)gap_ms * 1000u);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), kc, 1u);
        wl_display_roundtrip(dpy);
        usleep((useconds_t)hold_ms * 1000u);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), kc, 0u);
        wl_display_roundtrip(dpy);
    } else if (strcmp(argv[1], "chord") == 0 && argc >= 4) {
        uint32_t mod_kc = (uint32_t)strtoul(argv[2], NULL, 0);
        uint32_t key_kc = (uint32_t)strtoul(argv[3], NULL, 0);
        /* Super_L=125 -> Mod4 (64), Shift_L=42 -> Shift (1) */
        uint32_t mod_mask = (mod_kc == 125u) ? 64u : ((mod_kc == 42u) ? 1u : 0u);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), mod_kc, 1u);
        if (mod_mask) wl_proxy_marshal_flags(vkb, 2, NULL, 1, 0, mod_mask, 0u, 0u, 0u);
        wl_display_roundtrip(dpy);
        usleep(20000);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), key_kc, 1u);
        wl_display_roundtrip(dpy);
        usleep(30000);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), key_kc, 0u);
        wl_display_roundtrip(dpy);
        usleep(15000);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), mod_kc, 0u);
        if (mod_mask) wl_proxy_marshal_flags(vkb, 2, NULL, 1, 0, 0u, 0u, 0u, 0u);
        wl_display_roundtrip(dpy);
    }

    wl_proxy_marshal_flags(vkb, 3, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
    wl_display_flush(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
"""


X11_WP20_HELPER_C = r"""
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*XTestFakeKeyEventFn)(Display *, unsigned int, Bool, unsigned long);

static Window find_win_with_prop(Display *dpy, Window w, Atom prop) {
    Atom act = None;
    int fmt = 0;
    unsigned long n = 0, rem = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, prop, 0, 4, False, AnyPropertyType,
                           &act, &fmt, &n, &rem, &data) == Success && data) {
        XFree(data);
        if (act != None) return w;
    }
    Window root = None, parent = None, *kids = NULL;
    unsigned int nkids = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &kids, &nkids)) return None;
    Window found = None;
    for (unsigned int i = 0; i < nkids && !found; ++i) {
        found = find_win_with_prop(dpy, kids[i], prop);
    }
    if (kids) XFree(kids);
    return found;
}

static XTestFakeKeyEventFn load_xtest(void) {
    void *h = dlopen("libXtst.so.6", RTLD_LAZY);
    if (!h) h = dlopen("libXtst.so", RTLD_LAZY);
    return h ? (XTestFakeKeyEventFn)dlsym(h, "XTestFakeKeyEvent") : NULL;
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);

    if (argc < 2 || strcmp(argv[1], "state") == 0) {
        Atom a = XInternAtom(dpy, "_DT_ACCESSIBILITY_STATE", False);
        Atom act = None;
        int fmt = 0;
        unsigned long n = 0, rem = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(dpy, root, a, 0, 4096, False, AnyPropertyType,
                               &act, &fmt, &n, &rem, &prop) == Success && prop && fmt == 8) {
            printf("_DT_ACCESSIBILITY_STATE=%.*s\n", (int)n, (const char *)prop);
        } else {
            printf("_DT_ACCESSIBILITY_STATE=\n");
        }
        if (prop) XFree(prop);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "send-req") == 0 && argc >= 3) {
        Atom mwm_info = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        Atom req_atom = XInternAtom(dpy, "_DT_WM_REQUEST", False);
        Atom act = None;
        int fmt = 0;
        unsigned long n = 0, rem = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(dpy, root, mwm_info, 0, 2, False, AnyPropertyType,
                               &act, &fmt, &n, &rem, &prop) != Success || !prop || n < 2) {
            if (prop) XFree(prop);
            XCloseDisplay(dpy);
            return 2;
        }
        Window wm_win = (Window)((unsigned long *)prop)[1];
        XFree(prop);
        XChangeProperty(dpy, wm_win, req_atom, XA_STRING, 8, PropModeAppend,
                        (const unsigned char *)argv[2], (int)strlen(argv[2]) + 1);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "ring-bell") == 0) {
        XkbBell(dpy, None, 100, None);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "warp") == 0 && argc >= 4) {
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, atoi(argv[2]), atoi(argv[3]));
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "client-text") == 0) {
        Atom p = XInternAtom(dpy, "_WP20_TEXT_VALUE", False);
        Window w = find_win_with_prop(dpy, root, p);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom act = None;
        int fmt = 0;
        unsigned long n = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, w, p, 0, 1024, False, AnyPropertyType,
                               &act, &fmt, &n, &rem, &data) == Success && data) {
            fwrite(data, 1, n, stdout);
            XFree(data);
        }
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "client-clear") == 0) {
        Atom p = XInternAtom(dpy, "_WP20_TEXT_VALUE", False);
        Window w = find_win_with_prop(dpy, root, p);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom clr = XInternAtom(dpy, "_WP20_CLEAR_TEXT", False);
        XChangeProperty(dpy, w, clr, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)"1", 1);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "marker-center") == 0) {
        Atom p = XInternAtom(dpy, "_WP20_MARKER_CENTER", False);
        Window w = find_win_with_prop(dpy, root, p);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom act = None;
        int fmt = 0;
        unsigned long n = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, w, p, 0, 64, False, AnyPropertyType,
                               &act, &fmt, &n, &rem, &data) == Success && data) {
            fwrite(data, 1, n, stdout);
            XFree(data);
        }
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "tap") == 0 && argc >= 3) {
        XTestFakeKeyEventFn fk = load_xtest();
        KeySym ks = XStringToKeysym(argv[2]);
        KeyCode kc = XKeysymToKeycode(dpy, ks);
        int hold_ms = (argc >= 4) ? atoi(argv[3]) : 35;
        if (!fk || kc == 0) { XCloseDisplay(dpy); return 4; }
        fk(dpy, kc, True, CurrentTime);
        XSync(dpy, False);
        usleep((useconds_t)hold_ms * 1000u);
        fk(dpy, kc, False, CurrentTime);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "bounce-tap") == 0 && argc >= 5) {
        XTestFakeKeyEventFn fk = load_xtest();
        KeySym ks = XStringToKeysym(argv[2]);
        KeyCode kc = XKeysymToKeycode(dpy, ks);
        int hold_ms = atoi(argv[3]);
        int gap_ms = atoi(argv[4]);
        if (!fk || kc == 0) { XCloseDisplay(dpy); return 4; }
        fk(dpy, kc, True, CurrentTime);
        XSync(dpy, False);
        usleep((useconds_t)hold_ms * 1000u);
        fk(dpy, kc, False, CurrentTime);
        XSync(dpy, False);
        usleep((useconds_t)gap_ms * 1000u);
        fk(dpy, kc, True, CurrentTime);
        XSync(dpy, False);
        usleep((useconds_t)hold_ms * 1000u);
        fk(dpy, kc, False, CurrentTime);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "chord") == 0 && argc >= 4) {
        XTestFakeKeyEventFn fk = load_xtest();
        KeyCode mod_kc = XKeysymToKeycode(dpy, XStringToKeysym(argv[2]));
        KeyCode key_kc = XKeysymToKeycode(dpy, XStringToKeysym(argv[3]));
        if (!fk || mod_kc == 0 || key_kc == 0) { XCloseDisplay(dpy); return 4; }
        fk(dpy, mod_kc, True, CurrentTime);
        XSync(dpy, False);
        usleep(20000);
        fk(dpy, key_kc, True, CurrentTime);
        XSync(dpy, False);
        usleep(30000);
        fk(dpy, key_kc, False, CurrentTime);
        XSync(dpy, False);
        usleep(15000);
        fk(dpy, mod_kc, False, CurrentTime);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "screendump") == 0 && argc >= 3) {
        int scr = DefaultScreen(dpy);
        int w = DisplayWidth(dpy, scr);
        int h = DisplayHeight(dpy, scr);
        XImage *img = XGetImage(dpy, root, 0, 0, (unsigned int)w, (unsigned int)h, AllPlanes, ZPixmap);
        if (!img) { XCloseDisplay(dpy); return 5; }
        FILE *fp = fopen(argv[2], "wb");
        if (!fp) { XDestroyImage(img); XCloseDisplay(dpy); return 6; }
        fprintf(fp, "P6\n%d %d\n255\n", w, h);
        unsigned char *row = malloc((size_t)w * 3u);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                unsigned long px = XGetPixel(img, x, y);
                row[x * 3 + 0] = (unsigned char)((px >> 16) & 0xff);
                row[x * 3 + 1] = (unsigned char)((px >> 8) & 0xff);
                row[x * 3 + 2] = (unsigned char)(px & 0xff);
            }
            fwrite(row, 1, (size_t)w * 3u, fp);
        }
        free(row);
        fclose(fp);
        XDestroyImage(img);
        XCloseDisplay(dpy);
        return 0;
    }

    XCloseDisplay(dpy);
    return 1;
}
"""


MOTIF_A11Y_CLIENT_C = r"""
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/TextF.h>
#include <Xm/Xm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Widget g_top = NULL;
static Widget g_tf = NULL;
static Widget g_canvas = NULL;
static Atom g_atom_val = None;
static Atom g_atom_clr = None;
static Atom g_atom_ctr = None;
static Pixel g_bg_pix = 0;
static Pixel g_mark_pix = 0;

static void publish_text(void) {
    if (!g_top || !XtIsRealized(g_top) || !g_tf) return;
    char *s = XmTextFieldGetString(g_tf);
    const char *v = s ? s : "";
    XChangeProperty(XtDisplay(g_top), XtWindow(g_top), g_atom_val, XA_STRING, 8,
                    PropModeReplace, (const unsigned char *)v, (int)strlen(v));
    if (s) XtFree(s);
    XFlush(XtDisplay(g_top));
}

static void publish_marker_center(void) {
    if (!g_top || !XtIsRealized(g_top) || !g_canvas || !XtIsRealized(g_canvas)) return;
    Display *dpy = XtDisplay(g_top);
    Window child = None;
    int rx = 0, ry = 0;
    if (XTranslateCoordinates(dpy, XtWindow(g_canvas), DefaultRootWindow(dpy),
                              60, 40, &rx, &ry, &child)) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%d %d", rx, ry);
        XChangeProperty(dpy, XtWindow(g_top), g_atom_ctr, XA_STRING, 8,
                        PropModeReplace, (const unsigned char *)buf, n);
        XFlush(dpy);
    }
}

static void on_text_changed(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    publish_text();
}

static void on_expose(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data; (void)call_data;
    if (!XtIsRealized(w)) return;
    Display *dpy = XtDisplay(w);
    Window win = XtWindow(w);
    Dimension cw = 200, ch = 120;
    XtVaGetValues(w, XmNwidth, &cw, XmNheight, &ch, NULL);
    XGCValues gcv;
    gcv.foreground = g_bg_pix;
    GC gc = XCreateGC(dpy, win, GCForeground, &gcv);
    XFillRectangle(dpy, win, gc, 0, 0, cw, ch);
    XSetForeground(dpy, gc, g_mark_pix);
    /* Crisp 40x40 bright magenta square centered at (60, 40) */
    XFillRectangle(dpy, win, gc, 40, 20, 40, 40);
    XFreeGC(dpy, gc);
    publish_marker_center();
}

static void on_top_event(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w; (void)client_data; (void)cont;
    if (ev->type == MapNotify || ev->type == ConfigureNotify) {
        publish_text();
        publish_marker_center();
    } else if (ev->type == PropertyNotify && ev->xproperty.atom == g_atom_clr &&
               ev->xproperty.state == PropertyNewValue) {
        XmTextFieldSetString(g_tf, "");
        XmProcessTraversal(g_tf, XmTRAVERSE_CURRENT);
        publish_text();
    }
}

int main(int argc, char **argv) {
    XtAppContext app;
    g_top = XtVaAppInitialize(&app, "Wp20MotifClient", NULL, 0, &argc, argv, NULL,
                              XmNtitle, "WP20 Motif Accessibility Client",
                              XmNx, 140,
                              XmNy, 140,
                              XmNwidth, 340,
                              XmNheight, 220,
                              NULL);
    Display *dpy = XtDisplay(g_top);
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    XColor c1, c2, ex;
    g_bg_pix = XAllocNamedColor(dpy, cmap, "#182838", &c1, &ex) ? c1.pixel : BlackPixel(dpy, 0);
    g_mark_pix = XAllocNamedColor(dpy, cmap, "#ff00cc", &c2, &ex) ? c2.pixel : WhitePixel(dpy, 0);

    g_atom_val = XInternAtom(dpy, "_WP20_TEXT_VALUE", False);
    g_atom_clr = XInternAtom(dpy, "_WP20_CLEAR_TEXT", False);
    g_atom_ctr = XInternAtom(dpy, "_WP20_MARKER_CENTER", False);

    Widget form = XtVaCreateManagedWidget("form", xmFormWidgetClass, g_top, NULL);
    g_tf = XtVaCreateManagedWidget(
        "wp20Input", xmTextFieldWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 10,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 10,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 10,
        NULL);
    XtAddCallback(g_tf, XmNvalueChangedCallback, on_text_changed, NULL);

    g_canvas = XtVaCreateManagedWidget(
        "wp20Canvas", xmDrawingAreaWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, g_tf,
        XmNtopOffset, 10,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 10,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 10,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 10,
        XmNbackground, g_bg_pix,
        NULL);
    XtAddCallback(g_canvas, XmNexposeCallback, on_expose, NULL);
    XtAddEventHandler(g_top, StructureNotifyMask | PropertyChangeMask, False, on_top_event, NULL);

    XtRealizeWidget(g_top);
    publish_text();
    publish_marker_center();
    XmProcessTraversal(g_tf, XmTRAVERSE_CURRENT);
    XtAppMainLoop(app);
    return 0;
}
"""


ATSPI_MOCK_BUS_PY = r"""#!/usr/bin/env python3
import ctypes
import ctypes.util
import json
import os
import sys

lib_path = ctypes.util.find_library("dbus-1")
dbus = ctypes.CDLL(lib_path)

DBUS_BUS_SESSION = 0
DBUS_NAME_FLAG_REPLACE_EXISTING = 2
DBUS_HANDLER_RESULT_HANDLED = 0
DBUS_HANDLER_RESULT_NOT_YET_HANDLED = 1
DBUS_TYPE_STRING = ord("s")
DBUS_TYPE_BOOLEAN = ord("b")
DBUS_TYPE_VARIANT = ord("v")
DBUS_TYPE_OBJECT_PATH = ord("o")
DBUS_TYPE_STRUCT = ord("r")
DBUS_TYPE_INVALID = 0


class DBusError(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("message", ctypes.c_char_p),
        ("dummy1", ctypes.c_uint),
        ("padding1", ctypes.c_void_p),
    ]


class DBusMessageIter(ctypes.Structure):
    _fields_ = [("dummy", ctypes.c_byte * 128)]


MsgFunc = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)


class DBusObjectPathVTable(ctypes.Structure):
    _fields_ = [
        ("unregister_function", ctypes.c_void_p),
        ("message_function", MsgFunc),
        ("dbus_internal_pad1", ctypes.c_void_p),
        ("dbus_internal_pad2", ctypes.c_void_p),
        ("dbus_internal_pad3", ctypes.c_void_p),
        ("dbus_internal_pad4", ctypes.c_void_p),
    ]


dbus.dbus_error_init.argtypes = [ctypes.POINTER(DBusError)]
dbus.dbus_bus_get.argtypes = [ctypes.c_int, ctypes.POINTER(DBusError)]
dbus.dbus_bus_get.restype = ctypes.c_void_p
dbus.dbus_connection_open_private.argtypes = [ctypes.c_char_p, ctypes.POINTER(DBusError)]
dbus.dbus_connection_open_private.restype = ctypes.c_void_p
dbus.dbus_bus_register.argtypes = [ctypes.c_void_p, ctypes.POINTER(DBusError)]
dbus.dbus_bus_register.restype = ctypes.c_uint
dbus.dbus_bus_request_name.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint, ctypes.POINTER(DBusError)
]
dbus.dbus_bus_request_name.restype = ctypes.c_int
dbus.dbus_connection_register_object_path.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(DBusObjectPathVTable), ctypes.c_void_p
]
dbus.dbus_connection_register_object_path.restype = ctypes.c_uint
dbus.dbus_connection_read_write_dispatch.argtypes = [ctypes.c_void_p, ctypes.c_int]
dbus.dbus_connection_read_write_dispatch.restype = ctypes.c_uint
dbus.dbus_connection_send.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)
]
dbus.dbus_connection_send.restype = ctypes.c_uint
dbus.dbus_connection_flush.argtypes = [ctypes.c_void_p]
dbus.dbus_message_is_method_call.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
dbus.dbus_message_is_method_call.restype = ctypes.c_uint
dbus.dbus_message_new_method_return.argtypes = [ctypes.c_void_p]
dbus.dbus_message_new_method_return.restype = ctypes.c_void_p
dbus.dbus_message_unref.argtypes = [ctypes.c_void_p]
dbus.dbus_message_iter_init.argtypes = [ctypes.c_void_p, ctypes.POINTER(DBusMessageIter)]
dbus.dbus_message_iter_init.restype = ctypes.c_uint
dbus.dbus_message_iter_get_arg_type.argtypes = [ctypes.POINTER(DBusMessageIter)]
dbus.dbus_message_iter_get_arg_type.restype = ctypes.c_int
dbus.dbus_message_iter_recurse.argtypes = [
    ctypes.POINTER(DBusMessageIter), ctypes.POINTER(DBusMessageIter)
]
dbus.dbus_message_iter_next.argtypes = [ctypes.POINTER(DBusMessageIter)]
dbus.dbus_message_iter_next.restype = ctypes.c_uint
dbus.dbus_message_iter_get_basic.argtypes = [ctypes.POINTER(DBusMessageIter), ctypes.c_void_p]
dbus.dbus_message_iter_init_append.argtypes = [ctypes.c_void_p, ctypes.POINTER(DBusMessageIter)]
dbus.dbus_message_iter_append_basic.argtypes = [
    ctypes.POINTER(DBusMessageIter), ctypes.c_int, ctypes.c_void_p
]
dbus.dbus_message_iter_append_basic.restype = ctypes.c_uint
dbus.dbus_message_iter_open_container.argtypes = [
    ctypes.POINTER(DBusMessageIter), ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(DBusMessageIter)
]
dbus.dbus_message_iter_open_container.restype = ctypes.c_uint
dbus.dbus_message_iter_close_container.argtypes = [
    ctypes.POINTER(DBusMessageIter), ctypes.POINTER(DBusMessageIter)
]
dbus.dbus_message_iter_close_container.restype = ctypes.c_uint

atspi_addr = sys.argv[1].encode("utf-8")
state_file = sys.argv[2]
state = {"embedded": [], "is_enabled": True}


def save_state():
    tmp = state_file + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fp:
        json.dump(state, fp)
    os.replace(tmp, state_file)


@MsgFunc
def on_session_bus_msg(conn, msg, _user_data):
    if dbus.dbus_message_is_method_call(msg, b"org.a11y.Bus", b"GetAddress"):
        rep = dbus.dbus_message_new_method_return(msg)
        it = DBusMessageIter()
        dbus.dbus_message_iter_init_append(rep, ctypes.byref(it))
        val = ctypes.c_char_p(atspi_addr)
        dbus.dbus_message_iter_append_basic(
            ctypes.byref(it), DBUS_TYPE_STRING, ctypes.byref(val)
        )
        dbus.dbus_connection_send(conn, rep, None)
        dbus.dbus_connection_flush(conn)
        dbus.dbus_message_unref(rep)
        return DBUS_HANDLER_RESULT_HANDLED
    if dbus.dbus_message_is_method_call(msg, b"org.freedesktop.DBus.Properties", b"Get"):
        rep = dbus.dbus_message_new_method_return(msg)
        it = DBusMessageIter()
        sub = DBusMessageIter()
        dbus.dbus_message_iter_init_append(rep, ctypes.byref(it))
        dbus.dbus_message_iter_open_container(
            ctypes.byref(it), DBUS_TYPE_VARIANT, b"b", ctypes.byref(sub)
        )
        bval = ctypes.c_uint32(1 if state["is_enabled"] else 0)
        dbus.dbus_message_iter_append_basic(
            ctypes.byref(sub), DBUS_TYPE_BOOLEAN, ctypes.byref(bval)
        )
        dbus.dbus_message_iter_close_container(ctypes.byref(it), ctypes.byref(sub))
        dbus.dbus_connection_send(conn, rep, None)
        dbus.dbus_connection_flush(conn)
        dbus.dbus_message_unref(rep)
        return DBUS_HANDLER_RESULT_HANDLED
    if dbus.dbus_message_is_method_call(msg, b"org.freedesktop.DBus.Properties", b"Set"):
        rep = dbus.dbus_message_new_method_return(msg)
        dbus.dbus_connection_send(conn, rep, None)
        dbus.dbus_connection_flush(conn)
        dbus.dbus_message_unref(rep)
        return DBUS_HANDLER_RESULT_HANDLED
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED


@MsgFunc
def on_registry_msg(conn, msg, _user_data):
    if dbus.dbus_message_is_method_call(msg, b"org.a11y.atspi.Socket", b"Embed"):
        it = DBusMessageIter()
        sub = DBusMessageIter()
        bus_name = ""
        obj_path = ""
        if dbus.dbus_message_iter_init(msg, ctypes.byref(it)):
            if dbus.dbus_message_iter_get_arg_type(ctypes.byref(it)) == DBUS_TYPE_STRUCT:
                dbus.dbus_message_iter_recurse(ctypes.byref(it), ctypes.byref(sub))
                s = ctypes.c_char_p()
                dbus.dbus_message_iter_get_basic(ctypes.byref(sub), ctypes.byref(s))
                bus_name = (s.value or b"").decode("utf-8")
                dbus.dbus_message_iter_next(ctypes.byref(sub))
                o = ctypes.c_char_p()
                dbus.dbus_message_iter_get_basic(ctypes.byref(sub), ctypes.byref(o))
                obj_path = (o.value or b"").decode("utf-8")
        state["embedded"].append({"bus_name": bus_name, "root_path": obj_path})
        save_state()

        rep = dbus.dbus_message_new_method_return(msg)
        out = DBusMessageIter()
        st = DBusMessageIter()
        dbus.dbus_message_iter_init_append(rep, ctypes.byref(out))
        dbus.dbus_message_iter_open_container(
            ctypes.byref(out), DBUS_TYPE_STRUCT, None, ctypes.byref(st)
        )
        reg_bus = ctypes.c_char_p(b"org.a11y.atspi.Registry")
        reg_path = ctypes.c_char_p(b"/org/a11y/atspi/accessible/root")
        dbus.dbus_message_iter_append_basic(
            ctypes.byref(st), DBUS_TYPE_STRING, ctypes.byref(reg_bus)
        )
        dbus.dbus_message_iter_append_basic(
            ctypes.byref(st), DBUS_TYPE_OBJECT_PATH, ctypes.byref(reg_path)
        )
        dbus.dbus_message_iter_close_container(ctypes.byref(out), ctypes.byref(st))
        dbus.dbus_connection_send(conn, rep, None)
        dbus.dbus_connection_flush(conn)
        dbus.dbus_message_unref(rep)
        return DBUS_HANDLER_RESULT_HANDLED
    if dbus.dbus_message_is_method_call(msg, b"org.a11y.atspi.Socket", b"Unembed"):
        rep = dbus.dbus_message_new_method_return(msg)
        dbus.dbus_connection_send(conn, rep, None)
        dbus.dbus_connection_flush(conn)
        dbus.dbus_message_unref(rep)
        return DBUS_HANDLER_RESULT_HANDLED
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED


err = DBusError()
dbus.dbus_error_init(ctypes.byref(err))
sess = dbus.dbus_bus_get(DBUS_BUS_SESSION, ctypes.byref(err))
dbus.dbus_bus_request_name(sess, b"org.a11y.Bus", DBUS_NAME_FLAG_REPLACE_EXISTING, ctypes.byref(err))
vtable_sess = DBusObjectPathVTable(None, on_session_bus_msg, None, None, None, None)
dbus.dbus_connection_register_object_path(sess, b"/org/a11y/bus", ctypes.byref(vtable_sess), None)

dbus.dbus_error_init(ctypes.byref(err))
atspi = dbus.dbus_connection_open_private(atspi_addr, ctypes.byref(err))
dbus.dbus_bus_register(atspi, ctypes.byref(err))
dbus.dbus_bus_request_name(
    atspi, b"org.a11y.atspi.Registry", DBUS_NAME_FLAG_REPLACE_EXISTING, ctypes.byref(err)
)
vtable_reg = DBusObjectPathVTable(None, on_registry_msg, None, None, None, None)
dbus.dbus_connection_register_object_path(
    atspi, b"/org/a11y/atspi/accessible/root", ctypes.byref(vtable_reg), None
)

save_state()
while True:
    dbus.dbus_connection_read_write_dispatch(sess, 15)
    dbus.dbus_connection_read_write_dispatch(atspi, 15)
"""


DBUS_CONF_TEMPLATE = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path={socket_path}</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""


def wait_until(pred, timeout: float = 8.0, interval: float = 0.05, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_val = None
    while time.monotonic() < deadline:
        try:
            last_val = pred()
            if last_val:
                return last_val
        except Exception:
            pass
        time.sleep(interval)
    raise RuntimeError(f"Timed out waiting for {desc} (last={last_val!r})")


def parse_state_tokens(raw_output: str) -> dict[str, str]:
    for line in raw_output.splitlines():
        if line.startswith("_DT_ACCESSIBILITY_STATE="):
            body = line.split("=", 1)[1].strip()
            out: dict[str, str] = {}
            for tok in body.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    out[k] = v
            return out
    return {}


def verify_pid_maps_motif9(pid: int, label: str, expected_prefix: Path | None = None) -> str:
    maps_path = Path(f"/proc/{pid}/maps")
    txt = maps_path.read_text(encoding="utf-8", errors="replace")
    matches = [ln for ln in txt.splitlines() if "libXm.so.9" in ln]
    if not matches:
        raise RuntimeError(f"{label} (pid={pid}) does not map libXm.so.9!\n{txt[:600]}")
    if expected_prefix is not None:
        exp_str = str(expected_prefix / "lib")
        if not any(exp_str in ln for ln in matches):
            raise RuntimeError(
                f"{label} (pid={pid}) mapped wrong libXm.so.9 (expected {exp_str}): {matches[0]}"
            )
    return matches[0].split()[-1]


def start_isolated_dbus(work: Path, name: str) -> tuple[str, subprocess.Popen]:
    sock = work / f"{name}.sock"
    conf = work / f"{name}.conf"
    conf.write_text(DBUS_CONF_TEMPLATE.format(socket_path=sock), encoding="utf-8")
    p = subprocess.Popen(
        ["dbus-daemon", f"--config-file={conf}", "--nofork", "--print-address"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    assert p.stdout is not None
    addr = p.stdout.readline().strip()
    if not addr:
        raise RuntimeError(f"Failed to start isolated dbus-daemon ({name})")
    return addr, p


def measure_magenta_bbox(img_path: Path) -> tuple[int, int]:
    """Measures (width, height) bounding box of #ff00cc marker pixels."""
    with Image.open(img_path) as im:
        rgb = im.convert("RGB")
        w, h = rgb.size
        px = rgb.load()
        min_x, min_y = w, h
        max_x, max_y = -1, -1
        for y in range(h):
            for x in range(w):
                r, g, b = px[x, y]
                if r >= 210 and g <= 45 and b >= 165:
                    if x < min_x:
                        min_x = x
                    if y < min_y:
                        min_y = y
                    if x > max_x:
                        max_x = x
                    if y > max_y:
                        max_y = y
        if max_x < min_x or max_y < min_y:
            return (0, 0)
        return (max_x - min_x + 1, max_y - min_y + 1)


def count_vbell_flash_pixels(img_path: Path) -> int:
    """Counts warm-bright Visual Bell flash pixels (#fff5c8 / R>=240, G>=230, B>=170)."""
    with Image.open(img_path) as im:
        rgb = im.convert("RGB")
        w, h = rgb.size
        px = rgb.load()
        cnt = 0
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                r, g, b = px[x, y]
                if r >= 240 and g >= 230 and b >= 170:
                    cnt += 1
        return cnt


def gdbus_atspi(atspi_addr: str, env: dict[str, str], dest: str, path: str, method: str, *args: str) -> str:
    cmd = [
        "gdbus",
        "call",
        "--address",
        atspi_addr,
        "--dest",
        dest,
        "--object-path",
        path,
        "--method",
        method,
    ]
    if args:
        cmd.append("--")
        cmd.extend(args)
    return subprocess.check_output(cmd, env=env, stderr=subprocess.DEVNULL, text=True).strip()


def gdbus_atspi_prop(atspi_addr: str, env: dict[str, str], dest: str, path: str, iface: str, prop: str) -> str:
    return gdbus_atspi(
        atspi_addr, env, dest, path, "org.freedesktop.DBus.Properties.Get", iface, prop
    )


def walk_atspi_tree(atspi_addr: str, env: dict[str, str], bus_name: str, _root_path: str) -> list[dict]:
    """Queries org.a11y.atspi.Cache.GetItems atomically in a single D-Bus round-trip."""
    raw = gdbus_atspi(
        atspi_addr, env, bus_name, "/org/a11y/atspi/cache", "org.a11y.atspi.Cache.GetItems"
    )
    item_pat = re.compile(
        r"\(\('[^']+',\s*(?:objectpath\s+)?'(/org/a11y/atspi/accessible/[^']+)'\),\s*"
        r"\('[^']*',\s*(?:objectpath\s+)?'[^']+'\),\s*"
        r"\('[^']*',\s*(?:objectpath\s+)?'[^']+'\),\s*"
        r"-?\d+,\s*-?\d+,\s*\[[^\]]*\],\s*'([^']*)',\s*(?:uint32\s+)?(\d+),"
    )
    nodes: list[dict] = []
    for m in item_pat.finditer(raw):
        nodes.append({"path": m.group(1), "name": m.group(2), "role": int(m.group(3))})
    return nodes


def get_atspi_states(atspi_addr: str, env: dict[str, str], bus_name: str, path: str) -> int:
    raw = gdbus_atspi(atspi_addr, env, bus_name, path, "org.a11y.atspi.Accessible.GetState")
    nums = [int(x) for x in re.findall(r"(?<!uint)\b(\d+)\b", raw)]
    lo = nums[0] if len(nums) >= 1 else 0
    hi = nums[1] if len(nums) >= 2 else 0
    return lo | (hi << 32)


def terminate_procs(procs: list[subprocess.Popen]):
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + 2.5
    for p in reversed(procs):
        while p.poll() is None and time.monotonic() < deadline:
            time.sleep(0.05)
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def build_common_env(
    home: Path,
    xdg_rt: Path,
    motif_prefix: Path | None,
    cde_build_root: Path,
    apps_bin_dir: Path,
) -> dict[str, str]:
    env = os.environ.copy()
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    env.pop("NO_AT_BRIDGE", None)
    env.pop("XM_ACCESSIBILITY", None)
    env["HOME"] = str(home)
    env["XDG_RUNTIME_DIR"] = str(xdg_rt)
    env["XAPPLRESDIR"] = str(cde_build_root / "programs/dtstyle")

    path_parts = [
        str(apps_bin_dir),
        str(cde_build_root / "programs/dtwm"),
        str(cde_build_root / "programs/dtstyle/.libs"),
        "/usr/dt/bin",
        env.get("PATH", "/usr/bin:/bin"),
    ]
    env["PATH"] = ":".join(path_parts)

    ld_parts = []
    if motif_prefix is not None and (motif_prefix / "lib").is_dir():
        ld_parts.append(str(motif_prefix / "lib"))
    for sub in (
        "lib/DtPrint/.libs",
        "lib/DtHelp/.libs",
        "lib/DtWidget/.libs",
        "lib/DtSvc/.libs",
        "lib/tt/lib/.libs",
        "lib/DtXinerama/.libs",
    ):
        cand = cde_build_root / sub
        if cand.is_dir():
            ld_parts.append(str(cand))
    if Path("/var/tmp/ck-wayland-build/wp00/fs-prefix/lib").is_dir():
        ld_parts.append("/var/tmp/ck-wayland-build/wp00/fs-prefix/lib")
    if env.get("LD_LIBRARY_PATH"):
        ld_parts.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
    return env


def verify_atspi_apps(
    lane_label: str,
    env: dict[str, str],
    atspi_addr: str,
    atspi_state_file: Path,
    dtstyle_bin: Path,
    ck_tasks_bin: Path,
    motif_prefix: Path | None,
    procs: list[subprocess.Popen],
) -> str:
    a11y_env = dict(env)
    a11y_env["XM_ACCESSIBILITY"] = "1"
    a11y_env["AT_SPI_BUS_ADDRESS"] = atspi_addr

    prev_count = len(json.loads(atspi_state_file.read_text(encoding="utf-8"))["embedded"])
    dtstyle_p = subprocess.Popen(
        [str(dtstyle_bin), "-accessibility"],
        env=a11y_env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    procs.append(dtstyle_p)

    def dtstyle_embedded():
        st = json.loads(atspi_state_file.read_text(encoding="utf-8"))
        return st["embedded"][prev_count] if len(st["embedded"]) > prev_count else None

    emb_style = wait_until(dtstyle_embedded, timeout=8.0, desc=f"{lane_label} dtstyle AT-SPI2 Embed")
    verify_pid_maps_motif9(dtstyle_p.pid, f"{lane_label} dtstyle", motif_prefix)

    def find_sticky_toggle():
        nodes = walk_atspi_tree(
            atspi_addr, a11y_env, emb_style["bus_name"], emb_style["root_path"]
        )
        for n in nodes:
            if "Sticky Keys" in n["name"] and n["role"] == 7:
                return (nodes, n)
        return None

    style_nodes, sticky_node = wait_until(
        find_sticky_toggle, timeout=8.0, desc=f"{lane_label} dtstyle Sticky Keys AT-SPI2 node"
    )
    # Invoke Action.DoAction(0) over D-Bus on Sticky Keys toggle inside dtstyle!
    ATSPI_STATE_CHECKED = 1 << 4
    before_st = get_atspi_states(
        atspi_addr, a11y_env, emb_style["bus_name"], sticky_node["path"]
    )
    gdbus_atspi(
        atspi_addr,
        a11y_env,
        emb_style["bus_name"],
        sticky_node["path"],
        "org.a11y.atspi.Action.DoAction",
        "0",
    )
    wait_until(
        lambda: (
            (get_atspi_states(atspi_addr, a11y_env, emb_style["bus_name"], sticky_node["path"]) & ATSPI_STATE_CHECKED)
            != (before_st & ATSPI_STATE_CHECKED)
        ),
        timeout=5.0,
        desc=f"{lane_label} dtstyle Sticky Keys DoAction(0) state toggle",
    )

    # Now launch ck-tasks with XM_ACCESSIBILITY=1 and verify CkTable semantic Table/Selection/Action!
    prev_count2 = len(json.loads(atspi_state_file.read_text(encoding="utf-8"))["embedded"])
    tasks_p = subprocess.Popen(
        [str(ck_tasks_bin)],
        env=a11y_env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    procs.append(tasks_p)

    def tasks_embedded():
        st = json.loads(atspi_state_file.read_text(encoding="utf-8"))
        return st["embedded"][prev_count2] if len(st["embedded"]) > prev_count2 else None

    emb_tasks = wait_until(tasks_embedded, timeout=8.0, desc=f"{lane_label} ck-tasks AT-SPI2 Embed")
    verify_pid_maps_motif9(tasks_p.pid, f"{lane_label} ck-tasks", motif_prefix)

    def find_populated_ck_table():
        nodes = walk_atspi_tree(
            atspi_addr, a11y_env, emb_tasks["bus_name"], emb_tasks["root_path"]
        )
        for n in nodes:
            if n["role"] == 55:  # XmACCESSIBLE_TABLE (ATSPI_ROLE_TABLE)
                nr_raw = gdbus_atspi_prop(
                    atspi_addr, a11y_env, emb_tasks["bus_name"], n["path"], "org.a11y.atspi.Table", "NRows"
                )
                nc_raw = gdbus_atspi_prop(
                    atspi_addr, a11y_env, emb_tasks["bus_name"], n["path"], "org.a11y.atspi.Table", "NColumns"
                )
                m_r = re.search(r"<(?:int32\s+)?(\d+)>", nr_raw)
                m_c = re.search(r"<(?:int32\s+)?(\d+)>", nc_raw)
                nrows = int(m_r.group(1)) if m_r else 0
                ncols = int(m_c.group(1)) if m_c else 0
                if nrows >= 2 and ncols >= 3:
                    return (n["path"], nrows, ncols)
        return None

    tbl_path, nrows, ncols = wait_until(
        find_populated_ck_table, timeout=8.0, desc=f"{lane_label} ck-tasks CkTable populated"
    )
    # Query cell (0, 0) via Table.GetAccessibleAt(0, 0) and select row 0 via Selection.SelectChild(0)
    cell_raw = gdbus_atspi(
        atspi_addr, a11y_env, emb_tasks["bus_name"], tbl_path, "org.a11y.atspi.Table.GetAccessibleAt", "0", "0"
    )
    m_cell = re.search(r"(?:objectpath\s+)?'(/org/a11y/atspi/accessible/[^']+)'", cell_raw)
    if not m_cell:
        raise RuntimeError(f"{lane_label}: CkTable GetAccessibleAt(0,0) returned invalid ref: {cell_raw}")
    sel_res = gdbus_atspi(
        atspi_addr, a11y_env, emb_tasks["bus_name"], tbl_path, "org.a11y.atspi.Selection.SelectChild", "0"
    )
    if "true" not in sel_res:
        raise RuntimeError(f"{lane_label}: CkTable SelectChild(0) failed: {sel_res}")

    return (
        f"AT-SPI2(dtstyle={len(style_nodes)} nodes+DoAction, "
        f"ck-tasks CkTable={nrows}x{ncols}+SelectChild)"
    )


def run_wayland_lane(
    comp_bin: Path,
    dtstyle_bin: Path,
    ck_tasks_bin: Path,
    wl_helper_bin: Path,
    x11_helper_bin: Path,
    motif_client_bin: Path,
    atspi_mock_py: Path,
    motif_prefix: Path | None,
    cde_build_root: Path,
    apps_bin_dir: Path,
) -> str:
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp20-wl-", dir=tmp_base) as td:
        work = Path(td)
        home = work / "home"
        xdg_rt = work / "rt"
        (home / ".dt/sessions").mkdir(parents=True, exist_ok=True)
        xdg_rt.mkdir(parents=True, exist_ok=True)
        xdg_rt.chmod(0o700)

        env = build_common_env(home, xdg_rt, motif_prefix, cde_build_root, apps_bin_dir)
        env["XDG_SESSION_TYPE"] = "wayland"
        env["WLR_BACKENDS"] = "headless"
        env["WLR_RENDERER"] = "pixman"

        procs: list[subprocess.Popen] = []
        try:
            sess_addr, sess_dbus = start_isolated_dbus(work, "wl-session-dbus")
            procs.append(sess_dbus)
            atspi_addr, atspi_dbus = start_isolated_dbus(work, "wl-atspi-dbus")
            procs.append(atspi_dbus)
            env["DBUS_SESSION_BUS_ADDRESS"] = sess_addr

            atspi_state = work / "atspi-state.json"
            mock_reg = subprocess.Popen(
                [sys.executable, str(atspi_mock_py), atspi_addr, str(atspi_state)],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(mock_reg)
            wait_until(lambda: atspi_state.is_file(), timeout=5.0, desc="Wayland AT-SPI2 mock bus")

            status_file = work / "comp.status"
            comp = subprocess.Popen(
                [str(comp_bin), "--no-panel", "--status-file", str(status_file)],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(comp)
            wait_until(lambda: status_file.is_file(), timeout=10.0, desc="dtwm-wayland status file")
            for ln in status_file.read_text(encoding="utf-8").splitlines():
                if "=" in ln:
                    k, v = ln.split("=", 1)
                    if k in ("WAYLAND_DISPLAY", "DISPLAY"):
                        env[k] = v

            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("zoom_pct")
                == "100",
                timeout=8.0,
                desc="Wayland initial _DT_ACCESSIBILITY_STATE",
            )

            # Launch Motif test client (XmTextField + 40x40 #ff00cc marker)
            client_p = subprocess.Popen(
                [str(motif_client_bin)],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(client_p)
            marker_xy = wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "marker-center"], env=env, text=True
                ).strip(),
                timeout=8.0,
                desc="Wayland Motif client mapped & marker coordinates ready",
            )
            verify_pid_maps_motif9(client_p.pid, "Wayland Motif test client", motif_prefix)

            # 1. Sticky Keys via dtstyle -apply-accessibility
            subprocess.run(
                [
                    str(dtstyle_bin),
                    "-apply-accessibility",
                    "sticky=1,two_keys_off=0,slow=0,bounce=0,vbell=window,reduced_motion=1",
                ],
                env=env,
                check=True,
            )
            conf_txt = (home / ".dt/accessibility.conf").read_text(encoding="utf-8")
            if "sticky_keys=1" not in conf_txt or "reduced_motion=1" not in conf_txt:
                raise RuntimeError(f"dtstyle did not persist ~/.dt/accessibility.conf:\n{conf_txt}")

            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("sticky")
                == "1",
                timeout=5.0,
                desc="Wayland sticky=1 state",
            )
            subprocess.run([str(x11_helper_bin), "client-clear"], env=env, check=True)
            time.sleep(0.1)

            # Tap Shift_L (42), release, tap 'a' (30) -> 'A'; tap 'b' (48) -> 'Ab'
            subprocess.run([str(wl_helper_bin), "tap", "42", "35"], env=env, check=True)
            wait_until(
                lambda: int(
                    parse_state_tokens(
                        subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                    ).get("latched", "0")
                )
                != 0,
                timeout=3.0,
                desc="Wayland Sticky Keys Shift latched",
            )
            subprocess.run([str(wl_helper_bin), "tap", "30", "35"], env=env, check=True)
            subprocess.run([str(wl_helper_bin), "tap", "48", "35"], env=env, check=True)
            wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "client-text"], env=env, text=True
                )
                == "Ab",
                timeout=4.0,
                desc="Wayland Sticky Keys latch ('Ab')",
            )

            # Double-tap Shift_L -> lock; tap 'c' (46), 'd' (32) -> 'AbCD'; tap Shift_L -> unlock; tap 'e' (18) -> 'AbCDe'
            subprocess.run([str(wl_helper_bin), "tap", "42", "35"], env=env, check=True)
            subprocess.run([str(wl_helper_bin), "tap", "42", "35"], env=env, check=True)
            wait_until(
                lambda: int(
                    parse_state_tokens(
                        subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                    ).get("locked", "0")
                )
                != 0,
                timeout=3.0,
                desc="Wayland Sticky Keys Shift locked",
            )
            subprocess.run([str(wl_helper_bin), "tap", "46", "35"], env=env, check=True)
            subprocess.run([str(wl_helper_bin), "tap", "32", "35"], env=env, check=True)
            subprocess.run([str(wl_helper_bin), "tap", "42", "35"], env=env, check=True)
            subprocess.run([str(wl_helper_bin), "tap", "18", "35"], env=env, check=True)
            wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "client-text"], env=env, text=True
                )
                == "AbCDe",
                timeout=4.0,
                desc="Wayland Sticky Keys lock+unlock ('AbCDe')",
            )

            # 2. Slow Keys (slow_ms=260): 50ms tap 's' (31) rejected, 360ms hold 's' accepted
            subprocess.run(
                [str(x11_helper_bin), "send-req", "f.set_accessibility sticky=0 slow=1 slow_ms=260 bounce=0"],
                env=env,
                check=True,
            )
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("slow")
                == "1",
                timeout=4.0,
                desc="Wayland slow=1",
            )
            subprocess.run([str(x11_helper_bin), "client-clear"], env=env, check=True)
            time.sleep(0.1)
            subprocess.run([str(wl_helper_bin), "tap", "31", "50"], env=env, check=True)
            time.sleep(0.32)
            txt_after_short = subprocess.check_output(
                [str(x11_helper_bin), "client-text"], env=env, text=True
            )
            if txt_after_short != "":
                raise RuntimeError(f"Wayland Slow Keys failed to reject 50ms tap: got {txt_after_short!r}")
            subprocess.run([str(wl_helper_bin), "tap", "31", "360"], env=env, check=True)
            wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "client-text"], env=env, text=True
                )
                == "s",
                timeout=4.0,
                desc="Wayland Slow Keys 360ms hold accepted ('s')",
            )

            # 3. Bounce Keys (bounce_ms=260): two rapid taps of 'k' (37) 45ms apart -> single 'k'
            subprocess.run(
                [str(x11_helper_bin), "send-req", "f.set_accessibility slow=0 bounce=1 bounce_ms=260"],
                env=env,
                check=True,
            )
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("bounce")
                == "1",
                timeout=4.0,
                desc="Wayland bounce=1",
            )
            subprocess.run([str(x11_helper_bin), "client-clear"], env=env, check=True)
            time.sleep(0.1)
            subprocess.run([str(wl_helper_bin), "bounce-tap", "37", "35", "45"], env=env, check=True)
            time.sleep(0.32)
            txt_bounce = subprocess.check_output(
                [str(x11_helper_bin), "client-text"], env=env, text=True
            )
            if txt_bounce != "k":
                raise RuntimeError(f"Wayland Bounce Keys expected single 'k', got {txt_bounce!r}")
            subprocess.run([str(wl_helper_bin), "tap", "37", "35"], env=env, check=True)
            wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "client-text"], env=env, text=True
                )
                == "kk",
                timeout=4.0,
                desc="Wayland Bounce Keys post-interval tap accepted ('kk')",
            )
            subprocess.run(
                [str(x11_helper_bin), "send-req", "f.set_accessibility bounce=0"],
                env=env,
                check=True,
            )

            # 4. Visual Bell (via X11 XkbBell on XWayland + grim flash verification)
            subprocess.run(
                [str(x11_helper_bin), "send-req", "f.set_accessibility vbell=screen reduced_motion=1"],
                env=env,
                check=True,
            )
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("vbell")
                == "screen",
                timeout=4.0,
                desc="Wayland vbell=screen",
            )
            subprocess.run([str(x11_helper_bin), "ring-bell"], env=env, check=True)
            vbell_png = work / "wl-vbell.png"
            subprocess.run(["grim", str(vbell_png)], env=env, check=True)
            flash_px = count_vbell_flash_pixels(vbell_png)
            wait_until(
                lambda: int(
                    parse_state_tokens(
                        subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                    ).get("vbell_count", "0")
                )
                >= 1,
                timeout=4.0,
                desc="Wayland XWayland XkbBellNotify incremented vbell_count",
            )
            if flash_px < 1000:
                raise RuntimeError(f"Wayland Visual Bell flash not visible in grim ({flash_px} px)")
            time.sleep(0.22)

            # 5. Screen Magnification (Super+Equal x4 -> 200%, Super+Minus -> 175%, Super+0 -> 100%)
            mx_s, my_s = marker_xy.split()
            mx, my = int(mx_s), int(my_s)
            subprocess.run(
                [str(wl_helper_bin), "warp", str(mx), str(my), "1280", "900"],
                env=env,
                check=True,
            )
            time.sleep(0.1)
            mag100_png = work / "wl-mag-100.png"
            subprocess.run(["grim", str(mag100_png)], env=env, check=True)
            w100, h100 = measure_magenta_bbox(mag100_png)
            if w100 < 30 or h100 < 30:
                raise RuntimeError(f"Wayland 100% marker bbox unexpected: {(w100, h100)}")

            # Super_L (125) + equal (13) x 4 -> 200%
            for _ in range(4):
                subprocess.run([str(wl_helper_bin), "chord", "125", "13"], env=env, check=True)
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("zoom_pct")
                == "200",
                timeout=4.0,
                desc="Wayland Super+Equal zoom_pct=200",
            )
            time.sleep(0.1)
            mag200_png = work / "wl-mag-200.png"
            subprocess.run(["grim", str(mag200_png)], env=env, check=True)
            w200, h200 = measure_magenta_bbox(mag200_png)
            if w200 < int(w100 * 1.75) or h200 < int(h100 * 1.75):
                raise RuntimeError(
                    f"Wayland 200% magnified marker did not scale ~2x: 100%={(w100, h100)}, 200%={(w200, h200)}"
                )

            # Super_L (125) + minus (12) -> 175%, then Super_L (125) + 0 (11) -> 100%
            subprocess.run([str(wl_helper_bin), "chord", "125", "12"], env=env, check=True)
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("zoom_pct")
                == "175",
                timeout=4.0,
                desc="Wayland Super+Minus zoom_pct=175",
            )
            subprocess.run([str(wl_helper_bin), "chord", "125", "11"], env=env, check=True)
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("zoom_pct")
                == "100",
                timeout=4.0,
                desc="Wayland Super+0 zoom_pct=100",
            )

            # 6. Motif AT-SPI2 Auto-Bridge on dtstyle + ck-tasks (CkTable)
            atspi_summary = verify_atspi_apps(
                "Wayland",
                env,
                atspi_addr,
                atspi_state,
                dtstyle_bin,
                ck_tasks_bin,
                motif_prefix,
                procs,
            )

            return (
                f"Wayland OK (AccessX Sticky/Slow/Bounce, vbell flash={flash_px}px, "
                f"Magnifier 100%={w100}x{h100}->200%={w200}x{h200}->100%, {atspi_summary})"
            )
        finally:
            terminate_procs(procs)


def pick_free_x11_display() -> int:
    base = 70 + (os.getpid() % 20)
    for offset in range(30):
        d = 62 + ((base - 62 + offset) % 35)
        if not Path(f"/tmp/.X{d}-lock").exists() and not Path(f"/tmp/.X11-unix/X{d}").exists():
            return d
    raise RuntimeError("No free Xvfb display found in 62..96")


def run_x11_lane(
    dtwm_x11_bin: Path,
    dtstyle_bin: Path,
    ck_tasks_bin: Path,
    ck_magnifier_bin: Path,
    x11_helper_bin: Path,
    motif_client_bin: Path,
    atspi_mock_py: Path,
    motif_prefix: Path | None,
    cde_build_root: Path,
    apps_bin_dir: Path,
) -> str:
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp20-x11-", dir=tmp_base) as td:
        work = Path(td)
        home = work / "home"
        xdg_rt = work / "rt"
        (home / ".dt/sessions").mkdir(parents=True, exist_ok=True)
        xdg_rt.mkdir(parents=True, exist_ok=True)
        xdg_rt.chmod(0o700)

        env = build_common_env(home, xdg_rt, motif_prefix, cde_build_root, apps_bin_dir)
        env["XDG_SESSION_TYPE"] = "x11"
        disp = f":{pick_free_x11_display()}"
        env["DISPLAY"] = disp

        procs: list[subprocess.Popen] = []
        try:
            sess_addr, sess_dbus = start_isolated_dbus(work, "x11-session-dbus")
            procs.append(sess_dbus)
            atspi_addr, atspi_dbus = start_isolated_dbus(work, "x11-atspi-dbus")
            procs.append(atspi_dbus)
            env["DBUS_SESSION_BUS_ADDRESS"] = sess_addr

            atspi_state = work / "atspi-state.json"
            mock_reg = subprocess.Popen(
                [sys.executable, str(atspi_mock_py), atspi_addr, str(atspi_state)],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(mock_reg)
            wait_until(lambda: atspi_state.is_file(), timeout=5.0, desc="X11 AT-SPI2 mock bus")

            xvfb = subprocess.Popen(
                ["Xvfb", disp, "-screen", "0", "1280x900x24", "+extension", "Composite", "-nolisten", "tcp", "-ac"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(xvfb)
            wait_until(
                lambda: subprocess.run(
                    [str(x11_helper_bin), "state"],
                    env=env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                ).returncode
                == 0,
                timeout=8.0,
                desc=f"Xvfb {disp} ready",
            )

            dtwm_p = subprocess.Popen(
                [str(dtwm_x11_bin)],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(dtwm_p)
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("zoom_pct")
                == "100",
                timeout=8.0,
                desc="X11 dtwm _DT_ACCESSIBILITY_STATE ready",
            )
            verify_pid_maps_motif9(dtwm_p.pid, "X11 dtwm", motif_prefix)

            # Start ck-magnifier daemon on X11 and verify its libXm.so.9 provenance
            mag_p = subprocess.Popen(
                [str(ck_magnifier_bin), "--daemon"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(mag_p)
            time.sleep(0.3)
            verify_pid_maps_motif9(mag_p.pid, "X11 ck-magnifier", motif_prefix)

            # Launch Motif test client
            client_p = subprocess.Popen(
                [str(motif_client_bin)],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(client_p)
            marker_xy = wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "marker-center"], env=env, text=True
                ).strip(),
                timeout=8.0,
                desc="X11 Motif client mapped & marker coordinates ready",
            )
            verify_pid_maps_motif9(client_p.pid, "X11 Motif test client", motif_prefix)

            # Focus client window by warping pointer into it and clicking/clearing
            mx_s, my_s = marker_xy.split()
            mx, my = int(mx_s), int(my_s)
            subprocess.run([str(x11_helper_bin), "warp", str(mx), str(my - 55)], env=env, check=True)
            subprocess.run([str(x11_helper_bin), "client-clear"], env=env, check=True)
            time.sleep(0.15)

            # 1. X11 XKB Sticky Keys via dtstyle -apply-accessibility
            subprocess.run(
                [
                    str(dtstyle_bin),
                    "-apply-accessibility",
                    "sticky=1,two_keys_off=0,slow=0,bounce=0,vbell=screen,reduced_motion=1",
                ],
                env=env,
                check=True,
            )
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("sticky")
                == "1",
                timeout=5.0,
                desc="X11 sticky=1 state",
            )
            subprocess.run([str(x11_helper_bin), "client-clear"], env=env, check=True)
            time.sleep(0.1)

            subprocess.run([str(x11_helper_bin), "tap", "Shift_L", "35"], env=env, check=True)
            wait_until(
                lambda: int(
                    parse_state_tokens(
                        subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                    ).get("latched", "0")
                )
                != 0,
                timeout=3.0,
                desc="X11 XKB Sticky Keys Shift latched",
            )
            subprocess.run([str(x11_helper_bin), "tap", "a", "35"], env=env, check=True)
            subprocess.run([str(x11_helper_bin), "tap", "b", "35"], env=env, check=True)
            wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "client-text"], env=env, text=True
                )
                == "Ab",
                timeout=4.0,
                desc="X11 XKB Sticky Keys latch ('Ab')",
            )

            subprocess.run([str(x11_helper_bin), "tap", "Shift_L", "35"], env=env, check=True)
            subprocess.run([str(x11_helper_bin), "tap", "Shift_L", "35"], env=env, check=True)
            wait_until(
                lambda: int(
                    parse_state_tokens(
                        subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                    ).get("locked", "0")
                )
                != 0,
                timeout=3.0,
                desc="X11 XKB Sticky Keys Shift locked",
            )
            subprocess.run([str(x11_helper_bin), "tap", "c", "35"], env=env, check=True)
            subprocess.run([str(x11_helper_bin), "tap", "d", "35"], env=env, check=True)
            subprocess.run([str(x11_helper_bin), "tap", "Shift_L", "35"], env=env, check=True)
            subprocess.run([str(x11_helper_bin), "tap", "e", "35"], env=env, check=True)
            wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "client-text"], env=env, text=True
                )
                == "AbCDe",
                timeout=4.0,
                desc="X11 XKB Sticky Keys lock+unlock ('AbCDe')",
            )

            # 2. X11 XKB Slow Keys (slow_ms=260)
            subprocess.run(
                [str(x11_helper_bin), "send-req", "f.set_accessibility sticky=0 slow=1 slow_ms=260 bounce=0"],
                env=env,
                check=True,
            )
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("slow")
                == "1",
                timeout=4.0,
                desc="X11 slow=1",
            )
            subprocess.run([str(x11_helper_bin), "client-clear"], env=env, check=True)
            time.sleep(0.1)
            subprocess.run([str(x11_helper_bin), "tap", "s", "50"], env=env, check=True)
            time.sleep(0.32)
            txt_short = subprocess.check_output(
                [str(x11_helper_bin), "client-text"], env=env, text=True
            )
            if txt_short != "":
                raise RuntimeError(f"X11 Slow Keys failed to reject 50ms tap: got {txt_short!r}")
            subprocess.run([str(x11_helper_bin), "tap", "s", "360"], env=env, check=True)
            wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "client-text"], env=env, text=True
                )
                == "s",
                timeout=4.0,
                desc="X11 Slow Keys 360ms hold accepted ('s')",
            )

            # 3. X11 XKB Bounce Keys (bounce_ms=260)
            subprocess.run(
                [str(x11_helper_bin), "send-req", "f.set_accessibility slow=0 bounce=1 bounce_ms=260"],
                env=env,
                check=True,
            )
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("bounce")
                == "1",
                timeout=4.0,
                desc="X11 bounce=1",
            )
            subprocess.run([str(x11_helper_bin), "client-clear"], env=env, check=True)
            time.sleep(0.1)
            subprocess.run([str(x11_helper_bin), "bounce-tap", "k", "35", "45"], env=env, check=True)
            time.sleep(0.32)
            txt_bounce = subprocess.check_output(
                [str(x11_helper_bin), "client-text"], env=env, text=True
            )
            if txt_bounce != "k":
                raise RuntimeError(f"X11 Bounce Keys expected single 'k', got {txt_bounce!r}")
            subprocess.run([str(x11_helper_bin), "tap", "k", "35"], env=env, check=True)
            wait_until(
                lambda: subprocess.check_output(
                    [str(x11_helper_bin), "client-text"], env=env, text=True
                )
                == "kk",
                timeout=4.0,
                desc="X11 Bounce Keys post-interval tap accepted ('kk')",
            )
            subprocess.run(
                [str(x11_helper_bin), "send-req", "f.set_accessibility bounce=0"],
                env=env,
                check=True,
            )

            # 4. X11 Visual Bell (XkbBellNotify -> override_redirect flash window)
            subprocess.run([str(x11_helper_bin), "ring-bell"], env=env, check=True)
            vbell_ppm = work / "x11-vbell.ppm"
            subprocess.run([str(x11_helper_bin), "screendump", str(vbell_ppm)], env=env, check=True)
            flash_px = count_vbell_flash_pixels(vbell_ppm)
            wait_until(
                lambda: int(
                    parse_state_tokens(
                        subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                    ).get("vbell_count", "0")
                )
                >= 1,
                timeout=4.0,
                desc="X11 XkbBellNotify incremented vbell_count",
            )
            if flash_px < 1000:
                raise RuntimeError(f"X11 Visual Bell flash not visible ({flash_px} px)")
            time.sleep(0.22)

            # 5. X11 Screen Magnification (Super+equal x4 -> ck-magnifier 200%, Super+minus -> 175%, Super+0 -> 100%)
            marker_xy = subprocess.check_output(
                [str(x11_helper_bin), "marker-center"], env=env, text=True
            ).strip()
            mx_s, my_s = marker_xy.split()
            mx, my = int(mx_s), int(my_s)
            subprocess.run([str(x11_helper_bin), "warp", str(mx), str(my)], env=env, check=True)
            time.sleep(0.1)
            mag100_ppm = work / "x11-mag-100.ppm"
            subprocess.run([str(x11_helper_bin), "screendump", str(mag100_ppm)], env=env, check=True)
            w100, h100 = measure_magenta_bbox(mag100_ppm)
            if w100 < 30 or h100 < 30:
                raise RuntimeError(f"X11 100% marker bbox unexpected: {(w100, h100)}")

            for expected_zoom in ("125", "150", "175", "200"):
                subprocess.run([str(x11_helper_bin), "chord", "Super_L", "equal"], env=env, check=True)
                wait_until(
                    lambda ez=expected_zoom: parse_state_tokens(
                        subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                    ).get("zoom_pct")
                    == ez,
                    timeout=5.0,
                    desc=f"X11 Super+equal zoom_pct={expected_zoom}",
                )
            time.sleep(0.15)
            mag200_ppm = work / "x11-mag-200.ppm"
            subprocess.run([str(x11_helper_bin), "screendump", str(mag200_ppm)], env=env, check=True)
            w200, h200 = measure_magenta_bbox(mag200_ppm)
            if w200 < int(w100 * 1.75) or h200 < int(h100 * 1.75):
                raise RuntimeError(
                    f"X11 ck-magnifier 200% marker did not scale ~2x: 100%={(w100, h100)}, 200%={(w200, h200)}"
                )

            subprocess.run([str(x11_helper_bin), "chord", "Super_L", "minus"], env=env, check=True)
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("zoom_pct")
                == "175",
                timeout=5.0,
                desc="X11 Super+minus zoom_pct=175",
            )
            subprocess.run([str(x11_helper_bin), "chord", "Super_L", "0"], env=env, check=True)
            wait_until(
                lambda: parse_state_tokens(
                    subprocess.check_output([str(x11_helper_bin), "state"], env=env, text=True)
                ).get("zoom_pct")
                == "100",
                timeout=5.0,
                desc="X11 Super+0 zoom_pct=100",
            )

            # 6. Motif AT-SPI2 Auto-Bridge on dtstyle + ck-tasks (CkTable)
            atspi_summary = verify_atspi_apps(
                "X11",
                env,
                atspi_addr,
                atspi_state,
                dtstyle_bin,
                ck_tasks_bin,
                motif_prefix,
                procs,
            )

            return (
                f"X11 OK ({disp}, XKB Sticky/Slow/Bounce, vbell flash={flash_px}px, "
                f"ck-magnifier 100%={w100}x{h100}->200%={w200}x{h200}->100%, {atspi_summary})"
            )
        finally:
            terminate_procs(procs)


def resolve_motif_prefix(comp_bin: Path) -> Path | None:
    env_pfx = os.environ.get("MOTIF_PREFIX")
    if env_pfx and Path(env_pfx).is_dir():
        return Path(env_pfx).resolve()
    wp20_cand = comp_bin.parents[3] / "motif-prefix"
    if (wp20_cand / "lib/libXm.so.9").exists():
        return wp20_cand.resolve()
    return None


def resolve_apps_bin_dir(comp_bin: Path) -> Path:
    env_dir = os.environ.get("CK_APPS_BIN_DIR")
    if env_dir and Path(env_dir).is_dir():
        return Path(env_dir).resolve()
    wp20_cand = comp_bin.parents[3] / "apps-src/build/bin"
    if (wp20_cand / "ck-magnifier").is_file():
        return wp20_cand.resolve()
    wp00_cand = Path("/var/tmp/ck-wayland-build/wp00/apps-src/build/bin")
    if wp00_cand.is_dir():
        return wp00_cand
    return Path("/usr/dt/bin")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True, type=Path)
    args = ap.parse_args()

    comp_bin = args.compositor.resolve()
    cde_build_root = comp_bin.parents[2]
    dtwm_x11_bin = comp_bin.parent / ".libs/dtwm"
    if not dtwm_x11_bin.is_file():
        dtwm_x11_bin = comp_bin.parent / "dtwm"

    dtstyle_bin = cde_build_root / "programs/dtstyle/.libs/dtstyle"
    if not dtstyle_bin.is_file():
        dtstyle_bin = Path("/usr/dt/bin/dtstyle")

    motif_prefix = resolve_motif_prefix(comp_bin)
    apps_bin_dir = resolve_apps_bin_dir(comp_bin)
    ck_tasks_bin = apps_bin_dir / "ck-tasks"
    if not ck_tasks_bin.is_file():
        ck_tasks_bin = Path(shutil.which("ck-tasks") or "/usr/dt/bin/ck-tasks")
    ck_magnifier_bin = apps_bin_dir / "ck-magnifier"
    if not ck_magnifier_bin.is_file():
        ck_magnifier_bin = Path(shutil.which("ck-magnifier") or "/usr/dt/bin/ck-magnifier")

    cc = os.environ.get("CC", "cc")
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp20-fx-", dir=tmp_base) as fx_s:
        fx = Path(fx_s)
        wl_c = fx / "wl_input_helper.c"
        wl_bin = fx / "wl_input_helper"
        wl_c.write_text(WL_INPUT_HELPER_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", "-Wall", str(wl_c), "-lwayland-client", "-lxkbcommon", "-o", str(wl_bin)],
            check=True,
        )

        x11_c = fx / "x11_wp20_helper.c"
        x11_bin = fx / "x11_wp20_helper"
        x11_c.write_text(X11_WP20_HELPER_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", "-Wall", str(x11_c), "-lX11", "-ldl", "-o", str(x11_bin)],
            check=True,
        )

        mc_c = fx / "motif_a11y_client.c"
        mc_bin = fx / "motif_a11y_client"
        mc_c.write_text(MOTIF_A11Y_CLIENT_C, encoding="utf-8")
        mc_cmd = [cc, "-O2", "-Wall"]
        if motif_prefix is not None:
            mc_cmd += [
                f"-I{motif_prefix / 'include'}",
                f"-L{motif_prefix / 'lib'}",
                f"-Wl,-rpath,{motif_prefix / 'lib'}",
            ]
        mc_cmd += [str(mc_c), "-lXm", "-lXt", "-lX11", "-o", str(mc_bin)]
        subprocess.run(mc_cmd, check=True)

        atspi_py = fx / "atspi_mock_bus.py"
        atspi_py.write_text(ATSPI_MOCK_BUS_PY, encoding="utf-8")

        t0 = time.monotonic()
        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_wl = pool.submit(
                run_wayland_lane,
                comp_bin,
                dtstyle_bin,
                ck_tasks_bin,
                wl_bin,
                x11_bin,
                mc_bin,
                atspi_py,
                motif_prefix,
                cde_build_root,
                apps_bin_dir,
            )
            fut_x11 = pool.submit(
                run_x11_lane,
                dtwm_x11_bin,
                dtstyle_bin,
                ck_tasks_bin,
                ck_magnifier_bin,
                x11_bin,
                mc_bin,
                atspi_py,
                motif_prefix,
                cde_build_root,
                apps_bin_dir,
            )
            wl_res = fut_wl.result()
            x11_res = fut_x11.result()
        elapsed = time.monotonic() - t0
        print(f"[WP-20 PASS in {elapsed:.2f}s]")
        print(f"  - {wl_res}")
        print(f"  - {x11_res}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
