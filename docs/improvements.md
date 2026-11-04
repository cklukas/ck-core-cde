---
title: What CK Core CDE improves
description: The maintained toolkit integration, desktop behavior and application usability that distinguish CK Core CDE.
---

# What CK Core CDE improves

The upstream CDE project remains the portability baseline. CK Core CDE builds
on it with a maintained Motif foundation, targeted desktop and application
improvements, and a reproducible validation path. Choose CK Core CDE when these
integrated changes matter more than staying on the upstream feature set.

## At a glance

| Area | CK Core CDE improvement | Practical result |
| --- | --- | --- |
| [Toolkit](improvements/toolkit.md) | Builds and tests CDE with the latest CK Core Motif ABI 9 as one matched native stack. | Desktop and toolkit improvements arrive together. |
| [Window state](improvements/window-state.md) | Optional desktop icons represent open windows, distinguish active state and repaint client-supplied live icon pixmaps. | Open windows, keyboard focus and compact application state remain visible at a glance. |
| [Window management](improvements/window-management.md) | Task switching, snap feedback, open-window icons, Shift grid movement, desktop-click defocus and title-bar double-click maximize are integrated in dtwm. | Everyday window navigation gains familiar, visible interactions without replacing the CDE window manager. |
| [Terminal](improvements/terminal.md) | Filename drops insert shell-safe escaped paths, and the selected font size persists for later terminals and clones. | Dragging files into a shell and keeping a readable terminal setup both work predictably. |
| [Editor](improvements/editor.md) | Dropped files support automatic, path-list or content handling, with the choice persisted and propagated through ToolTalk. | A drop can match the editing workflow instead of having one hard-coded meaning. |
| [File Manager](improvements/file-manager.md) | Adds a shelf, image-content icons, extra-large views, storage details and optional read-only folder comparison with saved preferences. | Browse files and inspect folder differences; existing Copy/Move commands remain available. |
| [Subprocess control](improvements/subprocess-control.md) | `dtspcd` and the SPC protocol validate headers, payload limits, request types and counted strings, name authentication files unpredictably and stop if privileges cannot be dropped. | Remote command execution accepts only well-formed requests from a user who proved their identity. |
| [Calendar server](improvements/calendar-server.md) | `rpc.cmsd` validates request counts, attribute indexes and calendar names, escapes every string it logs and stops if it cannot keep its privileges apart. | Calendar requests from the network cannot overrun buffers or select files outside the calendar directory. |
| [Login manager](improvements/login-manager.md) | `dtlogin` validates XDMCP requests, draws authorization cookies only from `/dev/urandom`, runs `xrdb` without a shell and stops on a failed privilege drop. | The root login service accepts only well-formed network requests and keeps its privileges apart from the session's. |
| [ToolTalk](improvements/tooltalk.md) | `ttsession` decodes requests within their bounds, applies its authentication level to every request, draws the session cookie from `/dev/urandom` and limits the variables a message can set for a program it starts; `rpc.ttdbserverd` checks the descriptors, records and paths of its requests. | A session server that listens on the network cannot be crashed by a malformed request, and a message cannot choose what the programs it starts run. |
| [DtSearch](improvements/dtsearch.md) | Databases store addresses and timestamps in 8 bytes whatever the build's feature macros, the maintenance tools no longer crash at start, and a test indexes a corpus and queries it through the DtSrAPI. | A database means the same to the tools, the library and its C++ users, and a database of another layout is refused instead of misread. |
| [Session manager](improvements/session-manager.md) | `dtsession` gives up its installed identity before it reads the environment and stops when that fails. | A session manager installed setuid root cannot be left running as root by a refused system call. |
| [Session manager](improvements/session-manager.md) | `dtsession` gives up its installed identity before it reads the environment and stops when that fails, checks passwords without trusting `crypt()`, handles session directories without a shell, reads saved sessions and palettes within their buffers and keeps XSMP clients from crashing it. | A session manager installed setuid root cannot be left running as root, and a damaged session file, a restored client or a stray `SIGTERM` no longer takes the session down. |
| [Session manager](improvements/session-manager.md) | `dtsession` gives up its installed identity before it reads the environment and stops when that fails, checks passwords without trusting `crypt()`, handles signals, session directories and saves safely, starts clients without leaking their environment, reads saved sessions and palettes within their buffers and keeps XSMP clients from crashing it. | A session manager installed setuid root cannot be left running as root, and a damaged session file, a restored client, a failed save or a stray `SIGTERM` no longer takes the session down. |
| [Session manager](improvements/session-manager.md) | `dtsession` gives up its installed identity before it reads the environment and stops when that fails, unlocks the screen only with the starting user's password, checked through PAM, handles signals, session directories and saves safely, starts clients without leaking their environment, reads saved sessions and palettes within their buffers and keeps XSMP clients from crashing it. | A session manager installed setuid root cannot be left running as root, and a damaged session file, a restored client, a failed save or a stray `SIGTERM` no longer takes the session down. |
| [Launching](improvements/launching.md) | Distinguishes direct GUI launch from the interactive `Run...` dialog and lets that dialog choose whether to use a Terminal. | Starting a graphical application no longer requires an unnecessary terminal wrapper. |
| [Trash](improvements/trash.md) | Improves trash state and full-icon handling and can raise and focus the Trash window. | The desktop communicates trash state and returns to the relevant window more reliably. |
| [Validation](improvements/validation.md) | ARM64 CI builds the current Motif dependency, compiles CDE, runs `make check`, verifies the ABI link and launches the desktop smoke test. | A successful result covers the native stack and a real CDE session, not just compilation. |

## A cohesive native stack

CDE subclasses Motif widgets and relies on private widget records. CK Core CDE
therefore treats the desktop and CK Core Motif as one versioned native cohort,
rather than accepting whichever system `libXm` happens to be installed. The
[build guide](build.md) explains the isolated prefix and the checks that enforce
this boundary.

## Desktop changes you can see

Open-window icons are a CK Core CDE desktop extension. They keep running
applications represented on the desktop and distinguish the focused window
while retaining the classic CDE icon model.

The separate [CK Core Apps](https://cklukas.github.io/ck-core-apps/) companion
suite demonstrates the same desktop contract with high-resolution live icons
for time, system load, pointer tracking, browser favicons, market data and an
animated plasma demo.

dtwm also carries a task switcher with keyboard focus handling, window snap
feedback, workspace-aware open-window icons, Shift-modified grid movement and
resize, desktop-click focus clearing, and title-bar double-click
maximize/restore. These changes extend CDE's own window-management model rather
than introducing a second desktop shell.

The [desktop gallery](desktop.md) shows the actual default session, Terminal,
Style Manager and File Manager from the uConsole smoke run.

## Improvements in everyday applications

- **Terminal:** safely escaped filename drops and persistent font-size choices.
- **Editor:** selectable dropped-file behavior with persisted configuration.
- **File Manager:** a shelf, image-content icons, extra-large views, storage
  information, IconicPath interaction, improved Trash handling and support for
  filenames containing quotes or backslashes; optional
  [read-only folder comparison](improvements/folder-comparison.md) adds filters,
  matching-rule review and saved preferences.
- **Run actions:** direct graphical launch alongside the configurable
  Terminal-backed `Run...` workflow.

These are small interactions individually, but they are the parts of a desktop
that users touch repeatedly.

## Evidence, not screenshots alone

The ARM64 smoke workflow records the executable and Motif build behind each
capture, while CI independently runs the complete automated suite. See
[CI and validation](ci.md) for the current contract.
