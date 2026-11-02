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

Help-string regressions compile the production canvas string implementation
and exercise byte offsets inside multibyte characters, malformed sequences,
wide-string bounds and alignment-copy capacity. Invalid table alignment
characters fall back to left justification before any unsafe copy is attempted.

The session-manager regression executes its production XSMP/client-property
reader and recursive top-level-window lookup with controlled Xlib replies.
It checks property formats, counts, truncation, command validity, native-long
WM state storage and Xlib buffer ownership, including empty child lists.

The dtwm component check also executes the production `WM_STATE`, Motif WM
info, normal hints, colormap and UTF-8 property readers against controlled Xlib replies. It checks valid
values, failed reads, incorrect types and formats, missing data, invalid counts,
truncation and storage cleanup under AddressSanitizer and UndefinedBehaviorSanitizer.
Motif hints require the complete five-field format-32 prefix, matching the
public Motif header contract; following extension words are permitted.
Normal hints accept the complete 15-field and 18-field layouts. Colormap tests
exercise insertion of an omitted client window, each allocation failure, and
the case where none of the listed windows supplies a usable colormap.
This check requires Python 3 and a C compiler with those sanitizers. Run it alone
with `python3 cde/programs/dtwm/tests/check-property-readers.py` from the repository
root, setting `TMPDIR` to a task-specific temporary directory first.

These focused checks do not establish coverage for every desktop component.
The older terminal and other component test directories must be assessed
separately before their presence is treated as automated regression coverage.

`make check` runs the complete CDE suite, including dtksh. Platform-specific
legacy assumptions are separated from behavior Linux actually provides, so the
result remains meaningful on the ARM64 build host.

## Desktop smoke test

An isolated X server launches `/usr/dt/bin/Xsession`, waits for the Front Panel,
opens Style Manager, Terminal and File Manager, and verifies a command typed
through the visible Terminal. The capture records each executable and its
mapped Motif library alongside the screenshots.

The native `cde/programs/dtwm/tests/property-client.c` fixture can additionally
run inside that verified desktop session. Compile it against the selected
Motif headers and X11, then launch it with the session's `DISPLAY`. It creates
valid and malformed hints/title properties and a child colormap list, and checks
that the actual window manager manages every client without an X11 error.

See [The desktop and its tools](../desktop.md) for the current captured session.
