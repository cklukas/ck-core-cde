#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland WP-03:
CDE Workspaces, Per-Workspace Backdrops, libDtSvc WSM Bridge, ext-workspace-v1 & Front Panel.
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


DTSVC_CLIENT_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Intrinsic.h>
#include <Xm/Xm.h>
#include <Xm/DrawingA.h>
#include <Dt/Wsm.h>
#include <Dt/WsmM.h>

extern int DtWsmAddWorkspace(Widget widget, char *pchTitle);
extern int DtWsmDeleteWorkspace(Widget widget, Atom aWs);
extern int DtWsmSetWorkspaceTitle(Widget widget, Atom aWs, char *pchNewName);

static void pump(XtAppContext app, int ms) {
    for (int i = 0; i < ms / 20; ++i) {
        while (XtAppPending(app)) {
            XEvent ev;
            XtAppNextEvent(app, &ev);
            XtDispatchEvent(&ev);
        }
        usleep(20000);
    }
}

int main(int argc, char **argv) {
    XtAppContext app;
    Widget shell = XtVaAppInitialize(&app, "DtwmWsTest", NULL, 0, &argc, argv, NULL,
                                     XmNwidth, 320, XmNheight, 200,
                                     XmNx, 220, XmNy, 120, NULL);
    Widget canvas = XmCreateDrawingArea(shell, "canvas", NULL, 0);
    XtManageChild(canvas);
    XtRealizeWidget(shell);
    Display *dpy = XtDisplay(shell);
    Window root = DefaultRootWindow(dpy);
    Window win = XtWindow(shell);
    pump(app, 300);

    const char *mode = (argc > 1) ? argv[1] : "query";

    if (strcmp(mode, "query") == 0) {
        Atom *ws_list = NULL;
        int num_ws = 0;
        if (DtWsmGetWorkspaceList(dpy, root, &ws_list, &num_ws) != Success || num_ws < 1) {
            fprintf(stderr, "DtWsmGetWorkspaceList failed\n");
            return 2;
        }
        Atom cur = None;
        if (DtWsmGetCurrentWorkspace(dpy, root, &cur) != Success) {
            fprintf(stderr, "DtWsmGetCurrentWorkspace failed\n");
            return 3;
        }
        char *cur_name = XGetAtomName(dpy, cur);
        printf("CURRENT=%s COUNT=%d\n", cur_name ? cur_name : "?", num_ws);
        if (cur_name) XFree(cur_name);

        for (int i = 0; i < num_ws; ++i) {
            DtWsmWorkspaceInfo *info = NULL;
            if (DtWsmGetWorkspaceInfo(dpy, root, ws_list[i], &info) != Success || !info) {
                fprintf(stderr, "DtWsmGetWorkspaceInfo failed for %d\n", i);
                return 4;
            }
            char *idn = XGetAtomName(dpy, ws_list[i]);
            char *bdn = info->backdropName ? XGetAtomName(dpy, info->backdropName) : NULL;
            printf("WS[%d]=%s TITLE=%s CS=%d BD=%s\n",
                   i, idn ? idn : "?", info->pchTitle ? info->pchTitle : "?",
                   info->colorSetId, bdn ? bdn : "none");
            if (idn) XFree(idn);
            if (bdn) XFree(bdn);
            DtWsmFreeWorkspaceInfo(info);
        }
        XFree(ws_list);
        return 0;
    }

    if (strcmp(mode, "mutations") == 0) {
        /* 1. Occupy all workspaces and verify _DT_WORKSPACE_PRESENCE */
        DtWsmOccupyAllWorkspaces(dpy, win);
        XFlush(dpy);
        pump(app, 300);

        Atom *occ = NULL;
        unsigned long n_occ = 0;
        if (DtWsmGetWorkspacesOccupied(dpy, win, &occ, &n_occ) != Success || n_occ != 4) {
            fprintf(stderr, "expected 4 occupied workspaces after OccupyAll, got %lu\n", n_occ);
            return 5;
        }
        XFree(occ);

        /* 2. Switch to ws2 ("Three") via DtWsmSetCurrentWorkspace (returns dtmsg_SUCCESS == 1) */
        Atom ws2 = XInternAtom(dpy, "ws2", False);
        if (DtWsmSetCurrentWorkspace(canvas, ws2) != 1) {
            fprintf(stderr, "DtWsmSetCurrentWorkspace failed\n");
            return 6;
        }
        pump(app, 300);
        Atom cur = None;
        DtWsmGetCurrentWorkspace(dpy, root, &cur);
        if (cur != ws2) {
            fprintf(stderr, "expected current workspace ws2 (%lu), got %lu\n", (unsigned long)ws2, (unsigned long)cur);
            return 7;
        }

        /* 3. Change backdrop on ws2 to BrickWall */
        DtWsmChangeBackdrop(dpy, root, "BrickWall", None);
        XFlush(dpy);
        pump(app, 300);
        DtWsmWorkspaceInfo *info = NULL;
        if (DtWsmGetWorkspaceInfo(dpy, root, ws2, &info) != Success || !info) {
            return 8;
        }
        char *bdn = XGetAtomName(dpy, info->backdropName);
        if (!bdn || strcmp(bdn, "BrickWall") != 0) {
            fprintf(stderr, "expected BrickWall backdrop atom, got %s\n", bdn ? bdn : "NULL");
            return 9;
        }
        XFree(bdn);
        DtWsmFreeWorkspaceInfo(info);

        /* 4. Add workspace "Five", rename to "Studio", then delete it */
        if (DtWsmAddWorkspace(canvas, "Five") != Success) {
            fprintf(stderr, "DtWsmAddWorkspace failed\n");
            return 10;
        }
        pump(app, 300);
        Atom *ws_list = NULL;
        int num_ws = 0;
        DtWsmGetWorkspaceList(dpy, root, &ws_list, &num_ws);
        if (num_ws != 5) {
            fprintf(stderr, "expected 5 workspaces after add, got %d\n", num_ws);
            return 11;
        }
        Atom ws4 = ws_list[4];
        XFree(ws_list);

        if (DtWsmSetWorkspaceTitle(canvas, ws4, "Studio") != Success) {
            fprintf(stderr, "DtWsmSetWorkspaceTitle failed\n");
            return 12;
        }
        pump(app, 300);
        DtWsmGetWorkspaceInfo(dpy, root, ws4, &info);
        if (!info || strcmp(info->pchTitle, "Studio") != 0) {
            fprintf(stderr, "expected renamed workspace Studio, got %s\n", info ? info->pchTitle : "NULL");
            return 13;
        }
        DtWsmFreeWorkspaceInfo(info);

        if (DtWsmDeleteWorkspace(canvas, ws4) != Success) {
            fprintf(stderr, "DtWsmDeleteWorkspace failed\n");
            return 14;
        }
        pump(app, 300);
        DtWsmGetWorkspaceList(dpy, root, &ws_list, &num_ws);
        if (num_ws != 4) {
            fprintf(stderr, "expected 4 workspaces after delete, got %d\n", num_ws);
            return 15;
        }
        XFree(ws_list);
        printf("MUTATIONS_OK\n");
        return 0;
    }

    return 1;
}
"""


EXT_WS_CLIENT_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "ext-workspace-v1-client-protocol.h"

typedef struct {
    struct ext_workspace_handle_v1 *h;
    char name[64];
    uint32_t state;
} WsItem;

static struct ext_workspace_manager_v1 *g_mgr = NULL;
static WsItem g_items[16];
static int g_count = 0;
static int g_done = 0;

static void ws_id(void *data, struct ext_workspace_handle_v1 *h, const char *id) { (void)data; (void)h; (void)id; }
static void ws_name(void *data, struct ext_workspace_handle_v1 *h, const char *name) {
    (void)h;
    WsItem *it = data;
    snprintf(it->name, sizeof(it->name), "%s", name);
}
static void ws_coords(void *data, struct ext_workspace_handle_v1 *h, struct wl_array *c) { (void)data; (void)h; (void)c; }
static void ws_state(void *data, struct ext_workspace_handle_v1 *h, uint32_t st) {
    (void)h;
    WsItem *it = data;
    it->state = st;
}
static void ws_caps(void *data, struct ext_workspace_handle_v1 *h, uint32_t c) { (void)data; (void)h; (void)c; }
static void ws_removed(void *data, struct ext_workspace_handle_v1 *h) { (void)data; (void)h; }

static const struct ext_workspace_handle_v1_listener ws_listener = {
    .id = ws_id, .name = ws_name, .coordinates = ws_coords,
    .state = ws_state, .capabilities = ws_caps, .removed = ws_removed,
};

static void grp_caps(void *d, struct ext_workspace_group_handle_v1 *g, uint32_t c) { (void)d; (void)g; (void)c; }
static void grp_out_in(void *d, struct ext_workspace_group_handle_v1 *g, struct wl_output *o) { (void)d; (void)g; (void)o; }
static void grp_out_out(void *d, struct ext_workspace_group_handle_v1 *g, struct wl_output *o) { (void)d; (void)g; (void)o; }
static void grp_ws_in(void *d, struct ext_workspace_group_handle_v1 *g, struct ext_workspace_handle_v1 *w) { (void)d; (void)g; (void)w; }
static void grp_ws_out(void *d, struct ext_workspace_group_handle_v1 *g, struct ext_workspace_handle_v1 *w) { (void)d; (void)g; (void)w; }
static void grp_rem(void *d, struct ext_workspace_group_handle_v1 *g) { (void)d; (void)g; }

static const struct ext_workspace_group_handle_v1_listener grp_listener = {
    .capabilities = grp_caps, .output_enter = grp_out_in, .output_leave = grp_out_out,
    .workspace_enter = grp_ws_in, .workspace_leave = grp_ws_out, .removed = grp_rem,
};

static void mgr_grp(void *d, struct ext_workspace_manager_v1 *m, struct ext_workspace_group_handle_v1 *g) {
    (void)d; (void)m;
    ext_workspace_group_handle_v1_add_listener(g, &grp_listener, NULL);
}
static void mgr_ws(void *d, struct ext_workspace_manager_v1 *m, struct ext_workspace_handle_v1 *w) {
    (void)d; (void)m;
    if (g_count < 16) {
        WsItem *it = &g_items[g_count++];
        it->h = w;
        ext_workspace_handle_v1_add_listener(w, &ws_listener, it);
    }
}
static void mgr_done(void *d, struct ext_workspace_manager_v1 *m) { (void)d; (void)m; g_done++; }
static void mgr_fin(void *d, struct ext_workspace_manager_v1 *m) { (void)d; (void)m; }

static const struct ext_workspace_manager_v1_listener mgr_listener = {
    .workspace_group = mgr_grp, .workspace = mgr_ws, .done = mgr_done, .finished = mgr_fin,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (strcmp(iface, ext_workspace_manager_v1_interface.name) == 0) {
        g_mgr = wl_registry_bind(r, name, &ext_workspace_manager_v1_interface, 1);
        ext_workspace_manager_v1_add_listener(g_mgr, &mgr_listener, NULL);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_rem };

int main(int argc, char **argv) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!g_mgr) {
        fprintf(stderr, "ext_workspace_manager_v1 global missing\n");
        return 2;
    }
    while (g_done == 0 && wl_display_dispatch(dpy) != -1) {}

    if (argc > 2 && strcmp(argv[1], "activate") == 0) {
        int idx = atoi(argv[2]);
        if (idx < 0 || idx >= g_count) return 3;
        int prev_done = g_done;
        ext_workspace_handle_v1_activate(g_items[idx].h);
        ext_workspace_manager_v1_commit(g_mgr);
        wl_display_roundtrip(dpy);
        while (g_done == prev_done && wl_display_dispatch(dpy) != -1) {}
    }

    for (int i = 0; i < g_count; ++i) {
        printf("EXT_WS[%d]=%s STATE=%u\n", i, g_items[i].name, g_items[i].state);
    }
    wl_display_disconnect(dpy);
    return 0;
}
"""


def find_cde_build_and_src(comp_bin: Path) -> tuple[Path, Path]:
    # comp_bin is <cde-build>/programs/dtwm/dtwm-wayland
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
    inc, lib = [], []
    if not makefile.is_file():
        return inc, lib
    for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
        if ln.startswith("X_CFLAGS =") or ln.startswith("DTCLIENTLIBS =") or ln.startswith("CPPFLAGS =") or ln.startswith("LDFLAGS ="):
            for tok in ln.split("=", 1)[1].split():
                if tok.startswith("-I"):
                    inc.append(tok)
                elif tok.startswith("-L") or tok.startswith("-Wl,-rpath"):
                    lib.append(tok)
    return inc, lib


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    ap.add_argument("--save-dir", default="")
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    cde_build, cde_src = find_cde_build_and_src(comp_bin)
    inc_flags, lib_flags = extract_motif_dirs(comp_bin.parent / "Makefile")
    cc = os.environ.get("CC", "cc")

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-wayland-ws-", dir=tmp_base) as work_s:
        work = Path(work_s)
        xdg = work / "xdg"
        home = work / "home"
        xdg.mkdir(mode=0o700)
        home.mkdir(mode=0o700)
        os.chmod(xdg, 0o700)
        os.chmod(home, 0o700)

        # Compile libDtSvc client
        dtsvc_c = work / "dtsvc_ws_client.c"
        dtsvc_bin = work / "dtsvc_ws_client"
        dtsvc_c.write_text(DTSVC_CLIENT_C, encoding="utf-8")
        dtsvc_lib_dir = cde_build / "lib" / "DtSvc" / ".libs"
        if not dtsvc_lib_dir.is_dir():
            dtsvc_lib_dir = cde_build / "lib" / "DtSvc"
        tt_lib_dir = cde_build / "lib" / "tt" / "lib" / ".libs"
        dtxin_lib_dir = cde_build / "lib" / "DtXinerama" / ".libs"
        compile_cmd = [
            cc, "-O2", str(dtsvc_c),
            f"-I{cde_build / 'include'}", f"-I{cde_src / 'include'}",
            *[f for f in inc_flags if "$(" not in f],
            f"-L{dtsvc_lib_dir}", f"-Wl,-rpath,{dtsvc_lib_dir}",
            *( [f"-L{tt_lib_dir}", f"-Wl,-rpath,{tt_lib_dir}"] if tt_lib_dir.is_dir() else [] ),
            *( [f"-L{dtxin_lib_dir}", f"-Wl,-rpath,{dtxin_lib_dir}"] if dtxin_lib_dir.is_dir() else [] ),
            *[f for f in lib_flags if "$(" not in f],
            "-lDtSvc", "-ltt", "-lDtXinerama", "-lXm", "-lXt", "-lX11",
            "-o", str(dtsvc_bin),
        ]
        subprocess.run(compile_cmd, check=True)

        # Compile ext-workspace-v1 Wayland client
        wl_xml = "/usr/share/wayland-protocols/staging/ext-workspace/ext-workspace-v1.xml"
        ext_hdr = work / "ext-workspace-v1-client-protocol.h"
        ext_code = work / "ext-workspace-v1-protocol.c"
        subprocess.run(["wayland-scanner", "client-header", wl_xml, str(ext_hdr)], check=True)
        subprocess.run(["wayland-scanner", "private-code", wl_xml, str(ext_code)], check=True)
        ext_c = work / "ext_ws_client.c"
        ext_bin = work / "ext_ws_client"
        ext_c.write_text(EXT_WS_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", f"-I{work}", str(ext_c), str(ext_code), "-lwayland-client", "-o", str(ext_bin)],
            check=True,
        )

        backdrops_dir = cde_src / "programs" / "backdrops"
        env = os.environ.copy()
        env.update({
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "2",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
            "CDE_BACKDROP_DIR": str(backdrops_dir),
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
        procs = [comp]

        try:
            wl_sock = x_sock = None
            for _ in range(75):
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

            # 1. Verify initial libDtSvc + ext-workspace-v1 workspace discovery
            q1 = subprocess.run([str(dtsvc_bin), "query"], env=c_env, check=True, capture_output=True, text=True).stdout
            if "CURRENT=ws0 COUNT=4" not in q1 or "TITLE=One" not in q1 or "TITLE=Four" not in q1:
                raise SystemExit(f"unexpected initial libDtSvc query:\n{q1}")

            ew1 = subprocess.run([str(ext_bin)], env=c_env, check=True, capture_output=True, text=True).stdout
            if "EXT_WS[0]=One STATE=1" not in ew1 or "EXT_WS[1]=Two STATE=0" not in ew1:
                raise SystemExit(f"unexpected initial ext-workspace-v1 output:\n{ew1}")

            # 2. Spawn Wayland foot window on Workspace 1 ("One"), capture screenshot
            p_wl = subprocess.Popen(["foot"], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs.append(p_wl)
            time.sleep(1.0)

            shot_ws1 = work / "wp03-ws1-one.png"
            subprocess.run(["grim", str(shot_ws1)], env=c_env, check=True)

            # 3. Click Workspace "Two" button on the docked CDE Front Panel!
            # Output 1 is 1280x900 (or 1280x720 headless default — query image height!)
            with Image.open(shot_ws1) as im1:
                total_w, out_h = im1.size
                out1_w = total_w // 2
                sample_ws1 = im1.convert("RGB").getpixel((out1_w - 60, 40))

            # Panel is 840x74 centered on output 1 at y = out_h - 76; central well is 248px wide centered at out1_w/2.
            # Top-right button in 2x2 grid is Workspace "Two" ("ws1") at (out1_w/2 + 60, out_h - 54)
            btn_two_x = out1_w // 2 + 60
            btn_two_y = out_h - 54
            subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
            subprocess.run(["wlrctl", "pointer", "move", str(btn_two_x), str(btn_two_y)], env=c_env, check=True)
            subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
            time.sleep(0.4)

            q2 = subprocess.run([str(dtsvc_bin), "query"], env=c_env, check=True, capture_output=True, text=True).stdout
            if "CURRENT=ws1 COUNT=4" not in q2:
                raise SystemExit(f"Front Panel click did not switch workspace to ws1:\n{q2}")

            shot_ws2 = work / "wp03-ws2-two.png"
            subprocess.run(["grim", str(shot_ws2)], env=c_env, check=True)
            with Image.open(shot_ws2) as im2:
                sample_ws2 = im2.convert("RGB").getpixel((out1_w - 60, 40))
                if sample_ws1 == sample_ws2:
                    raise SystemExit(f"per-workspace backdrop pixel did not change between ws0 {sample_ws1} and ws1 {sample_ws2}")

            # 4. Run full libDtSvc WSM mutations (OccupyAll, SetCurrentWorkspace, ChangeBackdrop, Add, Rename, Delete)
            mut = subprocess.run([str(dtsvc_bin), "mutations"], env=c_env, check=True, capture_output=True, text=True).stdout
            if "MUTATIONS_OK" not in mut:
                raise SystemExit(f"libDtSvc mutations failed:\n{mut}")

            shot_ws3 = work / "wp03-ws3-brickwall.png"
            subprocess.run(["grim", str(shot_ws3)], env=c_env, check=True)

            # 5. Switch back to Workspace 1 ("One") via native Wayland ext-workspace-v1 protocol & verify maximize strut
            ew_act = subprocess.run([str(ext_bin), "activate", "0"], env=c_env, check=True, capture_output=True, text=True).stdout
            if "EXT_WS[0]=One STATE=1" not in ew_act:
                raise SystemExit(f"ext-workspace-v1 activate(0) failed:\n{ew_act}")

            # Double-click foot titlebar (120, 56) to maximize -> verify Front Panel bottom strut remains uncovered!
            subprocess.run(["wlrctl", "pointer", "move", "-10000", "-10000"], env=c_env, check=True)
            subprocess.run(["wlrctl", "pointer", "move", "120", "56"], env=c_env, check=True)
            subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
            time.sleep(0.08)
            subprocess.run(["wlrctl", "pointer", "click", "left"], env=c_env, check=True)
            time.sleep(0.4)

            shot_max = work / "wp03-ws1-maximized-strut.png"
            subprocess.run(["grim", str(shot_max)], env=c_env, check=True)
            with Image.open(shot_max) as im_max:
                # Sample Front Panel chassis at (out1_w//2 - 200, out_h - 10) -> must still be CDE panel slate (~174,178,195), NOT foot terminal black!
                panel_px = im_max.convert("RGB").getpixel((out1_w // 2 - 200, out_h - 10))
                if panel_px[0] < 120 or panel_px[1] < 120:
                    raise SystemExit(f"maximized window covered Front Panel strut! pixel at panel={panel_px}")

            if args.save_dir:
                sdir = Path(args.save_dir)
                sdir.mkdir(parents=True, exist_ok=True)
                for f in (shot_ws1, shot_ws2, shot_ws3, shot_max):
                    shutil.copy2(f, sdir / f.name)

            print(
                f"PASS: dtwm-wayland WP-03 workspaces, backdrops ({sample_ws1}->{sample_ws2}), "
                f"libDtSvc WSM bridge, ext-workspace-v1, and Front Panel strut ({panel_px}) verified"
            )
            return 0
        finally:
            for p in reversed(procs):
                if p.poll() is None:
                    p.terminate()
            for p in reversed(procs):
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
            comp_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
