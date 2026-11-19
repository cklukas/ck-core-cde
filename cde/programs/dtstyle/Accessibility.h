/*
 * CDE - Common Desktop Environment
 * Accessibility.h: Style Manager Accessibility dialog & persistence (WP-20)
 */

#ifndef _dtstyle_accessibility_h
#define _dtstyle_accessibility_h

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <Xm/Xm.h>

void popup_accessibilityBB(Widget shell);
void restoreAccessibility(Widget shell, XrmDatabase db);
void saveAccessibility(int fd);
int ApplyAccessibilitySpecCLI(const char *spec);

#endif /* _dtstyle_accessibility_h */
