#!/usr/bin/env python3
"""Automated integration test for the CDE Front Panel Busy / Activity Indicator
Light (`CONTROL Busy`: `Fplite` -> `FpliteY`) in `dtwm-panel` + `dtwm-wayland`:

1. Verifies `dtwm-panel` maps CK Core Motif ABI 9 (`libXm.so.9`) under `dtwm-wayland`.
2. Broadcasts ToolTalk `DtActivity_Beginning` (`_DtSendActivityNotification`),
   verifies `dtwm-panel` lights and blinks `FpliteY` on the Front Panel Switch box,
   then broadcasts `DtActivity_Began` (`_DtSendActivityDoneNotification`) and
   verifies the indicator returns cleanly to idle `Fplite`.
3. Starts the busy indicator and maps a top-level client window, verifying
   `dtwm-wayland`'s `_DT_PANEL_BUSY_STOP_SEQ` bridge stops the Front Panel busy
   indicator automatically once the client window maps.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageChops


HELPER_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Intrinsic.h>
#include <X11/Xatom.h>
#include <Xm/Xm.h>
#include <Dt/Dt.h>
#include <Dt/SvcTT.h>
#include <Tt/tttk.h>

extern void _DtSendActivityNotification(int duration);
extern void _DtSendActivityDoneNotification(void);

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "begin";
    XtToolkitInitialize();
    XtAppContext app = XtCreateApplicationContext();
    Display *dpy = XtOpenDisplay(app, NULL, "busyprobe", "BusyProbe", NULL, 0, &argc, argv);
    if (!dpy) {
        fprintf(stderr, "XtOpenDisplay failed\n");
        return 2;
    }

    if (strcmp(mode, "mapwin") == 0) {
        Window win = XCreateSimpleWindow(
            dpy, DefaultRootWindow(dpy), 120, 90, 360, 220, 0,
            BlackPixel(dpy, DefaultScreen(dpy)), WhitePixel(dpy, DefaultScreen(dpy))
        );
        XStoreName(dpy, win, "BusyStopClient");
        XMapWindow(dpy, win);
        XSync(dpy, False);
        usleep(1200000);
        XDestroyWindow(dpy, win);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    Widget shell = XtVaAppCreateShell(
        "busyprobe", "BusyProbe", applicationShellWidgetClass, dpy,
        XmNmappedWhenManaged, False, XmNwidth, 1, XmNheight, 1, NULL
    );
    if (!DtAppInitialize(app, dpy, shell, "busyprobe", "BusyProbe")) {
        fprintf(stderr, "DtAppInitialize failed\n");
        return 3;
    }
    if (_DtSvcInitToolTalk(shell) != TT_OK) {
        fprintf(stderr, "_DtSvcInitToolTalk failed\n");
        return 4;
    }

    if (strcmp(mode, "begin") == 0) {
        _DtSendActivityNotification(10000);
    } else if (strcmp(mode, "done") == 0) {
        _DtSendActivityDoneNotification();
    }
    XSync(dpy, False);
    usleep(80000);
    XCloseDisplay(dpy);
    return 0;
}
"""


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


def extract_motif_dirs(makefile: Path) -> tuple[list[str], list[str]]:
    inc: list[str] = []
    lib: list[str] = []
    if not makefile.is_file():
        return inc, lib
    for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
        if ln.startswith(("CFLAGS =", "X_CFLAGS =", "DTCLIENTLIBS =", "CPPFLAGS =", "LDFLAGS =")):
            for tok in ln.split("=", 1)[1].split():
                if tok.startswith("-I") and "$(" not in tok:
                    inc.append(tok)
                elif (tok.startswith("-L") or tok.startswith("-Wl,-rpath")) and "$(" not in tok:
                    lib.append(tok)
    return inc, lib


def find_panel_box(c_env: dict[str, str]) -> tuple[int, int, int, int]:
    for _ in range(80):
        out = subprocess.run(
            ["xwininfo", "-root", "-tree"], env=c_env, capture_output=True, text=True
        ).stdout
        for line in out.splitlines():
            if "FrontPanel" in line or '"Dtwm"' in line:
                m = re.search(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", line)
                if m and int(m.group(1)) >= 600 and 50 <= int(m.group(2)) <= 140:
                    w, h, x, y = map(int, m.groups())
                    return (x, y, w, h)
        time.sleep(0.1)
    raise SystemExit("FrontPanel window did not map on XWayland")


def diff_panel_pixels(ref_im: Image.Image, cur_im: Image.Image, fp_box: tuple[int, int, int, int]) -> int:
    px, py, pw, ph = fp_box
    box = (max(0, px), max(0, py), px + pw, py + ph)
    raw = ImageChops.difference(ref_im.crop(box).convert("RGB"), cur_im.crop(box).convert("RGB")).tobytes()
    return sum(1 for i in range(0, len(raw), 3) if (raw[i] + raw[i + 1] + raw[i + 2]) > 40)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    cde_build, cde_src = find_cde_build_and_src(comp_bin)
    inc_flags, lib_flags = extract_motif_dirs(comp_bin.parent / "Makefile")
    cc = os.environ.get("CC", "cc")

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-wayland-busy-", dir=tmp_base) as work_s:
        work = Path(work_s)
        xdg = work / "xdg"
        home = work / "home"
        xdg.mkdir(mode=0o700)
        home.mkdir(mode=0o700)

        helper_c = work / "busy_helper.c"
        helper_bin = work / "busy_helper"
        helper_c.write_text(HELPER_C, encoding="utf-8")

        dtsvc_lib_dir = cde_build / "lib" / "DtSvc" / ".libs"
        if not dtsvc_lib_dir.is_dir():
            dtsvc_lib_dir = cde_build / "lib" / "DtSvc"
        tt_lib_dir = cde_build / "lib" / "tt" / "lib" / ".libs"
        dtxin_lib_dir = cde_build / "lib" / "DtXinerama" / ".libs"

        subprocess.run(
            [
                cc, "-O2", str(helper_c),
                f"-I{cde_build / 'include'}", f"-I{cde_src / 'include'}",
                *inc_flags,
                f"-L{dtsvc_lib_dir}", f"-Wl,-rpath,{dtsvc_lib_dir}",
                *([f"-L{tt_lib_dir}", f"-Wl,-rpath,{tt_lib_dir}"] if tt_lib_dir.is_dir() else []),
                *([f"-L{dtxin_lib_dir}", f"-Wl,-rpath,{dtxin_lib_dir}"] if dtxin_lib_dir.is_dir() else []),
                *lib_flags,
                "-lDtSvc", "-ltt", "-lDtXinerama", "-lXm", "-lXt", "-lX11",
                "-o", str(helper_bin),
            ],
            check=True,
        )

        env = os.environ.copy()
        for k in ("WAYLAND_DISPLAY", "DISPLAY", "TT_SESSION", "DTUSERSESSION"):
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

        status_file = work / "compositor.status"
        comp_log = (work / "dtwm-wayland.log").open("w", encoding="utf-8")
        comp = subprocess.Popen(
            [str(comp_bin), "--status-file", str(status_file)],
            env=env,
            stdout=comp_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        tt_pid: int | None = None

        try:
            wl_sock = x_sock = None
            for _ in range(80):
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
            fp_box = find_panel_box(c_env)

            # Verify dtwm-panel child maps libXm.so.9 and resolve isolated ttsession PID
            panel_pid = None
            for proc_entry in Path("/proc").iterdir():
                if not proc_entry.name.isdigit():
                    continue
                try:
                    st = (proc_entry / "stat").read_text(encoding="utf-8")
                    if "(dtwm-panel)" in st and f" {comp.pid} " in st:
                        panel_pid = int(proc_entry.name)
                        break
                except OSError:
                    continue
            if not panel_pid:
                raise SystemExit("failed to find child dtwm-panel process")
            maps = Path(f"/proc/{panel_pid}/maps").read_text(encoding="utf-8", errors="replace")
            if "libXm.so.9" not in maps:
                raise SystemExit("dtwm-panel is not mapped against libXm.so.9")

            # Wait for ToolTalk registration and panel settle
            for _ in range(40):
                tt_prop = subprocess.run(
                    ["xprop", "-root", "_SUN_TT_SESSION"], env=c_env, capture_output=True, text=True
                ).stdout
                m = re.search(r'"\S+\s+(\d+)\s+', tt_prop)
                if m:
                    tt_pid = int(m.group(1))
                    break
                time.sleep(0.1)
            time.sleep(0.8)

            idle_shot = work / "idle.png"
            subprocess.run(["grim", str(idle_shot)], env=c_env, check=True)
            im_idle = Image.open(idle_shot)

            # 1. ToolTalk DtActivity_Beginning -> lit & blinking -> DtActivity_Began -> off
            subprocess.run([str(helper_bin), "begin"], env=c_env, check=True)
            diffs_seen: list[int] = []
            for idx in range(7):
                shot = work / f"busy1_{idx}.png"
                subprocess.run(["grim", str(shot)], env=c_env, check=True)
                diffs_seen.append(diff_panel_pixels(im_idle, Image.open(shot), fp_box))
                time.sleep(0.24)
            if max(diffs_seen) < 20 or min(diffs_seen) >= max(diffs_seen):
                raise SystemExit(f"expected blinking FpliteY transitions on DtActivity_Beginning, got diffs={diffs_seen}")

            subprocess.run([str(helper_bin), "done"], env=c_env, check=True)
            settled_ok = False
            for _ in range(12):
                time.sleep(0.15)
                shot = work / "settled1.png"
                subprocess.run(["grim", str(shot)], env=c_env, check=True)
                if diff_panel_pixels(im_idle, Image.open(shot), fp_box) == 0:
                    settled_ok = True
                    break
            if not settled_ok:
                raise SystemExit("Front Panel busy light did not turn off after DtActivity_Began")

            # 2. DtActivity_Beginning -> client top-level window map (_DT_PANEL_BUSY_STOP_SEQ) -> auto-off
            subprocess.run([str(helper_bin), "begin"], env=c_env, check=True)
            lit_before_map = False
            for idx in range(5):
                shot = work / f"busy2_{idx}.png"
                subprocess.run(["grim", str(shot)], env=c_env, check=True)
                if diff_panel_pixels(im_idle, Image.open(shot), fp_box) >= 20:
                    lit_before_map = True
                    break
                time.sleep(0.2)
            if not lit_before_map:
                raise SystemExit("Front Panel busy light did not light before client window map")

            map_proc = subprocess.Popen([str(helper_bin), "mapwin"], env=c_env)
            try:
                stopped_on_map = False
                for _ in range(12):
                    time.sleep(0.15)
                    shot = work / "settled2.png"
                    subprocess.run(["grim", str(shot)], env=c_env, check=True)
                    if diff_panel_pixels(im_idle, Image.open(shot), fp_box) == 0:
                        stopped_on_map = True
                        break
                if not stopped_on_map:
                    raise SystemExit("Front Panel busy light did not stop when client window mapped")
            finally:
                map_proc.wait(timeout=5)

            print(f"check-dtwm-wayland-busy-light: OK (blink_diffs={diffs_seen})")
            return 0
        finally:
            try:
                os.killpg(comp.pid, signal.SIGTERM)
            except OSError:
                pass
            try:
                comp.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(comp.pid, signal.SIGKILL)
                except OSError:
                    pass
            if tt_pid:
                try:
                    os.kill(tt_pid, signal.SIGTERM)
                except OSError:
                    pass
            comp_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
