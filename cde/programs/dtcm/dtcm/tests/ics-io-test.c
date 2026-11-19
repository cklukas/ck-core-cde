#define _DEFAULT_SOURCE
#include "ics_io.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void)
{
    const char *sample_ics =
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "PRODID:-//Test//EN\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:evt-001@ckcore.test\r\n"
        "SUMMARY:Design Review\\, Wayland\\\r\n"
        "  & X11\r\n"
        "LOCATION:Room 42\\; Floor 3\r\n"
        "DESCRIPTION:Line one\\nLine two\\, with comma\r\n"
        "DTSTART:20261120T090000Z\r\n"
        "DTEND:20261120T103000Z\r\n"
        "RRULE:FREQ=WEEKLY;INTERVAL=2;COUNT=6\r\n"
        "END:VEVENT\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:evt-002@ckcore.test\r\n"
        "SUMMARY:Release Milestone\r\n"
        "DTSTART;VALUE=DATE:20261201\r\n"
        "END:VEVENT\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:evt-003@ckcore.test\r\n"
        "SUMMARY:Quick Standup\r\n"
        "DTSTART:20261121T140000Z\r\n"
        "DURATION:PT45M\r\n"
        "RRULE:FREQ=DAILY\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n";

    FILE *fp = fopen("input.ics", "wb");
    assert(fp != NULL);
    assert(fwrite(sample_ics, 1, strlen(sample_ics), fp) == strlen(sample_ics));
    fclose(fp);

    DtcmIcsEvent *events = NULL;
    int count = 0;
    assert(dtcm_ics_parse_file("input.ics", &events, &count) == 0);
    assert(count == 3);
    assert(events != NULL);

    /* Event 1: folded line + escaped punctuation + RRULE WEEKLY;INTERVAL=2;COUNT=6 */
    assert(strcmp(events[0].uid, "evt-001@ckcore.test") == 0);
    assert(strstr(events[0].summary, "Design Review, Wayland") != NULL);
    assert(strstr(events[0].summary, "& X11") != NULL);
    assert(strcmp(events[0].location, "Room 42; Floor 3") == 0);
    assert(strcmp(events[0].description, "Line one\nLine two, with comma") == 0);
    assert(events[0].all_day == 0);
    assert(events[0].end_utc - events[0].start_utc == 5400);
    assert(events[0].freq == DTCM_ICS_WEEKLY);
    assert(events[0].interval == 2);
    assert(events[0].count == 6);

    /* Event 2: VALUE=DATE all-day event */
    assert(strcmp(events[1].uid, "evt-002@ckcore.test") == 0);
    assert(strcmp(events[1].summary, "Release Milestone") == 0);
    assert(events[1].all_day == 1);
    assert(events[1].end_utc - events[1].start_utc == 86400);
    assert(events[1].freq == DTCM_ICS_ONCE);

    /* Event 3: DURATION:PT45M + RRULE:FREQ=DAILY (unbounded count == 0) */
    assert(strcmp(events[2].uid, "evt-003@ckcore.test") == 0);
    assert(events[2].end_utc - events[2].start_utc == 45 * 60);
    assert(events[2].freq == DTCM_ICS_DAILY);
    assert(events[2].interval == 1);
    assert(events[2].count == 0);

    /* Compose + decompose CSA "what" text */
    char what[2048];
    dtcm_ics_compose_what(&events[0], what, sizeof(what));
    char sum[512], loc[512], desc[2048];
    dtcm_ics_decompose_what(what, sum, sizeof(sum), loc, sizeof(loc), desc, sizeof(desc));
    assert(strcmp(sum, events[0].summary) == 0);
    assert(strcmp(loc, events[0].location) == 0);
    assert(strcmp(desc, events[0].description) == 0);

    /* Write -> parse round-trip */
    assert(dtcm_ics_write_file("roundtrip.ics", events, count) == 0);
    DtcmIcsEvent *rt = NULL;
    int rt_count = 0;
    assert(dtcm_ics_parse_file("roundtrip.ics", &rt, &rt_count) == 0);
    assert(rt_count == 3);

    for (int i = 0; i < 3; ++i) {
        assert(strcmp(rt[i].uid, events[i].uid) == 0);
        assert(strcmp(rt[i].summary, events[i].summary) == 0);
        assert(strcmp(rt[i].location, events[i].location) == 0);
        assert(strcmp(rt[i].description, events[i].description) == 0);
        assert(rt[i].start_utc == events[i].start_utc);
        assert(rt[i].end_utc == events[i].end_utc);
        assert(rt[i].all_day == events[i].all_day);
        assert(rt[i].freq == events[i].freq);
        assert(rt[i].interval == events[i].interval);
        assert(rt[i].count == events[i].count);
    }

    dtcm_ics_free_events(rt);
    dtcm_ics_free_events(events);
    printf("ics-io-test: all assertions passed\n");
    return 0;
}
