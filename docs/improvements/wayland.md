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

## Readiness reporting and automated verification

Passing `--status-file PATH` writes `WAYLAND_DISPLAY`, `DISPLAY`, and `PID`
atomically once both the Wayland socket and rootless `Xwayland` are ready.
`programs/dtwm/tests/check-dtwm-wayland.py` boots a dual-output headless
`dtwm-wayland` instance, maps both a native Wayland client and an X11 client,
and verifies active Crimson bevels, inactive Slate bevels, desktop-click
defocus, and refocusing via `zwlr_screencopy_manager_v1` captures.
