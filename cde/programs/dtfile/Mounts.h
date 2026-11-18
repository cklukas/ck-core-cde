/*
 * Mounts & Devices dialog and shared navigable-mount discovery for dtfile.
 */
#ifndef _DtMounts_h
#define _DtMounts_h

#include <stddef.h>
#include <X11/Intrinsic.h>

typedef struct
{
   char *shelf_token;
   char *list_label;
   char *mount_point;
   char *fs_type;
   char *device;
   unsigned long long total_bytes;
   unsigned long long free_bytes;
   Boolean is_managed_dev;
   Boolean is_mounted;
   Boolean is_luks_locked;
   Boolean can_mount;
   Boolean can_unmount;
   Boolean can_eject;
} DtfileMountNavEntry;

extern int DtfileCollectNavigableMounts(DtfileMountNavEntry **entries_out);
extern void DtfileFreeNavigableMounts(DtfileMountNavEntry *entries, int count);
extern void DtfileFormatMountBytes(unsigned long long bytes, char *buf, size_t len);

extern void ShowMountsDialog(
        Widget w,
        XtPointer client_data,
        XtPointer call_data );

#endif /* _DtMounts_h */
