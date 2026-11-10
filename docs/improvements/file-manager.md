---
title: File Manager improvements
description: Optional folder comparison, saved preferences, shelf, image previews and storage information in CDE File Manager.
---

# File Manager improvements

An optional native [folder comparison window](folder-comparison.md) opens from
**File → Synchronize Folders...** when CDE is built with `--with-ck-core-fs`
and installed ck-core-fs-lib 0.3 or newer. It provides paged comparison, metadata
assessment, byte/date filters, scoped matching-rule review and saved named
preferences. Apply remains compile-time disabled: operation choices preview a
plan, and mirror/two-way execution is not delivered.

The ordinary **Copy to...** and **Move to...** commands still perform file
operations through the existing helper. They are separate from the new window's
read-only Copy/Update plan modes. The [design and delivery status](file-operations-design.md)
documents the remaining execution and reconciliation requirements.

Folder-operation feedback distinguishes completed, partial, failed and cancelled
work. Cancellation or an explicit overwrite skip blocks deletion of a move's source.
Cancellation reports that it is settling, then leaves the final result visible;
only successful completion can close the progress window automatically.

File-copy transfers complete each input block even when the operating system
accepts only part of a write. Interrupted reads and writes are retried; a zero-byte
write is reported as an I/O error. The first transfer error is retained while
both descriptors are closed, and delayed close failures are reported before
the copy is accepted. The transfer regression injects these conditions into
the production copy routines and checks copied bytes and source preservation.
The regular-file fallback used directly by File Manager follows the same
completion rule: a failed destination close blocks source removal during a
cross-filesystem move, including when no error-dialog callback is installed.
Dropped or pasted buffers are written completely before success is sent to the
parent window. Invalid lengths, missing nonempty data, zero-progress writes and
delayed close failures report an error; an empty buffer creates an empty file.
New targets are created exclusively. An unexpected existing file or symbolic
link is rejected rather than overwritten; approved buffer replacement removes
the old target through the existing conflict workflow before creation.
For multi-file operations, a later successful item does not erase an earlier
transfer or preflight failure in the result returned to the parent window.
Dismissing a subsequent conflict dialog likewise retains an earlier failure.
A desktop drop whose source cannot be resolved reports that error and skips
that item instead of passing a missing source into the transfer routine.
Failed transfers attempt to remove their newly created incomplete destination;
cleanup failure can leave it in place. These legacy operations do not provide
crash recovery or transactional replacement.

An ignored copy error is tracked separately from successful completion.
Cross-filesystem moves retain their source if any file or recursive child was
not copied completely, even when the user chooses to ignore the error.

Recursive copy and delete operations allocate complete child paths instead of
appending names to fixed buffers. Cross-filesystem moves likewise check the
complete source parent. Directory iteration retries interruption and reports
other read and close errors. Symbolic-link copies grow their read buffer when
it fills, preserving the complete link target. The filesystem regression covers
deep fixture paths longer than 1024 bytes, directory errors and link growth.

Directory refresh also validates the complete worker message and bounds file
names, relative symbolic-link expansion, data types and action labels before
encoding them for the browser. An oversized or incomplete entry is rejected as
one failed refresh response, so it cannot install partial metadata or overwrite
a fixed buffer. If account lookup is temporarily unavailable, icon-position
storage uses the numeric user ID instead of dereferencing a missing account.
Refresh-time child paths and cached modified-subdirectory paths use the same
bounded join. Attribute view also bounds owner, group and symbolic-link display
text and shows a placeholder if an unusual timestamp cannot be converted,
rather than allowing account data or formatting failures to corrupt the row.
Copy, move and link dialogs compose their selected-object captions as Motif
strings, so a translated caption or long object name is not copied through a
fixed text buffer. Their destination and preview paths are joined with explicit
capacity checks; an oversized resolved path is rejected before an operation is
started instead of being truncated or copied past the target buffer.
The final host, parent directory and leaf split is also transactional. It uses
the source directory—not uninitialized destination storage—to decide how to
join a relative name, preserves the root parent for `/name`, and reports an
invalid or oversized target before file creation or transfer setup begins.
Workspace icons affected by a completed move follow the same bounded path
rules. Overlong or malformed refresh paths are ignored without changing the
completed file operation, parent-directory matching observes component
boundaries, and refreshed desktop metadata replaces its owned optional strings
without duplicate allocation or null-string copies.
Conflict review cancellation is a terminal non-success outcome: work completed
before cancellation remains in place, but the operation is not reported as
successfully complete. When several conflict rows fail while the worker drains
the batch, the first failure remains the operation result; later transfer,
rollback, cleanup or protocol failures cannot hide its original cause.
Background operation control messages are also written completely, with
interrupted and partial writes handled and a closed reader reported without a
fatal `SIGPIPE`. If the parent channel is lost before a modification notice,
the worker stops before changing that item; if final status cannot be
published, the worker exits unsuccessfully instead of claiming completion.

CK Core CDE develops File Manager as a visual workspace rather than only a
directory browser.

## Shelf and path interaction

The shelf keeps frequently used files and directories close to the current
view. Shelf items support opening, running, rearranging and file operations.
IconicPath entries expose the same popup interaction expected from other path
components.

The initial shelf frame uses a deterministic small-icon placeholder until the
dialog's saved view data is available. Applying the real view then rebuilds the
shelf at its selected icon size; startup never reads an uninitialized view.

## Richer views

- supported image files can render their own content as icons;
- extra-large presentation and additional Icon Editor zoom levels are
  available;
- icon and shelf layouts account for labels and center their content; and
- a Storage Size dialog reports detailed use for a selected object.

![File Manager Set View Options dialog showing Shelf, extra-large icons, image rendering and centered labels](../images/features/file-manager-options.png)

*File Manager exposes the new view controls together in Set View Options.*

File actions also accept filenames containing quotes and backslashes.

The [desktop gallery](../desktop.md#file-manager) shows File Manager browsing
the staged CDE installation.
