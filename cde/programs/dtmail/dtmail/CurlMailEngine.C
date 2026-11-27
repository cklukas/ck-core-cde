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
#include <stdint.h>
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
#include "MailThreadRules.h"

int
dtmail_account_is_jmap(const DtMailAccount *acct)
{
    if (!acct || !acct->imap_host[0])
        return 0;
    if (strncasecmp(acct->imap_host, "https://", 8) == 0 ||
        strncasecmp(acct->imap_host, "http://", 7) == 0 ||
        strncasecmp(acct->imap_host, "jmap://", 7) == 0 ||
        strncasecmp(acct->imap_host, "jmaps://", 8) == 0) {
        return 1;
    }
    return 0;
}

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

static int jmap_test_connection(const DtMailAccount *acct, const char *secret,
                                char *err_buf, size_t err_len);
static int jmap_list_folders(const DtMailAccount *acct, const char *secret,
                             char folders[][128], int max_folders,
                             char *err_buf, size_t err_len);
static int jmap_sync_folder(const DtMailAccount *acct, const char *secret,
                            const char *folder, const char *local_mbox_path,
                            int *out_added, char *err_buf, size_t err_len);
static int jmap_set_flag(const DtMailAccount *acct, const char *secret,
                         const char *folder, unsigned long uid,
                         const char *flag, int add_flag,
                         char *err_buf, size_t err_len);
static int jmap_send_mail(const DtMailAccount *acct, const char *secret,
                          const char *from_addr, const char *const *rcpts,
                          int rcpt_count, const char *raw_rfc5322,
                          size_t raw_len, char *err_buf, size_t err_len);

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
    if (dtmail_account_is_jmap(acct))
        return jmap_test_connection(acct, secret, err_buf, err_len);

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
    if (dtmail_account_is_jmap(acct))
        return jmap_list_folders(acct, secret, folders, max_folders, err_buf, err_len);

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

static void
extract_header_value(const char *msg, size_t len, const char *hdr_name, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!msg || !hdr_name) return;
    size_t hlen = strlen(hdr_name);
    const char *p = msg;
    const char *end = msg + len;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *lend = nl ? nl : end;
        if (lend == p || (lend == p + 1 && *p == '\r')) break;
        if ((size_t)(lend - p) > hlen + 1 &&
            strncasecmp(p, hdr_name, hlen) == 0 && p[hlen] == ':') {
            const char *v = p + hlen + 1;
            while (v < lend && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = (size_t)(lend - v);
            while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' || v[vlen - 1] == ' '))
                vlen--;
            if (vlen >= out_len) vlen = out_len - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return;
        }
        p = nl ? nl + 1 : end;
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
    if (dtmail_account_is_jmap(acct))
        return jmap_sync_folder(acct, secret, folder, local_mbox_path, out_added, err_buf, err_len);

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
            char from_addr[256], to_hdr[256], subj_hdr[256];
            extract_from_header(msg.data, msg.len, from_addr, sizeof(from_addr));
            extract_header_value(msg.data, msg.len, "To", to_hdr, sizeof(to_hdr));
            extract_header_value(msg.data, msg.len, "Subject", subj_hdr, sizeof(subj_hdr));

            /* Evaluate local mail rules (~/.dt/dtmail_rules) on incoming messages */
            const char *dest_path = local_mbox_path;
            char rule_dest[1024] = "";
            const char *prefix_hdr = NULL;
            dtmail_rules_init();
            int ridx = dtmail_rules_evaluate(from_addr, to_hdr, subj_hdr);
            if (ridx >= 0) {
                const DtMailRule *rule = dtmail_rules_get(ridx);
                if (rule) {
                    if (rule->action == DTMAIL_RULE_ACT_MOVE && rule->target_folder[0] &&
                        dtmail_account_folder_path(acct->id, rule->target_folder, rule_dest, sizeof(rule_dest)) == 0) {
                        dest_path = rule_dest;
                    } else if (rule->action == DTMAIL_RULE_ACT_JUNK &&
                               dtmail_account_folder_path(acct->id, "Junk", rule_dest, sizeof(rule_dest)) == 0) {
                        dest_path = rule_dest;
                    } else if (rule->action == DTMAIL_RULE_ACT_MARK_READ) {
                        prefix_hdr = "Status: RO\r\n";
                    } else if (rule->action == DTMAIL_RULE_ACT_STAR) {
                        prefix_hdr = "X-DtMail-Starred: 1\r\n";
                    }
                }
            }

            int app_rc = -1;
            if (prefix_hdr) {
                size_t plen = strlen(prefix_hdr);
                char *aug = (char *)malloc(plen + msg.len);
                if (aug) {
                    memcpy(aug, prefix_hdr, plen);
                    memcpy(aug + plen, msg.data, msg.len);
                    app_rc = dtmail_mbox_append_rfc5322(dest_path, from_addr, aug, plen + msg.len);
                    free(aug);
                }
            } else {
                app_rc = dtmail_mbox_append_rfc5322(dest_path, from_addr, msg.data, msg.len);
            }

            if (app_rc == 0) {
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
    if (dtmail_account_is_jmap(acct))
        return jmap_set_flag(acct, secret, folder, uid, flag, add_flag, err_buf, err_len);

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
    if (dtmail_account_is_jmap(acct) ||
        strncasecmp(acct->smtp_host, "https://", 8) == 0 ||
        strncasecmp(acct->smtp_host, "http://", 7) == 0) {
        return jmap_send_mail(acct, secret, from_addr, rcpts, rcpt_count,
                              raw_rfc5322, raw_len, err_buf, err_len);
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

/* -------------------------------------------------------------------------
 * RFC 8620 / RFC 8621 JMAP over HTTP(S) transport (OAuth2 Bearer / API token)
 * ------------------------------------------------------------------------- */

typedef struct {
    char api_url[512];
    char download_url[512];
    char upload_url[512];
    char account_id[128];
} JmapSession;

static void
jmap_normalize_url(const char *raw, char *out, size_t out_len)
{
    if (!raw || !out || out_len == 0) return;
    if (strncasecmp(raw, "jmaps://", 8) == 0) {
        snprintf(out, out_len, "https://%s", raw + 8);
    } else if (strncasecmp(raw, "jmap://", 7) == 0) {
        snprintf(out, out_len, "http://%s", raw + 7);
    } else {
        snprintf(out, out_len, "%s", raw);
    }
}

static int
jmap_json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) return -1;
    out[0] = '\0';
    char pat[160];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':'))
        p++;
    if (*p != '"') return -1;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') out[o++] = '\n';
            else if (*p == 'r') out[o++] = '\r';
            else if (*p == 't') out[o++] = '\t';
            else out[o++] = *p;
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return 0;
}

static void
jmap_json_escape(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 3 < out_len; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (*p == '\r') {
            out[o++] = '\\';
            out[o++] = 'r';
        } else if (*p >= 0x20) {
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
}

static void
jmap_expand_template(const char *tmpl, const char *acct_id, const char *blob_id,
                     const char *name, const char *mime_type,
                     char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    size_t o = 0;
    const char *p = tmpl ? tmpl : "";
    while (*p && o + 1 < out_len) {
        if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (end) {
                size_t klen = (size_t)(end - (p + 1));
                const char *val = "";
                if (klen == 9 && strncmp(p + 1, "accountId", 9) == 0)
                    val = acct_id ? acct_id : "";
                else if (klen == 6 && strncmp(p + 1, "blobId", 6) == 0)
                    val = blob_id ? blob_id : "";
                else if (klen == 4 && strncmp(p + 1, "name", 4) == 0)
                    val = name ? name : "message.eml";
                else if (klen == 4 && strncmp(p + 1, "type", 4) == 0)
                    val = mime_type ? mime_type : "application/octet-stream";
                while (*val && o + 1 < out_len)
                    out[o++] = *val++;
                p = end + 1;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

static unsigned long
jmap_hash_email_id(const char *id)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)(id ? id : ""); *p; p++) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    unsigned long u = (unsigned long)(h & 0x7fffffffUL);
    return u == 0 ? 1UL : u;
}

static int
jmap_http_call(const DtMailAccount *acct, const char *secret,
               const char *url, const char *method,
               const char *content_type,
               const char *body, size_t body_len,
               MemBuf *out_buf, char *err_buf, size_t err_len)
{
    memset(out_buf, 0, sizeof(*out_buf));
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *hdrs = NULL;
    char ct_hdr[128];
    if (content_type && content_type[0]) {
        snprintf(ct_hdr, sizeof(ct_hdr), "Content-Type: %s", content_type);
        hdrs = curl_slist_append(hdrs, ct_hdr);
    }
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    if (secret && secret[0]) {
        if (acct->auth_mode == DTMAIL_AUTH_OAUTH2 ||
            strncmp(secret, "fmu1-", 5) == 0 ||
            !acct->username[0]) {
            char auth_hdr[2300];
            snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", secret);
            hdrs = curl_slist_append(hdrs, auth_hdr);
        } else {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERNAME, acct->username);
            curl_easy_setopt(curl, CURLOPT_PASSWORD, secret);
        }
    }

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_buf);

    if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "JMAP HTTP %ld (%s)",
                     http_code, curl_easy_strerror(res));
        }
        free(out_buf->data);
        out_buf->data = NULL;
        out_buf->len = 0;
        return -1;
    }
    return 0;
}

static int
jmap_discover_session(const DtMailAccount *acct, const char *secret,
                      JmapSession *out_sess, char *err_buf, size_t err_len)
{
    memset(out_sess, 0, sizeof(*out_sess));
    char sess_url[512];
    jmap_normalize_url(acct->imap_host[0] ? acct->imap_host : acct->smtp_host,
                       sess_url, sizeof(sess_url));

    MemBuf mb;
    if (jmap_http_call(acct, secret, sess_url, "GET", NULL, NULL, 0, &mb, err_buf, err_len) != 0)
        return -1;

    (void)jmap_json_get_str(mb.data, "apiUrl", out_sess->api_url, sizeof(out_sess->api_url));
    (void)jmap_json_get_str(mb.data, "downloadUrl", out_sess->download_url, sizeof(out_sess->download_url));
    (void)jmap_json_get_str(mb.data, "uploadUrl", out_sess->upload_url, sizeof(out_sess->upload_url));

    const char *prim = strstr(mb.data ? mb.data : "", "\"primaryAccounts\"");
    if (prim) {
        (void)jmap_json_get_str(prim, "urn:ietf:params:jmap:mail",
                                out_sess->account_id, sizeof(out_sess->account_id));
    }
    if (!out_sess->account_id[0]) {
        const char *accts = strstr(mb.data ? mb.data : "", "\"accounts\"");
        if (accts) {
            const char *brace = strchr(accts, '{');
            const char *q1 = brace ? strchr(brace + 1, '"') : NULL;
            const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2 && q2 > q1 + 1) {
                size_t al = (size_t)(q2 - q1 - 1);
                if (al >= sizeof(out_sess->account_id)) al = sizeof(out_sess->account_id) - 1;
                memcpy(out_sess->account_id, q1 + 1, al);
                out_sess->account_id[al] = '\0';
            }
        }
    }
    free(mb.data);

    if (!out_sess->api_url[0] || !out_sess->account_id[0]) {
        if (err_buf && err_len > 0)
            snprintf(err_buf, err_len, "JMAP session missing apiUrl or mail accountId");
        return -1;
    }
    return 0;
}

static int
jmap_test_connection(const DtMailAccount *acct, const char *secret,
                     char *err_buf, size_t err_len)
{
    JmapSession sess;
    return jmap_discover_session(acct, secret, &sess, err_buf, err_len);
}

typedef struct {
    char id[128];
    char name[128];
    char role[64];
} JmapMailbox;

static int
jmap_fetch_mailboxes(const DtMailAccount *acct, const char *secret,
                     const JmapSession *sess,
                     JmapMailbox *out_mbs, int max_mbs,
                     char *err_buf, size_t err_len)
{
    char acct_esc[256];
    jmap_json_escape(sess->account_id, acct_esc, sizeof(acct_esc));
    char req[1024];
    snprintf(req, sizeof(req),
             "{\"using\":[\"urn:ietf:params:jmap:core\",\"urn:ietf:params:jmap:mail\"],"
             "\"methodCalls\":[[\"Mailbox/get\",{\"accountId\":\"%s\"},\"c0\"]]}",
             acct_esc);

    MemBuf mb;
    if (jmap_http_call(acct, secret, sess->api_url, "POST", "application/json",
                       req, strlen(req), &mb, err_buf, err_len) != 0) {
        return -1;
    }

    int count = 0;
    const char *list_p = mb.data ? strstr(mb.data, "\"list\"") : NULL;
    const char *p = list_p ? strchr(list_p, '[') : NULL;
    while (p && *p && count < max_mbs) {
        const char *obj = strchr(p, '{');
        if (!obj) break;
        /* Find matching '}' of this shallow mailbox object */
        int depth = 0;
        int in_str = 0;
        const char *end = obj;
        for (; *end; end++) {
            if (*end == '"' && (end == obj || end[-1] != '\\'))
                in_str = !in_str;
            else if (!in_str) {
                if (*end == '{') depth++;
                else if (*end == '}') {
                    depth--;
                    if (depth == 0) break;
                }
            }
        }
        if (*end != '}') break;
        size_t olen = (size_t)(end - obj + 1);
        char *slice = (char *)malloc(olen + 1);
        if (slice) {
            memcpy(slice, obj, olen);
            slice[olen] = '\0';
            JmapMailbox m;
            memset(&m, 0, sizeof(m));
            (void)jmap_json_get_str(slice, "id", m.id, sizeof(m.id));
            (void)jmap_json_get_str(slice, "name", m.name, sizeof(m.name));
            (void)jmap_json_get_str(slice, "role", m.role, sizeof(m.role));
            if (m.id[0] && m.name[0]) {
                out_mbs[count++] = m;
            }
            free(slice);
        }
        p = end + 1;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == ']') break;
    }
    free(mb.data);
    return count;
}

static int
jmap_list_folders(const DtMailAccount *acct, const char *secret,
                  char folders[][128], int max_folders,
                  char *err_buf, size_t err_len)
{
    JmapSession sess;
    if (jmap_discover_session(acct, secret, &sess, err_buf, err_len) != 0)
        return -1;
    JmapMailbox mbs[64];
    int cap = max_folders < 64 ? max_folders : 64;
    int n = jmap_fetch_mailboxes(acct, secret, &sess, mbs, cap, err_buf, err_len);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        if (strcasecmp(mbs[i].role, "inbox") == 0)
            snprintf(folders[i], 128, "INBOX");
        else
            snprintf(folders[i], 128, "%s", mbs[i].name);
    }
    return n;
}

static int
jmap_resolve_mailbox_id(const JmapMailbox *mbs, int n, const char *folder,
                        char *out_id, size_t out_len)
{
    for (int i = 0; i < n; i++) {
        if (strcasecmp(folder, "INBOX") == 0 && strcasecmp(mbs[i].role, "inbox") == 0) {
            snprintf(out_id, out_len, "%s", mbs[i].id);
            return 0;
        }
        if (strcasecmp(mbs[i].name, folder) == 0 ||
            (mbs[i].role[0] && strcasecmp(mbs[i].role, folder) == 0)) {
            snprintf(out_id, out_len, "%s", mbs[i].id);
            return 0;
        }
    }
    return -1;
}

static void
jmap_record_id_map(const char *mbox_path, unsigned long uid, const char *email_id)
{
    if (!mbox_path || !email_id || !email_id[0]) return;
    char map_path[1280];
    snprintf(map_path, sizeof(map_path), "%s.jmap_ids", mbox_path);
    int fd = open(map_path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    char line[256];
    int n = snprintf(line, sizeof(line), "%lu\t%s\n", uid, email_id);
    if (n > 0) (void)write(fd, line, (size_t)n);
    close(fd);
}

static int
jmap_lookup_id_map(const char *acct_id, const char *folder, unsigned long uid,
                   char *out_email_id, size_t out_len)
{
    char mbox_path[1024], map_path[1280];
    if (dtmail_account_folder_path(acct_id, folder, mbox_path, sizeof(mbox_path)) != 0)
        return -1;
    snprintf(map_path, sizeof(map_path), "%s.jmap_ids", mbox_path);
    FILE *fp = fopen(map_path, "r");
    if (!fp) return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long u = 0;
        char eid[128] = "";
        if (sscanf(line, "%lu\t%127s", &u, eid) == 2 && u == uid) {
            snprintf(out_email_id, out_len, "%s", eid);
            found = 0;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int
jmap_sync_folder(const DtMailAccount *acct, const char *secret,
                 const char *folder, const char *local_mbox_path,
                 int *out_added, char *err_buf, size_t err_len)
{
    JmapSession sess;
    if (jmap_discover_session(acct, secret, &sess, err_buf, err_len) != 0)
        return -1;

    JmapMailbox mbs[64];
    int nmbs = jmap_fetch_mailboxes(acct, secret, &sess, mbs, 64, err_buf, err_len);
    if (nmbs <= 0) return (nmbs == 0) ? 0 : -1;

    char mb_id[128] = "";
    if (jmap_resolve_mailbox_id(mbs, nmbs, folder, mb_id, sizeof(mb_id)) != 0) {
        if (err_buf && err_len > 0)
            snprintf(err_buf, err_len, "JMAP mailbox not found: %s", folder);
        return -1;
    }

    char acct_esc[256], mb_esc[256];
    jmap_json_escape(sess.account_id, acct_esc, sizeof(acct_esc));
    jmap_json_escape(mb_id, mb_esc, sizeof(mb_esc));

    char qreq[1280];
    snprintf(qreq, sizeof(qreq),
             "{\"using\":[\"urn:ietf:params:jmap:core\",\"urn:ietf:params:jmap:mail\"],"
             "\"methodCalls\":["
             "[\"Email/query\",{\"accountId\":\"%s\",\"filter\":{\"inMailbox\":\"%s\"},\"limit\":50},\"q0\"],"
             "[\"Email/get\",{\"accountId\":\"%s\",\"#ids\":{\"resultOf\":\"q0\",\"name\":\"Email/query\",\"path\":\"/ids\"},\"properties\":[\"id\",\"blobId\"]},\"g0\"]"
             "]}",
             acct_esc, mb_esc, acct_esc);

    MemBuf qmb;
    if (jmap_http_call(acct, secret, sess.api_url, "POST", "application/json",
                       qreq, strlen(qreq), &qmb, err_buf, err_len) != 0) {
        return -1;
    }

    /* Extract (id, blobId) pairs from Email/get "list":[ ... ] */
    struct { char id[128]; char blob_id[128]; } items[64];
    int nitems = 0;
    const char *eg = qmb.data ? strstr(qmb.data, "\"Email/get\"") : NULL;
    const char *lp = eg ? strstr(eg, "\"list\"") : NULL;
    const char *p = lp ? strchr(lp, '[') : NULL;
    while (p && *p && nitems < 64) {
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *end = strchr(obj, '}');
        if (!end) break;
        size_t olen = (size_t)(end - obj + 1);
        char slice[512];
        if (olen < sizeof(slice)) {
            memcpy(slice, obj, olen);
            slice[olen] = '\0';
            items[nitems].id[0] = '\0';
            items[nitems].blob_id[0] = '\0';
            (void)jmap_json_get_str(slice, "id", items[nitems].id, sizeof(items[nitems].id));
            (void)jmap_json_get_str(slice, "blobId", items[nitems].blob_id, sizeof(items[nitems].blob_id));
            if (items[nitems].id[0] && items[nitems].blob_id[0])
                nitems++;
        }
        p = end + 1;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ']') break;
    }
    free(qmb.data);

    char uid_path[1280];
    snprintf(uid_path, sizeof(uid_path), "%s.uids", local_mbox_path);
    unsigned long *seen_uids = NULL;
    int seen_count = 0;
    load_uid_file(uid_path, &seen_uids, &seen_count);

    int added = 0;
    for (int i = 0; i < nitems; i++) {
        unsigned long uid = jmap_hash_email_id(items[i].id);
        if (uid_list_contains(seen_uids, seen_count, uid))
            continue;

        char dl_url[1024];
        jmap_expand_template(sess.download_url, sess.account_id, items[i].blob_id,
                             "message.eml", "message/rfc822", dl_url, sizeof(dl_url));

        MemBuf raw;
        if (jmap_http_call(acct, secret, dl_url, "GET", NULL, NULL, 0, &raw, err_buf, err_len) == 0 &&
            raw.data && raw.len > 0) {
            char from_addr[256], to_hdr[256], subj_hdr[256];
            extract_from_header(raw.data, raw.len, from_addr, sizeof(from_addr));
            extract_header_value(raw.data, raw.len, "To", to_hdr, sizeof(to_hdr));
            extract_header_value(raw.data, raw.len, "Subject", subj_hdr, sizeof(subj_hdr));

            const char *dest_path = local_mbox_path;
            char rule_dest[1024] = "";
            const char *prefix_hdr = NULL;
            dtmail_rules_init();
            int ridx = dtmail_rules_evaluate(from_addr, to_hdr, subj_hdr);
            if (ridx >= 0) {
                const DtMailRule *rule = dtmail_rules_get(ridx);
                if (rule) {
                    if (rule->action == DTMAIL_RULE_ACT_MOVE && rule->target_folder[0] &&
                        dtmail_account_folder_path(acct->id, rule->target_folder, rule_dest, sizeof(rule_dest)) == 0) {
                        dest_path = rule_dest;
                    } else if (rule->action == DTMAIL_RULE_ACT_JUNK &&
                               dtmail_account_folder_path(acct->id, "Junk", rule_dest, sizeof(rule_dest)) == 0) {
                        dest_path = rule_dest;
                    } else if (rule->action == DTMAIL_RULE_ACT_MARK_READ) {
                        prefix_hdr = "Status: RO\r\n";
                    } else if (rule->action == DTMAIL_RULE_ACT_STAR) {
                        prefix_hdr = "X-DtMail-Starred: 1\r\n";
                    }
                }
            }

            int app_rc = -1;
            if (prefix_hdr) {
                size_t plen = strlen(prefix_hdr);
                char *aug = (char *)malloc(plen + raw.len);
                if (aug) {
                    memcpy(aug, prefix_hdr, plen);
                    memcpy(aug + plen, raw.data, raw.len);
                    app_rc = dtmail_mbox_append_rfc5322(dest_path, from_addr, aug, plen + raw.len);
                    free(aug);
                }
            } else {
                app_rc = dtmail_mbox_append_rfc5322(dest_path, from_addr, raw.data, raw.len);
            }

            if (app_rc == 0) {
                unsigned long *ns = (unsigned long *)realloc(
                    seen_uids, (size_t)(seen_count + 1) * sizeof(unsigned long));
                if (ns) {
                    seen_uids = ns;
                    seen_uids[seen_count++] = uid;
                }
                jmap_record_id_map(local_mbox_path, uid, items[i].id);
                added++;
            }
        }
        free(raw.data);
    }

    if (added > 0)
        save_uid_file(uid_path, seen_uids, seen_count);
    free(seen_uids);
    if (out_added) *out_added = added;
    return 0;
}

static int
jmap_set_flag(const DtMailAccount *acct, const char *secret,
              const char *folder, unsigned long uid,
              const char *flag, int add_flag,
              char *err_buf, size_t err_len)
{
    JmapSession sess;
    if (jmap_discover_session(acct, secret, &sess, err_buf, err_len) != 0)
        return -1;

    char email_id[128] = "";
    if (jmap_lookup_id_map(acct->id, folder, uid, email_id, sizeof(email_id)) != 0) {
        snprintf(email_id, sizeof(email_id), "%lu", uid);
    }

    const char *jkw = "$seen";
    if (strcasestr(flag, "Flagged") != NULL || strcasestr(flag, "flagged") != NULL)
        jkw = "$flagged";
    else if (strcasestr(flag, "Answered") != NULL)
        jkw = "$answered";
    else if (strcasestr(flag, "Draft") != NULL)
        jkw = "$draft";

    char acct_esc[256], id_esc[256];
    jmap_json_escape(sess.account_id, acct_esc, sizeof(acct_esc));
    jmap_json_escape(email_id, id_esc, sizeof(id_esc));

    char req[1024];
    snprintf(req, sizeof(req),
             "{\"using\":[\"urn:ietf:params:jmap:core\",\"urn:ietf:params:jmap:mail\"],"
             "\"methodCalls\":[[\"Email/set\",{\"accountId\":\"%s\",\"update\":{\"%s\":{\"keywords/%s\":%s}}},\"s0\"]]}",
             acct_esc, id_esc, jkw, add_flag ? "true" : "null");

    MemBuf mb;
    int rc = jmap_http_call(acct, secret, sess.api_url, "POST", "application/json",
                            req, strlen(req), &mb, err_buf, err_len);
    free(mb.data);
    return rc;
}

static int
jmap_send_mail(const DtMailAccount *acct, const char *secret,
               const char *from_addr, const char *const *rcpts,
               int rcpt_count, const char *raw_rfc5322,
               size_t raw_len, char *err_buf, size_t err_len)
{
    JmapSession sess;
    if (jmap_discover_session(acct, secret, &sess, err_buf, err_len) != 0)
        return -1;
    if (!sess.upload_url[0]) {
        if (err_buf && err_len > 0)
            snprintf(err_buf, err_len, "JMAP session missing uploadUrl");
        return -1;
    }

    /* 1. Upload RFC 5322 blob */
    char up_url[1024];
    jmap_expand_template(sess.upload_url, sess.account_id, "", "message.eml",
                         "message/rfc822", up_url, sizeof(up_url));
    MemBuf up_mb;
    if (jmap_http_call(acct, secret, up_url, "POST", "message/rfc822",
                       raw_rfc5322, raw_len, &up_mb, err_buf, err_len) != 0) {
        return -1;
    }
    char blob_id[128] = "";
    (void)jmap_json_get_str(up_mb.data ? up_mb.data : "", "blobId", blob_id, sizeof(blob_id));
    free(up_mb.data);
    if (!blob_id[0]) {
        if (err_buf && err_len > 0)
            snprintf(err_buf, err_len, "JMAP upload response missing blobId");
        return -1;
    }

    /* 2. Resolve Sent/Drafts mailbox and sender Identity */
    JmapMailbox mbs[64];
    int nmbs = jmap_fetch_mailboxes(acct, secret, &sess, mbs, 64, err_buf, err_len);
    char target_mb[128] = "";
    if (nmbs > 0) {
        if (jmap_resolve_mailbox_id(mbs, nmbs, "Sent", target_mb, sizeof(target_mb)) != 0 &&
            jmap_resolve_mailbox_id(mbs, nmbs, "Drafts", target_mb, sizeof(target_mb)) != 0) {
            snprintf(target_mb, sizeof(target_mb), "%s", mbs[0].id);
        }
    }

    char acct_esc[256], blob_esc[256], mb_esc[256], from_esc[320];
    jmap_json_escape(sess.account_id, acct_esc, sizeof(acct_esc));
    jmap_json_escape(blob_id, blob_esc, sizeof(blob_esc));
    jmap_json_escape(target_mb[0] ? target_mb : "inbox", mb_esc, sizeof(mb_esc));
    jmap_json_escape(from_addr, from_esc, sizeof(from_esc));

    /* Fetch Identity/get (fallback identityId = "default") */
    char id_req[512];
    snprintf(id_req, sizeof(id_req),
             "{\"using\":[\"urn:ietf:params:jmap:core\",\"urn:ietf:params:jmap:submission\"],"
             "\"methodCalls\":[[\"Identity/get\",{\"accountId\":\"%s\"},\"i0\"]]}",
             acct_esc);
    char identity_id[128] = "default";
    MemBuf id_mb;
    if (jmap_http_call(acct, secret, sess.api_url, "POST", "application/json",
                       id_req, strlen(id_req), &id_mb, NULL, 0) == 0 && id_mb.data) {
        const char *lp = strstr(id_mb.data, "\"list\"");
        const char *obj = lp ? strchr(lp, '{') : NULL;
        if (obj) (void)jmap_json_get_str(obj, "id", identity_id, sizeof(identity_id));
        free(id_mb.data);
    }
    char ident_esc[256];
    jmap_json_escape(identity_id, ident_esc, sizeof(ident_esc));

    /* 3. Email/import -> extract imported email id -> EmailSubmission/set */
    char imp_req[1024];
    snprintf(imp_req, sizeof(imp_req),
             "{\"using\":[\"urn:ietf:params:jmap:core\",\"urn:ietf:params:jmap:mail\"],"
             "\"methodCalls\":[[\"Email/import\",{\"accountId\":\"%s\",\"emails\":{"
             "\"m0\":{\"blobId\":\"%s\",\"mailboxIds\":{\"%s\":true},\"keywords\":{\"$seen\":true}}"
             "}},\"imp0\"]]}",
             acct_esc, blob_esc, mb_esc);
    MemBuf imp_mb;
    if (jmap_http_call(acct, secret, sess.api_url, "POST", "application/json",
                       imp_req, strlen(imp_req), &imp_mb, err_buf, err_len) != 0) {
        return -1;
    }
    char imported_id[128] = "";
    const char *cr = imp_mb.data ? strstr(imp_mb.data, "\"created\"") : NULL;
    const char *m0 = cr ? strstr(cr, "\"m0\"") : NULL;
    if (m0) (void)jmap_json_get_str(m0, "id", imported_id, sizeof(imported_id));
    free(imp_mb.data);
    if (!imported_id[0]) {
        if (err_buf && err_len > 0)
            snprintf(err_buf, err_len, "JMAP Email/import did not return created email id");
        return -1;
    }

    char rcpt_json[2048];
    size_t rpos = 0;
    rcpt_json[0] = '\0';
    for (int i = 0; i < rcpt_count; i++) {
        if (!rcpts[i] || !rcpts[i][0]) continue;
        char resc[320];
        jmap_json_escape(rcpts[i], resc, sizeof(resc));
        int nw = snprintf(rcpt_json + rpos, sizeof(rcpt_json) - rpos,
                          "%s{\"email\":\"%s\"}", (rpos > 0) ? "," : "", resc);
        if (nw < 0 || (size_t)nw >= sizeof(rcpt_json) - rpos) break;
        rpos += (size_t)nw;
    }

    char imp_id_esc[256];
    jmap_json_escape(imported_id, imp_id_esc, sizeof(imp_id_esc));
    char sub_req[3584];
    snprintf(sub_req, sizeof(sub_req),
             "{\"using\":[\"urn:ietf:params:jmap:core\",\"urn:ietf:params:jmap:mail\",\"urn:ietf:params:jmap:submission\"],"
             "\"methodCalls\":[[\"EmailSubmission/set\",{\"accountId\":\"%s\",\"create\":{"
             "\"sub0\":{\"identityId\":\"%s\",\"emailId\":\"%s\","
             "\"envelope\":{\"mailFrom\":{\"email\":\"%s\"},\"rcptTo\":[%s]}}"
             "}},\"sub0\"]]}",
             acct_esc, ident_esc, imp_id_esc, from_esc, rcpt_json);

    MemBuf sub_mb;
    int src = jmap_http_call(acct, secret, sess.api_url, "POST", "application/json",
                             sub_req, strlen(sub_req), &sub_mb, err_buf, err_len);
    free(sub_mb.data);
    return src;
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
