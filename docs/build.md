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
The dtksh build copies its vendored ksh93 tree into the private build directory
and passes `-std=gnu17` only to that package build; the maintained dtksh glue
continues to inherit the enclosing C23 qualification flags. This also keeps
generated ksh93 objects out of the source checkout. The compatibility build
uses the lane's configured compiler, so both GCC and Clang exercise GNU17.
Generated dtksh scripts likewise name their source-tree prerequisites
explicitly, allowing the check and install targets to run out of tree.
The maintained dtksh command layer is compiled in the enclosing C23 mode. Its
Xt, Xlib, ToolTalk, print-setup and ksh discipline callbacks use their native
prototypes; operations with different signatures have separate typed paths or
small value adapters rather than common untyped dispatch.
The same rule applies to Motif constructors and list, text and subwidget
operations: related commands share a helper only when their callback types are
actually compatible.

The `dtsearchpath` executable searches the libCliSrv build directory before its
maintained source directory. Generated interfaces can therefore override their
source counterparts when present, while a clean out-of-tree build still finds
the checked-in `Environ.h` interface without copying it into the build tree.

ToolTalk's mini-ISAM library is qualified with both GCC and Clang in C23 mode.
Its public conversion routines, internal operations and record-method callbacks
use complete prototypes; the conversion regression test also compiles const
encoded input buffers through those interfaces and verifies integer, floating
point and fixed-width character round trips.

ToolTalk's drag-and-drop helper also publishes one typed interface to C and C++
consumers rather than retaining a K&R fallback. Its send/receive callbacks and
the internal ToolTalk reply callbacks use their complete argument lists. The
library builds and links with both C23 compilers, and its type negotiation is
exercised against the linked archive for common-type and fallback selection.
The `ttdbck` utility searches both build and source directories for its private
ToolTalk, mini-ISAM and typed-database headers, so a clean out-of-tree build no
longer relies on headers copied into a sibling build directory. The
`tt_type_comp` utility likewise links its generated grammar include to the
maintained source header and searches the private ToolTalk source directories.
The `ttauth` command utility declares its public entry points explicitly and
uses separate typed dispatch contracts for command handlers and authority-entry
iterators, so C23 does not reinterpret either table as accepting no arguments.
The ToolTalk database server also searches the maintained ToolTalk and
mini-ISAM source directories, allowing its C++ sources to build cleanly from an
out-of-tree C23 qualification directory. The ToolTalk session server uses the
same source-aware include contract for its private session, message and utility
headers.

The DtMrm resource-registration library searches both build and source trees
for the private DtHelp and DtTerm headers included by its widget-private
interfaces, rather than relying on an in-tree sibling layout.

The dtmail library and its Motif, RFC, desktop and print consumers likewise
include their maintained header trees explicitly. Clean qualification builds
therefore do not depend on source headers having been mirrored into sibling
build directories.
The desktop executable consumes `X_CFLAGS` as a complete optional flag set, so
an empty platform X include path cannot swallow the following source include.

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

DtSearch's embedded Raima runtime builds as a complete static archive with GCC
and Clang in C23 mode. Its public single-database declarations, internal startup
contract, linked-list API, private helpers and database error callback all use
complete prototypes. The collation path explicitly crosses from character data
to unsigned table indices, so the compiler no longer has to infer signedness or
discard a legacy dereference expression while advancing an input pointer.

The DtInfo widget library's C classes use the complete Xt method contracts for
initialization, realization, geometry, redisplay and resource changes. The same
C23 qualification covers DtSvc's private hash callbacks, Instant's typed
element/content tree visitors, and dtscreen's mode dispatch and fireworks
helpers. Callback implementations accept their framework-defined context type
and recover private state inside the function, rather than relying on an
incompatible function pointer or a cast at registration time.

DtHelp's image library uses exact callback types for pipeline initialization,
cleanup, destruction and execution, conversion records, and image-object
lifecycle hooks. Implementations accept the pipeline or object context defined
by the framework and recover their private record internally. This also makes
the cleanup ABI explicit: cleanup hooks receive the private record and the
abort flag, while image descriptors belong only to initialization hooks. The
complete image-library archive builds from clean objects with GCC and Clang in
C23 mode.

The surrounding DtHelp library also has typed contracts for formatter UI
services, SDL content models, display-area notifications, interpreter filters,
and region cleanup. Its buffered-I/O layer keeps read and write operations in
separate callback slots: input callbacks receive the buffer, while output
callbacks receive the byte and buffer. This removes the former dependence on a
single unspecified-argument function pointer serving two incompatible ABIs.
The complete shared DtHelp library compiles and links with both C23 toolchains.
Dialog map callbacks return `void` as `XtCallbackProc` requires. Menu, message
and display-area event handlers accept Xt's dispatch-control pointer and are
registered directly, without function-pointer casts concealing a three- versus
four-argument ABI mismatch.

The maintained desktop launch and session paths also use their framework and
system interfaces directly. `dtexec` relies on the standard and POSIX headers
instead of shadowing them with unspecified-argument declarations; `dtaction`
timer callbacks use the Xt timeout signature; and `dtsession` declares its
XSMP, ICE, save-state, signal and UI routines with complete prototypes. The
session manager's client- and window-manager readiness hooks implement the
complete four-argument `XtEventHandler` contract at add/remove registration.
Its X I/O hook accepts the display directly, while its fatal Xt hook is marked
non-returning and has an unconditional `_exit` fallback if orderly session
shutdown is not yet permitted.
The `dsdm` X error and recursive-window callbacks, interest-property reader and
drag-and-drop proxy boundary use exact shared Xlib contracts; the `dticon` X
error hooks likewise avoid function-pointer casts.
The welcome-screen timeout likewise registers its existing `void(int)` alarm
handler directly rather than casting it to a no-argument function type.

The icon editor's remaining cross-file interfaces are fully prototyped as
well. Its ToolTalk contract and media callbacks use the toolkit-defined
argument lists, dialog constructors declare explicit no-argument contracts,
and dialog-management operations accept a single `Widget`. The selection
flash timer implements `XtTimerCallbackProc` directly, so registrations do not
cast around an incompatible client-data parameter.

The window manager likewise registers callbacks without disguising their
types. Workspace-presence and front-panel callbacks receive `XtPointer`
payloads, event handlers include Xt's `Boolean *` dispatch parameter, and
icon-box and window-manager timers implement their framework contracts
directly. The window-manager command table and its argument-parser table use
the concrete signatures shared by their implementations, including explicit
encoding of legacy numeric command arguments instead of writing through an
incompatible pointer type. Supporting desktop APIs now preserve the read-only
tokens returned by `shellscan`, distinguish shared Xt callbacks from event
handlers, and publish the complete four-argument `_DtCreateView` factory
prototype.

The File Manager and the DtSvc dependency path now link as complete
executables and shared libraries with GCC and Clang in C23 mode. Converted File
Manager resource serialization, file-operation dispatch, menu construction,
signal handling and X/Xt callback paths use their exact contracts. DtSvc's
command invoker, hash traversal and destruction callbacks, workspace and marquee
notifications, and dynamically loaded Help entry points are typed at their
definitions and call sites. Both linked File Manager executables resolve
`libXm.so.9` from the pinned CK Core Motif qualification prefix.

The shared public-interface layer is qualified in both C and C++ consumers as
well. ToolTalk message callbacks, DtSvc receive/message/status callbacks,
command-invoker initialization, action allocation, multimedia database
lifecycle functions, the symbolic-table API, and the Sb event-loop hooks now
publish their complete parameter lists. Instant's initialization, browsing,
ID-reference and output-buffer declarations match their definitions. Header
probes compile these interfaces with GCC and Clang in C23 mode and with G++ and
Clang++ in C++17 mode; the corresponding ToolTalk, DtSvc, DtMmdb, dtspcd,
dtfile and dtdocbook consumers build and link in both C23 configurations.

The editor, calculator, print-display manager, terminal primitive layer and
selected calendar/mail interfaces are qualified with GCC and Clang C23 as
well. `dtpad` uses `XtCallbackProc` and the complete ToolTalk contract for its
file workflow; `dtcalc` registers its X error handler without a cast and keeps
its button dispatch explicitly parameterless; and `dtpdm` publishes explicit
no-argument constructors. DtTerm's parser action table and cleanup entry points
now carry their actual arguments, while dtmail's generated utility accessors
state that they take no arguments.

The calendar storage containers use concrete key, comparison, enumeration,
destruction and tree-action callback types. Their v4/v5 adapters recover the
typed appointment or entry key from the container's opaque key argument, and
the v4 result callback uses one explicit output-slot contract instead of a
function-pointer cast. The caller keeps each concrete appointment result in a
separately typed local. The affected libcsa and calendar-server objects compile
with both C23 toolchains. The calendar RPC table now stores one exact dispatch
signature and uses per-procedure adapters to call each service through its real
argument and result types. Its tree/list selection hooks and signal handlers
are exact as well, so the complete `rpc.cmsd` server builds and links in both
C23 configurations. The libcsa RPC-generation rules resolve all templates and
filter scripts through `$(srcdir)`, allowing the same clean out-of-tree build.
The checked-in calendar RPC v2-v5 headers now expose one typed interface instead
of retaining alternate K&R declarations. Out-of-tree calendar builds take the
generated `agent.h` from the build tree while resolving checked-in CSA and
libDtCmP headers from the source tree. The dtcm parser query hook, Xt callbacks,
ToolTalk load callbacks, session helpers and standalone entry points likewise
use their exact contracts. As a result, clean libcsa, libDtCmP, `rpc.cmsd` and
calendar-program builds link under both GCC and Clang C23.

The next maintained-program tranche removes unspecified argument lists from
the remaining small DtHelp, DtTerm and DtWidget helpers and from dtcreate,
dtlogin, dtsession and dtstyle. DtWidget's editor and search callbacks expose
`XtPointer` at the toolkit boundary; its icon timer receives `XtPointer`, and
its icon and format event handlers implement the full `XtEventHandler` ABI.
These paths register directly without function-pointer casts. In dtlogin, the
network-address helpers are
declared once in the shared interface, optional authentication initializers
match the dispatch table's `void` contract, and chooser callbacks carry their
complete Xt types. Local socket, chooser and XDMCP code relies on those shared
host/address prototypes instead of shadowing them with `f()` declarations;
dtcreate no longer retains an unused untyped signal-function pointer.
Dtappbuilder's browser, editor and generated-application
utilities now state their real contracts instead of relying on implicit
arguments or an untyped undo slot. Its object verifier also installs a real
`int`-argument signal handler for guarded memory probes, while its callable
UIL compiler uses separate, exact message and status callback contracts
without registration casts. Dtudcexch similarly publishes complete BDF
conversion and utility interfaces; its primary UI and file-selection callbacks
accept `XtPointer` client data directly, without callback casts. The bundled
PAM dispatcher uses one exact module-function type, while the font loader uses
the system `<dlfcn.h>` declarations and `RTLD_*` flags. DtHelp's parent library
also searches the image library and JPEG helper source include directories, so
a clean out-of-tree build does not depend on headers left in the build tree.
DtTerm's utility, terminal and view libraries likewise resolve their private
line-font and terminal headers from the source tree, and its generated line
table reads the checked-in data file there.
Its cursor and pointer-blanking timers implement `XtTimerCallbackProc`
directly, and dtterm's session-save hook uses the exact Xt callback boundary;
none requires a registration cast.
App Builder resolves each private include from `$(top_srcdir)`, feeds its
message catalog from source-qualified inputs, and stages read-only links to its
BIL project inputs when generating code outside the source tree. It seeds the
build with the maintained generated header and stub files so application
resources and user-code sections survive code generation, and its exit path
assigns `SIG_IGN` through the native signal-handler type. Project, module,
browser and modal-prompt chooser arrays store exact `XtCallbackProc`
implementations rather than cast functions specialized to a Motif callback
record. Those implementations accept `XtPointer` at the toolkit boundary and
recover `XmSelectionBoxCallbackStruct` only inside the callback. App Builder's
generated message-dialog runtime uses the same direct callback entries, and
the code emitter writes plain null callback values instead of generated
function-pointer casts. Palette and editor registrations likewise store their
callbacks directly. List and toggle callbacks accept Xt's opaque call-data
argument and recover the concrete Motif callback record inside the function;
the callback-map helper returns a typed null without a function-pointer cast.
The maintained generated application source also initializes project-open and
import chooser arrays with the direct callbacks. Their shared implementations
recover the selection-box record internally. The final application also
includes the parser header from the sibling build directory rather than
depending on an in-source generated copy. App Builder's remaining internal and
public no-argument declarations use `(void)`, and unused generic function
pointer aliases have been removed. A complete GNU17 diagnostic audit with
`-Wstrict-prototypes -Wold-style-definition` reports no remaining ambiguous
declaration or old-style definition before the same target is rebuilt in C23
mode. Dtterm resolves its
terminfo and fallback-resource generation scripts and inputs through
`$(srcdir)`, while its compile includes cover both generated build headers and
the checked-in private terminal headers.
Dtstyle's fatal Xt error hook carries Xt's non-returning contract, and its map
and window-manager reparent handlers implement `XtEventHandler` directly
without casts at registration or removal sites.
Its help and startup callbacks likewise implement `XtCallbackProc` at the
registration boundary, recovering toggle or help records from `call_data` only
inside the callback. Font and cursor-preview labels retain the mutable string
type required by the legacy Motif and message-catalog interfaces.
Dtcreate uses the ToolTalk toolkit's input handler for Xt dispatch and reserves
its exact three-argument contract callback for `ttdt_session_join`, avoiding an
incompatible callback cast and a duplicate ToolTalk connection. Its help-dialog
close hook is also registered directly through its exact Xt callback type. The
icon selector stores and accepts `XtCallbackProc` values rather than unspecified
function pointers, and its directory scanner gives the selection hook its real
two-argument contract.
Generated widget registrations now pass their callbacks without function-pointer
casts. Those callbacks expose Xt's three-argument boundary and recover Motif- or
CDE-specific callback records from `call_data` only where the implementation
actually reads one.
Dtlogin retains X I/O and alarm handlers through their native function-pointer
types and restores X's default error handler with `NULL`, without handler casts.
Its language enumerator table carries the exact next/end method contracts, and
chooser/XDMCP code uses the shared typed environment and opaque-address APIs.
Dialog file access comes from `<stdio.h>` rather than local `fopen()`
redeclarations, and the fatal Xt error callback carries Xt's non-returning
contract.
Dtstyle registers dedicated `int`-argument signal handlers rather than casting
Xt callbacks or no-argument helpers to the signal ABI.

To repeat that focused qualification from a configured C23 build tree:

```sh
make -j4 -C lib/tt/mini_isam check
make -j4 -C lib/tt/lib/api/dnd
make -j4 -C lib/DtHelp/il
make -j4 -C lib/DtSvc
make -j4 -C lib/DtMmdb LEX=flex
make -j4 -C lib/csa
make -j4 -C programs/dtspcd
make -j4 -C programs/dtfile
make -j4 -C programs/dtdocbook LEX=flex
make -j4 -C programs/dtcalc
make -j4 -C programs/dtpad
make -j4 -C programs/dtpdm
make -j4 -C programs/dtcm/libDtCmP
make -j4 -C programs/dtcm/server
make -j4 -C programs/dtcm/dtcm
make -j4 -C programs/dtappbuilder
make -j4 -C programs/dtudcexch
make -j4 -C programs/dtlogin
```

This focused result does not imply that the rest of CDE is C23-clean. The
listed legacy component directories currently provide little or no executable
`make check` coverage, so successful dual-compiler builds and header probes are
qualification evidence for this batch, not a replacement for the project-wide
test gate. The project will select C23 by default only after the complete
build, `make check`,
and the uConsole desktop smoke suite pass with the same qualified source.

The DtInfo C++ viewer also consumes the typed DtHelp hyperlink and resize
interfaces directly. Its callbacks accept the interface's opaque display/client
pointers and recover the concrete viewer objects inside the callback. They do
not cast through unspecified-argument function-pointer aliases. Rebuilding
`programs/dtinfo/dtinfo/src/Agents/libAgents_a-NodeWindowAgentMotif.o` checks this
consumer against the actual private DtHelp declarations.
