/*
 * CDE - Common Desktop Environment
 *
 * Display.h - Style Manager Multi-Monitor Arrangement, Rotation & HiDPI Dialog
 *             (Wayland dtwm-wayland & X11/XLibre RandR).
 */

#ifndef _dtstyle_display_h
#define _dtstyle_display_h

#include <X11/Intrinsic.h>

extern void popup_displayBB(Widget shell);
extern void restoreDisplay(Widget shell, XrmDatabase db);
extern void saveDisplay(int fd);

#endif /* _dtstyle_display_h */
