#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland Batch 1 protocols:
1. xdg-toplevel-drag-v1 (Topic 1.2):
   - Protocol error enforcement (invalid_source on duplicate source / selection source,
     ongoing_drag on premature destroy, toplevel_attached on double attach).
   - Live window/tab tear-off attached to wl_data_device.start_drag: verifies torn-off
     xdg_toplevel tracks cursor motion live, drop target underneath still receives DND
     enter/motion/drop (excluded from self-interception), and unmapping mid-drag detaches cleanly.
2. zwp-input-method-v2 / zwp_input_popup_surface_v2 Caret Anchoring (Topic 1.4a):
   - Verifies candidate popup positioning on a child wl_subsurface in global scene coordinates,
     bottom-of-workarea flip above caret, and live popup re-anchoring during interactive window move.
3. xdg_toplevel Fullscreen & Unoccluded Direct Scanout Fast Path (Topic 1.5):
   - Verifies xdg_toplevel.set_fullscreen configures output-resolution borderless geometry,
     triggers unoccluded scanout_candidates evaluation, suppresses scanout when a popup occludes
     the view, and restores saved window geometry on unset_fullscreen.
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


DRAG_TEST_CLIENT_C = r"""
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
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-drag-v1-client-protocol.h"

typedef struct {
    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_toplevel *xt;
    struct wl_buffer *buf;
    int w, h;
    uint32_t color;
    bool configured;
    int cfg_w, cfg_h;
    bool cfg_fullscreen;
} Win;

static struct wl_display *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_subcompositor *g_subcomp;
static struct wl_shm *g_shm;
static struct wl_seat *g_seat;
static struct wl_pointer *g_ptr;
static struct wl_keyboard *g_kbd;
static struct wl_data_device_manager *g_ddm;
static struct wl_data_device *g_dd;
static struct xdg_wm_base *g_wm;
static struct xdg_toplevel_drag_manager_v1 *g_tdm;

static uint32_t g_last_btn_serial = 0;
static uint32_t g_last_kbd_serial = 0;
static int g_btn_count = 0;
static struct wl_surface *g_ptr_surf = NULL;
static struct wl_data_offer *g_cur_offer = NULL;
static bool g_dnd_entered = false;
static int g_dnd_motions = 0;
static bool g_dnd_dropped = false;
static bool g_src_drop_performed = false;
static bool g_src_finished = false;

static int create_shm_fd(size_t sz) {
    char name[64];
    snprintf(name, sizeof(name), "/dtwm-drag-%d-%ld", (int)getpid(), (long)random());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)sz) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static struct wl_buffer *make_solid_buf(int w, int h, uint32_t argb) {
    size_t stride = (size_t)w * 4u;
    size_t sz = stride * (size_t)h;
    int fd = create_shm_fd(sz);
    if (fd < 0) return NULL;
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) px[i] = argb;
    munmap(px, sz);
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)sz);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener g_wm_l = { .ping = wm_ping };

static void xt_conf(void *d, struct xdg_toplevel *xt, int32_t w, int32_t h, struct wl_array *states) {
    (void)xt;
    Win *win = d;
    win->cfg_w = w;
    win->cfg_h = h;
    win->cfg_fullscreen = false;
    uint32_t *st;
    wl_array_for_each(st, states) {
        if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN) win->cfg_fullscreen = true;
    }
}
static void xt_close(void *d, struct xdg_toplevel *xt) { (void)d; (void)xt; }
static void xt_bounds(void *d, struct xdg_toplevel *xt, int32_t w, int32_t h) { (void)d; (void)xt; (void)w; (void)h; }
static void xt_caps(void *d, struct xdg_toplevel *xt, struct wl_array *c) { (void)d; (void)xt; (void)c; }
static const struct xdg_toplevel_listener g_xt_l = {
    .configure = xt_conf, .close = xt_close, .configure_bounds = xt_bounds, .wm_capabilities = xt_caps
};

static void xs_conf(void *d, struct xdg_surface *xs, uint32_t serial) {
    Win *win = d;
    xdg_surface_ack_configure(xs, serial);
    int nw = win->cfg_w > 0 ? win->cfg_w : win->w;
    int nh = win->cfg_h > 0 ? win->cfg_h : win->h;
    if (!win->buf || nw != win->w || nh != win->h) {
        if (win->buf) wl_buffer_destroy(win->buf);
        win->w = nw;
        win->h = nh;
        win->buf = make_solid_buf(win->w, win->h, win->color);
    }
    wl_surface_attach(win->surf, win->buf, 0, 0);
    wl_surface_damage_buffer(win->surf, 0, 0, win->w, win->h);
    wl_surface_commit(win->surf);
    win->configured = true;
}
static const struct xdg_surface_listener g_xs_l = { .configure = xs_conf };

static void init_win(Win *win, const char *title, int w, int h, uint32_t color) {
    memset(win, 0, sizeof(*win));
    win->w = w;
    win->h = h;
    win->color = color;
    win->surf = wl_compositor_create_surface(g_comp);
    win->xs = xdg_wm_base_get_xdg_surface(g_wm, win->surf);
    xdg_surface_add_listener(win->xs, &g_xs_l, win);
    win->xt = xdg_surface_get_toplevel(win->xs);
    xdg_toplevel_add_listener(win->xt, &g_xt_l, win);
    xdg_toplevel_set_title(win->xt, title);
    xdg_toplevel_set_app_id(win->xt, title);
    wl_surface_commit(win->surf);
}

static void destroy_win(Win *win) {
    if (win->xt) xdg_toplevel_destroy(win->xt);
    if (win->xs) xdg_surface_destroy(win->xs);
    if (win->surf) wl_surface_destroy(win->surf);
    if (win->buf) wl_buffer_destroy(win->buf);
    memset(win, 0, sizeof(*win));
}

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)s; (void)x; (void)y;
    g_ptr_surf = surf;
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf) {
    (void)d; (void)p; (void)s; (void)surf;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t; (void)x; (void)y;
}
static void ptr_btn(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
    (void)d; (void)p; (void)t; (void)b;
    if (st == WL_POINTER_BUTTON_STATE_PRESSED) {
        g_last_btn_serial = s;
        g_btn_count++;
    }
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
    (void)d; (void)p; (void)t; (void)a; (void)v;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_src(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener g_ptr_l = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_btn,
    .axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_src,
    .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_disc
};

static void kbd_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) {
    (void)d; (void)k; (void)f; (void)sz;
    close(fd);
}
static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf, struct wl_array *keys) {
    (void)d; (void)k; (void)surf; (void)keys;
    g_last_kbd_serial = s;
}
static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf) {
    (void)d; (void)k; (void)s; (void)surf;
}
static void kbd_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) {
    (void)d; (void)k; (void)s; (void)t; (void)key; (void)st;
}
static void kbd_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) {
    (void)d; (void)k; (void)s; (void)dep; (void)lat; (void)lck; (void)grp;
}
static void kbd_rep(void *d, struct wl_keyboard *k, int32_t r, int32_t del) {
    (void)d; (void)k; (void)r; (void)del;
}
static const struct wl_keyboard_listener g_kbd_l = {
    .keymap = kbd_keymap, .enter = kbd_enter, .leave = kbd_leave,
    .key = kbd_key, .modifiers = kbd_mods, .repeat_info = kbd_rep
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_ptr) {
        g_ptr = wl_seat_get_pointer(s);
        wl_pointer_add_listener(g_ptr, &g_ptr_l, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_kbd) {
        g_kbd = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(g_kbd, &g_kbd_l, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener g_seat_l = { .capabilities = seat_caps, .name = seat_name };

static void offer_mime(void *d, struct wl_data_offer *o, const char *m) { (void)d; (void)o; (void)m; }
static void offer_src_act(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static void offer_act(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener g_offer_l = {
    .offer = offer_mime, .source_actions = offer_src_act, .action = offer_act
};

static void dd_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *id) {
    (void)d; (void)dd;
    wl_data_offer_add_listener(id, &g_offer_l, NULL);
}
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t ser, struct wl_surface *surf,
                     wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id) {
    (void)d; (void)dd; (void)x; (void)y;
    Win *target = d;
    g_cur_offer = id;
    if (target && surf == target->surf && id) {
        g_dnd_entered = true;
        wl_data_offer_accept(id, ser, "application/x-cde-tab");
        wl_data_offer_set_actions(id, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    }
}
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)dd; (void)t; (void)x; (void)y;
    if (g_dnd_entered) g_dnd_motions++;
}
static void dd_drop(void *d, struct wl_data_device *dd) {
    (void)d; (void)dd;
    g_dnd_dropped = true;
    if (g_cur_offer) {
        wl_data_offer_finish(g_cur_offer);
        wl_data_offer_destroy(g_cur_offer);
        g_cur_offer = NULL;
    }
}
static void dd_sel(void *d, struct wl_data_device *dd, struct wl_data_offer *id) { (void)d; (void)dd; (void)id; }
static const struct wl_data_device_listener g_dd_l = {
    .data_offer = dd_offer, .enter = dd_enter, .leave = dd_leave,
    .motion = dd_motion, .drop = dd_drop, .selection = dd_sel
};

static void ds_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void ds_send(void *d, struct wl_data_source *s, const char *m, int32_t fd) {
    (void)d; (void)s; (void)m;
    close(fd);
}
static void ds_cancel(void *d, struct wl_data_source *s) { (void)d; (void)s; g_src_finished = true; }
static void ds_drop_perf(void *d, struct wl_data_source *s) { (void)d; (void)s; g_src_drop_performed = true; }
static void ds_fin(void *d, struct wl_data_source *s) { (void)d; (void)s; g_src_finished = true; }
static void ds_act(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener g_ds_l = {
    .target = ds_target, .send = ds_send, .cancelled = ds_cancel,
    .dnd_drop_performed = ds_drop_perf, .dnd_finished = ds_fin, .action = ds_act
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name))
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, wl_subcompositor_interface.name))
        g_subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name))
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(g_seat, &g_seat_l, NULL);
    } else if (!strcmp(iface, wl_data_device_manager_interface.name))
        g_ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, ver < 3 ? ver : 3);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        g_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
        xdg_wm_base_add_listener(g_wm, &g_wm_l, NULL);
    } else if (!strcmp(iface, xdg_toplevel_drag_manager_v1_interface.name))
        g_tdm = wl_registry_bind(r, name, &xdg_toplevel_drag_manager_v1_interface, 1);
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

static void pump_ms(int ms) {
    struct pollfd pfd = { .fd = wl_display_get_fd(g_dpy), .events = POLLIN };
    int elapsed = 0;
    while (elapsed < ms) {
        while (wl_display_prepare_read(g_dpy) != 0) wl_display_dispatch_pending(g_dpy);
        wl_display_flush(g_dpy);
        if (poll(&pfd, 1, 20) > 0) wl_display_read_events(g_dpy);
        else wl_display_cancel_read(g_dpy);
        wl_display_dispatch_pending(g_dpy);
        elapsed += 20;
    }
}

static void write_state(const char *path, const Win *target, const Win *src, const Win *torn, bool drag_active) {
    if (!path) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f,
            "READY=1\nTARGET_CFG=%d\nSRC_CFG=%d\nTORN_CFG=%d\nBTN_COUNT=%d\nDRAG_ACTIVE=%d\n"
            "DND_ENTER=%d\nDND_MOTIONS=%d\nDND_DROP=%d\nDROP_PERFORMED=%d\nFINISHED=%d\n"
            "FS_CFG=%d\nFS_W=%d\nFS_H=%d\n",
            target->configured ? 1 : 0,
            src->configured ? 1 : 0,
            torn->configured ? 1 : 0,
            g_btn_count,
            drag_active ? 1 : 0,
            g_dnd_entered ? 1 : 0,
            g_dnd_motions,
            g_dnd_dropped ? 1 : 0,
            g_src_drop_performed ? 1 : 0,
            g_src_finished ? 1 : 0,
            src->cfg_fullscreen ? 1 : 0,
            src->w,
            src->h);
    fclose(f);
    rename(tmp, path);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *mode = argv[1];
    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &g_reg_l, NULL);
    wl_display_roundtrip(g_dpy);
    wl_display_roundtrip(g_dpy);
    if (!g_comp || !g_shm || !g_seat || !g_ddm || !g_wm || !g_tdm) return 3;

    /* Protocol error tests */
    if (!strcmp(mode, "err_dup_source")) {
        struct wl_data_source *ds = wl_data_device_manager_create_data_source(g_ddm);
        struct xdg_toplevel_drag_v1 *d1 = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(g_tdm, ds);
        struct xdg_toplevel_drag_v1 *d2 = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(g_tdm, ds);
        (void)d1; (void)d2;
        int rc = wl_display_roundtrip(g_dpy);
        int err = wl_display_get_error(g_dpy);
        wl_display_disconnect(g_dpy);
        return (rc < 0 || err != 0) ? 0 : 10;
    }
    if (!strcmp(mode, "err_set_selection") || !strcmp(mode, "err_selection_then_drag")) {
        Win w1;
        init_win(&w1, "FocusForSerial", 200, 140, 0xffff0000u);
        while (!w1.configured || g_last_kbd_serial == 0) wl_display_roundtrip(g_dpy);
        struct wl_data_device *dd = wl_data_device_manager_get_data_device(g_ddm, g_seat);
        struct wl_data_source *ds = wl_data_device_manager_create_data_source(g_ddm);
        wl_data_source_offer(ds, "text/plain");
        if (!strcmp(mode, "err_set_selection")) {
            struct xdg_toplevel_drag_v1 *d1 = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(g_tdm, ds);
            (void)d1;
            wl_data_device_set_selection(dd, ds, g_last_kbd_serial);
        } else {
            wl_data_device_set_selection(dd, ds, g_last_kbd_serial);
            wl_display_roundtrip(g_dpy);
            struct xdg_toplevel_drag_v1 *d1 = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(g_tdm, ds);
            (void)d1;
        }
        int rc = wl_display_roundtrip(g_dpy);
        int err = wl_display_get_error(g_dpy);
        wl_display_disconnect(g_dpy);
        return (rc < 0 || err != 0) ? 0 : 11;
    }
    if (!strcmp(mode, "err_double_attach")) {
        Win w1, w2;
        init_win(&w1, "AttachOne", 200, 140, 0xffff0000u);
        init_win(&w2, "AttachTwo", 200, 140, 0xff00ff00u);
        while (!w1.configured || !w2.configured) wl_display_roundtrip(g_dpy);
        struct wl_data_source *ds = wl_data_device_manager_create_data_source(g_ddm);
        struct xdg_toplevel_drag_v1 *d1 = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(g_tdm, ds);
        xdg_toplevel_drag_v1_attach(d1, w1.xt, 10, 10);
        xdg_toplevel_drag_v1_attach(d1, w2.xt, 10, 10);
        int rc = wl_display_roundtrip(g_dpy);
        int err = wl_display_get_error(g_dpy);
        wl_display_disconnect(g_dpy);
        return (rc < 0 || err != 0) ? 0 : 12;
    }

    if (argc < 4) return 4;
    const char *state_path = argv[2];
    const char *cmd_path = argv[3];

    if (!strcmp(mode, "target_only")) {
        Win target = {0}, dummy = {0};
        init_win(&target, "DropTargetWin", 380, 320, 0xff1030ccu);
        while (!target.configured) wl_display_roundtrip(g_dpy);
        g_dd = wl_data_device_manager_get_data_device(g_ddm, g_seat);
        wl_data_device_add_listener(g_dd, &g_dd_l, &target);
        wl_display_roundtrip(g_dpy);
        write_state(state_path, &target, &dummy, &dummy, false);
        for (;;) {
            pump_ms(25);
            if (access(cmd_path, R_OK) == 0) {
                unlink(cmd_path);
                break;
            }
            write_state(state_path, &target, &dummy, &dummy, false);
        }
        destroy_win(&target);
        wl_display_disconnect(g_dpy);
        return 0;
    }

    Win target = {0}, src = {0}, torn = {0}, pop_win = {0};
    struct xdg_popup *pop_xp = NULL;

    init_win(&src, "DragSourceWin", 320, 220, 0xff10aa20u);    /* green */
    while (!src.configured) wl_display_roundtrip(g_dpy);

    g_dd = wl_data_device_manager_get_data_device(g_ddm, g_seat);
    wl_data_device_add_listener(g_dd, &g_dd_l, NULL);
    wl_display_roundtrip(g_dpy);

    struct wl_data_source *active_ds = NULL;
    struct xdg_toplevel_drag_v1 *active_td = NULL;
    bool drag_active = false;
    write_state(state_path, &target, &src, &torn, drag_active);

    for (;;) {
        pump_ms(25);
        if (access(cmd_path, R_OK) == 0) {
            char cmd[128] = {0};
            FILE *cf = fopen(cmd_path, "r");
            if (cf) {
                if (fgets(cmd, sizeof(cmd), cf)) cmd[strcspn(cmd, "\r\n")] = '\0';
                fclose(cf);
            }

            if (!strcmp(cmd, "start_tearoff_drag") && g_last_btn_serial != 0 && !drag_active) {
                active_ds = wl_data_device_manager_create_data_source(g_ddm);
                wl_data_source_add_listener(active_ds, &g_ds_l, NULL);
                wl_data_source_offer(active_ds, "application/x-cde-tab");
                wl_data_source_set_actions(active_ds, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
                active_td = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(g_tdm, active_ds);
                init_win(&torn, "TornOffTabWin", 220, 150, 0xffee11ccu); /* bright magenta */
                xdg_toplevel_drag_v1_attach(active_td, torn.xt, 40, 20);
                wl_data_device_start_drag(g_dd, active_ds, src.surf, NULL, g_last_btn_serial);
                drag_active = true;
                while (!torn.configured) wl_display_roundtrip(g_dpy);
                wl_display_roundtrip(g_dpy);
            } else if (!strcmp(cmd, "unmap_reattach") && active_td && torn.surf) {
                /* Unmap attached toplevel -> auto-detaches per spec, then map & re-attach */
                wl_surface_attach(torn.surf, NULL, 0, 0);
                wl_surface_commit(torn.surf);
                wl_display_roundtrip(g_dpy);
                destroy_win(&torn);
                init_win(&torn, "TornOffTabWin2", 220, 150, 0xffee11ccu);
                xdg_toplevel_drag_v1_attach(active_td, torn.xt, 40, 20);
                while (!torn.configured) wl_display_roundtrip(g_dpy);
                wl_display_roundtrip(g_dpy);
            } else if (!strcmp(cmd, "finish_drag_objs") && active_td) {
                xdg_toplevel_drag_v1_destroy(active_td);
                active_td = NULL;
                if (active_ds) {
                    wl_data_source_destroy(active_ds);
                    active_ds = NULL;
                }
                drag_active = false;
                wl_display_roundtrip(g_dpy);
            } else if (!strcmp(cmd, "fullscreen_on")) {
                xdg_toplevel_set_fullscreen(src.xt, NULL);
                wl_surface_commit(src.surf);
                wl_display_roundtrip(g_dpy);
            } else if (!strcmp(cmd, "commit_frame")) {
                wl_surface_attach(src.surf, src.buf, 0, 0);
                wl_surface_damage_buffer(src.surf, 0, 0, src.w, src.h);
                wl_surface_commit(src.surf);
                wl_display_roundtrip(g_dpy);
            } else if (!strcmp(cmd, "open_occluding_popup") && !pop_xp) {
                memset(&pop_win, 0, sizeof(pop_win));
                pop_win.w = 120;
                pop_win.h = 80;
                pop_win.color = 0xffffcc00u;
                struct xdg_positioner *pos = xdg_wm_base_create_positioner(g_wm);
                xdg_positioner_set_size(pos, 120, 80);
                xdg_positioner_set_anchor_rect(pos, 20, 20, 40, 40);
                xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
                xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
                pop_win.surf = wl_compositor_create_surface(g_comp);
                pop_win.xs = xdg_wm_base_get_xdg_surface(g_wm, pop_win.surf);
                xdg_surface_add_listener(pop_win.xs, &g_xs_l, &pop_win);
                pop_xp = xdg_surface_get_popup(pop_win.xs, src.xs, pos);
                xdg_positioner_destroy(pos);
                wl_surface_commit(pop_win.surf);
                while (!pop_win.configured) wl_display_roundtrip(g_dpy);
            } else if (!strcmp(cmd, "close_occluding_popup") && pop_xp) {
                xdg_popup_destroy(pop_xp);
                pop_xp = NULL;
                destroy_win(&pop_win);
                wl_display_roundtrip(g_dpy);
            } else if (!strcmp(cmd, "fullscreen_off")) {
                xdg_toplevel_unset_fullscreen(src.xt);
                wl_surface_commit(src.surf);
                wl_display_roundtrip(g_dpy);
            } else if (!strcmp(cmd, "quit")) {
                write_state(state_path, &target, &src, &torn, drag_active);
                unlink(cmd_path);
                break;
            }
            write_state(state_path, &target, &src, &torn, drag_active);
            unlink(cmd_path);
        }
        write_state(state_path, &target, &src, &torn, drag_active);
    }

    if (active_td) xdg_toplevel_drag_v1_destroy(active_td);
    if (active_ds) wl_data_source_destroy(active_ds);
    if (torn.surf) destroy_win(&torn);
    destroy_win(&src);
    destroy_win(&target);
    wl_display_disconnect(g_dpy);
    return 0;
}
"""


IME_SUBSURFACE_CLIENT_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
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
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"

static struct wl_display *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_subcompositor *g_subcomp;
static struct wl_shm *g_shm;
static struct wl_seat *g_seat;
static struct xdg_wm_base *g_wm;
static struct zxdg_decoration_manager_v1 *g_dec_mgr;
static struct zwp_text_input_manager_v3 *g_ti_mgr;
static struct zwp_input_method_manager_v2 *g_im_mgr;

static bool g_win_cfg = false;
static struct wl_surface *g_ti_entered_surf = NULL;
static bool g_im_active = false;
static int g_rect_events = 0;
static int32_t g_rel_x = 0, g_rel_y = 0, g_rel_w = 0, g_rel_h = 0;

static struct wl_buffer *make_buf(int w, int h, uint32_t argb) {
    char name[64];
    snprintf(name, sizeof(name), "/dtwm-ime-%d-%ld", (int)getpid(), (long)random());
    size_t sz = (size_t)w * (size_t)h * 4u;
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return NULL;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)sz) < 0) { close(fd); return NULL; }
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) px[i] = argb;
    munmap(px, sz);
    struct wl_shm_pool *p = wl_shm_create_pool(g_shm, fd, (int32_t)sz);
    struct wl_buffer *b = wl_shm_pool_create_buffer(p, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(p);
    close(fd);
    return b;
}

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener g_wm_l = { .ping = wm_ping };

static void xt_conf(void *d, struct xdg_toplevel *xt, int32_t w, int32_t h, struct wl_array *s) {
    (void)d; (void)xt; (void)w; (void)h; (void)s;
}
static void xt_close(void *d, struct xdg_toplevel *xt) { (void)d; (void)xt; }
static const struct xdg_toplevel_listener g_xt_l = { .configure = xt_conf, .close = xt_close };

static void xs_conf(void *d, struct xdg_surface *xs, uint32_t ser) {
    struct wl_surface *surf = d;
    xdg_surface_ack_configure(xs, ser);
    if (!g_win_cfg) {
        struct wl_buffer *b = make_buf(480, 360, 0xfff0f0f0u);
        wl_surface_attach(surf, b, 0, 0);
        wl_surface_damage_buffer(surf, 0, 0, 480, 360);
        wl_surface_commit(surf);
        g_win_cfg = true;
    }
}
static const struct xdg_surface_listener g_xs_l = { .configure = xs_conf };

static void ti_enter(void *d, struct zwp_text_input_v3 *ti, struct wl_surface *s) {
    (void)d; (void)ti;
    g_ti_entered_surf = s;
}
static void ti_leave(void *d, struct zwp_text_input_v3 *ti, struct wl_surface *s) {
    (void)d; (void)ti; (void)s;
}
static void ti_pre(void *d, struct zwp_text_input_v3 *ti, const char *t, int32_t b, int32_t e) {
    (void)d; (void)ti; (void)t; (void)b; (void)e;
}
static void ti_com(void *d, struct zwp_text_input_v3 *ti, const char *t) { (void)d; (void)ti; (void)t; }
static void ti_del(void *d, struct zwp_text_input_v3 *ti, uint32_t b, uint32_t a) { (void)d; (void)ti; (void)b; (void)a; }
static void ti_done(void *d, struct zwp_text_input_v3 *ti, uint32_t s) { (void)d; (void)ti; (void)s; }
static const struct zwp_text_input_v3_listener g_ti_l = {
    .enter = ti_enter, .leave = ti_leave, .preedit_string = ti_pre,
    .commit_string = ti_com, .delete_surrounding_text = ti_del, .done = ti_done
};

static bool g_pend_act = false;
static void im_act(void *d, struct zwp_input_method_v2 *im) { (void)d; (void)im; g_pend_act = true; }
static void im_deact(void *d, struct zwp_input_method_v2 *im) { (void)d; (void)im; g_pend_act = false; }
static void im_surr(void *d, struct zwp_input_method_v2 *im, const char *t, uint32_t c, uint32_t a) {
    (void)d; (void)im; (void)t; (void)c; (void)a;
}
static void im_cause(void *d, struct zwp_input_method_v2 *im, uint32_t c) { (void)d; (void)im; (void)c; }
static void im_ctype(void *d, struct zwp_input_method_v2 *im, uint32_t h, uint32_t p) { (void)d; (void)im; (void)h; (void)p; }
static void im_done(void *d, struct zwp_input_method_v2 *im) { (void)d; (void)im; g_im_active = g_pend_act; }
static void im_unavail(void *d, struct zwp_input_method_v2 *im) { (void)d; (void)im; }
static const struct zwp_input_method_v2_listener g_im_l = {
    .activate = im_act, .deactivate = im_deact, .surrounding_text = im_surr,
    .text_change_cause = im_cause, .content_type = im_ctype, .done = im_done, .unavailable = im_unavail
};

static void pop_rect(void *d, struct zwp_input_popup_surface_v2 *p, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)d; (void)p;
    g_rel_x = x; g_rel_y = y; g_rel_w = w; g_rel_h = h;
    g_rect_events++;
}
static const struct zwp_input_popup_surface_v2_listener g_pop_l = { .text_input_rectangle = pop_rect };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name))
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, wl_subcompositor_interface.name))
        g_subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name))
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name))
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        g_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2);
        xdg_wm_base_add_listener(g_wm, &g_wm_l, NULL);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
        g_dec_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name))
        g_ti_mgr = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
    else if (!strcmp(iface, zwp_input_method_manager_v2_interface.name))
        g_im_mgr = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

static void pump(int ms) {
    struct pollfd pfd = { .fd = wl_display_get_fd(g_dpy), .events = POLLIN };
    int elapsed = 0;
    while (elapsed < ms) {
        while (wl_display_prepare_read(g_dpy) != 0) wl_display_dispatch_pending(g_dpy);
        wl_display_flush(g_dpy);
        if (poll(&pfd, 1, 20) > 0) wl_display_read_events(g_dpy);
        else wl_display_cancel_read(g_dpy);
        wl_display_dispatch_pending(g_dpy);
        elapsed += 20;
    }
}

static void save_state(const char *path, struct wl_surface *sub_surf) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "WIN_CFG=%d\nSUB_ENTER=%d\nIM_ACTIVE=%d\nRECT_EVENTS=%d\nREL_X=%d\nREL_Y=%d\nREL_W=%d\nREL_H=%d\n",
            g_win_cfg ? 1 : 0,
            (g_ti_entered_surf == sub_surf) ? 1 : 0,
            g_im_active ? 1 : 0,
            g_rect_events,
            g_rel_x, g_rel_y, g_rel_w, g_rel_h);
    fclose(f);
    rename(tmp, path);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *state_path = argv[1];
    const char *cmd_path = argv[2];
    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &g_reg_l, NULL);
    wl_display_roundtrip(g_dpy);
    if (!g_comp || !g_subcomp || !g_shm || !g_seat || !g_wm || !g_ti_mgr || !g_im_mgr) return 3;

    struct zwp_input_method_v2 *im = zwp_input_method_manager_v2_get_input_method(g_im_mgr, g_seat);
    zwp_input_method_v2_add_listener(im, &g_im_l, NULL);
    struct wl_surface *pop_surf = wl_compositor_create_surface(g_comp);
    struct zwp_input_popup_surface_v2 *pop = zwp_input_method_v2_get_input_popup_surface(im, pop_surf);
    zwp_input_popup_surface_v2_add_listener(pop, &g_pop_l, NULL);
    struct wl_buffer *pop_buf = make_buf(180, 44, 0xff222233u);

    struct zwp_text_input_v3 *ti = zwp_text_input_manager_v3_get_text_input(g_ti_mgr, g_seat);
    zwp_text_input_v3_add_listener(ti, &g_ti_l, NULL);

    struct wl_surface *main_surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g_wm, main_surf);
    xdg_surface_add_listener(xs, &g_xs_l, main_surf);
    struct xdg_toplevel *xt = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(xt, &g_xt_l, NULL);
    xdg_toplevel_set_title(xt, "ImeSubsurfaceWin");
    struct zxdg_toplevel_decoration_v1 *dec = NULL;
    if (g_dec_mgr) {
        dec = zxdg_decoration_manager_v1_get_toplevel_decoration(g_dec_mgr, xt);
        zxdg_toplevel_decoration_v1_set_mode(dec, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(main_surf);
    while (!g_win_cfg) wl_display_roundtrip(g_dpy);

    /* Attach child subsurface at (110, 90) inside main_surf */
    struct wl_surface *sub_surf = wl_compositor_create_surface(g_comp);
    struct wl_subsurface *sub = wl_subcompositor_get_subsurface(g_subcomp, sub_surf, main_surf);
    wl_subsurface_set_desync(sub);
    wl_subsurface_set_position(sub, 110, 90);
    struct wl_buffer *sub_buf = make_buf(240, 180, 0xffddeeffu);
    wl_surface_attach(sub_surf, sub_buf, 0, 0);
    wl_surface_damage_buffer(sub_surf, 0, 0, 240, 180);
    wl_surface_commit(sub_surf);
    wl_surface_commit(main_surf);
    wl_display_roundtrip(g_dpy);

    save_state(state_path, sub_surf);
    for (;;) {
        pump(25);
        if (access(cmd_path, R_OK) == 0) {
            char cmd[128] = {0};
            FILE *cf = fopen(cmd_path, "r");
            if (cf) {
                if (fgets(cmd, sizeof(cmd), cf)) cmd[strcspn(cmd, "\r\n")] = '\0';
                fclose(cf);
            }
            int cx = 0, cy = 0, cw = 0, ch = 0;
            if (sscanf(cmd, "caret %d %d %d %d", &cx, &cy, &cw, &ch) == 4) {
                zwp_text_input_v3_enable(ti);
                zwp_text_input_v3_set_cursor_rectangle(ti, cx, cy, cw, ch);
                zwp_text_input_v3_commit(ti);
                wl_surface_attach(pop_surf, pop_buf, 0, 0);
                wl_surface_damage_buffer(pop_surf, 0, 0, 180, 44);
                wl_surface_commit(pop_surf);
                wl_display_roundtrip(g_dpy);
            } else if (!strcmp(cmd, "quit")) {
                save_state(state_path, sub_surf);
                unlink(cmd_path);
                break;
            }
            save_state(state_path, sub_surf);
            unlink(cmd_path);
        }
        save_state(state_path, sub_surf);
    }

    zwp_input_popup_surface_v2_destroy(pop);
    wl_surface_destroy(pop_surf);
    zwp_input_method_v2_destroy(im);
    zwp_text_input_v3_destroy(ti);
    wl_subsurface_destroy(sub);
    wl_surface_destroy(sub_surf);
    xdg_toplevel_destroy(xt);
    xdg_surface_destroy(xs);
    wl_surface_destroy(main_surf);
    wl_display_disconnect(g_dpy);
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


def send_cmd(cmd_path: Path, line: str, timeout: float = 6.0) -> None:
    if not wait_until(lambda: not cmd_path.exists(), timeout=timeout):
        raise AssertionError(f"previous command still pending at {cmd_path}")
    tmp_path = cmd_path.with_suffix(cmd_path.suffix + ".tmp")
    tmp_path.write_text(line if line.endswith("\n") else (line + "\n"), encoding="utf-8")
    os.replace(tmp_path, cmd_path)
    if not wait_until(lambda: not cmd_path.exists(), timeout=timeout):
        raise AssertionError(f"client did not consume command {line!r} at {cmd_path}")


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
    idx += 1
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
    tests_dir = Path(__file__).resolve().parent
    src_dtwm = tests_dir.parent

    cc = os.environ.get("CC", "cc")
    wl_scanner = shutil.which("wayland-scanner")
    grim = shutil.which("grim")
    if not wl_scanner or not grim:
        raise SystemExit("wayland-scanner and grim are required")

    tmp_parent = os.environ.get("TMPDIR")
    with tempfile.TemporaryDirectory(prefix="dtwm-drag-ime-scanout-", dir=tmp_parent) as td:
        tmp = Path(td)
        rt_dir = tmp / "rt"
        home_dir = tmp / "home"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt").mkdir(parents=True)

        proto_root = Path("/usr/share/wayland-protocols")
        protos = [
            ("xdg-shell", proto_root / "stable/xdg-shell/xdg-shell.xml"),
            ("xdg-decoration-unstable-v1", proto_root / "unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"),
            ("xdg-toplevel-drag-v1", proto_root / "staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml"),
            ("text-input-unstable-v3", proto_root / "unstable/text-input/text-input-unstable-v3.xml"),
            ("input-method-unstable-v2", src_dtwm / "wayland/protocols/input-method-unstable-v2.xml"),
        ]
        for tag, xml in protos:
            if not xml.is_file():
                raise SystemExit(f"missing protocol XML {tag}: {xml}")
            subprocess.run([wl_scanner, "client-header", str(xml), str(tmp / f"{tag}-client-protocol.h")], check=True)
            subprocess.run([wl_scanner, "private-code", str(xml), str(tmp / f"{tag}-protocol.c")], check=True)

        wl_cflags = subprocess.check_output(["pkg-config", "--cflags", "wayland-client"], text=True).split()
        wl_libs = subprocess.check_output(["pkg-config", "--libs", "wayland-client"], text=True).split()

        drag_c = tmp / "drag_client.c"
        drag_bin = tmp / "drag_client"
        drag_c.write_text(DRAG_TEST_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [
                cc, "-O2", "-Wall", f"-I{tmp}", str(drag_c),
                str(tmp / "xdg-shell-protocol.c"),
                str(tmp / "xdg-toplevel-drag-v1-protocol.c"),
                *wl_cflags, *wl_libs, "-lrt", "-o", str(drag_bin),
            ],
            check=True,
        )

        ime_c = tmp / "ime_sub_client.c"
        ime_bin = tmp / "ime_sub_client"
        ime_c.write_text(IME_SUBSURFACE_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [
                cc, "-O2", "-Wall", f"-I{tmp}", str(ime_c),
                str(tmp / "xdg-shell-protocol.c"),
                str(tmp / "xdg-decoration-unstable-v1-protocol.c"),
                str(tmp / "text-input-unstable-v3-protocol.c"),
                str(tmp / "input-method-unstable-v2-protocol.c"),
                *wl_cflags, *wl_libs, "-lrt", "-o", str(ime_bin),
            ],
            check=True,
        )

        vptr_bin = tmp / "vptr_helper"
        subprocess.run(
            [cc, "-O2", str(tests_dir / "wayland-test-pointer.c"), *wl_cflags, *wl_libs, "-o", str(vptr_bin)],
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

        comp_proc = subprocess.Popen(
            [str(comp_path), "--no-xwayland", "--no-panel", "--status-file", str(status_file)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        vptr_proc: subprocess.Popen | None = None
        target_proc: subprocess.Popen | None = None
        drag_proc: subprocess.Popen | None = None
        ime_proc: subprocess.Popen | None = None

        try:
            if not wait_until(lambda: "WAYLAND_DISPLAY" in parse_kv(status_file), timeout=10.0):
                raise RuntimeError("dtwm-wayland failed to write status file")
            wl_sock = parse_kv(status_file)["WAYLAND_DISPLAY"]
            env["WAYLAND_DISPLAY"] = wl_sock
            ipc_path = rt_dir / f"dtwm-wayland-{wl_sock}.ipc"
            if not wait_until(lambda: ipc_path.exists(), timeout=5.0):
                raise RuntimeError(f"missing IPC socket {ipc_path}")

            def get_pres_state() -> dict[str, str]:
                props = ipc_read_props(ipc_path)
                raw = props.get("_DT_WAYLAND_PRESENTATION_STATE", b"").decode("utf-8", errors="replace")
                return parse_semi_kv(raw)

            def get_ime_state() -> dict[str, str]:
                props = ipc_read_props(ipc_path)
                raw = props.get("_DT_WAYLAND_IME_POPUP_STATE", b"").decode("utf-8", errors="replace")
                return parse_semi_kv(raw)

            st0 = get_pres_state()
            assert st0.get("toplevel_drag_mgr") == "1", f"expected toplevel_drag_mgr=1, got {st0}"

            # 1. Verify xdg-toplevel-drag-v1 protocol error guards
            for err_mode in ("err_dup_source", "err_set_selection", "err_selection_then_drag", "err_double_attach"):
                subprocess.run([str(drag_bin), err_mode], env=env, check=True, timeout=5.0)

            vptr_proc = subprocess.Popen(
                [str(vptr_bin)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
            )
            ppm0 = subprocess.check_output([grim, "-t", "ppm", "-"], env=env, timeout=5.0)
            gw, gh, _ = read_ppm(ppm0)

            # 2. Verify live xdg-toplevel-drag-v1 tab tear-off, cursor tracking, unmap re-attach, & DND drop underneath
            t_state = tmp / "target.state"
            t_cmd = tmp / "target.cmd"
            target_proc = subprocess.Popen([str(drag_bin), "target_only", str(t_state), str(t_cmd)], env=env)
            if not wait_until(lambda: parse_kv(t_state).get("TARGET_CFG") == "1", timeout=6.0):
                raise AssertionError(f"DropTargetWin failed to configure: {parse_kv(t_state)}")
            target_box = parse_active_box(ipc_read_props(ipc_path))
            assert target_box is not None, "missing active box for DropTargetWin"
            tx, ty, tow, toh, tcx, tcy, tcw, tch = target_box

            d_state = tmp / "drag.state"
            d_cmd = tmp / "drag.cmd"
            drag_proc = subprocess.Popen([str(drag_bin), "live", str(d_state), str(d_cmd)], env=env)
            if not wait_until(lambda: parse_kv(d_state).get("SRC_CFG") == "1", timeout=8.0):
                raise AssertionError(f"DragSourceWin failed to configure: {parse_kv(d_state)}")
            if not wait_until(
                lambda: parse_active_box(ipc_read_props(ipc_path)) != target_box,
                timeout=4.0,
            ):
                raise AssertionError("DragSourceWin did not become active")

            src_box = parse_active_box(ipc_read_props(ipc_path))
            assert src_box is not None, "missing active box for DragSourceWin"
            sx, sy, sow, soh, scx, scy, scw, sch = src_box
            # Click inside DragSourceWin client area to acquire button press serial and hold button 272 down
            press_x = scx + 60
            press_y = scy + 60
            vptr_cmd(vptr_proc, f"move {press_x} {press_y} {gw} {gh}")
            vptr_cmd(vptr_proc, "down 272")
            if not wait_until(lambda: int(parse_kv(d_state).get("BTN_COUNT", "0")) >= 1, timeout=4.0):
                raise AssertionError(f"DragSourceWin did not receive button press: {parse_kv(d_state)}")

            send_cmd(d_cmd, "start_tearoff_drag")
            if not wait_until(
                lambda: parse_kv(d_state).get("TORN_CFG") == "1"
                and int(get_pres_state().get("toplevel_drags", "0")) >= 1,
                timeout=6.0,
            ):
                raise AssertionError(f"tear-off drag did not attach: {parse_kv(d_state)} / {get_pres_state()}")

            # Move pointer to (720, 460): TornOffTabWin (magenta #ee11cc, attached at offset 40,20) must follow live!
            vptr_cmd(vptr_proc, f"move 720 460 {gw} {gh}")
            time.sleep(0.12)
            ppm_drag = subprocess.check_output([grim, "-t", "ppm", "-"], env=env, timeout=5.0)
            _, _, rgb_drag = read_ppm(ppm_drag)
            mr, mg, mb = sample_rgb(rgb_drag, gw, 760, 500)  # inside TornOffTabWin client (+40,+40 from cursor)
            assert mr >= 200 and mg < 50 and mb >= 160, (
                f"expected torn-off magenta window tracking cursor around (760,500), got RGB={(mr, mg, mb)}"
            )

            # Test mid-drag unmap -> auto-detach -> re-attach replacement window
            drags_before_reattach = int(get_pres_state().get("toplevel_drags", "0"))
            send_cmd(d_cmd, "unmap_reattach")
            if not wait_until(
                lambda: int(get_pres_state().get("toplevel_drags", "0")) > drags_before_reattach,
                timeout=5.0,
            ):
                raise AssertionError(f"expected toplevel_drags>{drags_before_reattach} after unmap_reattach, got {get_pres_state()}")
            time.sleep(0.08)

            # Move pointer over DropTargetWin's exposed bottom-left client region (tcx+14, tcy+290)
            drop_x = tcx + 14
            drop_y = tcy + 290
            vptr_cmd(vptr_proc, f"move {drop_x} {drop_y} {gw} {gh}")
            if not wait_until(lambda: parse_kv(t_state).get("DND_ENTER") == "1", timeout=5.0):
                raise AssertionError(
                    f"DropTargetWin underneath attached toplevel did not receive wl_data_device.enter: {parse_kv(t_state)}"
                )
            vptr_cmd(vptr_proc, f"move {drop_x + 8} {drop_y + 6} {gw} {gh}")
            vptr_cmd(vptr_proc, "up 272")
            if not wait_until(
                lambda: parse_kv(t_state).get("DND_DROP") == "1"
                and parse_kv(d_state).get("DROP_PERFORMED") == "1"
                and parse_kv(d_state).get("FINISHED") == "1",
                timeout=5.0,
            ):
                raise AssertionError(
                    f"DND drop did not complete on target window: target={parse_kv(t_state)} src={parse_kv(d_state)}"
                )

            send_cmd(d_cmd, "finish_drag_objs")
            send_cmd(t_cmd, "quit")
            target_proc.wait(timeout=5.0)
            target_proc = None
            time.sleep(0.08)

            # 3. Verify xdg_toplevel fullscreen + unoccluded direct scanout fast path + occlusion guard + restore
            # Focus DragSourceWin first by clicking its interior
            vptr_cmd(vptr_proc, f"move {scx + 220} {scy + 160} {gw} {gh}")
            vptr_cmd(vptr_proc, "down 272")
            vptr_cmd(vptr_proc, "up 272")
            time.sleep(0.1)
            pre_fs_box = parse_active_box(ipc_read_props(ipc_path))
            assert pre_fs_box is not None

            send_cmd(d_cmd, "fullscreen_on")
            if not wait_until(
                lambda: parse_kv(d_state).get("FS_CFG") == "1"
                and int(parse_kv(d_state).get("FS_W", "0")) == gw
                and int(parse_kv(d_state).get("FS_H", "0")) == gh,
                timeout=6.0,
            ):
                raise AssertionError(f"fullscreen configure did not match {gw}x{gh}: {parse_kv(d_state)}")

            send_cmd(d_cmd, "commit_frame")
            if not wait_until(lambda: int(get_pres_state().get("scanout_candidates", "0")) >= 1, timeout=5.0):
                raise AssertionError(f"unoccluded fullscreen view did not hit scanout candidate path: {get_pres_state()}")

            # Open an xdg_popup child -> view is now occluded -> scanout_candidates must NOT advance
            send_cmd(d_cmd, "open_occluding_popup")
            time.sleep(0.08)
            cand_before_occluded = int(get_pres_state().get("scanout_candidates", "0"))
            send_cmd(d_cmd, "commit_frame")
            time.sleep(0.08)
            cand_while_occluded = int(get_pres_state().get("scanout_candidates", "0"))
            assert cand_while_occluded == cand_before_occluded, (
                f"expected scanout suppressed while popup occludes fullscreen view ({cand_before_occluded} -> {cand_while_occluded})"
            )

            send_cmd(d_cmd, "close_occluding_popup")
            send_cmd(d_cmd, "fullscreen_off")
            if not wait_until(
                lambda: parse_kv(d_state).get("FS_CFG") == "0"
                and int(parse_kv(d_state).get("FS_W", "0")) == 320,
                timeout=5.0,
            ):
                raise AssertionError(f"unset_fullscreen did not restore 320x220 geometry: {parse_kv(d_state)}")

            send_cmd(d_cmd, "quit")
            drag_proc.wait(timeout=5.0)
            drag_proc = None

            # 4. Verify zwp_input_popup_surface_v2 child wl_subsurface caret anchoring, flip, & live window move
            i_state = tmp / "ime.state"
            i_cmd = tmp / "ime.cmd"
            ime_proc = subprocess.Popen([str(ime_bin), str(i_state), str(i_cmd)], env=env)
            if not wait_until(lambda: parse_kv(i_state).get("WIN_CFG") == "1", timeout=6.0):
                raise AssertionError(f"IME subsurface window failed to configure: {parse_kv(i_state)}")

            ime_box = parse_active_box(ipc_read_props(ipc_path))
            assert ime_box is not None
            ix, iy, iow, ioh, icx, icy, icw, ich = ime_box
            # Click directly inside the child wl_subsurface at (client_origin + 110 + 40, client_origin + 90 + 40)
            sub_click_x = icx + 150
            sub_click_y = icy + 130
            vptr_cmd(vptr_proc, f"move {sub_click_x} {sub_click_y} {gw} {gh}")
            vptr_cmd(vptr_proc, "down 272")
            vptr_cmd(vptr_proc, "up 272")
            if not wait_until(lambda: parse_kv(i_state).get("SUB_ENTER") == "1", timeout=5.0):
                raise AssertionError(f"zwp_text_input_v3 did not enter child wl_subsurface: {parse_kv(i_state)}")

            # Enable text_input with caret (25, 30, 2, 22) inside subsurface (110, 90)
            send_cmd(i_cmd, "caret 25 30 2 22")
            exp_px = icx + 110 + 25
            exp_py = icy + 90 + 30 + 22 + 2
            if not wait_until(
                lambda: get_ime_state().get("visible") == "1"
                and int(parse_kv(i_state).get("RECT_EVENTS", "0")) >= 1,
                timeout=5.0,
            ):
                raise AssertionError(f"IME popup did not show: {get_ime_state()} / {parse_kv(i_state)}")
            ist = get_ime_state()
            assert abs(int(ist["px"]) - exp_px) <= 2 and abs(int(ist["py"]) - exp_py) <= 2, (
                f"expected subsurface IME popup at ({exp_px},{exp_py}), got {ist}"
            )

            # Drag parent window titlebar +90px right, +50px down -> popup must re-anchor live!
            title_x = ix + iow // 2
            title_y = iy + max(6, (icy - iy) // 2)
            vptr_cmd(vptr_proc, f"move {title_x} {title_y} {gw} {gh}")
            vptr_cmd(vptr_proc, "down 272")
            vptr_cmd(vptr_proc, f"move {title_x + 90} {title_y + 50} {gw} {gh}")
            vptr_cmd(vptr_proc, "up 272")
            if not wait_until(
                lambda: abs(int(get_ime_state().get("px", "0")) - (exp_px + 90)) <= 4
                and abs(int(get_ime_state().get("py", "0")) - (exp_py + 50)) <= 4,
                timeout=5.0,
            ):
                raise AssertionError(
                    f"expected live IME popup re-anchor to ({exp_px + 90},{exp_py + 50}), "
                    f"got ime={get_ime_state()} orig_box={ime_box} new_box={parse_active_box(ipc_read_props(ipc_path))} "
                    f"client={parse_kv(i_state)}"
                )

            # Set caret near bottom of monitor so popup flips ABOVE caret (rel_y > 0)
            send_cmd(i_cmd, f"caret 25 {gh} 2 22")
            if not wait_until(lambda: int(get_ime_state().get("rel_y", "-1")) > 0, timeout=5.0):
                raise AssertionError(f"expected bottom-edge flip (rel_y > 0), got {get_ime_state()}")

            send_cmd(i_cmd, "quit")
            ime_proc.wait(timeout=5.0)
            ime_proc = None
            vptr_cmd(vptr_proc, "quit")
            vptr_proc.wait(timeout=3.0)
            vptr_proc = None
            print("PASS: check-dtwm-wayland-drag-ime-scanout")
            return 0
        finally:
            for p in (ime_proc, drag_proc, target_proc, vptr_proc, comp_proc):
                if p and p.poll() is None:
                    with suppress(Exception):
                        p.terminate()
                        p.wait(timeout=4.0)
                    if p.poll() is None:
                        with suppress(Exception):
                            p.kill()


if __name__ == "__main__":
    raise SystemExit(main())
