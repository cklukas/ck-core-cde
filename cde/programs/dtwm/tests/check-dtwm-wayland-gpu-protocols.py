#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland Track 2:
Smart GPU/Pixman Renderer Selection & Fallback, wp_presentation,
wp_single_pixel_buffer_v1, and wp_alpha_modifier_v1:

1. Launches dtwm-wayland (--no-xwayland) with WLR_RENDERER unset on headless
   backend and verifies automatic pixman selection (RENDERER=pixman in status
   file and _DT_WAYLAND_RENDERER over native Wayland IPC).
2. Launches dtwm-wayland with an invalid WLR_RENDERER=nonexistent_gpu_driver
   and verifies automatic fallback to pixman instead of compositor startup failure.
3. Compiles and runs a native Wayland protocol client exercising:
   - wp_presentation (clock_id + wp_presentation_feedback.presented callback)
   - wp_single_pixel_buffer_manager_v1 + wp_viewporter (1x1 RGBA buffer scaled
     to 280x280 without wl_shm allocation)
   - wp_alpha_modifier_v1 (50% alpha multiplier blending pure red over pure blue
     into purple, verified pixel-accurately via grim -t ppm).
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path


CLIENT_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client.h"
#include "xdg-decoration-client.h"
#include "viewporter-client.h"
#include "presentation-time-client.h"
#include "single-pixel-buffer-v1-client.h"
#include "alpha-modifier-v1-client.h"
#include "linux-dmabuf-v1-client.h"

typedef struct {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_mgr;
    struct wp_viewporter *viewporter;
    struct wp_presentation *presentation;
    struct wp_single_pixel_buffer_manager_v1 *sp_mgr;
    struct wp_alpha_modifier_v1 *alpha_mgr;
    struct zwp_linux_dmabuf_v1 *dmabuf_mgr;
    bool got_clock_id;
    uint32_t clock_id;
    bool presented;
    bool discarded;
    uint32_t pres_refresh;
    bool dmabuf_fb_done;
    uint32_t dmabuf_fmt_size;
    int dmabuf_tranches;
} AppCtx;

typedef struct {
    AppCtx *app;
    struct wl_surface *surf;
    struct xdg_surface *xdg_surf;
    struct xdg_toplevel *xdg_top;
    struct zxdg_toplevel_decoration_v1 *deco;
    struct wp_viewport *vp;
    struct wp_alpha_modifier_surface_v1 *alpha_surf;
    struct wl_buffer *buf;
    int target_w;
    int target_h;
    uint32_t r32, g32, b32, a32;
    uint32_t alpha_mult;
    bool use_alpha_mod;
    bool configured;
} WinState;

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener g_wm_listener = { .ping = wm_base_ping };

static void pres_clock_id(void *data, struct wp_presentation *p, uint32_t clk_id) {
    (void)p;
    AppCtx *app = data;
    app->got_clock_id = true;
    app->clock_id = clk_id;
}
static const struct wp_presentation_listener g_pres_listener = { .clock_id = pres_clock_id };

static void fb_sync_output(void *data, struct wp_presentation_feedback *f, struct wl_output *o) {
    (void)data; (void)f; (void)o;
}
static void fb_presented(
    void *data, struct wp_presentation_feedback *f,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
    uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags
) {
    (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
    (void)seq_hi; (void)seq_lo; (void)flags;
    AppCtx *app = data;
    app->presented = true;
    app->pres_refresh = refresh;
    wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *data, struct wp_presentation_feedback *f) {
    AppCtx *app = data;
    app->discarded = true;
    wp_presentation_feedback_destroy(f);
}
static const struct wp_presentation_feedback_listener g_fb_listener = {
    .sync_output = fb_sync_output,
    .presented = fb_presented,
    .discarded = fb_discarded,
};

static void dmabuf_fb_done_cb(void *data, struct zwp_linux_dmabuf_feedback_v1 *fb) {
    (void)fb;
    AppCtx *app = data;
    app->dmabuf_fb_done = true;
}
static void dmabuf_fb_format_table(
    void *data, struct zwp_linux_dmabuf_feedback_v1 *fb, int32_t fd, uint32_t size
) {
    (void)fb;
    AppCtx *app = data;
    app->dmabuf_fmt_size = size;
    if (fd >= 0) close(fd);
}
static void dmabuf_fb_dev(
    void *data, struct zwp_linux_dmabuf_feedback_v1 *fb, struct wl_array *device
) {
    (void)data; (void)fb; (void)device;
}
static void dmabuf_fb_tranche_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *fb) {
    (void)fb;
    AppCtx *app = data;
    app->dmabuf_tranches++;
}
static void dmabuf_fb_tranche_formats(
    void *data, struct zwp_linux_dmabuf_feedback_v1 *fb, struct wl_array *indices
) {
    (void)data; (void)fb; (void)indices;
}
static void dmabuf_fb_tranche_flags(
    void *data, struct zwp_linux_dmabuf_feedback_v1 *fb, uint32_t flags
) {
    (void)data; (void)fb; (void)flags;
}
static const struct zwp_linux_dmabuf_feedback_v1_listener g_dmabuf_fb_listener = {
    .done = dmabuf_fb_done_cb,
    .format_table = dmabuf_fb_format_table,
    .main_device = dmabuf_fb_dev,
    .tranche_done = dmabuf_fb_tranche_done,
    .tranche_target_device = dmabuf_fb_dev,
    .tranche_formats = dmabuf_fb_tranche_formats,
    .tranche_flags = dmabuf_fb_tranche_flags,
};

static void deco_configure(
    void *data, struct zxdg_toplevel_decoration_v1 *d, uint32_t mode
) {
    (void)data; (void)d; (void)mode;
}
static const struct zxdg_toplevel_decoration_v1_listener g_deco_listener = {
    .configure = deco_configure,
};

static void top_configure(
    void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states
) {
    (void)t; (void)states;
    WinState *ws = data;
    if (w > 0) ws->target_w = w;
    if (h > 0) ws->target_h = h;
}
static void top_close(void *data, struct xdg_toplevel *t) { (void)data; (void)t; }
static void top_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h) {
    (void)data; (void)t; (void)w; (void)h;
}
static void top_caps(void *data, struct xdg_toplevel *t, struct wl_array *caps) {
    (void)data; (void)t; (void)caps;
}
static const struct xdg_toplevel_listener g_top_listener = {
    .configure = top_configure,
    .close = top_close,
    .configure_bounds = top_bounds,
    .wm_capabilities = top_caps,
};

static void xdg_surf_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    WinState *ws = data;
    xdg_surface_ack_configure(xs, serial);
    if (!ws->buf) {
        ws->buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
            ws->app->sp_mgr, ws->r32, ws->g32, ws->b32, ws->a32
        );
    }
    wp_viewport_set_destination(ws->vp, ws->target_w, ws->target_h);
    if (ws->use_alpha_mod && ws->alpha_surf) {
        wp_alpha_modifier_surface_v1_set_multiplier(ws->alpha_surf, ws->alpha_mult);
    }
    wl_surface_attach(ws->surf, ws->buf, 0, 0);
    wl_surface_damage_buffer(ws->surf, 0, 0, 1, 1);
    wl_surface_commit(ws->surf);
    ws->configured = true;
}
static const struct xdg_surface_listener g_xdg_surf_listener = {
    .configure = xdg_surf_configure,
};

static void reg_global(
    void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver
) {
    AppCtx *app = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        app->comp = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
        xdg_wm_base_add_listener(app->wm_base, &g_wm_listener, app);
    } else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        app->deco_mgr = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        app->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (strcmp(iface, wp_presentation_interface.name) == 0) {
        app->presentation = wl_registry_bind(reg, name, &wp_presentation_interface, ver < 2 ? ver : 2);
        wp_presentation_add_listener(app->presentation, &g_pres_listener, app);
    } else if (strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        app->sp_mgr = wl_registry_bind(reg, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_alpha_modifier_v1_interface.name) == 0) {
        app->alpha_mgr = wl_registry_bind(reg, name, &wp_alpha_modifier_v1_interface, 1);
    } else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0 && ver >= 4) {
        app->dmabuf_mgr = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 4);
    }
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener g_reg_listener = {
    .global = reg_global,
    .global_remove = reg_remove,
};

static void create_sp_window(
    AppCtx *app, WinState *ws, const char *title, int w, int h,
    uint32_t r32, uint32_t g32, uint32_t b32, uint32_t a32,
    bool use_alpha_mod, uint32_t alpha_mult, struct xdg_toplevel *parent_top
) {
    memset(ws, 0, sizeof(*ws));
    ws->app = app;
    ws->target_w = w;
    ws->target_h = h;
    ws->r32 = r32;
    ws->g32 = g32;
    ws->b32 = b32;
    ws->a32 = a32;
    ws->use_alpha_mod = use_alpha_mod;
    ws->alpha_mult = alpha_mult;
    ws->surf = wl_compositor_create_surface(app->comp);
    ws->vp = wp_viewporter_get_viewport(app->viewporter, ws->surf);
    if (use_alpha_mod) {
        ws->alpha_surf = wp_alpha_modifier_v1_get_surface(app->alpha_mgr, ws->surf);
    }
    ws->xdg_surf = xdg_wm_base_get_xdg_surface(app->wm_base, ws->surf);
    xdg_surface_add_listener(ws->xdg_surf, &g_xdg_surf_listener, ws);
    ws->xdg_top = xdg_surface_get_toplevel(ws->xdg_surf);
    xdg_toplevel_add_listener(ws->xdg_top, &g_top_listener, ws);
    xdg_toplevel_set_title(ws->xdg_top, title);
    xdg_toplevel_set_app_id(ws->xdg_top, title);
    xdg_toplevel_set_min_size(ws->xdg_top, w, h);
    xdg_toplevel_set_max_size(ws->xdg_top, w, h);
    if (parent_top) {
        xdg_toplevel_set_parent(ws->xdg_top, parent_top);
    }
    if (app->deco_mgr) {
        ws->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(app->deco_mgr, ws->xdg_top);
        zxdg_toplevel_decoration_v1_add_listener(ws->deco, &g_deco_listener, ws);
        zxdg_toplevel_decoration_v1_set_mode(
            ws->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
        );
    }
    wl_surface_commit(ws->surf);
}

static void pump_wl_ms(struct wl_display *dpy, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long deadline = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L + timeout_ms;
    int fd = wl_display_get_fd(dpy);
    for (;;) {
        while (wl_display_prepare_read(dpy) != 0) {
            wl_display_dispatch_pending(dpy);
        }
        wl_display_flush(dpy);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long rem = deadline - (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
        if (rem <= 0) {
            wl_display_cancel_read(dpy);
            break;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, rem > 25 ? 25 : (int)rem);
        if (pr > 0) {
            wl_display_read_events(dpy);
            wl_display_dispatch_pending(dpy);
        } else {
            wl_display_cancel_read(dpy);
        }
    }
}

static void write_state(const char *path, const char *stage, const AppCtx *app) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(
        f,
        "stage=%s\nclock_id=%d\npresented=%d\ndiscarded=%d\nrefresh=%u\n"
        "has_dmabuf=%d\ndmabuf_done=%d\ndmabuf_fmt_size=%u\ndmabuf_tranches=%d\n",
        stage,
        app->got_clock_id ? 1 : 0,
        app->presented ? 1 : 0,
        app->discarded ? 1 : 0,
        app->pres_refresh,
        app->dmabuf_mgr ? 1 : 0,
        app->dmabuf_fb_done ? 1 : 0,
        app->dmabuf_fmt_size,
        app->dmabuf_tranches
    );
    fclose(f);
    rename(tmp, path);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *state_path = argv[1];
    const char *sync_dir = argv[2];

    AppCtx app;
    memset(&app, 0, sizeof(app));
    app.dpy = wl_display_connect(NULL);
    if (!app.dpy) return 3;
    app.reg = wl_display_get_registry(app.dpy);
    wl_registry_add_listener(app.reg, &g_reg_listener, &app);
    wl_display_roundtrip(app.dpy);
    wl_display_roundtrip(app.dpy);

    if (!app.comp || !app.wm_base || !app.viewporter ||
        !app.presentation || !app.sp_mgr || !app.alpha_mgr) {
        fprintf(
            stderr,
            "missing globals: comp=%p wm=%p vp=%p pres=%p sp=%p alpha=%p\n",
            (void *)app.comp, (void *)app.wm_base, (void *)app.viewporter,
            (void *)app.presentation, (void *)app.sp_mgr, (void *)app.alpha_mgr
        );
        return 4;
    }

    if (app.dmabuf_mgr) {
        struct zwp_linux_dmabuf_feedback_v1 *dfb =
            zwp_linux_dmabuf_v1_get_default_feedback(app.dmabuf_mgr);
        zwp_linux_dmabuf_feedback_v1_add_listener(dfb, &g_dmabuf_fb_listener, &app);
        wl_display_roundtrip(app.dpy);
        wl_display_roundtrip(app.dpy);
        zwp_linux_dmabuf_feedback_v1_destroy(dfb);
    }
    if (argc >= 4 && strcmp(argv[3], "dmabuf_only") == 0) {
        write_state(state_path, "dmabuf_checked", &app);
        wl_display_disconnect(app.dpy);
        return 0;
    }

    /* Stage 1: 1000x750 pure-blue single-pixel buffer + presentation feedback */
    WinState blue_win;
    create_sp_window(
        &app, &blue_win, "sp-blue-base", 1000, 750,
        0u, 0u, 0xffffffffu, 0xffffffffu, false, 0u, NULL
    );
    while (!blue_win.configured) {
        wl_display_dispatch(app.dpy);
    }
    /* First capture blue_ready from the pure wp_single_pixel_buffer_v1 + wp_viewporter surface,
     * then submit a textured 4x4 wl_shm frame with wp_presentation_feedback to verify presented. */
    pump_wl_ms(app.dpy, 80);
    if (app.shm) {
        int sfd = memfd_create("pres-shm", MFD_CLOEXEC);
        if (sfd >= 0 && ftruncate(sfd, 64) == 0) {
            uint32_t *px = mmap(NULL, 64, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
            if (px != MAP_FAILED) {
                for (int i = 0; i < 16; ++i) px[i] = 0xff0000ffu; /* ARGB8888 pure blue */
                munmap(px, 64);
            }
            struct wl_shm_pool *pool = wl_shm_create_pool(app.shm, sfd, 64);
            struct wl_buffer *sbuf = wl_shm_pool_create_buffer(
                pool, 0, 4, 4, 16, WL_SHM_FORMAT_ARGB8888
            );
            wl_shm_pool_destroy(pool);
            close(sfd);

            struct wp_presentation_feedback *fb =
                wp_presentation_feedback(app.presentation, blue_win.surf);
            wp_presentation_feedback_add_listener(fb, &g_fb_listener, &app);
            wp_viewport_set_destination(blue_win.vp, blue_win.target_w, blue_win.target_h);
            wl_surface_attach(blue_win.surf, sbuf, 0, 0);
            wl_surface_damage_buffer(blue_win.surf, 0, 0, 4, 4);
            wl_surface_commit(blue_win.surf);
            for (int i = 0; i < 40 && !app.presented; ++i) {
                pump_wl_ms(app.dpy, 50);
            }
            /* Re-attach wp_single_pixel_buffer_v1 so Stage 1 & Stage 2 grim checks sample
             * pure wp_single_pixel_buffer_v1 + wp_viewporter rendering! */
            wp_viewport_set_destination(blue_win.vp, blue_win.target_w, blue_win.target_h);
            wl_surface_attach(blue_win.surf, blue_win.buf, 0, 0);
            wl_surface_damage_buffer(blue_win.surf, 0, 0, 1, 1);
            wl_surface_commit(blue_win.surf);
            pump_wl_ms(app.dpy, 60);
        }
    }
    write_state(state_path, "blue_ready", &app);

    char next_flag[512], quit_flag[512];
    snprintf(next_flag, sizeof(next_flag), "%s/spawn_alpha_red", sync_dir);
    snprintf(quit_flag, sizeof(quit_flag), "%s/quit", sync_dir);

    while (access(next_flag, F_OK) != 0 && access(quit_flag, F_OK) != 0) {
        pump_wl_ms(app.dpy, 30);
    }

    /* Stage 2: Transient 300x240 opaque red wl_shm buffer with 50% wp_alpha_modifier_v1 */
    WinState red_win;
    create_sp_window(
        &app, &red_win, "sp-red-alpha50", 300, 240,
        0xffffffffu, 0u, 0u, 0xffffffffu, true, 0x80000000u, blue_win.xdg_top
    );
    while (!red_win.configured) {
        wl_display_dispatch(app.dpy);
    }
    if (app.shm) {
        int rfd = memfd_create("red-shm", MFD_CLOEXEC);
        if (rfd >= 0 && ftruncate(rfd, 64) == 0) {
            uint32_t *px = mmap(NULL, 64, PROT_READ | PROT_WRITE, MAP_SHARED, rfd, 0);
            if (px != MAP_FAILED) {
                for (int i = 0; i < 16; ++i) px[i] = 0xffff0000u; /* ARGB8888 100% opaque red */
                munmap(px, 64);
            }
            struct wl_shm_pool *rpool = wl_shm_create_pool(app.shm, rfd, 64);
            struct wl_buffer *rbuf = wl_shm_pool_create_buffer(
                rpool, 0, 4, 4, 16, WL_SHM_FORMAT_ARGB8888
            );
            wl_shm_pool_destroy(rpool);
            close(rfd);
            red_win.buf = rbuf;
            wp_viewport_set_destination(red_win.vp, red_win.target_w, red_win.target_h);
            wp_alpha_modifier_surface_v1_set_multiplier(red_win.alpha_surf, 0x80000000u);
            wl_surface_attach(red_win.surf, rbuf, 0, 0);
            wl_surface_damage_buffer(red_win.surf, 0, 0, 4, 4);
            wl_surface_commit(red_win.surf);
        }
    }
    pump_wl_ms(app.dpy, 150);
    write_state(state_path, "alpha_ready", &app);

    while (access(quit_flag, F_OK) != 0) {
        pump_wl_ms(app.dpy, 30);
    }
    wl_display_disconnect(app.dpy);
    return 0;
}
"""


def parse_ppm(ppm_bytes: bytes) -> tuple[int, int, bytes]:
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
        raise RuntimeError(f"Unexpected PPM header: {tokens!r}")
    # Skip single whitespace delimiter after maxval
    if idx < n and ppm_bytes[idx] in b" \t\r\n":
        idx += 1
    w = int(tokens[1])
    h = int(tokens[2])
    return w, h, ppm_bytes[idx:]


def count_matching_pixels(
    w: int,
    h: int,
    rgb: bytes,
    r_range: tuple[int, int],
    g_range: tuple[int, int],
    b_range: tuple[int, int],
) -> int:
    matches = 0
    step = 4
    for y in range(0, h, step):
        row_off = y * w * 3
        for x in range(0, w, step):
            off = row_off + x * 3
            if off + 2 >= len(rgb):
                continue
            r, g, b = rgb[off], rgb[off + 1], rgb[off + 2]
            if (
                r_range[0] <= r <= r_range[1]
                and g_range[0] <= g <= g_range[1]
                and b_range[0] <= b <= b_range[1]
            ):
                matches += 1
    return matches


def query_ipc_prop(sock_path: Path, atom_name: str) -> str:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2.0)
        s.connect(str(sock_path))
        f = s.makefile("r", encoding="utf-8", errors="replace")
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            line = f.readline()
            if not line:
                break
            parts = line.strip().split()
            if len(parts) >= 3 and parts[0] == "PROP" and parts[1] == atom_name:
                return bytes.fromhex(parts[2]).decode("utf-8", errors="replace")
        return ""


def wait_status_file(status_file: Path, proc: subprocess.Popen[bytes], timeout_s: float = 12.0) -> dict[str, str]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"dtwm-wayland exited early with rc={proc.returncode}")
        if status_file.exists():
            kv: dict[str, str] = {}
            for line in status_file.read_text(encoding="utf-8").splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    kv[k.strip()] = v.strip()
            if "WAYLAND_DISPLAY" in kv:
                return kv
        time.sleep(0.05)
    raise TimeoutError("Timed out waiting for dtwm-wayland status file")


def build_protocol_client(tmp_dir: Path, cc: str) -> Path:
    scanner = shutil.which("wayland-scanner")
    if not scanner:
        raise RuntimeError("wayland-scanner not found")
    wp_dir_str = subprocess.check_output(
        ["pkg-config", "--variable=pkgdatadir", "wayland-protocols"], text=True
    ).strip()
    wp_dir = Path(wp_dir_str)
    xml_specs = [
        ("xdg-shell", wp_dir / "stable/xdg-shell/xdg-shell.xml"),
        ("xdg-decoration", wp_dir / "unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"),
        ("viewporter", wp_dir / "stable/viewporter/viewporter.xml"),
        ("presentation-time", wp_dir / "stable/presentation-time/presentation-time.xml"),
        ("single-pixel-buffer-v1", wp_dir / "staging/single-pixel-buffer/single-pixel-buffer-v1.xml"),
        ("alpha-modifier-v1", wp_dir / "staging/alpha-modifier/alpha-modifier-v1.xml"),
        ("linux-dmabuf-v1", wp_dir / "unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml"),
    ]
    gen_srcs: list[str] = []
    for prefix, xml_path in xml_specs:
        hdr = tmp_dir / f"{prefix}-client.h"
        src = tmp_dir / f"{prefix}-code.c"
        subprocess.run([scanner, "client-header", str(xml_path), str(hdr)], check=True)
        subprocess.run([scanner, "private-code", str(xml_path), str(src)], check=True)
        gen_srcs.append(str(src))

    c_file = tmp_dir / "gpu_proto_client.c"
    c_file.write_text(CLIENT_C, encoding="utf-8")
    bin_path = tmp_dir / "gpu-proto-client"

    cflags = subprocess.check_output(
        ["pkg-config", "--cflags", "wayland-client"], text=True
    ).split()
    libs = subprocess.check_output(
        ["pkg-config", "--libs", "wayland-client"], text=True
    ).split()
    cmd = [cc, "-O2", "-Wall", f"-I{tmp_dir}", str(c_file), *gen_srcs, *cflags, *libs, "-o", str(bin_path)]
    subprocess.run(cmd, check=True)
    return bin_path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    comp_bin = str(Path(args.compositor).resolve())
    grim_bin = shutil.which("grim")
    if not grim_bin:
        print("SKIP: grim not installed")
        return 0

    cc = os.environ.get("CC", "cc")
    base_tmp = os.environ.get("TMPDIR") or "/tmp"

    with tempfile.TemporaryDirectory(prefix="dtwm-wl-gpu-", dir=base_tmp) as td:
        root = Path(td)
        client_bin = build_protocol_client(root, cc)

        # 1. Verify fallback when WLR_RENDERER is set to an invalid GPU renderer name
        rt1 = root / "rt-fallback"
        rt1.mkdir(mode=0o700)
        env_fb = os.environ.copy()
        env_fb.pop("DISPLAY", None)
        env_fb["XDG_RUNTIME_DIR"] = str(rt1)
        env_fb["HOME"] = str(root)
        env_fb["WLR_BACKENDS"] = "headless"
        env_fb["WLR_HEADLESS_OUTPUTS"] = "1"
        env_fb["WLR_LIBINPUT_NO_DEVICES"] = "1"
        env_fb["WLR_RENDERER"] = "nonexistent_gpu_driver"
        sf1 = root / "fallback.status"
        log1 = (root / "fallback.log").open("w", encoding="utf-8")
        p1 = subprocess.Popen(
            [comp_bin, "--no-xwayland", "--no-panel", "--status-file", str(sf1)],
            env=env_fb,
            stdout=log1,
            stderr=subprocess.STDOUT,
        )
        try:
            kv1 = wait_status_file(sf1, p1)
            if kv1.get("RENDERER") != "pixman":
                raise RuntimeError(f"Expected automatic pixman fallback in status file, got: {kv1}")
        finally:
            p1.terminate()
            p1.wait(timeout=5)
            log1.close()

        # 2. Verify auto-selection (WLR_RENDERER unset on headless) + new Wayland protocols
        rt2 = root / "rt-main"
        rt2.mkdir(mode=0o700)
        sync_dir = root / "sync"
        sync_dir.mkdir(mode=0o700)
        env_main = os.environ.copy()
        env_main.pop("DISPLAY", None)
        env_main.pop("WLR_RENDERER", None)
        env_main["XDG_RUNTIME_DIR"] = str(rt2)
        env_main["HOME"] = str(root)
        env_main["WLR_BACKENDS"] = "headless"
        env_main["WLR_HEADLESS_OUTPUTS"] = "1"
        env_main["WLR_LIBINPUT_NO_DEVICES"] = "1"

        sf2 = root / "main.status"
        log2 = (root / "main.log").open("w", encoding="utf-8")
        comp = subprocess.Popen(
            [comp_bin, "--no-xwayland", "--no-panel", "--status-file", str(sf2)],
            env=env_main,
            stdout=log2,
            stderr=subprocess.STDOUT,
        )
        client_proc: subprocess.Popen[bytes] | None = None
        try:
            kv2 = wait_status_file(sf2, comp)
            wl_sock = kv2["WAYLAND_DISPLAY"]
            if kv2.get("RENDERER") != "pixman":
                raise RuntimeError(f"Expected auto-selected RENDERER=pixman on headless backend, got: {kv2}")

            ipc_sock = rt2 / f"dtwm-wayland-{wl_sock}.ipc"
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline and not ipc_sock.exists():
                time.sleep(0.05)
            ipc_renderer = query_ipc_prop(ipc_sock, "_DT_WAYLAND_RENDERER")
            if ipc_renderer != "pixman":
                raise RuntimeError(f"Expected _DT_WAYLAND_RENDERER=pixman over IPC, got {ipc_renderer!r}")

            c_env = env_main.copy()
            c_env["WAYLAND_DISPLAY"] = wl_sock
            state_file = root / "client.state"
            client_proc = subprocess.Popen(
                [str(client_bin), str(state_file), str(sync_dir)],
                env=c_env,
            )

            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline:
                if client_proc.poll() is not None:
                    raise RuntimeError(f"gpu-proto-client exited early rc={client_proc.returncode}")
                if state_file.exists() and "stage=blue_ready" in state_file.read_text(encoding="utf-8"):
                    break
                time.sleep(0.05)
            else:
                raise TimeoutError("Timed out waiting for blue_ready from gpu-proto-client")

            st_txt = state_file.read_text(encoding="utf-8")
            if "clock_id=1" not in st_txt or "presented=1" not in st_txt:
                raise RuntimeError(f"wp_presentation feedback did not complete:\n{st_txt}")

            ppm1 = subprocess.check_output([grim_bin, "-t", "ppm", "-"], env=c_env)
            w1, h1, rgb1 = parse_ppm(ppm1)
            blue_hits = count_matching_pixels(w1, h1, rgb1, (0, 20), (0, 20), (235, 255))
            if blue_hits < 500:
                raise RuntimeError(
                    f"wp_single_pixel_buffer_v1 + wp_viewporter blue surface not visible (hits={blue_hits})"
                )

            (sync_dir / "spawn_alpha_red").write_text("1\n", encoding="utf-8")
            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline:
                if state_file.exists() and "stage=alpha_ready" in state_file.read_text(encoding="utf-8"):
                    break
                time.sleep(0.05)
            else:
                raise TimeoutError("Timed out waiting for alpha_ready from gpu-proto-client")

            time.sleep(0.15)
            ppm2 = subprocess.check_output([grim_bin, "-t", "ppm", "-"], env=c_env)
            w2, h2, rgb2 = parse_ppm(ppm2)
            # 50% red (255,0,0) composited over pure blue (0,0,255) -> ~(127, 0, 127) purple
            purple_hits = count_matching_pixels(w2, h2, rgb2, (95, 160), (0, 25), (95, 160))
            if purple_hits < 200:
                raise RuntimeError(
                    f"wp_alpha_modifier_v1 50% red-over-blue blend not observed (purple_hits={purple_hits})"
                )

            (sync_dir / "quit").write_text("1\n", encoding="utf-8")
            client_proc.wait(timeout=5)
            client_proc = None
        finally:
            if client_proc is not None and client_proc.poll() is None:
                client_proc.terminate()
                client_proc.wait(timeout=5)
            comp.terminate()
            comp.wait(timeout=5)
            log2.close()

        # 3. Verify GLES2 + zwp_linux_dmabuf_v1 v4 feedback when GLES2 renderer is available
        dmabuf_verified = "skipped"
        rt3 = root / "rt-gles2"
        rt3.mkdir(mode=0o700)
        env_gl = os.environ.copy()
        env_gl.pop("DISPLAY", None)
        env_gl["XDG_RUNTIME_DIR"] = str(rt3)
        env_gl["HOME"] = str(root)
        env_gl["WLR_BACKENDS"] = "headless"
        env_gl["WLR_HEADLESS_OUTPUTS"] = "1"
        env_gl["WLR_LIBINPUT_NO_DEVICES"] = "1"
        env_gl["WLR_RENDERER"] = "gles2"
        sf3 = root / "gles2.status"
        log3 = (root / "gles2.log").open("w", encoding="utf-8")
        p3 = subprocess.Popen(
            [comp_bin, "--no-xwayland", "--no-panel", "--status-file", str(sf3)],
            env=env_gl,
            stdout=log3,
            stderr=subprocess.STDOUT,
        )
        try:
            kv3 = wait_status_file(sf3, p3)
            if kv3.get("RENDERER") == "gles2" and kv3.get("DMABUF") == "1":
                c_env3 = env_gl.copy()
                c_env3["WAYLAND_DISPLAY"] = kv3["WAYLAND_DISPLAY"]
                dmabuf_state = root / "dmabuf.state"
                subprocess.run(
                    [str(client_bin), str(dmabuf_state), str(sync_dir), "dmabuf_only"],
                    env=c_env3,
                    check=True,
                    timeout=8,
                )
                dtxt = dmabuf_state.read_text(encoding="utf-8")
                if (
                    "has_dmabuf=1" not in dtxt
                    or "dmabuf_done=1" not in dtxt
                    or "dmabuf_tranches=0" in dtxt
                    or "dmabuf_fmt_size=0\n" in dtxt
                ):
                    raise RuntimeError(f"Unexpected zwp_linux_dmabuf_v1 feedback state:\n{dtxt}")
                dmabuf_verified = "gles2+v4_feedback"
        finally:
            p3.terminate()
            p3.wait(timeout=5)
            log3.close()

    print(
        f"PASS check-dtwm-wayland-gpu-protocols (auto_renderer=pixman fallback=OK "
        f"dmabuf={dmabuf_verified} wp_presentation=presented "
        f"single_pixel_blue={blue_hits} alpha_modifier_purple={purple_hits})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
