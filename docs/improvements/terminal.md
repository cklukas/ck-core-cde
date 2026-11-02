---
title: Terminal improvements
description: Filename drops and persistent font choices in CDE Terminal.
---

# Terminal improvements

CK Core CDE makes the Terminal easier to use with files and across repeated
sessions.

## Drop files into the shell

Dropping one or more files into a Terminal inserts their paths at the input
cursor. Spaces, quotes and backslashes are shell-escaped, and multiple paths
are separated so the resulting command line remains editable.

## Keep the chosen font size

The Options → Font Size choice is stored in `~/.dt/dttermrc` and becomes the
default for later terminals and clones. A font size restored explicitly by a
saved session still takes precedence.

![CDE Terminal Options menu with persistent font-size choices](../images/features/terminal-font-size.png)

*The live Terminal font-size menu in the uConsole session.*

The [desktop gallery](../desktop.md#terminal) shows the Terminal from the ARM64
smoke session.
