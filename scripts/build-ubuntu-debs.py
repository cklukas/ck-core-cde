#!/usr/bin/env python3
"""Build matched Ubuntu/Debian .deb packages and APT repository for CK Core Motif, fs-lib, CDE, dtlogin, and Apps.

Produces:
  - ck-core-motif9_2.4.0-1_<arch>.deb
  - ck-core-fs-lib_0.3.0-1_<arch>.deb
  - ck-core-cde_3.0.0-1_<arch>.deb
  - ck-core-dtlogin_3.0.0-1_<arch>.deb
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
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DTLOGIN_BINS = {"dtlogin", "dtgreet", "dtchooser"}
CDE_PACKAGES = ("ck-core-dtlogin", "ck-core-apps", "ck-core-cde", "ck-core-fs-lib", "ck-core-motif9")


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
    out_dir = Path(args.output_dir).resolve() if args.output_dir else (task_root / "receipts" / "wp08-apt-repo")
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
    if args.skip_harness_sync:
        cmd.append("--skip-harness-sync")
    if args.verify_repo:
        repo = Path(args.verify_repo).resolve()
        verify_mac_temp_root(repo)
        cmd.extend(["--verify-repo", str(repo)])
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


def require_clean_package_guest() -> None:
    status = subprocess.run(
        ["dpkg-query", "-W", "-f=${Package}\t${db:Status-Status}\n", *CDE_PACKAGES],
        capture_output=True, text=True, check=False,
    )
    present = [line.split("\t", 1)[0] for line in status.stdout.splitlines()
               if not line.endswith("\tnot-installed")]
    if present:
        raise SystemExit("APT install verification needs a clean guest. Existing packages: " + ", ".join(present))


def stop_fixture_tooltalk(home: Path) -> None:
    marker = os.fsencode(f"HOME={home}")
    for proc in Path("/proc").iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if (proc / "comm").read_bytes().strip() != b"ttsession":
                continue
            if marker in (proc / "environ").read_bytes().split(b"\0"):
                os.kill(int(proc.name), signal.SIGTERM)
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue


def stop_process_groups(procs: list[subprocess.Popen]) -> None:
    for proc in reversed(procs):
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + 8
    for proc in reversed(procs):
        try:
            proc.wait(timeout=max(0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()


def wait_for_xvfb_display(read_end: int, log_path: Path) -> str:
    deadline = time.monotonic() + 30
    reply = bytearray()
    while b"\n" not in reply and len(reply) < 16:
        remaining = max(0, deadline - time.monotonic())
        if not select.select([read_end], [], [], remaining)[0]:
            break
        chunk = os.read(read_end, 16 - len(reply))
        if not chunk:
            break
        reply.extend(chunk)
    number, newline, _ = reply.partition(b"\n")
    if newline and number.isdigit():
        return ":" + number.decode("ascii")
    raise SystemExit(f"Xvfb did not report a ready display; see {log_path}")


def wait_for_wayland_windows(ipc: Path, clients: dict[str, subprocess.Popen]) -> list[str]:
    wanted = set(clients)
    visible: set[str] = set()
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        for name, proc in clients.items():
            if proc.poll() is not None:
                raise SystemExit(f"Installed Wayland {name} exited with {proc.returncode} before mapping")
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.settimeout(1)
                sock.connect(str(ipc))
                with sock.makefile("rb") as stream:
                    for line in stream:
                        fields = line.split()
                        if len(fields) == 3 and fields[:2] == [b"PROP", b"_DT_WAYLAND_CLIENT_IDS"]:
                            ids = bytes.fromhex(fields[2].decode("ascii")).decode("utf-8").split("\0")
                            visible = {item.casefold().rsplit(".", 1)[-1] for item in ids if item}
                        if line == b"READY\n":
                            break
        except OSError:
            visible.clear()
        if wanted <= visible:
            return sorted(visible)
        time.sleep(0.15)
    raise SystemExit(f"Installed Wayland windows missing before capture: {sorted(wanted - visible)} (saw {sorted(visible)})")


def verify_signed_repo(repo: Path, out_dir: Path, tmp_dir: Path) -> dict[str, object]:
    require_clean_package_guest()
    motif_pin = (repo / "motif-revision.txt").read_text(encoding="utf-8").strip()
    libxm_sha = (repo / "motif.sha256").read_text(encoding="utf-8").split()[0]
    tmp_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="apt-verify-", dir=tmp_dir) as scratch:
        work = Path(scratch)
        work.chmod(0o755)
        try:
            return verify_package_sessions(repo, out_dir, work, motif_pin, libxm_sha)
        finally:
            runtime = work / "runtime"
            if runtime.exists():
                stop_fixture_tooltalk(runtime / "home")
                # Installed privileged helpers can leave root-owned runtime sockets.
                try:
                    shutil.rmtree(runtime)
                except PermissionError:
                    subprocess.run(["sudo", "rm", "-rf", "--", str(runtime)], check=True)


def verify_package_sessions(
    repo: Path, out_dir: Path, work: Path, motif_pin: str, libxm_sha: str,
) -> dict[str, object]:
    out_dir.mkdir(parents=True, exist_ok=True)
    guest_repo = work / "repo"
    guest_repo.mkdir(mode=0o755)
    for item in repo.iterdir():
        if item.is_file():
            shutil.copy2(item, guest_repo / item.name)
            os.chmod(guest_repo / item.name, 0o644)

    list_file = work / "verify.list"
    keyring = guest_repo / "ck-core-archive-keyring.asc"
    if subprocess.run(["sudo", "-u", "_apt", "test", "-r", str(keyring)], check=False).returncode:
        raise SystemExit(f"the _apt user cannot read {keyring}; check the scratch directory permissions")
    list_file.write_text(f"deb [signed-by={keyring}] file:{guest_repo} ./\n", encoding="utf-8")
    install_attempted = False
    try:
        subprocess.run(
            ["sudo", "apt-get", "update", "-o", f"Dir::Etc::sourcelist={list_file}",
             "-o", "Dir::Etc::sourceparts=-", "-o", "APT::Get::List-Cleanup=0"],
            check=True,
        )
        install_attempted = True
        subprocess.run(
            [
                "sudo",
                "env",
                "DEBIAN_FRONTEND=noninteractive",
                "apt-get",
                "install",
                "-y",
                "-o", f"Dir::Etc::sourcelist={list_file}",
                *CDE_PACKAGES,
                "libxm4",
            ],
            check=True,
        )

        installed_motif = Path("/usr/dt/lib/libXm.so.9").resolve(strict=True)
        if sha256_file(installed_motif) != libxm_sha:
            raise SystemExit(f"signed install did not map the packaged Motif cohort: {installed_motif}")

        # Verify installed files, Wayland binaries, _dtgreeter user, and pam_systemd presence
        for must_exist in [
            Path("/usr/dt/lib/libXm.so.9"),
            Path("/usr/dt/bin/dtwm"),
            Path("/usr/dt/bin/dtwm-wayland"),
            Path("/usr/dt/bin/dtwm-panel"),
            Path("/usr/dt/bin/dtsession"),
            Path("/usr/dt/bin/rpc.cmsd"),
            Path("/usr/dt/bin/dtlogin"),
            Path("/usr/dt/bin/dtgreet"),
            Path("/usr/dt/bin/Xsession.wayland"),
            Path("/usr/share/xsessions/cde.desktop"),
            Path("/usr/share/wayland-sessions/cde-wayland.desktop"),
            Path("/lib/systemd/system/rpc-cmsd.service"),
            Path("/lib/systemd/system/dtlogin.service"),
            Path("/etc/pam.d/dtlogin"),
            Path("/etc/pam.d/dtlogin-greeter"),
            Path("/etc/pam.d/dtsession"),
            Path("/usr/bin/ck-clock"),
            Path("/usr/bin/ck-grab"),
        ]:
            if not must_exist.exists():
                raise SystemExit(f"missing installed path: {must_exist}")

        if subprocess.run(["id", "-u", "_dtgreeter"], capture_output=True).returncode != 0:
            raise SystemExit("_dtgreeter system user was not created by ck-core-dtlogin.postinst")

        pam_txt = Path("/etc/pam.d/dtlogin").read_text(encoding="utf-8")
        if "pam_systemd.so" not in pam_txt:
            raise SystemExit("/etc/pam.d/dtlogin is missing pam_systemd.so")

        if Path("/run/systemd/system").is_dir():
            cmsd_state = subprocess.run(
                ["systemctl", "is-active", "rpc-cmsd.service"],
                capture_output=True,
                text=True,
            )
            if cmsd_state.returncode != 0:
                raise SystemExit(
                    f"rpc-cmsd.service is not active after ck-core-cde install: {cmsd_state.stdout.strip()} {cmsd_state.stderr.strip()}"
                )
            if not Path("/run/rpc.cmsd/cmsd.sock").is_socket():
                raise SystemExit("/run/rpc.cmsd/cmsd.sock is missing after ck-core-cde install")

        # 1) Boot installed CDE (Wayland) desktop session WITHOUT LD_LIBRARY_PATH, verify libXm.so.9 maps,
        #    and capture full desktop via installed /usr/bin/ck-grab --capture --full-screen!
        v_root = work / "runtime"
        xdg_d = v_root / "xdg"
        home_d = v_root / "home"
        dt_types = home_d / ".dt" / "types"
        xdg_d.mkdir(parents=True, mode=0o700)
        dt_types.mkdir(parents=True, mode=0o755)
        os.chmod(xdg_d, 0o700)
        sample_doc = v_root / "wayland-notes.txt"
        sample_doc.write_text(
            "CK Core CDE 3.0.0 + Motif ABI 9\n"
            "Installed from signed Ubuntu 26.04 LTS APT repository\n"
            "Coexisting with system libxm4 under /usr/dt\n",
            encoding="utf-8",
        )
        cur_user = os.environ.get("USER", "klukas")
        cde_common_env = {
            "PATH": "/usr/local/bin:/usr/bin:/bin:/usr/dt/bin",
            "HOME": str(home_d),
            "USER": cur_user,
            "LOGNAME": cur_user,
            "SHELL": "/bin/sh",
            "TERM": "xterm-256color",
            "LANG": "C.UTF-8",
            "DTDATABASESEARCHPATH": f"{home_d}/.dt/types,/usr/dt/appconfig/types/C",
            "XMICONSEARCHPATH": f"{home_d}/.dt/icons/%B%M.pm:/usr/dt/appconfig/icons/C/%B%M.pm:/usr/dt/appconfig/icons/C/%B%M.bm",
            "XMICONBMSEARCHPATH": "/usr/dt/appconfig/icons/C/%B%M.bm:/usr/dt/appconfig/icons/C/%B%M.pm",
            "XFILESEARCHPATH": "/usr/dt/app-defaults/%L/%N:/usr/dt/app-defaults/C/%N",
            "CDE_BACKDROP_DIR": "/usr/dt/backdrops",
        }
        clean_env = {
            **cde_common_env,
            "TMPDIR": str(work),
            "XDG_RUNTIME_DIR": str(xdg_d),
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
        }
        wl_status = v_root / "dtwm-wayland.status"
        wl_log_path = out_dir / "installed-wayland.log"
        with wl_log_path.open("w", encoding="utf-8") as wl_log:
            comp = subprocess.Popen(
                ["/usr/dt/bin/dtwm-wayland", "--no-xwayland", "--output-dpi", "HEADLESS-1=192",
                 "--status-file", str(wl_status)],
                env=clean_env,
                stdout=wl_log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        procs = [comp]
        mapped_wayland: dict[str, str] = {}
        mapped_x11: dict[str, str] = {}
        wl_shot = out_dir / "wp08-cde-wayland-desktop.png"
        x11_shot = out_dir / "wp08-cde-x11-desktop.png"
        try:
            wl_s, native = None, False
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if wl_status.is_file():
                    kv = dict(ln.split("=", 1) for ln in wl_status.read_text().splitlines() if "=" in ln)
                    wl_s = kv.get("WAYLAND_DISPLAY")
                    native = kv.get("NO_XWAYLAND") == "1" and "DISPLAY" not in kv
                    if wl_s and native:
                        break
                if comp.poll() is not None:
                    raise SystemExit(f"Installed Wayland compositor exited with {comp.returncode}; see {wl_log_path}")
                time.sleep(0.15)
            if not wl_s or not native:
                raise SystemExit(f"Installed Wayland compositor did not publish native readiness; see {wl_log_path}")

            # Wait briefly for genuine dtwm-panel to map its default CDE Front Panel controls
            time.sleep(1.5)
            c_env = {**clean_env, "WAYLAND_DISPLAY": wl_s, "CK_MOTIF_BACKEND": "wayland",
                     "DTWM_WAYLAND_NO_XWAYLAND": "1"}
            p_pad = subprocess.Popen(["/usr/dt/bin/dtpad", "-standAlone", "-xrm", "*rows: 9", "-xrm", "*columns: 46", "-geometry", "+36+32", str(sample_doc)], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            p_term = subprocess.Popen(["/usr/dt/bin/dtterm", "-geometry", "58x12+460+36"], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            p_calc = subprocess.Popen(["/usr/bin/ck-calc", "-geometry", "+970+36"], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            p_foot = subprocess.Popen(
                ["/usr/bin/foot", "--title=Wayland foot (native xdg_toplevel)", "-w", "460x200", "sh", "-c", "uname -srmo; echo WAYLAND_DISPLAY=$WAYLAND_DISPLAY; sleep 30"],
                env=c_env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            p_cm = subprocess.Popen(["/usr/dt/bin/dtcm"], env=c_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs.extend([p_pad, p_term, p_calc, p_cm, p_foot])
            clients = {"dtpad": p_pad, "dtterm": p_term, "ck-calc": p_calc, "dtcm": p_cm}
            wayland_windows = wait_for_wayland_windows(xdg_d / f"dtwm-wayland-{wl_s}.ipc", clients)

            for label, proc in clients.items():
                if proc.poll() is not None:
                    raise SystemExit(f"installed Wayland {label} exited with {proc.returncode}")
                maps = Path(f"/proc/{proc.pid}/maps").read_text(encoding="utf-8", errors="replace")
                xm_lines = [ln.split()[-1] for ln in maps.splitlines() if "libXm.so" in ln]
                if not xm_lines or not xm_lines[0].startswith("/usr/dt/lib/libXm.so.9"):
                    raise SystemExit(f"Wayland {label} mapped unexpected libXm: {xm_lines}")
                mapped_wayland[label] = xm_lines[0]

            subprocess.run(
                ["/usr/bin/ck-grab", "--full-screen", "--capture", str(wl_shot)],
                env=c_env,
                check=True,
            )
        finally:
            stop_process_groups(procs)
        if comp.returncode:
            raise SystemExit(f"installed Wayland compositor exited with {comp.returncode}; see {wl_log_path}")

        # 2) Boot installed CDE (X11 / XLibre) desktop session under Xvfb WITHOUT LD_LIBRARY_PATH,
        #    verify libXm.so.9 maps on dtwm + clients, and capture via /usr/bin/ck-grab --capture --full-screen!
        xvfb_command = shutil.which("Xvfb")
        if xvfb_command is None:
            raise SystemExit("Xvfb is required for installed X11 session verification")
        xvfb_path = Path(xvfb_command).absolute()
        xvfb_log_path = out_dir / "installed-xvfb.log"
        with xvfb_log_path.open("w", encoding="utf-8") as xvfb_log:
            xvfb = subprocess.Popen(
                [str(xvfb_path), "-displayfd", "1", "-screen", "0", "1280x900x24",
                 "-nolisten", "tcp", "-noreset"],
                stdout=subprocess.PIPE,
                stderr=xvfb_log,
                start_new_session=True,
            )
        x11_procs = [xvfb]
        try:
            with xvfb.stdout as displayfd:
                x11_dpy = wait_for_xvfb_display(displayfd.fileno(), xvfb_log_path)
            running_xvfb = Path(f"/proc/{xvfb.pid}/exe")
            x11_server = {
                "executable": str(running_xvfb.resolve(strict=True)),
                "sha256": sha256_file(running_xvfb),
            }
            x11_env = {
                **cde_common_env,
                "DISPLAY": x11_dpy,
            }
            subprocess.run(["xsetroot", "-solid", "#3c586e"], env=x11_env, check=True)
            x11_log_path = out_dir / "installed-x11.log"
            with x11_log_path.open("w", encoding="utf-8") as x11_log:
                p_dtwm = subprocess.Popen(
                    ["/usr/dt/bin/dtwm"], env=x11_env,
                    stdout=x11_log, stderr=subprocess.STDOUT, start_new_session=True,
                )
            x11_procs.append(p_dtwm)
            time.sleep(2.5)
            p_xclk = subprocess.Popen(["/usr/bin/ck-clock", "-geometry", "52x52"], env=x11_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            p_xpad = subprocess.Popen(["/usr/dt/bin/dtpad", "-standAlone", "-xrm", "*rows: 9", "-xrm", "*columns: 46", "-geometry", "+36+32", str(sample_doc)], env=x11_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            p_xterm = subprocess.Popen(["/usr/dt/bin/dtterm", "-geometry", "58x12+460+36"], env=x11_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            p_xcalc = subprocess.Popen(["/usr/bin/ck-calc", "-geometry", "+970+36"], env=x11_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            x11_procs.extend([p_xclk, p_xpad, p_xterm, p_xcalc])
            time.sleep(2.5)

            for label, proc in [("dtwm", p_dtwm), ("dtpad", p_xpad), ("dtterm", p_xterm), ("ck-calc", p_xcalc)]:
                if proc.poll() is not None:
                    raise SystemExit(f"installed X11 {label} exited with {proc.returncode}; see {x11_log_path}")
                maps = Path(f"/proc/{proc.pid}/maps").read_text(encoding="utf-8", errors="replace")
                xm_lines = [ln.split()[-1] for ln in maps.splitlines() if "libXm.so" in ln]
                if not xm_lines or not xm_lines[0].startswith("/usr/dt/lib/libXm.so.9"):
                    raise SystemExit(f"X11 {label} mapped unexpected libXm: {xm_lines}")
                mapped_x11[label] = xm_lines[0]

            subprocess.run(
                ["/usr/bin/ck-grab", "--full-screen", "--capture", str(x11_shot)],
                env=x11_env,
                check=True,
            )
        finally:
            stop_process_groups(x11_procs)

        prov_data = {
            "motif_revision": motif_pin,
            "libXm_sha256": libxm_sha,
            "wayland_maps": mapped_wayland,
            "x11_maps": mapped_x11,
            "x11_server": x11_server,
            "wayland_screenshot": wl_shot.name,
            "wayland_screenshot_sha256": sha256_file(wl_shot),
            "x11_screenshot": x11_shot.name,
            "x11_screenshot_sha256": sha256_file(x11_shot),
        }
        (out_dir / "proc-maps-provenance.json").write_text(json.dumps(prov_data, indent=2) + "\n", encoding="utf-8")

        verification = {
            "verified_install": True,
            "signed_apt_repo_verified": True,
            "coexists_with_ubuntu_libxm4": True,
            "wayland_session_maps": mapped_wayland,
            "wayland_windows": wayland_windows,
            "wayland_compositor_exit": comp.returncode,
            "x11_session_maps": mapped_x11,
            "x11_server": x11_server,
            "wayland_screenshot_sha256": prov_data["wayland_screenshot_sha256"],
            "x11_screenshot_sha256": prov_data["x11_screenshot_sha256"],
        }
    finally:
        if install_attempted:
            subprocess.run(
                [
                    "sudo",
                    "env",
                    "DEBIAN_FRONTEND=noninteractive",
                    "apt-get",
                    "purge",
                    "-y",
                    *CDE_PACKAGES,
                ],
                check=False,
            )
    return verification


def guest_build_and_verify(args: argparse.Namespace) -> int:
    if args.verify_install:
        require_clean_package_guest()
    t0 = time.monotonic()
    started_utc = utc_now()
    cde_repo = Path(args.cde_repo)
    build_root = Path(args.guest_build_root)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Ensure incremental build is completely fresh (including dtlogin.src update)
    if not args.skip_harness_sync:
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
    # Session .desktop entries, XDG Desktop Portal config, and Wayland Xsession entry point
    (r_cde / "usr" / "share" / "xsessions").mkdir(parents=True, exist_ok=True)
    (r_cde / "usr" / "share" / "wayland-sessions").mkdir(parents=True, exist_ok=True)
    (r_cde / "usr" / "share" / "xdg-desktop-portal").mkdir(parents=True, exist_ok=True)
    shutil.copy2(deb_meta / "cde.desktop", r_cde / "usr" / "share" / "xsessions" / "cde.desktop")
    shutil.copy2(deb_meta / "cde-wayland.desktop", r_cde / "usr" / "share" / "wayland-sessions" / "cde-wayland.desktop")
    if (deb_meta / "cde-portals.conf").is_file():
        shutil.copy2(deb_meta / "cde-portals.conf", r_cde / "usr" / "share" / "xdg-desktop-portal" / "cde-portals.conf")
    shutil.copy2(deb_meta / "Xsession.wayland", r_cde / "usr" / "dt" / "bin" / "Xsession.wayland")
    os.chmod(r_cde / "usr" / "dt" / "bin" / "Xsession.wayland", 0o755)
    (r_cde / "lib" / "systemd" / "system").mkdir(parents=True, exist_ok=True)
    shutil.copy2(deb_meta / "rpc-cmsd.service", r_cde / "lib" / "systemd" / "system" / "rpc-cmsd.service")

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
            "Version": "3.0.0-1",
            "Section": "x11",
            "Priority": "optional",
            "Architecture": arch,
            "Installed-Size": str(installed_size_kb(r_cde)),
            "Depends": ", ".join(dict.fromkeys(cde_deps)),
            "Recommends": "ck-core-apps, gnome-keyring, gnupg, gpgsm, libsecret-tools, "
            "xdg-desktop-portal, xdg-desktop-portal-wlr, pipewire, wlr-randr, libnotify-bin, xfonts-100dpi",
            "Suggests": "fcitx5 | ibus, fonts-noto-cjk",
            "Conflicts": "cde-desktop",
            "Replaces": "cde-desktop",
            "Maintainer": "CK Core Project <cklukas@users.noreply.github.com>",
            "Description": "CK Core Common Desktop Environment (X11/XLibre and Wayland)\n"
            " Modernized C23 Common Desktop Environment built against CK Core Motif ABI 9.",
        },
    )
    for sfx in ("postinst", "prerm", "postrm"):
        dst_s = r_cde / "DEBIAN" / sfx
        shutil.copy2(deb_meta / f"ck-core-cde.{sfx}", dst_s)
        os.chmod(dst_s, 0o755)
    deb_cde = out_dir / f"ck-core-cde_3.0.0-1_{arch}.deb"
    build_deb(r_cde, deb_cde)
    built_debs["ck-core-cde"] = deb_cde

    # Finish ck-core-dtlogin
    (r_dtlogin / "etc" / "pam.d").mkdir(parents=True, exist_ok=True)
    if (deb_meta / "dtlogin-greeter.pam").is_file():
        shutil.copy2(deb_meta / "dtlogin-greeter.pam", r_dtlogin / "etc" / "pam.d" / "dtlogin-greeter")
    (r_dtlogin / "lib" / "systemd" / "system").mkdir(parents=True, exist_ok=True)
    shutil.copy2(deb_meta / "dtlogin.service", r_dtlogin / "lib" / "systemd" / "system" / "dtlogin.service")
    dtl_needed = rewrite_rpath_and_collect_needed(r_dtlogin)
    dtl_shlibs = resolve_system_deb_deps(dtl_needed, internal_sonames, multiarch)
    dtl_deps = [
        "ck-core-cde (= 3.0.0-1)",
        "debconf (>= 0.5) | debconf-2.0",
        *dtl_shlibs,
    ]
    write_control(
        r_dtlogin,
        {
            "Package": "ck-core-dtlogin",
            "Version": "3.0.0-1",
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
    deb_dtlogin = out_dir / f"ck-core-dtlogin_3.0.0-1_{arch}.deb"
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
        "ck-core-cde (= 3.0.0-1)",
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

    # Generate signed APT repository metadata (Packages, Packages.gz, Release, Release.gpg, InRelease)
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

    # Sign APT Release metadata with GPG (producing Release.gpg, InRelease, ck-core-archive-keyring.asc)
    gpg_home = deb_work / "gnupg"
    gpg_home.mkdir(parents=True, mode=0o700)
    os.chmod(gpg_home, 0o700)
    gpg_env = {**os.environ, "GNUPGHOME": str(gpg_home)}
    key_spec = (
        "%echo Generating CK Core APT signing key\n"
        "Key-Type: RSA\n"
        "Key-Length: 3072\n"
        "Name-Real: CK Core APT Archive\n"
        "Name-Email: apt@ck-core.local\n"
        "Expire-Date: 0\n"
        "%no-protection\n"
        "%commit\n"
    )
    subprocess.run(["gpg", "--batch", "--generate-key"], input=key_spec, text=True, env=gpg_env, check=True)
    for old_sig in (out_dir / "Release.gpg", out_dir / "InRelease", out_dir / "ck-core-archive-keyring.asc"):
        if old_sig.exists():
            old_sig.unlink()
    subprocess.run(
        ["gpg", "--batch", "--yes", "--armor", "--detach-sign", "-o", str(out_dir / "Release.gpg"), str(out_dir / "Release")],
        env=gpg_env,
        check=True,
    )
    subprocess.run(
        ["gpg", "--batch", "--yes", "--clearsign", "-o", str(out_dir / "InRelease"), str(out_dir / "Release")],
        env=gpg_env,
        check=True,
    )
    keyring_asc = subprocess.check_output(["gpg", "--batch", "--armor", "--export", "apt@ck-core.local"], env=gpg_env)
    (out_dir / "ck-core-archive-keyring.asc").write_bytes(keyring_asc)

    # Record Motif & CDE provenance files alongside the APT repo and screenshots
    motif_pin = (cde_repo / ".ckgit" / "motif-revision").read_text(encoding="utf-8").strip()
    libxm_real = (r_motif / "usr" / "dt" / "lib" / "libXm.so.9").resolve()
    libxm_sha = sha256_file(libxm_real)
    (out_dir / "motif-revision.txt").write_text(f"{motif_pin}\n", encoding="utf-8")
    (out_dir / "motif.sha256").write_text(f"{libxm_sha}  /usr/dt/lib/{libxm_real.name}\n", encoding="utf-8")

    verification: dict[str, object] = {"verified_install": False}

    if args.verify_install:
        verification = verify_signed_repo(out_dir, out_dir, build_root / "tmp")

    shutil.rmtree(deb_work, ignore_errors=True)

    # Also run dtlogin's unit tests to confirm dtlogin.src change breaks nothing
    dtl_tmp = build_root / "tmp"
    dtl_tmp.mkdir(parents=True, exist_ok=True)
    dtl_env = os.environ.copy()
    dtl_env["TMPDIR"] = str(dtl_tmp)
    dtl_env["MOTIF_INCLUDE"] = str(motif_pfx / "include")
    dtl_env["DTLOGIN_CDE_STAGE"] = str(cde_stage / "usr" / "dt")
    dtl_env["LD_LIBRARY_PATH"] = f"{motif_pfx / 'lib'}:{fs_pfx / 'lib'}"
    subprocess.run(["make", "-j4", "-C", str(build_root / "cde-build" / "programs" / "dtlogin"), "check"], env=dtl_env, check=True)

    receipt = {
        "work_package": "WP-08",
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
    ap.add_argument("--verify-repo", metavar="DIR",
                    help="Verify an existing signed repository in a clean Ubuntu guest")
    ap.add_argument("--skip-harness-sync", action="store_true", help="Reuse existing staged build root without re-running harness")
    args = ap.parse_args()
    if args.verify_repo and (not args.verify_install or not args.output_dir):
        ap.error("--verify-repo requires --verify-install and --output-dir")
    if args.in_guest and args.verify_repo:
        verification = verify_signed_repo(
            Path(args.verify_repo).resolve(),
            Path(args.output_dir).resolve(),
            Path(args.guest_build_root) / "tmp",
        )
        receipt = json.dumps(verification, indent=2) + "\n"
        (Path(args.output_dir) / "apt-verification.json").write_text(receipt, encoding="utf-8")
        print(receipt, end="")
        return 0
    if args.in_guest:
        return guest_build_and_verify(args)
    return host_dispatch(args)


if __name__ == "__main__":
    sys.exit(main())
