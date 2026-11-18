/*
 * Mounts & Devices dialog for dtfile (local mounts, UDisks2 removable/LUKS, and GVFS shares).
 */
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>
#include <Xm/ToggleBG.h>

#include <mntent.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#include "Encaps.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "Mounts.h"
#include "Shelf.h"
#include "Devices.h"

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
} MountEntry;

typedef struct
{
   Widget dialog;
   Widget list;
   Widget fs_label;
   Widget dev_label;
   Widget size_label;
   Widget free_label;
   Widget ignore_toggle;
   Widget open_btn;
   Widget mount_btn;
   Widget unmount_btn;
   Widget eject_btn;
   Widget connect_btn;
   FileMgrRec *file_mgr_rec;
   MountEntry *entries;
   int entry_count;
} MountsDialog;

static MountsDialog *mounts_dialog = nullptr;
static void MountsRefresh(MountsDialog *dlg);

static int
MountsGetSelectedIndex(MountsDialog *dlg)
{
   int *positions = nullptr;
   int count = 0;
   int index = -1;

   if (!XmListGetSelectedPos(dlg->list, &positions, &count))
      return -1;

   if (count > 0)
      index = positions[0] - 1;

   XtFree((char *)positions);

   if (index < 0 || index >= dlg->entry_count)
      return -1;

   return index;
}

static Boolean
IsLocalFsType(const char *fs_type)
{
   if (fs_type == nullptr || *fs_type == '\0')
      return False;

   if (strcmp(fs_type, "nfs") == 0 ||
       strcmp(fs_type, "nfs4") == 0 ||
       strcmp(fs_type, "smbfs") == 0 ||
       strcmp(fs_type, "cifs") == 0 ||
       strcmp(fs_type, "sshfs") == 0 ||
       strcmp(fs_type, "fuse.sshfs") == 0 ||
       strcmp(fs_type, "fuse.gvfsd-fuse") == 0)
      return False;

   if (strcmp(fs_type, "proc") == 0 ||
       strcmp(fs_type, "sysfs") == 0 ||
       strcmp(fs_type, "devtmpfs") == 0 ||
       strcmp(fs_type, "devpts") == 0 ||
       strcmp(fs_type, "tmpfs") == 0 ||
       strcmp(fs_type, "ramfs") == 0 ||
       strcmp(fs_type, "cgroup") == 0 ||
       strcmp(fs_type, "cgroup2") == 0 ||
       strcmp(fs_type, "overlay") == 0 ||
       strcmp(fs_type, "portal") == 0 ||
       strcmp(fs_type, "fuse.portal") == 0 ||
       strcmp(fs_type, "autofs") == 0 ||
       strcmp(fs_type, "binfmt_misc") == 0 ||
       strcmp(fs_type, "mqueue") == 0 ||
       strcmp(fs_type, "hugetlbfs") == 0 ||
       strcmp(fs_type, "securityfs") == 0 ||
       strcmp(fs_type, "debugfs") == 0 ||
       strcmp(fs_type, "tracefs") == 0 ||
       strcmp(fs_type, "configfs") == 0 ||
       strcmp(fs_type, "fusectl") == 0 ||
       strcmp(fs_type, "pstore") == 0 ||
       strcmp(fs_type, "efivarfs") == 0 ||
       strcmp(fs_type, "bpf") == 0 ||
       strcmp(fs_type, "nsfs") == 0 ||
       strcmp(fs_type, "rpc_pipefs") == 0 ||
       strcmp(fs_type, "squashfs") == 0)
      return False;

   return True;
}

static void
FreeEntries(MountsDialog *dlg)
{
   int i;

   if (dlg->entries)
   {
      for (i = 0; i < dlg->entry_count; i++)
      {
         XtFree(dlg->entries[i].shelf_token);
         XtFree(dlg->entries[i].list_label);
         XtFree(dlg->entries[i].mount_point);
         XtFree(dlg->entries[i].fs_type);
         XtFree(dlg->entries[i].device);
      }
      XtFree((char *)dlg->entries);
      dlg->entries = nullptr;
      dlg->entry_count = 0;
   }
}

static void
FormatSize(unsigned long long bytes, char *buf, size_t len)
{
   const char *unit = "B";
   double value = (double)bytes;

   if (value >= 1024.0) { value /= 1024.0; unit = "KB"; }
   if (value >= 1024.0) { value /= 1024.0; unit = "MB"; }
   if (value >= 1024.0) { value /= 1024.0; unit = "GB"; }
   if (value >= 1024.0) { value /= 1024.0; unit = "TB"; }

   snprintf(buf, len, "%.2f %s", value, unit);
}

static void
MountsUpdateDetails(MountsDialog *dlg, int index)
{
   Arg args[2];
   XmString label;
   char buf[192];

   if (index < 0 || index >= dlg->entry_count)
      return;

   MountEntry *e = &dlg->entries[index];

   snprintf(buf, sizeof(buf), "%s", e->fs_type);
   label = XmStringCreateLocalized(buf);
   XtSetArg(args[0], XmNlabelString, label);
   XtSetValues(dlg->fs_label, args, 1);
   XmStringFree(label);

   snprintf(buf, sizeof(buf), "%s", e->device);
   label = XmStringCreateLocalized(buf);
   XtSetArg(args[0], XmNlabelString, label);
   XtSetValues(dlg->dev_label, args, 1);
   XmStringFree(label);

   if (e->total_bytes > 0)
      FormatSize(e->total_bytes, buf, sizeof(buf));
   else
      snprintf(buf, sizeof(buf), "N/A");
   label = XmStringCreateLocalized(buf);
   XtSetArg(args[0], XmNlabelString, label);
   XtSetValues(dlg->size_label, args, 1);
   XmStringFree(label);

   if (e->is_mounted && e->total_bytes > 0)
      FormatSize(e->free_bytes, buf, sizeof(buf));
   else if (!e->is_mounted)
      snprintf(buf, sizeof(buf), e->is_luks_locked ? "Locked (LUKS)" : "Unmounted");
   else
      snprintf(buf, sizeof(buf), "N/A");
   label = XmStringCreateLocalized(buf);
   XtSetArg(args[0], XmNlabelString, label);
   XtSetValues(dlg->free_label, args, 1);
   XmStringFree(label);

   XmToggleButtonGadgetSetState(dlg->ignore_toggle,
      ShelfIsIgnoredMount(e->shelf_token), False);

   XtSetSensitive(dlg->open_btn, e->is_mounted);
   XtSetSensitive(dlg->mount_btn, e->can_mount);
   XtSetSensitive(dlg->unmount_btn, e->can_unmount);
   XtSetSensitive(dlg->eject_btn, e->can_eject);

   XmString mstr = XmStringCreateLocalized(e->is_luks_locked ? "Unlock..." : "Mount");
   XtSetArg(args[0], XmNlabelString, mstr);
   XtSetValues(dlg->mount_btn, args, 1);
   XmStringFree(mstr);
}

static void
MountsListCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   MountsDialog *dlg = (MountsDialog *) client_data;
   XmListCallbackStruct *cb = (XmListCallbackStruct *) call_data;
   int index = cb->item_position - 1;

   (void) w;
   MountsUpdateDetails(dlg, index);
}

static void
MountsIgnoreToggleCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   MountsDialog *dlg = (MountsDialog *) client_data;
   XmToggleButtonCallbackStruct *cb = (XmToggleButtonCallbackStruct *) call_data;
   int index;

   (void) w;

   index = MountsGetSelectedIndex(dlg);
   if (index < 0)
      return;

   if (cb->set)
      ShelfIgnoreMount(dlg->entries[index].shelf_token);
   else
      ShelfUnignoreMount(dlg->entries[index].shelf_token);

   DtfileDevicesRescan();
   ShelfRequestRebuild(dlg->dialog);
}

static FileMgrData *
MountsGetFileMgrData(MountsDialog *dlg)
{
   if (!dlg || !dlg->file_mgr_rec)
      return nullptr;
   DialogData *dd = _DtGetInstanceData((XtPointer)dlg->file_mgr_rec);
   return dd ? (FileMgrData *)dd->data : nullptr;
}

static void
MountsOpenCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   MountsDialog *dlg = (MountsDialog *) client_data;
   int index = MountsGetSelectedIndex(dlg);
   FileMgrData *file_mgr_data = MountsGetFileMgrData(dlg);

   (void) w;
   (void) call_data;

   if (index < 0 || !file_mgr_data)
      return;

   if (!dlg->entries[index].is_mounted)
   {
      DtfileDevicesActivatePath(dlg->file_mgr_rec, file_mgr_data,
                                dlg->entries[index].shelf_token);
      return;
   }

   ShowNewDirectory(file_mgr_data, file_mgr_data->host,
                    dlg->entries[index].mount_point);
}

static void
MountsMountBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   MountsDialog *dlg = (MountsDialog *)client_data;
   int index = MountsGetSelectedIndex(dlg);
   FileMgrData *fmd = MountsGetFileMgrData(dlg);

   (void)w;
   (void)call_data;

   if (index < 0)
      return;
   DtfileDevicesActivatePath(dlg->file_mgr_rec, fmd, dlg->entries[index].shelf_token);
}

static void
MountsUnmountBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   MountsDialog *dlg = (MountsDialog *)client_data;
   int index = MountsGetSelectedIndex(dlg);
   FileMgrData *fmd = MountsGetFileMgrData(dlg);

   (void)w;
   (void)call_data;

   if (index < 0)
      return;
   DtfileDevicesUnmountOrEject(dlg->dialog, fmd, dlg->entries[index].shelf_token, False);
   MountsRefresh(dlg);
}

static void
MountsEjectBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   MountsDialog *dlg = (MountsDialog *)client_data;
   int index = MountsGetSelectedIndex(dlg);
   FileMgrData *fmd = MountsGetFileMgrData(dlg);

   (void)w;
   (void)call_data;

   if (index < 0)
      return;
   DtfileDevicesUnmountOrEject(dlg->dialog, fmd, dlg->entries[index].shelf_token, True);
   MountsRefresh(dlg);
}

static void
MountsConnectBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   (void)w;
   (void)client_data;
   (void)call_data;
   ShowConnectServerDialog(nullptr, nullptr, nullptr);
}

static void
MountsCloseCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   MountsDialog *dlg = (MountsDialog *) client_data;

   (void) w;
   (void) call_data;
   XtUnmanageChild(dlg->dialog);
}

static void
AppendMountEntry(MountsDialog *dlg, int *count, int *capacity, MountEntry *entry)
{
   if (*count == *capacity)
   {
      *capacity = *capacity ? (*capacity * 2) : 12;
      dlg->entries = (MountEntry *)XtRealloc((char *)dlg->entries,
                                             sizeof(MountEntry) * (*capacity));
   }
   dlg->entries[(*count)++] = *entry;
}

static void
MountsRefresh(MountsDialog *dlg)
{
   FILE *fp;
   struct mntent *ent;
   XmStringTable items;
   int count = 0;
   int capacity = 0;
   struct statvfs vfs;

   FreeEntries(dlg);
   DtfileDevicesRescan();

   /* 1. Managed UDisks2 removable/LUKS volumes & GVFS network/MTP mounts */
   int dev_count = DtfileDevicesGetCount();
   for (int d = 0; d < dev_count; ++d)
   {
      DtfileDeviceEntry de;
      if (!DtfileDevicesGetEntry(d, &de))
         continue;

      MountEntry entry;
      memset(&entry, 0, sizeof(entry));
      entry.shelf_token = XtNewString(de.shelf_token);
      entry.mount_point = XtNewString(de.mount_point[0] ? de.mount_point : "(unmounted)");
      entry.fs_type = XtNewString(de.fs_type);
      entry.device = XtNewString(de.device_node);
      entry.is_managed_dev = True;
      entry.is_mounted = de.is_mounted;
      entry.is_luks_locked = de.is_luks_locked;
      entry.can_mount = de.can_mount;
      entry.can_unmount = de.can_unmount;
      entry.can_eject = de.can_eject;

      if (de.is_mounted && de.mount_point[0] == '/' && statvfs(de.mount_point, &vfs) == 0)
      {
         entry.total_bytes = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
         entry.free_bytes = (unsigned long long)vfs.f_bfree * vfs.f_frsize;
      }
      else
      {
         entry.total_bytes = de.size_bytes;
         entry.free_bytes = 0;
      }

      char line[MAX_PATH + 160];
      if (de.is_mounted)
         snprintf(line, sizeof(line), "%s  (%s)", de.display_label, de.mount_point);
      else
         snprintf(line, sizeof(line), "%s  (%s)", de.display_label, de.device_node);
      entry.list_label = XtNewString(line);

      AppendMountEntry(dlg, &count, &capacity, &entry);
   }

   /* 2. Local system mounts not already covered above */
   fp = setmntent("/proc/mounts", "r");
   if (fp == nullptr)
      fp = setmntent("/etc/mtab", "r");
   if (fp != nullptr)
   {
      while ((ent = getmntent(fp)) != nullptr)
      {
         Boolean dup = False;
         if (!IsLocalFsType(ent->mnt_type) ||
             strncmp(ent->mnt_dir, "/proc/", 6) == 0 ||
             strncmp(ent->mnt_dir, "/sys/", 5) == 0 ||
             strncmp(ent->mnt_dir, "/dev/", 5) == 0 ||
             strncmp(ent->mnt_dir, "/run/credentials/", 17) == 0 ||
             strncmp(ent->mnt_dir, "/snap/", 6) == 0)
            continue;
         for (int i = 0; i < count; ++i)
         {
            if (strcmp(dlg->entries[i].mount_point, ent->mnt_dir) == 0)
            {
               dup = True;
               break;
            }
         }
         if (dup)
            continue;

         MountEntry entry;
         memset(&entry, 0, sizeof(entry));
         if (statvfs(ent->mnt_dir, &vfs) == 0)
         {
            entry.total_bytes = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
            entry.free_bytes = (unsigned long long)vfs.f_bfree * vfs.f_frsize;
         }
         entry.shelf_token = XtNewString(ent->mnt_dir);
         entry.list_label = XtNewString(ent->mnt_dir);
         entry.mount_point = XtNewString(ent->mnt_dir);
         entry.fs_type = XtNewString(ent->mnt_type);
         entry.device = XtNewString(ent->mnt_fsname);
         entry.is_mounted = True;
         AppendMountEntry(dlg, &count, &capacity, &entry);
      }
      endmntent(fp);
   }

   dlg->entry_count = count;

   items = (XmStringTable)XtCalloc(count ? count : 1, sizeof(XmString));
   for (int i = 0; i < count; i++)
      items[i] = XmStringCreateLocalized(dlg->entries[i].list_label);

   XmListDeleteAllItems(dlg->list);
   if (count > 0)
   {
      XmListAddItems(dlg->list, items, count, 1);
      XmListSelectPos(dlg->list, 1, True);
      MountsUpdateDetails(dlg, 0);
   }

   for (int i = 0; i < count; i++)
      XmStringFree(items[i]);
   XtFree((char *)items);

   if (count == 0)
   {
      XtSetSensitive(dlg->open_btn, False);
      XtSetSensitive(dlg->mount_btn, False);
      XtSetSensitive(dlg->unmount_btn, False);
      XtSetSensitive(dlg->eject_btn, False);
   }
}

static MountsDialog *
CreateMountsDialog(Widget parent, FileMgrRec *file_mgr_rec)
{
   MountsDialog *dlg;
   Widget form, list, sep, label, fs_label, dev_label, size_label, free_label;
   Widget ignore_toggle, open_btn, mount_btn, unmount_btn, eject_btn, connect_btn, close_btn;
   Arg args[8];
   int n;
   XmString str;

   dlg = (MountsDialog *)XtCalloc(1, sizeof(MountsDialog));
   dlg->file_mgr_rec = file_mgr_rec;

   dlg->dialog = XmCreateFormDialog(parent, "mountsDialog", nullptr, 0);
   XtVaSetValues(XtParent(dlg->dialog),
                 XmNtitle, "Mounts & Devices",
                 nullptr);
   XtVaSetValues(dlg->dialog,
                 XmNmarginWidth, 10,
                 XmNmarginHeight, 10,
                 nullptr);

   form = dlg->dialog;

   n = 0;
   XtSetArg(args[n], XmNvisibleItemCount, 9); n++;
   XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
   XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
   XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
   XtSetArg(args[n], XmNtopOffset, 10); n++;
   XtSetArg(args[n], XmNleftOffset, 10); n++;
   XtSetArg(args[n], XmNrightOffset, 10); n++;
   list = XmCreateScrolledList(form, "mountList", args, n);
   XtManageChild(list);
   dlg->list = list;
   XtAddCallback(list, XmNsingleSelectionCallback, MountsListCB, dlg);
   XtAddCallback(list, XmNbrowseSelectionCallback, MountsListCB, dlg);

   sep = XmCreateSeparatorGadget(form, "sep", nullptr, 0);
   XtVaSetValues(sep,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, XtParent(list),
                 XmNleftAttachment, XmATTACH_FORM,
                 XmNrightAttachment, XmATTACH_FORM,
                 XmNleftOffset, 10,
                 XmNrightOffset, 10,
                 nullptr);
   XtManageChild(sep);

   str = XmStringCreateLocalized(GETMESSAGE(41, 2, "File system:"));
   label = XmCreateLabelGadget(form, "fsLabel", nullptr, 0);
   XtVaSetValues(label, XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, sep,
                 XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10, nullptr);
   XmStringFree(str);
   XtManageChild(label);

   fs_label = XmCreateLabelGadget(form, "fsValue", nullptr, 0);
   XtVaSetValues(fs_label,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, sep,
                 XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, label, XmNleftOffset, 6,
                 nullptr);
   XtManageChild(fs_label);
   dlg->fs_label = fs_label;

   str = XmStringCreateLocalized(GETMESSAGE(41, 3, "Device:"));
   label = XmCreateLabelGadget(form, "devLabel", nullptr, 0);
   XtVaSetValues(label, XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, fs_label,
                 XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10, nullptr);
   XmStringFree(str);
   XtManageChild(label);

   dev_label = XmCreateLabelGadget(form, "devValue", nullptr, 0);
   XtVaSetValues(dev_label,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, fs_label,
                 XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, label, XmNleftOffset, 6,
                 nullptr);
   XtManageChild(dev_label);
   dlg->dev_label = dev_label;

   str = XmStringCreateLocalized(GETMESSAGE(41, 4, "Size:"));
   label = XmCreateLabelGadget(form, "sizeLabel", nullptr, 0);
   XtVaSetValues(label, XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, dev_label,
                 XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10, nullptr);
   XmStringFree(str);
   XtManageChild(label);

   size_label = XmCreateLabelGadget(form, "sizeValue", nullptr, 0);
   XtVaSetValues(size_label,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, dev_label,
                 XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, label, XmNleftOffset, 6,
                 nullptr);
   XtManageChild(size_label);
   dlg->size_label = size_label;

   str = XmStringCreateLocalized(GETMESSAGE(41, 5, "Free / Status:"));
   label = XmCreateLabelGadget(form, "freeLabel", nullptr, 0);
   XtVaSetValues(label, XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, size_label,
                 XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10, nullptr);
   XmStringFree(str);
   XtManageChild(label);

   free_label = XmCreateLabelGadget(form, "freeValue", nullptr, 0);
   XtVaSetValues(free_label,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, size_label,
                 XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, label, XmNleftOffset, 6,
                 nullptr);
   XtManageChild(free_label);
   dlg->free_label = free_label;

   str = XmStringCreateLocalized(GETMESSAGE(41, 6, "Don't show this mountpoint on Shelf"));
   ignore_toggle = XmCreateToggleButtonGadget(form, "ignoreToggle", nullptr, 0);
   XtVaSetValues(ignore_toggle, XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, free_label,
                 XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10,
                 XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 10, nullptr);
   XmStringFree(str);
   XtManageChild(ignore_toggle);
   dlg->ignore_toggle = ignore_toggle;
   XtAddCallback(ignore_toggle, XmNvalueChangedCallback, MountsIgnoreToggleCB, dlg);

   Widget btn_sep = XmCreateSeparatorGadget(form, "btnSep", nullptr, 0);
   XtVaSetValues(btn_sep,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, ignore_toggle, XmNtopOffset, 10,
                 XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
                 XmNseparatorType, XmSHADOW_ETCHED_IN, nullptr);
   XtManageChild(btn_sep);

   XmString open_xm = XmStringCreateLocalized(GETMESSAGE(41, 7, "Open"));
   open_btn = XmCreatePushButtonGadget(form, "openBtn", nullptr, 0);
   XtVaSetValues(open_btn, XmNlabelString, open_xm,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, btn_sep, XmNtopOffset, 10,
                 XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10,
                 XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 10, nullptr);
   XmStringFree(open_xm);
   XtManageChild(open_btn);
   dlg->open_btn = open_btn;
   XtAddCallback(open_btn, XmNactivateCallback, MountsOpenCB, dlg);

   XmString mnt_xm = XmStringCreateLocalized("Mount");
   mount_btn = XmCreatePushButtonGadget(form, "mountBtn", nullptr, 0);
   XtVaSetValues(mount_btn, XmNlabelString, mnt_xm,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, btn_sep, XmNtopOffset, 10,
                 XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, open_btn, XmNleftOffset, 8,
                 XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 10, nullptr);
   XmStringFree(mnt_xm);
   XtManageChild(mount_btn);
   dlg->mount_btn = mount_btn;
   XtAddCallback(mount_btn, XmNactivateCallback, MountsMountBtnCB, dlg);

   XmString umnt_xm = XmStringCreateLocalized("Unmount");
   unmount_btn = XmCreatePushButtonGadget(form, "unmountBtn", nullptr, 0);
   XtVaSetValues(unmount_btn, XmNlabelString, umnt_xm,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, btn_sep, XmNtopOffset, 10,
                 XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, mount_btn, XmNleftOffset, 8,
                 XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 10, nullptr);
   XmStringFree(umnt_xm);
   XtManageChild(unmount_btn);
   dlg->unmount_btn = unmount_btn;
   XtAddCallback(unmount_btn, XmNactivateCallback, MountsUnmountBtnCB, dlg);

   XmString ej_xm = XmStringCreateLocalized("Eject");
   eject_btn = XmCreatePushButtonGadget(form, "ejectBtn", nullptr, 0);
   XtVaSetValues(eject_btn, XmNlabelString, ej_xm,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, btn_sep, XmNtopOffset, 10,
                 XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, unmount_btn, XmNleftOffset, 8,
                 XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 10, nullptr);
   XmStringFree(ej_xm);
   XtManageChild(eject_btn);
   dlg->eject_btn = eject_btn;
   XtAddCallback(eject_btn, XmNactivateCallback, MountsEjectBtnCB, dlg);

   XmString conn_xm = XmStringCreateLocalized("Connect Server...");
   connect_btn = XmCreatePushButtonGadget(form, "connectServerBtn", nullptr, 0);
   XtVaSetValues(connect_btn, XmNlabelString, conn_xm,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, btn_sep, XmNtopOffset, 10,
                 XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, eject_btn, XmNleftOffset, 8,
                 XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 10, nullptr);
   XmStringFree(conn_xm);
   XtManageChild(connect_btn);
   dlg->connect_btn = connect_btn;
   XtAddCallback(connect_btn, XmNactivateCallback, MountsConnectBtnCB, dlg);

   XmString cls_xm = XmStringCreateLocalized(GETMESSAGE(41, 8, "Close"));
   close_btn = XmCreatePushButtonGadget(form, "closeBtn", nullptr, 0);
   XtVaSetValues(close_btn, XmNlabelString, cls_xm,
                 XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, btn_sep, XmNtopOffset, 10,
                 XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, connect_btn, XmNleftOffset, 8,
                 XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 10,
                 XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 10, nullptr);
   XmStringFree(cls_xm);
   XtManageChild(close_btn);
   XtAddCallback(close_btn, XmNactivateCallback, MountsCloseCB, dlg);

   XtVaSetValues(form, XmNdefaultButton, open_btn, XmNcancelButton, close_btn, nullptr);
   XtVaSetValues(open_btn, XmNshowAsDefault, 1, XmNdefaultButtonShadowThickness, 1, nullptr);

   return dlg;
}

void
ShowMountsDialog(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   FileMgrRec *file_mgr_rec;
   Arg args[1];
   Widget mbar;

   (void) call_data;

   if (w != nullptr)
   {
      mbar = XmGetPostedFromWidget(XtParent(w));
      XtSetArg(args[0], XmNuserData, &file_mgr_rec);
      XtGetValues(mbar, args, 1);
   }
   else
   {
      file_mgr_rec = (FileMgrRec *) client_data;
   }

   if (file_mgr_rec == nullptr)
      return;

   if (mounts_dialog == nullptr || mounts_dialog->dialog == nullptr)
      mounts_dialog = CreateMountsDialog(file_mgr_rec->shell, file_mgr_rec);

   mounts_dialog->file_mgr_rec = file_mgr_rec;
   MountsRefresh(mounts_dialog);
   XtManageChild(mounts_dialog->dialog);
   XmProcessTraversal(mounts_dialog->list, XmTRAVERSE_CURRENT);
}
