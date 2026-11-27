/*
 * Fuzz target: dtmail RFC 5322 / MIME / HTML / iTIP untrusted message parsers:
 *   - RFCMIME::readBase64, RFCMIME::readQPrint, RFCMIME::readTextEnriched
 *     (libDtMail/RFC/RFCMIME.C)
 *   - getNamedValueString, getNamedValueInt, PartialText (libDtMail/RFC/MIMEPartial.C)
 *   - dtmail_html_to_readable_text, dtmail_sanitize_html_sandboxed (dtmail/HtmlReadable.C)
 *   - dtmail_itip_parse (dtmail/ItipBanner.C)
 *   - extract_from_header, extract_header_value (dtmail/CurlMailEngine.C)
 *
 * Input layout: byte 0 selects the parser mode (or runs all modes when bit 7 is set);
 * bytes 1..size-1 are placed in an exact-size heap buffer so AddressSanitizer catches
 * any 1-byte underflow or over-read past the untrusted payload boundary.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include FUZZ_TYPES_H
#include FUZZ_PRODUCTION_H

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;

    uint8_t sel = data[0];
    const uint8_t *payload = data + 1;
    size_t plen = size - 1;
    if (plen > 65536)
        plen = 65536;

    /* Exact-sized non-NUL-terminated buffer for length-bounded parsers */
    char *exact = (char *)malloc(plen ? plen : 1);
    if (!exact)
        return 0;
    if (plen > 0)
        memcpy(exact, payload, plen);

    /* NUL-terminated copy for C-string header parameter parsers */
    char *zstr = (char *)malloc(plen + 1);
    if (!zstr) {
        free(exact);
        return 0;
    }
    if (plen > 0)
        memcpy(zstr, payload, plen);
    zstr[plen] = '\0';

    int run_all = (sel & 0x80) != 0;
    unsigned int mode = sel & 0x07;

    if (run_all || mode == 0) {
        char *out = (char *)malloc(plen + 2);
        if (out) {
            int off = 0;
            RFCMIME::readBase64(out, off, exact, (unsigned long)plen);
            out[off] = '\0';
            free(out);
        }
    }

    if (run_all || mode == 1) {
        char *out = (char *)malloc(plen + 2);
        if (out) {
            int off = 0;
            RFCMIME::readQPrint(out, off, exact, (unsigned long)plen);
            out[off] = '\0';
            free(out);
        }
    }

    if (run_all || mode == 2) {
        char *out = (char *)malloc(plen + 2);
        if (out) {
            int off = 0;
            RFCMIME::readTextEnriched(out, off, exact, (unsigned long)plen);
            out[off] = '\0';
            free(out);
        }
    }

    if (run_all || mode == 3) {
        char *id_v = getNamedValueString(zstr, "id");
        char *bd_v = getNamedValueString(zstr, "boundary");
        (void)getNamedValueInt(zstr, "number");
        (void)getNamedValueInt(zstr, "total");
        free(id_v);
        free(bd_v);

        PartialText pt;
        pt.append(exact, plen);
        char *rel = pt.release();
        free(rel);
    }

    if (run_all || mode == 4) {
        char *txt = dtmail_html_to_readable_text(exact, plen);
        free(txt);
    }

    if (run_all || mode == 5) {
        int blocked = 0;
        char *san = dtmail_sanitize_html_sandboxed(exact, plen, sel & 0x40 ? 1 : 0, &blocked);
        free(san);
    }

    if (run_all || mode == 6) {
        DtMailItipInvite inv;
        (void)dtmail_itip_parse(exact, plen, &inv);
    }

    if (run_all || mode == 7) {
        char from_b[256], subj_b[256], ct_b[256];
        extract_from_header(exact, plen, from_b, sizeof(from_b));
        extract_header_value(exact, plen, "Subject", subj_b, sizeof(subj_b));
        extract_header_value(exact, plen, "Content-Type", ct_b, sizeof(ct_b));
    }

    free(zstr);
    free(exact);
    return 0;
}
