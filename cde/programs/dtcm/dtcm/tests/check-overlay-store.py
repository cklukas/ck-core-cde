#!/usr/bin/env python3
"""ASan/UBSan unit tests for dtcm overlay_store (webcal normalization, 0600 TSV, ICS fetch & query)."""

import os
import subprocess
import sys
import tempfile

C_TEST = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "overlay_store.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *dir = argv[1];

    /* 1. URL normalization */
    char norm[256];
    assert(dtcm_overlay_normalize_url("webcal://example.com/holidays.ics", norm, sizeof(norm)) == 0);
    assert(strcmp(norm, "https://example.com/holidays.ics") == 0);
    assert(dtcm_overlay_normalize_url("webcals://example.com/team.ics", norm, sizeof(norm)) == 0);
    assert(strcmp(norm, "https://example.com/team.ics") == 0);
    assert(dtcm_overlay_normalize_url("javascript:alert(1)", norm, sizeof(norm)) != 0);
    assert(dtcm_overlay_normalize_url("   ", norm, sizeof(norm)) != 0);
    printf("ok    url-normalization\n");

    /* 2. Local .ics subscription fetch + query + toggle */
    char ics_path[1024], store_path[1024];
    snprintf(ics_path, sizeof(ics_path), "%s/holidays.ics", dir);
    snprintf(store_path, sizeof(store_path), "%s/dtcm_overlays", dir);

    const char *ics =
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:hol-1@example.com\r\n"
        "DTSTART;VALUE=DATE:20261126\r\n"
        "DTEND;VALUE=DATE:20261128\r\n"
        "SUMMARY:Thanksgiving Break\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    FILE *fp = fopen(ics_path, "wb");
    assert(fp != NULL);
    fwrite(ics, 1, strlen(ics), fp);
    fclose(fp);

    dtcm_overlay_shutdown();
    int idx = dtcm_overlay_add("US Holidays", ics_path, DTCM_CAT_PERSONAL, 1);
    assert(idx == 0);
    assert(dtcm_overlay_count() == 1);
    assert(dtcm_overlay_refresh_one(0) == 0);
    assert(dtcm_overlay_get(0)->event_count == 1);

    /* Query Nov 2026 */
    struct tm t1 = {0}, t2 = {0};
    t1.tm_year = 126; t1.tm_mon = 10; t1.tm_mday = 1;
    t2.tm_year = 126; t2.tm_mon = 10; t2.tm_mday = 30;
    time_t r1 = timegm(&t1), r2 = timegm(&t2);
    DtcmOverlayOccurrence occ[8];
    int n = dtcm_overlay_query_range(r1, r2, occ, 8);
    assert(n == 1);
    assert(strcmp(occ[0].summary, "Thanksgiving Break") == 0);
    assert(occ[0].color == DTCM_CAT_PERSONAL);
    assert(occ[0].all_day == 1);

    /* Disable overlay -> query returns 0 */
    assert(dtcm_overlay_set_enabled(0, 0) == 0);
    assert(dtcm_overlay_query_range(r1, r2, occ, 8) == 0);
    assert(dtcm_overlay_set_enabled(0, 1) == 0);
    printf("ok    local-ics-fetch-and-query\n");

    /* 3. Save/load with 0600 permissions */
    assert(dtcm_overlay_save_file(store_path) == 0);
    struct stat st;
    assert(stat(store_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    dtcm_overlay_shutdown();
    assert(dtcm_overlay_count() == 0);
    assert(dtcm_overlay_load_file(store_path) == 0);
    assert(dtcm_overlay_count() == 1);
    assert(strcmp(dtcm_overlay_get(0)->name, "US Holidays") == 0);
    assert(dtcm_overlay_get(0)->color == DTCM_CAT_PERSONAL);
    assert(dtcm_overlay_get(0)->enabled == 1);
    dtcm_overlay_shutdown();
    printf("ok    store-0600-roundtrip\n");
    return 0;
}
"""


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    dtcm_dir = os.path.dirname(test_dir)
    cc = os.environ.get("CC", "clang")

    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "overlay-test.c")
        exe = os.path.join(td, "overlay-test")
        with open(src, "w", encoding="utf-8") as f:
            f.write(C_TEST)
        subprocess.run(
            [cc, "-O1", "-g", "-fsanitize=address,undefined",
             f"-I{dtcm_dir}", src,
             os.path.join(dtcm_dir, "overlay_store.c"),
             os.path.join(dtcm_dir, "ics_io.c"),
             os.path.join(dtcm_dir, "ux_helpers.c"),
             "-o", exe],
            check=True,
        )
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        subprocess.run([exe, td], env=env, check=True)
    print("check-overlay-store: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
