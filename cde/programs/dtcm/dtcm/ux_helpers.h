#ifndef DTCM_UX_HELPERS_H
#define DTCM_UX_HELPERS_H

#include <stddef.h>
#include <time.h>

typedef enum {
    DTCM_CAT_NONE = 0,
    DTCM_CAT_WORK,
    DTCM_CAT_PERSONAL,
    DTCM_CAT_IMPORTANT,
    DTCM_CAT_TRAVEL,
    DTCM_CAT_COUNT
} DtcmCategory;

typedef struct {
    int x;
    int y;
    int w;
    int h;
    int cache_idx;
    time_t day;
} DtcmHitBox;

int dtcm_iso_week_number(int year, int month, int day);
int dtcm_is_same_day(time_t a, time_t b);
time_t dtcm_shift_time_by_days(time_t t, int delta_days);

DtcmCategory dtcm_category_from_name(const char *name);
const char *dtcm_category_name(DtcmCategory cat);
void dtcm_category_rgb(DtcmCategory cat, unsigned short *r, unsigned short *g, unsigned short *b);
void dtcm_category_pastel_rgb(DtcmCategory cat, unsigned short *r, unsigned short *g, unsigned short *b);

typedef struct {
    int cache_idx;
    int start_min; /* minutes from midnight [0..1440] */
    int end_min;   /* clamped >= start_min + 15 */
    int col;       /* assigned sub-column [0..num_cols-1] */
    int num_cols;  /* total concurrent columns in this overlap cluster */
} DtcmDayBlock;

int dtcm_layout_day_events(DtcmDayBlock *blocks, int count);
int dtcm_snap_minutes(int raw_mins, int step_mins);

typedef struct {
    int cache_idx;       /* index in paint_cache or negative for overlay */
    int start_col;       /* inclusive day column [0..num_cols-1] */
    int end_col;         /* inclusive day column [start_col..num_cols-1] */
    int lane;            /* assigned vertical lane [0..] */
    int continues_left;  /* 1 if event began before this row/week */
    int continues_right; /* 1 if event continues past this row/week */
} DtcmMultiDaySpan;

int dtcm_compute_multiday_spans(DtcmMultiDaySpan *spans, int count, int num_cols);

void dtcm_hittest_clear(void);
void dtcm_hittest_add(int x, int y, int w, int h, int cache_idx, time_t day);
const DtcmHitBox *dtcm_hittest_find(int x, int y);

/* Direct appointment selection state (survives repaints by matching start_time + summary) */
void dtcm_selected_appt_set(int cache_idx, time_t start_time, time_t end_time, int show_time, const char *summary);
void dtcm_selected_appt_clear(void);
int dtcm_selected_appt_is_set(void);
int dtcm_selected_appt_matches(time_t start_time, const char *summary);
int dtcm_selected_appt_get(int *out_cache_idx, time_t *out_start, time_t *out_end, int *out_show_time, char *out_summary, size_t sum_len);

/* Appointment clipboard for Copy / Paste across days and hours */
void dtcm_clipboard_copy(time_t start_time, time_t end_time, int show_time, const char *summary);
int dtcm_clipboard_has_item(void);
int dtcm_clipboard_get(time_t *out_start, time_t *out_end, int *out_show_time, char *out_summary, size_t sum_len);

#endif /* DTCM_UX_HELPERS_H */
