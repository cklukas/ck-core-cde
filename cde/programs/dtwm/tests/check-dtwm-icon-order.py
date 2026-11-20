#!/usr/bin/env python3
"""Automated integration test for invariant desktop & Icon Box icon ordering
across window minimize, restore, focus changes, and window close in both
Wayland (dtwm-wayland) and X11 (dtwm).
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image


X11_COLOR_CLIENT_C = r"""
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

extern const struct wl_interface zwlr_virtual_pointer_v1_interface;
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
    struct wl_proxy *vptr_mgr;
} WlDragState;

static void on_wl_global(void *data, struct wl_registry *reg, uint32_t name,
                         const char *iface, uint32_t ver) {
    WlDragState *st = (WlDragState *)data;
    (void)ver;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !st->seat) {
        st->seat = (struct wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, "zwlr_virtual_pointer_manager_v1") == 0 && !st->vptr_mgr) {
        st->vptr_mgr = (struct wl_proxy *)wl_registry_bind(
            reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}
static void on_wl_global_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener wl_reg_l = { on_wl_global, on_wl_global_rem };

static uint32_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

static int g_xerr = 0;
static int ignore_xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; g_xerr = 1; return 0; }
static Window find_iconbox_rec(Display *dpy, Window w, int depth) {
    if (depth > 4) return None;
    if (depth > 0) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable) return None;
    }
    XClassHint ch = {NULL, NULL};
    int match = 0;
    if (XGetClassHint(dpy, w, &ch)) {
        if (ch.res_name && strcasecmp(ch.res_name, "iconbox") == 0) match = 1;
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    if (!match) {
        char *wname = NULL;
        if (XFetchName(dpy, w, &wname) && wname) {
            if (strcmp(wname, "Icons") == 0) match = 1;
            XFree(wname);
        }
    }
    if (match) return w;
    Window r = None, p = None, *ch_list = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, w, &r, &p, &ch_list, &n) || !ch_list) return None;
    Window found = None;
    for (unsigned int i = 0; i < n && found == None; ++i) {
        found = find_iconbox_rec(dpy, ch_list[i], depth + 1);
    }
    XFree(ch_list);
    return found;
}
static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *mode = argv[1];

    if (strcmp(mode, "wl_drag") == 0 && argc >= 8) {
        uint32_t x0 = (uint32_t)strtoul(argv[2], NULL, 0);
        uint32_t y0 = (uint32_t)strtoul(argv[3], NULL, 0);
        uint32_t x1 = (uint32_t)strtoul(argv[4], NULL, 0);
        uint32_t y1 = (uint32_t)strtoul(argv[5], NULL, 0);
        uint32_t ow = (uint32_t)strtoul(argv[6], NULL, 0);
        uint32_t oh = (uint32_t)strtoul(argv[7], NULL, 0);
        const char *mid_shot = (argc > 8) ? argv[8] : NULL;

        struct wl_display *wdpy = wl_display_connect(NULL);
        if (!wdpy) return 3;
        WlDragState st = {0};
        struct wl_registry *reg = wl_display_get_registry(wdpy);
        wl_registry_add_listener(reg, &wl_reg_l, &st);
        wl_display_roundtrip(wdpy);
        if (!st.seat || !st.vptr_mgr) return 4;
        struct wl_proxy *vp = wl_proxy_marshal_flags(
            st.vptr_mgr, 0, &zwlr_virtual_pointer_v1_interface, 1, 0, st.seat, NULL);
        wl_display_roundtrip(wdpy);

        wl_proxy_marshal(vp, 1, mono_ms(), x0, y0, ow, oh);
        wl_proxy_marshal(vp, 4);
        wl_display_roundtrip(wdpy);
        usleep(30000);

        wl_proxy_marshal(vp, 2, mono_ms(), (uint32_t)BTN_LEFT, 1u);
        wl_proxy_marshal(vp, 4);
        wl_display_roundtrip(wdpy);
        usleep(30000);

        wl_proxy_marshal(vp, 1, mono_ms(), (x0 + x1) / 2u, (y0 + y1) / 2u, ow, oh);
        wl_proxy_marshal(vp, 4);
        wl_display_roundtrip(wdpy);
        usleep(25000);

        wl_proxy_marshal(vp, 1, mono_ms(), x1, y1, ow, oh);
        wl_proxy_marshal(vp, 4);
        wl_display_roundtrip(wdpy);
        usleep(40000);

        if (mid_shot && mid_shot[0]) {
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "grim '%s'", mid_shot);
            if (system(cmd) != 0) {}
        }

        wl_proxy_marshal(vp, 2, mono_ms(), (uint32_t)BTN_LEFT, 0u);
        wl_proxy_marshal(vp, 4);
        wl_display_roundtrip(wdpy);
        usleep(30000);

        wl_proxy_marshal(vp, 8);
        wl_display_disconnect(wdpy);
        return 0;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);

    if (strcmp(mode, "client") == 0) {
        /* client <title> <hex_rgb> <x> <y> */
        const char *title = (argc > 2) ? argv[2] : "IconOrderWin";
        unsigned long rgb = (argc > 3) ? strtoul(argv[3], NULL, 16) : 0xE02828UL;
        int wx = (argc > 4) ? atoi(argv[4]) : 220;
        int wy = (argc > 5) ? atoi(argv[5]) : 120;

        signal(SIGTERM, on_sig);
        signal(SIGINT, on_sig);

        Window win = XCreateSimpleWindow(
            dpy, root, wx, wy, 240, 160, 0,
            BlackPixel(dpy, DefaultScreen(dpy)),
            (rgb & 0xFFFFFFUL)
        );
        XStoreName(dpy, win, title);
        XSetIconName(dpy, win, title);

        XSizeHints sh;
        memset(&sh, 0, sizeof(sh));
        sh.flags = USPosition | USSize;
        sh.x = wx;
        sh.y = wy;
        sh.width = 240;
        sh.height = 160;
        XSetWMNormalHints(dpy, win, &sh);

        XClassHint ch;
        ch.res_name = (char *)title;
        ch.res_class = (char *)"IconOrderTest";
        XSetClassHint(dpy, win, &ch);

        /* Set 48x48 _NET_WM_ICON with solid ARGB color */
        Atom net_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
        unsigned long icon_data[2 + 48 * 48];
        icon_data[0] = 48UL;
        icon_data[1] = 48UL;
        unsigned long argb = 0xFF000000UL | (rgb & 0xFFFFFFUL);
        for (int i = 0; i < 48 * 48; ++i) {
            icon_data[2 + i] = argb;
        }
        XChangeProperty(
            dpy, win, net_icon, XA_CARDINAL, 32, PropModeReplace,
            (const unsigned char *)icon_data, 2 + 48 * 48
        );

        Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy, win, &wm_del, 1);
        XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
        XMapWindow(dpy, win);
        XSync(dpy, False);

        printf("WIN=0x%lx\n", (unsigned long)win);
        fflush(stdout);

        while (!g_stop) {
            while (XPending(dpy) > 0) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wm_del) {
                    g_stop = 1;
                    break;
                }
            }
            usleep(20000);
        }
        XDestroyWindow(dpy, win);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(mode, "icon_geom") == 0 && argc > 2) {
        Window win = (Window)strtoul(argv[2], NULL, 0);
        Atom geom_a = XInternAtom(dpy, "_NET_WM_ICON_GEOMETRY", False);
        Atom act = None;
        int fmt = 0;
        unsigned long nitems = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, win, geom_a, 0, 4, False, XA_CARDINAL,
                               &act, &fmt, &nitems, &rem, &data) != Success ||
            !data || fmt != 32 || nitems < 4) {
            if (data) XFree(data);
            printf("NONE\n");
            return 0;
        }
        unsigned long *vals = (unsigned long *)data;
        printf("%lu,%lu,%lu,%lu\n", vals[0], vals[1], vals[2], vals[3]);
        XFree(data);
        return 0;
    }

    if (strcmp(mode, "iconify") == 0 && argc > 2) {
        Window win = (Window)strtoul(argv[2], NULL, 0);
        XIconifyWindow(dpy, win, DefaultScreen(dpy));
        XSync(dpy, False);
        return 0;
    }

    if (strcmp(mode, "restore") == 0 && argc > 2) {
        Window win = (Window)strtoul(argv[2], NULL, 0);
        XMapRaised(dpy, win);
        Atom act_a = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = win;
        ev.xclient.message_type = act_a;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 2L;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XSync(dpy, False);
        return 0;
    }

    if (strcmp(mode, "req") == 0 && argc > 2) {
        Atom info_a = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        Atom act = None;
        int fmt = 0;
        unsigned long nitems = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, info_a, 0, 2, False, AnyPropertyType,
                               &act, &fmt, &nitems, &rem, &data) != Success || !data || nitems < 2) {
            return 5;
        }
        Window wm_win = (Window)((unsigned long *)data)[1];
        XFree(data);
        Atom req_a = XInternAtom(dpy, "_DT_WM_REQUEST", False);
        XChangeProperty(dpy, wm_win, req_a, XA_STRING, 8, PropModeAppend,
                        (const unsigned char *)argv[2], (int)strlen(argv[2]) + 1);
        XSync(dpy, False);
        return 0;
    }

    if (strcmp(mode, "xrdb") == 0 && argc > 2) {
        XChangeProperty(dpy, root, XA_RESOURCE_MANAGER, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)argv[2], (int)strlen(argv[2]));
        XSync(dpy, False);
        return 0;
    }

    if (strcmp(mode, "maximize") == 0 && argc > 3) {
        Window win = (Window)strtoul(argv[2], NULL, 0);
        int on = atoi(argv[3]);
        Atom st_a = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom mv_a = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
        Atom mh_a = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = win;
        ev.xclient.message_type = st_a;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = on ? 1L : 0L;
        ev.xclient.data.l[1] = (long)mv_a;
        ev.xclient.data.l[2] = (long)mh_a;
        ev.xclient.data.l[3] = 2L;
        XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XSync(dpy, False);
        return 0;
    }

    if (strcmp(mode, "win_size") == 0 && argc > 2) {
        Window win = (Window)strtoul(argv[2], NULL, 0);
        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, win, &attr)) return 1;
        printf("%d,%d\n", attr.width, attr.height);
        return 0;
    }

    if (strcmp(mode, "resize") == 0 && argc > 4) {
        Window win = (Window)strtoul(argv[2], NULL, 0);
        unsigned int nw = (unsigned int)strtoul(argv[3], NULL, 0);
        unsigned int nh = (unsigned int)strtoul(argv[4], NULL, 0);
        XResizeWindow(dpy, win, nw, nh);
        XSync(dpy, False);
        return 0;
    }

    if (strcmp(mode, "click_max_btn") == 0 && argc > 2) {
        Window cur = (Window)strtoul(argv[2], NULL, 0);
        Window frame = cur;
        for (int k = 0; k < 5 && cur != None && cur != root; ++k) {
            Window r_ret = None, p_ret = None, *ch = NULL;
            unsigned int n = 0;
            if (!XQueryTree(dpy, cur, &r_ret, &p_ret, &ch, &n)) break;
            if (ch) XFree(ch);
            if (p_ret == root) {
                frame = cur;
                break;
            }
            cur = p_ret;
        }
        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, frame, &attr)) return 1;
        int mx = attr.x + attr.width - 14;
        int my = attr.y + 14;
        XTestFakeMotionEvent(dpy, -1, mx, my, CurrentTime);
        XSync(dpy, False);
        usleep(20000);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        usleep(20000);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        return 0;
    }

    if (strcmp(mode, "find_iconbox") == 0) {
        g_xerr = 0;
        XSetErrorHandler(ignore_xerr);
        Window found = find_iconbox_rec(dpy, root, 0);
        if (found != None) {
            printf("0x%lx\n", (unsigned long)found);
        } else {
            printf("NONE\n");
        }
        return 0;
    }

    if (strcmp(mode, "wm_ready") == 0) {
        Atom info_a = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        Atom act = None;
        int fmt = 0;
        unsigned long nitems = 0, rem = 0;
        unsigned char *data = NULL;
        int ok = 0;
        if (XGetWindowProperty(dpy, root, info_a, 0, 2, False, AnyPropertyType,
                               &act, &fmt, &nitems, &rem, &data) == Success &&
            data && nitems >= 2) {
            Window wm_win = (Window)((unsigned long *)data)[1];
            XWindowAttributes attr;
            g_xerr = 0;
            XSetErrorHandler(ignore_xerr);
            if (wm_win != None && XGetWindowAttributes(dpy, wm_win, &attr) && !g_xerr) {
                ok = 1;
            }
            XSync(dpy, False);
            if (g_xerr) ok = 0;
        }
        if (data) XFree(data);
        return ok ? 0 : 1;
    }

    return 1;
}
"""


def count_near_rgb_in_crop(
    im: Image.Image, box: tuple[int, int, int, int], rgb: tuple[int, int, int], tol: int = 24
) -> int:
    sub = im.crop(box).convert("RGB").tobytes()
    tr, tg, tb = rgb
    n = 0
    for i in range(0, len(sub), 3):
        if abs(sub[i] - tr) <= tol and abs(sub[i + 1] - tg) <= tol and abs(sub[i + 2] - tb) <= tol:
            n += 1
    return n


def spawn_color_win(
    helper_bin: Path, env: dict[str, str], title: str, hex_rgb: str, x: int, y: int
) -> tuple[subprocess.Popen[str], str]:
    p = subprocess.Popen(
        [str(helper_bin), "client", title, hex_rgb, str(x), str(y)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        start_new_session=True,
    )
    assert p.stdout is not None
    win_id = ""
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        ln = p.stdout.readline().strip()
        if ln.startswith("WIN="):
            win_id = ln.split("=", 1)[1]
            break
    if not win_id:
        p.kill()
        raise SystemExit(f"client {title} failed to report X11 window ID")
    return p, win_id


def wait_icon_geom(
    helper_bin: Path, env: dict[str, str], win_id: str, timeout: float = 4.0
) -> tuple[int, int, int, int]:
    deadline = time.monotonic() + timeout
    last = "NONE"
    while time.monotonic() < deadline:
        out = subprocess.run(
            [str(helper_bin), "icon_geom", win_id],
            env=env,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        last = out
        if out != "NONE" and "," in out:
            parts = [int(v) for v in out.split(",")]
            if len(parts) == 4 and parts[2] > 0 and parts[3] > 0:
                return (parts[0], parts[1], parts[2], parts[3])
        time.sleep(0.05)
    raise SystemExit(f"timed out waiting for _NET_WM_ICON_GEOMETRY on {win_id} (last={last})")


def assert_slot_color(
    im: Image.Image, geom: tuple[int, int, int, int], rgb: tuple[int, int, int], label: str
) -> None:
    gx, gy, gw, gh = geom
    px = count_near_rgb_in_crop(im, (gx, gy, gx + gw, gy + gh), rgb, tol=24)
    if px < 400:
        raise SystemExit(f"{label}: expected >=400px of {rgb} inside icon slot {geom}, got {px}px")


def build_ld_env(base_env: dict[str, str], ref_bin: Path) -> dict[str, str]:
    env = base_env.copy()
    ld_parts = []
    for anc in ref_bin.resolve().parents:
        for sub in ("motif-prefix/lib", "cde-stage/usr/dt/lib", "fs-prefix/lib"):
            cand = anc / sub
            if cand.is_dir():
                ld_parts.append(str(cand))
    for sys_cand in ("/usr/dt/lib",):
        if Path(sys_cand).is_dir():
            ld_parts.append(sys_cand)
    if env.get("LD_LIBRARY_PATH"):
        ld_parts.append(env["LD_LIBRARY_PATH"])
    if ld_parts:
        env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
    return env


def verify_wayland_icon_order(comp_bin: Path, helper_bin: Path, work: Path) -> None:
    xdg = work / "wl-xdg"
    home = work / "wl-home"
    xdg.mkdir(mode=0o700)
    home.mkdir(mode=0o700)
    os.chmod(xdg, 0o700)
    os.chmod(home, 0o700)

    cde_src = comp_bin.parent
    makefile = comp_bin.parent / "Makefile"
    if makefile.is_file():
        for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
            if ln.startswith("top_srcdir ="):
                cde_src = (comp_bin.parent / ln.split("=", 1)[1].strip()).resolve()
                break

    env = build_ld_env(os.environ, comp_bin)
    env.update({
        "HOME": str(home),
        "XDG_RUNTIME_DIR": str(xdg),
        "WLR_BACKENDS": "headless",
        "WLR_HEADLESS_OUTPUTS": "1",
        "WLR_RENDERER": "pixman",
        "WLR_LIBINPUT_NO_DEVICES": "1",
        "CDE_BACKDROP_DIR": str(cde_src / "programs" / "backdrops"),
    })

    status_file = work / "wl.status"
    comp_log = (work / "dtwm-wayland-order.log").open("w", encoding="utf-8")
    comp = subprocess.Popen(
        [str(comp_bin), "--no-panel", "--status-file", str(status_file)],
        env=env,
        stdout=comp_log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    procs: list[subprocess.Popen] = [comp]
    try:
        wl_sock = x_sock = None
        for _ in range(75):
            if status_file.is_file():
                kv = dict(
                    ln.split("=", 1)
                    for ln in status_file.read_text(encoding="utf-8").splitlines()
                    if "=" in ln
                )
                wl_sock = kv.get("WAYLAND_DISPLAY")
                x_sock = kv.get("DISPLAY")
                if wl_sock and x_sock:
                    break
            time.sleep(0.1)
        if not wl_sock or not x_sock:
            raise SystemExit("dtwm-wayland failed to start for icon-order check")

        c_env = {**env, "WAYLAND_DISPLAY": wl_sock, "DISPLAY": x_sock}
        subprocess.run(
            [str(helper_bin), "xrdb", "Dtwm*animateMinimizeRestore: False\nDtwm*minimizeAnimation: none\n"],
            env=c_env,
            check=True,
        )
        time.sleep(0.08)

        # 1. Open-window desktop icons (default showOpenWindowIcons=True): launch Red, Green, Blue
        p_a, win_a = spawn_color_win(helper_bin, c_env, "OrderAlphaRed", "E02828", 240, 80)
        procs.append(p_a)
        time.sleep(0.18)
        p_b, win_b = spawn_color_win(helper_bin, c_env, "OrderBetaGreen", "18D050", 520, 80)
        procs.append(p_b)
        time.sleep(0.18)
        p_c, win_c = spawn_color_win(helper_bin, c_env, "OrderGammaBlue", "2878F0", 240, 300)
        procs.append(p_c)
        time.sleep(0.25)

        ga0 = wait_icon_geom(helper_bin, c_env, win_a)
        gb0 = wait_icon_geom(helper_bin, c_env, win_b)
        gc0 = wait_icon_geom(helper_bin, c_env, win_c)
        if not (ga0[1] < gb0[1] < gc0[1]):
            raise SystemExit(f"expected initial top-to-bottom order A < B < C, got {ga0}, {gb0}, {gc0}")

        # Minimize A via titlebar minimize button click, then minimize B via XIconifyWindow
        # Also exercise pointer click restore on desktop icon!
        subprocess.run(["wlrctl", "toplevel", "focus", "title:OrderAlphaRed"], env=c_env, check=True)
        time.sleep(0.08)
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:OrderAlphaRed"], env=c_env, check=True)
        time.sleep(0.12)
        subprocess.run([str(helper_bin), "iconify", win_b], env=c_env, check=True)
        time.sleep(0.12)

        if (
            wait_icon_geom(helper_bin, c_env, win_a) != ga0
            or wait_icon_geom(helper_bin, c_env, win_b) != gb0
            or wait_icon_geom(helper_bin, c_env, win_c) != gc0
        ):
            raise SystemExit(
                "desktop icon positions/order changed after minimizing A and B: "
                f"expected {(ga0, gb0, gc0)}, got "
                f"{(wait_icon_geom(helper_bin, c_env, win_a), wait_icon_geom(helper_bin, c_env, win_b), wait_icon_geom(helper_bin, c_env, win_c))}"
            )

        # Restore A by clicking its desktop icon at Slot 0, focus C, minimize A again, restore B via desktop icon click
        subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
        subprocess.run(
            ["wlrctl", "pointer", "move", str(ga0[0] + ga0[2] // 2), str(ga0[1] + ga0[3] // 2)],
            env=c_env,
            check=True,
        )
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        time.sleep(0.12)
        subprocess.run(["wlrctl", "toplevel", "focus", "title:OrderGammaBlue"], env=c_env, check=True)
        time.sleep(0.08)
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:OrderAlphaRed"], env=c_env, check=True)
        time.sleep(0.12)
        subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
        subprocess.run(
            ["wlrctl", "pointer", "move", str(gb0[0] + gb0[2] // 2), str(gb0[1] + gb0[3] // 2)],
            env=c_env,
            check=True,
        )
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        time.sleep(0.15)

        ga1 = wait_icon_geom(helper_bin, c_env, win_a)
        gb1 = wait_icon_geom(helper_bin, c_env, win_b)
        gc1 = wait_icon_geom(helper_bin, c_env, win_c)
        if (ga1, gb1, gc1) != (ga0, gb0, gc0):
            raise SystemExit(
                f"desktop icon order changed across restore/focus/re-minimize! expected {(ga0, gb0, gc0)}, got {(ga1, gb1, gc1)}"
            )

        # Also verify right-click Window Menu -> "Restore" (item 0) on minimized desktop icon A
        # restores A without altering desktop icon ordering!
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:OrderAlphaRed"], env=c_env, check=True)
        time.sleep(0.1)
        subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
        subprocess.run(
            ["wlrctl", "pointer", "move", str(ga0[0] + ga0[2] // 2), str(ga0[1] + ga0[3] // 2)],
            env=c_env,
            check=True,
        )
        subprocess.run(["wlrctl", "pointer", "click", "right"], env=c_env, check=True)
        time.sleep(0.08)
        subprocess.run(["wlrctl", "pointer", "move", "24", "14"], env=c_env, check=True)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        time.sleep(0.12)
        if (
            wait_icon_geom(helper_bin, c_env, win_a) != ga0
            or wait_icon_geom(helper_bin, c_env, win_b) != gb0
            or wait_icon_geom(helper_bin, c_env, win_c) != gc0
        ):
            raise SystemExit("desktop icon order changed after Window Menu Restore on A!")

        shot_keep = work / "wl-order-preserved.png"
        subprocess.run(["grim", str(shot_keep)], env=c_env, check=True)
        with Image.open(shot_keep) as im:
            assert_slot_color(im, ga0, (0xE0, 0x28, 0x28), "Slot0 Red after min/restore")
            assert_slot_color(im, gb0, (0x18, 0xD0, 0x50), "Slot1 Green after min/restore")
            assert_slot_color(im, gc0, (0x28, 0x78, 0xF0), "Slot2 Blue after min/restore")

        # 1b. Verify interactive desktop icon dragging with placement grid & slot highlight:
        # Drag Window C's icon further down the desktop (+260px Y), confirm it snaps to a lower
        # grid cell and stays there across minimize/restore, then pack back via f.pack_icons!
        with Image.open(shot_keep) as im_sz:
            out_w, out_h = im_sz.size
        shot_drag_grid = work / "wl-icon-drag-grid.png"
        cx0 = gc0[0] + gc0[2] // 2
        cy0 = gc0[1] + gc0[3] // 2
        subprocess.run(
            [
                str(helper_bin),
                "wl_drag",
                str(cx0),
                str(cy0),
                str(cx0),
                str(cy0 + 260),
                str(out_w),
                str(out_h),
                str(shot_drag_grid),
            ],
            env=c_env,
            check=True,
        )
        time.sleep(0.12)

        gc_dragged = wait_icon_geom(helper_bin, c_env, win_c)
        if gc_dragged[1] < gc0[1] + 180 or gc_dragged[0] != gc0[0]:
            raise SystemExit(
                f"expected dragged desktop icon C to snap further down in column 0, got {gc_dragged} (was {gc0})"
            )
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:OrderGammaBlue"], env=c_env, check=True)
        time.sleep(0.1)
        subprocess.run(["wlrctl", "toplevel", "focus", "title:OrderGammaBlue"], env=c_env, check=True)
        time.sleep(0.1)
        if wait_icon_geom(helper_bin, c_env, win_c) != gc_dragged:
            raise SystemExit("manually dragged desktop icon C did not stay in place across minimize/restore!")

        subprocess.run([str(helper_bin), "req", "f.pack_icons"], env=c_env, check=True)
        time.sleep(0.12)
        if wait_icon_geom(helper_bin, c_env, win_c) != gc0:
            raise SystemExit(
                f"expected f.pack_icons to pack C back to {gc0}, got {wait_icon_geom(helper_bin, c_env, win_c)}"
            )

        # 2. Verify Icon Box mode preserves mental-map ordering across client minimize,
        # restore, maximize, unmaximize, Icon Box icon clicks, Icon Box frame maximize/restore,
        # and closing a middle window inside the Icon Box!
        subprocess.run([str(helper_bin), "req", "f.set_use_icon_box 1"], env=c_env, check=True)
        time.sleep(0.15)
        iba = wait_icon_geom(helper_bin, c_env, win_a)
        ibb = wait_icon_geom(helper_bin, c_env, win_b)
        ibc = wait_icon_geom(helper_bin, c_env, win_c)
        if not (iba[0] < ibb[0] < ibc[0]):
            raise SystemExit(f"expected Icon Box order A < B < C, got {iba}, {ibb}, {ibc}")

        # Focus A, minimize B, maximize C, unmaximize C, maximize A, unmaximize A
        subprocess.run(["wlrctl", "toplevel", "focus", "title:OrderAlphaRed"], env=c_env, check=True)
        time.sleep(0.08)
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:OrderBetaGreen"], env=c_env, check=True)
        time.sleep(0.08)
        subprocess.run(["wlrctl", "toplevel", "maximize", "title:OrderGammaBlue"], env=c_env, check=True)
        time.sleep(0.1)
        subprocess.run([str(helper_bin), "maximize", win_c, "0"], env=c_env, check=True)
        time.sleep(0.1)
        subprocess.run(["wlrctl", "toplevel", "maximize", "title:OrderAlphaRed"], env=c_env, check=True)
        time.sleep(0.1)
        subprocess.run([str(helper_bin), "maximize", win_a, "0"], env=c_env, check=True)
        time.sleep(0.1)
        if (
            wait_icon_geom(helper_bin, c_env, win_a) != iba
            or wait_icon_geom(helper_bin, c_env, win_b) != ibb
            or wait_icon_geom(helper_bin, c_env, win_c) != ibc
        ):
            raise SystemExit("Icon Box icon order changed across client minimize/maximize/unmaximize!")

        # Click minimized Icon B inside the Icon Box to restore & focus B, then click focused Icon B
        # again inside the Icon Box to toggle-minimize B in-place without reordering!
        subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
        subprocess.run(
            ["wlrctl", "pointer", "move", str(ibb[0] + ibb[2] // 2), str(ibb[1] + ibb[3] // 2)],
            env=c_env,
            check=True,
        )
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        time.sleep(0.1)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        time.sleep(0.1)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        time.sleep(0.1)
        if (
            wait_icon_geom(helper_bin, c_env, win_a) != iba
            or wait_icon_geom(helper_bin, c_env, win_b) != ibb
            or wait_icon_geom(helper_bin, c_env, win_c) != ibc
        ):
            raise SystemExit("Icon Box icon order changed after left-clicking Icon B inside Icon Box!")

        # Maximize and restore the Icon Box frame itself via double-clicking its titlebar
        # (icon_x = icon_box_x + DTWM_BORDER_WIDTH(6) + 5 + 8 -> icon_box_x = iba[0] - 19,
        #  icon_y = icon_box_y + DTWM_BORDER_WIDTH(6) + DTWM_TITLE_HEIGHT(24) + 5 + 6 -> icon_box_y = iba[1] - 41)
        ib_title_x = iba[0] + 120
        ib_title_y = iba[1] - 41 + 16
        subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
        subprocess.run(["wlrctl", "pointer", "move", str(ib_title_x), str(ib_title_y)], env=c_env, check=True)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        time.sleep(0.12)
        ib_max_a = wait_icon_geom(helper_bin, c_env, win_a)
        ib_max_b = wait_icon_geom(helper_bin, c_env, win_b)
        ib_max_c = wait_icon_geom(helper_bin, c_env, win_c)
        if not (ib_max_a[0] < ib_max_b[0] < ib_max_c[0]) or ib_max_a[1] >= iba[1]:
            raise SystemExit(
                f"expected maximized Icon Box order A < B < C near top, got {ib_max_a}, {ib_max_b}, {ib_max_c}"
            )
        ib_max_title_x = ib_max_a[0] + 120
        ib_max_title_y = ib_max_a[1] - 41 + 16
        subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
        subprocess.run(["wlrctl", "pointer", "move", str(ib_max_title_x), str(ib_max_title_y)], env=c_env, check=True)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
        time.sleep(0.12)
        if (
            wait_icon_geom(helper_bin, c_env, win_a) != iba
            or wait_icon_geom(helper_bin, c_env, win_b) != ibb
            or wait_icon_geom(helper_bin, c_env, win_c) != ibc
        ):
            raise SystemExit("Icon Box icon positions changed after maximizing and restoring Icon Box frame!")

        # Spawn 4th window D while in Icon Box, then close C inside Icon Box -> D shifts into C's slot!
        p_d_ib, win_d_ib = spawn_color_win(helper_bin, c_env, "OrderDeltaGold", "E0B020", 520, 300)
        procs.append(p_d_ib)
        time.sleep(0.2)
        ibd = wait_icon_geom(helper_bin, c_env, win_d_ib)
        if not (ibc[0] < ibd[0]):
            raise SystemExit(f"expected new window D at Slot 3 > C in Icon Box, got C={ibc}, D={ibd}")
        p_c.terminate()
        p_c.wait(timeout=3)
        time.sleep(0.2)
        if wait_icon_geom(helper_bin, c_env, win_d_ib)[:2] != ibc[:2]:
            raise SystemExit("closing C inside Wayland Icon Box did not shift D into C's slot!")
        shot_ib = work / "wl-iconbox-order.png"
        subprocess.run(["grim", str(shot_ib)], env=c_env, check=True)
        with Image.open(shot_ib) as im_ib:
            assert_slot_color(im_ib, iba, (0xE0, 0x28, 0x28), "IconBox Slot0 Red")
            assert_slot_color(im_ib, ibb, (0x18, 0xD0, 0x50), "IconBox Slot1 Green")
            assert_slot_color(im_ib, ibc, (0xE0, 0xB0, 0x20), "IconBox Slot2 Gold after closing Blue")

        # Switch back to desktop icons: [A, B, D] must appear at [ga0, gb0, gc0]!
        subprocess.run([str(helper_bin), "req", "f.set_use_icon_box 0"], env=c_env, check=True)
        time.sleep(0.15)
        p_c = p_d_ib
        win_c = win_d_ib
        subprocess.run(["wlrctl", "toplevel", "focus", "title:OrderBetaGreen"], env=c_env, check=True)
        time.sleep(0.08)
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:OrderAlphaRed"], env=c_env, check=True)
        time.sleep(0.1)

        # 3. Close middle open window B -> C (OrderDeltaGold) must move up into Slot 1 (gb0), while A stays at Slot 0 (ga0)!
        p_b.terminate()
        p_b.wait(timeout=3)
        time.sleep(0.2)
        if wait_icon_geom(helper_bin, c_env, win_a) != ga0:
            raise SystemExit("Slot 0 icon moved when closing middle window B!")
        if wait_icon_geom(helper_bin, c_env, win_c)[:2] != gb0[:2]:
            raise SystemExit(
                f"expected C to move up to Slot 1 {gb0[:2]} after closing B, got {wait_icon_geom(helper_bin, c_env, win_c)}"
            )

        # Close minimized top window A -> C must move up into Slot 0 (ga0)!
        p_a.terminate()
        p_a.wait(timeout=3)
        time.sleep(0.2)
        if wait_icon_geom(helper_bin, c_env, win_c)[:2] != ga0[:2]:
            raise SystemExit(
                f"expected C to move up to Slot 0 {ga0[:2]} after closing A, got {wait_icon_geom(helper_bin, c_env, win_c)}"
            )
        shot_closed = work / "wl-order-after-close.png"
        subprocess.run(["grim", str(shot_closed)], env=c_env, check=True)
        with Image.open(shot_closed) as im2:
            assert_slot_color(im2, wait_icon_geom(helper_bin, c_env, win_c), (0xE0, 0xB0, 0x20), "Slot0 Gold after closing A & B")

        p_c.terminate()
        p_c.wait(timeout=3)
        time.sleep(0.15)

        # 4. Minimized-only mode (showOpenWindowIcons: False): order of first minimization is maintained across restore/re-minimize
        subprocess.run(
            [
                str(helper_bin),
                "xrdb",
                "Dtwm*showOpenWindowIcons: False\nDtwm*animateMinimizeRestore: False\nDtwm*minimizeAnimation: none\n",
            ],
            env=c_env,
            check=True,
        )
        time.sleep(0.12)
        p_p, win_p = spawn_color_win(helper_bin, c_env, "MinOnlyP", "E02828", 220, 80)
        p_q, win_q = spawn_color_win(helper_bin, c_env, "MinOnlyQ", "18D050", 500, 80)
        p_r, win_r = spawn_color_win(helper_bin, c_env, "MinOnlyR", "2878F0", 220, 280)
        procs.extend([p_p, p_q, p_r])
        time.sleep(0.25)

        # Minimize R first, then P, then Q -> order [R, P, Q]
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:MinOnlyR"], env=c_env, check=True)
        time.sleep(0.1)
        gr = wait_icon_geom(helper_bin, c_env, win_r)
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:MinOnlyP"], env=c_env, check=True)
        time.sleep(0.1)
        gp = wait_icon_geom(helper_bin, c_env, win_p)
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:MinOnlyQ"], env=c_env, check=True)
        time.sleep(0.1)
        gq = wait_icon_geom(helper_bin, c_env, win_q)
        if not (gr[1] < gp[1] < gq[1]) or wait_icon_geom(helper_bin, c_env, win_r) != gr:
            raise SystemExit(f"minimized-only initial order [R, P, Q] broken: {gr}, {gp}, {gq}")

        # Restore P and re-minimize P -> [R, P, Q] must still hold!
        subprocess.run(["wlrctl", "toplevel", "focus", "title:MinOnlyP"], env=c_env, check=True)
        time.sleep(0.1)
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:MinOnlyP"], env=c_env, check=True)
        time.sleep(0.1)
        if (
            wait_icon_geom(helper_bin, c_env, win_r) != gr
            or wait_icon_geom(helper_bin, c_env, win_p) != gp
            or wait_icon_geom(helper_bin, c_env, win_q) != gq
        ):
            raise SystemExit("minimized-only [R, P, Q] ordering changed after restoring and re-minimizing P!")

        # Close R -> P and Q move up to gr and gp!
        p_r.terminate()
        p_r.wait(timeout=3)
        time.sleep(0.2)
        if wait_icon_geom(helper_bin, c_env, win_p)[:2] != gr[:2] or wait_icon_geom(helper_bin, c_env, win_q)[:2] != gp[:2]:
            raise SystemExit("minimized-only icons below closed R did not move up cleanly!")
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.terminate()
        for p in reversed(procs):
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
        comp_log.close()


def pick_free_x_display() -> int:
    for d in range(45, 99):
        if not Path(f"/tmp/.X11-unix/X{d}").exists():
            return d
    return 87


def verify_x11_dtwm_icon_order(dtwm_bin: Path, helper_bin: Path, work: Path) -> str:
    xvfb = shutil.which("Xvfb")
    if not xvfb or not dtwm_bin.is_file():
        return "X11 dtwm skipped (Xvfb or dtwm binary unavailable)"

    disp_num = pick_free_x_display()
    disp = f":{disp_num}"
    home = work / "x11-home"
    home.mkdir(mode=0o700)
    (home / ".dt").mkdir(parents=True, exist_ok=True)
    # Minimal dtwmrc without FrontPanel launch so standalone dtwm starts cleanly in <0.3s
    dtwmrc = home / ".dt" / "dtwmrc"
    dtwmrc.write_text(
        'Menu DtRootMenu\n{\n  "Pack Icons" f.pack_icons\n}\n',
        encoding="utf-8",
    )

    env = build_ld_env(os.environ, dtwm_bin)
    env.update({
        "HOME": str(home),
        "DISPLAY": disp,
    })

    xvfb_proc = subprocess.Popen(
        [xvfb, disp, "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    procs: list[subprocess.Popen] = [xvfb_proc]
    dtwm_log = (work / "dtwm-x11-order.log").open("w", encoding="utf-8")
    try:
        for _ in range(50):
            if Path(f"/tmp/.X11-unix/X{disp_num}").exists():
                break
            time.sleep(0.05)

        subprocess.run(
            [
                str(helper_bin),
                "xrdb",
                "Dtwm*useFrontPanel: False\n"
                "Dtwm*useIconBox: False\n"
                "Dtwm*showOpenWindowIcons: True\n"
                "Dtwm*iconPlacement: Top Left\n"
                "Dtwm*animateMinimizeRestore: False\n"
                "Dtwm*minimizeAnimation: none\n",
            ],
            env=env,
            check=True,
        )

        dtwm_proc = subprocess.Popen(
            [
                str(dtwm_bin),
                "-xrm", "Dtwm*useFrontPanel: False",
                "-xrm", "Dtwm*showOpenWindowIcons: True",
                "-xrm", "Dtwm*iconPlacement: Top Left",
                "-xrm", "Dtwm*minimizeAnimation: none",
            ],
            env=env,
            stdout=dtwm_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        procs.append(dtwm_proc)

        wm_up = False
        for _ in range(80):
            if dtwm_proc.poll() is not None:
                raise SystemExit(f"X11 dtwm exited early with rc={dtwm_proc.returncode}")
            if subprocess.run([str(helper_bin), "wm_ready"], env=env).returncode == 0:
                wm_up = True
                break
            time.sleep(0.05)
        if not wm_up:
            raise SystemExit("X11 dtwm failed to publish _MOTIF_WM_INFO")

        maps = Path(f"/proc/{dtwm_proc.pid}/maps").read_text(encoding="utf-8")
        if "libXm.so.9" not in maps:
            raise SystemExit(f"X11 dtwm PID {dtwm_proc.pid} did not map libXm.so.9!")

        p_a, win_a = spawn_color_win(helper_bin, env, "X11OrderA", "E02828", 260, 90)
        procs.append(p_a)
        time.sleep(0.18)
        p_b, win_b = spawn_color_win(helper_bin, env, "X11OrderB", "18D050", 540, 90)
        procs.append(p_b)
        time.sleep(0.18)
        p_c, win_c = spawn_color_win(helper_bin, env, "X11OrderC", "2878F0", 260, 310)
        procs.append(p_c)
        time.sleep(0.25)

        ga0 = wait_icon_geom(helper_bin, env, win_a)
        gb0 = wait_icon_geom(helper_bin, env, win_b)
        gc0 = wait_icon_geom(helper_bin, env, win_c)
        if not (ga0[1] < gb0[1] < gc0[1]):
            raise SystemExit(f"X11 dtwm expected initial order A < B < C, got {ga0}, {gb0}, {gc0}")

        # Minimize & restore A and B -> icon slots remain invariant
        subprocess.run([str(helper_bin), "iconify", win_a], env=env, check=True)
        subprocess.run([str(helper_bin), "iconify", win_b], env=env, check=True)
        time.sleep(0.15)
        subprocess.run([str(helper_bin), "restore", win_a], env=env, check=True)
        subprocess.run([str(helper_bin), "restore", win_b], env=env, check=True)
        time.sleep(0.15)
        subprocess.run([str(helper_bin), "iconify", win_a], env=env, check=True)
        time.sleep(0.15)

        if (
            wait_icon_geom(helper_bin, env, win_a)[:2] != ga0[:2]
            or wait_icon_geom(helper_bin, env, win_b)[:2] != gb0[:2]
            or wait_icon_geom(helper_bin, env, win_c)[:2] != gc0[:2]
        ):
            raise SystemExit("X11 dtwm desktop icon order changed across minimize/restore!")

        # Close NORMAL_STATE (open) middle window B -> C must pack up to gb0, and new window D goes to gc0!
        p_b.terminate()
        p_b.wait(timeout=3)
        time.sleep(0.2)
        if wait_icon_geom(helper_bin, env, win_a)[:2] != ga0[:2]:
            raise SystemExit("X11 dtwm Slot 0 moved when closing middle open window B!")
        if wait_icon_geom(helper_bin, env, win_c)[:2] != gb0[:2]:
            raise SystemExit(
                f"X11 dtwm expected C to pack up to {gb0[:2]} after closing open window B, got {wait_icon_geom(helper_bin, env, win_c)}"
            )

        p_d, win_d = spawn_color_win(helper_bin, env, "X11OrderD", "E0B020", 540, 310)
        procs.append(p_d)
        time.sleep(0.2)
        if wait_icon_geom(helper_bin, env, win_d)[:2] != gc0[:2]:
            raise SystemExit(
                f"X11 dtwm expected newly opened D at Slot 2 {gc0[:2]}, got {wait_icon_geom(helper_bin, env, win_d)}"
            )

        # Close MINIMIZED_STATE top window A -> C moves to ga0 and D moves to gb0!
        p_a.terminate()
        p_a.wait(timeout=3)
        time.sleep(0.2)
        if wait_icon_geom(helper_bin, env, win_c)[:2] != ga0[:2] or wait_icon_geom(helper_bin, env, win_d)[:2] != gb0[:2]:
            raise SystemExit("X11 dtwm icons below closed minimized window A did not pack up!")

        # Clean up desktop-icon phase clients & dtwm, then restart dtwm in Icon Box mode (3x2 grid)
        for p_cl in (p_c, p_d, dtwm_proc):
            if p_cl.poll() is None:
                p_cl.kill()
                p_cl.wait(timeout=3)
        time.sleep(0.15)

        subprocess.run(
            [
                str(helper_bin),
                "xrdb",
                "Dtwm*useFrontPanel: False\n"
                "Dtwm*useIconBox: True\n"
                "Dtwm*iconBoxGeometry: 3x2+20-20\n"
                "Dtwm*iconBoxSBDisplayPolicy: vertical\n"
                "Dtwm*showOpenWindowIcons: True\n"
                "Dtwm*iconPlacement: Left Top\n"
                "Dtwm*animateMinimizeRestore: False\n"
                "Dtwm*minimizeAnimation: none\n",
            ],
            env=env,
            check=True,
        )

        dtwm_ib_proc = subprocess.Popen(
            [
                str(dtwm_bin),
                "-xrm", "Dtwm*useFrontPanel: False",
                "-xrm", "Dtwm*useIconBox: True",
                "-xrm", "Dtwm*iconBoxGeometry: 3x2+20-20",
                "-xrm", "Dtwm*iconBoxSBDisplayPolicy: vertical",
                "-xrm", "Dtwm*showOpenWindowIcons: True",
                "-xrm", "Dtwm*iconPlacement: Left Top",
                "-xrm", "Dtwm*minimizeAnimation: none",
            ],
            env=env,
            stdout=dtwm_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        procs.append(dtwm_ib_proc)

        wm_up = False
        for _ in range(80):
            if dtwm_ib_proc.poll() is not None:
                raise SystemExit(f"X11 dtwm (Icon Box) exited early with rc={dtwm_ib_proc.returncode}")
            if subprocess.run([str(helper_bin), "wm_ready"], env=env).returncode == 0:
                wm_up = True
                break
            time.sleep(0.05)
        if not wm_up:
            raise SystemExit("X11 dtwm (Icon Box) failed to publish _MOTIF_WM_INFO")

        # Launch 5 windows [A, B, C, D, E] -> Row 0 has [A, B, C], Row 1 has [D, E]
        ib_wins: list[tuple[subprocess.Popen[str], str]] = []
        for idx, (nm, hx) in enumerate([
            ("X11IB_A", "E02828"),
            ("X11IB_B", "18D050"),
            ("X11IB_C", "2878F0"),
            ("X11IB_D", "E0B020"),
            ("X11IB_E", "B040E0"),
        ]):
            pr, wid = spawn_color_win(helper_bin, env, nm, hx, 320 + idx * 40, 80 + idx * 30)
            procs.append(pr)
            ib_wins.append((pr, wid))
            time.sleep(0.15)
        time.sleep(0.2)

        g0 = [wait_icon_geom(helper_bin, env, wid)[:2] for _, wid in ib_wins]
        # Row 0: A, B, C (same Y, increasing X); Row 1: D, E (larger Y, increasing X)
        if not (
            g0[0][1] == g0[1][1] == g0[2][1]
            and g0[0][0] < g0[1][0] < g0[2][0]
            and g0[3][1] > g0[0][1]
            and g0[3][1] == g0[4][1]
            and g0[3][0] < g0[4][0]
        ):
            raise SystemExit(f"X11 Icon Box expected 3x2 row-major [A,B,C / D,E], got {g0}")

        # Client minimize, restore, maximize, unmaximize -> Icon Box order stays strictly g0!
        subprocess.run([str(helper_bin), "iconify", ib_wins[0][1]], env=env, check=True)
        subprocess.run([str(helper_bin), "maximize", ib_wins[1][1], "1"], env=env, check=True)
        time.sleep(0.15)
        subprocess.run([str(helper_bin), "maximize", ib_wins[1][1], "0"], env=env, check=True)
        subprocess.run([str(helper_bin), "restore", ib_wins[0][1]], env=env, check=True)
        subprocess.run([str(helper_bin), "iconify", ib_wins[3][1]], env=env, check=True)
        time.sleep(0.15)
        g_after_minmax = [wait_icon_geom(helper_bin, env, wid)[:2] for _, wid in ib_wins]
        if g_after_minmax != g0:
            raise SystemExit(
                f"X11 Icon Box order changed across client minimize/maximize/restore: expected {g0}, got {g_after_minmax}"
            )

        # Maximize the Icon Box window via _NET_WM_STATE -> widens to 5 columns so [A,B,C,D,E] reflow onto Row 0,
        # then unmaximize back to initial 3-column width and verify [A,B,C / D,E] order g0 is preserved!
        ib_xwin = subprocess.run(
            [str(helper_bin), "find_iconbox"], env=env, check=True, capture_output=True, text=True
        ).stdout.strip()
        if ib_xwin == "NONE":
            raise SystemExit("failed to find X11 Icon Box client window")

        subprocess.run([str(helper_bin), "click_max_btn", ib_xwin], env=env, check=True)
        time.sleep(0.55)
        g_ib_wide = [wait_icon_geom(helper_bin, env, wid)[:2] for _, wid in ib_wins]
        if not (
            g_ib_wide[0][1] == g_ib_wide[1][1] == g_ib_wide[2][1] == g_ib_wide[3][1] == g_ib_wide[4][1]
            and g_ib_wide[0][0] < g_ib_wide[1][0] < g_ib_wide[2][0] < g_ib_wide[3][0] < g_ib_wide[4][0]
        ):
            raise SystemExit(f"X11 maximized Icon Box did not reflow [A,B,C,D,E] cleanly onto Row 0: {g_ib_wide}")

        subprocess.run([str(helper_bin), "click_max_btn", ib_xwin], env=env, check=True)
        time.sleep(0.2)
        g_ib_rest = [wait_icon_geom(helper_bin, env, wid)[:2] for _, wid in ib_wins]
        if g_ib_rest != g0:
            raise SystemExit(
                f"X11 unmaximized Icon Box scrambled or failed to restore 3x2 order: expected {g0}, got {g_ib_rest}"
            )

        # Close middle window B -> [A, C, D] occupy Row 0 (g0[0..2]) and [E] occupies Row 1 Slot 0 (g0[3])!
        ib_wins[1][0].terminate()
        ib_wins[1][0].wait(timeout=3)
        time.sleep(0.2)
        g_rem = [
            wait_icon_geom(helper_bin, env, ib_wins[i][1])[:2]
            for i in (0, 2, 3, 4)
        ]
        if g_rem != g0[:4]:
            raise SystemExit(
                f"X11 Icon Box closing B did not compact [A, C, D, E] into {g0[:4]}, got {g_rem}"
            )

        return "X11 dtwm (libXm.so.9) desktop + Icon Box mental-map ordering & reflow verified"
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.terminate()
        for p in reversed(procs):
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
        dtwm_log.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    ap.add_argument("--dtwm", default="")
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    dtwm_bin = Path(args.dtwm).resolve() if args.dtwm else (comp_bin.parent / "dtwm")
    cc = os.environ.get("CC", "cc")

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-icon-order-", dir=tmp_base) as work_s:
        work = Path(work_s)
        helper_c = work / "x11_order_helper.c"
        helper_bin = work / "x11_order_helper"
        helper_c.write_text(X11_COLOR_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", str(helper_c), "-lX11", "-lXtst", "-lwayland-client", "-o", str(helper_bin)],
            check=True,
        )

        verify_wayland_icon_order(comp_bin, helper_bin, work)
        x11_summary = verify_x11_dtwm_icon_order(dtwm_bin, helper_bin, work)

        print(f"PASS: desktop & Icon Box icon ordering verified (Wayland dtwm-wayland + {x11_summary})")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
