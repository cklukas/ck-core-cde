#define _DEFAULT_SOURCE
#include "ics_io.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void ics_unescape_text(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    size_t j = 0;
    if (src) {
        for (size_t i = 0; src[i] != '\0' && j + 1 < dst_len; ++i) {
            if (src[i] == '\\' && src[i + 1] != '\0') {
                char nxt = src[++i];
                if (nxt == 'n' || nxt == 'N') dst[j++] = '\n';
                else if (nxt == 't' || nxt == 'T') dst[j++] = '\t';
                else dst[j++] = nxt;
            } else {
                dst[j++] = src[i];
            }
        }
    }
    dst[j] = '\0';
}

static void ics_write_escaped(FILE *fp, const char *s)
{
    if (!s) return;
    for (const char *p = s; *p; ++p) {
        if (*p == '\n') {
            fputs("\\n", fp);
        } else if (*p == '\r') {
            continue;
        } else if (*p == ',' || *p == ';' || *p == '\\') {
            fputc('\\', fp);
            fputc(*p, fp);
        } else {
            fputc(*p, fp);
        }
    }
}

static int parse_2dig(const char *p)
{
    if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static int parse_4dig(const char *p)
{
    int hi = parse_2dig(p);
    int lo = parse_2dig(p + 2);
    if (hi < 0 || lo < 0) return -1;
    return hi * 100 + lo;
}

static int parse_ics_datetime(const char *val, const char *params,
                              time_t *out_utc, int *out_all_day)
{
    if (!val || !out_utc) return -1;
    while (*val && isspace((unsigned char)*val)) val++;

    size_t vlen = strlen(val);
    if (vlen < 8) return -1;

    int yr = parse_4dig(val);
    int mo = parse_2dig(val + 4);
    int dy = parse_2dig(val + 6);
    if (yr < 1970 || mo < 1 || mo > 12 || dy < 1 || dy > 31) return -1;

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = yr - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = dy;
    tmv.tm_isdst = -1;

    int is_date_only = (params && strcasestr(params, "VALUE=DATE") != NULL &&
                        strcasestr(params, "VALUE=DATE-TIME") == NULL) ? 1 : 0;
    if (val[8] != 'T' && val[8] != 't') {
        is_date_only = 1;
    }

    if (is_date_only) {
        tmv.tm_hour = 0;
        tmv.tm_min = 0;
        tmv.tm_sec = 0;
        *out_utc = timegm(&tmv);
        if (out_all_day) *out_all_day = 1;
        return 0;
    }

    if (vlen < 15) return -1;
    int hh = parse_2dig(val + 9);
    int mm = parse_2dig(val + 11);
    int ss = parse_2dig(val + 13);
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return -1;
    tmv.tm_hour = hh;
    tmv.tm_min = mm;
    tmv.tm_sec = ss;

    int is_utc = (val[15] == 'Z' || val[15] == 'z') ? 1 : 0;
    if (out_all_day) *out_all_day = 0;
    *out_utc = is_utc ? timegm(&tmv) : mktime(&tmv);
    return 0;
}

static long parse_ics_duration_sec(const char *val)
{
    if (!val) return 0;
    while (*val && isspace((unsigned char)*val)) val++;
    int sign = 1;
    if (*val == '-') {
        sign = -1;
        val++;
    } else if (*val == '+') {
        val++;
    }
    if (*val != 'P' && *val != 'p') return 0;
    val++;

    long total = 0;
    int in_time = 0;
    while (*val) {
        if (*val == 'T' || *val == 't') {
            in_time = 1;
            val++;
            continue;
        }
        if (!isdigit((unsigned char)*val)) break;
        char *endp = NULL;
        long num = strtol(val, &endp, 10);
        if (!endp || endp == val || !*endp) break;
        char unit = (char)toupper((unsigned char)*endp);
        if (!in_time) {
            if (unit == 'W') total += num * 7L * 86400L;
            else if (unit == 'D') total += num * 86400L;
        } else {
            if (unit == 'H') total += num * 3600L;
            else if (unit == 'M') total += num * 60L;
            else if (unit == 'S') total += num;
        }
        val = endp + 1;
    }
    return total * sign;
}

static void parse_ics_rrule(const char *val, DtcmIcsEvent *ev)
{
    if (!val || !ev) return;
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", val);

    ev->interval = 1;
    ev->count = 0;
    time_t until_ts = 0;
    int has_until = 0;

    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ";", &saveptr); tok != NULL; tok = strtok_r(NULL, ";", &saveptr)) {
        if (strncasecmp(tok, "FREQ=", 5) == 0) {
            const char *f = tok + 5;
            if (strcasecmp(f, "DAILY") == 0) ev->freq = DTCM_ICS_DAILY;
            else if (strcasecmp(f, "WEEKLY") == 0) ev->freq = DTCM_ICS_WEEKLY;
            else if (strcasecmp(f, "MONTHLY") == 0) ev->freq = DTCM_ICS_MONTHLY;
            else if (strcasecmp(f, "YEARLY") == 0) ev->freq = DTCM_ICS_YEARLY;
        } else if (strncasecmp(tok, "INTERVAL=", 9) == 0) {
            int iv = atoi(tok + 9);
            if (iv >= 1) ev->interval = iv;
        } else if (strncasecmp(tok, "COUNT=", 6) == 0) {
            int cnt = atoi(tok + 6);
            if (cnt >= 1) ev->count = cnt;
        } else if (strncasecmp(tok, "UNTIL=", 6) == 0) {
            if (parse_ics_datetime(tok + 6, NULL, &until_ts, NULL) == 0) {
                has_until = 1;
            }
        }
    }

    if (ev->freq != DTCM_ICS_ONCE && ev->count == 0 && has_until && until_ts > ev->start_utc) {
        long span = (long)(until_ts - ev->start_utc);
        long step = 86400L;
        if (ev->freq == DTCM_ICS_WEEKLY) step = 7L * 86400L;
        else if (ev->freq == DTCM_ICS_MONTHLY) step = 30L * 86400L;
        else if (ev->freq == DTCM_ICS_YEARLY) step = 365L * 86400L;
        step *= (long)(ev->interval > 0 ? ev->interval : 1);
        int approx = (int)(span / step) + 1;
        if (approx < 1) approx = 1;
        ev->count = approx;
    }
}

static void handle_unfolded_line(const char *line,
                                 int *in_vevent,
                                 int *subcomponent_depth,
                                 DtcmIcsEvent *cur,
                                 long *cur_duration,
                                 DtcmIcsEvent **events,
                                 int *count,
                                 int *cap)
{
    if (!line || !line[0]) return;

    if (strcasecmp(line, "BEGIN:VEVENT") == 0) {
        *in_vevent = 1;
        *subcomponent_depth = 0;
        memset(cur, 0, sizeof(*cur));
        cur->interval = 1;
        *cur_duration = 0;
        return;
    }
    if (!*in_vevent) return;

    if (strncasecmp(line, "BEGIN:", 6) == 0) {
        (*subcomponent_depth)++;
        return;
    }
    if (strncasecmp(line, "END:", 4) == 0) {
        if (strcasecmp(line, "END:VEVENT") == 0 && *subcomponent_depth == 0) {
            *in_vevent = 0;
            if (cur->start_utc > 0) {
                if (cur->end_utc <= 0) {
                    if (*cur_duration > 0) {
                        cur->end_utc = cur->start_utc + *cur_duration;
                    } else {
                        cur->end_utc = cur->start_utc + (cur->all_day ? 86400 : 3600);
                    }
                }
                if (!cur->summary[0]) {
                    snprintf(cur->summary, sizeof(cur->summary), "Appointment");
                }
                if (*count >= *cap) {
                    int ncap = *cap ? (*cap * 2) : 16;
                    DtcmIcsEvent *resized = (DtcmIcsEvent *)realloc(
                        *events, sizeof(DtcmIcsEvent) * (size_t)ncap);
                    if (resized) {
                        *events = resized;
                        *cap = ncap;
                    }
                }
                if (*count < *cap) {
                    (*events)[(*count)++] = *cur;
                }
            }
            return;
        }
        if (*subcomponent_depth > 0) (*subcomponent_depth)--;
        return;
    }

    if (*subcomponent_depth > 0) return; /* Ignore VALARM inner properties */

    const char *colon = strchr(line, ':');
    if (!colon) return;
    size_t key_part_len = (size_t)(colon - line);
    char key_part[512];
    if (key_part_len >= sizeof(key_part)) key_part_len = sizeof(key_part) - 1;
    memcpy(key_part, line, key_part_len);
    key_part[key_part_len] = '\0';

    char *semi = strchr(key_part, ';');
    const char *params = "";
    if (semi) {
        *semi = '\0';
        params = semi + 1;
    }
    const char *val = colon + 1;

    if (strcasecmp(key_part, "UID") == 0) {
        ics_unescape_text(val, cur->uid, sizeof(cur->uid));
    } else if (strcasecmp(key_part, "SUMMARY") == 0) {
        ics_unescape_text(val, cur->summary, sizeof(cur->summary));
    } else if (strcasecmp(key_part, "LOCATION") == 0) {
        ics_unescape_text(val, cur->location, sizeof(cur->location));
    } else if (strcasecmp(key_part, "DESCRIPTION") == 0) {
        ics_unescape_text(val, cur->description, sizeof(cur->description));
    } else if (strcasecmp(key_part, "DTSTART") == 0) {
        (void)parse_ics_datetime(val, params, &cur->start_utc, &cur->all_day);
    } else if (strcasecmp(key_part, "DTEND") == 0) {
        (void)parse_ics_datetime(val, params, &cur->end_utc, NULL);
    } else if (strcasecmp(key_part, "DURATION") == 0) {
        *cur_duration = parse_ics_duration_sec(val);
    } else if (strcasecmp(key_part, "RRULE") == 0) {
        parse_ics_rrule(val, cur);
    }
}

int dtcm_ics_parse_file(const char *path, DtcmIcsEvent **out_events, int *out_count)
{
    if (!path || !path[0] || !out_events || !out_count) return -1;
    *out_events = NULL;
    *out_count = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    DtcmIcsEvent *events = NULL;
    int count = 0;
    int cap = 0;

    int in_vevent = 0;
    int sub_depth = 0;
    DtcmIcsEvent cur;
    memset(&cur, 0, sizeof(cur));
    long cur_dur = 0;

    size_t acc_cap = 4096;
    size_t acc_len = 0;
    char *acc = (char *)malloc(acc_cap);
    if (!acc) {
        fclose(fp);
        return -1;
    }
    acc[0] = '\0';

    char raw[2048];
    while (fgets(raw, sizeof(raw), fp)) {
        size_t rlen = strlen(raw);
        while (rlen > 0 && (raw[rlen - 1] == '\n' || raw[rlen - 1] == '\r')) {
            raw[--rlen] = '\0';
        }
        if (rlen > 0 && (raw[0] == ' ' || raw[0] == '\t')) {
            /* RFC 5545 folded continuation line */
            const char *cont = raw + 1;
            size_t clen = rlen - 1;
            if (acc_len + clen + 1 > acc_cap) {
                size_t ncap = (acc_len + clen + 1) * 2;
                char *resized = (char *)realloc(acc, ncap);
                if (!resized) break;
                acc = resized;
                acc_cap = ncap;
            }
            memcpy(acc + acc_len, cont, clen);
            acc_len += clen;
            acc[acc_len] = '\0';
        } else {
            if (acc_len > 0) {
                handle_unfolded_line(acc, &in_vevent, &sub_depth, &cur, &cur_dur,
                                     &events, &count, &cap);
            }
            if (rlen + 1 > acc_cap) {
                size_t ncap = (rlen + 1) * 2;
                char *resized = (char *)realloc(acc, ncap);
                if (!resized) break;
                acc = resized;
                acc_cap = ncap;
            }
            memcpy(acc, raw, rlen + 1);
            acc_len = rlen;
        }
    }
    if (acc_len > 0) {
        handle_unfolded_line(acc, &in_vevent, &sub_depth, &cur, &cur_dur,
                             &events, &count, &cap);
    }

    free(acc);
    fclose(fp);
    *out_events = events;
    *out_count = count;
    return 0;
}

void dtcm_ics_free_events(DtcmIcsEvent *events)
{
    free(events);
}

static void format_utc_stamp(time_t ts, int date_only, char *buf, size_t len)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    gmtime_r(&ts, &tmv);
    if (date_only) {
        snprintf(buf, len, "%04d%02d%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    } else {
        snprintf(buf, len, "%04d%02d%02dT%02d%02d%02dZ",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    }
}

int dtcm_ics_write_file(const char *path, const DtcmIcsEvent *events, int count)
{
    if (!path || !path[0] || (count > 0 && !events) || count < 0) return -1;

    char tmp_path[4200];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    fprintf(fp, "BEGIN:VCALENDAR\r\n");
    fprintf(fp, "VERSION:2.0\r\n");
    fprintf(fp, "PRODID:-//CK-Core CDE//dtcm//EN\r\n");
    fprintf(fp, "CALSCALE:GREGORIAN\r\n");

    char stamp[32];
    format_utc_stamp(time(NULL), 0, stamp, sizeof(stamp));

    for (int i = 0; i < count; ++i) {
        const DtcmIcsEvent *ev = &events[i];
        fprintf(fp, "BEGIN:VEVENT\r\n");
        if (ev->uid[0]) {
            fprintf(fp, "UID:");
            ics_write_escaped(fp, ev->uid);
            fprintf(fp, "\r\n");
        } else {
            fprintf(fp, "UID:dtcm-%lld-%d@ck-core-cde\r\n", (long long)ev->start_utc, i + 1);
        }
        fprintf(fp, "DTSTAMP:%s\r\n", stamp);

        char dtstart[32], dtend[32];
        if (ev->all_day) {
            format_utc_stamp(ev->start_utc, 1, dtstart, sizeof(dtstart));
            time_t end_ts = ev->end_utc > ev->start_utc ? ev->end_utc : (ev->start_utc + 86400);
            format_utc_stamp(end_ts, 1, dtend, sizeof(dtend));
            fprintf(fp, "DTSTART;VALUE=DATE:%s\r\n", dtstart);
            fprintf(fp, "DTEND;VALUE=DATE:%s\r\n", dtend);
        } else {
            format_utc_stamp(ev->start_utc, 0, dtstart, sizeof(dtstart));
            time_t end_ts = ev->end_utc >= ev->start_utc ? ev->end_utc : (ev->start_utc + 3600);
            format_utc_stamp(end_ts, 0, dtend, sizeof(dtend));
            fprintf(fp, "DTSTART:%s\r\n", dtstart);
            fprintf(fp, "DTEND:%s\r\n", dtend);
        }

        fprintf(fp, "SUMMARY:");
        ics_write_escaped(fp, ev->summary[0] ? ev->summary : "Appointment");
        fprintf(fp, "\r\n");

        if (ev->location[0]) {
            fprintf(fp, "LOCATION:");
            ics_write_escaped(fp, ev->location);
            fprintf(fp, "\r\n");
        }
        if (ev->description[0]) {
            fprintf(fp, "DESCRIPTION:");
            ics_write_escaped(fp, ev->description);
            fprintf(fp, "\r\n");
        }

        if (ev->freq != DTCM_ICS_ONCE) {
            const char *fstr = "DAILY";
            if (ev->freq == DTCM_ICS_WEEKLY) fstr = "WEEKLY";
            else if (ev->freq == DTCM_ICS_MONTHLY) fstr = "MONTHLY";
            else if (ev->freq == DTCM_ICS_YEARLY) fstr = "YEARLY";
            fprintf(fp, "RRULE:FREQ=%s", fstr);
            if (ev->interval > 1) fprintf(fp, ";INTERVAL=%d", ev->interval);
            if (ev->count > 0) fprintf(fp, ";COUNT=%d", ev->count);
            fprintf(fp, "\r\n");
        }

        fprintf(fp, "END:VEVENT\r\n");
    }

    fprintf(fp, "END:VCALENDAR\r\n");
    if (fclose(fp) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

void dtcm_ics_compose_what(const DtcmIcsEvent *ev, char *out_what, size_t out_len)
{
    if (!out_what || out_len == 0) return;
    out_what[0] = '\0';
    if (!ev) return;

    const char *sum = ev->summary[0] ? ev->summary : "Appointment";
    if (ev->location[0] && ev->description[0]) {
        snprintf(out_what, out_len, "%s\nLocation: %s\n%s", sum, ev->location, ev->description);
    } else if (ev->location[0]) {
        snprintf(out_what, out_len, "%s\nLocation: %s", sum, ev->location);
    } else if (ev->description[0]) {
        snprintf(out_what, out_len, "%s\n%s", sum, ev->description);
    } else {
        snprintf(out_what, out_len, "%s", sum);
    }
}

void dtcm_ics_decompose_what(const char *what,
                             char *out_summary, size_t sum_len,
                             char *out_location, size_t loc_len,
                             char *out_desc, size_t desc_len)
{
    if (out_summary && sum_len > 0) out_summary[0] = '\0';
    if (out_location && loc_len > 0) out_location[0] = '\0';
    if (out_desc && desc_len > 0) out_desc[0] = '\0';
    if (!what || !what[0]) return;

    const char *nl1 = strchr(what, '\n');
    size_t slen = nl1 ? (size_t)(nl1 - what) : strlen(what);
    if (out_summary && sum_len > 0) {
        if (slen >= sum_len) slen = sum_len - 1;
        memcpy(out_summary, what, slen);
        out_summary[slen] = '\0';
    }
    if (!nl1) return;

    const char *rest = nl1 + 1;
    while (*rest == '\t') rest++;
    if (strncmp(rest, "Location: ", 10) == 0) {
        const char *lstart = rest + 10;
        const char *nl2 = strchr(lstart, '\n');
        size_t llen = nl2 ? (size_t)(nl2 - lstart) : strlen(lstart);
        if (out_location && loc_len > 0) {
            if (llen >= loc_len) llen = loc_len - 1;
            memcpy(out_location, lstart, llen);
            out_location[llen] = '\0';
        }
        if (!nl2) return;
        rest = nl2 + 1;
        while (*rest == '\t') rest++;
    }
    if (out_desc && desc_len > 0 && rest[0]) {
        snprintf(out_desc, desc_len, "%s", rest);
    }
}
