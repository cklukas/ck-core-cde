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

- **Built-in palettes (`ModernLight.dp`, `ModernDark.dp`, `NordicNight.dp`, `ObsidianTeal.dp`, `EspressoDark.dp`, `HighContrast.dp`)**:
  `cde/programs/palettes/` ships curated 8-colorset desktop palettes aligned with
  the Motif appearance profiles so the CDE Front Panel, workspace switch buttons,
  workspace backdrops, and terminal canvas match the active widget theme with
  distinct, non-monotone surface and workspace slots.
- **`dtsession` Color Server (`SrvPalette.c`, `SrvFile_io.c`)**:
  When `*appearanceProfile` is set to `modern_light`, `modern_dark`, or
  `high_contrast`, `dtsession` synchronizes `ColorPalette`, `*appearanceProfile`,
  and `*appearanceDensity` in `RESOURCE_MANAGER` while `libXm` harmonizes
  `XmColorObj` dialog, menu, Front Panel, and canvas-well surfaces and preserves
  raw `XmColorObj` pixels for drift-free restoration to `classic`.
- **`dtwm` window frames and Front Panel (`WmCDecor.c`, `WmResource.c`, `UI.c`, `Control.c`, `Clock.c`)**:
  When a modern or high-contrast appearance profile is active, `dtwm` and
  `dtwm-panel` update window frames, workspace backdrops, `DtControlGadget` /
  `DtClockGadget` Front Panel icons, and workspace switch buttons live from the
  Color Server palette and active Motif appearance profile.
- **`dtstyle` Color & Theme dialog and embedded preview (`ColorMain.c`, `ColorPreviewHelper.c`, `ColorFile.c`)**:
  **Style Manager - Color & Theme** combines palette selection with **Widget
  Style** (`Classic CDE (3D Bevel)`, `Modern Motif (Flat & Rounded)`, `High
  Contrast (Accessible)`) and **Layout Spacing** (`Comfortable`, `Compact`),
  automatically choosing `modern_light` vs. `modern_dark` from palette luminance,
  embedding a live Motif **Sample Dialog** (`dtstyle --preview-helper`) inside
  authentic window decorations in the preview canvas, applying desktop-wide
  updates on **Apply** / **OK**, and reverting cleanly on **Cancel**.
- **`DtHelp` live palette and profile redraw (`Callbacks.c`, `SetList.c`, `PathArea.c`)**:
  `DtHelpDialog` and `DtHelpQuickDialog` synchronize `dispWid` foreground,
  background, highlight color, GCs, and border thickness across live palette and
  appearance-profile updates and reconcile child display-area resizes when
  scrollbar or shadow metrics change inside an unchanged parent form, keeping
  both the TOC pane and help canvas rendered without manual window resizing.

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
