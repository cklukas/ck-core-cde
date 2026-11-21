#ifndef DTCM_CALDAV_CLIENT_H
#define DTCM_CALDAV_CLIENT_H

#include <stddef.h>
#include "ics_io.h"

typedef struct {
    char url[1024];
    char username[256];
    char password[256];
    int verify_tls; /* 1 = verify TLS certs (default), 0 = skip for self-signed */
} DtcmCaldavConfig;

typedef struct {
    char href[1024];
    char etag[256];
} DtcmCaldavItem;

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

int dtcm_caldav_load_profile(DtcmCaldavConfig *cfg);
int dtcm_caldav_save_profile(const DtcmCaldavConfig *cfg);

#endif /* DTCM_CALDAV_CLIENT_H */
