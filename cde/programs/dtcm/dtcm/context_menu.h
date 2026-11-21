#ifndef DTCM_CONTEXT_MENU_H
#define DTCM_CONTEXT_MENU_H

#include <X11/Xlib.h>
#include "calendar.h"

void dtcm_context_menu_popup(Calendar *c, XButtonEvent *ev, Tick day, int appt_cache_idx);
void dtcm_copy_selected_appt(Calendar *c);
void dtcm_paste_appt(Calendar *c, Tick target_time);
void dtcm_delete_selected_appt(Calendar *c);

#endif /* DTCM_CONTEXT_MENU_H */
