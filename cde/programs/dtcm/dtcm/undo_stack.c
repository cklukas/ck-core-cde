/*
 * CDE - Common Desktop Environment
 *
 * Bounded 10-entry Undo ring buffer for Calendar Manager (dtcm)
 * delete and drag-reschedule/resize operations.
 */

#include <stdio.h>
#include <string.h>

#include "undo_stack.h"

static DtcmUndoEntry g_ring[DTCM_UNDO_MAX];
static int g_head = 0;  /* next write slot */
static int g_count = 0; /* valid entries in ring [0..DTCM_UNDO_MAX] */

void
dtcm_undo_clear(void)
{
    g_head = 0;
    g_count = 0;
    memset(g_ring, 0, sizeof(g_ring));
}

int
dtcm_undo_count(void)
{
    return g_count;
}

int
dtcm_undo_can_undo(void)
{
    return g_count > 0 ? 1 : 0;
}

static void
push_entry(const DtcmUndoEntry *e)
{
    g_ring[g_head] = *e;
    g_head = (g_head + 1) % DTCM_UNDO_MAX;
    if (g_count < DTCM_UNDO_MAX)
        g_count++;
}

void
dtcm_undo_push_delete(time_t start_time, time_t end_time, int show_time, const char *summary)
{
    DtcmUndoEntry e;
    memset(&e, 0, sizeof(e));
    e.kind = DTCM_UNDO_DELETE;
    e.old_start = start_time;
    e.old_end = end_time;
    e.show_time = show_time;
    if (summary)
        snprintf(e.summary, sizeof(e.summary), "%s", summary);
    push_entry(&e);
}

void
dtcm_undo_push_reschedule(time_t old_start, time_t old_end,
                          time_t new_start, time_t new_end,
                          int show_time, const char *summary)
{
    DtcmUndoEntry e;
    memset(&e, 0, sizeof(e));
    e.kind = DTCM_UNDO_RESCHEDULE;
    e.old_start = old_start;
    e.old_end = old_end;
    e.new_start = new_start;
    e.new_end = new_end;
    e.show_time = show_time;
    if (summary)
        snprintf(e.summary, sizeof(e.summary), "%s", summary);
    push_entry(&e);
}

int
dtcm_undo_pop(DtcmUndoEntry *out)
{
    if (g_count <= 0)
        return 0;
    g_head = (g_head - 1 + DTCM_UNDO_MAX) % DTCM_UNDO_MAX;
    g_count--;
    if (out)
        *out = g_ring[g_head];
    return 1;
}
