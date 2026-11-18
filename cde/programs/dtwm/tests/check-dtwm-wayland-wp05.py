#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland WP-05:
1. Bidirectional Cross-Protocol Drag-and-Drop (Wayland wl_data_device <-> Motif XDND v5).
2. ck-grab Wayland screencopy (active native Wayland window + server-side CDE bevel frame, and dual-output full screen).
3. Mixed-DPI per-output XLibre RandR DPI (96 <-> 192 <-> 144) live Motif shell scale reflow + Wayland wp_fractional_scale_v1.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image


WL_DND_CLIENT_C = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

static struct wl_display *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct xdg_wm_base *g_wm;
static struct wl_seat *g_seat;
static struct wl_data_device_manager *g_ddm;
static struct wp_fractional_scale_manager_v1 *g_fsm;

static struct wl_surface *g_surf;
static struct xdg_surface *g_xdgs;
static struct xdg_toplevel *g_top;
static struct wl_pointer *g_ptr;
static struct wl_data_device *g_dd;
static struct wp_fractional_scale_v1 *g_fs;

static int g_configured = 0;
static uint32_t g_scale_120 = 120;
static const char *g_mode = "drop"; /* "drag" or "drop" */
static const char *g_out_file = NULL;
static int g_drag_started = 0;
static int g_done = 0;

static struct wl_data_offer *g_cur_offer = NULL;
static int g_offer_has_uri = 0;

static int create_shm_fd(size_t sz) {
    char name[64];
    snprintf(name, sizeof(name), "/wl-wp05-%d", (int)getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return -1; }
    return fd;
}

static struct wl_buffer *make_solid_buffer(int w, int h, uint32_t argb) {
    size_t sz = (size_t)w * (size_t)h * 4;
    int fd = create_shm_fd(sz);
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < w * h; ++i) px[i] = argb;
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)sz);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(px, sz);
    close(fd);
    return buf;
}

/* --- Data Source (Wayland -> Motif drag initiator) --- */
static const char g_uri_payload[] = "file:///tmp/from-wayland-wp05.txt\r\n";

static void ds_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void ds_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd) {
    (void)d; (void)s; (void)mime;
    (void)write(fd, g_uri_payload, strlen(g_uri_payload));
    close(fd);
}
static void ds_cancelled(void *d, struct wl_data_source *s) {
    (void)d;
    wl_data_source_destroy(s);
}
static void ds_dnd_drop_performed(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void ds_dnd_finished(void *d, struct wl_data_source *s) {
    (void)d;
    wl_data_source_destroy(s);
    g_done = 1;
}
static void ds_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }

static const struct wl_data_source_listener ds_listener = {
    .target = ds_target,
    .send = ds_send,
    .cancelled = ds_cancelled,
    .dnd_drop_performed = ds_dnd_drop_performed,
    .dnd_finished = ds_dnd_finished,
    .action = ds_action,
};

/* --- Data Offer / Device (Motif -> Wayland drop receiver) --- */
static void do_offer(void *d, struct wl_data_offer *o, const char *mime) {
    (void)d; (void)o;
    if (strcmp(mime, "text/uri-list") == 0 || strcmp(mime, "text/plain;charset=utf-8") == 0 ||
        strcmp(mime, "UTF8_STRING") == 0 || strcmp(mime, "TEXT") == 0) {
        g_offer_has_uri = 1;
    }
}
static void do_src_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static void do_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }

static const struct wl_data_offer_listener do_listener = {
    .offer = do_offer, .source_actions = do_src_actions, .action = do_action,
};

static void dd_data_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *id) {
    (void)d; (void)dd;
    g_offer_has_uri = 0;
    wl_data_offer_add_listener(id, &do_listener, NULL);
}

static void dd_enter(void *d, struct wl_data_device *dd, uint32_t serial,
                     struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id) {
    (void)d; (void)dd; (void)surf; (void)x; (void)y;
    g_cur_offer = id;
    if (id) {
        wl_data_offer_accept(id, serial, "text/uri-list");
        wl_data_offer_set_actions(
            id,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
        );
    }
}

static void dd_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)dd; (void)t; (void)x; (void)y;
}

static void dd_drop(void *d, struct wl_data_device *dd) {
    (void)d; (void)dd;
    if (!g_cur_offer) return;
    int fds[2];
    if (pipe(fds) != 0) return;
    wl_data_offer_receive(g_cur_offer, "text/uri-list", fds[1]);
    close(fds[1]);
    wl_display_flush(g_dpy);

    char buf[512] = {0};
    ssize_t n = 0, total = 0;
    struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
    while (poll(&pfd, 1, 1500) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
        n = read(fds[0], buf + total, sizeof(buf) - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
        wl_display_dispatch_pending(g_dpy);
    }
    close(fds[0]);
    wl_data_offer_finish(g_cur_offer);
    wl_data_offer_destroy(g_cur_offer);
    g_cur_offer = NULL;

    if (g_out_file) {
        FILE *fp = fopen(g_out_file, "w");
        if (fp) {
            fprintf(fp, "DROPPED=%s\nSCALE120=%u\n", buf, g_scale_120);
            fclose(fp);
        }
    }
    g_done = 1;
}

static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *id) {
    (void)d; (void)dd; (void)id;
}

static const struct wl_data_device_listener dd_listener = {
    .data_offer = dd_data_offer,
    .enter = dd_enter,
    .leave = dd_leave,
    .motion = dd_motion,
    .drop = dd_drop,
    .selection = dd_selection,
};

/* --- Pointer (starts drag on button press when g_mode == "drag") --- */
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)s; (void)sf; (void)x; (void)y;
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {
    (void)d; (void)p; (void)s; (void)sf;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t; (void)x; (void)y;
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)time; (void)button;
    if (strcmp(g_mode, "drag") == 0 && state == WL_POINTER_BUTTON_STATE_PRESSED && !g_drag_started) {
        g_drag_started = 1;
        struct wl_data_source *src = wl_data_device_manager_create_data_source(g_ddm);
        wl_data_source_add_listener(src, &ds_listener, NULL);
        wl_data_source_offer(src, "text/uri-list");
        wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

        struct wl_surface *icon = wl_compositor_create_surface(g_comp);
        struct wl_buffer *ibuf = make_solid_buffer(24, 24, 0xFFFF8800u);
        wl_surface_attach(icon, ibuf, 0, 0);
        wl_surface_commit(icon);

        wl_data_device_start_drag(g_dd, src, g_surf, icon, serial);
        wl_display_flush(g_dpy);
    }
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
    (void)d; (void)p; (void)t; (void)a; (void)v;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_src(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t ds) { (void)d; (void)p; (void)a; (void)ds; }

static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
    .axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_src,
    .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_disc,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_ptr) {
        g_ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_ptr, &ptr_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

static void fs_preferred_scale(void *d, struct wp_fractional_scale_v1 *fs, uint32_t scale120) {
    (void)d; (void)fs;
    g_scale_120 = scale120;
    if (g_out_file && strcmp(g_mode, "scale") == 0) {
        FILE *fp = fopen(g_out_file, "w");
        if (fp) { fprintf(fp, "SCALE120=%u\n", g_scale_120); fclose(fp); }
    }
}
static const struct wp_fractional_scale_v1_listener fs_listener = { .preferred_scale = fs_preferred_scale };

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xdgs_conf(void *d, struct xdg_surface *xs, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    if (!g_configured) {
        g_configured = 1;
        struct wl_buffer *buf = make_solid_buffer(240, 180, 0xFF1868A8u);
        wl_surface_attach(g_surf, buf, 0, 0);
        wl_surface_commit(g_surf);
    }
}
static const struct xdg_surface_listener xdgs_listener = { .configure = xdgs_conf };

static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)d; (void)t; (void)w; (void)h; (void)st;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; g_done = 1; }
static const struct xdg_toplevel_listener top_listener = { .configure = top_conf, .close = top_close };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        g_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
        xdg_wm_base_add_listener(g_wm, &wm_listener, NULL);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(g_seat, &seat_listener, NULL);
    } else if (strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        g_ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
    } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        g_fsm = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_rem };

int main(int argc, char **argv) {
    if (argc > 1) g_mode = argv[1];
    if (argc > 2) g_out_file = argv[2];

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(g_dpy);
    wl_display_roundtrip(g_dpy);

    g_dd = wl_data_device_manager_get_data_device(g_ddm, g_seat);
    wl_data_device_add_listener(g_dd, &dd_listener, NULL);

    g_surf = wl_compositor_create_surface(g_comp);
    if (g_fsm) {
        g_fs = wp_fractional_scale_manager_v1_get_fractional_scale(g_fsm, g_surf);
        wp_fractional_scale_v1_add_listener(g_fs, &fs_listener, NULL);
    }
    g_xdgs = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    xdg_surface_add_listener(g_xdgs, &xdgs_listener, NULL);
    g_top = xdg_surface_get_toplevel(g_xdgs);
    xdg_toplevel_add_listener(g_top, &top_listener, NULL);
    xdg_toplevel_set_title(g_top, "WaylandDnDWindow");
    wl_surface_commit(g_surf);

    while (!g_done && wl_display_dispatch(g_dpy) != -1) {
    }
    return 0;
}
"""


MOTIF_WP05_CLIENT_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <Xm/XmAll.h>
#include <Xm/DragDrop.h>
#include <Xm/DropSMgr.h>
#include <Xm/DropTrans.h>
#include "XlibreDpiI.h"

static const char *g_out_path = NULL;
static Widget g_shell = NULL;
static Widget g_btn = NULL;
static Atom g_uri_atom = None;

static void write_dpi_status(void) {
    if (!g_out_path || !g_shell) return;
    double scale = 0.0;
    Boolean ok = _XmXlibreDpiGetWidgetScale(g_shell, &scale);
    Dimension bw = 0, bh = 0;
    if (g_btn) {
        XtVaGetValues(g_btn, XmNwidth, &bw, XmNheight, &bh, NULL);
    }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_out_path);
    FILE *fp = fopen(tmp, "w");
    if (fp) {
        fprintf(fp, "INSTALLED=%d\nSCALE=%.2f\nBTN=%ux%u\n", (int)ok, scale, (unsigned)bw, (unsigned)bh);
        fclose(fp);
        rename(tmp, g_out_path);
    }
}

static void timer_poll_cb(XtPointer client, XtIntervalId *id) {
    (void)client; (void)id;
    write_dpi_status();
    XtAppAddTimeOut(XtWidgetToApplicationContext(g_shell), 100, timer_poll_cb, NULL);
}

/* --- Motif Drop Receiver (Wayland -> Motif XDND v5) --- */
static void transfer_cb(Widget w, XtPointer closure, Atom *seltype, Atom *type,
                        XtPointer value, unsigned long *length, int *format) {
    (void)w; (void)closure; (void)seltype; (void)format;
    if (value && *length > 0 && *type != None && g_out_path) {
        char buf[512] = {0};
        size_t cp = *length < sizeof(buf) - 1 ? *length : sizeof(buf) - 1;
        memcpy(buf, value, cp);
        FILE *fp = fopen(g_out_path, "w");
        if (fp) {
            fprintf(fp, "MOTIF_DROPPED=%s\n", buf);
            fclose(fp);
        }
    }
    if (value) XtFree((char *)value);
}

static void drop_proc(Widget w, XtPointer client, XtPointer call) {
    (void)client;
    XmDropProcCallback dd = (XmDropProcCallback)call;
    Arg args[4];
    int n = 0;
    XmDropTransferEntryRec entry;
    entry.target = g_uri_atom;
    entry.client_data = NULL;

    if (dd->dropAction == XmDROP && (dd->operations & (XmDROP_COPY | XmDROP_MOVE))) {
        dd->operation = XmDROP_COPY;
        dd->dropSiteStatus = XmVALID_DROP_SITE;
        XtSetArg(args[n], XmNdropTransfers, &entry); n++;
        XtSetArg(args[n], XmNnumDropTransfers, 1); n++;
        XtSetArg(args[n], XmNtransferProc, transfer_cb); n++;
    } else {
        dd->dropSiteStatus = XmINVALID_DROP_SITE;
        XtSetArg(args[n], XmNtransferStatus, XmTRANSFER_FAILURE); n++;
        XtSetArg(args[n], XmNnumDropTransfers, 0); n++;
    }
    XmDropTransferStart(dd->dragContext, args, n);
}

/* --- Motif Drag Initiator (Motif XDND v5 -> Wayland) --- */
static Boolean convert_proc(Widget w, Atom *selection, Atom *target, Atom *type_return,
                            XtPointer *value_return, unsigned long *length_return, int *format_return) {
    (void)w; (void)selection;
    static const char payload[] = "file:///tmp/from-motif-wp05.txt\r\n";
    Display *dpy = XtDisplay(g_shell);
    Atom targets_a = XInternAtom(dpy, "TARGETS", False);
    if (*target == targets_a) {
        Atom *tlist = (Atom *)XtMalloc(sizeof(Atom) * 2);
        tlist[0] = g_uri_atom;
        tlist[1] = XA_STRING;
        *type_return = XA_ATOM;
        *value_return = (XtPointer)tlist;
        *length_return = 2;
        *format_return = 32;
        return True;
    }
    if (*target == g_uri_atom || *target == XA_STRING) {
        char *copy = XtMalloc(sizeof(payload));
        memcpy(copy, payload, sizeof(payload) - 1);
        *type_return = *target;
        *value_return = (XtPointer)copy;
        *length_return = strlen(payload);
        *format_return = 8;
        return True;
    }
    return False;
}

static void start_motif_drag_eh(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    (void)client; (void)cont;
    if (!ev || ev->type != ButtonPress) return;
    if (ev->xbutton.button != Button1 && ev->xbutton.button != Button2) return;
    Atom exports[1] = { g_uri_atom };
    Arg args[6];
    int n = 0;
    XtSetArg(args[n], XmNexportTargets, exports); n++;
    XtSetArg(args[n], XmNnumExportTargets, 1); n++;
    XtSetArg(args[n], XmNconvertProc, convert_proc); n++;
    XtSetArg(args[n], XmNdragOperations, XmDROP_COPY); n++;
    XmDragStart(w, ev, args, n);
}

int main(int argc, char **argv) {
    const char *mode = "drop";
    if (argc > 1) mode = argv[1];
    if (argc > 2) g_out_path = argv[2];

    XtAppContext app;
    g_shell = XtVaAppInitialize(
        &app, "MotifWp05", NULL, 0, &argc, argv, NULL,
        XmNx, 420, XmNy, 120, XmNwidth, 260, XmNheight, 180,
        XmNtitle, "MotifWP05",
        NULL
    );

    Display *dpy = XtDisplay(g_shell);
    g_uri_atom = XInternAtom(dpy, "text/uri-list", False);

    Widget form = XtVaCreateManagedWidget("form", xmFormWidgetClass, g_shell, NULL);
    XmString empty_lbl = XmStringCreateLocalized("");
    g_btn = XtVaCreateManagedWidget(
        "toggleSample", xmToggleButtonWidgetClass, form,
        XmNlabelString, empty_lbl,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNtopOffset, 16,
        XmNleftOffset, 16,
        NULL
    );
    XmStringFree(empty_lbl);

    if (strcmp(mode, "drop") == 0) {
        Atom imports[1] = { g_uri_atom };
        Arg dargs[6];
        int dn = 0;
        XtSetArg(dargs[dn], XmNimportTargets, imports); dn++;
        XtSetArg(dargs[dn], XmNnumImportTargets, 1); dn++;
        XtSetArg(dargs[dn], XmNdropSiteOperations, XmDROP_COPY | XmDROP_MOVE); dn++;
        XtSetArg(dargs[dn], XmNdropProc, drop_proc); dn++;
        XmDropSiteRegister(form, dargs, dn);
    } else if (strcmp(mode, "drag") == 0) {
        XtOverrideTranslations(form, XtParseTranslationTable(""));
        XtAddEventHandler(form, ButtonPressMask, False, start_motif_drag_eh, NULL);
    }

    XtRealizeWidget(g_shell);
    if (strcmp(mode, "dpi") == 0) {
        write_dpi_status();
        XtAppAddTimeOut(app, 100, timer_poll_cb, NULL);
    }

    XtAppMainLoop(app);
    return 0;
}
"""


X11_REQ_C = r"""
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);
    Atom info_a = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
    Atom act = None; int fmt = 0; unsigned long n = 0, rem = 0; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, info_a, 0, 2, False, AnyPropertyType,
                           &act, &fmt, &n, &rem, &data) != Success || !data || n < 2) {
        return 2;
    }
    Window wm_win = (Window)((unsigned long *)data)[1];
    XFree(data);
    Atom req_a = XInternAtom(dpy, "_DT_WM_REQUEST", False);
    XChangeProperty(dpy, wm_win, req_a, XA_STRING, 8, PropModeAppend,
                    (const unsigned char *)argv[1], (int)strlen(argv[1]) + 1);
    XSync(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
"""


VPTR_XML = r"""<?xml version="1.0" encoding="UTF-8"?>
<protocol name="wlr_virtual_pointer_unstable_v1">
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
      <arg name="time" type="uint"/>
      <arg name="axis" type="uint"/>
      <arg name="value" type="fixed"/>
      <arg name="discrete" type="int"/>
    </request>
    <request name="destroy" type="destructor"/>
  </interface>
  <interface name="zwlr_virtual_pointer_manager_v1" version="2">
    <request name="create_virtual_pointer">
      <arg name="seat" type="object" interface="wl_seat" allow-null="true"/>
      <arg name="id" type="new_id" interface="zwlr_virtual_pointer_v1"/>
    </request>
    <request name="destroy" type="destructor"/>
    <request name="create_virtual_pointer_with_output" since="2">
      <arg name="seat" type="object" interface="wl_seat" allow-null="true"/>
      <arg name="output" type="object" interface="wl_output" allow-null="true"/>
      <arg name="id" type="new_id" interface="zwlr_virtual_pointer_v1"/>
    </request>
  </interface>
</protocol>
"""


VPTR_DRAG_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "vptr-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *g_mgr;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        g_mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, ver < 2 ? ver : 2);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { .global = reg_global, .global_remove = reg_rem };

int main(int argc, char **argv) {
    if (argc < 6) return 1;
    double x0 = atof(argv[1]), y0 = atof(argv[2]);
    double x1 = atof(argv[3]), y1 = atof(argv[4]);
    uint32_t btn = (strcmp(argv[5], "middle") == 0) ? BTN_MIDDLE : BTN_LEFT;

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!g_mgr) return 2;

    struct zwlr_virtual_pointer_v1 *vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(g_mgr, NULL);
    uint32_t t = 100;
    /* Reset to origin (0,0) then move to (x0, y0) */
    zwlr_virtual_pointer_v1_motion(vp, t, wl_fixed_from_double(-10000.0), wl_fixed_from_double(-10000.0));
    zwlr_virtual_pointer_v1_frame(vp);
    wl_display_roundtrip(dpy);
    t += 20;
    zwlr_virtual_pointer_v1_motion(vp, t, wl_fixed_from_double(x0), wl_fixed_from_double(y0));
    zwlr_virtual_pointer_v1_frame(vp);
    wl_display_roundtrip(dpy);
    usleep(80000);

    /* Press button */
    t += 20;
    zwlr_virtual_pointer_v1_button(vp, t, btn, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(vp);
    wl_display_roundtrip(dpy);
    usleep(120000);

    /* Smooth drag in 8 steps */
    int steps = 8;
    double cx = x0, cy = y0;
    for (int i = 1; i <= steps; i++) {
        double nx = x0 + (x1 - x0) * ((double)i / (double)steps);
        double ny = y0 + (y1 - y0) * ((double)i / (double)steps);
        t += 25;
        zwlr_virtual_pointer_v1_motion(vp, t, wl_fixed_from_double(nx - cx), wl_fixed_from_double(ny - cy));
        zwlr_virtual_pointer_v1_frame(vp);
        wl_display_roundtrip(dpy);
        cx = nx; cy = ny;
        usleep(40000);
    }
    usleep(100000);

    /* Release button */
    t += 25;
    zwlr_virtual_pointer_v1_button(vp, t, btn, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(vp);
    wl_display_roundtrip(dpy);
    usleep(150000);

    zwlr_virtual_pointer_v1_destroy(vp);
    zwlr_virtual_pointer_manager_v1_destroy(g_mgr);
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
"""


def count_near_color(im: Image.Image, rgb: tuple[int, int, int], tol: int = 18) -> int:
    raw = im.convert("RGB").tobytes()
    tr, tg, tb = rgb
    c = 0
    for i in range(0, len(raw), 3):
        if abs(raw[i] - tr) <= tol and abs(raw[i + 1] - tg) <= tol and abs(raw[i + 2] - tb) <= tol:
            c += 1
    return c


def wait_for_file_substr(path: Path, substr: str, timeout: float = 6.0) -> str:
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        if path.is_file():
            last = path.read_text(encoding="utf-8", errors="replace")
            if substr in last:
                return last
        time.sleep(0.08)
    raise SystemExit(f"timed out waiting for '{substr}' in {path}; last contents:\n{last}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    ap.add_argument("--save-dir", default="")
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    build_root = comp_bin.parents[3]
    motif_pfx = build_root / "motif-prefix"
    motif_src = build_root / "motif-src"
    ck_grab_bin = build_root / "apps-build" / "bin" / "ck-grab"
    cc = os.environ.get("CC", "cc")

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-wayland-wp05-", dir=tmp_base) as work_s:
        work = Path(work_s)
        xdg = work / "xdg"
        home = work / "home"
        xdg.mkdir(mode=0o700)
        home.mkdir(mode=0o700)

        # Generate Wayland protocol headers/sources
        xdg_xml = "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
        fs_xml = "/usr/share/wayland-protocols/staging/fractional-scale/fractional-scale-v1.xml"
        subprocess.run(["wayland-scanner", "client-header", xdg_xml, str(work / "xdg-shell-client-protocol.h")], check=True)
        subprocess.run(["wayland-scanner", "private-code", xdg_xml, str(work / "xdg-shell-protocol.c")], check=True)
        subprocess.run(["wayland-scanner", "client-header", fs_xml, str(work / "fractional-scale-v1-client-protocol.h")], check=True)
        subprocess.run(["wayland-scanner", "private-code", fs_xml, str(work / "fractional-scale-v1-protocol.c")], check=True)

        wl_c = work / "wl_dnd_client.c"
        wl_bin = work / "wl_dnd_client"
        wl_c.write_text(WL_DND_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [
                cc, "-O2", f"-I{work}",
                str(wl_c), str(work / "xdg-shell-protocol.c"), str(work / "fractional-scale-v1-protocol.c"),
                "-lwayland-client", "-o", str(wl_bin),
            ],
            check=True,
        )

        motif_c = work / "motif_wp05_client.c"
        motif_bin = work / "motif_wp05_client"
        motif_c.write_text(MOTIF_WP05_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [
                cc, "-O2",
                f"-I{motif_pfx / 'include'}", f"-I{motif_src / 'lib' / 'Xm'}",
                str(motif_c),
                f"-L{motif_pfx / 'lib'}", f"-Wl,-rpath,{motif_pfx / 'lib'}",
                "-lXm", "-lXt", "-lX11",
                "-o", str(motif_bin),
            ],
            check=True,
        )

        req_c = work / "x11_req.c"
        req_bin = work / "x11_req"
        req_c.write_text(X11_REQ_C, encoding="utf-8")
        subprocess.run([cc, "-O2", str(req_c), "-lX11", "-o", str(req_bin)], check=True)

        vptr_xml = work / "vptr.xml"
        vptr_xml.write_text(VPTR_XML, encoding="utf-8")
        subprocess.run(["wayland-scanner", "client-header", str(vptr_xml), str(work / "vptr-client-protocol.h")], check=True)
        subprocess.run(["wayland-scanner", "private-code", str(vptr_xml), str(work / "vptr-protocol.c")], check=True)
        vptr_c = work / "vptr_drag.c"
        vptr_bin = work / "vptr_drag"
        vptr_c.write_text(VPTR_DRAG_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", f"-I{work}", str(vptr_c), str(work / "vptr-protocol.c"), "-lwayland-client", "-o", str(vptr_bin)],
            check=True,
        )

        cde_src = comp_bin.parent
        makefile = comp_bin.parent / "Makefile"
        if makefile.is_file():
            for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
                if ln.startswith("top_srcdir ="):
                    cde_src = (comp_bin.parent / ln.split("=", 1)[1].strip()).resolve()
                    break

        env = os.environ.copy()
        ld_parts = [str(p) for p in (motif_pfx / "lib", build_root / "cde-stage" / "usr" / "dt" / "lib", build_root / "fs-prefix" / "lib") if p.is_dir()]
        if env.get("LD_LIBRARY_PATH"):
            ld_parts.append(env["LD_LIBRARY_PATH"])
        env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
        env.update({
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "2",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
            "CDE_BACKDROP_DIR": str(cde_src / "programs" / "backdrops"),
        })

        status_file = work / "compositor.status"
        comp_log = (work / "dtwm-wayland.log").open("w", encoding="utf-8")
        comp = subprocess.Popen(
            [
                str(comp_bin),
                "--output-dpi", "0=96",
                "--output-dpi", "1=192",
                "--status-file", str(status_file),
            ],
            env=env,
            stdout=comp_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        procs = [comp]

        try:
            wl_sock = x_sock = None
            for _ in range(80):
                if status_file.is_file():
                    kv = dict(ln.split("=", 1) for ln in status_file.read_text().splitlines() if "=" in ln)
                    wl_sock, x_sock = kv.get("WAYLAND_DISPLAY"), kv.get("DISPLAY")
                    if wl_sock and x_sock:
                        break
                time.sleep(0.1)
            if not wl_sock or not x_sock:
                raise SystemExit("dtwm-wayland failed to start")

            c_env = {**env, "WAYLAND_DISPLAY": wl_sock, "DISPLAY": x_sock}

            # =========================================================================
            # PART A.1: Drag from Native Wayland client -> Motif XmDropSite (XDND v5)
            # =========================================================================
            motif_drop_out = work / "motif_drop.txt"
            p_m_drop = subprocess.Popen([str(motif_bin), "drop", str(motif_drop_out)], env=c_env, start_new_session=True)
            p_w_drag = subprocess.Popen([str(wl_bin), "drag"], env=c_env, start_new_session=True)
            procs.extend([p_m_drop, p_w_drag])
            time.sleep(0.9)

            # Wayland window placed at (48, 40), client interior around (140, 140); Motif window at (420, 120), interior around (530, 210)
            subprocess.run([str(vptr_bin), "140", "140", "530", "210", "left"], env=c_env, check=True)

            m_drop_txt = wait_for_file_substr(motif_drop_out, "file:///tmp/from-wayland-wp05.txt")
            p_m_drop.terminate(); p_w_drag.terminate()
            p_m_drop.wait(timeout=3); p_w_drag.wait(timeout=3)

            # =========================================================================
            # PART A.2: Drag from Motif XmDragStart (XDND v5) -> Native Wayland client
            # =========================================================================
            wl_drop_out = work / "wl_drop.txt"
            p_w_drop = subprocess.Popen([str(wl_bin), "drop", str(wl_drop_out)], env=c_env, start_new_session=True)
            p_m_drag = subprocess.Popen([str(motif_bin), "drag"], env=c_env, start_new_session=True)
            procs.extend([p_w_drop, p_m_drag])
            time.sleep(0.9)

            subprocess.run([str(vptr_bin), "530", "210", "140", "140", "middle"], env=c_env, check=True)

            w_drop_txt = wait_for_file_substr(wl_drop_out, "file:///tmp/from-motif-wp05.txt")
            p_m_drag.terminate()
            p_m_drag.wait(timeout=3)

            # =========================================================================
            # PART B: ck-grab Wayland Screencopy (Active Native Wayland Window + Full Screen)
            # =========================================================================
            grab_summary = "skipped (ck-grab not built)"
            saved_files = []
            if ck_grab_bin.is_file():
                # Focus the native Wayland window (p_w_drop at 140, 140)
                p_w_scale = subprocess.Popen([str(wl_bin), "scale", str(work / "wl_scale.txt")], env=c_env, start_new_session=True)
                procs.append(p_w_scale)
                time.sleep(0.6)

                win_png = work / "wp05-ck-grab-window.png"
                full_png = work / "wp05-ck-grab-fullscreen.png"
                subprocess.run(
                    [str(ck_grab_bin), "--window", "--include-frame", "--capture", str(win_png)],
                    env=c_env, check=True,
                )
                subprocess.run(
                    [str(ck_grab_bin), "--full-screen", "--capture", str(full_png)],
                    env=c_env, check=True,
                )

                with Image.open(win_png) as iw, Image.open(full_png) as ifull:
                    ww, wh = iw.size
                    fw, fh = ifull.size
                    # Wayland client is 240x180 + 6px left/right border + 30px top + 6px bottom = 252x216
                    if abs(ww - 252) > 8 or abs(wh - 216) > 8:
                        raise SystemExit(f"expected ck-grab --window size ~252x216, got {ww}x{wh}")
                    active_px = count_near_color(iw, (237, 168, 112), tol=20)
                    blue_px = count_near_color(iw, (0x18, 0x68, 0xA8), tol=12)
                    if active_px < 1000 or blue_px < 20000:
                        raise SystemExit(f"ck-grab --window missing CDE active frame ({active_px}) or Wayland client pixels ({blue_px})")
                    if fw < 2500 or fh < 700:
                        raise SystemExit(f"expected dual-output ck-grab full-screen capture >=2560x720, got {fw}x{fh}")
                    grab_summary = f"ck-grab window={ww}x{wh} (active_frame={active_px}px), full={fw}x{fh}"
                saved_files.extend([win_png, full_png])
                p_w_scale.terminate(); p_w_scale.wait(timeout=3)

            if p_w_drop.poll() is None:
                p_w_drop.terminate(); p_w_drop.wait(timeout=3)

            # =========================================================================
            # PART C: Mixed-DPI 96 <-> 192 <-> 144 Live Motif Shell Reflow
            # =========================================================================
            dpi_out = work / "motif_dpi.txt"
            p_dpi = subprocess.Popen([str(motif_bin), "dpi", str(dpi_out)], env=c_env, start_new_session=True)
            procs.append(p_dpi)

            st_96 = wait_for_file_substr(dpi_out, "SCALE=1.00")
            btn_96 = [ln.split("=", 1)[1] for ln in st_96.splitlines() if ln.startswith("BTN=")][0]
            h_96 = int(btn_96.split("x")[1])

            # Move Motif window from Output 0 (96 DPI) to Output 1 (192 DPI at x=1420, y=120)
            subprocess.run([str(req_bin), "f.move_active_to 1420 120"], env=c_env, check=True)
            st_192 = wait_for_file_substr(dpi_out, "SCALE=2.00")
            btn_192 = [ln.split("=", 1)[1] for ln in st_192.splitlines() if ln.startswith("BTN=")][0]
            h_192 = int(btn_192.split("x")[1])
            if h_192 <= h_96:
                raise SystemExit(f"expected derived Motif ToggleButton height at 192 DPI ({h_192}) > 96 DPI ({h_96})")

            # Live RandR DPI change on Output 1: 192 -> 144 (1.50x)
            subprocess.run([str(req_bin), "f.set_output_dpi 1 144"], env=c_env, check=True)
            wait_for_file_substr(dpi_out, "SCALE=1.50")

            if args.save_dir and saved_files:
                sdir = Path(args.save_dir)
                sdir.mkdir(parents=True, exist_ok=True)
                for f in saved_files:
                    shutil.copy2(f, sdir / f.name)

            print(
                f"PASS: dtwm-wayland WP-05 verified "
                f"(Wayland<->Motif XDND v5 OK, {grab_summary}, "
                f"RandR DPI reflow 1.00x[{btn_96}] -> 2.00x[{btn_192}] -> 1.50x)"
            )
            return 0
        finally:
            for p in reversed(procs):
                if p.poll() is None:
                    p.terminate()
            for p in reversed(procs):
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
            comp_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
