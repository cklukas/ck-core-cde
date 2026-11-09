/*
 * CDE - Common Desktop Environment
 *
 * These libraries and programs are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * These libraries and programs are distributed without any warranty; without
 * even the implied warranty of merchantability or fitness for a particular
 * purpose. See the GNU Lesser General Public License for more details.
 */

/* Per-operation messages between dtfile's legacy worker and its UI.
 * Native paths never come back from a formatted widget label. These records
 * express intent; filesystem identity and permission checks belong to the
 * worker immediately before acting, not to this transport model.
 */
#ifndef DTFILE_FILE_CONFLICT_H
#define DTFILE_FILE_CONFLICT_H

#include <stddef.h>
#include <stdint.h>

#define DTFILE_CONFLICT_PATH_LIMIT (64u * 1024u)
#define DTFILE_CONFLICT_ITEM_LIMIT 65536u
#define DTFILE_CONFLICT_BYTES_LIMIT (64u * 1024u * 1024u)

typedef enum {
    DTFILE_CONFLICT_COPY = 1,
    DTFILE_CONFLICT_MOVE,
    DTFILE_CONFLICT_LINK,
    DTFILE_CONFLICT_BUFFER
} DtfileConflictOperation;

enum {
    DTFILE_CONFLICT_SAME_ENTRY = 1u,
    DTFILE_CONFLICT_CAN_MERGE = 2u
};

typedef enum {
    DTFILE_CONFLICT_REPLACE = 1,
    DTFILE_CONFLICT_KEEP_BOTH,
    DTFILE_CONFLICT_RENAME_EXISTING,
    DTFILE_CONFLICT_RENAME_INCOMING,
    DTFILE_CONFLICT_MERGE,
    DTFILE_CONFLICT_SKIP
} DtfileConflictAction;

typedef struct {
    uint32_t request_index;
    unsigned int flags;
    char *source;                 /* NULL only for dropped-buffer data. */
    char *target;                 /* Complete actual target, not a leaf. */
} DtfileConflictItem;

typedef struct {
    DtfileConflictOperation operation;
    size_t selected_count;
    size_t count;
    size_t native_bytes;
    DtfileConflictItem *items;
} DtfileConflictBatch;

typedef struct {
    uint32_t request_index;
    DtfileConflictAction action;
    char *name;                   /* Only explicit manual rename actions. */
} DtfileConflictDecision;

/* Initialize an empty/uninitialized batch. All owned strings/arrays use
 * malloc/free. SetItem copies exact byte spans, rejects embedded NULs, and
 * leaves a slot unchanged on failure. Validate must succeed before a batch
 * is presented or any decision is executed; it rejects duplicate indices.
 * Return 0 on success, -1 with errno on error. No filesystem I/O occurs.
 */
int DtfileConflictBatchInit(DtfileConflictBatch *batch,
                           DtfileConflictOperation operation,
                           size_t selected_count, size_t count);
int DtfileConflictSetItem(DtfileConflictBatch *batch, size_t slot,
                          uint32_t request_index, unsigned int flags,
                          const char *source, size_t source_length,
                          const char *target, size_t target_length);
int DtfileConflictBatchValidate(const DtfileConflictBatch *batch);
void DtfileConflictBatchFree(DtfileConflictBatch *batch);

DtfileConflictAction DtfileConflictDefaultAction(const DtfileConflictBatch *batch,
                                                size_t slot);
int DtfileConflictActionAllowed(const DtfileConflictBatch *batch, size_t slot,
                                DtfileConflictAction action);
int DtfileConflictChangesExisting(const DtfileConflictBatch *batch, size_t slot,
                                  DtfileConflictAction action);
int DtfileConflictDecisionValidate(const DtfileConflictBatch *batch,
                                   size_t slot,
                                   const DtfileConflictDecision *decision);
void DtfileConflictDecisionsFree(DtfileConflictDecision *decisions, size_t count);

/* ASCII presentation only: escape backslash, control and non-ASCII bytes as
 * \xHH. The caller frees the result. Never decode this text to obtain a path.
 */
char *DtfileConflictDisplayPath(const char *path);

#endif
