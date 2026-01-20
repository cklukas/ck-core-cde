/*
 * Mounts dialog for dtfile.
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

typedef struct
{
   char *mount_point;
   char *fs_type;
   char *device;
   unsigned long long total_bytes;
   unsigned long long free_bytes;
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
   FileMgrRec *file_mgr_rec;
   MountEntry *entries;
   int entry_count;
} MountsDialog;

static MountsDialog *mounts_dialog = NULL;

static int
MountsGetSelectedIndex(MountsDialog *dlg)
{
   int *positions = NULL;
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
   if (fs_type == NULL || *fs_type == '\0')
      return False;

   if (strcmp(fs_type, "nfs") == 0 ||
       strcmp(fs_type, "nfs4") == 0 ||
       strcmp(fs_type, "smbfs") == 0 ||
       strcmp(fs_type, "cifs") == 0 ||
       strcmp(fs_type, "sshfs") == 0 ||
       strcmp(fs_type, "fuse.sshfs") == 0)
      return False;

   if (strcmp(fs_type, "proc") == 0 ||
       strcmp(fs_type, "sysfs") == 0 ||
       strcmp(fs_type, "devtmpfs") == 0 ||
       strcmp(fs_type, "devpts") == 0 ||
       strcmp(fs_type, "tmpfs") == 0 ||
       strcmp(fs_type, "cgroup") == 0 ||
       strcmp(fs_type, "cgroup2") == 0 ||
       strcmp(fs_type, "overlay") == 0)
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
         XtFree(dlg->entries[i].mount_point);
         XtFree(dlg->entries[i].fs_type);
         XtFree(dlg->entries[i].device);
      }
      XtFree((char *)dlg->entries);
      dlg->entries = NULL;
      dlg->entry_count = 0;
   }
}

static void
FormatSize(unsigned long long bytes, char *buf, size_t len)
{
   const char *unit = "B";
   double value = (double)bytes;

   if (value >= 1024.0)
   {
      value /= 1024.0;
      unit = "KB";
   }
   if (value >= 1024.0)
   {
      value /= 1024.0;
      unit = "MB";
   }
   if (value >= 1024.0)
   {
      value /= 1024.0;
      unit = "GB";
   }
   if (value >= 1024.0)
   {
      value /= 1024.0;
      unit = "TB";
   }

   snprintf(buf, len, "%.2f %s", value, unit);
}

static void
MountsUpdateDetails(MountsDialog *dlg, int index)
{
   Arg args[2];
   XmString label;
   char buf[128];

   if (index < 0 || index >= dlg->entry_count)
      return;

   snprintf(buf, sizeof(buf), "%s", dlg->entries[index].fs_type);
   label = XmStringCreateLocalized(buf);
   XtSetArg(args[0], XmNlabelString, label);
   XtSetValues(dlg->fs_label, args, 1);
   XmStringFree(label);

   snprintf(buf, sizeof(buf), "%s", dlg->entries[index].device);
   label = XmStringCreateLocalized(buf);
   XtSetArg(args[0], XmNlabelString, label);
   XtSetValues(dlg->dev_label, args, 1);
   XmStringFree(label);

   FormatSize(dlg->entries[index].total_bytes, buf, sizeof(buf));
   label = XmStringCreateLocalized(buf);
   XtSetArg(args[0], XmNlabelString, label);
   XtSetValues(dlg->size_label, args, 1);
   XmStringFree(label);

   FormatSize(dlg->entries[index].free_bytes, buf, sizeof(buf));
   label = XmStringCreateLocalized(buf);
   XtSetArg(args[0], XmNlabelString, label);
   XtSetValues(dlg->free_label, args, 1);
   XmStringFree(label);

   XmToggleButtonGadgetSetState(dlg->ignore_toggle,
      ShelfIsIgnoredMount(dlg->entries[index].mount_point), False);

   XtSetSensitive(dlg->open_btn, True);
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
      ShelfIgnoreMount(dlg->entries[index].mount_point);
   else
      ShelfUnignoreMount(dlg->entries[index].mount_point);

   ShelfRequestRebuild(dlg->dialog);
}

static void
MountsOpenCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   MountsDialog *dlg = (MountsDialog *) client_data;
   int index;
   DialogData *dialog_data;
   FileMgrData *file_mgr_data;

   (void) w;
   (void) call_data;

   index = MountsGetSelectedIndex(dlg);
   if (index < 0)
      return;

   dialog_data = _DtGetInstanceData ((XtPointer)dlg->file_mgr_rec);
   if (dialog_data == NULL)
      return;

   file_mgr_data = (FileMgrData *) dialog_data->data;
   ShowNewDirectory(file_mgr_data, file_mgr_data->host,
                    dlg->entries[index].mount_point);
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
MountsRefresh(MountsDialog *dlg)
{
   FILE *fp;
   struct mntent *ent;
   XmStringTable items;
   int count = 0;
   int capacity = 0;
   struct statvfs vfs;

   FreeEntries(dlg);

   fp = setmntent("/proc/mounts", "r");
   if (fp == NULL)
      fp = setmntent("/etc/mtab", "r");
   if (fp == NULL)
      return;

   while ((ent = getmntent(fp)) != NULL)
   {
      MountEntry entry;

      if (!IsLocalFsType(ent->mnt_type))
         continue;

      if (statvfs(ent->mnt_dir, &vfs) != 0)
      {
         entry.total_bytes = 0;
         entry.free_bytes = 0;
      }
      else
      {
         entry.total_bytes =
            (unsigned long long)vfs.f_blocks * vfs.f_frsize;
         entry.free_bytes =
            (unsigned long long)vfs.f_bfree * vfs.f_frsize;
      }

      entry.mount_point = XtNewString(ent->mnt_dir);
      entry.fs_type = XtNewString(ent->mnt_type);
      entry.device = XtNewString(ent->mnt_fsname);

      if (count == capacity)
      {
         capacity = capacity ? capacity * 2 : 8;
         dlg->entries = (MountEntry *)XtRealloc((char *)dlg->entries,
                                                sizeof(MountEntry) * capacity);
      }
      dlg->entries[count++] = entry;
   }

   endmntent(fp);

   dlg->entry_count = count;

   items = (XmStringTable)XtMalloc(sizeof(XmString) * count);
   for (int i = 0; i < count; i++)
      items[i] = XmStringCreateLocalized(dlg->entries[i].mount_point);

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
      XtSetSensitive(dlg->open_btn, False);
}

static MountsDialog *
CreateMountsDialog(Widget parent, FileMgrRec *file_mgr_rec)
{
   MountsDialog *dlg;
   Widget form;
   Widget list;
   Widget sep;
   Widget label;
   Widget fs_label;
   Widget dev_label;
   Widget size_label;
   Widget free_label;
   Widget ignore_toggle;
   Widget open_btn;
   Widget close_btn;
   Arg args[8];
   int n;
   XmString str;

   dlg = (MountsDialog *)XtCalloc(1, sizeof(MountsDialog));
   dlg->file_mgr_rec = file_mgr_rec;

   dlg->dialog = XmCreateFormDialog(parent, "mountsDialog", NULL, 0);
   XtVaSetValues(XtParent(dlg->dialog),
                 XmNtitle, GETMESSAGE(41, 1, "Mounts"),
                 NULL);
   XtVaSetValues(dlg->dialog,
                 XmNmarginWidth, 10,
                 XmNmarginHeight, 10,
                 NULL);

   form = dlg->dialog;

   n = 0;
   XtSetArg(args[n], XmNvisibleItemCount, 8); n++;
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

   sep = XmCreateSeparatorGadget(form, "sep", NULL, 0);
   XtVaSetValues(sep,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, XtParent(list),
                 XmNleftAttachment, XmATTACH_FORM,
                 XmNrightAttachment, XmATTACH_FORM,
                 XmNleftOffset, 10,
                 XmNrightOffset, 10,
                 NULL);
   XtManageChild(sep);

   str = XmStringCreateLocalized(GETMESSAGE(41, 2, "File system:"));
   label = XmCreateLabelGadget(form, "fsLabel", NULL, 0);
   XtVaSetValues(label,
                 XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, sep,
                 XmNleftAttachment, XmATTACH_FORM,
                 XmNleftOffset, 10,
                 NULL);
   XmStringFree(str);
   XtManageChild(label);

   fs_label = XmCreateLabelGadget(form, "fsValue", NULL, 0);
   XtVaSetValues(fs_label,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, sep,
                 XmNleftAttachment, XmATTACH_WIDGET,
                 XmNleftWidget, label,
                 XmNleftOffset, 6,
                 NULL);
   XtManageChild(fs_label);
   dlg->fs_label = fs_label;

   str = XmStringCreateLocalized(GETMESSAGE(41, 3, "Device:"));
   label = XmCreateLabelGadget(form, "devLabel", NULL, 0);
   XtVaSetValues(label,
                 XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, fs_label,
                 XmNleftAttachment, XmATTACH_FORM,
                 XmNleftOffset, 10,
                 NULL);
   XmStringFree(str);
   XtManageChild(label);

   dev_label = XmCreateLabelGadget(form, "devValue", NULL, 0);
   XtVaSetValues(dev_label,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, fs_label,
                 XmNleftAttachment, XmATTACH_WIDGET,
                 XmNleftWidget, label,
                 XmNleftOffset, 6,
                 NULL);
   XtManageChild(dev_label);
   dlg->dev_label = dev_label;

   str = XmStringCreateLocalized(GETMESSAGE(41, 4, "Size:"));
   label = XmCreateLabelGadget(form, "sizeLabel", NULL, 0);
   XtVaSetValues(label,
                 XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, dev_label,
                 XmNleftAttachment, XmATTACH_FORM,
                 XmNleftOffset, 10,
                 NULL);
   XmStringFree(str);
   XtManageChild(label);

   size_label = XmCreateLabelGadget(form, "sizeValue", NULL, 0);
   XtVaSetValues(size_label,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, dev_label,
                 XmNleftAttachment, XmATTACH_WIDGET,
                 XmNleftWidget, label,
                 XmNleftOffset, 6,
                 NULL);
   XtManageChild(size_label);
   dlg->size_label = size_label;

   str = XmStringCreateLocalized(GETMESSAGE(41, 5, "Free:"));
   label = XmCreateLabelGadget(form, "freeLabel", NULL, 0);
   XtVaSetValues(label,
                 XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, size_label,
                 XmNleftAttachment, XmATTACH_FORM,
                 XmNleftOffset, 10,
                 NULL);
   XmStringFree(str);
   XtManageChild(label);

   free_label = XmCreateLabelGadget(form, "freeValue", NULL, 0);
   XtVaSetValues(free_label,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, size_label,
                 XmNleftAttachment, XmATTACH_WIDGET,
                 XmNleftWidget, label,
                 XmNleftOffset, 6,
                 NULL);
   XtManageChild(free_label);
   dlg->free_label = free_label;

   str = XmStringCreateLocalized(GETMESSAGE(41, 6, "Don't show this mountpoint on Shelf"));
   ignore_toggle = XmCreateToggleButtonGadget(form, "ignoreToggle", NULL, 0);
   XtVaSetValues(ignore_toggle,
                 XmNlabelString, str,
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, free_label,
                 XmNleftAttachment, XmATTACH_FORM,
                 XmNleftOffset, 10,
                 XmNrightAttachment, XmATTACH_FORM,
                 XmNrightOffset, 10,
                 NULL);
   XmStringFree(str);
   XtManageChild(ignore_toggle);
   dlg->ignore_toggle = ignore_toggle;
   XtAddCallback(ignore_toggle, XmNvalueChangedCallback, MountsIgnoreToggleCB, dlg);

   Widget btn_sep = XmCreateSeparatorGadget(form, "btnSep", NULL, 0);

   XtVaSetValues(btn_sep,
               XmNtopAttachment,   XmATTACH_WIDGET,
               XmNtopWidget,       ignore_toggle,
               XmNtopOffset,       10,
               XmNleftAttachment,  XmATTACH_FORM,
               XmNleftOffset,      0,
               XmNrightAttachment, XmATTACH_FORM,
               XmNrightOffset,     0,
               XmNseparatorType,   XmSHADOW_ETCHED_IN,
               NULL);

   XtManageChild(btn_sep);

   open_btn = XmCreatePushButtonGadget(form, "openBtn", NULL, 0);
   XtVaSetValues(open_btn,
                 XmNlabelString, XmStringCreateLocalized(GETMESSAGE(41, 7, "Open")),
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, btn_sep,
                 XmNtopOffset, 10,
                 XmNleftAttachment, XmATTACH_FORM,
                 XmNleftOffset, 10,
                 XmNbottomAttachment, XmATTACH_FORM,
                 XmNbottomOffset, 10,
                 NULL);
   XtManageChild(open_btn);
   dlg->open_btn = open_btn;
   XtAddCallback(open_btn, XmNactivateCallback, MountsOpenCB, dlg);

   close_btn = XmCreatePushButtonGadget(form, "closeBtn", NULL, 0);
   XtVaSetValues(close_btn,
                 XmNlabelString, XmStringCreateLocalized(GETMESSAGE(41, 8, "Close")),
                 XmNtopAttachment, XmATTACH_WIDGET,
                 XmNtopWidget, btn_sep,
                 XmNtopOffset, 10,
                 XmNleftAttachment, XmATTACH_WIDGET,
                 XmNleftWidget, open_btn,
                 XmNleftOffset, 10,
                 XmNbottomAttachment, XmATTACH_FORM,
                 XmNbottomOffset, 10,
                 NULL);
   XtManageChild(close_btn);
   XtAddCallback(close_btn, XmNactivateCallback, MountsCloseCB, dlg);

   /* Make Open the default button for the dialog */
   XtVaSetValues(form,
               XmNdefaultButton, open_btn,
               NULL);

   /* Give it the default-button look */
   XtVaSetValues(open_btn,
               XmNshowAsDefault, 1,   /* thickness in pixels */
               NULL);

   /* allow Return to activate it even when focus is elsewhere */
   XtVaSetValues(open_btn,
               XmNdefaultButtonShadowThickness, 1,
               NULL);

   /* Make Close the cancel button for the dialog */
   XtVaSetValues(form,
              XmNcancelButton, close_btn,
              NULL);

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

   if (w != NULL)
   {
      mbar = XmGetPostedFromWidget(XtParent(w));
      XtSetArg(args[0], XmNuserData, &file_mgr_rec);
      XtGetValues(mbar, args, 1);
   }
   else
   {
      file_mgr_rec = (FileMgrRec *) client_data;
   }

   if (file_mgr_rec == NULL)
      return;

   if (mounts_dialog == NULL || mounts_dialog->dialog == NULL)
      mounts_dialog = CreateMountsDialog(file_mgr_rec->shell, file_mgr_rec);

   mounts_dialog->file_mgr_rec = file_mgr_rec;
   MountsRefresh(mounts_dialog);
   XtManageChild(mounts_dialog->dialog);
   /* enforce initial focus of dialog, focus on list */
   XmProcessTraversal(mounts_dialog->list, XmTRAVERSE_CURRENT);
}
