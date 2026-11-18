/*
 * ClipboardUndo.h - Standard File Clipboard (Ctrl+C/X/V: x-special/gnome-copied-files
 *                   & text/uri-list) and Bounded Session Undo Journal (Ctrl+Z)
 *                   for CK-Core CDE File Manager (WP-17).
 */

#ifndef _Dtfile_ClipboardUndo_h
#define _Dtfile_ClipboardUndo_h

#include <X11/Intrinsic.h>
#include <stddef.h>
#ifndef DTFILE_CLIPBOARD_UNIT_TEST
#include <Xm/Xm.h>
#include "Encaps.h"
#include "FileMgr.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DTFILE_UNDO_NONE = 0,
    DTFILE_UNDO_RENAME = 1,
    DTFILE_UNDO_MOVE = 2,
    DTFILE_UNDO_COPY = 3,
    DTFILE_UNDO_TRASH = 4
} DtfileUndoKind;

/* URI & clipboard payload helpers (also exposed for unit tests) */
char *DtfilePathToFileUri(const char *abs_path);
char *DtfileFileUriToPath(const char *uri);
Boolean DtfileParseClipboardPayload(
    const char *mime_type,
    const char *data,
    size_t data_len,
    Boolean *out_is_cut,
    char ***out_paths,
    int *out_count);
void DtfileFreeStringArray(char **arr, int count);

#ifndef DTFILE_CLIPBOARD_UNIT_TEST
/* Lifecycle & hooks */
void DtfileClipboardUndoInit(Widget toplevel_widget);

/* Clipboard operations on active FileMgrData */
Boolean DtfileClipboardCopyOrCutSelection(
    FileMgrData *fmd,
    Boolean is_cut,
    Time event_time);
void DtfileClipboardPasteIntoView(
    FileMgrData *fmd,
    Time event_time);
Boolean DtfileClipboardHasLocalOwnership(Boolean *out_is_cut, int *out_count);

/* Undo journal recording & execution */
void DtfileUndoRecordRename(const char *old_path, const char *new_path);
void DtfileUndoRecordMoveBatch(const char *const *src_paths,
                               const char *const *dst_paths,
                               int count);
void DtfileUndoRecordCopyBatch(const char *const *dst_paths, int count);
void DtfileUndoRecordTrashBatch(const char *const *orig_paths, int count);

int DtfileUndoDepth(void);
const char *DtfileUndoTopSummary(void);
Boolean DtfileUndoExecuteLast(FileMgrData *active_fmd);

/* Menu callbacks */
void DtfileCopyMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void DtfileCutMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void DtfilePasteMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void DtfileUndoMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
#endif /* !DTFILE_CLIPBOARD_UNIT_TEST */

#ifdef __cplusplus
}
#endif

#endif /* _Dtfile_ClipboardUndo_h */
