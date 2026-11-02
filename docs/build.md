---
title: Build
description: Build CK Core CDE against CK Core Motif ABI 9.
---

# Build CK Core CDE

Configured Makefiles are build outputs. File Manager's Makefile is generated
from its tracked `Makefile.am`; no configured checkout paths are shipped in it.
When reusing a build tree, run bootstrap and configuration before cleaning if
the source Makefile definitions have changed.

At the start of a repair batch, fetch the latest published CK Core Motif
`master`, record its full revision in `.ckgit/motif-revision` and update the
rpi4 sister pin to match. Build and install that revision into an isolated
prefix. Keep the prefix fixed for all ten rounds, their validation and
publication. Adopt further toolkit updates at the start of the next batch.
The prefix must contain its ABI 9 Xm, Mrm, and Uil libraries, Motif headers,
and UIL compiler.

Keep the configured CDE and Motif build trees for subsequent repair batches.
Use incremental builds for changed sources and affected header/library
dependencies. Do not clean or recreate the trees between batches. Keep the SDK
prefix path stable and preserve unchanged installed headers' bytes and timestamps
during SDK updates. Reconfigure when its inputs or settings change, and record
any ABI or toolchain change that requires broader object invalidation.

```sh
export MOTIF_SDK=/absolute/path/to/ck-core-motif-prefix
export PATH="$MOTIF_SDK/bin:$PATH"
export CPPFLAGS="-I$MOTIF_SDK/include $(pkg-config --cflags xft)"
export LDFLAGS="-L$MOTIF_SDK/lib -Wl,-rpath,$MOTIF_SDK/lib"

cd cde
./autogen.sh
./configure
make -j4
make -j4 check
```

Configuration verifies both the `XmToolkitABI` symbol and the ABI 9 header
constant. It fails when the selected include or library path resolves to stock
Motif or a different CK Core Motif ABI.

The complete desktop also needs the X11, PAM, ToolTalk, RPC, JPEG, LMDB, Tcl,
terminal, font, and document-processing development packages listed by the
Debian package metadata under `cde/debian/control`.

## C23 qualification

CDE is being qualified subsystem by subsystem before C23 becomes the default
language mode. The embedded ksh93 sources and genuinely historical utilities
remain explicit GNU17 compatibility components during this work.

ToolTalk's mini-ISAM library is qualified with both GCC and Clang in C23 mode.
Its public conversion routines, internal operations and record-method callbacks
use complete prototypes; the conversion regression test also compiles const
encoded input buffers through those interfaces and verifies integer, floating
point and fixed-width character round trips.

The `dtimsstart` desktop input-method launcher is also qualified at the
translation-unit level with GCC and Clang in C23 mode. Its module interface and
private functions use complete prototypes, Xt callbacks use `XtCallbackProc`,
the Xlib error handler uses `XErrorHandler`, and X property results retain the
unsigned-byte and `unsigned long` types required by `XGetWindowProperty`.
Relinking the launcher remains part of the complete-tree qualification because
it depends on CDE libraries that are being converted in later batches.

The `dtpdmd` print-display manager is qualified at the translation-unit level
with both compilers as well. Its dispatch, mailbox, manager, record, setup and
utility interfaces use complete prototypes, including the `XIOErrorHandler`
contract. Child output that contains no PDM status token leaves the match state
explicitly empty rather than consulting uninitialized storage. Like
`dtimsstart`, final relinking follows the conversion of its CDE library
dependencies.

To repeat that focused qualification from a configured C23 build tree:

```sh
make -j4 -C lib/tt/mini_isam check
```

This focused result does not imply that the rest of CDE is C23-clean. The
project will select C23 by default only after the complete build, `make check`,
and the uConsole desktop smoke suite pass with the same qualified source.
