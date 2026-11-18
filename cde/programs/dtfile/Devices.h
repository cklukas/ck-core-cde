/*
 * Devices.h - UDisks2 Removable/LUKS Volumes & GVFS Network Shares / MTP
 *             Integration for CK-Core CDE File Manager (Wayland & X11, WP-16).
 */

#ifndef _Dtfile_Devices_h
#define _Dtfile_Devices_h

#include <limits.h>
#include <X11/Intrinsic.h>
#include "FileMgr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char shelf_token[PATH_MAX];     /* Mount path if mounted, else "udisks://<obj_path>" */
    char display_label[128];        /* Friendly Shelf / Mounts label */
    char fs_type[64];               /* e.g. "vfat", "ext4", "crypto_LUKS", "gvfs/sftp" */
    char device_node[256];          /* e.g. "/dev/sdb1" or GVFS URI stem */
    char block_obj_path[256];       /* /org/freedesktop/UDisks2/block_devices/... */
    char crypto_backing_path[256];  /* Backing LUKS object path if unlocked cleartext */
    char drive_obj_path[256];       /* /org/freedesktop/UDisks2/drives/... */
    char mount_point[PATH_MAX];     /* Empty if unmounted/locked */
    unsigned long long size_bytes;
    Boolean is_udisks;
    Boolean is_gvfs;
    Boolean is_mtp;
    Boolean is_luks_locked;
    Boolean is_mounted;
    Boolean can_mount;
    Boolean can_unmount;
    Boolean can_eject;
} DtfileDeviceEntry;

/* Initialize UDisks2 D-Bus ObjectManager watcher and GVFS FUSE directory inotify watcher. */
void DtfileDevicesInit(Widget toplevel_widget);
void DtfileDevicesShutdown(void);

/* Rescan UDisks2 + GVFS and synchronize dynamic Shelf slots across open views. */
void DtfileDevicesRescan(void);

/* Return True if `path` is a synthetic unmounted/locked UDisks2 token ("udisks://..."). */
Boolean DtfileDevicesIsSyntheticPath(const char *path);

/* Return True if `path` is a GVFS mount inside /run/user/$UID/gvfs (or DTFILE_GVFS_ROOT). */
Boolean DtfileDevicesIsGvfsPath(const char *path);

/* Query current discovered UDisks2 & GVFS entries. */
int DtfileDevicesGetCount(void);
Boolean DtfileDevicesGetEntry(int index, DtfileDeviceEntry *out_entry);
Boolean DtfileDevicesLookupByPath(const char *path, DtfileDeviceEntry *out_entry);

/* Format a custom Shelf label and fallback CDE icon name if `path` is a managed device/share. */
Boolean DtfileDevicesDescribeShelfSlot(
    const char *path,
    char **label_out,
    const char **icon_fallback_out);

/* Handle activating (clicking / opening) a managed device path:
 * - Locked LUKS -> prompts Motif passphrase -> Encrypted.Unlock -> Filesystem.Mount -> navigate
 * - Unmounted UDisks2 -> Filesystem.Mount -> navigate
 * - Mounted path -> returns False so normal directory navigation proceeds. */
Boolean DtfileDevicesActivatePath(
    FileMgrRec *file_mgr_rec,
    FileMgrData *file_mgr_data,
    const char *path);

/* Unmount (and optionally Lock LUKS + Eject/PowerOff drive) a UDisks2 or GVFS mount. */
void DtfileDevicesUnmountOrEject(
    Widget parent,
    FileMgrData *file_mgr_data,
    const char *path,
    Boolean eject_drive);

/* File -> Connect to Server... Motif dialog (smb://, sftp://, davs://, nfs://). */
void ShowConnectServerDialog(Widget w, XtPointer client_data, XtPointer call_data);

#ifdef __cplusplus
}
#endif

#endif /* _Dtfile_Devices_h */
