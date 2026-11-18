#!/usr/bin/env python3
"""Automated Wayland XWayland Motif menu reliability regression test:
1. Verifies `dtfile` maps CK Core Motif ABI 9 (`libXm.so.9`) under `dtwm-wayland`.
2. Mid-press `zwlr_virtual_pointer_v1` crash/disconnect recovery:
   - Opens `dtfile`'s right-click popup via `BTN_RIGHT` press and abruptly terminates
     the virtual pointer client without releasing `BTN_RIGHT`.
   - Verifies `dtwm-wayland` synthesizes button release, clears `pointer_buttons_down`,
     and dismisses the orphaned Xwayland override-redirect popup menu.
3. Persistent `wayvnc`-style virtual pointer stuck-button auto-heal:
   - Leaves `BTN_RIGHT` held on a persistent `zwlr_virtual_pointer_v1` (`> 600ms`,
     simulating browser context-menu `mouseup` loss or VNC client reconnect), then
     clicks `File -> Go Up` with `BTN_LEFT`.
   - Verifies `dtwm-wayland` auto-releases the stale button, routes `wl_pointer`
     focus onto the override-redirect `XmMenuShell` (`fileMenu`), and navigates
     `leaf2 -> leaf1`.
4. Stationary click after popup dismissal (`_MOTIF_DRAG_WINDOW` filter + `notify_enter`):
   - Clicks the window titlebar so `dismiss_open_client_popups` runs while Motif's
     off-screen `10x10+-100+-100` `_MOTIF_DRAG_WINDOW` is mapped, then performs a
     stationary `BTN_LEFT` click on `File` and verifies `fileMenu` posts immediately.
5. Live `wlr-randr` output modeset + HiDPI scale (`1280x720 -> 3120x1952 @ 1.75`,
   logical `1782x1115`) while `dtfile` is already running:
   - Drags `dtfile` into the expanded screen region `(1100, 630)` beyond the initial
     `1280x720` boot screen bounds.
   - Verifies `libDtSvc` / `dtfile` root `ConfigureNotify` updates `Screen` bounds so
     `fileMenu` opens directly beneath `File` (`mx >= 1080, my >= 635`) rather than
     clamping back inside `1280x720`.
   - Verifies both **two-click traversal** (`Click File` -> `Click Go Up`:
     `leaf1 -> sub1`) and **press-drag-release** (`Press File` -> drag down ->
     `Release on Go Home`: `sub1 -> home_dir`).
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


WL_VPTR_HELPER_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

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
} InputState;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t ver) {
    InputState *st = (InputState *)data;
    (void)ver;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !st->seat) {
        st->seat = (struct wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, "zwlr_virtual_pointer_manager_v1") == 0 && !st->vptr_mgr) {
        st->vptr_mgr = (struct wl_proxy *)wl_registry_bind(
            reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) {
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = { on_global, on_global_remove };

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

static uint32_t parse_btn(const char *s) {
    if (strcmp(s, "right") == 0) return BTN_RIGHT;
    if (strcmp(s, "middle") == 0) return BTN_MIDDLE;
    return BTN_LEFT;
}

static void vp_move(struct wl_display *dpy, struct wl_proxy *vp,
                    uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    wl_proxy_marshal(vp, 1, now_ms(), x, y, w, h);
    wl_proxy_marshal(vp, 4);
    wl_display_roundtrip(dpy);
}

static void vp_btn(struct wl_display *dpy, struct wl_proxy *vp,
                   uint32_t btn, uint32_t state) {
    wl_proxy_marshal(vp, 2, now_ms(), btn, state);
    wl_proxy_marshal(vp, 4);
    wl_display_roundtrip(dpy);
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 2;

    InputState st = {0};
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, &st);
    wl_display_roundtrip(dpy);
    if (!st.seat || !st.vptr_mgr) return 3;

    struct wl_proxy *vp = wl_proxy_marshal_flags(
        st.vptr_mgr, 0, &zwlr_virtual_pointer_v1_interface, 1, 0, st.seat, NULL);
    wl_display_roundtrip(dpy);

    if (strcmp(argv[1], "crash-press") == 0 && argc >= 7) {
        uint32_t x = (uint32_t)strtoul(argv[2], NULL, 0);
        uint32_t y = (uint32_t)strtoul(argv[3], NULL, 0);
        uint32_t w = (uint32_t)strtoul(argv[4], NULL, 0);
        uint32_t h = (uint32_t)strtoul(argv[5], NULL, 0);
        uint32_t btn = parse_btn(argv[6]);
        vp_move(dpy, vp, x, y, w, h);
        usleep(40000);
        vp_btn(dpy, vp, btn, 1u);
        /* Hold past Motif multiClickTime (500ms) into drag mode, move outside popup,
         * then crash without sending button release. */
        usleep(600000);
        vp_move(dpy, vp, x > 40 ? x - 40 : 10, y > 40 ? y - 40 : 10, w, h);
        usleep(40000);
        _exit(0);
    }

    if (strcmp(argv[1], "daemon") == 0) {
        char line[256];
        while (fgets(line, sizeof(line), stdin) != NULL) {
            char cmd[64] = {0}, bname[32] = {0};
            unsigned int x = 0, y = 0, x2 = 0, y2 = 0, w = 0, h = 0, st_val = 0;
            if (sscanf(line, "%63s", cmd) != 1) continue;
            if (strcmp(cmd, "move") == 0 &&
                sscanf(line, "%*s %u %u %u %u", &x, &y, &w, &h) == 4) {
                vp_move(dpy, vp, x, y, w, h);
                printf("OK\n");
                fflush(stdout);
            } else if (strcmp(cmd, "button") == 0 &&
                       sscanf(line, "%*s %31s %u", bname, &st_val) == 2) {
                vp_btn(dpy, vp, parse_btn(bname), st_val ? 1u : 0u);
                printf("OK\n");
                fflush(stdout);
            } else if (strcmp(cmd, "click") == 0 &&
                       sscanf(line, "%*s %u %u %u %u %31s", &x, &y, &w, &h, bname) == 5) {
                uint32_t btn = parse_btn(bname);
                vp_move(dpy, vp, x, y, w, h);
                usleep(35000);
                vp_btn(dpy, vp, btn, 1u);
                usleep(45000);
                vp_btn(dpy, vp, btn, 0u);
                printf("OK\n");
                fflush(stdout);
            } else if (strcmp(cmd, "stationary-click") == 0 &&
                       sscanf(line, "%*s %31s", bname) == 1) {
                uint32_t btn = parse_btn(bname);
                vp_btn(dpy, vp, btn, 1u);
                usleep(45000);
                vp_btn(dpy, vp, btn, 0u);
                printf("OK\n");
                fflush(stdout);
            } else if (strcmp(cmd, "drag") == 0 &&
                       sscanf(line, "%*s %u %u %u %u %u %u %31s",
                              &x, &y, &x2, &y2, &w, &h, bname) == 7) {
                uint32_t btn = parse_btn(bname);
                vp_move(dpy, vp, x, y, w, h);
                usleep(40000);
                vp_btn(dpy, vp, btn, 1u);
                usleep(60000);
                for (unsigned int s = 1; s <= 6; ++s) {
                    uint32_t cx = (uint32_t)((int)x + (((int)x2 - (int)x) * (int)s) / 6);
                    uint32_t cy = (uint32_t)((int)y + (((int)y2 - (int)y) * (int)s) / 6);
                    vp_move(dpy, vp, cx, cy, w, h);
                    usleep(30000);
                }
                usleep(50000);
                vp_btn(dpy, vp, btn, 0u);
                printf("OK\n");
                fflush(stdout);
            } else if (strcmp(cmd, "quit") == 0) {
                break;
            }
        }
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


X11_QUERY_C = r"""
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>

static void walk(Display *dpy, Window w) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (wa.map_state == IsViewable) {
        Window child = None;
        int rx = 0, ry = 0;
        XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child);
        char *wm_name = NULL;
        XFetchName(dpy, w, &wm_name);
        XClassHint ch = {0};
        XGetClassHint(dpy, w, &ch);
        if (wa.override_redirect && wa.width > 2 && wa.height > 2 &&
            rx + wa.width > 0 && ry + wa.height > 0) {
            printf("POPUP|0x%lx|%d|%d|%d|%d|%s|%s\n",
                   w, rx, ry, wa.width, wa.height,
                   ch.res_name ? ch.res_name : "",
                   ch.res_class ? ch.res_class : "");
        } else if (!wa.override_redirect && ch.res_class &&
                   strcmp(ch.res_class, "Dtfile") == 0 && wa.width >= 180) {
            printf("DTFILE|0x%lx|%d|%d|%d|%d|%s\n",
                   w, rx, ry, wa.width, wa.height,
                   wm_name ? wm_name : "");
        }
        if (wm_name) XFree(wm_name);
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    if (wa.map_state != IsViewable && w != DefaultRootWindow(dpy)) return;
    Window root_r, parent_r, *kids = NULL;
    unsigned int nkids = 0;
    if (XQueryTree(dpy, w, &root_r, &parent_r, &kids, &nkids) && kids) {
        for (unsigned int i = 0; i < nkids; ++i) walk(dpy, kids[i]);
        XFree(kids);
    }
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Screen *scr = DefaultScreenOfDisplay(dpy);
    printf("ROOT|%d|%d\n", scr->width, scr->height);
    walk(dpy, DefaultRootWindow(dpy));
    XCloseDisplay(dpy);
    return 0;
}
"""


def compile_helper(cc: str, src: str, out: Path, pkg_libs: list[str]) -> None:
    c_file = out.with_suffix(".c")
    c_file.write_text(src, encoding="utf-8")
    cflags: list[str] = []
    libs: list[str] = []
    if pkg_libs:
        cflags = subprocess.check_output(
            ["pkg-config", "--cflags", *pkg_libs], text=True
        ).split()
        libs = subprocess.check_output(
            ["pkg-config", "--libs", *pkg_libs], text=True
        ).split()
    subprocess.check_call([cc, "-O2", "-Wall", str(c_file), *cflags, *libs, "-o", str(out)])


def resolve_dtfile_bin(compositor: Path) -> Path:
    candidates = [
        compositor.parent.parent / "dtfile" / ".libs" / "dtfile",
        compositor.parent.parent / "dtfile" / "dtfile",
        Path("/usr/dt/bin/dtfile"),
    ]
    for cand in candidates:
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand.resolve()
    raise RuntimeError("Unable to locate built dtfile binary")


def verify_dtfile_motif_abi9(home_dir: Path) -> int:
    deadline = time.monotonic() + 10.0
    home_tag = f"HOME={home_dir}\x00"
    while time.monotonic() < deadline:
        for ent in Path("/proc").iterdir():
            if not ent.name.isdigit():
                continue
            try:
                if (ent / "comm").read_text(encoding="utf-8").strip() != "dtfile":
                    continue
                ebytes = (ent / "environ").read_bytes().decode("utf-8", errors="replace")
                if home_tag not in ebytes:
                    continue
                txt = (ent / "maps").read_text(encoding="utf-8", errors="replace")
                if "libXm.so.9" in txt:
                    if "libXm.so.4" in txt:
                        raise AssertionError(f"PID {ent.name} mapped system libXm.so.4:\n{txt}")
                    return int(ent.name)
            except OSError:
                continue
        time.sleep(0.1)
    raise AssertionError("dtfile did not map CK Core Motif ABI 9 (libXm.so.9)")


class PersistentVPtr:
    def __init__(self, helper: Path, env: dict[str, str]) -> None:
        self.proc = subprocess.Popen(
            [str(helper), "daemon"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            env=env,
        )

    def cmd(self, line: str) -> None:
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        resp = self.proc.stdout.readline().strip()
        if resp != "OK":
            raise RuntimeError(f"vptr daemon failed on {line!r}: {resp!r}")

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                assert self.proc.stdin is not None
                self.proc.stdin.write("quit\n")
                self.proc.stdin.flush()
                self.proc.wait(timeout=2.0)
            except Exception:
                self.proc.kill()


def query_x11_state(x11_query: Path, env: dict[str, str]) -> tuple[tuple[int, int], list[dict], list[dict]]:
    out = subprocess.check_output([str(x11_query)], text=True, env=env, timeout=5.0)
    root_wh = (0, 0)
    dtfiles: list[dict] = []
    popups: list[dict] = []
    for line in out.splitlines():
        parts = line.split("|")
        if parts[0] == "ROOT" and len(parts) >= 3:
            root_wh = (int(parts[1]), int(parts[2]))
        elif parts[0] == "DTFILE" and len(parts) >= 7:
            dtfiles.append({
                "win": parts[1],
                "x": int(parts[2]),
                "y": int(parts[3]),
                "w": int(parts[4]),
                "h": int(parts[5]),
                "title": parts[6],
            })
        elif parts[0] == "POPUP" and len(parts) >= 8:
            popups.append({
                "win": parts[1],
                "x": int(parts[2]),
                "y": int(parts[3]),
                "w": int(parts[4]),
                "h": int(parts[5]),
                "name": parts[6],
                "cls": parts[7],
            })
    return root_wh, dtfiles, popups


def wait_for(desc: str, timeout: float, pred):
    deadline = time.monotonic() + timeout
    last_val = None
    while time.monotonic() < deadline:
        last_val = pred()
        if last_val:
            return last_val
        time.sleep(0.08)
    raise AssertionError(f"Timed out waiting for {desc} (last={last_val!r})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True, type=Path)
    args = ap.parse_args()

    compositor = args.compositor.resolve()
    if not compositor.is_file():
        raise FileNotFoundError(f"Compositor not found: {compositor}")

    wlr_randr = shutil.which("wlr-randr")
    if not wlr_randr:
        print("SKIP: wlr-randr not installed", file=sys.stderr)
        return 0

    cc = os.environ.get("CC", "cc")
    dtfile_bin = resolve_dtfile_bin(compositor)

    with tempfile.TemporaryDirectory(prefix="dtwm-wl-menus-") as tmp:
        tdir = Path(tmp)
        vptr_bin = tdir / "wl_vptr_helper"
        x11q_bin = tdir / "x11_query"
        compile_helper(cc, WL_VPTR_HELPER_C, vptr_bin, ["wayland-client"])
        compile_helper(cc, X11_QUERY_C, x11q_bin, ["x11"])

        home_dir = tdir / "home"
        leaf2_dir = home_dir / "sub1" / "leaf1" / "leaf2"
        leaf2_dir.mkdir(parents=True, exist_ok=True)
        (leaf2_dir / "sample.txt").write_text("menu test\n", encoding="utf-8")

        xdg_run = tdir / "xdg"
        xdg_run.mkdir(mode=0o700, exist_ok=True)
        status_file = tdir / "compositor.status"
        comp_log = tdir / "compositor.log"

        base_env = os.environ.copy()
        lib_dirs = ["/usr/dt/lib"]
        cde_build_root = compositor.parent.parent.parent
        for sub in (
            "lib/DtSvc/.libs",
            "lib/DtWidget/.libs",
            "lib/DtTerm/.libs",
            "lib/DtHelp/.libs",
            "lib/DtSearch/.libs",
            "lib/DtPrint/.libs",
            "lib/csa/.libs",
            "lib/tt/lib/.libs",
        ):
            cand = cde_build_root / sub
            if cand.is_dir():
                lib_dirs.insert(0, str(cand))
        old_ld = base_env.get("LD_LIBRARY_PATH", "")
        base_env["LD_LIBRARY_PATH"] = ":".join(lib_dirs + ([old_ld] if old_ld else []))
        base_env.update({
            "HOME": str(home_dir),
            "XDG_RUNTIME_DIR": str(xdg_run),
            "XDG_CONFIG_HOME": str(home_dir / ".config"),
            "XDG_CACHE_HOME": str(home_dir / ".cache"),
            "XDG_DATA_HOME": str(home_dir / ".local" / "share"),
            "WLR_BACKENDS": "headless",
            "WLR_RENDERER": "pixman",
            "WLR_WL_OUTPUTS": "1",
            "NO_AT_BRIDGE": "1",
        })

        comp_proc: subprocess.Popen | None = None
        tt_proc: subprocess.Popen | None = None
        dtfile_proc: subprocess.Popen | None = None
        vptr: PersistentVPtr | None = None

        try:
            # Isolated session D-Bus so dtfile never collides with an existing session
            dbus_out = subprocess.check_output(
                ["dbus-daemon", "--session", "--fork", "--print-address=1", "--print-pid=1"],
                text=True,
                env=base_env,
            ).splitlines()
            base_env["DBUS_SESSION_BUS_ADDRESS"] = dbus_out[0].strip()
            dbus_pid = int(dbus_out[1].strip())

            with comp_log.open("w", encoding="utf-8") as logf:
                comp_proc = subprocess.Popen(
                    [str(compositor), "--status-file", str(status_file)],
                    stdout=logf,
                    stderr=subprocess.STDOUT,
                    env=base_env,
                    start_new_session=True,
                )

            wait_for("dtwm-wayland status file", 12.0, lambda: status_file.is_file() and status_file.stat().st_size > 0)
            for line in status_file.read_text(encoding="utf-8").splitlines():
                if "=" in line:
                    k, v = line.strip().split("=", 1)
                    base_env[k] = v

            # Start at 1280x720 @ scale 1.0
            subprocess.check_call(
                [wlr_randr, "--output", "HEADLESS-1", "--custom-mode", "1280x720@60Hz", "--scale", "1.0"],
                env=base_env,
            )

            # Start ToolTalk session daemon required by dtfile window views
            ttsession_cand = cde_build_root / "lib" / "tt" / "bin" / "ttsession" / ".libs" / "ttsession"
            ttsession_bin = str(ttsession_cand) if ttsession_cand.is_file() else "/usr/dt/bin/ttsession"
            tt_proc = subprocess.Popen(
                [ttsession_bin, "-s"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=base_env,
                start_new_session=True,
            )
            time.sleep(0.5)

            dtfile_log = tdir / "dtfile.log"
            with dtfile_log.open("w", encoding="utf-8") as dflog:
                dtfile_proc = subprocess.Popen(
                    [str(dtfile_bin), "-dir", str(leaf2_dir)],
                    stdout=dflog,
                    stderr=subprocess.STDOUT,
                    env=base_env,
                    start_new_session=True,
                )

            verify_dtfile_motif_abi9(home_dir)

            def get_dtfile_win():
                _, wins, _ = query_x11_state(x11q_bin, base_env)
                for w in wins:
                    if "leaf2" in w["title"] or w["w"] >= 250:
                        return w
                return None

            win = wait_for("dtfile leaf2 window", 15.0, get_dtfile_win)
            time.sleep(0.4)
            _, wins, _ = query_x11_state(x11q_bin, base_env)
            win = wins[0]
            ext_w, ext_h = 1280, 720

            # 1) Crash virtual pointer while holding BTN_RIGHT inside dtfile's file view
            cx = win["x"] + win["w"] // 2
            cy = win["y"] + (win["h"] * 2) // 3
            subprocess.check_call(
                [str(vptr_bin), "crash-press", str(cx), str(cy), str(ext_w), str(ext_h), "right"],
                env=base_env,
            )

            def no_orphaned_popups():
                _, _, pops = query_x11_state(x11q_bin, base_env)
                return pops == []

            wait_for("orphaned popup dismissal after vptr crash", 5.0, no_orphaned_popups)

            # 2) Persistent wayvnc-style virtual pointer with stuck BTN_RIGHT -> auto-heal on File -> Go Up
            vptr = PersistentVPtr(vptr_bin, base_env)
            vptr.cmd(f"move {cx} {cy} {ext_w} {ext_h}")
            vptr.cmd("button right 1")
            # Exceed Motif multiClickTime (500ms) so auto-heal triggers cleanly on next button press
            time.sleep(0.65)

            file_x = win["x"] + 24
            file_y = win["y"] + 14
            vptr.cmd(f"click {file_x} {file_y} {ext_w} {ext_h} left")

            def get_file_menu():
                _, _, pops = query_x11_state(x11q_bin, base_env)
                for p in pops:
                    if p["cls"] == "XmMenuShell" or p["h"] >= 120:
                        return p
                return None

            fmenu = wait_for("File pulldown menu after stuck BTN_RIGHT auto-heal", 5.0, get_file_menu)
            go_up_x = fmenu["x"] + min(60, fmenu["w"] // 2)
            go_up_y = fmenu["y"] + int(fmenu["h"] * 0.31)
            vptr.cmd(f"click {go_up_x} {go_up_y} {ext_w} {ext_h} left")

            def navigated_to(suffix: str):
                _, ws, _ = query_x11_state(x11q_bin, base_env)
                for w in ws:
                    if w["title"].endswith(suffix):
                        return w
                return None

            win = wait_for("dtfile navigation leaf2 -> leaf1", 6.0, lambda: navigated_to("leaf1"))

            # 3) Stationary click on File after clicking frame titlebar (dismiss_open_client_popups + notify_enter)
            title_x = win["x"] + win["w"] // 2
            title_y = max(4, win["y"] - 10)
            vptr.cmd(f"click {title_x} {title_y} {ext_w} {ext_h} left")
            time.sleep(0.15)
            file_x = win["x"] + 24
            file_y = win["y"] + 14
            vptr.cmd(f"move {file_x} {file_y} {ext_w} {ext_h}")
            time.sleep(0.08)
            vptr.cmd("stationary-click left")
            fmenu = wait_for("File pulldown menu on stationary click", 5.0, get_file_menu)
            # Dismiss by clicking titlebar
            vptr.cmd(f"click {title_x} {title_y} {ext_w} {ext_h} left")
            wait_for("File pulldown menu dismissed", 5.0, no_orphaned_popups)

            # 4) Live wlr-randr output resize + HiDPI scale (1280x720 -> 3120x1952 @ 1.75 = 1782x1115)
            subprocess.check_call(
                [wlr_randr, "--output", "HEADLESS-1", "--custom-mode", "3120x1952@60Hz", "--scale", "1.75"],
                env=base_env,
            )

            def xwayland_resized():
                rwh, _, _ = query_x11_state(x11q_bin, base_env)
                return rwh if rwh[0] >= 1700 and rwh[1] >= 1050 else None

            ext_w, ext_h = wait_for("Xwayland root resize >= 1700x1050", 6.0, xwayland_resized)

            # Drag dtfile window into expanded desktop area (beyond boot 1280x720 bounds)
            _, wins, _ = query_x11_state(x11q_bin, base_env)
            win = wins[0]
            drag_start_x = win["x"] + 120
            drag_start_y = max(4, win["y"] - 10)
            target_frame_x = 1320
            target_frame_y = 640
            vptr.cmd(
                f"drag {drag_start_x} {drag_start_y} {target_frame_x} {target_frame_y} {ext_w} {ext_h} left"
            )
            time.sleep(0.25)
            _, wins, _ = query_x11_state(x11q_bin, base_env)
            win = wins[0]
            if win["x"] < 1000 or win["y"] < 600:
                raise AssertionError(f"Expected dtfile in expanded area, got {win!r}")

            # 4a) Two-click File -> Go Up in expanded screen area (verifies no stale 1280x720 menu clamp)
            file_x = win["x"] + 24
            file_y = win["y"] + 14
            vptr.cmd(f"click {file_x} {file_y} {ext_w} {ext_h} left")
            fmenu = wait_for("File menu in expanded post-modeset region", 5.0, get_file_menu)
            if abs(fmenu["x"] - win["x"]) > 80 or fmenu["y"] < win["y"]:
                raise AssertionError(
                    f"Menu clamped to stale screen bounds! dtfile={win!r}, fileMenu={fmenu!r}"
                )
            go_up_x = fmenu["x"] + min(60, fmenu["w"] // 2)
            go_up_y = fmenu["y"] + int(fmenu["h"] * 0.31)
            vptr.cmd(f"click {go_up_x} {go_up_y} {ext_w} {ext_h} left")
            win = wait_for("dtfile two-click navigation leaf1 -> sub1", 6.0, lambda: navigated_to("sub1"))

            # 4b) Press-drag-release File -> Go Home in expanded screen area
            time.sleep(0.6)
            file_x = win["x"] + 24
            file_y = win["y"] + 14
            go_home_x = fmenu["x"] + min(60, fmenu["w"] // 2)
            go_home_y = (win["y"] + (fmenu["y"] - win["y"])) + int(fmenu["h"] * 0.23)
            vptr.cmd(
                f"drag {file_x} {file_y} {go_home_x} {go_home_y} {ext_w} {ext_h} left"
            )
            wait_for("dtfile press-drag-release navigation sub1 -> home", 6.0, lambda: navigated_to("home"))

            print(
                "PASS: check-dtwm-wayland-menus verified mid-press vptr crash cleanup, "
                "stuck BTN_RIGHT auto-heal, stationary click focus, and post-modeset "
                "HiDPI two-click + press-drag-release Motif menu activation"
            )
            return 0
        finally:
            if vptr is not None:
                vptr.close()
            for proc in (dtfile_proc, tt_proc, comp_proc):
                if proc is not None:
                    try:
                        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                    except OSError:
                        pass
                    if proc.poll() is None:
                        try:
                            proc.wait(timeout=3.0)
                        except subprocess.TimeoutExpired:
                            try:
                                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                            except OSError:
                                proc.kill()
            if "dbus_pid" in locals():
                try:
                    os.kill(dbus_pid, signal.SIGTERM)
                except OSError:
                    pass


if __name__ == "__main__":
    raise SystemExit(main())
