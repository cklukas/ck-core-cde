/*
 * CK-Core CDE - Desktop File Manager "Open With..." XDG MIME Dialog (WP-12)
 */

#ifndef DTFILE_OPEN_WITH_H
#define DTFILE_OPEN_WITH_H

#include <X11/Intrinsic.h>
#include <stdbool.h>

void OpenWithMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void OpenWithPopupCB(Widget w, XtPointer client_data, XtPointer call_data);

/* Testable pure-C helpers for MIME detection & ~/.config/mimeapps.list persistence */
void DtfileDetectMimeType(const char *file_path, char *out_mime, size_t out_sz);
bool DtfileSetMimeAppsDefault(const char *mime_type, const char *desktop_id);

#endif /* DTFILE_OPEN_WITH_H */
