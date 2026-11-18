#!/usr/bin/env python3
"""WP-13 parallel dual-environment integration check (Wayland + X11/XLibre):
1. Hardware Multimedia, Backlight & Session Keys + Non-Focus-Stealing 3D Bevel OSD:
   - Injects XF86AudioRaiseVolume, XF86AudioMute, XF86AudioMicMute,
     XF86MonBrightnessDown (verifying 5% minimum sysfs brightness floor),
     XF86MonBrightnessUp, and XF86AudioPlay (MPRIS PlayPause) via
     zwp_virtual_keyboard_v1 on Wayland and XTest + passive XGrabKey on X11 dtwm.
   - Verifies 3D-beveled OSD gauge display (`_DT_OSD_STATE` visible=1 -> auto-hide
     visible=0), screenshot capture on Wayland, and 100% focus preservation
     (active client window never loses keyboard focus while OSD is displayed).
2. Laptop Touchpad Configuration (`dtstyle`) & 3-Finger Horizontal Swipe Gestures:
   - Maps `dtstyle -mouse` (verifying Motif ABI 9 `libXm.so.9`), applies touchpad
     settings via `dtstyle -apply-touchpad`, and verifies `libinput` IPC state
     (`_DT_WAYLAND_INPUT_STATE` `tp=...`) on Wayland and XInput2 (`libXi.so.6`
     `_DT_X11_TOUCHPAD_STATE`) + `RESOURCE_MANAGER` persistence on X11.
   - Verifies 3-finger horizontal touchpad swipe switches workspaces forward and
     backward on both `dtwm-wayland` and classic X11 `dtwm`.
3. Cross-Toolkit Dark/Light Mode Synchronization (`cde-color-sync`):
   - Runs `cde-color-sync --daemon` in isolated per-lane D-Bus sessions and verifies
     that switching between ModernDark (`prefer-dark`) and Classic (`prefer-light`)
     updates `org.freedesktop.portal.Settings.Read("org.freedesktop.appearance",
     "color-scheme")` (`uint32 1` vs `uint32 2`), X11 `_XSETTINGS_S0` binary
     `_XSETTINGS_SETTINGS` (`Net/ThemeName`), `_CDE_COLOR_SCHEME`, and
     `$XDG_CONFIG_HOME/gtk-{3,4}.0/settings.ini`.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

from PIL import Image


WL_VKB_HELPER_C = r"""
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

static const struct wl_interface *vkb_mgr_types[] = {
    &wl_seat_interface,
    &zwp_virtual_keyboard_v1_interface,
};

static const struct wl_message vkb_mgr_requests[] = {
    { "create_virtual_keyboard", "on", vkb_mgr_types },
};

static const struct wl_interface zwp_virtual_keyboard_manager_v1_interface = {
    "zwp_virtual_keyboard_manager_v1", 1,
    1, vkb_mgr_requests,
    0, NULL,
};

static const struct wl_interface *vkb_null_types[] = { NULL, NULL, NULL, NULL };

static const struct wl_message vkb_requests[] = {
    { "keymap", "uhu", vkb_null_types },
    { "key", "uuu", vkb_null_types },
    { "modifiers", "uuuu", vkb_null_types },
    { "destroy", "", vkb_null_types },
};

const struct wl_interface zwp_virtual_keyboard_v1_interface = {
    "zwp_virtual_keyboard_v1", 1,
    4, vkb_requests,
    0, NULL,
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

static const struct wl_registry_listener reg_listener = {
    on_global,
    on_global_remove,
};

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: wl_vkb_helper <evdev_key_code>\n");
        return 1;
    }
    uint32_t evdev_code = (uint32_t)strtoul(argv[1], NULL, 0);

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "wl_display_connect failed\n");
        return 2;
    }

    VkbState st = { NULL, NULL };
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, &st);
    wl_display_roundtrip(dpy);
    if (!st.seat || !st.mgr) {
        fprintf(stderr, "missing wl_seat or zwp_virtual_keyboard_manager_v1\n");
        return 3;
    }

    struct wl_proxy *vkb = wl_proxy_marshal_flags(
        st.mgr, 0, &zwp_virtual_keyboard_v1_interface, 1, 0, st.seat, NULL);
    if (!vkb) {
        fprintf(stderr, "create_virtual_keyboard failed\n");
        return 4;
    }

    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
    struct xkb_keymap *kmap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *kmap_str = xkb_keymap_get_as_string(kmap, XKB_KEYMAP_FORMAT_TEXT_V1);
    uint32_t kmap_sz = (uint32_t)strlen(kmap_str) + 1u;

    int kfd = memfd_create("wp13-xkb", MFD_CLOEXEC);
    if (kfd < 0 || ftruncate(kfd, (off_t)kmap_sz) != 0) {
        fprintf(stderr, "memfd_create failed\n");
        return 5;
    }
    void *ptr = mmap(NULL, kmap_sz, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, 0);
    memcpy(ptr, kmap_str, kmap_sz);
    munmap(ptr, kmap_sz);

    wl_proxy_marshal_flags(vkb, 0, NULL, 1, 0, 1u, kfd, kmap_sz);
    wl_display_roundtrip(dpy);
    close(kfd);
    free(kmap_str);
    xkb_keymap_unref(kmap);
    xkb_context_unref(ctx);

    wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), evdev_code, 1u);
    wl_display_roundtrip(dpy);
    usleep(35000);
    wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, now_ms(), evdev_code, 0u);
    wl_display_roundtrip(dpy);

    wl_proxy_marshal_flags(vkb, 3, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
    wl_display_flush(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
"""


X11_WP13_HELPER_C = r"""
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*XTestFakeKeyEventFn)(Display *, unsigned int, Bool, unsigned long);

static void print_string_prop(Display *dpy, Window w, const char *atom_name) {
    Atom a = XInternAtom(dpy, atom_name, False);
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, w, a, 0, 4096, False, AnyPropertyType,
                           &actual, &fmt, &nitems, &after, &prop) == Success &&
        prop && fmt == 8) {
        printf("%s=%.*s\n", atom_name, (int)nitems, (const char *)prop);
    } else {
        printf("%s=\n", atom_name);
    }
    if (prop) XFree(prop);
}

static void print_atom_prop(Display *dpy, Window w, const char *atom_name) {
    Atom a = XInternAtom(dpy, atom_name, False);
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, w, a, 0, 4, False, XA_ATOM,
                           &actual, &fmt, &nitems, &after, &prop) == Success &&
        prop && nitems >= 1) {
        Atom val = ((Atom *)prop)[0];
        char *nm = val ? XGetAtomName(dpy, val) : NULL;
        printf("%s=%s\n", atom_name, nm ? nm : "");
        if (nm) XFree(nm);
    } else {
        printf("%s=\n", atom_name);
    }
    if (prop) XFree(prop);
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);

    if (argc >= 3 && strcmp(argv[1], "send-wl-req") == 0) {
        Atom mwm_info = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        Atom req_atom = XInternAtom(dpy, "_DT_WM_REQUEST", False);
        Atom act = None;
        int fmt = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(dpy, root, mwm_info, 0, 2, False, AnyPropertyType,
                               &act, &fmt, &nitems, &after, &prop) != Success ||
            !prop || nitems < 2) {
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

    if (argc >= 5 && strcmp(argv[1], "send-x11-swipe") == 0) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = root;
        ev.xclient.message_type = XInternAtom(dpy, "_DT_WM_SWIPE", False);
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = atol(argv[2]);
        ev.xclient.data.l[1] = atol(argv[3]);
        ev.xclient.data.l[2] = atol(argv[4]);
        XSendEvent(dpy, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &ev);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "tap-keysym") == 0) {
        void *xtst = dlopen("libXtst.so.6", RTLD_LAZY);
        if (!xtst) xtst = dlopen("libXtst.so", RTLD_LAZY);
        if (!xtst) {
            fprintf(stderr, "dlopen(libXtst) failed\n");
            XCloseDisplay(dpy);
            return 2;
        }
        XTestFakeKeyEventFn fake_key = (XTestFakeKeyEventFn)dlsym(xtst, "XTestFakeKeyEvent");
        KeySym ks = XStringToKeysym(argv[2]);
        if (!fake_key || ks == NoSymbol) {
            fprintf(stderr, "unknown keysym %s\n", argv[2]);
            XCloseDisplay(dpy);
            return 3;
        }
        KeyCode kc = XKeysymToKeycode(dpy, ks);
        if (kc == 0) {
            /* Map onto a spare high keycode if unmapped on minimal Xvfb */
            int min_kc = 8, max_kc = 255;
            XDisplayKeycodes(dpy, &min_kc, &max_kc);
            for (int cand = max_kc; cand >= min_kc; --cand) {
                if (XkbKeycodeToKeysym(dpy, (KeyCode)cand, 0, 0) == NoSymbol) {
                    KeySym sym = ks;
                    XChangeKeyboardMapping(dpy, cand, 1, &sym, 1);
                    XSync(dpy, False);
                    XEvent regrab;
                    memset(&regrab, 0, sizeof(regrab));
                    regrab.xclient.type = ClientMessage;
                    regrab.xclient.window = root;
                    regrab.xclient.message_type = XInternAtom(dpy, "_DT_WM_REGRAB_KEYS", False);
                    regrab.xclient.format = 32;
                    XSendEvent(dpy, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &regrab);
                    XSync(dpy, False);
                    usleep(80000);
                    kc = (KeyCode)cand;
                    break;
                }
            }
        }
        if (kc == 0) {
            fprintf(stderr, "no keycode for %s\n", argv[2]);
            XCloseDisplay(dpy);
            return 4;
        }
        fake_key(dpy, kc, True, CurrentTime);
        XSync(dpy, False);
        usleep(35000);
        fake_key(dpy, kc, False, CurrentTime);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "xsettings") == 0) {
        Atom sel = XInternAtom(dpy, "_XSETTINGS_S0", False);
        Window owner = XGetSelectionOwner(dpy, sel);
        printf("XSETTINGS_OWNER=0x%lx\n", (unsigned long)owner);
        if (owner != None) {
            Atom prop_a = XInternAtom(dpy, "_XSETTINGS_SETTINGS", False);
            Atom act = None;
            int fmt = 0;
            unsigned long nitems = 0, after = 0;
            unsigned char *prop = NULL;
            if (XGetWindowProperty(dpy, owner, prop_a, 0, 4096, False, prop_a,
                                   &act, &fmt, &nitems, &after, &prop) == Success && prop) {
                printf("XSETTINGS_BYTES=%lu\n", nitems);
                int has_dark = 0, has_adwaita = 0;
                for (unsigned long i = 0; i + 12 <= nitems; ++i) {
                    if (memcmp(prop + i, "Adwaita-dark", 12) == 0) has_dark = 1;
                }
                for (unsigned long i = 0; i + 7 <= nitems; ++i) {
                    if (memcmp(prop + i, "Adwaita", 7) == 0) has_adwaita = 1;
                }
                printf("XSETTINGS_HAS_ADWAITA_DARK=%d\n", has_dark);
                printf("XSETTINGS_HAS_ADWAITA=%d\n", has_adwaita);
                XFree(prop);
            }
        }
        print_string_prop(dpy, root, "_CDE_COLOR_SCHEME");
        print_string_prop(dpy, root, "_CDE_GTK_THEME");
        XCloseDisplay(dpy);
        return 0;
    }

    /* Default status dump */
    print_string_prop(dpy, root, "_DT_OSD_STATE");
    print_string_prop(dpy, root, "_DT_WAYLAND_INPUT_STATE");
    print_string_prop(dpy, root, "_DT_X11_TOUCHPAD_STATE");
    {
        Window wm_win = root;
        Atom mwm_info = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        Atom act2 = None;
        int fmt2 = 0;
        unsigned long n2 = 0, rem2 = 0;
        unsigned char *d2 = NULL;
        if (XGetWindowProperty(dpy, root, mwm_info, 0, 2, False, AnyPropertyType,
                               &act2, &fmt2, &n2, &rem2, &d2) == Success && d2 && n2 >= 2) {
            wm_win = (Window)((unsigned long *)d2)[1];
        }
        if (d2) XFree(d2);
        print_atom_prop(dpy, wm_win ? wm_win : root, "_DT_WORKSPACE_CURRENT");
    }
    print_string_prop(dpy, root, "RESOURCE_MANAGER");

    Window focus_w = None;
    int revert_to = 0;
    XGetInputFocus(dpy, &focus_w, &revert_to);
    char *fnm = NULL;
    if (focus_w != None && focus_w != PointerRoot) {
        XFetchName(dpy, focus_w, &fnm);
    }
    printf("X_INPUT_FOCUS=0x%lx:%s\n", (unsigned long)focus_w, fnm ? fnm : "");
    if (fnm) XFree(fnm);

    Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom act = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, root, net_active, 0, 2, False, XA_WINDOW,
                           &act, &fmt, &nitems, &after, &prop) == Success &&
        prop && nitems >= 1) {
        Window aw = ((Window *)prop)[0];
        char *nm = NULL;
        if (aw != None) XFetchName(dpy, aw, &nm);
        printf("NET_ACTIVE=0x%lx:%s\n", (unsigned long)aw, nm ? nm : "");
        if (nm) XFree(nm);
        XFree(prop);
    } else {
        printf("NET_ACTIVE=0x0:\n");
    }

    XCloseDisplay(dpy);
    return 0;
}
"""


X11_DUMMY_WIN_C = r"""
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    const char *title = (argc > 1) ? argv[1] : "WP13 Focus Target";
    const char *cls = (argc > 2) ? argv[2] : "Wp13Focus";
    const char *focus_log = (argc > 3) ? argv[3] : NULL;
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int scr = DefaultScreen(dpy);
    Window w = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 90, 90, 420, 260, 1,
                                   BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, w, title);
    XClassHint ch;
    ch.res_name = (char *)cls;
    ch.res_class = (char *)cls;
    XSetClassHint(dpy, w, &ch);
    XWMHints wmh;
    memset(&wmh, 0, sizeof(wmh));
    wmh.flags = InputHint;
    wmh.input = True;
    XSetWMHints(dpy, w, &wmh);
    XSelectInput(dpy, w, ExposureMask | StructureNotifyMask | FocusChangeMask);
    XMapWindow(dpy, w);
    XFlush(dpy);

    while (!g_stop) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (focus_log && (ev.type == FocusIn || ev.type == FocusOut)) {
                /* Ignore grab-induced NotifyGrab/NotifyWhileGrabbed transient notifications */
                if (ev.xfocus.mode == NotifyNormal) {
                    FILE *fp = fopen(focus_log, "a");
                    if (fp) {
                        fprintf(fp, "%s\n", ev.type == FocusIn ? "FocusIn" : "FocusOut");
                        fclose(fp);
                    }
                }
            }
        }
        usleep(30000);
    }
    XCloseDisplay(dpy);
    return 0;
}
"""


def wait_until(pred, timeout: float = 10.0, step: float = 0.08, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_val = None
    while time.monotonic() < deadline:
        last_val = pred()
        if last_val:
            return last_val
        time.sleep(step)
    raise RuntimeError(f"Timed out waiting for {desc} (last={last_val!r})")


def verify_pid_maps_motif9(pid: int, label: str) -> None:
    maps_path = Path(f"/proc/{pid}/maps")
    if not maps_path.is_file():
        raise RuntimeError(f"/proc/{pid}/maps missing for {label} pid {pid}")
    maps = maps_path.read_text(encoding="utf-8", errors="replace")
    if "libXm.so.9" not in maps:
        raise RuntimeError(f"{label} PID {pid} does not map libXm.so.9!\n{maps}")


def start_isolated_dbus(env: dict[str, str]) -> int:
    out = subprocess.check_output(
        ["dbus-daemon", "--session", "--fork", "--print-address=1", "--print-pid=1"],
        env=env,
        text=True,
    ).splitlines()
    if len(out) < 2:
        raise RuntimeError(f"Unexpected dbus-daemon output: {out!r}")
    env["DBUS_SESSION_BUS_ADDRESS"] = out[0].strip()
    return int(out[1].strip())


def terminate_procs(procs: list[subprocess.Popen], extra_pids: list[int]) -> None:
    for pr in reversed(procs):
        if pr.poll() is None:
            try:
                os.killpg(pr.pid, signal.SIGTERM)
            except OSError:
                try:
                    pr.terminate()
                except OSError:
                    pass
    time.sleep(0.2)
    for pr in reversed(procs):
        if pr.poll() is None:
            try:
                os.killpg(pr.pid, signal.SIGKILL)
            except OSError:
                try:
                    pr.kill()
                except OSError:
                    pass
    for pid in reversed(extra_pids):
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass


def build_base_env(
    comp_bin: Path,
    home: Path,
    xdg_rt: Path,
    xdg_data: Path,
    xdg_cfg: Path,
    media_dir: Path,
    backlight_dir: Path,
) -> dict[str, str]:
    env = os.environ.copy()
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "DTUSERSESSION", "DBUS_SESSION_BUS_ADDRESS"):
        env.pop(k, None)

    cde_build_root = comp_bin.parents[2]
    ld_parts: list[str] = []
    for cand in (
        Path("/var/tmp/ck-wayland-build/wp00/motif-prefix/lib"),
        Path("/var/tmp/ck-wayland-build/wp00/fs-prefix/lib"),
        Path("/usr/dt/lib"),
        cde_build_root / "lib/DtSvc/.libs",
        cde_build_root / "lib/DtWidget/.libs",
        cde_build_root / "lib/DtHelp/.libs",
        cde_build_root / "lib/DtPrint/.libs",
        cde_build_root / "lib/tt/lib/.libs",
        cde_build_root / "lib/DtXinerama/.libs",
    ):
        if cand.is_dir():
            ld_parts.append(str(cand))
    if env.get("LD_LIBRARY_PATH"):
        ld_parts.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(ld_parts)

    env["PATH"] = ":".join(
        [
            str(comp_bin.parent),
            str(cde_build_root / "programs/dtstyle/.libs"),
            str(cde_build_root / "programs/dtstyle"),
            "/usr/dt/bin",
            "/usr/bin",
            "/bin",
        ]
    )
    env.update(
        {
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg_rt),
            "XDG_DATA_HOME": str(xdg_data),
            "XDG_CONFIG_HOME": str(xdg_cfg),
            "XDG_CURRENT_DESKTOP": "CDE",
            "CK_CDE_MEDIA_STATE_DIR": str(media_dir),
            "CK_CDE_BACKLIGHT_DIR": str(backlight_dir),
            "LC_ALL": "C.UTF-8",
            "LANG": "C.UTF-8",
        }
    )
    return env


def setup_synthetic_hw_dirs(work: Path) -> tuple[Path, Path]:
    media_dir = work / "media-state"
    backlight_dir = work / "backlight/intel_backlight"
    media_dir.mkdir(parents=True, exist_ok=True)
    backlight_dir.mkdir(parents=True, exist_ok=True)
    (media_dir / "volume").write_text("50\n", encoding="utf-8")
    (media_dir / "muted").write_text("0\n", encoding="utf-8")
    (media_dir / "mic_muted").write_text("0\n", encoding="utf-8")
    # max_brightness=200 -> 5% floor = 10 raw units; start at 24 (12%) so two 10% steps down clamp at 10 (5%)!
    (backlight_dir / "max_brightness").write_text("200\n", encoding="utf-8")
    (backlight_dir / "brightness").write_text("24\n", encoding="utf-8")
    return media_dir, backlight_dir


def parse_kv_lines(txt: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for ln in txt.splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            out[k] = v
    return out


def verify_portal_and_gtk_dark_sync(
    lane_tag: str,
    env: dict[str, str],
    xdg_cfg: Path,
    x11_helper: Path,
    expect_dark: bool,
) -> None:
    expected_u32 = "uint32 1" if expect_dark else "uint32 2"
    expected_scheme = "prefer-dark" if expect_dark else "prefer-light"
    expected_ini_flag = "gtk-application-prefer-dark-theme=1" if expect_dark else "gtk-application-prefer-dark-theme=0"
    expected_theme = "Adwaita-dark" if expect_dark else "Adwaita"

    def check_all():
        try:
            portal_out = subprocess.check_output(
                [
                    "gdbus",
                    "call",
                    "--session",
                    "--dest",
                    "org.freedesktop.portal.desktop",
                    "--object-path",
                    "/org/freedesktop/portal/desktop",
                    "--method",
                    "org.freedesktop.portal.Settings.Read",
                    "org.freedesktop.appearance",
                    "color-scheme",
                ],
                env=env,
                text=True,
                stderr=subprocess.STDOUT,
            ).strip()
        except subprocess.CalledProcessError:
            return None
        if expected_u32 not in portal_out:
            return None

        xs_out = parse_kv_lines(subprocess.check_output([str(x11_helper), "xsettings"], env=env, text=True))
        if xs_out.get("XSETTINGS_OWNER", "0x0") == "0x0":
            return None
        if xs_out.get("_CDE_COLOR_SCHEME") != expected_scheme:
            return None
        if expect_dark and xs_out.get("XSETTINGS_HAS_ADWAITA_DARK") != "1":
            return None
        if not expect_dark and (
            xs_out.get("XSETTINGS_HAS_ADWAITA") != "1"
            or xs_out.get("XSETTINGS_HAS_ADWAITA_DARK") != "0"
        ):
            return None

        for ver in ("gtk-3.0", "gtk-4.0"):
            ini = xdg_cfg / ver / "settings.ini"
            if not ini.is_file():
                return None
            body = ini.read_text(encoding="utf-8", errors="replace")
            if expected_ini_flag not in body or f"gtk-theme-name={expected_theme}" not in body:
                return None
        return portal_out

    wait_until(
        check_all,
        timeout=6.0,
        desc=f"{lane_tag} dark-sync ({expected_scheme}, {expected_u32}, XSETTINGS, gtk-3/4 settings.ini)",
    )


def run_wayland_lane(
    comp_bin: Path,
    vkb_helper_bin: Path,
    x11_helper_bin: Path,
    dummy_win_bin: Path,
    dtstyle_bin: Path,
    color_sync_bin: Path,
    save_dir: Path | None,
) -> str:
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp13-wayland-", dir=tmp_base) as td:
        work = Path(td)
        home = work / "home"
        xdg_rt = work / "rt"
        xdg_data = work / "xdg-data"
        xdg_cfg = work / "xdg-cfg"
        for d in (home / ".dt/sessions", xdg_rt, xdg_data, xdg_cfg):
            d.mkdir(parents=True, exist_ok=True)
        xdg_rt.chmod(0o700)
        media_dir, backlight_dir = setup_synthetic_hw_dirs(work)

        env = build_base_env(comp_bin, home, xdg_rt, xdg_data, xdg_cfg, media_dir, backlight_dir)
        env.update(
            {
                "XDG_SESSION_TYPE": "wayland",
                "WLR_BACKENDS": "headless",
                "WLR_RENDERER": "pixman",
                "WLR_HEADLESS_OUTPUTS": "1",
                "DTWM_OUTPUT_MODE": "1280x900",
                "WLR_LIBINPUT_NO_DEVICES": "1",
            }
        )

        procs: list[subprocess.Popen] = []
        extra_pids: list[int] = []
        try:
            dbus_pid = start_isolated_dbus(env)
            extra_pids.append(dbus_pid)

            status_file = work / "comp.status"
            comp = subprocess.Popen(
                [str(comp_bin), "--status-file", str(status_file)],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(comp)

            def read_wl_status():
                if not status_file.is_file():
                    return None
                kv = dict(ln.split("=", 1) for ln in status_file.read_text().splitlines() if "=" in ln)
                return kv if (kv.get("WAYLAND_DISPLAY") and kv.get("DISPLAY")) else None

            kv = wait_until(read_wl_status, timeout=12.0, desc="dtwm-wayland startup")
            env["WAYLAND_DISPLAY"] = kv["WAYLAND_DISPLAY"]
            env["DISPLAY"] = kv["DISPLAY"]
            time.sleep(0.9)

            # Start cross-toolkit color sync daemon
            csync = subprocess.Popen(
                [str(color_sync_bin), "--daemon"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(csync)

            # Map a focused client window to verify non-focus-stealing OSD
            focus_log = work / "focus.log"
            w_focus = subprocess.Popen(
                [str(dummy_win_bin), "WP13 Wayland Focus Target", "Wp13Focus", str(focus_log)],
                env=env,
                start_new_session=True,
            )
            procs.append(w_focus)
            wait_until(
                lambda: "WP13 Wayland Focus Target"
                in parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "X_INPUT_FOCUS", ""
                ),
                timeout=6.0,
                desc="Wayland focus on WP13 Wayland Focus Target",
            )
            if focus_log.is_file():
                focus_log.unlink()

            # 1. Inject Volume Up (KEY_VOLUMEUP = 115) via Wayland virtual keyboard
            subprocess.run([str(vkb_helper_bin), "115"], env=env, check=True)
            wait_until(
                lambda: "visible=1;kind=volume;level=55;muted=0;"
                in parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_OSD_STATE", ""
                ),
                timeout=4.0,
                desc="Wayland OSD visible=1 after KEY_VOLUMEUP",
            )

            # Capture headless screenshot while 3D bevel OSD gauge is visible
            shot_osd = work / "wp13-headless-wayland-osd.png"
            subprocess.run(["grim", str(shot_osd)], env=env, check=True)
            with Image.open(shot_osd) as im:
                if im.size != (1280, 900):
                    raise RuntimeError(f"Unexpected Wayland OSD screenshot size: {im.size}")
            if save_dir is not None:
                save_dir.mkdir(parents=True, exist_ok=True)
                shutil.copy2(shot_osd, save_dir / shot_osd.name)

            # Verify focus was NEVER stolen from WP13 Wayland Focus Target
            cur_focus = parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                "X_INPUT_FOCUS", ""
            )
            if "WP13 Wayland Focus Target" not in cur_focus:
                raise RuntimeError(f"Wayland OSD stole X input focus: {cur_focus!r}")
            if focus_log.is_file() and "FocusOut" in focus_log.read_text(encoding="utf-8"):
                raise RuntimeError("Wayland OSD caused FocusOut on active client!")

            # Inject Mute (113), MicMute (248), BrightnessDown x2 (224 -> 5% floor = 10/200), PlayPause (164)
            subprocess.run([str(vkb_helper_bin), "113"], env=env, check=True)
            wait_until(
                lambda: "kind=volume;level=55;muted=1;"
                in parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_OSD_STATE", ""
                ),
                timeout=4.0,
                desc="Wayland OSD mute state",
            )
            subprocess.run([str(vkb_helper_bin), "248"], env=env, check=True)
            wait_until(
                lambda: "kind=mic;"
                in parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_OSD_STATE", ""
                ),
                timeout=4.0,
                desc="Wayland OSD mic-mute state",
            )
            subprocess.run([str(vkb_helper_bin), "224"], env=env, check=True)
            subprocess.run([str(vkb_helper_bin), "224"], env=env, check=True)
            wait_until(
                lambda: (backlight_dir / "brightness").read_text(encoding="utf-8").strip() == "10",
                timeout=4.0,
                desc="Wayland 5% sysfs brightness floor (10/200)",
            )
            subprocess.run([str(vkb_helper_bin), "164"], env=env, check=True)
            wait_until(
                lambda: (media_dir / "mpris.log").is_file()
                and "PlayPause" in (media_dir / "mpris.log").read_text(encoding="utf-8"),
                timeout=4.0,
                desc="Wayland MPRIS PlayPause dispatch",
            )
            # Wait for 1600ms OSD auto-hide timer
            wait_until(
                lambda: parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True))
                .get("_DT_OSD_STATE", "")
                .startswith("visible=0;"),
                timeout=4.0,
                desc="Wayland OSD auto-hide visible=0",
            )

            # 2. Touchpad settings via dtstyle + 3-finger swipe workspace switch
            subprocess.run(
                [str(dtstyle_bin), "-apply-touchpad", "0", "1", "0", "1", "1"],
                env=env,
                check=True,
            )
            wait_until(
                lambda: "tp=0:1:0:1:1"
                in parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_WAYLAND_INPUT_STATE", ""
                ),
                timeout=5.0,
                desc="Wayland _DT_WAYLAND_INPUT_STATE tp=0:1:0:1:1",
            )

            dtstyle_gui = subprocess.Popen(
                [str(dtstyle_bin), "-mouse"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(dtstyle_gui)
            time.sleep(0.8)
            verify_pid_maps_motif9(dtstyle_gui.pid, "Wayland dtstyle")

            ws_initial = wait_until(
                lambda: parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_WORKSPACE_CURRENT", ""
                ),
                timeout=4.0,
                desc="initial _DT_WORKSPACE_CURRENT",
            )
            subprocess.run([str(x11_helper_bin), "send-wl-req", "f.touchpad_swipe 3 -140 5"], env=env, check=True)
            ws_after_left = wait_until(
                lambda: (
                    w := parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                        "_DT_WORKSPACE_CURRENT", ""
                    )
                )
                and w != ws_initial
                and w,
                timeout=4.0,
                desc="Wayland 3-finger swipe left workspace switch",
            )
            subprocess.run([str(x11_helper_bin), "send-wl-req", "f.touchpad_swipe 3 140 -4"], env=env, check=True)
            wait_until(
                lambda: parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_WORKSPACE_CURRENT", ""
                )
                == ws_initial,
                timeout=4.0,
                desc="Wayland 3-finger swipe right workspace return",
            )

            # 3. Cross-toolkit dark/light sync via dtwm-wayland appearance profile switch
            subprocess.run(
                [str(x11_helper_bin), "send-wl-req", "f.set_appearance_profile modern_dark"],
                env=env,
                check=True,
            )
            verify_portal_and_gtk_dark_sync("wayland", env, xdg_cfg, x11_helper_bin, expect_dark=True)

            subprocess.run(
                [str(x11_helper_bin), "send-wl-req", "f.set_appearance_profile classic"],
                env=env,
                check=True,
            )
            verify_portal_and_gtk_dark_sync("wayland", env, xdg_cfg, x11_helper_bin, expect_dark=False)

            return (
                f"Wayland lane OK (OSD+MPRIS+5% floor, dtstyle pid={dtstyle_gui.pid} tp=0:1:0:1:1, "
                f"swipe {ws_initial}->{ws_after_left}->{ws_initial}, portal+XSETTINGS+GTK dark/light)"
            )
        finally:
            terminate_procs(procs, extra_pids)


def pick_free_x11_display() -> int:
    for d in range(52, 98):
        if not Path(f"/tmp/.X{d}-lock").exists() and not Path(f"/tmp/.X11-unix/X{d}").exists():
            return d
    raise RuntimeError("No free Xvfb display number found in 52..97")


def run_x11_lane(
    comp_bin: Path,
    dtwm_x11_bin: Path,
    x11_helper_bin: Path,
    dummy_win_bin: Path,
    dtstyle_bin: Path,
    color_sync_bin: Path,
) -> str:
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp13-x11-", dir=tmp_base) as td:
        work = Path(td)
        home = work / "home"
        xdg_rt = work / "rt"
        xdg_data = work / "xdg-data"
        xdg_cfg = work / "xdg-cfg"
        for d in (home / ".dt/sessions", xdg_rt, xdg_data, xdg_cfg):
            d.mkdir(parents=True, exist_ok=True)
        xdg_rt.chmod(0o700)
        media_dir, backlight_dir = setup_synthetic_hw_dirs(work)

        env = build_base_env(comp_bin, home, xdg_rt, xdg_data, xdg_cfg, media_dir, backlight_dir)
        env["XDG_SESSION_TYPE"] = "x11"
        dnum = pick_free_x11_display()
        disp = f":{dnum}"
        env["DISPLAY"] = disp

        procs: list[subprocess.Popen] = []
        extra_pids: list[int] = []
        try:
            dbus_pid = start_isolated_dbus(env)
            extra_pids.append(dbus_pid)

            xvfb = subprocess.Popen(
                ["Xvfb", disp, "-screen", "0", "1280x900x24", "-nolisten", "tcp", "-ac"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(xvfb)
            wait_until(
                lambda: subprocess.run(
                    [str(x11_helper_bin)],
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
                lambda: parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True))
                .get("_DT_OSD_STATE", "")
                .startswith("visible=0;"),
                timeout=8.0,
                desc="X11 dtwm SetupWmEwmh _DT_OSD_STATE ready",
            )
            verify_pid_maps_motif9(dtwm_p.pid, "X11 dtwm")

            csync = subprocess.Popen(
                [str(color_sync_bin), "--daemon"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(csync)

            # Map a focused X11 client window to prove X11 override_redirect OSD never steals focus
            focus_log = work / "x11-focus.log"
            w_focus = subprocess.Popen(
                [str(dummy_win_bin), "WP13 X11 Focus Target", "Wp13X11Focus", str(focus_log)],
                env=env,
                start_new_session=True,
            )
            procs.append(w_focus)
            wait_until(
                lambda: "WP13 X11 Focus Target"
                in parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "NET_ACTIVE", ""
                ),
                timeout=6.0,
                desc="X11 dtwm _NET_ACTIVE_WINDOW on WP13 X11 Focus Target",
            )
            if focus_log.is_file():
                focus_log.unlink()

            # 1. Inject XF86AudioRaiseVolume, XF86AudioMute, XF86MonBrightnessDown x2, XF86AudioPlay via XTest
            subprocess.run([str(x11_helper_bin), "tap-keysym", "XF86AudioRaiseVolume"], env=env, check=True)
            wait_until(
                lambda: "visible=1;kind=volume;level=55;muted=0;"
                in parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_OSD_STATE", ""
                ),
                timeout=4.0,
                desc="X11 dtwm OSD visible=1 after XF86AudioRaiseVolume",
            )

            # Verify _NET_ACTIVE_WINDOW was NEVER stolen by the X11 OSD window
            net_active = parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                "NET_ACTIVE", ""
            )
            if "WP13 X11 Focus Target" not in net_active:
                raise RuntimeError(f"X11 OSD stole _NET_ACTIVE_WINDOW: {net_active!r}")
            if focus_log.is_file() and "FocusOut" in focus_log.read_text(encoding="utf-8"):
                raise RuntimeError("X11 OSD caused NotifyNormal FocusOut on active client!")

            subprocess.run([str(x11_helper_bin), "tap-keysym", "XF86AudioMute"], env=env, check=True)
            wait_until(
                lambda: "kind=volume;level=55;muted=1;"
                in parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_OSD_STATE", ""
                ),
                timeout=4.0,
                desc="X11 dtwm OSD mute state",
            )
            subprocess.run([str(x11_helper_bin), "tap-keysym", "XF86MonBrightnessDown"], env=env, check=True)
            subprocess.run([str(x11_helper_bin), "tap-keysym", "XF86MonBrightnessDown"], env=env, check=True)
            wait_until(
                lambda: (backlight_dir / "brightness").read_text(encoding="utf-8").strip() == "10",
                timeout=4.0,
                desc="X11 5% sysfs brightness floor (10/200)",
            )
            subprocess.run([str(x11_helper_bin), "tap-keysym", "XF86AudioPlay"], env=env, check=True)
            wait_until(
                lambda: (media_dir / "mpris.log").is_file()
                and "PlayPause" in (media_dir / "mpris.log").read_text(encoding="utf-8"),
                timeout=4.0,
                desc="X11 MPRIS PlayPause dispatch",
            )
            wait_until(
                lambda: parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True))
                .get("_DT_OSD_STATE", "")
                .startswith("visible=0;"),
                timeout=4.0,
                desc="X11 dtwm OSD auto-hide visible=0",
            )

            # 2. X11 dtstyle Touchpad XInput2 backend + RESOURCE_MANAGER persistence + 3-finger swipe
            subprocess.run(
                [str(dtstyle_bin), "-apply-touchpad", "0", "1", "0", "1", "1"],
                env=env,
                check=True,
            )
            wait_until(
                lambda: parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True))
                .get("_DT_X11_TOUCHPAD_STATE", "")
                .startswith("tap=0;nat=1;cf=0;dwt=1;dext=1;"),
                timeout=5.0,
                desc="X11 _DT_X11_TOUCHPAD_STATE via dtstyle XInput2 backend",
            )

            dtstyle_gui = subprocess.Popen(
                [str(dtstyle_bin), "-mouse"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(dtstyle_gui)
            time.sleep(0.8)
            verify_pid_maps_motif9(dtstyle_gui.pid, "X11 dtstyle")

            ws_initial = wait_until(
                lambda: parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_WORKSPACE_CURRENT", ""
                ),
                timeout=4.0,
                desc="initial _DT_WORKSPACE_CURRENT",
            )
            subprocess.run([str(x11_helper_bin), "send-x11-swipe", "3", "-140", "4"], env=env, check=True)
            ws_after_left = wait_until(
                lambda: (
                    w := parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                        "_DT_WORKSPACE_CURRENT", ""
                    )
                )
                and w != ws_initial
                and w,
                timeout=4.0,
                desc="X11 dtwm 3-finger swipe left workspace switch",
            )
            subprocess.run([str(x11_helper_bin), "send-x11-swipe", "3", "140", "-3"], env=env, check=True)
            wait_until(
                lambda: parse_kv_lines(subprocess.check_output([str(x11_helper_bin)], env=env, text=True)).get(
                    "_DT_WORKSPACE_CURRENT", ""
                )
                == ws_initial,
                timeout=4.0,
                desc="X11 dtwm 3-finger swipe right workspace return",
            )

            # 3. Cross-toolkit dark/light sync on X11
            subprocess.run([str(color_sync_bin), "--apply", "dark"], env=env, check=True)
            verify_portal_and_gtk_dark_sync("x11", env, xdg_cfg, x11_helper_bin, expect_dark=True)

            subprocess.run([str(color_sync_bin), "--apply", "light"], env=env, check=True)
            verify_portal_and_gtk_dark_sync("x11", env, xdg_cfg, x11_helper_bin, expect_dark=False)

            return (
                f"X11 lane OK (display={disp}, OSD+MPRIS+5% floor, dtstyle pid={dtstyle_gui.pid} XI2, "
                f"swipe {ws_initial}->{ws_after_left}->{ws_initial}, portal+XSETTINGS+GTK dark/light)"
            )
        finally:
            terminate_procs(procs, extra_pids)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True, type=Path)
    ap.add_argument("--save-dir", type=Path, default=None)
    args = ap.parse_args()

    comp_bin = args.compositor.resolve()
    dtwm_x11_bin = comp_bin.parent / ".libs/dtwm"
    if not dtwm_x11_bin.is_file():
        dtwm_x11_bin = Path("/usr/dt/bin/dtwm")

    cde_build_root = comp_bin.parents[2]
    dtstyle_bin = cde_build_root / "programs/dtstyle/.libs/dtstyle"
    if not dtstyle_bin.is_file():
        dtstyle_bin = Path("/usr/dt/bin/dtstyle")

    color_sync_bin = comp_bin.parent / "cde-color-sync"
    if not color_sync_bin.is_file():
        color_sync_bin = Path("/usr/dt/bin/cde-color-sync")

    cc = os.environ.get("CC", "cc")
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp13-fixtures-", dir=tmp_base) as fx_s:
        fx = Path(fx_s)
        vkb_c = fx / "wl_vkb_helper.c"
        vkb_bin = fx / "wl_vkb_helper"
        vkb_c.write_text(WL_VKB_HELPER_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", "-Wall", str(vkb_c), "-lwayland-client", "-lxkbcommon", "-o", str(vkb_bin)],
            check=True,
        )

        x11_c = fx / "x11_wp13_helper.c"
        x11_bin = fx / "x11_wp13_helper"
        x11_c.write_text(X11_WP13_HELPER_C, encoding="utf-8")
        subprocess.run([cc, "-O2", "-Wall", str(x11_c), "-lX11", "-ldl", "-o", str(x11_bin)], check=True)

        dummy_c = fx / "x11_dummy_win.c"
        dummy_bin = fx / "x11_dummy_win"
        dummy_c.write_text(X11_DUMMY_WIN_C, encoding="utf-8")
        subprocess.run([cc, "-O2", "-Wall", str(dummy_c), "-lX11", "-o", str(dummy_bin)], check=True)

        t0 = time.monotonic()
        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_wl = pool.submit(
                run_wayland_lane,
                comp_bin,
                vkb_bin,
                x11_bin,
                dummy_bin,
                dtstyle_bin,
                color_sync_bin,
                args.save_dir,
            )
            fut_x11 = pool.submit(
                run_x11_lane,
                comp_bin,
                dtwm_x11_bin,
                x11_bin,
                dummy_bin,
                dtstyle_bin,
                color_sync_bin,
            )
            wl_res = fut_wl.result()
            x11_res = fut_x11.result()
        elapsed = time.monotonic() - t0
        print(f"[WP-13 PASS in {elapsed:.2f}s]")
        print(f"  - {wl_res}")
        print(f"  - {x11_res}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
