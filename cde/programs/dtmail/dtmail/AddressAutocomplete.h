#ifndef DTMAIL_ADDRESS_AUTOCOMPLETE_H
#define DTMAIL_ADDRESS_AUTOCOMPLETE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DTMAIL_MAX_CONTACTS 512

typedef struct {
    char name[128];
    char email[256];
} DtMailContact;

int  dtmail_contacts_default_path(char *out, size_t out_len);
void dtmail_contacts_init(void);
void dtmail_contacts_shutdown(void);
int  dtmail_contacts_load_file(const char *path);
int  dtmail_contacts_save_file(const char *path);
int  dtmail_contacts_count(void);
int  dtmail_contacts_add(const char *name, const char *email);
int  dtmail_contacts_harvest_header(const char *header_val);

/* Returns number of matching contacts (formatted as "Name <email>" or "email") */
int  dtmail_contacts_match(const char *prefix, char results[][256], int max_results);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_ADDRESS_AUTOCOMPLETE_H */
