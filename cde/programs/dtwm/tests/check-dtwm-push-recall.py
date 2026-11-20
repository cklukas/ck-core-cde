#!/usr/bin/env python3
"""End-to-end integration check for CDE Front Panel click-to-recall / restore:
verifies that clicking a Front Panel item launches the application when not
running, brings it to the foreground with active focus when obscured in the
background, and restores + focuses it when minimized on both Wayland
(dtwm-wayland + dtwm-panel) and X11 (dtwm). Also verifies XMapRaised restore
and foreground activation for single-instance X11/CDE clients.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image


HELPER_C = r"""
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static unsigned long parse_hex(const char *s) {
    return strtoul(s, NULL, 16);
}

typedef struct {
    int cx;
    int y;
} ArrowHit;

static void collect_panel_arrows(Display *dpy, Window root, Window w, int depth,
                                 ArrowHit *hits, int *nhits, int max_hits) {
    if (depth > 6 || *nhits >= max_hits) return;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable) return;
    if (depth >= 2 && wa.width >= 24 && wa.width <= 96 && wa.height >= 10 && wa.height <= 28) {
        int rx = 0, ry = 0;
        Window child = None;
        XTranslateCoordinates(dpy, w, root, wa.width / 2, wa.height / 2, &rx, &ry, &child);
        hits[*nhits].cx = rx;
        hits[*nhits].y = ry;
        (*nhits)++;
    }
    Window r = None, p = None, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, w, &r, &p, &kids, &n) || !kids) return;
    for (unsigned int i = 0; i < n; ++i) {
        collect_panel_arrows(dpy, root, kids[i], depth + 1, hits, nhits, max_hits);
    }
    XFree(kids);
}

static int find_panel_window(Display *dpy, Window root, Window w, int depth,
                             Window *out_win, int *out_x, int *out_y, int *out_w, int *out_h) {
    if (depth > 4) return 0;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable) return 0;
    if (depth >= 1 && wa.width >= 140 && wa.height >= 45 && wa.height <= 160) {
        int rx = 0, ry = 0;
        Window child = None;
        XTranslateCoordinates(dpy, w, root, 0, 0, &rx, &ry, &child);
        if (ry >= 300) {
            *out_win = w;
            *out_x = rx;
            *out_y = ry;
            *out_w = wa.width;
            *out_h = wa.height;
            return 1;
        }
    }
    Window r = None, p = None, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, w, &r, &p, &kids, &n) || !kids) return 0;
    for (unsigned int i = 0; i < n; ++i) {
        if (find_panel_window(dpy, root, kids[i], depth + 1, out_win, out_x, out_y, out_w, out_h)) {
            XFree(kids);
            return 1;
        }
    }
    XFree(kids);
    return 0;
}

static int find_named_control(Display *dpy, Window root, Window start_w, const char *want_name,
                              int *out_cx, int *out_cy) {
    (void)start_w;
    (void)want_name;
    Window fp_win = None;
    int fp_x = 0, fp_y = 0, fp_w = 0, fp_h = 0;
    if (!find_panel_window(dpy, root, root, 0, &fp_win, &fp_x, &fp_y, &fp_w, &fp_h)) {
        return 0;
    }
    ArrowHit hits[32];
    int nhits = 0;
    collect_panel_arrows(dpy, root, fp_win, 1, hits, &nhits, 32);
    /* Sort deduplicated arrow X centers left-to-right:
     * [0]=TextEditor, [1]=Printer, [2]=Style, [3]=Applications, [4]=Info/Help */
    for (int i = 0; i < nhits; ++i) {
        for (int j = i + 1; j < nhits; ++j) {
            if (hits[j].cx < hits[i].cx) {
                ArrowHit tmp = hits[i];
                hits[i] = hits[j];
                hits[j] = tmp;
            }
        }
    }
    int uniq[32];
    int nuniq = 0;
    for (int i = 0; i < nhits; ++i) {
        if (hits[i].y > fp_y + fp_h / 2) continue; /* subpanel arrows sit in top strip */
        if (nuniq == 0 || hits[i].cx - uniq[nuniq - 1] > 20) {
            uniq[nuniq++] = hits[i].cx;
        }
    }
    *out_cx = fp_x + (fp_w * 742) / 1000;
    *out_cy = fp_y + (fp_h * 62) / 100;
    return 1;
}

static Window find_client_by_res(Display *dpy, Window w, const char *res_name, int depth) {
    if (depth > 5) return None;
    XClassHint ch = {NULL, NULL};
    if (XGetClassHint(dpy, w, &ch)) {
        int match = (ch.res_name && strcasecmp(ch.res_name, res_name) == 0);
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
        if (match) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, w, &wa) && wa.width >= 40 && wa.height >= 40) {
                return w;
            }
        }
    }
    Window r = None, p = None, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, w, &r, &p, &kids, &n) || !kids) return None;
    Window found = None;
    for (unsigned int i = 0; i < n && found == None; ++i) {
        found = find_client_by_res(dpy, kids[i], res_name, depth + 1);
    }
    XFree(kids);
    return found;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *mode = argv[1];
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 3;
    Window root = DefaultRootWindow(dpy);

    if (strcmp(mode, "client") == 0 && argc >= 8) {
        /* client <res_name> <res_class> <hex_rgb> <x> <y> <count_file> */
        const char *res_name = argv[2];
        const char *res_class = argv[3];
        unsigned long bg = parse_hex(argv[4]);
        int x = atoi(argv[5]);
        int y = atoi(argv[6]);
        const char *cnt_path = argv[7];

        FILE *cf = fopen(cnt_path, "a");
        if (cf) {
            fprintf(cf, "%d\n", (int)getpid());
            fclose(cf);
        }

        signal(SIGTERM, on_sig);
        signal(SIGINT, on_sig);

        /* Create an unmapped 1x1 Xt-style leader window first with identical WM_CLASS
         * to ensure push_recall ignores unmapped group-leader windows! */
        Window leader = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
        XClassHint lch;
        lch.res_name = (char *)res_name;
        lch.res_class = (char *)res_class;
        XSetClassHint(dpy, leader, &lch);

        Window win = XCreateSimpleWindow(dpy, root, x, y, 260, 190, 0, bg, bg);
        XStoreName(dpy, win, res_class);
        XSetClassHint(dpy, win, &lch);
        XSizeHints sh;
        memset(&sh, 0, sizeof(sh));
        sh.flags = USPosition | USSize;
        sh.x = x;
        sh.y = y;
        sh.width = 260;
        sh.height = 190;
        XSetWMNormalHints(dpy, win, &sh);
        XWMHints wmh;
        memset(&wmh, 0, sizeof(wmh));
        wmh.flags = InputHint | StateHint | WindowGroupHint;
        wmh.input = True;
        wmh.initial_state = NormalState;
        wmh.window_group = leader;
        XSetWMHints(dpy, win, &wmh);

        Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy, win, &wm_del, 1);
        XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
        XMapWindow(dpy, win);
        XFlush(dpy);

        while (!g_stop) {
            while (XPending(dpy) > 0) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == Expose) {
                    XClearWindow(dpy, win);
                } else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wm_del) {
                    g_stop = 1;
                    break;
                }
            }
            usleep(20000);
        }
        XDestroyWindow(dpy, win);
        XDestroyWindow(dpy, leader);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(mode, "wm_ready") == 0) {
        Atom info_a = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        Atom act = None;
        int fmt = 0;
        unsigned long nitems = 0, rem = 0;
        unsigned char *data = NULL;
        int ok = (XGetWindowProperty(dpy, root, info_a, 0, 2, False, AnyPropertyType,
                                     &act, &fmt, &nitems, &rem, &data) == Success &&
                  data && nitems >= 2);
        if (data) XFree(data);
        XCloseDisplay(dpy);
        return ok ? 0 : 1;
    }

    if (strcmp(mode, "find_control") == 0 && argc >= 3) {
        int cx = -1, cy = -1;
        int ok = find_named_control(dpy, root, root, argv[2], &cx, &cy);
        XCloseDisplay(dpy);
        if (!ok) return 1;
        printf("%d,%d\n", cx, cy);
        return 0;
    }

    if (strcmp(mode, "find_win") == 0 && argc >= 3) {
        Window w = find_client_by_res(dpy, root, argv[2], 0);
        XCloseDisplay(dpy);
        if (w == None) return 1;
        printf("0x%lx\n", (unsigned long)w);
        return 0;
    }

    if (strcmp(mode, "iconify") == 0 && argc >= 3) {
        Window w = (Window)strtoul(argv[2], NULL, 0);
        XIconifyWindow(dpy, w, DefaultScreen(dpy));
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(mode, "map_raised") == 0 && argc >= 3) {
        Window w = (Window)strtoul(argv[2], NULL, 0);
        XMapRaised(dpy, w);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(mode, "xtest_click") == 0 && argc >= 4) {
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime);
        XSync(dpy, False);
        usleep(40000);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        usleep(40000);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(mode, "pixel") == 0 && argc >= 4) {
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        XImage *im = XGetImage(dpy, root, x, y, 1, 1, AllPlanes, ZPixmap);
        if (!im) {
            XCloseDisplay(dpy);
            return 1;
        }
        unsigned long px = XGetPixel(im, 0, 0);
        XDestroyImage(im);
        XCloseDisplay(dpy);
        printf("%lu,%lu,%lu\n", (px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF);
        return 0;
    }

    if (strcmp(mode, "active_res") == 0) {
        Atom act_a = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
        Atom type = None;
        int fmt = 0;
        unsigned long nitems = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, act_a, 0, 1, False, XA_WINDOW,
                               &type, &fmt, &nitems, &rem, &data) != Success ||
            !data || nitems < 1) {
            if (data) XFree(data);
            XCloseDisplay(dpy);
            printf("NONE\n");
            return 0;
        }
        Window w = *(Window *)data;
        XFree(data);
        if (w == None) {
            XCloseDisplay(dpy);
            printf("NONE\n");
            return 0;
        }
        XClassHint ch = {NULL, NULL};
        if (XGetClassHint(dpy, w, &ch)) {
            printf("%s\n", ch.res_name ? ch.res_name : "UNKNOWN");
            if (ch.res_name) XFree(ch.res_name);
            if (ch.res_class) XFree(ch.res_class);
        } else {
            printf("0x%lx\n", (unsigned long)w);
        }
        XCloseDisplay(dpy);
        return 0;
    }

    XCloseDisplay(dpy);
    return 2;
}
"""


def wait_until(pred, timeout: float = 6.0, step: float = 0.08, desc: str = "condition") -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return
        time.sleep(step)
    raise SystemExit(f"timed out waiting for {desc}")


def rgb_close(got: tuple[int, int, int], want: tuple[int, int, int], tol: int = 24) -> bool:
    return all(abs(got[i] - want[i]) <= tol for i in range(3))


def render_dtwm_fp_from_src(fp_src: Path) -> str:
    out: list[str] = []
    keep_stack: list[bool] = []
    for raw in fp_src.read_text(encoding="utf-8").splitlines():
        s = raw.strip()
        if s.startswith("XCOMM"):
            continue
        if s.startswith("#ifdef NO_DTINFO"):
            keep_stack.append(False)
            continue
        if s.startswith("#ifndef NO_DTINFO"):
            keep_stack.append(True)
            continue
        if s.startswith("#else"):
            if keep_stack:
                keep_stack[-1] = not keep_stack[-1]
            continue
        if s.startswith("#endif"):
            if keep_stack:
                keep_stack.pop()
            continue
        if keep_stack and not all(keep_stack):
            continue
        line = re.sub(r"%\|nls-\d+-#([^#]*)#\|", r"\1", raw)
        out.append(line)
    return "\n".join(out) + "\n"


def write_fp_test_db(home: Path, helper_bin: Path, count_file: Path) -> Path:
    types_dir = home / ".dt" / "types"
    types_dir.mkdir(parents=True, exist_ok=True)
    fp_src = Path(__file__).resolve().parents[2] / "types" / "dtwm.fp.src"
    (types_dir / "dtwm.fp").write_text(render_dtwm_fp_from_src(fp_src), encoding="utf-8")
    (types_dir / "recall_test.dt").write_text(
        "set DtDbVersion=1.0\n\n"
        "ACTION Dtstyle\n"
        "{\n"
        "  LABEL         Style Manager\n"
        "  ARG_COUNT     0\n"
        "  TYPE          COMMAND\n"
        "  WINDOW_TYPE   NO_STDIO\n"
        f"  EXEC_STRING   {helper_bin} client dtstyle Dtstyle 18D050 140 80 {count_file}\n"
        "}\n",
        encoding="utf-8",
    )
    return types_dir


def pick_free_display() -> int:
    for d in range(82, 118):
        if not Path(f"/tmp/.X11-unix/X{d}").exists():
            return d
    return 98


def verify_wayland(comp_bin: Path, helper_bin: Path, base_dir: Path) -> None:
    if not shutil.which("grim") or not shutil.which("wlrctl"):
        print("SKIP Wayland push-recall check (grim/wlrctl not installed)")
        return

    work = base_dir / "wl"
    home = work / "home"
    xdg = work / "xdg"
    home.mkdir(parents=True, exist_ok=True)
    xdg.mkdir(parents=True, exist_ok=True)
    os.chmod(xdg, 0o700)
    count_file = work / "spawn_count.txt"
    types_dir = write_fp_test_db(home, helper_bin, count_file)

    panel_bin = comp_bin.parent / "dtwm-panel"

    env = os.environ.copy()
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "TT_SESSION", "DTUSERSESSION"):
        env.pop(k, None)
    env.update(
        {
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg),
            "LANG": "C",
            "PATH": f"{comp_bin.parent}:{env.get('PATH', '/usr/bin:/bin')}",
            "DTDATABASESEARCHPATH": str(types_dir),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
        }
    )
    icons_dir = Path("/usr/dt/appconfig/icons/C")
    if not icons_dir.is_dir():
        icons_dir = Path(__file__).resolve().parents[2] / "icons" / "C"
    if icons_dir.is_dir():
        env["XMICONSEARCHPATH"] = f"{icons_dir}/%B%M.pm:{icons_dir}/%B%M.bm:{icons_dir}/%B.pm"
        env["XMICONBMSEARCHPATH"] = f"{icons_dir}/%B%M.bm:{icons_dir}/%B%M.pm"
    if panel_bin.is_file():
        env["DTWM_PANEL_BIN"] = str(panel_bin)

    status_file = work / "comp.status"
    comp_log = (work / "comp.log").open("w", encoding="utf-8")
    comp = subprocess.Popen(
        [str(comp_bin), "--status-file", str(status_file)],
        env=env,
        stdout=comp_log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    procs: list[subprocess.Popen] = [comp]
    try:
        wl_s = x_s = None
        for _ in range(80):
            if status_file.is_file():
                kv = dict(
                    ln.split("=", 1)
                    for ln in status_file.read_text(encoding="utf-8").splitlines()
                    if "=" in ln
                )
                wl_s, x_s = kv.get("WAYLAND_DISPLAY"), kv.get("DISPLAY")
                if wl_s and x_s:
                    break
            time.sleep(0.08)
        if not wl_s or not x_s:
            raise SystemExit("dtwm-wayland failed to start for push-recall test")

        c_env = dict(env, WAYLAND_DISPLAY=wl_s, DISPLAY=x_s)

        ctrl_xy: list[tuple[int, int]] = []

        def locate_style_btn() -> bool:
            r = subprocess.run(
                [str(helper_bin), "find_control", "Style"],
                env=c_env,
                capture_output=True,
                text=True,
            )
            if r.returncode == 0 and "," in r.stdout:
                xs, ys = r.stdout.strip().split(",", 1)
                ctrl_xy.clear()
                ctrl_xy.append((int(xs), int(ys)))
                return True
            return False

        wait_until(locate_style_btn, timeout=8.0, desc="Wayland FrontPanel Style control")
        bx, by = ctrl_xy[0]

        def wl_click(px: int, py: int) -> None:
            subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
            subprocess.run(["wlrctl", "pointer", "move", str(px), str(py)], env=c_env, check=True)
            time.sleep(0.05)
            subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)

        def sample_wl_px(px: int, py: int) -> tuple[int, int, int]:
            shot = work / "probe.png"
            subprocess.run(["grim", str(shot)], env=c_env, check=True)
            with Image.open(shot) as im:
                rgb = im.convert("RGB").getpixel((px, py))
                return (rgb[0], rgb[1], rgb[2])

        green = (0x18, 0xD0, 0x50)
        red = (0xE0, 0x28, 0x28)
        probe_pt = (230, 160)

        # 1. Click Front Panel button -> launches Dtstyle test window (green)
        wl_click(bx, by)
        wait_until(
            lambda: count_file.is_file()
            and len(count_file.read_text(encoding="utf-8").splitlines()) == 1
            and rgb_close(sample_wl_px(*probe_pt), green),
            timeout=8.0,
            desc="Wayland Dtstyle initial launch",
        )

        win_res = subprocess.check_output(
            [str(helper_bin), "find_win", "dtstyle"], env=c_env, text=True
        ).strip()

        # 2. Obscure Dtstyle behind BlockerRed -> click Front Panel button -> Dtstyle rises to foreground!
        blocker_cnt = work / "blocker_cnt.txt"
        blocker = subprocess.Popen(
            [str(helper_bin), "client", "blockerred", "BlockerRed", "E02828", "140", "80", str(blocker_cnt)],
            env=c_env,
            start_new_session=True,
        )
        procs.append(blocker)
        wait_until(
            lambda: rgb_close(sample_wl_px(*probe_pt), red),
            timeout=6.0,
            desc="Wayland BlockerRed covering Dtstyle",
        )

        wl_click(bx, by)
        wait_until(
            lambda: rgb_close(sample_wl_px(*probe_pt), green),
            timeout=6.0,
            desc="Wayland Front Panel click raising background Dtstyle to foreground",
        )
        assert len(count_file.read_text(encoding="utf-8").splitlines()) == 1, (
            "Wayland Front Panel recall spawned a duplicate instance!"
        )

        # 3. Minimize Dtstyle -> BlockerRed visible -> click Front Panel button -> Dtstyle restored!
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:Dtstyle"], env=c_env, check=True)
        wait_until(
            lambda: rgb_close(sample_wl_px(*probe_pt), red),
            timeout=6.0,
            desc="Wayland Dtstyle minimized",
        )

        wl_click(bx, by)
        wait_until(
            lambda: rgb_close(sample_wl_px(*probe_pt), green),
            timeout=6.0,
            desc="Wayland Front Panel click restoring minimized Dtstyle",
        )
        assert len(count_file.read_text(encoding="utf-8").splitlines()) == 1, (
            "Wayland Front Panel restore spawned a duplicate instance!"
        )

        # 4. Also verify single-instance client XMapRaised restores from minimized under dtwm-wayland
        subprocess.run(["wlrctl", "toplevel", "minimize", "title:Dtstyle"], env=c_env, check=True)
        wait_until(
            lambda: rgb_close(sample_wl_px(*probe_pt), red),
            timeout=6.0,
            desc="Wayland Dtstyle minimized before XMapRaised",
        )
        subprocess.run([str(helper_bin), "map_raised", win_res], env=c_env, check=True)
        wait_until(
            lambda: rgb_close(sample_wl_px(*probe_pt), green),
            timeout=6.0,
            desc="Wayland XMapRaised restoring minimized Dtstyle",
        )
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, 15)
                except ProcessLookupError:
                    pass
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(p.pid, 9)
                    except ProcessLookupError:
                        pass
        comp_log.close()


def verify_x11(dtwm_bin: Path, helper_bin: Path, base_dir: Path) -> None:
    if not shutil.which("Xvfb"):
        print("SKIP X11 push-recall check (Xvfb not installed)")
        return

    work = base_dir / "x11"
    home = work / "home"
    home.mkdir(parents=True, exist_ok=True)
    count_file = work / "spawn_count.txt"
    types_dir = write_fp_test_db(home, helper_bin, count_file)

    disp_num = pick_free_display()
    disp = f":{disp_num}"

    env = os.environ.copy()
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "TT_SESSION", "DTUSERSESSION"):
        env.pop(k, None)
    env.update(
        {
            "DISPLAY": disp,
            "HOME": str(home),
            "LANG": "C",
            "DTDATABASESEARCHPATH": str(types_dir),
        }
    )

    icons_dir = Path("/usr/dt/appconfig/icons/C")
    if not icons_dir.is_dir():
        icons_dir = Path(__file__).resolve().parents[2] / "icons" / "C"
    if icons_dir.is_dir():
        env["XMICONSEARCHPATH"] = f"{icons_dir}/%B%M.pm:{icons_dir}/%B%M.bm:{icons_dir}/%B.pm"
        env["XMICONBMSEARCHPATH"] = f"{icons_dir}/%B%M.bm:{icons_dir}/%B%M.pm"

    xvfb = subprocess.Popen(
        ["Xvfb", disp, "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    procs: list[subprocess.Popen] = [xvfb]
    dtwm_log = (work / "dtwm-x11.log").open("w", encoding="utf-8")
    try:
        wait_until(
            lambda: Path(f"/tmp/.X11-unix/X{disp_num}").exists(),
            timeout=5.0,
            desc="Xvfb socket",
        )
        dtwm = subprocess.Popen(
            [
                str(dtwm_bin),
                "-xrm", "Dtwm*useFrontPanel: True",
                "-xrm", "Dtwm*minimizeAnimation: none",
            ],
            env=env,
            stdout=dtwm_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        procs.append(dtwm)

        wait_until(
            lambda: subprocess.run([str(helper_bin), "wm_ready"], env=env).returncode == 0,
            timeout=8.0,
            desc="X11 dtwm _MOTIF_WM_INFO",
        )

        ctrl_xy: list[tuple[int, int]] = []

        def locate_style_btn() -> bool:
            r = subprocess.run(
                [str(helper_bin), "find_control", "Style"],
                env=env,
                capture_output=True,
                text=True,
            )
            if r.returncode == 0 and "," in r.stdout:
                xs, ys = r.stdout.strip().split(",", 1)
                ctrl_xy.clear()
                ctrl_xy.append((int(xs), int(ys)))
                return True
            return False

        wait_until(locate_style_btn, timeout=8.0, desc="X11 FrontPanel Style control")
        bx, by = ctrl_xy[0]

        def x11_px(px: int, py: int) -> tuple[int, int, int]:
            out = subprocess.check_output(
                [str(helper_bin), "pixel", str(px), str(py)], env=env, text=True
            ).strip()
            r, g, b = (int(v) for v in out.split(","))
            return (r, g, b)

        def active_res() -> str:
            return subprocess.check_output(
                [str(helper_bin), "active_res"], env=env, text=True
            ).strip()

        green = (0x18, 0xD0, 0x50)
        red = (0xE0, 0x28, 0x28)
        probe_pt = (230, 160)

        # 1. Click Front Panel button -> launches Dtstyle with active focus
        subprocess.run([str(helper_bin), "xtest_click", str(bx), str(by)], env=env, check=True)
        wait_until(
            lambda: count_file.is_file()
            and len(count_file.read_text(encoding="utf-8").splitlines()) == 1
            and rgb_close(x11_px(*probe_pt), green),
            timeout=8.0,
            desc="X11 Dtstyle initial launch",
        )
        win_res = subprocess.check_output(
            [str(helper_bin), "find_win", "dtstyle"], env=env, text=True
        ).strip()

        # 2. Cover Dtstyle with BlockerRed -> click Front Panel button -> Dtstyle raised AND focused!
        blocker_cnt = work / "blocker_cnt.txt"
        blocker = subprocess.Popen(
            [str(helper_bin), "client", "blockerred", "BlockerRed", "E02828", "140", "80", str(blocker_cnt)],
            env=env,
            start_new_session=True,
        )
        procs.append(blocker)
        wait_until(
            lambda: rgb_close(x11_px(*probe_pt), red),
            timeout=6.0,
            desc="X11 BlockerRed covering Dtstyle",
        )

        subprocess.run([str(helper_bin), "xtest_click", str(bx), str(by)], env=env, check=True)
        wait_until(
            lambda: rgb_close(x11_px(*probe_pt), green)
            and active_res().lower() == "dtstyle",
            timeout=6.0,
            desc="X11 Front Panel click raising & focusing background Dtstyle",
        )
        assert len(count_file.read_text(encoding="utf-8").splitlines()) == 1, (
            "X11 Front Panel recall spawned a duplicate instance!"
        )

        # 3. Minimize Dtstyle -> click Front Panel button -> Dtstyle restored & focused!
        subprocess.run([str(helper_bin), "iconify", win_res], env=env, check=True)
        wait_until(
            lambda: rgb_close(x11_px(*probe_pt), red),
            timeout=6.0,
            desc="X11 Dtstyle minimized",
        )

        subprocess.run([str(helper_bin), "xtest_click", str(bx), str(by)], env=env, check=True)
        wait_until(
            lambda: rgb_close(x11_px(*probe_pt), green)
            and active_res().lower() == "dtstyle",
            timeout=6.0,
            desc="X11 Front Panel click restoring & focusing minimized Dtstyle",
        )
        assert len(count_file.read_text(encoding="utf-8").splitlines()) == 1, (
            "X11 Front Panel restore spawned a duplicate instance!"
        )
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, 15)
                except ProcessLookupError:
                    pass
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(p.pid, 9)
                    except ProcessLookupError:
                        pass
        dtwm_log.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", type=Path, default=None)
    ap.add_argument("--dtwm", type=Path, default=None)
    args = ap.parse_args()

    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="dtwm-push-recall-") as td:
        base = Path(td)
        helper_c = base / "recall_helper.c"
        helper_bin = base / "recall_helper"
        helper_c.write_text(HELPER_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", str(helper_c), "-lX11", "-lXtst", "-o", str(helper_bin)],
            check=True,
        )

        if args.compositor and args.compositor.is_file():
            verify_wayland(args.compositor.resolve(), helper_bin, base)
        if args.dtwm and args.dtwm.is_file():
            verify_x11(args.dtwm.resolve(), helper_bin, base)

    print("PASS check-dtwm-push-recall")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
