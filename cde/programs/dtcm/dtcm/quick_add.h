#ifndef DTCM_QUICK_ADD_H
#define DTCM_QUICK_ADD_H

#include <time.h>

#ifndef DTCM_QUICK_ADD_NO_GUI
#include "calendar.h"
#endif

typedef struct {
    char   summary[256];
    char   location[256];
    time_t start_time;
    time_t end_time;
    int    all_day;
} DtcmQuickAddEvent;

/* Natural-language parser: e.g. "Design Sync tomorrow 14:00-15:30 @ Studio 2" */
int dtcm_quick_add_parse(const char *input, time_t now, DtcmQuickAddEvent *out);

#ifndef DTCM_QUICK_ADD_NO_GUI
void dtcm_quick_add_toggle(Calendar *c, Boolean show_and_focus);
#endif

#endif /* DTCM_QUICK_ADD_H */
