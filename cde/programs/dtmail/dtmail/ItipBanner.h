#ifndef DTMAIL_ITIP_BANNER_H
#define DTMAIL_ITIP_BANNER_H

#include <stddef.h>
#ifndef DTMAIL_ITIP_NO_GUI
#include <Xm/Xm.h>
#endif

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

/* Save .ics safely under ~/.dt/calendar/incoming_invite.ics (0600) and open dtcm
 * (via _DTCM_IPC IMPORT_ICS if running, or launching dtcm --import-ics <path>) */
int  dtmail_itip_open_in_dtcm(const char *ics_raw, size_t ics_len,
                              char *out_saved_path, size_t out_path_len);

/* Track current displayed message's iTIP invite (called from Editor / RoamMenuWindow) */
typedef void (*DtMailItipChangeCB)(int has_invite, const DtMailItipInvite *inv, void *userdata);
void dtmail_itip_register_change_cb(DtMailItipChangeCB cb, void *userdata);
void dtmail_itip_set_current(const char *raw, size_t len);
int  dtmail_itip_has_current(void);
const DtMailItipInvite *dtmail_itip_get_current(void);
int  dtmail_itip_open_current_in_dtcm(void);

#ifndef DTMAIL_ITIP_NO_GUI
typedef struct DtMailItipBanner DtMailItipBanner;
DtMailItipBanner *dtmail_itip_banner_create(Widget parent_form,
                                            Widget above_widget,
                                            Widget below_widget);
void dtmail_itip_banner_destroy(DtMailItipBanner *banner);
void dtmail_itip_banner_refresh(DtMailItipBanner *banner);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_ITIP_BANNER_H */
