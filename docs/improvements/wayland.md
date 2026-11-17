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
  maximize/restore;
- Holding `Shift` during pointer move or resize aligns geometry to the 16-pixel
  desktop grid;
- Dragging toward output edges or corners previews half-screen and quadrant snap
  placement and commits on release;
- Pressing `Alt+Tab` opens and cycles the centered 3D-beveled keyboard task
  switcher, and `Alt+Space` opens the window menu;
- Clicking the desktop background clears active-window focus.

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

- **Open-window and minimized desktop icons (`Dtwm*showOpenWindowIcons`):**
  Publishes `_DT_WM_SETTINGS_V1` (`DT_WM_SETTINGS_V1_SHOW_OPEN_WINDOW_ICONS`) on
  both the workspace-manager window and the XWayland root window, renders
  active-focus Crimson and inactive Slate 3D bevel icon tiles in
  `desktop_icon_tree`, and restores or focuses windows when their desktop icon
  is clicked (`--no-open-window-icons` disables open-window icons);
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
- **`dtfile` pinned desktop icons and Front Panel `TYPE client` embedding:**
  Places `dtfile` `Desktop.c` workspace objects in `desktop_icon_tree` below
  normal application windows without duplicate open-window icons, and embeds
  `.fp` `TYPE client` controls (such as `CLIENT_NAME ck-clock` in
  `~/.dt/types/ckclock.fp`) directly inside `dtwm-panel` via `XReparentWindow`
  (with `--swallow NAME` retained as an explicit compositor override).

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
- **`ck-grab` Wayland screencopy (`zwlr_screencopy_manager_v1`):**
  `dtwm-wayland` publishes `_DT_ACTIVE_WINDOW_BOX` (`outer_x`, `outer_y`,
  `outer_w`, `outer_h`, `client_x`, `client_y`, `client_w`, `client_h`) on the
  XWayland root window whenever focus or geometry changes so `ck-grab` captures
  active native Wayland windows (with or without their server-side CDE 3D bevel
  frames), arbitrary desktop regions, and stitched multi-output full-screen
  desktops;
- **Per-output XLibre RandR `DPI` and `wp_fractional_scale_v1`:**
  `dtwm-wayland` opts rootless `Xwayland` into CK Core Motif's per-output RandR
  observer via `_MOTIF_RANDR_OUTPUT_DPI` (`CARDINAL = 1`) and publishes each
  output's `DPI` (`INTEGER`, format 32, configurable at startup with
  `--output-dpi INDEX=DPI` or live via `_DT_WM_REQUEST`
  `f.set_output_dpi INDEX DPI`) alongside `wp_fractional_scale_v1` for native
  Wayland clients, triggering crisp live Motif shell reflow when windows move
  across mixed-DPI monitors (`96`, `144`, `192`, ...).

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

## Readiness reporting and automated verification

Passing `--status-file PATH` writes `WAYLAND_DISPLAY`, `DISPLAY`, and `PID`
atomically once both the Wayland socket and rootless `Xwayland` are ready.
`programs/dtwm/tests/check-dtwm-wayland.py` boots a dual-output headless
`dtwm-wayland` instance, maps both a native Wayland client and an X11 client,
and verifies active Crimson bevels, inactive Slate bevels, desktop-click
defocus, and refocusing via `zwlr_screencopy_manager_v1` captures.
