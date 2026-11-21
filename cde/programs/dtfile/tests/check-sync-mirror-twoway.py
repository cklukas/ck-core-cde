#!/usr/bin/env python3
"""Parallel Wayland + X11 integration check for dtfile_sync Mirror & Two-Way Sync:
1. Runtime linkage check:
   - Verifies live `dtfile_sync` maps `libXm.so.9` and `libck-core-fs-lib.so.0.5`.
2. Mirror (`CKFSP_MIRROR`, profile:5):
   - Verifies review without `approve_deletions` keeps `Apply reviewed plan` disabled.
   - Enables `approve_deletions:1` + `Use source` (`rule:0:1`) with retained replacement,
     reviews, executes `Apply reviewed plan`, and verifies destination-only files and
     folders are removed, source updates and new subfolders are materialized, and the
     replaced version is retained on disk.
3. Synchronize two ways (`SYNC_PROFILE_TWO_WAY`, profile:6):
   - Gen 0 -> Gen 1: Verifies review without `approve_first_run` keeps `Apply` disabled,
     then approves initial baseline (`approve_first_run:1`) and executes bidirectional
     recursive folder synchronization (`docs/readme.txt` <-> `notes/todo.txt`).
   - Gen 1 -> Gen 2: Edits `both.txt` concurrently on both sides and deletes `notes/`
     on Right, resolves `Both edited` (`kind=1`) with `Keep Right` (`choice=18`),
     executes `Apply reviewed plan`, and verifies `tw_gen=2`, conflict convergence,
     retained version preservation, and tracked directory deletion propagation.
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


X11_SYNC_HELPER_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 3;
    Window root = DefaultRootWindow(dpy);

    if (strcmp(argv[1], "props") == 0) {
        Atom a = XInternAtom(dpy, "_DT_SYNC_STATE", False);
        Atom act = None;
        int fmt = 0;
        unsigned long nitems = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, a, 0, 8192, False, AnyPropertyType,
                               &act, &fmt, &nitems, &rem, &data) == Success && data) {
            if (fmt == 8 && nitems > 0) {
                fwrite(data, 1, nitems, stdout);
                fputc('\n', stdout);
            }
            XFree(data);
        }
        XCloseDisplay(dpy);
        return 0;
    }

    if (strcmp(argv[1], "cmd") == 0 && argc >= 3) {
        Atom a = XInternAtom(dpy, "_DT_SYNC_CMD", False);
        XChangeProperty(dpy, root, a, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)argv[2], (int)strlen(argv[2]));
        XSync(dpy, False);
        if (strcmp(argv[2], "close") != 0) {
            for (int i = 0; i < 200; ++i) {
                Atom act = None;
                int fmt = 0;
                unsigned long nitems = 0, rem = 0;
                unsigned char *data = NULL;
                int rc = XGetWindowProperty(dpy, root, a, 0, 64, False, AnyPropertyType,
                                            &act, &fmt, &nitems, &rem, &data);
                if (data) XFree(data);
                if (rc != Success || act == None) break;
                struct timespec ts = {0, 15000000};
                nanosleep(&ts, NULL);
            }
        }
        XCloseDisplay(dpy);
        return 0;
    }

    XCloseDisplay(dpy);
    return 1;
}
"""


def wait_for(pred, timeout: float = 15.0, interval: float = 0.08, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_val = None
    while time.monotonic() < deadline:
        last_val = pred()
        if last_val:
            return last_val
        time.sleep(interval)
    raise RuntimeError(f"Timed out waiting for {desc} (last={last_val!r})")


def read_sync_state(helper: Path, env: dict[str, str]) -> dict[str, str]:
    cp = subprocess.run([str(helper), "props"], env=env, capture_output=True, text=True, check=True)
    out: dict[str, str] = {}
    for tok in cp.stdout.strip().split("|"):
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def send_sync_cmd(helper: Path, env: dict[str, str], cmd: str) -> None:
    before = int(read_sync_state(helper, env).get("cmd_seq", "0"))
    subprocess.run([str(helper), "cmd", cmd], env=env, check=True)
    if cmd != "close":
        wait_for(
            lambda: int(read_sync_state(helper, env).get("cmd_seq", "0")) > before,
            timeout=5.0,
            interval=0.03,
            desc=f"cmd_seq advance for {cmd!r}",
        )


def clear_sync_state(helper: Path, env: dict[str, str]) -> None:
    subprocess.run(
        ["xprop", "-root", "-remove", "_DT_SYNC_STATE"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
    )


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


def maybe_capture_screenshot(lane: str, env: dict[str, str], shot_dir: Path | None, name: str) -> None:
    if not shot_dir:
        return
    shot_dir.mkdir(parents=True, exist_ok=True)
    dest = shot_dir / f"{name}_{lane}.png"
    if lane == "wayland" and shutil.which("grim"):
        subprocess.run(["grim", str(dest)], env=env, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif shutil.which("import"):
        subprocess.run(["import", "-window", "root", str(dest)], env=env, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_lane(
    lane: str,
    disp_idx: int,
    comp_bin: Path,
    dtwm_x11_bin: Path,
    dtsync_bin: Path,
    helper_bin: Path,
    base_tmp: Path,
    shot_dir: Path | None,
) -> str:
    lane_root = base_tmp / f"lane-{lane}"
    lane_root.mkdir(parents=True, exist_ok=True)
    lane_root.chmod(0o700)
    home = lane_root / "home"
    xdg_rt = lane_root / "runtime"
    for d in (home, xdg_rt, home / ".local/share", home / ".cache", home / ".config", home / ".dt"):
        d.mkdir(parents=True, exist_ok=True)
        d.chmod(0o700)

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
            "XDG_DATA_HOME": str(home / ".local/share"),
            "XDG_CACHE_HOME": str(home / ".cache"),
            "XDG_CONFIG_HOME": str(home / ".config"),
            "NLSPATH": "/var/tmp/ck-wayland-build/wp00/cde-stage/usr/dt/lib/nls/msg/C/%N.cat:/usr/dt/lib/nls/msg/C/%N.cat",
            "LC_ALL": "C.UTF-8",
            "LANG": "C.UTF-8",
        })

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
        else:
            xdisp = f":{disp_idx}"
            xvfb_proc = subprocess.Popen(
                ["Xvfb", xdisp, "-screen", "0", "1280x900x24", "-nolisten", "tcp"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(xvfb_proc)
            env["DISPLAY"] = xdisp
            wait_for(lambda: subprocess.run([str(helper_bin), "props"], env=env,
                                            capture_output=True).returncode == 0,
                     timeout=8.0, desc="Xvfb ready")
            dtwm_proc = subprocess.Popen(
                [str(dtwm_x11_bin)],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(dtwm_proc)
            time.sleep(0.5)

        # ---- PART 1: Mirror (`CKFSP_MIRROR`, profile:5) ----
        mir_src = home / "MirrorSrc"
        mir_dst = home / "MirrorDst"
        (mir_src / "keep_sub").mkdir(parents=True)
        (mir_dst / "obsolete_sub").mkdir(parents=True)
        (mir_src / "keep_sub" / "fresh.txt").write_text("mirror added nested file\n", encoding="utf-8")
        (mir_src / "shared.txt").write_text("mirror updated source\n", encoding="utf-8")
        (mir_dst / "shared.txt").write_text("mirror older target\n", encoding="utf-8")
        (mir_dst / "obsolete_sub" / "stale.txt").write_text("stale nested file\n", encoding="utf-8")
        (mir_dst / "extra_root.txt").write_text("stale root file\n", encoding="utf-8")

        clear_sync_state(helper_bin, env)
        mir_log = lane_root / "dtsync-mirror.log"
        with mir_log.open("w", encoding="utf-8") as ds_out:
            mir_proc = subprocess.Popen(
                [str(dtsync_bin), str(mir_src), str(mir_dst)],
                env=env, stdout=ds_out, stderr=subprocess.STDOUT, start_new_session=True,
            )
        procs.append(mir_proc)

        mir_pid = wait_for(
            lambda: find_proc_mapping_libs("dtfile_sync", home, ("libXm.so.9", "libck-core-fs-lib.so.0.5")),
            timeout=8.0,
            desc=f"{lane} dtfile_sync (Mirror) mapping libXm.so.9 + libck-core-fs-lib.so.0.5",
        )
        wait_for(lambda: "ready" in read_sync_state(helper_bin, env), timeout=8.0, desc=f"{lane} Mirror _DT_SYNC_STATE")

        send_sync_cmd(helper_bin, env, "profile:5")
        time.sleep(0.2)
        send_sync_cmd(helper_bin, env, "metadata:1")
        time.sleep(0.2)
        send_sync_cmd(helper_bin, env, "compare")
        wait_for(
            lambda: (s := read_sync_state(helper_bin, env)).get("ready") == "1"
            and s.get("state") == "1"
            and s.get("removals") == "3",
            timeout=15.0,
            desc=f"{lane} Mirror comparison ready with removals=3",
        )

        # Review WITHOUT approve_deletions -> must reject (apply_sensitive=0)
        send_sync_cmd(helper_bin, env, "retain:1")
        time.sleep(0.15)
        send_sync_cmd(helper_bin, env, "rule:0:1")
        time.sleep(0.2)
        send_sync_cmd(helper_bin, env, "review")
        time.sleep(0.25)
        st_unapproved = wait_for(
            lambda: s if (s := read_sync_state(helper_bin, env)).get("review_sensitive") == "1" else None,
            timeout=10.0,
            desc=f"{lane} Mirror unapproved review settled",
        )
        assert st_unapproved.get("apply_sensitive") == "0", f"{lane} Mirror enabled Apply without deletion approval: {st_unapproved}"

        # Approve deletions and review -> apply_sensitive=1!
        send_sync_cmd(helper_bin, env, "tab:3")
        time.sleep(0.15)
        send_sync_cmd(helper_bin, env, "approve_deletions:1")
        time.sleep(0.2)
        send_sync_cmd(helper_bin, env, "review")
        wait_for(
            lambda: (s := read_sync_state(helper_bin, env)).get("apply_sensitive") == "1"
            and int(s.get("receipt", "0")) > 0,
            timeout=10.0,
            desc=f"{lane} Mirror reviewed with deletion approval",
        )
        send_sync_cmd(helper_bin, env, "tab:2")
        time.sleep(0.25)
        maybe_capture_screenshot(lane, env, shot_dir, "dtsync_mirror_reviewed")

        send_sync_cmd(helper_bin, env, "apply")
        wait_for(
            lambda: (s := read_sync_state(helper_bin, env)).get("state") == "4"
            and s.get("finished") == "1",
            timeout=15.0,
            desc=f"{lane} Mirror Apply completed",
        )
        time.sleep(0.25)
        maybe_capture_screenshot(lane, env, shot_dir, "dtsync_mirror_completed")

        assert (mir_dst / "keep_sub" / "fresh.txt").read_text(encoding="utf-8") == "mirror added nested file\n"
        assert (mir_dst / "shared.txt").read_text(encoding="utf-8") == "mirror updated source\n"
        assert not (mir_dst / "obsolete_sub").exists()
        assert not (mir_dst / "extra_root.txt").exists()
        assert any(
            p.name.startswith(".ck-reviewed-retained-")
            and p.read_text(encoding="utf-8") == "mirror older target\n"
            for p in mir_dst.iterdir()
        ), f"{lane} Mirror did not preserve retained version in {mir_dst}"

        send_sync_cmd(helper_bin, env, "close")
        mir_proc.wait(timeout=10.0)

        # ---- PART 2: Synchronize two ways (`SYNC_PROFILE_TWO_WAY`, profile:6) ----
        tw_left = home / "TwoWayLeft"
        tw_right = home / "TwoWayRight"
        (tw_left / "docs").mkdir(parents=True)
        (tw_right / "notes").mkdir(parents=True)
        (tw_left / "docs" / "readme.txt").write_text("left handbook v1\n", encoding="utf-8")
        (tw_left / "both.txt").write_text("shared baseline v1\n", encoding="utf-8")
        (tw_right / "notes" / "todo.txt").write_text("right checklist v1\n", encoding="utf-8")

        clear_sync_state(helper_bin, env)
        tw_log = lane_root / "dtsync-twoway.log"
        with tw_log.open("w", encoding="utf-8") as ds_out:
            tw_proc = subprocess.Popen(
                [str(dtsync_bin), str(tw_left), str(tw_right)],
                env=env, stdout=ds_out, stderr=subprocess.STDOUT, start_new_session=True,
            )
        procs.append(tw_proc)

        wait_for(lambda: "ready" in read_sync_state(helper_bin, env), timeout=8.0, desc=f"{lane} TwoWay _DT_SYNC_STATE")
        send_sync_cmd(helper_bin, env, "profile:6")
        time.sleep(0.2)
        send_sync_cmd(helper_bin, env, "metadata:1")
        time.sleep(0.2)
        send_sync_cmd(helper_bin, env, "compare")
        wait_for(
            lambda: (s := read_sync_state(helper_bin, env)).get("ready") == "1"
            and s.get("two_way") == "1"
            and s.get("tw_gen") == "0"
            and s.get("first_run") == "1",
            timeout=15.0,
            desc=f"{lane} TwoWay Gen 0 comparison ready",
        )

        # Review WITHOUT approve_first_run -> must stay disabled
        send_sync_cmd(helper_bin, env, "review")
        time.sleep(0.25)
        st_nofirst = wait_for(
            lambda: s if (s := read_sync_state(helper_bin, env)).get("review_sensitive") == "1" else None,
            timeout=10.0,
            desc=f"{lane} TwoWay Gen 0 unapproved review settled",
        )
        assert st_nofirst.get("apply_sensitive") == "0", f"{lane} TwoWay Gen 0 enabled Apply without first-run approval: {st_nofirst}"

        # Enable approve_first_run -> review -> apply!
        send_sync_cmd(helper_bin, env, "tab:3")
        time.sleep(0.15)
        send_sync_cmd(helper_bin, env, "approve_first_run:1")
        time.sleep(0.2)
        send_sync_cmd(helper_bin, env, "review")
        try:
            wait_for(
                lambda: s if (s := read_sync_state(helper_bin, env)).get("apply_sensitive") == "1"
                and int(s.get("receipt", "0")) > 0 else None,
                timeout=10.0,
                desc=f"{lane} TwoWay Gen 0 reviewed",
            )
        except Exception as exc:
            raise RuntimeError(f"{exc} | state={read_sync_state(helper_bin, env)}") from exc
        send_sync_cmd(helper_bin, env, "tab:2")
        time.sleep(0.25)
        maybe_capture_screenshot(lane, env, shot_dir, "dtsync_twoway_gen0_reviewed")

        send_sync_cmd(helper_bin, env, "apply")
        wait_for(
            lambda: (s := read_sync_state(helper_bin, env)).get("state") == "4"
            and s.get("finished") == "1"
            and s.get("tw_gen") == "1",
            timeout=15.0,
            desc=f"{lane} TwoWay Gen 0 -> Gen 1 completed",
        )
        assert (tw_right / "docs" / "readme.txt").read_text(encoding="utf-8") == "left handbook v1\n"
        assert (tw_left / "notes" / "todo.txt").read_text(encoding="utf-8") == "right checklist v1\n"

        # Gen 1 -> Gen 2 in the SAME live window: concurrent edit + tracked folder deletion!
        (tw_left / "both.txt").write_text("left conflicting edit\n", encoding="utf-8")
        (tw_right / "both.txt").write_text("right winning edit\n", encoding="utf-8")
        (tw_right / "notes" / "todo.txt").unlink()
        (tw_right / "notes").rmdir()

        send_sync_cmd(helper_bin, env, "compare")
        wait_for(
            lambda: (s := read_sync_state(helper_bin, env)).get("ready") == "1"
            and s.get("state") == "1"
            and s.get("tw_gen") == "1"
            and s.get("first_run") == "0"
            and s.get("unresolved") == "1",
            timeout=15.0,
            desc=f"{lane} TwoWay Gen 1 conflict detected",
        )

        # Resolve concurrent file conflict (ExistingRegular=0 / BothEdited=1) -> Keep Right (18), review, and apply!
        send_sync_cmd(helper_bin, env, "rule:0:18")
        time.sleep(0.15)
        send_sync_cmd(helper_bin, env, "rule:1:18")
        time.sleep(0.2)
        send_sync_cmd(helper_bin, env, "review")
        wait_for(
            lambda: (s := read_sync_state(helper_bin, env)).get("apply_sensitive") == "1"
            and s.get("unresolved") == "0",
            timeout=10.0,
            desc=f"{lane} TwoWay Gen 1 conflict resolved via Keep Right",
        )
        send_sync_cmd(helper_bin, env, "apply")
        wait_for(
            lambda: (s := read_sync_state(helper_bin, env)).get("state") == "4"
            and s.get("finished") == "1"
            and s.get("tw_gen") == "2",
            timeout=15.0,
            desc=f"{lane} TwoWay Gen 1 -> Gen 2 completed",
        )
        time.sleep(0.25)
        maybe_capture_screenshot(lane, env, shot_dir, "dtsync_twoway_gen2_completed")

        assert (tw_left / "both.txt").read_text(encoding="utf-8") == "right winning edit\n"
        assert (tw_right / "both.txt").read_text(encoding="utf-8") == "right winning edit\n"
        assert not (tw_left / "notes").exists(), f"{lane} tracked folder deletion did not propagate to Left"
        assert any(
            p.name.startswith(".ck-sync-retained-")
            for p in tw_left.iterdir()
        ), f"{lane} TwoWay did not retain overwritten/deleted originals in {tw_left}"

        send_sync_cmd(helper_bin, env, "close")
        tw_proc.wait(timeout=10.0)

        return f"PASS [{lane}]: dtsync_pid={mir_pid} (Mirror deletion+retention & Two-Way Gen 0->1->2 verified)"
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        time.sleep(0.2)
        for p in reversed(procs):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True, help="Path to dtwm-wayland binary")
    ap.add_argument("--screenshot-dir", type=Path, default=None, help="Optional directory to save window screenshots")
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    if not comp_bin.is_file():
        print(f"ERROR: compositor not found: {comp_bin}", file=sys.stderr)
        return 1

    dtwm_x11_bin = comp_bin.parent / "dtwm"
    if not dtwm_x11_bin.is_file():
        dtwm_x11_bin = Path("/usr/dt/bin/dtwm")

    dtsync_bin = comp_bin.parent.parent / "dtfile" / "dtsync" / "dtfile_sync"
    if not dtsync_bin.is_file():
        dtsync_bin = Path("/usr/dt/bin/dtfile_sync")

    cc_cmd = shlex.split(os.environ.get("CC", "cc"))
    pkg_cfg = os.environ.get("PKG_CONFIG", "pkg-config")

    raw_tmp = os.environ.get("DTFILE_PRIVATE_TEST_ROOT") or os.environ.get("TMPDIR")
    if raw_tmp:
        st = os.stat(raw_tmp)
        if (st.st_mode & 0o022) and not (st.st_mode & 0o1000):
            if st.st_uid == os.geteuid():
                os.chmod(raw_tmp, 0o700)
            else:
                raw_tmp = "/var/tmp" if os.path.isdir("/var/tmp") else None

    with tempfile.TemporaryDirectory(prefix="dtsync-mirror-twoway-", dir=raw_tmp) as td:
        base_tmp = Path(td)
        base_tmp.chmod(0o700)
        c_path = base_tmp / "x11_sync_helper.c"
        helper_bin = base_tmp / "x11-sync-helper"
        c_path.write_text(X11_SYNC_HELPER_C, encoding="utf-8")
        x11_flags = shlex.split(subprocess.check_output([pkg_cfg, "--cflags", "--libs", "x11"], text=True))
        subprocess.run([*cc_cmd, "-O2", "-Wall", "-Wextra", str(c_path), "-o", str(helper_bin), *x11_flags], check=True)

        for ln in ("wayland", "x11"):
            lr = base_tmp / f"lane-{ln}"
            lr.mkdir(parents=True, exist_ok=True)
            lr.chmod(0o700)

        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_wl = pool.submit(
                run_lane, "wayland", 0, comp_bin, dtwm_x11_bin, dtsync_bin, helper_bin, base_tmp, args.screenshot_dir
            )
            fut_x11 = pool.submit(
                run_lane, "x11", 91, comp_bin, dtwm_x11_bin, dtsync_bin, helper_bin, base_tmp, args.screenshot_dir
            )
            res_wl = fut_wl.result()
            res_x11 = fut_x11.result()

        print(res_wl)
        print(res_x11)
        print("dtfile_sync Mirror + Two-Way parallel Wayland & X11 checks: ALL PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
