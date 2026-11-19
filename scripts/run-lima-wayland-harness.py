#!/usr/bin/env python3
"""Build and qualify CK Core Motif + CDE + ck-core-apps inside the Ubuntu 26.04 Lima Wayland harness.

Enforces:
- Mac temporary paths strictly beneath /Volumes/S8T/tmp (fallback /Volumes/PRO-BLADE/tmp only if /Volumes/S8T is unmounted).
- At most 6 parallel build/test jobs (-j6).
- Incremental VM-local native builds (/var/tmp/ck-wayland-build) preserving unchanged SDK header timestamps.
- Runtime /proc/<pid>/maps verification that every CDE and ck-core-apps client maps the recorded libXm.so.9.
- Dual-output headless wlroots + rootless XWayland + virtual-pointer + zwlr_screencopy_manager_v1 (grim) capture.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

MAX_JOBS = 6
APPS_NON_CEF_TARGETS = [
    "ck-about",
    "ck-load",
    "ck-tasks",
    "ck-mixer",
    "ck-sound",
    "ck-midi",
    "ck-clock",
    "ck-calc",
    "ck-character-map",
    "ck-grab",
    "ck-status",
    "ck-eyes",
    "ck-coins",
    "ck-nibbles",
    "ck-mines",
    "ck-plasma-1",
]


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_mac_temp_root(task_root: Path) -> None:
    resolved = str(task_root.resolve())
    if resolved.startswith("/private/tmp") or resolved.startswith("/tmp"):
        raise SystemExit(f"error: refusing system temporary path on macOS: {resolved}")
    s8t_mounted = Path("/Volumes/S8T").is_dir()
    pro_blade_mounted = Path("/Volumes/PRO-BLADE").is_dir()
    if s8t_mounted:
        if not resolved.startswith("/Volumes/S8T/tmp/"):
            raise SystemExit(
                f"error: /Volumes/S8T is mounted; task-root must be beneath /Volumes/S8T/tmp/ (got {resolved})"
            )
    elif pro_blade_mounted:
        if not resolved.startswith("/Volumes/PRO-BLADE/tmp/"):
            raise SystemExit(
                f"error: task-root must be beneath /Volumes/PRO-BLADE/tmp/ when /Volumes/S8T is unavailable (got {resolved})"
            )
    else:
        raise SystemExit("error: neither /Volumes/S8T nor /Volumes/PRO-BLADE is mounted")


def run_checked(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    log_path: Path | None = None,
) -> float:
    t0 = time.monotonic()
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w", encoding="utf-8") as logf:
            proc = subprocess.run(cmd, cwd=cwd, env=env, stdout=logf, stderr=subprocess.STDOUT, text=True)
        if proc.returncode != 0:
            tail = "\n".join(log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-80:])
            raise SystemExit(f"command failed ({proc.returncode}): {' '.join(cmd)}\n--- log tail ({log_path}) ---\n{tail}")
    else:
        proc = subprocess.run(cmd, cwd=cwd, env=env)
        if proc.returncode != 0:
            raise SystemExit(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    return round(time.monotonic() - t0, 2)


def git_rev(repo: Path) -> str:
    out = subprocess.check_output(
        ["git", "-c", "safe.directory=*", "-C", str(repo), "rev-parse", "HEAD"],
        text=True,
    )
    return out.strip()


def git_object_exists(repo: Path, rev: str) -> bool:
    proc = subprocess.run(
        ["git", "-c", "safe.directory=*", "-C", str(repo), "cat-file", "-e", f"{rev}^{{commit}}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return proc.returncode == 0


def pinned_or_head_rev(cde_repo: Path, pin_relpath: str, sister_repo: Path) -> str:
    pin_file = cde_repo / pin_relpath
    if pin_file.is_file():
        pinned = pin_file.read_text(encoding="utf-8").strip()
        if pinned and git_object_exists(sister_repo, pinned):
            return pinned
    return git_rev(sister_repo)


def host_dispatch_to_lima(args: argparse.Namespace) -> int:
    task_root = Path(args.task_root).resolve()
    verify_mac_temp_root(task_root)
    lima_home = task_root / "lima"
    tmp_dir = task_root / "tmp"
    cache_dir = task_root / "cache"
    output_dir = Path(args.output_dir).resolve() if args.output_dir else (task_root / "receipts" / "wp00")
    verify_mac_temp_root(output_dir)

    for d in (lima_home, tmp_dir, cache_dir, output_dir):
        d.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["TMPDIR"] = str(tmp_dir)
    env["LIMA_HOME"] = str(lima_home)
    env["XDG_CACHE_HOME"] = str(cache_dir)

    script_in_guest = Path(__file__).resolve()
    guest_cmd = [
        "python3",
        str(script_in_guest),
        "--in-guest",
        "--cde-repo",
        str(Path(args.cde_repo).resolve()),
        "--motif-repo",
        str(Path(args.motif_repo).resolve()),
        "--fs-repo",
        str(Path(args.fs_repo).resolve()),
        "--apps-repo",
        str(Path(args.apps_repo).resolve()),
        "--guest-build-root",
        args.guest_build_root,
        "--output-dir",
        str(output_dir),
        "--jobs",
        str(min(args.jobs, MAX_JOBS)),
        "--compositor",
        args.compositor,
    ]
    if args.skip_cde_check:
        guest_cmd.append("--skip-cde-check")
    if args.fs_revision:
        guest_cmd.extend(["--fs-revision", args.fs_revision])

    cmd = ["limactl", "shell", args.vm_name, "--"] + guest_cmd
    return subprocess.call(cmd, env=env)


def overlay_worktree_diff(src_repo: Path, dst_dir: Path) -> str:
    diff = subprocess.check_output(
        ["git", "-c", "safe.directory=*", "-C", str(src_repo), "diff", "--binary", "HEAD"],
    )
    if not diff:
        return ""
    diff_hash = hashlib.sha256(diff).hexdigest()[:16]
    proc = subprocess.run(
        ["git", "apply", "--whitespace=nowarn", "-"],
        cwd=dst_dir,
        input=diff,
    )
    if proc.returncode != 0:
        raise SystemExit(f"git apply of worktree diff failed in {dst_dir}")
    return diff_hash


def sync_git_checkout(src_repo: Path, dst_dir: Path, rev: str, *, include_worktree_diff: bool = False) -> str:
    diff_bytes = (
        subprocess.check_output(["git", "-c", "safe.directory=*", "-C", str(src_repo), "diff", "--binary", "HEAD"])
        if include_worktree_diff
        else b""
    )
    stamp = f"{rev}:{hashlib.sha256(diff_bytes).hexdigest()[:16]}" if diff_bytes else rev
    marker = dst_dir / ".ck-synced-revision"
    if marker.is_file() and marker.read_text(encoding="utf-8").strip() == stamp:
        return stamp

    # Incremental update across commits on the same repo: only touch files that changed
    # between the previous synced commit and `rev` (plus any uncommitted worktree edits),
    # preserving timestamps on all untouched files for fast incremental Make builds.
    if dst_dir.is_dir() and marker.is_file():
        old_stamp = marker.read_text(encoding="utf-8").strip()
        old_rev = old_stamp.split(":", 1)[0]
        if git_object_exists(src_repo, old_rev):
            changed = set(
                subprocess.check_output(
                    ["git", "-c", "safe.directory=*", "-C", str(src_repo), "diff", "--name-only", old_rev, rev],
                    text=True,
                ).splitlines()
            )
            if include_worktree_diff:
                changed.update(
                    subprocess.check_output(
                        ["git", "-c", "safe.directory=*", "-C", str(src_repo), "diff", "--name-only", "HEAD"],
                        text=True,
                    ).splitlines()
                )
                untracked = subprocess.check_output(
                    ["git", "-c", "safe.directory=*", "-C", str(src_repo), "ls-files", "--others", "--exclude-standard"],
                    text=True,
                ).splitlines()
                changed.update(untracked)
            for rel in sorted(changed):
                if include_worktree_diff:
                    sfile = src_repo / rel
                    if sfile.is_file():
                        dfile = dst_dir / rel
                        dfile.parent.mkdir(parents=True, exist_ok=True)
                        data = sfile.read_bytes()
                        if not dfile.is_file() or dfile.read_bytes() != data:
                            dfile.write_bytes(data)
                            shutil.copymode(sfile, dfile)
                    elif (dst_dir / rel).exists():
                        (dst_dir / rel).unlink()
                else:
                    check = subprocess.run(
                        ["git", "-c", "safe.directory=*", "-C", str(src_repo), "cat-file", "-e", f"{rev}:{rel}"],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                    dfile = dst_dir / rel
                    if check.returncode == 0:
                        data = subprocess.check_output(
                            ["git", "-c", "safe.directory=*", "-C", str(src_repo), "show", f"{rev}:{rel}"]
                        )
                        dfile.parent.mkdir(parents=True, exist_ok=True)
                        if not dfile.is_file() or dfile.read_bytes() != data:
                            dfile.write_bytes(data)
                    elif dfile.exists():
                        dfile.unlink()
            marker.write_text(stamp + "\n", encoding="utf-8")
            return stamp

    if dst_dir.exists():
        shutil.rmtree(dst_dir)
    dst_dir.mkdir(parents=True, exist_ok=True)
    archive = subprocess.Popen(
        ["git", "-c", "safe.directory=*", "-C", str(src_repo), "archive", rev],
        stdout=subprocess.PIPE,
    )
    assert archive.stdout is not None
    subprocess.check_call(["tar", "-x", "-C", str(dst_dir)], stdin=archive.stdout)
    if archive.wait() != 0:
        raise SystemExit(f"git archive failed for {src_repo} @ {rev}")
    if diff_bytes:
        overlay_worktree_diff(src_repo, dst_dir)
    if include_worktree_diff:
        untracked = subprocess.check_output(
            ["git", "-c", "safe.directory=*", "-C", str(src_repo), "ls-files", "--others", "--exclude-standard"],
            text=True,
        ).splitlines()
        for rel in sorted(untracked):
            sfile = src_repo / rel
            if sfile.is_file():
                dfile = dst_dir / rel
                dfile.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(sfile, dfile)
    marker.write_text(stamp + "\n", encoding="utf-8")
    return stamp


def verify_pid_maps_libxm(pid: int, expected_libxm: Path) -> str:
    maps_path = Path(f"/proc/{pid}/maps")
    expected_real = str(expected_libxm.resolve())
    for line in maps_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "libXm.so" in line:
            parts = line.split()
            mapped = parts[-1] if len(parts) >= 6 else ""
            if Path(mapped).resolve() == Path(expected_real):
                return mapped
            raise SystemExit(
                f"PID {pid} mapped unexpected libXm: {mapped} (expected {expected_real})"
            )
    raise SystemExit(f"PID {pid} did not map libXm.so.9 at all")


def guest_run(args: argparse.Namespace) -> int:
    jobs = max(1, min(args.jobs, MAX_JOBS))
    cde_repo = Path(args.cde_repo)
    motif_repo = Path(args.motif_repo)
    fs_repo = Path(args.fs_repo)
    apps_repo = Path(args.apps_repo)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    cde_rev = git_rev(cde_repo)
    motif_rev = args.motif_revision or git_rev(motif_repo)
    fs_rev = args.fs_revision or pinned_or_head_rev(cde_repo, ".ckgit/fs-revision", fs_repo)
    apps_rev = git_rev(apps_repo)

    build_root = Path(args.guest_build_root)
    tmp_dir = build_root / "tmp"
    locale_dir = build_root / "locale"
    for d in (build_root, tmp_dir, locale_dir):
        d.mkdir(parents=True, exist_ok=True)
    # Strip group/other write bits created under Ubuntu's 0002 umask so dtfile's
    # private-storage ancestor verification and unprivileged _dtgreeter both succeed.
    cur_anc = build_root
    while str(cur_anc) not in ("/", "/var", "/var/tmp", "/tmp"):
        try:
            os.chmod(cur_anc, 0o755)
        except OSError:
            pass
        cur_anc = cur_anc.parent
    os.chmod(tmp_dir, 0o755)

    if not (locale_dir / "en_US.UTF-8").exists():
        run_checked(["localedef", "-i", "en_US", "-f", "UTF-8", str(locale_dir / "en_US.UTF-8")])

    base_env = os.environ.copy()
    base_env["TMPDIR"] = str(tmp_dir)
    base_env["LOCPATH"] = str(locale_dir)
    base_env["LANG"] = "en_US.UTF-8"
    base_env["LC_ALL"] = "en_US.UTF-8"
    base_env["INSTALL"] = "/usr/bin/install -p"

    started_utc = utc_now()
    t_total = time.monotonic()
    timings: dict[str, float] = {}

    # 1. CK Core Motif ABI 9
    motif_src = build_root / "motif-src"
    motif_bld = build_root / "motif-build"
    motif_pfx = build_root / "motif-prefix"
    motif_stamp = sync_git_checkout(motif_repo, motif_src, motif_rev, include_worktree_diff=True)
    motif_bld.mkdir(parents=True, exist_ok=True)
    motif_pfx.mkdir(parents=True, exist_ok=True)

    if not (motif_src / "configure").exists():
        timings["motif_autoreconf_s"] = run_checked(
            ["autoreconf", "-fi"], cwd=motif_src, env=base_env, log_path=output_dir / "motif-autoreconf.log"
        )
    if not (motif_bld / "Makefile").exists():
        timings["motif_configure_s"] = run_checked(
            [
                str(motif_src / "configure"),
                f"--prefix={motif_pfx}",
                "--disable-demos",
                "--disable-static",
                "--enable-xft",
                "--enable-unicode-text",
                "--enable-atspi",
                "--enable-svg",
                "--enable-xlibre-dpi",
                "CC=clang",
                "CXX=clang++",
                "CFLAGS=-O2 -g -std=c23",
                "CXXFLAGS=-O2 -g",
            ],
            cwd=motif_bld,
            env=base_env,
            log_path=output_dir / "motif-configure.log",
        )
    t_m = time.monotonic()
    for comp in ["bindings", "bitmaps", "config", "localized", "lib", "include", "tools/wml", "clients/uil"]:
        run_checked(["make", f"-j{jobs}", "-C", str(motif_bld / comp)], env=base_env, log_path=output_dir / f"motif-make-{comp.replace('/', '_')}.log")
        run_checked(["make", "-j1", "-C", str(motif_bld / comp), "install"], env=base_env, log_path=output_dir / f"motif-install-{comp.replace('/', '_')}.log")
    timings["motif_build_s"] = round(time.monotonic() - t_m, 2)

    libxm_so = motif_pfx / "lib" / "libXm.so.9"
    if not libxm_so.exists():
        raise SystemExit(f"missing {libxm_so}")

    # 2. ck-core-fs-lib
    fs_src = build_root / "fs-src"
    fs_bld = build_root / "fs-build"
    fs_pfx = build_root / "fs-prefix"
    sync_git_checkout(fs_repo, fs_src, fs_rev)
    t_fs = time.monotonic()
    run_checked(
        [
            "cmake",
            "-S",
            str(fs_src),
            "-B",
            str(fs_bld),
            "-GNinja",
            f"-DCMAKE_INSTALL_PREFIX={fs_pfx}",
            "-DCMAKE_INSTALL_LIBDIR=lib",
            "-DCK_CORE_FS_BUILD_BROWSER=OFF",
            "-DBUILD_TESTING=OFF",
            "-DCMAKE_C_COMPILER=clang",
            "-DCMAKE_CXX_COMPILER=clang++",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        ],
        env=base_env,
        log_path=output_dir / "fs-configure.log",
    )
    run_checked(["cmake", "--build", str(fs_bld), "--parallel", str(jobs)], env=base_env, log_path=output_dir / "fs-build.log")
    run_checked(["cmake", "--install", str(fs_bld)], env=base_env, log_path=output_dir / "fs-install.log")
    timings["fs_build_s"] = round(time.monotonic() - t_fs, 2)

    # 3. CK Core CDE
    cde_src = build_root / "cde-src"
    cde_bld = build_root / "cde-build"
    cde_stage = build_root / "cde-stage"
    cde_stamp = sync_git_checkout(cde_repo, cde_src, cde_rev, include_worktree_diff=True)
    cde_bld.mkdir(parents=True, exist_ok=True)

    cde_env = base_env.copy()
    xft_cflags = subprocess.check_output(["pkg-config", "--cflags", "xft"], text=True).strip()
    cde_env["PATH"] = f"{motif_pfx / 'bin'}:/usr/sbin:{cde_env['PATH']}"
    cde_env["PKG_CONFIG_PATH"] = str(fs_pfx / "lib" / "pkgconfig")
    cde_env["CPPFLAGS"] = f"-I{motif_pfx / 'include'} {xft_cflags}"
    cde_env["LDFLAGS"] = f"-L{motif_pfx / 'lib'} -Wl,-rpath,{motif_pfx / 'lib'} -Wl,-rpath,{fs_pfx / 'lib'}"
    cde_env["LD_LIBRARY_PATH"] = f"{motif_pfx / 'lib'}:{fs_pfx / 'lib'}"

    if not (cde_src / "cde" / "configure").exists():
        timings["cde_autogen_s"] = run_checked(
            ["./autogen.sh"], cwd=cde_src / "cde", env=cde_env, log_path=output_dir / "cde-autogen.log"
        )
    if not (cde_bld / "Makefile").exists():
        timings["cde_configure_s"] = run_checked(
            [
                str(cde_src / "cde" / "configure"),
                "--prefix=/usr/dt",
                "--with-ck-core-fs",
                "CC=clang",
                "CXX=clang++",
                "CFLAGS=-O2 -g",
                "CXXFLAGS=-O2 -g",
            ],
            cwd=cde_bld,
            env=cde_env,
            log_path=output_dir / "cde-configure.log",
        )
    t_cde = time.monotonic()
    run_checked(["make", "-j1", "-C", "programs/dtksh", "prepare-ksh"], cwd=cde_bld, env=cde_env, log_path=output_dir / "cde-prepare-ksh.log")
    run_checked(["make", "--output-sync=target", f"-j{jobs}"], cwd=cde_bld, env=cde_env, log_path=output_dir / "cde-build.log")
    run_checked(["fakeroot", "make", "-j1", f"DESTDIR={cde_stage}", "install"], cwd=cde_bld, env=cde_env, log_path=output_dir / "cde-install.log")
    timings["cde_build_s"] = round(time.monotonic() - t_cde, 2)

    if not args.skip_cde_check:
        timings["cde_dtksh_widget_check_s"] = run_checked(
            ["xvfb-run", "-a", "make", "-j1", "-C", "programs/dtksh", "check-widget"],
            cwd=cde_bld,
            env=cde_env,
            log_path=output_dir / "cde-dtksh-widget-check.log",
        )

    # 4. ck-core-apps (non-CEF targets)
    apps_src = build_root / "apps-src"
    apps_bld = build_root / "apps-build"
    apps_stamp = sync_git_checkout(apps_repo, apps_src, apps_rev, include_worktree_diff=True)
    apps_bld.mkdir(parents=True, exist_ok=True)
    cde_usr_dt = cde_stage / "usr" / "dt"
    t_apps = time.monotonic()
    run_checked(
        [
            "make",
            f"-j{jobs}",
            "CC=clang",
            "CXX=clang++",
            f"BUILD_DIR={apps_bld}",
            f"MOTIF_PREFIX={motif_pfx}",
            f"CDE_PREFIX={cde_usr_dt}",
            f"CDE_SRC_INC={cde_src / 'cde' / 'include'}",
            f"CDE_LDFLAGS=-L{motif_pfx / 'lib'} -Wl,-rpath,{motif_pfx / 'lib'} -L{cde_usr_dt / 'lib'} -Wl,-rpath,{cde_usr_dt / 'lib'}",
            *APPS_NON_CEF_TARGETS,
        ],
        cwd=apps_src,
        env=cde_env,
        log_path=output_dir / "apps-build.log",
    )
    timings["apps_build_s"] = round(time.monotonic() - t_apps, 2)

    # 5. Headless Dual-Output Wayland + Rootless XWayland Qualification Smoke Run
    run_root = build_root / "wayland-smoke-run"
    if run_root.exists():
        shutil.rmtree(run_root)
    xdg_runtime = run_root / "xdg"
    priv_home = run_root / "home"
    xdg_runtime.mkdir(parents=True, mode=0o700)
    priv_home.mkdir(parents=True, mode=0o700)
    os.chmod(xdg_runtime, 0o700)
    os.chmod(priv_home, 0o700)

    labwc_cfg = priv_home / ".config" / "labwc"
    labwc_cfg.mkdir(parents=True, exist_ok=True)
    (labwc_cfg / "rc.xml").write_text(
        '<?xml version="1.0"?>\n<labwc_config><core><xwayland>yes</xwayland></core></labwc_config>\n',
        encoding="utf-8",
    )

    wl_env = cde_env.copy()
    wl_env["HOME"] = str(priv_home)
    wl_env["XDG_RUNTIME_DIR"] = str(xdg_runtime)
    wl_env["WLR_BACKENDS"] = "headless"
    wl_env["WLR_HEADLESS_OUTPUTS"] = "2"
    wl_env["WLR_RENDERER"] = "pixman"
    wl_env["WLR_LIBINPUT_NO_DEVICES"] = "1"
    wl_env["LD_LIBRARY_PATH"] = f"{motif_pfx / 'lib'}:{cde_usr_dt / 'lib'}:{fs_pfx / 'lib'}"

    x11_unix_dir = Path("/tmp/.X11-unix")
    pre_existing_x = set(x11_unix_dir.glob("X*")) if x11_unix_dir.exists() else set()

    status_file = run_root / "compositor.status"
    comp_bin = shutil.which(args.compositor) or args.compositor
    comp_cmd = [comp_bin]
    if "dtwm-wayland" in Path(comp_bin).name:
        comp_cmd.extend(["--status-file", str(status_file)])

    t_smoke = time.monotonic()
    comp_log = (output_dir / "wayland-compositor.log").open("w", encoding="utf-8")
    comp_proc = subprocess.Popen(
        comp_cmd,
        env=wl_env,
        stdout=comp_log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    spawned: list[subprocess.Popen[bytes]] = [comp_proc]
    verified_maps: dict[str, str] = {}

    try:
        wayland_sock = None
        x11_sock = None
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            if comp_proc.poll() is not None:
                raise SystemExit(f"compositor exited prematurely with {comp_proc.returncode}")
            if status_file.is_file():
                kv = dict(
                    ln.split("=", 1)
                    for ln in status_file.read_text(encoding="utf-8").splitlines()
                    if "=" in ln
                )
                wayland_sock = kv.get("WAYLAND_DISPLAY")
                x11_sock = kv.get("DISPLAY")
                if wayland_sock and x11_sock:
                    break
            else:
                wl_candidates = sorted(xdg_runtime.glob("wayland-*"))
                wl_candidates = [p for p in wl_candidates if not p.name.endswith(".lock")]
                new_x = sorted((set(x11_unix_dir.glob("X*")) if x11_unix_dir.exists() else set()) - pre_existing_x)
                if wl_candidates and new_x:
                    wayland_sock = wl_candidates[0].name
                    x11_sock = ":" + new_x[-1].name[1:]
                    break
            time.sleep(0.1)

        if not wayland_sock or not x11_sock:
            raise SystemExit(f"timed out waiting for WAYLAND_DISPLAY ({wayland_sock}) / XWayland DISPLAY ({x11_sock})")

        client_env = wl_env.copy()
        client_env["WAYLAND_DISPLAY"] = wayland_sock
        client_env["DISPLAY"] = x11_sock

        clients_to_launch = [
            ("foot-wayland", ["foot"]),
            ("dtcalc", [str(cde_usr_dt / "bin" / "dtcalc")]),
            ("dtpad", [str(cde_usr_dt / "bin" / "dtpad"), "-standAlone"]),
            ("ck-clock", [str(apps_bld / "bin" / "ck-clock")]),
            ("ck-eyes", [str(apps_bld / "bin" / "ck-eyes")]),
        ]

        named_procs: list[tuple[str, subprocess.Popen[bytes], bool]] = []
        for name, cmd in clients_to_launch:
            clog = (output_dir / f"client-{name}.log").open("w", encoding="utf-8")
            p = subprocess.Popen(cmd, env=client_env, stdout=clog, stderr=subprocess.STDOUT, start_new_session=True)
            spawned.append(p)
            named_procs.append((name, p, name != "foot-wayland"))

        time.sleep(2.5)
        for name, p, uses_motif in named_procs:
            if p.poll() is not None:
                raise SystemExit(f"client {name} exited prematurely with {p.returncode}")
            if uses_motif:
                verified_maps[name] = verify_pid_maps_libxm(p.pid, libxm_so)

        # Drive virtual pointer movement on the headless Wayland compositor and capture outputs via screencopy
        run_checked(["wlrctl", "pointer", "move", "240", "180"], env=client_env)
        time.sleep(0.5)

        shot_all = output_dir / "wayland-dual-output.png"
        shot_out1 = output_dir / "wayland-headless-1.png"
        shot_out2 = output_dir / "wayland-headless-2.png"
        run_checked(["grim", str(shot_all)], env=client_env)
        run_checked(["grim", "-o", "HEADLESS-1", str(shot_out1)], env=client_env)
        run_checked(["grim", "-o", "HEADLESS-2", str(shot_out2)], env=client_env)

        from PIL import Image

        with Image.open(shot_all) as im:
            full_dims = im.size
            extrema = im.convert("RGB").getextrema()
            if extrema == ((0, 0), (0, 0), (0, 0)):
                raise SystemExit("captured Wayland frame is completely black")
    finally:
        for p in reversed(spawned):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        time.sleep(0.5)
        for p in reversed(spawned):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                p.wait(timeout=5)
        shutil.rmtree(run_root, ignore_errors=True)

    timings["wayland_smoke_s"] = round(time.monotonic() - t_smoke, 2)
    timings["total_elapsed_s"] = round(time.monotonic() - t_total, 2)

    dtwm_bin = cde_usr_dt / "bin" / "dtwm"
    receipt = {
        "work_package": "WP-00",
        "started_utc": started_utc,
        "finished_utc": utc_now(),
        "guest_os": platform.platform(),
        "parallel_jobs": jobs,
        "revisions": {
            "ck_core_cde": cde_stamp,
            "ck_core_motif": motif_stamp,
            "ck_core_fs_lib": fs_rev,
            "ck_core_apps": apps_stamp,
        },
        "artifacts": {
            "motif_prefix": str(motif_pfx),
            "libXm_so_9_sha256": sha256_file(libxm_so.resolve()),
            "dtwm_sha256": sha256_file(dtwm_bin),
            "ck_eyes_sha256": sha256_file(apps_bld / "bin" / "ck-eyes"),
            "wayland_dual_output_png": str(shot_all),
            "wayland_dual_output_png_sha256": sha256_file(shot_all),
            "wayland_dual_output_dimensions": list(full_dims),
        },
        "wayland_session": {
            "compositor": args.compositor,
            "wayland_display": wayland_sock,
            "xwayland_display": x11_sock,
            "outputs": ["HEADLESS-1", "HEADLESS-2"],
            "verified_libXm_maps": verified_maps,
        },
        "timings": timings,
        "result": "pass",
    }
    receipt_path = output_dir / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in-guest", action="store_true", help="Run directly inside the Linux guest")
    ap.add_argument("--vm-name", default="cde-ubuntu2604", help="Lima VM instance name")
    ap.add_argument(
        "--task-root",
        default="/Volumes/S8T/tmp/cde-wayland-wp00-9a09ad77",
        help="Mac temporary workspace root beneath /Volumes/S8T/tmp",
    )
    ap.add_argument("--cde-repo", default="/Volumes/S8T/tmp/cde-wayland-wp00-9a09ad77/ck-core-cde-wt")
    ap.add_argument("--motif-repo", default="/Volumes/S8T/tmp/cde-wayland-wp00-9a09ad77/ck-core-motif-wt")
    ap.add_argument("--motif-revision", default="", help="Explicit Motif commit (default: motif-repo HEAD)")
    ap.add_argument("--fs-repo", default="/Volumes/PRO-BLADE/git/ck-core-fs-lib")
    ap.add_argument("--fs-revision", default="", help="Explicit ck-core-fs-lib commit")
    ap.add_argument("--apps-repo", default="/Volumes/S8T/tmp/cde-wayland-wp00-9a09ad77/ck-core-apps-wt")
    ap.add_argument("--guest-build-root", default="/var/tmp/ck-wayland-build/wp00")
    ap.add_argument("--output-dir", default="", help="Durable receipt & screenshot output directory")
    ap.add_argument("--compositor", default="/var/tmp/ck-wayland-build/wp00/cde-stage/usr/dt/bin/dtwm-wayland", help="Wayland compositor binary for headless smoke run")
    ap.add_argument("--jobs", type=int, default=MAX_JOBS, help="Max parallel jobs (capped at 6)")
    ap.add_argument("--skip-cde-check", action="store_true", help="Skip Xvfb dtksh widget check")
    args = ap.parse_args()

    if args.in_guest:
        return guest_run(args)
    return host_dispatch_to_lima(args)


if __name__ == "__main__":
    sys.exit(main())
