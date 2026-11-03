---
title: File operations and synchronization design
description: Read-only folder comparison delivery status and remaining shared-engine execution requirements.
---

# File operations and synchronization design

## Current delivery

CDE can optionally build `dtfile_sync`, a native **read-only folder comparison
and rule-review window**, using installed ck-core-fs-lib >= 0.3. Enable it with
`--with-ck-core-fs`; default builds do not require that dependency or show its
menu command. Configuration checks C11 atomics, platform `closefrom` support
and the required tree C API symbols, including rule counts, command
acknowledgement and actual worker-drain observation, not just a package version.
The helper closes inherited descriptors >= 3 at entry after exec, before
locale/catalog, Xt or worker setup. The [user guide](folder-comparison.md) covers setup,
comparison, metadata/filters, matching rules and named preferences.

| Workflow | Delivered behavior |
| --- | --- |
| File Manager Copy to... / Move to... | Existing file operations through `dtfile_copy`, including explicit completion/partial/failure/cancellation feedback and move-source retention. |
| Synchronize Folders... | Source/Destination comparison, bounded pages, plan totals and per-side observations/reasons. Reads contents and metadata; does not apply the proposed tree changes. |
| Operation and metadata choices | Preview Copy, Update, Copy missing, one-way or Mirror policy under complete/ordinary metadata assessment. Unsupported/unknown assessments remain visible. |
| Filters and matching rules | Byte/date bounds; item, selected-page rows, subtree and JOB scopes with backend review and matched/resolved/unresolved counts. “Resolved” refers to the plan, not filesystem mutations. |
| Named profiles | Asynchronous save/load/delete of exact native folder bytes, operation/metadata choices, filter timestamps and at most 64 reusable JOB rules. Fresh comparison and review are required after loading. |
| Apply reviewed plan | Compile-time gate `DTFILE_SYNC_MUTATION_QUALIFIED` remains zero; execution is unavailable in this delivery. |
| Two-way reconciliation / mirror execution | Not delivered. A mirror preview is not a deletion facility. Profiles are not reconciliation baselines. |

Date-only filter input uses local midnight. Saved seconds/nanoseconds reload as
exact UTC timestamps, including fractions, so a time-zone change cannot shift
the interval. Native POSIX path bytes are owned separately from escaped labels;
the UI never parses a display escape back into a native filename or identity.
Unrepresentable profile options are refused rather than silently weakened.

Preferences use the user-owned private `$HOME/.dtfile-sync` directory and bounded
versioned `profiles.v1` store. A worker checks no-follow opens, owner/mode/type
and single-link files, and checks the held access path and macOS ACL grants.
Unsafe or uninspectable permissions are refused rather than inferred from mode
bits alone. It locks cooperatively, reloads and merges current entries, then
stages/flushes/renames/flushes. The home directory is flushed after both creation
and adoption of the private config directory: EEXIST alone does not establish
that another creator made the parent entry durable. Parse/privacy/I/O/limit errors preserve the
existing store; post-rename flush failure reports uncertain durability. This
lock coordinates cooperating helpers and does not protect against malicious
same-UID writers. Row IDs, selections, subtree keys, plan generations, receipts
and baseline history are never saved as reusable authority.

Inventory, page/name retrieval, rule evaluation and preference I/O run outside
the Xt thread. Pause/cancel requests stay responsive. Close waits for the backend
worker's actual drain and preference-worker completion before releasing their
resources; a terminal snapshot alone is not sufficient proof of cleanup.

## Planned execution and reconciliation

The remaining sections specify future behavior and acceptance requirements.
They do not enable any of the proposed writes, deletions, retention or two-way
workflows in the current helper. Existing legacy Copy/Move operations remain
available independently of those gates.

### Action catalogue

| Action | Meaning | Default protection |
| --- | --- | --- |
| Copy | Copy selected objects; review destination collisions | Ask before replacing |
| Move | Relocate selected objects; verify cross-filesystem copies before source cleanup | Retain every source not successfully transferred |
| Copy missing | Add objects absent at the destination | Never replace an existing object |
| Update | Add missing objects and copy source-newer files | Protect destination-newer files; ask about ambiguous comparisons |
| Synchronize one way | Make included destination files match the source; retain destination-only objects | Preview replacements, including destination-newer files |
| Mirror | Also remove destination-only objects in the included scope | Separate deletion review and recoverable removal |
| Synchronize both ways | Reconcile changes in a saved folder pair since their last successful reconciliation | Ask on conflicts; preview before execution |
| Compare | Inspect differences without changing either tree | No baseline advancement or mutation |
| Verify | Read and compare content and requested metadata | Report unreadable or unstable objects as unknown |
| Rename multiple | Preview naming templates, substitutions and counters | Reject collisions and ambiguous case changes |
| Trash / Restore | Recoverable removal and restoration | Explain retention limits and restore collisions |
| Delete permanently | Irrecoverable removal | Explicit action and scope confirmation |
| Create folder / link | Create a directory, symbolic link or supported hard link | Explain link type and target; reject unsafe overlap |

Copy and Move stay directly accessible. A Synchronize Folders command opens
the comparison workspace. Advanced comparison, filtering and preservation
settings are shared across applicable actions, with unavailable options explained
by backend capabilities. Saved profiles store choices, not stale permissions
to overwrite or delete files. Initial delivery is user initiated; unattended
scheduling requires separate lifecycle and notification acceptance.

### Comparison and operation options

Offer source-newer, size-different, size-and-time quick comparison and full-content
comparison with plain descriptions. Size-only and quick comparison indicate
"same selected attributes", never "identical content". Two-way reconciliation
must not infer unchanged content from size/time alone when that could silently
overwrite an independently changed version; a fast mode must expose its weaker
detection guarantee and cannot be the correctness default.

Show both timestamps with their available precision. Timestamp tolerance is an
explicit profile setting tied to provider precision; do not infer clock order
across machines or silently compensate for daylight saving time. A "newer wins"
conflict rule is an explicit, potentially lossy policy, not a conflict detector.

Advanced sections cover include/exclude patterns with sample matches; file type,
size and date bounds; recursion and mount boundaries; link handling; ordinary
permissions/times and supported security metadata; content verification;
version retention; bounded retries, concurrency and bandwidth. Date bounds say
which timestamp they use. Metadata capabilities and requested loss concessions
are visible before execution. Ordinary copy must not silently enable metadata
loss to bypass a backend refusal.

An explicitly selected ordinary-copy policy may request ordinary permission bits,
access/modification times and new caller ownership without claiming preservation
of original ownership, hidden attribute namespaces, creation/change times or
physical representation. It is separate from complete metadata preservation;
qualifying the ordinary policy does not satisfy the complete-policy gate. Keep
the metadata-loss override disabled. The plan/result must report requested and
achieved fields, exclusions, unknowns and provider limitations structurally.

Execution qualification must cover visible extended attributes, ACLs, special
mode bits and other representations excluded by an ordinary policy. Their
presence must lead to an explicit supported assessment or refusal, never an
implicit preservation claim. Caller UID/GID behavior versus inherited
destination group must be explicit. Required permissions and timestamp
precision must be verified or refused, never silently weakened. Destination
inherited security state must also be qualified before publication.

Ordinary-copy qualification alone does not authorize move source deletion or
destructive replacement: metadata excluded by that policy may exist only on the
object being removed. Those operations require their own explicit policy,
retention semantics and acceptance tests. The UI must not silently adopt an
ordinary scope as a workaround for a complete-preservation refusal.

Exclusions protect excluded counterparts from deletion. An incomplete inventory,
unreadable subtree or unknown comparison cannot authorize deletion or establish
content equality. Selection scope is part of the plan, not an implicit whole-root
mirror. Recovery files, baseline stores and engine staging areas are outside the
sync namespace or explicitly protected by the engine.

### Two-way reconciliation rules

Use two named folders, Left and Right, without a privileged source. Each pair
has a durable versioned baseline of the last verified reconciled state, including
absence records needed to identify deletions. Bind it to root/provider identity,
scope and comparison/preservation policy. A pathname alone does not identify a
removable volume or a mounted share.

| State relative to baseline | Proposed action |
| --- | --- |
| First run; present on one side only | Copy to the other side; never infer a deletion |
| First run; different versions on both sides | Conflict; retain both until a decision |
| Only Left changed, Right unchanged | Copy Left to Right |
| Only Right changed, Left unchanged | Copy Right to Left |
| Both changed to verified equivalent state | Mark reconciled without replacement |
| Both changed differently | Conflict |
| One deleted, other unchanged | Propagate deletion only if enabled and reviewed; otherwise require an explicit keep/restore decision |
| One deleted, other modified | Deletion-versus-edit conflict |
| Both deleted with complete accessible inventories | Record converged absence |
| Either side unavailable or inventory incomplete | Block affected scope; do not treat it as empty |
| File/directory conflict or directory deletion versus changed descendant | Conflict across the affected subtree |

Equality includes the metadata required by the selected policy; equal bytes do
not resolve conflicting required permissions. A skipped deletion remains pending
and is not silently adopted into the baseline. Baseline loss/corruption, changed
filters or replaced roots require an explicit reconciliation workflow. Rebuilding
history does not infer historical deletions. Keep the former baseline available
for diagnosis until a reviewed replacement is durable.

Do not guess renames from names or modification times. Proven identity/content
may support an optimization, but ambiguous renames, rename-versus-edit and name
normalization collisions require review. Otherwise propose safe equivalent
copy/removal operations with their complete consequences visible.

### Planned two-way presentation and execution

The current helper uses Source/Destination fields and has no two-way mode.
The following Left/Right interaction is a requirement for a future two-way
workflow, not a description of the delivered window.

The proposed Synchronize Folders window has editable Left and Right folder fields,
a mode selector (both ways, one way, mirror), a saved-profile selector and a
Compare button. Picking a one-way mode shows a conspicuous direction arrow;
switching direction requires a fresh comparison. First-run text explains that
there is no history yet and no historical deletion will be propagated.

The comparison view contains a paged tree/table:

| Path | Left | Proposed action | Right | Reason |
| --- | --- | --- | --- | --- |
| report.txt | Changed | Copy to Right | Unchanged | Left changed since last sync |
| budget.csv | Changed | Resolve conflict | Changed | Both versions changed |
| old.txt | Deleted | Review deletion on Right | Unchanged | Deletion propagation enabled |
| notes.txt | Absent | Copy to Left | Added | New on Right |

Each row shows observed presence, sizes/times, comparison strength,
requested action and its backend reason. Filters select All differences,
Conflicts, Deletions, Skipped or Errors. Totals distinguish scanned, planned and
unknown counts; scanning an unknown tree uses indeterminate progress.

Normal profile-selected changes receive one plan confirmation, without an
approval prompt for every file. Setup presets and reusable conflict rules make
large jobs practical. The future execution UI must expose only backend-qualified
choices:

| Choice | Required behavior |
| --- | --- |
| Ask / defer | Leave unresolved items pending without mutation |
| Use source / replace target | Replace eligible destination objects |
| Keep target / skip existing | Preserve existing destination objects |
| Use newest | Compare supported timestamps; use an explicit fallback for ties or unknown order |
| Strict source-newer | Replace only when the source is provably newer |
| Source-newer or size-different at equal time | Also replace equal-time files whose sizes differ |
| Replace if size differs | Apply only a size comparison; do not imply content equality |
| Prefer larger / prefer smaller | Advanced, explicit size preference with tie/unknown fallback |
| Keep both — rename incoming | Preserve target under its current name; preview a collision-free incoming name |
| Keep both — rename existing | Preserve target under a reviewed new name; publish incoming under the original name |
| Manual rename | Validate the chosen name and recheck collision preconditions |
| Skip / skip matching | Retain unresolved baseline state where appropriate |
| Compare / staged merge | Inspect content or review validated staged merge output |
| Merge folders | Combine eligible children under independently evaluated child rules |
| Retry / retry matching | Use bounded attempts and backoff for eligible transient failures |
| Cancel | Request cancellation and settle safely |

Zero-length source protection and preservation of replaced versions are separate
options, independent of replacement preference. Empty files can be intentional;
protection must report the skipped item rather than silently claiming completion.
Appending or regetting requires verified resume support, never a blanket
overwrite rule. Tie and unknown fallbacks are explicit Ask, Keep target or Keep
both choices where meaningful; they never bypass missing identity or safety data.

Two-way controls use Keep Left, Keep Right, Newest and Keep Both labels.
"Keep Both" previews deterministic collision-free names and resulting
placement on each side. Its two rename choices remain distinct in setup,
preview and in-progress conflict controls. In two-way mode, first show which
side's version will occupy each resulting name; do not leave "incoming"
ambiguous. Renaming the existing object requires the engine to journal and
verify both preservation and incoming publication before accepting completion.
Deletion conflicts use explicit labels such as Restore
deleted version, Keep edited version or Delete edited version, with recoverable preservation before
destructive resolution. Required metadata/type conflicts get applicable choices,
not meaningless file-content buttons. External compare/merge programs receive
explicit argument arrays, never interpolated shell commands; merged output is
staged and revalidated before publication.

Rule scope can be this item, selected rows, a matching conflict class within a
folder/subtree, or all matching eligible items in the job. Optional path/type
filters refine the scope. Show matched counts and consequences, and maintain an
editable active-rule list. Offer "Apply to remaining matching items" after a
representative review. A job-wide choice is valid for declared eligible classes;
it does not override separate security or deletion policies.

Precedence is explicit: item decisions override selected-row/subtree rules,
which override job/profile defaults. Overlapping rules at the same precedence
must be corrected explicitly, never depend on insertion order. Value ties and
unknown comparison values use the declared fallback only where applicable.
Core policy evaluation supplies classifications, matched counts,
reasons and identity-bound receipts. Saved rules express preferences; newly
observed items require new backend evaluation and receipts, without repetitive
user prompts when the existing rule safely applies. Unknown/stale identities,
incomplete scans, ambiguous commits and unsafe overlap block affected actions.

Conflict-free independent paths may run while
unresolved paths and their dependent ancestors remain blocked; the final state
is partial until those paths are resolved.

The footer summarizes copies in each direction, replacements, recoverable
deletions, bytes and unresolved conflicts. Review Deletions opens a dedicated
list. User-configured absolute and percentage deletion limits are independent;
exceeding either blocks execution until the exact plan receives explicit review.
Execution revalidates identities and preconditions; stale rows return to review.

Progress reports phase, current path, transferred bytes, completed/skipped/failed
items and known totals. Pause has a defined safe boundary; Cancel immediately
acknowledges the request, then displays "Cancelling — finishing safe cleanup".
The UI remains responsive until a terminal result. Closing presentation does
not destroy or join a running worker on the Xt thread. A job controller owns
the operation independently of its widgets; worker events are bounded/coalesced
and rendered only on the Xt thread.

Results distinguish Completed, Partial, Cancelled, Failed and Recovery Required.
Show preserved versions and recovery locations, affected paths and permitted
retry actions. Keep skipped/failed selections available for another attempt.
Resume means revalidate an engine journal, not reuse an untrusted partial file.
Keyboard traversal, text action labels, accessible contrast and translated
catalog messages are required; arrows and colors are supplementary cues.

### Shared-engine ownership and durability

ck-core-fs-lib owns inventory, classification, policy, plan generation, conflict
receipts, execution, preservation and recovery through its installed C ABI.
dtfile owns presentation, user choices and lifecycle integration. It must not
reimplement reconciliation rules or invoke CLI tools as the operation engine.

Use opaque session/baseline/plan handles, bounded asynchronous pages and events,
typed unknown/unsupported states, explicit lifetimes and versioned options.
One identity-bound plan supplies both preview and execution. Concurrent sessions
must coordinate overlapping roots, including differently named pairs sharing a
root; a pair-name lock alone is insufficient. Revalidate inputs before mutation.

The native C binding must connect asynchronous held-object inventory to a paged
plan, scoped rule decisions and execution of that exact reviewed generation.
Changing a rule creates a new generation and invalidates superseded receipts.
Caller-supplied preview facts cannot be promoted into native mutation authority.
Expose byte-native POSIX names and UTF-16 Windows names separately from display
labels, with explicit ownership and lifetime rules. Typed capability/refusal
codes drive UI choices; diagnostic strings are not a machine interface.

Preview rows or companion bounded assessments must expose both sides' observed
presence/kind, size, times and precision, requested metadata assessment,
permitted choices, matching-rule counts and consequences. Enum membership alone
does not establish that a choice is available. Manual rename needs exact native
name input; both Keep Both variants need exact resulting-name/side copy-outs
before execution. Include preserved-target locations in authoritative results.

Command admission must either allow only one unacknowledged in-flight request
or retain authoritative outcomes addressable by command ID. A latest-command
snapshot and lossy event queue alone cannot guarantee delivery of every command
outcome. Distinguish a pause request from reaching a safe paused boundary.
Copy/validate bounded command payloads outside the snapshot mutex, then recheck
generation and admission during a short critical section. Build new row caches
off-lock, swap under lock and reclaim superseded caches off-lock. Validate
unsigned operands against each limit before computing remaining capacity.

Started jobs own all required plan, entry and session lifetimes. Cancel, pause
and resume requests must not block the Xt thread. Event overflow requires a
snapshot/resynchronization mechanism that retains authoritative conflict,
recovery and terminal states. Provide per-item dispositions, retention locations
and safe retry eligibility. Any blocking destruction/join belongs to a non-UI
reaper with defined shutdown behavior, not a widget destroy callback.

Native rename, Trash and unlink require an honest source-identity contract.
A name check followed by a mutation syscall leaves a substitution window;
successful completion does not prove that the reviewed object was mutated.
Distinguish provider-native conditional mutation from operations requiring an
explicitly constrained exclusive-parent protocol. Cooperative locks do not
protect against noncooperating parent writers. A post-mutation identity check
alone cannot safely undo arbitrary wrong-object mutation. Unsupported guarantees
must be reported without claiming the operation is implemented for that scope.
The dtfile adapter must not bypass a refusal with its own path-based fallback.

Journal publication and advance baseline entries only after verified convergence
and required durability. Keep unresolved entries at their previous state. A
crash between filesystem publication and baseline commit must recover without
mistaking the completed action for an unrelated new edit. No atomic transaction
across two arbitrary filesystem roots is promised. Retention cleanup must not
remove the only recoverable version of an unresolved operation. Report low-space,
permission, security-metadata and flush failures explicitly.

## Acceptance and delivery

The controller checks below used installed ck-core-fs-lib 0.3 revision
`39725163e43530b9ae35e9fed55f4fedb4d759b6` and matched CK Core Motif ABI 9
revision `1a2407723eb319ac5c18afa0fb386cf46aa7efdc`. Results apply to the
listed cases, not all metadata, rule or execution combinations.

The Linux production-controller checks cover real 70-row inventory and paging,
stale-token rejection, command admission/acknowledgement and counts, page-cache
coherence, pause/resume, cancellation and cleanup without applying changes.
Date parsing and bounds are checked independently of display time zone. A
separate ordinary-copy controller case executes a reviewed plan and verifies
all 70 destination contents, permission modes and modification times, with
`COMPLETE` and actual worker drain. This headless execution fixture does not
enable the graphical helper's Apply gate or qualify mirror/two-way execution.

Native GUI inspection exercised actual profile save/load/delete, comparison
paging, date/byte fields, Help, asynchronous drain and unchanged folder data.
Notebook tabs are at the top, page contents scroll independently, and initial
window dimensions are bounded by the screen to keep the bottom actions
visible. The layout was inspected on a 1280-pixel-wide native test screen.
Help and Delete confirmation have descriptive dialog titles, and operation
errors remain visible. These checks cover those interactions; Apply stays
compile-time disabled pending execution qualification.

A native Copy/Ordinary review with a JOB-scoped Keep target rule and both
nonempty protection and replaced-version retention enabled showed one matched,
one resolved and zero unresolved cases. Its preview reported 69 copies plus
one skip and 681 bytes. Graphical Apply remained disabled; these were reviewed
plan counts, not executed filesystem changes.

The standalone production profile regression passes on macOS and Linux with
warnings-as-errors and AddressSanitizer/UndefinedBehaviorSanitizer. Its cases
cover exact-byte/timestamp round trips, updates/deletion, concurrent merging,
corrupt-store preservation, bounds, unsafe links, private modes, cancellation,
and parent flush on both creation and concurrent adoption. Native macOS cases
also exercise outsider/inherited ACL grants, ACL-query failures and access-path
checks before writes. Positive Mac fixtures use ownership-enforcing APFS; the
separate `noowners` fixture verifies refusal. LeakSanitizer is unsupported on
that Mac runtime. Preference qualification does not qualify arbitrary provider
security or tree execution.

Read-only Compare with Complete metadata and an Ask rule accepts review without
`HOME`, a journal root or an operation ID. Copy/Sync mutation credentials remain
required. File Manager's menu launches the absolute helper executable with the
current local folder as a single native argument. The helper closes inherited
cache/IPC descriptors before opening its own resources.

Default backend-disabled CDE configuration and component checks also pass with
the global configuration header's bytes and timestamp unchanged. A private generated feature header
tracks menu dependencies across configuration toggles. Optional-helper compilation,
headless controller checks and actual native-window inspection are separate
validation steps; none substitutes for the others.

Remaining delivery gates are:

1. Keep installed C ABI, controller and consuming GUI checks tied to the
   selected backend/toolkit revisions. Extend coverage across offered metadata,
   profile and rule combinations, and verify actual worker drain on shutdown.
   Retain failing regression cases until fixed.
2. Qualify native recursive copy/update and one-way execution with preservation,
   filter, stale-plan, command-delivery and fault-injection coverage before
   enabling Apply in the consuming window.
3. Qualify deletion, replaced-version retention and recovery independently
   before enabling mirror or other destructive scopes.
4. Qualify durable two-way baselines, overlap coordination, restart and recovery
   before exposing two-way reconciliation.

Conflict-policy tests cover every offered choice, tie/unknown fallback, rule
scope and precedence; incoming/existing rename collisions; zero-length and
preserved-version policies; matched-count accuracy; changing an active rule;
and rejection of broad rules for ineligible security/deletion/identity cases.
Exercise selection limits at the limit, one past it and the maximum integer;
coalesced events with multiple submitted commands; pause during native I/O;
manual rename and both Keep Both resulting-name copy-outs; and snapshot access
while preparing a maximum-sized rule batch. Invalid payloads must not change
the active plan, consume mutation authority or leave nonzero output handles.
Confirm that one reviewed profile plan can execute many eligible changes without
per-file prompts while stale decisions still fail closed.

Mandatory two-way tests cover every reconciliation table row; identical and different concurrent
edits; same-size/same-time changed bytes; directory deletion versus edited child;
rename/edit and type changes; case/normalization collisions; hard links and link
swaps; disconnected/replaced mounts; unreadable trees; changed filters; missing,
corrupt and incompatible baselines; competing sessions and overlapping roots;
stale receipts; interrupted read/write/publish/flush/baseline commit; cancellation;
full disks; retention exhaustion; metadata failures and idempotent recovery.
Inject source-name and ancestor substitution deterministically at publication,
retirement, Trash and recovery boundaries. Assert unrelated objects remain
unchanged and recovery never blindly renames or unlinks a substituted path.
Assert final bytes, required metadata, retained versions and baseline state.
No loss claim may be based solely on a successful job status.

Group implementation into ten cohesive repair rounds before the combined
incremental checkpoint. Reuse configured builds and compile changed objects and
dependencies; pin the established Motif version throughout that checkpoint.
Record source/output identities and elapsed times. Re-run a gate earlier only
when its failure is needed to establish a contract or unblock integration.
After shared tests and actual native dtfile workflow inspection pass, commit
cohesive CDE fixes with CHANGES and documentation and push to rpi4 and GitHub.
Refresh Motif only at the next round boundary; refer toolkit defects to its peer.

## Research basis

These tools inform the design; the requirements above are this project's
proposed contract, not claims that every cited tool has the same semantics.

- [Robocopy](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/robocopy)
  provides detailed selection, metadata, retry and restart controls; mirror
  includes removal, so our UI gives it a distinct destructive scope.
- [rsync](https://rsync.samba.org/ftp/rsync/rsync.1) separates quick comparison,
  checksum comparison, update rules, filtering and deletion. Its ordinary
  one-direction operation does not supply two-way history.
- [Midnight Commander](https://source.midnight-commander.org/man/mc.html) and
  [Total Commander](https://www.ghisler.com/advanced.htm) inform efficient
  comparison, conflict and multi-rename interaction.
- [Unison manual](https://github.com/bcpierce00/unison/blob/master/doc/unison-manual.tex)
  describes per-pair archives, change detection against prior state and conflicts
  when independently changed replicas differ.
- [rclone bisync](https://rclone.org/bisync/) documents prior listings, access
  checks, filter-change hazards, deletion limits and recovery safeguards.
- [Syncthing synchronization](https://docs.syncthing.net/users/syncing) and
  [versioning](https://docs.syncthing.net/users/versioning) inform conflict-copy,
  content verification and retention considerations; retained versions are not a
  substitute for an independent backup.
