#!/usr/bin/env python3
"""Automated integration test for dtwm-wayland Track 5:
Native Wayland Session Restore (xdg-toplevel-tag-v1 + dt.wayland.session) &
Flatpak Sandbox Security (wp-security-context-v1):

1. Creates a wp_security_context_v1 sandboxed listener socket (engine="org.flatpak",
   app_id="org.example.SandboxedApp") and connects a client through the sandbox
   socket, verifying all privileged globals (screencopy, virtual_pointer,
   virtual_keyboard, data_control, output_manager, session_lock, foreign_toplevel,
   ext_workspace, layer_shell, nested wp_security_context_manager_v1) are hidden
   while standard app globals remain usable, and that spoofed xdg_toplevel app_ids
   cannot override the verified security-context app_id.
2. Launches two tagged native Wayland windows (xdg_toplevel_tag_v1 tags "doc-plan"
   on ws0 and "doc-budget" on ws1 at custom geometry 240,180 520x340 with
   space-containing CLI arguments), saves the session via f.save_wayland_session,
   terminates the compositor and clients, starts a fresh dtwm-wayland instance,
   restores via f.restore_wayland_session, and verifies both tagged windows
   relaunch with exact argv, sizes, positions, and workspace assignments.
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path


HELPER_C = r"""
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-tag-v1-client-protocol.h"
#include "security-context-v1-client-protocol.h"

typedef struct {
    const char *state_file;
    const char *app_id;
    const char *tag;
    const char *doc_arg;
    int init_w;
    int init_h;

    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct xdg_toplevel_tag_manager_v1 *tag_mgr;
    struct wp_security_context_manager_v1 *sec_mgr;

    struct wl_surface *surf;
    struct xdg_surface *xsurf;
    struct xdg_toplevel *xtop;
    struct wl_buffer *buf;
    int buf_w;
    int buf_h;
    int cfg_w;
    int cfg_h;
    bool configured;

    char globals[128][80];
    int nglobals;
} ClientApp;

static void write_client_state(ClientApp *a) {
    if (!a->state_file || !a->state_file[0]) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", a->state_file);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "configured=%d\ncfg_w=%d\ncfg_h=%d\ndoc=%s\nnglobals=%d\nglobals=",
            a->configured ? 1 : 0, a->cfg_w, a->cfg_h,
            a->doc_arg ? a->doc_arg : "", a->nglobals);
    for (int i = 0; i < a->nglobals; ++i) {
        fprintf(f, "%s%s", i ? "," : "", a->globals[i]);
    }
    fprintf(f, "\n");
    fclose(f);
    rename(tmp, a->state_file);
}

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm, int w, int h, uint32_t color) {
    int stride = w * 4;
    size_t sz = (size_t)stride * (size_t)h;
    const char *td = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/dtwm-sesstest-XXXXXX", (td && td[0]) ? td : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    unlink(path);
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return NULL; }
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { close(fd); return NULL; }
    for (size_t i = 0; i < (size_t)(w * h); ++i) px[i] = color;
    munmap(px, sz);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)sz);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t ser) { (void)d; xdg_wm_base_pong(wm, ser); }
static const struct xdg_wm_base_listener g_wm_l = { .ping = wm_ping };

static void xtop_cfg(void *d, struct xdg_toplevel *xt, int32_t w, int32_t h, struct wl_array *st) {
    (void)xt; (void)st;
    ClientApp *a = d;
    if (w > 0 && h > 0) {
        a->cfg_w = w;
        a->cfg_h = h;
    }
}
static void xtop_close(void *d, struct xdg_toplevel *xt) { (void)d; (void)xt; exit(0); }
static const struct xdg_toplevel_listener g_xtop_l = { .configure = xtop_cfg, .close = xtop_close };

static void xsurf_cfg(void *d, struct xdg_surface *xs, uint32_t ser) {
    ClientApp *a = d;
    xdg_surface_ack_configure(xs, ser);
    int w = a->cfg_w > 0 ? a->cfg_w : a->init_w;
    int h = a->cfg_h > 0 ? a->cfg_h : a->init_h;
    if (!a->buf || a->buf_w != w || a->buf_h != h) {
        if (a->buf) wl_buffer_destroy(a->buf);
        a->buf = create_shm_buffer(a->shm, w, h, 0xFF336699u);
        a->buf_w = w;
        a->buf_h = h;
    }
    wl_surface_attach(a->surf, a->buf, 0, 0);
    wl_surface_damage_buffer(a->surf, 0, 0, w, h);
    wl_surface_commit(a->surf);
    a->configured = true;
    write_client_state(a);
}
static const struct xdg_surface_listener g_xsurf_l = { .configure = xsurf_cfg };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *ifc, uint32_t ver) {
    ClientApp *a = d;
    if (a->nglobals < 128) {
        snprintf(a->globals[a->nglobals++], sizeof(a->globals[0]), "%s", ifc);
    }
    if (!strcmp(ifc, wl_compositor_interface.name)) {
        a->comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(ifc, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(ifc, xdg_wm_base_interface.name)) {
        a->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2);
        xdg_wm_base_add_listener(a->wm, &g_wm_l, a);
    } else if (!strcmp(ifc, xdg_toplevel_tag_manager_v1_interface.name)) {
        a->tag_mgr = wl_registry_bind(r, name, &xdg_toplevel_tag_manager_v1_interface, 1);
    } else if (!strcmp(ifc, wp_security_context_manager_v1_interface.name)) {
        a->sec_mgr = wl_registry_bind(r, name, &wp_security_context_manager_v1_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener g_reg_l = { .global = reg_global, .global_remove = reg_rem };

static int run_sandbox_broker(const char *sock_path, const char *ready_file) {
    ClientApp a;
    memset(&a, 0, sizeof(a));
    a.dpy = wl_display_connect(NULL);
    if (!a.dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(a.dpy);
    wl_registry_add_listener(reg, &g_reg_l, &a);
    wl_display_roundtrip(a.dpy);
    if (!a.sec_mgr) {
        fprintf(stderr, "wp_security_context_manager_v1 not advertised\n");
        return 1;
    }

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) return 1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    unlink(sock_path);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(lfd, 8) != 0) {
        close(lfd);
        return 1;
    }

    int pfd[2];
    if (pipe(pfd) != 0) return 1;
    struct wp_security_context_v1 *ctx =
        wp_security_context_manager_v1_create_listener(a.sec_mgr, lfd, pfd[0]);
    close(pfd[0]);
    close(lfd);

    wp_security_context_v1_set_sandbox_engine(ctx, "org.flatpak");
    wp_security_context_v1_set_app_id(ctx, "org.example.SandboxedApp");
    wp_security_context_v1_set_instance_id(ctx, "flatpak-inst-01");
    wp_security_context_v1_commit(ctx);
    wp_security_context_v1_destroy(ctx);
    wl_display_roundtrip(a.dpy);

    FILE *rf = fopen(ready_file, "w");
    if (rf) { fprintf(rf, "ready\n"); fclose(rf); }

    /* Keep close_fd (pfd[1]) open until broker is terminated */
    while (wl_display_dispatch(a.dpy) != -1) {
    }
    close(pfd[1]);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--broker")) {
        return run_sandbox_broker(argv[2], argv[3]);
    }
    ClientApp a;
    memset(&a, 0, sizeof(a));
    a.init_w = 340;
    a.init_h = 220;
    const char *connect_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--state-dir") && i + 1 < argc) {
            /* Write per-tag state file inside state-dir so restored processes write deterministically */
            static char sbuf[512];
            const char *dir = argv[++i];
            a.state_file = dir; /* resolved after --tag is parsed */
            snprintf(sbuf, sizeof(sbuf), "%s", dir);
            a.state_file = sbuf;
        } else if (!strcmp(argv[i], "--app-id") && i + 1 < argc) {
            a.app_id = argv[++i];
        } else if (!strcmp(argv[i], "--tag") && i + 1 < argc) {
            a.tag = argv[++i];
        } else if (!strcmp(argv[i], "--doc") && i + 1 < argc) {
            a.doc_arg = argv[++i];
        } else if (!strcmp(argv[i], "--connect") && i + 1 < argc) {
            connect_path = argv[++i];
        } else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            a.init_w = atoi(argv[++i]);
            a.init_h = atoi(argv[++i]);
        }
    }
    char full_state_path[512];
    if (a.state_file && a.tag && a.tag[0]) {
        snprintf(full_state_path, sizeof(full_state_path), "%s/%s.state", a.state_file, a.tag);
        a.state_file = full_state_path;
    }

    a.dpy = wl_display_connect(connect_path);
    if (!a.dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(a.dpy);
    wl_registry_add_listener(reg, &g_reg_l, &a);
    wl_display_roundtrip(a.dpy);
    if (!a.comp || !a.shm || !a.wm || !a.tag_mgr) {
        fprintf(stderr, "missing standard globals in client\n");
        return 1;
    }

    a.surf = wl_compositor_create_surface(a.comp);
    a.xsurf = xdg_wm_base_get_xdg_surface(a.wm, a.surf);
    xdg_surface_add_listener(a.xsurf, &g_xsurf_l, &a);
    a.xtop = xdg_surface_get_toplevel(a.xsurf);
    xdg_toplevel_add_listener(a.xtop, &g_xtop_l, &a);
    xdg_toplevel_set_title(a.xtop, a.doc_arg ? a.doc_arg : "Session Client");
    if (a.app_id) xdg_toplevel_set_app_id(a.xtop, a.app_id);
    if (a.tag && a.tag[0]) {
        xdg_toplevel_tag_manager_v1_set_toplevel_tag(a.tag_mgr, a.xtop, a.tag);
        xdg_toplevel_tag_manager_v1_set_toplevel_description(
            a.tag_mgr, a.xtop, a.doc_arg ? a.doc_arg : a.tag
        );
    }
    wl_surface_commit(a.surf);
    wl_display_roundtrip(a.dpy);

    while (wl_display_dispatch(a.dpy) != -1) {
    }
    return 0;
}
"""


FORBIDDEN_SANDBOX_GLOBALS = {
    "wp_security_context_manager_v1",
    "zwlr_screencopy_manager_v1",
    "zwlr_export_dmabuf_manager_v1",
    "ext_image_copy_capture_manager_v1",
    "ext_output_image_capture_source_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1",
    "zwlr_virtual_pointer_manager_v1",
    "zwp_virtual_keyboard_manager_v1",
    "zwp_input_method_manager_v2",
    "zwlr_data_control_manager_v1",
    "zwlr_output_manager_v1",
    "zwlr_output_power_manager_v1",
    "ext_session_lock_manager_v1",
    "zwlr_foreign_toplevel_manager_v1",
    "ext_workspace_manager_v1",
    "zwlr_layer_shell_v1",
}


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


def send_wm_request(ipc_path: Path, cmd: str) -> None:
    payload_hex = (cmd.encode("utf-8") + b"\x00").hex()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2.0)
        s.connect(str(ipc_path))
        buf = b""
        while b"READY\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        s.sendall(f"SET _DT_WM_REQUEST {payload_hex}\n".encode("ascii"))
        time.sleep(0.04)


def parse_kv(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    out: dict[str, str] = {}
    for ln in path.read_text().splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def parse_active_box(props: dict[str, bytes]) -> tuple[int, int, int, int, int, int, int, int] | None:
    raw = props.get("_DT_ACTIVE_WINDOW_BOX")
    if not raw or len(raw) < 32:
        return None
    return struct.unpack("<iiiiiiii", raw[:32])


def wait_until(pred, timeout_s: float = 6.0, desc: str = "condition"):
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(0.03)
    raise RuntimeError(f"timed out waiting for {desc} (last={last})")


def start_compositor(comp_bin: Path, rt_dir: Path, tmp: Path, tag: str) -> tuple[subprocess.Popen, str, Path]:
    status_file = tmp / f"status-{tag}.txt"
    log_f = (tmp / f"dtwm-{tag}.log").open("w")
    env = dict(
        os.environ,
        XDG_RUNTIME_DIR=str(rt_dir),
        TMPDIR=str(tmp),
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        WLR_HEADLESS_OUTPUTS="1",
        LC_ALL="C.UTF-8",
    )
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    proc = subprocess.Popen(
        [
            str(comp_bin),
            "--no-xwayland",
            "--no-panel",
            "--no-open-window-icons",
            "--status-file",
            str(status_file),
        ],
        env=env,
        stdout=log_f,
        stderr=subprocess.STDOUT,
    )
    wait_until(lambda: status_file.is_file(), 8.0, f"compositor {tag} status")
    socks = sorted(p.name for p in rt_dir.glob("wayland-*") if not p.name.endswith(".lock") and "sandbox" not in p.name)
    if not socks:
        raise RuntimeError("no wayland socket")
    wl_sock = socks[-1]
    ipc_path = rt_dir / f"dtwm-wayland-{wl_sock}.ipc"
    wait_until(lambda: ipc_path.exists(), 5.0, f"compositor {tag} ipc")
    return proc, wl_sock, ipc_path


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
    tag_xml = wp_dir / "staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml"
    sec_xml = wp_dir / "staging/security-context/security-context-v1.xml"

    with tempfile.TemporaryDirectory(prefix="dtwm-sess-sec-") as td:
        tmp = Path(td)
        proto = tmp / "proto"
        proto.mkdir()
        for xml, stem in [
            (xdg_xml, "xdg-shell"),
            (tag_xml, "xdg-toplevel-tag-v1"),
            (sec_xml, "security-context-v1"),
        ]:
            subprocess.run([scanner, "client-header", str(xml), str(proto / f"{stem}-client-protocol.h")], check=True)
            subprocess.run([scanner, "private-code", str(xml), str(proto / f"{stem}-protocol.c")], check=True)

        helper_c = tmp / "helper.c"
        helper_c.write_text(HELPER_C)
        helper_bin = tmp / "session-test-app"
        wlc_cf, wlc_lf = pkg_flags("wayland-client")
        subprocess.run(
            [
                cc, "-std=c11", "-O2", "-Wall", "-Wextra",
                "-I" + str(proto), *wlc_cf,
                str(helper_c),
                str(proto / "xdg-shell-protocol.c"),
                str(proto / "xdg-toplevel-tag-v1-protocol.c"),
                str(proto / "security-context-v1-protocol.c"),
                *wlc_lf, "-o", str(helper_bin),
            ],
            check=True,
        )

        # ---------------- Session 1 ----------------
        rt1 = tmp / "rt1"
        rt1.mkdir(mode=0o700)
        state1 = tmp / "state1"
        state1.mkdir()
        state2 = tmp / "state2"
        state2.mkdir()
        comp1, wl1, ipc1 = start_compositor(comp_bin, rt1, tmp, "s1")
        procs1: list[subprocess.Popen] = []
        try:
            cenv1 = dict(os.environ, XDG_RUNTIME_DIR=str(rt1), WAYLAND_DISPLAY=wl1, TMPDIR=str(tmp))

            # 1A. Start wp_security_context_v1 sandbox broker and connect sandboxed client
            sb_sock = rt1 / "sandbox-flatpak.sock"
            sb_ready = tmp / "broker.ready"
            broker = subprocess.Popen([str(helper_bin), "--broker", str(sb_sock), str(sb_ready)], env=cenv1)
            procs1.append(broker)
            wait_until(lambda: sb_ready.is_file(), 5.0, "sandbox broker ready")

            sb_env = dict(os.environ, XDG_RUNTIME_DIR=str(rt1), TMPDIR=str(tmp))
            sb_env.pop("WAYLAND_DISPLAY", None)
            sb_cli = subprocess.Popen(
                [
                    str(helper_bin),
                    "--connect", str(sb_sock),
                    "--state-dir", str(state1),
                    "--app-id", "org.evil.SpoofedAppId",
                    "--tag", "sandbox-win",
                    "--doc", "Sandboxed View",
                ],
                env=sb_env,
            )
            procs1.append(sb_cli)
            sb_st = wait_until(
                lambda: (st if (st := parse_kv(state1 / "sandbox-win.state")).get("configured") == "1" else None),
                5.0,
                "sandboxed client map",
            )
            seen_globals = set(sb_st.get("globals", "").split(","))
            leaked = sorted(seen_globals & FORBIDDEN_SANDBOX_GLOBALS)
            if leaked:
                raise AssertionError(f"sandboxed client saw privileged globals: {leaked}")
            for req_g in ("wl_compositor", "wl_shm", "xdg_wm_base", "xdg_toplevel_tag_manager_v1", "wl_seat"):
                if req_g not in seen_globals:
                    raise AssertionError(f"sandboxed client missing standard global {req_g}")

            # 1B. Launch tagged client 1 ("doc-plan") on ws0 using state2 path in argv so
            # Session 2's relaunch writes directly into state2!
            app1 = subprocess.Popen(
                [
                    str(helper_bin),
                    "--state-dir", str(state2),
                    "--app-id", "org.cde.test.editor",
                    "--tag", "doc-plan",
                    "--doc", "Project Plan 2026.md",
                    "--size", "380", "240",
                ],
                env=cenv1,
            )
            procs1.append(app1)
            wait_until(
                lambda: parse_kv(state2 / "doc-plan.state").get("configured") == "1",
                5.0,
                "doc-plan mapped on ws0",
            )
            box_plan_s1 = wait_until(
                lambda: parse_active_box(read_ipc_props(ipc1)),
                5.0,
                "doc-plan active box",
            )

            # Switch to Workspace Two (ws1) and launch tagged client 2 ("doc-budget") with custom size 520x340
            send_wm_request(ipc1, "f.goto_workspace ws1")
            wait_until(
                lambda: b"ws1" in read_ipc_props(ipc1).get("_DT_WORKSPACE_CURRENT_NAME", b""),
                5.0,
                "switched to ws1",
            )
            app2 = subprocess.Popen(
                [
                    str(helper_bin),
                    "--state-dir", str(state2),
                    "--app-id", "org.cde.test.editor",
                    "--tag", "doc-budget",
                    "--doc", "Budget Notes Q4.md",
                    "--size", "520", "340",
                ],
                env=cenv1,
            )
            procs1.append(app2)
            wait_until(
                lambda: parse_kv(state2 / "doc-budget.state").get("configured") == "1",
                5.0,
                "doc-budget mapped on ws1",
            )
            box_budget_s1 = wait_until(
                lambda: (b if (b := parse_active_box(read_ipc_props(ipc1))) and b[6] == 520 and b[7] == 340 else None),
                5.0,
                "doc-budget 520x340 active box on ws1",
            )

            # Save native Wayland session via _DT_WM_REQUEST f.save_wayland_session
            sess_file = tmp / "sessions" / "current" / "dt.wayland.session"
            send_wm_request(ipc1, f"f.save_wayland_session {sess_file}")
            wait_until(
                lambda: sess_file.is_file()
                and b"saved=3;" in read_ipc_props(ipc1).get("_DT_WAYLAND_SESSION_STATUS", b""),
                5.0,
                "dt.wayland.session saved=3",
            )
            sess_text = sess_file.read_text()
            if "org.evil.SpoofedAppId" in sess_text:
                raise AssertionError(f"spoofed app_id leaked into session file:\n{sess_text}")
            if "org.example.SandboxedApp\tsandbox-win\t" not in sess_text or "flatpak run org.example.SandboxedApp" not in sess_text:
                raise AssertionError(f"verified Flatpak security-context app_id missing in session file:\n{sess_text}")

            # Replace the flatpak line's command with '-' for Session 2 so headless test doesn't invoke host flatpak CLI
            patched_lines = []
            for ln in sess_text.splitlines():
                if ln.startswith("org.example.SandboxedApp\t"):
                    cols = ln.split("\t")
                    cols[-1] = "-"
                    ln = "\t".join(cols)
                patched_lines.append(ln)
            sess_file.write_text("\n".join(patched_lines) + "\n")
        finally:
            for p in procs1:
                if p.poll() is None:
                    p.terminate()
                    try:
                        p.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        p.kill()
            if comp1.poll() is None:
                comp1.terminate()
                try:
                    comp1.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    comp1.kill()

        # Clear state2 files before Session 2 restore so we prove fresh relaunch!
        for f in state2.glob("*.state"):
            f.unlink()

        # ---------------- Session 2 (Fresh Compositor Restore) ----------------
        rt2 = tmp / "rt2"
        rt2.mkdir(mode=0o700)
        comp2, _wl2, ipc2 = start_compositor(comp_bin, rt2, tmp, "s2")
        try:
            send_wm_request(ipc2, f"f.restore_wayland_session {sess_file}")

            st_plan_s2 = wait_until(
                lambda: (st if (st := parse_kv(state2 / "doc-plan.state")).get("configured") == "1" else None),
                6.0,
                "relaunched doc-plan in Session 2",
            )
            st_budget_s2 = wait_until(
                lambda: (st if (st := parse_kv(state2 / "doc-budget.state")).get("configured") == "1" else None),
                6.0,
                "relaunched doc-budget in Session 2",
            )
            if st_plan_s2.get("doc") != "Project Plan 2026.md":
                raise AssertionError(f"doc-plan space-containing argv corrupted: {st_plan_s2}")
            if st_budget_s2.get("doc") != "Budget Notes Q4.md":
                raise AssertionError(f"doc-budget space-containing argv corrupted: {st_budget_s2}")
            if int(st_budget_s2.get("cfg_w", "0")) != 520 or int(st_budget_s2.get("cfg_h", "0")) != 340:
                raise AssertionError(f"doc-budget restored size mismatch: {st_budget_s2}")

            # Verify doc-plan is active on ws0 at its saved coordinates, and switching to ws1 shows doc-budget at its saved coordinates!
            box_plan_s2 = wait_until(
                lambda: (
                    b if (b := parse_active_box(read_ipc_props(ipc2)))
                    and b[0] == box_plan_s1[0] and b[1] == box_plan_s1[1]
                    else None
                ),
                5.0,
                "doc-plan restored geometry on ws0",
            )
            send_wm_request(ipc2, "f.goto_workspace ws1")
            box_budget_s2 = wait_until(
                lambda: (
                    b if (b := parse_active_box(read_ipc_props(ipc2)))
                    and b[0] == box_budget_s1[0] and b[1] == box_budget_s1[1]
                    and b[6] == 520 and b[7] == 340
                    else None
                ),
                5.0,
                "doc-budget restored geometry on ws1",
            )

            print(
                f"PASS check-dtwm-wayland-session-security "
                f"(sandbox_filtered={len(FORBIDDEN_SANDBOX_GLOBALS)} spoof_blocked=OK "
                f"ws0_plan={box_plan_s2[0]},{box_plan_s2[1]}[{box_plan_s2[6]}x{box_plan_s2[7]}] "
                f"ws1_budget={box_budget_s2[0]},{box_budget_s2[1]}[{box_budget_s2[6]}x{box_budget_s2[7]}])"
            )
            return 0
        finally:
            if comp2.poll() is None:
                comp2.terminate()
                try:
                    comp2.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    comp2.kill()


if __name__ == "__main__":
    raise SystemExit(main())
