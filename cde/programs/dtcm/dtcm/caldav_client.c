/*
 * CDE - Common Desktop Environment
 *
 * Pure-C CalDAV (RFC 4791) client for Calendar Manager (dtcm).
 * Reuses ics_io.c for RFC 5545 parsing/serialization and libcurl for
 * authenticated HTTPS/TLS transport.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(HAVE_LIBCURL) && HAVE_LIBCURL
#include <curl/curl.h>
#endif

#include "caldav_client.h"

#define CALDAV_MAX_RESP_BYTES (16U * 1024U * 1024U)

int
dtcm_caldav_is_supported(void)
{
#if defined(HAVE_LIBCURL) && HAVE_LIBCURL
    return 1;
#else
    return 0;
#endif
}

int
dtcm_caldav_validate_url(const char *url, char *errbuf, size_t errsz)
{
    if (!url || !url[0]) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "CalDAV URL is empty");
        return -1;
    }
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "CalDAV URL must begin with https:// or http://");
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p <= 0x20 || *p >= 0x7f) {
            if (errbuf && errsz)
                snprintf(errbuf, errsz, "CalDAV URL contains invalid whitespace or control characters");
            return -1;
        }
    }
    return 0;
}

void
dtcm_caldav_free_items(DtcmCaldavItem *items)
{
    free(items);
}

#if defined(HAVE_LIBCURL) && HAVE_LIBCURL

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int overflow;
} MemBuf;

static size_t
curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    MemBuf *mb = (MemBuf *)userdata;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        mb->overflow = 1;
        return 0;
    }
    size_t n = size * nmemb;
    if (n == 0)
        return 0;
    if (mb->len + n + 1 > CALDAV_MAX_RESP_BYTES) {
        mb->overflow = 1;
        return 0;
    }
    if (mb->len + n + 1 > mb->cap) {
        size_t new_cap = mb->cap ? mb->cap * 2 : 8192;
        while (new_cap < mb->len + n + 1)
            new_cap *= 2;
        if (new_cap > CALDAV_MAX_RESP_BYTES)
            new_cap = CALDAV_MAX_RESP_BYTES;
        char *nb = (char *)realloc(mb->data, new_cap);
        if (!nb) {
            mb->overflow = 1;
            return 0;
        }
        mb->data = nb;
        mb->cap = new_cap;
    }
    memcpy(mb->data + mb->len, ptr, n);
    mb->len += n;
    mb->data[mb->len] = '\0';
    return n;
}

static void
apply_common_curl_opts(CURL *curl, const DtcmCaldavConfig *cfg, const char *url,
                       MemBuf *resp)
{
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CK-Core-CDE-dtcm/3.0");
#ifdef CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http");
#endif
    if (cfg->verify_tls) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (cfg->username[0]) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg->username);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg->password);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
}

/*
 * Case-insensitive substring search for XML tag local names regardless of
 * namespace prefix (e.g. <D:href>, <d:href>, <href>).
 */
static const char *
find_xml_tag_open(const char *hay, const char *local_name, const char **content_start)
{
    size_t nlen = strlen(local_name);
    const char *p = hay;

    while ((p = strchr(p, '<')) != NULL) {
        const char *q = p + 1;
        if (*q == '/' || *q == '?' || *q == '!') {
            p++;
            continue;
        }
        /* Skip optional namespace prefix up to ':' */
        const char *name = q;
        while (*q && *q != '>' && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && *q != '/') {
            if (*q == ':')
                name = q + 1;
            q++;
        }
        if ((size_t)(q - name) == nlen && strncasecmp(name, local_name, nlen) == 0) {
            const char *end = strchr(q, '>');
            if (!end)
                return NULL;
            if (end > p && *(end - 1) == '/') {
                /* Self-closing empty tag */
                p = end + 1;
                continue;
            }
            if (content_start)
                *content_start = end + 1;
            return p;
        }
        p++;
    }
    return NULL;
}

static const char *
find_xml_tag_close(const char *hay, const char *local_name)
{
    size_t nlen = strlen(local_name);
    const char *p = hay;

    while ((p = strstr(p, "</")) != NULL) {
        const char *q = p + 2;
        const char *name = q;
        while (*q && *q != '>' && *q != ' ' && *q != '\t') {
            if (*q == ':')
                name = q + 1;
            q++;
        }
        if ((size_t)(q - name) == nlen && strncasecmp(name, local_name, nlen) == 0)
            return p;
        p += 2;
    }
    return NULL;
}

static void
trim_copy(const char *start, const char *end, char *out, size_t out_sz)
{
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n'))
        start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        end--;
    size_t len = (size_t)(end - start);
    if (len >= out_sz)
        len = out_sz - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

static void
resolve_href_url(const char *base_url, const char *href, char *out, size_t out_sz)
{
    if (strncmp(href, "https://", 8) == 0 || strncmp(href, "http://", 7) == 0) {
        snprintf(out, out_sz, "%s", href);
        return;
    }
    /* Extract scheme://host[:port] from base_url */
    const char *scheme_end = strstr(base_url, "://");
    if (!scheme_end) {
        snprintf(out, out_sz, "%s", href);
        return;
    }
    const char *path_start = strchr(scheme_end + 3, '/');
    size_t origin_len = path_start ? (size_t)(path_start - base_url) : strlen(base_url);
    if (href[0] == '/') {
        snprintf(out, out_sz, "%.*s%s", (int)origin_len, base_url, href);
    } else {
        size_t blen = strlen(base_url);
        int has_slash = (blen > 0 && base_url[blen - 1] == '/');
        snprintf(out, out_sz, "%s%s%s", base_url, has_slash ? "" : "/", href);
    }
}

static int
ends_with_ics(const char *s)
{
    size_t len = strlen(s);
    return (len >= 4 && strcasecmp(s + len - 4, ".ics") == 0);
}

int
dtcm_caldav_propfind(const DtcmCaldavConfig *cfg,
                     DtcmCaldavItem **out_items,
                     int *out_count,
                     char *errbuf,
                     size_t errsz)
{
    if (!cfg || !out_items || !out_count)
        return -1;
    *out_items = NULL;
    *out_count = 0;

    if (dtcm_caldav_validate_url(cfg->url, errbuf, errsz) != 0)
        return -1;

    CURL *curl = curl_easy_init();
    if (!curl) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "curl_easy_init failed");
        return -1;
    }

    static const char propfind_xml[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<D:propfind xmlns:D=\"DAV:\">\n"
        "  <D:prop>\n"
        "    <D:getetag/>\n"
        "    <D:getcontenttype/>\n"
        "  </D:prop>\n"
        "</D:propfind>\n";

    MemBuf resp = { 0 };
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Depth: 1");
    headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");

    apply_common_curl_opts(curl, cfg, cfg->url, &resp);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, propfind_xml);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(sizeof(propfind_xml) - 1));

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "PROPFIND failed: %s", curl_easy_strerror(rc));
        free(resp.data);
        return -1;
    }
    if (http_code != 207 && http_code != 200) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "PROPFIND HTTP error %ld", http_code);
        free(resp.data);
        return -1;
    }

    DtcmCaldavItem *items = NULL;
    int count = 0;
    int cap = 0;

    const char *cursor = resp.data ? resp.data : "";
    const char *rstart = NULL;
    while (find_xml_tag_open(cursor, "response", &rstart) != NULL) {
        const char *rend = find_xml_tag_close(rstart, "response");
        if (!rend)
            break;

        size_t block_len = (size_t)(rend - rstart);
        char *block = (char *)malloc(block_len + 1);
        if (!block)
            break;
        memcpy(block, rstart, block_len);
        block[block_len] = '\0';

        const char *hs = NULL;
        const char *es = NULL;
        const char *cts = NULL;
        char href_raw[1024] = "";
        char etag[256] = "";
        char ctype[128] = "";

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
        free(block);

        if (href_raw[0] &&
            (ends_with_ics(href_raw) || strstr(ctype, "text/calendar") != NULL)) {
            if (count == cap) {
                int new_cap = cap ? cap * 2 : 16;
                DtcmCaldavItem *ni = (DtcmCaldavItem *)realloc(
                    items, (size_t)new_cap * sizeof(DtcmCaldavItem));
                if (!ni)
                    break;
                items = ni;
                cap = new_cap;
            }
            memset(&items[count], 0, sizeof(items[count]));
            resolve_href_url(cfg->url, href_raw, items[count].href, sizeof(items[count].href));
            snprintf(items[count].etag, sizeof(items[count].etag), "%s", etag);
            count++;
        }

        cursor = rend + 2;
    }

    free(resp.data);
    *out_items = items;
    *out_count = count;
    return 0;
}

static int
make_temp_ics_path(char *path_out, size_t path_sz)
{
    const char *td = getenv("TMPDIR");
    if (!td || !td[0])
        td = "/tmp";
    snprintf(path_out, path_sz, "%s/dtcm-caldav-XXXXXX", td);
    int fd = mkstemp(path_out);
    if (fd < 0)
        return -1;
    (void)fchmod(fd, 0600);
    return fd;
}

int
dtcm_caldav_get_events(const DtcmCaldavConfig *cfg,
                       const char *item_url,
                       DtcmIcsEvent **out_events,
                       int *out_count,
                       char *errbuf,
                       size_t errsz)
{
    if (!cfg || !item_url || !out_events || !out_count)
        return -1;
    *out_events = NULL;
    *out_count = 0;

    if (dtcm_caldav_validate_url(item_url, errbuf, errsz) != 0)
        return -1;

    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    MemBuf resp = { 0 };
    apply_common_curl_opts(curl, cfg, item_url, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || http_code < 200 || http_code >= 300) {
        if (errbuf && errsz) {
            if (rc != CURLE_OK)
                snprintf(errbuf, errsz, "GET failed: %s", curl_easy_strerror(rc));
            else
                snprintf(errbuf, errsz, "GET HTTP error %ld", http_code);
        }
        free(resp.data);
        return -1;
    }

    char tmp_path[512];
    int fd = make_temp_ics_path(tmp_path, sizeof(tmp_path));
    if (fd < 0) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "Failed to create temporary .ics file");
        free(resp.data);
        return -1;
    }
    if (resp.data && resp.len > 0) {
        size_t written = 0;
        while (written < resp.len) {
            ssize_t w = write(fd, resp.data + written, resp.len - written);
            if (w <= 0)
                break;
            written += (size_t)w;
        }
    }
    close(fd);
    free(resp.data);

    int prc = dtcm_ics_parse_file(tmp_path, out_events, out_count);
    (void)unlink(tmp_path);
    if (prc != 0 && errbuf && errsz)
        snprintf(errbuf, errsz, "Failed to parse downloaded iCalendar payload");
    return prc;
}

static void
sanitize_uid_filename(const char *uid, char *out, size_t out_sz)
{
    size_t j = 0;
    if (!uid || !uid[0]) {
        snprintf(out, out_sz, "event-%lu", (unsigned long)time(NULL));
        return;
    }
    for (size_t i = 0; uid[i] && j + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)uid[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '@')
            out[j++] = (char)c;
        else if (c == '.' && j > 0 && out[j - 1] != '.')
            out[j++] = '.';
        else
            out[j++] = '_';
    }
    out[j] = '\0';
}

typedef struct {
    char etag[256];
} EtagCapture;

static size_t
curl_etag_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    EtagCapture *ec = (EtagCapture *)userdata;
    size_t total = size * nitems;
    if (ec && total > 5 && strncasecmp(buffer, "ETag:", 5) == 0) {
        const char *s = buffer + 5;
        const char *e = buffer + total;
        while (s < e && (*s == ' ' || *s == '\t'))
            s++;
        while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t'))
            e--;
        size_t len = (size_t)(e - s);
        if (len >= sizeof(ec->etag))
            len = sizeof(ec->etag) - 1;
        memcpy(ec->etag, s, len);
        ec->etag[len] = '\0';
    }
    return total;
}

int
dtcm_caldav_put_event_ex(const DtcmCaldavConfig *cfg,
                         const DtcmIcsEvent *ev,
                         char *out_href,
                         size_t href_sz,
                         char *out_etag,
                         size_t etag_sz,
                         char *errbuf,
                         size_t errsz)
{
    if (!cfg || !ev)
        return -1;
    if (out_href && href_sz) out_href[0] = '\0';
    if (out_etag && etag_sz) out_etag[0] = '\0';
    if (dtcm_caldav_validate_url(cfg->url, errbuf, errsz) != 0)
        return -1;

    char tmp_path[512];
    int fd = make_temp_ics_path(tmp_path, sizeof(tmp_path));
    if (fd < 0)
        return -1;
    close(fd);

    if (dtcm_ics_write_file(tmp_path, ev, 1) != 0) {
        (void)unlink(tmp_path);
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "Failed to serialize event to iCalendar");
        return -1;
    }

    FILE *fp = fopen(tmp_path, "rb");
    if (!fp) {
        (void)unlink(tmp_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsz <= 0 || (size_t)fsz > CALDAV_MAX_RESP_BYTES) {
        fclose(fp);
        (void)unlink(tmp_path);
        return -1;
    }
    char *payload = (char *)malloc((size_t)fsz + 1);
    if (!payload) {
        fclose(fp);
        (void)unlink(tmp_path);
        return -1;
    }
    size_t nread = fread(payload, 1, (size_t)fsz, fp);
    fclose(fp);
    (void)unlink(tmp_path);
    payload[nread] = '\0';

    char safe_uid[256];
    char put_url[1536];
    sanitize_uid_filename(ev->uid, safe_uid, sizeof(safe_uid));
    size_t ulen = strlen(cfg->url);
    int has_slash = (ulen > 0 && cfg->url[ulen - 1] == '/');
    snprintf(put_url, sizeof(put_url), "%s%s%s.ics",
             cfg->url, has_slash ? "" : "/", safe_uid);

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(payload);
        return -1;
    }

    MemBuf resp = { 0 };
    EtagCapture ec = { 0 };
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: text/calendar; charset=utf-8");

    apply_common_curl_opts(curl, cfg, put_url, &resp);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)nread);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_etag_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ec);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(payload);
    free(resp.data);

    if (rc != CURLE_OK) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "PUT failed: %s", curl_easy_strerror(rc));
        return -1;
    }
    if (http_code != 200 && http_code != 201 && http_code != 204) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "PUT HTTP error %ld", http_code);
        return -1;
    }
    if (out_href && href_sz)
        snprintf(out_href, href_sz, "%s", put_url);
    if (out_etag && etag_sz)
        snprintf(out_etag, etag_sz, "%s", ec.etag);
    return 0;
}

int
dtcm_caldav_put_event(const DtcmCaldavConfig *cfg,
                      const DtcmIcsEvent *ev,
                      char *errbuf,
                      size_t errsz)
{
    return dtcm_caldav_put_event_ex(cfg, ev, NULL, 0, NULL, 0, errbuf, errsz);
}

#else /* !HAVE_LIBCURL */

int
dtcm_caldav_propfind(const DtcmCaldavConfig *cfg, DtcmCaldavItem **out_items,
                     int *out_count, char *errbuf, size_t errsz)
{
    (void)cfg;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;
    if (errbuf && errsz)
        snprintf(errbuf, errsz, "CalDAV support requires libcurl (rebuild with libcurl4-openssl-dev)");
    return -1;
}

int
dtcm_caldav_get_events(const DtcmCaldavConfig *cfg, const char *item_url,
                       DtcmIcsEvent **out_events, int *out_count,
                       char *errbuf, size_t errsz)
{
    (void)cfg;
    (void)item_url;
    if (out_events) *out_events = NULL;
    if (out_count) *out_count = 0;
    if (errbuf && errsz)
        snprintf(errbuf, errsz, "CalDAV support requires libcurl");
    return -1;
}

int
dtcm_caldav_put_event_ex(const DtcmCaldavConfig *cfg, const DtcmIcsEvent *ev,
                         char *out_href, size_t href_sz,
                         char *out_etag, size_t etag_sz,
                         char *errbuf, size_t errsz)
{
    (void)cfg;
    (void)ev;
    if (out_href && href_sz) out_href[0] = '\0';
    if (out_etag && etag_sz) out_etag[0] = '\0';
    if (errbuf && errsz)
        snprintf(errbuf, errsz, "CalDAV support requires libcurl");
    return -1;
}

int
dtcm_caldav_put_event(const DtcmCaldavConfig *cfg, const DtcmIcsEvent *ev,
                      char *errbuf, size_t errsz)
{
    return dtcm_caldav_put_event_ex(cfg, ev, NULL, 0, NULL, 0, errbuf, errsz);
}

#endif /* HAVE_LIBCURL */

static int
profile_path(char *out, size_t out_sz)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return -1;
    snprintf(out, out_sz, "%s/.dt/dtcm_caldav.conf", home);
    return 0;
}

int
dtcm_caldav_load_profile(DtcmCaldavConfig *cfg)
{
    char path[512];
    char line[1280];

    if (!cfg)
        return -1;
    memset(cfg, 0, sizeof(*cfg));
    cfg->verify_tls = 1;
    cfg->auto_sync_mins = 0;

    if (profile_path(path, sizeof(path)) != 0)
        return -1;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (strncmp(line, "url=", 4) == 0)
            snprintf(cfg->url, sizeof(cfg->url), "%s", line + 4);
        else if (strncmp(line, "username=", 9) == 0)
            snprintf(cfg->username, sizeof(cfg->username), "%s", line + 9);
        else if (strncmp(line, "verify_tls=", 11) == 0)
            cfg->verify_tls = (atoi(line + 11) != 0) ? 1 : 0;
        else if (strncmp(line, "auto_sync_mins=", 15) == 0) {
            int mins = atoi(line + 15);
            cfg->auto_sync_mins = (mins > 0 && mins <= 1440) ? mins : 0;
        }
    }
    fclose(fp);
    return 0;
}

int
dtcm_caldav_save_profile(const DtcmCaldavConfig *cfg)
{
    char dir[512];
    char path[512];
    const char *home = getenv("HOME");

    if (!cfg || !home || !home[0])
        return -1;

    snprintf(dir, sizeof(dir), "%s/.dt", home);
    (void)mkdir(dir, 0700);

    if (profile_path(path, sizeof(path)) != 0)
        return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    (void)fchmod(fd, 0600);

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return -1;
    }
    int mins = (cfg->auto_sync_mins > 0 && cfg->auto_sync_mins <= 1440) ? cfg->auto_sync_mins : 0;
    /* Intentionally never persist plaintext password to disk */
    fprintf(fp, "url=%s\nusername=%s\nverify_tls=%d\nauto_sync_mins=%d\n",
            cfg->url, cfg->username, cfg->verify_tls ? 1 : 0, mins);
    fclose(fp);
    return 0;
}

static int
state_path(char *out, size_t out_sz)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return -1;
    snprintf(out, out_sz, "%s/.dt/dtcm_caldav_state", home);
    return 0;
}

void
dtcm_caldav_state_free(DtcmCaldavSyncState *st)
{
    if (!st)
        return;
    free(st->entries);
    st->entries = NULL;
    st->count = 0;
    st->cap = 0;
}

const DtcmCaldavStateEntry *
dtcm_caldav_state_find_href(const DtcmCaldavSyncState *st,
                            const char *url,
                            const char *href)
{
    if (!st || !url || !href)
        return NULL;
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->entries[i].url, url) == 0 &&
            strcmp(st->entries[i].href, href) == 0)
            return &st->entries[i];
    }
    return NULL;
}

const DtcmCaldavStateEntry *
dtcm_caldav_state_find_uid(const DtcmCaldavSyncState *st,
                           const char *url,
                           const char *uid)
{
    if (!st || !url || !uid || !uid[0])
        return NULL;
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->entries[i].url, url) == 0 &&
            strcmp(st->entries[i].uid, uid) == 0)
            return &st->entries[i];
    }
    return NULL;
}

int
dtcm_caldav_state_upsert(DtcmCaldavSyncState *st,
                         const char *url,
                         const char *href,
                         const char *uid,
                         const char *etag)
{
    if (!st || !url || !href)
        return -1;
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->entries[i].url, url) == 0 &&
            (strcmp(st->entries[i].href, href) == 0 ||
             (uid && uid[0] && strcmp(st->entries[i].uid, uid) == 0))) {
            snprintf(st->entries[i].href, sizeof(st->entries[i].href), "%s", href);
            if (uid && uid[0])
                snprintf(st->entries[i].uid, sizeof(st->entries[i].uid), "%s", uid);
            snprintf(st->entries[i].etag, sizeof(st->entries[i].etag), "%s", etag ? etag : "");
            return 0;
        }
    }
    if (st->count == st->cap) {
        int ncap = st->cap ? st->cap * 2 : 32;
        DtcmCaldavStateEntry *ne = (DtcmCaldavStateEntry *)realloc(
            st->entries, (size_t)ncap * sizeof(DtcmCaldavStateEntry));
        if (!ne)
            return -1;
        st->entries = ne;
        st->cap = ncap;
    }
    DtcmCaldavStateEntry *e = &st->entries[st->count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->url, sizeof(e->url), "%s", url);
    snprintf(e->href, sizeof(e->href), "%s", href);
    snprintf(e->uid, sizeof(e->uid), "%s", uid ? uid : "");
    snprintf(e->etag, sizeof(e->etag), "%s", etag ? etag : "");
    return 0;
}

int
dtcm_caldav_state_load(DtcmCaldavSyncState *st)
{
    char path[512];
    char line[3072];

    if (!st)
        return -1;
    memset(st, 0, sizeof(*st));

    if (state_path(path, sizeof(path)) != 0)
        return 0;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1++ = '\0';
        char *t2 = strchr(t1, '\t');
        if (!t2) continue;
        *t2++ = '\0';
        char *t3 = strchr(t2, '\t');
        if (!t3) continue;
        *t3++ = '\0';
        (void)dtcm_caldav_state_upsert(st, line, t1, t2, t3);
    }
    fclose(fp);
    return 0;
}

int
dtcm_caldav_state_save(const DtcmCaldavSyncState *st)
{
    char dir[512];
    char path[512];
    const char *home = getenv("HOME");

    if (!st || !home || !home[0])
        return -1;

    snprintf(dir, sizeof(dir), "%s/.dt", home);
    (void)mkdir(dir, 0700);
    if (state_path(path, sizeof(path)) != 0)
        return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    (void)fchmod(fd, 0600);

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return -1;
    }
    for (int i = 0; i < st->count; i++) {
        fprintf(fp, "%s\t%s\t%s\t%s\n",
                st->entries[i].url,
                st->entries[i].href,
                st->entries[i].uid,
                st->entries[i].etag);
    }
    fclose(fp);
    return 0;
}

static const char *
secret_tool_bin(void)
{
    const char *b = getenv("DTCM_SECRET_TOOL_BIN");
    return (b && b[0]) ? b : "secret-tool";
}

int
dtcm_caldav_keyring_store(const char *url, const char *username, const char *password)
{
    int pipefd[2];
    pid_t pid;

    if (!url || !url[0] || !username || !username[0] || !password || !password[0])
        return -1;
    if (pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const char *bin = secret_tool_bin();
        char *argv[] = {
            (char *)bin,
            "store",
            "--label=CDE Calendar CalDAV",
            "application", "dtcm",
            "service", "caldav",
            "url", (char *)url,
            "username", (char *)username,
            NULL
        };
        execvp(bin, argv);
        _exit(127);
    }

    close(pipefd[0]);
    size_t plen = strlen(password);
    size_t off = 0;
    while (off < plen) {
        ssize_t w = write(pipefd[1], password + off, plen - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    close(pipefd[1]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (errno == ECHILD)
            return 0;
        return -1;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int
dtcm_caldav_keyring_lookup(const char *url, const char *username,
                           char *out_pass, size_t pass_sz)
{
    int pipefd[2];
    pid_t pid;

    if (!url || !url[0] || !username || !username[0] || !out_pass || pass_sz == 0)
        return -1;
    out_pass[0] = '\0';
    if (pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const char *bin = secret_tool_bin();
        char *argv[] = {
            (char *)bin,
            "lookup",
            "application", "dtcm",
            "service", "caldav",
            "url", (char *)url,
            "username", (char *)username,
            NULL
        };
        execvp(bin, argv);
        _exit(127);
    }

    close(pipefd[1]);
    size_t total = 0;
    while (total + 1 < pass_sz) {
        ssize_t n = read(pipefd[0], out_pass + total, pass_sz - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    out_pass[total] = '\0';
    close(pipefd[0]);

    while (total > 0 && (out_pass[total - 1] == '\n' || out_pass[total - 1] == '\r'))
        out_pass[--total] = '\0';

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (errno == ECHILD)
            return (total > 0) ? 0 : -1;
        return -1;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0 && total > 0) ? 0 : -1;
}

int
dtcm_caldav_keyring_clear(const char *url, const char *username)
{
    if (!url || !url[0] || !username || !username[0])
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const char *bin = secret_tool_bin();
        char *argv[] = {
            (char *)bin,
            "clear",
            "application", "dtcm",
            "service", "caldav",
            "url", (char *)url,
            "username", (char *)username,
            NULL
        };
        execvp(bin, argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (errno == ECHILD)
            return 0;
        return -1;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}
