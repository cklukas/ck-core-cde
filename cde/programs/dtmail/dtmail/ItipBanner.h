#ifndef DTMAIL_ITIP_BANNER_H
#define DTMAIL_ITIP_BANNER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  valid;
    char method[32];
    char summary[256];
    char dtstart[64];
    char dtend[64];
    char location[256];
    char organizer[256];
    char uid[128];
} DtMailItipInvite;

/* Parse an iCalendar (BEGIN:VCALENDAR ... END:VCALENDAR) block from text */
int  dtmail_itip_parse(const char *raw, size_t len, DtMailItipInvite *out);

/* Save .ics safely under ~/.dt/calendar/incoming_invite.ics (0600) and open dtcm */
int  dtmail_itip_open_in_dtcm(const char *ics_raw, size_t ics_len,
                              char *out_saved_path, size_t out_path_len);

/* Track current displayed message's iTIP invite (called from Editor / RoamMenuWindow) */
void dtmail_itip_set_current(const char *raw, size_t len);
int  dtmail_itip_has_current(void);
const DtMailItipInvite *dtmail_itip_get_current(void);
int  dtmail_itip_open_current_in_dtcm(void);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_ITIP_BANNER_H */
