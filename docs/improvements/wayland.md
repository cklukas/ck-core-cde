---
title: Wayland compositor
description: How dtwm-wayland brings authentic server-side CDE 3D bevel window frames, rootless XWayland, snap feedback and task switching to Wayland alongside X11/XLibre.
---

# Wayland compositor

`dtwm-wayland` is CK Core CDE's optional `wlroots-0.19` Wayland compositor, built
alongside classic X11 `dtwm` when `--enable-wayland` (`default=auto`) finds the
Wayland compositor development stack. It lets native Wayland clients and
Motif/X11 clients share one CDE desktop session without giving up CDE's visual
framing or interaction rules.

## Server-side CDE 3D bevel decorations

`dtwm-wayland` negotiates server-side decorations through
`zxdg_decoration_manager_v1` for Wayland `xdg_toplevel` windows and reads
`_MOTIF_WM_HINTS` decoration flags for rootless `Xwayland` clients:

- Mitered 3D top- and bottom-shadow bevel borders (`cde_bevel.c`) match `dtwm`'s
  `BevelRectangle` geometry across all four CK Core Motif ABI 9 appearance
  profiles (`classic`, `modern_light`, `modern_dark`, and `high_contrast`,
  harmonized with `ModernLight.dp`, `ModernDark.dp`, and `HighContrast.dp`),
  switchable at launch via `--appearance-profile PROFILE` or live via X11
  `RESOURCE_MANAGER` `*appearanceProfile:` updates and `_DT_WM_REQUEST`
  (`f.set_appearance_profile PROFILE`);
- Corner resize-handle grooves (`NW`, `N`, `NE`, `E`, `SE`, `S`, `SW`, `W`), the
  left MWM Window Menu button (`Restore`, `Move`, `Size`, `Minimize`, `Maximize`,
  `Lower`, `Close`), centered embossed title bar, and Minimize/Maximize buttons
  frame both Wayland and XWayland windows identically;
- XWayland client coordinates are offset by the frame's left and top insets so
  override-redirect Motif popup menus and dropdowns align directly beneath their
  parent menu bars.

## Window management interactions

`dtwm-wayland` preserves the CK Core `dtwm` interaction model:

- Double-clicking a title bar or pressing the Maximize button toggles
  maximize/restore, and double-clicking the top-left Window Menu button (`[ - ]`)
  closes the window;
- Clicking or dragging a window title bar, frame border, window button, native
  Wayland surface, or desktop background dismisses any open client popup or
  Motif menu (`XmMenuShell` / `xdg_popup`), while holding a mouse button during
  menu-bar press-drag-release keeps Wayland pointer focus on the grabbed client
  surface across override-redirect pulldown items until release;
- Interactive window resizing suppresses no-op configures and coalesces rapid
  pointer-motion bursts against client surface commits (paired with `dtfile`
  `ConfigureNotify` queue draining and 16 ms icon-relayout coalescing) so
  resizing finishes immediately without trailing redraw lag, and `DtHelp`
  display and TOC canvases support mouse-wheel scrolling (`Button4`/`Button5`
  and horizontal `Shift+Wheel` / `Button6`/`Button7`);
- Holding `Shift` during pointer move or resize aligns geometry to the 16-pixel
  desktop grid;
- Dragging toward output edges or corners previews half-screen and quadrant snap
  placement and commits on release;
- Pressing `Alt+Tab` opens and cycles the centered 3D-beveled keyboard task
  switcher, and `Alt+Space` opens the window menu;
- Left-clicking the desktop background clears active-window focus, while
  right-clicking opens the 3D-beveled **Workspace Menu** (supporting both
  click-to-post and Motif press-drag-release, plus `Up`/`Down`/`Enter`/`Esc`
  keyboard navigation) with matching actions across Wayland (`dtwm-wayland`) and
  X11 (`dtwm` `DtRootMenu`): *Open Terminal*, *Open File Manager*, *Application
  Manager*, *Change Backdrop...* (`dtstyle -backdrop`), *Display Settings...*
  (`dtstyle -monitors`), *Style Manager...*, *Clean Up Icons*, *Shuffle Up*,
  *Shuffle Down*, *Minimize/Restore Front Panel*, *Refresh*, *Lock Screen*, and
  *Log Out...*.

## CDE workspaces, per-workspace backdrops, and Front Panel

`cde_workspace.c` implements CDE's multi-workspace desktop model across both
Wayland and XWayland clients:

- Four default workspaces (`ws0` = *One*, `ws1` = *Two*, `ws2` = *Three*,
  `ws3` = *Four*) with dynamic workspace creation, renaming, and deletion up to
  eight workspaces, per-window occupancy bitmasks, and the *Occupy All
  Workspaces* / *Unoccupy Other Workspaces* action in the CDE Window Menu;
- Per-workspace CDE `.pm` (XPM) and `.bm` (XBM) backdrop rendering
  (`InlayPlain`, `SkyDark`, `Pebbles`, `Paver`, `BrickWall`, and `/usr/dt/backdrops`
  assets) recolored dynamically to each workspace's CDE color set in tiled,
  centered, or scaled mode;
- Full XWayland `libDtSvc` Workspace Manager property bridge publishing
  `_MOTIF_WM_INFO`, `_DT_WORKSPACE_LIST`, `_DT_WORKSPACE_CURRENT`,
  `_DT_WORKSPACE_INFO_<ws>`, and per-window `_DT_WORKSPACE_PRESENCE`, honoring
  client `_DT_WORKSPACE_HINTS` (`DtWsmSetWorkspacesOccupied`,
  `DtWsmOccupyAllWorkspaces`), and servicing `_DT_WM_REQUEST` commands
  (`f.change_backdrop`, `f.goto_workspace`, `f.add_workspace`,
  `f.delete_workspace`, `f.set_workspace_title`), with `libDtSvc` falling back
  cleanly to `_DT_WM_REQUEST` when `ttsession` is not active;
- Native Wayland `ext-workspace-v1` server global (`ext_workspace_manager_v1`)
  advertising workspace groups, coordinates, active state, and atomic commit
  activation/creation/removal;
- Hosts the **genuine CDE Front Panel** (`dtwm-panel`, built directly from
  `dtwm`'s `WmFP.c`, `UI.c`, `DataBaseLoad.c`, `Clock.c`, `Button.c`,
  `Callback.c`, `PanelS.c`, `PopupMenu.c`, and `Session.c`) on the primary
  output with full `.fp` database loading (`/usr/dt/appconfig/types/C/dtwm.fp`
  and `~/.dt/types/*.fp`), slide-up subpanels (`DtWM_BEHAVIOR_SUBPANEL`),
  `DtActionInvoke` controls, `Ctrl+Alt+1..8` and `Ctrl+Alt+Left/Right`
  shortcuts, and bottom strut reservation (`dtwm_get_usable_output_box`)
  respected by maximized and edge-snapped windows (`--no-panel` and `--greeter`
  hide the panel when needed).

## Open-window desktop icons, live dynamic icons, and Front Panel swallowing

`cde_icons.c` brings CK Core CDE's desktop icon and Front Panel embedding
contracts to Wayland without requiring application code changes:

- **Open-window and minimized desktop icons (`Dtwm*showOpenWindowIcons`) and
  interactive CDE Icon Box (`Dtwm*useIconBox`):** Publishes `_DT_WM_SETTINGS_V1`
  (`DT_WM_SETTINGS_V1_SHOW_OPEN_WINDOW_ICONS` and `DT_WM_SETTINGS_V1_USE_ICON_BOX`)
  on both the workspace-manager window and the XWayland root window, renders
  active-focus Crimson and inactive Slate 3D bevel icon tiles (honoring
  `Dtwm*iconPlacementMargin` when placed on the workspace), and switches live to
  a movable, resizable, mouse-wheel-scrollable CDE **`Icons`** (**Icon Box**)
  window when **Use Icon Box** is selected in `Style Manager - Window` (which
  reloads live `RESOURCE_MANAGER` values on reopen so all toggles persist across
  both dialog reopens and process restarts);
- **Normal (`48×48`) and increased (`96×96`) `WM_ICON_SIZE`:** Publishes
  `WM_ICON_SIZE` on the XWayland root window and supports live switching via
  `--large-icons` or `_DT_WM_REQUEST` (`f.set_large_icons 0|1`,
  `f.set_open_window_icons 0|1`);
- **Live dynamic icons across XWayland and Wayland:** Streams XWayland
  `WM_HINTS` `IconPixmapHint` and 1-bit `IconMaskHint` shape masks (`ck-clock`,
  `ck-load`, `ck-eyes`, `ck-coins`, `ck-plasma-1`, `ck-browser`) via XCB image
  capture, and implements the native Wayland `xdg-toplevel-icon-v1` server
  global (`xdg_toplevel_icon_manager_v1`) for `wl_shm` client icons;
- **Global cursor tracking for `ck-eyes`:** Publishes per-window `WM_STATE`
  (`NormalState` / `IconicState`) and `_NET_WM_ICON_GEOMETRY`, and synchronizes
  the XWayland root pointer whenever the cursor moves across native Wayland
  surfaces, the Front Panel, or the desktop background;
- **`dtfile` pinned desktop icons and genuine Front Panel / Subpanel UX:**
  Places `dtfile` `Desktop.c` workspace objects in `desktop_icon_tree` below
  normal application windows without duplicate open-window icons, keeps the
  classic CDE analog `Clock` as the default Front Panel clock, tracks open
  `Trash` window state (`_DT_TRASH_WINDOW` / `Fptrsh5`), constrains inline
  workspace rename editing to the exact toggle button geometry, and opens
  subpanels docked and frameless (`Fpup` closed / `Fpdown` open) until dragged
  away into a titled tool window (`_DT_SUBPANEL_TORN_OFF`). Optional `.fp`
  `TYPE client` controls (such as `CLIENT_NAME ck-clock` in
  `~/.dt/types/ckclock.fp`) embed directly inside `dtwm-panel` via
  `XReparentWindow` (with `--swallow NAME` retained as an explicit compositor
  override).

## Cross-protocol drag-and-drop, `ck-grab` screencopy, and mixed-DPI scaling

`dtwm-wayland` pairs with CK Core Motif ABI 9 (`libXm.so.9`) and `ck-grab` so
clipboard/drag-and-drop, screenshots, and mixed-DPI multi-monitor setups work
across native Wayland and XWayland clients simultaneously:

- **Bidirectional Motif XDND v5 ↔ Wayland `wl_data_device` drag-and-drop:**
  Native Wayland drags (`wl_data_device.start_drag`) render `wlr_scene_drag_icon`
  surfaces in `overlay_tree` and bridge automatically to Motif `XmDropSite`
  targets via `wlroots`'s XDND v5 forwarder, while `cde_xdnd_bridge.c` watches
  `XdndSelection` via XFixes on `dtwm-wayland`'s XCB connection and promotes
  active Motif `XmDragStart` drags into a `wlr_drag` (`text/uri-list`,
  `UTF8_STRING`, `text/plain;charset=utf-8`) the moment the pointer enters a
  native Wayland surface — preserving native X11-to-X11 Motif drags untouched
  over XWayland surfaces;
- **`ck-grab` Wayland screencopy, region overlay, and clipboard (`zwlr_screencopy_manager_v1`, `zwlr_layer_shell_v1`, `zwlr_data_control_manager_v1`):**
  `dtwm-wayland` publishes `_DT_ACTIVE_WINDOW_BOX` (`outer_x`, `outer_y`,
  `outer_w`, `outer_h`, `client_x`, `client_y`, `client_w`, `client_h`) on the
  XWayland root window, grants keyboard/pointer focus to interactive
  `zwlr_layer_shell_v1` `LAYER_OVERLAY` surfaces, and advertises
  `zwlr_data_control_manager_v1` alongside `wl_data_device_manager` so `ck-grab`
  captures active native Wayland windows, interactive drag-rectangle regions
  (`--region` / `Shift+Print`), and stitched multi-output desktops and copies
  `image/png` directly onto the desktop clipboard;
- **Per-output XLibre RandR `DPI` and `wp_fractional_scale_v1`:**
  `dtwm-wayland` opts rootless `Xwayland` into CK Core Motif's per-output RandR
  observer via `_MOTIF_RANDR_OUTPUT_DPI` (`CARDINAL = 1`) and publishes each
  output's `DPI` (`INTEGER`, format 32, configurable at startup with
  `--output-dpi INDEX=DPI` or live via `_DT_WM_REQUEST`
  `f.set_output_dpi INDEX DPI`) alongside `wp_fractional_scale_v1` for native
  Wayland clients, triggering crisp live Motif shell reflow when windows move
  across mixed-DPI monitors (`96`, `144`, `192`, ...);
- **XWayland Motif menu focus, virtual-pointer button recovery, and post-modeset
  `Screen` bounds (`dtwm_wayland_main.c`, `cde_xdnd_bridge.c`, `DtUtil.c`):**
  `dtwm-wayland` tracks per-device and bitmask pointer button state across
  physical and `zwlr_virtual_pointer_v1` (`wayvnc`) inputs—auto-releasing
  orphaned buttons on virtual-pointer disconnect or stale cross-button state,
  filtering off-screen helper windows (`_MOTIF_DRAG_WINDOW`) during outside-click
  popup dismissal, re-establishing `wl_pointer` surface focus before stationary
  clicks and override-redirect `XmMenuShell` map/geometry updates—while
  `DtInitialize` / `DtAppInitialize` (`libDtSvc`) and `dtfile` synchronize Xlib
  `Screen` dimensions on root `ConfigureNotify` so Motif pulldown and popup menus
  never clamp to boot dimensions after live `wlr-randr` / HiDPI output changes.

## `dtsession` Wayland session startup, `ext-session-lock-v1`, and `dtstyle` hardware IPC

`dtsession --wayland` (invoked by `/usr/dt/bin/Xsession.wayland`),
`cde_session_lock.c`, and `dtstyle` integrate CDE's session lifecycle, screen
lock, and Style Manager controls natively with Wayland:

- **`dtsession --wayland` session bootstrap:** Starts `dtwm-wayland` if
  `WAYLAND_DISPLAY` is not yet active, ensures `ttsession -s` is running with
  `_SUN_TT_SESSION` mirrored into `TT_SESSION` for both Wayland and XWayland
  clients, loads `xrdb` resources and the CDE Color Server (`InitializeDtcolor`),
  skips X11 `dtwm` fork while keeping XSMP and session restore active, and
  terminates a managed `dtwm-wayland` instance cleanly on logout;
- **`ext-session-lock-v1` and `ext-idle-notify-v1` screen lock:**
  `dtwm-wayland` implements `ext_session_lock_manager_v1` and
  `ext_idle_notifier_v1`, disabling all non-lock desktop/application/overlay
  scene trees while locked so `zwlr_screencopy_manager_v1` and keyboard/pointer
  shortcuts cannot leak a single pixel or event to underlying windows, covering
  hotplugged monitors on their first frame, keeping all outputs locked and
  black if a lock client crashes without `unlock_and_destroy()`, and hosting
  `dtsession`'s genuine Motif padlock PAM dialog (`SmLock.c` + `SmPam.c`,
  verified by `_DT_SM_WINDOW_INFO` client-ID ownership) in `lock_dialog_tree`;
- **`dtstyle` Wayland hardware and input IPC:** When `WAYLAND_DISPLAY` is set,
  `dtstyle` (`Protocol.c`) forwards Mouse acceleration/threshold and
  left-handedness (`f.set_pointer_accel`, `f.set_left_handed`), Keyboard
  auto-repeat and XKB layout (`f.set_key_repeat`, `f.set_xkb_layout`), Audio
  bell parameters (`f.set_bell`), and Screen blanker/lock timeouts
  (`f.set_saver_timeout`) to `dtwm-wayland`, which applies `libinput` and
  `wlr_keyboard` settings live and publishes `_DT_WAYLAND_INPUT_STATE` on the
  XWayland root window.

## Wayland-capable `dtlogin` display manager (`WP-07`)

`dtlogin` supports local Wayland seats alongside classic X11 `local` /
`local_uid` and `XDMCP` configurations:

- **`wayland` and `wayland_uid` display types in `Xservers`**: configuring a
  seat entry such as `:0 Local wayland@tty1 /usr/dt/bin/dtwm-wayland --greeter`
  runs an unprivileged `_dtgreeter` PAM `greeter` session (`dtlogin-greeter` /
  `pam_systemd.so class=greeter`), starting `dtwm-wayland --greeter` with
  `SI:localuser:root` XWayland authorization so the genuine Motif `dtgreet`
  greeter maps frameless at `(0, 0)` with compositor WM shortcuts disabled.
- **Dynamic FreeDesktop session menu**: `dtlogin` scans
  `/usr/share/wayland-sessions/*.desktop` and `/usr/share/xsessions/*.desktop`,
  populating `dtgreet`'s **Options → Session** menu with **CDE (Wayland)** first
  and **CDE (X11 / XLibre)** second followed by any additional installed
  Wayland and X11 sessions (filtering `Hidden`, `NoDisplay`, and missing
  `TryExec` targets and disambiguating duplicate names).
- **Clean VT/seat handoff and PAM session environment**: once PAM verification
  and any expired-password conversation (`SvcPam.c`) succeed, `dtlogin`
  terminates the greeter compositor to release `libseat`/DRM master, exports
  `XDG_SESSION_TYPE`, `XDG_SEAT`, `XDG_VTNR`, and `XDG_CURRENT_DESKTOP` into PAM
  (`_DtSvcPamPutenv`) and copies `pam_getenvlist` (`_DtSvcPamGetenvlist`) into
  the user session environment, then starts either the chosen Wayland session or
  a dedicated seat X11 server for X11 sessions and respawns `dtgreet` cleanly on
  logout.

## Workstation & laptop status, desktop notifications, and Front Panel integration (`WP-09`)

`CDE (Wayland)` and `CDE (X11 / XLibre)` share a native Motif workstation and
laptop status architecture designed in the style of classic SunSoft and IBM CDE
panel instrumentation:

- **Central Switch Notification Lamp (`dtwm.fp`):** Slot 3 (`POSITION_HINTS 3`,
  below `Lock`) of the central workspace Switch hosts `CONTROL Notifications`
  (`MONITOR_TYPE file`, polling `$HOME/.dt/notifications.unread` every `1.5 s`
  with symbolic-color `Fpnotif` / `FpnotifA` pixmaps). Incoming FreeDesktop
  notifications (`org.freedesktop.Notifications`) display a 3D-beveled CDE toast
  banner above the Front Panel and light the Switch indicator lamp; clicking the
  bell opens `ck-status --tab notifications` (`CkNotify`).
- **Slide-Up *System & Hardware* Subpanel (`SystemSubpanel`):** Positioned above
  `Desktop Style` (`Style`), providing one-click access to *System Status &
  Tray* (`CkStatus`), *Power & Battery* (`CkPower`), *Network & Wi-Fi*
  (`CkNetwork`), *Bluetooth* (`CkBluetooth`), *Audio Mixer* (`CkMixer`), and
  *Monitors & Display* (`DtstyleDisplay`). Right-clicking any subpanel item and
  choosing **Copy To Main Panel** immediately updates the main Front Panel and
  persists `~/.dt/sessions/dtwmfp.session` (saved both on customization and on
  `dtwm-panel` `SIGTERM`/`SIGINT` shutdown).
- **Live 4-Quadrant Status Gauge (`ck-status --gauge`):** Can be pinned into
  `CONTROL Blank2` (`POSITION_HINTS 8`, `~/.dt/types/ckstatus_gauge.fp`) as a
  borderless, centered `74×54` `TYPE client` (`ck-status-gauge`) panel
  instrument showing Wi-Fi signal bars, Bluetooth + notification/`StatusNotifier`
  lamp, and either an AA-proportioned battery + AC plug + percentage (on
  laptops) or a 3D CDE AC mains plug + live CPU usage percentage (on desktop
  workstations without a battery), with live `f.restart` reload and client
  re-embedding support in `dtwm-panel` / `dtwm-wayland`.

## Graphical multi-monitor arrangement & WebRTC desktop portals (`WP-10`)

- **Style Manager *Monitors* (`Display.c`):** `dtstyle` includes a dedicated
  **Monitors** component (`dtstyle -display_dialog` / `-monitors`) with an
  interactive 3D-beveled monitor-arrangement canvas (click to select, drag with
  automatic neighbor edge-snapping, *Side-by-Side* / *Stack Vertically* /
  *Mirror (0,0)* presets, output enable/disable, mode resolution, rotation
  `0°`/`90°`/`180°`/`270°`, and per-output HiDPI scale `100%`–`200%`). On
  Wayland it reads `_DT_WAYLAND_OUTPUTS` and applies live changes via
  `f.set_output_layout`; on X11/XLibre it drives `libXrandr`
  (`XRRGetScreenResourcesCurrent` / `XRRSetCrtcConfig` and per-output `DPI`).
- **Standard Wayland output & portal protocols (`cde_output_mgmt.c`):**
  `dtwm-wayland` exposes `zwlr_output_manager_v1` (enabling `wlr-randr` and
  `kanshi`), `zwlr_output_power_manager_v1`, `zxdg_output_manager_v1`,
  `zwlr_export_dmabuf_manager_v1`, `ext_image_copy_capture_manager_v1` +
  `ext_output_image_capture_source_manager_v1`, and
  `zwlr_foreign_toplevel_manager_v1`.
- **Packaged XDG Desktop Portals (`cde-portals.conf`):** `ck-core-cde` installs
  `/usr/share/xdg-desktop-portal/cde-portals.conf` routing `ScreenCast` and
  `Screenshot` to `wlr` (`xdg-desktop-portal-wlr` + PipeWire) and synchronizes
  `WAYLAND_DISPLAY`, `DISPLAY`, `XDG_CURRENT_DESKTOP`, and `XDG_SESSION_TYPE`
  into the D-Bus/systemd user activation environment at session startup.

## Native Wayland CJK Input Method framework (`WP-11`)

- **Compositor IME relay (`cde_ime.c`):** `dtwm-wayland` advertises
  `zwp_text_input_manager_v3` and `zwp_input_method_manager_v2` via
  `wlroots-0.19`, forwarding `enable`/`disable`, `surrounding_text`,
  `content_type`, `text_change_cause`, `preedit_string`, `commit_string`, and
  `delete_surrounding_text` between focused Wayland editors/browsers and modern
  IME daemons (`fcitx5`, `ibus`).
- **Keyboard grab & CJK candidate popups:** `zwp_input_method_keyboard_grab_v2`
  intercepts hardware keystrokes for kana/romaji/pinyin conversion while
  allowing synthetic keys emitted by the IME daemon's own
  `zwp_virtual_keyboard_v1` to pass directly to the client without feedback
  loops. `zwp_input_popup_surface_v2` candidate windows are anchored automatically
  below the client's `cursor_rectangle` (`popup_tree`) and clamped to the usable
  monitor workarea above the docked Front Panel.
- **Session-lock isolation & `dtstyle` Internationalization (`I18nEnv.c`):**
  Acquiring `ext-session-lock-v1` immediately deactivates `zwp_input_method_v2`,
  sends `leave` to `zwp_text_input_v3`, hides candidate popups, and bypasses
  `keyboard_grab` so lock-screen password entry is never exposed to third-party
  IME daemons. On local Linux workstations, `dtstyle`'s **Internationalization**
  dialog (`I18nEnv.c`) discovers installed `fcitx5` and `ibus-daemon` backends
  alongside `dtimsstart`, and `Xsession.wayland` / `dtsession --wayland`
  synchronize `XMODIFIERS`, `GTK_IM_MODULE`, and `QT_IM_MODULE` so native Wayland
  and Motif XWayland (`XmIm`) applications share the same IME daemon.

## Hardware media/brightness OSD, touchpad configuration, and cross-toolkit dark-mode sync (`WP-13`)

`CDE (Wayland)` and `CDE (X11 / XLibre)` provide unified laptop/workstation
hardware controls, touchpad configuration, and cross-toolkit appearance
synchronization:

- **Hardware multimedia, backlight & session keys + non-focus-stealing 3D OSD
  (`dtwm-media-ctl`):** Both `dtwm-wayland` and classic X11 `dtwm` (`WmEwmh.c`
  passive `XGrabKey`s + `sys.dtwmrc`) bind `XF86AudioRaiseVolume`,
  `XF86AudioLowerVolume`, `XF86AudioMute`, `XF86AudioMicMute`,
  `XF86MonBrightnessUp/Down` (enforcing a 5% minimum sysfs brightness floor via
  `/sys/class/backlight` or `systemd-logind` `SetBrightness`),
  `XF86KbdBrightnessUp/Down`, `XF86AudioPlay/Pause/Next/Prev/Stop` (MPRIS
  `org.mpris.MediaPlayer2.Player`), `Print` (`ck-grab`), and `Super+L`
  (`dtaction LockDisplay`). Volume, microphone, backlight, and media actions
  render a `336×84` 3D-beveled CDE OSD gauge (`_DT_OSD_STATE`, auto-hiding after
  `1.6 s`) without stealing focus from the active window.
- **Laptop touchpad settings (`dtstyle`) & 3-finger workspace swipe:** `dtstyle`
  **Mouse** (`dtstyle -mouse`) adds an etched **Touchpad** section controlling
  *Tap to Click*, *Natural Scrolling*, *Two-Finger Right-Click* (`clickfinger`
  vs. button areas), *Disable While Typing*, and *Disable With External Mouse*,
  persisting `*touchpad*` X resources while driving `libinput` IPC
  (`f.set_touchpad`, `_DT_WAYLAND_INPUT_STATE`) on Wayland and `XInput2`
  (`libXi.so.6` `libinput *` properties + `_DT_X11_TOUCHPAD_STATE`) on X11.
  Three-finger horizontal touchpad swipes (`wlr_pointer_gestures_v1` on Wayland;
  XInput 2.4 `XI_GestureSwipe*` and `_DT_WM_SWIPE` on X11) switch workspaces
  forward and backward.
- **Cross-toolkit dark/light mode synchronization (`cde-color-sync`):** Started
  automatically as `cde-color-sync --daemon` in `Xsession.wayland`,
  `dtsession --wayland`, and X11 `Xsession` (and registered in
  `cde-portals.conf` as `org.freedesktop.impl.portal.Settings=cde;gtk`).
  Switching between `ModernDark.dp` (`modern_dark`), `HighContrast.dp`
  (`high_contrast`), and light/classic palettes in `dtstyle`, `dtsession`, or
  `dtwm-wayland` updates D-Bus `org.freedesktop.portal.Settings`
  (`org.freedesktop.appearance` `color-scheme` `1` = `prefer-dark` / `2` =
  `prefer-light` + `SettingChanged`), X11 `_XSETTINGS_S0` binary
  `_XSETTINGS_SETTINGS` (`Net/ThemeName`), `gsettings`, and
  `~/.config/gtk-{3,4}.0/settings.ini` live.

## Polkit authentication, Secret Service PAM auto-unlock, and interactive region/clipboard capture (`WP-14`)

Both `CDE (Wayland)` and `CDE (X11 / XLibre)` integrate native administrative
elevation, automatic Secret Service keyring unlocking, and interactive region
clipboard screenshots:

- **Native Motif Polkit Authentication Agent (`ck-polkit-agent`):** Auto-started
  in `Xsession.wayland`, `dtsession --wayland`, and X11 `Xsession`. Registers
  `/org/cde/PolicyKit1/AuthenticationAgent` with `org.freedesktop.PolicyKit1`
  (`unix-session` with `systemd --user` `org.freedesktop.login1.User` `Display`
  discovery and `unix-process` fallback) and authenticates via Polkit 124+
  `/run/polkit/agent-helper.socket` (`polkit-agent-helper-1 --socket-activated`)
  or classic `polkit-agent-helper-1` pipes inside a `PR_SET_DUMPABLE=0`
  hardened, workspace-sticky CDE padlock modal dialog.
- **Secret Service PAM keyring auto-unlock:** `/etc/pam.d/dtlogin`
  (`dtlogin.src`) wires optional `-auth` / `-session ... auto_start` /
  `-password` `pam_gnome_keyring.so` hooks and `/etc/pam.d/dtsession`
  (`dtsession.src`) wires `-auth optional pam_gnome_keyring.so` (while keeping
  `session required pam_deny.so` and `password required pam_deny.so`), paired
  with `gnome-keyring-daemon --start --components=secrets,ssh` in both Wayland
  and X11 session startups so `org.freedesktop.secrets` unlocks automatically at
  login without modal prompts.
- **Interactive `ck-grab` region & clipboard capture (`Shift+Print`):** Bound in
  both `dtwm-wayland` and X11 `dtwm` (`WmEwmh.c` + `sys.dtwmrc`) to
  `ck-grab --region --clipboard`, freezing the desktop behind a dimmed crosshair
  rectangle selector (`zwlr_layer_shell_v1` `LAYER_OVERLAY` on Wayland;
  `XGrabPointer` rubber-band overlay on X11) and publishing cropped `image/png`
  directly to `zwlr_data_control_manager_v1` / `CLIPBOARD`.

## `dtfile` removable media (`UDisks2`), network shares (`GVFS`), archives, thumbnails, live XDG Trash, and `FileManager1` (`WP-16`)

`dtfile` delivers shared desktop storage, archive, thumbnail, trash, and D-Bus
file-manager services across both `CDE (Wayland)` and `CDE (X11 / XLibre)` (see
[File Manager improvements](file-manager.md)):

- **`UDisks2` USB/SD & LUKS volumes + `GVFS` shares & MTP phones (`Devices.c`):**
  Dynamically populates `dtfile`'s Shelf and **View → Mounts & Devices...** with
  hotplugged USB/SD drives, Motif passphrase-unlocked `crypto_LUKS` volumes,
  **File → Connect to Server...** (`smb://`, `sftp://`, `davs://`, `nfs://`),
  and `/run/user/$UID/gvfs` MTP/PTP phones, with safe *Unmount* and *Eject /
  Power Off*.
- **Built-in `libarchive` actions, shared thumbnails, live XDG Trash &
  `org.freedesktop.FileManager1`:** Adds **Extract Here**, **Extract To...**, and
  **Compress...** (`.zip`, `.tar.gz`, `.tar.xz`, `.tar.zst`, `.7z`) with
  two-pass `openat(..., O_NOFOLLOW)` Zip-Slip rejection; resolves
  `$XDG_CACHE_HOME/thumbnails/{large,normal}` previews; synchronizes
  `$XDG_DATA_HOME/Trash` and per-mount `<mount>/.Trash-$UID` live in both
  directions via `inotify`; and serves `org.freedesktop.FileManager1`
  (`ShowFolders`, `ShowItems`, `ShowItemProperties`).

## Readiness reporting, Ubuntu LTS APT packaging, and automated qualification

Passing `--status-file PATH` writes `WAYLAND_DISPLAY`, `DISPLAY`, and `PID`
atomically once both the Wayland socket and rootless `Xwayland` are ready.

- **Focused Wayland regression suite (`programs/dtwm` and `programs/dtlogin`):**
  `check-dtwm-wayland.py` (`WP-02` bevels, move/resize, grid/snap, `Alt+Tab`,
  cohort appearance profiles), `check-dtwm-wayland-ws.py` (`WP-03` workspaces,
  backdrops, `libDtSvc`, `ext-workspace-v1`, genuine `dtwm-panel`),
  `check-dtwm-wayland-icons.py` (`WP-04` open-window icons, `48`/`96`
  `WM_ICON_SIZE`, live XWayland & `xdg-toplevel-icon-v1` icons, `dtfile` pins,
  `ck-clock` Front Panel embedding), `check-dtwm-wayland-wp05.py` (`WP-05`
  bidirectional Motif XDND v5 ↔ Wayland `wl_data_device`, `ck-grab` screencopy,
  per-output mixed-DPI reflow), `check-dtwm-wayland-wp06.py` (`WP-06`
  `ext-session-lock-v1`, `ext-idle-notify-v1`, `dtsession --wayland`, `dtstyle`
  IPC), `check-wayland-dtlogin.py` (`WP-07` `_dtgreeter`, `.desktop`
  session discovery, Wayland/X11 seat handoff),
  `check-dtwm-wayland-wp09.py` (`WP-09` central Switch notification lamp,
  `SystemSubpanel` + `Copy To Main Panel` persistence, live `ck-status-gauge`
  embedding, `StatusNotifier` tray, and toast banner),
  `check-dtwm-wayland-wp10.py` (`WP-10` `wlr-randr`, `dtstyle` *Display &
  Monitors*, `zwlr_foreign_toplevel_manager_v1`, `ext-image-copy-capture-v1`,
  and `xdg-desktop-portal-wlr`), `check-dtwm-wayland-wp11.py` (`WP-11`
  `zwp_text_input_v3`, `zwp_input_method_v2`, `zwp_input_popup_surface_v2` CJK
  candidate popups, `keyboard_grab`, and `ext-session-lock-v1` isolation),
  `check-dtwm-wayland-wp12.py` (`WP-12` parallel dual Wayland + X11 `Xvfb`
  verification of `dtappintegrate --xdg --watch` live `inotify` indexing,
  `Super`/`Super+Space` → `ck-launch` `libXm.so.9` activation, Wayland
  `zwlr_foreign_toplevel_manager_v1` and X11 `_NET_CLIENT_LIST` /
  `_NET_ACTIVE_WINDOW` window switching, and `dtopen` / `mimeapps.list`
  persistence), `check-dtwm-wayland-wp13.py` (`WP-13` parallel dual Wayland +
  X11 `Xvfb` verification of `XF86Audio*` / `XF86MonBrightness*` 5% floor /
  MPRIS keys + non-focus-stealing 3D bevel OSD, `dtstyle` touchpad settings +
  3-finger workspace swipe, and `cde-color-sync` portal / `XSETTINGS` / GTK3-4
  dark-light sync), `check-dtwm-wayland-wp14.py` (`WP-14` parallel dual
  Wayland + X11 `Xvfb` verification of `ck-polkit-agent` `libXm.so.9` +
  `PR_SET_DUMPABLE=0` password retry/elevation/cancellation, `gnome-keyring-daemon`
  + `secret-tool` auto-unlock round-trip, and `ck-grab --region --clipboard` +
  `--paste-png` pixel-accurate crop/clipboard transfer),
  `check-dtwm-wayland-wp16.py` (`WP-16` parallel dual Wayland + X11 `Xvfb`
  verification of `dtfile` `libXm.so.9` `UDisks2` LUKS unlock/hotplug/eject,
  `GVFS` *Connect to Server* + MTP phone `inotify` discovery, 5-format archive
  round-trips + Zip-Slip rejection, FreeDesktop thumbnail resolution,
  `org.freedesktop.FileManager1` window raise + selection, and live
  bidirectional `inotify` XDG + `.Trash-$UID` Trash), and
  `check-dtwm-wayland-menus.py` (XWayland Motif pulldown/popup menu focus,
  mid-press `zwlr_virtual_pointer_v1` crash cleanup, `wayvnc` stuck-button
  auto-heal, stationary-click `wl_pointer` focus restoration, and post-modeset
  HiDPI two-click + press-drag-release menu activation) run automatically under
  `make check`.
- **Signed Ubuntu LTS `.deb` and APT repository qualification (`WP-08`):**
  `scripts/build-ubuntu-debs.py --verify-install` packages `ck-core-motif9`,
  `ck-core-fs-lib`, `ck-core-cde`, `ck-core-dtlogin`, and `ck-core-apps` into a
  GPG-signed APT repository (`Release.gpg`, `InRelease`,
  `ck-core-archive-keyring.asc`), installs them via
  `[signed-by=/etc/apt/keyrings/ck-core-archive-keyring.asc]` alongside Ubuntu's
  system `libxm4`, boots both the installed `CDE (Wayland)` and
  `CDE (X11 / XLibre)` desktops without `LD_LIBRARY_PATH`, verifies `/proc/<pid>/maps`
  against `/usr/dt/lib/libXm.so.9`, and captures full-desktop verification
  screenshots via `/usr/bin/ck-grab`.
