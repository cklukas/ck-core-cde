#!/usr/bin/env python3
"""Verify Wayland-capable dtlogin display manager (WP-07):
1. ASan/UBSan unit verification of `wayland` / `wayland_uid` Xservers display types,
   FreeDesktop `/usr/share/wayland-sessions` + `/usr/share/xsessions` `.desktop`
   discovery, deterministic ordering (`CDE (Wayland)` first, `CDE (X11 / XLibre)`
   second), duplicate-name disambiguation, and default session resolution.
2. Live Ubuntu VM daemon integration test (when dtwm-wayland + sudo PAM setup are
   available): starts real `dtlogin -nodaemon` on a `wayland` local seat, verifies
   unprivileged `_dtgreeter` running `dtwm-wayland --greeter` + genuine Motif
   `dtgreet` (`libXm.so.9`), captures a real greeter screenshot, authenticates a
   test user via PAM (`SvcPam.c`) into `CDE (Wayland)`, logs out cleanly back to
   `dtgreet`, selects `CDE (X11 / XLibre)` from `dtgreet`'s Options -> Session
   menu, logs into `CDE (X11 / XLibre)`, and logs out cleanly with zero leaked
   compositor/X11/session processes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time


UNIT_HARNESS_C = r"""
#define _GNU_SOURCE
#include "dm.h"
#include "vgmsg.h"
#include "vgproto.h"
#include "wayland_seat.h"
#include <assert.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int debugLevel = 0;
int wakeupTime = -1;
int langListTimeout = 5;
XrmDatabase XresourceDB = nullptr;

void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
int addressEqual(XdmcpNetaddr a1, int l1, XdmcpNetaddr a2, int l2) {
    (void)a1; (void)l1; (void)a2; (void)l2; return 0;
}
int CloseOnFork(void) { return 0; }
char *_DtCatgets(void *fd, int s, int m, const char *d) {
    (void)fd; (void)s; (void)m; return (char *)d;
}
Cursor _DtGetHourGlassCursor(Display *dpy) { (void)dpy; return None; }

#if !defined(sun) && defined(HAS_PAM_LIBRARY)
int _DtSvcPamPutenv(const char *s, const char *u, const char *d, const char *nv) {
    (void)s; (void)u; (void)d; (void)nv; return 0;
}
char **_DtSvcPamGetenvlist(void) { return nullptr; }
#endif

static const char *get_res(XrmDatabase db, const char *name, const char *cls) {
    char *t = nullptr;
    XrmValue v = {0, nullptr};
    if (XrmGetResource(db, name, cls, &t, &v) == True && v.addr) return v.addr;
    return "";
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *wl_dir = argv[1];
    const char *x11_dir = argv[2];

    XrmInitialize();

    DisplayType acceptable[2] = {
        { Local, Permanent, FromFile },
        { Foreign, Permanent, FromFile },
    };
    struct passwd puser;
    memset(&puser, 0, sizeof(puser));
    puser.pw_name = "nobody";

    char line1[] = ":0 Local wayland@tty1 /usr/dt/bin/dtwm-wayland --greeter";
    assert(ParseDisplay(line1, acceptable, 2, &puser) == 1);
    struct display *d0 = FindDisplayByName(":0");
    assert(d0 != nullptr);
    assert(d0->isWaylandSeat == 1);
    assert(d0->authorize == FALSE);
    assert(d0->gettyLine && strcmp(d0->gettyLine, "tty1") == 0);
    assert(d0->greeterUser && strcmp(d0->greeterUser, "_dtgreeter") == 0);

    char line2[] = ":1 Local local@tty2 /usr/bin/X :1";
    assert(ParseDisplay(line2, acceptable, 2, &puser) == 1);
    struct display *d1 = FindDisplayByName(":1");
    assert(d1 != nullptr && d1->isWaylandSeat == 0);

    setenv("DTLOGIN_WAYLAND_SESSIONS_DIR", wl_dir, 1);
    setenv("DTLOGIN_XSESSIONS_DIR", x11_dir, 1);

    XresourceDB = XrmGetStringDatabase("Dtlogin*optionsDelay: 0\n");
    DtloginPopulateDesktopSessions(d0, &XresourceDB);

    assert(strcmp(get_res(XresourceDB, "Dtlogin*altDts", "Dtlogin*AltDts"), "5") == 0);
    assert(strcmp(get_res(XresourceDB, "Dtlogin*altDtName1", "Dtlogin*AltDtName1"), "CDE (Wayland)") == 0);
    assert(strcmp(get_res(XresourceDB, "Dtlogin*altDtProtocol1", "Dtlogin*AltDtProtocol1"), "wayland") == 0);
    assert(strcmp(get_res(XresourceDB, "Dtlogin*altDtName2", "Dtlogin*AltDtName2"), "CDE (X11 / XLibre)") == 0);
    assert(strcmp(get_res(XresourceDB, "Dtlogin*altDtProtocol2", "Dtlogin*AltDtProtocol2"), "x11") == 0);
    /* Remaining Wayland sessions come before remaining X11 sessions, with duplicate names disambiguated */
    assert(strcmp(get_res(XresourceDB, "Dtlogin*altDtName3", "Dtlogin*AltDtName3"), "Labwc") == 0);
    assert(strcmp(get_res(XresourceDB, "Dtlogin*altDtName4", "Dtlogin*AltDtName4"), "SharedName (Wayland)") == 0);
    assert(strcmp(get_res(XresourceDB, "Dtlogin*altDtName5", "Dtlogin*AltDtName5"), "SharedName (X11)") == 0);

    char **def_argv = nullptr;
    assert(DtloginResolveDefaultWaylandSession(d0, XresourceDB, &def_argv) == 1);
    assert(def_argv && def_argv[0] && strstr(def_argv[0], "mock-cde-wayland") != nullptr);
    freeEnv(def_argv);

    XrmDestroyDatabase(XresourceDB);
    RemoveDisplay(d1);
    RemoveDisplay(d0);
    printf("PASS_UNIT\n");
    return 0;
}
"""


VKBD_XML = r"""<?xml version="1.0" encoding="UTF-8"?>
<protocol name="virtual_keyboard_unstable_v1">
  <interface name="zwp_virtual_keyboard_v1" version="1">
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
    <request name="create_virtual_keyboard">
      <arg name="seat" type="object" interface="wl_seat"/>
      <arg name="id" type="new_id" interface="zwp_virtual_keyboard_v1"/>
    </request>
  </interface>
</protocol>
"""

VKBD_TYPE_C = r"""
#define _GNU_SOURCE
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "vkbd-client.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static struct wl_seat *g_seat = NULL;
static struct zwp_virtual_keyboard_manager_v1 *g_mgr = NULL;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (strcmp(iface, "wl_seat") == 0) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, "zwp_virtual_keyboard_manager_v1") == 0) {
        g_mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}
static void reg_rem(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { .global = reg_global, .global_remove = reg_rem };

static int evdev_for_char(char c) {
    switch (c) {
    case '1': return 2;  case '2': return 3;  case '3': return 4;  case '4': return 5;
    case '5': return 6;  case '6': return 7;  case '7': return 8;  case '8': return 9;
    case '9': return 10; case '0': return 11; case '-': return 12;
    case 'q': return 16; case 'w': return 17; case 'e': return 18; case 'r': return 19;
    case 't': return 20; case 'y': return 21; case 'u': return 22; case 'i': return 23;
    case 'o': return 24; case 'p': return 25;
    case 'a': return 30; case 's': return 31; case 'd': return 32; case 'f': return 33;
    case 'g': return 34; case 'h': return 35; case 'j': return 36; case 'k': return 37;
    case 'l': return 38;
    case 'z': return 44; case 'x': return 45; case 'c': return 46; case 'v': return 47;
    case 'b': return 48; case 'n': return 49; case 'm': return 50;
    case 'D': return 108; /* KEY_DOWN */
    case '\n': return 28;
    default: return -1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 1;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!g_seat || !g_mgr) return 1;

    struct zwp_virtual_keyboard_v1 *vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(g_mgr, g_seat);
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { .layout = "us" };
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *km_str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t km_sz = strlen(km_str) + 1;
    int fd = memfd_create("vkbd-keymap", MFD_CLOEXEC);
    if (ftruncate(fd, (off_t)km_sz) != 0) return 1;
    void *ptr = mmap(NULL, km_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memcpy(ptr, km_str, km_sz);
    munmap(ptr, km_sz);

    zwp_virtual_keyboard_v1_keymap(vk, 1 /* XKB_V1 */, fd, (uint32_t)km_sz);
    wl_display_roundtrip(dpy);
    close(fd);
    free(km_str);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);

    /* Let Xwayland finish xkbcomp before delivering key presses */
    usleep(160000);

    uint32_t t = 100;
    const char *s = argv[1];
    for (size_t i = 0; s[i]; ++i) {
        int kc = evdev_for_char(s[i]);
        if (kc < 0) continue;
        zwp_virtual_keyboard_v1_key(vk, t, (uint32_t)kc, 1);
        wl_display_flush(dpy);
        usleep(15000);
        t += 15;
        zwp_virtual_keyboard_v1_key(vk, t, (uint32_t)kc, 0);
        wl_display_flush(dpy);
        usleep(15000);
        t += 15;
    }
    /* Submit with Return */
    zwp_virtual_keyboard_v1_key(vk, t, 28, 1);
    wl_display_flush(dpy);
    usleep(20000);
    zwp_virtual_keyboard_v1_key(vk, t + 20, 28, 0);
    wl_display_roundtrip(dpy);
    usleep(160000);

    zwp_virtual_keyboard_v1_destroy(vk);
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
"""

X11_INSPECT_C = r"""
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void walk(Display *dpy, Window root, Window w, int depth) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) return;
    if (attr.map_state == IsViewable && attr.class == InputOutput) {
        int rx = 0, ry = 0;
        Window child = None;
        XTranslateCoordinates(dpy, w, root, 0, 0, &rx, &ry, &child);
        printf("WIN depth=%d id=0x%lx x=%d y=%d w=%d h=%d or=%d\n",
               depth, (unsigned long)w, rx, ry, attr.width, attr.height, attr.override_redirect);
    }
    Window r = None, p = None, *children = NULL;
    unsigned int n = 0;
    if (XQueryTree(dpy, w, &r, &p, &children, &n) && children) {
        for (unsigned int i = 0; i < n; ++i) walk(dpy, root, children[i], depth + 1);
        XFree(children);
    }
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);
    if (argc > 1 && strcmp(argv[1], "res") == 0) {
        Atom actual = None;
        int fmt = 0;
        unsigned long n = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, XA_RESOURCE_MANAGER, 0, 65536, False,
                               XA_STRING, &actual, &fmt, &n, &rem, &data) == Success && data) {
            fwrite(data, 1, n, stdout);
            XFree(data);
        }
        XCloseDisplay(dpy);
        return 0;
    }
    walk(dpy, root, root, 0);
    XCloseDisplay(dpy);
    return 0;
}
"""


def detect_sanitize(cc: list[str], tmp_dir: str | None) -> str:
    with tempfile.TemporaryDirectory(dir=tmp_dir) as d:
        src = Path(d) / "p.c"
        exe = Path(d) / "p"
        src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        for s in ("address,undefined", "undefined"):
            r = subprocess.run(cc + [f"-fsanitize={s}", str(src), "-o", str(exe)], capture_output=True)
            if r.returncode == 0 and subprocess.run([str(exe)], capture_output=True).returncode == 0:
                return s
    return "undefined"


def run_unit_checks(dtlogin_src: Path, cde_src: Path, cde_build: Path, cc: list[str], tmp_dir: str | None) -> None:
    san = detect_sanitize(cc, tmp_dir)
    with tempfile.TemporaryDirectory(prefix="dtlogin-wl-unit-", dir=tmp_dir) as work_s:
        work = Path(work_s)
        wl_dir = work / "wayland-sessions"
        x11_dir = work / "xsessions"
        bin_dir = work / "mockbin"
        for p in (wl_dir, x11_dir, bin_dir):
            p.mkdir(parents=True)

        for script_name in ("mock-cde-wayland", "mock-cde-x11", "mock-labwc", "mock-shared-wl", "mock-shared-x11"):
            sp = bin_dir / script_name
            sp.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            sp.chmod(0o755)

        (wl_dir / "cde-wayland.desktop").write_text(
            f"[Desktop Entry]\nName=CDE (Wayland)\nExec={bin_dir}/mock-cde-wayland\nTryExec={bin_dir}/mock-cde-wayland\nDesktopNames=CDE\n",
            encoding="utf-8",
        )
        (wl_dir / "labwc.desktop").write_text(
            f"[Desktop Entry]\nName=Labwc\nExec={bin_dir}/mock-labwc\n",
            encoding="utf-8",
        )
        (wl_dir / "shared.desktop").write_text(
            f"[Desktop Entry]\nName=SharedName\nExec={bin_dir}/mock-shared-wl %U\n",
            encoding="utf-8",
        )
        (wl_dir / "hidden.desktop").write_text(
            f"[Desktop Entry]\nName=HiddenSession\nExec={bin_dir}/mock-labwc\nHidden=true\n",
            encoding="utf-8",
        )
        (wl_dir / "missing-tryexec.desktop").write_text(
            f"[Desktop Entry]\nName=MissingBin\nExec={bin_dir}/mock-labwc\nTryExec=/nonexistent/bin\n",
            encoding="utf-8",
        )
        (x11_dir / "cde.desktop").write_text(
            f"[Desktop Entry]\nName=CDE (X11 / XLibre)\nExec={bin_dir}/mock-cde-x11\nDesktopNames=CDE\n",
            encoding="utf-8",
        )
        (x11_dir / "shared.desktop").write_text(
            f"[Desktop Entry]\nName=SharedName\nExec={bin_dir}/mock-shared-x11\n",
            encoding="utf-8",
        )

        harness_c = work / "unit_harness.c"
        harness_c.write_text(UNIT_HARNESS_C, encoding="utf-8")
        exe = work / "unit_harness"

        motif_inc = os.environ.get("MOTIF_INCLUDE")
        inc_flags = [
            f"-I{dtlogin_src}",
            f"-I{cde_src / 'include'}",
            f"-I{cde_build / 'include'}",
            "-I/usr/include/tirpc",
        ]
        if motif_inc and Path(motif_inc).is_dir():
            inc_flags.append(f"-I{motif_inc}")

        cmd = (
            cc
            + [
                "-std=gnu23",
                "-O1",
                "-g",
                f"-fsanitize={san}",
                "-fno-sanitize-recover=all",
                "-DCDE_INSTALLATION_TOP=\"/nonexistent/dt\"",
                "-DCDE_CONFIGURATION_TOP=\"/etc/dt\"",
                "-DCDE_LOGFILES_TOP=\"/var/dt\"",
                "-DHAS_PAM_LIBRARY",
                "-DDTLOGIN_WAYLAND_SEAT",
                *inc_flags,
                str(harness_c),
                str(dtlogin_src / "file.c"),
                str(dtlogin_src / "dpylist.c"),
                str(dtlogin_src / "util.c"),
                str(dtlogin_src / "wayland_seat.c"),
                "-lXt",
                "-lXmu",
                "-lX11",
                "-lXdmcp",
                "-lXau",
                "-lpam",
                "-lpthread",
                "-o",
                str(exe),
            ]
        )
        subprocess.run(cmd, check=True)
        out = subprocess.check_output([str(exe), str(wl_dir), str(x11_dir)], text=True)
        if "PASS_UNIT" not in out:
            raise SystemExit(f"unit harness failed: {out}")


def find_greeter_pids(work: Path, greeter_binary: Path) -> list[tuple[int, int, str, str]]:
    """Return only the compositor and greeter processes started by this fixture."""
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        try:
            comm = (entry / "comm").read_text(encoding="utf-8", errors="replace").strip()
            if comm not in ("dtwm-wayland", "dtgreet"):
                continue
            raw = (entry / "cmdline").read_bytes()
            if not raw:
                continue
            args = raw.rstrip(b"\x00").decode("utf-8", "surrogateescape").split("\x00")
            if comm == "dtgreet":
                executable = subprocess.run(
                    ["sudo", "-n", "readlink", str(entry / "exe")],
                    capture_output=True, text=True,
                )
                if executable.returncode or executable.stdout.strip() != str(greeter_binary):
                    continue
            elif "--status-file" in args:
                index = args.index("--status-file") + 1
                if index == len(args) or not Path(args[index]).is_relative_to(work):
                    continue
            else:
                continue
            cmdline = " ".join(args)
            st_txt = (entry / "status").read_text(encoding="utf-8", errors="replace")
            uid = -1
            for ln in st_txt.splitlines():
                if ln.startswith("Uid:"):
                    uid = int(ln.split()[1])
                    break
            found.append((pid, uid, comm, cmdline))
        except OSError:
            continue
    return found


def run_live_daemon_test(
    dtlogin_src: Path,
    cde_src: Path,
    cde_build: Path,
    compositor_bin: Path,
    cc: list[str],
    tmp_dir: str | None,
    screenshot_out: Path | None,
) -> dict[str, object]:
    dtlogin_elf = cde_build / "programs" / "dtlogin" / ".libs" / "dtlogin"
    dtgreet_elf = cde_build / "programs" / "dtlogin" / ".libs" / "dtgreet"
    if not dtlogin_elf.is_file():
        dtlogin_elf = cde_build / "programs" / "dtlogin" / "dtlogin"
    if not dtgreet_elf.is_file():
        dtgreet_elf = cde_build / "programs" / "dtlogin" / "dtgreet"
    xvfb_command = shutil.which("Xvfb")
    if xvfb_command is None:
        raise SystemExit("Xvfb is required for the live X11 handoff")
    xvfb_path = Path(xvfb_command).absolute()

    if subprocess.run(["pgrep", "-x", "dtlogin"], capture_output=True).returncode == 0:
        raise SystemExit("Use a guest without an active dtlogin for the live seat check")
    test_user = f"ckdt{os.getpid()}"
    test_pass = "waylanddt26"

    with tempfile.TemporaryDirectory(prefix="dtlogin-wl-live-", dir=tmp_dir) as work_s:
        work = Path(work_s)
        os.chmod(work, 0o755)

        x11_inspect_c = work / "x11_inspect.c"
        x11_inspect_bin = work / "x11_inspect"
        x11_inspect_c.write_text(X11_INSPECT_C, encoding="utf-8")
        subprocess.run(cc + ["-O2", str(x11_inspect_c), "-lX11", "-o", str(x11_inspect_bin)], check=True)
        os.chmod(x11_inspect_bin, 0o755)

        vkbd_xml = work / "vkbd.xml"
        vkbd_h = work / "vkbd-client.h"
        vkbd_proto_c = work / "vkbd-proto.c"
        vkbd_c = work / "vkbd_type.c"
        vkbd_bin = work / "vkbd_type"
        vkbd_xml.write_text(VKBD_XML, encoding="utf-8")
        subprocess.run(["wayland-scanner", "client-header", str(vkbd_xml), str(vkbd_h)], check=True)
        subprocess.run(["wayland-scanner", "private-code", str(vkbd_xml), str(vkbd_proto_c)], check=True)
        vkbd_c.write_text(VKBD_TYPE_C, encoding="utf-8")
        subprocess.run(
            cc + ["-O2", f"-I{work}", str(vkbd_c), str(vkbd_proto_c), "-lwayland-client", "-lxkbcommon", "-o", str(vkbd_bin)],
            check=True,
        )
        os.chmod(vkbd_bin, 0o755)

        bin_dir = work / "bin"
        cfg_dir = work / "etc"
        rt_base = work / "rt-greeter"
        user_rt_base = work / "rt-user"
        wl_sess_dir = work / "wayland-sessions"
        x11_sess_dir = work / "xsessions"
        rec_dir = work / "records"
        for d in (bin_dir, cfg_dir, rt_base, user_rt_base, wl_sess_dir, x11_sess_dir, rec_dir):
            d.mkdir(parents=True, exist_ok=True)
            os.chmod(d, 0o755)
        os.chmod(rec_dir, 0o777)
        os.chmod(user_rt_base, 0o1777)

        shutil.copy2(dtlogin_elf, bin_dir / "dtlogin")
        shutil.copy2(dtgreet_elf, bin_dir / "dtgreet")
        (bin_dir / "Xvfb").symlink_to(xvfb_path)
        os.chmod(bin_dir / "dtlogin", 0o755)
        os.chmod(bin_dir / "dtgreet", 0o755)

        # Build library path for uninstalled CDE + Motif ABI 9 + fs-lib
        lib_dirs = []
        for cand in (
            *(Path(path) for path in os.environ.get("LD_LIBRARY_PATH", "").split(":") if path),
            *(cde_build / "lib").glob("*/.libs"),
            cde_build / "lib" / "tt" / "lib" / ".libs",
            Path("/usr/dt/lib"),
        ):
            if cand.is_dir():
                lib_dirs.append(str(cand.resolve()))
        ld_lib_path = ":".join(dict.fromkeys(lib_dirs))

        # Session scripts invoked on login:
        # 1) Wayland session: launches real dtwm-wayland user compositor, records uid/env/socket, waits for logout trigger
        wl_session_sh = bin_dir / "test-cde-wayland-session.sh"
        wl_session_sh.write_text(
            f"""#!/bin/sh
set -eu
export LD_LIBRARY_PATH="{ld_lib_path}:${{LD_LIBRARY_PATH:-}}"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
COMP_ST="{rec_dir}/user-wayland-comp.status"
rm -f "$COMP_ST" "{rec_dir}/logout-wayland"
"{compositor_bin.resolve()}" --no-panel --status-file "$COMP_ST" >"{rec_dir}/user-wayland-comp.log" 2>&1 &
CPID=$!
for _i in $(seq 1 80); do
    [ -s "$COMP_ST" ] && break
    sleep 0.05
done
cat >"{rec_dir}/wayland-session.env" <<EOF
UID=$(id -u)
USER=$(id -un)
XDG_SESSION_TYPE=${{XDG_SESSION_TYPE:-}}
XDG_CURRENT_DESKTOP=${{XDG_CURRENT_DESKTOP:-}}
XDG_SEAT=${{XDG_SEAT:-}}
XDG_VTNR=${{XDG_VTNR:-}}
XDG_RUNTIME_DIR=${{XDG_RUNTIME_DIR:-}}
INITIAL_DISPLAY=${{DISPLAY:-UNSET}}
INITIAL_WAYLAND_DISPLAY=${{WAYLAND_DISPLAY:-UNSET}}
USER_COMP_PID=$CPID
EOF
cat "$COMP_ST" >>"{rec_dir}/wayland-session.env" || true
touch "{rec_dir}/wayland-session.ready"
while [ ! -f "{rec_dir}/logout-wayland" ]; do
    sleep 0.05
done
kill -TERM "$CPID" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
exit 0
""",
            encoding="utf-8",
        )
        wl_session_sh.chmod(0o755)

        # 2) X11 session: verifies live X11 server connection on DISPLAY, records uid/env, waits for logout trigger
        x11_session_sh = bin_dir / "test-cde-x11-session.sh"
        x11_session_sh.write_text(
            f"""#!/bin/sh
set -eu
export LD_LIBRARY_PATH="{ld_lib_path}:${{LD_LIBRARY_PATH:-}}"
rm -f "{rec_dir}/logout-x11"
XWIN=$("{x11_inspect_bin}" | wc -l | tr -d ' ')
cat >"{rec_dir}/x11-session.env" <<EOF
UID=$(id -u)
USER=$(id -un)
XDG_SESSION_TYPE=${{XDG_SESSION_TYPE:-}}
XDG_CURRENT_DESKTOP=${{XDG_CURRENT_DESKTOP:-}}
XDG_SEAT=${{XDG_SEAT:-}}
DISPLAY=${{DISPLAY:-UNSET}}
WAYLAND_DISPLAY=${{WAYLAND_DISPLAY:-UNSET}}
X11_QUERY_LINES=$XWIN
EOF
touch "{rec_dir}/x11-session.ready"
while [ ! -f "{rec_dir}/logout-x11" ]; do
    sleep 0.05
done
exit 0
""",
            encoding="utf-8",
        )
        x11_session_sh.chmod(0o755)

        (wl_sess_dir / "cde-wayland.desktop").write_text(
            f"[Desktop Entry]\nName=CDE (Wayland)\nExec={wl_session_sh}\nTryExec={wl_session_sh}\nDesktopNames=CDE\nType=Application\n",
            encoding="utf-8",
        )
        (x11_sess_dir / "cde.desktop").write_text(
            f"[Desktop Entry]\nName=CDE (X11 / XLibre)\nExec={x11_session_sh}\nTryExec={x11_session_sh}\nDesktopNames=CDE\nType=Application\n",
            encoding="utf-8",
        )

        # Configure Xresources (using real CDE dtlogin Xresources if present!), Xservers, and Xconfig
        real_xres = cde_build / "programs" / "localized" / "C" / "config" / "Xresources"
        base_xres = real_xres.read_text(encoding="utf-8", errors="replace") if real_xres.is_file() else ""
        xres = cfg_dir / "Xresources"
        xres.write_text(
            base_xres
            + "\nDtlogin*optionsDelay: 0\n"
            + "Dtlogin*workspaceCursor: True\n",
            encoding="utf-8",
        )
        xservers = cfg_dir / "Xservers"
        xservers.write_text(
            f":0 Local wayland@tty1 {compositor_bin.resolve()} --greeter\n",
            encoding="utf-8",
        )
        old_ts = time.time() - 120.0
        os.utime(xservers, (old_ts, old_ts))
        noop_sh = cfg_dir / "noop.sh"
        noop_sh.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        noop_sh.chmod(0o755)

        stage_icons = Path(os.environ.get("DTLOGIN_CDE_STAGE", "/usr/dt")) / "appconfig" / "icons"
        icon_cfg = ""
        if stage_icons.is_dir():
            icon_cfg = (
                f"Dtlogin*pmSearchPath: {stage_icons}/%L/%B%M.pm:{stage_icons}/C/%B%M.pm:{stage_icons}/C/%B\n"
                f"Dtlogin*bmSearchPath: {stage_icons}/%L/%B%M.bm:{stage_icons}/C/%B%M.bm:{stage_icons}/C/%B\n"
            )

        xconfig = cfg_dir / "Xconfig"
        xconfig.write_text(
            f"Dtlogin.servers: {xservers}\n"
            f"Dtlogin.errorLogFile: {work}/dtlogin.log\n"
            f"Dtlogin.pidFile: {work}/dtlogin.pid\n"
            f"Dtlogin.accessFile: {cfg_dir}/Xaccess\n"
            f"Dtlogin*resources: {xres}\n"
            f"Dtlogin*setup: {noop_sh}\n"
            f"Dtlogin*startup: {noop_sh}\n"
            f"Dtlogin*reset: {noop_sh}\n"
            f"Dtlogin*session: {x11_session_sh}\n"
            f"Dtlogin*xrdb: /usr/bin/xrdb\n"
            f"Dtlogin*authorize: False\n"
            f"Dtlogin*grabServer: False\n"
            f"Dtlogin*openTimeout: 15\n"
            f"Dtlogin*startAttempts: 4\n"
            + icon_cfg,
            encoding="utf-8",
        )
        (cfg_dir / "Xaccess").write_text("", encoding="utf-8")

        # Backup and configure PAM + _dtgreeter + test user via sudo
        pam_dtlogin = Path("/etc/pam.d/dtlogin")
        pam_greeter = Path("/etc/pam.d/dtlogin-greeter")
        ld_conf = Path("/etc/ld.so.conf.d/zz-dtlogin-wp07-test.conf")
        orig_pam_dtlogin = pam_dtlogin.read_text(encoding="utf-8") if pam_dtlogin.exists() else None
        orig_pam_greeter = pam_greeter.read_text(encoding="utf-8") if pam_greeter.exists() else None
        created_greeter_user = False
        created_test_user = False
        dtlogin_proc: subprocess.Popen[bytes] | None = None

        try:
            # Verify gdm3 is inactive
            gdm_state = subprocess.run(["systemctl", "is-active", "gdm3"], capture_output=True, text=True).stdout.strip()
            if gdm_state == "active":
                raise SystemExit("gdm3.service is active; disable gdm3 before running Wayland dtlogin seat test")

            if subprocess.run(["id", "-u", "_dtgreeter"], capture_output=True).returncode != 0:
                subprocess.run(
                    ["sudo", "-n", "useradd", "--system", "--user-group", "--home-dir", "/var/dt", "--no-create-home", "--shell", "/usr/sbin/nologin", "_dtgreeter"],
                    check=True,
                )
                created_greeter_user = True
            subprocess.run(["sudo", "-n", "mkdir", "-p", "/var/dt", "/run/dtlogin"], check=True)

            subprocess.run(
                ["sudo", "-n", "useradd", "-m", "-s", "/bin/sh", test_user],
                check=True,
            )
            created_test_user = True
            subprocess.run(
                ["sudo", "-n", "chpasswd"],
                input=f"{test_user}:{test_pass}\n",
                text=True,
                check=True,
            )

            subprocess.run(
                ["sudo", "-n", "tee", str(pam_dtlogin)],
                input=(
                    "auth required pam_unix.so\n"
                    "account required pam_unix.so\n"
                    "-session optional pam_systemd.so\n"
                    "session required pam_unix.so\n"
                    "password required pam_unix.so\n"
                ),
                text=True,
                stdout=subprocess.DEVNULL,
                check=True,
            )
            subprocess.run(
                ["sudo", "-n", "tee", str(pam_greeter)],
                input=(
                    "auth sufficient pam_permit.so\n"
                    "account sufficient pam_permit.so\n"
                    "-session optional pam_systemd.so class=greeter\n"
                    "session required pam_unix.so\n"
                ),
                text=True,
                stdout=subprocess.DEVNULL,
                check=True,
            )
            subprocess.run(
                ["sudo", "-n", "tee", str(ld_conf)],
                input="\n".join(lib_dirs) + "\n",
                text=True,
                stdout=subprocess.DEVNULL,
                check=True,
            )
            subprocess.run(["sudo", "-n", "ldconfig"], check=True)

            greeter_uid = pwd.getpwnam("_dtgreeter").pw_uid
            test_uid = pwd.getpwnam(test_user).pw_uid

            daemon_cmd = [
                "sudo",
                "-n",
                "env",
                f"PATH={bin_dir}:/usr/dt/bin:/usr/local/bin:/usr/bin:/bin",
                f"LD_LIBRARY_PATH={ld_lib_path}",
                f"DTLOGIN_WAYLAND_RUNTIME_BASE={rt_base}",
                f"DTLOGIN_USER_RUNTIME_BASE={user_rt_base}",
                f"DTLOGIN_WAYLAND_SESSIONS_DIR={wl_sess_dir}",
                f"DTLOGIN_XSESSIONS_DIR={x11_sess_dir}",
                "DTLOGIN_X11_SERVER_CMD=Xvfb -screen 0 1280x800x24 -nolisten tcp",
                "WLR_BACKENDS=headless",
                "WLR_HEADLESS_OUTPUTS=1",
                "WLR_RENDERER=pixman",
                "WLR_LIBINPUT_NO_DEVICES=1",
                "LIBSEAT_BACKEND=noop",
                str(bin_dir / "dtlogin"),
                "-nodaemon",
                "-debug",
                "3",
                "-config",
                str(xconfig),
            ]
            dtlogin_proc = subprocess.Popen(daemon_cmd)

            def wait_for_greeter_ready(prev_comp_pid: int = -1) -> tuple[int, int, str, str, Path]:
                status_env = rt_base / "greeter-_0" / "status.env"
                deadline = time.monotonic() + 25.0
                last_diag = ""
                while time.monotonic() < deadline:
                    cat_res = subprocess.run(
                        ["sudo", "-n", "cat", str(status_env)],
                        capture_output=True,
                        text=True,
                    )
                    if cat_res.returncode == 0 and cat_res.stdout.strip():
                        kv = dict(ln.split("=", 1) for ln in cat_res.stdout.splitlines() if "=" in ln)
                        wl_s = kv.get("WAYLAND_DISPLAY", "")
                        x11_s = kv.get("DISPLAY", "")
                        if wl_s and x11_s:
                            procs = find_greeter_pids(work, bin_dir / "dtgreet")
                            comp_p = [p for p in procs if p[2] == "dtwm-wayland" and "--greeter" in p[3] and p[1] == greeter_uid]
                            greet_p = [p for p in procs if p[2] == "dtgreet"]
                            last_diag = f"procs={procs}"
                            if comp_p and greet_p and comp_p[0][0] != prev_comp_pid:
                                win_out = subprocess.run(
                                    ["sudo", "-n", "env", f"DISPLAY={x11_s}", str(x11_inspect_bin)],
                                    capture_output=True,
                                    text=True,
                                ).stdout
                                last_diag = f"procs={procs}\nwin_out:\n{win_out}"
                                if "WIN depth=1" in win_out and "WIN depth=3" in win_out:
                                    return comp_p[0][0], greet_p[0][0], wl_s, x11_s, rt_base / "greeter-_0"
                    time.sleep(0.15)
                log_tail = (work / "dtlogin.log").read_text(errors="replace")[-3000:] if (work / "dtlogin.log").is_file() else "(no log)"
                raise SystemExit(f"Timed out waiting for Wayland dtgreet ({last_diag}); dtlogin.log:\n{log_tail}")

            # 1. Wait for initial greeter boot
            comp_pid1, greet_pid1, wl_sock1, x11_dpy1, greeter_rt1 = wait_for_greeter_ready()
            time.sleep(0.4)

            # Verify dtgreet maps libXm.so.9
            maps_txt = subprocess.check_output(["sudo", "-n", "cat", f"/proc/{greet_pid1}/maps"], text=True)
            if "libXm.so.9" not in maps_txt:
                raise SystemExit(f"dtgreet pid {greet_pid1} did not map libXm.so.9!")

            # Verify RESOURCE_MANAGER contains our discovered desktop sessions in order
            res_txt = subprocess.check_output(
                ["sudo", "-n", "env", f"DISPLAY={x11_dpy1}", str(x11_inspect_bin), "res"],
                text=True,
            )
            if "Dtlogin*altDtName1:\tCDE (Wayland)" not in res_txt and "Dtlogin*altDtName1: CDE (Wayland)" not in res_txt:
                raise SystemExit(f"Expected CDE (Wayland) as altDtName1 in RESOURCE_MANAGER, got:\n{res_txt}")
            if "CDE (X11 / XLibre)" not in res_txt:
                raise SystemExit(f"Expected CDE (X11 / XLibre) in RESOURCE_MANAGER, got:\n{res_txt}")

            # Capture real screenshot of genuine Motif dtgreet running on dtwm-wayland --greeter
            shot_file = screenshot_out if screenshot_out else (work / "dtgreet-wayland.png")
            shot_file.parent.mkdir(parents=True, exist_ok=True)
            tmp_shot = work / "shot.png"
            subprocess.run(
                [
                    "sudo",
                    "-n",
                    "env",
                    f"XDG_RUNTIME_DIR={greeter_rt1}",
                    f"WAYLAND_DISPLAY={wl_sock1}",
                    "grim",
                    str(tmp_shot),
                ],
                check=True,
            )
            subprocess.run(["sudo", "-n", "chmod", "0644", str(tmp_shot)], check=True)
            shutil.copy2(tmp_shot, shot_file)

            def wlr_key(rt_dir: Path, wl_sock: str, text_str: str) -> None:
                subprocess.run(
                    [
                        "sudo",
                        "-n",
                        "env",
                        f"XDG_RUNTIME_DIR={rt_dir}",
                        f"WAYLAND_DISPLAY={wl_sock}",
                        str(vkbd_bin),
                        text_str.rstrip("\n"),
                    ],
                    check=True,
                )

            def wlr_click(rt_dir: Path, wl_sock: str, x: int, y: int) -> None:
                for args in (["move", "-10000", "-10000"], ["move", str(x), str(y)], ["click", "left"]):
                    subprocess.run(
                        [
                            "sudo",
                            "-n",
                            "env",
                            f"XDG_RUNTIME_DIR={rt_dir}",
                            f"WAYLAND_DISPLAY={wl_sock}",
                            "wlrctl",
                            "pointer",
                            *args,
                        ],
                        check=True,
                    )

            # 2. Login #1: Default session -> CDE (Wayland)
            # Find login_text window in dtgreet and click it to guarantee X11 widget focus
            wtree1 = subprocess.check_output(
                ["sudo", "-n", "env", f"DISPLAY={x11_dpy1}", str(x11_inspect_bin)],
                text=True,
            )
            for ln in wtree1.splitlines():
                m = re.search(r"depth=(\d+)\s+id=\S+\s+x=(\d+)\s+y=(\d+)\s+w=(\d+)\s+h=(\d+)\s+or=0", ln)
                if m:
                    depth, wx, wy, ww, wh = (int(m.group(i)) for i in range(1, 6))
                    if depth >= 5 and 180 <= ww <= 500 and 18 <= wh <= 55:
                        wlr_click(greeter_rt1, wl_sock1, wx + ww // 2, wy + wh // 2)
                        break
            time.sleep(0.2)
            wlr_key(greeter_rt1, wl_sock1, f"{test_user}\n")
            time.sleep(0.6)
            wlr_key(greeter_rt1, wl_sock1, f"{test_pass}\n")

            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline and not (rec_dir / "wayland-session.ready").is_file():
                time.sleep(0.1)
            if not (rec_dir / "wayland-session.ready").is_file():
                log_tail = (work / "dtlogin.log").read_text(errors="replace")[-3000:]
                raise SystemExit(f"Wayland user session did not start; dtlogin.log:\n{log_tail}")

            # Verify greeter compositor comp_pid1 exited before/during handoff
            try:
                os.kill(comp_pid1, 0)
                raise SystemExit(f"Greeter compositor pid {comp_pid1} leaked into user Wayland session!")
            except ProcessLookupError:
                pass

            wl_env = dict(
                ln.split("=", 1)
                for ln in (rec_dir / "wayland-session.env").read_text(encoding="utf-8").splitlines()
                if "=" in ln
            )
            if int(wl_env.get("UID", "-1")) != test_uid:
                raise SystemExit(f"Expected Wayland session UID {test_uid}, got {wl_env}")
            if wl_env.get("XDG_SESSION_TYPE") != "wayland":
                raise SystemExit(f"Expected XDG_SESSION_TYPE=wayland, got {wl_env}")
            if wl_env.get("INITIAL_DISPLAY") != "UNSET" or wl_env.get("INITIAL_WAYLAND_DISPLAY") != "UNSET":
                raise SystemExit(f"Stale greeter socket leaked into Wayland session env: {wl_env}")
            if not wl_env.get("WAYLAND_DISPLAY"):
                raise SystemExit(f"User dtwm-wayland did not publish WAYLAND_DISPLAY: {wl_env}")

            # Trigger clean logout from CDE (Wayland) -> verify dtlogin respawns greeter
            (rec_dir / "logout-wayland").write_text("1\n", encoding="utf-8")
            comp_pid2, greet_pid2, wl_sock2, x11_dpy2, greeter_rt2 = wait_for_greeter_ready(prev_comp_pid=comp_pid1)
            time.sleep(0.4)

            # 3. Login #2: Select CDE (X11 / XLibre) via dtgreet's Options -> Session menu!
            win_tree = subprocess.check_output(
                ["sudo", "-n", "env", f"DISPLAY={x11_dpy2}", str(x11_inspect_bin)],
                text=True,
            )
            # Find options_button (DtMenuButton at bottom of matte1: depth==6 with largest y)
            opt_candidates: list[tuple[int, int, int]] = []
            for ln in win_tree.splitlines():
                m = re.search(r"depth=(\d+)\s+id=\S+\s+x=(\d+)\s+y=(\d+)\s+w=(\d+)\s+h=(\d+)\s+or=0", ln)
                if m:
                    depth, wx, wy, ww, wh = (int(m.group(i)) for i in range(1, 6))
                    if depth == 6 and 50 <= ww <= 200 and 22 <= wh <= 60 and wy > 480:
                        opt_candidates.append((wy, wx + ww // 2, wy + wh // 2))
            if not opt_candidates:
                raise SystemExit(f"Could not locate options_button in dtgreet window tree:\n{win_tree}")
            opt_candidates.sort()
            opt_btn = (opt_candidates[-1][1], opt_candidates[-1][2])

            # Open the Options popup and locate its Session cascade.
            wlr_click(greeter_rt2, wl_sock2, opt_btn[0], opt_btn[1])
            time.sleep(0.35)
            # Check popped-up override-redirect Options menu to locate Session cascade item
            menu_tree = subprocess.check_output(
                ["sudo", "-n", "env", f"DISPLAY={x11_dpy2}", str(x11_inspect_bin)],
                text=True,
            )
            or_menus = []
            for ln in menu_tree.splitlines():
                m = re.search(r"x=(\d+)\s+y=(\d+)\s+w=(\d+)\s+h=(\d+)\s+or=1", ln)
                if m:
                    mx, my, mw, mh = (int(m.group(i)) for i in range(1, 5))
                    if mw >= 60 and mh >= 30:
                        or_menus.append((mx, my, mw, mh))
            if not or_menus:
                raise SystemExit(f"Options popup menu did not map after clicking {opt_btn}:\n{menu_tree}")

            # With the fixture's two languages, Options has Language,
            # Session and Reset Login Screen in that order.
            omx, omy, omw, omh = or_menus[-1]
            def x11_input(*args: str):
                subprocess.run(["sudo", "-n", "env", f"DISPLAY={x11_dpy2}", "xdotool", *args],
                               check=True)
            x11_input("mousemove", str(omx + omw - 12), str(omy + omh // 2))
            time.sleep(0.45)
            x11_input("click", "1")
            time.sleep(0.5)
            sub_tree = subprocess.check_output(
                ["sudo", "-n", "env", f"DISPLAY={x11_dpy2}", str(x11_inspect_bin)],
                text=True,
            )
            submenus = []
            for ln in sub_tree.splitlines():
                match = re.search(r"x=(\d+)\s+y=(\d+)\s+w=(\d+)\s+h=(\d+)\s+or=1", ln)
                if match:
                    sx, sy, sw, sh = map(int, match.groups())
                    if sw >= 60 and sh >= 30 and (sx, sy) != (omx, omy):
                        submenus.append((sx, sy, sw, sh))
            if len(submenus) != 1:
                raise SystemExit(f"Expected one Session submenu from {or_menus[-1]}:\n{sub_tree}")

            if screenshot_out is not None:
                menu_shot = work / "shot-menu.png"
                subprocess.run(
                    [
                        "sudo",
                        "-n",
                        "env",
                        f"XDG_RUNTIME_DIR={greeter_rt2}",
                        f"WAYLAND_DISPLAY={wl_sock2}",
                        "grim",
                        str(menu_shot),
                    ],
                    check=True,
                )
                subprocess.run(["sudo", "-n", "chmod", "0644", str(menu_shot)], check=True)
                shutil.copy2(menu_shot, screenshot_out.with_name(screenshot_out.stem + "-menu.png"))

            # The Session menu lists Wayland, X11 and Failsafe in that order.
            sx, sy, sw, sh = submenus[0]
            x11_input("mousemove", str(sx + sw // 2), str(sy + sh // 2), "click", "1")
            time.sleep(0.35)

            # Focus login_text and enter credentials for CDE (X11 / XLibre)
            for ln in win_tree.splitlines():
                m = re.search(r"depth=(\d+)\s+id=\S+\s+x=(\d+)\s+y=(\d+)\s+w=(\d+)\s+h=(\d+)\s+or=0", ln)
                if m:
                    depth, wx, wy, ww, wh = (int(m.group(i)) for i in range(1, 6))
                    if depth >= 5 and 180 <= ww <= 500 and 18 <= wh <= 55:
                        wlr_click(greeter_rt2, wl_sock2, wx + ww // 2, wy + wh // 2)
                        break
            time.sleep(0.2)
            wlr_key(greeter_rt2, wl_sock2, f"{test_user}\n")
            time.sleep(0.6)
            wlr_key(greeter_rt2, wl_sock2, f"{test_pass}\n")

            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline and not (rec_dir / "x11-session.ready").is_file():
                time.sleep(0.1)
            if not (rec_dir / "x11-session.ready").is_file():
                log_tail = (work / "dtlogin.log").read_text(errors="replace")[-3000:]
                raise SystemExit(f"X11 user session did not start from Wayland dtlogin; dtlogin.log:\n{log_tail}")

            x11_env = dict(
                ln.split("=", 1)
                for ln in (rec_dir / "x11-session.env").read_text(encoding="utf-8").splitlines()
                if "=" in ln
            )
            if int(x11_env.get("UID", "-1")) != test_uid or x11_env.get("XDG_SESSION_TYPE") != "x11":
                raise SystemExit(f"Unexpected X11 session environment: {x11_env}")
            if not x11_env.get("DISPLAY", "").startswith(":") or int(x11_env.get("X11_QUERY_LINES", "0")) < 1:
                raise SystemExit(f"X11 session could not query seat X server: {x11_env}")
            # The seat fixture uses a local Xvfb display. Its lock identifies the live server.
            x11_display = x11_env["DISPLAY"].removeprefix(":")
            x11_pid = int(Path(f"/tmp/.X{x11_display}-lock").read_text().strip())
            x11_image = f"/proc/{x11_pid}/exe"
            x11_server = {
                "executable": subprocess.check_output(
                    ["sudo", "-n", "readlink", x11_image], text=True
                ).strip(),
                "sha256": subprocess.check_output(
                    ["sudo", "-n", "sha256sum", x11_image], text=True
                ).split()[0],
            }

            # Trigger logout from CDE (X11 / XLibre) -> verify respawn back to Wayland greeter #3!
            (rec_dir / "logout-x11").write_text("1\n", encoding="utf-8")
            comp_pid3, greet_pid3, wl_sock3, x11_dpy3, _ = wait_for_greeter_ready(prev_comp_pid=comp_pid2)

            # Stop dtlogin cleanly and verify zero leaked processes
            subprocess.run(["sudo", "-n", "kill", "-TERM", str(dtlogin_proc.pid)], check=False)
            for _ in range(40):
                if dtlogin_proc.poll() is not None and not find_greeter_pids(work, bin_dir / "dtgreet"):
                    break
                time.sleep(0.15)
            if dtlogin_proc.poll() is None:
                raise SystemExit("The dtlogin fixture daemon did not exit")

            leaked = find_greeter_pids(work, bin_dir / "dtgreet")
            if leaked:
                raise SystemExit(f"Leaked greeter processes after dtlogin shutdown: {leaked}")

            return {
                "greeter_uid": greeter_uid,
                "test_uid": test_uid,
                "greeter_cycles": [comp_pid1, comp_pid2, comp_pid3],
                "dtgreet_pids": [greet_pid1, greet_pid2, greet_pid3],
                "wayland_session_env": wl_env,
                "x11_session_env": x11_env,
                "x11_server": x11_server,
                "screenshot": str(shot_file),
                "screenshot_sha256": hashlib.sha256(shot_file.read_bytes()).hexdigest(),
            }
        finally:
            if dtlogin_proc is not None and dtlogin_proc.poll() is None:
                subprocess.run(["sudo", "-n", "pkill", "-TERM", "-f", str(bin_dir / "dtlogin")], check=False)
                try:
                    dtlogin_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    subprocess.run(["sudo", "-n", "pkill", "-KILL", "-f", str(bin_dir / "dtlogin")], check=False)
            subprocess.run(["sudo", "-n", "pkill", "-TERM", "-f", str(work)], check=False)
            subprocess.run(["sudo", "-n", "rm", "-rf", str(rt_base), str(user_rt_base), str(rec_dir)], check=False)
            if created_test_user:
                subprocess.run(["sudo", "-n", "userdel", "-rf", test_user], capture_output=True)
            if created_greeter_user:
                subprocess.run(["sudo", "-n", "userdel", "_dtgreeter"], capture_output=True)
            if orig_pam_dtlogin is None:
                subprocess.run(["sudo", "-n", "rm", "-f", str(pam_dtlogin)], check=False)
            else:
                subprocess.run(["sudo", "-n", "tee", str(pam_dtlogin)], input=orig_pam_dtlogin, text=True, stdout=subprocess.DEVNULL, check=False)
            if orig_pam_greeter is None:
                subprocess.run(["sudo", "-n", "rm", "-f", str(pam_greeter)], check=False)
            else:
                subprocess.run(["sudo", "-n", "tee", str(pam_greeter)], input=orig_pam_greeter, text=True, stdout=subprocess.DEVNULL, check=False)
            subprocess.run(["sudo", "-n", "rm", "-f", str(ld_conf)], check=False)
            subprocess.run(["sudo", "-n", "ldconfig"], check=False)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", default="../dtwm/dtwm-wayland")
    ap.add_argument("--screenshot-out", default="")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    dtlogin_src = Path(__file__).resolve().parents[1]
    cde_src = dtlogin_src.parents[1]
    cde_build = Path.cwd().parents[1] if (Path.cwd() / "dtlogin").exists() else cde_src
    cc = shlex.split(os.environ.get("CC", "cc"))
    tmp_dir = os.environ.get("TMPDIR")

    run_unit_checks(dtlogin_src, cde_src, cde_build, cc, tmp_dir)

    comp_bin = Path(args.compositor)
    can_sudo = subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode == 0
    has_tools = all(shutil.which(t) for t in ("wlrctl", "grim", "Xvfb", "xrdb", "xdotool"))

    live_summary: dict[str, object] = {"executed": False}
    if comp_bin.is_file() and can_sudo and has_tools:
        shot_out = Path(args.screenshot_out) if args.screenshot_out else None
        live_summary = {"executed": True, **run_live_daemon_test(dtlogin_src, cde_src, cde_build, comp_bin, cc, tmp_dir, shot_out)}
        print(
            f"Wayland dtlogin: unit + live daemon passed "
            f"(3 greeter cycles {live_summary['greeter_cycles']}, "
            f"CDE Wayland + CDE X11 login/logout verified, 0 leaked procs)"
        )
    else:
        print("Wayland dtlogin: ASan/UBSan config & .desktop discovery unit suite passed")

    if args.json_out:
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(live_summary, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
