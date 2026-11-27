#!/usr/bin/env python3
"""Stage and build Fedora / RHEL / openSUSE RPM packages for the CK Core CDE cohort.

Stages the 5-package cohort (`ck-core-motif9`, `ck-core-fs-lib`, `ck-core-cde`,
`ck-core-dtlogin`, `ck-core-apps`) from a built prefix root, generates exact
per-subpackage `%files` manifests and distro-conditional RPM build tree, and
invokes `rpmbuild -bb --with prebuilt_stage` (plus `createrepo_c` if present)
when `rpmbuild` is available, or validates the staged RPM tree with `--stage-only`.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

DTLOGIN_BINS = {"dtlogin", "dtgreet", "dtchooser"}


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def verify_mac_temp_root(p: Path) -> None:
    if sys.platform != "darwin":
        return
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


def is_elf(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    try:
        with path.open("rb") as fh:
            return fh.read(4) == b"\x7fELF"
    except OSError:
        return False


def rewrite_rpath(root: Path) -> int:
    patchelf = shutil.which("patchelf")
    count = 0
    for p in root.rglob("*"):
        if not is_elf(p):
            continue
        count += 1
        if patchelf:
            mode = p.stat().st_mode
            os.chmod(p, mode | 0o200)
            subprocess.run([patchelf, "--set-rpath", "/usr/dt/lib", str(p)], check=True)
            os.chmod(p, mode)
    return count


def collect_rel_paths(tree: Path) -> list[str]:
    items: list[str] = []
    if not tree.exists():
        return items
    for p in sorted(tree.rglob("*")):
        if p.is_file() or p.is_symlink():
            rel = "/" + p.relative_to(tree).as_posix()
            items.append(f'"{rel}"' if " " in rel else rel)
    return items


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def stage_and_build_rpm(args: argparse.Namespace) -> int:
    cde_repo = Path(args.cde_repo).resolve()
    build_root = Path(args.guest_build_root).resolve()
    out_dir = Path(args.output_dir).resolve()
    verify_mac_temp_root(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    spec_src = cde_repo / "packaging" / "rpm" / "ck-core-cde.spec"
    if not spec_src.is_file():
        raise SystemExit(f"error: missing spec file {spec_src}")

    motif_pfx = build_root / "motif-prefix"
    fs_pfx = build_root / "fs-prefix"
    cde_stage = build_root / "cde-stage"
    apps_stage = build_root / "apps-stage"

    topdir = out_dir / "rpmbuild"
    if topdir.exists():
        shutil.rmtree(topdir)
    for sub in ("BUILD", "BUILDROOT", "RPMS", "SOURCES", "SPECS", "SRPMS"):
        (topdir / sub).mkdir(parents=True)

    stage_root = topdir / "STAGE"
    stage_root.mkdir(parents=True)

    # Split trees first so exact %files manifests never overlap across subpackages
    t_motif = topdir / "split-motif"
    t_fs = topdir / "split-fs"
    t_cde = topdir / "split-cde"
    t_apps = topdir / "split-apps"
    for t in (t_motif, t_fs, t_cde, t_apps):
        t.mkdir(parents=True)

    if motif_pfx.is_dir():
        (t_motif / "usr" / "dt").mkdir(parents=True)
        shutil.copytree(motif_pfx, t_motif / "usr" / "dt", symlinks=True, dirs_exist_ok=True)
        for la in t_motif.rglob("*.la"):
            la.unlink()

    if fs_pfx.is_dir():
        (t_fs / "usr" / "dt").mkdir(parents=True)
        shutil.copytree(fs_pfx, t_fs / "usr" / "dt", symlinks=True, dirs_exist_ok=True)
        for la in t_fs.rglob("*.la"):
            la.unlink()

    if cde_stage.is_dir():
        shutil.copytree(cde_stage, t_cde, symlinks=True, dirs_exist_ok=True)
        for la in t_cde.rglob("*.la"):
            la.unlink()

    if apps_stage.is_dir():
        shutil.copytree(apps_stage, t_apps, symlinks=True, dirs_exist_ok=True)

    # Remove files explicitly listed in %files or owned by ck-core-dtlogin from t_cde list
    explicit_cde = {
        "/etc/pam.d/dtlogin",
        "/etc/pam.d/dtlogin-greeter",
        "/etc/pam.d/dtsession",
        "/etc/profile.d/ck-core-cde.sh",
        "/usr/dt/bin/dtlogin",
        "/usr/dt/bin/dtgreet",
        "/usr/dt/bin/dtchooser",
        "/usr/share/xsessions/cde.desktop",
        "/usr/share/wayland-sessions/cde-wayland.desktop",
        "/usr/share/xdg-desktop-portal/cde-portals.conf",
        "/usr/share/xdg-desktop-portal/portals/cde.portal",
        "/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.cde.service",
    }

    deb_meta = cde_repo / "cde" / "debian"
    (t_cde / "usr" / "dt" / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy2(deb_meta / "Xsession.wayland", t_cde / "usr" / "dt" / "bin" / "Xsession.wayland")
    os.chmod(t_cde / "usr" / "dt" / "bin" / "Xsession.wayland", 0o755)

    motif_files = collect_rel_paths(t_motif)
    motif_set = set(motif_files)
    fs_files = [p for p in collect_rel_paths(t_fs) if p not in motif_set]
    fs_set = motif_set | set(fs_files)
    cde_files = [
        p for p in collect_rel_paths(t_cde)
        if p not in fs_set and p.strip('"') not in explicit_cde
    ]
    apps_files = [
        p for p in collect_rel_paths(t_apps)
        if p not in fs_set and p not in set(cde_files)
    ]

    (topdir / "BUILD" / "files-motif9.list").write_text("\n".join(motif_files) + "\n", encoding="utf-8")
    (topdir / "BUILD" / "files-fs-lib.list").write_text("\n".join(fs_files) + "\n", encoding="utf-8")
    (topdir / "BUILD" / "files-cde.list").write_text("\n".join(cde_files) + "\n", encoding="utf-8")
    (topdir / "BUILD" / "files-apps.list").write_text("\n".join(apps_files) + "\n", encoding="utf-8")

    # Merge into unified STAGE root expected by %with prebuilt_stage
    for src_tree in (t_motif, t_fs, t_cde, t_apps):
        shutil.copytree(src_tree, stage_root, symlinks=True, dirs_exist_ok=True)

    # Ensure required config/session/unit files exist in STAGE
    (stage_root / "etc" / "ld.so.conf.d").mkdir(parents=True, exist_ok=True)
    (stage_root / "etc" / "ld.so.conf.d" / "ck-core-cde.conf").write_text("/usr/dt/lib\n", encoding="utf-8")
    (stage_root / "etc" / "profile.d").mkdir(parents=True, exist_ok=True)
    (stage_root / "etc" / "profile.d" / "ck-core-cde.sh").write_text(
        'case ":${PATH}:" in *:/usr/dt/bin:*) ;; *) export PATH="${PATH}:/usr/dt/bin" ;; esac\n',
        encoding="utf-8",
    )
    (stage_root / "usr" / "share" / "xsessions").mkdir(parents=True, exist_ok=True)
    (stage_root / "usr" / "share" / "wayland-sessions").mkdir(parents=True, exist_ok=True)
    (stage_root / "usr" / "share" / "xdg-desktop-portal" / "portals").mkdir(parents=True, exist_ok=True)
    (stage_root / "usr" / "share" / "dbus-1" / "services").mkdir(parents=True, exist_ok=True)
    (stage_root / "usr" / "lib" / "systemd" / "system").mkdir(parents=True, exist_ok=True)
    (stage_root / "usr" / "lib" / "systemd" / "user").mkdir(parents=True, exist_ok=True)

    shutil.copy2(deb_meta / "cde.desktop", stage_root / "usr" / "share" / "xsessions" / "cde.desktop")
    shutil.copy2(deb_meta / "cde-wayland.desktop", stage_root / "usr" / "share" / "wayland-sessions" / "cde-wayland.desktop")
    shutil.copy2(deb_meta / "cde-portals.conf", stage_root / "usr" / "share" / "xdg-desktop-portal" / "cde-portals.conf")
    shutil.copy2(
        cde_repo / "cde" / "programs" / "dtportal" / "cde.portal",
        stage_root / "usr" / "share" / "xdg-desktop-portal" / "portals" / "cde.portal",
    )
    (stage_root / "usr" / "share" / "dbus-1" / "services" / "org.freedesktop.impl.portal.desktop.cde.service").write_text(
        "[D-BUS Service]\n"
        "Name=org.freedesktop.impl.portal.desktop.cde\n"
        "Exec=/usr/dt/bin/xdg-desktop-portal-cde\n"
        "SystemdService=xdg-desktop-portal-cde.service\n",
        encoding="utf-8",
    )
    (stage_root / "usr" / "lib" / "systemd" / "user" / "xdg-desktop-portal-cde.service").write_text(
        "[Unit]\n"
        "Description=CK Core CDE XDG Desktop Portal Backend (Native Motif)\n"
        "PartOf=graphical-session.target\n\n"
        "[Service]\n"
        "Type=dbus\n"
        "BusName=org.freedesktop.impl.portal.desktop.cde\n"
        "ExecStart=/usr/dt/bin/xdg-desktop-portal-cde\n"
        "Restart=on-failure\n",
        encoding="utf-8",
    )
    shutil.copy2(deb_meta / "rpc-cmsd.service", stage_root / "usr" / "lib" / "systemd" / "system" / "rpc-cmsd.service")
    dtlogin_unit = (deb_meta / "dtlogin.service").read_text(encoding="utf-8")
    dtlogin_unit = "\n".join(
        ln for ln in dtlogin_unit.splitlines() if not ln.startswith("ExecStartPre=")
    ) + "\n"
    (stage_root / "usr" / "lib" / "systemd" / "system" / "dtlogin.service").write_text(
        dtlogin_unit, encoding="utf-8"
    )

    elf_count = rewrite_rpath(stage_root)
    staged_spec = topdir / "SPECS" / "ck-core-cde.spec"
    shutil.copy2(spec_src, staged_spec)

    built_rpms: list[dict[str, str]] = []
    rpmbuild = shutil.which("rpmbuild")
    if not args.stage_only and rpmbuild:
        cmd = [
            rpmbuild,
            "-bb",
            "--with",
            "prebuilt_stage",
            "--define",
            f"_topdir {topdir}",
            "--define",
            f"prebuilt_stage_dir {stage_root}",
            "--define",
            "_unitdir /usr/lib/systemd/system",
            "--define",
            "_userunitdir /usr/lib/systemd/user",
        ]
        if args.distro_flavor == "opensuse":
            cmd.extend(["--define", "suse_version 1699"])
        elif args.distro_flavor == "rhel9":
            cmd.extend(["--define", "rhel 9", "--define", "dist .el9"])
        else:
            cmd.extend(["--define", "fedora 41", "--define", "dist .fc41"])
        cmd.append(str(staged_spec))
        subprocess.run(cmd, check=True)
        for rpm_file in sorted((topdir / "RPMS").rglob("*.rpm")):
            dst = out_dir / rpm_file.name
            shutil.copy2(rpm_file, dst)
            built_rpms.append({"name": dst.name, "sha256": sha256_file(dst)})
        createrepo = shutil.which("createrepo_c") or shutil.which("createrepo")
        if createrepo and built_rpms:
            subprocess.run([createrepo, str(out_dir)], check=True)
    elif not args.stage_only and not rpmbuild:
        raise SystemExit("error: rpmbuild not found in PATH (pass --stage-only to validate staging without rpmbuild)")

    # Remove intermediate split trees after packaging/validation
    for t in (t_motif, t_fs, t_cde, t_apps):
        shutil.rmtree(t, ignore_errors=True)

    receipt = {
        "timestamp_utc": utc_now(),
        "distro_flavor": args.distro_flavor,
        "stage_only": bool(args.stage_only),
        "elf_rpath_rewritten": elf_count,
        "manifest_counts": {
            "ck-core-motif9": len(motif_files),
            "ck-core-fs-lib": len(fs_files),
            "ck-core-cde": len(cde_files),
            "ck-core-apps": len(apps_files),
        },
        "built_rpms": built_rpms,
    }
    (out_dir / "rpm-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage and build Fedora/RHEL/openSUSE RPM packages for CK Core CDE")
    ap.add_argument("--cde-repo", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("--guest-build-root", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--distro-flavor", choices=("fedora", "rhel9", "opensuse"), default="fedora")
    ap.add_argument("--stage-only", action="store_true", help="Stage RPM tree and manifests without invoking rpmbuild")
    args = ap.parse_args()
    return stage_and_build_rpm(args)


if __name__ == "__main__":
    raise SystemExit(main())
