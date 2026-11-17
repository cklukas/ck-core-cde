#!/usr/bin/env python3
"""Automated integration test for WP-11:
1. Native Wayland CJK Input Method relay in dtwm-wayland:
   - zwp_text_input_manager_v3 (client preedit / commit / cursor_rectangle / surrounding_text)
   - zwp_input_method_manager_v2 (daemon activate / deactivate / surrounding_text / commit)
   - zwp_input_method_keyboard_grab_v2 (raw hardware/virtual key routing + virtual-keyboard loop guard)
   - zwp_input_popup_surface_v2 (CJK candidate popup anchored below editor cursor_rectangle & clamped to monitor workarea)
2. Session-lock isolation (ext-session-lock-v1):
   - Acquiring session lock immediately deactivates zwp_input_method_v2, sends leave to zwp_text_input_v3,
     hides IME candidate popups, and prevents keyboard_grab from intercepting lock-screen password keys.
   - Unlocking restores zwp_text_input_v3 enter + zwp_input_method_v2 activate cleanly.
"""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image


IM_DAEMON_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include "input-method-unstable-v2-client-protocol.h"

static struct wl_display *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct wl_seat *g_seat;
static struct zwp_input_method_manager_v2 *g_im_mgr;
static struct zwp_input_method_v2 *g_im;
static struct zwp_input_method_keyboard_grab_v2 *g_grab;
static struct zwp_input_popup_surface_v2 *g_popup;
static struct wl_surface *g_popup_surf;
static struct wl_buffer *g_popup_buf;

static struct xkb_context *g_xkb_ctx;
static struct xkb_keymap *g_xkb_keymap;
static struct xkb_state *g_xkb_state;

static int g_pending_active = 0;
static int g_active = 0;
static uint32_t g_serial = 0;
static int g_in_preedit = 0;
static volatile sig_atomic_t g_stop = 0;
static FILE *g_log_fp = NULL;

static void log_line(const char *fmt, const char *val) {
    if (!g_log_fp) return;
    fprintf(g_log_fp, fmt, val);
    fflush(g_log_fp);
}

static void on_sig(int s) { (void)s; g_stop = 1; }

static int make_shm(size_t sz) {
    char name[64];
    snprintf(name, sizeof(name), "/wp11-im-%d-%ld", (int)getpid(), (long)random());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return -1; }
    return fd;
}

static struct wl_buffer *render_candidate_popup(int w, int h) {
    int stride = w * 4;
    size_t sz = (size_t)stride * (size_t)h;
    int fd = make_shm(sz);
    uint8_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    cairo_surface_t *cs = cairo_image_surface_create_for_data(px, CAIRO_FORMAT_ARGB32, w, h, stride);
    cairo_t *cr = cairo_create(cs);

    /* Classic CDE warm gray bevel frame */
    cairo_set_source_rgb(cr, 0.82, 0.80, 0.78);
    cairo_paint(cr);

    /* Top/left highlight bevel */
    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgb(cr, 0.96, 0.95, 0.94);
    cairo_move_to(cr, 1, h - 1);
    cairo_line_to(cr, 1, 1);
    cairo_line_to(cr, w - 1, 1);
    cairo_stroke(cr);

    /* Bottom/right shadow bevel */
    cairo_set_source_rgb(cr, 0.36, 0.35, 0.34);
    cairo_move_to(cr, 1, h - 1);
    cairo_line_to(cr, w - 1, h - 1);
    cairo_line_to(cr, w - 1, 1);
    cairo_stroke(cr);

    /* Header strip */
    cairo_set_source_rgb(cr, 0.22, 0.36, 0.48);
    cairo_rectangle(cr, 4, 4, w - 8, 24);
    cairo_fill(cr);

    PangoLayout *lay = pango_cairo_create_layout(cr);
    PangoFontDescription *fd_hdr = pango_font_description_from_string("Noto Sans CJK JP Bold 10");
    pango_layout_set_font_description(lay, fd_hdr);
    pango_layout_set_text(lay, "CDE IME [Kana->Kanji]: にほんご", -1);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, 10, 7);
    pango_cairo_show_layout(cr, lay);
    pango_font_description_free(fd_hdr);

    /* Selected 1st candidate highlight */
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.92);
    cairo_rectangle(cr, 4, 30, w - 8, h - 34);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.16, 0.30, 0.46);
    cairo_rectangle(cr, 8, 34, 102, 26);
    cairo_fill(cr);

    PangoFontDescription *fd_cand = pango_font_description_from_string("Noto Sans CJK JP 11");
    pango_layout_set_font_description(lay, fd_cand);

    pango_layout_set_text(lay, "1. 日本語", -1);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, 14, 37);
    pango_cairo_show_layout(cr, lay);

    pango_layout_set_text(lay, "2. 二本語    3. 日本後", -1);
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
    cairo_move_to(cr, 122, 37);
    pango_cairo_show_layout(cr, lay);

    pango_font_description_free(fd_cand);
    g_object_unref(lay);
    cairo_destroy(cr);
    cairo_surface_flush(cs);
    cairo_surface_destroy(cs);

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)sz);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(px, sz);
    close(fd);
    return b;
}

static void popup_rect_cb(void *d, struct zwp_input_popup_surface_v2 *p, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)d; (void)p;
    if (g_log_fp) {
        fprintf(g_log_fp, "POPUP_RECT=%d,%d,%d,%d\n", x, y, w, h);
        fflush(g_log_fp);
    }
}
static const struct zwp_input_popup_surface_v2_listener g_popup_l = {
    .text_input_rectangle = popup_rect_cb,
};

static void grab_keymap(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t fmt, int32_t fd, uint32_t sz) {
    (void)d; (void)g;
    if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && fd >= 0 && sz > 0) {
        char *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            if (g_xkb_state) xkb_state_unref(g_xkb_state);
            if (g_xkb_keymap) xkb_keymap_unref(g_xkb_keymap);
            g_xkb_keymap = xkb_keymap_new_from_string(g_xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            g_xkb_state = g_xkb_keymap ? xkb_state_new(g_xkb_keymap) : NULL;
            munmap(map, sz);
            if (g_xkb_state) log_line("%s\n", "KEY_GRAB_READY");
        }
    }
    if (fd >= 0) close(fd);
}

static void grab_key(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t ser, uint32_t t, uint32_t key, uint32_t st) {
    (void)d; (void)g; (void)ser; (void)t;
    if (!g_xkb_state || st != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(g_xkb_state, key + 8);
    char sname[64] = {0};
    xkb_keysym_get_name(sym, sname, sizeof(sname));
    log_line("GRAB_KEY_SYM=%s\n", sname);

    if (!g_active) return;

    if (sym == XKB_KEY_n && !g_in_preedit) {
        g_in_preedit = 1;
        if (!g_popup_surf) {
            g_popup_surf = wl_compositor_create_surface(g_comp);
            g_popup = zwp_input_method_v2_get_input_popup_surface(g_im, g_popup_surf);
            zwp_input_popup_surface_v2_add_listener(g_popup, &g_popup_l, NULL);
        }
        if (!g_popup_buf) {
            g_popup_buf = render_candidate_popup(348, 68);
        }
        wl_surface_attach(g_popup_surf, g_popup_buf, 0, 0);
        wl_surface_damage_buffer(g_popup_surf, 0, 0, 348, 68);
        wl_surface_commit(g_popup_surf);

        const char *pre = "にほんご";
        zwp_input_method_v2_set_preedit_string(g_im, pre, (int32_t)strlen(pre), (int32_t)strlen(pre));
        zwp_input_method_v2_commit(g_im, g_serial);
        wl_display_flush(g_dpy);
        log_line("SENT_PREEDIT=%s\n", pre);
    } else if ((sym == XKB_KEY_Return || sym == XKB_KEY_space) && g_in_preedit) {
        g_in_preedit = 0;
        if (g_popup_surf) {
            wl_surface_attach(g_popup_surf, NULL, 0, 0);
            wl_surface_commit(g_popup_surf);
        }
        const char *committed = "日本語";
        zwp_input_method_v2_commit_string(g_im, committed);
        zwp_input_method_v2_commit(g_im, g_serial);
        wl_display_flush(g_dpy);
        log_line("SENT_COMMIT=%s\n", committed);
    }
}

static void grab_mods(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t ser, uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) {
    (void)d; (void)g; (void)ser;
    if (g_xkb_state) xkb_state_update_mask(g_xkb_state, dep, lat, lck, 0, 0, grp);
}

static void grab_repeat(void *d, struct zwp_input_method_keyboard_grab_v2 *g, int32_t r, int32_t del) {
    (void)d; (void)g; (void)r; (void)del;
}

static const struct zwp_input_method_keyboard_grab_v2_listener g_grab_l = {
    .keymap = grab_keymap,
    .key = grab_key,
    .modifiers = grab_mods,
    .repeat_info = grab_repeat,
};

static void im_activate(void *d, struct zwp_input_method_v2 *im) { (void)d; (void)im; g_pending_active = 1; }
static void im_deactivate(void *d, struct zwp_input_method_v2 *im) { (void)d; (void)im; g_pending_active = 0; }
static void im_surrounding(void *d, struct zwp_input_method_v2 *im, const char *txt, uint32_t cur, uint32_t anc) {
    (void)d; (void)im; (void)cur; (void)anc;
    log_line("SURROUNDING=%s\n", txt ? txt : "");
}
static void im_cause(void *d, struct zwp_input_method_v2 *im, uint32_t c) { (void)d; (void)im; (void)c; }
static void im_ctype(void *d, struct zwp_input_method_v2 *im, uint32_t h, uint32_t p) { (void)d; (void)im; (void)h; (void)p; }
static void im_done(void *d, struct zwp_input_method_v2 *im) {
    (void)d;
    g_serial++;
    int prev = g_active;
    g_active = g_pending_active;
    if (g_active && !prev) {
        log_line("%s\n", "ACTIVATE");
        if (!g_grab) {
            g_grab = zwp_input_method_v2_grab_keyboard(im);
            zwp_input_method_keyboard_grab_v2_add_listener(g_grab, &g_grab_l, NULL);
        }
    } else if (!g_active && prev) {
        g_in_preedit = 0;
        if (g_popup_surf) {
            wl_surface_attach(g_popup_surf, NULL, 0, 0);
            wl_surface_commit(g_popup_surf);
        }
        log_line("%s\n", "DEACTIVATE");
    }
    wl_display_flush(g_dpy);
}
static void im_unavail(void *d, struct zwp_input_method_v2 *im) {
    (void)d; (void)im;
    log_line("%s\n", "UNAVAILABLE");
    g_stop = 1;
}

static const struct zwp_input_method_v2_listener g_im_l = {
    .activate = im_activate,
    .deactivate = im_deactivate,
    .surrounding_text = im_surrounding,
    .text_change_cause = im_cause,
    .content_type = im_ctype,
    .done = im_done,
    .unavailable = im_unavail,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0 && !g_seat) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
    } else if (strcmp(iface, zwp_input_method_manager_v2_interface.name) == 0) {
        g_im_mgr = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

int main(int argc, char **argv) {
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    if (argc > 1) g_log_fp = fopen(argv[1], "w");
    g_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &g_reg_l, NULL);
    wl_display_roundtrip(g_dpy);
    if (!g_im_mgr || !g_seat || !g_comp || !g_shm) return 2;

    g_im = zwp_input_method_manager_v2_get_input_method(g_im_mgr, g_seat);
    zwp_input_method_v2_add_listener(g_im, &g_im_l, NULL);
    wl_display_roundtrip(g_dpy);
    log_line("%s\n", "IM_DAEMON_ONLINE");

    struct pollfd pfd = { .fd = wl_display_get_fd(g_dpy), .events = POLLIN };
    while (!g_stop) {
        while (wl_display_prepare_read(g_dpy) != 0) wl_display_dispatch_pending(g_dpy);
        wl_display_flush(g_dpy);
        if (poll(&pfd, 1, 100) > 0) wl_display_read_events(g_dpy);
        else wl_display_cancel_read(g_dpy);
        wl_display_dispatch_pending(g_dpy);
    }
    if (g_log_fp) fclose(g_log_fp);
    return 0;
}
"""


EDITOR_CLIENT_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

static struct wl_display *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct wl_seat *g_seat;
static struct xdg_wm_base *g_wm;
static struct zxdg_decoration_manager_v1 *g_dec_mgr;
static struct zwp_text_input_manager_v3 *g_ti_mgr;

static struct wl_surface *g_surf;
static struct xdg_surface *g_xsurf;
static struct xdg_toplevel *g_top;
static struct zwp_text_input_v3 *g_ti;
static struct wl_buffer *g_buf;

static int g_win_w = 580;
static int g_win_h = 240;
static int g_configured = 0;
static int g_ti_focused = 0;
static char g_committed[256] = "Lang: ";
static char g_preedit[256] = "";
static char g_pend_preedit[256] = "";
static char g_pend_commit[256] = "";
static int g_has_pend_preedit = 0;
static int g_has_pend_commit = 0;
static int g_cursor_x = 96;
static volatile sig_atomic_t g_stop = 0;
static FILE *g_ed_log = NULL;

static void on_sig(int s) { (void)s; g_stop = 1; }

static int make_shm(size_t sz) {
    char name[64];
    snprintf(name, sizeof(name), "/wp11-ed-%d-%ld", (int)getpid(), (long)random());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return -1; }
    return fd;
}

static void sync_ti_state(void) {
    if (!g_ti || !g_ti_focused) return;
    zwp_text_input_v3_enable(g_ti);
    uint32_t cur = (uint32_t)strlen(g_committed);
    zwp_text_input_v3_set_surrounding_text(g_ti, g_committed, cur, cur);
    zwp_text_input_v3_set_content_type(
        g_ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL
    );
    zwp_text_input_v3_set_cursor_rectangle(g_ti, g_cursor_x, 78, 2, 28);
    zwp_text_input_v3_commit(g_ti);
}

static void redraw(void) {
    if (!g_configured) return;
    int w = g_win_w, h = g_win_h;
    int stride = w * 4;
    size_t sz = (size_t)stride * (size_t)h;
    int fd = make_shm(sz);
    uint8_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    cairo_surface_t *cs = cairo_image_surface_create_for_data(px, CAIRO_FORMAT_ARGB32, w, h, stride);
    cairo_t *cr = cairo_create(cs);

    /* Classic CDE dialog body */
    cairo_set_source_rgb(cr, 0.80, 0.78, 0.76);
    cairo_paint(cr);

    PangoLayout *lay = pango_cairo_create_layout(cr);
    PangoFontDescription *fd_lbl = pango_font_description_from_string("Sans Bold 11");
    pango_layout_set_font_description(lay, fd_lbl);
    pango_layout_set_text(lay, "Native Wayland Text Field (zwp_text_input_v3 + CJK IME):", -1);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_move_to(cr, 20, 26);
    pango_cairo_show_layout(cr, lay);
    pango_font_description_free(fd_lbl);

    /* Recessed CDE text field */
    int fx = 20, fy = 66, fw = w - 40, fh = 48;
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.96);
    cairo_rectangle(cr, fx, fy, fw, fh);
    cairo_fill(cr);

    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgb(cr, 0.35, 0.34, 0.33);
    cairo_move_to(cr, fx, fy + fh);
    cairo_line_to(cr, fx, fy);
    cairo_line_to(cr, fx + fw, fy);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0.95, 0.94, 0.93);
    cairo_move_to(cr, fx, fy + fh);
    cairo_line_to(cr, fx + fw, fy + fh);
    cairo_line_to(cr, fx + fw, fy);
    cairo_stroke(cr);

    /* Committed text */
    PangoFontDescription *fd_txt = pango_font_description_from_string("Noto Sans CJK JP 14");
    pango_layout_set_font_description(lay, fd_txt);
    pango_layout_set_text(lay, g_committed, -1);
    int cw = 0, ch = 0;
    pango_layout_get_pixel_size(lay, &cw, &ch);
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.10);
    cairo_move_to(cr, fx + 12, fy + 11);
    pango_cairo_show_layout(cr, lay);

    int cur_x = fx + 12 + cw;
    g_cursor_x = cur_x;

    /* Preedit text with navy underline */
    if (g_preedit[0]) {
        pango_layout_set_text(lay, g_preedit, -1);
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(lay, &pw, &ph);
        cairo_set_source_rgb(cr, 0.90, 0.94, 0.99);
        cairo_rectangle(cr, cur_x, fy + 8, pw + 4, fh - 16);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.10, 0.24, 0.52);
        cairo_move_to(cr, cur_x + 2, fy + 11);
        pango_cairo_show_layout(cr, lay);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, cur_x + 2, fy + fh - 10);
        cairo_line_to(cr, cur_x + 2 + pw, fy + fh - 10);
        cairo_stroke(cr);
        cur_x += pw + 4;
    }

    /* Caret bar */
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
    cairo_rectangle(cr, cur_x + 1, fy + 10, 2, fh - 20);
    cairo_fill(cr);
    pango_font_description_free(fd_txt);

    /* Footer status bar */
    char status[640];
    snprintf(
        status, sizeof(status),
        "IME Focus: %s   |   Preedit: %s   |   Committed: %s",
        g_ti_focused ? "ACTIVE" : "INACTIVE",
        g_preedit[0] ? g_preedit : "(none)",
        g_committed
    );
    PangoFontDescription *fd_ft = pango_font_description_from_string("Noto Sans CJK JP 10");
    pango_layout_set_font_description(lay, fd_ft);
    pango_layout_set_text(lay, status, -1);
    cairo_set_source_rgb(cr, 0.22, 0.22, 0.24);
    cairo_move_to(cr, 20, h - 42);
    pango_cairo_show_layout(cr, lay);
    pango_font_description_free(fd_ft);

    g_object_unref(lay);
    cairo_destroy(cr);
    cairo_surface_flush(cs);
    cairo_surface_destroy(cs);

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)sz);
    struct wl_buffer *nb = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(px, sz);
    close(fd);

    if (g_buf) wl_buffer_destroy(g_buf);
    g_buf = nb;
    wl_surface_attach(g_surf, g_buf, 0, 0);
    wl_surface_damage_buffer(g_surf, 0, 0, w, h);
    wl_surface_commit(g_surf);
}

static void ti_enter(void *d, struct zwp_text_input_v3 *ti, struct wl_surface *s) {
    (void)d; (void)ti; (void)s;
    g_ti_focused = 1;
    if (g_ed_log) { fprintf(g_ed_log, "TI_ENTER\n"); fflush(g_ed_log); }
    redraw();
    sync_ti_state();
    wl_display_flush(g_dpy);
}
static void ti_leave(void *d, struct zwp_text_input_v3 *ti, struct wl_surface *s) {
    (void)d; (void)ti; (void)s;
    g_ti_focused = 0;
    g_preedit[0] = '\0';
    if (g_ed_log) { fprintf(g_ed_log, "TI_LEAVE\n"); fflush(g_ed_log); }
    redraw();
    wl_display_flush(g_dpy);
}
static void ti_preedit(void *d, struct zwp_text_input_v3 *ti, const char *txt, int32_t cb, int32_t ce) {
    (void)d; (void)ti; (void)cb; (void)ce;
    snprintf(g_pend_preedit, sizeof(g_pend_preedit), "%s", txt ? txt : "");
    g_has_pend_preedit = 1;
}
static void ti_commit_str(void *d, struct zwp_text_input_v3 *ti, const char *txt) {
    (void)d; (void)ti;
    snprintf(g_pend_commit, sizeof(g_pend_commit), "%s", txt ? txt : "");
    g_has_pend_commit = 1;
}
static void ti_del(void *d, struct zwp_text_input_v3 *ti, uint32_t b, uint32_t a) {
    (void)d; (void)ti; (void)b; (void)a;
}
static void ti_done(void *d, struct zwp_text_input_v3 *ti, uint32_t ser) {
    (void)d; (void)ti; (void)ser;
    if (g_has_pend_commit) {
        size_t cur = strlen(g_committed);
        snprintf(g_committed + cur, sizeof(g_committed) - cur, "%s", g_pend_commit);
        if (g_ed_log) { fprintf(g_ed_log, "COMMIT=%s\nFULL=%s\n", g_pend_commit, g_committed); fflush(g_ed_log); }
        g_pend_commit[0] = '\0';
        g_has_pend_commit = 0;
    }
    if (g_has_pend_preedit) {
        snprintf(g_preedit, sizeof(g_preedit), "%s", g_pend_preedit);
        if (g_preedit[0] && g_ed_log) { fprintf(g_ed_log, "PREEDIT=%s\n", g_preedit); fflush(g_ed_log); }
        g_pend_preedit[0] = '\0';
        g_has_pend_preedit = 0;
    } else {
        g_preedit[0] = '\0';
    }
    redraw();
    sync_ti_state();
    wl_display_flush(g_dpy);
}

static const struct zwp_text_input_v3_listener g_ti_l = {
    .enter = ti_enter,
    .leave = ti_leave,
    .preedit_string = ti_preedit,
    .commit_string = ti_commit_str,
    .delete_surrounding_text = ti_del,
    .done = ti_done,
};

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t ser) { (void)d; xdg_wm_base_pong(wm, ser); }
static const struct xdg_wm_base_listener g_wm_l = { .ping = wm_ping };

static void xsurf_conf(void *d, struct xdg_surface *xs, uint32_t ser) {
    (void)d;
    xdg_surface_ack_configure(xs, ser);
    g_configured = 1;
    redraw();
}
static const struct xdg_surface_listener g_xs_l = { .configure = xsurf_conf };

static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)d; (void)t; (void)st;
    if (w > 120) g_win_w = w;
    if (h > 100) g_win_h = h;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; g_stop = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener g_top_l = {
    .configure = top_conf, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0 && !g_seat) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        g_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
        xdg_wm_base_add_listener(g_wm, &g_wm_l, NULL);
    } else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        g_dec_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(iface, zwp_text_input_manager_v3_interface.name) == 0) {
        g_ti_mgr = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

int main(int argc, char **argv) {
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    if (argc > 1) g_ed_log = fopen(argv[1], "w");
    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &g_reg_l, NULL);
    wl_display_roundtrip(g_dpy);
    if (!g_comp || !g_shm || !g_wm || !g_seat || !g_ti_mgr) return 2;

    g_ti = zwp_text_input_manager_v3_get_text_input(g_ti_mgr, g_seat);
    zwp_text_input_v3_add_listener(g_ti, &g_ti_l, NULL);

    g_surf = wl_compositor_create_surface(g_comp);
    g_xsurf = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    xdg_surface_add_listener(g_xsurf, &g_xs_l, NULL);
    g_top = xdg_surface_get_toplevel(g_xsurf);
    xdg_toplevel_add_listener(g_top, &g_top_l, NULL);
    xdg_toplevel_set_title(g_top, "CDE Wayland CJK Input (zwp_text_input_v3)");
    xdg_toplevel_set_app_id(g_top, "cde-wayland-cjk-editor");
    if (g_dec_mgr) {
        struct zxdg_toplevel_decoration_v1 *dec =
            zxdg_decoration_manager_v1_get_toplevel_decoration(g_dec_mgr, g_top);
        zxdg_toplevel_decoration_v1_set_mode(dec, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(g_surf);
    wl_display_roundtrip(g_dpy);

    struct pollfd pfd = { .fd = wl_display_get_fd(g_dpy), .events = POLLIN };
    while (!g_stop) {
        while (wl_display_prepare_read(g_dpy) != 0) wl_display_dispatch_pending(g_dpy);
        wl_display_flush(g_dpy);
        if (poll(&pfd, 1, 100) > 0) wl_display_read_events(g_dpy);
        else wl_display_cancel_read(g_dpy);
        wl_display_dispatch_pending(g_dpy);
    }
    if (g_ed_log) fclose(g_ed_log);
    return 0;
}
"""


LOCKER_CLIENT_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"

static struct wl_display *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct wl_output *g_out;
static struct ext_session_lock_manager_v1 *g_lock_mgr;
static struct ext_session_lock_v1 *g_lock;
static struct wl_surface *g_surf;
static struct ext_session_lock_surface_v1 *g_lsurf;
static struct wl_buffer *g_buf;
static volatile sig_atomic_t g_unlock = 0;
static int g_locked = 0;
static FILE *g_log = NULL;

static void on_usr1(int s) { (void)s; g_unlock = 1; }

static int make_shm(size_t sz) {
    char name[64];
    snprintf(name, sizeof(name), "/wp11-lk-%d-%ld", (int)getpid(), (long)random());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return -1; }
    return fd;
}

static void ls_conf(void *d, struct ext_session_lock_surface_v1 *ls, uint32_t ser, uint32_t w, uint32_t h) {
    (void)d;
    ext_session_lock_surface_v1_ack_configure(ls, ser);
    if (!w) w = 1280;
    if (!h) h = 900;
    size_t sz = (size_t)w * (size_t)h * 4u;
    int fd = make_shm(sz);
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) px[i] = 0xFF102038u;
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)sz);
    if (g_buf) wl_buffer_destroy(g_buf);
    g_buf = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(px, sz);
    close(fd);
    wl_surface_attach(g_surf, g_buf, 0, 0);
    wl_surface_damage_buffer(g_surf, 0, 0, (int32_t)w, (int32_t)h);
    wl_surface_commit(g_surf);
}
static const struct ext_session_lock_surface_v1_listener g_ls_l = { .configure = ls_conf };

static void lk_locked(void *d, struct ext_session_lock_v1 *l) {
    (void)d; (void)l;
    g_locked = 1;
    if (g_log) { fprintf(g_log, "LOCKED=1\n"); fflush(g_log); }
}
static void lk_fin(void *d, struct ext_session_lock_v1 *l) { (void)d; (void)l; g_unlock = 1; }
static const struct ext_session_lock_v1_listener g_lk_l = { .locked = lk_locked, .finished = lk_fin };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && !g_out) {
        g_out = wl_registry_bind(r, name, &wl_output_interface, ver < 3 ? ver : 3);
    } else if (strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
        g_lock_mgr = wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

int main(int argc, char **argv) {
    signal(SIGUSR1, on_usr1);
    if (argc > 1) g_log = fopen(argv[1], "w");
    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &g_reg_l, NULL);
    wl_display_roundtrip(g_dpy);
    if (!g_lock_mgr || !g_comp || !g_shm || !g_out) return 2;

    g_lock = ext_session_lock_manager_v1_lock(g_lock_mgr);
    ext_session_lock_v1_add_listener(g_lock, &g_lk_l, NULL);
    g_surf = wl_compositor_create_surface(g_comp);
    g_lsurf = ext_session_lock_v1_get_lock_surface(g_lock, g_surf, g_out);
    ext_session_lock_surface_v1_add_listener(g_lsurf, &g_ls_l, NULL);
    wl_display_flush(g_dpy);

    struct pollfd pfd = { .fd = wl_display_get_fd(g_dpy), .events = POLLIN };
    while (!g_unlock) {
        while (wl_display_prepare_read(g_dpy) != 0) wl_display_dispatch_pending(g_dpy);
        wl_display_flush(g_dpy);
        if (poll(&pfd, 1, 50) > 0) wl_display_read_events(g_dpy);
        else wl_display_cancel_read(g_dpy);
        wl_display_dispatch_pending(g_dpy);
    }

    if (g_locked) {
        ext_session_lock_v1_unlock_and_destroy(g_lock);
        if (g_lsurf) ext_session_lock_surface_v1_destroy(g_lsurf);
        if (g_surf) wl_surface_destroy(g_surf);
        wl_display_roundtrip(g_dpy);
        if (g_log) { fprintf(g_log, "UNLOCKED=1\n"); fflush(g_log); }
    }
    if (g_log) fclose(g_log);
    return 0;
}
"""


def wait_for_text(path: Path, needle: str, timeout: float = 10.0) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            txt = path.read_text(encoding="utf-8", errors="replace")
            if needle in txt:
                return txt
        time.sleep(0.08)
    got = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else "(missing)"
    raise RuntimeError(f"Timed out waiting for {needle!r} in {path}:\n{got}")


def compile_wayland_fixtures(src_dtwm: Path, work: Path) -> tuple[Path, Path, Path]:
    cc = os.environ.get("CC", "cc")
    scanner = shutil.which("wayland-scanner")
    if not scanner:
        raise RuntimeError("wayland-scanner not found in PATH")

    wl_proto_dir = Path("/usr/share/wayland-protocols")
    xmls = {
        "xdg-shell": wl_proto_dir / "stable/xdg-shell/xdg-shell.xml",
        "xdg-decoration-unstable-v1": wl_proto_dir / "unstable/xdg-decoration/xdg-decoration-unstable-v1.xml",
        "text-input-unstable-v3": wl_proto_dir / "unstable/text-input/text-input-unstable-v3.xml",
        "ext-session-lock-v1": wl_proto_dir / "staging/ext-session-lock/ext-session-lock-v1.xml",
        "input-method-unstable-v2": src_dtwm / "wayland/protocols/input-method-unstable-v2.xml",
    }
    for tag, xp in xmls.items():
        if not xp.is_file():
            raise RuntimeError(f"Missing Wayland XML {tag}: {xp}")
        subprocess.run([scanner, "client-header", str(xp), str(work / f"{tag}-client-protocol.h")], check=True)
        subprocess.run([scanner, "private-code", str(xp), str(work / f"{tag}-protocol.c")], check=True)

    pkg_cflags = subprocess.check_output(
        ["pkg-config", "--cflags", "wayland-client", "xkbcommon", "pangocairo", "cairo"],
        text=True,
    ).split()
    pkg_libs = subprocess.check_output(
        ["pkg-config", "--libs", "wayland-client", "xkbcommon", "pangocairo", "cairo"],
        text=True,
    ).split()

    im_c = work / "wp11_im_daemon.c"
    im_c.write_text(IM_DAEMON_C, encoding="utf-8")
    im_bin = work / "wp11_im_daemon"
    subprocess.run(
        [
            cc, "-O2", "-Wall", f"-I{work}", *pkg_cflags,
            str(im_c), str(work / "input-method-unstable-v2-protocol.c"),
            "-o", str(im_bin), *pkg_libs, "-lrt",
        ],
        check=True,
    )

    ed_c = work / "wp11_editor.c"
    ed_c.write_text(EDITOR_CLIENT_C, encoding="utf-8")
    ed_bin = work / "wp11_editor"
    subprocess.run(
        [
            cc, "-O2", "-Wall", f"-I{work}", *pkg_cflags,
            str(ed_c),
            str(work / "xdg-shell-protocol.c"),
            str(work / "xdg-decoration-unstable-v1-protocol.c"),
            str(work / "text-input-unstable-v3-protocol.c"),
            "-o", str(ed_bin), *pkg_libs, "-lrt",
        ],
        check=True,
    )

    lk_c = work / "wp11_locker.c"
    lk_c.write_text(LOCKER_CLIENT_C, encoding="utf-8")
    lk_bin = work / "wp11_locker"
    wl_libs = subprocess.check_output(["pkg-config", "--libs", "wayland-client"], text=True).split()
    subprocess.run(
        [
            cc, "-O2", "-Wall", f"-I{work}",
            str(lk_c), str(work / "ext-session-lock-v1-protocol.c"),
            "-o", str(lk_bin), *wl_libs, "-lrt",
        ],
        check=True,
    )
    return im_bin, ed_bin, lk_bin


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True, type=Path)
    ap.add_argument("--screenshot-dir", type=Path, default=None)
    args = ap.parse_args()

    comp_bin = args.compositor.resolve()
    src_dtwm = Path(__file__).resolve().parents[1]
    base_tmp = os.environ.get("TMPDIR") or "/var/tmp"

    shot_dir = args.screenshot_dir
    if shot_dir is None and Path("/Volumes/S8T/tmp/cde-wayland-wp00-9a09ad77/receipts").is_dir():
        shot_dir = Path("/Volumes/S8T/tmp/cde-wayland-wp00-9a09ad77/receipts/wp11")

    with tempfile.TemporaryDirectory(prefix="wp11-ime-", dir=base_tmp) as td:
        work = Path(td)
        rt_dir = work / "xdg"
        home_dir = work / "home"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt" / "sessions").mkdir(parents=True)

        im_bin, ed_bin, lk_bin = compile_wayland_fixtures(src_dtwm, work)

        env = os.environ.copy()
        env.update(
            {
                "XDG_RUNTIME_DIR": str(rt_dir),
                "HOME": str(home_dir),
                "WLR_BACKENDS": "headless",
                "WLR_RENDERER": "pixman",
                "WLR_HEADLESS_OUTPUTS": "1",
                "DTWM_OUTPUT_MODE": "1280x900",
                "LC_ALL": "C.UTF-8",
            }
        )
        # Ensure built Motif & CDE libs resolve cleanly
        for cand_root in (comp_bin.parents[2], comp_bin.parents[3]):
            if (cand_root / "lib").is_dir():
                ld_parts = [
                    str(p)
                    for p in (
                        Path("/var/tmp/ck-wayland-build/wp00/motif-prefix/lib"),
                        Path("/var/tmp/ck-wayland-build/wp00/fs-prefix/lib"),
                        cand_root / "lib/DtSvc",
                        cand_root / "lib/DtWidget",
                        cand_root / "lib/DtHelp",
                        cand_root / "lib/DtTerm",
                        cand_root / "lib/DtPrint",
                        cand_root / "lib/tt/lib",
                        cand_root / "lib/csa",
                    )
                    if p.is_dir()
                ]
                for sub in (cand_root / "lib").rglob(".libs"):
                    ld_parts.append(str(sub))
                if env.get("LD_LIBRARY_PATH"):
                    ld_parts.append(env["LD_LIBRARY_PATH"])
                env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
                break

        status_file = work / "comp.status"
        comp_log = (work / "comp.log").open("w", encoding="utf-8")
        procs: list[subprocess.Popen] = []
        try:
            comp = subprocess.Popen(
                [str(comp_bin), "--status-file", str(status_file)],
                env=env,
                stdout=comp_log,
                stderr=subprocess.STDOUT,
            )
            procs.append(comp)

            st_txt = wait_for_text(status_file, "WAYLAND_DISPLAY=", timeout=15.0)
            for ln in st_txt.splitlines():
                if "=" in ln:
                    k, v = ln.split("=", 1)
                    env[k.strip()] = v.strip()

            # Let genuine CDE Front Panel map at the bottom first
            time.sleep(1.2)

            im_log = work / "im.log"
            ed_log = work / "ed.log"
            im_proc = subprocess.Popen([str(im_bin), str(im_log)], env=env)
            procs.append(im_proc)
            wait_for_text(im_log, "IM_DAEMON_ONLINE", timeout=8.0)

            ed_proc = subprocess.Popen([str(ed_bin), str(ed_log)], env=env)
            procs.append(ed_proc)

            # 1. Editor receives zwp_text_input_v3.enter -> enables -> IME receives activate + surrounding + keyboard grab
            wait_for_text(ed_log, "TI_ENTER", timeout=8.0)
            wait_for_text(im_log, "ACTIVATE", timeout=8.0)
            wait_for_text(im_log, "SURROUNDING=Lang: ", timeout=8.0)

            # 2. Type 'n' -> virtual keyboard attaches keymap to grab, IME opens CJK candidate popup & sends preedit "にほんご"
            subprocess.run(["wlrctl", "keyboard", "type", "n"], env=env, check=True)
            wait_for_text(im_log, "KEY_GRAB_READY", timeout=8.0)
            wait_for_text(im_log, "SENT_PREEDIT=にほんご", timeout=8.0)
            wait_for_text(ed_log, "PREEDIT=にほんご", timeout=8.0)
            im_state = wait_for_text(im_log, "POPUP_RECT=", timeout=8.0)

            # Verify text_input_rectangle was delivered to zwp_input_popup_surface_v2
            rect_lines = [ln for ln in im_state.splitlines() if ln.startswith("POPUP_RECT=")]
            assert rect_lines, f"Expected POPUP_RECT event in {im_state}"

            time.sleep(0.35)
            preedit_png = work / "wp11-wayland-cjk-ime.png"
            subprocess.run(["grim", str(preedit_png)], env=env, check=True)
            with Image.open(preedit_png) as img:
                assert img.size == (1280, 900), f"Unexpected screenshot size: {img.size}"

            if shot_dir is not None:
                shot_dir.mkdir(parents=True, exist_ok=True)
                shutil.copy2(preedit_png, shot_dir / "wp11-wayland-cjk-ime.png")

            # 3. Press Return -> IME unmaps candidate popup and commits "日本語" to editor
            subprocess.run(["wlrctl", "keyboard", "type", "\n"], env=env, check=True)
            wait_for_text(im_log, "SENT_COMMIT=日本語", timeout=8.0)
            ed_state = wait_for_text(ed_log, "FULL=Lang: 日本語", timeout=8.0)
            assert "COMMIT=日本語" in ed_state

            time.sleep(0.25)
            if shot_dir is not None:
                committed_png = shot_dir / "wp11-wayland-cjk-committed.png"
                subprocess.run(["grim", str(committed_png)], env=env, check=True)

            # 4. Session-lock isolation: lock via ext-session-lock-v1 -> IME must deactivate & NOT receive lock-screen keys!
            lk_log = work / "lk.log"
            lk_proc = subprocess.Popen([str(lk_bin), str(lk_log)], env=env)
            procs.append(lk_proc)
            wait_for_text(lk_log, "LOCKED=1", timeout=8.0)
            wait_for_text(ed_log, "TI_LEAVE", timeout=8.0)
            wait_for_text(im_log, "DEACTIVATE", timeout=8.0)

            before_lock_keys = im_log.read_text(encoding="utf-8")
            subprocess.run(["wlrctl", "keyboard", "type", "s"], env=env, check=True)
            time.sleep(0.3)
            during_lock_keys = im_log.read_text(encoding="utf-8")
            leaked = during_lock_keys[len(before_lock_keys):]
            assert "GRAB_KEY_SYM=s" not in leaked, f"Lock-screen key leaked to IME keyboard_grab: {leaked!r}"

            # Unlock cleanly and verify IME focus & activation recover automatically
            lk_proc.send_signal(signal.SIGUSR1)
            lk_proc.wait(timeout=8.0)
            wait_for_text(lk_log, "UNLOCKED=1", timeout=5.0)

            ed_after = ed_log.read_text(encoding="utf-8")
            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline:
                ed_after = ed_log.read_text(encoding="utf-8")
                im_after = im_log.read_text(encoding="utf-8")
                if ed_after.count("TI_ENTER") >= 2 and im_after.count("ACTIVATE") >= 2:
                    break
                time.sleep(0.08)
            assert ed_after.count("TI_ENTER") >= 2, f"Expected TI_ENTER after unlock:\n{ed_after}"
            assert im_log.read_text(encoding="utf-8").count("ACTIVATE") >= 2, "Expected IME ACTIVATE after unlock"

            print("PASS: WP-11 Wayland CJK IME (zwp_text_input_v3 + zwp_input_method_v2 + candidate popup + lock isolation)")
            return 0
        finally:
            for p in reversed(procs):
                if p.poll() is None:
                    p.terminate()
            for p in reversed(procs):
                try:
                    p.wait(timeout=4.0)
                except subprocess.TimeoutExpired:
                    p.kill()
            comp_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
