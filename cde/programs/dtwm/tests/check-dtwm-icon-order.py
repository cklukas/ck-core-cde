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
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);
    const char *mode = argv[1];

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

    if (strcmp(mode, "wm_ready") == 0) {
        Atom info_a = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        Atom act = None;
        int fmt = 0;
        unsigned long nitems = 0, rem = 0;
        unsigned char *data = NULL;
        int ok = (XGetWindowProperty(dpy, root, info_a, 0, 2, False, AnyPropertyType,
                                     &act, &fmt, &nitems, &rem, &data) == Success &&
                  data && nitems >= 2);
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
        subprocess.run(
            ["wlrctl", "pointer", "move", str(ga0[0] + ga0[2] // 2 + 24), str(ga0[1] + ga0[3] // 2 + 14)],
            env=c_env,
            check=True,
        )
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

        # 2. Verify Icon Box mode also preserves invariant ordering across minimize & restore
        subprocess.run([str(helper_bin), "req", "f.set_use_icon_box 1"], env=c_env, check=True)
        time.sleep(0.15)
        iba = wait_icon_geom(helper_bin, c_env, win_a)
        ibb = wait_icon_geom(helper_bin, c_env, win_b)
        ibc = wait_icon_geom(helper_bin, c_env, win_c)
        if not (iba[0] < ibb[0] < ibc[0]):
            raise SystemExit(f"expected Icon Box order A < B < C, got {iba}, {ibb}, {ibc}")
        subprocess.run(["wlrctl", "toplevel", "focus", "title:OrderAlphaRed"], env=c_env, check=True)
        time.sleep(0.08)
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:OrderBetaGreen"], env=c_env, check=True)
        time.sleep(0.1)
        if (
            wait_icon_geom(helper_bin, c_env, win_a) != iba
            or wait_icon_geom(helper_bin, c_env, win_b) != ibb
            or wait_icon_geom(helper_bin, c_env, win_c) != ibc
        ):
            raise SystemExit("Icon Box icon order changed after restore/minimize!")
        subprocess.run([str(helper_bin), "req", "f.set_use_icon_box 0"], env=c_env, check=True)
        time.sleep(0.15)
        subprocess.run(["wlrctl", "toplevel", "focus", "title:OrderBetaGreen"], env=c_env, check=True)
        time.sleep(0.08)
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:OrderAlphaRed"], env=c_env, check=True)
        time.sleep(0.1)

        # 3. Close middle open window B -> C must move up into Slot 1 (gb0), while A stays at Slot 0 (ga0)!
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
            assert_slot_color(im2, wait_icon_geom(helper_bin, c_env, win_c), (0x28, 0x78, 0xF0), "Slot0 Blue after closing A & B")

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

        return "X11 dtwm (libXm.so.9) open/minimized icon order + close-pack verified"
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
        subprocess.run([cc, "-O2", str(helper_c), "-lX11", "-o", str(helper_bin)], check=True)

        verify_wayland_icon_order(comp_bin, helper_bin, work)
        x11_summary = verify_x11_dtwm_icon_order(dtwm_bin, helper_bin, work)

        print(f"PASS: desktop & Icon Box icon ordering verified (Wayland dtwm-wayland + {x11_summary})")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
