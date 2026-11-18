/*
 * XdgTrashSync.h - Live Bidirectional XDG Trash (inotify) & Per-Mount .Trash-$UID
 *                  Integration for CK-Core CDE File Manager (WP-16).
 */

#ifndef _Dtfile_XdgTrashSync_h
#define _Dtfile_XdgTrashSync_h

#include <X11/Intrinsic.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callbacks implemented in Trash.c (preserving static trashCan encapsulation) */
int DtfileTrashBackendCount(void);
Boolean DtfileTrashBackendGet(
    int index,
    const char **int_new_out,
    const char **int_orig_out,
    const char **filename_out);
void DtfileTrashBackendUpdatePayload(int index, const char *new_payload_path);
Boolean DtfileTrashBackendAddExternal(
    const char *payload_path,
    const char *orig_path,
    const char *dt_leaf,
    const char *deletion_date);
void DtfileTrashBackendRemoveAt(int index);
void DtfileTrashBackendCommit(Boolean refresh_view);
void DtfileTrashBackendMovePaths(const char *const *paths, int count);

/* Format an RFC-3986 percent-encoded [Trash Info] record */
Boolean DtfileFormatXdgTrashInfoContent(
    const char *encoded_or_raw_path,
    const char *deletion_date,
    char *buf_out,
    size_t buf_sz);

/* Initialize live inotify watches on $XDG_DATA_HOME/Trash, ~/.dt/Trash, and
 * active external mount .Trash-$UID directories, and perform initial sync. */
void DtfileXdgTrashSyncInit(Widget toplevel_widget, const char *dt_trash_dir);

/* Refresh inotify watches when removable/GVFS mounts change and reconcile. */
void DtfileXdgTrashSyncRefreshMounts(void);

/* Reconcile dtfile Trash state with XDG Home Trash and per-mount .Trash-$UID. */
Boolean DtfileXdgTrashReconcileNow(void);

/* Move an item being trashed into same-filesystem XDG Trash ($XDG_DATA_HOME/Trash
 * or <mount_root>/.Trash-$UID) with atomic .trashinfo + ~/.dt/Trash proxy symlink.
 * Returns True on same-device XDG move, or False to fall back to FileManip. */
Boolean DtfileXdgTrashMoveItem(
    const char *src_path,
    const char *base_name,
    const char *dt_trash_dir,
    char **out_payload_path);

#ifdef __cplusplus
}
#endif

#endif /* _Dtfile_XdgTrashSync_h */
