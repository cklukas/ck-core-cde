#!/usr/bin/env python3
"""Build matched Ubuntu/Debian .deb packages and APT repository for CK Core Motif, fs-lib, CDE, dtlogin, and Apps.

Produces:
  - ck-core-motif9_2.4.0-1_<arch>.deb
  - ck-core-fs-lib_0.3.0-1_<arch>.deb
  - ck-core-cde_2.5.3-1_<arch>.deb
  - ck-core-dtlogin_2.5.3-1_<arch>.deb
  - ck-core-apps_1.0.0-1_<arch>.deb
Plus APT repository metadata (Packages, Packages.gz, Release) and an optional
end-to-end `apt-get install` + coexistence with system `libxm4` + runtime `/usr/dt/lib/libXm.so.9` verification.
"""

from __future__ import annotations

import argparse
import datetime as dt
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

DTLOGIN_BINS = {"dtlogin", "dtgreet", "dtchooser"}


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_mac_temp_root(p: Path) -> None:
    resolved = str(p.resolve())
    if resolved.startswith("/private/tmp") or resolved.startswith("/tmp"):
        raise SystemExit(f"error: refusing system temporary path on macOS: {resolved}")
    if Path("/Volumes/S8T").is_dir():
        if not resolved.startswith("/Volumes/S8T/tmp/"):
            raise SystemExit(f"error: path must be beneath /Volumes/S8T/tmp/: {resolved}")
    elif Path("/Volumes/PRO-BLADE").is_dir():
        if not resolved.startswith("/Volumes/PRO-BLADE/tmp/"):
            raise SystemExit(f"error: path must be beneath /Volumes/PRO-BLADE/tmp/: {resolved}")
    else:
        raise SystemExit("error: neither /Volumes/S8T nor /Volumes/PRO-BLADE is mounted")


def host_dispatch(args: argparse.Namespace) -> int:
    task_root = Path(args.task_root).resolve()
    verify_mac_temp_root(task_root)
    out_dir = Path(args.output_dir).resolve() if args.output_dir else (task_root / "receipts" / "wp01-apt-repo")
    verify_mac_temp_root(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["TMPDIR"] = str(task_root / "tmp")
    env["LIMA_HOME"] = str(task_root / "lima")
    env["XDG_CACHE_HOME"] = str(task_root / "cache")

    cmd = [
        "limactl",
        "shell",
        args.vm_name,
        "--",
        "python3",
        str(Path(__file__).resolve()),
        "--in-guest",
        "--cde-repo",
        str(Path(args.cde_repo).resolve()),
        "--guest-build-root",
        args.guest_build_root,
        "--output-dir",
        str(out_dir),
    ]
    if args.verify_install:
        cmd.append("--verify-install")
    return subprocess.call(cmd, env=env)


def is_elf(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    try:
        with path.open("rb") as fh:
            return fh.read(4) == b"\x7fELF"
    except OSError:
        return False


def rewrite_rpath_and_collect_needed(pkg_root: Path) -> set[str]:
    needed: set[str] = set()
    needed_re = re.compile(r"\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]")
    for p in pkg_root.rglob("*"):
        if not is_elf(p):
            continue
        # Ensure writable briefly for patchelf if installed 0555/0444
        mode = p.stat().st_mode
        os.chmod(p, mode | 0o200)
        subprocess.run(["patchelf", "--set-rpath", "/usr/dt/lib", str(p)], check=True)
        out = subprocess.check_output(["readelf", "-d", str(p)], text=True, errors="replace")
        for m in needed_re.finditer(out):
            needed.add(m.group(1))
        os.chmod(p, mode)
    return needed


def resolve_system_deb_deps(sonames: set[str], internal_sonames: set[str], multiarch: str) -> list[str]:
    search_dirs = [
        Path(f"/usr/lib/{multiarch}"),
        Path(f"/lib/{multiarch}"),
        Path("/usr/lib"),
        Path("/lib"),
    ]
    pkgs: set[str] = set()
    for soname in sorted(sonames):
        if soname in internal_sonames or soname.startswith(("ld-linux", "linux-vdso")):
            continue
        if soname.startswith(("libXm.so", "libMrm.so", "libUil.so", "libDt", "libtt.so", "libcsa.so", "libck_")):
            continue
        found_lib: Path | None = None
        for d in search_dirs:
            cand = d / soname
            if cand.exists():
                found_lib = cand
                break
        if not found_lib:
            continue
        # Query dpkg -S on both the path and usr-merged / non-usr-merged equivalents
        candidates = [str(found_lib), str(found_lib.resolve())]
        if str(found_lib).startswith("/usr/lib/"):
            candidates.append(str(found_lib)[4:])
        elif str(found_lib).startswith("/lib/"):
            candidates.append("/usr" + str(found_lib))
        owner = None
        for c in candidates:
            res = subprocess.run(["dpkg", "-S", c], capture_output=True, text=True)
            if res.returncode == 0 and res.stdout.strip():
                owner = res.stdout.splitlines()[0].split(":", 1)[0].strip()
                break
        if owner:
            pkgs.add(owner)
    return sorted(pkgs)


def installed_size_kb(root: Path) -> int:
    total = 0
    for p in root.rglob("*"):
        if not p.is_symlink() and p.is_file():
            total += p.stat().st_size
    return max(1, (total + 1023) // 1024)


def write_control(pkg_root: Path, fields: dict[str, str]) -> None:
    debian_dir = pkg_root / "DEBIAN"
    debian_dir.mkdir(parents=True, exist_ok=True)
    lines = [f"{k}: {v}" for k, v in fields.items() if v]
    (debian_dir / "control").write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_deb(pkg_root: Path, out_deb: Path) -> None:
    if out_deb.exists():
        out_deb.unlink()
    subprocess.run(
        ["fakeroot", "dpkg-deb", "-Zxz", "--build", "--root-owner-group", str(pkg_root), str(out_deb)],
        check=True,
    )


def guest_build_and_verify(args: argparse.Namespace) -> int:
    t0 = time.monotonic()
    started_utc = utc_now()
    cde_repo = Path(args.cde_repo)
    build_root = Path(args.guest_build_root)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Ensure incremental build is completely fresh (including dtlogin.src update)
    harness = cde_repo / "scripts" / "run-lima-wayland-harness.py"
    subprocess.run(
        [
            "python3",
            str(harness),
            "--in-guest",
            "--cde-repo",
            str(cde_repo),
            "--guest-build-root",
            str(build_root),
            "--output-dir",
            str(out_dir / "harness-sync"),
            "--skip-cde-check",
        ],
        check=True,
    )

    arch = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
    multiarch = subprocess.check_output(["dpkg-architecture", "-qDEB_HOST_MULTIARCH"], text=True).strip()

    motif_pfx = build_root / "motif-prefix"
    fs_pfx = build_root / "fs-prefix"
    cde_stage = build_root / "cde-stage"
    apps_bld = build_root / "apps-build"

    deb_work = build_root / "deb-work"
    if deb_work.exists():
        shutil.rmtree(deb_work)
    deb_work.mkdir(parents=True)

    # Collect internal SONAMEs provided by our /usr/dt/lib stack
    internal_sonames: set[str] = set()
    for pfx in (motif_pfx / "lib", fs_pfx / "lib", cde_stage / "usr" / "dt" / "lib"):
        if pfx.is_dir():
            for f in pfx.iterdir():
                if ".so" in f.name:
                    internal_sonames.add(f.name)

    built_debs: dict[str, Path] = {}

    # 1. ck-core-motif9
    r_motif = deb_work / "ck-core-motif9"
    (r_motif / "usr" / "dt").mkdir(parents=True)
    shutil.copytree(motif_pfx, r_motif / "usr" / "dt", symlinks=True, dirs_exist_ok=True)
    for la in (r_motif / "usr" / "dt").rglob("*.la"):
        la.unlink()
    (r_motif / "etc" / "ld.so.conf.d").mkdir(parents=True, exist_ok=True)
    (r_motif / "etc" / "ld.so.conf.d" / "ck-core-cde.conf").write_text("/usr/dt/lib\n", encoding="utf-8")
    m_needed = rewrite_rpath_and_collect_needed(r_motif)
    m_deps = resolve_system_deb_deps(m_needed, internal_sonames, multiarch)
    write_control(
        r_motif,
        {
            "Package": "ck-core-motif9",
            "Version": "2.4.0-1",
            "Section": "libs",
            "Priority": "optional",
            "Architecture": arch,
            "Installed-Size": str(installed_size_kb(r_motif)),
            "Depends": ", ".join(m_deps),
            "Maintainer": "CK Core Project <cklukas@users.noreply.github.com>",
            "Description": "CK Core Motif 2.4.0 native ABI 9 runtime, headers, and UIL compiler\n"
            " Installed into /usr/dt to coexist cleanly with system Motif 4 (libxm4).",
        },
    )
    for script_name in ("postinst", "postrm"):
        sp = r_motif / "DEBIAN" / script_name
        sp.write_text("#!/bin/sh\nset -e\nldconfig\nexit 0\n", encoding="utf-8")
        os.chmod(sp, 0o755)
    deb_motif = out_dir / f"ck-core-motif9_2.4.0-1_{arch}.deb"
    build_deb(r_motif, deb_motif)
    built_debs["ck-core-motif9"] = deb_motif

    # 2. ck-core-fs-lib
    r_fs = deb_work / "ck-core-fs-lib"
    (r_fs / "usr" / "dt").mkdir(parents=True)
    shutil.copytree(fs_pfx, r_fs / "usr" / "dt", symlinks=True, dirs_exist_ok=True)
    for pc in (r_fs / "usr" / "dt" / "lib" / "pkgconfig").glob("*.pc"):
        txt = pc.read_text(encoding="utf-8").replace(str(fs_pfx), "/usr/dt")
        pc.write_text(txt, encoding="utf-8")
    fs_needed = rewrite_rpath_and_collect_needed(r_fs)
    fs_deps = resolve_system_deb_deps(fs_needed, internal_sonames, multiarch)
    write_control(
        r_fs,
        {
            "Package": "ck-core-fs-lib",
            "Version": "0.3.0-1",
            "Section": "libs",
            "Priority": "optional",
            "Architecture": arch,
            "Installed-Size": str(installed_size_kb(r_fs)),
            "Depends": ", ".join(fs_deps),
            "Maintainer": "CK Core Project <cklukas@users.noreply.github.com>",
            "Description": "CK Core headless filesystem synchronization engine (0.3 ABI)",
        },
    )
    deb_fs = out_dir / f"ck-core-fs-lib_0.3.0-1_{arch}.deb"
    build_deb(r_fs, deb_fs)
    built_debs["ck-core-fs-lib"] = deb_fs

    # 3. ck-core-cde & 4. ck-core-dtlogin
    r_cde = deb_work / "ck-core-cde"
    r_dtlogin = deb_work / "ck-core-dtlogin"
    shutil.copytree(cde_stage, r_cde, symlinks=True)
    for la in r_cde.rglob("*.la"):
        la.unlink()
    for req_pal in ("ModernLight.dp", "ModernDark.dp", "HighContrast.dp"):
        if not (r_cde / "usr" / "dt" / "palettes" / req_pal).is_file():
            raise SystemExit(f"error: expected built-in appearance palette missing from stage: {req_pal}")

    # Split dtlogin binaries and /etc/pam.d/dtlogin into ck-core-dtlogin
    (r_dtlogin / "usr" / "dt" / "bin").mkdir(parents=True, exist_ok=True)
    for bname in DTLOGIN_BINS:
        src_b = r_cde / "usr" / "dt" / "bin" / bname
        if src_b.exists():
            shutil.move(str(src_b), str(r_dtlogin / "usr" / "dt" / "bin" / bname))
    pam_dtlogin = r_cde / "etc" / "pam.d" / "dtlogin"
    if pam_dtlogin.exists():
        (r_dtlogin / "etc" / "pam.d").mkdir(parents=True, exist_ok=True)
        shutil.move(str(pam_dtlogin), str(r_dtlogin / "etc" / "pam.d" / "dtlogin"))

    deb_meta = cde_repo / "cde" / "debian"
    # Session .desktop entries and Wayland Xsession entry point
    (r_cde / "usr" / "share" / "xsessions").mkdir(parents=True, exist_ok=True)
    (r_cde / "usr" / "share" / "wayland-sessions").mkdir(parents=True, exist_ok=True)
    shutil.copy2(deb_meta / "cde.desktop", r_cde / "usr" / "share" / "xsessions" / "cde.desktop")
    shutil.copy2(deb_meta / "cde-wayland.desktop", r_cde / "usr" / "share" / "wayland-sessions" / "cde-wayland.desktop")
    shutil.copy2(deb_meta / "Xsession.wayland", r_cde / "usr" / "dt" / "bin" / "Xsession.wayland")
    os.chmod(r_cde / "usr" / "dt" / "bin" / "Xsession.wayland", 0o755)

    (r_cde / "etc" / "profile.d").mkdir(parents=True, exist_ok=True)
    (r_cde / "etc" / "profile.d" / "ck-core-cde.sh").write_text(
        'case ":${PATH}:" in *:/usr/dt/bin:*) ;; *) export PATH="${PATH}:/usr/dt/bin" ;; esac\n',
        encoding="utf-8",
    )

    cde_needed = rewrite_rpath_and_collect_needed(r_cde)
    cde_shlibs = resolve_system_deb_deps(cde_needed, internal_sonames, multiarch)
    cde_deps = [
        "ck-core-motif9 (= 2.4.0-1)",
        "ck-core-fs-lib (= 0.3.0-1)",
        "ksh93u+m | ksh",
        "rpcbind",
        "x11-xserver-utils",
        "xfonts-base",
        "xwayland | xserver-xorg-core",
        *cde_shlibs,
    ]
    write_control(
        r_cde,
        {
            "Package": "ck-core-cde",
            "Version": "2.5.3-1",
            "Section": "x11",
            "Priority": "optional",
            "Architecture": arch,
            "Installed-Size": str(installed_size_kb(r_cde)),
            "Depends": ", ".join(dict.fromkeys(cde_deps)),
            "Recommends": "ck-core-apps, xfonts-100dpi",
            "Conflicts": "cde-desktop",
            "Replaces": "cde-desktop",
            "Maintainer": "CK Core Project <cklukas@users.noreply.github.com>",
            "Description": "CK Core Common Desktop Environment (X11/XLibre and Wayland)\n"
            " Modernized C23 Common Desktop Environment built against CK Core Motif ABI 9.",
        },
    )
    for script_name in ("postinst", "postrm"):
        sp = r_cde / "DEBIAN" / script_name
        sp.write_text("#!/bin/sh\nset -e\nldconfig\nexit 0\n", encoding="utf-8")
        os.chmod(sp, 0o755)
    deb_cde = out_dir / f"ck-core-cde_2.5.3-1_{arch}.deb"
    build_deb(r_cde, deb_cde)
    built_debs["ck-core-cde"] = deb_cde

    # Finish ck-core-dtlogin
    (r_dtlogin / "lib" / "systemd" / "system").mkdir(parents=True, exist_ok=True)
    shutil.copy2(deb_meta / "dtlogin.service", r_dtlogin / "lib" / "systemd" / "system" / "dtlogin.service")
    dtl_needed = rewrite_rpath_and_collect_needed(r_dtlogin)
    dtl_shlibs = resolve_system_deb_deps(dtl_needed, internal_sonames, multiarch)
    dtl_deps = [
        "ck-core-cde (= 2.5.3-1)",
        "debconf (>= 0.5) | debconf-2.0",
        *dtl_shlibs,
    ]
    write_control(
        r_dtlogin,
        {
            "Package": "ck-core-dtlogin",
            "Version": "2.5.3-1",
            "Section": "x11",
            "Priority": "optional",
            "Architecture": arch,
            "Installed-Size": str(installed_size_kb(r_dtlogin)),
            "Depends": ", ".join(dict.fromkeys(dtl_deps)),
            "Provides": "x-display-manager",
            "Maintainer": "CK Core Project <cklukas@users.noreply.github.com>",
            "Description": "CK Core CDE graphical login and display manager",
        },
    )
    shutil.copy2(deb_meta / "ck-core-dtlogin.templates", r_dtlogin / "DEBIAN" / "templates")
    for sfx in ("config", "postinst", "prerm", "postrm"):
        dst_s = r_dtlogin / "DEBIAN" / sfx
        shutil.copy2(deb_meta / f"ck-core-dtlogin.{sfx}", dst_s)
        os.chmod(dst_s, 0o755)
    deb_dtlogin = out_dir / f"ck-core-dtlogin_2.5.3-1_{arch}.deb"
    build_deb(r_dtlogin, deb_dtlogin)
    built_debs["ck-core-dtlogin"] = deb_dtlogin

    # 5. ck-core-apps
    r_apps = deb_work / "ck-core-apps"
    (r_apps / "usr" / "dt" / "bin").mkdir(parents=True)
    (r_apps / "usr" / "bin").mkdir(parents=True)
    for bin_file in sorted((apps_bld / "bin").iterdir()):
        if bin_file.is_file():
            dst_b = r_apps / "usr" / "dt" / "bin" / bin_file.name
            shutil.copy2(bin_file, dst_b)
            os.chmod(dst_b, 0o755)
            (r_apps / "usr" / "bin" / bin_file.name).symlink_to(f"/usr/dt/bin/{bin_file.name}")
    apps_needed = rewrite_rpath_and_collect_needed(r_apps)
    apps_shlibs = resolve_system_deb_deps(apps_needed, internal_sonames, multiarch)
    apps_deps = [
        "ck-core-motif9 (= 2.4.0-1)",
        "ck-core-cde (= 2.5.3-1)",
        *apps_shlibs,
    ]
    write_control(
        r_apps,
        {
            "Package": "ck-core-apps",
            "Version": "1.0.0-1",
            "Section": "x11",
            "Priority": "optional",
            "Architecture": arch,
            "Installed-Size": str(installed_size_kb(r_apps)),
            "Depends": ", ".join(dict.fromkeys(apps_deps)),
            "Maintainer": "CK Core Project <cklukas@users.noreply.github.com>",
            "Description": "CK Core companion desktop applications for CDE (ABI 9)\n"
            " Includes ck-clock, ck-load, ck-tasks, ck-calc, ck-eyes, ck-coins, ck-grab, and games.",
        },
    )
    deb_apps = out_dir / f"ck-core-apps_1.0.0-1_{arch}.deb"
    build_deb(r_apps, deb_apps)
    built_debs["ck-core-apps"] = deb_apps

    # Generate APT repository metadata (Packages, Packages.gz, Release)
    pkgs_raw = subprocess.check_output(["dpkg-scanpackages", "--multiversion", "."], cwd=out_dir)
    (out_dir / "Packages").write_bytes(pkgs_raw)
    with gzip.open(out_dir / "Packages.gz", "wb") as gz:
        gz.write(pkgs_raw)

    p_bytes = (out_dir / "Packages").read_bytes()
    pgz_bytes = (out_dir / "Packages.gz").read_bytes()
    release_txt = (
        "Origin: CK Core\n"
        "Label: CK Core CDE & Motif\n"
        "Suite: resolute\n"
        "Codename: resolute\n"
        f"Architectures: {arch}\n"
        f"Date: {dt.datetime.now(dt.timezone.utc).strftime('%a, %d %b %Y %H:%M:%S UTC')}\n"
        "SHA256:\n"
        f" {hashlib.sha256(p_bytes).hexdigest()} {len(p_bytes):16d} Packages\n"
        f" {hashlib.sha256(pgz_bytes).hexdigest()} {len(pgz_bytes):16d} Packages.gz\n"
    )
    (out_dir / "Release").write_text(release_txt, encoding="utf-8")

    verification: dict[str, object] = {"verified_install": False}

    if args.verify_install:
        # Copy repo to local guest directory readable by _apt user (0755)
        guest_repo = Path("/var/tmp/ck-apt-repo-verify")
        if guest_repo.exists():
            shutil.rmtree(guest_repo)
        guest_repo.mkdir(parents=True, mode=0o755)
        for item in out_dir.iterdir():
            if item.is_file():
                shutil.copy2(item, guest_repo / item.name)
                os.chmod(guest_repo / item.name, 0o644)

        list_file = Path("/etc/apt/sources.list.d/ck-core-verify.list")
        try:
            subprocess.run(
                ["sudo", "tee", str(list_file)],
                input=f"deb [trusted=yes] file:{guest_repo} ./\n",
                text=True,
                stdout=subprocess.DEVNULL,
                check=True,
            )
            subprocess.run(["sudo", "apt-get", "update", "-o", f"Dir::Etc::sourcelist={list_file}", "-o", "Dir::Etc::sourceparts=-", "-o", "APT::Get::List-Cleanup=0"], check=True)
            subprocess.run(
                [
                    "sudo",
                    "env",
                    "DEBIAN_FRONTEND=noninteractive",
                    "apt-get",
                    "install",
                    "-y",
                    "ck-core-cde",
                    "ck-core-apps",
                    "ck-core-dtlogin",
                    "libxm4",
                ],
                check=True,
            )

            # Verify installed files & pam_systemd presence
            for must_exist in [
                Path("/usr/dt/lib/libXm.so.9"),
                Path("/usr/dt/bin/dtwm"),
                Path("/usr/dt/bin/dtsession"),
                Path("/usr/dt/bin/dtlogin"),
                Path("/usr/dt/bin/Xsession.wayland"),
                Path("/usr/share/xsessions/cde.desktop"),
                Path("/usr/share/wayland-sessions/cde-wayland.desktop"),
                Path("/lib/systemd/system/dtlogin.service"),
                Path("/etc/pam.d/dtlogin"),
                Path("/etc/pam.d/dtsession"),
                Path("/usr/bin/ck-clock"),
            ]:
                if not must_exist.exists():
                    raise SystemExit(f"missing installed path: {must_exist}")

            pam_txt = Path("/etc/pam.d/dtlogin").read_text(encoding="utf-8")
            if "pam_systemd.so" not in pam_txt:
                raise SystemExit("/etc/pam.d/dtlogin is missing pam_systemd.so")

            # Launch installed /usr/dt/bin/dtpad and /usr/bin/ck-clock WITHOUT LD_LIBRARY_PATH
            # under headless Wayland + XWayland to prove /usr/dt/lib/libXm.so.9 loads cleanly alongside Ubuntu's libxm4!
            v_root = Path("/var/tmp/ck-deb-runtime-check")
            shutil.rmtree(v_root, ignore_errors=True)
            xdg_d = v_root / "xdg"
            home_d = v_root / "home"
            xdg_d.mkdir(parents=True, mode=0o700)
            home_d.mkdir(parents=True, mode=0o700)
            os.chmod(xdg_d, 0o700)
            os.chmod(home_d, 0o700)
            (home_d / ".config" / "labwc").mkdir(parents=True)
            (home_d / ".config" / "labwc" / "rc.xml").write_text(
                '<?xml version="1.0"?>\n<labwc_config><core><xwayland>yes</xwayland></core></labwc_config>\n',
                encoding="utf-8",
            )
            clean_env = {
                "PATH": "/usr/local/bin:/usr/bin:/bin:/usr/dt/bin",
                "HOME": str(home_d),
                "XDG_RUNTIME_DIR": str(xdg_d),
                "LANG": "C.UTF-8",
                "WLR_BACKENDS": "headless",
                "WLR_HEADLESS_OUTPUTS": "1",
                "WLR_RENDERER": "pixman",
                "WLR_LIBINPUT_NO_DEVICES": "1",
            }
            pre_x = set(Path("/tmp/.X11-unix").glob("X*")) if Path("/tmp/.X11-unix").exists() else set()
            comp = subprocess.Popen(["labwc"], env=clean_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs = [comp]
            mapped_installed: dict[str, str] = {}
            try:
                wl_s, x_s = None, None
                for _ in range(75):
                    wls = [p for p in xdg_d.glob("wayland-*") if not p.name.endswith(".lock")]
                    nxs = sorted((set(Path("/tmp/.X11-unix").glob("X*")) if Path("/tmp/.X11-unix").exists() else set()) - pre_x)
                    if wls and nxs:
                        wl_s = wls[0].name
                        x_s = ":" + nxs[-1].name[1:]
                        break
                    time.sleep(0.2)
                if not wl_s or not x_s:
                    raise SystemExit("Wayland/XWayland failed to start during .deb verification")
                c_env = {**clean_env, "WAYLAND_DISPLAY": wl_s, "DISPLAY": x_s}
                p_pad = subprocess.Popen(["/usr/dt/bin/dtpad", "-standAlone"], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                p_clk = subprocess.Popen(["/usr/bin/ck-clock"], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                procs.extend([p_pad, p_clk])
                time.sleep(2.0)
                for label, proc in [("dtpad", p_pad), ("ck-clock", p_clk)]:
                    if proc.poll() is not None:
                        raise SystemExit(f"installed {label} exited with {proc.returncode}")
                    maps = Path(f"/proc/{proc.pid}/maps").read_text(encoding="utf-8", errors="replace")
                    xm_lines = [ln.split()[-1] for ln in maps.splitlines() if "libXm.so" in ln]
                    if not xm_lines or not xm_lines[0].startswith("/usr/dt/lib/libXm.so.9"):
                        raise SystemExit(f"{label} mapped unexpected libXm: {xm_lines}")
                    mapped_installed[label] = xm_lines[0]
            finally:
                for pr in reversed(procs):
                    if pr.poll() is None:
                        try:
                            os.killpg(pr.pid, 15)
                        except ProcessLookupError:
                            pass
                time.sleep(0.4)
                for pr in reversed(procs):
                    if pr.poll() is None:
                        try:
                            os.killpg(pr.pid, 9)
                        except ProcessLookupError:
                            pass
                shutil.rmtree(v_root, ignore_errors=True)

            verification = {
                "verified_install": True,
                "coexists_with_ubuntu_libxm4": True,
                "no_ld_library_path_maps": mapped_installed,
            }
        finally:
            subprocess.run(
                [
                    "sudo",
                    "env",
                    "DEBIAN_FRONTEND=noninteractive",
                    "apt-get",
                    "purge",
                    "-y",
                    "ck-core-dtlogin",
                    "ck-core-apps",
                    "ck-core-cde",
                    "ck-core-fs-lib",
                    "ck-core-motif9",
                ],
                check=False,
            )
            subprocess.run(["sudo", "rm", "-f", str(list_file)], check=False)
            shutil.rmtree(guest_repo, ignore_errors=True)

    shutil.rmtree(deb_work, ignore_errors=True)

    # Also run dtlogin's unit tests to confirm dtlogin.src change breaks nothing
    dtl_tmp = build_root / "tmp"
    dtl_tmp.mkdir(parents=True, exist_ok=True)
    dtl_env = os.environ.copy()
    dtl_env["TMPDIR"] = str(dtl_tmp)
    dtl_env["LD_LIBRARY_PATH"] = f"{motif_pfx / 'lib'}:{fs_pfx / 'lib'}"
    subprocess.run(["make", "-j4", "-C", str(build_root / "cde-build" / "programs" / "dtlogin"), "check"], env=dtl_env, check=True)

    receipt = {
        "work_package": "WP-01",
        "started_utc": started_utc,
        "finished_utc": utc_now(),
        "elapsed_s": round(time.monotonic() - t0, 2),
        "architecture": arch,
        "packages": {
            name: {
                "filename": p.name,
                "size_bytes": p.stat().st_size,
                "sha256": sha256_file(p),
            }
            for name, p in built_debs.items()
        },
        "verification": verification,
        "dtlogin_make_check": "pass",
        "result": "pass",
    }
    (out_dir / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in-guest", action="store_true")
    ap.add_argument("--vm-name", default="cde-ubuntu2604")
    ap.add_argument("--task-root", default="/Volumes/S8T/tmp/cde-wayland-wp00-9a09ad77")
    ap.add_argument("--cde-repo", default="/Volumes/S8T/tmp/cde-wayland-wp00-9a09ad77/ck-core-cde-wt")
    ap.add_argument("--guest-build-root", default="/var/tmp/ck-wayland-build/wp00")
    ap.add_argument("--output-dir", default="")
    ap.add_argument("--verify-install", action="store_true", help="Test real `apt-get install` + coexistence with libxm4")
    args = ap.parse_args()
    if args.in_guest:
        return guest_build_and_verify(args)
    return host_dispatch(args)


if __name__ == "__main__":
    sys.exit(main())
