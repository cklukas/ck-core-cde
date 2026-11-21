#ifndef DTMAIL_QUICK_FILTER_H
#define DTMAIL_QUICK_FILTER_H

#include "MailThreadRules.h"
#include <Xm/Xm.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DtMailQuickFilterCB)(const char *query,
                                    DtMailFilterMode mode,
                                    void *userdata);

typedef struct DtMailQuickFilterBar DtMailQuickFilterBar;

DtMailQuickFilterBar *dtmail_quick_filter_create(
    Widget parent_form,
    Widget below_widget,
    DtMailQuickFilterCB on_change,
    void *userdata);

void dtmail_quick_filter_destroy(DtMailQuickFilterBar *bar);
void dtmail_quick_filter_set_visible(DtMailQuickFilterBar *bar, int visible);
int  dtmail_quick_filter_is_visible(const DtMailQuickFilterBar *bar);
void dtmail_quick_filter_focus(DtMailQuickFilterBar *bar);
void dtmail_quick_filter_clear(DtMailQuickFilterBar *bar);
void dtmail_quick_filter_set_status(DtMailQuickFilterBar *bar, const char *text);
Widget dtmail_quick_filter_widget(const DtMailQuickFilterBar *bar);

/* Mail Rules editor dialog (~/.dt/dtmail_rules, mode 0600) */
void dtmail_rules_dialog_open(Widget parent);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_QUICK_FILTER_H */
