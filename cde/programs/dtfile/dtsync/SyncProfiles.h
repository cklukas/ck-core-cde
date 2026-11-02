/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef DTFILE_SYNC_PROFILES_H
#define DTFILE_SYNC_PROFILES_H
#include <stddef.h>
#include <stdint.h>
#include <ck_fs_tree.h>

#define SYNC_PROFILES_VERSION 1u
#define SYNC_PROFILES_MAX 64u
#define SYNC_PROFILE_NAME_MAX 64u
#define SYNC_PROFILE_PATH_MAX 65536u
#define SYNC_PROFILE_RULES_MAX 64u
#define SYNC_PROFILE_DEPTH_MAX UINT64_C(65536)

/* Preferences only. Every rule implicitly has JOB scope. There is deliberately
 * no field for a row, generation, receipt, subtree, selection or authority.
 * Enum values use CKFSP_* profile / CKFET_* conflict, choice and tie constants;
 * metadata_scope is 0 (complete) or 1 (ordinary). Compare and Review afresh.
 */
typedef struct {
    uint32_t kind, choice, tie, protect_nonempty, retain_replaced;
} SyncProfileRule;
typedef struct {
    uint32_t version;
    /* Nonempty UTF-8 UI label, no control characters, at most 64 bytes. */
    char name[SYNC_PROFILE_NAME_MAX + 1];
    /* Absolute, NUL-terminated exact POSIX bytes. Not display-escaped text. */
    char source[SYNC_PROFILE_PATH_MAX + 1];
    char destination[SYNC_PROFILE_PATH_MAX + 1];
    uint32_t profile, metadata_scope;
    ckfsp_filters filters;
    uint32_t rule_count;
    SyncProfileRule rules[SYNC_PROFILE_RULES_MAX];
} SyncProfile;

typedef enum {
    SYNC_PROFILES_OK = 0, SYNC_PROFILES_BUSY, SYNC_PROFILES_NOT_READY,
    SYNC_PROFILES_NOT_FOUND, SYNC_PROFILES_INVALID, SYNC_PROFILES_LIMIT,
    SYNC_PROFILES_IO, SYNC_PROFILES_CORRUPT, SYNC_PROFILES_UNSAFE,
    SYNC_PROFILES_NOMEM, SYNC_PROFILES_STOPPED, SYNC_PROFILES_CANCELLED,
    /* Rename committed, but directory flush failed: persistence is uncertain.
     * The last visible DB is published; do not claim the save was durable. */
    SYNC_PROFILES_DURABILITY
} SyncProfilesResult;
typedef enum {
    SYNC_PROFILES_LOADING, SYNC_PROFILES_READY, SYNC_PROFILES_SAVING,
    SYNC_PROFILES_ERROR, SYNC_PROFILES_FINISHED
} SyncProfilesState;
typedef struct {
    uint64_t revision, submitted, completed;
    SyncProfilesState state;
    SyncProfilesResult result;
    uint32_t count;
    char names[SYNC_PROFILES_MAX][SYNC_PROFILE_NAME_MAX + 1];
    char error[512];
} SyncProfilesSnapshot;
typedef struct SyncProfiles SyncProfiles;

/* Starts an owned worker. Only bounded memory allocation/copy and thread setup
 * occur here. All native config I/O is on that worker, never the calling UI.
 * Store: <absolute home>/.dtfile-sync/profiles.v1; owner-only directory/files.
 * Home is resolved component-by-component without following symbolic links.
 * Ancestors must be root/caller-controlled (root group-zero administration and
 * sticky directories are trusted). Darwin extended ACLs are checked on held
 * descriptors; unknown ACLs and grants outside root/caller fail closed. Home
 * and private objects also exclude read/inheritable grants to other identities.
 * Parent-directory durability is established for both creation and adoption.
 * Invalid home/startup allocation errors are returned in error. No getenv.
 */
SyncProfiles *SyncProfilesStart(const char *home, char *error, size_t error_size);
/* Nonblocking trylock; BUSY leaves output unchanged. Snapshot is < 5 KiB;
 * GetProfile copies at most one bounded profile (about 130 KiB), not the DB.
 * Caller owns output; no borrowed pointers and no I/O. Check snapshot.result
 * after completion. Cached names/profiles survive a later load/save failure.
 */
SyncProfilesResult SyncProfilesGetSnapshot(SyncProfiles *, SyncProfilesSnapshot *);
SyncProfilesResult SyncProfilesGetProfile(SyncProfiles *, const char *name, SyncProfile *);
/* Validate + copy one bounded payload. Only one in-flight command is admitted.
 * Returns OK when queued, not persisted. Poll completed == submitted and result.
 * Save locks cooperatively, reloads/merges latest DB, then commits atomically.
 * Different names merge; concurrent same-name edits are last successful writer.
 * No protection is claimed against malicious processes running as the same UID.
 */
SyncProfilesResult SyncProfilesSave(SyncProfiles *, const SyncProfile *);
/* Same asynchronous completion contract. Missing names report NOT_FOUND. */
SyncProfilesResult SyncProfilesDelete(SyncProfiles *, const char *name);
SyncProfilesResult SyncProfilesReload(SyncProfiles *);
/* Stop is nonblocking and cancels lock waits. An atomic commit already underway
 * finishes and reports its result. Destroy returns 0 until the worker has
 * finished native I/O; then joins an already-finished worker and releases memory.
 * Calls are UI-thread-owned; do not race Destroy with another API call.
 */
void SyncProfilesStop(SyncProfiles *);
int SyncProfilesDestroy(SyncProfiles *);
#endif
