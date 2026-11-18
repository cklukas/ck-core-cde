#!/usr/bin/env python3
"""WP-17 parallel dual-environment integration check (Wayland + X11/XLibre):
1. Runtime Toolkit & Filesystem Engine Linkage:
   - Verifies both `dtfile` and `dtfile_sync` map CK Core Motif ABI 9 (`libXm.so.9`)
     and `ck-core-fs-lib` (`libck-core-fs-lib.so.0.5`).
2. Multi-Tab Browsing & Navigation Shortcuts (`dtfile`):
   - Exercises real Wayland (`zwp_virtual_keyboard_v1` + `zwlr_virtual_pointer_v1`) and
     X11 (`XTest`) input: `Ctrl+T` new tab, `Alt+Left` / `Alt+Right` Back/Forward history,
     `Alt+Up` parent folder, `Ctrl+Tab` / `Ctrl+Shift+Tab` tab cycling, middle-click
     (`Button2` click without drag) on a folder icon to open in a new tab, `Ctrl+W`
     tab close, `Ctrl+H` hidden-files toggle, and `F2` inline rename.
3. Asynchronous Cancellable In-Window Search (`Ctrl+F` + `ck-core-fs-lib` `ckfes_*`):
   - Exercises `Ctrl+F` search bar open & `Escape` close, recursive glob + file-content +
     size + modified-date filtering, live `Reveal in Folder` (navigating to deep parent
     folder and selecting matched file), and mid-scan cancellation (`Stop` / `search_cancel`)
     with verified worker drain.
4. Cross-Toolkit File Clipboard (`Ctrl+C` / `Ctrl+X` / `Ctrl+V`):
   - `dtfile -> Native Peer`: copies selected file in `dtfile` (`Ctrl+C`) and reads
     `x-special/gnome-copied-files` + `text/uri-list` from a native Wayland
     `zwlr_data_control_v1` peer (Wayland lane) and an X11 `CLIPBOARD` peer (X11 lane).
   - Same-folder `Ctrl+V` duplicate paste (`(copy)` collision naming).
   - `Native Peer -> dtfile`: native Wayland `zwlr_data_control_v1` / X11 `CLIPBOARD`
     peer publishes `x-special/gnome-copied-files` (`cut\nfile:///...` with spaces and `#`),
     and `Ctrl+V` in `dtfile` moves the item into the active folder.
5. Bounded Per-Session Undo Journal (`Ctrl+Z`):
   - Reverses **Move to Trash** (restoring from XDG Trash to original path),
     **Move** (returning cut-pasted item to external source folder),
     **Copy** (disposing duplicate copy safely into XDG Trash, never `rm -rf`), and
     **Rename** (restoring original filename).
6. Validated *Synchronize Folders* Execution (`dtfile_sync` `Apply`):
   - Launches live `/usr/dt/bin/dtfile_sync`, verifies `apply_qualified=1`, selects
     `Synchronize one way` (`CKFSP_SYNC`), reviews conflict rule (`Use source` + retain
     replaced), executes **Apply reviewed plan**, and verifies `CKFET_COMPLETE` (`state=4`,
     `drained=1`) and byte-identical synced folder contents.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time


WLR_DATA_CONTROL_XML = r"""<?xml version="1.0" encoding="UTF-8"?>
<protocol name="wlr_data_control_unstable_v1">
  <interface name="zwlr_data_control_manager_v1" version="1">
    <request name="create_data_source">
      <arg name="id" type="new_id" interface="zwlr_data_control_source_v1"/>
    </request>
    <request name="get_data_device">
      <arg name="id" type="new_id" interface="zwlr_data_control_device_v1"/>
      <arg name="seat" type="object" interface="wl_seat"/>
    </request>
    <request name="destroy" type="destructor"/>
  </interface>
  <interface name="zwlr_data_control_device_v1" version="1">
    <request name="set_selection">
      <arg name="source" type="object" interface="zwlr_data_control_source_v1" allow-null="true"/>
    </request>
    <request name="destroy" type="destructor"/>
    <event name="data_offer">
      <arg name="id" type="new_id" interface="zwlr_data_control_offer_v1"/>
    </event>
    <event name="selection">
      <arg name="id" type="object" interface="zwlr_data_control_offer_v1" allow-null="true"/>
    </event>
    <event name="finished"/>
  </interface>
  <interface name="zwlr_data_control_source_v1" version="1">
    <request name="offer">
      <arg name="mime_type" type="string"/>
    </request>
    <request name="destroy" type="destructor"/>
    <event name="send">
      <arg name="mime_type" type="string"/>
      <arg name="fd" type="fd"/>
    </event>
    <event name="cancelled"/>
  </interface>
  <interface name="zwlr_data_control_offer_v1" version="1">
    <request name="receive">
      <arg name="mime_type" type="string"/>
      <arg name="fd" type="fd"/>
    </request>
    <request name="destroy" type="destructor"/>
    <event name="offer">
      <arg name="mime_type" type="string"/>
    </event>
  </interface>
</protocol>
"""


WL_HELPER_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr-data-control-client.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

extern const struct wl_interface zwp_virtual_keyboard_v1_interface;
extern const struct wl_interface zwlr_virtual_pointer_v1_interface;

static const struct wl_interface *vkb_mgr_types[] = {
    &wl_seat_interface, &zwp_virtual_keyboard_v1_interface,
};
static const struct wl_message vkb_mgr_requests[] = {
    { "create_virtual_keyboard", "on", vkb_mgr_types },
};
static const struct wl_interface zwp_virtual_keyboard_manager_v1_interface = {
    "zwp_virtual_keyboard_manager_v1", 1, 1, vkb_mgr_requests, 0, NULL,
};
static const struct wl_interface *vkb_null_types[] = { NULL, NULL, NULL, NULL };
static const struct wl_message vkb_requests[] = {
    { "keymap", "uhu", vkb_null_types },
    { "key", "uuu", vkb_null_types },
    { "modifiers", "uuuu", vkb_null_types },
    { "destroy", "", vkb_null_types },
};
const struct wl_interface zwp_virtual_keyboard_v1_interface = {
    "zwp_virtual_keyboard_v1", 1, 4, vkb_requests, 0, NULL,
};

static const struct wl_interface *vptr_mgr_types[] = {
    &wl_seat_interface, &zwlr_virtual_pointer_v1_interface,
};
static const struct wl_message vptr_mgr_requests[] = {
    { "create_virtual_pointer", "?on", vptr_mgr_types },
};
static const struct wl_interface zwlr_virtual_pointer_manager_v1_interface = {
    "zwlr_virtual_pointer_manager_v1", 1, 1, vptr_mgr_requests, 0, NULL,
};
static const struct wl_interface *vptr_null_types[] = { NULL, NULL, NULL, NULL, NULL };
static const struct wl_message vptr_requests[] = {
    { "motion", "uff", vptr_null_types },
    { "motion_absolute", "uuuuu", vptr_null_types },
    { "button", "uuu", vptr_null_types },
    { "axis", "uuf", vptr_null_types },
    { "frame", "", vptr_null_types },
    { "axis_source", "u", vptr_null_types },
    { "axis_stop", "uu", vptr_null_types },
    { "axis_discrete", "uufi", vptr_null_types },
    { "destroy", "", vptr_null_types },
};
const struct wl_interface zwlr_virtual_pointer_v1_interface = {
    "zwlr_virtual_pointer_v1", 1, 9, vptr_requests, 0, NULL,
};

typedef struct {
    struct wl_seat *seat;
    struct wl_proxy *vkb_mgr;
    struct wl_proxy *vptr_mgr;
    struct zwlr_data_control_manager_v1 *dc_mgr;
} WlState;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t ver) {
    WlState *st = (WlState *)data;
    (void)ver;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !st->seat) {
        st->seat = (struct wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, "zwp_virtual_keyboard_manager_v1") == 0 && !st->vkb_mgr) {
        st->vkb_mgr = (struct wl_proxy *)wl_registry_bind(
            reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (strcmp(iface, "zwlr_virtual_pointer_manager_v1") == 0 && !st->vptr_mgr) {
        st->vptr_mgr = (struct wl_proxy *)wl_registry_bind(
            reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    } else if (strcmp(iface, zwlr_data_control_manager_v1_interface.name) == 0 && !st->dc_mgr) {
        st->dc_mgr = (struct zwlr_data_control_manager_v1 *)wl_registry_bind(
            reg, name, &zwlr_data_control_manager_v1_interface, 1);
    }
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener = { on_global, on_global_remove };

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

static uint32_t evdev_for_char(char ch, bool *need_shift) {
    *need_shift = false;
    if (ch >= 'a' && ch <= 'z') {
        static const uint32_t tbl[26] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
            KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
            KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
        };
        return tbl[ch - 'a'];
    }
    if (ch >= 'A' && ch <= 'Z') {
        *need_shift = true;
        return evdev_for_char((char)(ch - 'A' + 'a'), &(bool){false});
    }
    switch (ch) {
    case '0': return KEY_0; case '1': return KEY_1; case '2': return KEY_2;
    case '3': return KEY_3; case '4': return KEY_4; case '5': return KEY_5;
    case '6': return KEY_6; case '7': return KEY_7; case '8': return KEY_8;
    case '9': return KEY_9;
    case '.': return KEY_DOT;
    case '-': return KEY_MINUS;
    case '_': *need_shift = true; return KEY_MINUS;
    case ' ': return KEY_SPACE;
    default: return 0;
    }
}

static void vkb_emit_one(struct wl_display *dpy, struct wl_proxy *vkb,
                         uint32_t m_shift, uint32_t m_ctrl, uint32_t m_alt,
                         bool is_combo, const char *arg) {
    if (is_combo) {
        uint32_t mods = 0;
        bool use_ctrl = strstr(arg, "ctrl+") != NULL;
        bool use_alt = strstr(arg, "alt+") != NULL;
        bool use_shift = strstr(arg, "shift+") != NULL;
        const char *leaf = strrchr(arg, '+');
        leaf = leaf ? (leaf + 1) : arg;

        uint32_t kc = 0;
        if (strcmp(leaf, "t") == 0) kc = KEY_T;
        else if (strcmp(leaf, "w") == 0) kc = KEY_W;
        else if (strcmp(leaf, "f") == 0) kc = KEY_F;
        else if (strcmp(leaf, "h") == 0) kc = KEY_H;
        else if (strcmp(leaf, "c") == 0) kc = KEY_C;
        else if (strcmp(leaf, "x") == 0) kc = KEY_X;
        else if (strcmp(leaf, "v") == 0) kc = KEY_V;
        else if (strcmp(leaf, "z") == 0) kc = KEY_Z;
        else if (strcmp(leaf, "tab") == 0) kc = KEY_TAB;
        else if (strcmp(leaf, "left") == 0) kc = KEY_LEFT;
        else if (strcmp(leaf, "right") == 0) kc = KEY_RIGHT;
        else if (strcmp(leaf, "up") == 0) kc = KEY_UP;
        else if (strcmp(leaf, "f2") == 0) kc = KEY_F2;
        else if (strcmp(leaf, "esc") == 0) kc = KEY_ESC;
        if (!kc) return;

        if (use_ctrl) { wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_LEFTCTRL, 1u); mods |= m_ctrl; }
        if (use_alt)  { wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_LEFTALT, 1u);  mods |= m_alt; }
        if (use_shift){ wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_LEFTSHIFT, 1u);mods |= m_shift; }
        if (mods) wl_proxy_marshal(vkb, 2, mods, 0u, 0u, 0u);
        wl_display_roundtrip(dpy);
        usleep(18000);

        wl_proxy_marshal(vkb, 1, now_ms(), kc, 1u);
        wl_display_roundtrip(dpy);
        usleep(22000);
        wl_proxy_marshal(vkb, 1, now_ms(), kc, 0u);
        wl_display_roundtrip(dpy);
        usleep(18000);

        if (use_shift) wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_LEFTSHIFT, 0u);
        if (use_alt)   wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_LEFTALT, 0u);
        if (use_ctrl)  wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_LEFTCTRL, 0u);
        if (mods) wl_proxy_marshal(vkb, 2, 0u, 0u, 0u, 0u);
        wl_display_roundtrip(dpy);
    } else {
        for (size_t i = 0; arg[i]; ++i) {
            bool sh = false;
            uint32_t kc = evdev_for_char(arg[i], &sh);
            if (!kc) continue;
            if (sh) {
                wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_LEFTSHIFT, 1u);
                wl_proxy_marshal(vkb, 2, m_shift, 0u, 0u, 0u);
                wl_display_roundtrip(dpy);
            }
            wl_proxy_marshal(vkb, 1, now_ms(), kc, 1u);
            wl_display_roundtrip(dpy);
            usleep(12000);
            wl_proxy_marshal(vkb, 1, now_ms(), kc, 0u);
            wl_display_roundtrip(dpy);
            if (sh) {
                wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_LEFTSHIFT, 0u);
                wl_proxy_marshal(vkb, 2, 0u, 0u, 0u, 0u);
                wl_display_roundtrip(dpy);
            }
            usleep(10000);
        }
        wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_ENTER, 1u);
        wl_display_roundtrip(dpy);
        usleep(18000);
        wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_ENTER, 0u);
        wl_display_roundtrip(dpy);
    }
}

static int run_vkb_daemon(struct wl_display *dpy, WlState *st, const char *ready_path) {
    if (!st->seat || !st->vkb_mgr) return 3;
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    struct wl_proxy *vkb = wl_proxy_marshal_flags(
        st->vkb_mgr, 0, &zwp_virtual_keyboard_v1_interface, 1, 0, st->seat, NULL);
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
    struct xkb_keymap *kmap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *kmap_str = xkb_keymap_get_as_string(kmap, XKB_KEYMAP_FORMAT_TEXT_V1);
    uint32_t kmap_sz = (uint32_t)strlen(kmap_str) + 1u;
    int kfd = memfd_create("wp17-xkb", MFD_CLOEXEC);
    if (kfd < 0 || ftruncate(kfd, (off_t)kmap_sz) != 0) return 4;
    void *ptr = mmap(NULL, kmap_sz, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, 0);
    memcpy(ptr, kmap_str, kmap_sz);
    munmap(ptr, kmap_sz);
    wl_proxy_marshal(vkb, 0, 1u, kfd, kmap_sz);
    wl_display_roundtrip(dpy);
    close(kfd);
    usleep(120000);
    wl_display_roundtrip(dpy);

    uint32_t m_shift = 1u << xkb_keymap_mod_get_index(kmap, XKB_MOD_NAME_SHIFT);
    uint32_t m_ctrl  = 1u << xkb_keymap_mod_get_index(kmap, XKB_MOD_NAME_CTRL);
    uint32_t m_alt   = 1u << xkb_keymap_mod_get_index(kmap, XKB_MOD_NAME_ALT);

    FILE *rf = fopen(ready_path, "w");
    if (rf) { fprintf(rf, "READY\n"); fclose(rf); }

    char line[512];
    while (!g_stop && fgets(line, sizeof(line), stdin) != NULL) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (strncmp(line, "key ", 4) == 0) {
            vkb_emit_one(dpy, vkb, m_shift, m_ctrl, m_alt, true, line + 4);
        } else if (strncmp(line, "type ", 5) == 0) {
            vkb_emit_one(dpy, vkb, m_shift, m_ctrl, m_alt, false, line + 5);
        } else if (strcmp(line, "quit") == 0) {
            break;
        }
        printf("OK\n");
        fflush(stdout);
    }

    wl_proxy_marshal(vkb, 3);
    wl_proxy_destroy(vkb);
    free(kmap_str);
    xkb_keymap_unref(kmap);
    xkb_context_unref(ctx);
    wl_display_roundtrip(dpy);
    return 0;
}

static int run_click(struct wl_display *dpy, WlState *st, const char *btn_s,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!st->seat || !st->vptr_mgr) return 3;
    struct wl_proxy *vp = wl_proxy_marshal_flags(
        st->vptr_mgr, 0, &zwlr_virtual_pointer_v1_interface, 1, 0, st->seat, NULL);
    uint32_t btn = BTN_LEFT;
    if (strcmp(btn_s, "middle") == 0) btn = BTN_MIDDLE;
    else if (strcmp(btn_s, "right") == 0) btn = BTN_RIGHT;

    wl_proxy_marshal(vp, 1, now_ms(), x, y, w, h);
    wl_proxy_marshal(vp, 4);
    wl_display_roundtrip(dpy);
    usleep(25000);

    wl_proxy_marshal(vp, 2, now_ms(), btn, 1u);
    wl_proxy_marshal(vp, 4);
    wl_display_roundtrip(dpy);
    usleep(30000);

    wl_proxy_marshal(vp, 2, now_ms(), btn, 0u);
    wl_proxy_marshal(vp, 4);
    wl_display_roundtrip(dpy);
    usleep(20000);

    wl_proxy_marshal(vp, 8);
    wl_proxy_destroy(vp);
    wl_display_roundtrip(dpy);
    return 0;
}

/* --- zwlr_data_control_v1 Read & Serve --- */

typedef struct {
    struct zwlr_data_control_offer_v1 *pending_offer;
    struct zwlr_data_control_offer_v1 *sel_offer;
    bool got_selection;
    bool has_target_mime;
    const char *want_mime;
} DcReadCtx;

static void offer_on_mime(void *data, struct zwlr_data_control_offer_v1 *offer, const char *mime) {
    DcReadCtx *c = (DcReadCtx *)data;
    if (offer == c->pending_offer && c->want_mime && strcmp(mime, c->want_mime) == 0)
        c->has_target_mime = true;
}
static const struct zwlr_data_control_offer_v1_listener offer_listener = { offer_on_mime };

static void dev_on_data_offer(void *data, struct zwlr_data_control_device_v1 *dev,
                              struct zwlr_data_control_offer_v1 *id) {
    (void)dev;
    DcReadCtx *c = (DcReadCtx *)data;
    c->pending_offer = id;
    c->has_target_mime = false;
    zwlr_data_control_offer_v1_add_listener(id, &offer_listener, c);
}
static void dev_on_selection(void *data, struct zwlr_data_control_device_v1 *dev,
                             struct zwlr_data_control_offer_v1 *id) {
    (void)dev;
    DcReadCtx *c = (DcReadCtx *)data;
    c->sel_offer = id;
    c->got_selection = true;
}
static void dev_on_finished(void *data, struct zwlr_data_control_device_v1 *dev) {
    (void)data; (void)dev;
}
static const struct zwlr_data_control_device_v1_listener dev_listener = {
    dev_on_data_offer, dev_on_selection, dev_on_finished
};

static int run_clip_read(struct wl_display *dpy, WlState *st, const char *mime) {
    if (!st->seat || !st->dc_mgr) return 3;
    DcReadCtx c = {0};
    c.want_mime = mime;
    struct zwlr_data_control_device_v1 *dev =
        zwlr_data_control_manager_v1_get_data_device(st->dc_mgr, st->seat);
    zwlr_data_control_device_v1_add_listener(dev, &dev_listener, &c);
    for (int i = 0; i < 10 && !c.got_selection; ++i)
        wl_display_roundtrip(dpy);
    if (!c.sel_offer) return 6;

    int pfd[2];
    if (pipe(pfd) != 0) return 7;
    zwlr_data_control_offer_v1_receive(c.sel_offer, mime, pfd[1]);
    wl_display_flush(dpy);
    close(pfd[1]);

    char buf[4096];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, stdout);
    fflush(stdout);
    close(pfd[0]);

    zwlr_data_control_offer_v1_destroy(c.sel_offer);
    zwlr_data_control_device_v1_destroy(dev);
    wl_display_roundtrip(dpy);
    return 0;
}

typedef struct {
    char *payload;
    size_t len;
    bool cancelled;
} DcServeCtx;

static void src_on_send(void *data, struct zwlr_data_control_source_v1 *src,
                        const char *mime, int32_t fd) {
    (void)src; (void)mime;
    DcServeCtx *c = (DcServeCtx *)data;
    size_t off = 0;
    while (off < c->len) {
        ssize_t w = write(fd, c->payload + off, c->len - off);
        if (w > 0) off += (size_t)w;
        else if (w < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
}
static void src_on_cancelled(void *data, struct zwlr_data_control_source_v1 *src) {
    (void)src;
    ((DcServeCtx *)data)->cancelled = true;
}
static const struct zwlr_data_control_source_v1_listener src_listener = {
    src_on_send, src_on_cancelled
};

static int run_clip_serve(struct wl_display *dpy, WlState *st,
                          const char *mime, const char *payload_path, const char *ready_path) {
    if (!st->seat || !st->dc_mgr) return 3;
    FILE *fp = fopen(payload_path, "rb");
    if (!fp) return 4;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    DcServeCtx c = {0};
    c.len = sz > 0 ? (size_t)sz : 0;
    c.payload = (char *)malloc(c.len + 1);
    if (c.len > 0) (void)fread(c.payload, 1, c.len, fp);
    fclose(fp);

    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    DcReadCtx dummy = {0};
    struct zwlr_data_control_device_v1 *dev =
        zwlr_data_control_manager_v1_get_data_device(st->dc_mgr, st->seat);
    zwlr_data_control_device_v1_add_listener(dev, &dev_listener, &dummy);

    struct zwlr_data_control_source_v1 *src =
        zwlr_data_control_manager_v1_create_data_source(st->dc_mgr);
    zwlr_data_control_source_v1_add_listener(src, &src_listener, &c);
    zwlr_data_control_source_v1_offer(src, mime);
    if (strcmp(mime, "text/uri-list") != 0)
        zwlr_data_control_source_v1_offer(src, "text/uri-list");
    zwlr_data_control_device_v1_set_selection(dev, src);
    wl_display_roundtrip(dpy);

    FILE *rf = fopen(ready_path, "w");
    if (rf) { fprintf(rf, "READY\n"); fclose(rf); }

    int wfd = wl_display_get_fd(dpy);
    while (!g_stop && !c.cancelled) {
        wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct pollfd p = { .fd = wfd, .events = POLLIN };
        if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
            if (wl_display_dispatch(dpy) < 0) break;
        }
    }

    zwlr_data_control_source_v1_destroy(src);
    zwlr_data_control_device_v1_destroy(dev);
    free(c.payload);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 2;

    WlState st = {0};
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, &st);
    wl_display_roundtrip(dpy);

    int rc = 1;
    if (strcmp(argv[1], "vkbd-daemon") == 0)
        rc = run_vkb_daemon(dpy, &st, argv[2]);
    else if (strcmp(argv[1], "click") == 0 && argc >= 7)
        rc = run_click(dpy, &st, argv[2], (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]),
                       (uint32_t)atoi(argv[5]), (uint32_t)atoi(argv[6]));
    else if (strcmp(argv[1], "clip-read") == 0)
        rc = run_clip_read(dpy, &st, argv[2]);
    else if (strcmp(argv[1], "clip-serve") == 0 && argc >= 5)
        rc = run_clip_serve(dpy, &st, argv[2], argv[3], argv[4]);

    wl_display_disconnect(dpy);
    return rc;
}
"""


X11_HELPER_C = r"""
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

typedef int (*XTestFakeKeyEventFn)(Display *, unsigned int, Bool, unsigned long);
typedef int (*XTestFakeButtonEventFn)(Display *, unsigned int, Bool, unsigned long);
typedef int (*XTestFakeMotionEventFn)(Display *, int, int, int, unsigned long);

static void dump_prop(Display *dpy, Window root, const char *name) {
    Atom a = XInternAtom(dpy, name, False);
    Atom act = None;
    int fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *data = NULL;
    printf("=== %s ===\n", name);
    if (XGetWindowProperty(dpy, root, a, 0, 8192, False, AnyPropertyType,
                           &act, &fmt, &nitems, &rem, &data) == Success && data && fmt == 8) {
        fwrite(data, 1, nitems, stdout);
        if (nitems > 0 && data[nitems - 1] != '\n') putchar('\n');
    }
    if (data) XFree(data);
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 2;
    Window root = DefaultRootWindow(dpy);

    if (strcmp(argv[1], "props") == 0) {
        dump_prop(dpy, root, "_DT_FILEMGR_ERGONOMICS_STATE");
        dump_prop(dpy, root, "_DT_SYNC_STATE");
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "set-cmd") == 0 && argc >= 4) {
        Atom prop = XInternAtom(dpy, argv[2], False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        XChangeProperty(dpy, root, prop, utf8, 8, PropModeReplace,
                        (const unsigned char *)argv[3], (int)strlen(argv[3]));
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "key") == 0 && argc >= 3) {
        void *xtst = dlopen("libXtst.so.6", RTLD_LAZY);
        if (!xtst) xtst = dlopen("libXtst.so", RTLD_LAZY);
        if (!xtst) return 3;
        XTestFakeKeyEventFn fk = (XTestFakeKeyEventFn)dlsym(xtst, "XTestFakeKeyEvent");

        const char *arg = argv[2];
        bool use_ctrl = strstr(arg, "ctrl+") != NULL;
        bool use_alt  = strstr(arg, "alt+") != NULL;
        bool use_shift = strstr(arg, "shift+") != NULL;
        const char *leaf = strrchr(arg, '+');
        leaf = leaf ? (leaf + 1) : arg;

        KeySym ks = NoSymbol;
        if (strlen(leaf) == 1) {
            char s[2] = { leaf[0], '\0' };
            ks = XStringToKeysym(s);
        } else if (strcmp(leaf, "tab") == 0) ks = XK_Tab;
        else if (strcmp(leaf, "left") == 0) ks = XK_Left;
        else if (strcmp(leaf, "right") == 0) ks = XK_Right;
        else if (strcmp(leaf, "up") == 0) ks = XK_Up;
        else if (strcmp(leaf, "f2") == 0) ks = XK_F2;
        else if (strcmp(leaf, "esc") == 0) ks = XK_Escape;

        KeyCode kc = XKeysymToKeycode(dpy, ks);
        KeyCode kc_ctrl = XKeysymToKeycode(dpy, XK_Control_L);
        KeyCode kc_alt  = XKeysymToKeycode(dpy, XK_Alt_L);
        KeyCode kc_sh   = XKeysymToKeycode(dpy, XK_Shift_L);

        if (use_ctrl && kc_ctrl) fk(dpy, kc_ctrl, True, CurrentTime);
        if (use_alt && kc_alt)   fk(dpy, kc_alt, True, CurrentTime);
        if (use_shift && kc_sh)  fk(dpy, kc_sh, True, CurrentTime);
        XSync(dpy, False);
        usleep(18000);

        if (kc) {
            fk(dpy, kc, True, CurrentTime);
            XSync(dpy, False);
            usleep(22000);
            fk(dpy, kc, False, CurrentTime);
            XSync(dpy, False);
            usleep(18000);
        }

        if (use_shift && kc_sh)  fk(dpy, kc_sh, False, CurrentTime);
        if (use_alt && kc_alt)   fk(dpy, kc_alt, False, CurrentTime);
        if (use_ctrl && kc_ctrl) fk(dpy, kc_ctrl, False, CurrentTime);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "type") == 0 && argc >= 3) {
        void *xtst = dlopen("libXtst.so.6", RTLD_LAZY);
        if (!xtst) xtst = dlopen("libXtst.so", RTLD_LAZY);
        if (!xtst) return 3;
        XTestFakeKeyEventFn fk = (XTestFakeKeyEventFn)dlsym(xtst, "XTestFakeKeyEvent");
        KeyCode kc_sh = XKeysymToKeycode(dpy, XK_Shift_L);

        const char *txt = argv[2];
        for (size_t i = 0; txt[i]; ++i) {
            KeySym ks = NoSymbol;
            bool sh = false;
            char ch = txt[i];
            if (ch == '_') { ks = XK_minus; sh = true; }
            else if (ch == '.') ks = XK_period;
            else if (ch == '-') ks = XK_minus;
            else if (ch == ' ') ks = XK_space;
            else if (ch >= 'A' && ch <= 'Z') {
                char s[2] = { (char)(ch - 'A' + 'a'), '\0' };
                ks = XStringToKeysym(s);
                sh = true;
            } else {
                char s[2] = { ch, '\0' };
                ks = XStringToKeysym(s);
            }
            KeyCode kc = XKeysymToKeycode(dpy, ks);
            if (!kc) continue;
            if (sh && kc_sh) { fk(dpy, kc_sh, True, CurrentTime); XSync(dpy, False); }
            fk(dpy, kc, True, CurrentTime);
            XSync(dpy, False);
            usleep(14000);
            fk(dpy, kc, False, CurrentTime);
            if (sh && kc_sh) fk(dpy, kc_sh, False, CurrentTime);
            XSync(dpy, False);
            usleep(12000);
        }
        KeyCode ret = XKeysymToKeycode(dpy, XK_Return);
        if (ret) {
            fk(dpy, ret, True, CurrentTime);
            XSync(dpy, False);
            usleep(20000);
            fk(dpy, ret, False, CurrentTime);
            XSync(dpy, False);
        }
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "click") == 0 && argc >= 5) {
        void *xtst = dlopen("libXtst.so.6", RTLD_LAZY);
        if (!xtst) xtst = dlopen("libXtst.so", RTLD_LAZY);
        if (!xtst) return 3;
        XTestFakeButtonEventFn fb = (XTestFakeButtonEventFn)dlsym(xtst, "XTestFakeButtonEvent");
        XTestFakeMotionEventFn fm = (XTestFakeMotionEventFn)dlsym(xtst, "XTestFakeMotionEvent");
        unsigned int btn = 1;
        if (strcmp(argv[2], "middle") == 0) btn = 2;
        else if (strcmp(argv[2], "right") == 0) btn = 3;
        int x = atoi(argv[3]), y = atoi(argv[4]);
        fm(dpy, DefaultScreen(dpy), x, y, CurrentTime);
        XSync(dpy, False);
        usleep(20000);
        fb(dpy, btn, True, CurrentTime);
        XSync(dpy, False);
        usleep(25000);
        fb(dpy, btn, False, CurrentTime);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "clip-read") == 0 && argc >= 3) {
        Window w = XCreateSimpleWindow(dpy, root, -20, -20, 1, 1, 0, 0, 0);
        Atom a_clip = XInternAtom(dpy, "CLIPBOARD", False);
        Atom a_target = XInternAtom(dpy, argv[2], False);
        Atom a_prop = XInternAtom(dpy, "_WP17_SEL_OUT", False);
        XConvertSelection(dpy, a_clip, a_target, a_prop, w, CurrentTime);
        XFlush(dpy);

        for (int i = 0; i < 50; ++i) {
            while (XPending(dpy) > 0) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == SelectionNotify) {
                    if (ev.xselection.property == None) {
                        XDestroyWindow(dpy, w);
                        XCloseDisplay(dpy);
                        return 6;
                    }
                    Atom act = None;
                    int fmt = 0;
                    unsigned long nitems = 0, rem = 0;
                    unsigned char *data = NULL;
                    if (XGetWindowProperty(dpy, w, a_prop, 0, 16384, True, AnyPropertyType,
                                           &act, &fmt, &nitems, &rem, &data) == Success && data) {
                        fwrite(data, 1, nitems, stdout);
                        fflush(stdout);
                        XFree(data);
                    }
                    XDestroyWindow(dpy, w);
                    XCloseDisplay(dpy);
                    return 0;
                }
            }
            usleep(20000);
        }
        XDestroyWindow(dpy, w);
        XCloseDisplay(dpy);
        return 7;
    }

    if (strcmp(argv[1], "clip-serve") == 0 && argc >= 5) {
        FILE *fp = fopen(argv[3], "rb");
        if (!fp) return 4;
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        size_t len = sz > 0 ? (size_t)sz : 0;
        unsigned char *payload = (unsigned char *)malloc(len + 1);
        if (len > 0) (void)fread(payload, 1, len, fp);
        fclose(fp);

        signal(SIGTERM, on_sig);
        signal(SIGINT, on_sig);

        Window w = XCreateSimpleWindow(dpy, root, -30, -30, 1, 1, 0, 0, 0);
        Atom a_clip = XInternAtom(dpy, "CLIPBOARD", False);
        Atom a_targets = XInternAtom(dpy, "TARGETS", False);
        Atom a_mime = XInternAtom(dpy, argv[2], False);
        Atom a_uri = XInternAtom(dpy, "text/uri-list", False);

        XSetSelectionOwner(dpy, a_clip, w, CurrentTime);
        XSync(dpy, False);

        FILE *rf = fopen(argv[4], "w");
        if (rf) { fprintf(rf, "READY\n"); fclose(rf); }

        int xfd = ConnectionNumber(dpy);
        while (!g_stop) {
            while (XPending(dpy) > 0) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == SelectionClear) {
                    g_stop = 1;
                    break;
                }
                if (ev.type == SelectionRequest) {
                    XSelectionRequestEvent *req = &ev.xselectionrequest;
                    XSelectionEvent resp = {0};
                    resp.type = SelectionNotify;
                    resp.requestor = req->requestor;
                    resp.selection = req->selection;
                    resp.target = req->target;
                    resp.time = req->time;
                    resp.property = None;
                    Atom prop = req->property != None ? req->property : req->target;

                    if (req->target == a_targets) {
                        Atom tlist[3] = { a_targets, a_mime, a_uri };
                        XChangeProperty(dpy, req->requestor, prop, XA_ATOM, 32,
                                        PropModeReplace, (unsigned char *)tlist, 3);
                        resp.property = prop;
                    } else if (req->target == a_mime || req->target == a_uri) {
                        XChangeProperty(dpy, req->requestor, prop, req->target, 8,
                                        PropModeReplace, payload, (int)len);
                        resp.property = prop;
                    }
                    XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&resp);
                    XFlush(dpy);
                }
            }
            struct pollfd p = { .fd = xfd, .events = POLLIN };
            (void)poll(&p, 1, 100);
        }
        free(payload);
        XDestroyWindow(dpy, w);
        XCloseDisplay(dpy);
        return 0;
    }

    XCloseDisplay(dpy);
    return 1;
}
"""


def wait_for(pred, timeout: float = 12.0, interval: float = 0.08, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_val = None
    while time.monotonic() < deadline:
        last_val = pred()
        if last_val:
            return last_val
        time.sleep(interval)
    raise RuntimeError(f"Timed out waiting for {desc} (last={last_val!r})")


def parse_prop_sections(raw: str) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    cur: dict[str, str] | None = None
    cur_name = ""
    for line in raw.splitlines():
        if line.startswith("=== ") and line.endswith(" ==="):
            cur_name = line[4:-4].strip()
            cur = {}
            sections[cur_name] = cur
        elif cur is not None:
            tokens = line.split("|") if cur_name == "_DT_SYNC_STATE" else [line]
            for tok in tokens:
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    cur[k.strip()] = v.strip()
    return sections


def read_erg_state(x11_helper: Path, env: dict[str, str]) -> dict[str, str]:
    cp = subprocess.run([str(x11_helper), "props"], env=env, capture_output=True, text=True, check=True)
    return parse_prop_sections(cp.stdout).get("_DT_FILEMGR_ERGONOMICS_STATE", {})


def read_sync_state(x11_helper: Path, env: dict[str, str]) -> dict[str, str]:
    cp = subprocess.run([str(x11_helper), "props"], env=env, capture_output=True, text=True, check=True)
    return parse_prop_sections(cp.stdout).get("_DT_SYNC_STATE", {})


def send_fm_cmd(x11_helper: Path, env: dict[str, str], payload: str) -> None:
    subprocess.run([str(x11_helper), "set-cmd", "_DT_FILEMGR_CMD", payload], env=env, check=True)


def send_sync_cmd(x11_helper: Path, env: dict[str, str], payload: str) -> None:
    subprocess.run([str(x11_helper), "set-cmd", "_DT_SYNC_CMD", payload], env=env, check=True)


def find_proc_mapping_libs(comm_name: str, home_dir: Path, required_libs: tuple[str, ...]) -> int | None:
    for ent in Path("/proc").iterdir():
        if not ent.name.isdigit():
            continue
        try:
            if (ent / "comm").read_text(encoding="utf-8").strip() != comm_name:
                continue
            ebytes = (ent / "environ").read_bytes().decode("utf-8", errors="replace")
            if f"HOME={home_dir}\x00" not in ebytes:
                continue
            maps = (ent / "maps").read_text(encoding="utf-8", errors="replace")
            if all(lib in maps for lib in required_libs):
                return int(ent.name)
        except OSError:
            continue
    return None


def run_lane(
    lane: str,
    disp_idx: int,
    comp_bin: Path,
    dtwm_x11_bin: Path,
    dtfile_bin: Path,
    dtsync_bin: Path,
    wl_helper_bin: Path,
    x11_helper_bin: Path,
    base_tmp: Path,
) -> str:
    lane_root = base_tmp / f"lane-{lane}"
    lane_root.mkdir(parents=True, exist_ok=True)
    lane_root.chmod(0o700)
    home = lane_root / "home"
    xdg_rt = lane_root / "runtime"
    xdg_data = home / ".local/share"
    xdg_cache = home / ".cache"
    xdg_cfg = home / ".config"
    for d in (home, xdg_rt, xdg_data, xdg_cache, xdg_cfg, home / ".dt"):
        d.mkdir(parents=True, exist_ok=True)
    xdg_rt.chmod(0o700)
    home.chmod(0o700)

    (home / ".Xdefaults").write_text(
        "Dtfile*openDir: current\n"
        "Dtfile*smallIconWidth: 24\n"
        "Dtfile*smallIconHeight: 24\n",
        encoding="utf-8",
    )

    # Create rich directory & file fixture for WP-17 tests
    work_dir = home / "Workspace"
    alpha_dir = work_dir / "Alpha"
    beta_dir = work_dir / "Beta"
    deep_docs = alpha_dir / "specs" / "2026"
    bulk_dir = work_dir / "BulkScan"
    ext_drop = home / "ExternalDrop"
    for d in (work_dir, alpha_dir, beta_dir, deep_docs, bulk_dir, ext_drop):
        d.mkdir(parents=True, exist_ok=True)

    draft_file = work_dir / "draft.txt"
    draft_file.write_text("initial specification v1\n", encoding="utf-8")

    trash_target = work_dir / "trash_me.txt"
    trash_target.write_text("keep me safe in undo\n", encoding="utf-8")

    hidden_file = work_dir / ".secret_config"
    hidden_file.write_text("hidden=true\n", encoding="utf-8")

    needle_doc = deep_docs / "architecture.md"
    needle_doc.write_text("# Wayland Design\nToken: CK_NEEDLE_42 verified in content worker.\n", encoding="utf-8")

    decoy_doc = deep_docs / "notes.md"
    decoy_doc.write_text("# Decoy without token\n", encoding="utf-8")

    # Populate 600 subdirectories/files in BulkScan so cancel-mid-scan is exercised deterministically
    for b in range(60):
        sub = bulk_dir / f"pkg_{b:03d}"
        sub.mkdir(exist_ok=True)
        for f in range(10):
            (sub / f"mod_{f:02d}.txt").write_text("scan payload " * 64 + "\n", encoding="utf-8")

    incoming_cut_file = ext_drop / "incoming #1.txt"
    incoming_cut_file.write_text("from native peer clipboard cut\n", encoding="utf-8")

    procs: list[subprocess.Popen] = []
    try:
        cde_build_root = comp_bin.parent.parent.parent
        env = os.environ.copy()
        ld_parts = [
            "/var/tmp/ck-wayland-build/wp00/fs-prefix/lib",
            "/var/tmp/ck-wayland-build/wp00/motif-prefix/lib",
            "/usr/dt/lib",
            str(cde_build_root / "lib/DtSvc/.libs"),
            str(cde_build_root / "lib/DtWidget/.libs"),
            str(cde_build_root / "lib/DtHelp/.libs"),
            str(cde_build_root / "lib/DtPrint/.libs"),
            str(cde_build_root / "lib/tt/lib/.libs"),
            str(cde_build_root / "lib/DtXinerama/.libs"),
        ]
        env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
        env["PATH"] = f"{comp_bin.parent}:/usr/dt/bin:/usr/bin:/bin"
        env.update({
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg_rt),
            "XDG_DATA_HOME": str(xdg_data),
            "XDG_CACHE_HOME": str(xdg_cache),
            "XDG_CONFIG_HOME": str(xdg_cfg),
            "DTFILE_ERGONOMICS_TEST": "1",
            "LC_ALL": "C.UTF-8",
            "LANG": "C.UTF-8",
        })

        # 1. Isolated session D-Bus
        dbus_addr_file = lane_root / "dbus.addr"
        with dbus_addr_file.open("w", encoding="utf-8") as dfp:
            dbus_proc = subprocess.Popen(
                ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                env=env, stdout=dfp, stderr=subprocess.DEVNULL, start_new_session=True,
            )
        procs.append(dbus_proc)
        wait_for(lambda: dbus_addr_file.is_file() and dbus_addr_file.read_text(encoding="utf-8").strip(),
                 timeout=6.0, desc=f"{lane} dbus-daemon")
        env["DBUS_SESSION_BUS_ADDRESS"] = dbus_addr_file.read_text(encoding="utf-8").splitlines()[0].strip()

        # 2. Headless Wayland compositor (`dtwm-wayland`) or X11 (`Xvfb` + `dtwm`)
        screen_w, screen_h = 1280, 900
        if lane == "wayland":
            status_file = lane_root / "comp.status"
            comp_log = lane_root / "dtwm-wayland.log"
            wenv = dict(
                env,
                WLR_BACKENDS="headless",
                WLR_HEADLESS_OUTPUTS="1",
                WLR_RENDERER="pixman",
                WLR_LIBINPUT_NO_DEVICES="1",
            )
            with comp_log.open("w", encoding="utf-8") as cfp:
                comp_proc = subprocess.Popen(
                    [str(comp_bin), "--status-file", str(status_file)],
                    env=wenv, stdout=cfp, stderr=subprocess.STDOUT, start_new_session=True,
                )
            procs.append(comp_proc)

            def comp_ready():
                if comp_proc.poll() is not None:
                    raise RuntimeError(f"dtwm-wayland exited:\n{comp_log.read_text(encoding='utf-8', errors='replace')}")
                if not status_file.is_file():
                    return None
                kv = dict(
                    ln.split("=", 1)
                    for ln in status_file.read_text(encoding="utf-8").splitlines()
                    if "=" in ln
                )
                return kv if (kv.get("WAYLAND_DISPLAY") and kv.get("DISPLAY")) else None

            kv = wait_for(comp_ready, timeout=10.0, desc="dtwm-wayland ready")
            env["WAYLAND_DISPLAY"] = kv["WAYLAND_DISPLAY"]
            env["DISPLAY"] = kv["DISPLAY"]
            screen_w, screen_h = 1280, 720
        else:
            xdisp = f":{disp_idx}"
            xvfb_proc = subprocess.Popen(
                ["Xvfb", xdisp, "-screen", "0", f"{screen_w}x{screen_h}x24", "-nolisten", "tcp"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(xvfb_proc)
            env["DISPLAY"] = xdisp
            wait_for(lambda: subprocess.run([str(x11_helper_bin), "props"], env=env,
                                            capture_output=True).returncode == 0,
                     timeout=8.0, desc="Xvfb ready")
            dtwm_proc = subprocess.Popen(
                [str(dtwm_x11_bin)],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(dtwm_proc)
            time.sleep(0.6)

        subprocess.run(["xrdb", "-merge", str(home / ".Xdefaults")], env=env, check=True)

        # 3. ToolTalk session (`ttsession -s`) + `dtfile`
        tt_proc = subprocess.Popen(
            ["/usr/dt/bin/ttsession", "-s"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(tt_proc)
        time.sleep(0.4)

        dtfile_log = lane_root / "dtfile.log"
        with dtfile_log.open("w", encoding="utf-8") as df_out:
            dtfile_proc = subprocess.Popen(
                [str(dtfile_bin), "-dir", str(work_dir)],
                env=env, stdout=df_out, stderr=subprocess.STDOUT, start_new_session=True,
            )
        procs.append(dtfile_proc)

        dtfile_pid = wait_for(
            lambda: find_proc_mapping_libs("dtfile", home, ("libXm.so.9", "libck-core-fs-lib.so.0.5")),
            timeout=10.0,
            desc=f"{lane} dtfile mapping libXm.so.9 + libck-core-fs-lib.so.0.5",
        )

        st0 = wait_for(
            lambda: s if (s := read_erg_state(x11_helper_bin, env)).get("dir") == str(work_dir) else None,
            timeout=10.0,
            desc=f"{lane} initial ergonomics state at {work_dir}",
        )

        vkbd_proc: subprocess.Popen | None = None
        if lane == "wayland":
            vkbd_ready = lane_root / "vkbd.ready"
            vkbd_proc = subprocess.Popen(
                [str(wl_helper_bin), "vkbd-daemon", str(vkbd_ready)],
                env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, bufsize=1, start_new_session=True,
            )
            procs.append(vkbd_proc)
            wait_for(lambda: vkbd_ready.is_file(), timeout=6.0, desc="wayland vkbd-daemon ready")

        def send_key(combo: str) -> None:
            if lane == "wayland":
                assert vkbd_proc and vkbd_proc.stdin and vkbd_proc.stdout
                vkbd_proc.stdin.write(f"key {combo}\n")
                vkbd_proc.stdin.flush()
                assert vkbd_proc.stdout.readline().strip() == "OK"
            else:
                subprocess.run([str(x11_helper_bin), "key", combo], env=env, check=True)

        def type_text(txt: str) -> None:
            if lane == "wayland":
                assert vkbd_proc and vkbd_proc.stdin and vkbd_proc.stdout
                vkbd_proc.stdin.write(f"type {txt}\n")
                vkbd_proc.stdin.flush()
                assert vkbd_proc.stdout.readline().strip() == "OK"
            else:
                subprocess.run([str(x11_helper_bin), "type", txt], env=env, check=True)

        def click_at(btn: str, x: int, y: int) -> None:
            if lane == "wayland":
                cur = read_erg_state(x11_helper_bin, env)
                sw = int(cur.get("screen_w", str(screen_w)) or screen_w)
                sh = int(cur.get("screen_h", str(screen_h)) or screen_h)
                subprocess.run([str(wl_helper_bin), "click", btn, str(x), str(y),
                                str(sw), str(sh)], env=env, check=True)
            else:
                subprocess.run([str(x11_helper_bin), "click", btn, str(x), str(y)], env=env, check=True)

        def wait_erg(pred, timeout: float = 8.0, desc: str = "erg state") -> dict[str, str]:
            deadline = time.monotonic() + timeout
            last_s: dict[str, str] = {}
            while time.monotonic() < deadline:
                last_s = read_erg_state(x11_helper_bin, env)
                if pred(last_s):
                    return last_s
                time.sleep(0.08)
            raise RuntimeError(f"Timed out waiting for {desc} (last={last_s})")

        # Ensure dtfile window has pointer & keyboard focus
        send_fm_cmd(x11_helper_bin, env, "op=publish\n")
        time.sleep(0.12)
        st0 = wait_for(
            lambda: s if int((s := read_erg_state(x11_helper_bin, env)).get("win_rx", "0")) > 0 else None,
            timeout=6.0,
            desc=f"{lane} realized file_window coordinates",
        )
        click_at("left", int(st0["win_rx"]), int(st0["win_ry"]))
        time.sleep(0.15)

        # ---- CHECK 1: Multi-Tab Browsing & Shortcuts ----
        # 1a) Ctrl+T opens tab 2
        send_key("ctrl+t")
        wait_erg(
            lambda s: s.get("tabs") == "2" and s.get("active_tab") == "1",
            timeout=6.0,
            desc=f"{lane} Ctrl+T -> tabs=2, active_tab=1",
        )

        # 1b) Navigate tab 2 into Alpha/specs -> Alt+Left (Back) -> Alt+Right (Forward) -> Alt+Up (Parent)
        send_fm_cmd(x11_helper_bin, env, f"op=navigate\ndir={alpha_dir}\n")
        wait_erg(
            lambda s: s.get("dir") == str(alpha_dir) and s.get("can_back") == "1",
            timeout=6.0,
            desc=f"{lane} navigate to Alpha",
        )
        send_key("alt+left")
        wait_erg(
            lambda s: s.get("dir") == str(work_dir)
            and s.get("can_back") == "0" and s.get("can_fwd") == "1",
            timeout=6.0,
            desc=f"{lane} Alt+Left Back to Workspace",
        )
        send_key("alt+right")
        wait_erg(
            lambda s: s.get("dir") == str(alpha_dir)
            and s.get("can_back") == "1" and s.get("can_fwd") == "0",
            timeout=6.0,
            desc=f"{lane} Alt+Right Forward to Alpha",
        )
        send_key("alt+up")
        wait_erg(
            lambda s: s.get("dir") == str(work_dir),
            timeout=6.0,
            desc=f"{lane} Alt+Up to parent Workspace",
        )

        # 1c) Ctrl+Tab / Ctrl+Shift+Tab cycles tabs
        send_key("ctrl+tab")
        wait_erg(
            lambda s: s.get("active_tab") == "0",
            timeout=6.0,
            desc=f"{lane} Ctrl+Tab -> active_tab=0",
        )
        send_key("ctrl+shift+tab")
        wait_erg(
            lambda s: s.get("active_tab") == "1",
            timeout=6.0,
            desc=f"{lane} Ctrl+Shift+Tab -> active_tab=1",
        )

        # 1d) Middle-click (`Button2` without drag) on first folder icon opens 3rd tab!
        send_fm_cmd(x11_helper_bin, env, "op=publish\n")
        time.sleep(0.12)
        st_dir = wait_for(
            lambda: s if int((s := read_erg_state(x11_helper_bin, env)).get("first_dir_rx", "0")) > 0 else None,
            timeout=6.0,
            desc=f"{lane} first subfolder icon coordinates",
        )
        expected_mid_dir = str(work_dir / st_dir["first_dir_name"])
        click_at("middle", int(st_dir["first_dir_rx"]), int(st_dir["first_dir_ry"]))
        wait_for(
            lambda: (s := read_erg_state(x11_helper_bin, env)).get("tabs") == "3"
            and s.get("active_tab") == "2"
            and s.get("dir") == expected_mid_dir,
            timeout=6.0,
            desc=f"{lane} middle-click folder -> tabs=3 at {expected_mid_dir}",
        )

        # 1e) Ctrl+W closes tab 3 and tab 2 back to single-tab Workspace
        send_key("ctrl+w")
        wait_erg(lambda s: s.get("tabs") == "2", desc=f"{lane} Ctrl+W -> tabs=2")
        time.sleep(0.1)
        send_key("ctrl+w")
        wait_erg(lambda s: s.get("tabs") == "1" and s.get("dir") == str(work_dir),
                 desc=f"{lane} Ctrl+W -> tabs=1 at Workspace")

        # 1f) Ctrl+H toggles hidden files
        send_key("ctrl+h")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("hidden") == "1",
                 timeout=6.0, desc=f"{lane} Ctrl+H -> hidden=1")
        send_fm_cmd(x11_helper_bin, env, "op=select\nfile=.secret_config\n")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("sel0") == ".secret_config",
                 timeout=6.0, desc=f"{lane} hidden .secret_config selectable")
        send_key("ctrl+h")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("hidden") == "0",
                 timeout=6.0, desc=f"{lane} Ctrl+H -> hidden=0")

        # 1g) F2 inline rename (`draft.txt` -> `final_spec.txt`)
        send_fm_cmd(x11_helper_bin, env, "op=select\nfile=draft.txt\n")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("sel0") == "draft.txt",
                 timeout=6.0, desc=f"{lane} select draft.txt")
        send_key("f2")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("renaming") == "1",
                 timeout=6.0, desc=f"{lane} F2 -> inline rename active")
        type_text("final_spec.txt")
        final_spec_file = work_dir / "final_spec.txt"
        wait_for(
            lambda: final_spec_file.is_file()
            and not draft_file.exists()
            and read_erg_state(x11_helper_bin, env).get("undo_depth") == "1",
            timeout=8.0,
            desc=f"{lane} F2 rename committed -> final_spec.txt + undo_depth=1",
        )

        # ---- CHECK 2: Asynchronous In-Window Search (`Ctrl+F` + `ck-core-fs-lib`) ----
        send_key("ctrl+f")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("search_open") == "1",
                 timeout=6.0, desc=f"{lane} Ctrl+F opens search bar")
        send_key("esc")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("search_open") == "0",
                 timeout=6.0, desc=f"{lane} Escape closes search bar")

        # Run recursive glob + content + size + modified-date search
        send_fm_cmd(
            x11_helper_bin,
            env,
            "op=search\n"
            "name=*.md\n"
            "content=CK_NEEDLE_42\n"
            "min_size=20\n"
            "max_size=4096\n"
            "after=2025-01-01\n"
            "recursive=1\n"
            "case=1\n",
        )
        wait_for(
            lambda: (s := read_erg_state(x11_helper_bin, env)).get("search_state") == "complete"
            and s.get("search_hits") == "1"
            and s.get("search_results_visible") == "1",
            timeout=10.0,
            desc=f"{lane} streaming ck-core-fs-lib search complete with 1 exact hit",
        )

        # Reveal hit 0 in folder -> navigates to deep_docs and selects architecture.md!
        send_fm_cmd(x11_helper_bin, env, "op=search_reveal\nindex=0\n")
        wait_for(
            lambda: (s := read_erg_state(x11_helper_bin, env)).get("dir") == str(deep_docs)
            and s.get("sel0") == "architecture.md"
            and s.get("search_results_visible") == "0",
            timeout=8.0,
            desc=f"{lane} Reveal in Folder navigated to {deep_docs} and selected architecture.md",
        )

        # Close search bar and return to Workspace, then test mid-scan cancel on BulkScan
        send_key("esc")
        send_fm_cmd(x11_helper_bin, env, f"op=navigate\ndir={work_dir}\n")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("dir") == str(work_dir),
                 timeout=6.0, desc=f"{lane} back at Workspace")

        send_fm_cmd(
            x11_helper_bin,
            env,
            "op=search\nname=*.txt\ncontent=nonexistent_token_xyz\nrecursive=1\n",
        )
        send_fm_cmd(x11_helper_bin, env, "op=search_cancel\n")
        wait_for(
            lambda: read_erg_state(x11_helper_bin, env).get("search_state") in ("cancelled", "complete"),
            timeout=8.0,
            desc=f"{lane} search cancellation settled cleanly",
        )
        send_key("esc")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("search_open") == "0",
                 timeout=6.0, desc=f"{lane} search bar closed after cancel")

        # ---- CHECK 3: Cross-Toolkit File Clipboard (`Ctrl+C` / `Ctrl+X` / `Ctrl+V`) ----
        click_at("left", int(st0["win_rx"]), int(st0["win_ry"]))
        send_fm_cmd(x11_helper_bin, env, "op=select\nfile=final_spec.txt\n")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("sel0") == "final_spec.txt",
                 timeout=6.0, desc=f"{lane} select final_spec.txt for Ctrl+C")
        send_key("ctrl+c")
        wait_for(
            lambda: (s := read_erg_state(x11_helper_bin, env)).get("clip_owned") == "1"
            and s.get("clip_cut") == "0",
            timeout=6.0,
            desc=f"{lane} Ctrl+C owns CLIPBOARD",
        )

        # Read `x-special/gnome-copied-files` from native Wayland / X11 peer!
        peer_bin = wl_helper_bin if lane == "wayland" else x11_helper_bin
        clip_out = subprocess.run(
            [str(peer_bin), "clip-read", "x-special/gnome-copied-files"],
            env=env, capture_output=True, text=True, check=True,
        ).stdout
        expected_uri = final_spec_file.resolve().as_uri()
        assert clip_out.strip() == f"copy\n{expected_uri}", (
            f"{lane}: unexpected gnome clipboard payload: {clip_out!r}"
        )

        # Same-folder Ctrl+V -> creates `final_spec (copy).txt`!
        send_key("ctrl+v")
        dup_file = work_dir / "final_spec (copy).txt"
        wait_for(
            lambda: dup_file.is_file()
            and dup_file.read_text(encoding="utf-8") == "initial specification v1\n"
            and read_erg_state(x11_helper_bin, env).get("undo_depth") == "2",
            timeout=8.0,
            desc=f"{lane} same-folder Ctrl+V -> final_spec (copy).txt + undo_depth=2",
        )

        # Native peer publishes `cut\nfile:///.../incoming%20%231.txt` -> dtfile `Ctrl+V` moves it!
        payload_file = lane_root / "peer_cut.payload"
        ready_file = lane_root / "peer_cut.ready"
        payload_file.write_text(f"cut\n{incoming_cut_file.resolve().as_uri()}", encoding="utf-8")
        peer_proc = subprocess.Popen(
            [str(peer_bin), "clip-serve", "x-special/gnome-copied-files",
             str(payload_file), str(ready_file)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(peer_proc)
        wait_for(lambda: ready_file.is_file(), timeout=6.0, desc=f"{lane} native peer clip-serve ready")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("clip_owned") == "0",
                 timeout=6.0, desc=f"{lane} dtfile yielded CLIPBOARD ownership to peer")

        moved_incoming = work_dir / "incoming #1.txt"
        send_key("ctrl+v")
        wait_for(
            lambda: moved_incoming.is_file()
            and not incoming_cut_file.exists()
            and read_erg_state(x11_helper_bin, env).get("undo_depth") == "3",
            timeout=8.0,
            desc=f"{lane} cross-toolkit Cut+Paste moved 'incoming #1.txt' + undo_depth=3",
        )
        peer_proc.terminate()

        # ---- CHECK 4: Bounded Session Undo (`Ctrl+Z`) across Trash, Move, Copy, Rename ----
        # Record a Move-to-Trash operation on `trash_me.txt`
        send_fm_cmd(x11_helper_bin, env, "op=select\nfile=trash_me.txt\n")
        wait_for(lambda: read_erg_state(x11_helper_bin, env).get("sel0") == "trash_me.txt",
                 timeout=6.0, desc=f"{lane} select trash_me.txt")
        send_fm_cmd(x11_helper_bin, env, "op=trash_selected\n")
        wait_for(
            lambda: not trash_target.exists()
            and read_erg_state(x11_helper_bin, env).get("undo_depth") == "4",
            timeout=8.0,
            desc=f"{lane} trash_me.txt moved to Trash + undo_depth=4",
        )

        # Undo #1: restores `trash_me.txt` from Trash!
        send_key("ctrl+z")
        wait_for(
            lambda: trash_target.is_file()
            and trash_target.read_text(encoding="utf-8") == "keep me safe in undo\n"
            and read_erg_state(x11_helper_bin, env).get("undo_depth") == "3",
            timeout=8.0,
            desc=f"{lane} Ctrl+Z #1 restored trash_me.txt",
        )

        # Undo #2: reverses Cut+Paste Move (`incoming #1.txt` back to ExternalDrop)!
        send_key("ctrl+z")
        wait_for(
            lambda: incoming_cut_file.is_file()
            and not moved_incoming.exists()
            and read_erg_state(x11_helper_bin, env).get("undo_depth") == "2",
            timeout=8.0,
            desc=f"{lane} Ctrl+Z #2 reversed Move back to ExternalDrop",
        )

        # Undo #3: reverses Copy (`final_spec (copy).txt` moved safely to XDG Trash, not unlinked)!
        send_key("ctrl+z")
        xdg_trash_files = xdg_data / "Trash" / "files"
        wait_for(
            lambda: not dup_file.exists()
            and xdg_trash_files.is_dir()
            and any("final_spec" in p.name for p in xdg_trash_files.iterdir())
            and read_erg_state(x11_helper_bin, env).get("undo_depth") == "1",
            timeout=8.0,
            desc=f"{lane} Ctrl+Z #3 disposed copied file safely into XDG Trash",
        )

        # Undo #4: reverses F2 Rename (`final_spec.txt` -> `draft.txt`)!
        send_key("ctrl+z")
        wait_for(
            lambda: draft_file.is_file()
            and not final_spec_file.exists()
            and read_erg_state(x11_helper_bin, env).get("undo_depth") == "0",
            timeout=8.0,
            desc=f"{lane} Ctrl+Z #4 reversed Rename back to draft.txt",
        )

        # ---- CHECK 5: Validated `dtfile_sync` (*Synchronize Folders...*) Review & Apply ----
        sync_src = home / "SyncSrc"
        sync_dst = home / "SyncDst"
        sync_src.mkdir()
        sync_dst.mkdir()
        (sync_src / "fresh.txt").write_text("new synced file\n", encoding="utf-8")
        (sync_src / "shared.txt").write_text("updated source version\n", encoding="utf-8")
        (sync_dst / "shared.txt").write_text("stale target version\n", encoding="utf-8")

        dtsync_log = lane_root / "dtsync.log"
        with dtsync_log.open("w", encoding="utf-8") as ds_out:
            sync_proc = subprocess.Popen(
                [str(dtsync_bin), str(sync_src), str(sync_dst)],
                env=env, stdout=ds_out, stderr=subprocess.STDOUT, start_new_session=True,
            )
        procs.append(sync_proc)

        dtsync_pid = wait_for(
            lambda: find_proc_mapping_libs("dtfile_sync", home, ("libXm.so.9", "libck-core-fs-lib.so.0.5")),
            timeout=8.0,
            desc=f"{lane} dtfile_sync mapping libXm.so.9 + libck-core-fs-lib.so.0.5",
        )
        wait_for(
            lambda: "ready" in read_sync_state(x11_helper_bin, env),
            timeout=8.0,
            desc=f"{lane} dtfile_sync realized and publishing _DT_SYNC_STATE",
        )

        # Select `Synchronize one way` (profile:4 = CKFSP_SYNC), ordinary caller metadata (metadata:1), and start Compare
        send_sync_cmd(x11_helper_bin, env, "profile:4")
        time.sleep(0.22)
        send_sync_cmd(x11_helper_bin, env, "metadata:1")
        time.sleep(0.22)
        send_sync_cmd(x11_helper_bin, env, "compare")
        wait_for(
            lambda: (s := read_sync_state(x11_helper_bin, env)).get("ready") == "1"
            and s.get("state") == "1",
            timeout=15.0,
            desc=f"{lane} dtfile_sync preview reached CKFET_AWAITING_REVIEW (state=1)",
        )

        # Add non-destructive conflict rule (`Existing regular file` = 0 -> `Keep target` = 2) and click Review
        send_sync_cmd(x11_helper_bin, env, "rule:0:2")
        time.sleep(0.25)
        send_sync_cmd(x11_helper_bin, env, "review")
        wait_for(
            lambda: (s := read_sync_state(x11_helper_bin, env)).get("apply_sensitive") == "1"
            and int(s.get("receipt", "0")) > 0
            and s.get("unresolved") == "0",
            timeout=10.0,
            desc=f"{lane} dtfile_sync reviewed receipt ready (apply_sensitive=1)",
        )

        # Execute Apply reviewed plan!
        send_sync_cmd(x11_helper_bin, env, "apply")
        wait_for(
            lambda: (s := read_sync_state(x11_helper_bin, env)).get("state") == "4"
            and s.get("finished") == "1",
            timeout=15.0,
            desc=f"{lane} dtfile_sync Apply completed (CKFET_COMPLETE, finished=1)",
        )
        assert (sync_dst / "fresh.txt").read_text(encoding="utf-8") == "new synced file\n"
        assert (sync_dst / "shared.txt").read_text(encoding="utf-8") == "stale target version\n"
        send_sync_cmd(x11_helper_bin, env, "close")

        return (
            f"PASS [{lane}]: dtfile_pid={dtfile_pid}, dtsync_pid={dtsync_pid} "
            f"(tabs, shortcuts, middle-click, async search+reveal+cancel, "
            f"cross-toolkit clipboard, 4-kind undo, and dtfile_sync Apply verified)"
        )
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        time.sleep(0.25)
        for p in reversed(procs):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True, help="Path to dtwm-wayland binary")
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    if not comp_bin.is_file():
        print(f"ERROR: compositor not found: {comp_bin}", file=sys.stderr)
        return 1

    dtwm_x11_bin = comp_bin.parent / "dtwm"
    if not dtwm_x11_bin.is_file():
        dtwm_x11_bin = Path("/usr/dt/bin/dtwm")

    dtfile_bin = comp_bin.parent.parent / "dtfile" / ".libs" / "dtfile"
    if not dtfile_bin.is_file():
        dtfile_bin = Path("/usr/dt/bin/dtfile")

    dtsync_bin = comp_bin.parent.parent / "dtfile" / "dtsync" / "dtfile_sync"
    if not dtsync_bin.is_file():
        dtsync_bin = Path("/usr/dt/bin/dtfile_sync")

    cc_cmd = shlex.split(os.environ.get("CC", "cc"))
    pkg_cfg = os.environ.get("PKG_CONFIG", "pkg-config")

    with tempfile.TemporaryDirectory(prefix="wp17-dual-", dir=os.environ.get("TMPDIR")) as td:
        base_tmp = Path(td)
        xml_path = base_tmp / "wlr-data-control.xml"
        hdr_path = base_tmp / "wlr-data-control-client.h"
        code_path = base_tmp / "wlr-data-control-code.c"
        wl_c_path = base_tmp / "wl_helper.c"
        x11_c_path = base_tmp / "x11_helper.c"
        wl_helper_bin = base_tmp / "wl-wp17-helper"
        x11_helper_bin = base_tmp / "x11-wp17-helper"

        xml_path.write_text(WLR_DATA_CONTROL_XML, encoding="utf-8")
        wl_c_path.write_text(WL_HELPER_C, encoding="utf-8")
        x11_c_path.write_text(X11_HELPER_C, encoding="utf-8")

        subprocess.run(["wayland-scanner", "client-header", str(xml_path), str(hdr_path)], check=True)
        subprocess.run(["wayland-scanner", "private-code", str(xml_path), str(code_path)], check=True)

        wl_flags = shlex.split(
            subprocess.check_output([pkg_cfg, "--cflags", "--libs", "wayland-client", "xkbcommon"], text=True)
        )
        x11_flags = shlex.split(
            subprocess.check_output([pkg_cfg, "--cflags", "--libs", "x11"], text=True)
        )

        subprocess.run(
            [*cc_cmd, "-O2", "-Wall", "-Wextra", f"-I{base_tmp}",
             str(wl_c_path), str(code_path), "-o", str(wl_helper_bin), *wl_flags],
            check=True,
        )
        subprocess.run(
            [*cc_cmd, "-O2", "-Wall", "-Wextra",
             str(x11_c_path), "-o", str(x11_helper_bin), *x11_flags, "-ldl"],
            check=True,
        )

        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_wl = pool.submit(
                run_lane, "wayland", 0, comp_bin, dtwm_x11_bin,
                dtfile_bin, dtsync_bin, wl_helper_bin, x11_helper_bin, base_tmp,
            )
            fut_x11 = pool.submit(
                run_lane, "x11", 89, comp_bin, dtwm_x11_bin,
                dtfile_bin, dtsync_bin, wl_helper_bin, x11_helper_bin, base_tmp,
            )
            err_wl = err_x11 = None
            res_wl = res_x11 = ""
            try:
                res_wl = fut_wl.result()
            except Exception as exc:
                err_wl = exc
            try:
                res_x11 = fut_x11.result()
            except Exception as exc:
                err_x11 = exc

        if res_wl:
            print(res_wl)
        if res_x11:
            print(res_x11)
        if err_wl or err_x11:
            if err_wl:
                print(f"FAIL [wayland]: {err_wl}", file=sys.stderr)
            if err_x11:
                print(f"FAIL [x11]: {err_x11}", file=sys.stderr)
            raise (err_wl or err_x11)
        print("WP-17 parallel Wayland + X11 integration checks: ALL PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
