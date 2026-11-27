#!/usr/bin/env python3
"""Verify cross-distro RPM packaging, FreeBSD ports/pkg recipes, and seatd/evdev/LOCAL_PEERCRED portability."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


def extract_c_func(src: str, marker: str) -> str:
    idx = src.find(marker)
    if idx < 0:
        raise RuntimeError(f"marker not found: {marker}")
    brace = src.find("{", idx)
    if brace < 0:
        raise RuntimeError(f"opening brace not found for: {marker}")
    depth = 0
    for pos in range(brace, len(src)):
        if src[pos] == "{":
            depth += 1
        elif src[pos] == "}":
            depth -= 1
            if depth == 0:
                return src[idx : pos + 1]
    raise RuntimeError(f"unterminated function for: {marker}")


def test_seat_evdev_and_peercred(tmp: Path) -> None:
    wl_seat_src = (REPO_ROOT / "cde" / "programs" / "dtlogin" / "wayland_seat.c").read_text(encoding="utf-8")
    svcmain_src = (REPO_ROOT / "cde" / "programs" / "dtcm" / "server" / "svcmain.c").read_text(encoding="utf-8")

    fn_parse_vtnr = extract_c_func(wl_seat_src, "static int parse_vtnr(")
    fn_detect_seatd = extract_c_func(wl_seat_src, "static const char *detect_seatd_socket(")
    fn_peercred = extract_c_func(svcmain_src, "static int\nverify_unix_peercred(")

    test_c = tmp / "check_portability.c"
    test_bin = tmp / "check_portability"
    evdev_hdr = REPO_ROOT / "cde" / "programs" / "dtwm" / "wayland"

    test_c.write_text(
        f"""
#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <sys/ucred.h>
#endif
#include "cde_evdev_compat.h"

/* Minimal SunRPC stand-ins to exercise production verify_unix_peercred() */
#define NULLPROC 0UL
#define AUTH_UNIX 1
struct opaque_auth {{ int oa_flavor; }};
struct authunix_parms {{ unsigned int aup_uid; }};
struct svc_req {{
    unsigned long rq_proc;
    struct opaque_auth rq_cred;
    void *rq_clntcred;
}};
typedef struct {{ int xp_fd; }} SVCXPRT;
static int debug = 0;

{fn_parse_vtnr}

{fn_detect_seatd}

{fn_peercred}

int main(int argc, char **argv)
{{
    (void)argc;
    const char *work_dir = argv[1];

    /* 1. Portable evdev button constants */
    assert(BTN_LEFT == 0x110);
    assert(BTN_RIGHT == 0x111);
    assert(BTN_MIDDLE == 0x112);

    /* 2. Linux tty/vt AND FreeBSD vt(4) ttyv0..ttyvf VT parsing */
    assert(parse_vtnr("/dev/tty1") == 1);
    assert(parse_vtnr("tty7") == 7);
    assert(parse_vtnr("vt2") == 2);
    assert(parse_vtnr("/dev/ttyv0") == 1);
    assert(parse_vtnr("/dev/ttyv7") == 8);
    assert(parse_vtnr("ttyva") == 11);
    assert(parse_vtnr("ttyvf") == 16);
    assert(parse_vtnr("??") == 0);
    assert(parse_vtnr(nullptr) == 0);

    /* 3. seatd Unix socket detection vs systemd-logind coexistence */
    char sock_path[PATH_MAX];
    snprintf(sock_path, sizeof(sock_path), "%s/seatd.sock", work_dir);
    unlink(sock_path);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(sfd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    assert(bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(sfd, 4) == 0);

    char sd_present[PATH_MAX], sd_absent[PATH_MAX];
    snprintf(sd_present, sizeof(sd_present), "%s/systemd-seats", work_dir);
    snprintf(sd_absent, sizeof(sd_absent), "%s/no-systemd-seats", work_dir);
    mkdir(sd_present, 0755);

    char out_sock[PATH_MAX];
    setenv("SEATD_SOCK", sock_path, 1);
    unsetenv("LIBSEAT_BACKEND");

    /* When systemd-logind seat dir exists and LIBSEAT_BACKEND is unset, prefer logind */
    setenv("DTLOGIN_SYSTEMD_SEATS_DIR", sd_present, 1);
    assert(detect_seatd_socket(out_sock, sizeof(out_sock)) == nullptr);

    /* On FreeBSD / non-systemd seatd hosts, auto-detect SEATD_SOCK */
    setenv("DTLOGIN_SYSTEMD_SEATS_DIR", sd_absent, 1);
    const char *det = detect_seatd_socket(out_sock, sizeof(out_sock));
    assert(det != nullptr && strcmp(det, sock_path) == 0);

    /* Explicit LIBSEAT_BACKEND=seatd overrides even when systemd dir exists */
    setenv("DTLOGIN_SYSTEMD_SEATS_DIR", sd_present, 1);
    setenv("LIBSEAT_BACKEND", "seatd", 1);
    det = detect_seatd_socket(out_sock, sizeof(out_sock));
    assert(det != nullptr && strcmp(det, sock_path) == 0);
    close(sfd);
    unlink(sock_path);

    /* 4. Live AF_UNIX peer-credential check (SO_PEERCRED on Linux, LOCAL_PEERCRED/getpeereid on BSD/macOS) */
    int pair[2] = {{-1, -1}};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    SVCXPRT xprt = {{ .xp_fd = pair[0] }};
    struct authunix_parms good_aup = {{ .aup_uid = (unsigned int)getuid() }};
    struct authunix_parms spoof_aup = {{ .aup_uid = (getuid() == 0) ? 65534u : 0u }};

    struct svc_req ping_req = {{ .rq_proc = NULLPROC, .rq_cred = {{ 0 }}, .rq_clntcred = nullptr }};
    assert(verify_unix_peercred(&ping_req, &xprt) == 1);

    struct svc_req ok_req = {{ .rq_proc = 14UL, .rq_cred = {{ AUTH_UNIX }}, .rq_clntcred = &good_aup }};
    assert(verify_unix_peercred(&ok_req, &xprt) == 1);

    struct svc_req bad_req = {{ .rq_proc = 14UL, .rq_cred = {{ AUTH_UNIX }}, .rq_clntcred = &spoof_aup }};
    assert(verify_unix_peercred(&bad_req, &xprt) == -1);

    close(pair[0]);
    close(pair[1]);
    puts("PASS: seatd/ttyv/evdev/peercred portability checks");
    return 0;
}}
""",
        encoding="utf-8",
    )

    cc = os.environ.get("CC", "clang" if shutil.which("clang") else "cc")
    cmd = [
        cc,
        "-std=gnu23",
        "-g",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=undefined",
        f"-I{evdev_hdr}",
        str(test_c),
        "-o",
        str(test_bin),
    ]
    subprocess.run(cmd, check=True)
    subprocess.run([str(test_bin), str(tmp)], check=True)


def test_rpm_staging_and_spec(tmp: Path) -> None:
    spec_path = REPO_ROOT / "packaging" / "rpm" / "ck-core-cde.spec"
    spec_txt = spec_path.read_text(encoding="utf-8")
    for required in (
        "%global __provides_exclude_from ^/usr/dt/lib/.*\\\\.so.*$",
        "%package -n ck-core-motif9",
        "%package -n ck-core-fs-lib",
        "%package -n ck-core-dtlogin",
        "%package -n ck-core-apps",
        "%if 0%{?suse_version}",
        "password-auth",
        "common-auth",
        "%systemd_post rpc-cmsd.service",
        "%systemd_post dtlogin.service",
    ):
        assert required in spec_txt, f"missing RPM spec directive: {required}"

    # Create synthetic prefix layout if real guest build root is not supplied
    fake_build = tmp / "fake-build-root"
    (fake_build / "motif-prefix" / "lib").mkdir(parents=True)
    (fake_build / "motif-prefix" / "lib" / "libXm.so.9").write_bytes(b"fake-xm9")
    (fake_build / "fs-prefix" / "lib").mkdir(parents=True)
    (fake_build / "fs-prefix" / "lib" / "libck_file_engine.so").write_bytes(b"fake-fs")
    for b in ("dtwm", "dtwm-wayland", "dtwm-panel", "dtsession", "dtlogin", "dtgreet", "dtchooser", "rpc.cmsd", "xdg-desktop-portal-cde"):
        p = fake_build / "cde-stage" / "usr" / "dt" / "bin" / b
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        os.chmod(p, 0o755)
    (fake_build / "apps-stage" / "usr" / "bin").mkdir(parents=True)
    (fake_build / "apps-stage" / "usr" / "bin" / "ck-calc").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")

    for flavor in ("fedora", "rhel9", "opensuse"):
        out = tmp / f"rpm-{flavor}"
        subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "scripts" / "build-rpm.py"),
                "--cde-repo",
                str(REPO_ROOT),
                "--guest-build-root",
                str(fake_build),
                "--output-dir",
                str(out),
                "--distro-flavor",
                flavor,
                "--stage-only",
            ],
            check=True,
        )
        receipt = json.loads((out / "rpm-receipt.json").read_text(encoding="utf-8"))
        assert receipt["distro_flavor"] == flavor
        assert receipt["manifest_counts"]["ck-core-motif9"] >= 1
        assert receipt["manifest_counts"]["ck-core-fs-lib"] >= 1
        assert receipt["manifest_counts"]["ck-core-cde"] >= 1
        assert receipt["manifest_counts"]["ck-core-apps"] >= 1

        # Verify zero duplicate file paths across the 4 generated %files manifests
        seen: set[str] = set()
        for mname in ("files-motif9.list", "files-fs-lib.list", "files-cde.list", "files-apps.list"):
            entries = [
                ln.strip()
                for ln in (out / "rpmbuild" / "BUILD" / mname).read_text(encoding="utf-8").splitlines()
                if ln.strip()
            ]
            overlap = seen.intersection(entries)
            assert not overlap, f"overlapping RPM %files entries in {mname}: {overlap}"
            seen.update(entries)
            # dtlogin binaries must be owned exclusively by ck-core-dtlogin
            if mname == "files-cde.list":
                assert "/usr/dt/bin/dtlogin" not in entries
                assert "/usr/dt/bin/dtgreet" not in entries
    print("PASS: RPM spec & fedora/rhel9/opensuse staging checks")


def test_freebsd_ports_and_pkg_staging(tmp: Path) -> None:
    out = tmp / "fbsd-out"
    subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "scripts" / "build-freebsd-pkg.py"),
            "--cde-repo",
            str(REPO_ROOT),
            "--output-dir",
            str(out),
            "--stage-only",
        ],
        check=True,
    )
    receipt = json.loads((out / "freebsd-pkg-receipt.json").read_text(encoding="utf-8"))
    assert receipt["staged_ports"] == [
        "x11-toolkits/ck-core-motif9",
        "sysutils/ck-core-fs-lib",
        "x11/ck-core-cde",
        "x11/ck-core-apps",
    ]
    cde_stage = out / "freebsd-stage" / "ck-core-cde" / "root"
    for must in (
        cde_stage / "usr" / "local" / "etc" / "rc.d" / "dtlogin",
        cde_stage / "usr" / "local" / "etc" / "rc.d" / "rpc_cmsd",
        cde_stage / "usr" / "local" / "etc" / "pam.d" / "dtlogin",
        cde_stage / "usr" / "local" / "etc" / "pam.d" / "dtlogin-greeter",
        cde_stage / "usr" / "local" / "share" / "wayland-sessions" / "cde-wayland.desktop",
        cde_stage / "usr" / "local" / "share" / "xsessions" / "cde.desktop",
        cde_stage / "usr" / "dt" / "bin" / "Xsession.wayland",
    ):
        assert must.is_file(), f"missing staged FreeBSD path: {must}"

    pam_txt = (cde_stage / "usr" / "local" / "etc" / "pam.d" / "dtlogin").read_text(encoding="utf-8")
    assert "pam_xdg.so" in pam_txt

    # Validate POSIX sh -n syntax of FreeBSD rc.d scripts and Xsession.wayland
    for script in (
        cde_stage / "usr" / "local" / "etc" / "rc.d" / "dtlogin",
        cde_stage / "usr" / "local" / "etc" / "rc.d" / "rpc_cmsd",
        cde_stage / "usr" / "dt" / "bin" / "Xsession.wayland",
    ):
        subprocess.run(["/bin/sh", "-n", str(script)], check=True)

    # Validate key FreeBSD Porter's Handbook fields across all 4 port Makefiles
    for origin in receipt["staged_ports"]:
        mk = (REPO_ROOT / "packaging" / "freebsd" / origin / "Makefile").read_text(encoding="utf-8")
        assert re.search(r"^PORTNAME=\s+\S+", mk, re.M), f"missing PORTNAME in {origin}"
        assert re.search(r"^DISTVERSION=\s+\S+", mk, re.M), f"missing DISTVERSION in {origin}"
        assert ".include <bsd.port.mk>" in mk, f"missing bsd.port.mk in {origin}"
    print("PASS: FreeBSD ports, OpenPAM, rc.d syntax & pkg staging checks")


def main() -> int:
    base_tmp = os.environ.get("TMPDIR")
    if sys.platform == "darwin" and (not base_tmp or base_tmp.startswith(("/tmp", "/private/tmp"))):
        base_tmp = "/Volumes/S8T/tmp" if Path("/Volumes/S8T/tmp").is_dir() else "/Volumes/PRO-BLADE/tmp"
    with tempfile.TemporaryDirectory(prefix="cde-cross-pkg-", dir=base_tmp) as td:
        tmp = Path(td)
        test_seat_evdev_and_peercred(tmp)
        test_rpm_staging_and_spec(tmp)
        test_freebsd_ports_and_pkg_staging(tmp)
    print("ALL CROSS-DISTRO & FREEBSD PACKAGING CHECKS PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
