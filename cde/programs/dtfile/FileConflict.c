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

#include "FileConflict.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int
ConflictOperationValid(DtfileConflictOperation operation)
{
    return operation >= DTFILE_CONFLICT_COPY &&
           operation <= DTFILE_CONFLICT_BUFFER;
}

static int
ConflictPathValid(const char *bytes, size_t length)
{
    return bytes != NULL && length != 0 &&
           length <= DTFILE_CONFLICT_PATH_LIMIT && bytes[0] == '/' &&
           memchr(bytes, 0, length) == NULL;
}

static int
ConflictNameValid(const char *name)
{
    size_t length;
    if (name == NULL || name[0] == 0 || strchr(name, '/') != NULL ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    length = strnlen(name, DTFILE_CONFLICT_PATH_LIMIT + 1);
    return length <= DTFILE_CONFLICT_PATH_LIMIT;
}

int
DtfileConflictBatchInit(DtfileConflictBatch *batch,
                        DtfileConflictOperation operation,
                        size_t selected_count, size_t count)
{
    if (batch == NULL) { errno = EINVAL; return -1; }
    memset(batch, 0, sizeof(*batch));
    if (!ConflictOperationValid(operation) || count == 0 ||
        selected_count < count || selected_count > INT_MAX)
    { errno = EINVAL; return -1; }
    if (count > DTFILE_CONFLICT_ITEM_LIMIT ||
        count > SIZE_MAX / sizeof(*batch->items))
    { errno = E2BIG; return -1; }
    batch->items = calloc(count, sizeof(*batch->items));
    if (batch->items == NULL) return -1;
    batch->operation = operation;
    batch->selected_count = selected_count;
    batch->count = count;
    return 0;
}

void
DtfileConflictBatchFree(DtfileConflictBatch *batch)
{
    size_t i;
    if (batch == NULL) return;
    for (i = 0; i < batch->count; ++i)
    {
        free(batch->items[i].source);
        free(batch->items[i].target);
    }
    free(batch->items);
    memset(batch, 0, sizeof(*batch));
}

int
DtfileConflictSetItem(DtfileConflictBatch *batch, size_t slot,
                      uint32_t request_index, unsigned int flags,
                      const char *source, size_t source_length,
                      const char *target, size_t target_length)
{
    char *source_copy = NULL, *target_copy;
    size_t bytes;
    if (batch == NULL || batch->items == NULL || slot >= batch->count ||
        request_index >= batch->selected_count ||
        batch->items[slot].target != NULL ||
        (flags & ~(DTFILE_CONFLICT_SAME_ENTRY | DTFILE_CONFLICT_CAN_MERGE)) != 0 ||
        ((flags & DTFILE_CONFLICT_SAME_ENTRY) && (flags & DTFILE_CONFLICT_CAN_MERGE)) ||
        ((flags & DTFILE_CONFLICT_CAN_MERGE) && batch->operation != DTFILE_CONFLICT_COPY) ||
        !ConflictPathValid(target, target_length))
    { errno = EINVAL; return -1; }
    if (batch->operation == DTFILE_CONFLICT_BUFFER)
    {
        if (source != NULL || source_length != 0 || flags != 0)
        { errno = EINVAL; return -1; }
    }
    else if (!ConflictPathValid(source, source_length))
    { errno = EINVAL; return -1; }
    /* Both lengths have already been bounded independently. */
    bytes = source_length + target_length + 2;
    if (batch->native_bytes > DTFILE_CONFLICT_BYTES_LIMIT ||
        bytes > DTFILE_CONFLICT_BYTES_LIMIT - batch->native_bytes)
    { errno = E2BIG; return -1; }
    if (source != NULL)
    {
        source_copy = malloc(source_length + 1);
        if (source_copy == NULL) return -1;
        memcpy(source_copy, source, source_length);
        source_copy[source_length] = 0;
    }
    target_copy = malloc(target_length + 1);
    if (target_copy == NULL) { free(source_copy); return -1; }
    memcpy(target_copy, target, target_length);
    target_copy[target_length] = 0;
    batch->items[slot].request_index = request_index;
    batch->items[slot].flags = flags;
    batch->items[slot].source = source_copy;
    batch->items[slot].target = target_copy;
    batch->native_bytes += bytes;
    return 0;
}

static int
ConflictIndexCompare(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

int
DtfileConflictBatchValidate(const DtfileConflictBatch *batch)
{
    uint32_t *indices;
    size_t i, bytes = 0;
    int valid = 1;
    if (batch == NULL || !ConflictOperationValid(batch->operation) ||
        batch->items == NULL || batch->count == 0 ||
        batch->count > DTFILE_CONFLICT_ITEM_LIMIT ||
        batch->selected_count < batch->count || batch->selected_count > INT_MAX ||
        batch->native_bytes > DTFILE_CONFLICT_BYTES_LIMIT)
    { errno = EINVAL; return -1; }
    indices = malloc(batch->count * sizeof(*indices));
    if (indices == NULL) return -1;
    for (i = 0; i < batch->count; ++i)
    {
        const DtfileConflictItem *item = &batch->items[i];
        size_t source_length = item->source == NULL ? 0 :
                 strnlen(item->source, DTFILE_CONFLICT_PATH_LIMIT + 1);
        size_t target_length = item->target == NULL ? 0 :
                 strnlen(item->target, DTFILE_CONFLICT_PATH_LIMIT + 1);
        if (item->target == NULL || item->request_index >= batch->selected_count ||
            !ConflictPathValid(item->target, target_length) ||
            (batch->operation == DTFILE_CONFLICT_BUFFER
              ? item->source != NULL || item->flags != 0
              : !ConflictPathValid(item->source, source_length)) ||
            (item->flags & ~(DTFILE_CONFLICT_SAME_ENTRY | DTFILE_CONFLICT_CAN_MERGE)) != 0 ||
            ((item->flags & DTFILE_CONFLICT_CAN_MERGE) &&
             (batch->operation != DTFILE_CONFLICT_COPY ||
              (item->flags & DTFILE_CONFLICT_SAME_ENTRY))))
        { valid = 0; break; }
        if (source_length + target_length + 2 > DTFILE_CONFLICT_BYTES_LIMIT - bytes)
        { valid = 0; break; }
        bytes += source_length + target_length + 2;
        indices[i] = item->request_index;
    }
    if (bytes != batch->native_bytes) valid = 0;
    if (valid)
    {
        qsort(indices, batch->count, sizeof(*indices), ConflictIndexCompare);
        for (i = 1; i < batch->count; ++i)
            if (indices[i] == indices[i - 1]) { valid = 0; break; }
    }
    free(indices);
    if (!valid) { errno = EINVAL; return -1; }
    return 0;
}

DtfileConflictAction
DtfileConflictDefaultAction(const DtfileConflictBatch *batch, size_t slot)
{
    if (batch == NULL || batch->items == NULL || slot >= batch->count)
        return DTFILE_CONFLICT_SKIP;
    if (!(batch->items[slot].flags & DTFILE_CONFLICT_SAME_ENTRY))
        return DTFILE_CONFLICT_REPLACE;
    return batch->operation == DTFILE_CONFLICT_MOVE
           ? DTFILE_CONFLICT_SKIP : DTFILE_CONFLICT_KEEP_BOTH;
}

int
DtfileConflictActionAllowed(const DtfileConflictBatch *batch, size_t slot,
                            DtfileConflictAction action)
{
    const DtfileConflictItem *item;
    if (batch == NULL || batch->items == NULL || slot >= batch->count ||
        !ConflictOperationValid(batch->operation) ||
        action < DTFILE_CONFLICT_REPLACE || action > DTFILE_CONFLICT_SKIP)
        return 0;
    item = &batch->items[slot];
    if ((item->flags & DTFILE_CONFLICT_SAME_ENTRY) &&
        (action == DTFILE_CONFLICT_REPLACE || action == DTFILE_CONFLICT_RENAME_EXISTING ||
         (batch->operation == DTFILE_CONFLICT_MOVE && action == DTFILE_CONFLICT_KEEP_BOTH)))
        return 0;
    return action != DTFILE_CONFLICT_MERGE || (item->flags & DTFILE_CONFLICT_CAN_MERGE);
}

int
DtfileConflictChangesExisting(const DtfileConflictBatch *batch, size_t slot,
                              DtfileConflictAction action)
{
    if (!DtfileConflictActionAllowed(batch, slot, action)) return 0;
    return action == DTFILE_CONFLICT_REPLACE ||
           action == DTFILE_CONFLICT_RENAME_EXISTING;
}

int
DtfileConflictDecisionValidate(const DtfileConflictBatch *batch, size_t slot,
                               const DtfileConflictDecision *decision)
{
    int manual;
    if (decision == NULL || batch == NULL || batch->items == NULL || slot >= batch->count)
    { errno = EINVAL; return -1; }
    manual = decision->action == DTFILE_CONFLICT_RENAME_EXISTING ||
             decision->action == DTFILE_CONFLICT_RENAME_INCOMING;
    if (decision->request_index != batch->items[slot].request_index ||
        !DtfileConflictActionAllowed(batch, slot, decision->action) ||
        (manual && !ConflictNameValid(decision->name)) ||
        (!manual && decision->name != NULL))
    { errno = EINVAL; return -1; }
    return 0;
}

void
DtfileConflictDecisionsFree(DtfileConflictDecision *decisions, size_t count)
{
    size_t i;
    if (decisions == NULL) return;
    for (i = 0; i < count; ++i) free(decisions[i].name);
    free(decisions);
}

char *
DtfileConflictDisplayPath(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t length, i, used = 0;
    char *display;
    if (path == NULL) { errno = EINVAL; return NULL; }
    length = strnlen(path, DTFILE_CONFLICT_PATH_LIMIT + 1);
    if (length > DTFILE_CONFLICT_PATH_LIMIT || length > (SIZE_MAX - 1) / 4)
    { errno = E2BIG; return NULL; }
    display = malloc(length * 4 + 1);
    if (display == NULL) return NULL;
    for (i = 0; i < length; ++i)
    {
        unsigned char ch = (unsigned char)path[i];
        if (ch >= 32 && ch <= 126 && ch != '\\') display[used++] = (char)ch;
        else
        {
            display[used++] = '\\'; display[used++] = 'x';
            display[used++] = hex[ch >> 4]; display[used++] = hex[ch & 15];
        }
    }
    display[used] = 0;
    return display;
}
