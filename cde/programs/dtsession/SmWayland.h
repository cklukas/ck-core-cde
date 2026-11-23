/*
 * CK Core CDE - Wayland Session Bootstrap, ext-session-lock-v1, and
 * ext-idle-notify-v1 integration for dtsession (WP-06)
 */

#ifndef _smwayland_h
#define _smwayland_h

#include <X11/Intrinsic.h>
#include <X11/Xlib.h>

void SmWaylandPreInit(int *argc, char **argv);
Boolean SmWaylandActive(void);
Boolean SmWaylandManagedCompositor(void);
Boolean SmWaylandInitXtIntegration(XtAppContext app, Display *dpy);
void SmWaylandUpdateIdleTimeout(int timeout_seconds);
void SmWaylandMarkLockWindow(Display *dpy, Window win);
Boolean SmWaylandAcquireSessionLock(void);
Boolean SmWaylandSessionIsLocked(void);
void SmWaylandReleaseSessionLock(void);
void SmWaylandShutdown(int exit_status);

#endif /* _smwayland_h */
