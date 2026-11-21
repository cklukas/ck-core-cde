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

void dtcm_hittest_clear(void);
void dtcm_hittest_add(int x, int y, int w, int h, int cache_idx, time_t day);
const DtcmHitBox *dtcm_hittest_find(int x, int y);

#endif /* DTCM_UX_HELPERS_H */
