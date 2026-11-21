/*
 * CDE - Common Desktop Environment
 *
 * Overlaid calendars (CalDAV / read-only .ics / webcal:// subscriptions)
 * persistence, URL normalization, fetch/parse, and range query for dtcm.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#include "overlay_store.h"

static DtcmOverlaySource g_overlays[DTCM_MAX_OVERLAYS];
static int g_overlay_count = 0;

int
dtcm_overlay_normalize_url(const char *raw, char *out, size_t out_len)
{
    if (!raw || !out || out_len < 16)
        return -1;
    while (*raw && isspace((unsigned char)*raw))
        raw++;
    if (!raw[0])
        return -1;

    char trimmed[1024];
    snprintf(trimmed, sizeof(trimmed), "%s", raw);
    size_t len = strlen(trimmed);
    while (len > 0 && isspace((unsigned char)trimmed[len - 1]))
        trimmed[--len] = '\0';
    if (len == 0)
        return -1;

    if (strncasecmp(trimmed, "webcal://", 9) == 0) {
        snprintf(out, out_len, "https://%s", trimmed + 9);
        return 0;
    }
    if (strncasecmp(trimmed, "webcals://", 10) == 0) {
        snprintf(out, out_len, "https://%s", trimmed + 10);
        return 0;
    }
    if (strncasecmp(trimmed, "https://", 8) == 0 ||
        strncasecmp(trimmed, "http://", 7) == 0 ||
        strncasecmp(trimmed, "file://", 7) == 0 ||
        trimmed[0] == '/') {
        snprintf(out, out_len, "%s", trimmed);
        return 0;
    }
    return -1;
}

void
dtcm_overlay_init(void)
{
    dtcm_overlay_shutdown();
    char path[1024];
    if (dtcm_overlay_default_path(path, sizeof(path)) == 0)
        (void)dtcm_overlay_load_file(path);
}

void
dtcm_overlay_shutdown(void)
{
    for (int i = 0; i < g_overlay_count; i++) {
        dtcm_ics_free_events(g_overlays[i].events);
        g_overlays[i].events = NULL;
        g_overlays[i].event_count = 0;
    }
    g_overlay_count = 0;
}

int
dtcm_overlay_default_path(char *out, size_t out_len)
{
    const char *home = getenv("HOME");
    if (!home || !home[0] || !out || out_len < 32)
        return -1;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.dt", home);
    (void)mkdir(dir, 0700);
    snprintf(out, out_len, "%s/.dt/dtcm_overlays", home);
    return 0;
}

int
dtcm_overlay_load_file(const char *path)
{
    if (!path || !path[0])
        return -1;
    dtcm_overlay_shutdown();
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char line[1536];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = '\0';
        if (l == 0 || line[0] == '#')
            continue;
        /* Format: enabled\tcolor\tname\turl */
        char *p1 = strchr(line, '\t');
        if (!p1) continue;
        *p1++ = '\0';
        char *p2 = strchr(p1, '\t');
        if (!p2) continue;
        *p2++ = '\0';
        char *p3 = strchr(p2, '\t');
        if (!p3) continue;
        *p3++ = '\0';

        int enabled = atoi(line) ? 1 : 0;
        int col = atoi(p1);
        if (col <= (int)DTCM_CAT_NONE || col >= (int)DTCM_CAT_COUNT)
            col = (int)DTCM_CAT_WORK;
        (void)dtcm_overlay_add(p2, p3, (DtcmCategory)col, enabled);
    }
    fclose(fp);
    return 0;
}

int
dtcm_overlay_save_file(const char *path)
{
    if (!path || !path[0])
        return -1;
    char tmp[1280];
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    (void)fchmod(fd, 0600);
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    fprintf(fp, "# dtcm overlaid calendars: enabled<TAB>color<TAB>name<TAB>url\n");
    for (int i = 0; i < g_overlay_count; i++) {
        fprintf(fp, "%d\t%d\t%s\t%s\n",
                g_overlays[i].enabled ? 1 : 0,
                (int)g_overlays[i].color,
                g_overlays[i].name,
                g_overlays[i].url);
    }
    if (fclose(fp) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int
dtcm_overlay_count(void)
{
    return g_overlay_count;
}

const DtcmOverlaySource *
dtcm_overlay_get(int idx)
{
    if (idx < 0 || idx >= g_overlay_count)
        return NULL;
    return &g_overlays[idx];
}

int
dtcm_overlay_add(const char *name, const char *url, DtcmCategory color, int enabled)
{
    if (g_overlay_count >= DTCM_MAX_OVERLAYS)
        return -1;
    char norm[1024];
    if (dtcm_overlay_normalize_url(url, norm, sizeof(norm)) != 0)
        return -1;

    DtcmOverlaySource *s = &g_overlays[g_overlay_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", (name && name[0]) ? name : "Subscribed Calendar");
    /* Strip tabs/newlines from name for clean TSV serialization */
    for (char *p = s->name; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    }
    snprintf(s->url, sizeof(s->url), "%s", norm);
    s->color = (color > DTCM_CAT_NONE && color < DTCM_CAT_COUNT) ? color : DTCM_CAT_WORK;
    s->enabled = enabled ? 1 : 0;
    s->read_only = 1;
    return g_overlay_count++;
}

int
dtcm_overlay_remove(int idx)
{
    if (idx < 0 || idx >= g_overlay_count)
        return -1;
    dtcm_ics_free_events(g_overlays[idx].events);
    for (int i = idx; i + 1 < g_overlay_count; i++)
        g_overlays[i] = g_overlays[i + 1];
    g_overlay_count--;
    memset(&g_overlays[g_overlay_count], 0, sizeof(g_overlays[g_overlay_count]));
    return 0;
}

int
dtcm_overlay_set_enabled(int idx, int enabled)
{
    if (idx < 0 || idx >= g_overlay_count)
        return -1;
    g_overlays[idx].enabled = enabled ? 1 : 0;
    return 0;
}

int
dtcm_overlay_set_color(int idx, DtcmCategory color)
{
    if (idx < 0 || idx >= g_overlay_count)
        return -1;
    if (color <= DTCM_CAT_NONE || color >= DTCM_CAT_COUNT)
        color = DTCM_CAT_WORK;
    g_overlays[idx].color = color;
    return 0;
}

#ifdef HAVE_LIBCURL
static size_t
curl_write_fp(void *ptr, size_t size, size_t nmemb, void *stream)
{
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}
#endif

int
dtcm_overlay_fetch_url(const char *url,
                       DtcmIcsEvent **out_events,
                       int *out_count,
                       char *err_buf,
                       size_t err_len)
{
    if (out_events) *out_events = NULL;
    if (out_count) *out_count = 0;
    if (err_buf && err_len > 0) err_buf[0] = '\0';

    char norm[1024];
    if (dtcm_overlay_normalize_url(url, norm, sizeof(norm)) != 0) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Invalid URL");
        return -1;
    }

    if (norm[0] == '/' || strncasecmp(norm, "file://", 7) == 0) {
        const char *fpath = (norm[0] == '/') ? norm : (norm + 7);
        if (dtcm_ics_parse_file(fpath, out_events, out_count) != 0) {
            if (err_buf && err_len > 0)
                snprintf(err_buf, err_len, "Could not read %s", fpath);
            return -1;
        }
        return 0;
    }

#ifdef HAVE_LIBCURL
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    char tmppath[1024];
    snprintf(tmppath, sizeof(tmppath), "%s/dtcm-ovl-XXXXXX", tmpdir);
    int fd = mkstemp(tmppath);
    if (fd < 0) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "mkstemp failed");
        return -1;
    }
    (void)fchmod(fd, 0600);
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        unlink(tmppath);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        unlink(tmppath);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, norm);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_fp);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CK-Core-CDE-dtcm/1.0");
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK || (http_code >= 400 && http_code != 0)) {
        if (err_buf && err_len > 0) {
            if (res != CURLE_OK)
                snprintf(err_buf, err_len, "Fetch error: %s", curl_easy_strerror(res));
            else
                snprintf(err_buf, err_len, "HTTP %ld", http_code);
        }
        unlink(tmppath);
        return -1;
    }

    int prc = dtcm_ics_parse_file(tmppath, out_events, out_count);
    unlink(tmppath);
    if (prc != 0) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "ICS parse failed");
        return -1;
    }
    return 0;
#else
    if (err_buf && err_len > 0) snprintf(err_buf, err_len, "HTTP fetch requires libcurl");
    return -1;
#endif
}

void
dtcm_overlay_replace_events(int idx, DtcmIcsEvent *events, int count, const char *err)
{
    if (idx < 0 || idx >= g_overlay_count) {
        dtcm_ics_free_events(events);
        return;
    }
    dtcm_ics_free_events(g_overlays[idx].events);
    g_overlays[idx].events = events;
    g_overlays[idx].event_count = count > 0 ? count : 0;
    snprintf(g_overlays[idx].last_error, sizeof(g_overlays[idx].last_error), "%s", err ? err : "");
}

int
dtcm_overlay_refresh_one(int idx)
{
    if (idx < 0 || idx >= g_overlay_count)
        return -1;
    DtcmIcsEvent *evs = NULL;
    int cnt = 0;
    char err[256] = "";
    int rc = dtcm_overlay_fetch_url(g_overlays[idx].url, &evs, &cnt, err, sizeof(err));
    if (rc == 0) {
        dtcm_overlay_replace_events(idx, evs, cnt, "");
    } else {
        snprintf(g_overlays[idx].last_error, sizeof(g_overlays[idx].last_error), "%s", err);
    }
    return rc;
}

int
dtcm_overlay_query_range(time_t range_start,
                         time_t range_end,
                         DtcmOverlayOccurrence *out,
                         int max_out)
{
    if (!out || max_out <= 0 || range_end < range_start)
        return 0;
    int total = 0;
    for (int i = 0; i < g_overlay_count && total < max_out; i++) {
        const DtcmOverlaySource *src = &g_overlays[i];
        if (!src->enabled || !src->events || src->event_count <= 0)
            continue;
        for (int e = 0; e < src->event_count && total < max_out; e++) {
            const DtcmIcsEvent *ev = &src->events[e];
            time_t dur = (ev->end_utc > ev->start_utc)
                         ? (ev->end_utc - ev->start_utc)
                         : (ev->all_day ? 86400 : 3600);
            /* Expand starting slightly before range_start to catch multi-day spans */
            time_t qstart = range_start - (dur > 86400 ? dur : 86400);
            time_t starts[64];
            int n = dtcm_ics_expand_occurrences(ev, qstart, range_end, starts, 64);
            for (int k = 0; k < n && total < max_out; k++) {
                time_t st = starts[k];
                time_t en = st + dur;
                if (en < range_start || st > range_end)
                    continue;
                DtcmOverlayOccurrence *o = &out[total++];
                memset(o, 0, sizeof(*o));
                o->start_utc = st;
                o->end_utc = en;
                o->all_day = ev->all_day;
                o->color = src->color;
                snprintf(o->source_name, sizeof(o->source_name), "%s", src->name);
                snprintf(o->summary, sizeof(o->summary), "%s", ev->summary);
                snprintf(o->location, sizeof(o->location), "%s", ev->location);
            }
        }
    }
    return total;
}
