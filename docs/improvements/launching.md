---
title: Launching applications
description: Direct graphical launch and a configurable Run dialog.
---

# Launching applications

Action child-process discovery uses an iterative visited queue and independent
PID parsing. Nested children cannot invalidate a parent's parser or exhaust the
C stack, and long child lists do not split PID tokens across fixed buffers.
Executable names are accepted only after their terminating NUL is read;
interrupted and short reads are handled without accepting truncated names.
List growth and joined-command allocation sizes are checked before allocation.
The regression uses a private procfs fixture with nested siblings, cycles,
long child lists, invalid IDs and interrupted reads.

CK Core CDE separates two useful launch workflows:

- **Run** starts a graphical program directly; and
- **Run...** opens the command dialog, where **Run in Terminal** can be enabled
  for an interactive command or disabled for a graphical application.

This keeps terminal-backed commands available without forcing every graphical
program through an unnecessary terminal window.

![Run dialog with a Run in Terminal checkbox](../images/features/launching-run-dialog.png)

*The executable Run... action makes its Terminal choice explicit.*
