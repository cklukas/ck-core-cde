#!/usr/bin/env python3
"""WP-12 parallel dual-environment integration check (Wayland + X11/XLibre):
1. Automatic XDG .desktop discovery & live inotify watcher (`dtappintegrate --xdg --watch`):
   - Dynamically creates a synthetic .desktop entry while the session is active and
     verifies automatic `~/.dt/types/ck-xdg-apps.dt` + `~/.dt/appmanager/XDG_Graphics/*`
     generation without manual reload.
2. Quick-Launcher (`ck-launch`) & WM shortcut bindings (`Super` / `Super+Space` / `Alt+F2`):
   - Verifies `dtwm-wayland` (`Super+Space`) and classic X11 `dtwm` (`Super_L` tap)
     spawn `ck-launch --toggle` linked against CK-Core Motif ABI 9 (`libXm.so.9`).
   - Verifies open-window enumeration and activation via `zwlr_foreign_toplevel_manager_v1`
     on Wayland and `_NET_CLIENT_LIST` / `_NET_ACTIVE_WINDOW` on X11 `dtwm`.
   - Verifies inline math evaluation (`--eval`) and fuzzy XDG app launch (`--query --activate`).
3. Modernized `dtopen` & XDG `mimeapps.list` persistence:
   - Verifies `dtappintegrate-xdg --open-mime --desktop-id ... --set-default` updates
     `$XDG_CONFIG_HOME/mimeapps.list` `[Default Applications]` and subsequent `dtopen_pdf`
     dispatches to the selected XDG `.desktop` handler.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

from PIL import Image


X11_EWMH_HELPER_C = r"""
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*XTestFakeKeyEventFn)(Display *, unsigned int, Bool, unsigned long);

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);

    if (argc >= 2 && strcmp(argv[1], "tap-super") == 0) {
        void *xtst = dlopen("libXtst.so.6", RTLD_LAZY);
        if (!xtst) xtst = dlopen("libXtst.so", RTLD_LAZY);
        if (!xtst) {
            fprintf(stderr, "dlopen(libXtst) failed\n");
            XCloseDisplay(dpy);
            return 2;
        }
        XTestFakeKeyEventFn fake_key = (XTestFakeKeyEventFn)dlsym(xtst, "XTestFakeKeyEvent");
        KeyCode super_kc = XKeysymToKeycode(dpy, XK_Super_L);
        if (!fake_key || super_kc == 0) {
            fprintf(stderr, "missing XTestFakeKeyEvent or Super_L keycode\n");
            XCloseDisplay(dpy);
            return 3;
        }
        fake_key(dpy, super_kc, True, CurrentTime);
        XSync(dpy, False);
        usleep(60000);
        fake_key(dpy, super_kc, False, CurrentTime);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    Atom net_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, root, net_list, 0, 128, False, XA_WINDOW,
                           &actual, &fmt, &nitems, &after, &prop) == Success && prop) {
        Window *wins = (Window *)prop;
        printf("NET_CLIENT_COUNT=%lu\n", nitems);
        for (unsigned long i = 0; i < nitems; ++i) {
            char *nm = NULL;
            XFetchName(dpy, wins[i], &nm);
            printf("CLIENT=0x%lx:%s\n", (unsigned long)wins[i], nm ? nm : "");
            if (nm) XFree(nm);
        }
        XFree(prop);
    } else {
        printf("NET_CLIENT_COUNT=0\n");
    }

    prop = NULL;
    if (XGetWindowProperty(dpy, root, net_active, 0, 2, False, XA_WINDOW,
                           &actual, &fmt, &nitems, &after, &prop) == Success && prop && nitems >= 1) {
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
    const char *title = (argc > 1) ? argv[1] : "WP12 Dummy Window";
    const char *cls = (argc > 2) ? argv[2] : "Wp12Dummy";
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int scr = DefaultScreen(dpy);
    Window w = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 80, 80, 380, 240, 1,
                                   BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, w, title);
    XClassHint ch;
    ch.res_name = (char *)cls;
    ch.res_class = (char *)cls;
    XSetClassHint(dpy, w, &ch);
    XSelectInput(dpy, w, ExposureMask | StructureNotifyMask | FocusChangeMask);
    XMapWindow(dpy, w);
    XFlush(dpy);

    while (!g_stop) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }
        usleep(40000);
    }
    XCloseDisplay(dpy);
    return 0;
}
"""


def wait_until(pred, timeout: float = 10.0, step: float = 0.1, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_val = None
    while time.monotonic() < deadline:
        last_val = pred()
        if last_val:
            return last_val
        time.sleep(step)
    raise RuntimeError(f"Timed out waiting for {desc} (last={last_val!r})")


def verify_pid_maps_motif9(pid: int) -> None:
    maps_path = Path(f"/proc/{pid}/maps")
    if not maps_path.is_file():
        raise RuntimeError(f"/proc/{pid}/maps missing for ck-launch pid {pid}")
    maps = maps_path.read_text(encoding="utf-8", errors="replace")
    if "libXm.so.9" not in maps:
        raise RuntimeError(f"PID {pid} does not map libXm.so.9!\n{maps}")


def find_child_proc_by_cmd(needle: str, home_str: str) -> int | None:
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        try:
            comm = (entry / "comm").read_text(encoding="utf-8", errors="replace").strip()
            if comm != needle:
                continue
            environ = (entry / "environ").read_bytes().decode("utf-8", errors="replace")
            if f"HOME={home_str}\x00" in environ:
                return pid
        except OSError:
            continue
    return None


def terminate_procs(procs: list[subprocess.Popen]) -> None:
    for pr in reversed(procs):
        if pr.poll() is None:
            try:
                os.killpg(pr.pid, signal.SIGTERM)
            except OSError:
                try:
                    pr.terminate()
                except OSError:
                    pass
    time.sleep(0.25)
    for pr in reversed(procs):
        if pr.poll() is None:
            try:
                os.killpg(pr.pid, signal.SIGKILL)
            except OSError:
                try:
                    pr.kill()
                except OSError:
                    pass


def build_base_env(comp_bin: Path, home: Path, xdg_rt: Path, xdg_data: Path, xdg_cfg: Path, sys_xdg: Path) -> dict[str, str]:
    env = os.environ.copy()
    # Strip outer desktop variables so lanes are 100% isolated
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "DTUSERSESSION"):
        env.pop(k, None)

    cde_build_root = comp_bin.parents[2]
    ld_parts: list[str] = []
    for cand in (
        Path("/var/tmp/ck-wayland-build/wp00/motif-prefix/lib"),
        Path("/var/tmp/ck-wayland-build/wp00/fs-prefix/lib"),
        Path("/usr/dt/lib"),
        Path("/usr/dt/lib/dtksh/ksh93"),
        cde_build_root / "lib/DtSvc/.libs",
        cde_build_root / "lib/DtWidget/.libs",
        cde_build_root / "lib/DtHelp/.libs",
        cde_build_root / "lib/DtTerm/.libs",
        cde_build_root / "lib/DtPrint/.libs",
        cde_build_root / "lib/tt/lib/.libs",
        cde_build_root / "lib/csa/.libs",
    ):
        if cand.is_dir():
            ld_parts.append(str(cand))
    if env.get("LD_LIBRARY_PATH"):
        ld_parts.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(ld_parts)

    path_parts = [
        str(comp_bin.parent),
        str(cde_build_root / "programs/dtappintegrate"),
        str(cde_build_root / "programs/dtopen"),
        "/usr/dt/bin",
        "/usr/bin",
        "/bin",
    ]
    env["PATH"] = ":".join(path_parts)
    env.update(
        {
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg_rt),
            "XDG_DATA_HOME": str(xdg_data),
            "XDG_CONFIG_HOME": str(xdg_cfg),
            "XDG_DATA_DIRS": str(sys_xdg),
            "XDG_CONFIG_DIRS": str(xdg_cfg),
            "XDG_CURRENT_DESKTOP": "CDE",
            "LC_ALL": "C.UTF-8",
            "LANG": "C.UTF-8",
        }
    )
    return env


def verify_shared_xdg_and_dtopen(
    lane_tag: str,
    env: dict[str, str],
    work: Path,
    home: Path,
    xdg_data: Path,
    xdg_cfg: Path,
    dtappint_bin: Path,
    dtappint_xdg_bin: Path,
    dtopen_dir: Path,
    procs: list[subprocess.Popen],
) -> Path:
    apps_dir = xdg_data / "applications"
    apps_dir.mkdir(parents=True, exist_ok=True)
    pidfile = work / "xdg-watch.pid"

    watch_proc = subprocess.Popen(
        [str(dtappint_bin), "--xdg", "--watch", "--pidfile", str(pidfile)],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    procs.append(watch_proc)
    wait_until(lambda: pidfile.is_file(), timeout=6.0, desc=f"{lane_tag} watcher pidfile")

    # Dynamically drop a brand-new .desktop entry AFTER watcher startup to prove live inotify indexing
    launched_log = work / "synth-launched.log"
    desktop_id = f"wp12-{lane_tag}-synth.desktop"
    synth_name = f"WP12 {lane_tag.upper()} Synth Viewer"
    desktop_body = (
        "[Desktop Entry]\n"
        "Type=Application\n"
        f"Name={synth_name}\n"
        "GenericName=Synthetic PDF Handler\n"
        "Comment=WP12 live inotify & mimeapps verification helper\n"
        "Categories=Graphics;Viewer;\n"
        "MimeType=application/pdf;\n"
        f"Exec=/bin/sh -c 'printf \"%%s\\n\" \"$@\" >> \"{launched_log}\"' sh %F\n"
        "Terminal=false\n"
    )
    (apps_dir / desktop_id).write_text(desktop_body, encoding="utf-8")

    dt_file = home / ".dt/types/ck-xdg-apps.dt"
    expected_prefix = f"CkXdg_wp12_{lane_tag}_synth_"
    gfx_dir = home / ".dt/appmanager/XDG_Graphics"

    def check_indexed_stub():
        if not dt_file.is_file() or not gfx_dir.is_dir():
            return False
        txt = dt_file.read_text(encoding="utf-8", errors="replace")
        if expected_prefix not in txt or synth_name not in txt:
            return False
        stubs = list(gfx_dir.glob(f"{expected_prefix}*"))
        return bool(stubs and os.access(stubs[0], os.X_OK))

    wait_until(
        check_indexed_stub,
        timeout=6.0,
        desc=f"{lane_tag} live inotify ck-xdg-apps.dt + XDG_Graphics stub",
    )

    # Verify ck-launch headless math evaluator
    math_out = subprocess.check_output(["ck-launch", "--eval", "sqrt(144) + 2^5"], env=env, text=True).strip()
    if math_out != "44":
        raise RuntimeError(f"{lane_tag}: expected ck-launch --eval -> 44, got {math_out!r}")

    # Verify ck-launch fuzzy query + activation of the newly added XDG app
    q_act = subprocess.check_output(
        ["ck-launch", "--query", synth_name, "--activate"],
        env=env,
        text=True,
    )
    if synth_name not in q_act:
        raise RuntimeError(f"{lane_tag}: ck-launch --query missed {synth_name!r}:\n{q_act}")
    wait_until(lambda: launched_log.is_file(), timeout=5.0, desc=f"{lane_tag} ck-launch XDG app spawn")

    # Verify Open With... default association persistence in mimeapps.list + dtopen_pdf execution
    sample1 = work / f"doc1-{lane_tag}.pdf"
    sample2 = work / f"doc2-{lane_tag}.pdf"
    sample1.write_bytes(b"%PDF-1.4\n%WP12 test 1\n")
    sample2.write_bytes(b"%PDF-1.4\n%WP12 test 2\n")

    subprocess.run(
        [
            str(dtappint_xdg_bin),
            "--open-mime",
            "--desktop-id",
            desktop_id,
            "--set-default",
            str(sample1),
        ],
        env=env,
        check=True,
    )
    mimeapps = xdg_cfg / "mimeapps.list"
    wait_until(
        lambda: mimeapps.is_file()
        and f"application/pdf={desktop_id}" in mimeapps.read_text(encoding="utf-8", errors="replace"),
        timeout=4.0,
        desc=f"{lane_tag} mimeapps.list persistence",
    )
    wait_until(
        lambda: launched_log.is_file()
        and str(sample1) in launched_log.read_text(encoding="utf-8", errors="replace"),
        timeout=4.0,
        desc=f"{lane_tag} explicit --open-mime launch",
    )

    dtopen_pdf = dtopen_dir / "dtopen_pdf"
    if not dtopen_pdf.is_file():
        dtopen_pdf = Path("/usr/dt/bin/dtopen_pdf")
    subprocess.run([str(dtopen_pdf), str(sample2)], env=env, check=True)
    wait_until(
        lambda: str(sample2) in launched_log.read_text(encoding="utf-8", errors="replace"),
        timeout=4.0,
        desc=f"{lane_tag} dtopen_pdf via persisted mimeapps.list",
    )
    return launched_log


def run_wayland_lane(
    comp_bin: Path,
    dummy_win_bin: Path,
    dtappint_bin: Path,
    dtappint_xdg_bin: Path,
    dtopen_dir: Path,
    save_dir: Path | None,
) -> str:
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp12-wayland-", dir=tmp_base) as td:
        work = Path(td)
        home = work / "home"
        xdg_rt = work / "rt"
        xdg_data = work / "xdg-data"
        xdg_cfg = work / "xdg-cfg"
        sys_xdg = work / "sys-xdg"
        for d in (home / ".dt/sessions", xdg_rt, xdg_data, xdg_cfg, sys_xdg / "applications"):
            d.mkdir(parents=True, exist_ok=True)
        xdg_rt.chmod(0o700)

        env = build_base_env(comp_bin, home, xdg_rt, xdg_data, xdg_cfg, sys_xdg)
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
        try:
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
                if kv.get("WAYLAND_DISPLAY") and kv.get("DISPLAY"):
                    return kv
                return None

            kv = wait_until(read_wl_status, timeout=12.0, desc="dtwm-wayland startup")
            env["WAYLAND_DISPLAY"] = kv["WAYLAND_DISPLAY"]
            env["DISPLAY"] = kv["DISPLAY"]
            time.sleep(1.0)

            verify_shared_xdg_and_dtopen(
                "wayland", env, work, home, xdg_data, xdg_cfg,
                dtappint_bin, dtappint_xdg_bin, dtopen_dir, procs,
            )

            # Open two windows and verify foreign-toplevel discovery
            w1 = subprocess.Popen([str(dummy_win_bin), "WP12 Alpha Window", "Wp12Alpha"], env=env, start_new_session=True)
            w2 = subprocess.Popen([str(dummy_win_bin), "WP12 Beta Window", "Wp12Beta"], env=env, start_new_session=True)
            procs.extend([w1, w2])

            win_list = wait_until(
                lambda: (
                    out := subprocess.check_output(["ck-launch", "--list-windows"], env=env, text=True)
                )
                and ("WP12 Alpha Window" in out and "WP12 Beta Window" in out and "wl." in out)
                and out,
                timeout=8.0,
                desc="Wayland zwlr_foreign_toplevel_manager_v1 window discovery",
            )

            # Activate WP12 Alpha Window via ck-launch
            subprocess.run(["ck-launch", "--query", "WP12 Alpha Window", "--activate"], env=env, check=True)

            # Trigger Super+Space via wlrctl virtual keyboard -> dtwm-wayland spawns ck-launch --toggle
            subprocess.run(["wlrctl", "keyboard", "type", " ", "modifiers", "SUPER"], env=env, check=True)
            ck_pid = wait_until(
                lambda: find_child_proc_by_cmd("ck-launch", str(home)),
                timeout=8.0,
                desc="dtwm-wayland Super+Space spawning ck-launch",
            )
            verify_pid_maps_motif9(ck_pid)
            time.sleep(0.6)

            shot = work / "wp12-headless-wayland-ck-launch.png"
            subprocess.run(["grim", str(shot)], env=env, check=True)
            with Image.open(shot) as im:
                if im.size != (1280, 900):
                    raise RuntimeError(f"Unexpected Wayland screenshot size: {im.size}")
            if save_dir is not None:
                save_dir.mkdir(parents=True, exist_ok=True)
                shutil.copy2(shot, save_dir / shot.name)

            # Toggle ck-launch off cleanly via singleton IPC
            subprocess.run(["ck-launch", "--quit"], env=env, check=True)
            return f"Wayland lane OK (ck-launch pid={ck_pid} libXm.so.9, foreign-toplevel={len(win_list.splitlines())} wins)"
        finally:
            terminate_procs(procs)


def pick_free_x11_display() -> int:
    for d in range(42, 95):
        if not Path(f"/tmp/.X{d}-lock").exists() and not Path(f"/tmp/.X11-unix/X{d}").exists():
            return d
    raise RuntimeError("No free Xvfb display number found in 42..94")


def run_x11_lane(
    comp_bin: Path,
    dtwm_x11_bin: Path,
    ewmh_helper_bin: Path,
    dummy_win_bin: Path,
    dtappint_bin: Path,
    dtappint_xdg_bin: Path,
    dtopen_dir: Path,
) -> str:
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp12-x11-", dir=tmp_base) as td:
        work = Path(td)
        home = work / "home"
        xdg_rt = work / "rt"
        xdg_data = work / "xdg-data"
        xdg_cfg = work / "xdg-cfg"
        sys_xdg = work / "sys-xdg"
        for d in (home / ".dt/sessions", xdg_rt, xdg_data, xdg_cfg, sys_xdg / "applications"):
            d.mkdir(parents=True, exist_ok=True)
        xdg_rt.chmod(0o700)

        env = build_base_env(comp_bin, home, xdg_rt, xdg_data, xdg_cfg, sys_xdg)
        env["XDG_SESSION_TYPE"] = "x11"
        dnum = pick_free_x11_display()
        disp = f":{dnum}"
        env["DISPLAY"] = disp

        procs: list[subprocess.Popen] = []
        try:
            xvfb = subprocess.Popen(
                ["Xvfb", disp, "-screen", "0", "1280x900x24", "-nolisten", "tcp", "-ac"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            procs.append(xvfb)
            wait_until(
                lambda: subprocess.run([str(ewmh_helper_bin)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0,
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
            time.sleep(1.5)

            verify_shared_xdg_and_dtopen(
                "x11", env, work, home, xdg_data, xdg_cfg,
                dtappint_bin, dtappint_xdg_bin, dtopen_dir, procs,
            )

            # Open two X11 windows and verify X11 dtwm publishes _NET_CLIENT_LIST & _NET_ACTIVE_WINDOW
            w1 = subprocess.Popen([str(dummy_win_bin), "WP12 X11 First", "Wp12First"], env=env, start_new_session=True)
            time.sleep(0.35)
            w2 = subprocess.Popen([str(dummy_win_bin), "WP12 X11 Second", "Wp12Second"], env=env, start_new_session=True)
            procs.extend([w1, w2])

            ewmh_txt = wait_until(
                lambda: (
                    t := subprocess.check_output([str(ewmh_helper_bin)], env=env, text=True)
                )
                and ("WP12 X11 First" in t and "WP12 X11 Second" in t)
                and t,
                timeout=8.0,
                desc="X11 dtwm _NET_CLIENT_LIST population",
            )

            win_list = subprocess.check_output(["ck-launch", "--list-windows"], env=env, text=True)
            if "WP12 X11 First" not in win_list or "WP12 X11 Second" not in win_list or "x11.0x" not in win_list:
                raise RuntimeError(f"X11 ck-launch --list-windows missing X11 windows:\n{win_list}")

            # Switch active window back to "WP12 X11 First" via ck-launch -> _NET_ACTIVE_WINDOW ClientMessage!
            subprocess.run(["ck-launch", "--query", "WP12 X11 First", "--activate"], env=env, check=True)
            wait_until(
                lambda: "WP12 X11 First" in subprocess.check_output([str(ewmh_helper_bin)], env=env, text=True).split("NET_ACTIVE=", 1)[-1],
                timeout=6.0,
                desc="X11 dtwm _NET_ACTIVE_WINDOW activation of WP12 X11 First",
            )

            # Tap bare Super_L via XTest -> verify X11 dtwm passive grab spawns ck-launch --toggle!
            subprocess.run([str(ewmh_helper_bin), "tap-super"], env=env, check=True)
            ck_pid = wait_until(
                lambda: find_child_proc_by_cmd("ck-launch", str(home)),
                timeout=8.0,
                desc="X11 dtwm Super_L tap spawning ck-launch",
            )
            verify_pid_maps_motif9(ck_pid)

            subprocess.run(["ck-launch", "--quit"], env=env, check=True)
            return f"X11 lane OK (display={disp}, ck-launch pid={ck_pid} libXm.so.9, _NET_CLIENT_LIST verified)"
        finally:
            terminate_procs(procs)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True, type=Path)
    ap.add_argument("--save-dir", type=Path, default=None)
    args = ap.parse_args()

    if not os.environ.get("CK_WP12_IN_DBUS") and shutil.which("dbus-run-session"):
        env2 = os.environ.copy()
        env2["CK_WP12_IN_DBUS"] = "1"
        cmd = ["dbus-run-session", "--", "python3", str(Path(__file__).resolve()), "--compositor", str(args.compositor)]
        if args.save_dir is not None:
            cmd.extend(["--save-dir", str(args.save_dir)])
        return subprocess.call(cmd, env=env2)

    comp_bin = args.compositor.resolve()
    dtwm_x11_bin = comp_bin.parent / ".libs/dtwm"
    if not dtwm_x11_bin.is_file():
        dtwm_x11_bin = comp_bin.parent / "dtwm"
    if not dtwm_x11_bin.is_file():
        dtwm_x11_bin = Path("/usr/dt/bin/dtwm")

    cde_build_root = comp_bin.parents[2]
    src_dtwm = Path(__file__).resolve().parents[1]
    src_cde = src_dtwm.parents[1]

    dtappint_bin = cde_build_root / "programs/dtappintegrate/dtappintegrate"
    if not dtappint_bin.is_file():
        dtappint_bin = Path("/usr/dt/bin/dtappintegrate")
    dtappint_xdg_bin = src_cde / "programs/dtappintegrate/dtappintegrate-xdg"
    if not dtappint_xdg_bin.is_file():
        dtappint_xdg_bin = Path("/usr/dt/bin/dtappintegrate-xdg")
    dtopen_dir = cde_build_root / "programs/dtopen"

    cc = os.environ.get("CC", "cc")
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp12-fixtures-", dir=tmp_base) as fx_s:
        fx = Path(fx_s)
        ewmh_c = fx / "x11_ewmh_helper.c"
        ewmh_bin = fx / "x11_ewmh_helper"
        ewmh_c.write_text(X11_EWMH_HELPER_C, encoding="utf-8")
        subprocess.run([cc, "-O2", "-Wall", str(ewmh_c), "-lX11", "-ldl", "-o", str(ewmh_bin)], check=True)

        dummy_c = fx / "x11_dummy_win.c"
        dummy_bin = fx / "x11_dummy_win"
        dummy_c.write_text(X11_DUMMY_WIN_C, encoding="utf-8")
        subprocess.run([cc, "-O2", "-Wall", str(dummy_c), "-lX11", "-o", str(dummy_bin)], check=True)

        t0 = time.monotonic()
        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_wl = pool.submit(
                run_wayland_lane,
                comp_bin,
                dummy_bin,
                dtappint_bin,
                dtappint_xdg_bin,
                dtopen_dir,
                args.save_dir,
            )
            fut_x11 = pool.submit(
                run_x11_lane,
                comp_bin,
                dtwm_x11_bin,
                ewmh_bin,
                dummy_bin,
                dtappint_bin,
                dtappint_xdg_bin,
                dtopen_dir,
            )
            res_wl = fut_wl.result()
            res_x11 = fut_x11.result()
        dt = time.monotonic() - t0
        print(f"WP-12 parallel dual-environment check passed in {dt:.2f}s:\n  - {res_wl}\n  - {res_x11}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
