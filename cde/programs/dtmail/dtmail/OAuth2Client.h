#ifndef DTMAIL_OAUTH2_CLIENT_H
#define DTMAIL_OAUTH2_CLIENT_H

#include "MailAccountStore.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char code_verifier[128];
    char code_challenge[128];
    char state[64];
} DtMailPkcePair;

typedef struct {
    char access_token[2048];
    char refresh_token[2048];
    long expires_in;
} DtMailOAuthTokens;

/* RFC 7636 PKCE verifier/challenge generation (SHA-256 + base64url) */
int  dtmail_oauth2_compute_challenge(const char *verifier, char *out_challenge, size_t out_len);
int  dtmail_oauth2_generate_pkce(DtMailPkcePair *out);

/* Build provider authorization URL for loopback redirect */
int  dtmail_oauth2_build_auth_url(const DtMailAccount *acct,
                                  const DtMailPkcePair *pkce,
                                  int loopback_port,
                                  char *out_url, size_t out_len);

/* Start ephemeral 127.0.0.1:0 loopback listener; returns listening fd and port */
int  dtmail_oauth2_loopback_listen(int *out_port);

/* Accept one callback HTTP request on listen_fd (timeout_sec), validate state, extract code */
int  dtmail_oauth2_loopback_wait_code(int listen_fd,
                                      const char *expected_state,
                                      int timeout_sec,
                                      char *out_code, size_t out_len);

/* Exchange authorization code or refresh token via libcurl HTTPS POST */
int  dtmail_oauth2_exchange_code(const DtMailAccount *acct,
                                 const char *code,
                                 const char *code_verifier,
                                 const char *redirect_uri,
                                 DtMailOAuthTokens *out_tokens,
                                 char *errbuf, size_t errlen);

int  dtmail_oauth2_refresh_token(const DtMailAccount *acct,
                                 const char *refresh_token,
                                 DtMailOAuthTokens *out_tokens,
                                 char *errbuf, size_t errlen);

/* Launch ck-browser / $BROWSER / xdg-open for the authorization URL */
int  dtmail_oauth2_open_browser(const char *url);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_OAUTH2_CLIENT_H */
