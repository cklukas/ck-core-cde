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

void dtcm_hittest_clear(void);
void dtcm_hittest_add(int x, int y, int w, int h, int cache_idx, time_t day);
const DtcmHitBox *dtcm_hittest_find(int x, int y);

#endif /* DTCM_UX_HELPERS_H */
