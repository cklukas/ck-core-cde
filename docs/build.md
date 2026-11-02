---
title: Build
description: Build CK Core CDE against CK Core Motif ABI 9.
---

# Build CK Core CDE

Configured Makefiles are build outputs. File Manager's Makefile is generated
from its tracked `Makefile.am`; no configured checkout paths are shipped in it.
When reusing a build tree, run bootstrap and configuration before cleaning if
the source Makefile definitions have changed.

Build and install the latest CK Core Motif `master` into an isolated prefix
first. The prefix must contain its ABI 9 Xm, Mrm, and Uil libraries, Motif
headers, and UIL compiler.

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
