#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland WP-04:
Open-Window Desktop Icons, WM_ICON_SIZE (48/96), _DT_WM_SETTINGS_V1,
xdg-toplevel-icon-v1, XWayland root pointer sync for ck-eyes, dtfile pinned
desktop icons, live dynamic icons (ck-clock, ck-load, ck-eyes, ck-plasma-1),
and Front Panel TYPE client swallowing.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image


X11_INSPECTOR_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);
    const char *cmd = (argc > 1) ? argv[1] : "props";

    if (strcmp(cmd, "props") == 0) {
        int count = 0;
        XIconSize *sz = NULL;
        if (!XGetIconSizes(dpy, root, &sz, &count) || !sz || count < 1) {
            fprintf(stderr, "XGetIconSizes failed\n");
            return 2;
        }
        int max_w = sz[0].max_width;
        int max_h = sz[0].max_height;
        XFree(sz);

        Atom set_atom = XInternAtom(dpy, "_DT_WM_SETTINGS_V1", False);
        Atom act = None;
        int fmt = 0;
        unsigned long nitems = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, set_atom, 0, 2, False, XA_CARDINAL,
                               &act, &fmt, &nitems, &rem, &data) != Success || !data || nitems < 2) {
            fprintf(stderr, "_DT_WM_SETTINGS_V1 missing\n");
            return 3;
        }
        unsigned long ver = ((unsigned long *)data)[0];
        unsigned long flags = ((unsigned long *)data)[1];
        XFree(data);
        printf("ICON_MAX=%dx%d SETTINGS_V=%lu FLAGS=0x%lx\n", max_w, max_h, ver, flags);
        return 0;
    }

    if (strcmp(cmd, "pointer") == 0) {
        Window r_ret = None, c_ret = None;
        int rx = -1, ry = -1, wx = 0, wy = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(dpy, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask)) {
            return 4;
        }
        printf("POINTER=%d,%d\n", rx, ry);
        return 0;
    }

    if (strcmp(cmd, "req") == 0 && argc > 2) {
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
        XFlush(dpy);
        return 0;
    }

    if (strcmp(cmd, "pin") == 0) {
        /* Emulate dtfile Desktop.c MakeDesktopWindow (popup_name0 undecorated USPosition desktop object) */
        int s = DefaultScreen(dpy);
        Window w = XCreateSimpleWindow(dpy, root, 28, 520, 76, 76, 0,
                                       BlackPixel(dpy, s), 0xE6D28A);
        XClassHint ch = { .res_name = (char *)"popup_name0", .res_class = (char *)"Dtfile" };
        XSetClassHint(dpy, w, &ch);
        typedef struct { unsigned long flags, functions, decorations; long input_mode; unsigned long status; } MwmHints;
        MwmHints mh = { 3UL, 0UL, 0UL, 0L, 0UL };
        Atom mwm_a = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
        XChangeProperty(dpy, w, mwm_a, mwm_a, 32, PropModeReplace, (unsigned char *)&mh, 5);
        XMapWindow(dpy, w);
        XFlush(dpy);
        for (;;) pause();
    }

    return 1;
}
"""


WL_ICON_CLIENT_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"

static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct xdg_wm_base *g_wm;
static struct xdg_toplevel_icon_manager_v1 *g_icon_mgr;
static int g_configured = 0;

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { (void)d; xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_l = { .ping = wm_ping };

static void surf_conf(void *d, struct xdg_surface *s, uint32_t ser) {
    (void)d;
    xdg_surface_ack_configure(s, ser);
    g_configured = 1;
}
static const struct xdg_surface_listener surf_l = { .configure = surf_conf };

static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)d; (void)t; (void)w; (void)h; (void)st;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; exit(0); }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = {
    .configure = top_conf, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps
};

static void ic_sz(void *d, struct xdg_toplevel_icon_manager_v1 *m, int32_t s) { (void)d; (void)m; (void)s; }
static void ic_done(void *d, struct xdg_toplevel_icon_manager_v1 *m) { (void)d; (void)m; }
static const struct xdg_toplevel_icon_manager_v1_listener ic_l = { .icon_size = ic_sz, .done = ic_done };

static void reg_global(void *d, struct wl_registry *r, uint32_t n, const char *iface, uint32_t v) {
    (void)d;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        g_comp = wl_registry_bind(r, n, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        g_shm = wl_registry_bind(r, n, &wl_shm_interface, 1);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        g_wm = wl_registry_bind(r, n, &xdg_wm_base_interface, v < 5 ? v : 5);
        xdg_wm_base_add_listener(g_wm, &wm_l, NULL);
    } else if (strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name) == 0) {
        g_icon_mgr = wl_registry_bind(r, n, &xdg_toplevel_icon_manager_v1_interface, 1);
        xdg_toplevel_icon_manager_v1_add_listener(g_icon_mgr, &ic_l, NULL);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { .global = reg_global, .global_remove = reg_rem };

static struct wl_buffer *make_shm_buf(int w, int h, uint32_t argb) {
    size_t sz = (size_t)w * (size_t)h * 4;
    int fd = memfd_create("wl-icon-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)sz) < 0) return NULL;
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (size_t i = 0; i < (size_t)(w * h); ++i) px[i] = argb;
    munmap(px, sz);
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)sz);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

int main(void) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!g_comp || !g_shm || !g_wm || !g_icon_mgr) return 2;

    struct wl_surface *surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *xsurf = xdg_wm_base_get_xdg_surface(g_wm, surf);
    xdg_surface_add_listener(xsurf, &surf_l, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, "WaylandIconApp");

    /* Assign bright emerald #10D860 48x48 icon via xdg-toplevel-icon-v1 */
    struct wl_buffer *ibuf = make_shm_buf(48, 48, 0xFF10D860u);
    struct xdg_toplevel_icon_v1 *ic = xdg_toplevel_icon_manager_v1_create_icon(g_icon_mgr);
    xdg_toplevel_icon_v1_add_buffer(ic, ibuf, 1);
    xdg_toplevel_icon_manager_v1_set_icon(g_icon_mgr, top, ic);
    xdg_toplevel_icon_v1_destroy(ic);

    wl_surface_commit(surf);
    while (!g_configured && wl_display_dispatch(dpy) != -1) {}

    struct wl_buffer *wbuf = make_shm_buf(240, 160, 0xFF243448u);
    wl_surface_attach(surf, wbuf, 0, 0);
    wl_surface_commit(surf);
    wl_display_flush(dpy);

    while (wl_display_dispatch(dpy) != -1) {}
    return 0;
}
"""


def count_near_rgb_in_crop(
    im: Image.Image, box: tuple[int, int, int, int], rgb: tuple[int, int, int], tol: int = 18
) -> int:
    sub = im.crop(box).convert("RGB").tobytes()
    tr, tg, tb = rgb
    n = 0
    for i in range(0, len(sub), 3):
        if abs(sub[i] - tr) <= tol and abs(sub[i + 1] - tg) <= tol and abs(sub[i + 2] - tb) <= tol:
            n += 1
    return n


def crop_sha256(im: Image.Image, box: tuple[int, int, int, int]) -> str:
    return hashlib.sha256(im.crop(box).convert("RGB").tobytes()).hexdigest()


def discover_apps_dir(comp_bin: Path, explicit: str) -> Path | None:
    if explicit:
        p = Path(explicit)
        return p if p.is_dir() else None
    # <root>/cde-build/programs/dtwm/dtwm-wayland -> <root>/apps-build/bin
    cand = comp_bin.resolve().parents[3] / "apps-build" / "bin"
    if (cand / "ck-eyes").is_file() and (cand / "ck-clock").is_file():
        return cand
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    ap.add_argument("--apps-dir", default="")
    ap.add_argument("--save-dir", default="")
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    apps_dir = discover_apps_dir(comp_bin, args.apps_dir)
    cc = os.environ.get("CC", "cc")

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-wayland-icons-", dir=tmp_base) as work_s:
        work = Path(work_s)
        xdg = work / "xdg"
        home = work / "home"
        xdg.mkdir(mode=0o700)
        home.mkdir(mode=0o700)
        os.chmod(xdg, 0o700)
        os.chmod(home, 0o700)

        # Compile X11 inspector + dtfile desktop-pin emulator
        x11_c = work / "x11_inspect.c"
        x11_bin = work / "x11_inspect"
        x11_c.write_text(X11_INSPECTOR_C, encoding="utf-8")
        subprocess.run([cc, "-O2", str(x11_c), "-lX11", "-o", str(x11_bin)], check=True)

        # Compile native Wayland xdg-toplevel-icon-v1 client
        xdg_xml = "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
        ic_xml = "/usr/share/wayland-protocols/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml"
        subprocess.run(["wayland-scanner", "client-header", xdg_xml, str(work / "xdg-shell-client-protocol.h")], check=True)
        subprocess.run(["wayland-scanner", "private-code", xdg_xml, str(work / "xdg-shell-protocol.c")], check=True)
        subprocess.run(["wayland-scanner", "client-header", ic_xml, str(work / "xdg-toplevel-icon-v1-client-protocol.h")], check=True)
        subprocess.run(["wayland-scanner", "private-code", ic_xml, str(work / "xdg-toplevel-icon-v1-protocol.c")], check=True)
        wl_ic_c = work / "wl_icon_client.c"
        wl_ic_bin = work / "wl_icon_client"
        wl_ic_c.write_text(WL_ICON_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [
                cc, "-O2", f"-I{work}",
                str(wl_ic_c), str(work / "xdg-shell-protocol.c"), str(work / "xdg-toplevel-icon-v1-protocol.c"),
                "-lwayland-client", "-o", str(wl_ic_bin),
            ],
            check=True,
        )

        # Find CDE backdrops directory
        cde_src = comp_bin.parent
        makefile = comp_bin.parent / "Makefile"
        if makefile.is_file():
            for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
                if ln.startswith("top_srcdir ="):
                    cde_src = (comp_bin.parent / ln.split("=", 1)[1].strip()).resolve()
                    break

        env = os.environ.copy()
        env.update({
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "2",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
            "CDE_BACKDROP_DIR": str(cde_src / "programs" / "backdrops"),
        })

        status_file = work / "compositor.status"
        comp_log = (work / "dtwm-wayland.log").open("w", encoding="utf-8")
        comp = subprocess.Popen(
            [str(comp_bin), "--status-file", str(status_file)],
            env=env,
            stdout=comp_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        procs = [comp]

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
                raise SystemExit("dtwm-wayland failed to publish status file")

            c_env = {**env, "WAYLAND_DISPLAY": wl_sock, "DISPLAY": x_sock}

            # 1. Verify WM_ICON_SIZE (48x48) & _DT_WM_SETTINGS_V1 (show_open_window_icons=1)
            props = subprocess.run([str(x11_bin), "props"], env=c_env, check=True, capture_output=True, text=True).stdout
            if "ICON_MAX=48x48 SETTINGS_V=1 FLAGS=0x1" not in props:
                raise SystemExit(f"unexpected initial WM_ICON_SIZE / _DT_WM_SETTINGS_V1:\n{props}")

            # 2. Launch Wayland foot + xdg-toplevel-icon-v1 client + dtfile desktop pin
            p_foot = subprocess.Popen(["foot"], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs.append(p_foot)
            time.sleep(0.6)

            p_wlic = subprocess.Popen([str(wl_ic_bin)], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs.append(p_wlic)
            p_pin = subprocess.Popen([str(x11_bin), "pin"], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs.append(p_pin)
            time.sleep(0.8)

            # Move cursor over native Wayland foot window (100, 110) and verify XWayland root pointer sync!
            subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
            subprocess.run(["wlrctl", "pointer", "move", "100", "110"], env=c_env, check=True)
            time.sleep(0.15)
            ptr_out = subprocess.run([str(x11_bin), "pointer"], env=c_env, check=True, capture_output=True, text=True).stdout.strip()
            if ptr_out != "POINTER=100,110":
                raise SystemExit(f"expected XWayland root pointer synced to 100,110 over Wayland surface, got {ptr_out}")

            # Verify xdg-toplevel-icon-v1 emerald (#10D860) desktop icon on right margin of Output 1
            shot_proto = work / "wp04-proto-icons.png"
            subprocess.run(["grim", str(shot_proto)], env=c_env, check=True)
            with Image.open(shot_proto) as im_p:
                out1_w = im_p.size[0] // 2
                out_h = im_p.size[1]
                emerald_px = count_near_rgb_in_crop(
                    im_p, (out1_w - 120, 0, out1_w, 320), (0x10, 0xD8, 0x60), tol=18
                )
                if emerald_px < 800:
                    raise SystemExit(f"expected emerald xdg-toplevel-icon-v1 desktop icon pixels, got {emerald_px}")

            # 3. If ck-core-apps binaries are present, exercise full live dynamic icons + Front Panel swallowing!
            apps_summary = "skipped (apps-dir not present)"
            shots_to_save = [shot_proto]
            if apps_dir:
                # Launch swallowed Front Panel clock (-name fpclock) + normal ck-clock + ck-load + minimized ck-eyes + minimized ck-plasma-1
                p_fpclock = subprocess.Popen(
                    [str(apps_dir / "ck-clock")],
                    env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
                )
                procs.append(p_fpclock)
                time.sleep(0.5)

                p_clock = subprocess.Popen(
                    [str(apps_dir / "ck-clock"), "--no-embedd", "-name", "ck-clock-debug", "-geometry", "260x160+60+280"],
                    env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
                )
                p_load = subprocess.Popen(
                    [str(apps_dir / "ck-load"), "-geometry", "280x190+360+280"],
                    env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
                )
                p_eyes = subprocess.Popen(
                    [str(apps_dir / "ck-eyes"), "-iconic"],
                    env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
                )
                p_plasma = subprocess.Popen(
                    [str(apps_dir / "ck-plasma-1"), "-iconic"],
                    env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
                )
                procs.extend([p_clock, p_load, p_eyes, p_plasma])
                time.sleep(1.5)

                # Verify libXm.so.9 provenance on live processes
                for app_proc in (p_fpclock, p_load, p_eyes):
                    maps = Path(f"/proc/{app_proc.pid}/maps").read_text(encoding="utf-8")
                    if "libXm.so.9" not in maps:
                        raise SystemExit(f"PID {app_proc.pid} did not map libXm.so.9!")

                # Frame A: pointer over native Wayland foot window (100, 110)
                subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
                subprocess.run(["wlrctl", "pointer", "move", "100", "110"], env=c_env, check=True)
                time.sleep(0.35)
                shot_a = work / "wp04-dynamic-icons-a.png"
                subprocess.run(["grim", str(shot_a)], env=c_env, check=True)

                # Move pointer across desktop to (980, 680) and wait 1.2s so ck-eyes tracks pointer & ck-plasma-1 / ck-load advance frames!
                subprocess.run(["wlrctl", "pointer", "move", "880", "570"], env=c_env, check=True)
                time.sleep(1.2)
                shot_b = work / "wp04-dynamic-icons-b.png"
                subprocess.run(["grim", str(shot_b)], env=c_env, check=True)

                with Image.open(shot_a) as ima, Image.open(shot_b) as imb:
                    icon_strip_box = (out1_w - 220, 0, out1_w, out_h - 80)
                    ha = crop_sha256(ima, icon_strip_box)
                    hb = crop_sha256(imb, icon_strip_box)
                    if ha == hb:
                        raise SystemExit("desktop dynamic-icon strip hash did not animate across pointer move + timer tick!")
                    # Verify swallowed fpclock renders its dark LED time band inside the Front Panel's left Clock slot!
                    panel_x = (out1_w - 840) // 2
                    fp_clock_box = (panel_x + 24, out_h - 70, panel_x + 115, out_h - 8)
                    led_band_px = count_near_rgb_in_crop(ima, fp_clock_box, (28, 32, 28), tol=30)
                    if led_band_px < 150:
                        raise SystemExit(f"expected swallowed ck-clock LED band inside Front Panel Clock slot, got {led_band_px} px")

                # Toggle Increase Icon Size (96x96) live via _DT_WM_REQUEST and verify WM_ICON_SIZE updates to 96x96!
                subprocess.run([str(x11_bin), "req", "f.set_large_icons 1"], env=c_env, check=True)
                time.sleep(1.2)
                props_lg = subprocess.run([str(x11_bin), "props"], env=c_env, check=True, capture_output=True, text=True).stdout
                if "ICON_MAX=96x96" not in props_lg:
                    raise SystemExit(f"expected ICON_MAX=96x96 after f.set_large_icons 1, got:\n{props_lg}")

                shot_large = work / "wp04-dynamic-icons-96.png"
                subprocess.run(["grim", str(shot_large)], env=c_env, check=True)
                shots_to_save.extend([shot_a, shot_b, shot_large])
                apps_summary = f"live dynamic icons animated ({ha[:10]} -> {hb[:10]}), swallowed fpclock + 48->96px verified"

            if args.save_dir:
                sdir = Path(args.save_dir)
                sdir.mkdir(parents=True, exist_ok=True)
                for f in shots_to_save:
                    shutil.copy2(f, sdir / f.name)

            print(
                f"PASS: dtwm-wayland WP-04 icons verified "
                f"(emerald={emerald_px}px, ptr={ptr_out}, {apps_summary})"
            )
            return 0
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


if __name__ == "__main__":
    raise SystemExit(main())
