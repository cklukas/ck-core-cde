#!/usr/bin/env python3
"""Automated Wayland regression test for Front Panel subpanel interaction and
transparent Motif/dtfile drag-and-drop visuals:
1. Right-clicking the empty gray subpanel arrow bar above a control without a
   subpanel (e.g. Mail) posts the control's popup menu with "Add Subpanel" and
   creates the subpanel cleanly.
2. Docked subpanels do NOT auto-close when clicking the desktop or another
   application window (such as dtfile), matching X11 CDE behavior so users can
   drag items onto the subpanel's Install Icon drop zone; clicking the subpanel
   arrow button toggles the docked subpanel closed.
3. Dragging a folder from dtfile under default Motif settings displays a visible
   32-bit ARGB XmDragOverShell window with transparent surrounding pixels (no
   opaque white rectangle and no black hotspot debug marker).
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image


def find_cde_build_and_src(comp_bin: Path) -> tuple[Path, Path]:
    cde_build = comp_bin.parent.parent.parent
    makefile = comp_bin.parent / "Makefile"
    cde_src = cde_build
    if makefile.is_file():
        for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
            if ln.startswith("top_srcdir ="):
                rel = ln.split("=", 1)[1].strip()
                cde_src = (comp_bin.parent / rel).resolve()
                break
    return cde_build, cde_src


def map_state(cenv: dict[str, str], wid: str) -> str:
    out = subprocess.run(["xwininfo", "-id", wid], env=cenv, capture_output=True,
                         text=True, timeout=2).stdout
    for ln in out.splitlines():
        if "Map State:" in ln:
            return ln.split(":", 1)[1].strip()
    return ""


def find_win(cenv: dict[str, str], substr: str) -> tuple[str | None, tuple[int, int, int, int] | None]:
    out = subprocess.run(["xwininfo", "-root", "-tree"], env=cenv, capture_output=True,
                         text=True, timeout=2).stdout
    for ln in out.splitlines():
        if substr in ln:
            m = re.match(r"\s+(0x[0-9a-f]+)\s+.*?(\d+)x(\d+)[+-]\d+[+-]\d+\s+([+-]\d+)([+-]\d+)", ln)
            if m:
                return m.group(1), (int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
    return None, None


def read_file_manager_state(env: dict[str, str]) -> dict[str, str]:
    result = subprocess.run(["xprop", "-notype", "-root", "_DT_FILEMGR_ERGONOMICS_STATE"],
                            env=env, capture_output=True, text=True, timeout=2)
    value = result.stdout.partition(' = "')[2].rstrip('"\n')
    return dict(field.split("=", 1) for field in re.split(r"\\n|\n", value) if "=" in field)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    if sys.platform != "linux":
        print("SKIP: check-dtwm-wayland-panel-dnd requires Linux")
        return 0

    comp_bin = Path(args.compositor).resolve()
    cde_build, cde_src = find_cde_build_and_src(comp_bin)
    dtfile_bin = cde_build / "programs" / "dtfile" / "dtfile"
    if not dtfile_bin.is_file():
        dtfile_bin = comp_bin.parent / "dtfile"
    if not dtfile_bin.is_file():
        raise SystemExit(f"dtfile binary not found near {comp_bin}")

    cc = os.environ.get("CC", "cc")
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")

    with tempfile.TemporaryDirectory(prefix="dtwm-wl-panel-dnd-", dir=tmp_base) as td:
        work = Path(td)
        xdg = work / "xdg"
        home = work / "home"
        xdg.mkdir(mode=0o700)
        home.mkdir(mode=0o700)
        (home / "TestFolder").mkdir()

        vp_bin = work / "vp"
        vp_source = Path(__file__).with_name("wayland-test-pointer.c")
        subprocess.run([cc, "-O2", str(vp_source), "-lwayland-client", "-o", str(vp_bin)],
                       check=True)

        env = os.environ.copy()
        for k in ("WAYLAND_DISPLAY", "DISPLAY", "TT_SESSION", "_SUN_TT_SESSION", "DTUSERSESSION"):
            env.pop(k, None)
        env.update({
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg),
            "TMPDIR": str(work),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
            "CDE_BACKDROP_DIR": str(cde_src / "programs" / "backdrops"),
        })

        sf = work / "status"
        comp = subprocess.Popen(
            [str(comp_bin), "--status-file", str(sf)],
            env=env,
            stdout=open(work / "comp.log", "w", encoding="utf-8"),
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        vp = None
        dtf = None
        try:
            wl = x11 = None
            for _ in range(80):
                if sf.is_file():
                    kv = dict(
                        ln.split("=", 1)
                        for ln in sf.read_text(encoding="utf-8").splitlines()
                        if "=" in ln
                    )
                    wl, x11 = kv.get("WAYLAND_DISPLAY"), kv.get("DISPLAY")
                    if wl and x11:
                        break
                time.sleep(0.1)
            if not wl or not x11:
                raise SystemExit("dtwm-wayland failed to publish status file")

            cenv = {**env, "WAYLAND_DISPLAY": wl, "DISPLAY": x11, "CK_MOTIF_BACKEND": "x11"}
            init_png = work / "init.png"
            subprocess.run(["grim", str(init_png)], env=cenv, check=True, timeout=5)
            with Image.open(init_png) as im:
                out_w, out_h = im.size
                bg_sample = im.convert("RGB").getpixel((out_w // 2, out_h // 2))

            fp_box = None
            last_box = None
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                wid, box = find_win(cenv, '("dtwm-front-panel" "Dtwm")')
                if wid and box:
                    last_box = box
                    width, height, x, y = box
                    # The shell can be viewable before its compositor position arrives.
                    if (abs(2 * x + width - out_w) <= 2
                            and abs(y + height - out_h) <= 8
                            and map_state(cenv, wid) == "IsViewable"):
                        fp_box = box
                        break
                time.sleep(0.1)
            if not fp_box:
                raise SystemExit(f"The Front Panel did not dock on {out_w}x{out_h} (last: {last_box})")

            vp = subprocess.Popen(
                [str(vp_bin)], env=cenv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                start_new_session=True,
            )

            def send(cmd: str) -> None:
                assert vp.stdin and vp.stdout
                vp.stdin.write(cmd + "\n")
                vp.stdin.flush()
                if vp.stdout.readline().strip() != "OK":
                    raise SystemExit(f"Virtual pointer did not accept {cmd!r}")

            # 1. Right-click empty gray subpanel bar above Mail -> Add Subpanel
            fw, fh, fx, fy = fp_box
            mail_x = fx + 304
            bar_y = fy + 9
            send(f"move {mail_x} {bar_y} {out_w} {out_h}")
            send("sleep 80")
            send("down 273")
            send("sleep 40")
            send("up 273")
            send("sleep 250")

            out = subprocess.run(["xwininfo", "-root", "-tree"], env=cenv, capture_output=True, text=True).stdout
            pop_box = None
            for ln in out.splitlines():
                m = re.match(r"\s+(0x[0-9a-f]+)\s+\(has no name\):\s+\(\)\s+(\d+)x(\d+)\+(\d+)\+(\d+)", ln)
                if m:
                    pw, ph, px, py = map(int, m.groups()[1:])
                    if 60 <= pw <= 220 and 60 <= ph <= 220 and px >= mail_x - 20 and map_state(cenv, m.group(1)) == "IsViewable":
                        pop_box = (pw, ph, px, py)
                        break
            if not pop_box:
                raise SystemExit("Right-clicking empty arrow bar above Mail did not post FPPopup")

            pw, ph, px, py = pop_box
            send(f"move {px + pw // 2} {py + int(ph * 0.65)} {out_w} {out_h}")
            send("sleep 80")
            send("down 272")
            send("sleep 40")
            send("up 272")
            send("sleep 300")

            mail_sp_wid, _ = find_win(cenv, '"Mail"')
            if not mail_sp_wid:
                raise SystemExit("Clicking Add Subpanel from arrow-bar menu did not create Mail subpanel")

            # 2. Open Mail subpanel and verify it does NOT auto-close on desktop/dtfile clicks
            send(f"move {mail_x} {bar_y} {out_w} {out_h}")
            send("sleep 80")
            send("down 272")
            send("sleep 40")
            send("up 272")
            send("sleep 300")
            if map_state(cenv, mail_sp_wid) != "IsViewable":
                raise SystemExit("Mail subpanel did not open when clicking its arrow button")

            send(f"move 100 100 {out_w} {out_h}")
            send("sleep 60")
            send("down 272")
            send("sleep 40")
            send("up 272")
            send("sleep 250")
            if map_state(cenv, mail_sp_wid) != "IsViewable":
                raise SystemExit("Docked Mail subpanel auto-closed on desktop click")

            dtfile_log = work / "dtfile.log"
            with dtfile_log.open("wb") as log:
                dtf = subprocess.Popen(
                    [str(dtfile_bin), "-dir", str(home)],
                    env=cenv, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
                )
            dtf_box = None
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                dtf_wid, box = find_win(cenv, '"File Manager - home"')
                if dtf_wid and box and map_state(cenv, dtf_wid) == "IsViewable":
                    dtf_box = box
                    break
                if dtf.poll() is not None:
                    break
                time.sleep(0.1)
            if not dtf_box:
                tree = subprocess.run(["xwininfo", "-root", "-tree"], env=cenv,
                                      capture_output=True, text=True, timeout=2).stdout
                raise SystemExit(
                    f"dtfile did not map (status {dtf.returncode}):\n"
                    f"{dtfile_log.read_text(errors='replace')}\n{tree}"
                )
            dfw, dfh, dfx, dfy = dtf_box
            send(f"move {dfx + 100} {dfy + 300} {out_w} {out_h}")
            send("sleep 60")
            send("down 272")
            send("sleep 40")
            send("up 272")
            send("sleep 250")
            if map_state(cenv, mail_sp_wid) != "IsViewable":
                raise SystemExit("Docked Mail subpanel auto-closed when clicking dtfile")

            # 3. Drag TestFolder out over desktop -> verify visible transparent ARGB drag icon
            deadline = time.monotonic() + 5
            while True:
                state = read_file_manager_state(cenv)
                sx = int(state.get("first_dir_rx", "0"))
                sy = int(state.get("first_dir_ry", "0"))
                if (state.get("dir") == str(home) and state.get("first_dir_name") == "TestFolder"
                        and 0 < sx < out_w and 0 < sy < out_h):
                    break
                if time.monotonic() >= deadline:
                    raise SystemExit(f"File Manager did not publish the test folder's position: {state}")
                time.sleep(0.1)
            send(f"move {sx} {sy} {out_w} {out_h}")
            send("sleep 80")
            send("down 272")
            send("sleep 80")
            for step in range(1, 11):
                nx = sx + (480 * step) // 10
                ny = sy + (220 * step) // 10
                send(f"move {nx} {ny} {out_w} {out_h}")
                send("sleep 50")

            drag_win = None
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline and not drag_win:
                out_drag = subprocess.check_output(["xwininfo", "-root", "-tree"], env=cenv, text=True, timeout=5)
                for ln in out_drag.splitlines():
                    m = re.match(r"\s+(0x[0-9a-f]+)\s+\(has no name\):\s+\(\)\s+(\d+)x(\d+)\+(\d+)\+(\d+)", ln)
                    if m:
                        dw, dh, dx, dy = map(int, m.groups()[1:])
                        if 20 <= dw <= 220 and 20 <= dh <= 220 and dx > 500 and dy > 300 and map_state(cenv, m.group(1)) == "IsViewable":
                            drag_win = (m.group(1), dw, dh, dx, dy)
                            break
                if not drag_win:
                    time.sleep(0.1)
            if not drag_win:
                raise SystemExit("No visible XmDragOverShell drag icon window during dtfile drag")

            drag_png = work / "drag.png"
            subprocess.run(["grim", str(drag_png)], env=cenv, check=True)
            _, dw, dh, dx, dy = drag_win
            with Image.open(drag_png) as im:
                rgb = im.convert("RGB")
                corner = rgb.getpixel((dx + 2, dy + 2))
                # Corner pixel inside the drag window bounding box must match backdrop (transparent),
                # not solid white (255,255,255) or solid black (0,0,0).
                if corner == (255, 255, 255) or corner == (0, 0, 0):
                    raise SystemExit(f"Drag icon corner pixel is opaque {corner}, expected transparent backdrop near {bg_sample}")
                if abs(corner[0] - bg_sample[0]) > 45 or abs(corner[1] - bg_sample[1]) > 45 or abs(corner[2] - bg_sample[2]) > 45:
                    raise SystemExit(f"Drag icon corner pixel {corner} does not match transparent backdrop {bg_sample}")

            send("up 272")
            send("sleep 200")

            # 4. Toggle Mail subpanel closed via its arrow button
            send(f"move {mail_x} {bar_y} {out_w} {out_h}")
            send("sleep 80")
            send("down 272")
            send("sleep 40")
            send("up 272")
            send("sleep 300")
            if map_state(cenv, mail_sp_wid) != "IsUnMapped":
                raise SystemExit("Mail subpanel did not close when clicking its arrow button")

            print("PASS check-dtwm-wayland-panel-dnd: arrow-bar Add Subpanel, docked subpanel persistence, and transparent ARGB dtfile drag icon verified")
        finally:
            for p in (vp, dtf, comp):
                if p and p.poll() is None:
                    try:
                        os.killpg(p.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
            time.sleep(0.3)
            for p in (vp, dtf, comp):
                if p and p.poll() is None:
                    try:
                        os.killpg(p.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    p.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
