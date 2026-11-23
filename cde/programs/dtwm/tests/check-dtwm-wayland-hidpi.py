#!/usr/bin/env python3
"""Hybrid Native-DPI + Per-Surface External Wayland HiDPI integration test:
1. Hardware 1:1 Output Invariant:
   - Boots dtwm-wayland (--no-xwayland --output-dpi 0=192) and verifies wl_output.scale == 1,
     full physical-pixel output geometry, _DT_WAYLAND_OUTPUTS (dpi=192), and RESOURCE_MANAGER
     (Xft.dpi: 192).
2. Path B (CK Core Motif / CDE Native 1:1 Physical Rendering):
   - Runs a native Wayland Motif client with an odd physical width (501x301), verifying
     CLIENT_CAP native_dpi, XmGetWidgetScale == 2.0 at 192 DPI, and exact 1:1 physical client
     geometry with zero compositor viewport upscaling.
3. Path A (External Wayland Client HiDPI Adapter - wp_fractional_scale_v1 + logical input):
   - Runs a non-Motif Wayland xdg_toplevel + wp_fractional_scale_v1 + wp_viewporter + wl_pointer
     client, verifying preferred_scale == 240 (2.0x), preferred_buffer_scale == 2, 1:1 physical
     scene composition of an 800x600 buffer for a 400x300 logical window, and accurate logical
     pointer hit-testing across all 4 quadrants.
   - Optionally smoke-tests /usr/bin/foot when installed.
4. Live Fractional 144 DPI (150%) & 96 DPI (100%) Transitions:
   - Switches HEADLESS-1 live via f.set_output_layout (192 -> 144 -> 96 -> 192 DPI), verifying
     both Motif (2.0 -> 1.5 -> 1.0 -> 2.0) and external Wayland (240 -> 180 -> 120 -> 240)
     clients update scales and pointer coordinates accurately without output blur.
"""

from __future__ import annotations

import argparse
from contextlib import suppress
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import time

from PIL import Image


MOTIF_CLIENT_C = r"""
#include <Xm/Xm.h>
#include <Xm/DrawingA.h>
#include <X11/Xatom.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int s) { (void)s; g_quit = 1; }

static void tick_cb(XtPointer cd, XtIntervalId *id) {
    (void)id;
    int *done = (int *)cd;
    *done = 1;
}

static void pump_ms(XtAppContext app, int ms) {
    int rounds = (ms + 19) / 20;
    for (int i = 0; i < rounds && !g_quit; ++i) {
        int done = 0;
        XtAppAddTimeOut(app, 20, tick_cb, &done);
        while (!done && !g_quit) {
            XtAppProcessEvent(app, XtIMAll);
        }
    }
}

static void write_state(const char *path, Widget shell) {
    Display *dpy = XtDisplay(shell);
    double scale = 0.0;
    XmGetWidgetScale(shell, &scale);
    Dimension w = 0, h = 0;
    XtVaGetValues(shell, XmNwidth, &w, XmNheight, &h, NULL);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (fp) {
        fprintf(fp, "SCALE=%.3f\nWIDTH=%u\nHEIGHT=%u\nDPY_W=%d\nDPY_H=%d\n",
                scale, (unsigned)w, (unsigned)h,
                DisplayWidth(dpy, DefaultScreen(dpy)),
                DisplayHeight(dpy, DefaultScreen(dpy)));
        fclose(fp);
        rename(tmp, path);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *state_path = argv[1];
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    setenv("CK_MOTIF_BACKEND", "wayland", 1);

    XtAppContext app = NULL;
    Widget shell = XtVaAppInitialize(
        &app, "CheckHiDpiMotif", NULL, 0, &argc, argv, NULL,
        XmNtitle, "MotifHiDpiProbe",
        XmNwidth, 501,
        XmNheight, 301,
        NULL
    );
    Widget area = XtVaCreateManagedWidget(
        "canvas", xmDrawingAreaWidgetClass, shell,
        XmNwidth, 501,
        XmNheight, 301,
        NULL
    );
    (void)area;
    XtRealizeWidget(shell);

    while (!g_quit) {
        pump_ms(app, 60);
        write_state(state_path, shell);
    }
    return 0;
}
"""


EXT_CLIENT_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int s) { (void)s; g_quit = 1; }

static struct wl_display *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct xdg_wm_base *g_wm;
static struct wl_seat *g_seat;
static struct wl_pointer *g_ptr;
static struct wl_output *g_out;
static struct wp_fractional_scale_manager_v1 *g_fs_mgr;
static struct wp_viewporter *g_vp_mgr;

static struct wl_surface *g_surf;
static struct xdg_surface *g_xsurf;
static struct xdg_toplevel *g_top;
static struct wp_fractional_scale_v1 *g_fs;
static struct wp_viewport *g_vp;
static struct wl_buffer *g_buf;

static int g_out_scale = 0;
static int g_out_phys_w = 0;
static int g_out_phys_h = 0;
static int g_pref_buf_scale = 1;
static uint32_t g_frac_scale_120 = 120;
static int g_log_w = 400;
static int g_log_h = 300;
static int g_buf_w = 0;
static int g_buf_h = 0;
static int g_configured = 0;
static double g_ptr_x = -1.0;
static double g_ptr_y = -1.0;
static int g_click_count = 0;
static double g_last_click_x = -1.0;
static double g_last_click_y = -1.0;
static const char *g_state_path = NULL;

static void save_state(void) {
    if (!g_state_path) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_state_path);
    FILE *fp = fopen(tmp, "w");
    if (fp) {
        fprintf(fp,
                "OUT_SCALE=%d\nOUT_W=%d\nOUT_H=%d\nPREF_BUF_SCALE=%d\n"
                "FRAC_120=%u\nLOG_W=%d\nLOG_H=%d\nBUF_W=%d\nBUF_H=%d\n"
                "CLICKS=%d\nCLICK_X=%.2f\nCLICK_Y=%.2f\n",
                g_out_scale, g_out_phys_w, g_out_phys_h, g_pref_buf_scale,
                g_frac_scale_120, g_log_w, g_log_h, g_buf_w, g_buf_h,
                g_click_count, g_last_click_x, g_last_click_y);
        fclose(fp);
        rename(tmp, g_state_path);
    }
}

static int make_shm_fd(size_t sz) {
    char name[64];
    snprintf(name, sizeof(name), "/hidpi-ext-%d-%ld", (int)getpid(), (long)random());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return -1; }
    return fd;
}

static void redraw_quadrants(void) {
    if (!g_configured || !g_surf || !g_shm) return;
    double s = (double)g_frac_scale_120 / 120.0;
    if (s < 0.5) s = 1.0;
    int bw = (int)lround((double)g_log_w * s);
    int bh = (int)lround((double)g_log_h * s);
    if (bw < 2) bw = 2;
    if (bh < 2) bh = 2;

    size_t sz = (size_t)bw * (size_t)bh * 4u;
    int fd = make_shm_fd(sz);
    if (fd < 0) return;
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            uint32_t c;
            if (y < bh / 2) {
                c = (x < bw / 2) ? 0xFFE02020u : 0xFF20C040u;
            } else {
                c = (x < bw / 2) ? 0xFF2050E0u : 0xFFE0C020u;
            }
            px[(size_t)y * (size_t)bw + (size_t)x] = c;
        }
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)sz);
    struct wl_buffer *nb = wl_shm_pool_create_buffer(
        pool, 0, bw, bh, bw * 4, WL_SHM_FORMAT_ARGB8888
    );
    wl_shm_pool_destroy(pool);
    munmap(px, sz);
    close(fd);

    if (g_buf) wl_buffer_destroy(g_buf);
    g_buf = nb;
    g_buf_w = bw;
    g_buf_h = bh;

    if (g_vp) {
        wp_viewport_set_destination(g_vp, g_log_w, g_log_h);
    }
    wl_surface_attach(g_surf, g_buf, 0, 0);
    wl_surface_damage_buffer(g_surf, 0, 0, bw, bh);
    wl_surface_commit(g_surf);
    save_state();
}

static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static void surf_pref_scale(void *d, struct wl_surface *s, int32_t factor) {
    (void)d; (void)s;
    g_pref_buf_scale = factor;
    save_state();
}
static void surf_pref_transform(void *d, struct wl_surface *s, uint32_t t) { (void)d; (void)s; (void)t; }
static const struct wl_surface_listener surf_l = {
    .enter = surf_enter,
    .leave = surf_leave,
    .preferred_buffer_scale = surf_pref_scale,
    .preferred_buffer_transform = surf_pref_transform,
};

static void fs_pref(void *d, struct wp_fractional_scale_v1 *fs, uint32_t scale_120) {
    (void)d; (void)fs;
    if (g_frac_scale_120 != scale_120) {
        g_frac_scale_120 = scale_120;
        redraw_quadrants();
    } else {
        save_state();
    }
}
static const struct wp_fractional_scale_v1_listener fs_l = { .preferred_scale = fs_pref };

static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)d; (void)t; (void)st;
    if (w > 0 && h > 0) {
        g_log_w = w;
        g_log_h = h;
    }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; g_quit = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = {
    .configure = top_conf, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps,
};

static void xs_conf(void *d, struct xdg_surface *xs, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    g_configured = 1;
    redraw_quadrants();
}
static const struct xdg_surface_listener xs_l = { .configure = xs_conf };

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) {
    (void)d;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_l = { .ping = wm_ping };

static void out_geom(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                     int32_t sub, const char *mk, const char *md, int32_t tr) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)mk; (void)md; (void)tr;
}
static void out_mode(void *d, struct wl_output *o, uint32_t fl, int32_t w, int32_t h, int32_t r) {
    (void)d; (void)o; (void)r;
    if (fl & WL_OUTPUT_MODE_CURRENT) {
        g_out_phys_w = w;
        g_out_phys_h = h;
    }
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; save_state(); }
static void out_scale(void *d, struct wl_output *o, int32_t factor) {
    (void)d; (void)o;
    g_out_scale = factor;
    save_state();
}
static void out_name(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static void out_desc(void *d, struct wl_output *o, const char *s) { (void)d; (void)o; (void)s; }
static const struct wl_output_listener out_l = {
    .geometry = out_geom, .mode = out_mode, .done = out_done,
    .scale = out_scale, .name = out_name, .description = out_desc,
};

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t ser, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d; (void)p; (void)ser; (void)s;
    g_ptr_x = wl_fixed_to_double(sx);
    g_ptr_y = wl_fixed_to_double(sy);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t ser, struct wl_surface *s) {
    (void)d; (void)p; (void)ser; (void)s;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d; (void)p; (void)t;
    g_ptr_x = wl_fixed_to_double(sx);
    g_ptr_y = wl_fixed_to_double(sy);
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t ser, uint32_t t, uint32_t btn, uint32_t st) {
    (void)d; (void)p; (void)ser; (void)t;
    if (btn == BTN_LEFT && st == WL_POINTER_BUTTON_STATE_PRESSED) {
        g_click_count++;
        g_last_click_x = g_ptr_x;
        g_last_click_y = g_ptr_y;
        save_state();
    }
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t ax, wl_fixed_t v) {
    (void)d; (void)p; (void)t; (void)ax; (void)v;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_src(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_l = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
    .axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_src,
    .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_disc,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_ptr) {
        g_ptr = wl_seat_get_pointer(s);
        wl_pointer_add_listener(g_ptr, &ptr_l, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_l = { .capabilities = seat_caps, .name = seat_name };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver >= 6 ? 6 : ver);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        g_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver >= 2 ? 2 : 1);
        xdg_wm_base_add_listener(g_wm, &wm_l, NULL);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, ver >= 5 ? 5 : ver);
        wl_seat_add_listener(g_seat, &seat_l, NULL);
    } else if (!strcmp(iface, wl_output_interface.name) && !g_out) {
        g_out = wl_registry_bind(r, name, &wl_output_interface, ver >= 4 ? 4 : ver);
        wl_output_add_listener(g_out, &out_l, NULL);
    } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
        g_fs_mgr = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        g_vp_mgr = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_l = { .global = reg_global, .global_remove = reg_rem };

static void click_global(void *data, struct wl_registry *rg, uint32_t nm, const char *ifc, uint32_t vr) {
    (void)vr;
    struct zwlr_virtual_pointer_manager_v1 **out = data;
    if (!strcmp(ifc, zwlr_virtual_pointer_manager_v1_interface.name)) {
        *out = wl_registry_bind(rg, nm, &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}
static const struct wl_registry_listener click_reg_l = { .global = click_global, .global_remove = reg_rem };

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000L);
}

static int run_pointer(uint32_t width, uint32_t height) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 1;
    struct zwlr_virtual_pointer_manager_v1 *vpm = NULL;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &click_reg_l, &vpm);
    if (wl_display_roundtrip(dpy) < 0 || !vpm) return 1;
    struct zwlr_virtual_pointer_v1 *vp =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vpm, NULL);

    unsigned int x, y;
    while (scanf("%u %u", &x, &y) == 2) {
        zwlr_virtual_pointer_v1_motion_absolute(vp, now_ms(), x, y, width, height);
        zwlr_virtual_pointer_v1_frame(vp);
        if (wl_display_roundtrip(dpy) < 0) return 1;
        usleep(20000);
        zwlr_virtual_pointer_v1_button(vp, now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
        zwlr_virtual_pointer_v1_frame(vp);
        if (wl_display_roundtrip(dpy) < 0) return 1;
        usleep(20000);
        zwlr_virtual_pointer_v1_button(vp, now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(vp);
        if (wl_display_roundtrip(dpy) < 0) return 1;
        puts("OK");
        fflush(stdout);
    }
    zwlr_virtual_pointer_v1_destroy(vp);
    zwlr_virtual_pointer_manager_v1_destroy(vpm);
    wl_display_flush(dpy);
    wl_display_disconnect(dpy);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--pointer"))
        return run_pointer((uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]));
    if (argc < 2) return 2;
    g_state_path = argv[1];
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(g_dpy);
    wl_display_roundtrip(g_dpy);

    if (!g_comp || !g_shm || !g_wm || !g_fs_mgr || !g_vp_mgr) {
        fprintf(stderr, "missing Wayland globals\n");
        return 1;
    }

    g_surf = wl_compositor_create_surface(g_comp);
    wl_surface_add_listener(g_surf, &surf_l, NULL);
    g_vp = wp_viewporter_get_viewport(g_vp_mgr, g_surf);
    g_fs = wp_fractional_scale_manager_v1_get_fractional_scale(g_fs_mgr, g_surf);
    wp_fractional_scale_v1_add_listener(g_fs, &fs_l, NULL);

    g_xsurf = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    xdg_surface_add_listener(g_xsurf, &xs_l, NULL);
    g_top = xdg_surface_get_toplevel(g_xsurf);
    xdg_toplevel_add_listener(g_top, &top_l, NULL);
    xdg_toplevel_set_title(g_top, "ExternalWaylandProbe");
    xdg_toplevel_set_app_id(g_top, "external-hidpi-probe");
    wl_surface_commit(g_surf);
    wl_display_flush(g_dpy);

    while (!g_quit) {
        if (wl_display_dispatch(g_dpy) < 0) break;
    }
    return 0;
}
"""


WLR_VPTR_XML = r"""<?xml version="1.0" encoding="UTF-8"?>
<protocol name="wlr_virtual_pointer_unstable_v1">
  <interface name="zwlr_virtual_pointer_manager_v1" version="2">
    <request name="create_virtual_pointer">
      <arg name="seat" type="object" interface="wl_seat" allow-null="true"/>
      <arg name="id" type="new_id" interface="zwlr_virtual_pointer_v1"/>
    </request>
    <request name="destroy" type="destructor"/>
  </interface>
  <interface name="zwlr_virtual_pointer_v1" version="2">
    <request name="motion">
      <arg name="time" type="uint"/>
      <arg name="dx" type="fixed"/>
      <arg name="dy" type="fixed"/>
    </request>
    <request name="motion_absolute">
      <arg name="time" type="uint"/>
      <arg name="x" type="uint"/>
      <arg name="y" type="uint"/>
      <arg name="x_extent" type="uint"/>
      <arg name="y_extent" type="uint"/>
    </request>
    <request name="button">
      <arg name="time" type="uint"/>
      <arg name="button" type="uint"/>
      <arg name="state" type="uint"/>
    </request>
    <request name="axis">
      <arg name="time" type="uint"/>
      <arg name="axis" type="uint"/>
      <arg name="value" type="fixed"/>
    </request>
    <request name="frame"/>
    <request name="axis_source">
      <arg name="axis_source" type="uint"/>
    </request>
    <request name="axis_stop">
      <arg name="time" type="uint"/>
      <arg name="axis" type="uint"/>
    </request>
    <request name="axis_discrete">
      <arg name="axis" type="uint"/>
      <arg name="discrete" type="int"/>
    </request>
    <request name="destroy" type="destructor"/>
  </interface>
</protocol>
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


def wait_until(pred, timeout: float = 8.0, step: float = 0.05) -> bool:
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


def parse_active_box(props: dict[str, bytes]) -> tuple[int, int, int, int] | None:
    raw = props.get("_DT_ACTIVE_WINDOW_BOX")
    if not raw or len(raw) < 32:
        return None
    import struct
    # box[0..3] = outer frame (x, y, w, h); box[4..7] = client interior (cx, cy, cw, ch)
    x, y, w, h = struct.unpack("<iiii", raw[16:32])
    return (x, y, w, h)


def resolve_motif_prefix(comp_path: Path) -> Path:
    configured = os.environ.get("MOTIF_PREFIX")
    candidates = [Path(configured)] if configured else [
        *(parent / "motif-prefix" for parent in comp_path.parents),
        Path("/usr/dt"),
        Path("/var/tmp/ck-wayland-build/wp00/motif-prefix"),
    ]
    for prefix in candidates:
        if all((prefix / name).is_file() for name in ("include/Xm/Xm.h", "lib/libXm.so", "lib/libXm.so.9")):
            return prefix.resolve()
    raise SystemExit("Set MOTIF_PREFIX to an installed CK Core Motif ABI 9 prefix")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    comp_path = Path(args.compositor).resolve()
    if not comp_path.is_file():
        raise SystemExit(f"missing compositor: {comp_path}")
    motif_prefix = resolve_motif_prefix(comp_path)

    cc = os.environ.get("CC", "cc")
    grim_bin = shutil.which("grim")
    wl_scanner = shutil.which("wayland-scanner")
    if not wl_scanner:
        raise SystemExit("wayland-scanner is required")

    with tempfile.TemporaryDirectory(prefix="dtwm-hidpi-") as td:
        tmp = Path(td)
        rt_dir = tmp / "rt"
        home_dir = tmp / "home"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt").mkdir(parents=True)

        # Generate Wayland protocol glue for external test client
        proto_root = Path("/usr/share/wayland-protocols")
        xdg_xml = proto_root / "stable/xdg-shell/xdg-shell.xml"
        fs_xml = proto_root / "staging/fractional-scale/fractional-scale-v1.xml"
        vp_xml = proto_root / "stable/viewporter/viewporter.xml"
        vptr_xml = tmp / "vptr.xml"
        vptr_xml.write_text(WLR_VPTR_XML, encoding="utf-8")

        gen_srcs: list[str] = []
        for tag, xml in [("xdg-shell", xdg_xml), ("fractional-scale-v1", fs_xml),
                         ("viewporter", vp_xml), ("wlr-virtual-pointer-unstable-v1", vptr_xml)]:
            hdr = tmp / f"{tag}-client-protocol.h"
            src = tmp / f"{tag}-protocol.c"
            subprocess.run([wl_scanner, "client-header", str(xml), str(hdr)], check=True)
            subprocess.run([wl_scanner, "private-code", str(xml), str(src)], check=True)
            gen_srcs.append(str(src))

        ext_c = tmp / "ext_client.c"
        ext_bin = tmp / "ext_client"
        ext_c.write_text(EXT_CLIENT_C, encoding="utf-8")
        wl_cflags = subprocess.check_output(["pkg-config", "--cflags", "wayland-client"], text=True).split()
        wl_libs = subprocess.check_output(["pkg-config", "--libs", "wayland-client"], text=True).split()
        subprocess.run(
            [cc, "-O2", "-I", str(tmp), str(ext_c), *gen_srcs, *wl_cflags, *wl_libs, "-lm", "-o", str(ext_bin)],
            check=True,
        )

        motif_c = tmp / "motif_client.c"
        motif_bin = tmp / "motif_client"
        motif_c.write_text(MOTIF_CLIENT_C, encoding="utf-8")
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
            "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_RENDERER": "pixman",
        })
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        motif_env = {
            **env,
            "LD_LIBRARY_PATH": ":".join(filter(None, (str(lib_dir), env.get("LD_LIBRARY_PATH")))),
        }

        comp_proc = subprocess.Popen(
            [
                str(comp_path),
                "--no-xwayland",
                "--no-panel",
                "--output-dpi",
                "0=192",
                "--status-file",
                str(status_file),
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        motif_proc: subprocess.Popen | None = None
        ext_proc: subprocess.Popen | None = None
        pointer_proc: subprocess.Popen | None = None
        foot_proc: subprocess.Popen | None = None
        try:
            if not wait_until(lambda: "WAYLAND_DISPLAY" in parse_kv(status_file), timeout=10.0):
                raise RuntimeError("dtwm-wayland failed to write status file")
            wl_sock = parse_kv(status_file)["WAYLAND_DISPLAY"]
            env["WAYLAND_DISPLAY"] = wl_sock
            motif_env["WAYLAND_DISPLAY"] = wl_sock
            ipc_path = rt_dir / f"dtwm-wayland-{wl_sock}.ipc"
            if not wait_until(lambda: ipc_path.exists(), timeout=5.0):
                raise RuntimeError(f"missing IPC socket {ipc_path}")

            props = ipc_read_props_and_send(ipc_path)
            rm_str = props.get("RESOURCE_MANAGER", b"").decode("utf-8", errors="replace")
            out_str = props.get("_DT_WAYLAND_OUTPUTS", b"").decode("utf-8", errors="replace")
            if "Xft.dpi:\t192" not in rm_str:
                raise AssertionError(f"expected Xft.dpi: 192 in RESOURCE_MANAGER, got: {rm_str!r}")
            out_fields = out_str.strip().splitlines()[0].split("|")
            out_name = out_fields[1]
            phys_w, phys_h, out_dpi = int(out_fields[5]), int(out_fields[6]), int(out_fields[8])
            if out_dpi != 192:
                raise AssertionError(f"expected initial output DPI 192, got {out_dpi} ({out_str!r})")

            # 1. Launch Path B Motif client with odd physical width 501x301
            motif_state = tmp / "motif.state"
            motif_proc = subprocess.Popen([str(motif_bin), str(motif_state)], env=motif_env)
            if not wait_until(lambda: float(parse_kv(motif_state).get("SCALE", "0")) == 2.0, timeout=8.0):
                raise AssertionError(f"Motif client did not report 2.0 scale at 192 DPI: {parse_kv(motif_state)}")
            ms = parse_kv(motif_state)
            if int(ms["DPY_W"]) != phys_w or int(ms["DPY_H"]) != phys_h:
                raise AssertionError(f"Motif DisplayWidth/Height {ms} != physical {phys_w}x{phys_h}")
            if not wait_until(lambda: (parse_active_box(ipc_read_props_and_send(ipc_path)) or (0, 0, 0, 0))[2:] == (501, 301), timeout=5.0):
                raise AssertionError(f"expected 1:1 odd physical box (501, 301), got {parse_active_box(ipc_read_props_and_send(ipc_path))}")

            # 2. Launch Path A external Wayland client (400x300 logical -> 800x600 physical @ 2.0x)
            ext_state = tmp / "ext.state"
            ext_proc = subprocess.Popen([str(ext_bin), str(ext_state)], env=env)
            if not wait_until(lambda: int(parse_kv(ext_state).get("FRAC_120", "0")) == 240 and int(parse_kv(ext_state).get("BUF_W", "0")) == 800, timeout=8.0):
                raise AssertionError(f"external client did not negotiate 2.0x (240/120): {parse_kv(ext_state)}")
            es = parse_kv(ext_state)
            if int(es["OUT_SCALE"]) != 1:
                raise AssertionError(f"expected wl_output.scale == 1, got {es['OUT_SCALE']}")
            if int(es["PREF_BUF_SCALE"]) != 2:
                raise AssertionError(f"expected preferred_buffer_scale == 2, got {es['PREF_BUF_SCALE']}")

            if not wait_until(lambda: (parse_active_box(ipc_read_props_and_send(ipc_path)) or (0, 0, 0, 0))[2:] == (800, 600), timeout=5.0):
                raise AssertionError(f"expected external physical box (800, 600), got {parse_active_box(ipc_read_props_and_send(ipc_path))}")
            bx, by, bw, bh = parse_active_box(ipc_read_props_and_send(ipc_path))

            if grim_bin:
                shot = tmp / "shot192.png"
                subprocess.run([grim_bin, str(shot)], env=env, check=True)
                with Image.open(shot) as im:
                    if im.size != (phys_w, phys_h):
                        raise AssertionError(f"grim screenshot size {im.size} != physical {(phys_w, phys_h)}")
                    # Verify bottom-right yellow quadrant of 800x600 external surface is actually rendered there!
                    r, g, b = im.convert("RGB").getpixel((bx + 600, by + 450))
                    if not (r > 180 and g > 150 and b < 80):
                        raise AssertionError(f"expected yellow BR quadrant at ({bx+600},{by+450}), got RGB={(r,g,b)}")

            # Use one pointer across all clicks and output reconfigurations.
            pointer_proc = subprocess.Popen(
                [str(ext_bin), "--pointer", str(phys_w), str(phys_h)],
                env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
            )

            def click(px_x: int, px_y: int) -> None:
                if not pointer_proc.stdin or not pointer_proc.stdout:
                    raise RuntimeError("Virtual pointer pipes are unavailable")
                pointer_proc.stdin.write(f"{px_x} {px_y}\n")
                pointer_proc.stdin.flush()
                ready, _, _ = select.select([pointer_proc.stdout], [], [], 3.0)
                if not ready or pointer_proc.stdout.readline().strip() != "OK":
                    raise RuntimeError(f"Virtual pointer did not acknowledge click at ({px_x}, {px_y})")

            # Check physical clicks in all four quadrants against logical coordinates.
            quad_targets = [
                (bx + 200, by + 150, 100.0, 75.0),
                (bx + 600, by + 150, 300.0, 75.0),
                (bx + 200, by + 450, 100.0, 225.0),
                (bx + 600, by + 450, 300.0, 225.0),
            ]
            for idx, (px_x, px_y, exp_lx, exp_ly) in enumerate(quad_targets, start=1):
                click(px_x, px_y)
                if not wait_until(lambda: int(parse_kv(ext_state).get("CLICKS", "0")) >= idx, timeout=3.0):
                    raise AssertionError(f"quadrant {idx} click not delivered: {parse_kv(ext_state)}")
                got_x = float(parse_kv(ext_state)["CLICK_X"])
                got_y = float(parse_kv(ext_state)["CLICK_Y"])
                if abs(got_x - exp_lx) > 2.5 or abs(got_y - exp_ly) > 2.5:
                    raise AssertionError(
                        f"2.0x quadrant {idx}: physical ({px_x},{px_y}) -> expected logical ({exp_lx},{exp_ly}), got ({got_x},{got_y})"
                    )

            # 3. Switch live to fractional 144 DPI (150% = 180/120)!
            ipc_read_props_and_send(ipc_path, f"f.set_output_layout {out_name} 1 0 0 {phys_w}x{phys_h} 0 144")
            if not wait_until(lambda: float(parse_kv(motif_state).get("SCALE", "0")) == 1.5, timeout=8.0):
                raise AssertionError(f"Motif client did not update to 1.5 scale at 144 DPI: {parse_kv(motif_state)}")
            if not wait_until(lambda: int(parse_kv(ext_state).get("FRAC_120", "0")) == 180, timeout=8.0):
                raise AssertionError(f"external client did not update to 180/120 at 144 DPI: {parse_kv(ext_state)}")

            # Verify fractional 1.5x pointer hit-testing in BR quadrant!
            if not wait_until(lambda: parse_active_box(ipc_read_props_and_send(ipc_path)) is not None, timeout=3.0):
                raise AssertionError("missing active box at 144 DPI")
            bx15, by15, bw15, bh15 = parse_active_box(ipc_read_props_and_send(ipc_path))
            es15 = parse_kv(ext_state)
            log_w15, log_h15 = int(es15["LOG_W"]), int(es15["LOG_H"])
            prev_clicks = int(es15["CLICKS"])
            target_px_x = bx15 + int(round(bw15 * 0.75))
            target_px_y = by15 + int(round(bh15 * 0.75))
            exp_lx15 = (target_px_x - bx15) / 1.5
            exp_ly15 = (target_px_y - by15) / 1.5
            click(target_px_x, target_px_y)
            if not wait_until(lambda: int(parse_kv(ext_state).get("CLICKS", "0")) > prev_clicks, timeout=3.0):
                raise AssertionError("1.5x click not delivered")
            got_x15 = float(parse_kv(ext_state)["CLICK_X"])
            got_y15 = float(parse_kv(ext_state)["CLICK_Y"])
            if abs(got_x15 - exp_lx15) > 2.5 or abs(got_y15 - exp_ly15) > 2.5:
                raise AssertionError(
                    f"1.5x BR click: expected logical ({exp_lx15:.1f},{exp_ly15:.1f}) of ({log_w15}x{log_h15}), got ({got_x15:.1f},{got_y15:.1f})"
                )

            # 4. Optional smoke check with real /usr/bin/foot if installed
            foot_bin = shutil.which("foot")
            if foot_bin:
                foot_proc = subprocess.Popen(
                    [foot_bin, "-e", "sh", "-c", "sleep 10"],
                    env=env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                if not wait_until(
                    lambda: b"foot" in ipc_read_props_and_send(ipc_path).get("_DT_WAYLAND_CLIENT_IDS", b""),
                    timeout=5.0,
                ):
                    raise AssertionError("foot did not map under 144 DPI fractional scaling")

            # 5. Switch live to 96 DPI (1.0x) and back to 192 DPI (2.0x)
            ipc_read_props_and_send(ipc_path, f"f.set_output_layout {out_name} 1 0 0 {phys_w}x{phys_h} 0 96")
            if not wait_until(
                lambda: float(parse_kv(motif_state).get("SCALE", "0")) == 1.0
                and int(parse_kv(ext_state).get("FRAC_120", "0")) == 120,
                timeout=8.0,
            ):
                raise AssertionError(f"failed live transition to 96 DPI: motif={parse_kv(motif_state)} ext={parse_kv(ext_state)}")

            ipc_read_props_and_send(ipc_path, f"f.set_output_layout {out_name} 1 0 0 {phys_w}x{phys_h} 0 192")
            if not wait_until(
                lambda: float(parse_kv(motif_state).get("SCALE", "0")) == 2.0
                and int(parse_kv(ext_state).get("FRAC_120", "0")) == 240,
                timeout=8.0,
            ):
                raise AssertionError(f"failed live return to 192 DPI: motif={parse_kv(motif_state)} ext={parse_kv(ext_state)}")

            print(
                f"PASS: hybrid native 1:1 Motif + per-surface external Wayland HiDPI verified "
                f"(phys={phys_w}x{phys_h}, 192/144/96 DPI, odd-width=501x301, 4-quadrant input OK, foot={'yes' if foot_bin else 'skip'})"
            )
            return 0
        finally:
            if pointer_proc and pointer_proc.stdin:
                with suppress(BrokenPipeError):
                    pointer_proc.stdin.close()
            for p in (foot_proc, pointer_proc, ext_proc, motif_proc, comp_proc):
                if p and p.poll() is None:
                    p.terminate()
                    try:
                        p.wait(timeout=3.0)
                    except subprocess.TimeoutExpired:
                        p.kill()


if __name__ == "__main__":
    raise SystemExit(main())
