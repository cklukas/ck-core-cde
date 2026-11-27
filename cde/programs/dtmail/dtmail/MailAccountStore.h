#ifndef DTMAIL_ACCOUNT_STORE_H
#define DTMAIL_ACCOUNT_STORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DTMAIL_MAX_ACCOUNTS 16

typedef enum {
    DTMAIL_AUTH_PASSWORD = 0,
    DTMAIL_AUTH_OAUTH2,
    DTMAIL_AUTH_SENDMAIL_LOCAL
} DtMailAuthMode;

typedef enum {
    DTMAIL_TLS_IMPLICIT = 0, /* IMAPS 993 / SMTPS 465 */
    DTMAIL_TLS_STARTTLS,     /* IMAP 143 / SMTP 587 */
    DTMAIL_TLS_NONE          /* Plaintext (local test only) */
} DtMailTlsMode;

typedef enum {
    DTMAIL_OAUTH_NONE = 0,
    DTMAIL_OAUTH_GMAIL,
    DTMAIL_OAUTH_M365,
    DTMAIL_OAUTH_CUSTOM,
    DTMAIL_OAUTH_FASTMAIL
} DtMailOAuthProvider;

typedef struct {
    char id[64];
    char name[128];
    char email[256];
    char username[256];
    char imap_host[256];
    int  imap_port;
    DtMailTlsMode imap_tls;
    char smtp_host[256];
    int  smtp_port;
    DtMailTlsMode smtp_tls;
    DtMailAuthMode auth_mode;
    DtMailOAuthProvider oauth_provider;
    char oauth_client_id[256];
    char oauth_auth_url[512];
    char oauth_token_url[512];
    int  enabled;
    int  sync_interval_sec;
    char signature[256];
} DtMailAccount;

/* Provider presets: "gmail", "icloud", "fastmail", "fastmail_jmap", "jmap", "m365", "custom", "local" */
void dtmail_account_apply_preset(DtMailAccount *acct, const char *preset);

/* Sanitize identifier/folder for safe filesystem path under ~/.dt/mail */
void dtmail_sanitize_path_component(const char *raw, char *out, size_t out_len);

/* Persistence (~/.dt/dtmail_accounts, mode 0600, no secrets on disk) */
int  dtmail_accounts_default_path(char *out, size_t out_len);
void dtmail_accounts_init(void);
void dtmail_accounts_shutdown(void);
int  dtmail_accounts_load_file(const char *path);
int  dtmail_accounts_save_file(const char *path);

int  dtmail_accounts_count(void);
const DtMailAccount *dtmail_accounts_get(int idx);
int  dtmail_accounts_find_by_id(const char *id);
int  dtmail_accounts_upsert(const DtMailAccount *acct);
int  dtmail_accounts_remove(int idx);

/* Local mailbox paths & graceful fallback when /var/mail/$USER is unavailable */
int  dtmail_ensure_local_fallback_inbox(char *out, size_t out_len);
int  dtmail_account_folder_path(const char *acct_id, const char *folder,
                                char *out, size_t out_len);
int  dtmail_resolve_startup_inbox(const char *spool_path,
                                  char *out, size_t out_len,
                                  int *out_used_fallback);

/* Keyring integration via secret-tool (fork/exec, no shell interpolation) */
int  dtmail_keyring_store(const char *acct_id, const char *kind, const char *secret);
int  dtmail_keyring_lookup(const char *acct_id, const char *kind, char *out, size_t out_len);
int  dtmail_keyring_clear(const char *acct_id, const char *kind);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_ACCOUNT_STORE_H */
