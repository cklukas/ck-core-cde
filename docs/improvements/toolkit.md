---
title: CK Core Motif foundation
description: Why CK Core CDE and CK Core Motif are built and tested as one native stack.
---

# CK Core Motif foundation

CK Core CDE selects the latest published CK Core Motif `master` at the start of
each ten-round repair batch and requires ABI 9. That revision stays fixed through
the batch's validation and publication; subsequent toolkit updates are adopted
in the next batch. The desktop uses Motif widget internals, so CDE and the toolkit are treated
as one tested native stack rather than independent system packages.

## What the build enforces

- headers and libraries come from the same isolated Motif prefix;
- the ABI 9 header constant and `XmToolkitABI` runtime symbol are present;
- Xm, Mrm, Uil, the UIL compiler and CDE form one matched stack; and
- CI verifies that `dtwm` links to `libXm.so.9` from the selected prefix.

This keeps toolkit work—rendering, accessibility, widgets and platform fixes—
available to the desktop without accepting an unverified system Motif library.

## Motif appearance profiles and desktop palettes

CK Core Motif ABI 9 provides four built-in appearance profiles (`classic`,
`modern_light`, `modern_dark`, and `high_contrast`) and two layout densities
(`comfortable` and `compact`). CK Core CDE integrates these profiles across the
session manager, window manager, and Style Manager while keeping `classic` as the
default:

- **Built-in palettes (`ModernLight.dp`, `ModernDark.dp`, `HighContrast.dp`)**:
  `cde/programs/palettes/` ships three 8-colorset desktop palettes aligned with
  the Motif appearance profiles so the CDE Front Panel, workspace switch buttons,
  workspace backdrops, and terminal canvas match the active widget theme.
- **`dtsession` Color Server (`SrvPalette.c`, `SrvFile_io.c`)**:
  When `*appearanceProfile` is set to `modern_light`, `modern_dark`, or
  `high_contrast` (or when `ModernLight.dp`, `ModernDark.dp`, or
  `HighContrast.dp` is selected as the screen's `ColorPalette`), `dtsession`
  automatically synchronizes `ColorPalette` and `*appearanceProfile` in
  `RESOURCE_MANAGER`. To preserve `libXm`'s per-widget modern surface and accent
  colors, `dtsession` scopes its Xrdb text colors to `*DtTerm*background` and
  `*DtTerm*foreground` instead of emitting wildcard `*background` and
  `*foreground` strings when a modern profile is active.
- **`dtwm` window frames and Front Panel (`WmCDecor.c`, `WmResource.c`, `UI.c`)**:
  When a modern appearance profile is active, `dtwm` preserves CDE's classic 3D
  beveled window frames and titlebar controls while applying the active Color
  Server palette and anti-aliased Xft render tables to window titlebars, desktop
  icon labels, and the Front Panel workspace switcher.
- **`dtstyle` live preview and persistence (`ColorMain.c`, `ColorFile.c`)**:
  Selecting `ModernLight`, `ModernDark`, `HighContrast`, or any classic palette
  in the Style Manager Color dialog previews the corresponding Motif appearance
  profile immediately via `XmApplyAppearanceProfile()` and persists both the
  palette and `*appearanceProfile` when confirmed.

You can activate a profile either from **Style Manager -> Color** (by selecting
`ModernLight`, `ModernDark`, or `HighContrast`) or in `~/.Xdefaults`:

```ini
*appearanceProfile: modern_dark
*appearanceDensity: comfortable
```

See the [desktop screenshot gallery](../desktop.md) for side-by-side tabbed
captures of the default session, Style Manager, Terminal, and File Manager
across all four profiles, and [Build CK Core CDE](../build.md) for the source
workflow.

## Desktop version and Help / About attribution

CK Core CDE identifies the modernized C23 + Motif ABI 9 + Wayland/X11 desktop
release line as **3.0.0** (`DtVERSION = 3`, `DtREVISION = 0`,
`DtUPDATE_LEVEL = 0`, `DtVERSION_NUMBER = 30000`, and
`DtVERSION_STRING = "CK-Core CDE Version 3.0.0"` in `<Dt/Dt.h>` and `libDtSvc`).
All SDL Help **Version** / **About** topics (`CDEcopyright` in
`cde/doc/common/help/HELPEnt.sgm.in` and `Help4Help.sgm`), `dtcm`'s About
dialog, and `dtlogin`'s Copyright notice display **CK-Core Common Desktop
Environment 3.0.0**, Dr. Christian Klukas's copyright, and the project URLs
(`https://github.com/cklukas/ck-core-cde` and
`https://cklukas.github.io/ck-core-cde/`) alongside the upstream Open Group,
CDE Project, and GNU LGPL notices.
