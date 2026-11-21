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

static const char *map_windows_tzid(const char *tzid)
{
    static const struct { const char *win; const char *iana; } table[] = {
        { "UTC",                              "UTC" },
        { "GMT Standard Time",                "Europe/London" },
        { "W. Europe Standard Time",          "Europe/Berlin" },
        { "Central European Standard Time",   "Europe/Warsaw" },
        { "Romance Standard Time",            "Europe/Paris" },
        { "Eastern Standard Time",            "America/New_York" },
        { "Central Standard Time",            "America/Chicago" },
        { "Mountain Standard Time",           "America/Denver" },
        { "Pacific Standard Time",            "America/Los_Angeles" },
        { "Tokyo Standard Time",              "Asia/Tokyo" },
        { "AUS Eastern Standard Time",        "Australia/Sydney" },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcasecmp(tzid, table[i].win) == 0)
            return table[i].iana;
    }
    return tzid;
}

static int extract_tzid_param(const char *params, char *out_tz, size_t out_sz)
{
    if (!params || !out_tz || out_sz == 0) return 0;
    out_tz[0] = '\0';

    const char *p = params;
    while (*p) {
        if (strncasecmp(p, "TZID=", 5) == 0 &&
            (p == params || p[-1] == ';')) {
            p += 5;
            int quoted = 0;
            if (*p == '"') {
                quoted = 1;
                p++;
            }
            size_t j = 0;
            while (*p && (quoted ? (*p != '"') : (*p != ';'))) {
                unsigned char ch = (unsigned char)*p++;
                if (ch < 0x20 || ch >= 0x7f) return 0;
                if (j + 1 < out_sz)
                    out_tz[j++] = (char)ch;
            }
            out_tz[j] = '\0';
            if (out_tz[0] == '\0' || out_tz[0] == '/' || strstr(out_tz, "..") != NULL)
                return 0;
            return 1;
        }
        p++;
    }
    return 0;
}

static time_t mktime_in_tz(struct tm *tmv, const char *tzid)
{
    const char *mapped = map_windows_tzid(tzid);
    const char *orig_tz = getenv("TZ");
    char saved_tz[256];
    int had_tz = 0;

    if (orig_tz) {
        snprintf(saved_tz, sizeof(saved_tz), "%s", orig_tz);
        had_tz = 1;
    }

    char tz_env[256];
    if (strcmp(mapped, "UTC") == 0)
        snprintf(tz_env, sizeof(tz_env), "UTC");
    else
        snprintf(tz_env, sizeof(tz_env), ":%s", mapped);

    setenv("TZ", tz_env, 1);
    tzset();
    tmv->tm_isdst = -1;
    time_t result = mktime(tmv);

    if (had_tz)
        setenv("TZ", saved_tz, 1);
    else
        unsetenv("TZ");
    tzset();
    return result;
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
    if (is_utc) {
        *out_utc = timegm(&tmv);
    } else {
        char tzid[128];
        if (extract_tzid_param(params, tzid, sizeof(tzid)))
            *out_utc = mktime_in_tz(&tmv, tzid);
        else
            *out_utc = mktime(&tmv);
    }
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

static unsigned int parse_byday_list(const char *s)
{
    static const char *names[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
    unsigned int mask = 0;
    if (!s) return 0;
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", s);
    char *sp = NULL;
    for (char *tok = strtok_r(copy, ",", &sp); tok != NULL; tok = strtok_r(NULL, ",", &sp)) {
        /* Skip optional leading ordinal like +1MO or -1FR */
        const char *p = tok;
        while (*p && (*p == '+' || *p == '-' || isdigit((unsigned char)*p))) p++;
        for (int d = 0; d < 7; d++) {
            if (strncasecmp(p, names[d], 2) == 0) {
                mask |= (1u << d);
                break;
            }
        }
    }
    return mask;
}

static void parse_ics_rrule(const char *val, DtcmIcsEvent *ev)
{
    if (!val || !ev) return;
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", val);

    ev->interval = 1;
    ev->count = 0;
    ev->byday_mask = 0;
    ev->until_utc = 0;
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
        } else if (strncasecmp(tok, "BYDAY=", 6) == 0) {
            ev->byday_mask = parse_byday_list(tok + 6);
        } else if (strncasecmp(tok, "UNTIL=", 6) == 0) {
            if (parse_ics_datetime(tok + 6, NULL, &until_ts, NULL) == 0) {
                has_until = 1;
                ev->until_utc = until_ts;
            }
        }
    }

    if (ev->freq != DTCM_ICS_ONCE && ev->count == 0 && ev->byday_mask == 0 &&
        has_until && until_ts > ev->start_utc) {
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
    } else if (strcasecmp(key_part, "CATEGORIES") == 0) {
        char raw_cat[128];
        ics_unescape_text(val, raw_cat, sizeof(raw_cat));
        char *comma = strchr(raw_cat, ',');
        if (comma) *comma = '\0';
        snprintf(cur->category, sizeof(cur->category), "%s", raw_cat);
    } else if (strcasecmp(key_part, "DTSTART") == 0) {
        (void)parse_ics_datetime(val, params, &cur->start_utc, &cur->all_day);
    } else if (strcasecmp(key_part, "DTEND") == 0) {
        (void)parse_ics_datetime(val, params, &cur->end_utc, NULL);
    } else if (strcasecmp(key_part, "DURATION") == 0) {
        *cur_duration = parse_ics_duration_sec(val);
    } else if (strcasecmp(key_part, "RRULE") == 0) {
        parse_ics_rrule(val, cur);
    } else if (strcasecmp(key_part, "EXDATE") == 0) {
        char copy[1024];
        snprintf(copy, sizeof(copy), "%s", val);
        char *sp = NULL;
        for (char *tok = strtok_r(copy, ",", &sp); tok != NULL; tok = strtok_r(NULL, ",", &sp)) {
            if (cur->exdate_count >= 32) break;
            time_t ex_ts = 0;
            if (parse_ics_datetime(tok, params, &ex_ts, NULL) == 0 && ex_ts > 0) {
                cur->exdates[cur->exdate_count++] = ex_ts;
            }
        }
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
        if (ev->category[0]) {
            fprintf(fp, "CATEGORIES:");
            ics_write_escaped(fp, ev->category);
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
            if (ev->byday_mask != 0) {
                static const char *dnames[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
                int first = 1;
                fprintf(fp, ";BYDAY=");
                for (int d = 0; d < 7; d++) {
                    if (ev->byday_mask & (1u << d)) {
                        fprintf(fp, "%s%s", first ? "" : ",", dnames[d]);
                        first = 0;
                    }
                }
            }
            if (ev->until_utc > 0) {
                char ubuf[32];
                format_utc_stamp(ev->until_utc, 0, ubuf, sizeof(ubuf));
                fprintf(fp, ";UNTIL=%s", ubuf);
            } else if (ev->count > 0) {
                fprintf(fp, ";COUNT=%d", ev->count);
            }
            fprintf(fp, "\r\n");
        }

        if (ev->exdate_count > 0) {
            for (int e = 0; e < ev->exdate_count && e < 32; e++) {
                char exbuf[32];
                format_utc_stamp(ev->exdates[e], ev->all_day, exbuf, sizeof(exbuf));
                if (ev->all_day)
                    fprintf(fp, "EXDATE;VALUE=DATE:%s\r\n", exbuf);
                else
                    fprintf(fp, "EXDATE:%s\r\n", exbuf);
            }
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
    size_t off = (size_t)snprintf(out_what, out_len, "%s", sum);
    if (ev->location[0] && off < out_len)
        off += (size_t)snprintf(out_what + off, out_len - off, "\nLocation: %s", ev->location);
    if (ev->category[0] && off < out_len)
        off += (size_t)snprintf(out_what + off, out_len - off, "\nCategory: %s", ev->category);
    if (ev->description[0] && off < out_len)
        (void)snprintf(out_what + off, out_len - off, "\n%s", ev->description);
}

void dtcm_ics_decompose_what_ex(const char *what,
                                char *out_summary, size_t sum_len,
                                char *out_location, size_t loc_len,
                                char *out_category, size_t cat_len,
                                char *out_desc, size_t desc_len)
{
    if (out_summary && sum_len > 0) out_summary[0] = '\0';
    if (out_location && loc_len > 0) out_location[0] = '\0';
    if (out_category && cat_len > 0) out_category[0] = '\0';
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
    for (;;) {
        while (*rest == '\t') rest++;
        if (strncmp(rest, "Location: ", 10) == 0) {
            const char *vstart = rest + 10;
            const char *nl = strchr(vstart, '\n');
            size_t vlen = nl ? (size_t)(nl - vstart) : strlen(vstart);
            if (out_location && loc_len > 0) {
                if (vlen >= loc_len) vlen = loc_len - 1;
                memcpy(out_location, vstart, vlen);
                out_location[vlen] = '\0';
            }
            if (!nl) return;
            rest = nl + 1;
        } else if (strncmp(rest, "Category: ", 10) == 0) {
            const char *vstart = rest + 10;
            const char *nl = strchr(vstart, '\n');
            size_t vlen = nl ? (size_t)(nl - vstart) : strlen(vstart);
            if (out_category && cat_len > 0) {
                if (vlen >= cat_len) vlen = cat_len - 1;
                memcpy(out_category, vstart, vlen);
                out_category[vlen] = '\0';
            }
            if (!nl) return;
            rest = nl + 1;
        } else {
            break;
        }
    }
    if (out_desc && desc_len > 0 && rest[0]) {
        snprintf(out_desc, desc_len, "%s", rest);
    }
}

void dtcm_ics_decompose_what(const char *what,
                             char *out_summary, size_t sum_len,
                             char *out_location, size_t loc_len,
                             char *out_desc, size_t desc_len)
{
    dtcm_ics_decompose_what_ex(what, out_summary, sum_len,
                               out_location, loc_len,
                               NULL, 0,
                               out_desc, desc_len);
}

static int is_exdate_match(const DtcmIcsEvent *ev, time_t candidate)
{
    if (!ev || ev->exdate_count <= 0) return 0;
    struct tm ctm;
    gmtime_r(&candidate, &ctm);
    for (int i = 0; i < ev->exdate_count && i < 32; i++) {
        if (ev->exdates[i] == candidate)
            return 1;
        struct tm etm;
        gmtime_r(&ev->exdates[i], &etm);
        if (etm.tm_year == ctm.tm_year &&
            etm.tm_mon == ctm.tm_mon &&
            etm.tm_mday == ctm.tm_mday)
            return 1;
    }
    return 0;
}

int dtcm_ics_expand_occurrences(const DtcmIcsEvent *ev,
                                time_t range_start,
                                time_t range_end,
                                time_t *out_starts,
                                int max_out)
{
    if (!ev || !out_starts || max_out <= 0 || ev->start_utc <= 0 || range_end < range_start)
        return 0;

    int out = 0;
    if (ev->freq == DTCM_ICS_ONCE) {
        if (ev->start_utc >= range_start && ev->start_utc <= range_end &&
            !is_exdate_match(ev, ev->start_utc)) {
            out_starts[out++] = ev->start_utc;
        }
        return out;
    }

    int interval = ev->interval > 0 ? ev->interval : 1;
    int max_count = ev->count > 0 ? ev->count : 0;
    time_t hard_end = range_end;
    if (ev->until_utc > 0 && ev->until_utc < hard_end)
        hard_end = ev->until_utc;

    int emitted_total = 0;
    struct tm base_tm;
    gmtime_r(&ev->start_utc, &base_tm);

    if (ev->freq == DTCM_ICS_WEEKLY && ev->byday_mask != 0) {
        /* Start from Sunday of the week containing start_utc */
        time_t week_sun = ev->start_utc - (time_t)base_tm.tm_wday * 86400;
        for (int w = 0; w < 2000 && out < max_out; w++) {
            time_t wbase = week_sun + (time_t)w * (time_t)interval * 7 * 86400;
            if (wbase > hard_end)
                break;
            for (int d = 0; d < 7 && out < max_out; d++) {
                if (!(ev->byday_mask & (1u << d)))
                    continue;
                time_t cand = wbase + (time_t)d * 86400;
                if (cand < ev->start_utc)
                    continue;
                if (cand > hard_end)
                    return out;
                emitted_total++;
                if (max_count > 0 && emitted_total > max_count)
                    return out;
                if (cand >= range_start && !is_exdate_match(ev, cand))
                    out_starts[out++] = cand;
            }
        }
        return out;
    }

    for (int step = 0; step < 4000 && out < max_out; step++) {
        time_t cand = 0;
        if (ev->freq == DTCM_ICS_DAILY) {
            cand = ev->start_utc + (time_t)step * (time_t)interval * 86400;
        } else if (ev->freq == DTCM_ICS_WEEKLY) {
            cand = ev->start_utc + (time_t)step * (time_t)interval * 7 * 86400;
        } else if (ev->freq == DTCM_ICS_MONTHLY) {
            struct tm t = base_tm;
            t.tm_mon += step * interval;
            cand = timegm(&t);
        } else if (ev->freq == DTCM_ICS_YEARLY) {
            struct tm t = base_tm;
            t.tm_year += step * interval;
            cand = timegm(&t);
        } else {
            break;
        }
        if (cand > hard_end)
            break;
        emitted_total++;
        if (max_count > 0 && emitted_total > max_count)
            break;
        if (cand >= range_start && !is_exdate_match(ev, cand))
            out_starts[out++] = cand;
    }
    return out;
}
