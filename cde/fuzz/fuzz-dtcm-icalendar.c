/*
 * Fuzz target: dtcm RFC 5545 iCalendar (.ics) & RFC 4791 CalDAV XML parsers:
 *   - dtcm_ics_parse_file, dtcm_ics_expand_occurrences, dtcm_ics_compose_what,
 *     dtcm_ics_decompose_what_ex, dtcm_ics_write_file, dtcm_ics_write_itip_reply
 *     (programs/dtcm/dtcm/ics_io.c)
 *   - find_xml_tag_open, find_xml_tag_close, trim_copy, resolve_href_url,
 *     ends_with_ics, dtcm_caldav_validate_url (programs/dtcm/dtcm/caldav_client.c)
 *
 * Input layout: byte 0 bit 0 selects iCalendar (.ics) vs CalDAV multistatus XML
 * (bit 1 runs both).
 */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include FUZZ_TYPES_H
#include FUZZ_PRODUCTION_H

static void fuzz_caldav_multistatus(const char *xml)
{
    const char *base_url = "https://cal.example.org/dav/calendars/user/personal/";
    char errbuf[128];
    (void)dtcm_caldav_validate_url(base_url, errbuf, sizeof(errbuf));
    (void)dtcm_caldav_validate_url(xml, errbuf, sizeof(errbuf));

    const char *cursor = xml;
    const char *rstart = NULL;
    int guard = 0;
    while (guard++ < 128 && find_xml_tag_open(cursor, "response", &rstart) != NULL) {
        const char *rend = find_xml_tag_close(rstart, "response");
        if (!rend)
            break;
        size_t block_len = (size_t)(rend - rstart);
        char *block = (char *)malloc(block_len + 1);
        if (!block)
            break;
        memcpy(block, rstart, block_len);
        block[block_len] = '\0';

        const char *hs = NULL, *es = NULL, *cts = NULL;
        char href_raw[1024] = "", etag[256] = "", ctype[128] = "", resolved[1280] = "";
        if (find_xml_tag_open(block, "href", &hs)) {
            const char *he = find_xml_tag_close(hs, "href");
            if (he)
                trim_copy(hs, he, href_raw, sizeof(href_raw));
        }
        if (find_xml_tag_open(block, "getetag", &es)) {
            const char *ee = find_xml_tag_close(es, "getetag");
            if (ee)
                trim_copy(es, ee, etag, sizeof(etag));
        }
        if (find_xml_tag_open(block, "getcontenttype", &cts)) {
            const char *cte = find_xml_tag_close(cts, "getcontenttype");
            if (cte)
                trim_copy(cts, cte, ctype, sizeof(ctype));
        }
        if (href_raw[0]) {
            (void)ends_with_ics(href_raw);
            resolve_href_url(base_url, href_raw, resolved, sizeof(resolved));
        }
        free(block);
        cursor = rend + 2;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;

    uint8_t flags = data[0];
    const uint8_t *payload = data + 1;
    size_t plen = size - 1;
    if (plen > 32768)
        plen = 32768;

    if ((flags & 1) == 0 || (flags & 2)) {
        const char *tdir = getenv("TMPDIR");
        if (!tdir || !tdir[0])
            tdir = "/tmp";
        char tmppath[1024];
        snprintf(tmppath, sizeof(tmppath), "%s/fuzz-ics-XXXXXX", tdir);
        int fd = mkstemp(tmppath);
        if (fd >= 0) {
            if (plen > 0)
                (void)write(fd, payload, plen);
            close(fd);

            DtcmIcsEvent *events = NULL;
            int count = 0;
            if (dtcm_ics_parse_file(tmppath, &events, &count) == 0 && events) {
                time_t starts[32];
                for (int i = 0; i < count && i < 8; i++) {
                    time_t r0 = events[i].start_utc > 0 ? events[i].start_utc : 1700000000;
                    (void)dtcm_ics_expand_occurrences(&events[i], r0 - 86400,
                                                      r0 + 365 * 86400, starts, 32);
                    char what[3072], sum[512], loc[512], cat[64], desc[2048];
                    dtcm_ics_compose_what(&events[i], what, sizeof(what));
                    dtcm_ics_decompose_what_ex(what, sum, sizeof(sum), loc, sizeof(loc),
                                               cat, sizeof(cat), desc, sizeof(desc));
                }
                if (count > 0 && (flags & 4)) {
                    char outpath[1040];
                    snprintf(outpath, sizeof(outpath), "%s.out", tmppath);
                    (void)dtcm_ics_write_file(outpath, events, count < 4 ? count : 4);
                    unlink(outpath);
                    (void)dtcm_ics_write_itip_reply(outpath, &events[0],
                                                    "user@example.org", "ACCEPTED");
                    unlink(outpath);
                }
                dtcm_ics_free_events(events);
            }
            unlink(tmppath);
        }
    }

    if ((flags & 1) == 1 || (flags & 2)) {
        char *zxml = (char *)malloc(plen + 1);
        if (zxml) {
            if (plen > 0)
                memcpy(zxml, payload, plen);
            zxml[plen] = '\0';
            fuzz_caldav_multistatus(zxml);
            free(zxml);
        }
    }

    return 0;
}
