# CK Core CDE & Motif Wayland + Ubuntu Work Plan (`WP-00` – `WP-12`)

## Objective and Completion Rule

Deliver a first-class, `apt`-installable **CK Core CDE Wayland Desktop Session** (alongside classic **XLibre / X11** support from the same repositories) that preserves 100% of CDE's visual and behavioral contracts:
- Authentic 3D bevel window frames, resize handles, MWM hints, task switcher, and snap previews;
- Instant Front Panel workspace switching, multi-workspace window occupancy, and per-workspace backdrops;
- Open-window desktop icons (`Dtwm*showOpenWindowIcons`), active-focus markers, Front Panel `TYPE client` swallowing (`ck-clock`), and live dynamic icons (`ck-clock`, `ck-load`, `ck-eyes`, `ck-coins`, `ck-browser`, `ck-plasma-1`);
- PAM-backed Wayland screen locking (`ext-session-lock-v1` + `SmPam.c`);
- Wayland-capable `dtlogin` replacing `gdm3` cleanly on Ubuntu (`systemd-logind` / `libseat`); and
- Seamless coexistence of native Wayland apps (Firefox, GTK4/5) and Motif/X11 apps.

## Standing Workspace, Branch, Build & Qualification Rules

- **Dedicated branch (`x11_wayland`)**: All commits for this workstream across `ck-core-cde`, `ck-core-motif`, and `ck-core-apps` must go onto the **`x11_wayland`** branch (leaving `master`/`main` untouched until explicit merge approval).
- **Mac temporary workspace**: All Mac-side temporary data, Lima ephemeral VM disks (`LIMA_HOME`), caches (`XDG_CACHE_HOME`), scratch artifacts, and logs must live beneath `/Volumes/S8T/tmp/<task-id>` (fallback `/Volumes/PRO-BLADE/tmp` only if `/Volumes/S8T` is unmounted; never `/tmp` or `/private/tmp`).
- **Fast Lima Linux compilation on Apple Silicon (`vz`)**: Compile inside the ARM64 Lima VM on its native Linux ext4 filesystem (e.g. `/var/tmp/ck-wayland-build/<task-id>`, with `TMPDIR` set there) while reading source checkouts from the read-only host Git mount—avoiding VirtioFS metadata overhead during object compilation.
- **Parallelism cap**: Use at most **`-j6`** parallel build or test jobs per task.
- **Matched ABI 9 stack & build freshness**: Pin CK Core Motif ABI 9 (`.ckgit/motif-revision`), build into an isolated prefix, record start/end timestamps, source/binary SHA-256 hashes, and verify `/proc/<pid>/maps` proves every running CDE/Motif binary maps the recorded prefix's `libXm.so.9`. Retain only the current verified build; clean superseded scratch trees after recording `receipt.json`.
- **Topical commits & docs**: Update `cde/CHANGES` / `CHANGES` / `CHANGES.md` and `docs/` (`ckdocs check --config ckdocs.yml --strict`) in the same topic commit as implementation and automated tests.

---

## Track 1: Full CDE Wayland Session + Rootless XWayland + Ubuntu `dtlogin`

### [x] WP-00 — Mac Lima Ubuntu Qualification VM & Headless Wayland Test Harness
- **Repos**: `ck-core-cde` (`scripts/lima/cde-ubuntu-26.04.yaml`, `scripts/run-lima-wayland-harness.py`, `validation/`)
- **Scope**:
  - Provision an isolated ARM64 Ubuntu 26.04 LTS Lima VM (`vmType: vz`) rooted under `/Volumes/S8T/tmp/`.
  - Install build & Wayland test dependencies inside the VM (`clang`, `lld`, `libwlroots-dev`, `wayland-protocols`, `wayland-scanner`, `xwayland`, `libseat-dev`, `libinput-dev`, `libxkbcommon-dev`, `libdrm-dev`, `libpixman-1-dev`, `labwc`, `grim`, `wlrctl`, `wayvnc`, `xvfb`).
  - Provide `scripts/run-lima-wayland-harness.py` that builds CK Core Motif (ABI 9) + `ck-core-fs-lib` + CK Core CDE + `ck-core-apps` on VM-local storage (`-j6`), boots a dual-output headless `wlroots` + rootless `Xwayland` session, launches freshly built CDE and `ck-core-apps` clients, verifies `/proc/<pid>/maps` `libXm.so.9` provenance, captures dual-output PNG frames via `zwlr_screencopy_manager_v1` (`grim`), and writes `receipt.json`.
- **Acceptance gate**: Harness runs cleanly from macOS with `TMPDIR`/`LIMA_HOME` under `/Volumes/S8T/tmp`, builds the matched native C23 stack, passes headless dual-output Wayland + XWayland smoke checks, and emits a verified receipt.

### [x] WP-01 — Modern Ubuntu/Debian `.deb` Packaging (`apt install ck-core-cde`)
- **Repos**: `ck-core-motif`, `ck-core-cde` (`cde/debian/`), `ck-core-apps`
- **Scope**:
  - Replace legacy `cde/debian/` metadata with clean Debian/Ubuntu packaging producing `ck-core-motif9`, `ck-core-fs-lib`, `ck-core-cde`, `ck-core-dtlogin`, and `ck-core-apps` without colliding with system `libxm4`.
  - Ship `/usr/share/xsessions/cde.desktop`, `/usr/share/wayland-sessions/cde-wayland.desktop`, `/etc/pam.d/{dtsession,dtlogin}` (with `pam_systemd.so`), and `debconf` `shared/default-x-display-manager`.
- **Acceptance gate**: `apt install ./ck-core-*.deb` succeeds on clean Ubuntu 26.04 LTS with zero broken dependencies and launches a verified CDE session.

### [ ] WP-02 — `dtwm-wayland` Compositor Core, Server-Side CDE Bevel Frames & Rootless XWayland
- **Repos**: `ck-core-cde` (`cde/programs/dtwm/`)
- **Scope**:
  - Implement `wlroots` C23 compositor backend supporting DRM/KMS (`libseat`), nested Wayland, and headless backends, plus `xdg-shell`, `xdg-decoration-unstable-v1` (server-side CDE bevel frames), `zwlr_layer_shell_v1`, `zwlr_screencopy_manager_v1`, and rootless `wlr_xwayland`.
  - Render pixel-identical 3D CDE window frames, titlebars, MWM buttons, resize handles, Shift-grid alignment, double-click maximize, half/quadrant snap feedback, and keyboard task switcher (`Dtwm*showTaskSwitcher`) for both native Wayland and XWayland windows.
- **Acceptance gate**: Automated headless multi-window test verifies identical CDE frames, focus, snap preview, task switcher, and window menu actions across native Wayland and Motif clients under ASan/UBSan.

### [ ] WP-03 — CDE Workspaces, Per-Workspace Backdrops & Front Panel Integration
- **Repos**: `ck-core-cde` (`cde/programs/dtwm/`, `cde/lib/DtSvc/DtUtil1/`)
- **Scope**:
  - Implement CDE's workspace model (4 default workspaces, dynamic add/rename/delete, workspace occupancy bitmasks) bridged across `_DT_WORKSPACE_*` / `_DT_WM_REQUEST` XWayland atoms and `ext-workspace-v1` / `zck_cde_workspace_v1`.
  - Render palette-recolored per-workspace CDE backdrops (`WmBackdrop.c`) and dock Front Panel (`WmFP.c`) + subpanels with strut geometry respected by maximized Wayland and XWayland windows.
- **Acceptance gate**: Automated test exercises Front Panel workspace switching, *Occupy Workspace...*, and `DtWsmChangeBackdrop` across mixed Wayland/XWayland clients.

### [ ] WP-04 — Open-Window Desktop Icons, Live Dynamic Icons, `ck-eyes` & Panel Swallowing
- **Repos**: `ck-core-cde` (`cde/programs/dtwm/`, `cde/programs/dtfile/`), `ck-core-apps`
- **Scope**:
  - Implement `Dtwm*showOpenWindowIcons`, active-focus markers, `WM_ICON_SIZE` (`32/64` and `48/96`), live `WM_HINTS.icon_pixmap` / `xdg-toplevel-icon-v1` streaming (`ck-clock`, `ck-load`, `ck-coins`, `ck-browser`, `ck-plasma-1`), virtual root cursor + icon geometry sync for `ck-eyes`, `dtfile` pinned desktop icons, and Front Panel `TYPE client` swallowing (`ck-clock`).
- **Acceptance gate**: Smoke suite confirms live frame updates and minimized cursor tracking across all dynamic-icon apps.

### [ ] WP-05 — Cross-Protocol DnD (Motif ↔ Wayland XDND v5), `ck-grab` Screencopy & Mixed-DPI Scaling
- **Repos**: `ck-core-motif` (`lib/Xm/DragC.c`, `DropSMgr.c`, `XlibreDpi.c`), `ck-core-cde`, `ck-core-apps` (`src/ck-grab/`)
- **Scope**:
  - Add XDND v5 initiator/receiver support alongside Motif DnD so `wlroots` bridges Motif ↔ native Wayland `wl_data_device` drops cleanly; add `zwlr_screencopy_manager_v1` capture to `ck-grab`; run XWayland unscaled and publish per-output `XLibre` `DPI` RandR properties consumed by `XlibreDpi.c`.
- **Acceptance gate**: Automated tests pass bidirectional `dtfile` ↔ native Wayland drops, `ck-grab` mixed-window screenshots, and 96↔192 DPI live shell reflow.

### [ ] WP-06 — `dtsession` Wayland Screen Lock (`ext-session-lock-v1`), Session Startup & `dtstyle` Hardware IPC
- **Repos**: `ck-core-cde` (`cde/programs/dtsession/`, `cde/programs/dtstyle/`, `cde/programs/dtscreen/`, `cde/lib/tt/`)
- **Scope**:
  - Add `dtsession --wayland` session bootstrap (`ttsession` env + XWayland `_SUN_TT_SESSION`, `xrdb`, XSMP), `ext-session-lock-v1` screen lock backed by `SmPam.c`, `ext-idle-notify-v1` idle timers, and `dtstyle` Wayland `libinput`/XKB/bell/saver IPC.
- **Acceptance gate**: Security tests verify input/screencopy blocking while locked, hotplug output lock coverage, and PAM unlock.

### [ ] WP-07 — Wayland-Capable `dtlogin` Display Manager for Ubuntu (`systemd-logind` + `libseat`)
- **Repos**: `ck-core-cde` (`cde/programs/dtlogin/`, `cde/lib/DtSvc/DtUtil2/SvcPam.c`)
- **Scope**:
  - Add unprivileged `_dtgreeter` Wayland seat support (`libseat` + `dtwm-wayland --greeter` + `dtgreet`), dynamic `/usr/share/{wayland-sessions,xsessions}` menu population, and `pam_systemd` session handoff for `CDE (Wayland)`, `CDE (XLibre / X11)`, and installed Ubuntu desktops.
- **Acceptance gate**: VM service test boots `dtlogin.service`, logs into `CDE (Wayland)`, logs out to `dtgreet`, and logs into `CDE (X11)` cleanly.

### [ ] WP-08 — Full Ubuntu LTS Qualification, Documentation & APT Repository
- **Repos**: `ck-core-motif`, `ck-core-cde`, `ck-core-apps`
- **Scope**:
  - Pass full `make check`, sanitizer runs, real-session Wayland + X11 smoke screenshots with `libXm.so.9` provenance, `ckdocs check --config ckdocs.yml --strict`, and signed APT repository verification on Ubuntu LTS.

---

## Track 2 (Optional Later Phase): Pure XWayland-Free Native Motif Runtime

- **[ ] WP-09 — Dual `Xlib`/`Xt` Display Seam & In-Process Subwindow Virtualizer**
- **[ ] WP-10 — Pixman / `wl_shm` Widget Rasterizer, FreeType/HarfBuzz Blitter & `xdg_popup` Menus**
- **[ ] WP-11 — Native Wayland Selections, DnD (`wl_data_device`), `zwp_text_input_v3` IME & `wp_fractional_scale_v1`**
- **[ ] WP-12 — Unify External X11 Bypasses (`ck-clock`, `ck-coins`, `ck-browser`, `dtfile`)**
