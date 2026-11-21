#ifndef DTCM_CONTEXT_MENU_H
#define DTCM_CONTEXT_MENU_H

#include <X11/Xlib.h>
#include "calendar.h"

void dtcm_context_menu_popup(Calendar *c, XButtonEvent *ev, Tick day, int appt_cache_idx);

#endif /* DTCM_CONTEXT_MENU_H */
