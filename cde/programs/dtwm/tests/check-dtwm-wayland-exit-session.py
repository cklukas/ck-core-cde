#!/usr/bin/env python3
"""Automated regression test for Wayland ExitSession & compositor shutdown:
1. Direct _DT_WM_REQUEST `f.quit_mwm` cleanly terminates `dtwm-wayland` (exit 0).
2. External-compositor `dtwm-wayland` + `dtwm-panel` + `dtsession --wayland` session:
   ToolTalk `XSession_Exit` (`ExitSession`) is handled by `dtsession` (`TT_HANDLE_PUSH`
   priority over `dtwm-panel`'s `TT_HANDLE` fallback), exits `dtsession` cleanly, and
   terminates the externally-started `dtwm-wayland` via `f.quit_mwm` (exit 0).
3. Degraded / standalone session where `dtsession` has already exited (leaving a stale
   `_DT_SM_WINDOW_INFO` XID on root while `dtwm-wayland`, `dtwm-panel`, and `ttsession`
   stay alive): ToolTalk `XSession_Exit` succeeds (`TT_HANDLED`, no `TT_ERR_NO_MATCH`)
   via `dtwm-panel`'s `TT_HANDLE` fallback and terminates `dtwm-wayland` cleanly (exit 0).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
import time
from pathlib import Path


HELPER_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <Dt/SessionM.h>
#include <Dt/SvcTT.h>
#include <Tt/tttk.h>

static int g_xerr = 0;
static int trap_err(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    g_xerr = 1;
    return 0;
}

static Window find_mwm_win(Display *dpy) {
    Window root = DefaultRootWindow(dpy);
    Atom mwm_info = XInternAtom(dpy, "_MOTIF_WM_INFO", True);
    if (mwm_info == None) return None;
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *data = NULL;
    Window win = None;
    if (XGetWindowProperty(dpy, root, mwm_info, 0, 2, False, mwm_info,
                           &actual, &fmt, &nitems, &after, &data) == Success && data) {
        if (fmt == 32 && nitems >= 2) {
            const long *vals = (const long *)data;
            win = (Window)vals[1];
        }
        XFree(data);
    }
    return win;
}

static int g_sent_exit = 0;
static int g_tt_done = 0;
static int g_tt_failed = 0;
static Tt_status g_tt_status = TT_OK;

static int on_xio_shutdown(Display *dpy) {
    (void)dpy;
    if (g_sent_exit && !g_tt_failed) {
        printf("X_SHUTDOWN_AFTER_EXIT_REQUEST\n");
        fflush(stdout);
        _exit(0);
    }
    _exit(1);
}

static Tt_callback_action on_tt_reply(Tt_message m, Tt_pattern p) {
    (void)p;
    Tt_state st = tt_message_state(m);
    if (st == TT_HANDLED) {
        g_tt_done = 1;
        g_tt_failed = 0;
        g_tt_status = tt_message_status(m);
        tttk_message_destroy(m);
        return TT_CALLBACK_PROCESSED;
    }
    if (st == TT_FAILED) {
        g_tt_done = 1;
        g_tt_failed = 1;
        g_tt_status = tt_message_status(m);
        tttk_message_destroy(m);
        return TT_CALLBACK_PROCESSED;
    }
    return TT_CALLBACK_CONTINUE;
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "";

    if (strcmp(mode, "quit_mwm") == 0) {
        Display *dpy = XOpenDisplay(NULL);
        if (!dpy) return 2;
        Window wm_win = None;
        for (int i = 0; i < 50 && wm_win == None; ++i) {
            wm_win = find_mwm_win(dpy);
            if (wm_win == None) usleep(50000);
        }
        if (wm_win == None) {
            fprintf(stderr, "missing _MOTIF_WM_INFO\n");
            XCloseDisplay(dpy);
            return 3;
        }
        Atom req_a = XInternAtom(dpy, "_DT_WM_REQUEST", False);
        static const char cmd[] = "f.quit_mwm";
        XChangeProperty(dpy, wm_win, req_a, XA_STRING, 8, PropModeAppend,
                        (const unsigned char *)cmd, (int)sizeof(cmd));
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(mode, "wait_sm_and_set_quiet") == 0) {
        Display *dpy = XOpenDisplay(NULL);
        if (!dpy) return 2;
        Window root = DefaultRootWindow(dpy);
        Window sm_win = None;
        for (int i = 0; i < 80; ++i) {
            if (_DtGetSmWindow(dpy, root, &sm_win) == Success && sm_win != None) {
                XWindowAttributes wa;
                g_xerr = 0;
                int (*prev)(Display *, XErrorEvent *) = XSetErrorHandler(trap_err);
                Status ok = XGetWindowAttributes(dpy, sm_win, &wa);
                XSync(dpy, False);
                XSetErrorHandler(prev);
                if (ok && !g_xerr) break;
            }
            sm_win = None;
            usleep(100000);
        }
        if (sm_win == None) {
            fprintf(stderr, "timed out waiting for live dtsession window\n");
            XCloseDisplay(dpy);
            return 3;
        }
        SmStateInfo st;
        memset(&st, 0, sizeof(st));
        st.flags = SM_STATE_CONFIRM | SM_STATE_START;
        st.smConfirmMode = DtSM_QUIET_MODE;
        st.smStartState = DtSM_CURRENT_STATE;
        _DtSetSmState(dpy, sm_win, &st);
        XSync(dpy, False);
        usleep(150000);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(mode, "plant_stale_sm_win") == 0) {
        Display *dpy = XOpenDisplay(NULL);
        if (!dpy) return 2;
        Window root = DefaultRootWindow(dpy);
        Window dummy = XCreateSimpleWindow(dpy, root, -100, -100, 4, 4, 0, 0, 0);
        Atom sm_info = XInternAtom(dpy, _XA_DT_SM_WINDOW_INFO, False);
        unsigned long prop[2] = {0UL, (unsigned long)dummy};
        XChangeProperty(dpy, root, sm_info, sm_info, 32, PropModeReplace,
                        (const unsigned char *)prop, 2);
        XDestroyWindow(dpy, dummy);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(mode, "send_xsession_exit") == 0) {
        XtAppContext app;
        Widget top = XtVaAppInitialize(&app, "ExitProbe", NULL, 0, &argc, argv, NULL,
                                       XtNmappedWhenManaged, False,
                                       XtNwidth, 4, XtNheight, 4, NULL);
        XtRealizeWidget(top);
        Display *dpy = XtDisplay(top);
        Window root = DefaultRootWindow(dpy);
        Atom sun_tt = XInternAtom(dpy, "_SUN_TT_SESSION", True);
        if (sun_tt != None && (!getenv("TT_SESSION") || !getenv("TT_SESSION")[0])) {
            Atom act = None;
            int fmt = 0;
            unsigned long nitems = 0, rem = 0;
            unsigned char *val = NULL;
            if (XGetWindowProperty(dpy, root, sun_tt, 0, 256, False, AnyPropertyType,
                                   &act, &fmt, &nitems, &rem, &val) == Success && val && nitems > 0) {
                char buf[512];
                size_t cp = nitems < sizeof(buf) - 1 ? (size_t)nitems : sizeof(buf) - 1;
                memcpy(buf, val, cp);
                buf[cp] = '\0';
                setenv("TT_SESSION", buf, 1);
            }
            if (val) XFree(val);
        }
        if (_DtSvcInitToolTalk(top) != TT_OK) {
            fprintf(stderr, "_DtSvcInitToolTalk failed\n");
            return 4;
        }
        Tt_message req = tttk_message_create(
            NULL, TT_REQUEST, TT_SESSION, NULL, "XSession_Exit", on_tt_reply
        );
        if (tt_ptr_error(req) != TT_OK || !req) {
            fprintf(stderr, "tttk_message_create failed\n");
            return 5;
        }
        XSetIOErrorHandler(on_xio_shutdown);
        g_sent_exit = 1;
        Tt_status sst = tt_message_send(req);
        if (sst != TT_OK) {
            fprintf(stderr, "tt_message_send failed: %d (%s)\n", (int)sst, tt_status_message(sst));
            return 6;
        }
        for (int i = 0; i < 200 && !g_tt_done; ++i) {
            while (XtAppPending(app)) {
                XEvent ev;
                XtAppNextEvent(app, &ev);
                XtDispatchEvent(&ev);
            }
            usleep(25000);
        }
        if (!g_tt_done) {
            fprintf(stderr, "timed out waiting for XSession_Exit reply\n");
            return 7;
        }
        if (g_tt_failed) {
            fprintf(stderr, "XSession_Exit TT_FAILED: %d (%s)\n",
                    (int)g_tt_status, tt_status_message(g_tt_status));
            return 8;
        }
        printf("TT_HANDLED status=%d\n", (int)g_tt_status);
        return 0;
    }

    fprintf(stderr, "unknown mode: %s\n", mode);
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
    inc, lib = [], []
    if Path("/usr/include/tirpc").is_dir():
        inc.append("-I/usr/include/tirpc")
    if not makefile.is_file():
        return inc, lib
    for ln in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
        if ln.startswith(("X_CFLAGS =", "CFLAGS =", "DTCLIENTLIBS =", "CPPFLAGS =", "LDFLAGS =")):
            for tok in ln.split("=", 1)[1].split():
                if tok.startswith("-I"):
                    inc.append(tok)
                elif tok.startswith("-L") or tok.startswith("-Wl,-rpath"):
                    lib.append(tok)
    return inc, lib


def resolve_tool(candidates: list[Path]) -> Path | None:
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def wait_status_env(status_file: Path, timeout: float = 10.0) -> dict[str, str]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if status_file.is_file():
            kv: dict[str, str] = {}
            for ln in status_file.read_text(encoding="utf-8", errors="replace").splitlines():
                if "=" in ln:
                    k, v = ln.split("=", 1)
                    kv[k.strip()] = v.strip()
            if kv.get("WAYLAND_DISPLAY") and kv.get("DISPLAY"):
                return kv
        time.sleep(0.05)
    raise SystemExit(f"timed out waiting for status file {status_file}")


def wait_clean_exit(proc: subprocess.Popen[bytes], label: str, log_path: Path, timeout: float = 8.0) -> None:
    try:
        rc = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        proc.kill()
        log_txt = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else ""
        raise SystemExit(f"{label} did not exit within {timeout}s!\nLog:\n{log_txt}") from exc
    log_txt = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else ""
    if rc != 0:
        raise SystemExit(f"{label} exited with non-zero code {rc}!\nLog:\n{log_txt}")
    if "Assertion `" in log_txt:
        raise SystemExit(f"{label} hit assertion during shutdown!\nLog:\n{log_txt}")


def make_isolated_env(case_dir: Path, panel_bin: Path, extra_path_dirs: list[Path], ld_dirs: list[Path]) -> dict[str, str]:
    xdg = case_dir / "xdg"
    home = case_dir / "home"
    (home / ".dt" / "sessions").mkdir(parents=True, exist_ok=True)
    (home / ".dt" / "types").mkdir(parents=True, exist_ok=True)
    xdg.mkdir(mode=0o700, exist_ok=True)
    os.chmod(xdg, 0o700)

    env = os.environ.copy()
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    env.pop("TT_SESSION", None)
    env.pop("SESSION_MANAGER", None)
    env["HOME"] = str(home)
    env["XDG_RUNTIME_DIR"] = str(xdg)
    env["WLR_BACKENDS"] = "headless"
    env["WLR_RENDERER"] = "pixman"
    env["WLR_HEADLESS_OUTPUTS"] = "1"
    env["WLR_LIBINPUT_NO_DEVICES"] = "1"
    env["DTWM_PANEL_BIN"] = str(panel_bin)
    env["DTUSERSESSION"] = f"exittest-{os.getpid()}-{case_dir.name}"

    path_parts = [str(d) for d in extra_path_dirs if d.is_dir()] + ["/usr/dt/bin", env.get("PATH", "/usr/bin:/bin")]
    env["PATH"] = ":".join(path_parts)

    ld_parts = [str(d) for d in ld_dirs if d.is_dir()]
    if env.get("LD_LIBRARY_PATH"):
        ld_parts.append(env["LD_LIBRARY_PATH"])
    if ld_parts:
        env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
    return env


def kill_proc_group(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, 15)
    except OSError:
        proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, 9)
        except OSError:
            proc.kill()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True)
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    cde_build, cde_src = find_cde_build_and_src(comp_bin)
    inc_flags, lib_flags = extract_motif_dirs(comp_bin.parent / "Makefile")
    cc = os.environ.get("CC", "cc")

    panel_bin = resolve_tool([
        comp_bin.parent / ".libs" / "dtwm-panel",
        comp_bin.parent / "dtwm-panel",
        Path("/usr/dt/bin/dtwm-panel"),
    ])
    dtsession_bin = resolve_tool([
        cde_build / "programs" / "dtsession" / ".libs" / "dtsession",
        cde_build / "programs" / "dtsession" / "dtsession",
        Path("/usr/dt/bin/dtsession"),
    ])
    ttsession_bin = resolve_tool([
        cde_build / "lib" / "tt" / "bin" / "ttsession" / ".libs" / "ttsession",
        cde_build / "lib" / "tt" / "bin" / "ttsession" / "ttsession",
        Path("/usr/dt/bin/ttsession"),
    ])
    if not panel_bin:
        raise SystemExit("dtwm-panel binary not found")

    ld_dirs = [
        cde_build / "lib" / "DtSvc" / ".libs",
        cde_build / "lib" / "tt" / "lib" / ".libs",
        cde_build / "lib" / "DtWidget" / ".libs",
        cde_build / "lib" / "DtHelp" / ".libs",
        cde_build / "lib" / "DtPrint" / ".libs",
        cde_build / "lib" / "DtXinerama" / ".libs",
        cde_build / "lib" / "csa" / ".libs",
    ]
    for tok in lib_flags:
        if tok.startswith("-L") and "$(" not in tok:
            ld_dirs.append(Path(tok[2:]))

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="dtwm-wl-exit-", dir=tmp_base) as work_s:
        work = Path(work_s)
        helper_c = work / "exit_helper.c"
        helper_bin = work / "exit_helper"
        helper_c.write_text(HELPER_C, encoding="utf-8")

        dtsvc_lib = cde_build / "lib" / "DtSvc" / ".libs"
        if not dtsvc_lib.is_dir():
            dtsvc_lib = Path("/usr/dt/lib")
        tt_lib = cde_build / "lib" / "tt" / "lib" / ".libs"
        if not tt_lib.is_dir():
            tt_lib = Path("/usr/dt/lib")
        dtxin_lib = cde_build / "lib" / "DtXinerama" / ".libs"
        if not dtxin_lib.is_dir():
            dtxin_lib = Path("/usr/dt/lib")

        compile_cmd = [
            cc, "-O2", str(helper_c),
            f"-I{cde_build / 'include'}", f"-I{cde_src / 'include'}", "-I/usr/dt/include",
            *[f for f in inc_flags if "$(" not in f],
            f"-L{dtsvc_lib}", f"-Wl,-rpath,{dtsvc_lib}",
            f"-L{tt_lib}", f"-Wl,-rpath,{tt_lib}",
            f"-L{dtxin_lib}", f"-Wl,-rpath,{dtxin_lib}",
            *[f for f in lib_flags if "$(" not in f],
            "-lDtSvc", "-ltt", "-lDtXinerama", "-lXm", "-lXt", "-lX11",
            "-o", str(helper_bin),
        ]
        subprocess.run(compile_cmd, check=True)

        extra_bins = [p.parent for p in (panel_bin, dtsession_bin, ttsession_bin) if p is not None]

        # --- Case 1: Direct f.quit_mwm terminates dtwm-wayland cleanly ---
        c1 = work / "case1"
        env1 = make_isolated_env(c1, panel_bin, extra_bins, ld_dirs)
        st1 = c1 / "status"
        log1 = c1 / "comp.log"
        p_comp1 = None
        try:
            with log1.open("wb") as lf:
                p_comp1 = subprocess.Popen(
                    [str(comp_bin), "--no-panel", "--status-file", str(st1)],
                    env=env1, stdout=lf, stderr=subprocess.STDOUT, start_new_session=True,
                )
            kv1 = wait_status_env(st1)
            env1.update(kv1)
            subprocess.run([str(helper_bin), "quit_mwm"], env=env1, check=True)
            wait_clean_exit(p_comp1, "Case 1 dtwm-wayland (f.quit_mwm)", log1)
        finally:
            kill_proc_group(p_comp1)

        # --- Case 2: External dtwm-wayland + dtwm-panel + dtsession --wayland ---
        if dtsession_bin and ttsession_bin:
            c2 = work / "case2"
            env2 = make_isolated_env(c2, panel_bin, extra_bins, ld_dirs)
            st2 = c2 / "status"
            log2_comp = c2 / "comp.log"
            log2_sm = c2 / "sm.log"
            p_comp2 = None
            p_sm2 = None
            try:
                with log2_comp.open("wb") as lf:
                    p_comp2 = subprocess.Popen(
                        [str(comp_bin), "--status-file", str(st2)],
                        env=env2, stdout=lf, stderr=subprocess.STDOUT, start_new_session=True,
                    )
                kv2 = wait_status_env(st2)
                env2.update(kv2)
                tt_res2 = subprocess.run([str(ttsession_bin), "-s"], env=env2, check=False)
                if tt_res2.returncode not in (0, 2):
                    raise SystemExit(f"ttsession -s failed with rc={tt_res2.returncode}")
                with log2_sm.open("wb") as sf:
                    p_sm2 = subprocess.Popen(
                        [str(dtsession_bin), "--wayland"],
                        env=env2, stdout=sf, stderr=subprocess.STDOUT, start_new_session=True,
                    )
                subprocess.run([str(helper_bin), "wait_sm_and_set_quiet"], env=env2, check=True)
                # Allow dtwm-panel to register its TT_HANDLE fallback too so TT_HANDLE_PUSH precedence is exercised
                time.sleep(0.8)
                subprocess.run([str(helper_bin), "send_xsession_exit"], env=env2, check=True)
                wait_clean_exit(p_sm2, "Case 2 dtsession --wayland", log2_sm, timeout=10.0)
                wait_clean_exit(p_comp2, "Case 2 external dtwm-wayland", log2_comp, timeout=10.0)
            finally:
                kill_proc_group(p_sm2)
                kill_proc_group(p_comp2)

        # --- Case 3: Degraded session (dtsession already exited, stale _DT_SM_WINDOW_INFO on root) ---
        if ttsession_bin:
            c3 = work / "case3"
            env3 = make_isolated_env(c3, panel_bin, extra_bins, ld_dirs)
            st3 = c3 / "status"
            log3_comp = c3 / "comp.log"
            p_comp3 = None
            try:
                with log3_comp.open("wb") as lf:
                    p_comp3 = subprocess.Popen(
                        [str(comp_bin), "--status-file", str(st3)],
                        env=env3, stdout=lf, stderr=subprocess.STDOUT, start_new_session=True,
                    )
                kv3 = wait_status_env(st3)
                env3.update(kv3)
                tt_res3 = subprocess.run([str(ttsession_bin), "-s"], env=env3, check=False)
                if tt_res3.returncode not in (0, 2):
                    raise SystemExit(f"ttsession -s failed with rc={tt_res3.returncode}")
                subprocess.run([str(helper_bin), "plant_stale_sm_win"], env=env3, check=True)
                # Wait for dtwm-panel's retry_tt_wsm_timer_cb to register TT_HANDLE fallback on ttsession
                time.sleep(1.5)
                subprocess.run([str(helper_bin), "send_xsession_exit"], env=env3, check=True)
                wait_clean_exit(p_comp3, "Case 3 degraded dtwm-wayland (no dtsession)", log3_comp, timeout=10.0)
            finally:
                kill_proc_group(p_comp3)

        print("PASS: check-dtwm-wayland-exit-session verified (f.quit_mwm, external dtsession logout, degraded no-dtsession fallback)")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
