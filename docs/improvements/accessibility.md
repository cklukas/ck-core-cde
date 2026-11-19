---
title: Desktop and toolkit accessibility
description: Keyboard Accessibility (AccessX), Visual Bell, Reduced Motion, cursor-tracking Screen Magnification, and opt-in Motif AT-SPI2 screen-reader integration across CDE Wayland and X11.
---

# Desktop and toolkit accessibility

CK Core CDE provides unified keyboard, visual, magnification, and assistive-technology accessibility across both **`CDE (Wayland)`** (`dtwm-wayland`) and **`CDE (X11 / XLibre)`** (`dtwm`), configured from **Style Manager → Accessibility** (`dtstyle -accessibility`) and persisted in `~/.dt/accessibility.conf`.

## Style Manager Accessibility dialog (`dtstyle`)

Opening **Accessibility** in `dtstyle` (or running `dtstyle -accessibility` / `dtstyle -apply-accessibility <key=value,...>`) manages four groups of session-wide settings:

| Category | Setting | `~/.dt/accessibility.conf` key | Behavior |
| --- | --- | --- | --- |
| **Keyboard (`AccessX`)** | Sticky Keys | `sticky_keys=0\|1` | Single-tapping `Shift`, `Ctrl`, `Alt`, or `Super` latches that modifier for the next non-modifier keystroke; double-tapping locks it; tapping a third time unlocks it. |
| **Keyboard (`AccessX`)** | Two keys turn Sticky Keys off | `sticky_two_keys_off=0\|1` | Automatically disables Sticky Keys when two keys are held simultaneously as a chord. |
| **Keyboard (`AccessX`)** | Slow Keys & Delay (`100`–`1000 ms`) | `slow_keys=0\|1`, `slow_keys_ms=300` | Requires non-modifier keys (Wayland) or keys (X11 XKB) to remain depressed for at least the acceptance delay before delivering a key press; early release cancels the press. |
| **Keyboard (`AccessX`)** | Bounce Keys & Interval (`100`–`1000 ms`) | `bounce_keys=0\|1`, `bounce_keys_ms=300` | Ignores duplicate presses of the same key arriving sooner than the debounce interval after release. |
| **Visual Alerts & Motion** | Visual Bell (*Off*, *Flash Active Window*, *Flash Screen*) | `visual_bell=off\|window\|screen` | Flashes a `120 ms` high-contrast overlay across the focused window frame or active output whenever an X11 `XkbBell` / `XBell`, Wayland `xdg_system_bell_v1.ring`, or `_DT_WM_REQUEST` `f.visual_bell` occurs. |
| **Visual Alerts & Motion** | Reduced Motion | `reduced_motion=0\|1` | Disables wireframe zoom animations during window minimize and restore in both `dtwm-wayland` and X11 `dtwm`. |
| **Screen Magnifier** | Enable Full-Screen Magnifier & Zoom (`125%`–`400%`) | `magnifier=0\|1`, `zoom_pct=200` | Enables proportional cursor-aligned full-screen magnification in `25%` increments (`Super` or `Ctrl+Alt` + `=`/`+`, `-`, `0`, and `Scroll` on Wayland; `Super` + `=`/`-`/`0` on X11). |
| **Screen Reader** | Motif AT-SPI2 Bridge (`XM_ACCESSIBILITY=1`) | `atspi_bridge=0\|1` | Exports `XM_ACCESSIBILITY=1` at session login (`Xsession.wayland` and `dtsession`), updates `*enableAccessibility` in `RESOURCE_MANAGER`, and synchronizes `dbus-update-activation-environment` and `org.a11y.Status.IsEnabled`. |

## Dual-backend implementation

- **Wayland (`dtwm-wayland` — `cde_accessibility.c`):**
  - Implements real-time compositor keyboard filtering (`dtwm_a11y_filter_key`) before forwarding keystrokes or synthesized `wlr_keyboard_modifiers` masks to native Wayland and `Xwayland` clients.
  - Subscribes to `Xwayland` `XKEYBOARD` `XkbBellNotify` events directly over XCB alongside native Wayland `xdg_system_bell_v1` requests.
  - Renders magnified frames (`zoom_pct > 100`) by building the unmagnified `wlr_scene_output` into a per-output off-screen `wlr_swapchain` (`wlr_output_configure_primary_swapchain`) and blitting a proportionally panned viewport (`src_x = u * (bw - src_w)`, `src_y = v * (bh - src_h)`) into the output's primary swapchain via `wlr_output_begin_render_pass`, keeping the magnified UI pixel under the hardware/host cursor aligned 1:1 with pointer hit-testing across the entire screen while avoiding scene-graph feedback loops.
- **X11 / XLibre (`dtwm` — `WmEwmh.c` + `ck-magnifier`):**
  - Programs server-level XKB `AccessX` controls (`XkbStickyKeysMask`, `XkbSlowKeysMask`, `XkbBounceKeysMask`, `XkbAX_LatchToLockMask`, `XkbAX_TwoKeysMask`, `slow_keys_delay`, `debounce_delay`) via `XkbSetControls` and listens for `XkbBellNotify`, `XkbStateNotify`, and `XkbControlsNotify`.
  - Drives full-screen X11 magnification through `ck-magnifier` (`XComposite` off-screen window pixmap composition with click-through `XFixes` input shape regions), bound to `Super+=`, `Super+-`, and `Super+0`.

Both window managers publish live state on the root window property `_DT_ACCESSIBILITY_STATE`:

```text
sticky=<0|1> latched=<mask> locked=<mask> slow=<0|1> slow_ms=<int> bounce=<0|1> bounce_ms=<int> vbell=<off|window|screen> vbell_count=<int> reduced_motion=<0|1> zoom_pct=<100..400>
```

## Motif AT-SPI2 auto-bridge (`XM_ACCESSIBILITY=1`)

When built against CK Core Motif ABI 9 (`--enable-atspi`) and launched with `XM_ACCESSIBILITY=1` (or `*enableAccessibility: True`), unmodified CDE and `ck-core-apps` Motif binaries automatically register one per-`XtAppContext` `org.a11y.atspi` D-Bus tree on `VendorShell` realization without requiring per-application `main()` boilerplate. Custom composite widgets such as `CkTable` (`ck-tasks`) attach semantic `Table`, `Selection`, and `Action` child nodes through `XmAccessibilityGetDefault(widget)`.

## Automated verification

`cde/programs/dtwm/tests/check-dtwm-wayland-wp20.py` runs headless `dtwm-wayland` and `Xvfb` + X11 `dtwm` sessions in parallel, verifying `/proc/<pid>/maps` `libXm.so.9` provenance, Sticky/Slow/Bounce key filtering into a live Motif `XmTextField`, Visual Bell pixel flashes, `100% → 200% → 100%` magnified pixel bounding-box scaling, and live `org.a11y.atspi` D-Bus inspection and `Action.DoAction(0)` / `Selection.SelectChild(0)` dispatch against `dtstyle` and `ck-tasks`.
