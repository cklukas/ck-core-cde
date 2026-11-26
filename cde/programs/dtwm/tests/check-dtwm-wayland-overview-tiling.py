#!/usr/bin/env python3
"""Automated integration test for Topic 1.3:
1. Linked Split-Tile Resizing on BOTH Wayland (`dtwm-wayland --no-xwayland`)
   and X11 (`dtwm` under `Xvfb`):
   - Tiling two windows side-by-side (`f.tile_window left` / `right` on Wayland,
     `Super+Left` / `Super+Right` on X11) and dragging their shared vertical
     seam resizes both adjacent tiled windows together in lockstep without
     gaps or overlap.
   - Dragging past a neighbor's `min_width` clamps the split divider cleanly
     so neither window crushes its tiled peer.
2. Live Spatial Window Overview on `dtwm-wayland`:
   - Opening via 3-finger touchpad swipe-up (`f.touchpad_swipe 3 0 -140`),
     bare `Super` key tap (`zwp_virtual_keyboard_v1`), and `f.toggle_overview`.
   - Verifying `_DT_WAYLAND_OVERVIEW_STATE` card layout & workspace switcher pills.
   - Verifying scaled live client thumbnails in `grim` output, including live
     thumbnail pixel updates while the overview overlay remains open.
   - Clicking workspace pills to switch workspaces inside overview, clicking a
     background window's thumbnail card to raise/focus it and dismiss overview,
     and dismissing overview with `Escape`.
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


MOTIF_TILE_CLIENT_C = r"""
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <Xm/DrawingA.h>
#include <Xm/Wayland.h>
#include <Xm/Xm.h>

static Widget g_top = NULL;
static Widget g_canvas = NULL;
static const char *g_state_path = NULL;
static const char *g_cmd_path = NULL;
static unsigned long g_bg_rgb = 0xd92626UL;

static Pixel alloc_rgb(Display *dpy, unsigned long rgb) {
    XColor xc;
    memset(&xc, 0, sizeof(xc));
    xc.red = (unsigned short)(((rgb >> 16) & 0xffUL) * 257UL);
    xc.green = (unsigned short)(((rgb >> 8) & 0xffUL) * 257UL);
    xc.blue = (unsigned short)((rgb & 0xffUL) * 257UL);
    xc.flags = DoRed | DoGreen | DoBlue;
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    if (XAllocColor(dpy, cmap, &xc)) {
        return xc.pixel;
    }
    return BlackPixel(dpy, DefaultScreen(dpy));
}

static void repaint_canvas(void) {
    if (!g_canvas || !XtIsRealized(g_canvas)) return;
    Display *dpy = XtDisplay(g_canvas);
    Window win = XtWindow(g_canvas);
    Dimension w = 0, h = 0;
    XtVaGetValues(g_canvas, XmNwidth, &w, XmNheight, &h, NULL);
    if (w == 0 || h == 0) return;
    Pixel px = alloc_rgb(dpy, g_bg_rgb);
    XSetWindowBackground(dpy, win, px);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, px);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned int)w, (unsigned int)h);
    XFreeGC(dpy, gc);
    XFlush(dpy);
}

static void write_state(void) {
    if (!g_state_path || !g_top) return;
    Dimension w = 0, h = 0;
    Position x = 0, y = 0;
    XtVaGetValues(g_top, XmNx, &x, XmNy, &y, XmNwidth, &w, XmNheight, &h, NULL);
    Display *dpy = XtDisplay(g_top);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "WAYLAND=%d\nW=%u\nH=%u\nRGB=%06lx\n",
            (dpy && XmWaylandIsActive(dpy)) ? 1 : 0,
            (unsigned)w, (unsigned)h, g_bg_rgb);
    fclose(f);
    rename(tmp, g_state_path);
}

static void on_expose_or_resize(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    repaint_canvas();
    write_state();
}

static void on_cfg_event(Widget w, XtPointer cd, XEvent *ev, Boolean *cont) {
    (void)w; (void)cd; (void)cont;
    if (ev && (ev->type == ConfigureNotify || ev->type == Expose)) {
        repaint_canvas();
        write_state();
    }
}

static void on_poll(XtPointer cd, XtIntervalId *id) {
    (void)id;
    XtAppContext app = (XtAppContext)cd;
    if (g_cmd_path && access(g_cmd_path, R_OK) == 0) {
        FILE *f = fopen(g_cmd_path, "r");
        char line[128] = {0};
        if (f) {
            if (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = '\0';
            }
            fclose(f);
        }
        unlink(g_cmd_path);
        if (strncmp(line, "color ", 6) == 0) {
            g_bg_rgb = strtoul(line + 6, NULL, 16) & 0xffffffUL;
            repaint_canvas();
        } else if (strcmp(line, "quit") == 0) {
            XtAppSetExitFlag(app);
            return;
        }
    }
    write_state();
    XtAppAddTimeOut(app, 30, on_poll, app);
}

int main(int argc, char **argv) {
    const char *title = "TileWin";
    int min_w = 120;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--state") && i + 1 < argc) g_state_path = argv[++i];
        else if (!strcmp(argv[i], "--cmd") && i + 1 < argc) g_cmd_path = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--color") && i + 1 < argc) g_bg_rgb = strtoul(argv[++i], NULL, 16) & 0xffffffUL;
        else if (!strcmp(argv[i], "--min-w") && i + 1 < argc) min_w = atoi(argv[++i]);
    }
    XtAppContext app;
    g_top = XtVaAppInitialize(&app, "MotifTileClient", NULL, 0, &argc, argv, NULL,
                              XmNtitle, title,
                              XmNwidth, 440,
                              XmNheight, 360,
                              XmNminWidth, min_w,
                              XmNminHeight, 120,
                              NULL);
    Display *dpy = XtDisplay(g_top);
    Pixel px = alloc_rgb(dpy, g_bg_rgb);
    g_canvas = XtVaCreateManagedWidget("canvas", xmDrawingAreaWidgetClass, g_top,
                                       XmNwidth, 440,
                                       XmNheight, 360,
                                       XmNbackground, px,
                                       NULL);
    XtAddCallback(g_canvas, XmNexposeCallback, on_expose_or_resize, NULL);
    XtAddCallback(g_canvas, XmNresizeCallback, on_expose_or_resize, NULL);
    XtAddEventHandler(g_top, StructureNotifyMask | ExposureMask, False, on_cfg_event, NULL);
    XtRealizeWidget(g_top);
    repaint_canvas();
    write_state();
    XtAppAddTimeOut(app, 30, on_poll, app);
    XtAppMainLoop(app);
    return 0;
}
"""


WL_VKB_HELPER_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

extern const struct wl_interface zwp_virtual_keyboard_v1_interface;

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

typedef struct {
    struct wl_seat *seat;
    struct wl_proxy *mgr;
} VkbState;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t ver) {
    VkbState *st = (VkbState *)data;
    (void)ver;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !st->seat) {
        st->seat = (struct wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, "zwp_virtual_keyboard_manager_v1") == 0 && !st->mgr) {
        st->mgr = (struct wl_proxy *)wl_registry_bind(
            reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
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

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    uint32_t evdev_code = (uint32_t)strtoul(argv[1], NULL, 0);
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 2;
    VkbState st = { NULL, NULL };
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, &st);
    wl_display_roundtrip(dpy);
    if (!st.seat || !st.mgr) return 3;

    struct wl_proxy *vkb = wl_proxy_marshal_flags(
        st.mgr, 0, &zwp_virtual_keyboard_v1_interface, 1, 0, st.seat, NULL);
    if (!vkb) return 4;

    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
    struct xkb_keymap *kmap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *kmap_str = xkb_keymap_get_as_string(kmap, XKB_KEYMAP_FORMAT_TEXT_V1);
    uint32_t kmap_sz = (uint32_t)strlen(kmap_str) + 1u;
    int kfd = memfd_create("ov-xkb", MFD_CLOEXEC);
    if (kfd < 0 || ftruncate(kfd, (off_t)kmap_sz) != 0) return 5;
    void *ptr = mmap(NULL, kmap_sz, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, 0);
    memcpy(ptr, kmap_str, kmap_sz);
    munmap(ptr, kmap_sz);

    wl_proxy_marshal_flags(vkb, 0, NULL, 1, 0, 1u, kfd, kmap_sz);
    wl_display_roundtrip(dpy);
    close(kfd);
    free(kmap_str);
    xkb_keymap_unref(kmap);
    xkb_context_unref(ctx);

    if (evdev_code == 125u) {
        wl_proxy_marshal_flags(vkb, 2, NULL, 1, 0, 64u, 0u, 0u, 0u);
    }
    wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), evdev_code, 1u);
    wl_display_roundtrip(dpy);
    usleep(40000);
    if (evdev_code == 125u) {
        wl_proxy_marshal_flags(vkb, 2, NULL, 1, 0, 0u, 0u, 0u, 0u);
    }
    wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), evdev_code, 0u);
    wl_display_roundtrip(dpy);

    wl_proxy_marshal_flags(vkb, 3, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
    wl_display_flush(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
"""


X11_SPLIT_HELPER_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

static Window find_by_client_id(Display *dpy, Window w, Atom cid_atom, const char *want, int depth) {
    if (depth > 6) return None;
    Atom act = None;
    int fmt = 0;
    unsigned long n = 0, rem = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, cid_atom, 0, 64, False, AnyPropertyType,
                           &act, &fmt, &n, &rem, &data) == Success && data) {
        int match = (fmt == 8 && n == strlen(want) && memcmp(data, want, n) == 0);
        XFree(data);
        if (match) return w;
    }
    Window r = None, p = None, *kids = NULL;
    unsigned int nkids = 0;
    if (!XQueryTree(dpy, w, &r, &p, &kids, &nkids) || !kids) return None;
    Window hit = None;
    for (unsigned int i = 0; i < nkids; ++i) {
        hit = find_by_client_id(dpy, kids[i], cid_atom, want, depth + 1);
        if (hit != None) break;
    }
    XFree(kids);
    return hit;
}

static Window top_frame_of(Display *dpy, Window root, Window client) {
    Window cur = client;
    for (int i = 0; i < 6; ++i) {
        Window r = None, p = None, *kids = NULL;
        unsigned int n = 0;
        if (!XQueryTree(dpy, cur, &r, &p, &kids, &n)) break;
        if (kids) XFree(kids);
        if (p == root || p == None) return cur;
        cur = p;
    }
    return cur;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 2;
    Window root = DefaultRootWindow(dpy);

    if (!strcmp(argv[1], "wm_ready")) {
        Atom a = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", True);
        if (a == None) { XCloseDisplay(dpy); return 4; }
        Atom actual = None;
        int fmt = 0;
        unsigned long n = 0, after = 0;
        unsigned char *prop = NULL;
        int rc = XGetWindowProperty(dpy, root, a, 0, 4, False, AnyPropertyType,
                                    &actual, &fmt, &n, &after, &prop);
        if (prop) XFree(prop);
        XCloseDisplay(dpy);
        return (rc == Success && actual != None && n >= 1) ? 0 : 5;
    } else if (!strcmp(argv[1], "client") && argc >= 4) {
        const char *title = argv[2];
        int min_w = atoi(argv[3]);
        int w_inc = (argc >= 5) ? atoi(argv[4]) : 0;
        int max_w = (argc >= 6) ? atoi(argv[5]) : 0;
        int init_x = (strstr(title, "Right") != NULL) ? 560 : 60;
        int init_y = 90, init_w = 420, init_h = 340;
        Window w = XCreateSimpleWindow(dpy, root, init_x, init_y, init_w, init_h, 0, 0, 0x336699UL);
        XStoreName(dpy, w, title);
        Atom cid = XInternAtom(dpy, "_DT_TEST_CLIENT_ID", False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        XChangeProperty(dpy, w, cid, utf8, 8, PropModeReplace,
                        (const unsigned char *)title, (int)strlen(title));
        XSizeHints sh;
        memset(&sh, 0, sizeof(sh));
        sh.flags = USPosition | USSize | PPosition | PSize | PMinSize;
        sh.x = init_x;
        sh.y = init_y;
        sh.width = init_w;
        sh.height = init_h;
        sh.min_width = min_w;
        sh.min_height = 100;
        if (w_inc > 1) {
            sh.flags |= PResizeInc | PBaseSize;
            sh.width_inc = w_inc;
            sh.height_inc = 1;
            sh.base_width = 5;
            sh.base_height = 0;
        }
        if (max_w > 0) {
            sh.flags |= PMaxSize;
            sh.max_width = max_w;
            sh.max_height = 2000;
        }
        XSetWMNormalHints(dpy, w, &sh);
        XWMHints wh;
        memset(&wh, 0, sizeof(wh));
        wh.flags = InputHint;
        wh.input = True;
        XSetWMHints(dpy, w, &wh);
        XSelectInput(dpy, w, ExposureMask | StructureNotifyMask);
        XMapRaised(dpy, w);
        XFlush(dpy);
        for (;;) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }
    } else if (!strcmp(argv[1], "geom") && argc >= 3) {
        Atom cid = XInternAtom(dpy, "_DT_TEST_CLIENT_ID", False);
        Window cw = find_by_client_id(dpy, root, cid, argv[2], 0);
        if (cw == None) { XCloseDisplay(dpy); return 3; }
        Window fw = top_frame_of(dpy, root, cw);
        XWindowAttributes cwa, fwa;
        XGetWindowAttributes(dpy, cw, &cwa);
        XGetWindowAttributes(dpy, fw, &fwa);
        int cx = 0, cy = 0;
        Window ch = None;
        XTranslateCoordinates(dpy, cw, root, 0, 0, &cx, &cy, &ch);
        int ready = (fw != cw && cwa.map_state == IsViewable && fwa.map_state == IsViewable) ? 1 : 0;
        printf("FX=%d FY=%d FW=%d FH=%d CX=%d CY=%d CW=%d CH=%d READY=%d\n",
               fwa.x, fwa.y, fwa.width, fwa.height, cx, cy, cwa.width, cwa.height, ready);
    } else if (!strcmp(argv[1], "focus_and_tile") && argc >= 4) {
        Atom cid = XInternAtom(dpy, "_DT_TEST_CLIENT_ID", False);
        Window cw = find_by_client_id(dpy, root, cid, argv[2], 0);
        if (cw == None) { XCloseDisplay(dpy); return 3; }
        int cx = 0, cy = 0;
        Window ch = None;
        Window fw = top_frame_of(dpy, root, cw);
        XRaiseWindow(dpy, fw);
        XMapRaised(dpy, cw);
        XSync(dpy, False);
        usleep(50000);
        XSetInputFocus(dpy, cw, RevertToPointerRoot, CurrentTime);
        XTranslateCoordinates(dpy, cw, root, 40, 40, &cx, &cy, &ch);
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, cx, cy);
        XTestFakeMotionEvent(dpy, -1, cx, cy, CurrentTime);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        usleep(120000);

        KeyCode kc_sup = XKeysymToKeycode(dpy, XK_Super_L);
        KeySym dir_sym = !strcmp(argv[3], "left") ? XK_Left : XK_Right;
        KeyCode kc_dir = XKeysymToKeycode(dpy, dir_sym);
        XTestFakeKeyEvent(dpy, kc_sup, True, CurrentTime);
        XFlush(dpy);
        usleep(25000);
        XTestFakeKeyEvent(dpy, kc_dir, True, CurrentTime);
        XFlush(dpy);
        usleep(25000);
        XTestFakeKeyEvent(dpy, kc_dir, False, CurrentTime);
        XTestFakeKeyEvent(dpy, kc_sup, False, CurrentTime);
        XSync(dpy, False);
    } else if (!strcmp(argv[1], "drag") && argc >= 6) {
        int x1 = atoi(argv[2]), y1 = atoi(argv[3]);
        int x2 = atoi(argv[4]), y2 = atoi(argv[5]);
        XTestGrabControl(dpy, True);
        XTestFakeMotionEvent(dpy, -1, x1, y1, CurrentTime);
        XSync(dpy, False);
        usleep(60000);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        usleep(60000);
        int steps = 10;
        for (int i = 1; i <= steps; ++i) {
            int mx = x1 + ((x2 - x1) * i) / steps;
            int my = y1 + ((y2 - y1) * i) / steps;
            XTestFakeMotionEvent(dpy, -1, mx, my, CurrentTime);
            XSync(dpy, False);
            usleep(20000);
        }
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
    }
    XCloseDisplay(dpy);
    return 0;
}
"""


def wait_until(pred, timeout: float = 8.0, interval: float = 0.05, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_err = None
    while time.monotonic() < deadline:
        try:
            val = pred()
            if val:
                return val
        except Exception as exc:
            last_err = exc
        time.sleep(interval)
    raise RuntimeError(f"Timed out waiting for {desc} (last_err={last_err})")


def ipc_props_and_set(ipc_path: Path, set_atom: str | None = None, payload: bytes = b"") -> dict[str, bytes]:
    props: dict[str, bytes] = {}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(3.0)
        s.connect(str(ipc_path))
        buf = b""
        while b"READY\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        for line in buf.decode("utf-8", errors="replace").splitlines():
            if line.startswith("PROP "):
                parts = line.split(" ", 2)
                if len(parts) == 3:
                    props[parts[1]] = bytes.fromhex(parts[2])
        if set_atom is not None:
            s.sendall(f"SET {set_atom} {payload.hex()}\n".encode("ascii"))
            time.sleep(0.04)
    return props


def parse_active_box(props: dict[str, bytes]) -> tuple[int, ...]:
    raw = props.get("_DT_ACTIVE_WINDOW_BOX", b"")
    if len(raw) < 32:
        return ()
    return struct.unpack("<8i", raw[:32])


def parse_overview_state(props: dict[str, bytes]) -> dict:
    raw = props.get("_DT_WAYLAND_OVERVIEW_STATE", b"").decode("utf-8", errors="replace").strip("\x00")
    out: dict = {"raw": raw, "cards": [], "visible": 0, "count": 0, "selected": 0, "ws": 0}
    for part in raw.split(";"):
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        if k in ("visible", "count", "selected", "ws"):
            out[k] = int(v)
        elif k == "cards" and v:
            for item in v.split("|"):
                if "@" not in item or ":" not in item:
                    continue
                idx_title, coords = item.split("@", 1)
                idx_s, title = idx_title.split(":", 1)
                cx, cy, cw, ch = [int(n) for n in coords.split(",")]
                out["cards"].append({"idx": int(idx_s), "title": title, "box": (cx, cy, cw, ch)})
    return out


def parse_kv_file(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    out: dict[str, str] = {}
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def sample_ppm_avg(ppm_bytes: bytes, x: int, y: int, rw: int = 8, rh: int = 8) -> tuple[float, float, float]:
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
        while idx < n and ppm_bytes[idx] not in b" \t\r\n":
            idx += 1
        if start < idx:
            tokens.append(ppm_bytes[start:idx])
    idx += 1  # single whitespace byte after maxval
    width = int(tokens[1])
    height = int(tokens[2])
    pixels = ppm_bytes[idx:]
    rs = gs = bs = cnt = 0
    for py in range(max(0, y), min(height, y + rh)):
        for px in range(max(0, x), min(width, x + rw)):
            off = (py * width + px) * 3
            if off + 3 <= len(pixels):
                rs += pixels[off]
                gs += pixels[off + 1]
                bs += pixels[off + 2]
                cnt += 1
    if cnt == 0:
        return (0.0, 0.0, 0.0)
    return (rs / cnt, gs / cnt, bs / cnt)


def run_ptr_script(ptr_bin: Path, env: dict[str, str], lines: list[str]) -> None:
    payload = "\n".join(lines + ["quit", ""])
    subprocess.run([str(ptr_bin)], input=payload, text=True, env=env, check=True, timeout=8.0)


def verify_motif_lib(pid: int) -> None:
    maps = Path(f"/proc/{pid}/maps").read_text(encoding="utf-8", errors="replace")
    if "libXm.so.9" not in maps:
        raise RuntimeError(f"PID {pid} did not map CK Core Motif ABI 9 libXm.so.9")


def resolve_real_elf(p: Path) -> Path:
    cand = p.parent / ".libs" / p.name
    if cand.is_file():
        return cand
    return p


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", default="./dtwm-wayland")
    ap.add_argument("--dtwm", default="./dtwm")
    args = ap.parse_args()

    comp_bin = resolve_real_elf(Path(args.compositor).resolve())
    dtwm_bin = resolve_real_elf(Path(args.dtwm).resolve())
    tests_dir = Path(__file__).resolve().parent
    ptr_c = tests_dir / "wayland-test-pointer.c"
    if not comp_bin.is_file():
        raise RuntimeError(f"Missing dtwm-wayland binary: {comp_bin}")

    cc = os.environ.get("CC", "cc")
    motif_prefix = Path(os.environ.get("MOTIF_PREFIX", "/var/tmp/ck-wayland-build/wp00/motif-prefix"))
    if not (motif_prefix / "include/Xm/Wayland.h").is_file():
        for cand in (Path("/usr/local"), Path("/usr")):
            if (cand / "include/Xm/Wayland.h").is_file():
                motif_prefix = cand
                break

    tmp_parent = os.environ.get("TMPDIR") or "/var/tmp"
    with tempfile.TemporaryDirectory(prefix="dtwm-ov-tile-", dir=tmp_parent) as td:
        work = Path(td)
        rt_dir = work / "xdg"
        home_dir = work / "home"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt").mkdir(parents=True)

        # Compile helpers
        ptr_bin = work / "wayland-test-pointer"
        vkb_bin = work / "wl-vkb-helper"
        tile_client_bin = work / "motif-tile-client"
        x11_helper_bin = work / "x11-split-helper"

        wl_cflags = subprocess.check_output(["pkg-config", "--cflags", "wayland-client"], text=True).split()
        wl_libs = subprocess.check_output(["pkg-config", "--libs", "wayland-client"], text=True).split()
        xkb_cflags = subprocess.check_output(["pkg-config", "--cflags", "xkbcommon"], text=True).split()
        xkb_libs = subprocess.check_output(["pkg-config", "--libs", "xkbcommon"], text=True).split()

        subprocess.run([cc, "-O2", str(ptr_c), "-o", str(ptr_bin), *wl_cflags, *wl_libs], check=True)
        vkb_c = work / "wl_vkb_helper.c"
        vkb_c.write_text(WL_VKB_HELPER_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", str(vkb_c), "-o", str(vkb_bin), *wl_cflags, *xkb_cflags, *wl_libs, *xkb_libs],
            check=True,
        )

        tile_c = work / "motif_tile_client.c"
        tile_c.write_text(MOTIF_TILE_CLIENT_C, encoding="utf-8")
        m_inc = f"-I{motif_prefix}/include"
        m_lib = [f"-L{motif_prefix}/lib", f"-Wl,-rpath,{motif_prefix}/lib", "-lXm", "-lXt", "-lX11"]
        subprocess.run(
            [cc, "-O2", str(tile_c), "-o", str(tile_client_bin), m_inc, *wl_cflags, *m_lib, *wl_libs],
            check=True,
        )

        x11_c = work / "x11_split_helper.c"
        x11_c.write_text(X11_SPLIT_HELPER_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", str(x11_c), "-o", str(x11_helper_bin), "-lX11", "-lXtst"],
            check=True,
        )

        procs: list[subprocess.Popen] = []
        try:
            # =================================================================
            # LANE 1: Wayland (`dtwm-wayland --no-xwayland --no-panel`)
            # =================================================================
            status_file = work / "wl.status"
            wl_env = os.environ.copy()
            wl_env.update({
                "XDG_RUNTIME_DIR": str(rt_dir),
                "HOME": str(home_dir),
                "WLR_BACKENDS": "headless",
                "WLR_RENDERER": "pixman",
                "WLR_HEADLESS_OUTPUTS": "1",
                "CK_MOTIF_BACKEND": "wayland",
                "LD_LIBRARY_PATH": f"{motif_prefix}/lib:" + wl_env.get("LD_LIBRARY_PATH", ""),
            })
            wl_env.pop("DISPLAY", None)

            comp = subprocess.Popen(
                [str(comp_bin), "--no-xwayland", "--no-panel", "--status-file", str(status_file)],
                env=wl_env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            procs.append(comp)
            wait_until(lambda: status_file.is_file() and status_file.read_text().strip(), desc="dtwm-wayland status")
            st_kv = parse_kv_file(status_file)
            wl_sock = st_kv["WAYLAND_DISPLAY"]
            wl_env["WAYLAND_DISPLAY"] = wl_sock
            ipc_path = rt_dir / f"dtwm-wayland-{wl_sock}.ipc"
            wait_until(lambda: ipc_path.exists(), desc="Wayland IPC socket")

            left_state = work / "left.state"
            left_cmd = work / "left.cmd"
            right_state = work / "right.state"
            right_cmd = work / "right.cmd"

            left_p = subprocess.Popen(
                [
                    str(tile_client_bin),
                    "--title", "TileLeftWin",
                    "--color", "d92626",
                    "--min-w", "300",
                    "--state", str(left_state),
                    "--cmd", str(left_cmd),
                ],
                env=wl_env,
            )
            procs.append(left_p)
            wait_until(
                lambda: len(parse_active_box(ipc_props_and_set(ipc_path))) == 8
                and parse_kv_file(left_state).get("WAYLAND") == "1",
                desc="TileLeftWin mapped on compositor",
            )
            verify_motif_lib(left_p.pid)

            ipc_props_and_set(ipc_path, "_DT_WM_REQUEST", b"f.tile_window left\x00")
            box_l0 = wait_until(
                lambda: b
                if len(b := parse_active_box(ipc_props_and_set(ipc_path))) == 8
                and b[0] == 0
                and 636 <= b[2] <= 644
                and int(parse_kv_file(left_state).get("W", "0")) >= 620
                else None,
                desc="TileLeftWin tiled left (640px)",
            )

            right_p = subprocess.Popen(
                [
                    str(tile_client_bin),
                    "--title", "TileRightWin",
                    "--color", "1f6feb",
                    "--min-w", "340",
                    "--state", str(right_state),
                    "--cmd", str(right_cmd),
                ],
                env=wl_env,
            )
            procs.append(right_p)
            wait_until(
                lambda: parse_kv_file(right_state).get("WAYLAND") == "1"
                and len(b := parse_active_box(ipc_props_and_set(ipc_path))) == 8
                and b[2] < 520,
                desc="TileRightWin mapped and focused on compositor",
            )
            verify_motif_lib(right_p.pid)

            ipc_props_and_set(ipc_path, "_DT_WM_REQUEST", b"f.tile_window right\x00")
            box_r0 = wait_until(
                lambda: b
                if len(b := parse_active_box(ipc_props_and_set(ipc_path))) == 8
                and 636 <= b[0] <= 644
                and 636 <= b[2] <= 644
                and int(parse_kv_file(right_state).get("W", "0")) >= 620
                else None,
                desc="TileRightWin tiled right (640px)",
            )

            # 1A. Drag TileLeftWin's right resize border from x=637 to x=777 (+140px)
            run_ptr_script(ptr_bin, wl_env, [
                "move 637 450 1280 900",
                "sleep 40",
                "down 272",
                "sleep 40",
                "move 685 450 1280 900",
                "sleep 30",
                "move 735 450 1280 900",
                "sleep 30",
                "move 777 450 1280 900",
                "sleep 50",
                "up 272",
                "sleep 60",
            ])

            left_kv1 = wait_until(
                lambda: kv if int((kv := parse_kv_file(left_state)).get("W", "0")) >= 760 else None,
                desc="TileLeftWin linked split-divider growth",
            )
            right_kv1 = wait_until(
                lambda: kv if 460 <= int((kv := parse_kv_file(right_state)).get("W", "0")) <= 505 else None,
                desc="TileRightWin linked split-divider shrink",
            )
            box_l1 = wait_until(
                lambda: b if len(b := parse_active_box(ipc_props_and_set(ipc_path))) == 8 and b[2] >= 770 else None,
                desc="TileLeftWin settled outer box",
            )
            # Focus right window by clicking inside it and verify seamless abutment
            run_ptr_script(ptr_bin, wl_env, [
                "move 1050 450 1280 900",
                "down 272",
                "up 272",
                "sleep 40",
            ])
            box_r1 = wait_until(
                lambda: b if len(b := parse_active_box(ipc_props_and_set(ipc_path))) == 8 and b[0] >= 770 else None,
                desc="TileRightWin shifted right outer x",
            )
            seam_delta = abs((box_l1[0] + box_l1[2]) - box_r1[0])
            if seam_delta > 2 or abs((box_l1[2] + box_r1[2]) - 1280) > 4:
                raise RuntimeError(f"Wayland split-tile seam mismatch: left={box_l1}, right={box_r1}")

            # 1B. Min-width clamping: drag TileLeftWin's right border from seam to x=1180;
            # TileRightWin has min_width=340, so right client width must clamp >= 340!
            seam_x = box_r1[0] - 2
            run_ptr_script(ptr_bin, wl_env, [
                f"move {seam_x} 450 1280 900",
                "sleep 40",
                "down 272",
                "sleep 40",
                "move 980 450 1280 900",
                "sleep 40",
                "move 1180 450 1280 900",
                "sleep 60",
                "up 272",
                "sleep 80",
            ])
            wait_until(
                lambda: 340 <= int(parse_kv_file(right_state).get("W", "0")) <= 380,
                desc="TileRightWin client width clamped at min_w=340",
            )
            right_w_clamped = int(parse_kv_file(right_state)["W"])

            # =================================================================
            # PART 2: Live Spatial Window Overview (`Super` / 3-finger swipe-up)
            # =================================================================
            # 2A. Open Overview via 3-finger touchpad swipe-up (`dy < 0`)
            ipc_props_and_set(ipc_path, "_DT_WM_REQUEST", b"f.touchpad_swipe 3 0 -140\x00")
            ov = wait_until(
                lambda: st if (st := parse_overview_state(ipc_props_and_set(ipc_path)))["visible"] == 1 and st["count"] == 2 else None,
                desc="Overview opened via 3-finger swipe-up",
            )
            cards_by_title = {c["title"]: c["box"] for c in ov["cards"]}
            if "TileLeftWin" not in cards_by_title or "TileRightWin" not in cards_by_title:
                raise RuntimeError(f"Missing overview cards: {ov}")

            # Sample live scaled thumbnails in grim PPM output
            ppm1 = subprocess.check_output(["grim", "-t", "ppm", "-"], env=wl_env, timeout=5.0)
            lx, ly, lw, lh = cards_by_title["TileLeftWin"]
            rx, ry, rw, rh = cards_by_title["TileRightWin"]
            l_rgb1 = sample_ppm_avg(ppm1, lx + lw // 2 - 4, ly + (lh - 28) // 2 - 4)
            r_rgb1 = sample_ppm_avg(ppm1, rx + rw // 2 - 4, ry + (rh - 28) // 2 - 4)
            if not (l_rgb1[0] > 160 and l_rgb1[1] < 80):
                raise RuntimeError(f"Expected red live thumbnail in TileLeftWin card, got RGB={l_rgb1}")
            if not (r_rgb1[2] > 160 and r_rgb1[0] < 80):
                raise RuntimeError(f"Expected blue live thumbnail in TileRightWin card, got RGB={r_rgb1}")

            # 2B. Live thumbnail update WHILE overview remains open: recolor TileLeftWin -> #22c55e (green)
            left_cmd.write_text("color 22c55e\n", encoding="utf-8")
            def thumb_turned_green():
                ppm_live = subprocess.check_output(["grim", "-t", "ppm", "-"], env=wl_env, timeout=5.0)
                rgb = sample_ppm_avg(ppm_live, lx + lw // 2 - 4, ly + (lh - 28) // 2 - 4)
                return rgb if (rgb[1] > 145 and rgb[0] < 80) else None
            l_rgb_green = wait_until(thumb_turned_green, desc="live thumbnail recolor while overview open")

            # 2C. Click Workspace 2 pill at top of overview -> switches to WS 1, then back to WS 0
            # Workspace 1 & 2 pills sit centered near y = 18 + 23 = 41 on 1280px width (4 workspaces)
            run_ptr_script(ptr_bin, wl_env, [
                "move 565 40 1280 900",
                "down 272",
                "up 272",
                "sleep 50",
            ])
            wait_until(
                lambda: parse_overview_state(ipc_props_and_set(ipc_path))["ws"] == 1,
                desc="Overview workspace pill switch to WS 1",
            )
            run_ptr_script(ptr_bin, wl_env, [
                "move 420 40 1280 900",
                "down 272",
                "up 272",
                "sleep 50",
            ])
            ov_ws0 = wait_until(
                lambda: st if (st := parse_overview_state(ipc_props_and_set(ipc_path)))["ws"] == 0 and st["count"] == 2 else None,
                desc="Overview workspace pill switch back to WS 0",
            )

            # 2D. Click TileRightWin's thumbnail card -> closes overview & activates TileRightWin
            rc_box = {c["title"]: c["box"] for c in ov_ws0["cards"]}["TileRightWin"]
            rcx = rc_box[0] + rc_box[2] // 2
            rcy = rc_box[1] + rc_box[3] // 2
            run_ptr_script(ptr_bin, wl_env, [
                f"move {rcx} {rcy} 1280 900",
                "down 272",
                "up 272",
                "sleep 50",
            ])
            wait_until(
                lambda: parse_overview_state(ipc_props_and_set(ipc_path))["visible"] == 0,
                desc="Overview closed on card click",
            )
            act_after_pick = parse_active_box(ipc_props_and_set(ipc_path))
            if len(act_after_pick) != 8 or act_after_pick[0] < 700:
                raise RuntimeError(f"Expected TileRightWin focused after card click, got {act_after_pick}")

            # 2E. Bare `Super_L` tap (evdev 125) opens overview, `Escape` (evdev 1) dismisses it
            subprocess.run([str(vkb_bin), "125"], env=wl_env, check=True)
            wait_until(
                lambda: parse_overview_state(ipc_props_and_set(ipc_path))["visible"] == 1,
                desc="Overview opened via bare Super_L tap",
            )
            subprocess.run([str(vkb_bin), "1"], env=wl_env, check=True)
            wait_until(
                lambda: parse_overview_state(ipc_props_and_set(ipc_path))["visible"] == 0,
                desc="Overview dismissed via Escape",
            )

            left_cmd.write_text("quit\n", encoding="utf-8")
            right_cmd.write_text("quit\n", encoding="utf-8")

            # =================================================================
            # LANE 2: Classic X11 `dtwm` under `Xvfb` — Linked Split-Tile Resize
            # =================================================================
            x11_summary = "skipped (no Xvfb)"
            if dtwm_bin.is_file() and shutil.which("Xvfb"):
                disp = ":94"
                for dnum in range(91, 119):
                    if not Path(f"/tmp/.X{dnum}-lock").exists():
                        disp = f":{dnum}"
                        break
                x_home = work / "xhome"
                (x_home / ".dt").mkdir(parents=True)
                dtwmrc = x_home / ".dt/dtwmrc"
                for cand_rc in (comp_bin.parent / "sys.dtwmrc", comp_bin.parent.parent / "sys.dtwmrc"):
                    if cand_rc.is_file():
                        shutil.copy2(cand_rc, dtwmrc)
                        break

                xvfb = subprocess.Popen(
                    ["Xvfb", disp, "-screen", "0", "1280x900x24", "-nolisten", "tcp", "-ac"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                procs.append(xvfb)
                cde_build_root = dtwm_bin.parent.parent.parent
                if not (cde_build_root / "programs").is_dir():
                    cde_build_root = dtwm_bin.parent.parent.parent.parent
                fs_prefix = os.environ.get("FS_PREFIX", "/var/tmp/ck-wayland-build/wp00/fs-prefix")
                x_ld = [
                    f"{fs_prefix}/lib",
                    f"{motif_prefix}/lib",
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
                x_rt = work / "xrt"
                for d in (x_rt, x_home / ".dt/types", x_home / ".dt/sessions", x_home / ".config", x_home / ".cache", x_home / ".local/share"):
                    d.mkdir(parents=True, exist_ok=True)
                os.chmod(x_rt, 0o700)
                x_env = os.environ.copy()
                x_env["DISPLAY"] = disp
                x_env["HOME"] = str(x_home)
                x_env["XDG_RUNTIME_DIR"] = str(x_rt)
                x_env["XDG_CONFIG_HOME"] = str(x_home / ".config")
                x_env["XDG_CACHE_HOME"] = str(x_home / ".cache")
                x_env["XDG_DATA_HOME"] = str(x_home / ".local/share")
                x_env["DTDATABASESEARCHPATH"] = f"{x_home / '.dt/types'},/usr/dt/appconfig/types/C"
                for k in ("WAYLAND_DISPLAY", "CK_MOTIF_BACKEND", "TT_SESSION", "DTUSERSESSION", "DBUS_SESSION_BUS_ADDRESS"):
                    x_env.pop(k, None)
                x_env["LANG"] = "C.UTF-8"
                x_env["LC_ALL"] = "C.UTF-8"
                x_env["LD_LIBRARY_PATH"] = ":".join(x_ld) + ":" + x_env.get("LD_LIBRARY_PATH", "")
                tt_dirs = [
                    str(cde_build_root / "programs/tt/bin/ttsession"),
                    "/var/tmp/ck-wayland-build/wp00/cde-stage/usr/dt/bin",
                    "/usr/dt/bin",
                ]
                x_env["PATH"] = ":".join(tt_dirs) + ":" + x_env.get("PATH", "/usr/bin:/bin")

                if shutil.which("dbus-daemon"):
                    dbus_addr_f = work / "xdbus.addr"
                    with dbus_addr_f.open("w", encoding="utf-8") as dfp:
                        dbus_p = subprocess.Popen(
                            ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                            env=x_env, stdout=dfp, stderr=subprocess.DEVNULL,
                        )
                    procs.append(dbus_p)
                    wait_until(
                        lambda: dbus_addr_f.is_file() and dbus_addr_f.read_text(encoding="utf-8").strip(),
                        desc="X11 dbus-daemon ready",
                    )
                    x_env["DBUS_SESSION_BUS_ADDRESS"] = dbus_addr_f.read_text(encoding="utf-8").splitlines()[0].strip()

                wait_until(
                    lambda: Path(f"/tmp/.X11-unix/X{disp[1:]}").exists(),
                    desc="Xvfb socket ready",
                )
                for tt_cand in (
                    cde_build_root / "lib/tt/bin/ttsession/.libs/ttsession",
                    Path("/var/tmp/ck-wayland-build/wp00/cde-stage/usr/dt/bin/ttsession"),
                    Path("/usr/dt/bin/ttsession"),
                ):
                    if tt_cand.is_file():
                        tt_p = subprocess.Popen(
                            [str(tt_cand), "-s"],
                            env=x_env,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                        procs.append(tt_p)
                        time.sleep(0.4)
                        break
                (x_home / ".Xdefaults").write_text(
                    "*fontList: fixed\n"
                    "Dtwm*minimizeAnimation: none\n",
                    encoding="utf-8",
                )
                dtwm_err = work / "dtwm_x11.stderr"
                dtwm_cmd = [str(dtwm_bin)]
                dtwm_p = subprocess.Popen(dtwm_cmd, env=x_env, stdout=subprocess.DEVNULL, stderr=dtwm_err.open("w"))
                procs.append(dtwm_p)
                wait_until(
                    lambda: subprocess.run([str(x11_helper_bin), "wm_ready"], env=x_env).returncode == 0,
                    timeout=10.0,
                    desc="X11 dtwm _NET_SUPPORTING_WM_CHECK ready",
                )
                verify_motif_lib(dtwm_p.pid)
                time.sleep(0.3)

                xl_p = subprocess.Popen([str(x11_helper_bin), "client", "X11SplitLeft", "300"], env=x_env)
                xr_p = subprocess.Popen([str(x11_helper_bin), "client", "X11SplitRight", "340"], env=x_env)
                procs.extend([xl_p, xr_p])

                def x11_geom(title: str) -> dict[str, int]:
                    out = subprocess.check_output([str(x11_helper_bin), "geom", title], env=x_env, text=True).strip()
                    return {kv.split("=")[0]: int(kv.split("=")[1]) for kv in out.split()}

                wait_until(lambda: x11_geom("X11SplitLeft")["READY"] == 1, desc="X11SplitLeft reparented & viewable")
                wait_until(lambda: x11_geom("X11SplitRight")["READY"] == 1, desc="X11SplitRight reparented & viewable")
                time.sleep(0.25)

                subprocess.run([str(x11_helper_bin), "focus_and_tile", "X11SplitLeft", "left"], env=x_env, check=True)
                gl0 = wait_until(
                    lambda: g if (g := x11_geom("X11SplitLeft"))["FX"] <= 8 and 620 <= g["FW"] <= 650 else None,
                    desc="X11SplitLeft Super+Left half tile",
                )
                subprocess.run([str(x11_helper_bin), "focus_and_tile", "X11SplitRight", "right"], env=x_env, check=True)
                gr0 = wait_until(
                    lambda: g if 630 <= (g := x11_geom("X11SplitRight"))["FX"] <= 650 and 620 <= g["FW"] <= 650 else None,
                    desc="X11SplitRight Super+Right half tile",
                )

                # Drag X11SplitLeft's right resize border (`FX + FW - 3`, y=350) +130px right
                seam0 = gl0["FX"] + gl0["FW"] - 3
                subprocess.run(
                    [str(x11_helper_bin), "drag", str(seam0), "350", str(seam0 + 130), "350"],
                    env=x_env,
                    check=True,
                )
                gl1 = wait_until(
                    lambda: g if (g := x11_geom("X11SplitLeft"))["FW"] >= 750 else None,
                    desc="X11SplitLeft linked split-divider growth",
                )
                gr1 = wait_until(
                    lambda: g if (g := x11_geom("X11SplitRight"))["FX"] >= 750 and g["FW"] <= 530 else None,
                    desc="X11SplitRight linked split-divider shrink",
                )
                if abs((gl1["FX"] + gl1["FW"]) - gr1["FX"]) > 4:
                    raise RuntimeError(f"X11 split seam mismatch: left={gl1}, right={gr1}")

                # Clamp drag past X11SplitRight's min_width=340
                seam1 = gl1["FX"] + gl1["FW"] - 3
                subprocess.run(
                    [str(x11_helper_bin), "drag", str(seam1), "350", "1180", "350"],
                    env=x_env,
                    check=True,
                )
                time.sleep(0.2)
                gr2 = x11_geom("X11SplitRight")
                if gr2["CW"] < 340 or gr2["CW"] > 390:
                    raise RuntimeError(f"Expected X11SplitRight CW clamped near 340, got {gr2}")

                # Verify WM_NORMAL_HINTS width_inc=20 (>12px snapped gap) + max_width=700 clamp
                xl_p.terminate()
                xr_p.terminate()
                time.sleep(0.2)
                gxl_p = subprocess.Popen([str(x11_helper_bin), "client", "X11GridLeft", "205", "20", "0"], env=x_env)
                gxr_p = subprocess.Popen([str(x11_helper_bin), "client", "X11GridRight", "220", "1", "700"], env=x_env)
                procs.extend([gxl_p, gxr_p])
                wait_until(lambda: x11_geom("X11GridLeft")["READY"] == 1, desc="X11GridLeft ready")
                wait_until(lambda: x11_geom("X11GridRight")["READY"] == 1, desc="X11GridRight ready")
                time.sleep(0.2)
                subprocess.run([str(x11_helper_bin), "focus_and_tile", "X11GridLeft", "left"], env=x_env, check=True)
                ggl0 = wait_until(
                    lambda: g if (g := x11_geom("X11GridLeft"))["FX"] <= 8 and 600 <= g["FW"] <= 650 else None,
                    desc="X11GridLeft Super+Left half tile (snapped to width_inc=20)",
                )
                subprocess.run([str(x11_helper_bin), "focus_and_tile", "X11GridRight", "right"], env=x_env, check=True)
                wait_until(
                    lambda: g if 630 <= (g := x11_geom("X11GridRight"))["FX"] <= 650 else None,
                    desc="X11GridRight Super+Right half tile",
                )
                # Drag X11GridLeft's right border FAR left (x=280) so X11GridRight tries to exceed max_width=700!
                gseam0 = ggl0["FX"] + ggl0["FW"] - 3
                subprocess.run(
                    [str(x11_helper_bin), "drag", str(gseam0), "350", "280", "350"],
                    env=x_env,
                    check=True,
                )
                time.sleep(0.25)
                ggl1 = x11_geom("X11GridLeft")
                ggr1 = x11_geom("X11GridRight")
                if (ggl1["CW"] - 5) % 20 != 0:
                    raise RuntimeError(f"Expected X11GridLeft CW on 5+20*k grid after clamp, got {ggl1}")
                if ggr1["CW"] > 700 or ggr1["CW"] < 660:
                    raise RuntimeError(f"Expected X11GridRight CW clamped to max_width=700, got {ggr1}")

                x11_summary = (
                    f"X11 split resize OK (FW {gl0['FW']}->{gl1['FW']} / {gr0['FW']}->{gr1['FW']}, "
                    f"minCW={gr2['CW']}, gridCW={ggl1['CW']}, maxCW={ggr1['CW']})"
                )

            print(
                f"PASS: Wayland split-resize (W={left_kv1['W']}/{right_kv1['W']}, clamped={right_w_clamped}), "
                f"Overview (swipe-up, live green={tuple(round(c,1) for c in l_rgb_green)}, ws-pill, card-pick, Super+Esc), "
                f"{x11_summary}"
            )
            return 0
        finally:
            for p in reversed(procs):
                with suppress(Exception):
                    if p.poll() is None:
                        p.terminate()
            for p in reversed(procs):
                with suppress(Exception):
                    p.wait(timeout=3.0)
            work_bytes = str(work).encode("utf-8")
            for proc_dir in Path("/proc").iterdir():
                if not proc_dir.name.isdigit():
                    continue
                with suppress(Exception):
                    pid = int(proc_dir.name)
                    if pid != os.getpid() and work_bytes in (proc_dir / "environ").read_bytes():
                        os.kill(pid, signal.SIGTERM)


if __name__ == "__main__":
    raise SystemExit(main())
