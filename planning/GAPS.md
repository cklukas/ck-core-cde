# Desktop Experience Assessment & Gap Analysis: Ubuntu GNOME (Latest) vs. CK Core CDE / Wayland

**Date:** November 2026  
**Compared Baselines:**
- **Reference Modern Linux Desktop:** **Ubuntu 26.04 LTS** (*Resolute Raccoon*, GNOME 50, Wayland-only session) & **Ubuntu 26.10** (*Stonking Stingray*, GNOME 51), including default Ubuntu extensions (*Ubuntu Dock*, *Tiling Assistant*, *Desktop Icons NG*) and core desktop apps (*Nautilus / Files*, *GNOME Settings*, *App Center*, *Papers*, *Loupe*).
- **CK Core CDE / Wayland Stack:** `ck-core-cde` + `ck-core-motif` (ABI 9) + `ck-core-fs-lib` + `ck-core-apps` on branch `x11_wayland` (`WP-00` through `WP-11` complete), running `dtwm-wayland` (`wlroots-0.19`) with rootless `Xwayland`, `dtsession --wayland`, `dtlogin` Wayland greeter, `dtwm-panel`, modernized `dtfile` / `dtstyle`, and the 15-program `ck-core-apps` companion suite (`ck-status`, `ck-grab`, `ck-tasks`, `ck-mixer`, `ck-browser`, `ck-calc`, `ck-clock`, etc.).

---

## 1. Executive Summary

### What Ubuntu GNOME (50 / 51) Optimizes For
Ubuntu's latest GNOME desktop is an opinionated, tightly integrated consumer and developer workstation shell. Out of the box, a new user gets:
1. **Zero-configuration hardware & laptop UX:** Multimedia/backlight keys with OSD popups, multi-touch trackpad gestures, PipeWire audio auto-switching (Bluetooth headsets, HDMI, USB microphones), Night Light, VRR/HDR, power profiles, and lid-close suspend.
2. **Search-first workflow:** Pressing `Super` opens an instant overview combining live workspace/window thumbnails with unified fuzzy search across installed apps, files, settings panels, calculator expressions, and system actions.
3. **Seamless desktop plumbing:** Automatic XDG `.desktop` app menus, *App Center* GUI package management, `GVFS` / `UDisks2` one-click USB/phone/SMB/SFTP mounting in *Files* (Nautilus), Polkit privilege prompts, automatic keyring unlock, XDG portals, and dark-mode sync across toolkits.

### What CK Core CDE / Wayland Already Delivers (`WP-00` – `WP-11`)
Through `WP-00`–`WP-11`, CK Core CDE on Wayland has already closed the foundational compositor and session-infrastructure gulf that previously separated classic CDE from modern Wayland desktops:
- **Authentic Wayland Compositor (`dtwm-wayland`):** Server-side 3D mitered CDE bevel frames for both native Wayland (`xdg_toplevel`) and `Xwayland` windows, edge half-screen and corner quadrant snap previews, `Alt+Tab` task switcher, live wireframe/opaque move, sloppy/click focus, and 4 live-switchable appearance profiles (`classic`, `modern_light`, `modern_dark`, `high_contrast`) + `.dp` palettes.
- **Workspaces, Front Panel & Icons:** Genuine `dtwm-panel` Front Panel, 4–8 workspaces bridged across `libDtSvc`/ToolTalk and `ext-workspace-v1`, per-workspace recolored backdrops, open-window desktop icons, live dynamic icons (`ck-clock`, `ck-load`, `ck-eyes`, `ck-browser`, `ck-coins`), and panel `TYPE client` embedding.
- **Cross-Protocol Interop, Multi-Monitor & Portals:** Bidirectional Motif XDND v5 ↔ Wayland `wl_data_device` drag-and-drop, `dtstyle` graphical *Monitors* canvas with live `100%`–`200%` per-output HiDPI scaling (`wp_fractional_scale_v1` + XLibre RandR `DPI`), `zwlr_output_manager_v1` (`wlr-randr`/`kanshi`), and `xdg-desktop-portal` (`wlr` ScreenCast/Screenshot + `gtk` default).
- **Security, Login, IME & Workstation Status:** PAM-hardened `dtlogin` Wayland greeter (`_dtgreeter` + `libseat`), zero-leak `ext-session-lock-v1` screen lock, native Wayland CJK IME relay (`zwp_text_input_v3` + `zwp_input_method_v2` with `fcitx5`/`ibus`), and `ck-status` providing FreeDesktop desktop notification toasts, `StatusNotifier` system tray hosting, and Battery / Wi-Fi / Bluetooth monitoring in the Front Panel.

### Where the Major User-Facing Gaps Remain
If a typical Ubuntu GNOME user sits down in front of **CK Core CDE / Wayland** today, they will **not** be blocked by Wayland compatibility—Firefox, GTK4 apps, WebRTC screen sharing, CJK input, multi-monitor HiDPI, and notifications already work. Instead, **what they will immediately miss lies in desktop-ecosystem glue, modern file-manager workflows, laptop ergonomics, and settings/service integration**:
1. Newly `apt`/`snap`/`flatpak`-installed Linux apps do not appear automatically in any CDE launcher, and there is no `Super`-key search launcher.
2. Double-clicking common files (PDFs, photos, videos, archives) in `dtfile` hits legacy `dtopen` helpers instead of Ubuntu's installed XDG MIME apps, and plugging in a USB drive, phone, or SMB/SFTP share does not appear in `dtfile`.
3. Laptop hardware keys (volume, brightness, mute, screenshot), multi-touch touchpad settings/gestures, PipeWire Bluetooth/mic audio routing, and Polkit/Keyring prompts are missing or require manual CLI tools.

---

## 2. High-Level Feature Comparison Matrix

| Desktop Capability | Ubuntu 26.04 LTS / 26.10 (GNOME 50 / 51) | CK Core CDE / Wayland (`x11_wayland` + `ck-core-apps`) | User Impact / Gap |
| --- | --- | --- | --- |
| **Display Server & Framing** | Wayland-only Mutter (`Xwayland` on demand); client-side headerbars (CSD) | `dtwm-wayland` (`wlroots-0.19`) + rootless `Xwayland`; server-side 3D CDE bevel frames | **Parity** (different design philosophy; CDE frames unify Wayland + X11 apps) |
| **App Launching & Search** | *Ubuntu Dock* + `Super` *Activities* App Grid + instant global search (apps, files, settings, math) | CDE Front Panel + slide-up subpanels + static *Application Manager* + `Run...` command dialog | **Major Gap:** No auto-populated XDG `.desktop` menu; no `Super` type-to-search |
| **Software Installation** | *Ubuntu App Center* (Snap / DEB / drivers) + GUI *Software Updater* | APT CLI (`apt install`); signed `.deb` repo provided for CDE itself | **Major Gap:** No GUI software center or update notifier entry point |
| **Window Tiling & Overview** | Edge snap + *Tiling Assistant* top-edge layout picker, linked tile resize, `Super` live spatial overview | Drag-to-edge half/quadrant snap, Shift-grid align, `Alt+Tab` switcher, open-window desktop icons | **Moderate Gap:** No keyboard snap shortcuts, layout picker, or live spatial overview |
| **File Manager Core** | *Nautilus*: tabs (`Ctrl+T`), breadcrumbs, live recursive search (`Ctrl+F`), `Ctrl+Z` undo, `Ctrl+C/X/V` files, archives | `dtfile`: Shelf bar, `IconicPath`, image icons, Storage Size dedup, read-only Folder Compare | **Major Gap:** No tabs, live search, undo, file copy/paste shortcuts, or archive UI |
| **Removable & Network Storage** | Auto-discovered USB/LUKS, MTP phones, `smb://`, `sftp://`, Nextcloud/Google Drive in sidebar | Local POSIX paths only; manual mount or system automounter outside `dtfile` | **Major Gap:** USB drives, phones, and SMB/SFTP shares invisible in `dtfile` |
| **File Associations (MIME)** | XDG Shared MIME + `mimeapps.list` + *"Open With..."* dialog | CDE `.dt` datatypes/actions + `dtopen` hardcoded `xv`/`okular`/`xpdf`/`vlc` list | **Major Gap:** Double-clicking PDFs/images fails unless legacy helpers installed |
| **Notifications & System Tray** | Top-bar calendar/notification drawer, DND, Ubuntu AppIndicator/SNI dock/top-bar icons | `ck-status` (`org.freedesktop.Notifications` 3D toasts + history + DND + `StatusNotifier` host + Switch lamp) | **Near Parity** (missing actionable notification buttons & inline media player widget) |
| **Audio & Microphone** | PipeWire + WirePlumber in Quick Settings & Settings (output/input picker, Bluetooth codecs, per-app volume) | `ck-mixer` (ALSA playback channels only); no mic, PipeWire profile switch, or Bluetooth audio routing UI | **Major Gap:** Cannot switch to Bluetooth/HDMI audio or control mic easily |
| **Network & Bluetooth** | Quick Settings inline Wi-Fi password modal, VPN toggles, Hotspot, full BlueZ pairing wizard | `ck-status` shows link/IP/SSIDs & radio toggles; delegates setup to `nm-connection-editor`/`nmtui`; connect-only BT | **Moderate Gap:** No native Wi-Fi password/VPN dialog or Bluetooth pairing agent |
| **Display & Color** | Graphical layout, per-monitor fractional scale, VRR, HDR, Night Light, persistent monitor brightness | `dtstyle` *Monitors* layout, rotation, `100%`–`200%` live scale; `wlr-output-management-v1` | **Moderate Gap:** No Night Light, VRR/HDR, or laptop backlight slider/persistence |
| **Input & Laptop Touchpad** | Tap-to-click, natural scroll, 2-finger click, disable-with-mouse, 3-finger swipe, keyboard layout preview | `dtstyle` mouse accel/handedness, XCursor size, key repeat, XKB layout, `fcitx5`/`ibus` Wayland CJK IME | **Major Gap (Laptops):** Missing touchpad settings/gestures & hardware Fn/media keys |
| **Power & Suspend** | UPower + `power-profiles-daemon` slider, screen dim, idle suspend, lid-close policy, battery history | `ck-status` reads battery %/watts/platform profile; `dtstyle` blanker/lock timers (`ext-idle-notify-v1`) | **Moderate Gap:** No idle auto-suspend, lid-close policy, or interactive power-mode switch |
| **Security & Desktop Daemons** | Polkit GUI agent (`pkexec`), `gnome-keyring` PAM auto-unlock, `xdg-desktop-portal-gnome` | PAM `dtlogin` + `ext-session-lock-v1`; `wlr` + `gtk` portals; no Polkit agent or Keyring session unlock | **Major Gap:** `pkexec` admin apps fail silently; browsers/VS Code prompt repeatedly for keyring |
| **Screenshots & Screencasts** | Interactive `Print` overlay (region crop, window, screen, copy to clipboard, MP4/WebM screencast) | `ck-grab` (Full Screen / Active Window to PNG/XPM; CLI + GUI) + WebRTC `ScreenCast` portal | **Moderate Gap:** No interactive region crop, clipboard-only copy, or desktop video recorder |
| **Default Everyday Apps** | *Papers* (PDF + sign), *Loupe* (Image), *Text Editor*, *Snapshot* (Camera), *System Monitor*, *Clocks*, *Calendar* | `dtpad`, `dtterm`, `dtcm`, `dtmail`, `dtinfo`, `ck-calc`, `ck-clock`, `ck-browser`, `ck-tasks`, `ck-character-map` | **Moderate Gap:** No modern PDF/image viewer; `ck-tasks` cannot kill processes yet; legacy PIM |
| **Accessibility (a11y)** | Orca screen reader (AT-SPI2), desktop zoom, AccessX keys, reduced motion, *Myna* local dictation preview | `HighContrast.dp`, `96×96` large icons, scalable Xft fonts | **Major Gap:** No AT-SPI2 screen-reader exposure, compositor magnifier, or AccessX UI |

---

## 3. Detailed Gap Catalog (From a New Ubuntu GNOME User's Perspective)

### 3.1 Application Discovery, Launching & Software Management

- **GAP-LAUNCH-01 — No automatic XDG `.desktop` menu synchronization**
  - **GNOME experience:** Any application installed via `apt`, `snap`, or `flatpak` (`/usr/share/applications`, `/var/lib/snapd/desktop/applications`, `~/.local/share/applications`) immediately appears in the launcher with its icon, localized name, and category.
  - **CDE / Wayland today:** CDE's *Application Manager* reads `/usr/dt/appconfig/appmanager` and `~/.dt/appmanager` populated by `dtappintegrate` (CDE `.dt` actions). Standard Ubuntu desktop packages do not appear in *Application Manager* or Front Panel subpanels unless manually wired.
  - **What the user feels:** *"I installed GIMP, LibreOffice, and VLC, and I have no idea where they went unless I open a terminal and guess their binary names."*

- **GAP-LAUNCH-02 — No `Super`-key instant search launcher**
  - **GNOME experience:** Pressing the `Super` (Windows/Command) key opens an instant keyboard-driven search bar that fuzzy-matches apps, open windows, settings panels, recent documents, and quick calculations.
  - **CDE / Wayland today:** `Super` is unbound; launching requires clicking Front Panel subpanels, drilling through *Application Manager* folders, or typing an exact executable command into `Run...`.

- **GAP-LAUNCH-03 — No graphical Software Center or Update Notifier integration**
  - **GNOME experience:** *Ubuntu App Center* lets newcomers browse/install apps and GPU drivers, while *Software Updater* notifies when security updates are pending.
  - **CDE / Wayland today:** Software installation and OS updates are purely terminal-driven (`apt`), with no Front Panel launcher or `ck-status` security-update indicator.

---

### 3.2 File Manager (`dtfile` vs. GNOME Files / Nautilus)

- **GAP-FILES-01 — Removable media (USB drives, SD cards, phones) & encrypted volumes**
  - **GNOME experience:** Plugging in a USB drive, LUKS-encrypted stick, camera, or Android/iOS phone (`MTP`/`PTP`/`AFC`) pops up a notification and adds a sidebar entry in *Files* with one-click mount, passphrase unlock, and safe *Eject / Safely Remove*.
  - **CDE / Wayland today:** `dtfile` has no `UDisks2` or `GVFS` integration. Unless an external automounter is configured by hand, removable media do not mount on click or show an eject control in `dtfile` or the Shelf.

- **GAP-FILES-02 — Network filesystems & cloud storage (`smb://`, `sftp://`, `davs://`)**
  - **GNOME experience:** Users connect to Windows/NAS shares (`smb://`), SSH servers (`sftp://`), WebDAV/Nextcloud, and Google Drive directly from *Other Locations* in *Files*, backed by `gvfsd-fuse` so even non-GIO apps can read the mounted files.
  - **CDE / Wayland today:** `dtfile` navigates mounted local POSIX directories only; there is no graphical *Connect to Server...* dialog.

- **GAP-FILES-03 — Legacy `dtopen` file associations instead of XDG MIME & *"Open With..."***
  - **GNOME experience:** Double-clicking any file consults XDG Shared MIME (`~/.config/mimeapps.list`), and right-clicking offers *"Open With..."* listing all compatible installed `.desktop` applications with a *"Always use for this file type"* toggle.
  - **CDE / Wayland today:** `dtopen` hardcodes legacy helper lists (`xv display gimp` for images, `okular xpdf` for PDFs, `mgv gv` for PostScript, `vlc ffplay` for video) without falling back to `xdg-open` or Ubuntu's installed defaults (*Papers* / *Evince*, *Loupe*). Double-clicking a PDF on a fresh Ubuntu + CDE install displays a *"Helper not found"* error dialog.

- **GAP-FILES-04 — Everyday navigation & keyboard ergonomics (Tabs, Live Search, Clipboard, Undo)**
  - **GNOME experience:** Users rely heavily on `Ctrl+T` browser tabs, mouse Back/Forward buttons, `Ctrl+F` instant recursive file/content search, `Ctrl+C` / `Ctrl+X` / `Ctrl+V` file copy/cut/paste, `F2` inline rename, `Ctrl+H` hidden-files toggle, and `Ctrl+Z` Undo (reversing an accidental move, rename, or Move to Trash).
  - **CDE / Wayland today:** `dtfile` features CK Core's Shelf bookmark bar, `IconicPath`, Storage Size inspector, and read-only Folder Comparison preview (Apply remains compile-time disabled), but lacks multi-tab browsing, live type-ahead/recursive search in the view, standard `Ctrl+C/X/V` file clipboard operations, and `Ctrl+Z` undo history.

- **GAP-FILES-05 — Built-in archive extraction/creation, rich thumbnails & `FileManager1` D-Bus**
  - **GNOME experience:** Double-clicking `.zip`, `.tar.gz`, `.xz`, or `.7z` archives extracts them cleanly (or right-click *Compress...*); PDFs, videos, and SVGs show rich thumbnails (`~/.cache/thumbnails`); and clicking *"Show in Folder"* in Firefox/Chromium highlights the downloaded file via `org.freedesktop.FileManager1`.
  - **CDE / Wayland today:** `dtfile` renders direct raster image icons, imports `~/.local/share/Trash` one-way at startup (not live/bidirectional or per-mount `.Trash-$UID`), does not claim `org.freedesktop.FileManager1`, and has no graphical archive extract/compress workflow.

---

### 3.3 Desktop Shell, Window Management & Laptop Input (`dtwm-wayland` vs. GNOME Shell)

- **GAP-SHELL-01 — Unbound hardware media, volume, brightness & session keys + OSD**
  - **GNOME experience:** Laptop keys for Volume Up/Down/Mute, Mic Mute, Screen Brightness Up/Down, Keyboard Backlight, Media Play/Pause/Next, `Print` (Screenshot), and `Super+L` (Lock) work out of the box with visual OSD level indicators.
  - **CDE / Wayland today:** `dtwm-wayland` binds window/workspace shortcuts (`Alt+Tab`, `Alt+Space`, `Ctrl+Alt+1..8`, `Ctrl+Alt+Left/Right`), while hardware `XF86*` keys, `Print`, and `Super+L` are not bound by default and have no on-screen OSD gauge.

- **GAP-SHELL-02 — Laptop touchpad configuration & multi-touch Wayland gestures**
  - **GNOME experience:** *Settings → Mouse & Touchpad* configures tap-to-click, natural scrolling, two-finger right-click, touchpad speed, disable-while-typing, automatic touchpad disable when an external mouse is plugged in (Ubuntu 26.10), and 3-finger horizontal/vertical swipe gestures for workspace/overview navigation.
  - **CDE / Wayland today:** `dtstyle` *Mouse* configures pointer acceleration, threshold, button handedness, double-click time, and cursor size on `dtwm-wayland`, but does not configure touchpad-specific `libinput` options (tap-to-click, natural scroll, clickfinger) or multi-touch gestures.

- **GAP-SHELL-03 — Keyboard window tiling, split-resize & spatial window overview**
  - **GNOME experience:** Ubuntu's *Tiling Assistant* supports `Super+Arrow` half/quarter tiling, linked resizing of adjacent tiled windows, a Windows-11-style slide-in layout picker when dragging to the top screen edge (Ubuntu 26.10), and a `Super` overview displaying live scaled previews of all windows across workspaces.
  - **CDE / Wayland today:** `dtwm-wayland` supports pointer drag-snap to screen edges/corners and `Alt+Tab` cycling + open-window desktop icons, without keyboard snap bindings, linked split-resizing, or a live spatial thumbnail overview.

- **GAP-SHELL-04 — Interactive region screenshots, clipboard capture & screen recording**
  - **GNOME experience:** Pressing `Print` opens an interactive overlay to drag-select a rectangular region, pick a window, or capture a screen, copy directly to the Wayland clipboard (`wl-copy` workflow for pasting straight into chat/GitHub/docs), or record a screencast video.
  - **CDE / Wayland today:** `ck-grab` captures *Full Screen* or *Active Window* to a saved PNG/XPM file; interactive region crop, direct clipboard copy without saving a file, and desktop video recording UI are not yet implemented.

---

### 3.4 System Settings, Audio, Display & Background Desktop Services

- **GAP-SYS-01 — Modern PipeWire / WirePlumber audio & Bluetooth headset management**
  - **GNOME experience:** Users switch audio output/input devices (laptop speakers vs. Bluetooth headphones vs. HDMI monitor vs. USB mic), select A2DP high-fidelity vs. HFP headset profiles, adjust microphone gain, and control per-app audio streams from Quick Settings or *Settings → Sound*.
  - **CDE / Wayland today:** `ck-mixer` drives low-level ALSA (`libasound`) playback controls on a selected card. It lacks capture/microphone controls, PipeWire sink/source switching, per-application volume, and Bluetooth audio profile management—and exits if no hardware ALSA card is present.

- **GAP-SYS-02 — Missing Polkit authentication agent & Secret Service keyring auto-unlock**
  - **GNOME experience:** When an application needs admin privileges (`pkexec`, GParted, printer setup, system network edits), GNOME displays a modal Polkit password prompt; logging in via GDM unlocks `gnome-keyring` (`org.freedesktop.secrets`) automatically so Chrome, Chromium, `ck-browser`, VS Code, Git, and NetworkManager retrieve saved credentials seamlessly.
  - **CDE / Wayland today:** `Xsession.wayland` and `dtlogin` do not launch a Polkit authentication agent or unlock a Secret Service keyring via PAM, causing `pkexec` GUI actions to fail without a prompt and modern apps to either lose saved secrets or show unfamiliar fallback dialogs.

- **GAP-SYS-03 — Native Wi-Fi onboarding (WPA/Enterprise/VPN) & Bluetooth pairing agent**
  - **GNOME experience:** Clicking an unfamiliar Wi-Fi network in Quick Settings prompts inline for the password; Bluetooth *Settings* handles full discovery, PIN/passkey confirmation (`org.bluez.Agent1`), pairing, trust, and removal.
  - **CDE / Wayland today:** `ck-status` displays live Wi-Fi SSIDs/bars and paired Bluetooth devices and toggles radios, delegating new Wi-Fi/VPN configuration to external `nm-connection-editor` or `dtterm -e nmtui`, and invoking `Connect`/`Disconnect` on already-paired BlueZ devices without a built-in pairing agent.

- **GAP-SYS-04 — Display Night Light, VRR/HDR, Backlight & Laptop Suspend Policy**
  - **GNOME experience:** Users get scheduled warm *Night Light* (`wlr-gamma-control`), Variable Refresh Rate (VRR) and HDR toggles, remembered per-display brightness sliders, interactive *Power Saver / Balanced / Performance* switching, automatic idle suspend, and configurable laptop lid-close behavior (suspend vs. docked external monitor clam-shell mode).
  - **CDE / Wayland today:** `dtstyle` *Monitors* + *Screen* covers layout, rotation, resolution, `100%`–`200%` scaling, and screen blanker/lock timeouts (`zwlr_output_power_manager_v1`), while Night Light, backlight control, lid-close policy, idle suspend, and interactive power-profile switching remain uncovered.

- **GAP-SYS-05 — Toolkit Dark/Light Mode Portal Sync (`org.freedesktop.appearance`)**
  - **GNOME experience:** Toggling *Dark Style* switches GTK4/libadwaita, Firefox, Chromium, Electron, and Qt apps simultaneously via `org.freedesktop.portal.Settings` (`color-scheme`).
  - **CDE / Wayland today:** Switching `dtstyle` to `ModernDark.dp` / `modern_dark` recolors Motif/CDE apps and `dtwm-wayland` server-side frames live, while native GTK/Firefox/Qt client interiors stay light (or vice versa) unless configured separately.

- **GAP-SYS-06 — Printers (CUPS/IPP), Default Apps, Accounts, Time/Date & Firmware Panels**
  - **GNOME experience:** *Settings* provides zero-config network printer discovery (CUPS / IPP Everywhere), Default Applications (Web, Mail, Calendar, Music, Video, Photos), User Accounts, Timezone/NTP toggle, and Firmware Updates (`fwupd`).
  - **CDE / Wayland today:** `dtprintinfo` reflects classic LP print queues rather than modern driverless CUPS configuration, and there are no GUI panels for Default Applications, Users, NTP/Timezone, or `fwupd`.

---

### 3.5 Built-In Everyday Applications & Accessibility

- **GAP-APPS-01 — Missing modern PDF & Image viewers; Task Manager action controls** *(Closed in WP-19: `ck-view`, `ck-pdf`, and actionable `ck-tasks`)*
  - **GNOME experience:** Ubuntu ships *Papers* (fast PDF/DjVu viewer with search, annotations, and visual signatures in 26.10), *Loupe* (GPU-accelerated image viewer with WebP/HEIC/AVIF, zoom/pan, rotate, print), and *System Monitor* (kill/renice processes, live CPU/network graphs).
  - **CDE / Wayland today:** CDE has `dtinfo` / `dthelpview` (for CDE documentation) and `dticon` (for small XPM/XBM icon pixel editing), plus `ck-browser` (CEF web/PDF fallback), `ck-calc`, `ck-clock`, and `ck-tasks`—where `ck-tasks`'s *Processes* and *Services* tables are currently read-only (no *End Process* / *Start/Stop Service* actions yet, and *Networking* is a static layout preview).

- **GAP-APPS-02 — Legacy PIM & Editor expectations (`dtmail`, `dtcm`, `dtpad`)** *(Closed in WP-19: `dtpad` tabs/recent/line-numbers/autosave and `dtcm` RFC 5545 `.ics` import/export)*
  - **GNOME experience:** *GNOME Text Editor* offers multi-tab session autosave, crash recovery, and syntax highlighting; *GNOME Calendar* / *Thunderbird* (Ubuntu default) connects to Google/Microsoft/Nextcloud via OAuth2, CalDAV, CardDAV, and HTML mail.
  - **CDE / Wayland today:** `dtpad` is a clean single-document plain-text Motif editor; `dtmail` targets classic UNIX spool/IMAP without OAuth2 or modern HTML rendering; and `dtcm` uses CDE's `rpc.cmsd` daemon rather than `.ics` / CalDAV calendars.

- **GAP-APPS-03 — Accessibility (Screen Reader, Magnification & Keyboard Assist)** *(Closed in WP-20: `AccessX`, compositor/`ck-magnifier` zoom, Reduced Motion, and Motif AT-SPI2 bridge)*
  - **GNOME experience:** Full AT-SPI2 widget exposure to the Orca screen reader under Wayland, smooth compositor zoom/magnifier, visual bell alerts, Sticky/Slow/Bounce keys, Reduced Motion toggle, and local speech-to-text preview (*Myna* in 26.10).
  - **CDE / Wayland today:** Provides `HighContrast.dp`, `96×96` large icons, and scalable fonts, without AT-SPI2 accessibility tree export from Motif widgets or compositor screen magnification.

---

## 4. Work-Package Mapping (`WP-12` – `WP-20`) & Dual Wayland + X11/XLibre Execution Policy

CK Core CDE's strength is that it is **fast, deterministic, C/Motif-native, low-memory, and visually cohesive**—closing these gaps should **not** imitate GNOME's UI paradigm, but rather wire modern Linux desktop plumbing into authentic CDE / Motif workflows.

### Standing Dual-Environment & Parallel Testing Rule
- **Shared Wayland + X11/XLibre Availability:** Every gap resolution in `WP-12`–`WP-20` is architected to work in **both `CDE (Wayland)` (`dtwm-wayland` + `dtsession --wayland`) and `CDE (X11 / XLibre)` (`dtwm` + `dtsession`)** wherever technically sensible. Motif UI, D-Bus/PipeWire/UDisks2/Polkit services, `dtfile`, `dtstyle`, and `ck-core-apps` code are shared across both sessions, with thin protocol backends only where Wayland (`wlroots` / Wayland protocols) and X11 (`Xlib` / `Xrandr` / `XInput2` / `XKB` / `Xss`) diverge.
- **Parallel Verification Gate:** Every WP (`WP-12`–`WP-20` in [`../wayland_work_plan.md`](../wayland_work_plan.md)) executes automated acceptance tests **in parallel** against **both** the headless `dtwm-wayland` + rootless `Xwayland` harness **and** the headless `Xvfb` + X11 `dtwm` / `dtsession` harness.

### Gap-to-Work-Package Traceability (`WP-12` – `WP-20`)

| WP | Status | Tier | Gaps Closed | Wayland Execution (`CDE (Wayland)`) | X11 Execution (`CDE (X11 / XLibre)`) |
| --- | --- | --- | --- | --- | --- |
| **`WP-12`** | **Delivered** | **P0** | `GAP-LAUNCH-01..03`, `GAP-FILES-03` | XDG `.desktop` indexer, `Super` → `ck-launch` (`zwlr_foreign_toplevel_manager_v1`), `dtopen` XDG MIME & *"Open With..."* | Identical `.desktop` indexer, `Super` → `ck-launch` (`_NET_CLIENT_LIST` / `sys.dtwmrc`), identical `dtopen` & *"Open With..."* |
| **`WP-13`** | **Delivered** | **P0** | `GAP-SHELL-01..02`, `GAP-SYS-05` | `dtwm-wayland` `XF86*`/`Print`/`Super+L` + OSD, `libinput` touchpad IPC & 3-finger swipes, `org.freedesktop.appearance` dark sync | X11 `dtwm` `XGrabKey` + OSD, `XInput2` (`libXi` `libinput *`) touchpad properties, portal + `XSETTINGS` dark sync |
| **`WP-14`** | **Delivered** | **P0/P1** | `GAP-SYS-02`, `GAP-SHELL-04` | `ck-polkit-agent` in `Xsession.wayland`, PAM keyring unlock, `ck-grab` `zwlr_layer_shell_v1` region + `wl_data_device` PNG copy | `ck-polkit-agent` in X11 `Xsession`, PAM keyring unlock, `ck-grab` `XGrabPointer` region + X11 `CLIPBOARD` PNG copy |
| **`WP-15`** | **Delivered** | **P1** | `GAP-SYS-01`, `GAP-SYS-03` | `ck-mixer` PipeWire/PulseAudio sinks/sources/mic/BT profiles; `ck-status` inline Wi-Fi/VPN & `org.bluez.Agent1` pairing | **100% shared** Motif + PipeWire/D-Bus clients running natively on X11 |
| **`WP-16`** | **Delivered** | **P1** | `GAP-FILES-01..02`, `GAP-FILES-05` | `dtfile` `UDisks2` USB/LUKS mount/eject, `GVFS` shares/MTP, archives, thumbnails, live XDG Trash & `FileManager1` | **100% shared** `dtfile` + D-Bus/FUSE integration running natively on X11 |
| **`WP-17`** | **Delivered** | **P1/P2** | `GAP-FILES-04` | `dtfile` tabs (`Ctrl+T`), async `Ctrl+F` search, `Ctrl+C/X/V` file clipboard (bridged to Wayland), `Ctrl+Z` undo & Folder Sync Apply | **100% shared** `dtfile` + `ck-core-fs-lib` + X11 `CLIPBOARD` (`x-special/gnome-copied-files`) |
| **`WP-18`** | **Delivered** | **P2** | `GAP-SYS-04`, `GAP-SYS-06`, `GAP-SHELL-03` | `wlr-gamma-control` Night Light, `wlr_output` VRR, `ext-idle-notify-v1` + `WLR_SWITCH_TYPE_LID` suspend, `Super+Arrow` tiling, CUPS/admin | `XRRSetCrtcGamma` Night Light, RandR VRR (where driver supports), `Xss`/`UPower` suspend, `dtwm` `Super+Arrow` tiling, shared CUPS/admin |
| **`WP-19`** | **Delivered** | **P1/P2** | `GAP-APPS-01..02` | Actionable `ck-tasks` (kill/renice/net/systemd), `ck-view` image viewer, `ck-pdf` viewer, `dtpad` tabs/autosave & `dtcm` `.ics` | **100% shared** Motif ABI 9 + CDE applications running natively on X11 |
| **`WP-20`** | **Delivered** | **P2** | `GAP-APPS-03` | `dtwm-wayland` `AccessX` filters, compositor zoom (`Super+Scroll`), Reduced Motion & `libXm.so.9` `org.a11y.atspi` bridge | XKB `XkbSetControls` `AccessX`, `ck-magnifier` XComposite zoom, Reduced Motion & **shared** `libXm.so.9` `org.a11y.atspi` bridge |
