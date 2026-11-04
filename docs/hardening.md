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
have not had such a review. Fuzz targets exist for none of the components yet.

| Component | Exposure | Prior hardening | Unique warnings | Defect-class | cppcheck errors | Fuzz targets |
|---|---|---|---:|---:|---:|---|
| `programs/dtappbuilder` | developer tool; mostly generated stubs | none recorded | 1,594 | 59 | 28 | none |
| `programs/dtmail` | setgid mail; mail and MIME parsing | none recorded | 1,369 | 44 | 0 | none |
| `programs/dtcm` | includes rpc.cmsd (RPC server run as root); calendar | none recorded | 879 | 59 | 5 | none |
| `programs/dtfile` | file operations; drag and drop | Trash, copy and transfer (2026-11-02, 2026-11-03) | 813 | 203 | 10 | none |
| `programs/dthelp` | help compiler; SGML parser | none recorded | 644 | 64 | 4 | none |
| `lib/DtSvc` | SPC protocol shared by dtspcd and its clients; data-type database | SPC wire layer and data-type lookup (Dts.c) (2026-11-04); the rest not reviewed | 412 | 24 | 22 | none |
| `programs/dtwm` | window manager; property and request readers | property and request readers (2026-11-02) | 410 | 22 | 42 | none |
| `lib/tt` | ttsession and rpc.ttdbserverd (sockets, RPC) | none recorded | 394 | 242 | 1 | none |
| `programs/dtsr` | local user program | none recorded | 370 | 3 | 0 | none |
| `programs/ttsnoop` | local user program | none recorded | 359 | 9 | 0 | none |
| `programs/dtprintinfo` | local user program | none recorded | 345 | 20 | 0 | none |
| `lib/DtSearch` | full-text search; vendored raima subtree excluded | none recorded | 285 | 12 | 5 | none |
| `lib/DtTerm` | terminal emulation; escape-sequence parser | parser bounds (2026-11-02); setuid handling (2026-11-04) | 258 | 20 | 14 | none |
| `lib/DtHelp` | help file and markup parsing | string handling (2026-11-02) | 248 | 73 | 34 | none |
| `programs/dtstyle` | local user program | none recorded | 218 | 12 | 3 | none |
| `lib/csa` | RPC interface of rpc.cmsd | none recorded | 189 | 8 | 0 | none |
| `programs/dtlogin` | runs as root; XDMCP network service | environment and arguments (2026-11-02) | 163 | 21 | 9 | none |
| `programs/dtksh` | local user program | none recorded | 157 | 15 | 0 | none |
| `programs/dtinfo` | information browser; C++ | none recorded | 154 | 30 | 7 | none |
| `programs/dtsearchpath` | dtappgather is always setuid root | none recorded | 46 | 0 | 0 | none |
| `programs/dtspcd` | root daemon from inetd; network; parses requests before authentication | request handlers, registration and privilege drop (2026-11-04) | 6 | 3 | 1 | none |
| `programs/dtexec` | launches user commands | discovery (2026-11-02) | 5 | 1 | 2 | none |

Components below 150 unique warnings are not listed; see the JSON record for
all of them.

## Excluded from mechanical rewrites

Vendored and generated code is reviewed but not reformatted or mass-rewritten:
`lib/pam`, `util/tradcpp`, `lib/DtSearch/raima`, `lib/tt/mini_isam`, the
checked-in generator outputs (`programs/dtinfo` WWL tree, `programs/dtappbuilder/src`,
`programs/dthelp/parser`, ToolTalk `frozen.*`). Where a generator exists, fix
the generator.

## Known limits

- The sanitized build compiles and links every program. Its documentation step
  runs the `dtdocbook` information-library tools, which abort under
  AddressSanitizer, so sanitizer lanes configure with `--disable-docs` until
  that tool is fixed.
- The check suite and the desktop smoke test have not yet been run under the
  sanitizers; the baseline covers compilation and static analysis only.
- cppcheck ran with `--max-configs=1`, so code behind alternative `#ifdef`
  configurations is not analysed.
