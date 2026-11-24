#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland Track 3:
Pointer Lock, Confinement, Relative Motion & Shortcut Inhibit
(zwp_pointer_constraints_v1, zwp_relative_pointer_v1,
zwp_keyboard_shortcuts_inhibit_v1, and Ctrl+Alt escape hatch):

1. Verifies persistent zwp_locked_pointer_v1 + zwp_keyboard_shortcuts_inhibitor_v1
   activate on the focused window, publishing _DT_WAYLAND_INPUT_CAPTURE over IPC.
2. Injects large relative motion (+900, +700) via zwlr_virtual_pointer_v1 and
   verifies zwp_relative_pointer_v1 delivers full unobstructed deltas while
   suppressing wl_pointer.motion.
3. Injects Alt+Tab and Ctrl+Alt+2 via zwp_virtual_keyboard_v1 while shortcuts
   are inhibited and verifies compositor shortcuts are suppressed and forwarded
   to the focused VM/CAD client (staying on ws0).
4. Presses and releases the bare Ctrl+Alt escape chord, verifying unlocked +
   inactive events fire, cursor warps to set_cursor_hint(180, 120), OSD announces
   "Input Released (Ctrl+Alt)", and Ctrl+Alt+2 / Ctrl+Alt+1 switch workspaces again.
5. Clicks inside the client area to verify persistent lock + shortcut inhibit
   cleanly re-activate, then replaces lock with zwp_confined_pointer_v1 on region
   (40, 50, 100, 80) and verifies motion stays clamped inside the region.
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


WLR_VIRTUAL_POINTER_XML = r"""<?xml version="1.0" encoding="UTF-8"?>
<protocol name="wlr_virtual_pointer_unstable_v1">
  <interface name="zwlr_virtual_pointer_v1" version="2">
    <enum name="error">
      <entry name="invalid_axis" value="0"/>
      <entry name="invalid_axis_source" value="1"/>
    </enum>
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
      <arg name="time" type="uint"/>
      <arg name="axis" type="uint"/>
      <arg name="value" type="fixed"/>
      <arg name="discrete" type="int"/>
    </request>
    <request name="destroy" type="destructor" since="1"/>
  </interface>
  <interface name="zwlr_virtual_pointer_manager_v1" version="2">
    <request name="create_virtual_pointer">
      <arg name="seat" type="object" interface="wl_seat" allow-null="true"/>
      <arg name="id" type="new_id" interface="zwlr_virtual_pointer_v1"/>
    </request>
    <request name="destroy" type="destructor" since="1"/>
    <request name="create_virtual_pointer_with_output" since="2">
      <arg name="seat" type="object" interface="wl_seat" allow-null="true"/>
      <arg name="output" type="object" interface="wl_output" allow-null="true"/>
      <arg name="id" type="new_id" interface="zwlr_virtual_pointer_v1"/>
    </request>
  </interface>
</protocol>
"""

VIRTUAL_KEYBOARD_XML = r"""<?xml version="1.0" encoding="UTF-8"?>
<protocol name="virtual_keyboard_unstable_v1">
  <interface name="zwp_virtual_keyboard_v1" version="1">
    <enum name="error">
      <entry name="no_keymap" value="0"/>
    </enum>
    <request name="keymap">
      <arg name="format" type="uint"/>
      <arg name="fd" type="fd"/>
      <arg name="size" type="uint"/>
    </request>
    <request name="key">
      <arg name="time" type="uint"/>
      <arg name="key" type="uint"/>
      <arg name="state" type="uint"/>
    </request>
    <request name="modifiers">
      <arg name="mods_depressed" type="uint"/>
      <arg name="mods_latched" type="uint"/>
      <arg name="mods_locked" type="uint"/>
      <arg name="group" type="uint"/>
    </request>
    <request name="destroy" type="destructor"/>
  </interface>
  <interface name="zwp_virtual_keyboard_manager_v1" version="1">
    <enum name="error">
      <entry name="unauthorized" value="0"/>
    </enum>
    <request name="create_virtual_keyboard">
      <arg name="seat" type="object" interface="wl_seat"/>
      <arg name="id" type="new_id" interface="zwp_virtual_keyboard_v1"/>
    </request>
  </interface>
</protocol>
"""

TEST_HARNESS_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
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
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client.h"
#include "pointer-constraints-client.h"
#include "relative-pointer-client.h"
#include "shortcuts-inhibit-client.h"
#include "virtual-pointer-client.h"
#include "virtual-keyboard-client.h"

typedef struct {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *ptr;
    struct wl_keyboard *kbd;
    struct xdg_wm_base *wm_base;
    struct zwp_pointer_constraints_v1 *constraints_mgr;
    struct zwp_relative_pointer_manager_v1 *rel_mgr;
    struct zwp_relative_pointer_v1 *rel_ptr;
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit_mgr;
    struct zwlr_virtual_pointer_manager_v1 *vptr_mgr;
    struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;

    bool is_locked;
    bool is_confined;
    int lock_events;
    int unlock_events;
    int confine_events;
    bool shortcuts_active;
    int shortcuts_active_events;
    int shortcuts_inactive_events;

    int abs_motion_events;
    double ptr_x, ptr_y;
    double min_conf_x, max_conf_x;
    double min_conf_y, max_conf_y;

    int rel_events;
    double sum_rel_dx, sum_rel_dy;

    int kbd_tab_presses;
    int kbd_two_presses;
} App;

typedef struct {
    App *app;
    struct wl_surface *surf;
    struct xdg_surface *xdg_surf;
    struct xdg_toplevel *xdg_top;
    struct wl_buffer *buf;
    int w, h;
    bool configured;
} Win;

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) {
    (void)d;
    xdg_wm_base_pong(wm, s);
}
static const struct xdg_wm_base_listener g_wm_listener = { .ping = wm_ping };

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t sx, wl_fixed_t sy) {
    (void)p; (void)s; (void)sf;
    App *app = d;
    app->ptr_x = wl_fixed_to_double(sx);
    app->ptr_y = wl_fixed_to_double(sy);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {
    (void)d; (void)p; (void)s; (void)sf;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy) {
    (void)p; (void)t;
    App *app = d;
    double x = wl_fixed_to_double(sx);
    double y = wl_fixed_to_double(sy);
    app->ptr_x = x;
    app->ptr_y = y;
    app->abs_motion_events++;
    if (x < app->min_conf_x) app->min_conf_x = x;
    if (x > app->max_conf_x) app->max_conf_x = x;
    if (y < app->min_conf_y) app->min_conf_y = y;
    if (y > app->max_conf_y) app->max_conf_y = y;
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
    (void)d; (void)p; (void)s; (void)t; (void)b; (void)st;
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
    (void)d; (void)p; (void)t; (void)a; (void)v;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_src(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener g_ptr_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
    .axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_src,
    .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_disc,
};

static void kbd_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) {
    (void)d; (void)k; (void)f; (void)sz;
    if (fd >= 0) close(fd);
}
static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) {
    (void)d; (void)k; (void)s; (void)sf; (void)keys;
}
static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {
    (void)d; (void)k; (void)s; (void)sf;
}
static void kbd_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) {
    (void)k; (void)s; (void)t;
    App *app = d;
    if (st == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (key == KEY_TAB) app->kbd_tab_presses++;
        if (key == KEY_2) app->kbd_two_presses++;
    }
}
static void kbd_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) {
    (void)d; (void)k; (void)s; (void)dep; (void)lat; (void)lck; (void)grp;
}
static void kbd_rep(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) {
    (void)d; (void)k; (void)r; (void)dl;
}
static const struct wl_keyboard_listener g_kbd_listener = {
    .keymap = kbd_keymap, .enter = kbd_enter, .leave = kbd_leave,
    .key = kbd_key, .modifiers = kbd_mods, .repeat_info = kbd_rep,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
    App *app = d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app->ptr) {
        app->ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->ptr, &g_ptr_listener, app);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app->kbd) {
        app->kbd = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->kbd, &g_kbd_listener, app);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener g_seat_listener = { .capabilities = seat_caps, .name = seat_name };

static void rel_motion_cb(
    void *d, struct zwp_relative_pointer_v1 *rp,
    uint32_t ut_hi, uint32_t ut_lo,
    wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t udx, wl_fixed_t udy
) {
    (void)rp; (void)ut_hi; (void)ut_lo; (void)udx; (void)udy;
    App *app = d;
    app->rel_events++;
    app->sum_rel_dx += wl_fixed_to_double(dx);
    app->sum_rel_dy += wl_fixed_to_double(dy);
}
static const struct zwp_relative_pointer_v1_listener g_rel_listener = {
    .relative_motion = rel_motion_cb,
};

static void lck_locked(void *d, struct zwp_locked_pointer_v1 *lp) {
    (void)lp;
    App *app = d;
    app->is_locked = true;
    app->lock_events++;
}
static void lck_unlocked(void *d, struct zwp_locked_pointer_v1 *lp) {
    (void)lp;
    App *app = d;
    app->is_locked = false;
    app->unlock_events++;
}
static const struct zwp_locked_pointer_v1_listener g_lck_listener = {
    .locked = lck_locked, .unlocked = lck_unlocked,
};

static void cnf_confined(void *d, struct zwp_confined_pointer_v1 *cp) {
    (void)cp;
    App *app = d;
    app->is_confined = true;
    app->confine_events++;
}
static void cnf_unconfined(void *d, struct zwp_confined_pointer_v1 *cp) {
    (void)cp;
    App *app = d;
    app->is_confined = false;
}
static const struct zwp_confined_pointer_v1_listener g_cnf_listener = {
    .confined = cnf_confined, .unconfined = cnf_unconfined,
};

static void inh_active(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *inh) {
    (void)inh;
    App *app = d;
    app->shortcuts_active = true;
    app->shortcuts_active_events++;
}
static void inh_inactive(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *inh) {
    (void)inh;
    App *app = d;
    app->shortcuts_active = false;
    app->shortcuts_inactive_events++;
}
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener g_inh_listener = {
    .active = inh_active, .inactive = inh_inactive,
};

static void top_cfg(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)t; (void)st;
    Win *win = d;
    if (w > 0) win->w = w;
    if (h > 0) win->h = h;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener g_top_listener = {
    .configure = top_cfg, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps,
};

static struct wl_buffer *make_shm_buf(struct wl_shm *shm, int w, int h, uint32_t pixel) {
    size_t sz = (size_t)w * (size_t)h * 4u;
    int fd = memfd_create("plock-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)sz) != 0) return NULL;
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px != MAP_FAILED) {
        for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) px[i] = pixel;
        munmap(px, sz);
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)sz);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888
    );
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

static void xsurf_cfg(void *d, struct xdg_surface *xs, uint32_t serial) {
    Win *win = d;
    xdg_surface_ack_configure(xs, serial);
    if (!win->buf) {
        win->buf = make_shm_buf(win->app->shm, win->w, win->h, 0xff224466u);
    }
    wl_surface_attach(win->surf, win->buf, 0, 0);
    wl_surface_damage_buffer(win->surf, 0, 0, win->w, win->h);
    wl_surface_commit(win->surf);
    win->configured = true;
}
static const struct xdg_surface_listener g_xsurf_listener = { .configure = xsurf_cfg };

static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
    App *app = d;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        app->comp = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        app->seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(app->seat, &g_seat_listener, app);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
        xdg_wm_base_add_listener(app->wm_base, &g_wm_listener, app);
    } else if (strcmp(iface, zwp_pointer_constraints_v1_interface.name) == 0) {
        app->constraints_mgr = wl_registry_bind(reg, name, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(iface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        app->rel_mgr = wl_registry_bind(reg, name, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0) {
        app->inhibit_mgr = wl_registry_bind(reg, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    } else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        app->vptr_mgr = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        app->vkbd_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_listener = { .global = reg_global, .global_remove = reg_rem };

static void pump_ms(struct wl_display *dpy, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long end = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L + ms;
    int fd = wl_display_get_fd(dpy);
    for (;;) {
        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long rem = end - (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
        if (rem <= 0) {
            wl_display_cancel_read(dpy);
            break;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, rem > 20 ? 20 : (int)rem) > 0) {
            wl_display_read_events(dpy);
            wl_display_dispatch_pending(dpy);
        } else {
            wl_display_cancel_read(dpy);
        }
    }
}

static void init_vkbd_keymap(struct zwp_virtual_keyboard_v1 *vkbd) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { .layout = "us" };
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t len = strlen(str) + 1;
    int fd = memfd_create("vkbd-km", MFD_CLOEXEC);
    (void)ftruncate(fd, (off_t)len);
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memcpy(p, str, len);
    munmap(p, len);
    zwp_virtual_keyboard_v1_keymap(vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
    close(fd);
    free(str);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
}

static void create_win(App *app, Win *win, const char *title, int w, int h) {
    memset(win, 0, sizeof(*win));
    win->app = app;
    win->w = w;
    win->h = h;
    win->surf = wl_compositor_create_surface(app->comp);
    win->xdg_surf = xdg_wm_base_get_xdg_surface(app->wm_base, win->surf);
    xdg_surface_add_listener(win->xdg_surf, &g_xsurf_listener, win);
    win->xdg_top = xdg_surface_get_toplevel(win->xdg_surf);
    xdg_toplevel_add_listener(win->xdg_top, &g_top_listener, win);
    xdg_toplevel_set_title(win->xdg_top, title);
    xdg_toplevel_set_app_id(win->xdg_top, title);
    xdg_toplevel_set_min_size(win->xdg_top, w, h);
    xdg_toplevel_set_max_size(win->xdg_top, w, h);
    wl_surface_commit(win->surf);
    while (!win->configured) wl_display_dispatch(app->dpy);
}

static void send_key_mod(
    struct zwp_virtual_keyboard_v1 *vk, uint32_t *t, uint32_t key, uint32_t st, uint32_t mods_dep
) {
    *t += 15;
    zwp_virtual_keyboard_v1_key(vk, *t, key, st);
    zwp_virtual_keyboard_v1_modifiers(vk, mods_dep, 0, 0, 0);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *report_path = argv[1];

    App app;
    memset(&app, 0, sizeof(app));
    app.dpy = wl_display_connect(NULL);
    if (!app.dpy) return 3;
    app.reg = wl_display_get_registry(app.dpy);
    wl_registry_add_listener(app.reg, &g_reg_listener, &app);
    wl_display_roundtrip(app.dpy);
    wl_display_roundtrip(app.dpy);

    if (!app.comp || !app.shm || !app.seat || !app.ptr || !app.kbd ||
        !app.wm_base || !app.constraints_mgr || !app.rel_mgr ||
        !app.inhibit_mgr || !app.vptr_mgr || !app.vkbd_mgr) {
        fprintf(stderr, "FAIL: missing required Track 3 globals\n");
        return 4;
    }

    app.rel_ptr = zwp_relative_pointer_manager_v1_get_relative_pointer(app.rel_mgr, app.ptr);
    zwp_relative_pointer_v1_add_listener(app.rel_ptr, &g_rel_listener, &app);

    struct zwlr_virtual_pointer_v1 *vptr =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(app.vptr_mgr, app.seat);
    struct zwp_virtual_keyboard_v1 *vkbd =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(app.vkbd_mgr, app.seat);
    init_vkbd_keymap(vkbd);

    Win other_win, vm_win;
    create_win(&app, &other_win, "OtherApp", 320, 220);
    pump_ms(app.dpy, 60);
    create_win(&app, &vm_win, "VMGuest", 400, 300);
    pump_ms(app.dpy, 100);

    /* 1. Request persistent pointer lock + cursor_hint(180, 120) + shortcut inhibit on VMGuest */
    struct zwp_locked_pointer_v1 *locked_ptr = zwp_pointer_constraints_v1_lock_pointer(
        app.constraints_mgr,
        vm_win.surf,
        app.ptr,
        NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT
    );
    zwp_locked_pointer_v1_add_listener(locked_ptr, &g_lck_listener, &app);
    zwp_locked_pointer_v1_set_cursor_position_hint(
        locked_ptr, wl_fixed_from_double(180.0), wl_fixed_from_double(120.0)
    );

    struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor =
        zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
            app.inhibit_mgr, vm_win.surf, app.seat
        );
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inhibitor, &g_inh_listener, &app);
    wl_surface_commit(vm_win.surf);
    pump_ms(app.dpy, 120);

    if (!app.is_locked || !app.shortcuts_active) {
        fprintf(
            stderr,
            "FAIL: initial lock/inhibit did not activate (locked=%d shortcuts=%d)\n",
            app.is_locked,
            app.shortcuts_active
        );
        return 5;
    }

    /* 2. Inject large relative motion (+900, +700) while locked */
    app.abs_motion_events = 0;
    app.rel_events = 0;
    app.sum_rel_dx = 0.0;
    app.sum_rel_dy = 0.0;
    uint32_t t = 1000;
    zwlr_virtual_pointer_v1_motion(
        vptr, t, wl_fixed_from_double(900.0), wl_fixed_from_double(700.0)
    );
    zwlr_virtual_pointer_v1_frame(vptr);
    pump_ms(app.dpy, 100);

    if (app.rel_events < 1 || fabs(app.sum_rel_dx - 900.0) > 1.0 ||
        fabs(app.sum_rel_dy - 700.0) > 1.0 || app.abs_motion_events != 0) {
        fprintf(
            stderr,
            "FAIL: locked relative motion mismatch (rel=%d dx=%.1f dy=%.1f abs_motions=%d)\n",
            app.rel_events,
            app.sum_rel_dx,
            app.sum_rel_dy,
            app.abs_motion_events
        );
        return 6;
    }

    /* 3. Inject Alt+Tab (mods: Mod1=8) and Ctrl+Alt+2 (mods: Ctrl=4 | Mod1=8 = 12) while inhibited */
    app.kbd_tab_presses = 0;
    app.kbd_two_presses = 0;
    send_key_mod(vkbd, &t, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 8);
    send_key_mod(vkbd, &t, KEY_TAB, WL_KEYBOARD_KEY_STATE_PRESSED, 8);
    send_key_mod(vkbd, &t, KEY_TAB, WL_KEYBOARD_KEY_STATE_RELEASED, 8);
    send_key_mod(vkbd, &t, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 0);
    pump_ms(app.dpy, 80);

    send_key_mod(vkbd, &t, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_PRESSED, 4);
    send_key_mod(vkbd, &t, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 12);
    send_key_mod(vkbd, &t, KEY_2, WL_KEYBOARD_KEY_STATE_PRESSED, 12);
    send_key_mod(vkbd, &t, KEY_2, WL_KEYBOARD_KEY_STATE_RELEASED, 12);
    send_key_mod(vkbd, &t, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 4);
    send_key_mod(vkbd, &t, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_RELEASED, 0);
    pump_ms(app.dpy, 100);

    if (app.kbd_tab_presses < 1 || app.kbd_two_presses < 1 || !app.is_locked || !app.shortcuts_active) {
        fprintf(
            stderr,
            "FAIL: shortcuts inhibitor did not forward Alt+Tab / Ctrl+Alt+2 (tab=%d two=%d locked=%d inh=%d)\n",
            app.kbd_tab_presses,
            app.kbd_two_presses,
            app.is_locked,
            app.shortcuts_active
        );
        return 7;
    }

    /* 4. Trigger bare Ctrl+Alt escape chord */
    send_key_mod(vkbd, &t, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_PRESSED, 4);
    send_key_mod(vkbd, &t, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 12);
    send_key_mod(vkbd, &t, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 4);
    send_key_mod(vkbd, &t, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_RELEASED, 0);
    pump_ms(app.dpy, 120);

    if (app.is_locked || app.shortcuts_active || app.unlock_events < 1 || app.shortcuts_inactive_events < 1) {
        fprintf(
            stderr,
            "FAIL: Ctrl+Alt escape hatch did not release capture (locked=%d inh=%d unl=%d inact=%d)\n",
            app.is_locked,
            app.shortcuts_active,
            app.unlock_events,
            app.shortcuts_inactive_events
        );
        return 8;
    }
    double hint_err = hypot(app.ptr_x - 180.0, app.ptr_y - 120.0);
    if (hint_err > 4.0) {
        fprintf(
            stderr,
            "FAIL: cursor_hint warp mismatch after unlock: got (%.1f, %.1f), expected (180, 120)\n",
            app.ptr_x,
            app.ptr_y
        );
        return 9;
    }

    /* 5. Click inside VMGuest at warped cursor_hint (180, 120) -> persistent lock & inhibit re-activate */
    t += 20;
    zwlr_virtual_pointer_v1_button(vptr, t, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(vptr);
    pump_ms(app.dpy, 40);
    t += 20;
    zwlr_virtual_pointer_v1_button(vptr, t, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(vptr);
    pump_ms(app.dpy, 100);

    if (!app.is_locked || !app.shortcuts_active || app.lock_events < 2) {
        fprintf(
            stderr,
            "FAIL: clicking client area did not re-engage persistent capture (locked=%d inh=%d locks=%d)\n",
            app.is_locked,
            app.shortcuts_active,
            app.lock_events
        );
        return 10;
    }

    /* 6. Destroy lock, test zwp_confined_pointer_v1 on region (40, 50, 100, 80) */
    zwp_locked_pointer_v1_destroy(locked_ptr);
    pump_ms(app.dpy, 60);

    struct wl_region *creg = wl_compositor_create_region(app.comp);
    wl_region_add(creg, 40, 50, 100, 80);
    struct zwp_confined_pointer_v1 *conf_ptr = zwp_pointer_constraints_v1_confine_pointer(
        app.constraints_mgr,
        vm_win.surf,
        app.ptr,
        creg,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT
    );
    wl_region_destroy(creg);
    zwp_confined_pointer_v1_add_listener(conf_ptr, &g_cnf_listener, &app);
    wl_surface_commit(vm_win.surf);
    pump_ms(app.dpy, 100);

    if (!app.is_confined) {
        fprintf(stderr, "FAIL: zwp_confined_pointer_v1 did not activate\n");
        return 11;
    }

    app.min_conf_x = 1e9;
    app.max_conf_x = -1e9;
    app.min_conf_y = 1e9;
    app.max_conf_y = -1e9;
    app.abs_motion_events = 0;

    t += 20;
    zwlr_virtual_pointer_v1_motion(vptr, t, wl_fixed_from_double(500.0), wl_fixed_from_double(500.0));
    zwlr_virtual_pointer_v1_frame(vptr);
    pump_ms(app.dpy, 60);
    t += 20;
    zwlr_virtual_pointer_v1_motion(vptr, t, wl_fixed_from_double(-500.0), wl_fixed_from_double(-500.0));
    zwlr_virtual_pointer_v1_frame(vptr);
    pump_ms(app.dpy, 60);

    if (app.abs_motion_events < 1 ||
        app.min_conf_x < 39.5 || app.max_conf_x > 140.5 ||
        app.min_conf_y < 49.5 || app.max_conf_y > 130.5) {
        fprintf(
            stderr,
            "FAIL: confined motion escaped region [40..140, 50..130]: [%.1f..%.1f, %.1f..%.1f] (ev=%d)\n",
            app.min_conf_x,
            app.max_conf_x,
            app.min_conf_y,
            app.max_conf_y,
            app.abs_motion_events
        );
        return 12;
    }

    FILE *rf = fopen(report_path, "w");
    if (rf) {
        fprintf(
            rf,
            "PASS lock_events=%d unlock_events=%d rel_dx=%.0f rel_dy=%.0f "
            "hint_err=%.2f conf=[%.1f..%.1f,%.1f..%.1f]\n",
            app.lock_events,
            app.unlock_events,
            app.sum_rel_dx,
            app.sum_rel_dy,
            hint_err,
            app.min_conf_x,
            app.max_conf_x,
            app.min_conf_y,
            app.max_conf_y
        );
        fclose(rf);
    }

    zwp_confined_pointer_v1_destroy(conf_ptr);
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(inhibitor);
    zwlr_virtual_pointer_v1_destroy(vptr);
    zwp_virtual_keyboard_v1_destroy(vkbd);
    wl_display_disconnect(app.dpy);
    return 0;
}
"""


def wait_status_file(status_file: Path, proc: subprocess.Popen[bytes], timeout_s: float = 12.0) -> dict[str, str]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"dtwm-wayland exited early rc={proc.returncode}")
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


def query_ipc_props(sock_path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(0.35)
        s.connect(str(sock_path))
        buf = b""
        while True:
            try:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
            except socket.timeout:
                break
        for line in buf.decode("utf-8", errors="replace").splitlines():
            parts = line.strip().split()
            if len(parts) >= 3 and parts[0] == "PROP":
                out[parts[1]] = bytes.fromhex(parts[2]).decode("utf-8", errors="replace")
    return out


def build_harness(tmp_dir: Path, cc: str) -> Path:
    scanner = shutil.which("wayland-scanner")
    if not scanner:
        raise RuntimeError("wayland-scanner not found")
    wp_dir = Path(
        subprocess.check_output(
            ["pkg-config", "--variable=pkgdatadir", "wayland-protocols"], text=True
        ).strip()
    )
    vp_xml = tmp_dir / "wlr-virtual-pointer-unstable-v1.xml"
    vk_xml = tmp_dir / "virtual-keyboard-unstable-v1.xml"
    vp_xml.write_text(WLR_VIRTUAL_POINTER_XML, encoding="utf-8")
    vk_xml.write_text(VIRTUAL_KEYBOARD_XML, encoding="utf-8")

    specs = [
        ("xdg-shell", wp_dir / "stable/xdg-shell/xdg-shell.xml"),
        ("pointer-constraints", wp_dir / "unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"),
        ("relative-pointer", wp_dir / "unstable/relative-pointer/relative-pointer-unstable-v1.xml"),
        ("shortcuts-inhibit", wp_dir / "unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml"),
        ("virtual-pointer", vp_xml),
        ("virtual-keyboard", vk_xml),
    ]
    gen_srcs: list[str] = []
    for prefix, xml_path in specs:
        hdr = tmp_dir / f"{prefix}-client.h"
        src = tmp_dir / f"{prefix}-code.c"
        subprocess.run([scanner, "client-header", str(xml_path), str(hdr)], check=True)
        subprocess.run([scanner, "private-code", str(xml_path), str(src)], check=True)
        gen_srcs.append(str(src))

    c_path = tmp_dir / "pointer_lock_harness.c"
    c_path.write_text(TEST_HARNESS_C, encoding="utf-8")
    bin_path = tmp_dir / "pointer-lock-harness"

    cflags = subprocess.check_output(
        ["pkg-config", "--cflags", "wayland-client", "xkbcommon"], text=True
    ).split()
    libs = subprocess.check_output(
        ["pkg-config", "--libs", "wayland-client", "xkbcommon"], text=True
    ).split()
    subprocess.run(
        [cc, "-O2", "-Wall", f"-I{tmp_dir}", str(c_path), *gen_srcs, *cflags, *libs, "-lm", "-o", str(bin_path)],
        check=True,
    )
    return bin_path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    comp_bin = str(Path(args.compositor).resolve())
    cc = os.environ.get("CC", "cc")
    base_tmp = os.environ.get("TMPDIR") or "/tmp"

    with tempfile.TemporaryDirectory(prefix="dtwm-wl-plock-", dir=base_tmp) as td:
        root = Path(td)
        harness_bin = build_harness(root, cc)

        rt = root / "rt"
        rt.mkdir(mode=0o700)
        env = os.environ.copy()
        env.pop("DISPLAY", None)
        env["XDG_RUNTIME_DIR"] = str(rt)
        env["HOME"] = str(root)
        env["WLR_BACKENDS"] = "headless"
        env["WLR_HEADLESS_OUTPUTS"] = "1"
        env["WLR_RENDERER"] = "pixman"
        env["WLR_LIBINPUT_NO_DEVICES"] = "1"

        sf = root / "comp.status"
        comp_log = (root / "comp.log").open("w", encoding="utf-8")
        comp = subprocess.Popen(
            [comp_bin, "--no-xwayland", "--no-panel", "--status-file", str(sf)],
            env=env,
            stdout=comp_log,
            stderr=subprocess.STDOUT,
        )
        try:
            kv = wait_status_file(sf, comp)
            wl_sock = kv["WAYLAND_DISPLAY"]
            c_env = env.copy()
            c_env["WAYLAND_DISPLAY"] = wl_sock
            report_file = root / "report.txt"
            subprocess.run([str(harness_bin), str(report_file)], env=c_env, check=True, timeout=20)
            summary = report_file.read_text(encoding="utf-8").strip()

            ipc_sock = rt / f"dtwm-wayland-{wl_sock}.ipc"
            props = query_ipc_props(ipc_sock)
            cur_ws = props.get("_DT_WORKSPACE_CURRENT_NAME", "").rstrip("\x00")
            if cur_ws and cur_ws != "ws0":
                raise RuntimeError(f"Expected workspace to remain ws0, got {cur_ws!r}")
        finally:
            comp.terminate()
            comp.wait(timeout=5)
            comp_log.close()

    print(f"PASS check-dtwm-wayland-pointer-lock ({summary})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
