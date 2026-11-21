#ifndef DTCM_OVERLAY_UI_H
#define DTCM_OVERLAY_UI_H

#include <X11/Intrinsic.h>
#include "calendar.h"

void dtcm_overlay_ui_init(Calendar *c);
void dtcm_overlay_show_dialog(Calendar *c);
void dtcm_overlay_sidebar_toggle(Calendar *c, Boolean show);
Boolean dtcm_overlay_sidebar_is_visible(void);
Widget dtcm_overlay_sidebar_widget(void);
void dtcm_overlay_refresh_async(Calendar *c);

#endif /* DTCM_OVERLAY_UI_H */
