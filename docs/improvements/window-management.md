---
title: Window management
description: Task switching, snap feedback and familiar window movement in dtwm.
---

# Window management

CK Core CDE extends dtwm without replacing its workspace, icon or session
model.

## Task switching

The keyboard task switcher presents the available windows, keeps the current
selection visible and supports pointer hover and pinning. It is enabled by the
`Dtwm*showTaskSwitcher` resource.

![dtwm task switcher showing Terminal, File Manager, Style Manager and Front Panel](../images/features/window-management.png)

*The keyboard task switcher running in dtwm on uConsole.*

## Movement and focus

- snap feedback previews half-screen and quadrant placement while moving;
- holding Shift aligns movement and resize to the desktop grid;
- double-clicking a title bar maximizes or restores the window;
- clicking the desktop clears the active-window focus; and
- **Style Manager → Window** configures **Minimize / Restore Animation**
  (`Dtwm*minimizeAnimation`) and **Maximize / Restore Animation**
  (`Dtwm*maximizeAnimation`) dynamically for the active backend — offering
  `None`, `Wireframe` (minimize default), and `Titlebar` on X11 `dtwm` (maximize
  default `None`), and `None`, `Bevel Morph` / `Bevel Glide` (default),
  `Titlebar Shuttle`, and `Wireframe` on [Wayland (`dtwm-wayland`)](wayland.md).

Open-window desktop icons complement these controls by keeping running
applications represented on their workspace.
