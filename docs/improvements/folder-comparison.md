---
title: Compare folders
description: Use the optional read-only folder comparison window, matching rules and saved preferences.
---

# Compare folders

The optional **File → Synchronize Folders...** command opens a native comparison
window. It inventories two folders, shows their differences and evaluates
matching rules. **Apply reviewed plan is disabled in the current build.** The
helper does not copy, replace, move or delete either folder's contents. Two-way
reconciliation and mirror execution are not delivered.

The ordinary File Manager **Copy to...** and **Move to...** commands continue to
perform file operations through the existing copy/move helper. Choosing **Copy**
in this comparison window selects a proposed policy; it does not run that legacy
copy command.

![Native folder comparison with paged results and a disabled Apply action](../images/dtfile-folder-comparison.png)

Read-only comparison in the native helper. Apply remains disabled.

## Enable the optional helper

The default CDE build has no filesystem-library dependency and does not include
this command. To build `dtfile_sync`, install **ck-core-fs-lib 0.3 or newer**, point
pkg-config at that installation and configure CDE with `--with-ck-core-fs`:

```sh
PKG_CONFIG_PATH="/absolute/fs-prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
  ./configure --with-ck-core-fs
```

Run this from the CDE source/build configuration directory, keeping the other
options and CK Core Motif setup described in the [build guide](../build.md).
Use the installation's actual pkg-config directory, which may be `lib64/pkgconfig`.
Keep the selected shared library available to the desktop session at runtime.
For a custom prefix, add its library directory to the configured runtime linker
path, for example with `LDFLAGS="${LDFLAGS:+$LDFLAGS }-Wl,-rpath,/absolute/fs-prefix/lib"`,
following the Motif setup in the build guide, or use the system loader's library
installation path. The native qualification uses explicit frozen library paths.
The helper also requires C11 atomics and the platform's `closefrom` capability.
It closes inherited descriptors above standard input/output/error immediately
after exec, before locale/catalog, display or worker initialization, so File
Manager's cache and IPC descriptors do not remain open in the helper. Configure
link-checks `closefrom` together with the required tree API. An explicit request
fails if these capabilities or the library/version are unavailable; a version
label alone is insufficient. Enabling the helper does not enable Apply.

Changing this configure option updates a private generated menu header. An
incremental build automatically rebuilds the affected File Manager menu; a
clean build is unnecessary.

## Start a comparison

The three notebook tabs are at the top. Each page scrolls independently when
its content exceeds the available space. The initial window size is bounded by
the actual screen so the bottom action buttons remain visible.

1. Open the source folder in File Manager and choose **File → Synchronize
   Folders...**. The current view supplies the source, independently of selected
   objects. The command is unavailable in restricted/toolbox views.
2. In **Folders and options**, choose a destination with **Browse...**, or enter
   absolute source and destination paths.
3. Keep **Compare only**, or choose a policy to inspect its proposed plan.
   **Copy**, **Update: protect destination-newer**, **Copy missing only**,
   **Synchronize one way** and **Mirror** are preview modes here. Even proposed
   mirror removals are never executed.
4. Choose the metadata scope and optional byte/date bounds, then select
   **Compare folders**. Changing these options requires another comparison.
5. In **Comparison and results**, use **Previous page** and **Next page** and
   select a row to inspect both versions, the reason and applicable proposed
   names. Plan totals describe proposed copies, new folders, replacements,
   removals, skipped/protected objects and bytes; incomplete totals are labelled.

Comparison reads source/destination contents and metadata. A missing attribute,
unreadable object or unsupported capability is reported as unknown or
unsupported. Equal size/time is not a claim of identical content; inspect the
reported comparison reason. Read-only comparison can still create the helper's
private journal/configuration data under your home directory.

## Metadata and filters

**Complete preservation** requests assessment of the complete metadata policy.
**Ordinary mode/times; new caller ownership** selects the narrower ordinary
policy. The detail view reports each side's metadata assessment and timestamp
precision where known. Neither selection promises that an unsupported provider
can preserve metadata, and selecting the ordinary scope does not grant a
metadata-loss override or enable writes.

Minimum/maximum byte fields accept nonnegative integer byte counts. Empty fields
leave the corresponding bound unset. **Modified on/after** is inclusive;
**Modified before** is exclusive. Bounds accept a local calendar date such as
`2026-11-02`, or an exact UTC timestamp such as
`2026-11-02T12:34:56.123456789Z`. A date alone means local midnight. Saved bounds
reload as UTC timestamps with their fractional precision, so changing the time
zone does not move the saved interval. Invalid or reversed bounds are rejected.

The current window uses all object kinds and a fixed recursion-depth limit of
1024. Pattern editors, mount-boundary controls and configurable depth are not
part of this window.

## Review matching rules

In **Matching rules**, select a matching class and an available action, then
choose one of these scopes:

- **All matching in job**: reusable preferences for eligible matching objects.
- **Selected matching rows**: the selected rows on the displayed page.
- **This item**: exactly one selected object.
- **Matching in selected subtree**: exactly one selected subtree root.

Choose the tie/unknown fallback and the separate **Do not replace nonempty with
empty** and **Preserve replaced versions** preferences. Add/update the rule,
then select **Review rules** to obtain backend counts and consequences. Editing
or removing a rule requires review again; only actions offered for that class
and scope can be selected.

“Resolved” in a rule count means that the rule resolved a matching case in the
**proposed plan**. It does not mean files were changed. Keep Both, reviewed rename and
preservation controls describe proposals; they do not perform retention or
renaming while Apply is disabled. Unsupported choices remain unavailable or
unresolved. A new comparison discards item/selection/subtree decisions; its
retained job-wide preferences must be evaluated against the new inventory.
An Ask rule may remain unresolved in its own counts during Compare review:
Compare reports differences without approving a filesystem change. Selecting a
copy/update policy still requires a fresh comparison and decision review.

Compare works without a journal directory or `HOME`; unavailable saved
preferences do not prevent it. Other operation previews currently require
private operation state.

## Save, load and delete preferences

Enter a profile name and select **Save preferences**. Select an existing entry
and use **Load selected preferences** to restore it. **Delete profile** asks for
confirmation and removes only the named preferences, never the compared files.
The status line reports asynchronous load/save/delete completion or failure.
Saving an existing name updates that entry.

Profiles preserve the exact source/destination path bytes, operation and metadata
choices, filter bounds and reusable job-wide rules. Saving is refused while
item, selection or subtree decisions are in the rule list. Profiles contain no
row identities, comparison generations, execution receipts or synchronization
history. Loading one always requires a fresh comparison and review.

The store supports at most 64 profiles, names of 1–64 UTF-8 bytes, 64 job-wide
rules per profile and 65,536 bytes per native folder path. Limits are reported
rather than truncating or discarding entries. Preferences live in
`$HOME/.dtfile-sync/profiles.v1`, in an owner-only `0700` directory with regular,
single-link `0600` files. The helper also checks the access path and macOS ACLs;
mode bits alone do not establish privacy. Saves merge the latest valid database
under a cooperative lock, then write/flush/rename/flush atomically. Corrupt,
unreadable or unsafe stores are reported and retained, not silently reset. A post-rename flush
failure reports uncertain durability. Volumes mounted with ownership disabled
are refused for preferences; this does not imply that every filesystem's
security semantics have been qualified.

## Native names and cancellation

Folder paths supplied by File Manager, a chooser or a saved profile keep their
exact POSIX bytes separately from their presentation. Non-ASCII/control bytes
and backslashes may appear as `\xNN`; a long result label may be shortened.
Display labels are not native paths or rule identities. Editing a folder field
replaces its retained native value with the literal entered text; `\xNN` is not
parsed back into a filename. Use the chooser or reload the profile to retain an
unusual existing path without retyping its display label.

**Pause** requests a safe pause; **Resume** clears that request. **Cancel
operation** acknowledges cancellation and waits for safe cleanup while the
window stays responsive. **Close** also stops the comparison and preference
workers, waiting for actual worker/resource drain before the process exits.
A terminal progress label alone does not mean native cleanup has finished.

The [design and delivery status](file-operations-design.md) separates this
read-only workflow from future execution and two-way reconciliation requirements.

## Run focused checks

With the optional backend selected, run `make -j1 -C programs/dtfile check`.
The installed-library controller fixture verifies native inventory and paging,
review receipts, transport failures, cancellation and an actual ordinary copy
inside disposable fixtures. The profile fixture exercises the production
parser, concurrent saves, privacy and atomic persistence.

Set `TMPDIR` explicitly. Private test homes must be on an ownership-enforcing
filesystem with trusted ancestors; if that differs from the build's temporary
location, set `DTFILE_PRIVATE_TEST_ROOT` to an existing suitable fixture directory.
