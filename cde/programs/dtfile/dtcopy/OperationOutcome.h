#ifndef DTCOPY_OPERATION_OUTCOME_H
#define DTCOPY_OPERATION_OUTCOME_H

/* UI completion policy. Skipping/ignoring an operation is never proof that a
 * move's source may be removed. Backend adapters must retain that distinction.
 */
typedef enum {
    DTCOPY_COMPLETED,
    DTCOPY_PARTIAL,
    DTCOPY_CANCELLED,
    DTCOPY_FAILED
} DtcopyOutcome;

static inline DtcopyOutcome
DtcopyGetOutcome(int result, int cancelled, int errors, int skipped)
{
    if (cancelled) return DTCOPY_CANCELLED;
    if (result < 0) return DTCOPY_FAILED;
    if (result > 0 || errors || skipped) return DTCOPY_PARTIAL;
    return DTCOPY_COMPLETED;
}

#endif
