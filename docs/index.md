---
title: CK Core CDE
description: A maintained Common Desktop Environment built on CK Core Motif.
layout: home
hero:
  name: CK Core CDE
  text: The classic Unix desktop, maintained
  tagline: >
    A cohesive desktop, window manager and native application suite brought
    forward for current Unix systems with CK Core Motif ABI 9.
  image:
    src: images/default-desktop.png
    alt: CK Core CDE running on ARM64 with the Front Panel, four workspaces and desktop icons visible
  actions:
    - text: What's improved
      link: improvements.md
      theme: brand
    - text: Explore the desktop
      link: desktop.md
    - text: Build CDE
      link: build.md
    - text: CI and validation
      link: ci.md
features:
  - icon: ":desktop_computer:"
    title: Window-aware desktop icons
    details: Open windows can remain visible as desktop icons, with focus state and client-supplied live pixmaps clear at a glance.
    link: improvements/window-state.md
  - icon: ":keyboard:"
    title: Better daily tools
    details: Terminal accepts safely escaped filename drops and remembers font size; the editor offers configurable dropped-file handling.
    link: improvements/terminal.md
  - icon: "🏗️"
    title: CK Core Motif
    details: Explore the maintained native Xt toolkit that CDE builds and tests with as one desktop stack.
    link: https://cklukas.github.io/ck-core-motif/
  - icon: ":file_folder:"
    title: File Manager refinements
    details: A shelf, image previews, larger icon views, storage details and richer path interaction make file work more capable.
    link: improvements/file-manager.md
  - icon: ":sparkles:"
    title: Modern window ergonomics
    details: Task switching, snap feedback, open-window icons, Shift grid movement and familiar title-bar maximize behavior extend dtwm.
    link: improvements/window-management.md
  - icon: ":camera:"
    title: Proven on ARM64
    details: Clean uConsole builds run the full suite, launch a real desktop and capture the desktop and its core tools.
    link: improvements/validation.md
  - icon: "🧩"
    title: CK Core Apps
    details: Add native utilities, desktop companions, games and live high-resolution window icons to CDE.
    link: https://cklukas.github.io/ck-core-apps/
---

# Why CK Core CDE

CK Core CDE maintains the classic Common Desktop Environment and its native
applications on current Unix systems. The desktop includes the CDE window and
session managers, Front Panel, File Manager, Terminal, Style Manager, Help,
calendar, mail, application builder, and supporting desktop services.

The project develops and tests CDE together with CK Core Motif ABI 9, keeping
the toolkit and desktop aligned as one native stack.

The project keeps the desktop experience cohesive while making its build and
validation path reproducible. See [the running desktop](desktop.md) for
screenshots from the ARM64 smoke test, or start with [building CDE](build.md)
for source and toolkit requirements.
