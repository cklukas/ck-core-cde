---
title: Editor drop handling
description: Choose whether dropped files insert paths or content in CDE Editor.
---

# Editor drop handling

CDE Editor offers three meanings for a file drop:

- **Automatic** uses desktop type information to select suitable behavior;
- **Insert Paths** inserts the dropped filenames; and
- **Insert Content** reads the files into the document.

![CDE Text Editor Dropped Files submenu with Automatic, Insert Paths and Insert Content](../images/features/editor-drop-modes.png)

*The three dropped-file modes in CDE Text Editor.*

The Options → Dropped Files submenu exposes the choice. The selected mode is
saved in `~/.dt/dtpadrc`, applied to the editor widget and propagated when an
editor is opened through ToolTalk.

This makes the same drop gesture useful for writing a list of paths as well as
combining text files.
