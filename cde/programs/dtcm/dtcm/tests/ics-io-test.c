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
        "CATEGORIES:Work\r\n"
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
        "BEGIN:VEVENT\r\n"
        "UID:evt-004-tz-summer@ckcore.test\r\n"
        "SUMMARY:Berlin Summer Sync\r\n"
        "DTSTART;TZID=Europe/Berlin:20260715T140000\r\n"
        "DTEND;TZID=Europe/Berlin:20260715T150000\r\n"
        "END:VEVENT\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:evt-005-tz-winter@ckcore.test\r\n"
        "SUMMARY:New York Winter Call\r\n"
        "DTSTART;TZID=\"America/New_York\":20260115T090000\r\n"
        "DTEND;TZID=\"America/New_York\":20260115T100000\r\n"
        "END:VEVENT\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:evt-006-tz-windows@ckcore.test\r\n"
        "SUMMARY:Windows TZID Invite\r\n"
        "DTSTART;TZID=\"W. Europe Standard Time\":20260115T140000\r\n"
        "DTEND;TZID=\"W. Europe Standard Time\":20260115T150000\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n";

    FILE *fp = fopen("input.ics", "wb");
    assert(fp != NULL);
    assert(fwrite(sample_ics, 1, strlen(sample_ics), fp) == strlen(sample_ics));
    fclose(fp);

    DtcmIcsEvent *events = NULL;
    int count = 0;
    assert(dtcm_ics_parse_file("input.ics", &events, &count) == 0);
    assert(count == 6);
    assert(events != NULL);

    /* Event 1: folded line + escaped punctuation + RRULE WEEKLY;INTERVAL=2;COUNT=6 */
    assert(strcmp(events[0].uid, "evt-001@ckcore.test") == 0);
    assert(strstr(events[0].summary, "Design Review, Wayland") != NULL);
    assert(strstr(events[0].summary, "& X11") != NULL);
    assert(strcmp(events[0].location, "Room 42; Floor 3") == 0);
    assert(strcmp(events[0].category, "Work") == 0);
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

    /* Event 4: Europe/Berlin summer (CEST = UTC+2 -> 14:00 local == 12:00Z) */
    {
        struct tm exp = { 0 };
        exp.tm_year = 2026 - 1900; exp.tm_mon = 6; exp.tm_mday = 15; exp.tm_hour = 12;
        assert(events[3].start_utc == timegm(&exp));
        assert(events[3].end_utc - events[3].start_utc == 3600);
    }

    /* Event 5: America/New_York winter (EST = UTC-5 -> 09:00 local == 14:00Z) */
    {
        struct tm exp = { 0 };
        exp.tm_year = 2026 - 1900; exp.tm_mon = 0; exp.tm_mday = 15; exp.tm_hour = 14;
        assert(events[4].start_utc == timegm(&exp));
        assert(events[4].end_utc - events[4].start_utc == 3600);
    }

    /* Event 6: Windows TZID "W. Europe Standard Time" -> Europe/Berlin winter (CET = UTC+1 -> 14:00 == 13:00Z) */
    {
        struct tm exp = { 0 };
        exp.tm_year = 2026 - 1900; exp.tm_mon = 0; exp.tm_mday = 15; exp.tm_hour = 13;
        assert(events[5].start_utc == timegm(&exp));
        assert(events[5].end_utc - events[5].start_utc == 3600);
    }

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
    assert(rt_count == 6);

    for (int i = 0; i < 6; ++i) {
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

    /* Test extended RRULE (BYDAY + UNTIL) and EXDATE parsing, round-trip, and expansion */
    const char *rrule_ics =
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:byday-1@example.com\r\n"
        "DTSTART:20261102T090000Z\r\n" /* Monday Nov 2, 2026 */
        "DTEND:20261102T100000Z\r\n"
        "SUMMARY:MWF Standup\r\n"
        "RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR;UNTIL=20261113T235959Z\r\n"
        "EXDATE:20261106T090000Z\r\n" /* Skip Friday Nov 6 */
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n";

    const char *rrule_in = "rrule_in.ics";
    const char *rrule_out = "rrule_out.ics";
    FILE *rfp = fopen(rrule_in, "wb");
    assert(rfp != NULL);
    fwrite(rrule_ics, 1, strlen(rrule_ics), rfp);
    fclose(rfp);

    DtcmIcsEvent *rev = NULL;
    int rcnt = 0;
    assert(dtcm_ics_parse_file(rrule_in, &rev, &rcnt) == 0);
    assert(rcnt == 1);
    /* MO=bit1, WE=bit3, FR=bit5 => (1<<1)|(1<<3)|(1<<5) = 2+8+32 = 42 */
    assert(rev[0].byday_mask == ((1u << 1) | (1u << 3) | (1u << 5)));
    assert(rev[0].until_utc > rev[0].start_utc);
    assert(rev[0].exdate_count == 1);

    /* Expand over Nov 1..Nov 20: Mon 2, Wed 4, (skip Fri 6), Mon 9, Wed 11, Fri 13 => 5 occurrences */
    time_t occ[16];
    int nocc = dtcm_ics_expand_occurrences(&rev[0], rev[0].start_utc - 86400, rev[0].start_utc + 20 * 86400, occ, 16);
    assert(nocc == 5);

    /* Round-trip write + parse preserves BYDAY, UNTIL, EXDATE */
    assert(dtcm_ics_write_file(rrule_out, rev, rcnt) == 0);
    DtcmIcsEvent *rev2 = NULL;
    int rcnt2 = 0;
    assert(dtcm_ics_parse_file(rrule_out, &rev2, &rcnt2) == 0);
    assert(rcnt2 == 1);
    assert(rev2[0].byday_mask == rev[0].byday_mask);
    assert(rev2[0].until_utc == rev[0].until_utc);
    assert(rev2[0].exdate_count == 1);
    assert(rev2[0].exdates[0] == rev[0].exdates[0]);
    dtcm_ics_free_events(rev2);
    dtcm_ics_free_events(rev);

    /* Test iTIP METHOD:REQUEST parsing + METHOD:REPLY generation */
    const char *itip_req =
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "METHOD:REQUEST\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:invite-42@example.com\r\n"
        "DTSTART:20261124T140000Z\r\n"
        "DTEND:20261124T150000Z\r\n"
        "SUMMARY:Architecture Sync\r\n"
        "LOCATION:Boardroom 1\r\n"
        "ORGANIZER;CN=\"Alice Smith\":mailto:alice@example.com\r\n"
        "ATTENDEE;CN=\"Bob Jones\";PARTSTAT=NEEDS-ACTION:mailto:bob@example.com\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    FILE *ifp = fopen("itip_req.ics", "wb");
    assert(ifp != NULL);
    fwrite(itip_req, 1, strlen(itip_req), ifp);
    fclose(ifp);

    DtcmIcsEvent *iev = NULL;
    int icnt = 0;
    assert(dtcm_ics_parse_file("itip_req.ics", &iev, &icnt) == 0);
    assert(icnt == 1);
    assert(strcmp(iev[0].method, "REQUEST") == 0);
    assert(strcmp(iev[0].organizer_mailto, "alice@example.com") == 0);
    assert(strstr(iev[0].organizer, "Alice Smith") != NULL);
    assert(strstr(iev[0].attendees, "bob@example.com") != NULL);

    assert(dtcm_ics_write_itip_reply("itip_reply.ics", &iev[0], "bob@example.com", "ACCEPTED") == 0);
    DtcmIcsEvent *rep = NULL;
    int repcnt = 0;
    assert(dtcm_ics_parse_file("itip_reply.ics", &rep, &repcnt) == 0);
    assert(repcnt == 1);
    assert(strcmp(rep[0].method, "REPLY") == 0);
    assert(strcmp(rep[0].uid, "invite-42@example.com") == 0);
    assert(strstr(rep[0].attendees, "ACCEPTED") != NULL);
    dtcm_ics_free_events(rep);
    dtcm_ics_free_events(iev);

    printf("ics-io-test: all assertions passed\n");
    return 0;
}
