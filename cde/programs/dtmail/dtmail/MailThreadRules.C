/*
 * CDE - Common Desktop Environment
 *
 * Conversation threading, inline Quick-Filter matching, local Mail Rules
 * (~/.dt/dtmail_rules, mode 0600), and desktop new-mail notifications for dtmail.
 */

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "MailThreadRules.h"

static DtMailRule g_rules[DTMAIL_MAX_RULES];
static int g_rule_count = 0;

static int
ci_substr(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!hay || !hay[0]) return 0;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

void
dtmail_normalize_subject(const char *subject, char *out, size_t out_len)
{
    if (!out || out_len < 2) return;
    out[0] = '\0';
    if (!subject) return;

    const char *p = subject;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncasecmp(p, "re:", 3) == 0) { p += 3; continue; }
        if (strncasecmp(p, "fw:", 3) == 0) { p += 3; continue; }
        if (strncasecmp(p, "aw:", 3) == 0) { p += 3; continue; }
        if (strncasecmp(p, "fwd:", 4) == 0) { p += 4; continue; }
        break;
    }

    size_t o = 0;
    int last_sp = 0;
    while (*p && o + 1 < out_len) {
        unsigned char c = (unsigned char)*p++;
        if (isspace(c)) {
            if (!last_sp && o > 0) {
                out[o++] = ' ';
                last_sp = 1;
            }
        } else {
            out[o++] = (char)tolower(c);
            last_sp = 0;
        }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

static int
cmp_thread_item(const void *a, const void *b)
{
    const DtMailThreadItem *ia = (const DtMailThreadItem *)a;
    const DtMailThreadItem *ib = (const DtMailThreadItem *)b;
    if (ia->thread_id != ib->thread_id)
        return ia->thread_id - ib->thread_id;
    if (ia->depth != ib->depth)
        return ia->depth - ib->depth;
    if (ia->timestamp < ib->timestamp) return -1;
    if (ia->timestamp > ib->timestamp) return 1;
    return ia->orig_index - ib->orig_index;
}

int
dtmail_thread_group(DtMailThreadItem *items, int count)
{
    if (!items || count <= 0)
        return 0;

    char (*norm_subs)[256] = (char (*)[256])calloc((size_t)count, 256);
    if (!norm_subs)
        return 0;

    for (int i = 0; i < count; i++) {
        dtmail_normalize_subject(items[i].subject, norm_subs[i], 256);
        items[i].thread_id = -1;
        items[i].depth = 0;
    }

    int next_thread = 0;
    for (int i = 0; i < count; i++) {
        /* 1. Try matching In-Reply-To against an earlier Message-ID */
        int parent = -1;
        if (items[i].in_reply_to[0]) {
            for (int j = 0; j < count; j++) {
                if (j != i && items[j].msg_id[0] &&
                    strcmp(items[j].msg_id, items[i].in_reply_to) == 0) {
                    parent = j;
                    break;
                }
            }
        }
        /* 2. Fallback: match normalized non-empty subject */
        if (parent < 0 && norm_subs[i][0]) {
            for (int j = 0; j < i; j++) {
                if (strcmp(norm_subs[j], norm_subs[i]) == 0) {
                    parent = j;
                    break;
                }
            }
        }
        if (parent >= 0 && items[parent].thread_id >= 0) {
            items[i].thread_id = items[parent].thread_id;
            items[i].depth = items[parent].depth + 1;
            if (items[i].depth > 6) items[i].depth = 6;
        } else {
            items[i].thread_id = next_thread++;
            items[i].depth = 0;
        }
    }

    free(norm_subs);
    qsort(items, (size_t)count, sizeof(DtMailThreadItem), cmp_thread_item);
    return next_thread;
}

int
dtmail_quick_filter_matches(const char *from,
                            const char *subject,
                            const char *snippet,
                            int is_unread,
                            int is_starred,
                            int has_attach,
                            const char *query,
                            DtMailFilterMode mode)
{
    if (mode == DTMAIL_FILTER_UNREAD && !is_unread) return 0;
    if (mode == DTMAIL_FILTER_STARRED && !is_starred) return 0;
    if (mode == DTMAIL_FILTER_HAS_ATTACH && !has_attach) return 0;
    if (query && query[0]) {
        if (!ci_substr(from, query) &&
            !ci_substr(subject, query) &&
            !ci_substr(snippet, query))
            return 0;
    }
    return 1;
}

int
dtmail_rules_default_path(char *out, size_t out_len)
{
    const char *home = getenv("HOME");
    if (!home || !home[0] || !out || out_len < 32)
        return -1;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.dt", home);
    (void)mkdir(dir, 0700);
    snprintf(out, out_len, "%s/.dt/dtmail_rules", home);
    return 0;
}

void
dtmail_rules_shutdown(void)
{
    memset(g_rules, 0, sizeof(g_rules));
    g_rule_count = 0;
}

void
dtmail_rules_init(void)
{
    dtmail_rules_shutdown();
    char path[1024];
    if (dtmail_rules_default_path(path, sizeof(path)) == 0)
        (void)dtmail_rules_load_file(path);
}

int
dtmail_rules_load_file(const char *path)
{
    if (!path || !path[0]) return -1;
    dtmail_rules_shutdown();
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (l == 0 || line[0] == '#') continue;
        /* enabled \t field \t action \t name \t pattern \t target_folder */
        char *p1 = strchr(line, '\t'); if (!p1) continue; *p1++ = '\0';
        char *p2 = strchr(p1, '\t');   if (!p2) continue; *p2++ = '\0';
        char *p3 = strchr(p2, '\t');   if (!p3) continue; *p3++ = '\0';
        char *p4 = strchr(p3, '\t');   if (!p4) continue; *p4++ = '\0';
        char *p5 = strchr(p4, '\t');   if (!p5) continue; *p5++ = '\0';

        DtMailRule r;
        memset(&r, 0, sizeof(r));
        r.enabled = atoi(line) ? 1 : 0;
        r.field = (DtMailRuleField)atoi(p1);
        r.action = (DtMailRuleAction)atoi(p2);
        snprintf(r.name, sizeof(r.name), "%s", p3);
        snprintf(r.pattern, sizeof(r.pattern), "%s", p4);
        snprintf(r.target_folder, sizeof(r.target_folder), "%s", p5);
        (void)dtmail_rules_add(&r);
    }
    fclose(fp);
    return 0;
}

int
dtmail_rules_save_file(const char *path)
{
    if (!path || !path[0]) return -1;
    char tmp[1280];
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0600);
    FILE *fp = fdopen(fd, "w");
    if (!fp) { close(fd); unlink(tmp); return -1; }
    fprintf(fp, "# dtmail rules: enabled<TAB>field<TAB>action<TAB>name<TAB>pattern<TAB>target\n");
    for (int i = 0; i < g_rule_count; i++) {
        const DtMailRule *r = &g_rules[i];
        fprintf(fp, "%d\t%d\t%d\t%s\t%s\t%s\n",
                r->enabled ? 1 : 0, (int)r->field, (int)r->action,
                r->name, r->pattern, r->target_folder);
    }
    if (fclose(fp) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int dtmail_rules_count(void) { return g_rule_count; }

const DtMailRule *
dtmail_rules_get(int idx)
{
    if (idx < 0 || idx >= g_rule_count) return NULL;
    return &g_rules[idx];
}

int
dtmail_rules_add(const DtMailRule *rule)
{
    if (!rule || !rule->pattern[0] || g_rule_count >= DTMAIL_MAX_RULES)
        return -1;
    g_rules[g_rule_count] = *rule;
    return g_rule_count++;
}

int
dtmail_rules_remove(int idx)
{
    if (idx < 0 || idx >= g_rule_count) return -1;
    for (int i = idx; i + 1 < g_rule_count; i++)
        g_rules[i] = g_rules[i + 1];
    g_rule_count--;
    return 0;
}

int
dtmail_rules_evaluate(const char *from, const char *to, const char *subject)
{
    for (int i = 0; i < g_rule_count; i++) {
        const DtMailRule *r = &g_rules[i];
        if (!r->enabled || !r->pattern[0]) continue;
        const char *val = "";
        if (r->field == DTMAIL_RULE_FIELD_FROM) val = from ? from : "";
        else if (r->field == DTMAIL_RULE_FIELD_TO) val = to ? to : "";
        else val = subject ? subject : "";
        if (ci_substr(val, r->pattern))
            return i;
    }
    return -1;
}

int
dtmail_notify_new_mail(int count, const char *sender, const char *subject)
{
    const char *no_n = getenv("DTMAIL_NO_NOTIFY");
    if (no_n && strcmp(no_n, "1") == 0)
        return 0;
    if (count <= 0)
        return 0;

    char title[128], body[512];
    if (count == 1) {
        snprintf(title, sizeof(title), "New Mail from %s", (sender && sender[0]) ? sender : "Unknown");
        snprintf(body, sizeof(body), "%s", (subject && subject[0]) ? subject : "(No Subject)");
    } else {
        snprintf(title, sizeof(title), "%d New Messages", count);
        snprintf(body, sizeof(body), "Latest: %s - %s",
                 (sender && sender[0]) ? sender : "Unknown",
                 (subject && subject[0]) ? subject : "(No Subject)");
    }

    const char *cmd = getenv("DTMAIL_NOTIFY_CMD");
    if (!cmd || !cmd[0]) cmd = "notify-send";

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        char *argv[] = {
            (char *)cmd, (char *)"-a", (char *)"CDE Mailer",
            (char *)"--", title, body, NULL
        };
        execvp(cmd, argv);
        _exit(127);
    }
    return 0;
}
