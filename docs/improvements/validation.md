---
title: Build and runtime validation
description: Clean ARM64 builds, the full check suite and a real CDE desktop smoke session.
---

# Build and runtime validation

CK Core CDE validates the native stack at three levels.

## Build contract

CI builds the current CK Core Motif dependency in an isolated prefix, checks
its ABI, compiles CDE and verifies that `dtwm` links to `libXm.so.9`.

## Automated suite

`make check` runs the complete CDE suite, including dtksh. Platform-specific
legacy assumptions are separated from behavior Linux actually provides, so the
result remains meaningful on the ARM64 build host.

## Desktop smoke test

An isolated X server launches `/usr/dt/bin/Xsession`, waits for the Front Panel,
opens Style Manager, Terminal and File Manager, and verifies a command typed
through the visible Terminal. The capture records each executable and its
mapped Motif library alongside the screenshots.

See [The desktop and its tools](../desktop.md) for the current captured session.
