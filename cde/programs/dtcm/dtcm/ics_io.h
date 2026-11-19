#ifndef DTCM_ICS_IO_H
#define DTCM_ICS_IO_H

#include <stddef.h>
#include <time.h>

typedef enum {
    DTCM_ICS_ONCE = 0,
    DTCM_ICS_DAILY,
    DTCM_ICS_WEEKLY,
    DTCM_ICS_MONTHLY,
    DTCM_ICS_YEARLY,
} DtcmIcsFreq;

typedef struct {
    char uid[256];
    char summary[512];
    char location[512];
    char description[2048];
    time_t start_utc;
    time_t end_utc;
    int all_day;
    DtcmIcsFreq freq;
    int interval; /* >= 1 */
    int count;    /* 0 = forever (when freq != ONCE), >0 = bounded count */
} DtcmIcsEvent;

int dtcm_ics_parse_file(const char *path, DtcmIcsEvent **out_events, int *out_count);
void dtcm_ics_free_events(DtcmIcsEvent *events);

int dtcm_ics_write_file(const char *path, const DtcmIcsEvent *events, int count);

void dtcm_ics_compose_what(const DtcmIcsEvent *ev, char *out_what, size_t out_len);
void dtcm_ics_decompose_what(const char *what,
                             char *out_summary, size_t sum_len,
                             char *out_location, size_t loc_len,
                             char *out_desc, size_t desc_len);

#endif /* DTCM_ICS_IO_H */
