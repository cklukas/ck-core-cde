#!/usr/bin/env python3
"""Stage FreeBSD pkg(8) +MANIFEST metadata and root trees for CK Core Motif, fs-lib, CDE, and Apps.

Produces pkg(8)-compatible staging directories (+MANIFEST, plist, rc.d scripts,
OpenPAM stacks, and /usr/local/share/{wayland-sessions,xsessions}) and invokes
`pkg create` when running on FreeBSD (or validates staging with `--stage-only`).
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


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


def freebsd_abi() -> str:
    mach = platform.machine().lower()
    arch_map = {"x86_64": "amd64", "amd64": "amd64", "aarch64": "aarch64", "arm64": "aarch64"}
    farch = arch_map.get(mach, mach)
    return f"FreeBSD:14:{farch}"


def write_pkg_manifest(meta_dir: Path, manifest: dict[str, object]) -> None:
    meta_dir.mkdir(parents=True, exist_ok=True)
    (meta_dir / "+MANIFEST").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def build_freebsd_pkgs(args: argparse.Namespace) -> int:
    cde_repo = Path(args.cde_repo).resolve()
    out_dir = Path(args.output_dir).resolve()
    verify_mac_temp_root(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    fbsd_root = cde_repo / "packaging" / "freebsd"
    deb_meta = cde_repo / "cde" / "debian"
    abi = args.abi or freebsd_abi()

    pkgs_spec = [
        {
            "name": "ck-core-motif9",
            "version": "2.4.0",
            "origin": "x11-toolkits/ck-core-motif9",
            "prefix": "/usr/dt",
            "comment": "CK Core Motif ABI 9 toolkit (X11 and Wayland backends) under /usr/dt",
            "deps": {},
        },
        {
            "name": "ck-core-fs-lib",
            "version": "0.3.0",
            "origin": "sysutils/ck-core-fs-lib",
            "prefix": "/usr/dt",
            "comment": "CK Core headless filesystem synchronization, trash, and search engine",
            "deps": {},
        },
        {
            "name": "ck-core-cde",
            "version": "3.0.0",
            "origin": "x11/ck-core-cde",
            "prefix": "/usr/dt",
            "comment": "CK Core Common Desktop Environment (Wayland seatd/drm-kmod and X11)",
            "deps": {
                "ck-core-motif9": {"origin": "x11-toolkits/ck-core-motif9", "version": "2.4.0"},
                "ck-core-fs-lib": {"origin": "sysutils/ck-core-fs-lib", "version": "0.3.0"},
            },
            "users": ["_dtgreeter"],
            "groups": ["_dtgreeter"],
        },
        {
            "name": "ck-core-apps",
            "version": "1.0.0",
            "origin": "x11/ck-core-apps",
            "prefix": "/usr/local",
            "comment": "CK Core Motif ABI 9 desktop applications for CDE (X11 and Wayland)",
            "deps": {
                "ck-core-motif9": {"origin": "x11-toolkits/ck-core-motif9", "version": "2.4.0"},
                "ck-core-cde": {"origin": "x11/ck-core-cde", "version": "3.0.0"},
            },
        },
    ]

    stage_base = out_dir / "freebsd-stage"
    if stage_base.exists():
        shutil.rmtree(stage_base)
    stage_base.mkdir(parents=True)

    built_archives: list[str] = []
    pkg_bin = shutil.which("pkg") if sys.platform.startswith("freebsd") else None

    for spec in pkgs_spec:
        port_dir = fbsd_root / spec["origin"]
        if not (port_dir / "Makefile").is_file():
            raise SystemExit(f"error: missing FreeBSD port Makefile in {port_dir}")
        descr = (port_dir / "pkg-descr").read_text(encoding="utf-8").strip()
        plist_txt = (port_dir / "pkg-plist").read_text(encoding="utf-8")

        pkg_stage = stage_base / spec["name"]
        root_dir = pkg_stage / "root"
        meta_dir = pkg_stage / "meta"
        root_dir.mkdir(parents=True)
        meta_dir.mkdir(parents=True)
        (meta_dir / "plist").write_text(plist_txt, encoding="utf-8")

        if spec["name"] == "ck-core-cde":
            files_dir = port_dir / "files"
            rc_dir = root_dir / "usr" / "local" / "etc" / "rc.d"
            pam_dir = root_dir / "usr" / "local" / "etc" / "pam.d"
            wl_sess = root_dir / "usr" / "local" / "share" / "wayland-sessions"
            x_sess = root_dir / "usr" / "local" / "share" / "xsessions"
            portal_d = root_dir / "usr" / "local" / "share" / "xdg-desktop-portal" / "portals"
            dt_bin = root_dir / "usr" / "dt" / "bin"
            for d in (rc_dir, pam_dir, wl_sess, x_sess, portal_d, dt_bin):
                d.mkdir(parents=True, exist_ok=True)
            for rc_name in ("dtlogin", "rpc_cmsd"):
                dst = rc_dir / rc_name
                shutil.copy2(files_dir / f"{rc_name}.in", dst)
                os.chmod(dst, 0o755)
            for pam_name in ("dtlogin", "dtlogin-greeter", "dtsession"):
                shutil.copy2(files_dir / f"{pam_name}.pam.in", pam_dir / pam_name)
            shutil.copy2(deb_meta / "Xsession.wayland", dt_bin / "Xsession.wayland")
            os.chmod(dt_bin / "Xsession.wayland", 0o755)
            shutil.copy2(deb_meta / "cde-wayland.desktop", wl_sess / "cde-wayland.desktop")
            shutil.copy2(deb_meta / "cde.desktop", x_sess / "cde.desktop")
            shutil.copy2(deb_meta / "cde-portals.conf", portal_d.parent / "cde-portals.conf")
            shutil.copy2(cde_repo / "cde" / "programs" / "dtportal" / "cde.portal", portal_d / "cde.portal")

        manifest: dict[str, object] = {
            "name": spec["name"],
            "version": spec["version"],
            "origin": spec["origin"],
            "comment": spec["comment"],
            "arch": abi,
            "abi": abi,
            "www": "https://github.com/cklukas/ck-core-cde",
            "maintainer": "cklukas@users.noreply.github.com",
            "prefix": spec["prefix"],
            "desc": descr,
            "deps": spec["deps"],
        }
        if "users" in spec:
            manifest["users"] = spec["users"]
        if "groups" in spec:
            manifest["groups"] = spec["groups"]
        write_pkg_manifest(meta_dir, manifest)

        if not args.stage_only and pkg_bin:
            subprocess.run(
                [
                    pkg_bin,
                    "create",
                    "-M",
                    str(meta_dir / "+MANIFEST"),
                    "-r",
                    str(root_dir),
                    "-o",
                    str(out_dir),
                ],
                check=True,
            )
            built_archives.append(f"{spec['name']}-{spec['version']}.pkg")

    receipt = {
        "timestamp_utc": utc_now(),
        "abi": abi,
        "stage_only": bool(args.stage_only),
        "staged_ports": [s["origin"] for s in pkgs_spec],
        "built_pkgs": built_archives,
    }
    (out_dir / "freebsd-pkg-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage and build FreeBSD pkg archives for CK Core CDE")
    ap.add_argument("--cde-repo", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--abi", default="")
    ap.add_argument("--stage-only", action="store_true")
    args = ap.parse_args()
    return build_freebsd_pkgs(args)


if __name__ == "__main__":
    raise SystemExit(main())
