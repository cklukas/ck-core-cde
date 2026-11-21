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
    char category[64];
    char description[2048];
    time_t start_utc;
    time_t end_utc;
    int all_day;
    DtcmIcsFreq freq;
    int interval; /* >= 1 */
    int count;    /* 0 = forever (when freq != ONCE), >0 = bounded count */
    unsigned int byday_mask; /* bit 0=SU, 1=MO, 2=TU, 3=WE, 4=TH, 5=FR, 6=SA */
    time_t until_utc;        /* 0 = none */
    time_t exdates[32];      /* excluded occurrence start timestamps */
    int exdate_count;
    char method[32];         /* e.g. "REQUEST", "REPLY", "CANCEL" from VCALENDAR METHOD */
    char organizer[256];     /* display organizer, e.g. "Alice <alice@example.com>" */
    char organizer_mailto[256]; /* raw email address without mailto: */
    char attendees[1024];    /* newline-separated attendees */
} DtcmIcsEvent;

int dtcm_ics_parse_file(const char *path, DtcmIcsEvent **out_events, int *out_count);
void dtcm_ics_free_events(DtcmIcsEvent *events);

int dtcm_ics_write_file(const char *path, const DtcmIcsEvent *events, int count);

/* Writes an RFC 5546 iTIP METHOD:REPLY .ics file for req with PARTSTAT
 * ("ACCEPTED", "TENTATIVE", or "DECLINED"). Returns 0 on success. */
int dtcm_ics_write_itip_reply(const char *path,
                              const DtcmIcsEvent *req,
                              const char *attendee_email,
                              const char *partstat);

/* Expands occurrences of ev whose start time falls in [range_start, range_end].
 * Respects freq, interval, count, byday_mask, until_utc, and exdates.
 * Returns number of occurrences written to out_starts (up to max_out). */
int dtcm_ics_expand_occurrences(const DtcmIcsEvent *ev,
                                time_t range_start,
                                time_t range_end,
                                time_t *out_starts,
                                int max_out);

void dtcm_ics_compose_what(const DtcmIcsEvent *ev, char *out_what, size_t out_len);
void dtcm_ics_decompose_what(const char *what,
                             char *out_summary, size_t sum_len,
                             char *out_location, size_t loc_len,
                             char *out_desc, size_t desc_len);
void dtcm_ics_decompose_what_ex(const char *what,
                                char *out_summary, size_t sum_len,
                                char *out_location, size_t loc_len,
                                char *out_category, size_t cat_len,
                                char *out_desc, size_t desc_len);

#endif /* DTCM_ICS_IO_H */
