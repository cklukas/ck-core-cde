#!/usr/bin/env python3
"""Color Management (wp-color-management-v1) & Live Cross-Monitor Fractional-DPI Drag test:
1. Boots dtwm-wayland on a dual-monitor mixed-DPI layout (HEADLESS-1 @ 192 DPI, HEADLESS-2 @ 96 DPI).
2. Verifies wp_color_manager_v1 binding, advertised features/intents/transfer-functions/primaries,
   preferred surface image description feedback, parametric Display-P3 + HDR CLL/FALL surface image
   description attachment (_DT_WAYLAND_COLOR_STATE), and live f.set_output_color_profile all srgb.
3. Drags an external wp_fractional_scale_v1 + wp_viewporter Wayland window live across
   HEADLESS-1 (192 DPI) <-> HEADLESS-2 (96 DPI) while grabbing at ~75% across its titlebar,
   verifying proportional cursor-anchor retention and zero single-frame size jumps.
4. Drags a native CK Core Motif (CLIENT_CAP native_dpi) window live across HEADLESS-1 <-> HEADLESS-2,
   verifying bidirectional mid-drag XmGetWidgetScale (2.0 <-> 1.0) + physical shell reflow +
   proportional cursor-anchor retention.
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


EXT_COLOR_CLIENT_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "color-management-v1-client-protocol.h"

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int s) { (void)s; _exit(0); }

typedef struct {
    const char *state_path;
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wp_fractional_scale_manager_v1 *fs_mgr;
    struct wp_viewporter *vp_mgr;
    struct wp_color_manager_v1 *cm;

    struct wl_surface *surf;
    struct xdg_surface *xsurf;
    struct xdg_toplevel *xtop;
    struct wp_fractional_scale_v1 *fs_obj;
    struct wp_viewport *vp;
    struct wp_color_management_surface_v1 *cm_surf;
    struct wp_color_management_surface_feedback_v1 *cm_fb;
    struct wp_image_description_v1 *custom_desc;
    struct wp_image_description_v1 *pref_desc;

    bool cm_done;
    bool feat_parametric;
    bool feat_mdp;
    bool intent_perceptual;
    bool tf_srgb;
    bool tf_st2084;
    bool prim_srgb;
    bool prim_display_p3;
    bool prim_bt2020;
    bool custom_desc_ready;
    bool custom_desc_applied;
    bool pref_desc_ready;
    uint32_t fail_cause;
    char fail_msg[128];

    uint32_t frac_120;
    int32_t log_w, log_h;
    int32_t buf_w, buf_h;
    bool configured;
    bool needs_redraw;
    struct wl_buffer *cur_buf;
} App;

static void write_state(const App *a) {
    if (!a || !a->state_path) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", a->state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f,
            "CM_BOUND=%d\nCM_DONE=%d\nFEAT_PARAM=%d\nFEAT_MDP=%d\n"
            "INTENT_PERC=%d\nTF_SRGB=%d\nTF_PQ=%d\nPRIM_SRGB=%d\nPRIM_P3=%d\nPRIM_2020=%d\n"
            "PREF_READY=%d\nDESC_APPLIED=%d\nFAIL=%u:%s\n"
            "FRAC_120=%u\nLOG_W=%d\nLOG_H=%d\nBUF_W=%d\nBUF_H=%d\n",
            a->cm ? 1 : 0, a->cm_done ? 1 : 0,
            a->feat_parametric ? 1 : 0, a->feat_mdp ? 1 : 0,
            a->intent_perceptual ? 1 : 0, a->tf_srgb ? 1 : 0, a->tf_st2084 ? 1 : 0,
            a->prim_srgb ? 1 : 0, a->prim_display_p3 ? 1 : 0, a->prim_bt2020 ? 1 : 0,
            a->pref_desc_ready ? 1 : 0, a->custom_desc_applied ? 1 : 0,
            a->fail_cause, a->fail_msg,
            a->frac_120, a->log_w, a->log_h, a->buf_w, a->buf_h);
    fclose(f);
    rename(tmp, a->state_path);
}

static struct wl_buffer *make_shm_buffer(struct wl_shm *shm, int w, int h, uint32_t argb) {
    int stride = w * 4;
    size_t sz = (size_t)stride * (size_t)h;
    int fd = memfd_create("ext-cm-shm", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return NULL; }
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { close(fd); return NULL; }
    for (size_t i = 0; i < sz / 4; ++i) px[i] = argb;
    munmap(px, sz);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)sz);
    struct wl_buffer *b = wl_shm_pool_create_buffer(
        pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

static void redraw(App *a) {
    double s = a->frac_120 > 0 ? (double)a->frac_120 / 120.0 : 1.0;
    int lw = a->log_w > 0 ? a->log_w : 360;
    int lh = a->log_h > 0 ? a->log_h : 240;
    int bw = (int)lround((double)lw * s);
    int bh = (int)lround((double)lh * s);
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    struct wl_buffer *nb = make_shm_buffer(a->shm, bw, bh, 0xff2266aaU);
    if (!nb) return;
    if (a->cur_buf) wl_buffer_destroy(a->cur_buf);
    a->cur_buf = nb;
    a->buf_w = bw;
    a->buf_h = bh;
    if (a->vp) wp_viewport_set_destination(a->vp, lw, lh);
    if (a->custom_desc_ready && !a->custom_desc_applied && a->cm_surf && a->custom_desc) {
        wp_color_management_surface_v1_set_image_description(
            a->cm_surf, a->custom_desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        a->custom_desc_applied = true;
    }
    wl_surface_attach(a->surf, nb, 0, 0);
    wl_surface_damage_buffer(a->surf, 0, 0, bw, bh);
    wl_surface_commit(a->surf);
    wl_display_flush(a->dpy);
    a->needs_redraw = false;
    write_state(a);
}

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t ser) {
    (void)d;
    xdg_wm_base_pong(wm, ser);
}
static const struct xdg_wm_base_listener wm_l = { .ping = wm_ping };

static void xtop_cfg(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)t; (void)st;
    App *a = d;
    if (w > 0 && h > 0) {
        a->log_w = w;
        a->log_h = h;
    }
}
static void xtop_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; g_quit = 1; }
static void xtop_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void xtop_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener xtop_l = {
    .configure = xtop_cfg, .close = xtop_close, .configure_bounds = xtop_bounds, .wm_capabilities = xtop_caps,
};

static const struct wp_image_description_v1_listener pref_img_l;

static void xsurf_cfg(void *d, struct xdg_surface *xs, uint32_t ser) {
    App *a = d;
    xdg_surface_ack_configure(xs, ser);
    a->configured = true;
    if (!a->pref_desc && a->cm_fb) {
        a->pref_desc = wp_color_management_surface_feedback_v1_get_preferred(a->cm_fb);
        wp_image_description_v1_add_listener(a->pref_desc, &pref_img_l, a);
    }
    redraw(a);
}
static const struct xdg_surface_listener xsurf_l = { .configure = xsurf_cfg };

static void fs_pref(void *d, struct wp_fractional_scale_v1 *fs, uint32_t s120) {
    (void)fs;
    App *a = d;
    if (s120 > 0 && a->frac_120 != s120) {
        a->frac_120 = s120;
        if (a->configured) redraw(a);
        else write_state(a);
    }
}
static const struct wp_fractional_scale_v1_listener fs_l = { .preferred_scale = fs_pref };

static void cm_intent(void *d, struct wp_color_manager_v1 *cm, uint32_t v) {
    (void)cm;
    App *a = d;
    if (v == WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) a->intent_perceptual = true;
}
static void cm_feat(void *d, struct wp_color_manager_v1 *cm, uint32_t v) {
    (void)cm;
    App *a = d;
    if (v == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) a->feat_parametric = true;
    if (v == WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES) a->feat_mdp = true;
}
static void cm_tf(void *d, struct wp_color_manager_v1 *cm, uint32_t v) {
    (void)cm;
    App *a = d;
    if (v == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB) a->tf_srgb = true;
    if (v == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) a->tf_st2084 = true;
}
static void cm_prim(void *d, struct wp_color_manager_v1 *cm, uint32_t v) {
    (void)cm;
    App *a = d;
    if (v == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB) a->prim_srgb = true;
    if (v == WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3) a->prim_display_p3 = true;
    if (v == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) a->prim_bt2020 = true;
}
static void cm_done_ev(void *d, struct wp_color_manager_v1 *cm) {
    (void)cm;
    App *a = d;
    a->cm_done = true;
    write_state(a);
}
static const struct wp_color_manager_v1_listener cm_l = {
    .supported_intent = cm_intent,
    .supported_feature = cm_feat,
    .supported_tf_named = cm_tf,
    .supported_primaries_named = cm_prim,
    .done = cm_done_ev,
};

static void img_failed(void *d, struct wp_image_description_v1 *id, uint32_t c, const char *m) {
    (void)id;
    App *a = d;
    a->fail_cause = c;
    snprintf(a->fail_msg, sizeof(a->fail_msg), "%s", m ? m : "");
    write_state(a);
}
static void custom_img_ready(void *d, struct wp_image_description_v1 *id, uint32_t ident) {
    (void)id; (void)ident;
    App *a = d;
    a->custom_desc_ready = true;
    if (a->configured) redraw(a);
    else write_state(a);
}
static void custom_img_ready2(void *d, struct wp_image_description_v1 *id, uint32_t hi, uint32_t lo) {
    (void)hi;
    custom_img_ready(d, id, lo);
}
static const struct wp_image_description_v1_listener custom_img_l = {
    .failed = img_failed, .ready = custom_img_ready, .ready2 = custom_img_ready2,
};

static void pref_img_ready(void *d, struct wp_image_description_v1 *id, uint32_t ident) {
    (void)id; (void)ident;
    App *a = d;
    a->pref_desc_ready = true;
    write_state(a);
}
static void pref_img_ready2(void *d, struct wp_image_description_v1 *id, uint32_t hi, uint32_t lo) {
    (void)hi;
    pref_img_ready(d, id, lo);
}
static const struct wp_image_description_v1_listener pref_img_l = {
    .failed = img_failed, .ready = pref_img_ready, .ready2 = pref_img_ready2,
};

static void fb_pref_changed(void *d, struct wp_color_management_surface_feedback_v1 *fb, uint32_t ident) {
    (void)ident;
    App *a = d;
    if (!a->pref_desc) {
        a->pref_desc = wp_color_management_surface_feedback_v1_get_preferred(fb);
        wp_image_description_v1_add_listener(a->pref_desc, &pref_img_l, a);
    }
}
static void fb_pref_changed2(void *d, struct wp_color_management_surface_feedback_v1 *fb, uint32_t hi, uint32_t lo) {
    (void)hi;
    fb_pref_changed(d, fb, lo);
}
static const struct wp_color_management_surface_feedback_v1_listener fb_l = {
    .preferred_changed = fb_pref_changed,
    .preferred_changed2 = fb_pref_changed2,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    App *a = d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        a->comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 6 ? ver : 6);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        a->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
        xdg_wm_base_add_listener(a->wm, &wm_l, a);
    } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
        a->fs_mgr = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        a->vp_mgr = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(iface, wp_color_manager_v1_interface.name)) {
        a->cm = wl_registry_bind(r, name, &wp_color_manager_v1_interface, 1);
        wp_color_manager_v1_add_listener(a->cm, &cm_l, a);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { .global = reg_global, .global_remove = reg_remove };

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct sigaction sa = {0};
    sa.sa_handler = on_sig;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    App a = {0};
    a.state_path = argv[1];
    a.log_w = 360;
    a.log_h = 240;
    a.frac_120 = 120;

    a.dpy = wl_display_connect(NULL);
    if (!a.dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(a.dpy);
    wl_registry_add_listener(reg, &reg_l, &a);
    wl_display_roundtrip(a.dpy);
    wl_display_roundtrip(a.dpy);

    if (!a.comp || !a.shm || !a.wm || !a.fs_mgr || !a.vp_mgr || !a.cm) {
        write_state(&a);
        return 3;
    }

    a.surf = wl_compositor_create_surface(a.comp);
    a.vp = wp_viewporter_get_viewport(a.vp_mgr, a.surf);
    a.fs_obj = wp_fractional_scale_manager_v1_get_fractional_scale(a.fs_mgr, a.surf);
    wp_fractional_scale_v1_add_listener(a.fs_obj, &fs_l, &a);

    a.cm_surf = wp_color_manager_v1_get_surface(a.cm, a.surf);
    a.cm_fb = wp_color_manager_v1_get_surface_feedback(a.cm, a.surf);
    wp_color_management_surface_feedback_v1_add_listener(a.cm_fb, &fb_l, &a);

    struct wp_image_description_creator_params_v1 *params =
        wp_color_manager_v1_create_parametric_creator(a.cm);
    wp_image_description_creator_params_v1_set_tf_named(
        params, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
    wp_image_description_creator_params_v1_set_primaries_named(
        params, WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3);
    wp_image_description_creator_params_v1_set_max_cll(params, 500);
    wp_image_description_creator_params_v1_set_max_fall(params, 200);
    a.custom_desc = wp_image_description_creator_params_v1_create(params);
    wp_image_description_v1_add_listener(a.custom_desc, &custom_img_l, &a);

    a.xsurf = xdg_wm_base_get_xdg_surface(a.wm, a.surf);
    xdg_surface_add_listener(a.xsurf, &xsurf_l, &a);
    a.xtop = xdg_surface_get_toplevel(a.xsurf);
    xdg_toplevel_add_listener(a.xtop, &xtop_l, &a);
    xdg_toplevel_set_app_id(a.xtop, "org.cde.color-drag-ext");
    xdg_toplevel_set_title(a.xtop, "Color + Mixed-DPI External");
    wl_surface_commit(a.surf);
    wl_display_flush(a.dpy);

    while (!g_quit) {
        wl_display_flush(a.dpy);
        if (wl_display_dispatch(a.dpy) == -1) break;
        if (a.needs_redraw && a.configured) redraw(&a);
    }
    return 0;
}
"""


MOTIF_DRAG_CLIENT_C = r"""
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/PushB.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int s) { (void)s; _exit(0); }

typedef struct {
    const char *path;
    Widget shell;
    XtAppContext app;
} StateCtx;

static void write_state(const StateCtx *ctx) {
    double scale = 0.0;
    XmGetWidgetScale(ctx->shell, &scale);
    Dimension w = 0, h = 0;
    XtVaGetValues(ctx->shell, XmNwidth, &w, XmNheight, &h, NULL);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", ctx->path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "SCALE=%.3f\nWIDTH=%u\nHEIGHT=%u\n", scale, (unsigned)w, (unsigned)h);
        fclose(f);
        rename(tmp, ctx->path);
    }
}

static void poll_timer(XtPointer cd, XtIntervalId *id) {
    (void)id;
    StateCtx *ctx = (StateCtx *)cd;
    write_state(ctx);
    if (!g_quit) {
        XtAppAddTimeOut(ctx->app, 25, poll_timer, ctx);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *state_path = argv[1];
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    setenv("CK_MOTIF_BACKEND", "wayland", 1);

    static String fallbacks[] = {
        "*appearanceProfile: modern_light",
        NULL
    };
    XtAppContext app = NULL;
    Widget shell = XtVaAppInitialize(&app, "DtwmMixedDpiMotif", NULL, 0, &argc, argv, fallbacks,
                                     XmNtitle, "Motif Mixed-DPI Native",
                                     XmNallowShellResize, True,
                                     NULL);
    XmString label = XmStringCreateLocalized("Mixed-DPI Native Motif Reflow Verification");
    XtVaCreateManagedWidget("btn", xmPushButtonWidgetClass, shell,
                            XmNlabelString, label,
                            NULL);
    XmStringFree(label);

    XtRealizeWidget(shell);
    StateCtx ctx = { .path = state_path, .shell = shell, .app = app };
    XtAppAddTimeOut(app, 20, poll_timer, &ctx);
    while (!g_quit) {
        XtAppProcessEvent(app, XtIMAll);
    }
    return 0;
}
"""


def parse_kv(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def wait_until(pred, timeout: float = 8.0, step: float = 0.03) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(step)
    return pred()


def ipc_read_props_and_send(ipc_path: Path, cmd: str | None = None) -> dict[str, bytes]:
    props: dict[str, bytes] = {}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(3.0)
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
        if cmd is not None:
            payload = cmd.encode("utf-8") + b"\x00"
            s.sendall(f"SET _DT_WM_REQUEST {payload.hex()}\n".encode("ascii"))
            time.sleep(0.05)
    return props


def parse_full_box(props: dict[str, bytes]) -> tuple[int, int, int, int, int, int, int, int] | None:
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
        if all((prefix / name).is_file() for name in ("include/Xm/Xm.h", "lib/libXm.so", "lib/libXm.so.9")):
            return prefix.resolve()
    raise SystemExit("Set MOTIF_PREFIX to an installed CK Core Motif ABI 9 prefix")


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
    if not wl_scanner:
        raise SystemExit("wayland-scanner is required")

    tmp_parent = os.environ.get("TMPDIR")
    with tempfile.TemporaryDirectory(prefix="dtwm-color-multidpi-", dir=tmp_parent) as td:
        tmp = Path(td)
        rt_dir = tmp / "rt"
        home_dir = tmp / "home"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt").mkdir(parents=True)

        proto_root = Path("/usr/share/wayland-protocols")
        protos = [
            ("xdg-shell", proto_root / "stable/xdg-shell/xdg-shell.xml"),
            ("fractional-scale-v1", proto_root / "staging/fractional-scale/fractional-scale-v1.xml"),
            ("viewporter", proto_root / "stable/viewporter/viewporter.xml"),
            ("color-management-v1", proto_root / "staging/color-management/color-management-v1.xml"),
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

        ext_c = tmp / "ext_color_client.c"
        ext_bin = tmp / "ext_color_client"
        ext_c.write_text(EXT_COLOR_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", "-I", str(tmp), str(ext_c), *gen_srcs, *wl_cflags, *wl_libs, "-lm", "-o", str(ext_bin)],
            check=True,
        )

        vptr_bin = tmp / "vptr_helper"
        subprocess.run(
            [cc, "-O2", str(tests_dir / "wayland-test-pointer.c"), *wl_cflags, *wl_libs, "-o", str(vptr_bin)],
            check=True,
        )

        motif_c = tmp / "motif_drag_client.c"
        motif_bin = tmp / "motif_drag_client"
        motif_c.write_text(MOTIF_DRAG_CLIENT_C, encoding="utf-8")
        lib_dir = motif_prefix / "lib"
        subprocess.run(
            [
                cc, "-O2", f"-I{motif_prefix}/include", str(motif_c),
                f"-L{lib_dir}", f"-Wl,-rpath,{lib_dir}",
                "-lXm", "-lXt", "-lX11", "-o", str(motif_bin),
            ],
            check=True,
        )

        status_file = tmp / "comp.status"
        env = os.environ.copy()
        env.update({
            "HOME": str(home_dir),
            "XDG_RUNTIME_DIR": str(rt_dir),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "2",
            "WLR_RENDERER": "pixman",
        })
        for k in ("DISPLAY", "WAYLAND_DISPLAY", "DTWM_ICC_PROFILE"):
            env.pop(k, None)
        motif_env = {
            **env,
            "LD_LIBRARY_PATH": ":".join(filter(None, (str(lib_dir), env.get("LD_LIBRARY_PATH")))),
        }

        comp_proc = subprocess.Popen(
            [
                str(comp_path),
                "--no-xwayland",
                "--no-panel",
                "--output-dpi", "0=192",
                "--output-dpi", "1=96",
                "--status-file", str(status_file),
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        vptr_proc: subprocess.Popen | None = None
        ext_proc: subprocess.Popen | None = None
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

            def get_outputs() -> list[tuple[int, str, int, int, int, int, int]]:
                p = ipc_read_props_and_send(ipc_path)
                lines = p.get("_DT_WAYLAND_OUTPUTS", b"").decode("utf-8", errors="replace").strip().splitlines()
                res = []
                for ln in lines:
                    f = ln.split("|")
                    if len(f) >= 9:
                        res.append((int(f[0]), f[1], int(f[3]), int(f[4]), int(f[5]), int(f[6]), int(f[8])))
                return res

            if not wait_until(lambda: len(get_outputs()) == 2, timeout=6.0):
                raise AssertionError(f"expected 2 headless outputs, got {get_outputs()}")
            outs = get_outputs()
            out0, out1 = outs[0], outs[1]
            if out0[6] != 192 or out1[6] != 96 or out1[2] != out0[4]:
                raise AssertionError(f"unexpected mixed-DPI layout: {outs}")
            bound_x = out1[2]
            total_w = out0[4] + out1[4]
            total_h = max(out0[5], out1[5])

            vptr_proc = subprocess.Popen(
                [str(vptr_bin)],
                env=env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                text=True,
            )
            # Place cursor on Output 0 (192 DPI) before spawning clients
            vptr_cmd(vptr_proc, f"move 300 260 {total_w} {total_h}")

            # --- Phase 1: wp-color-management-v1 & External Mixed-DPI Live Drag ---
            ext_state = tmp / "ext.state"
            ext_proc = subprocess.Popen([str(ext_bin), str(ext_state)], env=env)
            if not wait_until(
                lambda: (
                    parse_kv(ext_state).get("CM_DONE") == "1"
                    and parse_kv(ext_state).get("DESC_APPLIED") == "1"
                    and parse_kv(ext_state).get("PREF_READY") == "1"
                    and parse_kv(ext_state).get("FRAC_120") == "240"
                    and parse_kv(ext_state).get("BUF_W") == "720"
                ),
                timeout=8.0,
            ):
                raise AssertionError(f"external color/HiDPI client init failed: {parse_kv(ext_state)}")

            es = parse_kv(ext_state)
            for req_key in ("FEAT_PARAM", "FEAT_MDP", "INTENT_PERC", "TF_SRGB", "TF_PQ", "PRIM_SRGB", "PRIM_P3", "PRIM_2020"):
                if es.get(req_key) != "1":
                    raise AssertionError(f"wp_color_manager_v1 missing capability {req_key}: {es}")

            def color_prop() -> str:
                return ipc_read_props_and_send(ipc_path).get("_DT_WAYLAND_COLOR_STATE", b"").decode("utf-8", errors="replace")

            if not wait_until(lambda: "tf=9;primaries=9;" in color_prop() and "max_cll=500;max_fall=200;" in color_prop(), timeout=5.0):
                raise AssertionError(f"compositor did not record Display-P3/HDR surface image description: {color_prop()!r}")

            ipc_read_props_and_send(ipc_path, "f.set_output_color_profile all srgb")
            if not wait_until(lambda: "output_transforms=2;profile=srgb" in color_prop(), timeout=5.0):
                raise AssertionError(f"f.set_output_color_profile all srgb not applied: {color_prop()!r}")

            # Verify initial physical geometry on 192 DPI output: client 720x480
            if not wait_until(lambda: (parse_full_box(ipc_read_props_and_send(ipc_path)) or (0,) * 8)[6:] == (720, 480), timeout=5.0):
                raise AssertionError(f"expected initial external client_w/h (720, 480), got {parse_full_box(ipc_read_props_and_send(ipc_path))}")
            box0 = parse_full_box(ipc_read_props_and_send(ipc_path))
            assert box0 is not None
            ox0, oy0, ow0, oh0, cx0, cy0, cw0, ch0 = box0
            top_in0 = cy0 - oy0
            # Grab at 75% horizontally across the titlebar and vertically centered in titlebar!
            grab_x0 = ox0 + int(round(ow0 * 0.75))
            grab_y0 = oy0 + max(8, top_in0 // 2)
            orig_ratio = (grab_x0 - ox0) / float(ow0)

            vptr_cmd(vptr_proc, f"move {grab_x0} {grab_y0} {total_w} {total_h}")
            vptr_cmd(vptr_proc, "down 272")

            # Drag step-by-step across boundary bound_x (Output 0 @ 192 DPI -> Output 1 @ 96 DPI)
            ext_waypoints = [
                grab_x0 + 60,
                bound_x - 30,
                bound_x - 4,
                bound_x + 8,
                bound_x + 80,
                bound_x + 220,
            ]
            for wx in ext_waypoints:
                vptr_cmd(vptr_proc, f"move {wx} {grab_y0} {total_w} {total_h}")
                b = parse_full_box(ipc_read_props_and_send(ipc_path))
                assert b is not None
                ox, oy, ow, oh, cx, cy, cw, ch = b
                if wx < bound_x:
                    if (cw, ch) != (720, 480):
                        raise AssertionError(f"premature external size change at x={wx} (<{bound_x}): {(cw, ch)}")
                else:
                    if (cw, ch) != (360, 240):
                        raise AssertionError(f"single-frame or logical size jump on Output 1 at x={wx}: got {(cw, ch)}, expected (360, 240)")
                cur_ratio = (wx - ox) / float(ow)
                if abs(cur_ratio - orig_ratio) > 0.035:
                    raise AssertionError(
                        f"cursor anchor drifted at x={wx}: ratio={cur_ratio:.3f} vs {orig_ratio:.3f} (box={b})"
                    )
                if not (oy <= grab_y0 <= cy):
                    raise AssertionError(f"cursor Y={grab_y0} fell outside titlebar [{oy}..{cy}] at x={wx}")

            if not wait_until(
                lambda: parse_kv(ext_state).get("FRAC_120") == "120" and parse_kv(ext_state).get("BUF_W") == "360",
                timeout=5.0,
            ):
                raise AssertionError(f"external client did not re-render 1.0x buffer on Output 1: {parse_kv(ext_state)}")

            # Drag back onto Output 0 (192 DPI) while still holding Button1!
            for wx in (bound_x + 12, bound_x - 12, bound_x - 180):
                vptr_cmd(vptr_proc, f"move {wx} {grab_y0} {total_w} {total_h}")
                b = parse_full_box(ipc_read_props_and_send(ipc_path))
                assert b is not None
                ox, oy, ow, oh, cx, cy, cw, ch = b
                expected_sz = (360, 240) if wx >= bound_x else (720, 480)
                if (cw, ch) != expected_sz:
                    raise AssertionError(f"return drag size jump at x={wx}: got {(cw, ch)}, expected {expected_sz}")
                cur_ratio = (wx - ox) / float(ow)
                if abs(cur_ratio - orig_ratio) > 0.035:
                    raise AssertionError(f"return drag cursor anchor drifted at x={wx}: {cur_ratio:.3f} vs {orig_ratio:.3f}")

            vptr_cmd(vptr_proc, "up 272")
            if not wait_until(
                lambda: parse_kv(ext_state).get("FRAC_120") == "240" and parse_kv(ext_state).get("BUF_W") == "720",
                timeout=5.0,
            ):
                raise AssertionError(f"external client did not restore 2.0x buffer on Output 0: {parse_kv(ext_state)}")

            ext_proc.terminate()
            ext_proc.wait(timeout=5)
            ext_proc = None

            # --- Phase 2: Native-DPI CK Core Motif Cross-Monitor Live Drag (192 <-> 96 DPI) ---
            vptr_cmd(vptr_proc, f"move 320 260 {total_w} {total_h}")
            motif_state = tmp / "motif.state"
            motif_proc = subprocess.Popen([str(motif_bin), str(motif_state)], env=motif_env)
            if not wait_until(lambda: abs(float(parse_kv(motif_state).get("SCALE", "0")) - 2.0) < 0.02, timeout=8.0):
                raise AssertionError(f"Motif client did not start at 2.0 scale on Output 0: {parse_kv(motif_state)}")

            ms_2x = parse_kv(motif_state)
            mw_2x, mh_2x = int(ms_2x["WIDTH"]), int(ms_2x["HEIGHT"])
            if not wait_until(lambda: (parse_full_box(ipc_read_props_and_send(ipc_path)) or (0,) * 8)[6:] == (mw_2x, mh_2x), timeout=5.0):
                raise AssertionError(f"Motif active box did not match {mw_2x}x{mh_2x}: {parse_full_box(ipc_read_props_and_send(ipc_path))}")

            mbox0 = parse_full_box(ipc_read_props_and_send(ipc_path))
            assert mbox0 is not None
            mox0, moy0, mow0, moh0, mcx0, mcy0, mcw0, mch0 = mbox0
            mtop_in0 = mcy0 - moy0
            mgrab_x0 = mox0 + int(round(mow0 * 0.75))
            mgrab_y0 = moy0 + max(8, mtop_in0 // 2)
            morig_ratio = (mgrab_x0 - mox0) / float(mow0)

            vptr_cmd(vptr_proc, f"move {mgrab_x0} {mgrab_y0} {total_w} {total_h}")
            vptr_cmd(vptr_proc, "down 272")
            vptr_cmd(vptr_proc, f"move {bound_x - 20} {mgrab_y0} {total_w} {total_h}")
            # Before cursor crosses bound_x, Motif window is straddling Output 1 on its right side,
            # yet cursor is still on Output 0 -> MUST stay 2.0x without straddle bounce!
            time.sleep(0.08)
            if abs(float(parse_kv(motif_state).get("SCALE", "0")) - 2.0) > 0.02:
                raise AssertionError(f"Motif client prematurely jumped scale before cursor crossed boundary: {parse_kv(motif_state)}")

            # Cross cursor onto Output 1 (96 DPI) while still holding Button1!
            target_x1 = bound_x + 180
            vptr_cmd(vptr_proc, f"move {target_x1} {mgrab_y0} {total_w} {total_h}")
            if not wait_until(lambda: abs(float(parse_kv(motif_state).get("SCALE", "0")) - 1.0) < 0.02, timeout=6.0):
                raise AssertionError(f"Motif client did not live-reflow to 1.0x mid-drag on Output 1: {parse_kv(motif_state)}")

            ms_1x = parse_kv(motif_state)
            mw_1x, mh_1x = int(ms_1x["WIDTH"]), int(ms_1x["HEIGHT"])
            if not (0.42 * mw_2x <= mw_1x <= 0.62 * mw_2x):
                raise AssertionError(f"expected ~0.5x Motif width reflow from {mw_2x} -> {mw_1x}")
            if not wait_until(lambda: (parse_full_box(ipc_read_props_and_send(ipc_path)) or (0,) * 8)[6:] == (mw_1x, mh_1x), timeout=5.0):
                raise AssertionError(f"compositor did not adopt rescaled Motif box {mw_1x}x{mh_1x}")

            mbox1 = parse_full_box(ipc_read_props_and_send(ipc_path))
            assert mbox1 is not None
            mcur_ratio1 = (target_x1 - mbox1[0]) / float(mbox1[2])
            if abs(mcur_ratio1 - morig_ratio) > 0.04:
                raise AssertionError(f"Motif mid-drag cursor anchor drifted on Output 1: {mcur_ratio1:.3f} vs {morig_ratio:.3f} ({mbox1})")
            if not (mbox1[1] <= mgrab_y0 <= mbox1[5]):
                raise AssertionError(f"cursor Y={mgrab_y0} fell outside rescaled Motif titlebar [{mbox1[1]}..{mbox1[5]}]")

            # Drag Motif window back onto Output 0 (192 DPI) while still holding Button1!
            target_x0 = bound_x - 200
            vptr_cmd(vptr_proc, f"move {target_x0} {mgrab_y0} {total_w} {total_h}")
            if not wait_until(lambda: abs(float(parse_kv(motif_state).get("SCALE", "0")) - 2.0) < 0.02, timeout=6.0):
                raise AssertionError(f"Motif client did not live-restore 2.0x mid-drag on Output 0: {parse_kv(motif_state)}")
            if not wait_until(lambda: (parse_full_box(ipc_read_props_and_send(ipc_path)) or (0,) * 8)[6:] == (mw_2x, mh_2x), timeout=5.0):
                raise AssertionError(f"compositor did not restore 2x Motif box {mw_2x}x{mh_2x}")

            mbox2 = parse_full_box(ipc_read_props_and_send(ipc_path))
            assert mbox2 is not None
            mcur_ratio2 = (target_x0 - mbox2[0]) / float(mbox2[2])
            if abs(mcur_ratio2 - morig_ratio) > 0.04:
                raise AssertionError(f"Motif return-drag cursor anchor drifted on Output 0: {mcur_ratio2:.3f} vs {morig_ratio:.3f}")

            vptr_cmd(vptr_proc, "up 272")
            vptr_cmd(vptr_proc, "quit")

            print(
                f"PASS check-dtwm-wayland-color-multidpi "
                f"(cm=P3+PQ+srgb_lut ext=720x480<->360x240[0-jump] motif={mw_2x}x{mh_2x}<->{mw_1x}x{mh_1x} ratio={morig_ratio:.2f})"
            )
            return 0
        finally:
            for p in (motif_proc, ext_proc, vptr_proc, comp_proc):
                if p is not None and p.poll() is None:
                    with suppress(Exception):
                        p.terminate()
                        p.wait(timeout=4)
                    if p.poll() is None:
                        with suppress(Exception):
                            p.kill()


if __name__ == "__main__":
    raise SystemExit(main())
