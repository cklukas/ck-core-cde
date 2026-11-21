#ifndef DTMAIL_CURL_MAIL_ENGINE_H
#define DTMAIL_CURL_MAIL_ENGINE_H

#include <stddef.h>
#include "MailAccountStore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Safely appends an RFC 5322 message to a UNIX mbox file (synthesizing a
 * valid "From <addr> <ctime>" separator and '>' escaping body "From " lines). */
int dtmail_mbox_append_rfc5322(const char *mbox_path,
                               const char *from_addr,
                               const char *raw_msg,
                               size_t raw_len);

/* IMAP operations over IMAPS / IMAP+STARTTLS (or plaintext for local tests) */
int dtmail_imap_test_connection(const DtMailAccount *acct,
                                const char *secret,
                                char *err_buf,
                                size_t err_len);

int dtmail_imap_list_folders(const DtMailAccount *acct,
                             const char *secret,
                             char folders[][128],
                             int max_folders,
                             char *err_buf,
                             size_t err_len);

int dtmail_imap_sync_folder(const DtMailAccount *acct,
                            const char *secret,
                            const char *folder,
                            const char *local_mbox_path,
                            int *out_added,
                            char *err_buf,
                            size_t err_len);

int dtmail_imap_set_flag(const DtMailAccount *acct,
                         const char *secret,
                         const char *folder,
                         unsigned long uid,
                         const char *flag,
                         int add_flag,
                         char *err_buf,
                         size_t err_len);

/* SMTP submission over SMTPS (465) / SMTP+STARTTLS (587) */
int dtmail_smtp_send(const DtMailAccount *acct,
                     const char *secret,
                     const char *from_addr,
                     const char *const *rcpts,
                     int rcpt_count,
                     const char *raw_rfc5322,
                     size_t raw_len,
                     char *err_buf,
                     size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_CURL_MAIL_ENGINE_H */
