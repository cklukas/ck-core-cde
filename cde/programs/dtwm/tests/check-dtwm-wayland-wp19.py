#!/usr/bin/env python3
"""WP-19 parallel dual-environment integration check (Wayland + X11/XLibre):
1. Runtime Toolkit & Library Linkage:
   - Verifies all 5 companion binaries (`ck-tasks`, `ck-view`, `ck-pdf`, `dtpad`, `dtcm`)
     map CK Core Motif ABI 9 (`libXm.so.9`).
2. Actionable `ck-tasks` System Monitor:
   - Verifies live network interface enumeration (`net_ifaces >= 1`), `cpu_count >= 1`,
     and Performance chart toggle (`SET_PERF_MODE percore` / `overall`).
   - Selects a spawned sleeper process (`SELECT_PID`), updates priority (`RENICE_SELECTED 6`
     -> `/proc/<pid>/stat` nice == 6), opens the Motif Force Kill confirmation modal
     (`OPEN_KILL_DIALOG` -> `kill_dialog_open == True`), and confirms `SIGKILL` termination
     (`CONFIRM_KILL_DIALOG`).
   - Exercises systemd service Start/Stop/Restart over D-Bus (`CK_TASKS_SYSTEM_BUS=session`
     against mock `org.freedesktop.systemd1.Manager`).
3. Lightweight Native Motif Image Viewer (`ck-view`):
   - Loads PNG, JPEG, WebP, TIFF, XPM, and SVG assets, steps folder `NEXT`/`PREV`,
     performs lossless 90-degree rotation (`ROTATE_CW`), crops (`CROP`), saves (`SAVE`),
     copies PNG to clipboard (`COPY_IMAGE`), sets CDE workspace backdrop (`SET_BACKDROP` ->
     `~/.dt/backdrops/CkViewBackdrop.pm`), and verifies `WM_SAVE_YOURSELF` -> `-session` restore.
4. Lightweight Native Motif PDF Viewer (`ck-pdf`):
   - Loads a 3-page PDF with Document Outline (`page_count == 3`, `outline_count == 3`),
     jumps pages (`GOTO_PAGE 2`), searches text (`FIND UniqueTokenAlpha` -> 2 matches,
     `FIND_NEXT` -> page 3), copies page text to X11 `CLIPBOARD` (`SELECT_PAGE_TEXT`),
     prints via mock CUPS `lp` (`PRINT OfficeLaser`), and verifies `WM_SAVE_YOURSELF` ->
     `-session` page restore.
5. Practical `dtpad` Polish:
   - Verifies tabbed documents (`OPEN_TAB`, `SELECT_TAB`, `tab_count == 2`), `Open Recent`
     MRU (`~/.dt/dtpad.recent`), `Show Line Numbers` gutter (`TOGGLE_LINE_NUMBERS`), and
     crash-recovery autosave (`SET_TEXT` + `FLUSH_AUTOSAVE` -> `SIGKILL` -> relaunch ->
     `recovered == True` -> `SAVE` -> swap file cleaned).
6. Practical `dtcm` RFC 5545 `.ics` Import & Export:
   - Imports a multi-event `.ics` calendar (`SUMMARY` folding, `LOCATION`, `DESCRIPTION`,
     `VALUE=DATE` all-day event, `RRULE:FREQ=WEEKLY;INTERVAL=2;COUNT=4`) and exports back
     to `.ics`, verifying round-trip fidelity.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time


X11_PROP_HELPER_C = r"""
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int win_has_prop(Display *dpy, Window w, Atom state_atom) {
    Atom act = None;
    int fmt = 0;
    unsigned long n = 0, rem = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, state_atom, 0, 4, False, AnyPropertyType,
                           &act, &fmt, &n, &rem, &data) == Success && data) {
        XFree(data);
        return act != None && n > 0;
    }
    return 0;
}

static Window find_by_state_atom(Display *dpy, Window w, Atom state_atom) {
    if (win_has_prop(dpy, w, state_atom)) return w;
    Window root = None, parent = None, *kids = NULL;
    unsigned int nkids = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &kids, &nkids)) return None;
    Window found = None;
    for (unsigned int i = 0; i < nkids && !found; ++i) {
        found = find_by_state_atom(dpy, kids[i], state_atom);
    }
    if (kids) XFree(kids);
    return found;
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 2;
    if (argc < 2 || strcmp(argv[1], "ping") == 0) {
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "get") == 0) {
        Atom prop = XInternAtom(dpy, argv[2], False);
        Window w = find_by_state_atom(dpy, DefaultRootWindow(dpy), prop);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom act = None;
        int fmt = 0;
        unsigned long n = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, w, prop, 0, 65536, False, AnyPropertyType,
                               &act, &fmt, &n, &rem, &data) == Success && data) {
            fwrite(data, 1, n, stdout);
            XFree(data);
            XCloseDisplay(dpy);
            return 0;
        }
        XCloseDisplay(dpy);
        return 4;
    }
    if (argc >= 5 && strcmp(argv[1], "set") == 0) {
        Atom state_atom = XInternAtom(dpy, argv[2], False);
        Window w = find_by_state_atom(dpy, DefaultRootWindow(dpy), state_atom);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom prop = XInternAtom(dpy, argv[3], False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        XChangeProperty(dpy, w, prop, utf8, 8, PropModeReplace,
                        (const unsigned char *)argv[4], (int)strlen(argv[4]));
        XFlush(dpy);
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "wm_save_yourself") == 0) {
        Window root = DefaultRootWindow(dpy);
        Atom a_save_mode = XInternAtom(dpy, "_DT_SAVE_MODE", False);
        Atom a_rest_mode = XInternAtom(dpy, "_DT_RESTORE_MODE", False);
        XChangeProperty(dpy, root, a_save_mode, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)"current", 7);
        XChangeProperty(dpy, root, a_rest_mode, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)"current", 7);
        XFlush(dpy);

        Atom state_atom = XInternAtom(dpy, argv[2], False);
        Window w = find_by_state_atom(dpy, DefaultRootWindow(dpy), state_atom);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom wm_proto = XInternAtom(dpy, "WM_PROTOCOLS", False);
        Atom wm_save = XInternAtom(dpy, "WM_SAVE_YOURSELF", False);
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = w;
        ev.xclient.message_type = wm_proto;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)wm_save;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, w, False, NoEventMask, &ev);
        XFlush(dpy);
        for (int attempt = 0; attempt < 40; ++attempt) {
            usleep(50000);
            char **cmd_argv = NULL;
            int cmd_argc = 0;
            if (XGetCommand(dpy, w, &cmd_argv, &cmd_argc) && cmd_argc >= 3 && cmd_argv) {
                for (int i = 0; i + 1 < cmd_argc; ++i) {
                    if (strcmp(cmd_argv[i], "-session") == 0) {
                        printf("%s", cmd_argv[i + 1]);
                        XFreeStringList(cmd_argv);
                        XCloseDisplay(dpy);
                        return 0;
                    }
                }
                XFreeStringList(cmd_argv);
            }
        }
        XCloseDisplay(dpy);
        return 5;
    }
    if (argc >= 2 && strcmp(argv[1], "get_clipboard_text") == 0) {
        Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 10, 10, 0, 0, 0);
        Atom clip = XInternAtom(dpy, "CLIPBOARD", False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        Atom dest = XInternAtom(dpy, "_WP19_CLIP_DEST", False);
        XConvertSelection(dpy, clip, utf8, dest, win, CurrentTime);
        XFlush(dpy);
        for (int i = 0; i < 40; ++i) {
            while (XPending(dpy)) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == SelectionNotify) {
                    if (ev.xselection.property == None) {
                        XConvertSelection(dpy, clip, XA_STRING, dest, win, CurrentTime);
                        XFlush(dpy);
                        continue;
                    }
                    Atom act = None;
                    int fmt = 0;
                    unsigned long n = 0, rem = 0;
                    unsigned char *data = NULL;
                    if (XGetWindowProperty(dpy, win, dest, 0, 65536, True, AnyPropertyType,
                                           &act, &fmt, &n, &rem, &data) == Success && data) {
                        fwrite(data, 1, n, stdout);
                        XFree(data);
                        XDestroyWindow(dpy, win);
                        XCloseDisplay(dpy);
                        return 0;
                    }
                }
            }
            usleep(30000);
        }
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 6;
    }
    XCloseDisplay(dpy);
    return 1;
}
"""


ASSET_GEN_C = r"""
#include <assert.h>
#include <cairo/cairo.h>
#include <cairo/cairo-pdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ck-view/ck-view.h"

static void make_img(CkViewImage *img, int w, int h) {
    ck_view_image_init(img);
    img->width = w;
    img->height = h;
    img->rgba = (unsigned char *)calloc((size_t)w * (size_t)h * 4U, 1);
    assert(img->rgba != NULL);
    snprintf(img->format_name, sizeof(img->format_name), "RAW");
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char *px = img->rgba + ((size_t)y * (size_t)w + (size_t)x) * 4U;
            px[0] = (unsigned char)((x * 255) / (w > 1 ? w - 1 : 1));
            px[1] = (unsigned char)((y * 200) / (h > 1 ? h - 1 : 1));
            px[2] = 140;
            px[3] = 255;
        }
    }
}

int main(int argc, char **argv) {
    assert(argc >= 3);
    const char *img_dir = argv[1];
    const char *pdf_path = argv[2];

    CkViewImage base;
    make_img(&base, 80, 50);

    char p[1024], err[256];
    snprintf(p, sizeof(p), "%s/01_sample.png", img_dir);
    assert(ck_view_image_save(&base, p, NULL, err, sizeof(err)) == 0);
    snprintf(p, sizeof(p), "%s/02_sample.jpg", img_dir);
    assert(ck_view_image_save(&base, p, NULL, err, sizeof(err)) == 0);
    snprintf(p, sizeof(p), "%s/03_sample.webp", img_dir);
    assert(ck_view_image_save(&base, p, NULL, err, sizeof(err)) == 0);
    snprintf(p, sizeof(p), "%s/04_sample.tiff", img_dir);
    assert(ck_view_image_save(&base, p, NULL, err, sizeof(err)) == 0);
    snprintf(p, sizeof(p), "%s/05_sample.xpm", img_dir);
    assert(ck_view_image_write_xpm(&base, p, 0, err, sizeof(err)) == 0);
    ck_view_image_free(&base);

    snprintf(p, sizeof(p), "%s/06_vector.svg", img_dir);
    FILE *sfp = fopen(p, "w");
    assert(sfp != NULL);
    fprintf(sfp,
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"40\">"
            "<rect width=\"64\" height=\"40\" fill=\"#2a6f97\"/>"
            "<circle cx=\"32\" cy=\"20\" r=\"14\" fill=\"#e9c46a\"/>"
            "</svg>\n");
    fclose(sfp);

    cairo_surface_t *surf = cairo_pdf_surface_create(pdf_path, 595.0, 842.0);
    assert(cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS);
    cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_TITLE, "WP19 Companion Manual");
    int r1 = cairo_pdf_surface_add_outline(surf, CAIRO_PDF_OUTLINE_ROOT,
                                           "Chapter 1: System Monitor", "page=1 pos=[0 800]",
                                           CAIRO_PDF_OUTLINE_FLAG_OPEN);
    cairo_pdf_surface_add_outline(surf, r1, "Section 1.1: Image Viewer", "page=2 pos=[0 800]", 0);
    cairo_pdf_surface_add_outline(surf, CAIRO_PDF_OUTLINE_ROOT,
                                  "Chapter 2: Calendar & Editor", "page=3 pos=[0 800]", 0);

    cairo_t *cr = cairo_create(surf);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    cairo_set_font_size(cr, 20.0);
    cairo_move_to(cr, 60.0, 90.0);
    cairo_show_text(cr, "Chapter 1: System Monitor");
    cairo_set_font_size(cr, 13.0);
    cairo_move_to(cr, 60.0, 140.0);
    cairo_show_text(cr, "UniqueTokenAlpha on page one of the manual.");
    cairo_show_page(cr);

    cairo_set_font_size(cr, 18.0);
    cairo_move_to(cr, 60.0, 90.0);
    cairo_show_text(cr, "Section 1.1: Image Viewer");
    cairo_set_font_size(cr, 13.0);
    cairo_move_to(cr, 60.0, 140.0);
    cairo_show_text(cr, "SectionTwoMarker describes lossless rotation and crop.");
    cairo_show_page(cr);

    cairo_set_font_size(cr, 18.0);
    cairo_move_to(cr, 60.0, 90.0);
    cairo_show_text(cr, "Chapter 2: Calendar & Editor");
    cairo_set_font_size(cr, 13.0);
    cairo_move_to(cr, 60.0, 140.0);
    cairo_show_text(cr, "UniqueTokenAlpha returns here on page three.");
    cairo_show_page(cr);

    cairo_destroy(cr);
    cairo_surface_finish(surf);
    cairo_surface_destroy(surf);
    return 0;
}
"""


MOCK_SYSTEMD_PY = r"""#!/usr/bin/env python3
import json
import os
import sys
import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

STATE_FILE = sys.argv[1]
READY_FILE = sys.argv[2]

svc_state = {
    "active": "active",
    "sub": "running",
    "calls": [],
}

def flush_state():
    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(svc_state, f)
    os.replace(tmp, STATE_FILE)

class SystemdManager(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop/systemd1")

    @dbus.service.method("org.freedesktop.systemd1.Manager", in_signature="", out_signature="a(ssssssouso)")
    def ListUnits(self):
        return [
            (
                "ck-demo.service",
                "CK Demo Companion Service",
                "loaded",
                svc_state["active"],
                svc_state["sub"],
                "",
                dbus.ObjectPath("/org/freedesktop/systemd1/unit/ck_2ddemo_2eservice"),
                dbus.UInt32(0),
                "",
                dbus.ObjectPath("/"),
            )
        ]

    @dbus.service.method("org.freedesktop.systemd1.Manager", in_signature="ss", out_signature="o")
    def StartUnit(self, unit, mode):
        svc_state["calls"].append(f"start:{unit}:{mode}")
        svc_state["active"] = "active"
        svc_state["sub"] = "running"
        flush_state()
        return dbus.ObjectPath("/org/freedesktop/systemd1/job/1")

    @dbus.service.method("org.freedesktop.systemd1.Manager", in_signature="ss", out_signature="o")
    def StopUnit(self, unit, mode):
        svc_state["calls"].append(f"stop:{unit}:{mode}")
        svc_state["active"] = "inactive"
        svc_state["sub"] = "dead"
        flush_state()
        return dbus.ObjectPath("/org/freedesktop/systemd1/job/2")

    @dbus.service.method("org.freedesktop.systemd1.Manager", in_signature="ss", out_signature="o")
    def RestartUnit(self, unit, mode):
        svc_state["calls"].append(f"restart:{unit}:{mode}")
        svc_state["active"] = "active"
        svc_state["sub"] = "running"
        flush_state()
        return dbus.ObjectPath("/org/freedesktop/systemd1/job/3")

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
bus_name = dbus.service.BusName("org.freedesktop.systemd1", bus)
mgr = SystemdManager(bus)
flush_state()
with open(READY_FILE, "w", encoding="utf-8") as rf:
    rf.write("ready\n")
GLib.MainLoop().run()
"""


def wait_for(pred, timeout: float = 10.0, interval: float = 0.08, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_err = None
    while time.monotonic() < deadline:
        try:
            val = pred()
            if val:
                return val
        except Exception as exc:  # noqa: BLE001
            last_err = exc
        time.sleep(interval)
    raise RuntimeError(f"Timed out waiting for {desc}" + (f" (last_err={last_err})" if last_err else ""))


def verify_proc_libs(pid: int, libs: tuple[str, ...], label: str) -> None:
    maps = Path(f"/proc/{pid}/maps").read_text(encoding="utf-8", errors="replace")
    for lib in libs:
        if lib not in maps:
            raise AssertionError(f"{label} (pid={pid}) does not map {lib}!\n{maps}")


def read_proc_nice(pid: int) -> int:
    stat_txt = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    after_comm = stat_txt[stat_txt.rfind(")") + 2:].split()
    # Field 19 of /proc/pid/stat is nice -> index 16 after ')'
    return int(after_comm[16])


def kill_procs(procs: list[subprocess.Popen]) -> None:
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
    time.sleep(0.15)
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                p.wait(timeout=2.0)
            except Exception:  # noqa: BLE001
                pass


def run_lane(
    lane: str,
    disp_idx: int,
    work_root: Path,
    comp_bin: Path,
    dtwm_x11_bin: Path,
    tasks_bin: Path,
    view_bin: Path,
    pdf_bin: Path,
    dtpad_bin: Path,
    dtcm_bin: Path,
    x11_helper: Path,
    asset_gen_bin: Path,
    mock_systemd_py: Path,
) -> str:
    lane_root = work_root / lane
    home = lane_root / "home"
    xdg_rt = lane_root / "rt"
    img_dir = lane_root / "images"
    for d in (home, xdg_rt, img_dir, home / ".config", home / ".cache", home / ".local/share",
              home / ".dt/sessions", home / ".dt/backdrops"):
        d.mkdir(parents=True, exist_ok=True)
    os.chmod(xdg_rt, 0o700)
    (home / ".Xdefaults").write_text("*fontList: fixed\n", encoding="utf-8")

    cde_build_root = comp_bin.parent.parent.parent
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = ":".join([
        "/var/tmp/ck-wayland-build/wp00/fs-prefix/lib",
        "/var/tmp/ck-wayland-build/wp00/motif-prefix/lib",
        str(cde_build_root / "lib/DtSvc/.libs"),
        str(cde_build_root / "lib/DtWidget/.libs"),
        str(cde_build_root / "lib/DtSearch/.libs"),
        str(cde_build_root / "lib/DtHelp/.libs"),
        str(cde_build_root / "lib/DtPrint/.libs"),
        str(cde_build_root / "lib/DtTerm/.libs"),
        str(cde_build_root / "lib/csa/.libs"),
        str(cde_build_root / "lib/tt/lib/.libs"),
        str(cde_build_root / "lib/DtXinerama/.libs"),
        "/usr/dt/lib",
    ])
    lp_log = lane_root / "lp.log"
    mock_lp = lane_root / "mock-lp.sh"
    mock_lpstat = lane_root / "mock-lpstat.sh"
    mock_lp.write_text(f"#!/bin/sh\necho \"$@\" >> {shlex.quote(str(lp_log))}\nexit 0\n", encoding="utf-8")
    mock_lpstat.write_text("#!/bin/sh\necho 'printer OfficeLaser is idle. enabled'\n", encoding="utf-8")
    mock_lp.chmod(0o755)
    mock_lpstat.chmod(0o755)

    env.update({
        "HOME": str(home),
        "XDG_RUNTIME_DIR": str(xdg_rt),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "CK_TASKS_SYSTEM_BUS": "session",
        "CK_PDF_LP_BIN": str(mock_lp),
        "CK_PDF_LPSTAT_BIN": str(mock_lpstat),
        "DTCM_SUPPRESS_RPC_POPUP": "1",
        "LC_ALL": "C.UTF-8",
        "LANG": "C.UTF-8",
    })

    pdf_path = lane_root / "manual.pdf"
    subprocess.run([str(asset_gen_bin), str(img_dir), str(pdf_path)], env=env, check=True)

    # Populate a test 24x24 toolbar icon directory to exercise ck_toolbar_apply_icon + dtpad icon loader
    tb_icon_dir = lane_root / "toolbar_icons" / "24x24"
    tb_icon_dir.mkdir(parents=True, exist_ok=True)
    for icon_nm in ("document-open", "document-save", "document-print", "sidebar-outline", "tab-new"):
        shutil.copyfile(img_dir / "01_sample.png", tb_icon_dir / f"{icon_nm}.png")
    env["CK_TOOLBAR_ICON_DIR"] = str(lane_root / "toolbar_icons")

    procs: list[subprocess.Popen] = []
    try:
        # 1. Isolated session D-Bus + mock org.freedesktop.systemd1
        dbus_addr_file = lane_root / "dbus.addr"
        with dbus_addr_file.open("w", encoding="utf-8") as dfp:
            dbus_proc = subprocess.Popen(
                ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                env=env, stdout=dfp, stderr=subprocess.DEVNULL, start_new_session=True,
            )
        procs.append(dbus_proc)
        wait_for(lambda: dbus_addr_file.is_file() and dbus_addr_file.read_text(encoding="utf-8").strip(),
                 timeout=6.0, desc=f"{lane} dbus-daemon")
        env["DBUS_SESSION_BUS_ADDRESS"] = dbus_addr_file.read_text(encoding="utf-8").splitlines()[0].strip()

        sd_state_file = lane_root / "systemd_state.json"
        sd_ready_file = lane_root / "systemd.ready"
        sd_proc = subprocess.Popen(
            ["python3", str(mock_systemd_py), str(sd_state_file), str(sd_ready_file)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, start_new_session=True,
        )
        procs.append(sd_proc)
        wait_for(lambda: sd_ready_file.is_file(), timeout=6.0, desc=f"{lane} mock systemd1")

        # 2. Start Wayland (`dtwm-wayland`) or X11 (`Xvfb` + `dtwm`)
        if lane == "wayland":
            status_file = lane_root / "comp.status"
            wenv = dict(env, WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1",
                        WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1")
            wenv.pop("WAYLAND_DISPLAY", None)
            wenv.pop("DISPLAY", None)
            comp_proc = subprocess.Popen(
                [str(comp_bin), "--status-file", str(status_file)],
                env=wenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(comp_proc)

            def wl_up():
                if not status_file.is_file():
                    return None
                kv = dict(ln.split("=", 1) for ln in status_file.read_text(encoding="utf-8").splitlines() if "=" in ln)
                return kv if kv.get("WAYLAND_DISPLAY") and kv.get("DISPLAY") else None

            kv = wait_for(wl_up, timeout=10.0, desc=f"{lane} dtwm-wayland")
            env["WAYLAND_DISPLAY"] = kv["WAYLAND_DISPLAY"]
            env["DISPLAY"] = kv["DISPLAY"]
        else:
            xdisp = f":{disp_idx}"
            xvfb_proc = subprocess.Popen(
                ["Xvfb", xdisp, "-screen", "0", "1280x900x24", "-nolisten", "tcp"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(xvfb_proc)
            env["DISPLAY"] = xdisp
            wait_for(lambda: subprocess.run([str(x11_helper), "ping"], env=env, capture_output=True).returncode == 0,
                     timeout=8.0, desc=f"{lane} Xvfb")
            dtwm_proc = subprocess.Popen(
                [str(dtwm_x11_bin)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
            )
            procs.append(dtwm_proc)
            time.sleep(0.5)

        def get_json(state_prop: str) -> dict:
            r = subprocess.run([str(x11_helper), "get", state_prop], env=env, capture_output=True, text=True)
            if r.returncode != 0 or not r.stdout.strip():
                return {}
            return json.loads(r.stdout)

        def send_ipc(state_prop: str, ipc_prop: str, cmd: str) -> None:
            subprocess.run([str(x11_helper), "set", state_prop, ipc_prop, cmd], env=env, check=True)

        # ---- PART A: Actionable `ck-tasks` ----
        sleeper = subprocess.Popen(["sleep", "300"], env=env, start_new_session=True)
        procs.append(sleeper)

        tasks_proc = subprocess.Popen(
            [str(tasks_bin)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
        )
        procs.append(tasks_proc)

        ts = wait_for(
            lambda: s if (s := get_json("_CK_TASKS_STATE")).get("cpu_count", 0) >= 1 else None,
            timeout=10.0, desc=f"{lane} ck-tasks _CK_TASKS_STATE",
        )
        verify_proc_libs(tasks_proc.pid, ("libXm.so.9",), f"{lane} ck-tasks")
        assert ts["net_ifaces"] >= 1, f"{lane}: expected >= 1 network interface, got {ts}"

        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", "SET_PERF_MODE percore")
        wait_for(lambda: get_json("_CK_TASKS_STATE").get("perf_per_core") is True,
                 timeout=5.0, desc=f"{lane} ck-tasks per-core chart mode")

        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", f"SELECT_PID {sleeper.pid}")
        wait_for(lambda: get_json("_CK_TASKS_STATE").get("selected_pid") == sleeper.pid,
                 timeout=6.0, desc=f"{lane} ck-tasks SELECT_PID {sleeper.pid}")

        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", "RENICE_SELECTED 6")
        wait_for(lambda: get_json("_CK_TASKS_STATE").get("selected_nice") == 6 and read_proc_nice(sleeper.pid) == 6,
                 timeout=6.0, desc=f"{lane} ck-tasks RENICE_SELECTED 6")

        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", "OPEN_KILL_DIALOG")
        wait_for(lambda: get_json("_CK_TASKS_STATE").get("kill_dialog_open") is True,
                 timeout=5.0, desc=f"{lane} ck-tasks Force Kill confirmation modal")

        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", "CONFIRM_KILL_DIALOG")
        wait_for(lambda: sleeper.poll() is not None, timeout=6.0, desc=f"{lane} sleeper SIGKILL termination")

        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", "SHOW_DISABLED_SERVICES 1")
        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", "SELECT_SERVICE ck-demo")
        wait_for(lambda: get_json("_CK_TASKS_STATE").get("selected_service") == "ck-demo",
                 timeout=6.0, desc=f"{lane} ck-tasks SELECT_SERVICE ck-demo")

        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", "SERVICE_ACTION stop")
        wait_for(lambda: get_json("_CK_TASKS_STATE").get("selected_service_state") == "stopped",
                 timeout=6.0, desc=f"{lane} ck-tasks stop service")

        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", "SERVICE_ACTION start")
        wait_for(lambda: get_json("_CK_TASKS_STATE").get("selected_service_state") == "running",
                 timeout=6.0, desc=f"{lane} ck-tasks start service")

        send_ipc("_CK_TASKS_STATE", "_CK_TASKS_IPC", "SERVICE_ACTION restart")
        wait_for(lambda: any(c.startswith("restart:ck-demo.service")
                             for c in json.loads(sd_state_file.read_text(encoding="utf-8")).get("calls", [])),
                 timeout=6.0, desc=f"{lane} ck-tasks restart service")
        tasks_proc.terminate()
        tasks_proc.wait(timeout=4.0)

        # ---- PART B: Image Viewer (`ck-view`) ----
        png1 = img_dir / "01_sample.png"
        view_proc = subprocess.Popen(
            [str(view_bin), str(png1)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
        )
        procs.append(view_proc)

        vs = wait_for(
            lambda: s if (s := get_json("_CK_VIEW_STATE")).get("loaded") is True else None,
            timeout=8.0, desc=f"{lane} ck-view initial load",
        )
        verify_proc_libs(view_proc.pid, ("libXm.so.9", "librsvg-2.so", "libwebp.so"), f"{lane} ck-view")
        assert (vs["width"], vs["height"], vs["folder_count"]) == (80, 50, 6), f"{lane}: unexpected view state {vs}"

        # Step through JPEG, WebP, TIFF, XPM, SVG
        expected_dims = [(80, 50), (80, 50), (80, 50), (80, 50), (64, 40)]
        for step_idx, (ew, eh) in enumerate(expected_dims, start=1):
            send_ipc("_CK_VIEW_STATE", "_CK_VIEW_IPC", "NEXT")
            wait_for(
                lambda idx=step_idx, w=ew, h=eh: (
                    (s := get_json("_CK_VIEW_STATE")).get("folder_index") == idx
                    and s.get("width") == w and s.get("height") == h
                ),
                timeout=6.0, desc=f"{lane} ck-view NEXT #{step_idx}",
            )

        # Return to 01_sample.png, rotate 90 CW (80x50 -> 50x80), crop (30x40), save
        send_ipc("_CK_VIEW_STATE", "_CK_VIEW_IPC", f"OPEN {png1}")
        wait_for(lambda: get_json("_CK_VIEW_STATE").get("width") == 80, timeout=5.0, desc=f"{lane} ck-view reopen png1")

        send_ipc("_CK_VIEW_STATE", "_CK_VIEW_IPC", "ROTATE_CW")
        wait_for(lambda: (s := get_json("_CK_VIEW_STATE")).get("width") == 50 and s.get("height") == 80,
                 timeout=5.0, desc=f"{lane} ck-view ROTATE_CW")

        send_ipc("_CK_VIEW_STATE", "_CK_VIEW_IPC", "CROP 5 5 30 40")
        wait_for(lambda: (s := get_json("_CK_VIEW_STATE")).get("width") == 30 and s.get("height") == 40,
                 timeout=5.0, desc=f"{lane} ck-view CROP")

        cropped_out = lane_root / "cropped_out.png"
        send_ipc("_CK_VIEW_STATE", "_CK_VIEW_IPC", f"SAVE {cropped_out}")
        wait_for(lambda: cropped_out.is_file() and get_json("_CK_VIEW_STATE").get("modified") is False,
                 timeout=5.0, desc=f"{lane} ck-view SAVE")

        send_ipc("_CK_VIEW_STATE", "_CK_VIEW_IPC", "SET_BACKDROP")
        bdrop_pm = home / ".dt/backdrops/CkViewBackdrop.pm"
        wait_for(lambda: bdrop_pm.is_file() and "XPM" in bdrop_pm.read_text(encoding="utf-8", errors="ignore"),
                 timeout=6.0, desc=f"{lane} ck-view SET_BACKDROP")

        # Session save & restore
        sess_res = subprocess.run([str(x11_helper), "wm_save_yourself", "_CK_VIEW_STATE"],
                                  env=env, capture_output=True, text=True, check=True)
        view_sess_id = sess_res.stdout.strip()
        assert view_sess_id, f"{lane}: empty ck-view session id"
        view_proc.terminate()
        view_proc.wait(timeout=4.0)

        view_proc2 = subprocess.Popen(
            [str(view_bin), "-session", view_sess_id],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
        )
        procs.append(view_proc2)
        wait_for(
            lambda: (s := get_json("_CK_VIEW_STATE")).get("loaded") is True
            and Path(s.get("path", "")).name == "cropped_out.png"
            and s.get("width") == 30 and s.get("height") == 40,
            timeout=8.0, desc=f"{lane} ck-view -session restore",
        )
        view_proc2.terminate()
        view_proc2.wait(timeout=4.0)

        # ---- PART C: PDF Viewer (`ck-pdf`) ----
        pdf_proc = subprocess.Popen(
            [str(pdf_bin), str(pdf_path)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
        )
        procs.append(pdf_proc)

        ps = wait_for(
            lambda: s if (s := get_json("_CK_PDF_STATE")).get("loaded") is True else None,
            timeout=8.0, desc=f"{lane} ck-pdf initial load",
        )
        verify_proc_libs(pdf_proc.pid, ("libXm.so.9", "libpoppler-glib.so"), f"{lane} ck-pdf")
        assert (ps["page_count"], ps["outline_count"], ps["page"]) == (3, 3, 1), f"{lane}: unexpected pdf state {ps}"

        send_ipc("_CK_PDF_STATE", "_CK_PDF_IPC", "GOTO_PAGE 2")
        wait_for(lambda: get_json("_CK_PDF_STATE").get("page") == 2, timeout=5.0, desc=f"{lane} ck-pdf GOTO_PAGE 2")

        send_ipc("_CK_PDF_STATE", "_CK_PDF_IPC", "FIND UniqueTokenAlpha")
        wait_for(
            lambda: (s := get_json("_CK_PDF_STATE")).get("match_count") == 2 and s.get("page") == 3,
            timeout=6.0, desc=f"{lane} ck-pdf FIND UniqueTokenAlpha -> page 3",
        )
        send_ipc("_CK_PDF_STATE", "_CK_PDF_IPC", "FIND_NEXT")
        wait_for(lambda: get_json("_CK_PDF_STATE").get("page") == 1, timeout=5.0, desc=f"{lane} ck-pdf FIND_NEXT wrap -> page 1")
        send_ipc("_CK_PDF_STATE", "_CK_PDF_IPC", "FIND_NEXT")
        wait_for(lambda: get_json("_CK_PDF_STATE").get("page") == 3, timeout=5.0, desc=f"{lane} ck-pdf FIND_NEXT -> page 3")

        send_ipc("_CK_PDF_STATE", "_CK_PDF_IPC", "SELECT_PAGE_TEXT")
        wait_for(
            lambda: "UniqueTokenAlpha" in get_json("_CK_PDF_STATE").get("selected_text", ""),
            timeout=5.0, desc=f"{lane} ck-pdf SELECT_PAGE_TEXT",
        )
        clip_r = subprocess.run([str(x11_helper), "get_clipboard_text"], env=env, capture_output=True, text=True, check=True)
        assert "UniqueTokenAlpha" in clip_r.stdout, f"{lane}: clipboard missing PDF text, got {clip_r.stdout!r}"

        send_ipc("_CK_PDF_STATE", "_CK_PDF_IPC", "PRINT OfficeLaser")
        wait_for(lambda: lp_log.is_file() and "OfficeLaser" in lp_log.read_text(encoding="utf-8"),
                 timeout=5.0, desc=f"{lane} ck-pdf PRINT OfficeLaser")

        send_ipc("_CK_PDF_STATE", "_CK_PDF_IPC", "SET_OUTLINE_WIDTH 310")
        wait_for(lambda: get_json("_CK_PDF_STATE").get("outline_width", 0) >= 300,
                 timeout=5.0, desc=f"{lane} ck-pdf SET_OUTLINE_WIDTH 310")

        pdf_sess_res = subprocess.run([str(x11_helper), "wm_save_yourself", "_CK_PDF_STATE"],
                                      env=env, capture_output=True, text=True, check=True)
        pdf_sess_id = pdf_sess_res.stdout.strip()
        assert pdf_sess_id, f"{lane}: empty ck-pdf session id"
        pdf_proc.terminate()
        pdf_proc.wait(timeout=4.0)

        pdf_proc2 = subprocess.Popen(
            [str(pdf_bin), "-session", pdf_sess_id],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
        )
        procs.append(pdf_proc2)
        wait_for(
            lambda: (s := get_json("_CK_PDF_STATE")).get("loaded") is True
            and s.get("page") == 3
            and s.get("outline_width", 0) >= 300,
            timeout=8.0, desc=f"{lane} ck-pdf -session restore page 3 + outline_width",
        )
        pdf_proc2.terminate()
        pdf_proc2.wait(timeout=4.0)

        # ---- PART D: `dtpad` Tabs, Open Recent, Line Numbers & Crash-Recovery Autosave ----
        doc1 = lane_root / "notes_alpha.txt"
        doc2 = lane_root / "notes_beta.txt"
        doc1.write_text("Alpha initial content\nSecond line\n", encoding="utf-8")
        doc2.write_text("Beta clean baseline\n", encoding="utf-8")

        pad_proc = subprocess.Popen(
            [str(dtpad_bin), "-standAlone", str(doc1)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(pad_proc)

        ds = wait_for(
            lambda: s if (s := get_json("_DTPAD_STATE")).get("tab_count") == 1 else None,
            timeout=10.0, desc=f"{lane} dtpad initial state",
        )
        verify_proc_libs(pad_proc.pid, ("libXm.so.9",), f"{lane} dtpad")
        assert ds["recent_count"] >= 1, f"{lane}: expected recent_count >= 1, got {ds}"

        send_ipc("_DTPAD_STATE", "_DTPAD_IPC", f"OPEN_TAB {doc2}")
        wait_for(
            lambda: (s := get_json("_DTPAD_STATE")).get("tab_count") == 2 and s.get("active_tab") == 1,
            timeout=6.0, desc=f"{lane} dtpad OPEN_TAB doc2",
        )

        send_ipc("_DTPAD_STATE", "_DTPAD_IPC", "TOGGLE_LINE_NUMBERS")
        wait_for(lambda: get_json("_DTPAD_STATE").get("line_numbers") is True,
                 timeout=5.0, desc=f"{lane} dtpad TOGGLE_LINE_NUMBERS")

        # Open a C source file in a 3rd tab and verify syntax highlighting auto-detection + toggle
        c_doc = lane_root / "sample_code.c"
        c_doc.write_text("#include <stdio.h>\nint main(void) {\n    return 0;\n}\n", encoding="utf-8")
        send_ipc("_DTPAD_STATE", "_DTPAD_IPC", f"OPEN_TAB {c_doc}")
        wait_for(
            lambda: (s := get_json("_DTPAD_STATE")).get("tab_count") == 3
            and s.get("syntax_highlighting") is True
            and s.get("syntax_lang") == "c",
            timeout=6.0, desc=f"{lane} dtpad syntax highlighting auto-detected C",
        )
        send_ipc("_DTPAD_STATE", "_DTPAD_IPC", "SET_SYNTAX_HIGHLIGHT 0")
        wait_for(lambda: get_json("_DTPAD_STATE").get("syntax_highlighting") is False,
                 timeout=5.0, desc=f"{lane} dtpad disable syntax highlighting")
        send_ipc("_DTPAD_STATE", "_DTPAD_IPC", "SET_SYNTAX_HIGHLIGHT 1")
        wait_for(
            lambda: (s := get_json("_DTPAD_STATE")).get("syntax_highlighting") is True
            and s.get("syntax_lang") == "c",
            timeout=5.0, desc=f"{lane} dtpad re-enable syntax highlighting",
        )
        send_ipc("_DTPAD_STATE", "_DTPAD_IPC", "SELECT_TAB 1")
        wait_for(lambda: get_json("_DTPAD_STATE").get("active_tab") == 1,
                 timeout=5.0, desc=f"{lane} dtpad switch back to doc2 tab")

        send_ipc("_DTPAD_STATE", "_DTPAD_IPC", "SET_TEXT CrashRecoveryPayload_WP19")
        wait_for(lambda: "CrashRecoveryPayload_WP19" in get_json("_DTPAD_STATE").get("preview", ""),
                 timeout=5.0, desc=f"{lane} dtpad SET_TEXT")
        send_ipc("_DTPAD_STATE", "_DTPAD_IPC", "FLUSH_AUTOSAVE")
        autosave_dir = home / ".dt/dtpad-autosave"
        wait_for(lambda: autosave_dir.is_dir() and len(list(autosave_dir.glob("*.autosave"))) >= 1,
                 timeout=5.0, desc=f"{lane} dtpad autosave file written")

        # Simulate hard crash with SIGKILL
        os.kill(pad_proc.pid, signal.SIGKILL)
        pad_proc.wait(timeout=4.0)

        pad_proc2 = subprocess.Popen(
            [str(dtpad_bin), "-standAlone", str(doc2)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(pad_proc2)
        wait_for(
            lambda: (s := get_json("_DTPAD_STATE")).get("recovered") is True
            and "CrashRecoveryPayload_WP19" in s.get("preview", ""),
            timeout=10.0, desc=f"{lane} dtpad crash-recovery restore",
        )
        send_ipc("_DTPAD_STATE", "_DTPAD_IPC", "SAVE")
        wait_for(
            lambda: "CrashRecoveryPayload_WP19" in doc2.read_text(encoding="utf-8")
            and len(list(autosave_dir.glob("*.autosave"))) == 0,
            timeout=6.0, desc=f"{lane} dtpad SAVE cleans autosave swap",
        )
        pad_proc2.terminate()
        pad_proc2.wait(timeout=4.0)

        # ---- PART E: `dtcm` RFC 5545 `.ics` Import & Export ----
        in_ics = lane_root / "import_sched.ics"
        out_ics = lane_root / "export_sched.ics"
        in_ics.write_text(
            "BEGIN:VCALENDAR\r\n"
            "VERSION:2.0\r\n"
            "PRODID:-//CK-Core//WP19//EN\r\n"
            "BEGIN:VEVENT\r\n"
            "UID:wp19-ev1@ckcore.test\r\n"
            "SUMMARY:Architecture Sync\\, Wayland\\\r\n"
            "  Desktop\r\n"
            "LOCATION:Lab B\r\n"
            "DESCRIPTION:Review ck-view and ck-pdf\r\n"
            "DTSTART:20261122T100000Z\r\n"
            "DTEND:20261122T110000Z\r\n"
            "RRULE:FREQ=WEEKLY;INTERVAL=2;COUNT=4\r\n"
            "END:VEVENT\r\n"
            "BEGIN:VEVENT\r\n"
            "UID:wp19-ev2@ckcore.test\r\n"
            "SUMMARY:Company Holiday\r\n"
            "DTSTART;VALUE=DATE:20261224\r\n"
            "END:VEVENT\r\n"
            "END:VCALENDAR\r\n",
            encoding="utf-8",
        )

        dtcm_proc = subprocess.Popen(
            [str(dtcm_bin)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
        )
        procs.append(dtcm_proc)
        wait_for(lambda: get_json("_DTCM_STATE").get("loaded") is True,
                 timeout=10.0, desc=f"{lane} dtcm _DTCM_STATE")
        verify_proc_libs(dtcm_proc.pid, ("libXm.so.9",), f"{lane} dtcm")

        send_ipc("_DTCM_STATE", "_DTCM_IPC", f"IMPORT_ICS {in_ics}")
        wait_for(lambda: get_json("_DTCM_STATE").get("last_imported") == 2,
                 timeout=8.0, desc=f"{lane} dtcm IMPORT_ICS")

        send_ipc("_DTCM_STATE", "_DTCM_IPC", f"EXPORT_ICS {out_ics}")
        wait_for(lambda: out_ics.is_file() and get_json("_DTCM_STATE").get("last_exported", 0) >= 2,
                 timeout=8.0, desc=f"{lane} dtcm EXPORT_ICS")

        exported_txt = out_ics.read_text(encoding="utf-8")
        assert "Architecture Sync\\, Wayland Desktop" in exported_txt or "Architecture Sync" in exported_txt
        assert "LOCATION:Lab B" in exported_txt
        assert "RRULE:FREQ=WEEKLY;INTERVAL=2;COUNT=4" in exported_txt
        assert "DTSTART;VALUE=DATE:20261224" in exported_txt

        dtcm_proc.terminate()
        dtcm_proc.wait(timeout=4.0)

        return f"{lane}: PASS (ck-tasks, ck-view, ck-pdf, dtpad, dtcm all verified)"
    finally:
        kill_procs(procs)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", type=Path, default=Path("./dtwm-wayland"))
    ap.add_argument("--apps-bin-dir", type=Path, default=Path("/var/tmp/ck-wayland-build/wp00/apps-build/bin"))
    ap.add_argument("--apps-src-dir", type=Path, default=Path("/var/tmp/ck-wayland-build/wp00/apps-src"))
    args = ap.parse_args()

    comp_bin = args.compositor.resolve()
    dtwm_x11_bin = (comp_bin.parent / "dtwm").resolve()
    cde_build_programs = comp_bin.parent.parent
    dtpad_bin = (cde_build_programs / "dtpad/dtpad").resolve()
    dtcm_bin = (cde_build_programs / "dtcm/dtcm/dtcm").resolve()

    tasks_bin = (args.apps_bin_dir / "ck-tasks").resolve()
    view_bin = (args.apps_bin_dir / "ck-view").resolve()
    pdf_bin = (args.apps_bin_dir / "ck-pdf").resolve()

    for b in (comp_bin, dtwm_x11_bin, dtpad_bin, dtcm_bin, tasks_bin, view_bin, pdf_bin):
        if not b.is_file():
            print(f"SKIP/FAIL: required binary not found: {b}", file=sys.stderr)
            return 1

    cc = shlex.split(os.environ.get("CC", "cc"))
    pkg_cflags = subprocess.run(
        ["pkg-config", "--cflags", "librsvg-2.0", "gdk-pixbuf-2.0", "cairo", "cairo-pdf", "libwebp"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    pkg_libs = subprocess.run(
        ["pkg-config", "--libs", "librsvg-2.0", "gdk-pixbuf-2.0", "cairo", "cairo-pdf", "libwebp"],
        capture_output=True, text=True, check=True,
    ).stdout.split()

    base_tmp = os.environ.get("TMPDIR") or "/var/tmp"
    with tempfile.TemporaryDirectory(prefix="wp19-dual-", dir=base_tmp) as td:
        work_root = Path(td)
        x11_src = work_root / "x11_prop_helper.c"
        x11_bin = work_root / "x11_prop_helper"
        x11_src.write_text(X11_PROP_HELPER_C, encoding="utf-8")
        subprocess.run([*cc, "-O2", str(x11_src), "-o", str(x11_bin), "-lX11"], check=True)

        agen_src = work_root / "asset_gen.c"
        agen_bin = work_root / "asset_gen"
        agen_src.write_text(ASSET_GEN_C, encoding="utf-8")
        subprocess.run(
            [*cc, "-O2", f"-I{args.apps_src_dir / 'src'}", *pkg_cflags,
             str(agen_src), str(args.apps_src_dir / "src/ck-view/ck-view-io.c"),
             "-o", str(agen_bin), *pkg_libs, "-lm"],
            check=True,
        )

        mock_sd_py = work_root / "mock_systemd.py"
        mock_sd_py.write_text(MOCK_SYSTEMD_PY, encoding="utf-8")

        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_wl = pool.submit(
                run_lane, "wayland", 91, work_root, comp_bin, dtwm_x11_bin,
                tasks_bin, view_bin, pdf_bin, dtpad_bin, dtcm_bin, x11_bin, agen_bin, mock_sd_py,
            )
            fut_x11 = pool.submit(
                run_lane, "x11", 92, work_root, comp_bin, dtwm_x11_bin,
                tasks_bin, view_bin, pdf_bin, dtpad_bin, dtcm_bin, x11_bin, agen_bin, mock_sd_py,
            )
            res_wl = fut_wl.result()
            res_x11 = fut_x11.result()

        print(res_wl)
        print(res_x11)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
