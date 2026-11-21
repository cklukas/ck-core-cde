#ifndef DTCM_AGENDA_UI_H
#define DTCM_AGENDA_UI_H

#include <Xm/Xm.h>
#include "calendar.h"

/* Collapsible inline Quick-Filter bar above the main canvas */
void dtcm_filter_bar_create(Calendar *c);
void dtcm_filter_bar_toggle(Calendar *c, Boolean show_and_focus);
Boolean dtcm_filter_bar_is_active(void);
Boolean dtcm_filter_matches(const char *what);

/* 5th Main View: Agenda (upcoming chronological schedule) */
void agenda_button(Widget w, XtPointer client_data, XtPointer call_data);
void paint_agendaview(Calendar *c);
void cleanup_after_agenda(Calendar *c);

#endif /* DTCM_AGENDA_UI_H */
