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
- **Backend-aware minimize/restore and maximize/restore animations (`Dtwm*minimizeAnimation`, `Dtwm*maximizeAnimation`, and `Dtwm*animationSpeed`):**
  `dtwm-wayland` drives window transitions from per-output vsync frame callbacks
  with mid-flight reversal and unmap/lock cancellation. **Style Manager → Window**
  dynamically detects Wayland vs. X11 and exposes the matching choices for each
  backend together with an **Animation Speed** selector (`Very Quick`, `Standard`
  [default], and `Slow`, live-configurable via `Dtwm*animationSpeed` or
  `f.set_animation_speed`):
  - **Wayland (`dtwm-wayland`):** **Bevel Morph** / **Bevel Glide** (`bevel`,
    default — zero-copy live client surface morph framed by crisp unscaled `2px`
    Motif bevels and a subtle mid-flight `3×3` CAD hairline that keeps the live
    window surface crisp across the workspace and symmetrically crossfades
    into/from the desktop icon tile only near the icon, finishing with a
    mechanical Motif bevel latch on arrival),
    **Titlebar Shuttle** (`titlebar` — detached 3D CDE title bar gliding into the
    icon tile or maximized bounds), **Wireframe** (`wireframe` — palette-tinted
    `3×3` CAD wireframe with phosphor decay trail), and **None** (`none`);
  - **X11 (`dtwm`):** **Wireframe** (`wireframe`, minimize default), **Titlebar**
    (`titlebar`, flying caption-bar outline), and **None** (`none`, maximize
    default), honoring `Dtwm*animationSpeed`, preserving legacy
    `Dtwm*animateMinimizeRestore` compatibility, and automatically falling back
    when roaming `$HOME` between Wayland and X11 sessions;
- Clicking or dragging a window title bar, frame border, window button, native
  Wayland surface of another client, or desktop background dismisses any open
  client popup or Motif menu (`XmMenuShell` / `xdg_popup`), while `xdg_popup`
  reposition events (`xdg_popup.reposition`) re-unconstrain shared `XmMenuShell`
  pulldowns as the user moves across menu-bar cascades and holding a mouse button
  during menu-bar press-drag-release keeps Wayland pointer focus on the grabbed
  client surface across pulldown items until release;
- Interactive window resizing suppresses no-op configures, enforces 1-in-flight
  latest-wins `wlr_xdg_toplevel_set_size` serial tracking (`pending_resize_serial`)
  so older in-flight buffer commits never snap the outer CDE bevel frame backward
  while preserving idle client-driven self-resizes, and pairs with CK Core Motif
  `VendorShell` `ConfigureNotify` queue compression, Wayland `xdg_surface.configure`
  coalescing, and `dtfile` 16 ms icon-relayout coalescing so resizing settles
  immediately at the latest geometry without backlog or rubber-banding, while
  `DtHelp` display and TOC canvases support mouse-wheel scrolling
  (`Button4`/`Button5` and horizontal `Shift+Wheel` / `Button6`/`Button7`);
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
  (`KnitLight`, `WaterDrops`, `SkyDark`, `Paver`, and `/usr/dt/backdrops`
  assets, honoring `Dtwm*ws0..ws3*backdrop*image`) recolored dynamically to
  each workspace's CDE color set in tiled, centered, or scaled mode;
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
  `DtActionInvoke` controls, live `CONTROL Busy` activity-indicator blinking
  (`Fplite` ↔ `FpliteY` + busy watch cursor driven by ToolTalk
  `DtActivity_Beginning` / `DtActivity_Began` notices, compositor launcher
  spawns, and automatic completion turn-off when a new native Wayland or
  XWayland top-level window maps), right-click *Add Subpanel* / *Delete
  Subpanel* context menus on both main controls and the subpanel arrow bar
  above them, X11-faithful docked subpanel persistence (staying open when
  clicking the desktop or `dtfile` so items can be dragged onto *Install Icon*,
  closing on arrow toggle or docked control activation), transparent 32-bit
  ARGB `XmDRAG_WINDOW` drag-and-drop icons from `dtfile` and Motif
  applications, `Ctrl+Alt+1..8` and `Ctrl+Alt+Left/Right`
  shortcuts, and bottom strut reservation (`dtwm_get_usable_output_box`)
  respected by maximized and edge-snapped windows (`--no-panel` and `--greeter`
  hide the panel when needed).

## Open-window desktop icons, live dynamic icons, and Front Panel swallowing

`cde_icons.c` brings CK Core CDE's desktop icon and Front Panel embedding
contracts to Wayland without requiring application code changes:

- **Open-window and minimized desktop icons (`Dtwm*showOpenWindowIcons`),
  `Top Left` placement (`Dtwm*iconPlacement`), 3D open-window indicator bars,
  and interactive CDE Icon Box (`Dtwm*useIconBox`):** Publishes
  `_DT_WM_SETTINGS_V1` (`DT_WM_SETTINGS_V1_SHOW_OPEN_WINDOW_ICONS` and
  `DT_WM_SETTINGS_V1_USE_ICON_BOX`) on both the workspace-manager window and the
  XWayland root window, places desktop icons along `Top Left` by default (with
  full `Dtwm*iconPlacement` and `Dtwm*iconPlacementMargin` support plus
  bottom-left `FrontPanel` `Fpfpmin` workspace icon placement), renders sunken
  `selectColor` bevels for open windows and raised bevels for minimized windows
  with `_NET_WM_ICON_NAME` / `WM_ICON_NAME` labels and a separate `10 px` 3D
  open-window focus indicator bar `7 px` to the right of each open icon
  (matching X11 `dtwm` `WmIDecor.c`), toggles minimize when clicking an
  already-focused open window's icon, and switches live to a movable, resizable,
  mouse-wheel-scrollable CDE **`Icons`** (**Icon Box**) window when **Use Icon
  Box** is selected in `Style Manager - Window`;
- **Normal (`48×48`) and increased (`96×96`) `WM_ICON_SIZE`:** Publishes
  `WM_ICON_SIZE` on the XWayland root window and supports live switching via
  `--large-icons` or `_DT_WM_REQUEST` (`f.set_large_icons 0|1`,
  `f.set_open_window_icons 0|1`);
- **Full CDE client icon resolution and live dynamic icons across XWayland and
  Wayland:** Resolves `Dtwm.defs` / xrdb `Dtwm*<Class>*iconImage` mappings
  (`Fpstyle`, `Fpterm`, `Fphelp`, `Fppenpd`, `Dtcalc`, `DtMail`, `Dtpaint`,
  `Fpfpmin`, etc.), CDE `.dt` `ACTION` `ICON` entries via `WM_COMMAND` and
  `_NET_WM_PID`, symbolic `.pm`/`.bm` icons, `_NET_WM_ICON`, live XWayland
  `WM_HINTS` `IconPixmapHint` + 1-bit `IconMaskHint` shape masks (`ck-clock`,
  `ck-load`, `ck-eyes`, `ck-coins`, `ck-plasma-1`, `ck-browser`), and the native
  Wayland `xdg-toplevel-icon-v1` server global (`xdg_toplevel_icon_manager_v1`);
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
  Native Wayland drags (`wl_data_device.start_drag`, including `dtfile` file-view
  and Shelf drags via CK Core Motif's `XmDragOverShell` `wl_drag_icon` surface)
  render cursor-attached folder/file drag icons in `overlay_tree`—excluded from
  `desktop_view_at` pointer hit-testing so the icon never shadows underlying drop
  sites—and bridge automatically to Motif `XmDropSite` targets via `wlroots`'s
  XDND v5 forwarder, while `cde_xdnd_bridge.c` watches `XdndSelection` via XFixes
  on `dtwm-wayland`'s XCB connection and promotes active Motif `XmDragStart`
  drags into a `wlr_drag` (`text/uri-list`, `UTF8_STRING`,
  `text/plain;charset=utf-8`) the moment the pointer enters a native Wayland
  surface — preserving native X11-to-X11 Motif drags untouched over XWayland
  surfaces;
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

- **`dtsession --wayland` session bootstrap and logout lifecycle:** Starts
  `dtwm-wayland` if `WAYLAND_DISPLAY` is not yet active, ensures `ttsession -s`
  is running with `_SUN_TT_SESSION` mirrored into `TT_SESSION` for both Wayland
  and XWayland clients, loads `xrdb` resources and the CDE Color Server
  (`InitializeDtcolor`), skips X11 `dtwm` fork while keeping XSMP and session
  restore active, and terminates `dtwm-wayland` cleanly on logout whether
  `dtwm-wayland` was spawned by `dtsession` (`SIGTERM`) or started externally
  before `dtsession --wayland` (`_DT_WM_REQUEST` `f.quit_mwm`). `dtsession`
  registers its `XSession_Exit` handler with `TT_HANDLE_PUSH` priority while
  `dtwm-panel` provides a `TT_HANDLE` fallback (verifying live
  `_DT_SM_WINDOW_INFO` window state), so `ExitSession` from the Front Panel,
  Workspace Menu, or `dtaction ExitSession` exits cleanly without
  `TT_ERR_NO_MATCH` even in standalone `dtwm-wayland` sessions or if `dtsession`
  exited prematurely;
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
  (`CkNetwork`), *Bluetooth* (`CkBluetooth`), *Audio Mixer* (`CkMixer`),
  *Sound & MIDI* (`CkSound`), and *Monitors & Display* (`DtstyleDisplay`);
  Style Manager (`dtstyle`) also exposes a top-level **Sound** (`Fpsound`)
  launcher alongside **Beep** (which now offers a live **Test** button and
  selectable **Classic Beep (Tone)** or **General MIDI Note** system bells
  backed by `~/.dt/beep.conf` across both `dtwm-wayland` and `dtwm`).
  Right-clicking any subpanel item and
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
  While the dialog is open, a semi-transparent, click-through 3D-beveled corner
  badge displaying each monitor's large number (`1`, `2`, `3`, ...) and output
  name (matching the corner badge in the virtual monitor arrangement canvas,
  with a gold highlight rim on the selected monitor) appears at the top-right
  of every active screen on both Wayland (`f.identify_outputs` in
  `layer_overlay_tree`) and X11/XLibre (`ShapeInput` `overrideShell` overlays),
  and disappears automatically when the dialog closes.
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

## Application Manager catalog curation, on-demand package installer, and XDG app discovery (`WP-12`)

`dtappintegrate --xdg` (`dtappintegrate-xdg`), `ck-pkg-install`, `ck-browser`,
and `ck-launch` keep Application Manager folders accurate and actionable across
both `CDE (Wayland)` and `CDE (X11 / XLibre)`:

- **Curated catalog detection, auto-gray download badges & `ck-pkg-install`:**
  Across curated Application Manager groups (`Internet`, `Graphics`,
  `Media_Tools`, `Office`, `Games`, `Education`, `System`, `Tex`),
  `dtappintegrate-xdg` checks installed executables and batch-queries the
  system package registry (`apt`, `dnf`, `yum`, `pacman`, `zypper`, `apk`,
  FreeBSD `pkg`):
  - **Installed applications** (including **Web Browser** / `CkBrowser` →
    `ck-browser` in `Internet`, `CkView` in `Graphics`, `CkPdf` in `Office`,
    `CkMines`/`CkNibbles` in `Games`, and `CkAdmin`/`CkTasks` in `System`) keep
    their full-color CDE icons and launch directly.
  - **Available in system package registry** (e.g. `firefox`, `chromium`,
    `thunderbird`): automatically generates desaturated grayscale
    `48×48` / `32×32` / `16×16` XPM icons (`~/.dt/icons/ckavail_<slug>.{l,m,t}.pm`)
    stamped at the bottom-right with a royal-blue downward arrow (`↓`) and
    wires double-click to the native Motif `ck-pkg-install` dialog, which
    displays package metadata, elevates via `pkexec` + `ck-polkit-agent`,
    streams live package-manager output, refreshes Application Manager back to
    full-color icons on completion, and optionally launches the newly installed
    app.
  - **Absent from both `PATH` and package registry** (discontinued legacy stubs
    such as `skype` or `nxclient`): automatically hidden from the user's
    Application Manager folder.
- **Live XDG `.desktop` indexing & `Super` launcher (`ck-launch`):** Indexes
  `/usr/share/applications` and `~/.local/share/applications` (`inotify` live
  reload via `dtappintegrate --xdg --watch`) into `XDG_*` Application Manager
  groups and binds `Super` / `Super+Space` to `ck-launch` for instant fuzzy
  application search, window switching, and inline calculator evaluation.

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
- **Scroll wheel settings, animated cursor previews, laptop touchpad settings (`dtstyle`), & 3-finger workspace swipe:** `dtstyle`
  **Mouse** (`dtstyle -mouse`) renders premultiplied-ARGB Xcursor previews
  centered within fixed `72×72` 6-column slots across all `16`–`64` cursor sizes
  (without clipping or label baseline shifts) and drives live multi-frame
  animation for animated busy cursors (such as `Adwaita`'s `watch` spinner),
  adds an etched **Scroll Wheel** section controlling
  *Direction* (*Standard* vs. *Natural* for physical/virtual mouse wheels,
  independent of touchpad natural scrolling), *Smooth Scrolling* (toggling
  CK Core Motif's ~83 Hz sub-line/pixel Wayland smooth interpolator vs. discrete
  notches), and *Speed* (`1..10`, default `3` lines per notch, persisted in
  `*mouseWheelNatural`, `*mouseWheelSpeed`, and `*mouseWheelSmooth` and applied
  live via `f.set_scroll_wheel` / `_DT_WAYLAND_WHEEL` on Wayland and XInput2 on
  X11), and provides an etched **Touchpad** section controlling
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

## PipeWire audio/mic routing, native Motif Wi-Fi/VPN onboarding, and BlueZ pairing agent (`WP-15`)

`ck-mixer` and `ck-status` share native PipeWire / PulseAudio (`libpulse`),
`NetworkManager`, `BlueZ`, and `MPRIS` desktop controls across both
`CDE (Wayland)` and `CDE (X11 / XLibre)`:

- **PipeWire / PulseAudio audio & microphone routing (`ck-mixer`):** Adds
  **Outputs** (default sink switching, volume, mute, port switching),
  **Input / Mic** (default microphone selection, gain, mute, and a 25-segment
  live LED peak level meter driven by `PA_STREAM_PEAK_DETECT`),
  **Applications** (per-stream playback & capture volume, mute, and live sink /
  source routing), and **Card Profiles** (Bluetooth `a2dp-sink` vs.
  `headset-head-unit` HSP/HFP, HDMI, Pro Audio, Off) alongside **ALSA Hardware**,
  running cleanly even when zero hardware ALSA cards are present.
- **Native Motif Wi-Fi & VPN onboarding + `SecretAgent` (`ck-status`):** Adds
  inline Motif dialogs for WPA2/WPA3-PSK, WPA-Enterprise `802.1X` (`PEAP`/`TTLS`,
  `MSCHAPv2`/`PAP`, identity, CA certificate / system CA, password), Open, and
  **Hidden Wi-Fi** networks (`AddAndActivateConnection`), saved **VPN /
  WireGuard** profile Connect/Disconnect, and asynchronous
  `org.freedesktop.NetworkManager.SecretAgent` (`/org/freedesktop/NetworkManager/SecretAgent`)
  password prompts.
- **Built-in BlueZ pairing agent (`org.bluez.Agent1`) & MPRIS transport
  (`ck-status`):** Registers `/org/ckcore/bluez/agent` (`KeyboardDisplay`) for
  non-blocking 6-digit passkey numeric comparison (`RequestConfirmation`),
  PIN/passkey entry, and service authorization modals alongside **Scan Devices**,
  **Pair**, **Trust / Untrust**, and **Forget** (`RemoveDevice`), plus MPRIS
  (`org.mpris.MediaPlayer2.Player`) track display and **Prev** / **Play-Pause** /
  **Next** controls.

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

## `dtfile` browser ergonomics: tabs, streaming search, clipboard, undo, and folder sync `Apply` (`WP-17`)

`dtfile` and `dtfile_sync` add multi-tab navigation, cancellable streaming
search, standard file clipboard interop, session undo, and reviewed one-way
folder synchronization across both `CDE (Wayland)` and `CDE (X11 / XLibre)` (see
[File Manager improvements](file-manager.md) and
[Compare and synchronize folders](folder-comparison.md)):

- **Multi-tab browsing & navigation shortcuts (`BrowserErgonomics.c`):** Adds
  `Ctrl+T` / `Ctrl+W` / `Ctrl+Tab` / `Ctrl+Shift+Tab`, middle-click folder icon
  and right-click **Open in New Tab**, per-tab Back/Forward (`Alt+Left` /
  `Button8`, `Alt+Right` / `Button9`), `Alt+Up`, `F2` inline rename, and `Ctrl+H`
  hidden-files toggle.
- **Cancellable in-window search (`Ctrl+F` + `ck_fs_search.h`):** Streams
  substring/glob filename and regular-file content matches (with size and
  modified-date filters) into `dtfile` with **Stop**, **Open**, and **Reveal in
  Folder**.
- **Cross-toolkit file clipboard (`Ctrl+C/X/V`), 64-entry Undo (`Ctrl+Z`) &
  `dtfile_sync` `Apply`:** Publishes and consumes `x-special/gnome-copied-files`
  and `text/uri-list` across native Wayland (`zwlr_data_control_v1` /
  `wl_data_device`) and X11 `CLIPBOARD` peers, reverses Rename/Move/Copy/Trash
  via `Ctrl+Z`, and enables **Apply reviewed plan** in `dtfile_sync`
  (`check-dtwm-wayland-wp17.py`).

## Night Light, Adaptive Sync (VRR), laptop lid/idle policy, keyboard tiling, and CUPS/admin tools (`WP-18`)

`cde_power_display.c`, `dtstyle` (*Display & Monitors* and *Startup*), classic
X11 `dtwm`, `dtprintinfo`, `ck-status`, and `ck-admin` provide unified display
ergonomics, laptop power management, keyboard tiling, driverless printing, and
system administration across both `CDE (Wayland)` and `CDE (X11 / XLibre)`:

- **Night Light, Variable Refresh Rate & backlight persistence (`cde_power_display.c`, `dtstyle/Display.c`, `dtwm_media_ctl.c`):**
  `dtstyle` *Monitors* adds a **Night Light & Eye Comfort** section (*Always
  On*, *Night Schedule 20:00–06:00*, *Sunset to Sunrise*, and `2700 K`–`6500 K`
  Planckian color-temperature scale) plus a per-monitor **Variable Refresh Rate
  (VRR)** toggle persisted in `~/.dt/display_ergonomics.conf`. On Wayland,
  `dtwm-wayland` advertises `zwlr_gamma_control_manager_v1`, programs 16-bit
  hardware CRTC gamma LUTs (`wlr_output_state_set_gamma_lut`, with a
  click-through warm scene buffer fallback on zero-gamma headless outputs), and
  applies per-output adaptive sync (`wlr_output_state_set_adaptive_sync_enabled`).
  On X11/XLibre, `dtstyle` and `ck-status` apply 16-bit RGB gamma ramps via
  `XRRSetCrtcGamma` and set RandR `VariableRefresh` / `_DT_VRR_ENABLED` output
  properties (reverting cleanly on **Cancel**). Display brightness changes via
  `dtwm-media-ctl` persist to `~/.dt/backlight.conf` and restore automatically
  at session login.
- **Power profiles, idle auto-suspend & laptop lid clamshell policy (`ck-status`, `cde_power_display.c`):**
  `ck-status` (*Power & Battery* tab) switches `power-saver` / `balanced` /
  `performance` over `org.freedesktop.UPower.PowerProfiles`
  (`net.hadess.PowerProfiles` and `/sys/firmware/acpi/platform_profile`
  fallbacks) and persists separate AC/battery idle auto-suspend timeouts and
  lid-close policy in `~/.config/ck-core/power-policy.conf`. Closing the laptop
  lid (`WLR_SWITCH_TYPE_LID` on Wayland; `org.freedesktop.UPower` `LidIsClosed`
  on X11) disables the internal `eDP`/`LVDS` panel in **Docked Clamshell** mode
  when an external monitor is active without suspending, and invokes
  `org.freedesktop.login1.Manager.Suspend` when undocked on battery.
- **`Super+Arrow` half & quadrant keyboard tiling (`cde_power_display.c`, `WmFunction.c`, `WmEwmh.c`):**
  `Super+Left/Right/Up/Down` (`f.tile_window left|right|up|down`) snaps the
  active window to halves and quadrants of the usable workarea (honoring the
  Front Panel strut), maximizes on repeated `Super+Up`, and restores the saved
  pre-tile geometry on `Super+Down` identically in `dtwm-wayland` and X11
  `dtwm`.
- **CUPS / IPP Everywhere in `dtprintinfo` & `ck-admin` System Administration:**
  `dtprintinfo` (`CupsDiscovery.C`) dynamically loads `libcups.so.2`
  (`cupsGetDests2`, `cupsGetJobs2`, `cupsCancelJob`, plus `--cups-summary`) to
  enumerate driverless IPP Everywhere queues and active print jobs without
  legacy BSD `lpstat` parsing, while `ck-admin` (`CkAdmin` CDE action in
  `System_Admin` and `dtstyle` *Startup*) provides an `XmTabStack` console for
  XDG *Default Applications* (`~/.config/mimeapps.list`), *Date, Time & NTP*
  (`org.freedesktop.timedate1`), and *Firmware Updates*
  (`org.freedesktop.fwupd`).

## Pure XWayland-free native Motif runtime (`--no-xwayland`, `WP-21` – `WP-24`)

When paired with CK Core Motif ABI 9 (`libXm.so.9` built with `--enable-wayland`),
`dtwm-wayland` and `dtsession` can run the complete CDE desktop and CK Core
companion applications **without starting an `Xwayland` server process**:

- **Launch modes:** Pass `--no-xwayland` (`-X` or `DTWM_WAYLAND_NO_XWAYLAND=1`)
  to `dtwm-wayland` or `dtsession --wayland --no-xwayland`. Default `dtwm-wayland`
  invocation keeps rootless `Xwayland` enabled alongside native Wayland so legacy
  third-party X11 applications continue to work unchanged.
- **Native Motif client runtime (`CK_MOTIF_BACKEND=wayland`):** With `DISPLAY`
  unset under `--no-xwayland`, `libXm.so.9` connects each Motif process directly
  to `WAYLAND_DISPLAY` via its in-process X11r11/XRender virtualizer
  (`$XDG_RUNTIME_DIR/ck-wlx11-<pid>-XXXXXX/S:0`), mapping shells to
  `xdg_toplevel` (`zxdg_decoration_v1` + `xdg_toplevel_icon_v1`), menus/tooltips
  to constrained `xdg_popup` surfaces, `CLIPBOARD`/`PRIMARY`/DnD to
  `wl_data_device` + `zwp_primary_selection_v1`, `XmText`/`XmTextField` IME to
  `zwp_text_input_v3`, and HiDPI buffers to `wp_fractional_scale_v1` +
  `wp_viewporter`. Embedded GPU/multimedia viewports (such as `ck-browser` CEF
  off-screen rendering) attach synchronized `wl_subsurface` viewports through
  `<Xm/Wayland.h>` (`XmWaylandGetDisplayHandles`, `XmWaylandGetWidgetSurface`).
- **Native Front Panel docking & session IPC bus:** `dtwm-wayland` docks native
  `dtwm-panel` (`dtwm-front-panel`) and slide-up subpanels (`dtwm-subpanel`)
  directly as `DTWM_VIEW_XDG` surfaces and hosts an `0600` UNIX stream bus at
  `$XDG_RUNTIME_DIR/dtwm-wayland-<WAYLAND_DISPLAY>.ipc` synchronizing
  `RESOURCE_MANAGER` palettes/appearance profiles, `_DT_WM_REQUEST` workspace and
  compositor commands, `_DT_WAYLAND_OUTPUTS` monitor topology,
  `_DT_WORKSPACE_LIST_NAMES` / `_DT_WORKSPACE_CURRENT_NAME` /
  `_DT_WORKSPACE_INFO_RAW_<ws>` workspace backdrop state, and `_DT_SM_*` Session
  Manager state across `dtsession`, `dtstyle` (including all 12 Style Manager
  dialogs, live embedded `Color & Theme` widget preview, `Backdrop`, `Font`,
  `Monitors`, `Window`, `Startup`, and `DtHelp`), `dtwm-panel`, `dtfile`, `dtpad`,
  `ck-clock`, `ck-coins`, and `ck-browser`.

## Readiness reporting, Ubuntu LTS APT packaging, and automated qualification

Passing `--status-file PATH` writes `WAYLAND_DISPLAY`, `DISPLAY` (or
`NO_XWAYLAND=1` when `--no-xwayland` is active), and `PID` atomically once the
compositor session is ready.

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
  `check-dtwm-wayland-wp15.py` (`WP-15` parallel dual Wayland + X11 `Xvfb`
  verification of `ck-mixer` PipeWire sink/source/stream/profile routing +
  zero-ALSA fallback + live mic peak meter, and `ck-status` WPA2-PSK /
  WPA-Enterprise `802.1X` onboarding, VPN `SecretAgent`, BlueZ `Agent1` pairing,
  and MPRIS transport),
  `check-dtwm-wayland-wp16.py` (`WP-16` parallel dual Wayland + X11 `Xvfb`
  verification of `dtfile` `libXm.so.9` `UDisks2` LUKS unlock/hotplug/eject,
  `GVFS` *Connect to Server* + MTP phone `inotify` discovery, 5-format archive
  round-trips + Zip-Slip rejection, FreeDesktop thumbnail resolution,
  `org.freedesktop.FileManager1` window raise + selection, and live
  bidirectional `inotify` XDG + `.Trash-$UID` Trash),
  `check-dtwm-wayland-wp17.py` (`WP-17` `dtfile` tabs, cancellable search,
  clipboard, undo, and Folder Sync execution),
  `check-dtwm-wayland-wp18.py` (`WP-18` parallel dual Wayland + X11 `Xvfb`
  verification of `dtstyle` `3400 K` Night Light gamma/warm scene tint + VRR +
  `backlight.conf` persistence, `Super+Arrow` half/quadrant/restore window
  tiling honoring Front Panel struts, `ck-status` PowerProfiles + docked
  clamshell vs. battery lid suspend + idle auto-suspend, `dtprintinfo` CUPS IPP
  Everywhere queue/job discovery, and `ck-admin` `mimeapps.list` / `timedate1` /
  `fwupd` + `-session` restore),
  `check-dtwm-wayland-wp19.py` (`WP-19` parallel dual Wayland + X11 `Xvfb`
  verification of actionable `ck-tasks` process `SIGKILL`/`renice`/live network
  & per-core charts/`systemd1` D-Bus control, `ck-view` PNG/JPEG/WebP/SVG/TIFF/XPM
  rendering + rotate/crop + `DtWsmChangeBackdrop` + `-session` restore, `ck-pdf`
  outline/search/clipboard/CUPS print + `-session` restore, `dtpad` tabs/line
  numbers/crash-recovery autosave, and `dtcm` RFC 5545 `.ics` round-trips),
  `check-dtwm-wayland-wp20.py` (`WP-20` parallel dual Wayland + X11 `Xvfb`
  verification of `AccessX` Sticky/Slow/Bounce keys, visual bell, cursor-tracking
  magnifier, and `libXm.so.9` AT-SPI2 D-Bus bridge),
  `check-dtwm-wayland-wp24-noxwayland.py` (`WP-24` Track 2 pure XWayland-free
  `dtwm-wayland --no-xwayland` session with zero `Xwayland` processes, docked
  native `dtwm-panel`, `dtstyle`, `dtfile`, `dtpad`, `ck-clock`, and `ck-coins`
  mapping `libXm.so.9`, live `.ipc` workspace and `modern_dark` `RESOURCE_MANAGER`
  broadcasts, plus parallel hybrid Wayland+XWayland verification),
  `check-dtwm-wayland-menus.py` (XWayland Motif pulldown/popup menu focus,
  mid-press `zwlr_virtual_pointer_v1` crash cleanup, `wayvnc` stuck-button
  auto-heal, stationary-click `wl_pointer` focus restoration, and post-modeset
  HiDPI two-click + press-drag-release menu activation),
  `check-dtwm-wayland-busy-light.py` (Front Panel `CONTROL Busy` `Fplite` /
  `FpliteY` blink activation and automatic clear on client map),
  `check-dtappintegrate-curated.py` (Application Manager installed vs.
  auto-gray + blue download arrow badge vs. hidden stub curation), and
  `check-dtwm-wayland-exit-session.py` (`f.quit_mwm`, external-compositor
  `dtsession --wayland` logout, and degraded/standalone `XSession_Exit`
  fallback) run automatically under `make check`.
- **Native Wayland Help Viewer (`dthelpview`) & Information Manager (`dtinfo`):**
  `dtwm-wayland --no-xwayland` exports `WAYLAND_DISPLAY` before launching the
  session `ttsession -p` daemon and publishes `_SUN_TT_SESSION` / `TT_SESSION` as
  `XA_STRING` onto each `libXm.so.9` virtual root window so Front Panel ToolTalk
  actions (`CONTROL InfoManager` → `DtLoadInfoLib` → `dtinfo_start` → `dtinfo`,
  plus the *Information* subpanel's `dthelpview` launchers) open directly
  without XWayland. `libDtHelp` (`_DtHelpXmFontListGetPropertyMax`,
  `__DtHelpFontDatabaseInit`) natively supports `XmFONT_IS_XFT` render-table
  entries alongside core XLFD fonts, `dtinfo` resolves Motif/CDE cursor bitmaps
  with standard cursor fallbacks and null-safe inline graphic type detection,
  and `libXm`'s `font_buckets` XLFD cache keeps large interactive window resizes
  responsive across dense help topics and book chapters.
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
