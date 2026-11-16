---
title: Desktop Screenshots
description: Actual screenshots from a live CK Core CDE session across Classic, Modern Light, Modern Dark, and High Contrast Motif appearance profiles.
---

# Desktop Screenshots

These screenshots come from a real CK Core CDE session launched with
`/usr/dt/bin/Xsession` on ARM64 Linux (`1280x900`, 24-bit color) against the
matched CK Core Motif ABI 9 (`libXm.so.9`) stack. Each scene is captured across
all four built-in Motif appearance profiles (`Classic`, `Modern Light`,
`Modern Dark`, and `High Contrast`) so the classic CDE baseline and the modern
vector/Xft appearance profiles can be compared side by side.

## Desktop and Front Panel

The default session starts `dtsession` and `dtwm`, displaying the CDE workspace
backdrop and the Front Panel with its workspace switcher, clock, mailer,
terminal, file manager, text editor, and style manager controls. In the modern
appearance profiles, `dtsession` selects the matching desktop palette
(`Default.dp` for `modern_light`, `ModernDark.dp` for `modern_dark`, or
`HighContrast.dp` for `high_contrast`) and `dtwm` renders the Front Panel
workspace switcher with Xft typography.

::: tabs
== Classic
![CK Core CDE default desktop and Front Panel (Classic)](images/default-desktop.png)
== Modern Light
![CK Core CDE default desktop and Front Panel (Modern Light)](images/default-desktop-modern-light.png)
== Modern Dark
![CK Core CDE default desktop and Front Panel (Modern Dark)](images/default-desktop-modern-dark.png)
== High Contrast
![CK Core CDE default desktop and Front Panel (High Contrast)](images/default-desktop-high-contrast.png)
:::

## Style Manager

`dtstyle` opens from the session and manages desktop color palettes, fonts,
backdrops, keyboard, mouse, beep, screen, window, and startup settings. Selecting
`ModernLight`, `ModernDark`, `HighContrast`, or any classic palette in the Color
dialog previews the corresponding Motif appearance profile live via
`XmApplyAppearanceProfile()` and persists both `ColorPalette` and
`*appearanceProfile` in `RESOURCE_MANAGER`.

::: tabs
== Classic
![CK Core CDE Style Manager in the running session (Classic)](images/style-manager.png)
== Modern Light
![CK Core CDE Style Manager in the running session (Modern Light)](images/style-manager-modern-light.png)
== Modern Dark
![CK Core CDE Style Manager in the running session (Modern Dark)](images/style-manager-modern-dark.png)
== High Contrast
![CK Core CDE Style Manager in the running session (High Contrast)](images/style-manager-high-contrast.png)
:::

## Terminal

`dtterm` runs inside the session, accepts keyboard input, and executes a shell
command in the user's home directory before this capture is taken. Under modern
appearance profiles, `dtwm` preserves CDE's classic 3D beveled window
decorations while rendering anti-aliased Xft titlebar and icon labels, and
`dtsession` scopes the palette's text colorset to `*DtTerm*background` and
`*DtTerm*foreground`.

::: tabs
== Classic
![CK Core CDE Terminal Emulator running a command in the session (Classic)](images/terminal.png)
== Modern Light
![CK Core CDE Terminal Emulator running a command in the session (Modern Light)](images/terminal-modern-light.png)
== Modern Dark
![CK Core CDE Terminal Emulator running a command in the session (Modern Dark)](images/terminal-modern-dark.png)
== High Contrast
![CK Core CDE Terminal Emulator running a command in the session (High Contrast)](images/terminal-high-contrast.png)
:::

## File Manager

`dtfile` opens `/usr/dt` inside the same session, showing the installed CDE
tree with menu bar, path header, icon view, and status line styled by the active
appearance profile.

::: tabs
== Classic
![CK Core CDE File Manager viewing /usr/dt (Classic)](images/file-manager.png)
== Modern Light
![CK Core CDE File Manager viewing /usr/dt (Modern Light)](images/file-manager-modern-light.png)
== Modern Dark
![CK Core CDE File Manager viewing /usr/dt (Modern Dark)](images/file-manager-modern-dark.png)
== High Contrast
![CK Core CDE File Manager viewing /usr/dt (High Contrast)](images/file-manager-high-contrast.png)
:::

## Capture Provenance

The screenshots are captured from a real `/usr/dt/bin/Xsession` session across
all four profiles (`classic`, `modern_light`, `modern_dark`, `high_contrast`)
from:

- CK Core Motif revision
  [`d85fb6604a827a7ebfcdcadcbac4525dccc684d7`](images/motif-revision.txt)
- `libXm.so.9.0.0` SHA-256 recorded in [`motif.sha256`](images/motif.sha256)
- `/proc/<pid>/exe` and `/proc/<pid>/maps` verification recorded in
  [`smoke-test-processes.txt`](images/smoke-test-processes.txt) for
  `dtsession`, `dtwm`, `dtstyle`, `dtterm`, and `dtfile`
