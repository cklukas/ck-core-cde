/*
 * CDE - Common Desktop Environment
 *
 * RFC 7636 PKCE OAuth2 loopback sign-in and token refresh client for dtmail.
 */

#include "OAuth2Client.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

/* -------------------------------------------------------------------------
 * Self-contained SHA-256 (FIPS 180-4) and RFC 4648 base64url
 * ------------------------------------------------------------------------- */

static const uint32_t k_sha256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static inline uint32_t rotr32(uint32_t x, unsigned int n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k_sha256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_bytes(const uint8_t *data, size_t len, uint8_t digest[32])
{
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    uint8_t block[64];
    size_t pos = 0;
    while (len - pos >= 64) {
        sha256_transform(state, data + pos);
        pos += 64;
    }
    size_t rem = len - pos;
    memset(block, 0, sizeof(block));
    if (rem > 0) memcpy(block, data + pos, rem);
    block[rem] = 0x80;
    if (rem >= 56) {
        sha256_transform(state, block);
        memset(block, 0, sizeof(block));
    }
    uint64_t bitlen = (uint64_t)len * 8ULL;
    for (int i = 0; i < 8; i++) {
        block[56 + i] = (uint8_t)((bitlen >> (56 - i * 8)) & 0xffu);
    }
    sha256_transform(state, block);
    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)((state[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = (uint8_t)((state[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = (uint8_t)((state[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = (uint8_t)(state[i] & 0xffu);
    }
}

static int base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t oi = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = ((uint32_t)in[i]) << 16;
        if (i + 1 < in_len) v |= ((uint32_t)in[i + 1]) << 8;
        if (i + 2 < in_len) v |= ((uint32_t)in[i + 2]);
        if (oi + 4 >= out_len) return -1;
        out[oi++] = tbl[(v >> 18) & 0x3f];
        out[oi++] = tbl[(v >> 12) & 0x3f];
        if (i + 1 < in_len) out[oi++] = tbl[(v >> 6) & 0x3f];
        if (i + 2 < in_len) out[oi++] = tbl[v & 0x3f];
    }
    if (oi >= out_len) return -1;
    out[oi] = '\0';
    return 0;
}

static int read_random_bytes(uint8_t *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    return 0;
}

int dtmail_oauth2_compute_challenge(const char *verifier, char *out_challenge, size_t out_len)
{
    if (!verifier || !verifier[0] || !out_challenge || out_len < 44) return -1;
    uint8_t digest[32];
    sha256_bytes((const uint8_t *)verifier, strlen(verifier), digest);
    return base64url_encode(digest, sizeof(digest), out_challenge, out_len);
}

int dtmail_oauth2_generate_pkce(DtMailPkcePair *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    uint8_t vbuf[32], sbuf[16];
    if (read_random_bytes(vbuf, sizeof(vbuf)) != 0 ||
        read_random_bytes(sbuf, sizeof(sbuf)) != 0) {
        return -1;
    }
    if (base64url_encode(vbuf, sizeof(vbuf), out->code_verifier, sizeof(out->code_verifier)) != 0) {
        return -1;
    }
    if (base64url_encode(sbuf, sizeof(sbuf), out->state, sizeof(out->state)) != 0) {
        return -1;
    }
    return dtmail_oauth2_compute_challenge(out->code_verifier, out->code_challenge, sizeof(out->code_challenge));
}

static void url_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; p && *p && o + 4 < out_len; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[(*p >> 4) & 0xf];
            out[o++] = hex[*p & 0xf];
        }
    }
    if (out_len > 0) out[o] = '\0';
}

int dtmail_oauth2_build_auth_url(const DtMailAccount *acct,
                                 const DtMailPkcePair *pkce,
                                 int loopback_port,
                                 char *out_url, size_t out_len)
{
    if (!acct || !pkce || loopback_port <= 0 || !out_url || out_len < 64) return -1;
    const char *base = acct->oauth_auth_url;
    const char *scope = "https://mail.google.com/";
    if (!base[0]) {
        if (acct->oauth_provider == DTMAIL_OAUTH_M365) {
            base = "https://login.microsoftonline.com/common/oauth2/v2.0/authorize";
            scope = "offline_access https://outlook.office.com/IMAP.AccessAsUser.All https://outlook.office.com/SMTP.Send";
        } else {
            base = "https://accounts.google.com/o/oauth2/v2/auth";
        }
    } else if (acct->oauth_provider == DTMAIL_OAUTH_M365) {
        scope = "offline_access https://outlook.office.com/IMAP.AccessAsUser.All https://outlook.office.com/SMTP.Send";
    }

    char redir_raw[128], redir_enc[256], scope_enc[512], cid_enc[512];
    snprintf(redir_raw, sizeof(redir_raw), "http://127.0.0.1:%d/callback", loopback_port);
    url_encode(redir_raw, redir_enc, sizeof(redir_enc));
    url_encode(scope, scope_enc, sizeof(scope_enc));
    url_encode(acct->oauth_client_id, cid_enc, sizeof(cid_enc));

    int n = snprintf(out_url, out_len,
                     "%s?client_id=%s&redirect_uri=%s&response_type=code"
                     "&scope=%s&code_challenge=%s&code_challenge_method=S256"
                     "&state=%s&access_type=offline&prompt=consent",
                     base, cid_enc, redir_enc, scope_enc,
                     pkce->code_challenge, pkce->state);
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

int dtmail_oauth2_loopback_listen(int *out_port)
{
    if (!out_port) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }

    socklen_t alen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) {
        close(fd);
        return -1;
    }
    *out_port = (int)ntohs(addr.sin_port);
    return fd;
}

static int extract_query_param(const char *query, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            const char *end = strpbrk(val, "& \r\n");
            size_t vlen = end ? (size_t)(end - val) : strlen(val);
            if (vlen >= out_len) vlen = out_len - 1;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return 0;
        }
        const char *amp = strchr(p, '&');
        p = amp ? amp + 1 : NULL;
    }
    return -1;
}

int dtmail_oauth2_loopback_wait_code(int listen_fd,
                                     const char *expected_state,
                                     int timeout_sec,
                                     char *out_code, size_t out_len)
{
    if (listen_fd < 0 || !expected_state || !out_code || out_len < 16) return -1;
    struct pollfd pfd;
    pfd.fd = listen_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, timeout_sec * 1000);
    if (pr <= 0) return -1;

    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0) return -1;

    char req[4096];
    ssize_t n = read(cfd, req, sizeof(req) - 1);
    int rc = -1;
    if (n > 0) {
        req[n] = '\0';
        const char *q = strchr(req, '?');
        if (q) {
            char got_state[128] = "";
            char got_code[512] = "";
            if (extract_query_param(q + 1, "state", got_state, sizeof(got_state)) == 0 &&
                strcmp(got_state, expected_state) == 0 &&
                extract_query_param(q + 1, "code", got_code, sizeof(got_code)) == 0 &&
                got_code[0]) {
                snprintf(out_code, out_len, "%s", got_code);
                rc = 0;
            }
        }
    }

    const char *body = (rc == 0)
        ? "<html><body><h2>Signed in to CK Core Mail</h2><p>You may close this tab.</p></body></html>"
        : "<html><body><h2>Sign-in failed</h2><p>Invalid or missing OAuth2 response.</p></body></html>";
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
                        "HTTP/1.1 %s\r\nContent-Type: text/html; charset=UTF-8\r\n"
                        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                        (rc == 0) ? "200 OK" : "400 Bad Request",
                        strlen(body), body);
    if (rlen > 0) (void)write(cfd, resp, (size_t)rlen);
    close(cfd);
    return rc;
}

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} OAuthBuf;

#ifdef HAVE_LIBCURL
static size_t oauth_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    OAuthBuf *b = (OAuthBuf *)ud;
    size_t total = size * nmemb;
    if (b->len + total + 1 > b->cap) {
        size_t ncap = (b->cap ? b->cap * 2 : 1024);
        while (ncap < b->len + total + 1) ncap *= 2;
        if (ncap > 256 * 1024) return 0;
        char *nb = (char *)realloc(b->data, ncap);
        if (!nb) return 0;
        b->data = nb;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}
#endif

static int json_extract_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n')) p++;
    if (*p != '"') return -1;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_len) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return 0;
}

static long json_extract_long(const char *json, const char *key, long fallback)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return fallback;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
    if (!isdigit((unsigned char)*p)) return fallback;
    return strtol(p, NULL, 10);
}

static int oauth_post_token(const char *token_url, const char *post_fields,
                            DtMailOAuthTokens *out_tokens, char *errbuf, size_t errlen)
{
#ifndef HAVE_LIBCURL
    (void)token_url; (void)post_fields; (void)out_tokens;
    if (errbuf && errlen) snprintf(errbuf, errlen, "libcurl support not enabled");
    return -1;
#else
    if (!token_url || !token_url[0] || !post_fields || !out_tokens) return -1;
    memset(out_tokens, 0, sizeof(*out_tokens));
    CURL *c = curl_easy_init();
    if (!c) return -1;
    OAuthBuf buf = { NULL, 0, 0 };
    curl_easy_setopt(c, CURLOPT_URL, token_url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_fields);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, oauth_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    CURLcode res = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || !buf.data || http_code < 200 || http_code >= 300) {
        if (errbuf && errlen) {
            snprintf(errbuf, errlen, "OAuth2 token request failed (curl=%d http=%ld)",
                     (int)res, http_code);
        }
        free(buf.data);
        return -1;
    }

    int ok = json_extract_string(buf.data, "access_token",
                                 out_tokens->access_token, sizeof(out_tokens->access_token));
    (void)json_extract_string(buf.data, "refresh_token",
                              out_tokens->refresh_token, sizeof(out_tokens->refresh_token));
    out_tokens->expires_in = json_extract_long(buf.data, "expires_in", 3600);
    free(buf.data);
    if (ok != 0 || !out_tokens->access_token[0]) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "OAuth2 response missing access_token");
        return -1;
    }
    return 0;
#endif
}

int dtmail_oauth2_exchange_code(const DtMailAccount *acct,
                                const char *code,
                                const char *code_verifier,
                                const char *redirect_uri,
                                DtMailOAuthTokens *out_tokens,
                                char *errbuf, size_t errlen)
{
    if (!acct || !code || !code_verifier || !redirect_uri || !out_tokens) return -1;
    const char *turl = acct->oauth_token_url;
    if (!turl[0]) {
        turl = (acct->oauth_provider == DTMAIL_OAUTH_M365)
            ? "https://login.microsoftonline.com/common/oauth2/v2.0/token"
            : "https://oauth2.googleapis.com/token";
    }
    char cid_e[512], code_e[512], ver_e[256], redir_e[256];
    url_encode(acct->oauth_client_id, cid_e, sizeof(cid_e));
    url_encode(code, code_e, sizeof(code_e));
    url_encode(code_verifier, ver_e, sizeof(ver_e));
    url_encode(redirect_uri, redir_e, sizeof(redir_e));

    char body[2048];
    snprintf(body, sizeof(body),
             "grant_type=authorization_code&client_id=%s&code=%s"
             "&code_verifier=%s&redirect_uri=%s",
             cid_e, code_e, ver_e, redir_e);
    return oauth_post_token(turl, body, out_tokens, errbuf, errlen);
}

int dtmail_oauth2_refresh_token(const DtMailAccount *acct,
                                const char *refresh_token,
                                DtMailOAuthTokens *out_tokens,
                                char *errbuf, size_t errlen)
{
    if (!acct || !refresh_token || !refresh_token[0] || !out_tokens) return -1;
    const char *turl = acct->oauth_token_url;
    if (!turl[0]) {
        turl = (acct->oauth_provider == DTMAIL_OAUTH_M365)
            ? "https://login.microsoftonline.com/common/oauth2/v2.0/token"
            : "https://oauth2.googleapis.com/token";
    }
    char cid_e[512], ref_e[2048];
    url_encode(acct->oauth_client_id, cid_e, sizeof(cid_e));
    url_encode(refresh_token, ref_e, sizeof(ref_e));

    char body[3072];
    snprintf(body, sizeof(body),
             "grant_type=refresh_token&client_id=%s&refresh_token=%s",
             cid_e, ref_e);
    return oauth_post_token(turl, body, out_tokens, errbuf, errlen);
}

int dtmail_oauth2_open_browser(const char *url)
{
    if (!url || !url[0]) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        const char *env_b = getenv("BROWSER");
        if (env_b && env_b[0]) {
            execlp(env_b, env_b, url, (char *)NULL);
        }
        execlp("ck-browser", "ck-browser", url, (char *)NULL);
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
        _exit(127);
    }
    return 0;
}
