#!/usr/bin/env python3
"""Automated integration test for Topic 1.2:
GUI Color Profile Management (dtstyle -monitors) & Mixed SDR + HDR10 (BT.2020 PQ)
Per-Surface Tone Mapping in dtwm-wayland (across both GLES2 shader and Pixman LUT).
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


HDR_SDR_CLIENT_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
#include "color-management-v1-client-protocol.h"

static void on_sig(int s) { (void)s; _exit(0); }

typedef struct {
    const char *mode;
    const char *state_path;
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wp_color_manager_v1 *cm;
    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_toplevel *xt;
    struct wp_color_management_surface_v1 *cm_surf;
    struct wp_color_management_surface_feedback_v1 *cm_fb;
    struct wp_image_description_v1 *desc;
    struct wp_image_description_v1 *pref_desc;
    struct wp_image_description_info_v1 *pref_info;
    uint32_t pref_tf;
    uint32_t pref_prim;
    bool desc_ready;
    bool configured;
    bool needs_draw;
    int32_t w, h;
} App;

static void write_state(const App *a) {
    if (!a || !a->state_path) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", a->state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f,
            "CONFIGURED=%d\nDESC_READY=%d\nPREF_TF=%u\nPREF_PRIM=%u\n"
            "PREF_IS_P3=%d\nPREF_IS_HDR10=%d\nW=%d\nH=%d\n",
            a->configured ? 1 : 0, a->desc_ready ? 1 : 0,
            a->pref_tf, a->pref_prim,
            (a->pref_prim == WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3) ? 1 : 0,
            (a->pref_prim == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 &&
             a->pref_tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) ? 1 : 0,
            a->w, a->h);
    fclose(f);
    rename(tmp, a->state_path);
}

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener g_wm_l = { .ping = wm_ping };

static void info_done(void *d, struct wp_image_description_info_v1 *inf) {
    App *a = d;
    wp_image_description_info_v1_destroy(inf);
    a->pref_info = NULL;
    write_state(a);
}
static void info_icc(void *d, struct wp_image_description_info_v1 *i, int32_t fd, uint32_t sz) {
    (void)d; (void)i; (void)sz; if (fd >= 0) close(fd);
}
static void info_prim(void *d, struct wp_image_description_info_v1 *i,
                      int32_t rx, int32_t ry, int32_t gx, int32_t gy,
                      int32_t bx, int32_t by, int32_t wx, int32_t wy) {
    (void)d; (void)i; (void)rx; (void)ry; (void)gx; (void)gy; (void)bx; (void)by; (void)wx; (void)wy;
}
static void info_prim_named(void *d, struct wp_image_description_info_v1 *i, uint32_t p) {
    (void)i; App *a = d; a->pref_prim = p;
}
static void info_tf_pow(void *d, struct wp_image_description_info_v1 *i, uint32_t e) { (void)d; (void)i; (void)e; }
static void info_tf_named(void *d, struct wp_image_description_info_v1 *i, uint32_t tf) {
    (void)i; App *a = d; a->pref_tf = tf;
}
static void info_lum(void *d, struct wp_image_description_info_v1 *i, uint32_t mn, uint32_t mx, uint32_t rf) {
    (void)d; (void)i; (void)mn; (void)mx; (void)rf;
}
static void info_tprim(void *d, struct wp_image_description_info_v1 *i,
                       int32_t rx, int32_t ry, int32_t gx, int32_t gy,
                       int32_t bx, int32_t by, int32_t wx, int32_t wy) {
    (void)d; (void)i; (void)rx; (void)ry; (void)gx; (void)gy; (void)bx; (void)by; (void)wx; (void)wy;
}
static void info_tlum(void *d, struct wp_image_description_info_v1 *i, uint32_t mn, uint32_t mx) {
    (void)d; (void)i; (void)mn; (void)mx;
}
static void info_cll(void *d, struct wp_image_description_info_v1 *i, uint32_t c) { (void)d; (void)i; (void)c; }
static void info_fall(void *d, struct wp_image_description_info_v1 *i, uint32_t f) { (void)d; (void)i; (void)f; }
static const struct wp_image_description_info_v1_listener g_info_l = {
    .done = info_done, .icc_file = info_icc, .primaries = info_prim,
    .primaries_named = info_prim_named, .tf_power = info_tf_pow, .tf_named = info_tf_named,
    .luminances = info_lum, .target_primaries = info_tprim, .target_luminance = info_tlum,
    .target_max_cll = info_cll, .target_max_fall = info_fall,
};

static void pref_ready(void *d, struct wp_image_description_v1 *desc, uint32_t id) {
    (void)id;
    App *a = d;
    if (a->pref_info) wp_image_description_info_v1_destroy(a->pref_info);
    a->pref_info = wp_image_description_v1_get_information(desc);
    wp_image_description_info_v1_add_listener(a->pref_info, &g_info_l, a);
    wl_display_flush(a->dpy);
}
static void pref_failed(void *d, struct wp_image_description_v1 *desc, uint32_t c, const char *m) {
    (void)d; (void)desc; (void)c; (void)m;
}
static const struct wp_image_description_v1_listener g_pref_l = {
    .failed = pref_failed, .ready = pref_ready
};

static void query_preferred(App *a) {
    if (!a->cm_fb) return;
    if (a->pref_desc) wp_image_description_v1_destroy(a->pref_desc);
    a->pref_desc = wp_color_management_surface_feedback_v1_get_preferred(a->cm_fb);
    wp_image_description_v1_add_listener(a->pref_desc, &g_pref_l, a);
    wl_display_flush(a->dpy);
}

static void fb_pref_changed(void *d, struct wp_color_management_surface_feedback_v1 *fb, uint32_t id) {
    (void)fb; (void)id;
    query_preferred((App *)d);
}
static const struct wp_color_management_surface_feedback_v1_listener g_fb_l = {
    .preferred_changed = fb_pref_changed
};

static void desc_ready_cb(void *d, struct wp_image_description_v1 *desc, uint32_t id) {
    (void)desc; (void)id;
    App *a = d;
    a->desc_ready = true;
}
static void desc_failed_cb(void *d, struct wp_image_description_v1 *desc, uint32_t c, const char *m) {
    (void)d; (void)desc; (void)c; (void)m;
}
static const struct wp_image_description_v1_listener g_desc_l = {
    .failed = desc_failed_cb, .ready = desc_ready_cb
};

static void top_cfg(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {
    (void)t; (void)s;
    App *a = d;
    if (w > 0) a->w = w;
    if (h > 0) a->h = h;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; _exit(0); }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener g_top_l = {
    .configure = top_cfg, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps
};

static void xs_cfg(void *d, struct xdg_surface *xs, uint32_t serial) {
    App *a = d;
    xdg_surface_ack_configure(xs, serial);
    a->configured = true;
    a->needs_draw = true;
}
static const struct xdg_surface_listener g_xs_l = { .configure = xs_cfg };

static void cm_feat(void *d, struct wp_color_manager_v1 *cm, uint32_t f) { (void)d; (void)cm; (void)f; }
static void cm_int(void *d, struct wp_color_manager_v1 *cm, uint32_t i) { (void)d; (void)cm; (void)i; }
static void cm_tf(void *d, struct wp_color_manager_v1 *cm, uint32_t t) { (void)d; (void)cm; (void)t; }
static void cm_pr(void *d, struct wp_color_manager_v1 *cm, uint32_t p) { (void)d; (void)cm; (void)p; }
static void cm_done(void *d, struct wp_color_manager_v1 *cm) { (void)d; (void)cm; }
static const struct wp_color_manager_v1_listener g_cm_l = {
    .supported_intent = cm_int, .supported_feature = cm_feat,
    .supported_tf_named = cm_tf, .supported_primaries_named = cm_pr, .done = cm_done
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    App *a = d;
    if (!strcmp(iface, wl_compositor_interface.name))
        a->comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        a->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        a->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
        xdg_wm_base_add_listener(a->wm, &g_wm_l, a);
    } else if (!strcmp(iface, wp_color_manager_v1_interface.name)) {
        a->cm = wl_registry_bind(r, name, &wp_color_manager_v1_interface, 1);
        wp_color_manager_v1_add_listener(a->cm, &g_cm_l, a);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

static struct wl_buffer *make_split_buffer(struct wl_shm *shm, int w, int h, uint32_t left_px, uint32_t right_px) {
    size_t sz = (size_t)w * (size_t)h * 4u;
    int fd = memfd_create("hdr-sdr-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)sz) < 0) return NULL;
    uint32_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return NULL; }
    for (int y = 0; y < h; ++y) {
        uint32_t *row = p + (size_t)y * (size_t)w;
        for (int x = 0; x < w; ++x) {
            row[x] = (x < w / 2) ? left_px : right_px;
        }
    }
    munmap(p, sz);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)sz);
    close(fd);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    App a = { .mode = argv[1], .state_path = argv[2], .w = 320, .h = 220 };
    a.dpy = wl_display_connect(NULL);
    if (!a.dpy) return 3;
    struct wl_registry *reg = wl_display_get_registry(a.dpy);
    wl_registry_add_listener(reg, &g_reg_l, &a);
    wl_display_roundtrip(a.dpy);
    wl_display_roundtrip(a.dpy);
    if (!a.comp || !a.shm || !a.wm || !a.cm) return 4;

    a.surf = wl_compositor_create_surface(a.comp);
    a.xs = xdg_wm_base_get_xdg_surface(a.wm, a.surf);
    xdg_surface_add_listener(a.xs, &g_xs_l, &a);
    a.xt = xdg_surface_get_toplevel(a.xs);
    xdg_toplevel_add_listener(a.xt, &g_top_l, &a);
    xdg_toplevel_set_title(a.xt, strcmp(a.mode, "hdr10") == 0 ? "Hdr10Win" : "SdrWin");

    a.cm_surf = wp_color_manager_v1_get_surface(a.cm, a.surf);
    a.cm_fb = wp_color_manager_v1_get_surface_feedback(a.cm, a.surf);
    wp_color_management_surface_feedback_v1_add_listener(a.cm_fb, &g_fb_l, &a);
    query_preferred(&a);

    if (strcmp(a.mode, "hdr10") == 0) {
        struct wp_image_description_creator_params_v1 *cr =
            wp_color_manager_v1_create_parametric_creator(a.cm);
        wp_image_description_creator_params_v1_set_tf_named(
            cr, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
        );
        wp_image_description_creator_params_v1_set_primaries_named(
            cr, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020
        );
        wp_image_description_creator_params_v1_set_max_cll(cr, 1000);
        wp_image_description_creator_params_v1_set_max_fall(cr, 400);
        a.desc = wp_image_description_creator_params_v1_create(cr);
        wp_image_description_v1_add_listener(a.desc, &g_desc_l, &a);
        while (!a.desc_ready) {
            if (wl_display_dispatch(a.dpy) < 0) return 5;
        }
        wp_color_management_surface_v1_set_image_description(
            a.cm_surf, a.desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
        );
    }

    wl_surface_commit(a.surf);
    while (!a.configured) {
        if (wl_display_dispatch(a.dpy) < 0) return 6;
    }

    /* HDR10: left = 5-nit PQ gray (65,65,65), right = 1000-nit PQ highlight (192,192,192)
     * SDR:   solid #00c800 green (0,200,0) */
    uint32_t lpx = (strcmp(a.mode, "hdr10") == 0) ? 0xff414141u : 0xff00c800u;
    uint32_t rpx = (strcmp(a.mode, "hdr10") == 0) ? 0xffc0c0c0u : 0xff00c800u;
    struct wl_buffer *cur_buf = NULL;
    while (1) {
        if (a.needs_draw) {
            a.needs_draw = false;
            struct wl_buffer *nb = make_split_buffer(a.shm, a.w, a.h, lpx, rpx);
            wl_surface_attach(a.surf, nb, 0, 0);
            wl_surface_damage_buffer(a.surf, 0, 0, a.w, a.h);
            wl_surface_commit(a.surf);
            wl_display_flush(a.dpy);
            if (cur_buf) wl_buffer_destroy(cur_buf);
            cur_buf = nb;
        }
        write_state(&a);
        if (wl_display_dispatch(a.dpy) < 0) break;
    }
    return 0;
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


def ipc_props_and_set(ipc_path: Path, prop_name: str | None = None, payload: bytes | None = None) -> dict[str, bytes]:
    for attempt in range(4):
        props: dict[str, bytes] = {}
        try:
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
                if prop_name is not None and payload is not None:
                    s.sendall(f"SET {prop_name} {payload.hex()}\n".encode("ascii"))
                    time.sleep(0.05)
            return props
        except OSError:
            if attempt == 3:
                raise
            time.sleep(0.05)
    return {}


def parse_active_box(props: dict[str, bytes]) -> tuple[int, int, int, int, int, int, int, int] | None:
    raw = props.get("_DT_ACTIVE_WINDOW_BOX")
    if not raw or len(raw) < 32:
        return None
    return struct.unpack("<iiiiiiii", raw[:32])


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
    idx += 1
    w = int(tokens[1])
    h = int(tokens[2])
    return w, h, ppm_bytes[idx: idx + w * h * 3]


def sample_rgb(rgb: bytes, w: int, x: int, y: int) -> tuple[int, int, int]:
    off = (y * w + x) * 3
    return rgb[off], rgb[off + 1], rgb[off + 2]


def make_test_icc_profile(path: Path) -> None:
    """Write a minimal valid 456-byte ICC v2.4 gamma-1.8 RGB matrix profile."""
    trc_sz = 16
    total = 440 + trc_sz
    buf = bytearray(total)
    struct.pack_into(">I", buf, 0, total)
    buf[4:8] = b"lcms"
    struct.pack_into(">I", buf, 8, 0x02400000)
    buf[12:16] = b"mntr"
    buf[16:20] = b"RGB "
    buf[20:24] = b"XYZ "
    struct.pack_into(">HHH", buf, 24, 2026, 1, 1)
    buf[36:40] = b"acsp"
    buf[40:44] = b"APPL"
    struct.pack_into(">III", buf, 68, 0x0000F6D6, 0x00010000, 0x0000D32D)
    buf[80:84] = b"lcms"
    struct.pack_into(">I", buf, 128, 9)
    tags = [
        (b"desc", 240, 108),
        (b"cprt", 348, 12),
        (b"wtpt", 360, 20),
        (b"rXYZ", 380, 20),
        (b"gXYZ", 400, 20),
        (b"bXYZ", 420, 20),
        (b"rTRC", 440, trc_sz),
        (b"gTRC", 440, trc_sz),
        (b"bTRC", 440, trc_sz),
    ]
    for i, (sig, off, sz) in enumerate(tags):
        toff = 132 + i * 12
        buf[toff:toff + 4] = sig
        struct.pack_into(">II", buf, toff + 4, off, sz)
    buf[240:248] = b"desc\x00\x00\x00\x00"
    desc = b"TestCustomICC\x00"
    struct.pack_into(">I", buf, 248, len(desc))
    buf[252:252 + len(desc)] = desc
    buf[348:360] = b"text\x00\x00\x00\x00CC0\x00"

    def put_xyz(off: int, x: float, y: float, z: float) -> None:
        buf[off:off + 8] = b"XYZ \x00\x00\x00\x00"
        struct.pack_into(">iii", buf, off + 8, round(x * 65536), round(y * 65536), round(z * 65536))

    put_xyz(360, 0.96420, 1.00000, 0.82491)
    put_xyz(380, 0.43607, 0.22250, 0.01393)
    put_xyz(400, 0.38508, 0.71687, 0.09710)
    put_xyz(420, 0.14305, 0.06061, 0.71417)
    buf[440:448] = b"curv\x00\x00\x00\x00"
    struct.pack_into(">IH", buf, 448, 1, 0x01CD)  # gamma 1.8
    path.write_bytes(bytes(buf))


def resolve_motif_prefix(comp_path: Path) -> Path:
    configured = os.environ.get("MOTIF_PREFIX")
    candidates = [Path(configured)] if configured else [
        *(parent / "motif-prefix" for parent in comp_path.parents),
        Path("/var/tmp/ck-wayland-build/wp00/motif-prefix"),
        Path("/usr/dt"),
    ]
    for prefix in candidates:
        if (prefix / "lib/libXm.so.9").is_file():
            return prefix.resolve()
    raise SystemExit("Set MOTIF_PREFIX to an installed CK Core Motif ABI 9 prefix")


def run_renderer_lane(
    comp_path: Path,
    dtstyle_bin: Path,
    client_bin: Path,
    grim: str,
    motif_lib: Path,
    renderer: str,
    expect_backend: str,
    test_dtstyle_gui: bool,
) -> None:
    tmp_parent = os.environ.get("TMPDIR")
    with tempfile.TemporaryDirectory(prefix=f"dtwm-hdr-{renderer}-", dir=tmp_parent) as td:
        tmp = Path(td)
        rt_dir = tmp / "rt"
        home_dir = tmp / "home"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt").mkdir(parents=True)
        custom_icc = tmp / "custom_gamma18.icc"
        make_test_icc_profile(custom_icc)

        status_file = tmp / "comp.status"
        env = os.environ.copy()
        env.update({
            "HOME": str(home_dir),
            "XDG_RUNTIME_DIR": str(rt_dir),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_RENDERER": renderer,
            "LD_LIBRARY_PATH": ":".join(filter(None, (str(motif_lib), env.get("LD_LIBRARY_PATH")))),
        })
        for k in ("DISPLAY", "WAYLAND_DISPLAY", "DTWM_ICC_PROFILE"):
            env.pop(k, None)

        comp_proc = subprocess.Popen(
            [str(comp_path), "--no-xwayland", "--no-panel", "--status-file", str(status_file)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        sdr_proc: subprocess.Popen | None = None
        hdr_proc: subprocess.Popen | None = None
        dtstyle_proc: subprocess.Popen | None = None

        try:
            if not wait_until(lambda: "WAYLAND_DISPLAY" in parse_kv(status_file), timeout=10.0):
                raise RuntimeError(f"dtwm-wayland ({renderer}) failed to start")
            st_kv = parse_kv(status_file)
            if renderer == "gles2" and st_kv.get("RENDERER") != "gles2":
                # Software headless environment without EGL/GLES2 render node -> skip gles2 lane cleanly
                print(f"INFO: headless gles2 unavailable (got {st_kv.get('RENDERER')}), skipping gles2 lane")
                return
            wl_sock = st_kv["WAYLAND_DISPLAY"]
            env["WAYLAND_DISPLAY"] = wl_sock
            ipc_path = rt_dir / f"dtwm-wayland-{wl_sock}.ipc"
            if not wait_until(lambda: ipc_path.exists(), timeout=5.0):
                raise RuntimeError(f"missing IPC socket {ipc_path}")

            def color_state() -> dict[str, str]:
                p = ipc_props_and_set(ipc_path)
                return parse_semi_kv(p.get("_DT_WAYLAND_COLOR_STATE", b"").decode("utf-8", errors="replace"))

            def profile_state() -> str:
                p = ipc_props_and_set(ipc_path)
                return p.get("_DT_COLOR_PROFILE_STATE", b"").decode("utf-8", errors="replace").strip()

            sdr_st = tmp / "sdr.state"
            sdr_proc = subprocess.Popen([str(client_bin), "sdr", str(sdr_st)], env=env)
            if not wait_until(lambda: parse_kv(sdr_st).get("CONFIGURED") == "1", timeout=6.0):
                raise AssertionError("SDR client failed to map")
            time.sleep(0.08)
            sdr_box = parse_active_box(ipc_props_and_set(ipc_path))
            assert sdr_box is not None
            # Tile SDR window left so HDR10 window doesn't cover it
            ipc_props_and_set(ipc_path, "_DT_WM_REQUEST", b"f.tile_window left\x00")
            if not wait_until(
                lambda: (parse_active_box(ipc_props_and_set(ipc_path)) or (999,))[0] <= 8,
                timeout=5.0,
            ):
                raise AssertionError("SDR window did not tile left")
            sdr_box = parse_active_box(ipc_props_and_set(ipc_path))
            assert sdr_box is not None

            hdr_st = tmp / "hdr.state"
            hdr_proc = subprocess.Popen([str(client_bin), "hdr10", str(hdr_st)], env=env)
            if not wait_until(
                lambda: parse_kv(hdr_st).get("CONFIGURED") == "1"
                and color_state().get("tonemapped_surfaces") == "1",
                timeout=6.0,
            ):
                raise AssertionError(f"HDR10 client did not trigger tonemap: {color_state()}")
            ipc_props_and_set(ipc_path, "_DT_WM_REQUEST", b"f.tile_window right\x00")
            if not wait_until(
                lambda: (parse_active_box(ipc_props_and_set(ipc_path)) or (0,))[0] >= 600,
                timeout=5.0,
            ):
                raise AssertionError("HDR10 window did not tile right")
            time.sleep(0.08)
            hdr_box = parse_active_box(ipc_props_and_set(ipc_path))
            assert hdr_box is not None

            cst = color_state()
            assert cst.get("tonemap_backend") == expect_backend, (
                f"expected tonemap_backend={expect_backend} on {renderer}, got {cst}"
            )
            sl = int(cst.get("tm_sample_l", "0"), 16)
            sr_hex = int(cst.get("tm_sample_r", "0"), 16)
            fbo_lr = (sl >> 16) & 0xFF
            fbo_lg = (sl >> 8) & 0xFF
            fbo_rr = (sr_hex >> 16) & 0xFF
            fbo_rg = (sr_hex >> 8) & 0xFF
            assert 32 <= fbo_lr <= 54 and abs(fbo_lr - fbo_lg) <= 3, (
                f"expected {renderer} FBO/LUT PQ 5-nit shadow ~42, got {cst}"
            )
            assert 248 <= fbo_rr <= 255 and abs(fbo_rr - fbo_rg) <= 3, (
                f"expected {renderer} FBO/LUT PQ 1000-nit max_cll peak -> ~255, got {cst}"
            )

            grim_res = subprocess.run([grim, "-t", "ppm", "-"], env=env, capture_output=True, timeout=5.0)
            if renderer != "gles2" and grim_res.returncode != 0:
                raise RuntimeError(f"grim failed on {renderer}: {grim_res.stderr.decode('utf-8', errors='replace')}")
            if grim_res.returncode == 0:
                gw, gh, rgb = read_ppm(grim_res.stdout)

                sx = sdr_box[4] + 30
                sy = sdr_box[5] + 30
                sr, sg, sb = sample_rgb(rgb, gw, sx, sy)
                assert sr < 15 and 188 <= sg <= 212 and sb < 15, (
                    f"expected untouched SDR green (~0,200,0) on {renderer}, got {(sr, sg, sb)}"
                )

                hx_left = hdr_box[4] + hdr_box[6] // 2 - 12
                hx_right = hdr_box[4] + hdr_box[6] // 2 + 12
                hy = hdr_box[5] + 18
                hl_r, hl_g, hl_b = sample_rgb(rgb, gw, hx_left, hy)
                hr_r, hr_g, hr_b = sample_rgb(rgb, gw, hx_right, hy)
                assert 32 <= hl_r <= 54 and abs(hl_r - hl_g) <= 3, (
                    f"expected PQ 5-nit shadow (raw 65) tone-mapped to ~42 on {renderer}, got {(hl_r, hl_g, hl_b)}"
                )
                assert 248 <= hr_r <= 255 and abs(hr_r - hr_g) <= 3, (
                    f"expected PQ 1000-nit max_cll peak (raw 192) tone-mapped to ~255 on {renderer}, got {(hr_r, hr_g, hr_b)}"
                )

            if not test_dtstyle_gui:
                return

            # Launch real dtstyle -monitors under dtwm-wayland --no-xwayland
            dtstyle_proc = subprocess.Popen([str(dtstyle_bin), "-monitors"], env=env)
            if not wait_until(
                lambda: ipc_props_and_set(ipc_path).get("_DT_STYLE_DISPLAY_READY") == b"1",
                timeout=10.0,
            ):
                raise RuntimeError("dtstyle -monitors did not publish _DT_STYLE_DISPLAY_READY")

            # 1. Assign Display P3 via dtstyle GUI IPC live preview, then Apply
            ergo_conf = home_dir / ".dt/display_ergonomics.conf"
            ipc_props_and_set(ipc_path, "_DT_STYLE_DISPLAY_IPC", b"set-color-profile HEADLESS-1 display-p3")
            if not wait_until(
                lambda: "HEADLESS-1=display-p3" in profile_state()
                and color_state().get("output_transforms") == "1",
                timeout=6.0,
            ):
                raise AssertionError(f"dtstyle display-p3 live preview failed: {profile_state()} / {color_state()}")
            ipc_props_and_set(ipc_path, "_DT_STYLE_DISPLAY_IPC", b"apply")
            if not wait_until(
                lambda: "HEADLESS-1=display-p3" in profile_state()
                and ergo_conf.is_file()
                and "color_profile.HEADLESS-1=display-p3" in ergo_conf.read_text(encoding="utf-8"),
                timeout=6.0,
            ):
                raise AssertionError(f"dtstyle display-p3 apply failed: {profile_state()} / {color_state()}")

            # 2. Switch monitor to HDR10 (BT.2020 PQ) passthrough via newline-batched dtstyle IPC
            ipc_props_and_set(ipc_path, "_DT_STYLE_DISPLAY_IPC", b"set-color-profile HEADLESS-1 hdr10\napply")
            if not wait_until(
                lambda: "HEADLESS-1=hdr10" in profile_state()
                and color_state().get("tonemapped_surfaces") == "0"
                and ergo_conf.is_file()
                and "color_profile.HEADLESS-1=hdr10" in ergo_conf.read_text(encoding="utf-8"),
                timeout=6.0,
            ):
                raise AssertionError(f"dtstyle hdr10 passthrough failed: {profile_state()} / {color_state()}")
            if not wait_until(lambda: parse_kv(hdr_st).get("PREF_IS_HDR10") == "1", timeout=5.0):
                raise AssertionError(f"expected PREF_IS_HDR10=1 (BT.2020 PQ), got {parse_kv(hdr_st)}")

            time.sleep(0.08)
            ppm_hdr = subprocess.check_output([grim, "-t", "ppm", "-"], env=env, timeout=5.0)
            _, _, rgb_hdr = read_ppm(ppm_hdr)
            pl_r, _, _ = sample_rgb(rgb_hdr, gw, hx_left, hy)
            pr_r, _, _ = sample_rgb(rgb_hdr, gw, hx_right, hy)
            assert 60 <= pl_r <= 70 and 187 <= pr_r <= 197, (
                f"expected raw PQ passthrough (65 / 192) on hdr10 output, got {pl_r} / {pr_r}"
            )

            # 3. Assign Custom ICC Profile file + OK atomically (closes dialog & persists)
            ipc_props_and_set(
                ipc_path, "_DT_STYLE_DISPLAY_IPC", f"set-color-profile HEADLESS-1 {custom_icc}\nok".encode("utf-8")
            )
            if not wait_until(
                lambda: str(custom_icc) in profile_state()
                and color_state().get("output_transforms") == "1"
                and color_state().get("tonemapped_surfaces") == "1"
                and ipc_props_and_set(ipc_path).get("_DT_STYLE_DISPLAY_READY") == b"0"
                and ergo_conf.is_file()
                and f"color_profile.HEADLESS-1={custom_icc}" in ergo_conf.read_text(encoding="utf-8"),
                timeout=6.0,
            ):
                raise AssertionError(f"dtstyle custom ICC apply failed: {profile_state()} / {color_state()}")

            for p in (dtstyle_proc, hdr_proc, sdr_proc, comp_proc):
                if p and p.poll() is None:
                    p.terminate()
                    p.wait(timeout=4.0)
            dtstyle_proc = hdr_proc = sdr_proc = comp_proc = None

            # 4. Restart dtwm-wayland and verify persisted custom ICC profile reloads automatically!
            status_file.unlink(missing_ok=True)
            comp_proc = subprocess.Popen(
                [str(comp_path), "--no-xwayland", "--no-panel", "--status-file", str(status_file)],
                env={k: v for k, v in env.items() if k != "WAYLAND_DISPLAY"},
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if not wait_until(lambda: "WAYLAND_DISPLAY" in parse_kv(status_file), timeout=10.0):
                raise RuntimeError("restarted dtwm-wayland failed to write status file")
            wl_sock2 = parse_kv(status_file)["WAYLAND_DISPLAY"]
            ipc_path2 = rt_dir / f"dtwm-wayland-{wl_sock2}.ipc"
            if not wait_until(lambda: ipc_path2.exists(), timeout=5.0):
                raise RuntimeError("restarted IPC socket missing")
            if not wait_until(
                lambda: str(custom_icc) in ipc_props_and_set(ipc_path2).get("_DT_COLOR_PROFILE_STATE", b"").decode("utf-8", errors="replace"),
                timeout=5.0,
            ):
                raise AssertionError("restarted dtwm-wayland did not restore persisted color_profile from display_ergonomics.conf")

        finally:
            for p in (dtstyle_proc, hdr_proc, sdr_proc, comp_proc):
                if p and p.poll() is None:
                    with suppress(Exception):
                        p.terminate()
                        p.wait(timeout=4.0)
                    if p.poll() is None:
                        with suppress(Exception):
                            p.kill()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    ap.add_argument("--dtstyle", default=None)
    args = ap.parse_args()

    comp_path = Path(args.compositor).resolve()
    if not comp_path.is_file():
        raise SystemExit(f"missing compositor: {comp_path}")
    if args.dtstyle:
        dtstyle_bin = Path(args.dtstyle).resolve()
    else:
        dtstyle_candidates = [
            comp_path.parent.parent / "dtstyle/dtstyle",
            comp_path.parent / "dtstyle",
            comp_path.parent.parent.parent / "dtstyle/dtstyle",
        ]
        dtstyle_bin = next(
            (cand.resolve() for cand in dtstyle_candidates if cand.is_file()),
            dtstyle_candidates[0].resolve(),
        )
    if not dtstyle_bin.is_file():
        raise SystemExit(f"missing dtstyle binary: {dtstyle_bin}")
    motif_prefix = resolve_motif_prefix(comp_path)

    cc = os.environ.get("CC", "cc")
    wl_scanner = shutil.which("wayland-scanner")
    grim = shutil.which("grim")
    if not wl_scanner or not grim:
        raise SystemExit("wayland-scanner and grim are required")

    tmp_parent = os.environ.get("TMPDIR")
    with tempfile.TemporaryDirectory(prefix="dtwm-hdr-build-", dir=tmp_parent) as td:
        tmp = Path(td)
        proto_root = Path("/usr/share/wayland-protocols")
        protos = [
            ("xdg-shell", proto_root / "stable/xdg-shell/xdg-shell.xml"),
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
        cli_c = tmp / "hdr_sdr_client.c"
        cli_bin = tmp / "hdr_sdr_client"
        cli_c.write_text(HDR_SDR_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", "-I", str(tmp), str(cli_c), *gen_srcs, *wl_cflags, *wl_libs, "-o", str(cli_bin)],
            check=True,
        )

        run_renderer_lane(
            comp_path, dtstyle_bin, cli_bin, grim, motif_prefix / "lib",
            renderer="gles2", expect_backend="gles2_shader", test_dtstyle_gui=False,
        )
        run_renderer_lane(
            comp_path, dtstyle_bin, cli_bin, grim, motif_prefix / "lib",
            renderer="pixman", expect_backend="cpu_lut", test_dtstyle_gui=True,
        )

    print("PASS: check-dtwm-wayland-hdr-dtstyle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
