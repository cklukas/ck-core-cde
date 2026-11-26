#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland & CK Core Motif ABI 9:
xdg-dialog-v1 (modal centering, parent client dimming, focus/click lock),
wp-tearing-control-v1 (ASYNC presentation hint & page-flip attempt), and
wp-fifo-v1 (set_barrier / wait_barrier frame pacing & protocol error guards).
"""

from __future__ import annotations

import argparse
from contextlib import suppress
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time


MOTIF_MODAL_CLIENT_C = r"""
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <Xm/DrawingA.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <Xm/Wayland.h>
#include <Xm/Xm.h>
#include <wayland-client.h>

static Widget g_top = NULL;
static Widget g_canvas = NULL;
static Widget g_dialog = NULL;
static int g_parent_clicks = 0;
static const char *g_state_path = NULL;
static const char *g_cmd_path = NULL;
static bool g_dialog_open = false;

static struct wl_surface *top_wl_surface(void) {
    struct wl_surface *s = NULL;
    if (g_top && XtIsRealized(g_top)) {
        (void)XmWaylandGetWidgetSurface(g_top, &s, NULL, NULL, NULL, NULL, NULL);
    }
    return s;
}

static void write_state(void) {
    if (!g_state_path) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    Dimension pw = 0, ph = 0, dw = 0, dh = 0;
    if (g_top) XtVaGetValues(g_top, XmNwidth, &pw, XmNheight, &ph, NULL);
    if (g_dialog) XtVaGetValues(XtParent(g_dialog), XmNwidth, &dw, XmNheight, &dh, NULL);
    Display *dpy = g_top ? XtDisplay(g_top) : NULL;
    struct wl_surface *psurf = top_wl_surface();
    fprintf(f,
            "WAYLAND=%d\nPARENT_SURF=%d\nDIALOG_OPEN=%d\nPARENT_CLICKS=%d\n"
            "PW=%u\nPH=%u\nDW=%u\nDH=%u\n",
            (dpy && XmWaylandIsActive(dpy)) ? 1 : 0,
            psurf ? 1 : 0,
            g_dialog_open ? 1 : 0,
            g_parent_clicks,
            (unsigned)pw, (unsigned)ph,
            (unsigned)dw, (unsigned)dh);
    fclose(f);
    rename(tmp, g_state_path);
}

static void on_parent_btn(Widget w, XtPointer cd, XEvent *ev, Boolean *cont) {
    (void)w; (void)cd; (void)cont;
    if (ev && ev->type == ButtonPress) {
        g_parent_clicks++;
        write_state();
    }
}

static void on_poll_cmd(XtPointer cd, XtIntervalId *id) {
    (void)id;
    XtAppContext app = (XtAppContext)cd;
    if (g_cmd_path && access(g_cmd_path, R_OK) == 0) {
        FILE *f = fopen(g_cmd_path, "r");
        char buf[64] = {0};
        if (f) {
            if (fgets(buf, sizeof(buf), f)) {
                buf[strcspn(buf, "\r\n")] = '\0';
            }
            fclose(f);
        }
        unlink(g_cmd_path);
        if (strcmp(buf, "open_modal") == 0 && g_dialog && !g_dialog_open) {
            XtManageChild(g_dialog);
            g_dialog_open = true;
            XFlush(XtDisplay(g_top));
        } else if (strcmp(buf, "close_modal") == 0 && g_dialog && g_dialog_open) {
            XtUnmanageChild(g_dialog);
            g_dialog_open = false;
            XFlush(XtDisplay(g_top));
        } else if (strcmp(buf, "tearing_on") == 0 || strcmp(buf, "tearing_off") == 0) {
            Display *dpy = XtDisplay(g_top);
            struct wl_surface *s = top_wl_surface();
            if (dpy && s) {
                struct wl_display *wld = NULL;
                XmWaylandLock(dpy);
                (void)XmWaylandGetDisplayHandles(dpy, &wld, NULL, NULL, NULL, NULL);
                (void)XmWaylandSetSurfaceTearingHint(
                    dpy, s, strcmp(buf, "tearing_on") == 0 ? True : False
                );
                wl_surface_commit(s);
                if (wld) wl_display_flush(wld);
                XmWaylandUnlock(dpy);
            }
        } else if (strcmp(buf, "quit") == 0) {
            XtAppSetExitFlag(app);
            return;
        }
    }
    write_state();
    XtAppAddTimeOut(app, 30, on_poll_cmd, app);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    g_state_path = argv[1];
    g_cmd_path = argv[2];
    XtAppContext app;
    g_top = XtVaAppInitialize(&app, "MotifModalTest", NULL, 0, &argc, argv, NULL,
                              XmNtitle, "ModalParentWin",
                              XmNwidth, 520,
                              XmNheight, 380,
                              NULL);
    Display *dpy = XtDisplay(g_top);
    Colormap cmap = DefaultColormapOfScreen(XtScreen(g_top));
    XColor green = {0};
    green.red = 0;
    green.green = 52428; /* ~204 in 8-bit (#00cc00) */
    green.blue = 0;
    green.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, cmap, &green);

    g_canvas = XtVaCreateManagedWidget("canvas", xmDrawingAreaWidgetClass, g_top,
                                       XmNwidth, 520,
                                       XmNheight, 380,
                                       XmNbackground, green.pixel,
                                       NULL);
    XtAddEventHandler(g_canvas, ButtonPressMask, False, on_parent_btn, NULL);

    XmString msg = XmStringCreateLocalized("Confirm critical operation on Wayland?");
    XmString dtitle = XmStringCreateLocalized("ModalChildDlg");
    Arg args[8];
    Cardinal n = 0;
    XtSetArg(args[n], XmNmessageString, msg); n++;
    XtSetArg(args[n], XmNdialogTitle, dtitle); n++;
    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++;
    g_dialog = XmCreateWarningDialog(g_top, "warnDlg", args, n);
    XmStringFree(msg);
    XmStringFree(dtitle);

    XtRealizeWidget(g_top);
    XFlush(dpy);
    struct wl_surface *psurf = top_wl_surface();
    if (psurf) {
        struct wl_display *wld = NULL;
        XmWaylandLock(dpy);
        (void)XmWaylandGetDisplayHandles(dpy, &wld, NULL, NULL, NULL, NULL);
        (void)XmWaylandSetSurfaceFifo(dpy, psurf, True, False);
        wl_surface_commit(psurf);
        if (wld) wl_display_flush(wld);
        XmWaylandUnlock(dpy);
    }
    write_state();
    XtAppAddTimeOut(app, 30, on_poll_cmd, app);
    XtAppMainLoop(app);
    psurf = top_wl_surface();
    if (psurf) {
        XmWaylandLock(dpy);
        XmWaylandReleaseSurfaceExtensions(dpy, psurf);
        XmWaylandUnlock(dpy);
    }
    return 0;
}
"""


PROTO_V2_CLIENT_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "tearing-control-v1-client-protocol.h"
#include "fifo-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"

typedef struct {
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct xdg_wm_dialog_v1 *dlg_mgr;
    struct wp_tearing_control_manager_v1 *tear_mgr;
    struct wp_fifo_manager_v1 *fifo_mgr;
    struct wp_presentation *pres;
    int presented_count;
    uint64_t pres_nsec[8];
    bool configured;
    int32_t w, h;
} Ctx;

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener g_wm_l = { .ping = wm_ping };

static void pres_clk(void *d, struct wp_presentation *p, uint32_t c) { (void)d; (void)p; (void)c; }
static const struct wp_presentation_listener g_pres_l = { .clock_id = pres_clk };

static void fb_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) {
    (void)d; (void)f; (void)o;
}
static void fb_pres(
    void *d, struct wp_presentation_feedback *f,
    uint32_t sh, uint32_t sl, uint32_t ns,
    uint32_t ref, uint32_t qh, uint32_t ql, uint32_t fl
) {
    (void)ref; (void)qh; (void)ql; (void)fl;
    Ctx *c = d;
    uint64_t sec = ((uint64_t)sh << 32) | sl;
    if (c->presented_count < 8) {
        c->pres_nsec[c->presented_count] = sec * 1000000000ull + ns;
    }
    c->presented_count++;
    wp_presentation_feedback_destroy(f);
}
static void fb_disc(void *d, struct wp_presentation_feedback *f) {
    (void)d;
    wp_presentation_feedback_destroy(f);
}
static const struct wp_presentation_feedback_listener g_fb_l = {
    .sync_output = fb_sync, .presented = fb_pres, .discarded = fb_disc
};

static void top_cfg(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {
    (void)t; (void)s;
    Ctx *c = d;
    if (w > 0) c->w = w;
    if (h > 0) c->h = h;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *a) { (void)d; (void)t; (void)a; }
static const struct xdg_toplevel_listener g_top_l = {
    .configure = top_cfg, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps
};

static void xsurf_cfg(void *d, struct xdg_surface *xs, uint32_t serial) {
    Ctx *c = d;
    xdg_surface_ack_configure(xs, serial);
    c->configured = true;
}
static const struct xdg_surface_listener g_xs_l = { .configure = xsurf_cfg };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    Ctx *c = d;
    if (!strcmp(iface, wl_compositor_interface.name))
        c->comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        c->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
        xdg_wm_base_add_listener(c->wm, &g_wm_l, c);
    } else if (!strcmp(iface, xdg_wm_dialog_v1_interface.name))
        c->dlg_mgr = wl_registry_bind(r, name, &xdg_wm_dialog_v1_interface, 1);
    else if (!strcmp(iface, wp_tearing_control_manager_v1_interface.name))
        c->tear_mgr = wl_registry_bind(r, name, &wp_tearing_control_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_fifo_manager_v1_interface.name))
        c->fifo_mgr = wl_registry_bind(r, name, &wp_fifo_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_presentation_interface.name)) {
        c->pres = wl_registry_bind(r, name, &wp_presentation_interface, ver < 2 ? ver : 2);
        wp_presentation_add_listener(c->pres, &g_pres_l, c);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

static struct wl_buffer *make_buf(struct wl_shm *shm, int w, int h, uint32_t pixel) {
    size_t sz = (size_t)w * (size_t)h * 4u;
    int fd = memfd_create("fifo-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)sz) < 0) return NULL;
    uint32_t *map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }
    for (size_t i = 0; i < (size_t)(w * h); ++i) map[i] = pixel;
    munmap(map, sz);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)sz);
    close(fd);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    return b;
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "normal";
    Ctx c = { .w = 240, .h = 180 };
    c.dpy = wl_display_connect(NULL);
    if (!c.dpy) return 2;
    struct wl_registry *reg = wl_display_get_registry(c.dpy);
    wl_registry_add_listener(reg, &g_reg_l, &c);
    wl_display_roundtrip(c.dpy);
    wl_display_roundtrip(c.dpy);
    if (!c.comp || !c.shm || !c.wm || !c.dlg_mgr || !c.tear_mgr || !c.fifo_mgr || !c.pres) {
        fprintf(stderr, "missing required globals\n");
        return 3;
    }

    if (strcmp(mode, "err_dup_fifo") == 0) {
        struct wl_surface *s = wl_compositor_create_surface(c.comp);
        struct wp_fifo_v1 *f1 = wp_fifo_manager_v1_get_fifo(c.fifo_mgr, s);
        struct wp_fifo_v1 *f2 = wp_fifo_manager_v1_get_fifo(c.fifo_mgr, s);
        (void)f1; (void)f2;
        int rc = wl_display_roundtrip(c.dpy);
        int err = wl_display_get_error(c.dpy);
        uint32_t pcode = 0;
        const struct wl_interface *pif = NULL;
        if (err == EPROTO) pcode = wl_display_get_protocol_error(c.dpy, &pif, NULL);
        printf("RC=%d ERR=%d PCODE=%u IF=%s\n", rc, err, pcode, pif ? pif->name : "none");
        return (err == EPROTO && pcode == WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS) ? 0 : 4;
    }

    if (strcmp(mode, "err_destroyed_surf") == 0) {
        struct wl_surface *s = wl_compositor_create_surface(c.comp);
        struct wp_fifo_v1 *f1 = wp_fifo_manager_v1_get_fifo(c.fifo_mgr, s);
        wl_surface_destroy(s);
        wl_display_roundtrip(c.dpy);
        wp_fifo_v1_set_barrier(f1);
        int rc = wl_display_roundtrip(c.dpy);
        int err = wl_display_get_error(c.dpy);
        uint32_t pcode = 0;
        const struct wl_interface *pif = NULL;
        if (err == EPROTO) pcode = wl_display_get_protocol_error(c.dpy, &pif, NULL);
        printf("RC=%d ERR=%d PCODE=%u IF=%s\n", rc, err, pcode, pif ? pif->name : "none");
        return (err == EPROTO && pcode == WP_FIFO_V1_ERROR_SURFACE_DESTROYED) ? 0 : 5;
    }

    struct wl_surface *surf = wl_compositor_create_surface(c.comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, surf);
    xdg_surface_add_listener(xs, &g_xs_l, &c);
    struct xdg_toplevel *xt = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(xt, &g_top_l, &c);
    xdg_toplevel_set_title(xt, "FifoTearingWin");

    struct xdg_dialog_v1 *dlg = xdg_wm_dialog_v1_get_xdg_dialog(c.dlg_mgr, xt);
    xdg_dialog_v1_set_modal(dlg);
    xdg_dialog_v1_unset_modal(dlg);

    struct wp_tearing_control_v1 *tc =
        wp_tearing_control_manager_v1_get_tearing_control(c.tear_mgr, surf);
    wp_tearing_control_v1_set_presentation_hint(
        tc, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC
    );
    struct wp_fifo_v1 *fifo = wp_fifo_manager_v1_get_fifo(c.fifo_mgr, surf);

    wl_surface_commit(surf);
    while (!c.configured) {
        if (wl_display_dispatch(c.dpy) < 0) return 6;
    }

    struct wl_buffer *b0 = make_buf(c.shm, c.w, c.h, 0xff204080u);
    struct wl_buffer *b1 = make_buf(c.shm, c.w, c.h, 0xff802040u);
    struct wl_buffer *b2 = make_buf(c.shm, c.w, c.h, 0xff208040u);
    struct wl_buffer *b3 = make_buf(c.shm, c.w, c.h, 0xff808020u);

    /* Initial map commit */
    wl_surface_attach(surf, b0, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, c.w, c.h);
    wl_surface_commit(surf);
    wl_display_roundtrip(c.dpy);
    usleep(50000);
    wl_display_roundtrip(c.dpy);

    /* Queue 3 back-to-back FIFO-constrained commits in a single burst */
    struct wl_buffer *burst[3] = { b1, b2, b3 };
    for (int i = 0; i < 3; ++i) {
        struct wp_presentation_feedback *fb = wp_presentation_feedback(c.pres, surf);
        wp_presentation_feedback_add_listener(fb, &g_fb_l, &c);
        if (i > 0) {
            wp_fifo_v1_wait_barrier(fifo);
        }
        wp_fifo_v1_set_barrier(fifo);
        wl_surface_attach(surf, burst[i], 0, 0);
        wl_surface_damage_buffer(surf, 0, 0, c.w, c.h);
        wl_surface_commit(surf);
    }
    wl_display_flush(c.dpy);

    struct timespec start_ts, cur_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    while (c.presented_count < 3) {
        wl_display_dispatch_pending(c.dpy);
        wl_display_flush(c.dpy);
        usleep(10000);
        wl_display_roundtrip(c.dpy);
        clock_gettime(CLOCK_MONOTONIC, &cur_ts);
        double elapsed = (cur_ts.tv_sec - start_ts.tv_sec) +
                         (cur_ts.tv_nsec - start_ts.tv_nsec) / 1e9;
        if (elapsed > 3.0) break;
    }

    wp_tearing_control_v1_set_presentation_hint(
        tc, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC
    );
    wl_surface_commit(surf);
    wl_display_roundtrip(c.dpy);

    printf("PRESENTED=%d\n", c.presented_count);
    wp_fifo_v1_destroy(fifo);
    wp_tearing_control_v1_destroy(tc);
    xdg_dialog_v1_destroy(dlg);
    xdg_toplevel_destroy(xt);
    xdg_surface_destroy(xs);
    wl_surface_destroy(surf);
    wl_buffer_destroy(b0);
    wl_buffer_destroy(b1);
    wl_buffer_destroy(b2);
    wl_buffer_destroy(b3);
    wl_display_roundtrip(c.dpy);
    wl_display_disconnect(c.dpy);
    return (c.presented_count >= 2) ? 0 : 7;
}
"""


def wait_until(pred, timeout: float = 8.0, interval: float = 0.04) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return pred()


def parse_kv(path: Path, sep: str = "=") -> dict[str, str]:
    if not path.is_file():
        return {}
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if sep in line:
            k, v = line.split(sep, 1)
            out[k.strip()] = v.strip()
    return out


def parse_semi_kv(raw: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for part in raw.strip().split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def ipc_read_props(ipc_path: Path) -> dict[str, bytes]:
    props: dict[str, bytes] = {}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2.0)
        s.connect(str(ipc_path))
        buf = b""
        while b"\nREADY\n" not in buf and not buf.startswith(b"READY\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        for raw in buf.decode("utf-8", errors="replace").splitlines():
            if raw == "READY":
                break
            if raw.startswith("PROP "):
                parts = raw.split(" ", 2)
                if len(parts) == 3:
                    props[parts[1]] = bytes.fromhex(parts[2])
    return props


def parse_active_box(props: dict[str, bytes]) -> tuple[int, int, int, int, int, int, int, int] | None:
    raw = props.get("_DT_ACTIVE_WINDOW_BOX")
    if not raw or len(raw) < 32:
        return None
    return struct.unpack("<iiiiiiii", raw[:32])


def resolve_motif_prefix(comp_path: Path) -> Path:
    configured = os.environ.get("MOTIF_PREFIX")
    candidates = [Path(configured)] if configured else [
        *(parent / "motif-prefix" for parent in comp_path.parents),
        Path("/var/tmp/ck-wayland-build/wp00/motif-prefix"),
        Path("/usr/dt"),
    ]
    for prefix in candidates:
        if all((prefix / name).is_file() for name in ("include/Xm/Xm.h", "include/Xm/Wayland.h", "lib/libXm.so.9")):
            return prefix.resolve()
    raise SystemExit("Set MOTIF_PREFIX to an installed CK Core Motif ABI 9 prefix")


def read_ppm(ppm_bytes: bytes) -> tuple[int, int, bytes]:
    idx = 0
    tokens: list[bytes] = []
    n = len(ppm_bytes)
    while len(tokens) < 4 and idx < n:
        while idx < n and ppm_bytes[idx] in b" \t\r\n":
            idx += 1
        if idx < n and ppm_bytes[idx] == ord("#"):
            while idx < n and ppm_bytes[idx] != ord("\n"):
                idx += 1
            continue
        start = idx
        while idx < n and ppm_bytes[idx] not in b" \t\r\n#":
            idx += 1
        if idx > start:
            tokens.append(ppm_bytes[start:idx])
    if len(tokens) < 4 or tokens[0] != b"P6":
        raise ValueError("invalid PPM header")
    idx += 1  # single whitespace after maxval
    w = int(tokens[1])
    h = int(tokens[2])
    return w, h, ppm_bytes[idx: idx + w * h * 3]


def sample_rgb(rgb: bytes, w: int, x: int, y: int) -> tuple[int, int, int]:
    off = (y * w + x) * 3
    return rgb[off], rgb[off + 1], rgb[off + 2]


def vptr_cmd(proc: subprocess.Popen, line: str) -> None:
    assert proc.stdin is not None and proc.stdout is not None
    proc.stdin.write(line + "\n")
    proc.stdin.flush()
    if line.strip() == "quit":
        return
    ack = proc.stdout.readline().strip()
    if ack != "OK":
        raise RuntimeError(f"virtual pointer helper failed on {line!r}: {ack!r}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    comp_path = Path(args.compositor).resolve()
    if not comp_path.is_file():
        raise SystemExit(f"missing compositor: {comp_path}")
    motif_prefix = resolve_motif_prefix(comp_path)
    tests_dir = Path(__file__).resolve().parent

    cc = os.environ.get("CC", "cc")
    wl_scanner = shutil.which("wayland-scanner")
    grim = shutil.which("grim")
    if not wl_scanner or not grim:
        raise SystemExit("wayland-scanner and grim are required")

    tmp_parent = os.environ.get("TMPDIR")
    with tempfile.TemporaryDirectory(prefix="dtwm-proto-v2-", dir=tmp_parent) as td:
        tmp = Path(td)
        rt_dir = tmp / "rt"
        home_dir = tmp / "home"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt").mkdir(parents=True)

        proto_root = Path("/usr/share/wayland-protocols")
        protos = [
            ("xdg-shell", proto_root / "stable/xdg-shell/xdg-shell.xml"),
            ("xdg-dialog-v1", proto_root / "staging/xdg-dialog/xdg-dialog-v1.xml"),
            ("tearing-control-v1", proto_root / "staging/tearing-control/tearing-control-v1.xml"),
            ("fifo-v1", proto_root / "staging/fifo/fifo-v1.xml"),
            ("presentation-time", proto_root / "stable/presentation-time/presentation-time.xml"),
        ]
        gen_srcs: list[str] = []
        for tag, xml in protos:
            hdr = tmp / f"{tag}-client-protocol.h"
            src = tmp / f"{tag}-protocol.c"
            subprocess.run([wl_scanner, "client-header", str(xml), str(hdr)], check=True)
            subprocess.run([wl_scanner, "private-code", str(xml), str(src)], check=True)
            gen_srcs.append(str(src))

        wl_cflags = subprocess.check_output(["pkg-config", "--cflags", "wayland-client"], text=True).split()
        wl_libs = subprocess.check_output(["pkg-config", "--libs", "wayland-client"], text=True).split()

        proto_c = tmp / "proto_v2_client.c"
        proto_bin = tmp / "proto_v2_client"
        proto_c.write_text(PROTO_V2_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", "-I", str(tmp), str(proto_c), *gen_srcs, *wl_cflags, *wl_libs, "-o", str(proto_bin)],
            check=True,
        )

        vptr_bin = tmp / "vptr_helper"
        subprocess.run(
            [cc, "-O2", str(tests_dir / "wayland-test-pointer.c"), *wl_cflags, *wl_libs, "-o", str(vptr_bin)],
            check=True,
        )

        motif_c = tmp / "motif_modal_client.c"
        motif_bin = tmp / "motif_modal_client"
        motif_c.write_text(MOTIF_MODAL_CLIENT_C, encoding="utf-8")
        lib_dir = motif_prefix / "lib"
        subprocess.run(
            [
                cc, "-O2", f"-I{motif_prefix}/include", *wl_cflags, str(motif_c),
                f"-L{lib_dir}", f"-Wl,-rpath,{lib_dir}",
                "-lXm", "-lXt", "-lX11", *wl_libs, "-o", str(motif_bin),
            ],
            check=True,
        )

        status_file = tmp / "comp.status"
        env = os.environ.copy()
        env.update({
            "HOME": str(home_dir),
            "XDG_RUNTIME_DIR": str(rt_dir),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_RENDERER": "pixman",
        })
        for k in ("DISPLAY", "WAYLAND_DISPLAY"):
            env.pop(k, None)
        motif_env = {
            **env,
            "LD_LIBRARY_PATH": ":".join(filter(None, (str(lib_dir), env.get("LD_LIBRARY_PATH")))),
        }

        comp_proc = subprocess.Popen(
            [str(comp_path), "--no-xwayland", "--no-panel", "--status-file", str(status_file)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        vptr_proc: subprocess.Popen | None = None
        motif_proc: subprocess.Popen | None = None

        try:
            if not wait_until(lambda: "WAYLAND_DISPLAY" in parse_kv(status_file), timeout=10.0):
                raise RuntimeError("dtwm-wayland failed to write status file")
            wl_sock = parse_kv(status_file)["WAYLAND_DISPLAY"]
            env["WAYLAND_DISPLAY"] = wl_sock
            motif_env["WAYLAND_DISPLAY"] = wl_sock
            ipc_path = rt_dir / f"dtwm-wayland-{wl_sock}.ipc"
            if not wait_until(lambda: ipc_path.exists(), timeout=5.0):
                raise RuntimeError(f"missing IPC socket {ipc_path}")

            def get_pres_state() -> dict[str, str]:
                props = ipc_read_props(ipc_path)
                raw = props.get("_DT_WAYLAND_PRESENTATION_STATE", b"").decode("utf-8", errors="replace")
                return parse_semi_kv(raw)

            st0 = get_pres_state()
            assert st0.get("xdg_dialog") == "1", f"expected xdg_dialog=1, got {st0}"
            assert st0.get("tearing_mgr") == "1", f"expected tearing_mgr=1, got {st0}"
            assert st0.get("fifo_mgr") == "1", f"expected fifo_mgr=1, got {st0}"

            # 1. Verify wp_fifo_v1 protocol error guards
            subprocess.run([str(proto_bin), "err_dup_fifo"], env=env, check=True, timeout=5.0)
            subprocess.run([str(proto_bin), "err_destroyed_surf"], env=env, check=True, timeout=5.0)

            # 2. Verify wp_fifo_v1 multi-frame barrier pacing + wp_tearing_control_v1 ASYNC flip attempt
            subprocess.run([str(proto_bin), "normal"], env=env, check=True, timeout=8.0)
            st1 = get_pres_state()
            assert int(st1.get("fifo_barriers", "0")) >= 3, f"expected >=3 fifo_barriers, got {st1}"
            assert int(st1.get("fifo_waits", "0")) >= 2, f"expected >=2 fifo_waits, got {st1}"
            assert int(st1.get("tearing_flips", "0")) >= 1, f"expected >=1 tearing_flips, got {st1}"

            # 3. Verify Motif XmDialogShell modal centering, parent client dimming, & click lock
            vptr_proc = subprocess.Popen(
                [str(vptr_bin)],
                env=env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                text=True,
            )
            m_state = tmp / "motif.state"
            m_cmd = tmp / "motif.cmd"
            motif_proc = subprocess.Popen([str(motif_bin), str(m_state), str(m_cmd)], env=motif_env)
            if not wait_until(lambda: parse_kv(m_state).get("WAYLAND") == "1", timeout=8.0):
                raise RuntimeError(f"Motif client failed to map on Wayland: {parse_kv(m_state)}")

            def get_box():
                return parse_active_box(ipc_read_props(ipc_path))

            if not wait_until(lambda: get_box() is not None and get_box()[2] >= 500, timeout=6.0):
                raise AssertionError(f"parent window did not become active: {get_box()}")
            parent_box = get_box()
            assert parent_box is not None
            px, py, pow_, poh, pl, pt, pr, pb = parent_box
            # Sample top-left corner of parent client interior (exposed even when modal dialog is centered)
            sample_x = px + pl + 16
            sample_y = py + pt + 16

            ppm_before = subprocess.check_output([grim, "-t", "ppm", "-"], env=env, timeout=5.0)
            gw, gh, rgb_before = read_ppm(ppm_before)
            r0, g0, b0 = sample_rgb(rgb_before, gw, sample_x, sample_y)
            assert g0 >= 180 and r0 < 30 and b0 < 30, (
                f"expected bright green parent interior before modal open at ({sample_x},{sample_y}), got {(r0, g0, b0)}"
            )

            # Open full-application-modal Motif dialog
            m_cmd.write_text("open_modal\n", encoding="utf-8")
            if not wait_until(
                lambda: parse_kv(m_state).get("DIALOG_OPEN") == "1"
                and get_pres_state().get("modal_views") == "1",
                timeout=6.0,
            ):
                raise AssertionError(f"modal dialog did not register: {parse_kv(m_state)} / {get_pres_state()}")

            if not wait_until(lambda: get_box() is not None and get_box()[2] < pow_, timeout=5.0):
                raise AssertionError(f"active window did not move to modal dialog: {get_box()}")
            modal_box = get_box()
            assert modal_box is not None
            mx, my, mow, moh, *_ = modal_box
            parent_cx = px + pow_ / 2.0
            parent_cy = py + poh / 2.0
            modal_cx = mx + mow / 2.0
            modal_cy = my + moh / 2.0
            assert abs(modal_cx - parent_cx) <= 8.0 and abs(modal_cy - parent_cy) <= 8.0, (
                f"expected modal dialog centered over parent ({parent_cx},{parent_cy}), got ({modal_cx},{modal_cy})"
            )

            # Verify parent's exposed green client corner is dimmed by modal_dim_rect (~0.68x brightness)
            ppm_dim = subprocess.check_output([grim, "-t", "ppm", "-"], env=env, timeout=5.0)
            _, _, rgb_dim = read_ppm(ppm_dim)
            r1, g1, b1 = sample_rgb(rgb_dim, gw, sample_x, sample_y)
            assert 95 <= g1 <= 165 and r1 < 25 and b1 < 25, (
                f"expected dimmed green parent interior while modal open, got {(r1, g1, b1)} vs before {(r0, g0, b0)}"
            )

            # Click directly on the locked parent's exposed client corner: parent must receive 0 clicks
            # and active focus must remain on the modal dialog!
            vptr_cmd(vptr_proc, f"move {sample_x} {sample_y} {gw} {gh}")
            vptr_cmd(vptr_proc, "down 272")
            vptr_cmd(vptr_proc, "up 272")
            time.sleep(0.15)
            assert int(parse_kv(m_state).get("PARENT_CLICKS", "0")) == 0, (
                f"locked parent window received click while modal child was open: {parse_kv(m_state)}"
            )
            after_click_box = get_box()
            assert after_click_box is not None and after_click_box[:4] == modal_box[:4], (
                f"expected focus to stay on modal dialog {modal_box[:4]}, got {after_click_box}"
            )

            # Also test XmWaylandSetSurfaceTearingHint from libXm
            m_cmd.write_text("tearing_on\n", encoding="utf-8")
            if not wait_until(lambda: get_pres_state().get("tearing_async") == "1", timeout=5.0):
                raise AssertionError(f"XmWaylandSetSurfaceTearingHint(True) not reflected: {get_pres_state()}")
            m_cmd.write_text("tearing_off\n", encoding="utf-8")
            if not wait_until(lambda: get_pres_state().get("tearing_async") == "0", timeout=5.0):
                raise AssertionError(f"XmWaylandSetSurfaceTearingHint(False) not reflected: {get_pres_state()}")

            # Close modal dialog -> dim rect disappears and clicking parent delivers ButtonPress
            m_cmd.write_text("close_modal\n", encoding="utf-8")
            if not wait_until(lambda: get_pres_state().get("modal_views") == "0", timeout=5.0):
                raise AssertionError(f"modal_views did not return to 0: {get_pres_state()}")
            time.sleep(0.08)
            ppm_restored = subprocess.check_output([grim, "-t", "ppm", "-"], env=env, timeout=5.0)
            _, _, rgb_restored = read_ppm(ppm_restored)
            r2, g2, b2 = sample_rgb(rgb_restored, gw, sample_x, sample_y)
            assert g2 >= 180, f"expected restored parent green after modal close, got {(r2, g2, b2)}"

            vptr_cmd(vptr_proc, f"move {sample_x} {sample_y} {gw} {gh}")
            vptr_cmd(vptr_proc, "down 272")
            vptr_cmd(vptr_proc, "up 272")
            if not wait_until(lambda: int(parse_kv(m_state).get("PARENT_CLICKS", "0")) >= 1, timeout=5.0):
                raise AssertionError(f"unlocked parent did not receive click after modal close: {parse_kv(m_state)}")

            m_cmd.write_text("quit\n", encoding="utf-8")
            motif_proc.wait(timeout=5.0)
            motif_proc = None
            vptr_cmd(vptr_proc, "quit")
            vptr_proc.wait(timeout=3.0)
            vptr_proc = None
            print("PASS: check-dtwm-wayland-protocols-v2")
            return 0
        finally:
            for p in (motif_proc, vptr_proc, comp_proc):
                if p and p.poll() is None:
                    with suppress(Exception):
                        p.terminate()
                        p.wait(timeout=4.0)
                    if p.poll() is None:
                        with suppress(Exception):
                            p.kill()


if __name__ == "__main__":
    raise SystemExit(main())
