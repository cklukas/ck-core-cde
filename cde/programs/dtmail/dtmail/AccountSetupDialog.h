#ifndef DTMAIL_ACCOUNT_SETUP_DIALOG_H
#define DTMAIL_ACCOUNT_SETUP_DIALOG_H

#include <X11/Intrinsic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DtMailSyncDoneCb)(int total_added, const char *status_msg, void *user_data);

void dtmail_show_accounts_dialog(Widget parent, DtMailSyncDoneCb on_sync_done, void *user_data);
void dtmail_start_async_sync(XtAppContext app, DtMailSyncDoneCb on_sync_done, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_ACCOUNT_SETUP_DIALOG_H */
