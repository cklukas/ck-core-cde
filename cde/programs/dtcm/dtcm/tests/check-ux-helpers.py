#!/usr/bin/env python3
"""Standalone ASan/UBSan test for dtcm UX helpers (ux_helpers.c + ics_io.c CATEGORIES)."""

import os
import subprocess
import sys
import tempfile

C_TEST = r"""
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ux_helpers.h"
#include "ics_io.h"

int main(void)
{
    /* 1. ISO-8601 week numbers */
    assert(dtcm_iso_week_number(2026, 1, 1) == 1);
    assert(dtcm_iso_week_number(2026, 11, 21) == 47);
    assert(dtcm_iso_week_number(2026, 12, 31) == 53);
    assert(dtcm_iso_week_number(2024, 2, 29) == 9);
    printf("ok    iso-week-numbers\n");

    /* 2. Same-day comparison */
    struct tm t1 = { 0 }, t2 = { 0 }, t3 = { 0 };
    t1.tm_year = 126; t1.tm_mon = 10; t1.tm_mday = 21; t1.tm_hour = 0; t1.tm_min = 5; t1.tm_isdst = -1;
    t2.tm_year = 126; t2.tm_mon = 10; t2.tm_mday = 21; t2.tm_hour = 23; t2.tm_min = 55; t2.tm_isdst = -1;
    t3.tm_year = 126; t3.tm_mon = 10; t3.tm_mday = 22; t3.tm_hour = 0; t3.tm_min = 5; t3.tm_isdst = -1;
    assert(dtcm_is_same_day(mktime(&t1), mktime(&t2)) == 1);
    assert(dtcm_is_same_day(mktime(&t1), mktime(&t3)) == 0);
    printf("ok    same-day\n");

    /* 3. DST-preserving day shift (Europe/Berlin spring forward 2026-03-29) */
    setenv("TZ", ":Europe/Berlin", 1);
    tzset();
    struct tm pre = { 0 };
    pre.tm_year = 126; pre.tm_mon = 2; pre.tm_mday = 28; pre.tm_hour = 10; pre.tm_min = 30; pre.tm_isdst = -1;
    time_t t_pre = mktime(&pre);
    time_t t_post = dtcm_shift_time_by_days(t_pre, 2);
    struct tm post = { 0 };
    localtime_r(&t_post, &post);
    assert(post.tm_mon == 2 && post.tm_mday == 30 && post.tm_hour == 10 && post.tm_min == 30);
    printf("ok    dst-preserving-day-shift\n");

    /* 4. Categories + compose/decompose round-trip */
    assert(dtcm_category_from_name("Work") == DTCM_CAT_WORK);
    assert(dtcm_category_from_name("personal") == DTCM_CAT_PERSONAL);
    assert(dtcm_category_from_name("Urgent") == DTCM_CAT_IMPORTANT);
    assert(dtcm_category_from_name("Travel") == DTCM_CAT_TRAVEL);
    assert(strcmp(dtcm_category_name(DTCM_CAT_WORK), "Work") == 0);

    DtcmIcsEvent ev = { 0 };
    snprintf(ev.summary, sizeof(ev.summary), "Architecture Review");
    snprintf(ev.location, sizeof(ev.location), "Conf Room B");
    snprintf(ev.category, sizeof(ev.category), "Work");
    snprintf(ev.description, sizeof(ev.description), "Bring slides");
    char what[2048];
    dtcm_ics_compose_what(&ev, what, sizeof(what));

    char sum[256], loc[256], cat[64], desc[512];
    dtcm_ics_decompose_what_ex(what, sum, sizeof(sum), loc, sizeof(loc),
                               cat, sizeof(cat), desc, sizeof(desc));
    assert(strcmp(sum, "Architecture Review") == 0);
    assert(strcmp(loc, "Conf Room B") == 0);
    assert(strcmp(cat, "Work") == 0);
    assert(strcmp(desc, "Bring slides") == 0);
    printf("ok    categories-and-what-roundtrip\n");

    /* 5. Hit-testing */
    dtcm_hittest_clear();
    dtcm_hittest_add(10, 20, 100, 18, 3, t_pre);
    dtcm_hittest_add(10, 40, 100, 18, 7, t_post);
    const DtcmHitBox *hb = dtcm_hittest_find(50, 25);
    assert(hb != NULL && hb->cache_idx == 3 && hb->day == t_pre);
    hb = dtcm_hittest_find(50, 45);
    assert(hb != NULL && hb->cache_idx == 7 && hb->day == t_post);
    assert(dtcm_hittest_find(5, 5) == NULL);
    dtcm_hittest_clear();
    assert(dtcm_hittest_find(50, 25) == NULL);
    printf("ok    hittest\n");

    /* 6. Day view overlap layout + 15-min snap */
    DtcmDayBlock blks[4] = {
        { .cache_idx = 0, .start_min = 600, .end_min = 660 }, /* 10:00-11:00 */
        { .cache_idx = 1, .start_min = 630, .end_min = 690 }, /* 10:30-11:30 */
        { .cache_idx = 2, .start_min = 660, .end_min = 720 }, /* 11:00-12:00 */
        { .cache_idx = 3, .start_min = 840, .end_min = 900 }, /* 14:00-15:00 (isolated) */
    };
    assert(dtcm_layout_day_events(blks, 4) == 4);
    assert(blks[0].num_cols == 2 && blks[1].num_cols == 2 && blks[2].num_cols == 2);
    assert(blks[0].col == 0 && blks[1].col == 1 && blks[2].col == 0);
    assert(blks[3].num_cols == 1 && blks[3].col == 0);
    assert(dtcm_snap_minutes(607, 15) == 600);
    assert(dtcm_snap_minutes(608, 15) == 615);
    assert(dtcm_snap_minutes(-10, 15) == 0);
    assert(dtcm_snap_minutes(1500, 15) == 1440);
    printf("ok    day-overlap-layout-and-snap\n");

    return 0;
}
"""


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    dtcm_dir = os.path.dirname(test_dir)
    cc = os.environ.get("CC", "clang")

    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "ux-test.c")
        exe = os.path.join(td, "ux-test")
        with open(src, "w", encoding="utf-8") as f:
            f.write(C_TEST)
        subprocess.run(
            [cc, "-O1", "-g", "-fsanitize=address,undefined",
             f"-I{dtcm_dir}", src,
             os.path.join(dtcm_dir, "ux_helpers.c"),
             os.path.join(dtcm_dir, "ics_io.c"),
             "-o", exe],
            check=True,
        )
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        subprocess.run([exe], env=env, check=True)
    print("check-ux-helpers: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
