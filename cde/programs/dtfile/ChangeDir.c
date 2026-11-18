/*
 * CDE - Common Desktop Environment
 *
 * Copyright (c) 1993-2012, The Open Group. All rights reserved.
 *
 * These libraries and programs are free software; you can
 * redistribute them and/or modify them under the terms of the GNU
 * Lesser General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * These libraries and programs are distributed in the hope that
 * they will be useful, but WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with these libraries and programs; if not, write
 * to the Free Software Foundation, Inc., 51 Franklin Street, Fifth
 * Floor, Boston, MA 02110-1301 USA
 */
/* $XConsortium: ChangeDir.c /main/7 1996/10/30 11:09:35 drk $ */
/************************************<+>*************************************
 ****************************************************************************
 *
 *   FILE:           ChangeDir.c
 *
 *   COMPONENT_NAME: Desktop File Manager (dtfile)
 *
 *   DESCRIPTION:    Source file for the change directory dialog.
 *
 *   FUNCTIONS: ChangeToNewDir
 *		CheckRestrictedDir
 *		Create
 *		Destroy
 *		DropOnChangeView
 *		FreeValues
 *		GetDefaultValues
 *		GetResourceValues
 *		GetValues
 *		GoToNewDir
 *		InstallChange
 *		InstallClose
 *		OkCallback
 *		SetFocus
 *		SetValues
 *		TextChange
 *		TryToChangeDir
 *		WriteResourceValues
 *
 *   (c) Copyright 1993, 1994, 1995 Hewlett-Packard Company
 *   (c) Copyright 1993, 1994, 1995 International Business Machines Corp.
 *   (c) Copyright 1993, 1994, 1995 Sun Microsystems, Inc.
 *   (c) Copyright 1993, 1994, 1995 Novell, Inc.
 *
 ****************************************************************************
 ************************************<+>*************************************/

#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <ctype.h>
#include <pwd.h>

#include <Xm/XmP.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <Xm/SelectioB.h>
#include <Xm/TabStack.h>
#include <Xm/TextF.h>
#include <Xm/VendorSEP.h>
#include <Xm/List.h>
#include <Xm/MwmUtil.h>
#include <Xm/Protocols.h>

#include <X11/ShellP.h>
#include <X11/Shell.h>
#include <X11/Xatom.h>

#include <Dt/Action.h>
#include <Dt/Connect.h>
#include <Dt/DtNlUtils.h>
#include <Dt/SharedProcs.h>

#include <Tt/tttk.h>

#include <Xm/DragIcon.h>
#include <Xm/DragC.h>
#include <Xm/TextF.h>
#include <Xm/XmPrivate.h> /* _XmStringUngenerate */
#include <Dt/Dnd.h>

#include "Encaps.h"
#include "SharedProcs.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "ChangeDir.h"
#include "Devices.h"
#include "Mounts.h"
#include "Help.h"
#include "SharedMsgs.h"


#define MAX_PATH PATH_MAX

static char * CHANGEDIRECTORY = "ChangeDirectory";


static DialogResource resources[] =
{
   { "string_path", XmRXmString, sizeof(XmString),
     XtOffset(ChangeDirDataPtr, string_path), nullptr, _DtXmStringToString },

   { "historyList", XmRXmStringTable, sizeof(XmStringTable),
      XtOffset(ChangeDirDataPtr, history_list), nullptr, _DtXmStringTableToString },

   { "visibleCount", XmRInt, sizeof(int),
      XtOffset(ChangeDirDataPtr, visible_count), (XtPointer) 8, _DtIntToString },

   { "listCount", XmRInt, sizeof(int),
      XtOffset(ChangeDirDataPtr, list_count), nullptr, _DtIntToString },
};


/********    Static Function Declarations    ********/

static void Create(     Display *display,
                        Widget parent,
                        Widget *shell,
                        XtPointer *dialog) ;
static void InstallChange( XtPointer dialog,
                           XtCallbackProc callback,
                           XtPointer client_data) ;
static void InstallClose( XtPointer dialog,
                          XtCallbackProc callback,
                          XtPointer client_data) ;
static void Destroy( XtPointer dialog) ;
static XtPointer GetValues( XtPointer dialog) ;
static XtPointer GetDefaultValues( void ) ;
static XtPointer GetResourceValues( XrmDatabase data_base,
                                  char **name_list) ;
static void SetValues( XtPointer dialog,
                       XtPointer values) ;
static void WriteResourceValues( DialogData *values,
                                 int fd,
                                 char **name_list) ;
static void FreeValues( XtPointer values) ;
typedef void (*ChangeDirViewProc)(FileMgrData *file_mgr_data,
                                  char *directory);
static Boolean TryToChangeDir( Boolean isFromDialog,
                               char * incoming_string,
                               FileMgrData * file_mgr_data,
                               FileMgrRec * file_mgr_rec,
                               ChangeDirData * change_dir_data,
                               Widget selection_box,
                               XtCallbackProc dialog_callback,
                               XtPointer dialog_client_data,
                               ChangeDirViewProc view_callback) ;
static void OkCallback( Widget selection_box,
                        XtPointer client_data,
                        XtPointer call_data) ;
static int CheckRestrictedDir ( FileMgrRec * file_mgr_rec,
                                FileMgrData * file_mgr_data,
                                char ** value,
                                Boolean relative_name);
static void GoToNewDir ( FileMgrData * file_mgr_data,
                         char * dir_name) ;
static void SetFocus(
                        XtPointer dialog,
                        XtPointer values) ;


/********    End Static Function Declarations    ********/

extern char *getwd(char *);

/*
 *  The Dialog Class structure.
 */

static DialogClass changeDirClassRec =
{
   resources,
   XtNumber(resources),
   Create,
   InstallChange,
   InstallClose,
   Destroy,
   GetValues,
   GetDefaultValues,
   GetResourceValues,
   SetValues,
   WriteResourceValues,
   FreeValues,
   _DtGenericMapWindow,
   SetFocus,
};

DialogClass * changeDirClass = (DialogClass *) &changeDirClassRec;




/************************************************************************
 *
 *  Create
 *
 ************************************************************************/

static void
PublishGoToDialogState(ChangeDirRec *rec)
{
   if (!rec || !rec->change_dir)
      return;
   Display *dpy = XtDisplay(rec->change_dir);
   if (!dpy)
      return;
   Widget sel_page = rec->tab_stack ? XmTabStackGetSelectedTab(rec->tab_stack) : nullptr;
   const char *tab_name = (sel_page && sel_page == rec->mounts_page) ? "mounts" : "history";
   char buf[128];
   snprintf(buf, sizeof(buf), "tab=%s\tmounts=%d", tab_name, rec->mount_entry_count);
   Atom prop = XInternAtom(dpy, "_DT_GOTO_DIALOG_STATE", False);
   Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
   XChangeProperty(dpy, DefaultRootWindow(dpy), prop, utf8, 8, PropModeReplace,
                   (const unsigned char *)buf, (int)strlen(buf));
   XFlush(dpy);
}

static void
GoToTriggerOk(ChangeDirRec *rec)
{
   if (!rec || !rec->change_dir)
      return;
   Widget ok_btn = XmSelectionBoxGetChild(rec->change_dir, XmDIALOG_OK_BUTTON);
   if (ok_btn)
   {
      XmAnyCallbackStruct any = { XmCR_ACTIVATE, nullptr };
      XtCallCallbacks(ok_btn, XmNactivateCallback, (XtPointer)&any);
   }
}

static void
GoToHistorySelectCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   ChangeDirRec *rec = (ChangeDirRec *)client_data;
   XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
   (void)w;
   if (!rec || !cbs || !cbs->item)
      return;
   char *txt = (char *)_XmStringUngenerate(cbs->item, XmFONTLIST_DEFAULT_TAG,
                                           XmCHARSET_TEXT, XmCHARSET_TEXT);
   if (txt)
   {
      Widget tf = XmSelectionBoxGetChild(rec->change_dir, XmDIALOG_TEXT);
      if (tf)
      {
         XmTextFieldSetString(tf, txt);
         XmTextFieldSetInsertionPosition(tf, (XmTextPosition)strlen(txt));
      }
      XtFree(txt);
   }
}

static void
GoToHistoryDefaultCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   GoToHistorySelectCB(w, client_data, call_data);
   GoToTriggerOk((ChangeDirRec *)client_data);
}

static void
GoToUpdateMountDetails(ChangeDirRec *rec, int idx, Boolean update_text_field)
{
   if (!rec)
      return;
   if (idx < 0 || idx >= rec->mount_entry_count)
   {
      XmString xm = XmStringCreateLocalized("No mount selected.");
      XtVaSetValues(rec->mounts_info_label, XmNlabelString, xm, nullptr);
      XmStringFree(xm);
      if (rec->mounts_unmount_btn)
         XtSetSensitive(rec->mounts_unmount_btn, False);
      return;
   }

   const DtfileMountNavEntry *e = &rec->mount_entries[idx];
   char sz_buf[64] = "N/A";
   char fr_buf[64] = "N/A";
   if (e->total_bytes > 0)
      DtfileFormatMountBytes(e->total_bytes, sz_buf, sizeof(sz_buf));
   if (e->is_mounted && e->total_bytes > 0)
      DtfileFormatMountBytes(e->free_bytes, fr_buf, sizeof(fr_buf));
   else if (!e->is_mounted)
      snprintf(fr_buf, sizeof(fr_buf), "%s", e->is_luks_locked ? "Locked (LUKS)" : "Unmounted");

   char info_line[512];
   snprintf(info_line, sizeof(info_line), "Type: %s  |  Source: %s  |  Free: %s / %s",
            e->fs_type ? e->fs_type : "-",
            e->device ? e->device : "-",
            fr_buf, sz_buf);
   XmString xm = XmStringCreateLocalized(info_line);
   XtVaSetValues(rec->mounts_info_label, XmNlabelString, xm, nullptr);
   XmStringFree(xm);

   if (rec->mounts_unmount_btn)
      XtSetSensitive(rec->mounts_unmount_btn, (e->can_unmount || e->can_eject) ? True : False);

   if (update_text_field)
   {
      const char *dest = e->is_mounted ? e->mount_point : e->shelf_token;
      Widget tf = XmSelectionBoxGetChild(rec->change_dir, XmDIALOG_TEXT);
      if (tf && dest)
      {
         XmTextFieldSetString(tf, (char *)dest);
         XmTextFieldSetInsertionPosition(tf, (XmTextPosition)strlen(dest));
      }
   }
}

static void
GoToRefreshMountsTab(ChangeDirRec *rec, Boolean update_text_on_select)
{
   if (!rec || !rec->mounts_tab_list)
      return;

   if (rec->mount_entries)
   {
      DtfileFreeNavigableMounts(rec->mount_entries, rec->mount_entry_count);
      rec->mount_entries = nullptr;
      rec->mount_entry_count = 0;
   }

   rec->mount_entry_count = DtfileCollectNavigableMounts(&rec->mount_entries);
   XmListDeleteAllItems(rec->mounts_tab_list);

   if (rec->mount_entry_count > 0)
   {
      XmString *items = (XmString *)XtCalloc((Cardinal)rec->mount_entry_count, sizeof(XmString));
      for (int i = 0; i < rec->mount_entry_count; ++i)
         items[i] = XmStringCreateLocalized(rec->mount_entries[i].list_label);
      XmListAddItems(rec->mounts_tab_list, items, rec->mount_entry_count, 1);
      for (int i = 0; i < rec->mount_entry_count; ++i)
         XmStringFree(items[i]);
      XtFree((char *)items);

      XmListSelectPos(rec->mounts_tab_list, 1, False);
      GoToUpdateMountDetails(rec, 0, update_text_on_select);
   }
   else
   {
      GoToUpdateMountDetails(rec, -1, False);
   }

   PublishGoToDialogState(rec);
}

static void
GoToMountsSelectCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   ChangeDirRec *rec = (ChangeDirRec *)client_data;
   XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
   (void)w;
   if (!rec || !cbs)
      return;
   GoToUpdateMountDetails(rec, cbs->item_position - 1, True);
}

static void
GoToMountsDefaultCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   GoToMountsSelectCB(w, client_data, call_data);
   GoToTriggerOk((ChangeDirRec *)client_data);
}

static void
GoToMountsRefreshBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   (void)w;
   (void)call_data;
   GoToRefreshMountsTab((ChangeDirRec *)client_data, True);
}

static void
GoToMountsConnectBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   ChangeDirRec *rec = (ChangeDirRec *)client_data;
   (void)w;
   (void)call_data;
   ShowConnectServerForFileMgr(rec ? (FileMgrRec *)rec->active_file_mgr_rec : nullptr, "smb");
}

static void
GoToMountsUnmountBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   ChangeDirRec *rec = (ChangeDirRec *)client_data;
   int *pos = nullptr;
   int cnt = 0;
   (void)w;
   (void)call_data;
   if (!rec || !rec->mounts_tab_list)
      return;
   if (!XmListGetSelectedPos(rec->mounts_tab_list, &pos, &cnt) || cnt <= 0)
      return;
   int idx = pos[0] - 1;
   XtFree((char *)pos);
   if (idx < 0 || idx >= rec->mount_entry_count)
      return;

   FileMgrData *fmd = nullptr;
   if (rec->active_file_mgr_rec)
   {
      DialogData *dd = _DtGetInstanceData(rec->active_file_mgr_rec);
      if (dd) fmd = (FileMgrData *)dd->data;
   }
   DtfileDevicesUnmountOrEject(rec->shell, fmd, rec->mount_entries[idx].shelf_token,
                               rec->mount_entries[idx].can_eject);
   GoToRefreshMountsTab(rec, False);
}

static void
GoToTabSelectedCB(Widget w, XtPointer client_data, XtPointer call_data)
{
   ChangeDirRec *rec = (ChangeDirRec *)client_data;
   (void)w;
   (void)call_data;
   if (!rec)
      return;
   Widget sel = XmTabStackGetSelectedTab(rec->tab_stack);
   if (sel && sel == rec->mounts_page)
      GoToRefreshMountsTab(rec, True);
   else
      PublishGoToDialogState(rec);
}

static void
Create(
        Display *display,
        Widget parent,
        Widget *shell,
        XtPointer *dialog )
{
   ChangeDirRec * change_dir_rec;
   XmString prompt;
   Widget w;
   Arg args[10];
   int n;
   XtTranslations trans_table;
   Widget change_dir;

   (void)display;

   /*  Allocate the change directory dialog instance record.  */

   change_dir_rec = (ChangeDirRec *) XtCalloc (1, sizeof (ChangeDirRec));

   prompt = XmStringCreateLocalized (((char *)GETMESSAGE(2,14, "Destination Folder:")));

   trans_table = XtParseTranslationTable(translations_space);
   n = 0;
   XtSetArg (args[n], XmNselectionLabelString, prompt);		n++;
   XtSetArg (args[n], XmNokLabelString, okXmString);		n++;
   XtSetArg (args[n], XmNcancelLabelString, cancelXmString);	n++;
   XtSetArg (args[n], XmNhelpLabelString, helpXmString);	n++;
   XtSetArg (args[n], XmNautoUnmanage, False);			n++;
   XtSetArg (args[n], XmNchildPlacement, XmPLACE_TOP);		n++;
   XtSetArg (args[n], XmNtextTranslations, trans_table);	n++;
   change_dir = change_dir_rec->change_dir =
      XmCreateSelectionDialog (parent, "change_directory", args, n);
   change_dir_rec->shell = XtParent(change_dir);
   XtVaSetValues(change_dir_rec->shell, XmNallowShellResize, True, nullptr);

   XmStringFree (prompt);

   /* Hide built-in flat list visually while preserving XmDIALOG_LIST as the
    * session-saved history store; replace UI with a 2-tab XmTabStack! */
   w = XmSelectionBoxGetChild(change_dir, XmDIALOG_LIST_LABEL);
   if (w) XtUnmanageChild(w);
   w = XmSelectionBoxGetChild(change_dir, XmDIALOG_LIST);
   if (w && XtParent(w)) XtUnmanageChild(XtParent(w));

   change_dir_rec->tab_stack = XmCreateTabStack(change_dir, "goToTabStack", nullptr, 0);
   XtVaSetValues(change_dir_rec->tab_stack,
                 XmNwidth, 470,
                 XmNheight, 260,
                 nullptr);
   XtAddCallback(change_dir_rec->tab_stack, XmNtabSelectedCallback,
                 GoToTabSelectedCB, (XtPointer)change_dir_rec);
   XtManageChild(change_dir_rec->tab_stack);

   /* ---- Tab 1: Folder History ---- */
   XmString hist_tab_xm = XmStringCreateLocalized("Folder History");
   change_dir_rec->history_page = XmCreateForm(change_dir_rec->tab_stack, "goToHistoryPage", nullptr, 0);
   XtVaSetValues(change_dir_rec->history_page,
                 XmNtabLabelString, hist_tab_xm,
                 XmNmarginWidth, 8,
                 XmNmarginHeight, 8,
                 nullptr);
   XmStringFree(hist_tab_xm);

   XmString sys_xm = XmStringCreateLocalized(GETMESSAGE(2, 16, "System Name: "));
   change_dir_rec->history_host_label = XtVaCreateManagedWidget(
      "historyHostLabel", xmLabelGadgetClass, change_dir_rec->history_page,
      XmNlabelString, sys_xm,
      XmNalignment, XmALIGNMENT_BEGINNING,
      XmNtopAttachment, XmATTACH_FORM,
      XmNleftAttachment, XmATTACH_FORM,
      XmNrightAttachment, XmATTACH_FORM,
      nullptr);
   XmStringFree(sys_xm);

   n = 0;
   XtSetArg(args[n], XmNvisibleItemCount, 8); n++;
   XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
   XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
   XtSetArg(args[n], XmNtopWidget, change_dir_rec->history_host_label); n++;
   XtSetArg(args[n], XmNtopOffset, 4); n++;
   XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
   XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
   XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
   change_dir_rec->history_tab_list = XmCreateScrolledList(
      change_dir_rec->history_page, "goToHistoryList", args, n);
   XtManageChild(change_dir_rec->history_tab_list);
   XtAddCallback(change_dir_rec->history_tab_list, XmNbrowseSelectionCallback,
                 GoToHistorySelectCB, (XtPointer)change_dir_rec);
   XtAddCallback(change_dir_rec->history_tab_list, XmNsingleSelectionCallback,
                 GoToHistorySelectCB, (XtPointer)change_dir_rec);
   XtAddCallback(change_dir_rec->history_tab_list, XmNdefaultActionCallback,
                 GoToHistoryDefaultCB, (XtPointer)change_dir_rec);
   XtManageChild(change_dir_rec->history_page);

   /* ---- Tab 2: Connected Mounts & Shares ---- */
   XmString mnt_tab_xm = XmStringCreateLocalized("Connected Mounts & Shares");
   change_dir_rec->mounts_page = XmCreateForm(change_dir_rec->tab_stack, "goToMountsPage", nullptr, 0);
   XtVaSetValues(change_dir_rec->mounts_page,
                 XmNtabLabelString, mnt_tab_xm,
                 XmNmarginWidth, 8,
                 XmNmarginHeight, 8,
                 nullptr);
   XmStringFree(mnt_tab_xm);

   XmString mnt_hdr_xm = XmStringCreateLocalized("Select a connected share, removable drive, or mount:");
   Widget mnt_hdr = XtVaCreateManagedWidget(
      "mountsTabHeader", xmLabelGadgetClass, change_dir_rec->mounts_page,
      XmNlabelString, mnt_hdr_xm,
      XmNalignment, XmALIGNMENT_BEGINNING,
      XmNtopAttachment, XmATTACH_FORM,
      XmNleftAttachment, XmATTACH_FORM,
      XmNrightAttachment, XmATTACH_FORM,
      nullptr);
   XmStringFree(mnt_hdr_xm);

   XmString conn_xm = XmStringCreateLocalized("Connect Server...");
   change_dir_rec->mounts_connect_btn = XtVaCreateManagedWidget(
      "mountsTabConnectBtn", xmPushButtonWidgetClass, change_dir_rec->mounts_page,
      XmNlabelString, conn_xm,
      XmNbottomAttachment, XmATTACH_FORM,
      XmNleftAttachment, XmATTACH_FORM,
      nullptr);
   XmStringFree(conn_xm);
   XtAddCallback(change_dir_rec->mounts_connect_btn, XmNactivateCallback,
                 GoToMountsConnectBtnCB, (XtPointer)change_dir_rec);

   XmString umnt_xm = XmStringCreateLocalized("Unmount / Disconnect");
   change_dir_rec->mounts_unmount_btn = XtVaCreateManagedWidget(
      "mountsTabUnmountBtn", xmPushButtonWidgetClass, change_dir_rec->mounts_page,
      XmNlabelString, umnt_xm,
      XmNsensitive, False,
      XmNbottomAttachment, XmATTACH_FORM,
      XmNleftAttachment, XmATTACH_WIDGET,
      XmNleftWidget, change_dir_rec->mounts_connect_btn,
      XmNleftOffset, 6,
      nullptr);
   XmStringFree(umnt_xm);
   XtAddCallback(change_dir_rec->mounts_unmount_btn, XmNactivateCallback,
                 GoToMountsUnmountBtnCB, (XtPointer)change_dir_rec);

   XmString ref_xm = XmStringCreateLocalized("Refresh");
   change_dir_rec->mounts_refresh_btn = XtVaCreateManagedWidget(
      "mountsTabRefreshBtn", xmPushButtonWidgetClass, change_dir_rec->mounts_page,
      XmNlabelString, ref_xm,
      XmNbottomAttachment, XmATTACH_FORM,
      XmNrightAttachment, XmATTACH_FORM,
      nullptr);
   XmStringFree(ref_xm);
   XtAddCallback(change_dir_rec->mounts_refresh_btn, XmNactivateCallback,
                 GoToMountsRefreshBtnCB, (XtPointer)change_dir_rec);

   XmString inf_xm = XmStringCreateLocalized(" ");
   change_dir_rec->mounts_info_label = XtVaCreateManagedWidget(
      "mountsTabInfoLabel", xmLabelGadgetClass, change_dir_rec->mounts_page,
      XmNlabelString, inf_xm,
      XmNalignment, XmALIGNMENT_BEGINNING,
      XmNbottomAttachment, XmATTACH_WIDGET,
      XmNbottomWidget, change_dir_rec->mounts_connect_btn,
      XmNbottomOffset, 5,
      XmNleftAttachment, XmATTACH_FORM,
      XmNrightAttachment, XmATTACH_FORM,
      nullptr);
   XmStringFree(inf_xm);

   n = 0;
   XtSetArg(args[n], XmNvisibleItemCount, 7); n++;
   XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
   XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
   XtSetArg(args[n], XmNtopWidget, mnt_hdr); n++;
   XtSetArg(args[n], XmNtopOffset, 4); n++;
   XtSetArg(args[n], XmNbottomAttachment, XmATTACH_WIDGET); n++;
   XtSetArg(args[n], XmNbottomWidget, change_dir_rec->mounts_info_label); n++;
   XtSetArg(args[n], XmNbottomOffset, 5); n++;
   XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
   XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
   change_dir_rec->mounts_tab_list = XmCreateScrolledList(
      change_dir_rec->mounts_page, "goToMountsList", args, n);
   XtManageChild(change_dir_rec->mounts_tab_list);
   XtAddCallback(change_dir_rec->mounts_tab_list, XmNbrowseSelectionCallback,
                 GoToMountsSelectCB, (XtPointer)change_dir_rec);
   XtAddCallback(change_dir_rec->mounts_tab_list, XmNsingleSelectionCallback,
                 GoToMountsSelectCB, (XtPointer)change_dir_rec);
   XtAddCallback(change_dir_rec->mounts_tab_list, XmNdefaultActionCallback,
                 GoToMountsDefaultCB, (XtPointer)change_dir_rec);
   XtManageChild(change_dir_rec->mounts_page);

   XmTabStackSelectTab(change_dir_rec->history_page, False);

   /* Add the help callback to each of the important sub-components */
   XtAddCallback(change_dir, XmNhelpCallback, HelpRequestCB,
                 HELP_CHANGEDIR_DIALOG_STR);
   w = XmSelectionBoxGetChild(change_dir, XmDIALOG_APPLY_BUTTON);
   XtAddCallback(w, XmNhelpCallback, HelpRequestCB,
                 HELP_CHANGEDIR_DIALOG_STR);
   w = XmSelectionBoxGetChild(change_dir, XmDIALOG_CANCEL_BUTTON);
   XtAddCallback(w, XmNhelpCallback, HelpRequestCB,
                 HELP_CHANGEDIR_DIALOG_STR);
   w = XmSelectionBoxGetChild(change_dir, XmDIALOG_HELP_BUTTON);
   XtAddCallback(w, XmNhelpCallback, HelpRequestCB,
                 HELP_CHANGEDIR_DIALOG_STR);
   XtAddCallback(w, XmNactivateCallback, HelpRequestCB,
                 HELP_CHANGEDIR_DIALOG_STR);
   w = XmSelectionBoxGetChild(change_dir, XmDIALOG_OK_BUTTON);
   XtAddCallback(w, XmNhelpCallback, HelpRequestCB,
                 HELP_CHANGEDIR_DIALOG_STR);
   w = XmSelectionBoxGetChild(change_dir, XmDIALOG_SELECTION_LABEL);
   XtAddCallback(w, XmNhelpCallback, HelpRequestCB,
                 HELP_CHANGEDIR_DIALOG_STR);
   w = XmSelectionBoxGetChild(change_dir, XmDIALOG_TEXT);
   XtAddCallback(w, XmNhelpCallback, HelpRequestCB,
                 HELP_CHANGEDIR_DIALOG_STR);


   /*  Adjust the margin widths and heights of each of the buttons  */

   XtSetArg (args[0], XmNmarginWidth, 6);
   XtSetArg (args[1], XmNmarginHeight, 2);

   w = XmSelectionBoxGetChild (change_dir_rec->change_dir, XmDIALOG_OK_BUTTON);
   XtSetValues (w, args, 2);

   w = XmSelectionBoxGetChild (change_dir_rec->change_dir,
                                                         XmDIALOG_HELP_BUTTON);
   XtSetValues (w, args, 2);

   w = XmSelectionBoxGetChild (change_dir_rec->change_dir,
                                                       XmDIALOG_CANCEL_BUTTON);
   XtSetValues (w, args, 2);

   XtUnmanageChild(XmSelectionBoxGetChild (change_dir_rec->change_dir,
                   XmDIALOG_APPLY_BUTTON));


   /*  Adjust the decorations for the dialog shell of the dialog  */

   XtSetArg(args[0], XmNmwmFunctions, MWM_FUNC_MOVE | MWM_FUNC_RESIZE | MWM_FUNC_CLOSE);
   XtSetArg(args[1], XmNmwmDecorations,
             MWM_DECOR_BORDER | MWM_DECOR_RESIZEH | MWM_DECOR_TITLE);
   XtSetValues (change_dir_rec->shell, args, 2);

   XtSetArg(args[0], XmNcancelButton, w);
   XtSetValues (change_dir_rec->change_dir, args, 1);

   /*  Set the return values for the dialog widget and dialog instance.  */

   *shell = change_dir_rec->change_dir;
   *dialog = (XtPointer) change_dir_rec;

}




/************************************************************************
 *
 *  InstallChange
 *
 ************************************************************************/

static void
InstallChange(
        XtPointer dialog,
        XtCallbackProc callback,
        XtPointer client_data )
{
   ChangeDirRec *change_dir_rec = dialog;
   Widget selection_box = change_dir_rec->change_dir;
   ChangeDirApply * apply_data;

   /*  Setup the callback data to be sent to the Ok callback.   */
   /*  This contains the encapsulation callback to invoke upon  */
   /*  the data within the dialog being changed.                */

   apply_data = (ChangeDirApply *) XtMalloc (sizeof (ChangeDirApply));
   apply_data->callback = callback;
   apply_data->client_data = client_data;
   apply_data->change_dir_rec = (XtPointer) change_dir_rec;
   change_dir_rec->apply_data = apply_data;

   /*  Add the callbacks for list item insertion and help.  */

   XtAddCallback (selection_box, XmNokCallback, OkCallback,
                                                      (XtPointer) apply_data);
}




/************************************************************************
 *
 *  InstallClose
 *
 ************************************************************************/

static void
InstallClose(
        XtPointer dialog,
        XtCallbackProc callback,
        XtPointer client_data )
{
   ChangeDirRec *change_dir_rec = dialog;
   Widget close_widget;
   Atom delete_window_atom;

   close_widget =
      XmSelectionBoxGetChild (change_dir_rec->change_dir,
                                             XmDIALOG_CANCEL_BUTTON);

   XtAddCallback (close_widget, XmNactivateCallback, callback, client_data);

   delete_window_atom = XmInternAtom (XtDisplay(change_dir_rec->shell),
                                      "WM_DELETE_WINDOW", True);
   XmRemoveWMProtocols( change_dir_rec->shell, &delete_window_atom, 1 );
   XmAddWMProtocolCallback( change_dir_rec->shell, delete_window_atom, callback,
                            (XtPointer)client_data );
}




/************************************************************************
 *
 *  Destroy
 *
 ************************************************************************/

static void
Destroy(
        XtPointer dialog )
{
   ChangeDirRec *change_dir_rec = dialog;
   if (change_dir_rec->mount_entries)
   {
      DtfileFreeNavigableMounts(change_dir_rec->mount_entries,
                                change_dir_rec->mount_entry_count);
      change_dir_rec->mount_entries = nullptr;
      change_dir_rec->mount_entry_count = 0;
   }
   XtDestroyWidget (change_dir_rec->change_dir);
   XtFree ((char *) change_dir_rec->apply_data);
   XtFree ((char *) change_dir_rec);
}




/************************************************************************
 *
 *  GetValues
 *
 ************************************************************************/

static XtPointer
GetValues(
        XtPointer dialog )
{
   ChangeDirRec *change_dir_rec = dialog;
   ChangeDirData * change_dir_data;
   Arg args[8];
   int n;
   XmStringTable history_list;
   XmString      string_path;


   /*  Allocate and initialize the change dir dialog data.  */

   change_dir_data = (ChangeDirData *) XtMalloc (sizeof (ChangeDirData));

   change_dir_data->displayed = True;
   change_dir_data->host_name = nullptr;
   change_dir_data->file_mgr_rec = nullptr;

   n = 0;
   XtSetArg (args[n], XmNx, &change_dir_data->x);		n++;
   XtSetArg (args[n], XmNy, &change_dir_data->y);		n++;
   XtSetArg (args[n], XmNwidth, &change_dir_data->width);		n++;
   XtSetArg (args[n], XmNheight, &change_dir_data->height);		n++;
   XtSetArg (args[n], XmNlistItemCount, &change_dir_data->list_count);	n++;
   XtSetArg (args[n], XmNlistVisibleItemCount, &change_dir_data->visible_count);	n++;
   XtSetArg (args[n], XmNtextString, &string_path);		n++;
   XtSetArg (args[n], XmNlistItems, &history_list);		n++;
   XtGetValues (change_dir_rec->change_dir, args, n);


   /*  Extract and make local copies of the XmString data.  */

   change_dir_data->history_list = (XmStringTable)
      XtCalloc((change_dir_data->list_count + 1), sizeof(XmStringTable));

   for (n = 0; n < change_dir_data->list_count; n++)
      change_dir_data->history_list[n] = XmStringCopy (history_list[n]);
   change_dir_data->history_list[n] = nullptr;

   if(change_dir_data->string_path)
      XmStringFree (change_dir_data->string_path);
   change_dir_data->string_path = XmStringCopy (string_path);

   return ((XtPointer) change_dir_data);
}




/************************************************************************
 *
 *  GetDefaultValues
 *
 ************************************************************************/

static XtPointer
GetDefaultValues( void )
{
   ChangeDirData * change_dir_data;


   /*  Allocate and initialize the default change dir dialog data.  */

   change_dir_data = (ChangeDirData *) XtMalloc (sizeof (ChangeDirData));

   change_dir_data->displayed = False;
   change_dir_data->x = 0;
   change_dir_data->y = 0;
   change_dir_data->height = 0;
   change_dir_data->width = 0;
   change_dir_data->string_path = nullptr;

/*
   This will cause users_home_dir to be displayed in
   Application Manager go to dialog.

   if(restrictMode)
      change_dir_data->string_path =
              XmStringCreateLocalized(users_home_dir, XmFONTLIST_DEFAULT_TAG);
   else
      change_dir_data->string_path = NULL;
*/
   change_dir_data->history_list = nullptr;
   change_dir_data->visible_count = 8;
   change_dir_data->list_count = 0;
   change_dir_data->host_name = nullptr;
   change_dir_data->file_mgr_rec = nullptr;


   return ((XtPointer) change_dir_data);
}




/************************************************************************
 *
 *  GetResourceValues
 *
 ************************************************************************/

static XtPointer
GetResourceValues(
        XrmDatabase data_base,
        char **name_list )
{
   ChangeDirData * change_dir_data;
   XmStringTable   new_table;
   int i;


   /*  Allocate and get the resources for change dir dialog data.  */

   change_dir_data = (ChangeDirData *) XtMalloc (sizeof (ChangeDirData));

   _DtDialogGetResources (data_base, name_list,
                       CHANGEDIRECTORY, (char *) change_dir_data,
                       resources, changeDirClass->resource_count);

   change_dir_data->host_name = nullptr;


   /*  If the history list is Non-Null, realloc the pointer array  */
   /*  one element larger and NULL out the new last element        */

   if (change_dir_data->list_count != 0)
   {
      new_table = (XmStringTable) XtCalloc((change_dir_data->list_count + 1), sizeof (XmStringTable));

      for (i = 0; i < change_dir_data->list_count; i++)
         new_table[i] = change_dir_data->history_list[i];

      new_table[change_dir_data->list_count] = nullptr;

/******************
  THIS WILL BE A MEMORY HOLE WHEN MOTIF FIXES THE XMSTRINGTABLE
  CONVERTER.  EITHER REALLOC THE RETURNED TABLE OR COPY AND FREE IT.
******************/
      change_dir_data->history_list = new_table;
   }

   return ((XtPointer) change_dir_data);
}




/************************************************************************
 *
 *  SetValues
 *
 ************************************************************************/

static void
SetValues(
        XtPointer dialog,
        XtPointer values )
{
   ChangeDirRec *change_dir_rec = dialog;
   ChangeDirData *change_dir_data = values;
   Arg args[4];
   char *tmp_str, *host_name;
   XmString host_string;
   DialogData * dialog_data;
   FileMgrData * file_mgr_data;

   dialog_data = _DtGetInstanceData (change_dir_data->file_mgr_rec);
   file_mgr_data = (FileMgrData *) dialog_data->data;

   if(change_dir_data->string_path)
      XmStringFree (change_dir_data->string_path);

   if( restrictMode
       && file_mgr_data->toolbox == False )
      change_dir_data->string_path =
              XmStringCreateLocalized( users_home_dir);
   else if( file_mgr_data->restricted_directory )
      change_dir_data->string_path =
        XmStringCreateLocalized( file_mgr_data->current_directory);
   else
      change_dir_data->string_path = nullptr;

   XtSetArg (args[0], XmNlistItemCount, change_dir_data->list_count);
   XtSetArg (args[1], XmNlistVisibleItemCount, change_dir_data->visible_count);
   XtSetArg (args[2], XmNtextString, change_dir_data->string_path);
   XtSetArg (args[3], XmNlistItems, change_dir_data->history_list);

   XtSetValues (change_dir_rec->change_dir, args, 4);

   change_dir_rec->active_file_mgr_rec = change_dir_data->file_mgr_rec;
   if (change_dir_rec->history_tab_list)
   {
      XmListDeleteAllItems(change_dir_rec->history_tab_list);
      if (change_dir_data->list_count > 0 && change_dir_data->history_list)
         XmListAddItems(change_dir_rec->history_tab_list,
                        change_dir_data->history_list,
                        change_dir_data->list_count, 1);
   }

   if( change_dir_data->string_path )
   {
      Widget text;

      text = XmSelectionBoxGetChild(change_dir_rec->change_dir, XmDIALOG_TEXT);
      XtAddCallback (text, XmNmodifyVerifyCallback,
                     TextChange, (XtPointer)file_mgr_data );
      XtAddCallback (text, XmNmotionVerifyCallback,
                     TextChange, (XtPointer)file_mgr_data );
      XtAddCallback (text, XmNvalueChangedCallback,
                     TextChange, (XtPointer)file_mgr_data );
   }

   tmp_str = GETMESSAGE(2, 16, "System Name: ");
   host_name = XtMalloc(strlen(tmp_str) + strlen(change_dir_data->host_name)+1);
   sprintf(host_name, "%s%s", tmp_str, change_dir_data->host_name);
   host_string = XmStringCreateLocalized (host_name);
   XtSetArg (args[0], XmNlabelString, host_string);
   XtSetValues (XmSelectionBoxGetChild (change_dir_rec->change_dir,
                                        XmDIALOG_LIST_LABEL), args, 1);
   if (change_dir_rec->history_host_label)
      XtSetValues (change_dir_rec->history_host_label, args, 1);

   XtFree(host_name);
   XmStringFree(host_string);

   GoToRefreshMountsTab(change_dir_rec,
      (change_dir_rec->tab_stack &&
       XmTabStackGetSelectedTab(change_dir_rec->tab_stack) == change_dir_rec->mounts_page)
         ? True : False);
}




/************************************************************************
 *
 *  WriteResourceValues
 *
 ************************************************************************/

static void
WriteResourceValues(
        DialogData *values,
        int fd,
        char **name_list )
{
   ChangeDirData * change_dir_data = (ChangeDirData *) values->data;
   ChangeDirRec * change_dir_rec;

   /*  If the dialog is currently displayed, update the geometry  */
   /*  fields to their current values.                            */

   if (change_dir_data->displayed == True)
   {
      _DtGenericUpdateWindowPosition(values);
      change_dir_rec = (ChangeDirRec *) _DtGetDialogInstance (values);
   }

   _DtDialogPutResources (fd, name_list, CHANGEDIRECTORY, values->data,
                          resources, changeDirClass->resource_count);
}




/************************************************************************
 *
 *  FreeValues
 *
 ************************************************************************/

static void
FreeValues(
        XtPointer values )
{
   ChangeDirData *change_dir_data = values;
   int i;

   for (i = 0; i < change_dir_data->list_count; i++)
      XmStringFree (change_dir_data->history_list[i]);
   XtFree ((char *) change_dir_data->history_list);

   XmStringFree (change_dir_data->string_path);
   XtFree ((char *) change_dir_data->host_name);

   XtFree ((char *) change_dir_data);
}


/************************************************************************
 ************************************************************************
 *
 *   Internal functions used by the ChangeDir dialog
 *
 ************************************************************************
 ************************************************************************/

/*****************************************************************************
  TryToChangeDir
  INPUT:
  OUTPUT:
  DESCRIPTION:
    This function is used both by the ChangeDir dialog, and the FileMgr
    dialog's text field; it attempts to verify the incoming string, and
    then change to the indicated directory.
  NOTE:
 *****************************************************************************/
static Boolean
TryToChangeDir(
       Boolean isFromDialog,
       char * incoming_string,
       FileMgrData * file_mgr_data,
       FileMgrRec * file_mgr_rec,
       ChangeDirData * change_dir_data,
       Widget selection_box,
       XtCallbackProc dialog_callback,
       XtPointer dialog_client_data,
       ChangeDirViewProc view_callback)
{
  char * new_directory;
  char * host;
  char * path;
  struct stat stat_buf;
  Widget list;
  XmString path_string;
  XmString host_string;
  Arg args[1];
  char * message_buf;
  char * tmpStr;
  char * title;
  char * msg;
  Boolean rc = True;
  char *restricted = nullptr;

  new_directory = XtNewString(incoming_string);

  if ((new_directory) && (strcmp(new_directory, "") != 0))
  {
     _DtPathFromInput(new_directory, file_mgr_data->current_directory,
                      &host, &path);
  }
  else
  {
     tmpStr = GetSharedMessage(CHANGE_DIR_ERROR_TITLE);
     title = XtNewString(tmpStr);
     tmpStr = GETMESSAGE(2,18, "Destination Folder name is missing.\nType in a folder name or select a folder from the list.");
     msg = XtNewString(tmpStr);

     if(isFromDialog)
       _DtMessage(selection_box, title, msg, nullptr, HelpRequestCB);
     else
       _DtMessage(file_mgr_rec->current_directory_text, title, msg, nullptr,
                  HelpRequestCB);

     XtFree(title);
     XtFree(msg);
     return False;
  }


   /* Don't allow access to the desktop directory */
   if (path)
   {
      char *ttpath = (char *) GetTTPath(path);
      char *dtpath = (char *) GetTTPath(desktop_dir);
      if( ttpath && strcmp(ttpath, dtpath) == 0)
      {
        restricted = ttpath;
        XtFree(path);
        path = nullptr;
      }
   }

   /* Stat the file and see if it is a valid directory.  (If the current view
      is restricted, make sure that the new directory doesn't violate the
      directory restrictions. If so, refilter the displayed file set to show
      the files in this directory.
   */
   if ((path)
       &&(stat(path, &stat_buf) == 0)
       &&((stat_buf.st_mode & S_IFMT) == S_IFDIR)
       &&(CheckRestrictedDir(file_mgr_rec, file_mgr_data, &path, False) == 0))
   {
     /* Check for read/xcute permission */
     if (CheckAccess(path, R_OK | X_OK))
     {
       tmpStr = GETMESSAGE(9, 6, "Action Error");
       title = XtNewString(tmpStr);
       msg = (char *)XtMalloc(strlen(GETMESSAGE(30, 1, "Cannot read from %s"))
                            + strlen(new_directory)
                            + 1);
       sprintf(msg, GETMESSAGE(30, 1, "Cannot read from %s"), new_directory);

       if(isFromDialog)
         _DtMessage(selection_box, title, msg, nullptr, HelpRequestCB);
       else
         _DtMessage(file_mgr_rec->current_directory_text, title,
                    msg, nullptr, HelpRequestCB);

       XtFree(title);
       XtFree(msg);
       return False;
     }
     else
     {
       if (selection_box)
       {
         ChangeDirRec *cd_rec = (file_mgr_data && file_mgr_data->change_dir)
                                   ? (ChangeDirRec *)_DtGetDialogInstance(file_mgr_data->change_dir)
                                   : nullptr;

         /* Adjust the selection box elements to add the text item
          into the list.
         */
         list = XmSelectionBoxGetChild(selection_box, XmDIALOG_LIST);

         path_string = XmStringCreateLocalized(path);

         if (XmListItemExists(list, path_string))
           XmListDeselectAllItems(list);
         else
           XmListAddItem(list, path_string, 0);

         XmListSelectItem(list, path_string, False);
         XmListSetBottomPos(list, 0);

         if (cd_rec && cd_rec->history_tab_list)
         {
           if (XmListItemExists(cd_rec->history_tab_list, path_string))
             XmListDeselectAllItems(cd_rec->history_tab_list);
           else
             XmListAddItem(cd_rec->history_tab_list, path_string, 0);
           XmListSelectItem(cd_rec->history_tab_list, path_string, False);
           XmListSetBottomPos(cd_rec->history_tab_list, 0);
         }

         XmStringFree(path_string);
       }

       /*  Clear out the text string  */
       if (isFromDialog)
       {
         if(restrictMode)
           XtSetArg(args[0],
                    XmNtextString,
                    XmStringCreateLocalized(users_home_dir));
         else
           XtSetArg(args[0], XmNtextString, NULL);
         XtSetValues(selection_box, args, 1);
       }
       else
       {
         if(!file_mgr_data || !file_mgr_data->restricted_directory)
           XmTextFieldSetString(file_mgr_rec->current_directory_text,
                              incoming_string);

       }


       /* Update the change directory host name field
          and the host name indicator widget in the dialog.
       */
       XtFree((char *) change_dir_data->host_name);
       change_dir_data->host_name = XtNewString(host);

       if (selection_box)
       {
         tmpStr = GETMESSAGE(2, 16, "System Name: ");
         message_buf = XtMalloc(strlen(tmpStr) +
                                strlen(change_dir_data->host_name) + 1);
         sprintf(message_buf, "%s%s", tmpStr, change_dir_data->host_name);
         host_string =
           XmStringCreateLocalized(message_buf);
         XtSetArg(args[0], XmNlabelString, host_string);
         XtSetValues(XmSelectionBoxGetChild(selection_box,XmDIALOG_LIST_LABEL),
                     args, 1);
         if (file_mgr_data && file_mgr_data->change_dir)
         {
           ChangeDirRec *cd_rec = (ChangeDirRec *)_DtGetDialogInstance(file_mgr_data->change_dir);
           if (cd_rec && cd_rec->history_host_label)
             XtSetValues(cd_rec->history_host_label, args, 1);
         }
         XtFree(message_buf);
         XmStringFree(host_string);

         XmUpdateDisplay (selection_box);
       }

       /*  Call the encapsulation change data callback  */
       if (isFromDialog)
         dialog_callback(selection_box, dialog_client_data, path);
       else
         view_callback(file_mgr_data, path);
     }
   }
   else
   {
     tmpStr = GetSharedMessage(CHANGE_DIR_ERROR_TITLE);
     title = XtNewString(tmpStr);
     if(restricted)
     {
       tmpStr = GETMESSAGE(2,20,"You cannot switch to folder:\n\n%s\n\nYou are not allowed to view  this folder\nbecause it is a restricted folder.");
       path = restricted;
     }
     else
       tmpStr = GETMESSAGE(2,19,"The following folder name is invalid.\n\n%s");
     if(path)
     {
       message_buf = XtMalloc(strlen(tmpStr) + strlen(path) + 1);
       sprintf(message_buf, tmpStr, path);
     }
     else
     {
       message_buf = XtMalloc(strlen(tmpStr) + strlen(new_directory) + 1);
       sprintf(message_buf, tmpStr, new_directory);
     }

     if (isFromDialog)
       _DtMessage(selection_box, title, message_buf, nullptr, HelpRequestCB);
     else
       _DtMessage(file_mgr_rec->current_directory_text, title, message_buf,
                  nullptr, HelpRequestCB);

     XtFree(title);
     XtFree(message_buf);
     rc = False;
   }

   if (path)
      XtFree((char *) path);
   XtFree((char *) host);
   XtFree((char *) new_directory);

   return rc;
}



/************************************************************************
 *
 *  OkCallback
 *	This is a callback function called when the Ok button (Apply)
 *	is pressed.  It adds the string contained within the call data
 *	into the selection boxes list.
 *
 ************************************************************************/

static void
OkCallback(
        Widget selection_box,
        XtPointer client_data,
        XtPointer call_data )
{
   ChangeDirApply *apply_data = client_data;
   XmSelectionBoxCallbackStruct *callback_data = call_data;
   DialogData * dialog_data;
   ChangeDirData * change_dir_data;
   FileMgrRec * file_mgr_rec;
   FileMgrData * file_mgr_data;
   char *strValue;
   char *value = nullptr;
   int result;
   char message_buf[MAX_PATH + 100];
   char * tmpStr;
   char * title;
   char * msg;


   /*  Get the change dir data record  */

   dialog_data = _DtGetInstanceData (apply_data->change_dir_rec);
   change_dir_data = (ChangeDirData *) dialog_data->data;

   file_mgr_rec = (FileMgrRec *) change_dir_data->file_mgr_rec;
   dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);
   file_mgr_data = (FileMgrData *) dialog_data->data;

   value = (char *) _XmStringUngenerate((XmString)callback_data->value,
                                        XmFONTLIST_DEFAULT_TAG,
                                        XmCHARSET_TEXT, XmCHARSET_TEXT);

   if ( value == nullptr)
   {
       tmpStr = GetSharedMessage(CHANGE_DIR_ERROR_TITLE);
      title = XtNewString(tmpStr);
      tmpStr = (char *) GETMESSAGE(2, 11, "Cannot get the new folder name.");
      msg = XtNewString(tmpStr);

      _DtMessage(selection_box, title, message_buf, nullptr, HelpRequestCB);

      XtFree(title);
      XtFree(msg);
      return;
   }

   strValue = XtNewString(value);
   if (!restrictMode && file_mgr_data->restricted_directory == nullptr &&
       DtfileDevicesIsSyntheticPath(strValue))
   {
      _DtHideDialog(file_mgr_data->change_dir, False);
      change_dir_data->displayed = False;
      file_mgr_rec->menuStates |= CHANGEDIR;
      DtfileDevicesActivatePath(file_mgr_rec, file_mgr_data, strValue);
      XtFree(value);
      XtFree(strValue);
      return;
   }

   if(file_mgr_data->toolbox && strValue[0] != '/')
      result = CheckRestrictedDir(file_mgr_rec, file_mgr_data, &strValue, True);
   else
      result = CheckRestrictedDir(file_mgr_rec, file_mgr_data, &strValue,False);

   /*  Process the string representing a directory to verify  */
   /*  that it is a valid path.                               */
   if (result == 0)
   {
      TryToChangeDir(True, (char *)strValue, file_mgr_data,
                     file_mgr_rec, change_dir_data, selection_box,
                     apply_data->callback, apply_data->client_data, nullptr);
   }

   XtFree(value);
   XtFree(strValue);
}


/************************************************************************
 *
 *  CheckRestrictedDir:
 *    Make sure we stay inside a restricted directory
 *
 ************************************************************************/

static int
CheckRestrictedDir (
	FileMgrRec * file_mgr_rec,
	FileMgrData * file_mgr_data,
	char ** value,
	Boolean relative_name)
{
   char *tmpStr, directory[MAXPATHLEN];
   int len;
   char *tmpBuffer, *title, *msg;

   /*  If not in restricte mode, everything is ok.  */
   if (file_mgr_data->restricted_directory == nullptr && !restrictMode)
     return 0;

   /* get real file name */
   if (relative_name)
   {
      tmpStr = (char *)XtMalloc(strlen(*value) +
                  strlen(file_mgr_data->restricted_directory) + 3);
      strcpy(tmpStr, file_mgr_data->restricted_directory);
      if(*value[0] != '/')
         strcat(tmpStr, "/");
      strcat(tmpStr, *value);
      XtFree(*value);
      *value = tmpStr;
   }
   *value = (char *) DtEliminateDots (*value);

   if( restrictMode
       && file_mgr_data->toolbox == False )
   {
      if( strcmp( users_home_dir, "/" ) == 0 )
         return 0;
      strcpy( directory, users_home_dir );
   }
   else
      strcpy( directory, file_mgr_data->restricted_directory );

   len = strlen(directory);

   if( len > 1 && directory[len-1] == '/' )
   {
      directory[len - 1] = 0x0;
      --len;
   }

   /* check if value is inside the restricted subdir */
   len = strlen(directory);
   if (strncmp(*value, directory, len) != 0 ||
       (*value)[len] != '/' && (*value)[len] != '\0')
   {
       tmpBuffer = GetSharedMessage(CHANGE_DIR_ERROR_TITLE);
       title = XtNewString(tmpBuffer);
       if(restrictMode)
          tmpBuffer = GETMESSAGE(2,8, "You cannot switch to this folder.  You are\nallowed to view only folders beneath your\ncurrent folder. You cannot specify an absolute\npath to the new folder.");
       else
          tmpBuffer = GETMESSAGE(2,9, "This view is in restricted mode.  You cannot go\nto the specified folder because it is not in the\nrelative path of the restricted folder.\n");
       msg = XtNewString(tmpBuffer);

       _DtMessage(file_mgr_rec->current_directory_text, title, msg,
                  nullptr, HelpRequestCB);

       XtFree(title);
       XtFree(msg);
       return -1;
   }

   return 0;
}


/************************************************************************
 *
 *  ChangeToNewDir
 *	This is the activate callback for the FileMgr text field provided
 *	for quickly changing the current directory being viewed.
 *
 ************************************************************************/

void
ChangeToNewDir (
       Widget text_field,
       XtPointer client_data,
       XtPointer call_data )
{
   XmAnyCallbackStruct *callback_data = call_data;
   DialogData * dialog_data;
   ChangeDirData * change_dir_data;
   ChangeDirRec * change_dir_rec;
   FileMgrRec * file_mgr_rec;
   FileMgrData * file_mgr_data;
   char * value;
   Widget selection_box;
   Arg args[1];
   char * tmpStr;
   int rc;

   /*  Get the change dir data record  */

   file_mgr_rec = (FileMgrRec *) client_data;
   dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);
   file_mgr_data = (FileMgrData *) dialog_data->data;
   change_dir_data = (ChangeDirData *) file_mgr_data->change_dir->data;
   change_dir_rec =(ChangeDirRec *)_DtGetDialogInstance(file_mgr_data->change_dir);

   if (change_dir_rec)
      selection_box = change_dir_rec->change_dir;
   else
      selection_box = nullptr;

   value = (char *)XmTextFieldGetString(text_field);

   /* check if user attempts to go outside restricted directory */
   rc = CheckRestrictedDir (file_mgr_rec, file_mgr_data, &value, True);

/*
   This code will de-activate the text widget.
   HCI requests that if the switch is not successful, don't de-activate it
   so user has a change to go back and correct it without having to pop it
   up again.

   XmProcessTraversal(file_mgr_rec->file_window, XmTRAVERSE_CURRENT);
   XtSetArg (args[0], XmNallowShellResize, False);
   XtSetValues (file_mgr_rec->shell, args, 1);
   XtUnmanageChild(file_mgr_rec->current_directory_text);
   XtSetArg (args[0], XmNallowShellResize, True);
   XtSetValues (file_mgr_rec->shell, args, 1);
   file_mgr_data->fast_cd_enabled = False;
*/

   /*  Process the string representing a directory to verify  */
   /*  that it is a valid path.                               */
   if (rc == 0)
   {
     if( TryToChangeDir(False, value, file_mgr_data, file_mgr_rec,
                        change_dir_data, selection_box, nullptr, nullptr,
                        GoToNewDir) )
       /* de-activate the text widget */
       UnpostTextPath( file_mgr_data );
   }

   XtFree(value);
}


/************************************************************************
 *
 *  DropOnChangeView
 *	This is the drop callback for the change-view icon provided
 *	for quickly changing the current directory being viewed.
 *
 ************************************************************************/

void
DropOnChangeView (
     Widget w,
     XtPointer client_data,
     XtPointer call_data)
{
   DtDndDropCallbackStruct *fileList = (DtDndDropCallbackStruct *)call_data;
   FileMgrRec *file_mgr_rec;
   DialogData * dialog_data;
   ChangeDirData * change_dir_data;
   ChangeDirRec * change_dir_rec;
   FileMgrData * file_mgr_data;
   Widget selection_box;
   char *full_path_name;
   int  numFiles;

   numFiles = fileList->dropData->numItems;

   /* if multiple selection, ignore all but the last one */

   if (numFiles > 0 && fileList->reason == DtCR_DND_DROP_ANIMATE)
   {
      file_mgr_rec = (FileMgrRec *) client_data;
      dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);
      file_mgr_data = (FileMgrData *) dialog_data->data;

      full_path_name = XtNewString(fileList->dropData->data.files[numFiles-1]);

      if (CheckRestrictedDir(file_mgr_rec, file_mgr_data,
                             &full_path_name, False) == 0)
      {
         change_dir_data = (ChangeDirData *) file_mgr_data->change_dir->data;
         change_dir_rec =
              (ChangeDirRec *)_DtGetDialogInstance(file_mgr_data->change_dir);
         if (change_dir_rec)
            selection_box = change_dir_rec->change_dir;
         else
            selection_box = nullptr;

         TryToChangeDir(False, full_path_name, file_mgr_data, file_mgr_rec,
                        change_dir_data, selection_box, nullptr, nullptr,
                        GoToNewDir);
      }
      XtFree(full_path_name);
   }

   fileList->status = DtDND_FAILURE;
}


static void
GoToNewDir(
       FileMgrData * file_mgr_data,
       char * dir_name )
{
   ChangeDirData * change_dir_data;
   int i;
   Boolean found;
   XmString xm_dir_name;

   /* Clear out any selected portion of the old CWD string */
   if (file_mgr_data->cd_select != nullptr)
   {
      XtFree(file_mgr_data->cd_select);
      file_mgr_data->cd_select = nullptr;
   }

   /* Update the internal copy of the dialog history list */
   if (file_mgr_data->change_dir)
   {
      change_dir_data = (ChangeDirData *) file_mgr_data->change_dir->data;
      xm_dir_name = XmStringCreateLocalized(dir_name);

      /* Add the directory only if not already in the list */
      for (i = 0, found = False; i < change_dir_data->list_count; i++)
      {
         if (XmStringCompare(xm_dir_name, change_dir_data->history_list[i]))
         {
            found = True;
            XmStringFree(xm_dir_name);
            break;
         }
      }

      if (!found)
      {
         change_dir_data->list_count++;
         change_dir_data->history_list = (XmStringTable)
            XtRealloc((char *)change_dir_data->history_list,
                   sizeof(XmStringTable) * (change_dir_data->list_count + 1));
         change_dir_data->history_list[change_dir_data->list_count - 1] =
                xm_dir_name;
         change_dir_data->history_list[change_dir_data->list_count] = nullptr;
      }

      /* Update view to display the new directory */
      ShowNewDirectory(file_mgr_data, change_dir_data->host_name, dir_name);
   }
}


/*
 * Class function for forcing the focus to the text field, each time
 * the Change Dir dialog is posted.
 */

static void
SetFocus(
        XtPointer dialog,
        XtPointer values )
{
   ChangeDirRec *change_dir_rec = dialog;
   Widget text;

   /* Force the focus to the text field */
   text = XmSelectionBoxGetChild (change_dir_rec->change_dir, XmDIALOG_TEXT);
   XmProcessTraversal(text, XmTRAVERSE_CURRENT);
}


/***************************************************************************
 *
 * TextChange() - callback from editable text widgets when something is
 *        typed in the text widget.  Only used when in restricted mode.
 *        Used to make sure the users are not going out of their
 *        "restricted" space.
 *
 ****************************************************************************/

void
TextChange(
        Widget text,
        XtPointer client_data,
        XtPointer call_data )
{
   XmTextVerifyCallbackStruct *callback_data = call_data;
   char *homeDir, *homeDirBackup;
   char *selection;

   if (callback_data->reason == XmCR_MODIFYING_TEXT_VALUE ||
       callback_data->reason == XmCR_MOVING_INSERT_CURSOR )
   {
      FileMgrData * file_mgr_data;

      file_mgr_data = (FileMgrData *) client_data;

      selection = XmTextFieldGetSelection(text);

      /* get whats in the text widget */
      homeDir = homeDirBackup = XmTextFieldGetString(text);

      if(selection != nullptr)
      {
         if(callback_data->event == nullptr)
            XmTextFieldClearSelection(text, CurrentTime);
         else
            XmTextFieldClearSelection(text, callback_data->event->xkey.time);
         callback_data->doit = False;
      }
      else if(callback_data->reason == XmCR_MODIFYING_TEXT_VALUE)
      {
         /* see if the user is trying to back over the 'restricted'
          * text, in this case users_home_dir
          */
         if( strcmp( homeDir, "" ) != 0
             && strcmp( homeDir, file_mgr_data->restricted_directory ) <= 0 )
         {
            if( callback_data->text->length != 0 )
            {
               if( strncmp( file_mgr_data->restricted_directory,
                            callback_data->text->ptr,
                            strlen(file_mgr_data->restricted_directory)) != 0)
               {
                  callback_data->doit = True;
               }
            }
            else
            {
               callback_data->doit = False;
            }
         }
      }
      else
      {
         int maxlength;

         maxlength = strlen( file_mgr_data->restricted_directory );

         if(callback_data->newInsert < maxlength)
         {
            if(callback_data->event == nullptr)
               _XmTextFieldSetDestination((Widget)text,
                            (Position)callback_data->currInsert, CurrentTime);
            else
            {
               _XmTextFieldSetDestination((Widget)text,
                                          (Position)callback_data->currInsert,
                                          callback_data->event->xkey.time);
               callback_data->doit = False;
            }
         }
      }

      XtFree(homeDirBackup);
      XtFree(selection);
   }
}

void
ChangeDirectoryToParent(
      FileMgrRec * file_mgr_rec,
      char *directory)
{
   DialogData * dialog_data;
   ChangeDirData * change_dir_data;
   ChangeDirRec * change_dir_rec;
   FileMgrData * file_mgr_data;
   Widget selection_box;
   char * tmpStr;
   int rc;

   /*  Get the change dir data record  */

   dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);
   file_mgr_data = (FileMgrData *) dialog_data->data;
   change_dir_data = (ChangeDirData *) file_mgr_data->change_dir->data;
   change_dir_rec =(ChangeDirRec *)_DtGetDialogInstance(file_mgr_data->change_dir);

   if (change_dir_rec)
      selection_box = change_dir_rec->change_dir;
   else
      selection_box = nullptr;

   /* check if user attempts to go outside restricted directory */
   rc = CheckRestrictedDir (file_mgr_rec, file_mgr_data, &directory, True);

   /*  Process the string representing a directory to verify  */
   /*  that it is a valid path.                               */
   if (rc == 0)
   {
      TryToChangeDir(False, directory, file_mgr_data, file_mgr_rec,
                     change_dir_data, selection_box, nullptr, nullptr,
                     GoToNewDir);
   }

}

void
DtfileOpenGoToDialogTab(
      FileMgrRec *file_mgr_rec,
      int tab_index)
{
   DialogData *dialog_data;
   FileMgrData *file_mgr_data;
   ChangeDirData *change_dir_data;
   ChangeDirRec *change_dir_rec;

   if (!file_mgr_rec && view_count > 0 && view_set && view_set[0] && view_set[0]->dialog_data)
   {
      FileMgrData *v_fmd = (FileMgrData *)((DialogData *)view_set[0]->dialog_data)->data;
      if (v_fmd)
         file_mgr_rec = (FileMgrRec *)v_fmd->file_mgr_rec;
   }
   if (!file_mgr_rec)
      return;

   dialog_data = _DtGetInstanceData((XtPointer)file_mgr_rec);
   if (!dialog_data)
      return;
   file_mgr_data = (FileMgrData *)dialog_data->data;
   if (!file_mgr_data || !file_mgr_data->change_dir)
      return;

   change_dir_data = (ChangeDirData *)file_mgr_data->change_dir->data;
   if (!change_dir_data->displayed)
      ShowChangeDirDialog(nullptr, (XtPointer)file_mgr_rec, nullptr);

   change_dir_rec = (ChangeDirRec *)_DtGetDialogInstance(file_mgr_data->change_dir);
   if (!change_dir_rec || !change_dir_rec->tab_stack)
      return;

   if (tab_index == 1 && change_dir_rec->mounts_page)
   {
      XmTabStackSelectTab(change_dir_rec->mounts_page, True);
      GoToRefreshMountsTab(change_dir_rec, True);
   }
   else if (change_dir_rec->history_page)
   {
      XmTabStackSelectTab(change_dir_rec->history_page, True);
      PublishGoToDialogState(change_dir_rec);
   }
}
