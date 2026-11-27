---
title: Packaging and Seat Portability
description: Native Debian/Ubuntu APT, Fedora/RHEL/openSUSE RPM, and FreeBSD ports/pkg packaging plus seatd, drm-kmod, and evdev Wayland seat portability.
---

# Cross-Distro and FreeBSD Packaging

CK Core CDE, CK Core Motif ABI 9 (`libXm.so.9`), `ck-core-fs-lib`, `dtlogin`,
and `ck-core-apps` package as a matched five-package cohort across Debian/Ubuntu
(`.deb` / APT), Fedora / RHEL / openSUSE (`.rpm` / DNF / Zypper), and FreeBSD
(`ports` / `pkg`).

All runtime libraries and executables of the desktop toolkit and CDE install
beneath `/usr/dt` with `RUNPATH=/usr/dt/lib`, isolating the native ABI 9
`libXm.so.9` stack from system Motif 2.3 (`libxm4` on Debian/Ubuntu, `motif` on
Fedora/RHEL, `libXm4` on openSUSE, and `x11-toolkits/open-motif` under
`/usr/local` on FreeBSD).

## Cohort Package Layout

| Package | Version | Prefix | Contents |
|---|---|---|---|
| `ck-core-motif9` | `2.4.0` | `/usr/dt` | Native ABI 9 `libXm.so.9`, `libMrm.so.9`, `libUil.so.9`, `<Xm/Wayland.h>`, `uil`, `xmbind`, loader path drop-in |
| `ck-core-fs-lib` | `0.3.0` | `/usr/dt` | `libck_file_engine.so` comparison, atomic copy/move, trash, and `ck_fs_search.h` streaming search |
| `ck-core-cde` | `3.0.0` | `/usr/dt` | `dtwm`, `dtwm-wayland`, `dtwm-panel`, `dtsession`, `dtfile`, `dtmail`, `dtcm`, `dtpad`, `dtterm`, `xdg-desktop-portal-cde`, `rpc.cmsd` |
| `ck-core-dtlogin` | `3.0.0` | `/usr/dt` | `dtlogin`, `dtgreet`, `dtchooser`, unprivileged `_dtgreeter` Wayland seat supervisor, PAM stacks, service unit (bundled into `x11/ck-core-cde` on FreeBSD) |
| `ck-core-apps` | `1.0.0` | `/usr` (`/usr/local` on FreeBSD) | `ck-calc`, `ck-clock`, `ck-grab`, `ck-pdf`, `ck-video`, `ck-view`, `ck-browser`, `ck-status`, `ck-tasks` |

## Debian and Ubuntu (`.deb` / APT)

`scripts/build-ubuntu-debs.py` builds the five `.deb` archives, signs APT
repository metadata (`InRelease`, `Release.gpg`, `ck-core-archive-keyring.asc`),
and optionally runs end-to-end `apt-get install` verification alongside
`libxm4`:

```sh
python3 scripts/build-ubuntu-debs.py --task-root "$TASK_ROOT" --verify-install
```

See [Build — Ubuntu and Debian `.deb` cohort packaging](build.md#ubuntu-and-debian-deb-cohort-packaging)
for guest qualification details.

## Fedora, RHEL / AlmaLinux / Rocky, and openSUSE (`.rpm`)

`packaging/rpm/ck-core-cde.spec` and `scripts/build-rpm.py` target Fedora 41+,
RHEL / AlmaLinux / Rocky 9–10, and openSUSE Tumbleweed / Leap 15.6:

- **SONAME isolation:** `%global __provides_exclude_from ^/usr/dt/lib/.*\.so.*$`
  and `%global __requires_exclude` prevent `/usr/dt/lib/libXm.so.9` from
  colliding with system `libXm.so.4` packages in DNF or Zypper dependency
  resolution.
- **Distribution PAM conditionals:** `%if 0%{?suse_version}` writes openSUSE
  `common-auth` / `common-account` / `common-password` / `common-session` +
  `pam_systemd.so` stacks under `/etc/pam.d/{dtlogin,dtsession}`, while Fedora
  and RHEL use `password-auth`, `postlogin`, `pam_selinux.so`, and
  `pam_systemd.so`.
- **Systemd presets and `_dtgreeter` creation:** `ck-core-cde` registers
  `rpc-cmsd.service` (`AF_UNIX` + `SO_PEERCRED`, `PrivateNetwork=yes`), and
  `ck-core-dtlogin` creates the unprivileged `_dtgreeter` account (`video`,
  `render`, and `input` supplementary groups) and registers `dtlogin.service`.

To stage and build binary RPMs (and `repodata/repomd.xml` when `createrepo_c` is
installed) from a built cohort root:

```sh
python3 scripts/build-rpm.py \
  --cde-repo /path/to/ck-core-cde \
  --guest-build-root /path/to/build-root \
  --output-dir /path/to/rpm-out \
  --distro-flavor fedora
```

Pass `--distro-flavor rhel9` or `--distro-flavor opensuse` for Enterprise Linux
or openSUSE macro sets, or `--stage-only` on hosts without `rpmbuild` to
validate per-subpackage `%files` manifests.

Install on Fedora / RHEL with `dnf` or on openSUSE with `zypper`:

```sh
# Fedora / RHEL / AlmaLinux / Rocky
sudo dnf install ./ck-core-motif9-*.rpm ./ck-core-fs-lib-*.rpm \
  ./ck-core-cde-*.rpm ./ck-core-dtlogin-*.rpm ./ck-core-apps-*.rpm

# openSUSE Tumbleweed / Leap
sudo zypper install ./ck-core-*.rpm
```

## FreeBSD Ports / `pkg` and `seatd` + `drm-kmod` + `evdev` Seat Portability

FreeBSD port skeletons live beneath `packaging/freebsd/`:

- `packaging/freebsd/x11-toolkits/ck-core-motif9`
- `packaging/freebsd/sysutils/ck-core-fs-lib`
- `packaging/freebsd/x11/ck-core-cde`
- `packaging/freebsd/x11/ck-core-apps`

`scripts/build-freebsd-pkg.py` stages `pkg(8)` `+MANIFEST` metadata, `rc.d`
service scripts (`/usr/local/etc/rc.d/dtlogin` and
`/usr/local/etc/rc.d/rpc_cmsd`), OpenPAM configurations
(`/usr/local/etc/pam.d/{dtlogin,dtlogin-greeter,dtsession}`), and
`/usr/local/share/{wayland-sessions,xsessions,xdg-desktop-portal}` descriptors.

### Seat, DRM, Input, and Local RPC Portability Details

1. **`seatd` auto-detection (`dtwm-wayland` and `dtlogin`):** When
   `/run/systemd/seats` is absent and `/var/run/seatd.sock` (FreeBSD default),
   `/run/seatd.sock`, or `$SEATD_SOCK` is a live Unix socket, both
   `DtloginStartWaylandGreeter()` / `DtloginPrepareWaylandSession()`
   (`cde/programs/dtlogin/wayland_seat.c`) and `ensure_seatd_backend_env()`
   (`cde/programs/dtwm/wayland/dtwm_wayland_main.c`) export `SEATD_SOCK` and
   `LIBSEAT_BACKEND=seatd`.
2. **FreeBSD `vt(4)` console naming & runtime directories:** `wayland_seat.c`
   maps FreeBSD `/dev/ttyv0`..`/dev/ttyvf` consoles to 1-based `XDG_VTNR`
   values (`1`..`16`), checks `/var/run/dtlogin` and FreeBSD `pam_xdg(8)`
   `/var/run/user/<uid>` runtime directories alongside `/run/...`, scans both
   `/usr/share/{wayland-sessions,xsessions}` and
   `/usr/local/share/{wayland-sessions,xsessions}`, and wraps non-systemd
   Wayland logins with `dbus-run-session` in `Xsession.wayland` when needed.
3. **`drm-kmod` and `evdev` compatibility:** `cde_evdev_compat.h` resolves
   `<linux/input-event-codes.h>` or FreeBSD `devel/evdev-proto`
   `<dev/evdev/input-event-codes.h>`; `probe_hw_3d_render_support()` inspects
   both `/dev/dri/renderD*` and `/dev/dri/card*` nodes exposed by `i915kms`,
   `amdgpu`, and `radeonkms`; `cde_power_display.c` adds FreeBSD
   `hw.acpi.acline` / `hw.acpi.battery.state` `sysctl` checks plus ConsoleKit2
   and `/usr/sbin/acpiconf -s 3` suspend fallbacks; and `configure.ac` exports
   `$(LIBDL)` so targets using `dlopen` link without `-ldl` on FreeBSD `libc`.
4. **Kernel-verified `AF_UNIX` credentials in `rpc.cmsd`:**
   `verify_unix_peercred()` (`cde/programs/dtcm/server/svcmain.c`) enforces
   `LOCAL_PEERCRED` (`struct xucred`) and `getpeereid()` on FreeBSD and other
   BSDs so `/var/run/rpc.cmsd/cmsd.sock` rejects forged `AUTH_UNIX` UIDs just as
   `SO_PEERCRED` does on Linux.

### Enabling the Wayland Greeter on FreeBSD 14+

```sh
# Load KMS driver and add users to the video group for /dev/dri access
sysrc kld_list+="i915kms"   # or amdgpu / radeonkms
pw groupmod video -m _dtgreeter
pw groupmod video -m "$USER"

# Enable seatd, D-Bus, rpcbind, rpc_cmsd, and dtlogin
sysrc seatd_enable="YES" dbus_enable="YES" rpcbind_enable="YES" \
      rpc_cmsd_enable="YES" dtlogin_enable="YES"
service seatd start
service dbus start
service rpc_cmsd start
service dtlogin start
```

## Automated Verification

Run the cross-distro and FreeBSD portability & packaging check suite:

```sh
python3 scripts/check-cross-distro-packaging.py
```
