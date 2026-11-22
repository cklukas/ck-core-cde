#!/usr/bin/env python3
"""Automated security & integration test for WP-06:
1. ext-idle-notify-v1 idle timeout -> ext-session-lock-v1 lock acquisition.
2. Zero screencopy & input/shortcut leakage to underlying Wayland/XWayland windows while locked.
3. Live monitor hotplug while locked (f.hotplug_headless_output) -> zero frame leakage.
4. Dead-locker SIGKILL failsafe (session remains locked & black until clean unlock_and_destroy).
5. dtsession --wayland CLI contract & dtstyle Wayland hardware/input IPC (_DT_WAYLAND_INPUT_STATE).
"""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image


WL_LOCK_CLIENT_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"

typedef struct OutNode {
    uint32_t name;
    struct wl_output *out;
    struct wl_surface *surf;
    struct ext_session_lock_surface_v1 *lsurf;
    struct wl_buffer *buf;
    struct OutNode *next;
} OutNode;

static struct wl_display *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct xdg_wm_base *g_wm;
static struct wl_seat *g_seat;
static struct wl_keyboard *g_kbd;
static struct ext_session_lock_manager_v1 *g_lock_mgr;
static struct ext_idle_notifier_v1 *g_idle_mgr;
static OutNode *g_outs;

static struct ext_session_lock_v1 *g_lock;
static volatile sig_atomic_t g_unlock_requested = 0;
static int g_locked = 0;
static int g_app_keys = 0;
static const char *g_status_out = NULL;

static void write_state(const char *tag) {
    if (!g_status_out) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_status_out);
    FILE *fp = fopen(tmp, "w");
    if (fp) {
        fprintf(fp, "STATE=%s\nLOCKED=%d\nAPP_KEYS=%d\n", tag, g_locked, g_app_keys);
        fclose(fp);
        rename(tmp, g_status_out);
    }
}

static void on_sigusr1(int sig) {
    (void)sig;
    g_unlock_requested = 1;
}

static int make_shm(size_t sz) {
    char name[64];
    snprintf(name, sizeof(name), "/wp06-shm-%d-%ld", (int)getpid(), (long)random());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return -1; }
    return fd;
}

static struct wl_buffer *solid_buf(uint32_t w, uint32_t h, uint32_t argb) {
    size_t sz = (size_t)w * (size_t)h * 4u;
    int fd = make_shm(sz);
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) px[i] = argb;
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)sz);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(px, sz);
    close(fd);
    return b;
}

static void ls_conf(void *d, struct ext_session_lock_surface_v1 *ls, uint32_t ser, uint32_t w, uint32_t h) {
    OutNode *n = d;
    ext_session_lock_surface_v1_ack_configure(ls, ser);
    if (n->buf) wl_buffer_destroy(n->buf);
    /* Deep navy lock shield (0xFF102038) to verify lock surface commits cleanly */
    n->buf = solid_buf(w ? w : 1280, h ? h : 720, 0xFF102038u);
    wl_surface_attach(n->surf, n->buf, 0, 0);
    wl_surface_damage_buffer(n->surf, 0, 0, (int32_t)(w ? w : 1280), (int32_t)(h ? h : 720));
    wl_surface_commit(n->surf);
}
static const struct ext_session_lock_surface_v1_listener ls_l = { .configure = ls_conf };

static void attach_out_lock(OutNode *n) {
    if (!g_lock || n->lsurf) return;
    n->surf = wl_compositor_create_surface(g_comp);
    n->lsurf = ext_session_lock_v1_get_lock_surface(g_lock, n->surf, n->out);
    ext_session_lock_surface_v1_add_listener(n->lsurf, &ls_l, n);
}

static void sl_locked(void *d, struct ext_session_lock_v1 *l) {
    (void)d; (void)l;
    g_locked = 1;
    write_state("LOCKED");
}
static void sl_finished(void *d, struct ext_session_lock_v1 *l) {
    (void)d; (void)l;
    g_locked = 0;
    write_state("FINISHED");
}
static const struct ext_session_lock_v1_listener sl_l = { .locked = sl_locked, .finished = sl_finished };

static void start_lock_now(void) {
    if (g_lock || !g_lock_mgr) return;
    g_lock = ext_session_lock_manager_v1_lock(g_lock_mgr);
    ext_session_lock_v1_add_listener(g_lock, &sl_l, NULL);
    for (OutNode *n = g_outs; n; n = n->next) attach_out_lock(n);
    wl_display_flush(g_dpy);
}

static void idle_idled(void *d, struct ext_idle_notification_v1 *n) {
    (void)d; (void)n;
    write_state("IDLED");
    start_lock_now();
}
static void idle_resumed(void *d, struct ext_idle_notification_v1 *n) { (void)d; (void)n; }
static const struct ext_idle_notification_v1_listener idle_l = { .idled = idle_idled, .resumed = idle_resumed };

/* App window keyboard observer (verifies zero keystrokes leak while locked) */
static void kb_map(void *d, struct wl_keyboard *k, uint32_t f, int fd, uint32_t sz) {
    (void)d; (void)k; (void)f; (void)sz; close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *ks) {
    (void)d; (void)k; (void)s; (void)sf; (void)ks;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {
    (void)d; (void)k; (void)s; (void)sf;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) {
    (void)d; (void)k; (void)s; (void)t; (void)key;
    if (st == 1) {
        g_app_keys++;
        write_state("APP_KEY");
    }
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) {
    (void)d; (void)k; (void)s; (void)dep; (void)lat; (void)lck; (void)grp;
}
static void kb_rep(void *d, struct wl_keyboard *k, int32_t r, int32_t del) { (void)d; (void)k; (void)r; (void)del; }
static const struct wl_keyboard_listener kb_l = {
    .keymap = kb_map, .enter = kb_enter, .leave = kb_leave, .key = kb_key, .modifiers = kb_mods, .repeat_info = kb_rep
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    const char *mode = d;
    if (strcmp(mode, "app") == 0 && (caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_kbd) {
        g_kbd = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(g_kbd, &kb_l, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_l = { .capabilities = seat_caps, .name = seat_name };

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = { .ping = wm_ping };

static struct wl_surface *g_app_surf;
static int g_app_conf = 0;
static void xdgs_conf(void *d, struct xdg_surface *xs, uint32_t s) {
    (void)d;
    xdg_surface_ack_configure(xs, s);
    if (!g_app_conf) {
        g_app_conf = 1;
        /* Bright red (0xFFE02020) so screencopy test can detect even 1 leaked pixel */
        struct wl_buffer *b = solid_buf(320, 220, 0xFFE02020u);
        wl_surface_attach(g_app_surf, b, 0, 0);
        wl_surface_commit(g_app_surf);
        write_state("APP_READY");
    }
}
static const struct xdg_surface_listener xdgs_l = { .configure = xdgs_conf };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        g_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2);
        xdg_wm_base_add_listener(g_wm, &wm_l, NULL);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(g_seat, &seat_l, d);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        OutNode *n = calloc(1, sizeof(*n));
        n->name = name;
        n->out = wl_registry_bind(r, name, &wl_output_interface, ver < 3 ? ver : 3);
        n->next = g_outs;
        g_outs = n;
        if (g_lock) { attach_out_lock(n); wl_display_flush(g_dpy); }
    } else if (strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
        g_lock_mgr = wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface, 1);
    } else if (strcmp(iface, ext_idle_notifier_v1_interface.name) == 0) {
        g_idle_mgr = wl_registry_bind(r, name, &ext_idle_notifier_v1_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { .global = reg_global, .global_remove = reg_rem };

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "app";
    if (argc > 2) g_status_out = argv[2];
    signal(SIGUSR1, on_sigusr1);

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &reg_l, (void *)mode);
    wl_display_roundtrip(g_dpy);
    wl_display_roundtrip(g_dpy);

    if (strcmp(mode, "app") == 0) {
        g_app_surf = wl_compositor_create_surface(g_comp);
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g_wm, g_app_surf);
        xdg_surface_add_listener(xs, &xdgs_l, NULL);
        struct xdg_toplevel *xt = xdg_surface_get_toplevel(xs);
        xdg_toplevel_set_title(xt, "SensitiveAppWindow");
        wl_surface_commit(g_app_surf);
    } else if (strcmp(mode, "idle_lock") == 0) {
        struct ext_idle_notification_v1 *in =
            ext_idle_notifier_v1_get_idle_notification(g_idle_mgr, 300, g_seat);
        ext_idle_notification_v1_add_listener(in, &idle_l, NULL);
    } else if (strcmp(mode, "recover_lock") == 0) {
        start_lock_now();
    }
    wl_display_flush(g_dpy);

    struct pollfd pfd = { .fd = wl_display_get_fd(g_dpy), .events = POLLIN };
    while (1) {
        if (g_unlock_requested && g_lock) {
            ext_session_lock_v1_unlock_and_destroy(g_lock);
            g_lock = NULL;
            g_locked = 0;
            wl_display_roundtrip(g_dpy);
            write_state("UNLOCKED");
            break;
        }
        wl_display_dispatch_pending(g_dpy);
        wl_display_flush(g_dpy);
        if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(g_dpy) < 0) break;
        }
    }
    return 0;
}
"""


X11_HELPER_C = r"""
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);

    if (strcmp(argv[1], "read_state") == 0) {
        Atom a = XInternAtom(dpy, "_DT_WAYLAND_INPUT_STATE", False);
        Atom act = None; int fmt = 0; unsigned long n = 0, rem = 0; unsigned char *d = NULL;
        if (XGetWindowProperty(dpy, root, a, 0, 256, False, AnyPropertyType, &act, &fmt, &n, &rem, &d) == Success && d) {
            fwrite(d, 1, n, stdout);
            putchar('\n');
            XFree(d);
            return 0;
        }
        return 2;
    }

    if (strcmp(argv[1], "read_ws") == 0) {
        Atom info_a = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        Atom act = None; int fmt = 0; unsigned long n = 0, rem = 0; unsigned char *d = NULL;
        if (XGetWindowProperty(dpy, root, info_a, 0, 2, False, AnyPropertyType, &act, &fmt, &n, &rem, &d) != Success || !d || n < 2) {
            return 2;
        }
        Window wm_win = (Window)((unsigned long *)d)[1];
        XFree(d); d = NULL;
        Atom a = XInternAtom(dpy, "_DT_WORKSPACE_CURRENT", False);
        if (XGetWindowProperty(dpy, wm_win, a, 0, 1, False, XA_ATOM, &act, &fmt, &n, &rem, &d) == Success && d && n >= 1) {
            char *nm = XGetAtomName(dpy, *(Atom *)d);
            if (nm) { printf("%s\n", nm); XFree(nm); }
            XFree(d);
            XCloseDisplay(dpy);
            return 0;
        }
        XCloseDisplay(dpy);
        return 2;
    }

    if (strcmp(argv[1], "req") == 0 && argc >= 3) {
        Atom info_a = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        Atom act = None; int fmt = 0; unsigned long n = 0, rem = 0; unsigned char *d = NULL;
        if (XGetWindowProperty(dpy, root, info_a, 0, 2, False, AnyPropertyType, &act, &fmt, &n, &rem, &d) != Success || !d || n < 2) {
            return 2;
        }
        Window wm_win = (Window)((unsigned long *)d)[1];
        XFree(d);
        Atom req_a = XInternAtom(dpy, "_DT_WM_REQUEST", False);
        XChangeProperty(dpy, wm_win, req_a, XA_STRING, 8, PropModeAppend,
                        (const unsigned char *)argv[2], (int)strlen(argv[2]) + 1);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }
    return 1;
}
"""


def count_near_color(im: Image.Image, rgb: tuple[int, int, int], tol: int = 16) -> int:
    raw = im.convert("RGB").tobytes()
    tr, tg, tb = rgb
    c = 0
    for i in range(0, len(raw), 3):
        if abs(raw[i] - tr) <= tol and abs(raw[i + 1] - tg) <= tol and abs(raw[i + 2] - tb) <= tol:
            c += 1
    return c


def wait_file(path: Path, substr: str, timeout: float = 6.0) -> str:
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        if path.is_file():
            last = path.read_text(encoding="utf-8", errors="replace")
            if substr in last:
                return last
        time.sleep(0.06)
    raise SystemExit(f"timed out waiting for '{substr}' in {path}; last:\n{last}")


def wait_x11_state(x11_bin: Path, env: dict[str, str], substr: str, timeout: float = 5.0) -> str:
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        res = subprocess.run([str(x11_bin), "read_state"], env=env, capture_output=True, text=True)
        if res.returncode == 0:
            last = res.stdout.strip()
            if substr in last:
                return last
        time.sleep(0.06)
    raise SystemExit(f"timed out waiting for '{substr}' in _DT_WAYLAND_INPUT_STATE; last: {last}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    build_root = comp_bin.parents[3]
    motif_pfx = build_root / "motif-prefix"
    dtsession_bin = comp_bin.parents[1] / "dtsession" / "dtsession"
    cc = os.environ.get("CC", "cc")

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-wayland-wp06-", dir=tmp_base) as work_s:
        work = Path(work_s)
        xdg = work / "xdg"
        home = work / "home"
        xdg.mkdir(mode=0o700)
        home.mkdir(mode=0o700)

        # Verify dtsession --help advertises --wayland if built
        if dtsession_bin.is_file():
            ld_check = os.environ.copy()
            ld_check["LD_LIBRARY_PATH"] = f"{motif_pfx / 'lib'}:{build_root / 'cde-stage/usr/dt/lib'}:{ld_check.get('LD_LIBRARY_PATH', '')}"
            h_res = subprocess.run([str(dtsession_bin), "--help"], env=ld_check, capture_output=True, text=True)
            if "--wayland" not in h_res.stdout:
                raise SystemExit(f"expected dtsession --help to advertise --wayland, got: {h_res.stdout}")

        xdg_xml = "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
        lock_xml = "/usr/share/wayland-protocols/staging/ext-session-lock/ext-session-lock-v1.xml"
        idle_xml = "/usr/share/wayland-protocols/staging/ext-idle-notify/ext-idle-notify-v1.xml"
        for xml, stem in ((xdg_xml, "xdg-shell"), (lock_xml, "ext-session-lock-v1"), (idle_xml, "ext-idle-notify-v1")):
            subprocess.run(["wayland-scanner", "client-header", xml, str(work / f"{stem}-client-protocol.h")], check=True)
            subprocess.run(["wayland-scanner", "private-code", xml, str(work / f"{stem}-protocol.c")], check=True)

        wl_c = work / "wl_lock_client.c"
        wl_bin = work / "wl_lock_client"
        wl_c.write_text(WL_LOCK_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [
                cc, "-O2", f"-I{work}", str(wl_c),
                str(work / "xdg-shell-protocol.c"),
                str(work / "ext-session-lock-v1-protocol.c"),
                str(work / "ext-idle-notify-v1-protocol.c"),
                "-lwayland-client", "-o", str(wl_bin),
            ],
            check=True,
        )

        x11_c = work / "x11_helper.c"
        x11_bin = work / "x11_helper"
        x11_c.write_text(X11_HELPER_C, encoding="utf-8")
        subprocess.run([cc, "-O2", str(x11_c), "-lX11", "-o", str(x11_bin)], check=True)

        cde_src = comp_bin.parent
        makefile = comp_bin.parent / "Makefile"
        if makefile.is_file():
            for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
                if ln.startswith("top_srcdir ="):
                    cde_src = (comp_bin.parent / ln.split("=", 1)[1].strip()).resolve()
                    break

        env = os.environ.copy()
        ld_parts = [str(p) for p in (motif_pfx / "lib", build_root / "cde-stage/usr/dt/lib", build_root / "fs-prefix/lib") if p.is_dir()]
        if env.get("LD_LIBRARY_PATH"):
            ld_parts.append(env["LD_LIBRARY_PATH"])
        env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
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
            env=env, stdout=comp_log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        procs = [comp]

        try:
            wl_sock = x_sock = None
            for _ in range(80):
                if status_file.is_file():
                    kv = dict(ln.split("=", 1) for ln in status_file.read_text().splitlines() if "=" in ln)
                    wl_sock, x_sock = kv.get("WAYLAND_DISPLAY"), kv.get("DISPLAY")
                    if wl_sock and x_sock:
                        break
                time.sleep(0.1)
            if not wl_sock or not x_sock:
                raise SystemExit("dtwm-wayland failed to start")

            c_env = {**env, "WAYLAND_DISPLAY": wl_sock, "DISPLAY": x_sock}

            # 1. Spawn sensitive red Wayland app window and verify pre-lock visibility
            app_st = work / "app.status"
            p_app = subprocess.Popen([str(wl_bin), "app", str(app_st)], env=c_env, start_new_session=True)
            procs.append(p_app)
            wait_file(app_st, "STATE=APP_READY")
            time.sleep(0.3)

            shot_pre = work / "pre-lock.png"
            subprocess.run(["grim", str(shot_pre)], env=c_env, check=True)
            with Image.open(shot_pre) as im:
                red_pre = count_near_color(im, (0xE0, 0x20, 0x20), tol=12)
                if red_pre < 50000:
                    raise SystemExit(f"expected red app pixels before lock, got {red_pre}")

            # 2. Test dtstyle Wayland hardware/input IPC before lock
            for req in (
                "f.set_pointer_accel 6 1 5",
                "f.set_left_handed 1",
                "f.set_key_repeat 1 350 40",
                "f.set_xkb_layout us",
                "f.set_bell 75 660 120",
                "f.set_saver_timeout 300 900",
            ):
                subprocess.run([str(x11_bin), "req", req], env=c_env, check=True)

            hw_st = wait_x11_state(
                x11_bin, c_env,
                "locked=0;accel=0.50;thresh=5;left_handed=1;tp=1:0:1:1:0;repeat=1:350:40;xkb=us;bell=75:660:120;saver=300:900",
            )

            # 3. Start ext-idle-notify-v1 (300ms) -> auto-acquires ext-session-lock-v1
            lock_st = work / "lock1.status"
            p_lock1 = subprocess.Popen([str(wl_bin), "idle_lock", str(lock_st)], env=c_env, start_new_session=True)
            procs.append(p_lock1)
            wait_file(lock_st, "LOCKED=1", timeout=5.0)
            wait_x11_state(x11_bin, c_env, "locked=1;")
            time.sleep(0.2)

            # 4. Verify zero screencopy leakage & zero input/shortcut leakage while locked
            shot_locked = work / "locked.png"
            subprocess.run(["grim", str(shot_locked)], env=c_env, check=True)
            with Image.open(shot_locked) as im:
                red_locked = count_near_color(im, (0xE0, 0x20, 0x20), tol=12)
                crimson_locked = count_near_color(im, (178, 77, 122), tol=16)
                shield_px = count_near_color(im, (0x10, 0x20, 0x38), tol=10)
                if red_locked != 0 or crimson_locked != 0:
                    raise SystemExit(
                        f"SECURITY FAIL: screencopy leaked {red_locked} app px / {crimson_locked} frame px while locked!"
                    )
                if shield_px < 500000:
                    raise SystemExit(f"expected lock shield pixels across dual outputs, got {shield_px}")

            subprocess.run(["wlrctl", "keyboard", "type", "secret"], env=c_env, check=True)
            subprocess.run(["wlrctl", "keyboard", "type", "2", "modifiers", "CTRL,ALT"], env=c_env, check=True)
            subprocess.run([str(x11_bin), "req", "f.goto_workspace ws1"], env=c_env, check=True)
            time.sleep(0.2)
            if "APP_KEYS=0" not in app_st.read_text(encoding="utf-8"):
                raise SystemExit(f"SECURITY FAIL: keystrokes leaked to app while locked: {app_st.read_text()}")
            ws_now = subprocess.check_output([str(x11_bin), "read_ws"], env=c_env, text=True).strip()
            if ws_now != "ws0":
                raise SystemExit(f"SECURITY FAIL: workspace switched while locked ({ws_now})")

            # 5. Hotplug a 3rd headless monitor (1024x768) WHILE LOCKED -> verify zero leakage
            subprocess.run([str(x11_bin), "req", "f.hotplug_headless_output 1024 768"], env=c_env, check=True)
            time.sleep(0.35)
            shot_hotplug = work / "locked-hotplug.png"
            subprocess.run(["grim", str(shot_hotplug)], env=c_env, check=True)
            with Image.open(shot_hotplug) as im:
                hw_w, hw_h = im.size
                red_hp = count_near_color(im, (0xE0, 0x20, 0x20), tol=12)
                if hw_w < 3500 or red_hp != 0:
                    raise SystemExit(
                        f"expected 3-output locked width >=3584 with 0 leaked app px, got {hw_w}x{hw_h} (red={red_hp})"
                    )

            # 6. Dead-locker SIGKILL failsafe: kill p_lock1 WITHOUT unlock_and_destroy -> stays locked & black!
            p_lock1.kill()
            p_lock1.wait(timeout=3)
            time.sleep(0.25)
            wait_x11_state(x11_bin, c_env, "locked=1;")
            shot_dead = work / "locked-dead-failsafe.png"
            subprocess.run(["grim", str(shot_dead)], env=c_env, check=True)
            with Image.open(shot_dead) as im:
                red_dead = count_near_color(im, (0xE0, 0x20, 0x20), tol=12)
                black_dead = count_near_color(im, (0, 0, 0), tol=4)
                if red_dead != 0 or black_dead < 1000000:
                    raise SystemExit(
                        f"SECURITY FAIL: dead-locker failsafe did not keep outputs black (red={red_dead}, black={black_dead})"
                    )

            # 7. Recovery locker connects, locks, and cleanly unlocks (SIGUSR1 -> unlock_and_destroy)
            rec_st = work / "recover.status"
            p_rec = subprocess.Popen([str(wl_bin), "recover_lock", str(rec_st)], env=c_env, start_new_session=True)
            procs.append(p_rec)
            wait_file(rec_st, "LOCKED=1")
            os.kill(p_rec.pid, signal.SIGUSR1)
            wait_file(rec_st, "STATE=UNLOCKED")
            wait_x11_state(x11_bin, c_env, "locked=0;")
            time.sleep(0.25)

            shot_unlocked = work / "post-unlock.png"
            subprocess.run(["grim", str(shot_unlocked)], env=c_env, check=True)
            with Image.open(shot_unlocked) as im:
                red_post = count_near_color(im, (0xE0, 0x20, 0x20), tol=12)
                if red_post < 50000:
                    raise SystemExit(f"expected desktop app window restored after unlock, got {red_post} red px")

            print(
                f"PASS: dtwm-wayland WP-06 verified "
                f"(ext-idle-notify-v1 -> ext-session-lock-v1 0px leak, 3-output hotplug {hw_w}x{hw_h} covered, "
                f"dead-locker failsafe black={black_dead}px, clean unlock restored={red_post}px, dtstyle IPC={hw_st})"
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
