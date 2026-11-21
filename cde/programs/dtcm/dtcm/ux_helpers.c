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
