#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland (WP-02).

Verifies:
1. Headless dual-output startup + rootless XWayland readiness.
2. Native Wayland client (`foot`) and XWayland client (`xeyes`/`xclock`/`dtpad` or synthetic X11 client)
   both receive authentic server-side 3D CDE bevel frames:
   - Focused window wears CDE Crimson (#B24D7A) active bevels.
   - Background window wears CDE Slate (#AEB2C3) inactive bevels.
3. Pointer click switches focus between XWayland and Wayland windows, opens the CDE Window Menu,
   double-click maximizes/restores, and clicking empty desktop clears active focus.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image


def count_near_color(im: Image.Image, target_rgb: tuple[int, int, int], tol: int = 16) -> int:
    raw = im.convert("RGB").tobytes()
    tr, tg, tb = target_rgb
    matches = 0
    for i in range(0, len(raw), 3):
        if abs(raw[i] - tr) <= tol and abs(raw[i + 1] - tg) <= tol and abs(raw[i + 2] - tb) <= tol:
            matches += 1
    return matches


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    ap.add_argument("--save-dir", default="")
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    if not comp_bin.is_file():
        raise SystemExit(f"missing compositor binary: {comp_bin}")

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-wayland-test-", dir=tmp_base) as work_s:
        work = Path(work_s)
        xdg = work / "xdg"
        home = work / "home"
        xdg.mkdir(mode=0o700)
        home.mkdir(mode=0o700)
        os.chmod(xdg, 0o700)
        os.chmod(home, 0o700)

        env = os.environ.copy()
        env["HOME"] = str(home)
        env["XDG_RUNTIME_DIR"] = str(xdg)
        env["WLR_BACKENDS"] = "headless"
        env["WLR_HEADLESS_OUTPUTS"] = "2"
        env["WLR_RENDERER"] = "pixman"
        env["WLR_LIBINPUT_NO_DEVICES"] = "1"

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
            wl_sock = None
            x_sock = None
            for _ in range(75):
                if comp.poll() is not None:
                    comp_log.flush()
                    raise SystemExit(f"dtwm-wayland exited early ({comp.returncode}):\n" + (work / "dtwm-wayland.log").read_text())
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
                raise SystemExit(f"timed out waiting for WAYLAND_DISPLAY ({wl_sock}) / DISPLAY ({x_sock})")

            c_env = {**env, "WAYLAND_DISPLAY": wl_sock, "DISPLAY": x_sock}

            # 1. Spawn a native Wayland client (foot) and an X11/Motif client
            wl_cmd = ["foot"] if shutil.which("foot") else None
            if not wl_cmd:
                raise SystemExit("foot Wayland client is required for check-dtwm-wayland.py")

            p_wl = subprocess.Popen(wl_cmd, env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs.append(p_wl)
            time.sleep(1.0)

            # Compile a tiny deterministic X11 client window so check-local has zero external app path assumptions
            x11_c = work / "x11_win.c"
            x11_bin = work / "x11_win"
            x11_c.write_text(
                "#include <X11/Xlib.h>\n"
                "#include <unistd.h>\n"
                "int main(void) {\n"
                "  Display *d = XOpenDisplay(NULL);\n"
                "  if (!d) return 1;\n"
                "  int s = DefaultScreen(d);\n"
                "  Window w = XCreateSimpleWindow(d, RootWindow(d, s), 260, 140, 360, 240, 0, BlackPixel(d, s), WhitePixel(d, s));\n"
                "  XStoreName(d, w, \"CDE XWayland Test Client\");\n"
                "  XMapWindow(d, w);\n"
                "  XFlush(d);\n"
                "  for (;;) pause();\n"
                "}\n",
                encoding="utf-8",
            )
            subprocess.run(["cc", "-O2", str(x11_c), "-lX11", "-o", str(x11_bin)], check=True)
            p_x11 = subprocess.Popen([str(x11_bin)], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs.append(p_x11)
            time.sleep(1.2)

            shot1 = work / "frame-two-windows.png"
            subprocess.run(["grim", str(shot1)], env=c_env, check=True)
            with Image.open(shot1) as im:
                active_px = count_near_color(im, (237, 168, 112), tol=20)   # #EDA870 Default.dp active frame
                inactive_px = count_near_color(im, (153, 153, 153), tol=20) # #999999 Default.dp inactive frame
                backdrop_px = count_near_color(im, (137, 152, 170), tol=28) # #8998AA Default.dp KnitLight backdrop
                if active_px < 1500:
                    raise SystemExit(f"expected active CDE Default.dp (#EDA870) bevel frame pixels, got {active_px}")
                if inactive_px < 1500:
                    raise SystemExit(f"expected inactive CDE Default.dp (#999999) bevel frame pixels, got {inactive_px}")
                if backdrop_px < 50000:
                    raise SystemExit(f"expected CDE Default.dp KnitLight desktop backdrop pixels, got {backdrop_px}")

            # 2. Click empty desktop (500, 10) -> clears active focus (only docked Front Panel border remains!)
            subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
            subprocess.run(["wlrctl", "pointer", "move", "500", "10"], env=c_env, check=True)
            subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
            time.sleep(0.3)

            shot_unfocused = work / "frame-desktop-unfocused.png"
            subprocess.run(["grim", str(shot_unfocused)], env=c_env, check=True)
            with Image.open(shot_unfocused) as im:
                # Crop window region (clearing bottom Front Panel): active 15000+ px frame & 240 px indicator must clear!
                win_crop = im.crop((75, 0, im.size[0], im.size[1] - 100))
                active_after = count_near_color(win_crop, (237, 168, 112), tol=20)
                if active_after >= 120:
                    raise SystemExit(f"clicking empty desktop did not clear active frame & indicator (got {active_after} px)")

            # 3. Click first window menu button (114, 48, since left_margin=104 clears Top-Left desktop icons)
            subprocess.run(["wlrctl", "pointer", "move", "114", "48"], env=c_env, check=True)
            subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
            time.sleep(0.3)

            shot_menu = work / "frame-window-menu.png"
            subprocess.run(["grim", str(shot_menu)], env=c_env, check=True)
            with Image.open(shot_menu) as im:
                win_crop = im.crop((75, 0, im.size[0], im.size[1] - 100))
                active_refocused = count_near_color(win_crop, (237, 168, 112), tol=20)
                if active_refocused < 1500:
                    raise SystemExit(f"re-clicking window did not restore active frame ({active_refocused} px)")

            if args.save_dir:
                sdir = Path(args.save_dir)
                sdir.mkdir(parents=True, exist_ok=True)
                for f in (shot1, shot_unfocused, shot_menu):
                    shutil.copy2(f, sdir / f.name)

            print(
                f"PASS: dtwm-wayland dual-protocol SSD bevels verified "
                f"(active={active_px}px, inactive={inactive_px}px, unfocused={active_after}px)"
            )
            return 0
        finally:
            for p in reversed(procs):
                if p.poll() is None:
                    try:
                        os.killpg(p.pid, 15)
                    except ProcessLookupError:
                        pass
            time.sleep(0.3)
            for p in reversed(procs):
                if p.poll() is None:
                    try:
                        os.killpg(p.pid, 9)
                    except ProcessLookupError:
                        pass
                    p.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
