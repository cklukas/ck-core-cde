/*
 * CDE - Common Desktop Environment
 *
 * Multi-account configuration store (~/.dt/dtmail_accounts, mode 0600),
 * provider presets, local mbox fallback inbox (~/.dt/mail/Inbox), and
 * desktop keyring (secret-tool) secret storage for dtmail.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "MailAccountStore.h"

static DtMailAccount g_accounts[DTMAIL_MAX_ACCOUNTS];
static int g_account_count = 0;

static void
strip_tsv_chars(char *s)
{
    if (!s) return;
    for (; *s; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
    }
}

void
dtmail_sanitize_path_component(const char *raw, char *out, size_t out_len)
{
    if (!out || out_len < 2)
        return;
    size_t o = 0;
    if (raw) {
        for (const char *p = raw; *p && o + 1 < out_len; p++) {
            unsigned char ch = (unsigned char)*p;
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
                if (ch == '.' && o == 0)
                    continue; /* never start with dot / ".." */
                out[o++] = (char)ch;
            } else {
                out[o++] = '_';
            }
        }
    }
    if (o == 0) {
        snprintf(out, out_len, "default");
    } else {
        out[o] = '\0';
        if (strstr(out, "..") != NULL)
            snprintf(out, out_len, "default");
    }
}

void
dtmail_account_apply_preset(DtMailAccount *acct, const char *preset)
{
    if (!acct || !preset)
        return;
    if (acct->sync_interval_sec <= 0)
        acct->sync_interval_sec = 300;
    acct->enabled = 1;

    if (strcasecmp(preset, "gmail") == 0) {
        snprintf(acct->imap_host, sizeof(acct->imap_host), "imap.gmail.com");
        acct->imap_port = 993;
        acct->imap_tls = DTMAIL_TLS_IMPLICIT;
        snprintf(acct->smtp_host, sizeof(acct->smtp_host), "smtp.gmail.com");
        acct->smtp_port = 465;
        acct->smtp_tls = DTMAIL_TLS_IMPLICIT;
        acct->auth_mode = DTMAIL_AUTH_OAUTH2;
        acct->oauth_provider = DTMAIL_OAUTH_GMAIL;
        snprintf(acct->oauth_auth_url, sizeof(acct->oauth_auth_url),
                 "https://accounts.google.com/o/oauth2/v2/auth");
        snprintf(acct->oauth_token_url, sizeof(acct->oauth_token_url),
                 "https://oauth2.googleapis.com/token");
    } else if (strcasecmp(preset, "icloud") == 0) {
        snprintf(acct->imap_host, sizeof(acct->imap_host), "imap.mail.me.com");
        acct->imap_port = 993;
        acct->imap_tls = DTMAIL_TLS_IMPLICIT;
        snprintf(acct->smtp_host, sizeof(acct->smtp_host), "smtp.mail.me.com");
        acct->smtp_port = 587;
        acct->smtp_tls = DTMAIL_TLS_STARTTLS;
        acct->auth_mode = DTMAIL_AUTH_PASSWORD;
        acct->oauth_provider = DTMAIL_OAUTH_NONE;
    } else if (strcasecmp(preset, "fastmail") == 0) {
        snprintf(acct->imap_host, sizeof(acct->imap_host), "imap.fastmail.com");
        acct->imap_port = 993;
        acct->imap_tls = DTMAIL_TLS_IMPLICIT;
        snprintf(acct->smtp_host, sizeof(acct->smtp_host), "smtp.fastmail.com");
        acct->smtp_port = 465;
        acct->smtp_tls = DTMAIL_TLS_IMPLICIT;
        acct->auth_mode = DTMAIL_AUTH_PASSWORD;
        acct->oauth_provider = DTMAIL_OAUTH_NONE;
    } else if (strcasecmp(preset, "m365") == 0 || strcasecmp(preset, "outlook") == 0) {
        snprintf(acct->imap_host, sizeof(acct->imap_host), "outlook.office365.com");
        acct->imap_port = 993;
        acct->imap_tls = DTMAIL_TLS_IMPLICIT;
        snprintf(acct->smtp_host, sizeof(acct->smtp_host), "smtp.office365.com");
        acct->smtp_port = 587;
        acct->smtp_tls = DTMAIL_TLS_STARTTLS;
        acct->auth_mode = DTMAIL_AUTH_OAUTH2;
        acct->oauth_provider = DTMAIL_OAUTH_M365;
        snprintf(acct->oauth_auth_url, sizeof(acct->oauth_auth_url),
                 "https://login.microsoftonline.com/common/oauth2/v2.0/authorize");
        snprintf(acct->oauth_token_url, sizeof(acct->oauth_token_url),
                 "https://login.microsoftonline.com/common/oauth2/v2.0/token");
    } else if (strcasecmp(preset, "local") == 0) {
        acct->imap_host[0] = '\0';
        acct->imap_port = 0;
        acct->smtp_host[0] = '\0';
        acct->smtp_port = 0;
        acct->auth_mode = DTMAIL_AUTH_SENDMAIL_LOCAL;
        acct->oauth_provider = DTMAIL_OAUTH_NONE;
    } else {
        /* Custom IMAP/SMTP defaults */
        if (acct->imap_port <= 0) acct->imap_port = 993;
        acct->imap_tls = DTMAIL_TLS_IMPLICIT;
        if (acct->smtp_port <= 0) acct->smtp_port = 587;
        acct->smtp_tls = DTMAIL_TLS_STARTTLS;
        acct->auth_mode = DTMAIL_AUTH_PASSWORD;
        acct->oauth_provider = DTMAIL_OAUTH_NONE;
    }
}

int
dtmail_accounts_default_path(char *out, size_t out_len)
{
    const char *home = getenv("HOME");
    if (!home || !home[0] || !out || out_len < 32)
        return -1;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.dt", home);
    (void)mkdir(dir, 0700);
    snprintf(out, out_len, "%s/.dt/dtmail_accounts", home);
    return 0;
}

void
dtmail_accounts_shutdown(void)
{
    memset(g_accounts, 0, sizeof(g_accounts));
    g_account_count = 0;
}

void
dtmail_accounts_init(void)
{
    dtmail_accounts_shutdown();
    char path[1024];
    if (dtmail_accounts_default_path(path, sizeof(path)) == 0)
        (void)dtmail_accounts_load_file(path);
}

static char *
next_tsv_field(char **cursor)
{
    if (!cursor || !*cursor)
        return NULL;
    char *start = *cursor;
    char *tab = strchr(start, '\t');
    if (tab) {
        *tab = '\0';
        *cursor = tab + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

int
dtmail_accounts_load_file(const char *path)
{
    if (!path || !path[0])
        return -1;
    dtmail_accounts_shutdown();
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#')
            continue;

        char *cur = line;
        char *f_id      = next_tsv_field(&cur);
        char *f_name    = next_tsv_field(&cur);
        char *f_email   = next_tsv_field(&cur);
        char *f_user    = next_tsv_field(&cur);
        char *f_ihost   = next_tsv_field(&cur);
        char *f_iport   = next_tsv_field(&cur);
        char *f_itls    = next_tsv_field(&cur);
        char *f_shost   = next_tsv_field(&cur);
        char *f_sport   = next_tsv_field(&cur);
        char *f_stls    = next_tsv_field(&cur);
        char *f_auth    = next_tsv_field(&cur);
        char *f_oprov   = next_tsv_field(&cur);
        char *f_ocid    = next_tsv_field(&cur);
        char *f_oauthu  = next_tsv_field(&cur);
        char *f_otoku   = next_tsv_field(&cur);
        char *f_enabled = next_tsv_field(&cur);
        char *f_sync    = next_tsv_field(&cur);

        if (!f_id || !f_id[0] || !f_email)
            continue;

        DtMailAccount a;
        memset(&a, 0, sizeof(a));
        dtmail_sanitize_path_component(f_id, a.id, sizeof(a.id));
        snprintf(a.name, sizeof(a.name), "%s", f_name ? f_name : a.id);
        snprintf(a.email, sizeof(a.email), "%s", f_email);
        snprintf(a.username, sizeof(a.username), "%s", (f_user && f_user[0]) ? f_user : f_email);
        snprintf(a.imap_host, sizeof(a.imap_host), "%s", f_ihost ? f_ihost : "");
        a.imap_port = f_iport ? atoi(f_iport) : 993;
        a.imap_tls = (DtMailTlsMode)(f_itls ? atoi(f_itls) : 0);
        snprintf(a.smtp_host, sizeof(a.smtp_host), "%s", f_shost ? f_shost : "");
        a.smtp_port = f_sport ? atoi(f_sport) : 587;
        a.smtp_tls = (DtMailTlsMode)(f_stls ? atoi(f_stls) : 1);
        a.auth_mode = (DtMailAuthMode)(f_auth ? atoi(f_auth) : 0);
        a.oauth_provider = (DtMailOAuthProvider)(f_oprov ? atoi(f_oprov) : 0);
        snprintf(a.oauth_client_id, sizeof(a.oauth_client_id), "%s", f_ocid ? f_ocid : "");
        snprintf(a.oauth_auth_url, sizeof(a.oauth_auth_url), "%s", f_oauthu ? f_oauthu : "");
        snprintf(a.oauth_token_url, sizeof(a.oauth_token_url), "%s", f_otoku ? f_otoku : "");
        a.enabled = f_enabled ? (atoi(f_enabled) ? 1 : 0) : 1;
        a.sync_interval_sec = f_sync ? atoi(f_sync) : 300;
        if (a.sync_interval_sec < 30) a.sync_interval_sec = 300;
        (void)dtmail_accounts_upsert(&a);
    }
    fclose(fp);
    return 0;
}

int
dtmail_accounts_save_file(const char *path)
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
    fprintf(fp, "# dtmail accounts (v1, TSV, secrets stored in keyring)\n");
    for (int i = 0; i < g_account_count; i++) {
        const DtMailAccount *a = &g_accounts[i];
        fprintf(fp, "%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%d\t%d\n",
                a->id, a->name, a->email, a->username,
                a->imap_host, a->imap_port, (int)a->imap_tls,
                a->smtp_host, a->smtp_port, (int)a->smtp_tls,
                (int)a->auth_mode, (int)a->oauth_provider,
                a->oauth_client_id, a->oauth_auth_url, a->oauth_token_url,
                a->enabled ? 1 : 0, a->sync_interval_sec);
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
dtmail_accounts_count(void)
{
    return g_account_count;
}

const DtMailAccount *
dtmail_accounts_get(int idx)
{
    if (idx < 0 || idx >= g_account_count)
        return NULL;
    return &g_accounts[idx];
}

int
dtmail_accounts_find_by_id(const char *id)
{
    if (!id || !id[0])
        return -1;
    for (int i = 0; i < g_account_count; i++) {
        if (strcmp(g_accounts[i].id, id) == 0)
            return i;
    }
    return -1;
}

int
dtmail_accounts_upsert(const DtMailAccount *acct)
{
    if (!acct)
        return -1;
    DtMailAccount clean = *acct;
    if (!clean.id[0]) {
        dtmail_sanitize_path_component(clean.email[0] ? clean.email : clean.name,
                                       clean.id, sizeof(clean.id));
    } else {
        char tmp_id[64];
        dtmail_sanitize_path_component(clean.id, tmp_id, sizeof(tmp_id));
        snprintf(clean.id, sizeof(clean.id), "%s", tmp_id);
    }
    strip_tsv_chars(clean.name);
    strip_tsv_chars(clean.email);
    strip_tsv_chars(clean.username);
    strip_tsv_chars(clean.imap_host);
    strip_tsv_chars(clean.smtp_host);
    strip_tsv_chars(clean.oauth_client_id);
    strip_tsv_chars(clean.oauth_auth_url);
    strip_tsv_chars(clean.oauth_token_url);
    if (clean.sync_interval_sec < 30)
        clean.sync_interval_sec = 300;

    int existing = dtmail_accounts_find_by_id(clean.id);
    if (existing >= 0) {
        g_accounts[existing] = clean;
        return existing;
    }
    if (g_account_count >= DTMAIL_MAX_ACCOUNTS)
        return -1;
    g_accounts[g_account_count] = clean;
    return g_account_count++;
}

int
dtmail_accounts_remove(int idx)
{
    if (idx < 0 || idx >= g_account_count)
        return -1;
    for (int i = idx; i + 1 < g_account_count; i++)
        g_accounts[i] = g_accounts[i + 1];
    g_account_count--;
    memset(&g_accounts[g_account_count], 0, sizeof(g_accounts[g_account_count]));
    return 0;
}

static int
ensure_empty_mbox(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISREG(st.st_mode) ? 0 : -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return (errno == EEXIST) ? 0 : -1;
    close(fd);
    return 0;
}

int
dtmail_ensure_local_fallback_inbox(char *out, size_t out_len)
{
    const char *home = getenv("HOME");
    if (!home || !home[0] || !out || out_len < 32)
        return -1;
    char d1[1024], d2[1024];
    snprintf(d1, sizeof(d1), "%s/.dt", home);
    (void)mkdir(d1, 0700);
    snprintf(d2, sizeof(d2), "%s/.dt/mail", home);
    (void)mkdir(d2, 0700);
    snprintf(out, out_len, "%s/.dt/mail/Inbox", home);
    return ensure_empty_mbox(out);
}

int
dtmail_account_folder_path(const char *acct_id, const char *folder,
                           char *out, size_t out_len)
{
    const char *home = getenv("HOME");
    if (!home || !home[0] || !out || out_len < 48)
        return -1;
    char safe_id[64], safe_folder[64];
    dtmail_sanitize_path_component(acct_id, safe_id, sizeof(safe_id));
    dtmail_sanitize_path_component(folder ? folder : "INBOX", safe_folder, sizeof(safe_folder));

    char d1[1024], d2[1024], d3[1152];
    snprintf(d1, sizeof(d1), "%s/.dt", home);
    (void)mkdir(d1, 0700);
    snprintf(d2, sizeof(d2), "%s/.dt/mail", home);
    (void)mkdir(d2, 0700);
    snprintf(d3, sizeof(d3), "%s/.dt/mail/%s", home, safe_id);
    (void)mkdir(d3, 0700);

    snprintf(out, out_len, "%s/%s", d3, safe_folder);
    return ensure_empty_mbox(out);
}

int
dtmail_resolve_startup_inbox(const char *spool_path,
                             char *out, size_t out_len,
                             int *out_used_fallback)
{
    if (out_used_fallback) *out_used_fallback = 0;
    if (!out || out_len < 32)
        return -1;

    /* 1. Prefer the first enabled remote IMAP/POP3 account if configured */
    for (int i = 0; i < g_account_count; i++) {
        if (g_accounts[i].enabled && g_accounts[i].imap_host[0]) {
            if (dtmail_account_folder_path(g_accounts[i].id, "INBOX", out, out_len) == 0)
                return 0;
        }
    }

    /* 2. Use traditional UNIX spool if it exists and is readable */
    if (spool_path && spool_path[0]) {
        struct stat st;
        if (stat(spool_path, &st) == 0 && S_ISREG(st.st_mode) && access(spool_path, R_OK) == 0) {
            snprintf(out, out_len, "%s", spool_path);
            return 0;
        }
    }

    /* 3. Graceful fallback: local ~/.dt/mail/Inbox so dtmail is always usable */
    if (dtmail_ensure_local_fallback_inbox(out, out_len) == 0) {
        if (out_used_fallback) *out_used_fallback = 1;
        return 0;
    }
    return -1;
}

/* --- Desktop Keyring (secret-tool via fork/exec, zero shell interpolation) --- */

static const char *
keyring_tool_cmd(void)
{
    const char *override = getenv("DTMAIL_KEYRING_CMD");
    return (override && override[0]) ? override : "secret-tool";
}

int
dtmail_keyring_store(const char *acct_id, const char *kind, const char *secret)
{
    if (!acct_id || !acct_id[0] || !kind || !kind[0] || !secret)
        return -1;

    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[1]);
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        char label[192];
        snprintf(label, sizeof(label), "CK-Core CDE Mail (%s:%s)", acct_id, kind);
        const char *cmd = keyring_tool_cmd();
        char *argv[] = {
            (char *)cmd, (char *)"store",
            (char *)"--label", label,
            (char *)"service", (char *)"ck-core-cde-dtmail",
            (char *)"account", (char *)acct_id,
            (char *)"kind",    (char *)kind,
            NULL
        };
        execvp(cmd, argv);
        _exit(127);
    }

    close(pfd[0]);
    size_t slen = strlen(secret);
    ssize_t wr = write(pfd[1], secret, slen);
    (void)wr;
    close(pfd[1]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int
dtmail_keyring_lookup(const char *acct_id, const char *kind, char *out, size_t out_len)
{
    if (!acct_id || !acct_id[0] || !kind || !kind[0] || !out || out_len < 2)
        return -1;
    out[0] = '\0';

    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const char *cmd = keyring_tool_cmd();
        char *argv[] = {
            (char *)cmd, (char *)"lookup",
            (char *)"service", (char *)"ck-core-cde-dtmail",
            (char *)"account", (char *)acct_id,
            (char *)"kind",    (char *)kind,
            NULL
        };
        execvp(cmd, argv);
        _exit(127);
    }

    close(pfd[1]);
    size_t total = 0;
    while (total + 1 < out_len) {
        ssize_t n = read(pfd[0], out + total, out_len - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    out[total] = '\0';
    close(pfd[0]);

    while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r'))
        out[--total] = '\0';

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0 && total > 0) ? 0 : -1;
}

int
dtmail_keyring_clear(const char *acct_id, const char *kind)
{
    if (!acct_id || !acct_id[0] || !kind || !kind[0])
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const char *cmd = keyring_tool_cmd();
        char *argv[] = {
            (char *)cmd, (char *)"clear",
            (char *)"service", (char *)"ck-core-cde-dtmail",
            (char *)"account", (char *)acct_id,
            (char *)"kind",    (char *)kind,
            NULL
        };
        execvp(cmd, argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}
