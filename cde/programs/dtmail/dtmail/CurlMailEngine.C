/*
 * CDE - Common Desktop Environment
 *
 * Modern TLS IMAP/SMTP client built on libcurl for dtmail:
 *  - IMAPS (993) / IMAP+STARTTLS (143) folder list, incremental UID sync,
 *    and two-way flag sync (\Seen, \Flagged, \Deleted)
 *  - SMTPS (465) / SMTP+STARTTLS (587) submission with Password or XOAUTH2
 *  - Safe mbox append with RFC 4155 "From " escaping
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#include "CurlMailEngine.h"

int
dtmail_mbox_append_rfc5322(const char *mbox_path,
                           const char *from_addr,
                           const char *raw_msg,
                           size_t raw_len)
{
    if (!mbox_path || !mbox_path[0] || !raw_msg || raw_len == 0)
        return -1;

    int fd = open(mbox_path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    FILE *fp = fdopen(fd, "ab");
    if (!fp) {
        close(fd);
        return -1;
    }

    char clean_from[256];
    snprintf(clean_from, sizeof(clean_from), "%s",
             (from_addr && from_addr[0]) ? from_addr : "MAILER-DAEMON");
    for (char *p = clean_from; *p; p++) {
        if (isspace((unsigned char)*p)) *p = '_';
    }

    time_t now_t = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now_t, &tm_utc);
    char cbuf[32];
    asctime_r(&tm_utc, cbuf);
    size_t clen = strlen(cbuf);
    while (clen > 0 && (cbuf[clen - 1] == '\n' || cbuf[clen - 1] == '\r'))
        cbuf[--clen] = '\0';

    fprintf(fp, "From %s %s\n", clean_from, cbuf);

    /* Write line-by-line, normalizing CRLF -> LF and '>' escaping "From " lines */
    int in_headers = 1;
    size_t i = 0;
    while (i < raw_len) {
        size_t ls = i;
        while (i < raw_len && raw_msg[i] != '\n')
            i++;
        size_t le = i;
        if (le > ls && raw_msg[le - 1] == '\r')
            le--;
        if (i < raw_len && raw_msg[i] == '\n')
            i++;

        size_t llen = le - ls;
        if (in_headers && llen == 0) {
            in_headers = 0;
        } else if (!in_headers && llen >= 5 && memcmp(raw_msg + ls, "From ", 5) == 0) {
            fputc('>', fp);
        }
        if (llen > 0)
            fwrite(raw_msg + ls, 1, llen, fp);
        fputc('\n', fp);
    }
    fputc('\n', fp);

    return (fclose(fp) == 0) ? 0 : -1;
}

#ifdef HAVE_LIBCURL

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} MemBuf;

static size_t
membuf_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    MemBuf *mb = (MemBuf *)userdata;
    size_t n = size * nmemb;
    if (n == 0) return 0;
    if (mb->len + n + 1 > mb->cap) {
        size_t ncap = mb->cap ? mb->cap * 2 : 4096;
        while (ncap < mb->len + n + 1)
            ncap *= 2;
        char *nd = (char *)realloc(mb->data, ncap);
        if (!nd) return 0;
        mb->data = nd;
        mb->cap = ncap;
    }
    memcpy(mb->data + mb->len, ptr, n);
    mb->len += n;
    mb->data[mb->len] = '\0';
    return n;
}

static void
configure_curl_auth_and_tls(CURL *curl, const DtMailAccount *acct,
                            const char *secret, DtMailTlsMode tls_mode)
{
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    if (tls_mode == DTMAIL_TLS_STARTTLS) {
        curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    }

    const char *user = acct->username[0] ? acct->username : acct->email;
    if (user && user[0])
        curl_easy_setopt(curl, CURLOPT_USERNAME, user);

    if (secret && secret[0]) {
        if (acct->auth_mode == DTMAIL_AUTH_OAUTH2) {
            curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, secret);
        } else {
            curl_easy_setopt(curl, CURLOPT_PASSWORD, secret);
        }
    }
}

static void
build_imap_base_url(const DtMailAccount *acct, char *out, size_t out_len)
{
    const char *scheme = (acct->imap_tls == DTMAIL_TLS_IMPLICIT) ? "imaps" : "imap";
    int port = acct->imap_port > 0 ? acct->imap_port
                                   : (acct->imap_tls == DTMAIL_TLS_IMPLICIT ? 993 : 143);
    snprintf(out, out_len, "%s://%s:%d", scheme, acct->imap_host, port);
}

int
dtmail_imap_test_connection(const DtMailAccount *acct,
                            const char *secret,
                            char *err_buf,
                            size_t err_len)
{
    if (err_buf && err_len > 0) err_buf[0] = '\0';
    if (!acct || !acct->imap_host[0]) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Missing IMAP host");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char url[512];
    build_imap_base_url(acct, url, sizeof(url));

    MemBuf mb = { NULL, 0, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "NOOP");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    configure_curl_auth_and_tls(curl, acct, secret, acct->imap_tls);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK && err_buf && err_len > 0)
        snprintf(err_buf, err_len, "IMAP connection failed: %s", curl_easy_strerror(res));

    curl_easy_cleanup(curl);
    free(mb.data);
    return (res == CURLE_OK) ? 0 : -1;
}

int
dtmail_imap_list_folders(const DtMailAccount *acct,
                         const char *secret,
                         char folders[][128],
                         int max_folders,
                         char *err_buf,
                         size_t err_len)
{
    if (err_buf && err_len > 0) err_buf[0] = '\0';
    if (!acct || !acct->imap_host[0] || !folders || max_folders <= 0)
        return -1;

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char url[512];
    build_imap_base_url(acct, url, sizeof(url));

    MemBuf mb = { NULL, 0, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    configure_curl_auth_and_tls(curl, acct, secret, acct->imap_tls);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        if (err_buf && err_len > 0)
            snprintf(err_buf, err_len, "IMAP LIST failed: %s", curl_easy_strerror(res));
        free(mb.data);
        return -1;
    }

    int count = 0;
    if (mb.data) {
        char *sp = NULL;
        for (char *line = strtok_r(mb.data, "\r\n", &sp);
             line != NULL && count < max_folders;
             line = strtok_r(NULL, "\r\n", &sp)) {
            /* Format: * LIST (\Flags) "/" "INBOX"  or  * LIST () "." INBOX */
            if (strncasecmp(line, "* LIST ", 7) != 0)
                continue;
            const char *p = strchr(line, ')');
            if (!p) continue;
            p++;
            while (*p == ' ') p++;
            /* Skip hierarchy delimiter token (quoted or NIL) */
            if (*p == '"') {
                p++;
                while (*p && *p != '"') p++;
                if (*p == '"') p++;
            } else {
                while (*p && *p != ' ') p++;
            }
            while (*p == ' ') p++;
            if (!*p) continue;
            char name[128];
            if (*p == '"') {
                p++;
                size_t o = 0;
                while (*p && *p != '"' && o + 1 < sizeof(name))
                    name[o++] = *p++;
                name[o] = '\0';
            } else {
                snprintf(name, sizeof(name), "%s", p);
            }
            if (name[0]) {
                snprintf(folders[count], 128, "%s", name);
                count++;
            }
        }
    }
    free(mb.data);
    return count;
}

static int
uid_list_contains(const unsigned long *uids, int count, unsigned long target)
{
    for (int i = 0; i < count; i++) {
        if (uids[i] == target) return 1;
    }
    return 0;
}

static void
load_uid_file(const char *uid_path, unsigned long **out_uids, int *out_count)
{
    *out_uids = NULL;
    *out_count = 0;
    FILE *fp = fopen(uid_path, "r");
    if (!fp) return;
    int cap = 64;
    unsigned long *arr = (unsigned long *)malloc((size_t)cap * sizeof(unsigned long));
    if (!arr) { fclose(fp); return; }
    int n = 0;
    unsigned long u = 0;
    while (fscanf(fp, "%lu", &u) == 1) {
        if (n == cap) {
            cap *= 2;
            unsigned long *na = (unsigned long *)realloc(arr, (size_t)cap * sizeof(unsigned long));
            if (!na) break;
            arr = na;
        }
        arr[n++] = u;
    }
    fclose(fp);
    *out_uids = arr;
    *out_count = n;
}

static void
save_uid_file(const char *uid_path, const unsigned long *uids, int count)
{
    char tmp[1280];
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", uid_path);
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    (void)fchmod(fd, 0600);
    FILE *fp = fdopen(fd, "w");
    if (!fp) { close(fd); unlink(tmp); return; }
    for (int i = 0; i < count; i++)
        fprintf(fp, "%lu\n", uids[i]);
    if (fclose(fp) == 0)
        (void)rename(tmp, uid_path);
    else
        unlink(tmp);
}

static void
extract_from_header(const char *raw, size_t len, char *out, size_t out_len)
{
    snprintf(out, out_len, "MAILER-DAEMON");
    if (!raw || len == 0) return;
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && raw[i] != '\n') i++;
        size_t le = i;
        if (le > ls && raw[le - 1] == '\r') le--;
        if (i < len && raw[i] == '\n') i++;
        if (le == ls) break; /* end of headers */
        if (le - ls > 5 && strncasecmp(raw + ls, "From:", 5) == 0) {
            const char *p = raw + ls + 5;
            const char *end = raw + le;
            const char *lt = (const char *)memchr(p, '<', (size_t)(end - p));
            const char *gt = lt ? (const char *)memchr(lt + 1, '>', (size_t)(end - lt - 1)) : NULL;
            if (lt && gt && gt > lt + 1) {
                size_t alen = (size_t)(gt - lt - 1);
                if (alen >= out_len) alen = out_len - 1;
                memcpy(out, lt + 1, alen);
                out[alen] = '\0';
            } else {
                while (p < end && isspace((unsigned char)*p)) p++;
                size_t alen = (size_t)(end - p);
                if (alen >= out_len) alen = out_len - 1;
                memcpy(out, p, alen);
                out[alen] = '\0';
            }
            return;
        }
    }
}

int
dtmail_imap_sync_folder(const DtMailAccount *acct,
                        const char *secret,
                        const char *folder,
                        const char *local_mbox_path,
                        int *out_added,
                        char *err_buf,
                        size_t err_len)
{
    if (out_added) *out_added = 0;
    if (err_buf && err_len > 0) err_buf[0] = '\0';
    if (!acct || !acct->imap_host[0] || !folder || !folder[0] || !local_mbox_path)
        return -1;

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char base_url[512], search_url[768];
    build_imap_base_url(acct, base_url, sizeof(base_url));
    char *esc_folder = curl_easy_escape(curl, folder, 0);
    snprintf(search_url, sizeof(search_url), "%s/%s", base_url, esc_folder ? esc_folder : folder);

    MemBuf mb = { NULL, 0, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, search_url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "UID SEARCH ALL");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    configure_curl_auth_and_tls(curl, acct, secret, acct->imap_tls);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        if (err_buf && err_len > 0)
            snprintf(err_buf, err_len, "IMAP UID SEARCH failed: %s", curl_easy_strerror(res));
        if (esc_folder) curl_free(esc_folder);
        curl_easy_cleanup(curl);
        free(mb.data);
        return -1;
    }

    /* Parse UIDs from "* SEARCH 1 2 3..." */
    unsigned long server_uids[256];
    int nserver = 0;
    if (mb.data) {
        const char *p = strstr(mb.data, "SEARCH");
        if (p) {
            p += 6;
            while (*p && nserver < 256) {
                while (*p && !isdigit((unsigned char)*p)) {
                    if (*p == '\n' || *p == '\r') break;
                    p++;
                }
                if (!isdigit((unsigned char)*p)) break;
                char *endp = NULL;
                unsigned long u = strtoul(p, &endp, 10);
                if (endp > p && u > 0) {
                    server_uids[nserver++] = u;
                    p = endp;
                } else {
                    break;
                }
            }
        }
    }
    free(mb.data);

    char uid_path[1280];
    snprintf(uid_path, sizeof(uid_path), "%s.uids", local_mbox_path);
    unsigned long *seen_uids = NULL;
    int seen_count = 0;
    load_uid_file(uid_path, &seen_uids, &seen_count);

    /* Cap initial sync to newest 50 messages */
    int start_idx = (seen_count == 0 && nserver > 50) ? (nserver - 50) : 0;
    int added = 0;

    for (int i = start_idx; i < nserver; i++) {
        unsigned long uid = server_uids[i];
        if (uid_list_contains(seen_uids, seen_count, uid))
            continue;

        char fetch_url[896];
        snprintf(fetch_url, sizeof(fetch_url), "%s/%s/;UID=%lu",
                 base_url, esc_folder ? esc_folder : folder, uid);

        MemBuf msg = { NULL, 0, 0 };
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL);
        curl_easy_setopt(curl, CURLOPT_URL, fetch_url);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &msg);
        CURLcode fres = curl_easy_perform(curl);
        if (fres == CURLE_OK && msg.data && msg.len > 0) {
            char from_addr[256];
            extract_from_header(msg.data, msg.len, from_addr, sizeof(from_addr));
            if (dtmail_mbox_append_rfc5322(local_mbox_path, from_addr, msg.data, msg.len) == 0) {
                unsigned long *ns = (unsigned long *)realloc(
                    seen_uids, (size_t)(seen_count + 1) * sizeof(unsigned long));
                if (ns) {
                    seen_uids = ns;
                    seen_uids[seen_count++] = uid;
                }
                added++;
            }
        }
        free(msg.data);
    }

    if (added > 0)
        save_uid_file(uid_path, seen_uids, seen_count);

    free(seen_uids);
    if (esc_folder) curl_free(esc_folder);
    curl_easy_cleanup(curl);

    if (out_added) *out_added = added;
    return 0;
}

int
dtmail_imap_set_flag(const DtMailAccount *acct,
                     const char *secret,
                     const char *folder,
                     unsigned long uid,
                     const char *flag,
                     int add_flag,
                     char *err_buf,
                     size_t err_len)
{
    if (err_buf && err_len > 0) err_buf[0] = '\0';
    if (!acct || !acct->imap_host[0] || !folder || !flag || uid == 0)
        return -1;

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char base_url[512], url[768], cmd[192];
    build_imap_base_url(acct, base_url, sizeof(base_url));
    char *esc_folder = curl_easy_escape(curl, folder, 0);
    snprintf(url, sizeof(url), "%s/%s", base_url, esc_folder ? esc_folder : folder);
    snprintf(cmd, sizeof(cmd), "UID STORE %lu %sFLAGS (%s)",
             uid, add_flag ? "+" : "-", flag);

    MemBuf mb = { NULL, 0, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cmd);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    configure_curl_auth_and_tls(curl, acct, secret, acct->imap_tls);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK && err_buf && err_len > 0)
        snprintf(err_buf, err_len, "IMAP flag sync failed: %s", curl_easy_strerror(res));

    if (esc_folder) curl_free(esc_folder);
    curl_easy_cleanup(curl);
    free(mb.data);
    return (res == CURLE_OK) ? 0 : -1;
}

typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
} UploadCtx;

static size_t
smtp_read_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    UploadCtx *u = (UploadCtx *)userdata;
    size_t max = size * nmemb;
    if (u->pos >= u->len || max == 0)
        return 0;
    size_t rem = u->len - u->pos;
    size_t chunk = rem < max ? rem : max;
    memcpy(ptr, u->data + u->pos, chunk);
    u->pos += chunk;
    return chunk;
}

int
dtmail_smtp_send(const DtMailAccount *acct,
                 const char *secret,
                 const char *from_addr,
                 const char *const *rcpts,
                 int rcpt_count,
                 const char *raw_rfc5322,
                 size_t raw_len,
                 char *err_buf,
                 size_t err_len)
{
    if (err_buf && err_len > 0) err_buf[0] = '\0';
    if (!acct || !acct->smtp_host[0] || !from_addr || !from_addr[0] ||
        !rcpts || rcpt_count <= 0 || !raw_rfc5322 || raw_len == 0) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Invalid SMTP send parameters");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    const char *scheme = (acct->smtp_tls == DTMAIL_TLS_IMPLICIT) ? "smtps" : "smtp";
    int port = acct->smtp_port > 0 ? acct->smtp_port
                                   : (acct->smtp_tls == DTMAIL_TLS_IMPLICIT ? 465 : 587);
    char url[512];
    snprintf(url, sizeof(url), "%s://%s:%d", scheme, acct->smtp_host, port);

    struct curl_slist *recipients = NULL;
    for (int i = 0; i < rcpt_count; i++) {
        if (rcpts[i] && rcpts[i][0])
            recipients = curl_slist_append(recipients, rcpts[i]);
    }
    if (!recipients) {
        curl_easy_cleanup(curl);
        return -1;
    }

    UploadCtx up = { raw_rfc5322, raw_len, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from_addr);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, smtp_read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA, &up);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    configure_curl_auth_and_tls(curl, acct, secret, acct->smtp_tls);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK && err_buf && err_len > 0)
        snprintf(err_buf, err_len, "SMTP send failed: %s", curl_easy_strerror(res));

    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : -1;
}

#else

int dtmail_imap_test_connection(const DtMailAccount *a, const char *s, char *e, size_t el)
{ (void)a; (void)s; if (e && el) snprintf(e, el, "libcurl not enabled"); return -1; }
int dtmail_imap_list_folders(const DtMailAccount *a, const char *s, char f[][128], int m, char *e, size_t el)
{ (void)a; (void)s; (void)f; (void)m; if (e && el) snprintf(e, el, "libcurl not enabled"); return -1; }
int dtmail_imap_sync_folder(const DtMailAccount *a, const char *s, const char *fo, const char *l, int *o, char *e, size_t el)
{ (void)a; (void)s; (void)fo; (void)l; (void)o; if (e && el) snprintf(e, el, "libcurl not enabled"); return -1; }
int dtmail_imap_set_flag(const DtMailAccount *a, const char *s, const char *fo, unsigned long u, const char *fl, int af, char *e, size_t el)
{ (void)a; (void)s; (void)fo; (void)u; (void)fl; (void)af; if (e && el) snprintf(e, el, "libcurl not enabled"); return -1; }
int dtmail_smtp_send(const DtMailAccount *a, const char *s, const char *fr, const char *const *r, int rc, const char *raw, size_t rl, char *e, size_t el)
{ (void)a; (void)s; (void)fr; (void)r; (void)rc; (void)raw; (void)rl; if (e && el) snprintf(e, el, "libcurl not enabled"); return -1; }

#endif
