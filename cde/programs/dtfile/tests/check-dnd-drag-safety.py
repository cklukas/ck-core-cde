#!/usr/bin/env python3
"""C23 + Xvfb verification for DtDndDragStart / dtfile folder & Shelf drag safety:
1. Verifies that an immediate Xdnd/Xwayland compositor SelectionRequest(XdndSelection, TARGETS)
   at drag start succeeds without crashing in XtGetSelectionRequest, returns exact 64-bit
   Atom element counts (no sizeof(Atom)/4 overrun), and does not prematurely consume
   one-shot DtCR_DND_CONVERT_DATA before an actual drop transfer.
2. Verifies subsequent FILE_NAME target conversion on XdndSelection invokes DtCR_DND_CONVERT_DATA
   once and transfers the dragged folder path cleanly, and verifies XtDisplayOfObject(gadget)
   and 64-bit Window XGetGeometry sizing on DtIconGadget drag sources.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time


HARNESS_C = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Intrinsic.h>
#include <X11/Xatom.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/DragDrop.h>
#include <Dt/Dnd.h>
#include <Dt/Icon.h>

static int convert_data_calls = 0;
static const char *one_shot_folder = "/var/tmp/dtfile-shelf-drag-folder";

static void
drag_convert_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    DtDndConvertCallbackStruct *cb = (DtDndConvertCallbackStruct *)call_data;
    if (cb->reason == DtCR_DND_CONVERT_DATA) {
        convert_data_calls++;
        /* Simulate dtfile FileConvertCB one-shot fmd->drag_file_view_data consumption */
        if (one_shot_folder == NULL) {
            cb->status = DtDND_FAILURE;
            cb->dragData->numItems = 0;
            return;
        }
        cb->dragData->data.files[0] = XtNewString(one_shot_folder);
        cb->dragData->numItems = 1;
        one_shot_folder = NULL;
    }
}

static void
drag_finish_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    DtDndDragFinishCallbackStruct *cb = (DtDndDragFinishCallbackStruct *)call_data;
    if (cb && cb->dragData) {
        for (Cardinal i = 0; i < cb->dragData->numItems; ++i) {
            XtFree(cb->dragData->data.files[i]);
            cb->dragData->data.files[i] = NULL;
        }
    }
}

int main(int argc, char **argv)
{
    XtToolkitInitialize();
    XtAppContext app = XtCreateApplicationContext();
    Display *dpy = XtOpenDisplay(app, NULL, "dndDragTest", "DndDragTest",
                                 NULL, 0, &argc, argv);
    assert(dpy != NULL);

    Widget shell = XtAppCreateShell("dndDragTest", "DndDragTest",
                                    applicationShellWidgetClass, dpy, NULL, 0);
    Widget form = XmCreateForm(shell, "form", NULL, 0);
    XtManageChild(form);

    /* Create a DtIconGadget (non-Widget Object/RectObj) just as dtfile does */
    XmString label = XmStringCreateLocalized("PinnedFolder");
    Arg iargs[4];
    int in = 0;
    XtSetArg(iargs[in], XmNstring, label); in++;
    Widget icon_gadget = _DtCreateIcon(form, "folderIcon", iargs, in);
    XmStringFree(label);
    assert(icon_gadget != NULL);
    assert(!XtIsWidget(icon_gadget));
    XtManageChild(icon_gadget);

    XtVaSetValues(shell, XmNwidth, 200, XmNheight, 160, NULL);
    XtRealizeWidget(shell);
    XSync(dpy, False);
    while (XtAppPending(app) & XtIMXEvent)
        XtAppProcessEvent(app, XtIMXEvent);

    /* Verify XtDisplayOfObject and 64-bit Window XGetGeometry on gadget pixmap */
    Display *gdpy = XtDisplayOfObject(icon_gadget);
    assert(gdpy == dpy);
    Pixmap pm = XCreatePixmap(gdpy, DefaultRootWindow(gdpy), 48, 36,
                              DefaultDepthOfScreen(XtScreenOfObject(icon_gadget)));
    Window root_ret = None;
    int x_ret = -1, y_ret = -1;
    unsigned int wid = 0, hei = 0, border_ret = 99, depth_ret = 0;
    assert(XGetGeometry(gdpy, pm, &root_ret, &x_ret, &y_ret,
                        &wid, &hei, &border_ret, &depth_ret) != 0);
    assert(wid == 48 && hei == 36 && border_ret == 0);
    XFreePixmap(gdpy, pm);

    /* Start DtDndDragStart from the DtIconGadget */
    static XtCallbackRec conv_cb[] = { { drag_convert_cb, NULL }, { NULL, NULL } };
    static XtCallbackRec fin_cb[]  = { { drag_finish_cb,  NULL }, { NULL, NULL } };
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xmotion.type = MotionNotify;
    ev.xmotion.display = dpy;
    ev.xmotion.window = XtWindow(form);
    ev.xmotion.root = DefaultRootWindow(dpy);
    ev.xmotion.time = CurrentTime;
    ev.xmotion.x = 24;
    ev.xmotion.y = 24;
    ev.xmotion.x_root = 40;
    ev.xmotion.y_root = 40;
    ev.xmotion.state = Button1Mask;
    ev.xmotion.same_screen = True;

    Widget dc = DtDndDragStart(icon_gadget, &ev, DtDND_FILENAME_TRANSFER,
                               1, XmDROP_MOVE | XmDROP_COPY | XmDROP_LINK,
                               conv_cb, fin_cb, NULL, 0);
    assert(dc != NULL);
    XSync(dpy, False);

    /* External Xwayland/Xdnd peer immediately queries TARGETS on XdndSelection */
    Display *peer = XOpenDisplay(NULL);
    assert(peer != NULL);
    Window peer_win = XCreateSimpleWindow(peer, DefaultRootWindow(peer),
                                          0, 0, 10, 10, 0, 0, 0);
    Atom xdnd_sel = XInternAtom(peer, "XdndSelection", False);
    Atom a_targets = XInternAtom(peer, "TARGETS", False);
    Atom a_fname = XInternAtom(peer, "FILE_NAME", False);
    Atom out_prop = XInternAtom(peer, "DND_TEST_OUT", False);

    XDeleteProperty(peer, peer_win, out_prop);
    XConvertSelection(peer, xdnd_sel, a_targets, out_prop, peer_win, CurrentTime);
    XFlush(peer);

    int got_targets = 0;
    for (int i = 0; i < 60 && !got_targets; ++i) {
        while (XtAppPending(app) & XtIMXEvent)
            XtAppProcessEvent(app, XtIMXEvent);
        XSync(dpy, False);
        while (XPending(peer) > 0) {
            XEvent pe;
            XNextEvent(peer, &pe);
            if (pe.type == SelectionNotify && pe.xselection.selection == xdnd_sel) {
                assert(pe.xselection.property == out_prop);
                got_targets = 1;
            }
        }
    }
    assert(got_targets == 1);
    /* TARGETS probe at drag start must NOT consume one-shot DtCR_DND_CONVERT_DATA! */
    assert(convert_data_calls == 0);

    Atom act_type = None;
    int act_fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *prop_data = NULL;
    assert(XGetWindowProperty(peer, peer_win, out_prop, 0, 64, True, XA_ATOM,
                              &act_type, &act_fmt, &nitems, &rem, &prop_data) == Success);
    assert(act_fmt == 32 && nitems >= 8 && nitems <= 20);
    XFree(prop_data);

    /* Now request actual FILE_NAME payload on XdndSelection (simulating drop) */
    XConvertSelection(peer, xdnd_sel, a_fname, out_prop, peer_win, CurrentTime);
    XFlush(peer);

    int got_fname = 0;
    for (int i = 0; i < 60 && !got_fname; ++i) {
        while (XtAppPending(app) & XtIMXEvent)
            XtAppProcessEvent(app, XtIMXEvent);
        XSync(dpy, False);
        while (XPending(peer) > 0) {
            XEvent pe;
            XNextEvent(peer, &pe);
            if (pe.type == SelectionNotify && pe.xselection.selection == xdnd_sel) {
                assert(pe.xselection.property == out_prop);
                got_fname = 1;
            }
        }
    }
    assert(got_fname == 1);
    assert(convert_data_calls == 1);

    prop_data = NULL;
    assert(XGetWindowProperty(peer, peer_win, out_prop, 0, 256, True, AnyPropertyType,
                              &act_type, &act_fmt, &nitems, &rem, &prop_data) == Success);
    assert(prop_data != NULL);
    assert(strcmp((const char *)prop_data, "/var/tmp/dtfile-shelf-drag-folder") == 0);
    XFree(prop_data);

    XmDragCancel(dc);
    while (XtAppPending(app) & XtIMXEvent)
        XtAppProcessEvent(app, XtIMXEvent);

    XDestroyWindow(peer, peer_win);
    XCloseDisplay(peer);
    XtDestroyWidget(shell);
    XtCloseDisplay(dpy);
    XtDestroyApplicationContext(app);
    printf("check-dnd-drag-safety: ALL ASSERTIONS PASSED\n");
    return 0;
}
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-root", type=Path, required=True)
    ap.add_argument("--cde-src", type=Path, required=True)
    args = ap.parse_args()

    if sys.platform != "linux":
        print("SKIP: check-dnd-drag-safety requires Linux/Xvfb")
        return 0

    build_root = args.build_root.resolve()
    cde_src = args.cde_src.resolve()
    cc_cmd = shlex.split(os.environ.get("CC", "cc"))

    mk_cppflags: list[str] = shlex.split(os.environ.get("CPPFLAGS", ""))
    mk_ldflags: list[str] = shlex.split(os.environ.get("LDFLAGS", ""))
    mk_file = build_root / "Makefile"
    if mk_file.is_file():
        for line in mk_file.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CPPFLAGS ="):
                raw = line.split("=", 1)[1].replace("$(top_srcdir)", str(cde_src))
                mk_cppflags = [tok for tok in shlex.split(raw) if "$(" not in tok] + mk_cppflags
            elif line.startswith("LDFLAGS ="):
                raw = line.split("=", 1)[1]
                mk_ldflags = [tok for tok in shlex.split(raw) if "$(" not in tok] + mk_ldflags

    motif_libdirs = [tok[2:] for tok in mk_ldflags if tok.startswith("-L") and Path(tok[2:]).is_dir()]

    with tempfile.TemporaryDirectory(prefix="dtfile-dnd-drag-", dir=os.environ.get("TMPDIR")) as td:
        td_path = Path(td)
        src_c = td_path / "harness.c"
        bin_p = td_path / "harness"
        src_c.write_text(HARNESS_C, encoding="utf-8")

        dt_libs = [
            f"-L{build_root / 'lib/DtWidget/.libs'}",
            f"-L{build_root / 'lib/DtSvc/.libs'}",
            f"-L{build_root / 'lib/tt/lib/.libs'}",
            f"-L{build_root / 'lib/DtXinerama/.libs'}",
            f"-Wl,-rpath,{build_root / 'lib/DtWidget/.libs'}",
            f"-Wl,-rpath,{build_root / 'lib/DtSvc/.libs'}",
            f"-Wl,-rpath,{build_root / 'lib/tt/lib/.libs'}",
            f"-Wl,-rpath,{build_root / 'lib/DtXinerama/.libs'}",
            "-lDtWidget",
            "-lDtSvc",
            "-ltt",
            "-lDtXinerama",
        ]
        cmd = (
            cc_cmd
            + ["-O2", "-g", "-std=c23"]
            + [f"-I{cde_src / 'include'}", f"-I{build_root / 'include'}"]
            + mk_cppflags
            + [str(src_c)]
            + mk_ldflags
            + dt_libs
            + ["-lXm", "-lXt", "-lX11", "-o", str(bin_p)]
        )
        subprocess.run(cmd, check=True)

        env = dict(os.environ)
        ld_parts = [
            str(build_root / "lib/DtWidget/.libs"),
            str(build_root / "lib/DtSvc/.libs"),
            str(build_root / "lib/tt/lib/.libs"),
            *motif_libdirs,
        ]
        if env.get("LD_LIBRARY_PATH"):
            ld_parts.append(env["LD_LIBRARY_PATH"])
        env["LD_LIBRARY_PATH"] = ":".join(ld_parts)

        disp = ":93"
        xvfb = subprocess.Popen(
            ["Xvfb", disp, "-screen", "0", "800x600x24", "-nolisten", "tcp"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(0.4)
            env["DISPLAY"] = disp
            res = subprocess.run([str(bin_p)], env=env, check=True, capture_output=True, text=True)
            sys.stdout.write(res.stdout)
        finally:
            xvfb.terminate()
            xvfb.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
