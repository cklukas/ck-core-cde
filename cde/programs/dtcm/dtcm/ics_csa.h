#ifndef DTCM_ICS_CSA_H
#define DTCM_ICS_CSA_H

#include "calendar.h"

int dtcm_import_ics_file(Calendar *c, const char *ics_path, int *out_count);
int dtcm_export_ics_file(Calendar *c, const char *ics_path, int *out_count);

void dtcm_ics_add_menu_items(Calendar *c, Widget file_menu);
void dtcm_ics_init_gui(Calendar *c);
void dtcm_ics_publish_state(Calendar *c);

#endif /* DTCM_ICS_CSA_H */
