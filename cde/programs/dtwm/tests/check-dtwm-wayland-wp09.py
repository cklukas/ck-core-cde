#!/usr/bin/env python3
"""WP-09 integration check for ck-status, Front Panel Notifications Switch lamp,
System & Hardware slide-up subpanel, live 4-quadrant gauge, and Copy To Main Panel persistence.
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

FIND_PANEL_ARROWS_C = r"""
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int inspect_error(Display *dpy, XErrorEvent *event) {
    /* A window may disappear between XQueryTree and its next query. */
    if (event->error_code == BadWindow) return 0;

    char error[256];
    XGetErrorText(dpy, event->error_code, error, sizeof(error));
    fprintf(stderr, "window inspection failed: %s (request %u)\n",
            error, (unsigned int)event->request_code);
    exit(EXIT_FAILURE);
}

static void walk(Display *dpy, Window root, Window w, int depth) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a) || a.map_state != IsViewable) return;
    char *name = NULL;
    XFetchName(dpy, w, &name);
    XClassHint ch = {0};
    XGetClassHint(dpy, w, &ch);
    int rx = 0, ry = 0;
    Window child = None;
    if (XTranslateCoordinates(dpy, w, root, 0, 0, &rx, &ry, &child) &&
        (depth == 1 || (name && name[0]) || (ch.res_name && ch.res_name[0]))) {
        printf("WIN depth=%d id=0x%lx name=\"%s\" res=\"%s\" x=%d y=%d w=%d h=%d or=%d\n",
               depth, (unsigned long)w, name ? name : "", ch.res_name ? ch.res_name : "",
               rx, ry, a.width, a.height, a.override_redirect);
    }
    if (name) XFree(name);
    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);

    Window r = None, p = None, *kids = NULL;
    unsigned int n = 0;
    if (XQueryTree(dpy, w, &r, &p, &kids, &n) && kids) {
        for (unsigned int i = 0; i < n; ++i) walk(dpy, root, kids[i], depth + 1);
        XFree(kids);
    }
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    XSetErrorHandler(inspect_error);
    walk(dpy, DefaultRootWindow(dpy), DefaultRootWindow(dpy), 0);
    XSync(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
"""


SNI_HELPER_C = r"""
#include <dbus/dbus.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!c) return 1;
    dbus_bus_request_name(c, "org.ckcore.Wp09TrayApp", DBUS_NAME_FLAG_REPLACE_EXISTING, &err);

    DBusMessage *reg = dbus_message_new_method_call(
        "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem"
    );
    const char *svc = "org.ckcore.Wp09TrayApp";
    dbus_message_append_args(reg, DBUS_TYPE_STRING, &svc, DBUS_TYPE_INVALID);
    dbus_connection_send(c, reg, NULL);
    dbus_connection_flush(c);
    dbus_message_unref(reg);

    for (;;) {
        dbus_connection_read_write(c, 50);
        DBusMessage *in;
        while ((in = dbus_connection_pop_message(c)) != NULL) {
            if (dbus_message_is_method_call(in, "org.freedesktop.DBus.Properties", "GetAll")) {
                DBusMessage *rp = dbus_message_new_method_return(in);
                DBusMessageIter rit, dict, ent, var, parr, pst, pbytes;
                dbus_message_iter_init_append(rp, &rit);
                dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "{sv}", &dict);

                const char *k_id = "Id", *v_id = "wp09-sync";
                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k_id);
                dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "s", &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v_id);
                dbus_message_iter_close_container(&ent, &var);
                dbus_message_iter_close_container(&dict, &ent);

                const char *k_tt = "Title", *v_tt = "Cloud Backup";
                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k_tt);
                dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "s", &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v_tt);
                dbus_message_iter_close_container(&ent, &var);
                dbus_message_iter_close_container(&dict, &ent);

                const char *k_px = "IconPixmap";
                unsigned char raw[16 * 16 * 4];
                for (int i = 0; i < 16 * 16; ++i) {
                    raw[i * 4 + 0] = 0xff;
                    raw[i * 4 + 1] = 0x20;
                    raw[i * 4 + 2] = (unsigned char)(100 + (i % 16) * 8);
                    raw[i * 4 + 3] = 0xe8;
                }
                dbus_int32_t iw = 16, ih = 16;
                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k_px);
                dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "a(iiay)", &var);
                dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(iiay)", &parr);
                dbus_message_iter_open_container(&parr, DBUS_TYPE_STRUCT, NULL, &pst);
                dbus_message_iter_append_basic(&pst, DBUS_TYPE_INT32, &iw);
                dbus_message_iter_append_basic(&pst, DBUS_TYPE_INT32, &ih);
                dbus_message_iter_open_container(&pst, DBUS_TYPE_ARRAY, "y", &pbytes);
                const unsigned char *rbp = raw;
                dbus_message_iter_append_fixed_array(&pbytes, DBUS_TYPE_BYTE, &rbp, (int)sizeof(raw));
                dbus_message_iter_close_container(&pst, &pbytes);
                dbus_message_iter_close_container(&parr, &pst);
                dbus_message_iter_close_container(&var, &parr);
                dbus_message_iter_close_container(&ent, &var);
                dbus_message_iter_close_container(&dict, &ent);

                dbus_message_iter_close_container(&rit, &dict);
                dbus_connection_send(c, rp, NULL);
                dbus_connection_flush(c);
                dbus_message_unref(rp);
            }
            dbus_message_unref(in);
        }
    }
    return 0;
}
"""


def pointer_click(env: dict[str, str], x: int, y: int, button: str = "left") -> None:
    subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=env, check=True)
    subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=env, check=True)
    subprocess.run(["wlrctl", "pointer", "click", button], env=env, check=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    ap.add_argument("--save-dir", default="")
    args = ap.parse_args()

    # Re-enter inside an isolated dbus-run-session if not already isolated
    if not os.environ.get("CK_WP09_IN_DBUS") and shutil.which("dbus-run-session"):
        env2 = os.environ.copy()
        env2["CK_WP09_IN_DBUS"] = "1"
        cmd = ["dbus-run-session", "--", "python3", str(Path(__file__).resolve()), "--compositor", args.compositor]
        if args.save_dir:
            cmd.extend(["--save-dir", args.save_dir])
        return subprocess.call(cmd, env=env2)

    comp_bin = Path(args.compositor).resolve()
    build_root = comp_bin.parents[3]
    for cand in (comp_bin.parents[3], comp_bin.parents[4]):
        if (cand / "apps-build" / "bin" / "ck-status").is_file() or (cand / "cde-stage").is_dir():
            build_root = cand
            break
    ck_status_bin = build_root / "apps-build" / "bin" / "ck-status"
    if not ck_status_bin.is_file():
        print("WP-09 check skipped: ck-status binary not present in apps-build")
        return 0

    cc = os.environ.get("CC", "cc")
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-wayland-wp09-", dir=tmp_base) as work_s:
        work = Path(work_s)
        xdg = work / "xdg"
        home = work / "home"
        sys_class = work / "sys_class"
        xdg.mkdir(mode=0o700)
        home.mkdir(mode=0o755)
        (home / ".dt" / "types" / "fp_dynamic").mkdir(parents=True, mode=0o755)
        (home / ".dt" / "sessions").mkdir(parents=True, mode=0o755)

        # Mock laptop battery (76% Discharging, 12.5W) & Wi-Fi (84%) + Bluetooth hci0
        bat0 = sys_class / "power_supply" / "BAT0"
        ac0 = sys_class / "power_supply" / "AC0"
        wlan0 = sys_class / "net" / "wlan0" / "statistics"
        (wlan0.parent / "wireless").mkdir(parents=True)
        wlan0.mkdir(parents=True)
        bat0.mkdir(parents=True)
        ac0.mkdir(parents=True)
        (sys_class / "bluetooth" / "hci0").mkdir(parents=True)

        (bat0 / "type").write_text("Battery\n", encoding="utf-8")
        (bat0 / "present").write_text("1\n", encoding="utf-8")
        (bat0 / "status").write_text("Discharging\n", encoding="utf-8")
        (bat0 / "capacity").write_text("76\n", encoding="utf-8")
        (bat0 / "model_name").write_text("SPARCbook-LiIon\n", encoding="utf-8")
        (bat0 / "energy_now").write_text("38000000\n", encoding="utf-8")
        (bat0 / "energy_full").write_text("50000000\n", encoding="utf-8")
        (bat0 / "power_now").write_text("12500000\n", encoding="utf-8")
        (ac0 / "type").write_text("Mains\n", encoding="utf-8")
        (ac0 / "online").write_text("0\n", encoding="utf-8")
        (wlan0.parent / "operstate").write_text("up\n", encoding="utf-8")
        (wlan0 / "rx_bytes").write_text("52428800\n", encoding="utf-8")
        (wlan0 / "tx_bytes").write_text("10485760\n", encoding="utf-8")
        proc_w = work / "wireless"
        proc_w.write_text(
            "Inter-| sta-|   Quality\n face | tus | link level noise\n wlan0: 0000   59.  -51.  -256\n",
            encoding="utf-8",
        )

        # Pin live 4-quadrant gauge into Front Panel (~/.dt/types/ckstatus_gauge.fp)
        (home / ".dt" / "types" / "ckstatus_gauge.fp").write_text(
            "CONTROL Blank2\n{\n  TYPE client\n  CONTAINER_NAME Top\n  CONTAINER_TYPE BOX\n"
            "  POSITION_HINTS 8\n  CLIENT_NAME ck-status-gauge\n  CLIENT_GEOMETRY 74x54\n  LABEL System Status\n}\n",
            encoding="utf-8",
        )

        # Compile X11 tree inspector & D-Bus SNI client
        insp_c = work / "find_wins.c"
        insp_bin = work / "find_wins"
        insp_c.write_text(FIND_PANEL_ARROWS_C, encoding="utf-8")
        subprocess.run([cc, "-O2", str(insp_c), "-lX11", "-o", str(insp_bin)], check=True)

        dbus_cflags = subprocess.check_output(["pkg-config", "--cflags", "dbus-1"], text=True).split()
        dbus_libs = subprocess.check_output(["pkg-config", "--libs", "dbus-1"], text=True).split()
        sni_c = work / "sni_helper.c"
        sni_bin = work / "sni_helper"
        sni_c.write_text(SNI_HELPER_C, encoding="utf-8")
        subprocess.run([cc, "-O2", *dbus_cflags, str(sni_c), *dbus_libs, "-o", str(sni_bin)], check=True)

        cde_stage_dt = build_root / "cde-stage" / "usr" / "dt"
        env = os.environ.copy()
        ld_parts = [str(p) for p in (build_root / "motif-prefix" / "lib", cde_stage_dt / "lib", build_root / "fs-prefix" / "lib") if p.is_dir()]
        env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
        env["PATH"] = f"{ck_status_bin.parent}:{cde_stage_dt / 'bin'}:{env.get('PATH', '/usr/bin:/bin')}"
        env.update({
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg),
            "LANG": "C",
            "CK_STATUS_SYSFS_ROOT": str(sys_class),
            "CK_STATUS_PROC_NET_WIRELESS": str(proc_w),
            "DTDATABASESEARCHPATH": f"{home}/.dt/types,{cde_stage_dt}/appconfig/types/C",
            "XMICONSEARCHPATH": f"{cde_stage_dt}/appconfig/icons/C/%B%M.pm:{cde_stage_dt}/appconfig/icons/C/%B%M.bm:{cde_stage_dt}/appconfig/icons/C/%B.pm",
            "XMICONBMSEARCHPATH": f"{cde_stage_dt}/appconfig/icons/C/%B%M.bm:{cde_stage_dt}/appconfig/icons/C/%B%M.pm",
            "CDE_BACKDROP_DIR": str(cde_stage_dt / "backdrops"),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
        })

        status_file = work / "comp.status"
        comp_log_path = work / "comp.log"
        comp_log_fh = comp_log_path.open("w", encoding="utf-8")
        comp = subprocess.Popen(
            [str(comp_bin), "--status-file", str(status_file)],
            env=env,
            stdout=comp_log_fh,
            stderr=subprocess.STDOUT,
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
                raise SystemExit("dtwm-wayland failed to start")

            c_env = {
                **env,
                "WAYLAND_DISPLAY": wl_s,
                "DISPLAY": x_s,
                "PATH": f"{ck_status_bin.parent}:{env.get('PATH', '/usr/bin:/bin')}",
            }
            time.sleep(1.2)

            # Launch ck-status --daemon (unmapped, just like real SmWayland session) + embedded --gauge + SNI tray app
            p_status = subprocess.Popen(
                [str(ck_status_bin), "--daemon", "-geometry", "+44+36"],
                env=c_env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            p_gauge = subprocess.Popen(
                [str(ck_status_bin), "--gauge"],
                env=c_env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.extend([p_status, p_gauge])
            # Wait deterministically until ck-status claims org.freedesktop.Notifications on D-Bus
            for _ in range(80):
                if p_status.poll() is not None:
                    raise SystemExit(f"ck-status exited early with {p_status.returncode}")
                r = subprocess.run(
                    [
                        "dbus-send", "--session", "--dest=org.freedesktop.DBus",
                        "--type=method_call", "--print-reply", "/org/freedesktop/DBus",
                        "org.freedesktop.DBus.NameHasOwner", "string:org.freedesktop.Notifications",
                    ],
                    env=c_env,
                    capture_output=True,
                    text=True,
                )
                if "boolean true" in r.stdout:
                    break
                time.sleep(0.1)

            p_sni = subprocess.Popen([str(sni_bin)], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs.append(p_sni)
            time.sleep(0.8)

            # Send live desktop notification via notify-send -> triggers toast banner + unread file!
            if shutil.which("notify-send"):
                subprocess.run(
                    [
                        "notify-send",
                        "-a", "Power Manager",
                        "-t", "12000",
                        "SPARCbook Battery on Discharge (76%)",
                        "Estimated runtime 3h 02m remaining at 12.5 W. Wi-Fi link up (84%).",
                    ],
                    env=c_env,
                    check=True,
                )
                time.sleep(1.7)

            unread_p = home / ".dt" / "notifications.unread"
            if not unread_p.is_file() or unread_p.stat().st_size == 0:
                raise SystemExit("expected ~/.dt/notifications.unread to be populated after notify-send")

            # Find Front Panel window geometry and embedded ck-status-gauge coordinates!
            tree_out = subprocess.check_output([str(insp_bin)], env=c_env, text=True)

            def parse_win_line(line: str) -> dict[str, int]:
                after = line.split('res="', 1)[1].split('"', 1)[1]
                return {k: int(v) for k, v in (tok.split("=", 1) for tok in after.split() if "=" in tok)}

            gauge_box: dict[str, int] | None = None
            for ln in tree_out.splitlines():
                if 'res="ck-status-gauge"' in ln and "depth=1 " not in ln:
                    gauge_box = parse_win_line(ln)
                    break
            if not gauge_box:
                raise SystemExit(f"expected ck-status-gauge reparented inside Front Panel:\n{tree_out}")

            # Regression test: click all 4 regions of the embedded ck-status-gauge (starting with
            # top-left Wi-Fi/Network quadrant while ck-status --daemon is unmapped after XSetWMHints!)
            gx, gy = gauge_box["x"], gauge_box["y"]
            gw, gh = max(gauge_box.get("w", 54), 20), max(gauge_box.get("h", 48), 20)
            for q_x, q_y in (
                (gx + gw // 4, gy + gh // 4),          # Top-left: Wi-Fi / Network
                (gx + (gw * 5) // 8, gy + gh // 4),    # Top-right left: Bluetooth
                (gx + gw // 2, gy + (gh * 3) // 4),    # Bottom: Power / Battery
                (gx + (gw * 7) // 8, gy + gh // 4),    # Top-right right: Notifications & Tray
            ):
                pointer_click(c_env, q_x, q_y, "left")
                time.sleep(0.35)
                if comp.poll() is not None:
                    raise SystemExit(f"dtwm-wayland crashed (rc={comp.returncode}) when clicking ck-status-gauge at ({q_x},{q_y})")

            tree_after_gauge = subprocess.check_output([str(insp_bin)], env=c_env, text=True)
            if not any('name="System Status & Notifications"' in ln for ln in tree_after_gauge.splitlines()):
                raise SystemExit(f"expected System Status & Notifications window mapped after clicking gauge:\n{tree_after_gauge}")

            fp_x, fp_y, fp_w, fp_h = 140, 816, 1000, 82
            for ln in tree_out.splitlines():
                if "depth=1 " in ln:
                    kv = parse_win_line(ln)
                    if kv.get("w", 0) >= 700 and 55 <= kv.get("h", 0) <= 130:
                        fp_x, fp_y, fp_w, fp_h = kv["x"], kv["y"], kv["w"], kv["h"]
                        break

            # Click the subpanel arrow right above Style (between Printer and CkStatusGauge on right side of FrontPanel)
            sub_box: dict[str, int] | None = None
            style_arrow_y = fp_y + 9
            for frac in (0.742, 0.722, 0.762, 0.702, 0.682):
                pointer_click(c_env, fp_x + int(fp_w * frac), style_arrow_y, "left")
                time.sleep(0.4)
                tree_after = subprocess.check_output([str(insp_bin)], env=c_env, text=True)
                for ln in tree_after.splitlines():
                    if "depth=1 " in ln:
                        kv = parse_win_line(ln)
                        # SystemSubpanel has 6 controls + Install Icon -> height ~240..420px, width ~140..320px
                        if 120 <= kv.get("w", 0) <= 360 and kv.get("h", 0) >= 200 and kv.get("x", 0) > fp_x + fp_w // 2:
                            sub_box = kv
                            break
                if sub_box:
                    break

            if not sub_box:
                comp_log_fh.flush()
                clog = comp_log_path.read_text(encoding="utf-8", errors="replace")
                raise SystemExit(f"System & Hardware subpanel did not map above Style:\n{tree_after}\n--- comp.log ---\n{clog}")

            shot_main = work / "wp09-status-subpanel-and-toast.png"
            subprocess.run(["grim", str(shot_main)], env=c_env, check=True)
            with Image.open(shot_main) as im:
                if im.size[0] < 1000 or im.size[1] < 700:
                    raise SystemExit(f"unexpected screenshot size: {im.size}")

            # Right-click item inside System & Hardware subpanel -> "Copy To Main Panel"
            # Note: item 0 at top is "Install Icon", item 1 is main copy ("Desktop Style"), item 2 is "System Status", item 3 is "Power & Battery"
            item_x = sub_box["x"] + sub_box["w"] // 2
            item_y = sub_box["y"] + min(sub_box["h"] - 30, 145)
            pointer_click(c_env, item_x, item_y, "right")
            time.sleep(0.35)
            tree_menu = subprocess.check_output([str(insp_bin)], env=c_env, text=True)
            menu_box: dict[str, int] | None = None
            for ln in tree_menu.splitlines():
                if "depth=1 " in ln and "or=1" in ln:
                    kv = parse_win_line(ln)
                    if 60 <= kv.get("w", 0) <= 280 and 60 <= kv.get("h", 0) <= 260:
                        menu_box = kv
            if menu_box:
                # In CDE Subpanel PopupMenu: Title + DoubleSep (~26px), Row 1 = Action ("System Status"), Row 2 = "Copy To Main Panel" (~y+56)
                pointer_click(c_env, menu_box["x"] + menu_box["w"] // 2, menu_box["y"] + 56, "left")
                time.sleep(0.5)

            shot_promoted = work / "wp09-after-copy-to-main.png"
            subprocess.run(["grim", str(shot_promoted)], env=c_env, check=True)

            sess_file = home / ".dt" / "sessions" / "dtwmfp.session"
            if not sess_file.is_file():
                subprocess.run(["pkill", "-TERM", "-f", "dtwm-panel"], env=c_env, check=False)
                time.sleep(0.6)
            if not sess_file.is_file():
                raise SystemExit(f"expected ~/.dt/sessions/dtwmfp.session to be written by dtwm-panel (menu={menu_box})")
            sess_txt = sess_file.read_text(encoding="utf-8", errors="replace")
            if "SystemSubpanel" not in sess_txt:
                raise SystemExit(f"expected SystemSubpanel Copy-To-Main-Panel entry in dtwmfp.session:\n{sess_txt}")

            if args.save_dir:
                save_d = Path(args.save_dir)
                save_d.mkdir(parents=True, exist_ok=True)
                shutil.copy2(shot_main, save_d / shot_main.name)
                shutil.copy2(shot_promoted, save_d / shot_promoted.name)

            print("WP-09 check passed: Switch Notifications lamp, System & Hardware subpanel, SNI tray, toast banner, and dtwmfp.session verified")
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
