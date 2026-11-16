---
title: Hardening
description: Opt-in hardening and sanitizer builds, the 2026-11-04 warning baseline and the per-component audit ledger.
---

# Hardening baseline and audit ledger

CK Core CDE is legacy C and C++ that is being brought to C23 and audited for
null-pointer, memory, overflow and unhandled-edge-case defects. This page
records the measured starting point and which components have been audited, so
each change can be checked against it. The opt-in build switches are described
in [Build](build.md#hardening-and-sanitizer-builds).

## Method

Build with `--enable-hardening` at `-O2` and keep the complete `make` log. The
counting tool `scripts/warning-baseline.py` attributes each compiler warning to
a component, counts it once however many translation units include its header,
and compares a later log with a recorded baseline:

```sh
scripts/warning-baseline.py record make.log --depth 2 --strip "$PWD" --label cde-hard > baseline.json
scripts/warning-baseline.py compare make.log baseline.json --depth 2 --strip "$PWD"
```

`compare` fails when any component and class has more warnings than the
baseline, so the baseline is a ratchet: counts may only fall. The `list` mode
prints the individual warnings of chosen classes for triage. Warnings are not
promoted to errors; a class becomes `-Werror` for a component once its count
reaches zero.

## Baseline 2026-11-04

GCC 14.2 on Debian 13 arm64, `-O2 -g0`, `--enable-hardening`, six jobs, against
CK Core Motif 20a1461b1 built with the same profile. The
build took 384 seconds. The machine-readable
record is
`validation/2026-11-04-hardening-baseline.json`.

The build reports 10,354 unique warnings.
1,126 of them belong to classes that
point at memory-safety, lifetime or error-handling defects; the rest are style
and signedness classes such as unused variables, `-Wformat-nonliteral`,
`-Wwrite-strings` and `-Wsign-compare`.

| Warning class | Unique warnings |
|---|---:|
| `-Wunused-result` | 389 |
| `-Wcast-function-type` | 221 |
| `-Wnull-dereference` | 116 |
| `-Wimplicit-fallthrough` | 108 |
| `-Wmisleading-indentation` | 106 |
| `-Wformat-truncation` | 52 |
| `-Wreturn-type` | 36 |
| `-Wmaybe-uninitialized` | 21 |
| `-Waddress` | 21 |
| `-Wstringop-truncation` | 17 |
| `-Wuse-after-free` | 13 |
| `-Wstringop-overflow` | 9 |
| `-Wformat-overflow` | 8 |
| `-Warray-bounds` | 6 |
| `-Wrestrict` | 2 |
| `-Wfree-nonheap-object` | 1 |

cppcheck reports 1,380 messages in product code (239 errors, 969 warnings, 167 portability). The error identifiers are `memleakOnRealloc` 73, `uninitvar` 69, `memleak` 19, `missingReturn` 16, `legacyUninitvar` 15, `nullPointerArithmeticOutOfMemory` 11, `shiftTooManyBitsSigned` 10, `nullPointer` 9. Test and demo directories are excluded, as are cppcheck's own
analysis-depth notices and parse failures.

## Audit ledger

A component is *audited* when a topic commit records a review of its complete
input path, not when a compiler is quiet. Components marked "none recorded"
have not had such a review. Ten libFuzzer targets under `cde/fuzz/` cover the
highest-exposure network, session, window-property, database and help parsers
and are replayed in CI via `python3 cde/fuzz/run-fuzz.py --all --replay`.

| Component | Exposure | Prior hardening | Unique warnings | Defect-class | cppcheck errors | Fuzz targets |
|---|---|---|---:|---:|---:|---|
| `programs/dtappbuilder` | developer tool; mostly generated stubs | temporary files, checked allocations, browser null checks and codegen bounds (2026-11-12, 2026-11-14); `abmf` `mf_calloc` overflow guard and `fopen` null check (2026-11-16) | 1,594 | 59 | 28 | none |
| `programs/dtmail` | unprivileged mailer; set-group-id spool helper (`dtmailspool`); mail and MIME parsing | mail spool helper split from the mailer (2026-11-10), mailbox save and path handling, text made from a message, command-line and session input, ToolTalk request data, mail server clients, message/partial assembly, MIME and lock file text (2026-11-10); signal handling, checked allocations and C++ allocator pairing (2026-11-13, 2026-11-14) | 1,369 | 44 | 0 | none |
| `programs/dtcm` | includes rpc.cmsd (RPC server run as root); calendar | rpc.cmsd request path (2026-11-04); signal handling, access checks, recurrence rules (2026-11-10); `mkstemp` drop/load files, checked allocations, `XtNewString`/`XtFree` pairing and bounded string formatting (2026-11-12, 2026-11-14) | 879 | 59 | 5 | none |
| `programs/dtfile` | file operations; drag and drop | Trash, copy and transfer (2026-11-02, 2026-11-03); `faccessat(AT_EACCESS)`, owner-before-mode attribute updates, signal dispositions and checked allocations (2026-11-13, 2026-11-14); in-process `$VAR`/`${VAR}` expansion in `_DtPathFromInput`, `_DtFollowLink`/`_DtPName`/`_DtBuildPath` bounds, and `(st_dev, st_ino)` hardlink block deduplication in `StorageSize` (2026-11-16) | 813 | 203 | 10 | none |
| `programs/dthelp` | help compiler; SGML parser | literal buffer bounds, checked allocations and `realloc` failure handling (2026-11-13, 2026-11-14); `canon1`/`pass1` `xref.c`, `malloc.c` and `help.c` lifetime fixes, `dthelpgen` topic-path bounds, and `dthelpprint` `O_CREAT | O_EXCL` (`0600`) temp-file creation and bounded formatting (2026-11-16) | 644 | 64 | 4 | none |
| `lib/DtSvc` | SPC protocol shared by dtspcd and its clients; data-type database | SPC wire layer and data-type lookup (Dts.c) (2026-11-04); data-type matching, database file reader, termios strings (2026-11-10); `_DtSignal`, self-pipe `SIGCHLD`, `DtDbReader` and `DtsMM` cache bounds, checked allocations and bounded strings (2026-11-13, 2026-11-14) | 412 | 24 | 22 | `spc-proto`, `dtsvc-dbreader`, `dtsvc-mmcache` |
| `programs/dtwm` | window manager; property and request readers | property and request readers (2026-11-02); `XtAppAddSignal` termination, session/resource parser bounds and checked allocations (2026-11-13, 2026-11-14); `WmParse`/`WmResParse` environment and session field bounds, `DataBaseLoad` duplicate elimination index and `Realloc` preservation, `WmWrkspace`/`WmResource`/`WmIPC`/`WmBackdrop` bounds (2026-11-16) | 410 | 22 | 42 | `dtwm-property`, `dtwm-session` |
| `lib/tt` | ttsession and rpc.ttdbserverd (sockets, RPC) | ttsession request decoding, authentication levels, start environment; rpc.ttdbserverd request path (2026-11-04); environment of privileged processes, object property updates, notices (2026-11-10); signal dispositions and `mini_isam` checked allocations (2026-11-13, 2026-11-14) | 394 | 242 | 1 | none |
| `programs/dtsr` | local user program | damaged index (2026-11-10); async-signal-safe shutdown (2026-11-13); `huffcode`, `dtsrclean`, `dtsrkdump`, `dtsrhan`, `tomita`, `dtsrindex` and `dtsrload` bounds and null checks (2026-11-16) | 370 | 3 | 0 | none |
| `programs/ttsnoop` | local user program | `DtTt` stream output bounds, `O_CREAT` mode and temp-file cleanup, `strdup` environment lifetime, `choiceOkayed`/`argOkayed` switch breaks, `msgReject` dispatch and `getopt` `-m` option argument (2026-11-16) | 359 | 9 | 0 | none |
| `programs/dtprintinfo` | local user program | `util/Privilege.h` privilege drop checks, checked allocations and C++ allocator pairing (2026-11-13, 2026-11-14); `ParseJobs` temp-file `mkstemp`, bounded parser tokens, child process reaping in `Invoke`, bounded `Queue`/`PrintJob`/`Process`/`MotifThread` command formatting and `snprintf` across `UI/` and `libUI/` (2026-11-16) | 345 | 20 | 0 | none |
| `lib/DtSearch` | full-text search; vendored raima subtree excluded | damaged index (2026-11-10); async-signal-safe `signal_abort` and checked allocations (2026-11-13, 2026-11-14); `split_llist`/`merge_llist`/`sort_llist` and `ditto_sort`, `clean_wrap` empty-string bounds, 64-bit `DtSrObjdate` validation, `fread_d99_header` `hole_count` clamping and `ocf`/`boolpars`/`hilite` bounds (2026-11-16) | 285 | 12 | 5 | none |
| `lib/DtTerm` | terminal emulation; escape-sequence parser | parser bounds (2026-11-02); setuid handling (2026-11-04); `_DtSignal`, checked allocations and bounded string formatting (2026-11-14) | 258 | 20 | 14 | none |
| `lib/DtHelp` | help file and markup parsing | string handling (2026-11-02); SDL parser bounds, stream EOF checks and `realloc` failure handling (2026-11-13, 2026-11-14); `FormatUtil` `mblen` error guard, `FormatCCDF` `titleChunks` free, `FormatTerm` null guard, `il/` unsigned shift masks and `realloc` preservation across `FormatSDL`, `Graphics`, `HelpUtil`, `Access`, `FileUtils` and `GlobSearch` (2026-11-16) | 248 | 73 | 34 | `dthelp-sdl` |
| `programs/dtstyle` | local user program | palette lookup null checks, `_DtSignal` and checked allocations (2026-11-13, 2026-11-14); `ColorFile` list-unlink and `ReadInInfo` bounds, `I18nEnv` pipe accumulation and `GetImsList` bounds, and bounded `snprintf` across `ColorMain`, `ColorEdit`, `Backdrop`, `SaveRestore`, `I18nMain`, `Dtwm`, `Startup`, `Audio`, `Keyboard`, `Font`, `Screen` and `Mouse` (2026-11-16) | 218 | 12 | 3 | none |
| `lib/csa` | RPC interface of rpc.cmsd | request decoding used by rpc.cmsd (2026-11-04); limits and lists of the protocols (2026-11-10); hash/name table checked allocations, ISO 8601 bounds and bounded strings (2026-11-13, 2026-11-14) | 189 | 8 | 0 | none |
| `programs/dtlogin` | runs as root; XDMCP network service | environment and arguments (2026-11-02); XDMCP, authorization, signals (2026-11-04); chooser, displays, configuration files, expired passwords, session signals, greeter requests (2026-11-10); XDMCP forward port and `Xaccess` alias cycle bounds, `BoundedCall`, checked allocations and bounded strings (2026-11-13, 2026-11-14) | 163 | 21 | 9 | `dtlogin-xdmcp`, `dtlogin-xaccess` |
| `programs/dtksh` | local user program | XDraw wrapper return values (2026-11-13); `xmcvt` and `dtkcvt` 64-bit resource/callback converters, `DtkshCvtStringToTtScope`, `DtkshNameToWidget` and `getname` bounds, and `xmcmds`/`dtkcmds` argument and EOF checks (2026-11-16) | 157 | 15 | 0 | none |
| `programs/dtinfo` | information browser; C++ | `DtMmdb`/`dtinfo` `new[]`/`delete[]` and `malloc`/`free` pairing and checked allocations (2026-11-14); `DtMmdb` `memory_pool` large-allocation header and `ll4` alignment, `buffer::expand_chunk` alignment and pointer-offset arithmetic, `DtSR_SearchResultsEntry::overlay_proximity` bitmask precedence and negative-zone guard, `StyleSheetMgr`/`OutlineListView` null guards and `realloc` preservation (2026-11-16) | 154 | 30 | 7 | none |
| `programs/dtsession` | session manager; screen lock checked through PAM; reads saved sessions; ICE and X clients | installed identity, screen lock, signals, session files and saves, client start, ICE authority, saved sessions and palettes, XSMP callbacks and client lifetimes, startup resources (2026-11-04); `mkdtemp`, palette/client parser bounds, checked allocations and bounded strings (2026-11-12, 2026-11-14) | 135 | 12 | 2 | `dtsession-client`, `dtsession-palette` |
| `programs/dtsearchpath` | dtappgather is always setuid root | `DTUSERSESSION` traversal, symlink and `lchown` checks, privilege-drop ordering before session cleanup, `CTokenizedString` bounds and `TTFile` allocator pairing (2026-11-15) | 46 | 0 | 0 | none |
| `programs/dtspcd` | root daemon from inetd; network; parses requests before authentication | request handlers, registration and privilege drop (2026-11-04); self-pipe signal loop, checked allocations and bounded strings (2026-11-13, 2026-11-14) | 6 | 3 | 1 | none |
| `programs/dtexec` | launches user commands | discovery (2026-11-02); self-pipe signal loop (2026-11-13); `CommandListJoin` negative-count guard and checked `realloc` in `ParseCommandLine` and error-pipe drain (2026-11-16) | 5 | 1 | 2 | none |

Components below 150 unique warnings are not listed; see the JSON record for
all of them.

## Excluded from mechanical rewrites

Vendored and generated code is reviewed but not reformatted or mass-rewritten:
`lib/pam`, `util/tradcpp`, `lib/DtSearch/raima`, `lib/tt/mini_isam`, the
checked-in generator outputs (`programs/dtinfo` WWL tree, `programs/dtappbuilder/src`,
`programs/dthelp/parser`, ToolTalk `frozen.*`). Where a generator exists, fix
the generator.

## Known limits

- The `sanitizers` CI job builds CK Core Motif and CK Core CDE with
  `--enable-sanitizers` (AddressSanitizer and UndefinedBehaviorSanitizer) and
  runs `make check`; the `fuzz-replay` CI job replays all 10 libFuzzer corpora
  via `python3 cde/fuzz/run-fuzz.py --all --replay`.
- The desktop smoke test has not yet been run under the sanitizers.
- cppcheck ran with `--max-configs=1`, so code behind alternative `#ifdef`
  configurations is not analysed.
