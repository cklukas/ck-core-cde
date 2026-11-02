---
title: CK Core Motif foundation
description: Why CK Core CDE and CK Core Motif are built and tested as one native stack.
---

# CK Core Motif foundation

CK Core CDE builds against the latest CK Core Motif `master` and requires ABI
9. The desktop uses Motif widget internals, so CDE and the toolkit are treated
as one tested native stack rather than independent system packages.

## What the build enforces

- headers and libraries come from the same isolated Motif prefix;
- the ABI 9 header constant and `XmToolkitABI` runtime symbol are present;
- Xm, Mrm, Uil, the UIL compiler and CDE are rebuilt together; and
- CI verifies that `dtwm` links to `libXm.so.9` from the selected prefix.

This keeps toolkit work—rendering, accessibility, widgets and platform fixes—
available to the desktop without accepting an unverified system Motif library.

See [Build CK Core CDE](../build.md) for the source workflow.
