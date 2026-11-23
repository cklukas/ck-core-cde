#!/usr/bin/env python3
"""ASan/UBSan tests for dtcm natural-language Quick-Add parser."""

import os
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DTCM_DIR = os.path.dirname(HERE)
CC = os.environ.get("CC", "clang")

HARNESS = r'''
#include "quick_add.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void) {
    struct tm base;
    memset(&base, 0, sizeof(base));
    base.tm_year = 2026 - 1900;
    base.tm_mon  = 11 - 1;
    base.tm_mday = 23;
    base.tm_hour = 10;
    base.tm_isdst = -1;
    time_t now = mktime(&base);

    DtcmQuickAddEvent ev;
    assert(dtcm_quick_add_parse("Design Review tomorrow 14:00-15:30 @ Studio 2", now, &ev) == 0);
    assert(strcmp(ev.summary, "Design Review") == 0);
    assert(strcmp(ev.location, "Studio 2") == 0);
    struct tm st, et;
    localtime_r(&ev.start_time, &st);
    localtime_r(&ev.end_time, &et);
    assert(st.tm_mday == 24 && st.tm_hour == 14 && st.tm_min == 0);
    assert(et.tm_mday == 24 && et.tm_hour == 15 && et.tm_min == 30);
    puts("ok    quick-add-tomorrow-range-location");

    assert(dtcm_quick_add_parse("Dentist 2026-12-05 09:15", now, &ev) == 0);
    assert(strcmp(ev.summary, "Dentist") == 0);
    assert(ev.location[0] == '\0');
    localtime_r(&ev.start_time, &st);
    assert(st.tm_year == 2026 - 1900 && st.tm_mon == 11 && st.tm_mday == 5);
    assert(st.tm_hour == 9 && st.tm_min == 15);
    assert(ev.end_time - ev.start_time == 3600);
    puts("ok    quick-add-explicit-date");

    assert(dtcm_quick_add_parse("   ", now, &ev) != 0);
    puts("ok    quick-add-empty-rejected");
    puts("check-quick-add: all assertions passed");
    return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix="dtcm-qa-") as td:
        src = os.path.join(td, "harness.c")
        exe = os.path.join(td, "test_qa")
        with open(src, "w") as f:
            f.write(HARNESS)
        subprocess.run([
            CC, "-std=c23", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", "-g", "-O1",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-DDTCM_QUICK_ADD_NO_GUI=1",
            f"-I{DTCM_DIR}",
            src,
            os.path.join(DTCM_DIR, "quick_add.c"),
            "-o", exe,
        ], check=True)
        subprocess.run([exe], check=True)

if __name__ == "__main__":
    main()
