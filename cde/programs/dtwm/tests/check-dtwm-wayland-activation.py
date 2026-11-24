#!/usr/bin/env python3
"""Automated integration test for Wayland Focus-Stealing Prevention & Startup Tokens
(`xdg-activation-v1`) across `dtwm-wayland`, `dtwm-panel`, and `libXm` (`--no-xwayland`):

1. Verifies `dtwm-wayland` advertises `xdg_activation_v1` and runs cleanly in
   `--no-xwayland` mode with `dtwm-panel` mapped against `libXm.so.9`.
2. Mints a startup token via `XmWaylandRequestActivationToken`, verifying
   `_DT_PANEL_BUSY_START_SEQ` is broadcast over native Wayland IPC, then spawns
   a Motif client with `XDG_ACTIVATION_TOKEN` and verifies the child consumes the
   token, becomes the active window, and broadcasts `_DT_PANEL_BUSY_STOP_SEQ`.
3. Injects live typing via `zwp_virtual_keyboard_v1` into the focused Motif client
   and maps an unauthenticated background Wayland window without `XDG_ACTIVATION_TOKEN`,
   verifying focus-stealing prevention keeps keyboard focus and top Z-order on the
   actively typed-in window and rejects unauthenticated self-activation lacking a
   seat serial.
4. Switches to Workspace 2 (`ws1`) and redeems a valid activation token for the
   window on Workspace 1 (`ws0`), verifying `dtwm-wayland` switches workspaces,
   raises, and activates the target window.
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


MOTIF_CLIENT_C = r"""
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/Xatom.h>
#include <Xm/DrawingA.h>
#include <Xm/Wayland.h>
#include <Xm/Xm.h>

static int g_has_focus = 0;

static void focus_eh(Widget w, XtPointer cd, XEvent *ev, Boolean *cont) {
    (void)w; (void)cd; (void)cont;
    if (ev->type == FocusIn) g_has_focus = 1;
    else if (ev->type == FocusOut) g_has_focus = 0;
}

static void write_state(const char *path, int focused, const char *tok) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "focused=%d\ntoken=%s\n", focused, tok ? tok : "");
    fclose(f);
    rename(tmp, path);
}

static void tick_cb(XtPointer cd, XtIntervalId *id) {
    (void)id;
    int *done = (int *)cd;
    *done = 1;
}

static void pump_ms(XtAppContext app, Display *dpy, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long end_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L + ms;
    for (;;) {
        while (XtAppPending(app)) {
            XtAppProcessEvent(app, XtIMAll);
        }
        XFlush(dpy);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L >= end_ms) break;
        int done = 0;
        XtAppAddTimeOut(app, 10, tick_cb, &done);
        while (!done) XtAppProcessEvent(app, XtIMAll);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    const char *app_name = argv[1];
    const char *state_file = argv[2];
    const char *cmd_dir = argv[3];

    XtSetLanguageProc(NULL, NULL, NULL);
    XtToolkitInitialize();
    XtAppContext app = XtCreateApplicationContext();
    int dargc = 1;
    char *dargv[] = { (char *)app_name, NULL };
    Display *dpy = XtOpenDisplay(app, NULL, app_name, app_name, NULL, 0, &dargc, dargv);
    if (!dpy || !XmWaylandIsActive(dpy)) {
        fprintf(stderr, "native Motif Wayland backend inactive for %s\n", app_name);
        return 3;
    }

    Widget shell = XtVaAppCreateShell(
        app_name, app_name, applicationShellWidgetClass, dpy,
        XmNtitle, app_name, XmNwidth, 320, XmNheight, 200, NULL
    );
    XtAddEventHandler(shell, FocusChangeMask, False, focus_eh, NULL);
    Widget da = XtVaCreateManagedWidget(
        "canvas", xmDrawingAreaWidgetClass, shell,
        XmNwidth, 320, XmNheight, 200, NULL
    );
    (void)da;
    XtRealizeWidget(shell);
    if (getenv("XDG_ACTIVATION_TOKEN") != NULL) {
        fprintf(stderr, "XDG_ACTIVATION_TOKEN leaked past XtRealizeWidget\n");
        return 4;
    }

    char last_tok[256] = "";
    char mint_req[512], redeem_req[512], quit_req[512];
    snprintf(mint_req, sizeof(mint_req), "%s/%s.mint", cmd_dir, app_name);
    snprintf(redeem_req, sizeof(redeem_req), "%s/%s.redeem", cmd_dir, app_name);
    snprintf(quit_req, sizeof(quit_req), "%s/%s.quit", cmd_dir, app_name);

    for (int iter = 0; iter < 600; ++iter) {
        pump_ms(app, dpy, 25);
        if (access(mint_req, F_OK) == 0) {
            unlink(mint_req);
            char *tok = NULL;
            if (XmWaylandRequestActivationToken(dpy, XtWindow(shell), "TargetApp", &tok) && tok) {
                snprintf(last_tok, sizeof(last_tok), "%s", tok);
                XtFree(tok);
            }
        }
        if (access(redeem_req, F_OK) == 0) {
            FILE *rf = fopen(redeem_req, "r");
            char rtok[256] = "";
            if (rf) {
                if (fgets(rtok, sizeof(rtok), rf)) {
                    rtok[strcspn(rtok, "\r\n")] = '\0';
                }
                fclose(rf);
            }
            unlink(redeem_req);
            if (rtok[0]) setenv("XDG_ACTIVATION_TOKEN", rtok, 1);
            Atom net_act = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
            XEvent xev;
            memset(&xev, 0, sizeof(xev));
            xev.xclient.type = ClientMessage;
            xev.xclient.window = XtWindow(shell);
            xev.xclient.message_type = net_act;
            xev.xclient.format = 32;
            xev.xclient.data.l[0] = 2;
            XSendEvent(dpy, DefaultRootWindow(dpy), False,
                       SubstructureRedirectMask | SubstructureNotifyMask, &xev);
            XFlush(dpy);
        }
        write_state(state_file, g_has_focus, last_tok);
        if (access(quit_req, F_OK) == 0) {
            unlink(quit_req);
            break;
        }
    }

    XtDestroyWidget(shell);
    XtCloseDisplay(dpy);
    XtDestroyApplicationContext(app);
    return 0;
}
"""


WL_PROBE_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client.h"
#include "xdg-activation-client.h"

/* Minimal zwp_virtual_keyboard_v1 wire definition */
extern const struct wl_interface zwp_virtual_keyboard_v1_interface;
static const struct wl_interface *vkb_mgr_types[] = {
    &wl_seat_interface, &zwp_virtual_keyboard_v1_interface,
};
static const struct wl_message vkb_mgr_requests[] = {
    { "create_virtual_keyboard", "on", vkb_mgr_types },
};
static const struct wl_interface zwp_virtual_keyboard_manager_v1_interface = {
    "zwp_virtual_keyboard_manager_v1", 1, 1, vkb_mgr_requests, 0, NULL,
};
static const struct wl_message vkb_requests[] = {
    { "keymap", "uhu", NULL },
    { "key", "uuu", NULL },
    { "modifiers", "uuuu", NULL },
    { "destroy", "", NULL },
};
const struct wl_interface zwp_virtual_keyboard_v1_interface = {
    "zwp_virtual_keyboard_v1", 1, 4, vkb_requests, 0, NULL,
};

typedef struct {
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wm;
    struct xdg_activation_v1 *act;
    struct wl_proxy *vkb_mgr;
    bool configured;
    bool activated;
    char minted_tok[256];
} Probe;

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) {
    (void)d;
    xdg_wm_base_pong(w, s);
}
static const struct xdg_wm_base_listener g_wm_l = { .ping = wm_ping };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)ver;
    Probe *p = d;
    if (!strcmp(iface, "wl_compositor"))
        p->comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm"))
        p->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_seat"))
        p->seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) {
        p->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
        xdg_wm_base_add_listener(p->wm, &g_wm_l, p);
    } else if (!strcmp(iface, "xdg_activation_v1"))
        p->act = wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
    else if (!strcmp(iface, "zwp_virtual_keyboard_manager_v1"))
        p->vkb_mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

static void xsurf_cfg(void *d, struct xdg_surface *xs, uint32_t ser) {
    Probe *p = d;
    xdg_surface_ack_configure(xs, ser);
    p->configured = true;
}
static const struct xdg_surface_listener g_xs_l = { .configure = xsurf_cfg };

static void xtop_cfg(void *d, struct xdg_toplevel *xt, int32_t w, int32_t h, struct wl_array *states) {
    (void)xt; (void)w; (void)h;
    Probe *p = d;
    p->activated = false;
    int32_t *st;
    wl_array_for_each(st, states) {
        if (*st == XDG_TOPLEVEL_STATE_ACTIVATED) p->activated = true;
    }
}
static void xtop_close(void *d, struct xdg_toplevel *xt) { (void)d; (void)xt; }
static const struct xdg_toplevel_listener g_xt_l = { .configure = xtop_cfg, .close = xtop_close };

static void tok_done(void *d, struct xdg_activation_token_v1 *t, const char *tok) {
    (void)t;
    Probe *p = d;
    if (tok) snprintf(p->minted_tok, sizeof(p->minted_tok), "%s", tok);
}
static const struct xdg_activation_token_v1_listener g_tok_l = { .done = tok_done };

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *mode = argv[1];

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 3;
    Probe p = {0};
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &g_reg_l, &p);
    wl_display_roundtrip(dpy);

    if (!strcmp(mode, "check-global")) {
        int rc = p.act ? 0 : 1;
        wl_display_disconnect(dpy);
        return rc;
    }

    if (!strcmp(mode, "type-key")) {
        if (!p.vkb_mgr || !p.seat) return 4;
        struct wl_proxy *vkb = wl_proxy_marshal_flags(
            p.vkb_mgr, 0, &zwp_virtual_keyboard_v1_interface, 1, 0, p.seat, NULL
        );
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        char *kms = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
        size_t sz = strlen(kms) + 1;
        int fd = memfd_create("vkb-km", MFD_CLOEXEC);
        if (ftruncate(fd, (off_t)sz) != 0) return 5;
        void *map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        memcpy(map, kms, sz);
        munmap(map, sz);
        free(kms);
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);

        wl_proxy_marshal_flags(vkb, 0, NULL, 1, 0, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)sz);
        close(fd);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, 100, 30 /* KEY_A */, 1);
        wl_proxy_marshal_flags(vkb, 1, NULL, 1, 0, 120, 30 /* KEY_A */, 0);
        wl_display_roundtrip(dpy);
        wl_proxy_marshal_flags(vkb, 3, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
        wl_display_disconnect(dpy);
        return 0;
    }

    if (!strcmp(mode, " rogue-steal") || !strcmp(mode, "rogue-steal")) {
        if (!p.comp || !p.shm || !p.wm || !p.act || argc < 4) return 4;
        const char *state_path = argv[2];
        const char *quit_path = argv[3];

        struct wl_surface *surf = wl_compositor_create_surface(p.comp);
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(p.wm, surf);
        xdg_surface_add_listener(xs, &g_xs_l, &p);
        struct xdg_toplevel *xt = xdg_surface_get_toplevel(xs);
        xdg_toplevel_add_listener(xt, &g_xt_l, &p);
        xdg_toplevel_set_title(xt, "RogueBackground");
        xdg_toplevel_set_app_id(xt, "rogue.bg");
        wl_surface_commit(surf);
        while (!p.configured) wl_display_dispatch(dpy);

        int stride = 200 * 4, size = stride * 140;
        int sfd = memfd_create("rogue-shm", MFD_CLOEXEC);
        if (ftruncate(sfd, size) != 0) return 5;
        uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
        for (int i = 0; i < 200 * 140; ++i) px[i] = 0xffaa2222u;
        struct wl_shm_pool *pool = wl_shm_create_pool(p.shm, sfd, size);
        struct wl_buffer *buf = wl_shm_pool_create_buffer(
            pool, 0, 200, 140, stride, WL_SHM_FORMAT_XRGB8888
        );
        close(sfd);
        wl_surface_attach(surf, buf, 0, 0);
        wl_surface_damage(surf, 0, 0, 200, 140);
        wl_surface_commit(surf);
        wl_display_roundtrip(dpy);
        usleep(120000);
        wl_display_roundtrip(dpy);

        bool mapped_unfocused = !p.activated;

        /* Now attempt unauthenticated self-activation with surface=surf and seat=NULL */
        struct xdg_activation_token_v1 *tok = xdg_activation_v1_get_activation_token(p.act);
        xdg_activation_token_v1_add_listener(tok, &g_tok_l, &p);
        xdg_activation_token_v1_set_surface(tok, surf);
        xdg_activation_token_v1_commit(tok);
        wl_display_roundtrip(dpy);
        if (p.minted_tok[0]) {
            xdg_activation_v1_activate(p.act, p.minted_tok, surf);
            wl_display_roundtrip(dpy);
            usleep(120000);
            wl_display_roundtrip(dpy);
        }
        bool self_steal_blocked = !p.activated;

        FILE *f = fopen(state_path, "w");
        if (f) {
            fprintf(f, "mapped_unfocused=%d\nself_steal_blocked=%d\n",
                    mapped_unfocused ? 1 : 0, self_steal_blocked ? 1 : 0);
            fclose(f);
        }
        while (access(quit_path, F_OK) != 0) {
            wl_display_dispatch_pending(dpy);
            wl_display_flush(dpy);
            usleep(20000);
        }
        munmap(px, (size_t)size);
        wl_display_disconnect(dpy);
        return 0;
    }

    wl_display_disconnect(dpy);
    return 1;
}
"""


def find_cde_build_and_src(comp_bin: Path) -> tuple[Path, Path]:
    cde_build = comp_bin.parent.parent.parent
    makefile = comp_bin.parent / "Makefile"
    cde_src = cde_build
    if makefile.is_file():
        for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
            if ln.startswith("top_srcdir ="):
                rel = ln.split("=", 1)[1].strip()
                cde_src = (comp_bin.parent / rel).resolve()
                break
    return cde_build, cde_src


def extract_motif_dirs(makefile: Path) -> tuple[list[str], list[str]]:
    inc: list[str] = []
    lib: list[str] = []
    if not makefile.is_file():
        return inc, lib
    for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
        if ln.startswith(("CFLAGS =", "X_CFLAGS =", "DTCLIENTLIBS =", "CPPFLAGS =", "LDFLAGS =")):
            for tok in ln.split("=", 1)[1].split():
                if tok.startswith("-I") and "$(" not in tok:
                    inc.append(tok)
                elif (tok.startswith("-L") or tok.startswith("-Wl,-rpath")) and "$(" not in tok:
                    lib.append(tok)
    return inc, lib


def parse_kv(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    out: dict[str, str] = {}
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def wait_until(pred, timeout_s: float = 8.0, desc: str = "condition") -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if pred():
            return
        time.sleep(0.04)
    raise SystemExit(f"timed out waiting for {desc}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    cde_build, cde_src = find_cde_build_and_src(comp_bin)
    inc_flags, lib_flags = extract_motif_dirs(comp_bin.parent / "Makefile")
    cc = os.environ.get("CC", "cc")
    scanner = shutil.which("wayland-scanner")
    if not scanner:
        raise SystemExit("wayland-scanner not found")

    wp_dir = Path(
        subprocess.check_output(
            ["pkg-config", "--variable=pkgdatadir", "wayland-protocols"], text=True
        ).strip()
    )
    wlc_cf = subprocess.check_output(["pkg-config", "--cflags", "wayland-client", "xkbcommon"], text=True).split()
    wlc_lf = subprocess.check_output(["pkg-config", "--libs", "wayland-client", "xkbcommon"], text=True).split()

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-act-", dir=tmp_base) as work_s:
        work = Path(work_s)
        xdg = work / "rt"
        home = work / "home"
        cmds = work / "cmds"
        proto = work / "proto"
        for d in (xdg, home, cmds, proto):
            d.mkdir(mode=0o700)

        subprocess.run([scanner, "client-header", str(wp_dir / "stable/xdg-shell/xdg-shell.xml"), str(proto / "xdg-shell-client.h")], check=True)
        subprocess.run([scanner, "private-code", str(wp_dir / "stable/xdg-shell/xdg-shell.xml"), str(proto / "xdg-shell-code.c")], check=True)
        subprocess.run([scanner, "client-header", str(wp_dir / "staging/xdg-activation/xdg-activation-v1.xml"), str(proto / "xdg-activation-client.h")], check=True)
        subprocess.run([scanner, "private-code", str(wp_dir / "staging/xdg-activation/xdg-activation-v1.xml"), str(proto / "xdg-activation-code.c")], check=True)

        motif_c = work / "motif_client.c"
        motif_bin = work / "motif_client"
        motif_c.write_text(MOTIF_CLIENT_C, encoding="utf-8")
        subprocess.run(
            [cc, "-O2", "-Wall", "-Wextra", str(motif_c), *inc_flags, *lib_flags, "-lXm", "-lXt", "-lX11", "-o", str(motif_bin)],
            check=True,
        )

        probe_c = work / "wl_probe.c"
        probe_bin = work / "wl_probe"
        probe_c.write_text(WL_PROBE_C, encoding="utf-8")
        subprocess.run(
            [
                cc, "-O2", "-Wall", "-Wextra", f"-I{proto}", *wlc_cf,
                str(probe_c), str(proto / "xdg-shell-code.c"), str(proto / "xdg-activation-code.c"),
                *wlc_lf, "-o", str(probe_bin),
            ],
            check=True,
        )

        env = os.environ.copy()
        for k in ("WAYLAND_DISPLAY", "DISPLAY", "TT_SESSION", "DTUSERSESSION", "XDG_ACTIVATION_TOKEN"):
            env.pop(k, None)
        env.update({
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg),
            "TMPDIR": str(work),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
            "CDE_BACKDROP_DIR": str(cde_src / "programs" / "backdrops"),
        })

        status_file = work / "comp.status"
        comp_log = (work / "comp.log").open("w", encoding="utf-8")
        comp = subprocess.Popen(
            [str(comp_bin), "--no-xwayland", "--status-file", str(status_file)],
            env=env,
            stdout=comp_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        procs: list[subprocess.Popen] = []
        ipc_sock: socket.socket | None = None

        try:
            wait_until(lambda: "WAYLAND_DISPLAY" in parse_kv(status_file), 10.0, "dtwm-wayland status")
            wl_sock = parse_kv(status_file)["WAYLAND_DISPLAY"]
            c_env = {**env, "WAYLAND_DISPLAY": wl_sock, "CK_MOTIF_BACKEND": "wayland"}

            # 1. Verify xdg_activation_v1 global is advertised
            subprocess.run([str(probe_bin), "check-global"], env=c_env, check=True, timeout=5)

            # Connect to native Wayland IPC socket to monitor _DT_PANEL_BUSY_*_SEQ & workspace
            ipc_path = xdg / f"dtwm-wayland-{wl_sock}.ipc"
            wait_until(ipc_path.exists, 5.0, "native IPC socket")
            ipc_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            ipc_sock.connect(str(ipc_path))
            ipc_sock.setblocking(False)
            ipc_buf = ""

            def drain_ipc() -> str:
                nonlocal ipc_buf
                while True:
                    try:
                        chunk = ipc_sock.recv(65536)
                        if not chunk:
                            break
                        ipc_buf += chunk.decode("utf-8", errors="replace")
                    except BlockingIOError:
                        break
                return ipc_buf

            # 2. Launch AppA (e.g. dtmail / dtfile role)
            state_a = work / "appA.state"
            proc_a = subprocess.Popen([str(motif_bin), "AppA", str(state_a), str(cmds)], env=c_env)
            procs.append(proc_a)
            wait_until(lambda: parse_kv(state_a).get("focused") == "1", 8.0, "AppA initial focus")
            drain_ipc()
            ipc_snapshot_len = len(ipc_buf)

            # Mint token from AppA -> verify _DT_PANEL_BUSY_START_SEQ is broadcast over IPC!
            (cmds / "AppA.mint").write_text("1\n", encoding="utf-8")
            wait_until(lambda: bool(parse_kv(state_a).get("token")), 5.0, "AppA minted token")
            minted_tok = parse_kv(state_a)["token"]
            wait_until(
                lambda: "PROP _DT_PANEL_BUSY_START_SEQ " in drain_ipc()[ipc_snapshot_len:],
                5.0,
                "_DT_PANEL_BUSY_START_SEQ IPC broadcast on token mint",
            )

            # Spawn AppB with XDG_ACTIVATION_TOKEN -> verify AppB activates & stops Busy light!
            ipc_before_b = len(ipc_buf)
            state_b = work / "appB.state"
            env_b = {**c_env, "XDG_ACTIVATION_TOKEN": minted_tok}
            proc_b = subprocess.Popen([str(motif_bin), "AppB", str(state_b), str(cmds)], env=env_b)
            procs.append(proc_b)
            wait_until(lambda: parse_kv(state_b).get("focused") == "1", 8.0, "AppB activated via startup token")
            wait_until(lambda: parse_kv(state_a).get("focused") == "0", 5.0, "AppA unfocused after AppB activation")
            wait_until(
                lambda: "PROP _DT_PANEL_BUSY_STOP_SEQ " in drain_ipc()[ipc_before_b:],
                5.0,
                "_DT_PANEL_BUSY_STOP_SEQ IPC broadcast on token consumption",
            )

            # 3. Inject live typing into AppB, then spawn rogue background client WITHOUT token!
            subprocess.run([str(probe_bin), "type-key"], env=c_env, check=True, timeout=5)
            rogue_state = work / "rogue.state"
            rogue_quit = work / "rogue.quit"
            proc_rogue = subprocess.Popen(
                [str(probe_bin), "rogue-steal", str(rogue_state), str(rogue_quit)], env=c_env
            )
            procs.append(proc_rogue)
            wait_until(rogue_state.is_file, 6.0, "rogue background probe completion")
            rkv = parse_kv(rogue_state)
            if rkv.get("mapped_unfocused") != "1" or rkv.get("self_steal_blocked") != "1":
                raise SystemExit(f"focus-stealing prevention failed: {rkv}")
            if parse_kv(state_b).get("focused") != "1":
                raise SystemExit("actively typed-in AppB lost focus to unauthenticated rogue window")
            rogue_quit.write_text("1\n", encoding="utf-8")

            # 4. Switch to Workspace 2 (ws1), open focused AppC (e.g. dtmail on ws1),
            #    mint a valid token from AppC, and redeem it in AppA (ck-browser on ws0)!
            cmd_hex = b"f.goto_workspace ws1\x00".hex()
            ipc_sock.setblocking(True)
            ipc_sock.sendall(f"SET _DT_WM_REQUEST {cmd_hex}\n".encode("ascii"))
            ipc_sock.setblocking(False)
            wait_until(
                lambda: f"PROP _DT_WORKSPACE_CURRENT_NAME {b'ws1'.hex()}" in drain_ipc(),
                5.0,
                "switch to ws1",
            )

            state_c = work / "appC.state"
            proc_c = subprocess.Popen([str(motif_bin), "AppC", str(state_c), str(cmds)], env=c_env)
            procs.append(proc_c)
            wait_until(lambda: parse_kv(state_c).get("focused") == "1", 8.0, "AppC focused on ws1")
            subprocess.run([str(probe_bin), "type-key"], env=c_env, check=True, timeout=5)

            (cmds / "AppC.mint").write_text("1\n", encoding="utf-8")
            wait_until(lambda: bool(parse_kv(state_c).get("token")), 5.0, "AppC cross-workspace token mint")
            ws_tok = parse_kv(state_c)["token"]
            ipc_before_ws_back = len(ipc_buf)
            (cmds / "AppA.redeem").write_text(ws_tok + "\n", encoding="utf-8")

            wait_until(
                lambda: f"PROP _DT_WORKSPACE_CURRENT_NAME {b'ws0'.hex()}" in drain_ipc()[ipc_before_ws_back:],
                6.0,
                "automatic switch back to ws0 on xdg_activation_v1.activate",
            )
            wait_until(lambda: parse_kv(state_a).get("focused") == "1", 5.0, "AppA refocused on ws0")

            for nm in ("AppA", "AppB", "AppC"):
                (cmds / f"{nm}.quit").write_text("1\n", encoding="utf-8")
            print(
                "PASS: check-dtwm-wayland-activation "
                "(xdg_activation_v1 global, Front Panel Busy IPC, focus-stealing prevention, cross-workspace activate)"
            )
            return 0
        finally:
            if ipc_sock is not None:
                ipc_sock.close()
            for p in procs:
                if p.poll() is None:
                    p.terminate()
                    try:
                        p.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        p.kill()
            if comp.poll() is None:
                comp.terminate()
                try:
                    comp.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    comp.kill()
            comp_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
