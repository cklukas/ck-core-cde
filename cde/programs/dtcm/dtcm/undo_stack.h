#ifndef DTCM_UNDO_STACK_H
#define DTCM_UNDO_STACK_H

#include <time.h>

#define DTCM_UNDO_MAX 10

typedef enum {
    DTCM_UNDO_NONE = 0,
    DTCM_UNDO_DELETE,
    DTCM_UNDO_RESCHEDULE
} DtcmUndoKind;

typedef struct {
    DtcmUndoKind kind;
    time_t old_start;
    time_t old_end;
    time_t new_start;
    time_t new_end;
    int show_time;
    char summary[2048];
} DtcmUndoEntry;

void dtcm_undo_clear(void);
int dtcm_undo_count(void);
int dtcm_undo_can_undo(void);
void dtcm_undo_push_delete(time_t start_time, time_t end_time, int show_time, const char *summary);
void dtcm_undo_push_reschedule(time_t old_start, time_t old_end,
                               time_t new_start, time_t new_end,
                               int show_time, const char *summary);
int dtcm_undo_pop(DtcmUndoEntry *out);

#endif /* DTCM_UNDO_STACK_H */
