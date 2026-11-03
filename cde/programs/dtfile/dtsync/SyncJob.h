/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef DTFILE_SYNC_JOB_H
#define DTFILE_SYNC_JOB_H
#include <ck_fs_tree.h>
#define SYNC_PAGE_SIZE 64
#define SYNC_MAX_RULES 64
#define SYNC_DISPLAY_PATH 2048
typedef struct SyncJob SyncJob;
typedef struct {
    ckfet_row_v1 observed;
    char path[SYNC_DISPLAY_PATH];
    char proposed[1028], retained[1028];
    char journal_name[1028], staging_name[1028];
    uint32_t name_failures;
} SyncRow;
typedef struct {
    ckfet_snapshot_v1 snapshot;
    ckfet_rule_counts_v1 counts[SYNC_MAX_RULES];
    SyncRow rows[SYNC_PAGE_SIZE];
    uint64_t revision, offset, total_rows, next_offset;
    unsigned row_count, has_next, ready, finished, stopped;
    unsigned cache_valid, names_valid, command_accepted, settling, drained;
    uint64_t cache_generation, cache_receipt, completed_submission, review_submission;
    ckfe_api_status command_api;
    uint64_t planned_copies, planned_folders, planned_replacements, planned_removals;
    uint64_t planned_skips, planned_reports, planned_bytes, summary_generation, summary_receipt;
    unsigned summary_valid, planned_bytes_known;
    uint64_t class_choices[15], class_rows[15];
    ckfe_api_status api;
    char error[512], journal[SYNC_DISPLAY_PATH];
} SyncView;
static inline int SyncJobViewCoherent(const SyncView *view)
{
    return view->cache_valid && view->names_valid && view->cache_generation==view->snapshot.plan_generation &&
        view->cache_receipt==view->snapshot.receipt_id;
}
typedef struct {
    uint32_t kind, choice, tie, scope, protect_nonempty, retain_replaced;
    uint64_t selected[SYNC_PAGE_SIZE];
    unsigned selected_count;
    /* Opaque subtree key is copied from the backend, never parsed as a path. */
    unsigned char *subtree;
    size_t subtree_size;
} SyncRule;
typedef struct {
    const char *source, *destination;
    uint32_t profile, metadata_scope;
    ckfsp_filters filters;
} SyncSetup;
/* All native I/O and library handles are confined to an owned worker. */
SyncJob *SyncJobStart(const SyncSetup *, char *error, size_t error_size);
/* Nonblocking snapshots: 0 means worker is publishing; retain the prior view. */
int SyncJobRead(SyncJob *, SyncView *);
int SyncJobPage(SyncJob *, uint64_t offset);
uint64_t SyncJobReview(SyncJob *, uint64_t generation, const SyncRule *, unsigned count,
                  const uint64_t *deletions, unsigned deletion_count,
                  uint64_t rename_row, const char *rename, uint32_t rename_side);
uint64_t SyncJobExecute(SyncJob *, uint64_t generation, uint64_t receipt);
void SyncJobPause(SyncJob *, int paused);
void SyncJobCancel(SyncJob *);
/* Stop requests cancellation. Destroy succeeds only after worker cleanup. */
void SyncJobStop(SyncJob *);
int SyncJobDestroy(SyncJob *);
#endif
