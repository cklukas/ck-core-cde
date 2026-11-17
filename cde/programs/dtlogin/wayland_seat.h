/*
 * Wayland local-seat display class, unprivileged _dtgreeter session management,
 * FreeDesktop session menu discovery, and VT/seat handoff for dtlogin.
 */

#ifndef _DTLOGIN_WAYLAND_SEAT_H
#define _DTLOGIN_WAYLAND_SEAT_H

#include "dm.h"
#include <X11/Xresource.h>

int DtloginStartWaylandGreeter(struct display *d);
void DtloginPrepareWaylandGreeterChild(struct display *d);
void DtloginPopulateDesktopSessions(struct display *d, XrmDatabase *db);
int DtloginResolveDefaultWaylandSession(struct display *d, XrmDatabase db, char ***argv_out);
int DtloginHandoffWaylandSeat(struct display *d, struct verify_info *verify, const char *pam_service);
void DtloginSyncPamSessionEnv(struct display *d, struct verify_info *verify);
void DtloginCleanupWaylandSeat(struct display *d);

#endif /* _DTLOGIN_WAYLAND_SEAT_H */
