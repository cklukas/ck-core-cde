#ifndef DTMAIL_THREAD_RULES_H
#define DTMAIL_THREAD_RULES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DTMAIL_MAX_RULES 64

typedef enum {
    DTMAIL_FILTER_ALL = 0,
    DTMAIL_FILTER_UNREAD,
    DTMAIL_FILTER_STARRED,
    DTMAIL_FILTER_HAS_ATTACH
} DtMailFilterMode;

typedef enum {
    DTMAIL_RULE_FIELD_FROM = 0,
    DTMAIL_RULE_FIELD_TO,
    DTMAIL_RULE_FIELD_SUBJECT
} DtMailRuleField;

typedef enum {
    DTMAIL_RULE_ACT_MOVE = 0,
    DTMAIL_RULE_ACT_MARK_READ,
    DTMAIL_RULE_ACT_STAR,
    DTMAIL_RULE_ACT_JUNK
} DtMailRuleAction;

typedef struct {
    char name[128];
    DtMailRuleField field;
    char pattern[256];
    DtMailRuleAction action;
    char target_folder[128];
    int  enabled;
} DtMailRule;

typedef struct {
    int  orig_index;
    char msg_id[256];
    char in_reply_to[256];
    char subject[256];
    long timestamp;
    /* Outputs filled by dtmail_thread_group() */
    int  thread_id;
    int  depth;
} DtMailThreadItem;

/* Strips Re:/Fwd:/Fw:/Aw: prefixes and collapses whitespace */
void dtmail_normalize_subject(const char *subject, char *out, size_t out_len);

/* Groups and sorts items into conversation threads by In-Reply-To / normalized Subject.
 * Returns number of distinct threads. */
int dtmail_thread_group(DtMailThreadItem *items, int count);

/* Case-insensitive quick filter matching */
int dtmail_quick_filter_matches(const char *from,
                                const char *subject,
                                const char *snippet,
                                int is_unread,
                                int is_starred,
                                int has_attach,
                                const char *query,
                                DtMailFilterMode mode);

/* Local mail rules (~/.dt/dtmail_rules, mode 0600) */
int  dtmail_rules_default_path(char *out, size_t out_len);
void dtmail_rules_init(void);
void dtmail_rules_shutdown(void);
int  dtmail_rules_load_file(const char *path);
int  dtmail_rules_save_file(const char *path);
int  dtmail_rules_count(void);
const DtMailRule *dtmail_rules_get(int idx);
int  dtmail_rules_add(const DtMailRule *rule);
int  dtmail_rules_remove(int idx);

/* Evaluates rules in order; returns index of first matching rule, or -1 if none. */
int  dtmail_rules_evaluate(const char *from, const char *to, const char *subject);

/* Non-blocking desktop notification via notify-send (respects DTMAIL_NO_NOTIFY=1) */
int  dtmail_notify_new_mail(int count, const char *sender, const char *subject);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_THREAD_RULES_H */
