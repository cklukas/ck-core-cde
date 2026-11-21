/*
 * CDE - Common Desktop Environment
 *
 * Pure-C UX helper routines for Calendar Manager (dtcm):
 * ISO-8601 week numbers, Today/DST-safe day shifting, color categories,
 * and rendered appointment hit-testing for hover previews, context menus,
 * and drag-to-reschedule.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "ux_helpers.h"

int
dtcm_iso_week_number(int year, int month, int day)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    if (mktime(&t) == (time_t)-1)
        return 1;

    char buf[8] = "";
    if (strftime(buf, sizeof(buf), "%V", &t) == 0)
        return 1;
    int w = atoi(buf);
    return (w >= 1 && w <= 53) ? w : 1;
}

int
dtcm_is_same_day(time_t a, time_t b)
{
    struct tm ta, tb;
    if (!localtime_r(&a, &ta) || !localtime_r(&b, &tb))
        return 0;
    return (ta.tm_year == tb.tm_year &&
            ta.tm_mon == tb.tm_mon &&
            ta.tm_mday == tb.tm_mday) ? 1 : 0;
}

time_t
dtcm_shift_time_by_days(time_t t, int delta_days)
{
    if (delta_days == 0)
        return t;
    struct tm tm_val;
    if (!localtime_r(&t, &tm_val))
        return t + (time_t)delta_days * 86400;
    tm_val.tm_mday += delta_days;
    tm_val.tm_isdst = -1;
    time_t res = mktime(&tm_val);
    return (res == (time_t)-1) ? (t + (time_t)delta_days * 86400) : res;
}

DtcmCategory
dtcm_category_from_name(const char *name)
{
    if (!name || !name[0])
        return DTCM_CAT_NONE;
    if (strcasecmp(name, "Work") == 0)
        return DTCM_CAT_WORK;
    if (strcasecmp(name, "Personal") == 0)
        return DTCM_CAT_PERSONAL;
    if (strcasecmp(name, "Important") == 0 || strcasecmp(name, "Urgent") == 0)
        return DTCM_CAT_IMPORTANT;
    if (strcasecmp(name, "Travel") == 0)
        return DTCM_CAT_TRAVEL;
    return DTCM_CAT_NONE;
}

const char *
dtcm_category_name(DtcmCategory cat)
{
    switch (cat) {
    case DTCM_CAT_WORK:      return "Work";
    case DTCM_CAT_PERSONAL:  return "Personal";
    case DTCM_CAT_IMPORTANT: return "Important";
    case DTCM_CAT_TRAVEL:    return "Travel";
    default:                 return "";
    }
}

void
dtcm_category_rgb(DtcmCategory cat, unsigned short *r, unsigned short *g, unsigned short *b)
{
    unsigned short rr = 0, gg = 0, bb = 0;
    switch (cat) {
    case DTCM_CAT_WORK:      /* #3b6ea5 */ rr = 0x3b3b; gg = 0x6e6e; bb = 0xa5a5; break;
    case DTCM_CAT_PERSONAL:  /* #3d8b59 */ rr = 0x3d3d; gg = 0x8b8b; bb = 0x5959; break;
    case DTCM_CAT_IMPORTANT: /* #c0392b */ rr = 0xc0c0; gg = 0x3939; bb = 0x2b2b; break;
    case DTCM_CAT_TRAVEL:    /* #c87d1a */ rr = 0xc8c8; gg = 0x7d7d; bb = 0x1a1a; break;
    default:                 rr = 0x6060; gg = 0x6060; bb = 0x6060; break;
    }
    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
}

void
dtcm_category_pastel_rgb(DtcmCategory cat, unsigned short *r, unsigned short *g, unsigned short *b)
{
    unsigned short rr = 0, gg = 0, bb = 0;
    switch (cat) {
    case DTCM_CAT_WORK:      /* soft blue  #d6e4f2 */ rr = 0xd6d6; gg = 0xe4e4; bb = 0xf2f2; break;
    case DTCM_CAT_PERSONAL:  /* soft green #d5ebd9 */ rr = 0xd5d5; gg = 0xebeb; bb = 0xd9d9; break;
    case DTCM_CAT_IMPORTANT: /* soft coral #f4d6d3 */ rr = 0xf4f4; gg = 0xd6d6; bb = 0xd3d3; break;
    case DTCM_CAT_TRAVEL:    /* soft amber #f5e5cc */ rr = 0xf5f5; gg = 0xe5e5; bb = 0xcccc; break;
    default:                 /* soft neutral #e4e1dc */ rr = 0xe4e4; gg = 0xe1e1; bb = 0xdcdc; break;
    }
    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
}

static int
cmp_day_block(const void *a, const void *b)
{
    const DtcmDayBlock *ba = (const DtcmDayBlock *)a;
    const DtcmDayBlock *bb = (const DtcmDayBlock *)b;
    if (ba->start_min != bb->start_min)
        return ba->start_min - bb->start_min;
    if (ba->end_min != bb->end_min)
        return bb->end_min - ba->end_min; /* longer first */
    return ba->cache_idx - bb->cache_idx;
}

int
dtcm_layout_day_events(DtcmDayBlock *blocks, int count)
{
    if (!blocks || count <= 0)
        return 0;

    for (int i = 0; i < count; i++) {
        if (blocks[i].start_min < 0) blocks[i].start_min = 0;
        if (blocks[i].start_min > 1425) blocks[i].start_min = 1425;
        if (blocks[i].end_min < blocks[i].start_min + 15)
            blocks[i].end_min = blocks[i].start_min + 15;
        if (blocks[i].end_min > 1440) blocks[i].end_min = 1440;
        blocks[i].col = 0;
        blocks[i].num_cols = 1;
    }

    qsort(blocks, (size_t)count, sizeof(DtcmDayBlock), cmp_day_block);

    int cluster_start = 0;
    while (cluster_start < count) {
        int cluster_end = cluster_start;
        int max_end = blocks[cluster_start].end_min;

        /* Find connected overlap cluster */
        while (cluster_end + 1 < count && blocks[cluster_end + 1].start_min < max_end) {
            cluster_end++;
            if (blocks[cluster_end].end_min > max_end)
                max_end = blocks[cluster_end].end_min;
        }

        /* Greedy interval coloring inside [cluster_start .. cluster_end] */
        int col_ends[32];
        int num_cols = 0;
        for (int i = cluster_start; i <= cluster_end; i++) {
            int chosen = -1;
            for (int c = 0; c < num_cols; c++) {
                if (col_ends[c] <= blocks[i].start_min) {
                    chosen = c;
                    break;
                }
            }
            if (chosen < 0) {
                if (num_cols < 32) {
                    chosen = num_cols++;
                } else {
                    chosen = 31;
                }
            }
            blocks[i].col = chosen;
            col_ends[chosen] = blocks[i].end_min;
        }
        if (num_cols < 1) num_cols = 1;
        for (int i = cluster_start; i <= cluster_end; i++) {
            blocks[i].num_cols = num_cols;
        }

        cluster_start = cluster_end + 1;
    }

    return count;
}

int
dtcm_snap_minutes(int raw_mins, int step_mins)
{
    if (step_mins <= 0) step_mins = 15;
    int snapped = ((raw_mins + step_mins / 2) / step_mins) * step_mins;
    if (snapped < 0) snapped = 0;
    if (snapped > 1440) snapped = 1440;
    return snapped;
}

static DtcmHitBox *g_hitboxes = NULL;
static int g_hitbox_count = 0;
static int g_hitbox_cap = 0;

void
dtcm_hittest_clear(void)
{
    g_hitbox_count = 0;
}

void
dtcm_hittest_add(int x, int y, int w, int h, int cache_idx, time_t day)
{
    if (w <= 0 || h <= 0 || cache_idx < 0)
        return;
    if (g_hitbox_count == g_hitbox_cap) {
        int ncap = g_hitbox_cap ? g_hitbox_cap * 2 : 64;
        DtcmHitBox *nb = (DtcmHitBox *)realloc(g_hitboxes, (size_t)ncap * sizeof(DtcmHitBox));
        if (!nb)
            return;
        g_hitboxes = nb;
        g_hitbox_cap = ncap;
    }
    DtcmHitBox *hb = &g_hitboxes[g_hitbox_count++];
    hb->x = x;
    hb->y = y;
    hb->w = w;
    hb->h = h;
    hb->cache_idx = cache_idx;
    hb->day = day;
}

const DtcmHitBox *
dtcm_hittest_find(int x, int y)
{
    for (int i = g_hitbox_count - 1; i >= 0; i--) {
        const DtcmHitBox *hb = &g_hitboxes[i];
        if (x >= hb->x && x < hb->x + hb->w &&
            y >= hb->y && y < hb->y + hb->h)
            return hb;
    }
    return NULL;
}
