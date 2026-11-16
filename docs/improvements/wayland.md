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
  `BevelRectangle` geometry, using active CDE Crimson and inactive CDE Slate
  color sets;
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
- Docked interactive 3D-beveled CDE Front Panel on the primary output with
  clickable 2x2 workspace buttons, `Ctrl+Alt+1..8` and `Ctrl+Alt+Left/Right`
  shortcuts, and bottom strut reservation (`DTWM_PANEL_STRUT`) respected by
  maximized and edge-snapped windows (`--no-panel` and `--greeter` hide the
  panel when needed).

## Readiness reporting and automated verification

Passing `--status-file PATH` writes `WAYLAND_DISPLAY`, `DISPLAY`, and `PID`
atomically once both the Wayland socket and rootless `Xwayland` are ready.
`programs/dtwm/tests/check-dtwm-wayland.py` boots a dual-output headless
`dtwm-wayland` instance, maps both a native Wayland client and an X11 client,
and verifies active Crimson bevels, inactive Slate bevels, desktop-click
defocus, and refocusing via `zwlr_screencopy_manager_v1` captures.
