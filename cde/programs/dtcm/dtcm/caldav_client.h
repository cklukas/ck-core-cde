#ifndef DTCM_CALDAV_CLIENT_H
#define DTCM_CALDAV_CLIENT_H

#include <stddef.h>
#include "ics_io.h"

typedef struct {
    char url[1024];
    char username[256];
    char password[256];
    int verify_tls;     /* 1 = verify TLS certs (default), 0 = skip for self-signed */
    int auto_sync_mins; /* 0 = disabled, >0 = periodic auto-sync interval in minutes */
} DtcmCaldavConfig;

typedef struct {
    char href[1024];
    char etag[256];
} DtcmCaldavItem;

typedef struct {
    char url[1024];
    char href[1024];
    char uid[256];
    char etag[256];
} DtcmCaldavStateEntry;

typedef struct {
    DtcmCaldavStateEntry *entries;
    int count;
    int cap;
} DtcmCaldavSyncState;

int dtcm_caldav_is_supported(void);

int dtcm_caldav_validate_url(const char *url, char *errbuf, size_t errsz);

int dtcm_caldav_propfind(const DtcmCaldavConfig *cfg,
                         DtcmCaldavItem **out_items,
                         int *out_count,
                         char *errbuf,
                         size_t errsz);

void dtcm_caldav_free_items(DtcmCaldavItem *items);

int dtcm_caldav_get_events(const DtcmCaldavConfig *cfg,
                           const char *item_url,
                           DtcmIcsEvent **out_events,
                           int *out_count,
                           char *errbuf,
                           size_t errsz);

int dtcm_caldav_put_event(const DtcmCaldavConfig *cfg,
                          const DtcmIcsEvent *ev,
                          char *errbuf,
                          size_t errsz);

int dtcm_caldav_put_event_ex(const DtcmCaldavConfig *cfg,
                             const DtcmIcsEvent *ev,
                             char *out_href,
                             size_t href_sz,
                             char *out_etag,
                             size_t etag_sz,
                             char *errbuf,
                             size_t errsz);

int dtcm_caldav_load_profile(DtcmCaldavConfig *cfg);
int dtcm_caldav_save_profile(const DtcmCaldavConfig *cfg);

/* Persistent (UID, ETag) sync state (~/.dt/dtcm_caldav_state, mode 0600) */
int dtcm_caldav_state_load(DtcmCaldavSyncState *st);
int dtcm_caldav_state_save(const DtcmCaldavSyncState *st);
void dtcm_caldav_state_free(DtcmCaldavSyncState *st);
const DtcmCaldavStateEntry *dtcm_caldav_state_find_href(const DtcmCaldavSyncState *st,
                                                        const char *url,
                                                        const char *href);
const DtcmCaldavStateEntry *dtcm_caldav_state_find_uid(const DtcmCaldavSyncState *st,
                                                       const char *url,
                                                       const char *uid);
int dtcm_caldav_state_upsert(DtcmCaldavSyncState *st,
                             const char *url,
                             const char *href,
                             const char *uid,
                             const char *etag);

/* Desktop keyring integration (secret-tool / gnome-keyring, optional) */
int dtcm_caldav_keyring_store(const char *url, const char *username, const char *password);
int dtcm_caldav_keyring_lookup(const char *url, const char *username,
                               char *out_pass, size_t pass_sz);
int dtcm_caldav_keyring_clear(const char *url, const char *username);

#endif /* DTCM_CALDAV_CLIENT_H */
