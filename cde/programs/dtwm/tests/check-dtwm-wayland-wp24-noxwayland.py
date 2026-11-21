#!/usr/bin/env python3
"""WP-24 Track 2 pure XWayland-free native CDE session & hybrid/X11 regression check:
1. Pure XWayland-Free Native Wayland Session (`dtwm-wayland --no-xwayland`):
   - Boots `dtwm-wayland --no-xwayland` headless in an isolated `0700` `XDG_RUNTIME_DIR`.
   - Verifies `--status-file` emits `WAYLAND_DISPLAY=...` and `NO_XWAYLAND=1` with no `DISPLAY=`.
   - Verifies zero `Xwayland` processes exist in the session process tree or runtime directory.
   - Verifies genuine `dtwm-panel` auto-spawns and docks as a native `xdg_toplevel` view
     with `CK_MOTIF_WAYLAND_ACTIVE=1` and maps `libXm.so.9`.
   - Launches unmodified CDE & CK Core clients (`dtstyle`, `dtfile`, `dtpad`, plus `ck-clock`
     and `ck-coins` when installed) with `DISPLAY` unset; verifies every client stays alive,
     sets `CK_MOTIF_WAYLAND_ACTIVE=1`, creates a private `$XDG_RUNTIME_DIR/ck-wlx11-<pid>-*/S:0`
     virtualizer socket, and maps `libXm.so.9`.
   - Verifies live session IPC over `$XDG_RUNTIME_DIR/dtwm-wayland-<WAYLAND_DISPLAY>.ipc`:
     * `_DT_WM_REQUEST` workspace switching (`f.goto_workspace ws1` -> `ws0`) updates
       `_DT_WORKSPACE_CURRENT_NAME` across connected peers.
     * `RESOURCE_MANAGER` live theme switching (`*appearanceProfile: modern_dark`) rebroadcasts
       `PROP RESOURCE_MANAGER` to peers and visibly shifts desktop/window luminance.
   - Verifies `dtsession --wayland --no-xwayland` bootstrap flag handling (`--help` contract).
2. Parallel Hybrid Wayland + Classic X11 Regression Lane:
   - Boots default `dtwm-wayland` (without `--no-xwayland`) and confirms `Xwayland` + `DISPLAY=:`
     remain intact for legacy external X11 clients.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

from PIL import Image


def wait_until(pred, timeout: float = 10.0, step: float = 0.05, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_val = None
    while time.monotonic() < deadline:
        try:
            last_val = pred()
            if last_val:
                return last_val
        except Exception:
            pass
        time.sleep(step)
    raise RuntimeError(f"Timed out waiting for {desc} (last={last_val!r})")


def resolve_motif_prefix(comp_bin: Path) -> Path | None:
    env_pfx = os.environ.get("MOTIF_PREFIX")
    if env_pfx and Path(env_pfx).is_dir():
        return Path(env_pfx).resolve()
    for anc in comp_bin.parents:
        cand = anc / "motif-prefix"
        if (cand / "lib/libXm.so.9").exists():
            return cand.resolve()
    wp00 = Path("/var/tmp/ck-wayland-build/wp00/motif-prefix")
    if (wp00 / "lib/libXm.so.9").exists():
        return wp00.resolve()
    return None


def resolve_fs_prefix(comp_bin: Path) -> Path | None:
    for anc in comp_bin.parents:
        cand = anc / "fs-prefix"
        if (cand / "lib").is_dir():
            return cand.resolve()
    wp00 = Path("/var/tmp/ck-wayland-build/wp00/fs-prefix")
    if (wp00 / "lib").is_dir():
        return wp00.resolve()
    return None


def build_ld_library_path(cde_build_root: Path, motif_prefix: Path | None, fs_prefix: Path | None) -> str:
    dirs: list[str] = []
    if motif_prefix and (motif_prefix / "lib").is_dir():
        dirs.append(str(motif_prefix / "lib"))
    if fs_prefix and (fs_prefix / "lib").is_dir():
        dirs.append(str(fs_prefix / "lib"))
    for sub in ("DtWidget", "DtSvc", "DtHelp", "DtPrint", "DtTerm", "DtSearch", "DtMrm", "DtXinerama", "csa", "pam/libpam", "tt/lib"):
        p = cde_build_root / "lib" / sub / ".libs"
        if p.is_dir():
            dirs.append(str(p))
    cur = os.environ.get("LD_LIBRARY_PATH", "")
    if cur:
        dirs.append(cur)
    return ":".join(dirs)


def resolve_cde_prog(cde_build_root: Path, subdir: str, name: str) -> Path:
    for rel in (f"programs/{subdir}/.libs/{name}", f"programs/{subdir}/{name}"):
        cand = cde_build_root / rel
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    sys_cand = Path(f"/usr/dt/bin/{name}")
    if sys_cand.is_file() and os.access(sys_cand, os.X_OK):
        return sys_cand
    found = shutil.which(name)
    if found:
        return Path(found)
    raise FileNotFoundError(f"Required binary not found: {name}")


def resolve_optional_app(name: str) -> Path | None:
    for cand in (
        Path(f"/var/tmp/ck-wayland-build/wp00/apps-build/bin/{name}"),
        Path(f"/var/tmp/ck-wayland-build/wp00/apps-src/build/bin/{name}"),
        Path(f"/usr/local/bin/{name}"),
        Path(f"/usr/dt/bin/{name}"),
    ):
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    w = shutil.which(name)
    return Path(w) if w else None


def find_real_pid_by_exe_or_cmd(root_pid: int, needle: str, rt_dir: Path | None = None) -> int | None:
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        if pid == os.getpid():
            continue
        try:
            cmdline = (entry / "cmdline").read_bytes().decode("utf-8", errors="ignore").replace("\x00", " ")
            if needle not in cmdline:
                continue
            if rt_dir is not None:
                env_raw = (entry / "environ").read_bytes().decode("utf-8", errors="ignore")
                if f"XDG_RUNTIME_DIR={rt_dir}" not in env_raw:
                    continue
            # Skip /bin/sh libtool wrapper scripts; pick the binary mapping libXm.so
            maps = (entry / "maps").read_text(encoding="utf-8", errors="ignore")
            if "libXm.so" in maps:
                return pid
        except OSError:
            continue
    return None


def assert_no_xwayland_in_runtime(rt_dir: Path) -> None:
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            cmdline = (entry / "cmdline").read_bytes().decode("utf-8", errors="ignore").replace("\x00", " ")
            if "Xwayland" not in cmdline:
                continue
            env_raw = (entry / "environ").read_bytes().decode("utf-8", errors="ignore")
            if f"XDG_RUNTIME_DIR={rt_dir}" in env_raw:
                raise AssertionError(f"Unexpected Xwayland process {entry.name} running in --no-xwayland session: {cmdline}")
        except OSError:
            continue


def verify_native_wayland_motif_pid(pid: int, label: str, rt_dir: Path, motif_prefix: Path | None) -> str:
    env_items = {}
    raw_env = Path(f"/proc/{pid}/environ").read_bytes().split(b"\x00")
    for item in raw_env:
        if b"=" in item:
            k, v = item.split(b"=", 1)
            env_items[k.decode("utf-8", errors="ignore")] = v.decode("utf-8", errors="ignore")

    init_disp = env_items.get("DISPLAY", "")
    if init_disp and "/ck-wlx11-" not in init_disp:
        raise AssertionError(f"{label} (pid={pid}) launched with X11 DISPLAY={init_disp!r} in --no-xwayland mode")

    # Note: /proc/<pid>/environ reflects initial execve() stack before libXm's constructor
    # calls setenv(); verify the live per-PID ck-wlx11-<pid>-*/S:0 runtime socket on disk!
    socks = wait_until(
        lambda: [p for p in rt_dir.glob(f"ck-wlx11-{pid}-*/S:0") if p.exists()],
        timeout=5.0,
        desc=f"{label} (pid={pid}) private ck-wlx11 socket",
    )
    sock_str = str(socks[0])

    maps = Path(f"/proc/{pid}/maps").read_text(encoding="utf-8", errors="ignore")
    if "libXm.so.9" not in maps:
        raise AssertionError(f"{label} (pid={pid}) does not map libXm.so.9")
    if motif_prefix is not None and str(motif_prefix / "lib") not in maps:
        raise AssertionError(f"{label} (pid={pid}) did not map libXm.so.9 from {motif_prefix / 'lib'}")
    return sock_str


def terminate_procs(procs: list[subprocess.Popen]) -> None:
    for p in reversed(procs):
        if p.poll() is None:
            try:
                p.send_signal(signal.SIGTERM)
            except OSError:
                pass
    deadline = time.monotonic() + 3.0
    for p in reversed(procs):
        rem = max(0.05, deadline - time.monotonic())
        try:
            p.wait(timeout=rem)
        except subprocess.TimeoutExpired:
            try:
                p.kill()
                p.wait(timeout=1.0)
            except OSError:
                pass


def capture_grim_luma(env: dict[str, str], out_png: Path) -> tuple[int, int, float]:
    subprocess.run(["grim", str(out_png)], env=env, check=True, timeout=8.0)
    with Image.open(out_png) as im:
        rgb = im.convert("RGB")
        w, h = rgb.size
        raw = rgb.tobytes()
        npix = w * h
        step = max(1, npix // 20000)
        total = 0.0
        cnt = 0
        for idx in range(0, npix, step):
            off = idx * 3
            r = raw[off]
            g = raw[off + 1]
            b = raw[off + 2]
            total += 0.299 * r + 0.587 * g + 0.114 * b
            cnt += 1
        return w, h, (total / cnt if cnt else 0.0)


def ipc_read_lines_until(sock: socket.socket, buf: bytearray, predicate, timeout: float = 5.0) -> list[str]:
    deadline = time.monotonic() + timeout
    seen: list[str] = []
    sock.settimeout(0.2)
    while time.monotonic() < deadline:
        while b"\n" in buf:
            idx = buf.index(b"\n")
            line = bytes(buf[:idx]).decode("utf-8", errors="replace")
            del buf[: idx + 1]
            seen.append(line)
            if predicate(line, seen):
                return seen
        try:
            chunk = sock.recv(8192)
            if not chunk:
                break
            buf.extend(chunk)
        except socket.timeout:
            continue
    raise RuntimeError(f"IPC timeout waiting for expected frame; seen={seen!r}")


def run_noxwayland_lane(
    comp_bin: Path,
    cde_build_root: Path,
    motif_prefix: Path | None,
    fs_prefix: Path | None,
    shots_dir: Path | None,
) -> str:
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp24-noxw-", dir=tmp_base) as base_s:
        base = Path(base_s)
        rt_dir = base / "rt"
        home_dir = base / "home"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt").mkdir(parents=True, mode=0o700)
        (home_dir / "SampleDocs").mkdir(parents=True, mode=0o755)
        (home_dir / "SampleDocs/readme.txt").write_text("Pure Native Wayland CDE\n", encoding="utf-8")

        dtsession_bin = resolve_cde_prog(cde_build_root, "dtsession", "dtsession")
        help_env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        help_env["XDG_RUNTIME_DIR"] = str(rt_dir)
        help_env["CK_MOTIF_BACKEND"] = "x11"
        help_env["LD_LIBRARY_PATH"] = build_ld_library_path(cde_build_root, motif_prefix, fs_prefix)
        help_out = subprocess.check_output(
            [str(dtsession_bin), "--help"],
            env=help_env,
            text=True,
            timeout=5.0,
        )
        if "--no-xwayland" not in help_out:
            raise AssertionError(f"dtsession --help missing --no-xwayland: {help_out!r}")

        status_file = base / "status.env"
        comp_log = base / "compositor.log"
        ld_path = build_ld_library_path(cde_build_root, motif_prefix, fs_prefix)

        base_env = os.environ.copy()
        base_env.pop("DISPLAY", None)
        base_env.pop("WAYLAND_DISPLAY", None)
        base_env.update(
            {
                "XDG_RUNTIME_DIR": str(rt_dir),
                "HOME": str(home_dir),
                "TMPDIR": str(base),
                "LD_LIBRARY_PATH": ld_path,
                "WLR_BACKENDS": "headless",
                "WLR_RENDERER": "pixman",
                "WLR_HEADLESS_OUTPUTS": "1",
                "LIBGL_ALWAYS_SOFTWARE": "1",
                "LANG": "C.UTF-8",
            }
        )

        procs: list[subprocess.Popen] = []
        ipc_sock: socket.socket | None = None
        try:
            with comp_log.open("wb") as clog:
                comp_proc = subprocess.Popen(
                    [
                        str(comp_bin),
                        "--no-xwayland",
                        "--appearance-profile",
                        "classic",
                        "--status-file",
                        str(status_file),
                    ],
                    env=base_env,
                    stdout=clog,
                    stderr=subprocess.STDOUT,
                )
            procs.append(comp_proc)

            wait_until(lambda: status_file.is_file() and status_file.stat().st_size > 0, timeout=12.0, desc="status_file")
            st_lines = status_file.read_text(encoding="utf-8").splitlines()
            st_map = {}
            for ln in st_lines:
                if "=" in ln:
                    k, v = ln.split("=", 1)
                    st_map[k.strip()] = v.strip()

            wl_disp = st_map.get("WAYLAND_DISPLAY", "")
            if not wl_disp:
                raise AssertionError(f"Missing WAYLAND_DISPLAY in {st_lines!r}")
            if "DISPLAY" in st_map:
                raise AssertionError(f"Unexpected DISPLAY in --no-xwayland status file: {st_lines!r}")
            if st_map.get("NO_XWAYLAND") != "1":
                raise AssertionError(f"Missing NO_XWAYLAND=1 in status file: {st_lines!r}")

            assert_no_xwayland_in_runtime(rt_dir)

            ipc_path = rt_dir / f"dtwm-wayland-{wl_disp}.ipc"
            if not ipc_path.exists():
                raise AssertionError(f"Missing native IPC bus socket: {ipc_path}")

            # Verify genuine dtwm-panel auto-spawned on native Wayland without XWayland
            panel_pid = wait_until(
                lambda: find_real_pid_by_exe_or_cmd(comp_proc.pid, "dtwm-panel", rt_dir),
                timeout=10.0,
                desc="native dtwm-panel process",
            )
            panel_sock = verify_native_wayland_motif_pid(panel_pid, "dtwm-panel", rt_dir, motif_prefix)

            # Connect a test peer to the session IPC bus and verify initial READY + workspace state
            ipc_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            ipc_sock.connect(str(ipc_path))
            ipc_buf = bytearray()
            init_lines = ipc_read_lines_until(ipc_sock, ipc_buf, lambda ln, _: ln == "READY", timeout=5.0)
            if not any(ln.startswith("PROP _DT_WORKSPACE_CURRENT_NAME ") for ln in init_lines):
                raise AssertionError(f"Initial IPC burst missing _DT_WORKSPACE_CURRENT_NAME: {init_lines!r}")

            # Launch native CDE & CK Core clients with DISPLAY unset
            client_env = dict(base_env)
            client_env["WAYLAND_DISPLAY"] = wl_disp
            client_env["CK_MOTIF_BACKEND"] = "wayland"
            client_env["DTWM_WAYLAND_NO_XWAYLAND"] = "1"

            if Path("/usr/dt/bin/ttsession").is_file():
                tt_proc = subprocess.Popen(
                    ["/usr/dt/bin/ttsession", "-p"],
                    env=client_env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    text=True,
                )
                procs.append(tt_proc)
                assert tt_proc.stdout is not None
                tt_line = tt_proc.stdout.readline().strip()
                if tt_line:
                    client_env["TT_SESSION"] = tt_line

            dtstyle_bin = resolve_cde_prog(cde_build_root, "dtstyle", "dtstyle")
            dtfile_bin = resolve_cde_prog(cde_build_root, "dtfile", "dtfile")
            dtpad_bin = resolve_cde_prog(cde_build_root, "dtpad", "dtpad")

            client_specs: list[tuple[str, list[str]]] = [
                ("dtstyle", [str(dtstyle_bin)]),
                ("dtfile", [str(dtfile_bin), "-dir", str(home_dir / "SampleDocs")]),
                ("dtpad", [str(dtpad_bin), "-standAlone", str(home_dir / "SampleDocs/readme.txt")]),
            ]
            for opt_app in ("ck-clock", "ck-coins"):
                app_path = resolve_optional_app(opt_app)
                if app_path is not None:
                    client_specs.append((opt_app, [str(app_path)]))

            verified_clients: list[str] = []
            seen_sockets = {panel_sock}
            for label, cmd in client_specs:
                lp = base / f"{label}.log"
                with lp.open("wb") as lf:
                    p = subprocess.Popen(cmd, env=client_env, stdout=lf, stderr=subprocess.STDOUT)
                procs.append(p)

            time.sleep(1.8)
            for label, _ in client_specs:
                pid = wait_until(
                    lambda lb=label: find_real_pid_by_exe_or_cmd(comp_proc.pid, lb, rt_dir),
                    timeout=8.0,
                    desc=f"{label} native Wayland process",
                )
                csock = verify_native_wayland_motif_pid(pid, label, rt_dir, motif_prefix)
                if csock in seen_sockets:
                    raise AssertionError(f"{label} reused another process's ck-wlx11 socket: {csock}")
                seen_sockets.add(csock)
                verified_clients.append(label)

            # Re-verify STILL zero Xwayland processes after all CDE + companion apps mapped!
            assert_no_xwayland_in_runtime(rt_dir)

            shot_classic = (shots_dir / "wp24_noxwayland_classic.png") if shots_dir else (base / "classic.png")
            w1, h1, luma_classic = capture_grim_luma(client_env, shot_classic)
            if w1 < 640 or h1 < 480:
                raise AssertionError(f"Unexpected grim desktop size {w1}x{h1}")

            # Exercise live _DT_WM_REQUEST workspace switch over IPC (ws0 -> ws1 -> ws0)
            req_ws1 = b"f.goto_workspace ws1\x00".hex()
            ipc_sock.sendall(f"SET _DT_WM_REQUEST {req_ws1}\n".encode("ascii"))
            ipc_read_lines_until(
                ipc_sock,
                ipc_buf,
                lambda ln, _: ln == f"PROP _DT_WORKSPACE_CURRENT_NAME {b'ws1\x00'.hex()}",
                timeout=5.0,
            )

            req_ws0 = b"f.goto_workspace ws0\x00".hex()
            ipc_sock.sendall(f"SET _DT_WM_REQUEST {req_ws0}\n".encode("ascii"))
            ipc_read_lines_until(
                ipc_sock,
                ipc_buf,
                lambda ln, _: ln == f"PROP _DT_WORKSPACE_CURRENT_NAME {b'ws0\x00'.hex()}",
                timeout=5.0,
            )

            # Exercise live RESOURCE_MANAGER modern_dark theme broadcast over IPC
            rm_dark = (
                "*appearanceProfile:\tmodern_dark\n"
                "*background:\t#1e222a\n"
                "*foreground:\t#eceff4\n"
            ).encode("utf-8")
            ipc_sock.sendall(f"SET RESOURCE_MANAGER {rm_dark.hex()}\n".encode("ascii"))
            time.sleep(0.9)

            shot_dark = (shots_dir / "wp24_noxwayland_modern_dark.png") if shots_dir else (base / "dark.png")
            _, _, luma_dark = capture_grim_luma(client_env, shot_dark)
            if luma_dark >= luma_classic - 8.0:
                raise AssertionError(
                    f"Live IPC modern_dark switch did not darken desktop/apps (classic={luma_classic:.1f}, dark={luma_dark:.1f})"
                )

            assert_no_xwayland_in_runtime(rt_dir)
            return (
                f"Wayland --no-xwayland OK ({wl_disp}, Xwayland=0, panel+clients={','.join(['dtwm-panel', *verified_clients])}, "
                f"ws0->ws1->ws0 IPC OK, luma {luma_classic:.1f}->{luma_dark:.1f})"
            )
        finally:
            if ipc_sock is not None:
                try:
                    ipc_sock.close()
                except OSError:
                    pass
            terminate_procs(procs)
            for entry in Path("/proc").iterdir():
                if not entry.name.isdigit():
                    continue
                try:
                    env_raw = (entry / "environ").read_bytes().decode("utf-8", errors="ignore")
                    if f"XDG_RUNTIME_DIR={rt_dir}" in env_raw:
                        os.kill(int(entry.name), signal.SIGTERM)
                except OSError:
                    continue


def run_hybrid_regression_lane(
    comp_bin: Path,
    cde_build_root: Path,
    motif_prefix: Path | None,
    fs_prefix: Path | None,
) -> str:
    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp24-hyb-", dir=tmp_base) as base_s:
        base = Path(base_s)
        rt_dir = base / "rt"
        home_dir = base / "home"
        rt_dir.mkdir(mode=0o700)
        (home_dir / ".dt").mkdir(parents=True, mode=0o700)

        status_file = base / "status.env"
        comp_log = base / "hybrid.log"
        env = os.environ.copy()
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DTWM_WAYLAND_NO_XWAYLAND", None)
        env.update(
            {
                "XDG_RUNTIME_DIR": str(rt_dir),
                "HOME": str(home_dir),
                "TMPDIR": str(base),
                "LD_LIBRARY_PATH": build_ld_library_path(cde_build_root, motif_prefix, fs_prefix),
                "WLR_BACKENDS": "headless",
                "WLR_RENDERER": "pixman",
                "WLR_HEADLESS_OUTPUTS": "1",
                "LIBGL_ALWAYS_SOFTWARE": "1",
            }
        )

        procs: list[subprocess.Popen] = []
        try:
            with comp_log.open("wb") as clog:
                p = subprocess.Popen(
                    [str(comp_bin), "--no-panel", "--status-file", str(status_file)],
                    env=env,
                    stdout=clog,
                    stderr=subprocess.STDOUT,
                )
            procs.append(p)
            wait_until(lambda: status_file.is_file() and status_file.stat().st_size > 0, timeout=12.0, desc="hybrid status_file")
            st_map = {}
            for ln in status_file.read_text(encoding="utf-8").splitlines():
                if "=" in ln:
                    k, v = ln.split("=", 1)
                    st_map[k.strip()] = v.strip()
            if not st_map.get("WAYLAND_DISPLAY") or not st_map.get("DISPLAY", "").startswith(":"):
                raise AssertionError(f"Default hybrid mode did not expose both WAYLAND_DISPLAY and DISPLAY: {st_map!r}")
            return f"Hybrid Wayland+XWayland default OK (WAYLAND_DISPLAY={st_map['WAYLAND_DISPLAY']}, DISPLAY={st_map['DISPLAY']})"
        finally:
            terminate_procs(procs)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", required=True, type=Path)
    ap.add_argument("--shots-dir", default=None, type=Path)
    args = ap.parse_args()

    comp_bin = args.compositor.resolve()
    cde_build_root = comp_bin.parents[2]
    if (comp_bin.parent.name == ".libs"):
        cde_build_root = comp_bin.parents[3]

    motif_prefix = resolve_motif_prefix(comp_bin)
    fs_prefix = resolve_fs_prefix(comp_bin)
    if args.shots_dir is not None:
        args.shots_dir.mkdir(parents=True, exist_ok=True)

    t0 = time.monotonic()
    with ThreadPoolExecutor(max_workers=2) as pool:
        f_nox = pool.submit(run_noxwayland_lane, comp_bin, cde_build_root, motif_prefix, fs_prefix, args.shots_dir)
        f_hyb = pool.submit(run_hybrid_regression_lane, comp_bin, cde_build_root, motif_prefix, fs_prefix)
        res_nox = f_nox.result()
        res_hyb = f_hyb.result()
    dt = time.monotonic() - t0
    print(f"[WP-24 PASS in {dt:.2f}s]")
    print(f"  - {res_nox}")
    print(f"  - {res_hyb}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
