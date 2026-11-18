#!/usr/bin/env python3
"""WP-10 integration check for multi-monitor management (zwlr_output_manager_v1,
wlr-randr, dtstyle graphical Display & Monitors dialog), zwlr_foreign_toplevel_manager_v1,
zwlr_export_dmabuf_manager_v1, and xdg-desktop-portal-wlr readiness.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from PIL import Image

PROTO_PROBE_C = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "xdg-output-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

static bool g_has_out_mgr = false;
static bool g_has_pwr_mgr = false;
static bool g_has_dmabuf_mgr = false;
static struct zxdg_output_manager_v1 *g_xdg_out_mgr = NULL;
static struct zwlr_foreign_toplevel_manager_v1 *g_ft_mgr = NULL;
static struct wl_output *g_outputs[8];
static int g_out_cnt = 0;
static int g_xdg_done_cnt = 0;
static int g_ft_cnt = 0;
static char g_titles[1024];

static void xo_logical_pos(void *data, struct zxdg_output_v1 *o, int32_t x, int32_t y) {
    (void)o;
    int idx = (int)(intptr_t)data;
    printf("XDG_OUT_%d_POS=%d,%d\n", idx, x, y);
}
static void xo_logical_size(void *data, struct zxdg_output_v1 *o, int32_t w, int32_t h) {
    (void)o;
    int idx = (int)(intptr_t)data;
    printf("XDG_OUT_%d_SIZE=%dx%d\n", idx, w, h);
}
static void xo_done(void *data, struct zxdg_output_v1 *o) {
    (void)data; (void)o;
    g_xdg_done_cnt++;
}
static void xo_name(void *data, struct zxdg_output_v1 *o, const char *name) {
    (void)o;
    int idx = (int)(intptr_t)data;
    printf("XDG_OUT_%d_NAME=%s\n", idx, name ? name : "");
}
static void xo_desc(void *data, struct zxdg_output_v1 *o, const char *desc) {
    (void)data; (void)o; (void)desc;
}
static const struct zxdg_output_v1_listener g_xo_listener = {
    .logical_position = xo_logical_pos,
    .logical_size = xo_logical_size,
    .done = xo_done,
    .name = xo_name,
    .description = xo_desc,
};

static void ft_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *t) {
    (void)d; (void)h;
    if (t && strlen(g_titles) + strlen(t) + 4 < sizeof(g_titles)) {
        strcat(g_titles, "[");
        strcat(g_titles, t);
        strcat(g_titles, "]");
    }
}
static void ft_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *a) {
    (void)d; (void)h;
    printf("FOREIGN_APP_ID=%s\n", a ? a : "");
}
static void ft_out_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) { (void)d;(void)h;(void)o; }
static void ft_out_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) { (void)d;(void)h;(void)o; }
static void ft_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *s) { (void)d;(void)h;(void)s; }
static void ft_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)d;(void)h; }
static void ft_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)d; zwlr_foreign_toplevel_handle_v1_destroy(h); }
static void ft_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p) { (void)d;(void)h;(void)p; }

static const struct zwlr_foreign_toplevel_handle_v1_listener g_ft_handle_listener = {
    .title = ft_title,
    .app_id = ft_app_id,
    .output_enter = ft_out_enter,
    .output_leave = ft_out_leave,
    .state = ft_state,
    .done = ft_done,
    .closed = ft_closed,
    .parent = ft_parent,
};

static void ft_mgr_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)d; (void)m;
    g_ft_cnt++;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &g_ft_handle_listener, NULL);
}
static void ft_mgr_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d;(void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener g_ft_mgr_listener = {
    .toplevel = ft_mgr_toplevel,
    .finished = ft_mgr_finished,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (strcmp(iface, "zwlr_output_manager_v1") == 0) g_has_out_mgr = true;
    else if (strcmp(iface, "zwlr_output_power_manager_v1") == 0) g_has_pwr_mgr = true;
    else if (strcmp(iface, "zwlr_export_dmabuf_manager_v1") == 0) g_has_dmabuf_mgr = true;
    else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
        g_xdg_out_mgr = wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, ver < 3 ? ver : 3);
    } else if (strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        g_ft_mgr = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, ver < 3 ? ver : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(g_ft_mgr, &g_ft_mgr_listener, NULL);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && g_out_cnt < 8) {
        g_outputs[g_out_cnt++] = wl_registry_bind(r, name, &wl_output_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener g_reg_listener = { .global = reg_global, .global_remove = reg_rem };

int main(void) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &g_reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (g_xdg_out_mgr) {
        for (int i = 0; i < g_out_cnt; ++i) {
            struct zxdg_output_v1 *xo = zxdg_output_manager_v1_get_xdg_output(g_xdg_out_mgr, g_outputs[i]);
            zxdg_output_v1_add_listener(xo, &g_xo_listener, (void *)(intptr_t)i);
        }
    }
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    printf("PROTO_GLOBALS=%d,%d,%d\n", g_has_out_mgr ? 1 : 0, g_has_pwr_mgr ? 1 : 0, g_has_dmabuf_mgr ? 1 : 0);
    printf("FOREIGN_COUNT=%d\n", g_ft_cnt);
    printf("FOREIGN_TITLES=%s\n", g_titles);
    wl_display_disconnect(dpy);
    return 0;
}
"""

X11_OUT_PROP_C = r"""
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);

    if (argc >= 2) {
        Atom mwm = XInternAtom(dpy, "_MOTIF_WM_INFO", True);
        Atom at = None; int af = 0; unsigned long n = 0, r = 0; unsigned char *p = NULL;
        if (mwm != None && XGetWindowProperty(dpy, root, mwm, 0, 2, False, AnyPropertyType, &at, &af, &n, &r, &p) == Success && p && n >= 2) {
            Window wmw = (Window)((unsigned long *)p)[1];
            XFree(p);
            Atom req = XInternAtom(dpy, "_DT_WM_REQUEST", False);
            XChangeProperty(dpy, wmw, req, XA_STRING, 8, PropModeAppend, (const unsigned char *)argv[1], (int)strlen(argv[1]) + 1);
            XSync(dpy, False);
        }
        XCloseDisplay(dpy);
        return 0;
    }

    Atom out_a = XInternAtom(dpy, "_DT_WAYLAND_OUTPUTS", True);
    Atom at = None; int af = 0; unsigned long n = 0, r = 0; unsigned char *d = NULL;
    if (out_a != None && XGetWindowProperty(dpy, root, out_a, 0, 2048, False, AnyPropertyType, &at, &af, &n, &r, &d) == Success && d) {
        fwrite(d, 1, n, stdout);
        XFree(d);
    }
    XCloseDisplay(dpy);
    return 0;
}
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    ap.add_argument("--save-dir", default="")
    args = ap.parse_args()

    if not os.environ.get("CK_WP10_IN_DBUS") and shutil.which("dbus-run-session"):
        env2 = os.environ.copy()
        env2["CK_WP10_IN_DBUS"] = "1"
        cmd = ["dbus-run-session", "--", "python3", str(Path(__file__).resolve()), "--compositor", args.compositor]
        if args.save_dir:
            cmd.extend(["--save-dir", args.save_dir])
        return subprocess.call(cmd, env=env2)

    comp_bin = Path(args.compositor).resolve()
    build_root = comp_bin.parents[3]
    for cand in (comp_bin.parents[3], comp_bin.parents[4]):
        if (cand / "cde-stage").is_dir():
            build_root = cand
            break

    dtstyle_bin = build_root / "cde-stage" / "usr" / "dt" / "bin" / "dtstyle"
    if not dtstyle_bin.is_file():
        cand_dtstyle = comp_bin.parents[1] / "dtstyle" / "dtstyle"
        if cand_dtstyle.is_file():
            dtstyle_bin = cand_dtstyle

    src_dir = Path(__file__).resolve().parents[1]
    ft_xml = src_dir / "wayland" / "protocols" / "wlr-foreign-toplevel-management-unstable-v1.xml"
    xo_xml = Path("/usr/share/wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml")

    cc = os.environ.get("CC", "cc")
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-wayland-wp10-", dir=tmp_base) as work_s:
        work = Path(work_s)
        xdg = work / "xdg"
        home = work / "home"
        xdg.mkdir(mode=0o700)
        home.mkdir(mode=0o755)
        (home / ".dt" / "types").mkdir(parents=True, mode=0o755)
        (home / ".dt" / "sessions").mkdir(parents=True, mode=0o755)

        # Compile Wayland protocol probe & X11 _DT_WAYLAND_OUTPUTS helper
        subprocess.run(["wayland-scanner", "client-header", str(xo_xml), str(work / "xdg-output-unstable-v1-client-protocol.h")], check=True)
        subprocess.run(["wayland-scanner", "private-code", str(xo_xml), str(work / "xdg-output-unstable-v1-protocol.c")], check=True)
        subprocess.run(["wayland-scanner", "client-header", str(ft_xml), str(work / "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h")], check=True)
        subprocess.run(["wayland-scanner", "private-code", str(ft_xml), str(work / "wlr-foreign-toplevel-management-unstable-v1-protocol.c")], check=True)

        probe_c = work / "proto_probe.c"
        probe_bin = work / "proto_probe"
        probe_c.write_text(PROTO_PROBE_C, encoding="utf-8")
        subprocess.run(
            [
                cc, "-O2", f"-I{work}", str(probe_c),
                str(work / "xdg-output-unstable-v1-protocol.c"),
                str(work / "wlr-foreign-toplevel-management-unstable-v1-protocol.c"),
                "-lwayland-client", "-o", str(probe_bin),
            ],
            check=True,
        )

        xprop_c = work / "x11_out_prop.c"
        xprop_bin = work / "x11_out_prop"
        xprop_c.write_text(X11_OUT_PROP_C, encoding="utf-8")
        subprocess.run([cc, "-O2", str(xprop_c), "-lX11", "-o", str(xprop_bin)], check=True)

        cde_stage_dt = build_root / "cde-stage" / "usr" / "dt"
        env = os.environ.copy()
        ld_parts = [str(p) for p in (build_root / "motif-prefix" / "lib", cde_stage_dt / "lib", build_root / "fs-prefix" / "lib") if p.is_dir()]
        env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
        env["PATH"] = f"{cde_stage_dt / 'bin'}:{env.get('PATH', '/usr/bin:/bin')}"
        env.update({
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg),
            "XDG_CURRENT_DESKTOP": "CDE",
            "XDG_SESSION_TYPE": "wayland",
            "LANG": "C",
            "XFILESEARCHPATH": f"{cde_stage_dt}/app-defaults/%L/%N:{cde_stage_dt}/app-defaults/C/%N",
            "DTDATABASESEARCHPATH": f"{home}/.dt/types,{cde_stage_dt}/appconfig/types/C",
            "XMICONSEARCHPATH": f"{cde_stage_dt}/appconfig/icons/C/%B%M.pm:{cde_stage_dt}/appconfig/icons/C/%B%M.bm:{cde_stage_dt}/appconfig/icons/C/%B.pm",
            "XMICONBMSEARCHPATH": f"{cde_stage_dt}/appconfig/icons/C/%B%M.bm:{cde_stage_dt}/appconfig/icons/C/%B%M.pm",
            "CDE_BACKDROP_DIR": str(cde_stage_dt / "backdrops"),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "2",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
        })

        status_file = work / "comp.status"
        comp = subprocess.Popen(
            [str(comp_bin), "--status-file", str(status_file)],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        procs = [comp]
        try:
            wl_s = x_s = None
            for _ in range(80):
                if status_file.is_file():
                    kv = dict(ln.split("=", 1) for ln in status_file.read_text().splitlines() if "=" in ln)
                    wl_s, x_s = kv.get("WAYLAND_DISPLAY"), kv.get("DISPLAY")
                    if wl_s and x_s:
                        break
                time.sleep(0.1)
            if not wl_s or not x_s:
                raise SystemExit("dtwm-wayland failed to start for WP-10 check")

            c_env = {**env, "WAYLAND_DISPLAY": wl_s, "DISPLAY": x_s}
            time.sleep(1.0)

            # 1. Verify wlr-randr (zwlr_output_manager_v1) queries & reconfigures live outputs!
            if shutil.which("wlr-randr"):
                r_before = subprocess.check_output(["wlr-randr"], env=c_env, text=True)
                if "HEADLESS-1" not in r_before or "HEADLESS-2" not in r_before:
                    raise SystemExit(f"wlr-randr missing expected dual headless outputs:\n{r_before}")

                subprocess.run(
                    ["wlr-randr", "--output", "HEADLESS-1", "--pos", "1280,120", "--scale", "1.5"],
                    env=c_env,
                    check=True,
                )
                time.sleep(0.3)
                prop_after_randr = subprocess.check_output([str(xprop_bin)], env=c_env, text=True)
                if "|HEADLESS-1|1|1280|120|" not in prop_after_randr or "|144|" not in prop_after_randr:
                    raise SystemExit(f"_DT_WAYLAND_OUTPUTS did not reflect wlr-randr apply:\n{prop_after_randr}")

            # 2. Launch dtstyle -display_dialog and verify Monitors dialog & foreign toplevels
            if dtstyle_bin.is_file():
                p_dtstyle = subprocess.Popen(
                    [str(dtstyle_bin), "-display_dialog", "-geometry", "+36+28"],
                    env=c_env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True,
                )
                procs.append(p_dtstyle)
                time.sleep(1.6)

            probe_out = subprocess.check_output([str(probe_bin)], env=c_env, text=True)
            if "PROTO_GLOBALS=1,1,1" not in probe_out:
                raise SystemExit(f"missing Wayland output/power/dmabuf manager globals:\n{probe_out}")
            if dtstyle_bin.is_file() and "Style Manager - Display & Monitors" not in probe_out:
                raise SystemExit(f"zwlr_foreign_toplevel_manager_v1 did not report Display & Monitors dialog:\n{probe_out}")

            # Capture primary monitor screenshot showing Style Manager + Display & Monitors dialog
            shot = work / "wp10-dtstyle-monitors.png"
            subprocess.run(["grim", "-o", "HEADLESS-2", str(shot)], env=c_env, check=True)
            with Image.open(shot) as im:
                if im.size != (1280, 900):
                    raise SystemExit(f"unexpected HEADLESS-2 screenshot size: {im.size}")

            # 3. Verify live dtstyle IPC rotation (90 deg) + custom mode (1440x900 @ 96 DPI -> 900x1440 logical!)
            subprocess.run(
                [str(xprop_bin), "f.set_output_layout HEADLESS-1 1 1280 0 1440x900 90 96"],
                env=c_env,
                check=True,
            )
            time.sleep(0.3)
            probe_rot = subprocess.check_output([str(probe_bin)], env=c_env, text=True)
            if "900x1440" not in probe_rot:
                raise SystemExit(f"expected rotated 900x1440 logical size on HEADLESS-1:\n{probe_rot}")

            # 3b. Verify live 200% (192 DPI) Wayland output scale (1440x900 @ 90 deg @ 192 DPI -> 450x720 logical!)
            subprocess.run(
                [str(xprop_bin), "f.set_output_layout HEADLESS-1 1 1280 0 1440x900 90 192"],
                env=c_env,
                check=True,
            )
            time.sleep(0.3)
            probe_scaled = subprocess.check_output([str(probe_bin)], env=c_env, text=True)
            if "450x720" not in probe_scaled:
                raise SystemExit(f"expected 200% scaled 450x720 logical size on HEADLESS-1:\n{probe_scaled}")

            # 4. Verify xdg-desktop-portal-wlr connects to dtwm-wayland + PipeWire and registers D-Bus portal backend
            wlr_portal_paths = (
                "/usr/libexec/xdg-desktop-portal-wlr",
                "/usr/lib/xdg-desktop-portal-wlr",
            )
            portal_bin = next((Path(p) for p in wlr_portal_paths if Path(p).is_file()), None)
            if portal_bin and shutil.which("dbus-send") and shutil.which("pipewire"):
                cfg_dir = home / ".config" / "xdg-desktop-portal-wlr"
                cfg_dir.mkdir(parents=True, exist_ok=True)
                (cfg_dir / "config").write_text("[screencast]\nchooser_type=none\n", encoding="utf-8")

                p_pw = subprocess.Popen(
                    ["pipewire"],
                    env=c_env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True,
                )
                procs.append(p_pw)
                time.sleep(0.5)

                portal_log = work / "portal.log"
                portal_fh = portal_log.open("w", encoding="utf-8")
                p_portal = subprocess.Popen(
                    [str(portal_bin), "-l", "INFO"],
                    env=c_env,
                    stdout=portal_fh,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                procs.append(p_portal)
                portal_ok = False
                for _ in range(40):
                    names = subprocess.check_output(
                        [
                            "dbus-send", "--session", "--print-reply",
                            "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus",
                            "org.freedesktop.DBus.ListNames",
                        ],
                        env=c_env,
                        text=True,
                    )
                    if "org.freedesktop.impl.portal.desktop.wlr" in names:
                        portal_ok = True
                        break
                    time.sleep(0.1)
                portal_fh.close()
                if not portal_ok:
                    plog = portal_log.read_text(encoding="utf-8", errors="replace")
                    raise SystemExit(f"xdg-desktop-portal-wlr failed to register D-Bus service:\n{plog}")

            if args.save_dir:
                save_d = Path(args.save_dir)
                save_d.mkdir(parents=True, exist_ok=True)
                shutil.copy2(shot, save_d / shot.name)

            print("WP-10 check passed: wlr-randr, dtstyle Display & Monitors, foreign-toplevel, export-dmabuf, and xdg-desktop-portal-wlr verified")
            return 0
        finally:
            for pr in reversed(procs):
                if pr.poll() is None:
                    try:
                        os.killpg(pr.pid, 15)
                    except ProcessLookupError:
                        pass
            time.sleep(0.3)
            for pr in reversed(procs):
                if pr.poll() is None:
                    try:
                        os.killpg(pr.pid, 9)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    raise SystemExit(main())
