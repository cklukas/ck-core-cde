#ifndef DTCM_OVERLAY_STORE_H
#define DTCM_OVERLAY_STORE_H

#include <stddef.h>
#include <time.h>

#include "ics_io.h"
#include "ux_helpers.h"

#define DTCM_MAX_OVERLAYS 16

typedef struct {
    char name[128];
    char url[1024];
    DtcmCategory color;
    int enabled;
    int read_only;
    DtcmIcsEvent *events;
    int event_count;
    char last_error[256];
} DtcmOverlaySource;

typedef struct {
    time_t start_utc;
    time_t end_utc;
    int all_day;
    DtcmCategory color;
    char source_name[128];
    char summary[512];
    char location[256];
} DtcmOverlayOccurrence;

int dtcm_overlay_normalize_url(const char *raw, char *out, size_t out_len);

void dtcm_overlay_init(void);
void dtcm_overlay_shutdown(void);

int dtcm_overlay_default_path(char *out, size_t out_len);
int dtcm_overlay_load_file(const char *path);
int dtcm_overlay_save_file(const char *path);

int dtcm_overlay_count(void);
const DtcmOverlaySource *dtcm_overlay_get(int idx);
int dtcm_overlay_add(const char *name, const char *url, DtcmCategory color, int enabled);
int dtcm_overlay_remove(int idx);
int dtcm_overlay_set_enabled(int idx, int enabled);
int dtcm_overlay_set_color(int idx, DtcmCategory color);

/* Fetches and parses events for overlay idx (blocking; safe to run on worker thread
 * when copying URL/results via dtcm_overlay_fetch_url). */
int dtcm_overlay_fetch_url(const char *url,
                           DtcmIcsEvent **out_events,
                           int *out_count,
                           char *err_buf,
                           size_t err_len);
int dtcm_overlay_refresh_one(int idx);
void dtcm_overlay_replace_events(int idx, DtcmIcsEvent *events, int count, const char *err);

/* Queries all enabled overlay calendars for occurrences intersecting [range_start, range_end]. */
int dtcm_overlay_query_range(time_t range_start,
                             time_t range_end,
                             DtcmOverlayOccurrence *out,
                             int max_out);

#endif /* DTCM_OVERLAY_STORE_H */
