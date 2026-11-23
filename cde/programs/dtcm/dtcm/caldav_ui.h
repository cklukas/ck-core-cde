#ifndef DTCM_CALDAV_UI_H
#define DTCM_CALDAV_UI_H

#include "calendar.h"

void dtcm_caldav_add_menu_item(Calendar *c, Widget file_menu);
void dtcm_caldav_show_dialog(Calendar *c);
void dtcm_caldav_trigger_writeback(Calendar *c);

#endif /* DTCM_CALDAV_UI_H */
