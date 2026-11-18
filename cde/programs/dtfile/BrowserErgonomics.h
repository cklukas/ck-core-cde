/*
 * BrowserErgonomics.h - Multi-Tab Browsing (Ctrl+T / Ctrl+W / Ctrl+Tab / Middle-Click),
 *                       Per-Tab Back/Forward History (Alt+Left / Alt+Right / Button8/9),
 *                       Inline Rename (F2), Hidden Toggle (Ctrl+H), and Cancellable
 *                       ck-core-fs-lib Streaming Search Bar (Ctrl+F) for dtfile (WP-17).
 */

#ifndef _Dtfile_BrowserErgonomics_h
#define _Dtfile_BrowserErgonomics_h

#include <X11/Intrinsic.h>
#include <Xm/Xm.h>
#include "Encaps.h"
#include "FileMgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Root command listener (active when DTFILE_ERGONOMICS_TEST is set) */
void DtfileErgonomicsInitRootCmd(Widget toplevel_widget);
Boolean DtfileErgonomicsFilterKeyEvent(XEvent *event);

/* Window & view lifecycle hooks */
void DtfileErgonomicsAttachWindow(FileMgrRec *rec, Boolean is_trash_view);
void DtfileErgonomicsBindView(FileMgrRec *rec, FileMgrData *fmd);
void DtfileErgonomicsUnbindView(FileMgrRec *rec, FileMgrData *fmd);
void DtfileErgonomicsSyncHeaderLayout(FileMgrRec *rec, FileMgrData *fmd);
void DtfileErgonomicsOnDirectoryShown(FileMgrData *fmd);
void DtfileErgonomicsOnIconsLaidOut(FileMgrData *fmd);
void DtfileErgonomicsPublishState(FileMgrData *fmd);

/* Tab & navigation operations */
Boolean DtfileTabOpen(FileMgrData *fmd,
                      const char *host,
                      const char *dir,
                      Boolean switch_to);
Boolean DtfileTabCloseActive(FileMgrData *fmd);
Boolean DtfileTabCycle(FileMgrData *fmd, int delta);
Boolean DtfileHistoryStep(FileMgrData *fmd, int delta);
Boolean DtfileGoParent(FileMgrData *fmd);
Boolean DtfileStartInlineRename(FileMgrData *fmd);
Boolean DtfileToggleHiddenFiles(FileMgrData *fmd);

/* Asynchronous in-window search operations */
void DtfileSearchOpenBar(FileMgrData *fmd);
void DtfileSearchCloseBar(FileMgrData *fmd);
Boolean DtfileSearchStartCustom(FileMgrData *fmd,
                                const char *name_query,
                                const char *content_query,
                                const char *min_size_str,
                                const char *max_size_str,
                                const char *after_date_str,
                                const char *before_date_str,
                                Boolean recursive,
                                Boolean case_sensitive);
void DtfileSearchCancelActive(FileMgrData *fmd);
Boolean DtfileSearchRevealHitIndex(FileMgrData *fmd, int hit_idx);

/* Menu & popup callbacks */
void DtfileNewTabMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void DtfileCloseTabMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void DtfileSearchWindowMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void DtfileOpenNewTabPopupCB(Widget w, XtPointer client_data, XtPointer call_data);

#ifdef __cplusplus
}
#endif

#endif /* _Dtfile_BrowserErgonomics_h */
