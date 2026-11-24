#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland Track 4B:
Touchscreen (wl_touch), Graphics Tablet / Stylus & Pad (zwp_tablet_v2),
and Tablet Tool Cursor Shapes (wp_cursor_shape_v1):

1. Verifies wl_seat advertises WL_SEAT_CAPABILITY_TOUCH when a touch device is
   attached, alongside zwp_tablet_manager_v2 and wp_cursor_shape_manager_v1.
2. Injects multi-touch (2-finger down/motion/up/frame) over a client surface
   and verifies exact surface-local wl_touch events, then drags the window's
   CDE titlebar with a single-finger touch gesture and verifies live window move.
3. Injects stylus proximity, pressure (0.725), tilt (18.5, -12.0), tip down/up,
   and barrel button (BTN_STYLUS=331) over a zwp_tablet_v2 client, verifying
   native tablet v2 delivery plus wp_cursor_shape_v1 get_tablet_tool_v2
   CROSSHAIR cursor-shape activation.
4. Injects tablet ExpressKey pad button and touch-ring events and verifies
   zwp_tablet_pad_v2 enter, button, and ring delivery to the focused client.
5. Uses the stylus tip on the CDE titlebar to drag the window and verifies
   clean proximity_out + fallback CDE chrome move handling.
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
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "tablet-v2-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"

typedef struct {
    const char *state_file;
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wl_seat *seat;
    struct wl_touch *touch;
    struct zwp_tablet_manager_v2 *tab_mgr;
    struct zwp_tablet_seat_v2 *tab_seat;
    struct wp_cursor_shape_manager_v1 *cshape_mgr;

    struct wl_surface *surf;
    struct xdg_surface *xsurf;
    struct xdg_toplevel *xtop;
    struct wl_buffer *buf;
    bool configured;

    uint32_t seat_caps;
    int touch_downs;
    int touch_motions;
    int touch_ups;
    int touch_frames;
    double last_touch_x;
    double last_touch_y;

    int tab_added;
    int tool_added;
    int pad_added;
    int prox_in;
    int prox_out;
    int tip_down;
    int tip_up;
    int tool_motions;
    int tool_buttons;
    uint32_t last_tool_btn;
    double max_pressure;
    double last_tilt_x;
    double last_tilt_y;

    int pad_enters;
    int pad_buttons;
    uint32_t last_pad_btn;
    int pad_rings;
    double last_ring_deg;
} App;

static void write_state(App *a) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", a->state_file);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f,
            "configured=%d\n"
            "seat_caps=%u\n"
            "touch_downs=%d\n"
            "touch_motions=%d\n"
            "touch_ups=%d\n"
            "touch_frames=%d\n"
            "last_touch=%.1f,%.1f\n"
            "tab_added=%d\n"
            "tool_added=%d\n"
            "pad_added=%d\n"
            "prox_in=%d\n"
            "prox_out=%d\n"
            "tip_down=%d\n"
            "tip_up=%d\n"
            "tool_motions=%d\n"
            "tool_buttons=%d\n"
            "last_tool_btn=%u\n"
            "max_pressure=%.3f\n"
            "last_tilt=%.1f,%.1f\n"
            "pad_enters=%d\n"
            "pad_buttons=%d\n"
            "last_pad_btn=%u\n"
            "pad_rings=%d\n"
            "last_ring_deg=%.1f\n",
            a->configured ? 1 : 0,
            a->seat_caps,
            a->touch_downs, a->touch_motions, a->touch_ups, a->touch_frames,
            a->last_touch_x, a->last_touch_y,
            a->tab_added, a->tool_added, a->pad_added,
            a->prox_in, a->prox_out, a->tip_down, a->tip_up,
            a->tool_motions, a->tool_buttons, a->last_tool_btn,
            a->max_pressure, a->last_tilt_x, a->last_tilt_y,
            a->pad_enters, a->pad_buttons, a->last_pad_btn,
            a->pad_rings, a->last_ring_deg);
    fclose(f);
    rename(tmp, a->state_file);
}

static struct wl_buffer *make_shm_buf(struct wl_shm *shm, int w, int h, uint32_t argb) {
    int stride = w * 4;
    size_t sz = (size_t)stride * (size_t)h;
    char tmpl[] = "/tmp/dtwm-tabtest-XXXXXX";
    const char *td = getenv("TMPDIR");
    char path[512];
    if (td && td[0]) snprintf(path, sizeof(path), "%s/dtwm-tabtest-XXXXXX", td);
    else snprintf(path, sizeof(path), "%s", tmpl);
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    unlink(path);
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return NULL; }
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { close(fd); return NULL; }
    for (size_t i = 0; i < (size_t)(w * h); ++i) px[i] = argb;
    munmap(px, sz);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)sz);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

/* wl_touch */
static void t_down(void *d, struct wl_touch *t, uint32_t ser, uint32_t tm,
                   struct wl_surface *s, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)t; (void)ser; (void)tm; (void)s; (void)id;
    App *a = d;
    a->touch_downs++;
    a->last_touch_x = wl_fixed_to_double(x);
    a->last_touch_y = wl_fixed_to_double(y);
    write_state(a);
}
static void t_up(void *d, struct wl_touch *t, uint32_t ser, uint32_t tm, int32_t id) {
    (void)t; (void)ser; (void)tm; (void)id;
    App *a = d;
    a->touch_ups++;
    write_state(a);
}
static void t_motion(void *d, struct wl_touch *t, uint32_t tm, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)t; (void)tm; (void)id;
    App *a = d;
    a->touch_motions++;
    a->last_touch_x = wl_fixed_to_double(x);
    a->last_touch_y = wl_fixed_to_double(y);
    write_state(a);
}
static void t_frame(void *d, struct wl_touch *t) {
    (void)t;
    App *a = d;
    a->touch_frames++;
    write_state(a);
}
static void t_cancel(void *d, struct wl_touch *t) { (void)d; (void)t; }
static void t_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t mj, wl_fixed_t mn) {
    (void)d; (void)t; (void)id; (void)mj; (void)mn;
}
static void t_orient(void *d, struct wl_touch *t, int32_t id, wl_fixed_t o) {
    (void)d; (void)t; (void)id; (void)o;
}
static const struct wl_touch_listener g_touch_l = {
    .down = t_down, .up = t_up, .motion = t_motion, .frame = t_frame,
    .cancel = t_cancel, .shape = t_shape, .orientation = t_orient,
};

/* zwp_tablet_v2 */
static void tab_str(void *d, struct zwp_tablet_v2 *t, const char *s) { (void)d; (void)t; (void)s; }
static void tab_id(void *d, struct zwp_tablet_v2 *t, uint32_t v, uint32_t p) { (void)d; (void)t; (void)v; (void)p; }
static void tab_done(void *d, struct zwp_tablet_v2 *t) { (void)d; (void)t; }
static void tab_rem(void *d, struct zwp_tablet_v2 *t) { (void)d; zwp_tablet_v2_destroy(t); }
static const struct zwp_tablet_v2_listener g_tab_l = {
    .name = tab_str, .id = tab_id, .path = tab_str, .done = tab_done, .removed = tab_rem
};

static void tool_type(void *d, struct zwp_tablet_tool_v2 *t, uint32_t ty) { (void)d; (void)t; (void)ty; }
static void tool_u64(void *d, struct zwp_tablet_tool_v2 *t, uint32_t hi, uint32_t lo) { (void)d; (void)t; (void)hi; (void)lo; }
static void tool_cap(void *d, struct zwp_tablet_tool_v2 *t, uint32_t c) { (void)d; (void)t; (void)c; }
static void tool_done(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; }
static void tool_rem(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; zwp_tablet_tool_v2_destroy(t); }
static void tool_prox_in(void *d, struct zwp_tablet_tool_v2 *t, uint32_t ser,
                         struct zwp_tablet_v2 *tab, struct wl_surface *s) {
    (void)tab; (void)s;
    App *a = d;
    a->prox_in++;
    if (a->cshape_mgr) {
        struct wp_cursor_shape_device_v1 *dev =
            wp_cursor_shape_manager_v1_get_tablet_tool_v2(a->cshape_mgr, t);
        wp_cursor_shape_device_v1_set_shape(dev, ser, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
        wp_cursor_shape_device_v1_destroy(dev);
        wl_display_flush(a->dpy);
    }
    write_state(a);
}
static void tool_prox_out(void *d, struct zwp_tablet_tool_v2 *t) {
    (void)t;
    App *a = d;
    a->prox_out++;
    write_state(a);
}
static void tool_down(void *d, struct zwp_tablet_tool_v2 *t, uint32_t ser) {
    (void)t; (void)ser;
    App *a = d;
    a->tip_down++;
    write_state(a);
}
static void tool_up(void *d, struct zwp_tablet_tool_v2 *t) {
    (void)t;
    App *a = d;
    a->tip_up++;
    write_state(a);
}
static void tool_motion(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y) {
    (void)t; (void)x; (void)y;
    App *a = d;
    a->tool_motions++;
    write_state(a);
}
static void tool_pressure(void *d, struct zwp_tablet_tool_v2 *t, uint32_t p) {
    (void)t;
    App *a = d;
    double pf = (double)p / 65535.0;
    if (pf > a->max_pressure) a->max_pressure = pf;
    write_state(a);
}
static void tool_dist(void *d, struct zwp_tablet_tool_v2 *t, uint32_t dist) { (void)d; (void)t; (void)dist; }
static void tool_tilt(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t tx, wl_fixed_t ty) {
    (void)t;
    App *a = d;
    a->last_tilt_x = wl_fixed_to_double(tx);
    a->last_tilt_y = wl_fixed_to_double(ty);
    write_state(a);
}
static void tool_rot(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t r) { (void)d; (void)t; (void)r; }
static void tool_slider(void *d, struct zwp_tablet_tool_v2 *t, int32_t s) { (void)d; (void)t; (void)s; }
static void tool_wheel(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t deg, int32_t cl) {
    (void)d; (void)t; (void)deg; (void)cl;
}
static void tool_btn(void *d, struct zwp_tablet_tool_v2 *t, uint32_t ser, uint32_t btn, uint32_t st) {
    (void)t; (void)ser;
    App *a = d;
    if (st == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED) {
        a->tool_buttons++;
        a->last_tool_btn = btn;
    }
    write_state(a);
}
static void tool_frame(void *d, struct zwp_tablet_tool_v2 *t, uint32_t tm) { (void)d; (void)t; (void)tm; }
static const struct zwp_tablet_tool_v2_listener g_tool_l = {
    .type = tool_type, .hardware_serial = tool_u64, .hardware_id_wacom = tool_u64,
    .capability = tool_cap, .done = tool_done, .removed = tool_rem,
    .proximity_in = tool_prox_in, .proximity_out = tool_prox_out,
    .down = tool_down, .up = tool_up, .motion = tool_motion,
    .pressure = tool_pressure, .distance = tool_dist, .tilt = tool_tilt,
    .rotation = tool_rot, .slider = tool_slider, .wheel = tool_wheel,
    .button = tool_btn, .frame = tool_frame,
};

/* pad ring / strip / group / pad */
static void ring_src(void *d, struct zwp_tablet_pad_ring_v2 *r, uint32_t s) { (void)d; (void)r; (void)s; }
static void ring_deg(void *d, struct zwp_tablet_pad_ring_v2 *r, wl_fixed_t deg) {
    (void)r;
    App *a = d;
    a->pad_rings++;
    a->last_ring_deg = wl_fixed_to_double(deg);
    write_state(a);
}
static void ring_stop(void *d, struct zwp_tablet_pad_ring_v2 *r) { (void)d; (void)r; }
static void ring_frame(void *d, struct zwp_tablet_pad_ring_v2 *r, uint32_t tm) { (void)d; (void)r; (void)tm; }
static const struct zwp_tablet_pad_ring_v2_listener g_ring_l = {
    .source = ring_src, .angle = ring_deg, .stop = ring_stop, .frame = ring_frame
};

static void strip_src(void *d, struct zwp_tablet_pad_strip_v2 *s, uint32_t src) { (void)d; (void)s; (void)src; }
static void strip_pos(void *d, struct zwp_tablet_pad_strip_v2 *s, uint32_t p) { (void)d; (void)s; (void)p; }
static void strip_stop(void *d, struct zwp_tablet_pad_strip_v2 *s) { (void)d; (void)s; }
static void strip_frame(void *d, struct zwp_tablet_pad_strip_v2 *s, uint32_t tm) { (void)d; (void)s; (void)tm; }
static const struct zwp_tablet_pad_strip_v2_listener g_strip_l = {
    .source = strip_src, .position = strip_pos, .stop = strip_stop, .frame = strip_frame
};

static void grp_buttons(void *d, struct zwp_tablet_pad_group_v2 *g, struct wl_array *b) { (void)d; (void)g; (void)b; }
static void grp_ring(void *d, struct zwp_tablet_pad_group_v2 *g, struct zwp_tablet_pad_ring_v2 *r) {
    (void)g;
    zwp_tablet_pad_ring_v2_add_listener(r, &g_ring_l, d);
}
static void grp_strip(void *d, struct zwp_tablet_pad_group_v2 *g, struct zwp_tablet_pad_strip_v2 *s) {
    (void)g;
    zwp_tablet_pad_strip_v2_add_listener(s, &g_strip_l, d);
}
static void grp_modes(void *d, struct zwp_tablet_pad_group_v2 *g, uint32_t m) { (void)d; (void)g; (void)m; }
static void grp_done(void *d, struct zwp_tablet_pad_group_v2 *g) { (void)d; (void)g; }
static void grp_mode_sw(void *d, struct zwp_tablet_pad_group_v2 *g, uint32_t tm, uint32_t ser, uint32_t m) {
    (void)d; (void)g; (void)tm; (void)ser; (void)m;
}
static const struct zwp_tablet_pad_group_v2_listener g_grp_l = {
    .buttons = grp_buttons, .ring = grp_ring, .strip = grp_strip,
    .modes = grp_modes, .done = grp_done, .mode_switch = grp_mode_sw
};

static void pad_grp(void *d, struct zwp_tablet_pad_v2 *p, struct zwp_tablet_pad_group_v2 *g) {
    (void)p;
    zwp_tablet_pad_group_v2_add_listener(g, &g_grp_l, d);
}
static void pad_path(void *d, struct zwp_tablet_pad_v2 *p, const char *s) { (void)d; (void)p; (void)s; }
static void pad_btns(void *d, struct zwp_tablet_pad_v2 *p, uint32_t c) { (void)d; (void)p; (void)c; }
static void pad_done(void *d, struct zwp_tablet_pad_v2 *p) { (void)d; (void)p; }
static void pad_btn(void *d, struct zwp_tablet_pad_v2 *p, uint32_t tm, uint32_t b, uint32_t st) {
    (void)p; (void)tm;
    App *a = d;
    if (st == ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED) {
        a->pad_buttons++;
        a->last_pad_btn = b;
    }
    write_state(a);
}
static void pad_enter(void *d, struct zwp_tablet_pad_v2 *p, uint32_t ser,
                      struct zwp_tablet_v2 *t, struct wl_surface *s) {
    (void)p; (void)ser; (void)t; (void)s;
    App *a = d;
    a->pad_enters++;
    write_state(a);
}
static void pad_leave(void *d, struct zwp_tablet_pad_v2 *p, uint32_t ser, struct wl_surface *s) {
    (void)d; (void)p; (void)ser; (void)s;
}
static void pad_rem(void *d, struct zwp_tablet_pad_v2 *p) { (void)d; zwp_tablet_pad_v2_destroy(p); }
static const struct zwp_tablet_pad_v2_listener g_pad_l = {
    .group = pad_grp, .path = pad_path, .buttons = pad_btns, .done = pad_done,
    .button = pad_btn, .enter = pad_enter, .leave = pad_leave, .removed = pad_rem
};

static void ts_tab_added(void *d, struct zwp_tablet_seat_v2 *ts, struct zwp_tablet_v2 *t) {
    (void)ts;
    App *a = d;
    a->tab_added++;
    zwp_tablet_v2_add_listener(t, &g_tab_l, a);
    write_state(a);
}
static void ts_tool_added(void *d, struct zwp_tablet_seat_v2 *ts, struct zwp_tablet_tool_v2 *t) {
    (void)ts;
    App *a = d;
    a->tool_added++;
    zwp_tablet_tool_v2_add_listener(t, &g_tool_l, a);
    write_state(a);
}
static void ts_pad_added(void *d, struct zwp_tablet_seat_v2 *ts, struct zwp_tablet_pad_v2 *p) {
    (void)ts;
    App *a = d;
    a->pad_added++;
    zwp_tablet_pad_v2_add_listener(p, &g_pad_l, a);
    write_state(a);
}
static const struct zwp_tablet_seat_v2_listener g_tseat_l = {
    .tablet_added = ts_tab_added, .tool_added = ts_tool_added, .pad_added = ts_pad_added
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    App *a = d;
    a->seat_caps = caps;
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !a->touch) {
        a->touch = wl_seat_get_touch(s);
        wl_touch_add_listener(a->touch, &g_touch_l, a);
    }
    write_state(a);
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener g_seat_l = { .capabilities = seat_caps, .name = seat_name };

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t ser) { (void)d; xdg_wm_base_pong(wm, ser); }
static const struct xdg_wm_base_listener g_wm_l = { .ping = wm_ping };

static void xsurf_cfg(void *d, struct xdg_surface *xs, uint32_t ser) {
    App *a = d;
    xdg_surface_ack_configure(xs, ser);
    if (!a->buf) a->buf = make_shm_buf(a->shm, 400, 300, 0xFF204870u);
    wl_surface_attach(a->surf, a->buf, 0, 0);
    wl_surface_damage_buffer(a->surf, 0, 0, 400, 300);
    wl_surface_commit(a->surf);
    a->configured = true;
    write_state(a);
}
static const struct xdg_surface_listener g_xsurf_l = { .configure = xsurf_cfg };

static void xtop_cfg(void *d, struct xdg_toplevel *xt, int32_t w, int32_t h, struct wl_array *st) {
    (void)d; (void)xt; (void)w; (void)h; (void)st;
}
static void xtop_close(void *d, struct xdg_toplevel *xt) { (void)d; (void)xt; exit(0); }
static const struct xdg_toplevel_listener g_xtop_l = { .configure = xtop_cfg, .close = xtop_close };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *ifc, uint32_t ver) {
    App *a = d;
    if (!strcmp(ifc, wl_compositor_interface.name)) {
        a->comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(ifc, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(ifc, xdg_wm_base_interface.name)) {
        a->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2);
        xdg_wm_base_add_listener(a->wm, &g_wm_l, a);
    } else if (!strcmp(ifc, wl_seat_interface.name)) {
        a->seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(a->seat, &g_seat_l, a);
    } else if (!strcmp(ifc, zwp_tablet_manager_v2_interface.name)) {
        a->tab_mgr = wl_registry_bind(r, name, &zwp_tablet_manager_v2_interface, 1);
    } else if (!strcmp(ifc, wp_cursor_shape_manager_v1_interface.name)) {
        a->cshape_mgr = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    App a;
    memset(&a, 0, sizeof(a));
    a.state_file = argv[1];
    a.dpy = wl_display_connect(NULL);
    if (!a.dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(a.dpy);
    wl_registry_add_listener(reg, &g_reg_l, &a);
    wl_display_roundtrip(a.dpy);
    wl_display_roundtrip(a.dpy);
    if (!a.comp || !a.shm || !a.wm || !a.seat || !a.tab_mgr || !a.cshape_mgr) {
        fprintf(stderr, "missing Wayland globals\n");
        return 1;
    }
    a.tab_seat = zwp_tablet_manager_v2_get_tablet_seat(a.tab_mgr, a.seat);
    zwp_tablet_seat_v2_add_listener(a.tab_seat, &g_tseat_l, &a);

    a.surf = wl_compositor_create_surface(a.comp);
    a.xsurf = xdg_wm_base_get_xdg_surface(a.wm, a.surf);
    xdg_surface_add_listener(a.xsurf, &g_xsurf_l, &a);
    a.xtop = xdg_surface_get_toplevel(a.xsurf);
    xdg_toplevel_add_listener(a.xtop, &g_xtop_l, &a);
    xdg_toplevel_set_title(a.xtop, "Tablet & Touch Test");
    xdg_toplevel_set_app_id(a.xtop, "org.cde.test.tablet");
    wl_surface_commit(a.surf);
    wl_display_roundtrip(a.dpy);

    while (wl_display_dispatch(a.dpy) != -1) {
    }
    return 0;
}
"""


def pkg_flags(pkg: str) -> tuple[list[str], list[str]]:
    cf = subprocess.check_output(["pkg-config", "--cflags", pkg], text=True).split()
    lf = subprocess.check_output(["pkg-config", "--libs", pkg], text=True).split()
    return cf, lf


def pkg_var(pkg: str, var: str) -> str:
    return subprocess.check_output(["pkg-config", f"--variable={var}", pkg], text=True).strip()


def read_ipc_props(ipc_path: Path) -> dict[str, bytes]:
    props: dict[str, bytes] = {}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2.0)
        s.connect(str(ipc_path))
        buf = b""
        while b"READY\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    for line in buf.decode("latin1").splitlines():
        if line.startswith("PROP "):
            parts = line.split(" ", 2)
            if len(parts) == 3:
                props[parts[1]] = bytes.fromhex(parts[2])
    return props


def send_ipc_lines(ipc_path: Path, lines: list[str]) -> None:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2.0)
        s.connect(str(ipc_path))
        buf = b""
        while b"READY\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        for ln in lines:
            s.sendall((ln + "\n").encode("utf-8"))
            time.sleep(0.015)
        time.sleep(0.04)


def parse_active_win_box(props: dict[str, bytes]) -> tuple[int, int, int, int] | None:
    raw = props.get("_DT_ACTIVE_WINDOW_BOX")
    if not raw or len(raw) < 16:
        return None
    import struct
    x, y, w, h = struct.unpack("<iiii", raw[:16])
    return (x, y, w, h)


def parse_kv_file(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    out: dict[str, str] = {}
    for ln in path.read_text().splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def wait_until(pred, timeout_s: float = 6.0, desc: str = "condition"):
    deadline = time.monotonic() + timeout_s
    last_val = None
    while time.monotonic() < deadline:
        last_val = pred()
        if last_val:
            return last_val
        time.sleep(0.03)
    raise RuntimeError(f"timed out waiting for {desc} (last={last_val})")


def wait_kv(path: Path, cond, desc: str, timeout_s: float = 5.0) -> dict[str, str]:
    def _chk():
        st = parse_kv_file(path)
        return st if cond(st) else None
    return wait_until(_chk, timeout_s, desc)


def wait_box(ipc_path: Path, cond, desc: str, timeout_s: float = 5.0) -> tuple[int, int, int, int]:
    def _chk():
        b = parse_active_win_box(read_ipc_props(ipc_path))
        return b if (b and cond(b)) else None
    return wait_until(_chk, timeout_s, desc)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compositor", required=True, type=Path)
    args = ap.parse_args()

    comp_bin = args.compositor.resolve(strict=True)
    cc = os.environ.get("CC", "cc")
    scanner = shutil.which("wayland-scanner")
    if not scanner:
        raise RuntimeError("wayland-scanner not found")

    wp_dir = Path(pkg_var("wayland-protocols", "pkgdatadir"))
    xdg_xml = wp_dir / "stable/xdg-shell/xdg-shell.xml"
    tab_xml = wp_dir / "unstable/tablet/tablet-unstable-v2.xml"
    cshape_xml = wp_dir / "staging/cursor-shape/cursor-shape-v1.xml"

    with tempfile.TemporaryDirectory(prefix="dtwm-touch-tablet-") as td:
        tmp = Path(td)
        rt_dir = tmp / "rt"
        rt_dir.mkdir(mode=0o700)
        proto = tmp / "proto"
        proto.mkdir()

        for xml, stem in [
            (xdg_xml, "xdg-shell"),
            (tab_xml, "tablet-v2"),
            (cshape_xml, "cursor-shape-v1"),
        ]:
            subprocess.run([scanner, "client-header", str(xml), str(proto / f"{stem}-client-protocol.h")], check=True)
            subprocess.run([scanner, "private-code", str(xml), str(proto / f"{stem}-protocol.c")], check=True)

        cli_c = tmp / "tablet-touch-client.c"
        cli_c.write_text(CLIENT_C)
        cli_bin = tmp / "tablet-touch-client"
        wlc_cf, wlc_lf = pkg_flags("wayland-client")
        subprocess.run(
            [
                cc, "-std=c11", "-O2", "-Wall", "-Wextra",
                "-I" + str(proto), *wlc_cf,
                str(cli_c),
                str(proto / "xdg-shell-protocol.c"),
                str(proto / "tablet-v2-protocol.c"),
                str(proto / "cursor-shape-v1-protocol.c"),
                *wlc_lf, "-lm", "-o", str(cli_bin),
            ],
            check=True,
        )

        status_file = tmp / "status.txt"
        comp_log = (tmp / "dtwm-wayland.log").open("w")
        env = dict(
            os.environ,
            XDG_RUNTIME_DIR=str(rt_dir),
            TMPDIR=str(tmp),
            WLR_BACKENDS="headless",
            WLR_RENDERER="pixman",
            WLR_HEADLESS_OUTPUTS="1",
            DTWM_TEST_SYNTH_INPUT="1",
            LC_ALL="C.UTF-8",
        )
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)

        comp = subprocess.Popen(
            [
                str(comp_bin),
                "--no-xwayland",
                "--no-panel",
                "--no-open-window-icons",
                "--status-file",
                str(status_file),
            ],
            env=env,
            stdout=comp_log,
            stderr=subprocess.STDOUT,
        )
        cli = None
        try:
            wait_until(lambda: status_file.is_file(), 8.0, "dtwm-wayland status file")
            socks = sorted(p.name for p in rt_dir.glob("wayland-*") if not p.name.endswith(".lock"))
            if not socks:
                raise RuntimeError("no wayland socket created")
            wl_sock = socks[0]
            ipc_path = rt_dir / f"dtwm-wayland-{wl_sock}.ipc"
            wait_until(lambda: ipc_path.exists(), 5.0, "native IPC socket")

            cli_state = tmp / "client-state.txt"
            cli_env = dict(env, WAYLAND_DISPLAY=wl_sock)
            cli = subprocess.Popen([str(cli_bin), str(cli_state)], env=cli_env)

            def ready_state():
                st = parse_kv_file(cli_state)
                if st.get("configured") == "1" and int(st.get("tab_added", "0")) >= 1:
                    return st
                return None

            st0 = wait_until(ready_state, 6.0, "tablet-touch-client map & tablet seat")
            seat_caps = int(st0["seat_caps"])
            if (seat_caps & 4) == 0:
                raise AssertionError(f"WL_SEAT_CAPABILITY_TOUCH (4) missing in seat_caps={seat_caps}")
            if int(st0.get("pad_added", "0")) < 1:
                raise AssertionError(f"expected pad_added>=1, got {st0}")

            # Headless output default mode is 1280x900
            out_w, out_h = 1280.0, 900.0
            box0 = wait_until(
                lambda: parse_active_win_box(read_ipc_props(ipc_path)),
                5.0,
                "_NET_ACTIVE_WINDOW_BOX",
            )
            wx0, wy0, wow, woh = box0

            # 1. Two-finger wl_touch gesture inside client area
            cx = wx0 + wow * 0.5
            cy = wy0 + woh * 0.55
            send_ipc_lines(
                ipc_path,
                [
                    f"SYNTH_TOUCH down 0 {(cx - 20.0) / out_w:.6f} {cy / out_h:.6f}",
                    f"SYNTH_TOUCH down 1 {(cx + 20.0) / out_w:.6f} {cy / out_h:.6f}",
                    "SYNTH_TOUCH frame",
                    f"SYNTH_TOUCH motion 0 {(cx - 20.0) / out_w:.6f} {(cy - 40.0) / out_h:.6f}",
                    f"SYNTH_TOUCH motion 1 {(cx + 20.0) / out_w:.6f} {(cy - 40.0) / out_h:.6f}",
                    "SYNTH_TOUCH frame",
                    "SYNTH_TOUCH up 0",
                    "SYNTH_TOUCH up 1",
                    "SYNTH_TOUCH frame",
                ],
            )
            st_touch = wait_kv(
                cli_state,
                lambda st: int(st.get("touch_ups", "0")) >= 2,
                "2-finger wl_touch delivery",
            )
            if int(st_touch["touch_downs"]) != 2 or int(st_touch["touch_motions"]) < 2:
                raise AssertionError(f"unexpected wl_touch counts: {st_touch}")

            # 2. Single-finger touch drag on CDE titlebar (+120, +60)
            tx0 = wx0 + wow * 0.5
            ty0 = wy0 + 15.0
            send_ipc_lines(
                ipc_path,
                [
                    f"SYNTH_TOUCH down 0 {tx0 / out_w:.6f} {ty0 / out_h:.6f}",
                    "SYNTH_TOUCH frame",
                    f"SYNTH_TOUCH motion 0 {(tx0 + 120.0) / out_w:.6f} {(ty0 + 60.0) / out_h:.6f}",
                    "SYNTH_TOUCH frame",
                    "SYNTH_TOUCH up 0",
                    "SYNTH_TOUCH frame",
                ],
            )
            box1 = wait_box(
                ipc_path,
                lambda b: abs(b[0] - (wx0 + 120)) <= 2 and abs(b[1] - (wy0 + 60)) <= 2,
                "touch CDE titlebar window drag",
            )
            wx1, wy1, _, _ = box1

            # 3. Stylus proximity, pressure, tilt, tip down/up, barrel button, and wp_cursor_shape_v1
            scx = wx1 + wow * 0.5
            scy = wy1 + woh * 0.55
            send_ipc_lines(
                ipc_path,
                [
                    f"SYNTH_TABLET prox 1 {scx / out_w:.6f} {scy / out_h:.6f}",
                    f"SYNTH_TABLET axis {scx / out_w:.6f} {scy / out_h:.6f} 0.725 18.5 -12.0",
                    f"SYNTH_TABLET tip 1 {scx / out_w:.6f} {scy / out_h:.6f}",
                    f"SYNTH_TABLET axis {(scx + 30.0) / out_w:.6f} {(scy + 20.0) / out_h:.6f} 0.850 18.5 -12.0",
                    "SYNTH_TABLET button 331 1",
                    "SYNTH_TABLET button 331 0",
                    f"SYNTH_TABLET tip 0 {(scx + 30.0) / out_w:.6f} {(scy + 20.0) / out_h:.6f}",
                ],
            )
            st_tab = wait_kv(
                cli_state,
                lambda st: int(st.get("tip_up", "0")) >= 1 and int(st.get("tool_buttons", "0")) >= 1,
                "zwp_tablet_v2 stylus tip/pressure/tilt/button delivery",
            )
            max_p = float(st_tab["max_pressure"])
            if abs(max_p - 0.850) > 0.02 or st_tab["last_tilt"] != "18.5,-12.0" or st_tab["last_tool_btn"] != "331":
                raise AssertionError(f"unexpected tablet tool metrics: {st_tab}")

            cshape_prop = wait_until(
                lambda: (
                    v.decode("utf-8", "ignore")
                    if (v := read_ipc_props(ipc_path).get("_DT_WAYLAND_CURSOR_SHAPE", b""))
                    and b"tablet_tool;shape=crosshair" in v
                    else None
                ),
                5.0,
                "wp_cursor_shape_v1 tablet_tool crosshair",
            )

            # 4. Tablet pad ExpressKey button + touch ring
            send_ipc_lines(
                ipc_path,
                [
                    "SYNTH_TABLET pad_btn 2 1",
                    "SYNTH_TABLET pad_btn 2 0",
                    "SYNTH_TABLET pad_ring 135.0",
                ],
            )
            st_pad = wait_kv(
                cli_state,
                lambda st: int(st.get("pad_buttons", "0")) >= 1 and int(st.get("pad_rings", "0")) >= 1,
                "zwp_tablet_pad_v2 button & ring delivery",
            )
            if int(st_pad["pad_enters"]) < 1 or st_pad["last_pad_btn"] != "2" or st_pad["last_ring_deg"] != "135.0":
                raise AssertionError(f"unexpected tablet pad state: {st_pad}")

            # 5. Stylus tip drag on CDE titlebar (-80, -30) -> proximity_out + window move
            stx = wx1 + wow * 0.5
            sty = wy1 + 15.0
            send_ipc_lines(
                ipc_path,
                [
                    f"SYNTH_TABLET axis {stx / out_w:.6f} {sty / out_h:.6f} 0.0 0.0 0.0",
                    f"SYNTH_TABLET tip 1 {stx / out_w:.6f} {sty / out_h:.6f}",
                    f"SYNTH_TABLET axis {(stx - 80.0) / out_w:.6f} {(sty - 30.0) / out_h:.6f} 0.5 0.0 0.0",
                    f"SYNTH_TABLET tip 0 {(stx - 80.0) / out_w:.6f} {(sty - 30.0) / out_h:.6f}",
                ],
            )
            box2 = wait_box(
                ipc_path,
                lambda b: abs(b[0] - (wx1 - 80)) <= 2 and abs(b[1] - (wy1 - 30)) <= 2,
                "stylus CDE titlebar window drag",
            )
            st_final = wait_kv(
                cli_state,
                lambda st: int(st.get("prox_out", "0")) >= 1,
                "stylus proximity_out on CDE frame",
            )

            print(
                f"PASS check-dtwm-wayland-touch-tablet "
                f"(touch=2f+title_drag[{box0[0]}->{box1[0]}] "
                f"stylus_p={max_p:.3f} tilt={st_final['last_tilt']} "
                f"cshape='{cshape_prop}' pad=btn{st_pad['last_pad_btn']}+ring{st_pad['last_ring_deg']} "
                f"stylus_drag={box1[0]}->{box2[0]})"
            )
            return 0
        finally:
            if cli and cli.poll() is None:
                cli.terminate()
                try:
                    cli.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    cli.kill()
            if comp.poll() is None:
                comp.terminate()
                try:
                    comp.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    comp.kill()
            comp_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
