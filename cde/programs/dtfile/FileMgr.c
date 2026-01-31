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
/* $TOG: FileMgr.c /main/19 1998/01/27 12:19:54 mgreess $ */
/************************************<+>*************************************
 ****************************************************************************
 *
 *   FILE:           FileMgr.c
 *
 *   COMPONENT_NAME: Desktop File Manager (dtfile)
 *
 *   Description:    Source file for the File Manager dialog.
 *
 *   FUNCTIONS: BranchListToString
 *		CheckMoveType
 *		Close
 *		CountDirectories
 *		Create
 *		CreateFmPopup
 *		Destroy
 *		DirTreeExpand
 *		DoTheMove
 *		DropOnFileWindow
 *		DropOnGadget
 *		DropOnObject
 *		DropOnPrimaryHotspot
 *		FMInput
 *		ActivateClist
 *		FileMgrBuildDirectories
 *		FileMgrPropagateSettings
 *		FileMgrRedisplayFiles
 *		FileMgrReread
 *		FreeDirectoryData
 *		FreeDirectorySet
 *		FreePositionInfo
 *		FreeValues
 *		GetDefaultValues
 *		GetFileData
 *		GetPixmapData
 *		GetResourceValues
 *		GetSessionDir
 *		GetTopInfo
 *		InheritPositionInfo
 *		IsShown
 *		LoadPositionInfo
 *		MakeDirectorySets
 *		MoveCancelCB
 *		MoveOkCB
 *		MsgTimerEvent
 *		NewDirectorySet
 *		PositionFlagSet
 *		PositioningEnabledInView
 *		ProcessDropOnFileWindow
 *		ProcessDropOnObject
 *		QueryBranchList
 *		ReadTreeDirectory
 *		SavePositionInfo
 *		SelectVisible
 *		SelectionListToString
 *		SetDisplayedRecur
 *		SetFocus
 *		SetIconAttributes
 *		SetPWD
 *		SetSpecialMsg
 *		SetValues
 *		ShowChangeDirField
 *		ShowNewDirectory
 *		StringToBranchList
 *		StringToSelectionList
 *		SystemClose
 *		UpdateBranchList
 *		UpdateBranchState
 *		UpdateHeaders
 *		UpdateStatusLine
 *		WriteResourceValues
 *
 *   (c) Copyright 1993, 1994, 1995 Hewlett-Packard Company
 *   (c) Copyright 1993, 1994, 1995 International Business Machines Corp.
 *   (c) Copyright 1993, 1994, 1995 Sun Microsystems, Inc.
 *   (c) Copyright 1993, 1994, 1995 Novell, Inc.
 *
 ****************************************************************************
 ************************************<+>*************************************/

#include <stdio.h>
#include <sys/types.h>
#include <pwd.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <ctype.h>

#include <Xm/XmP.h>
#include <Xm/CascadeB.h>
#include <Xm/DrawingA.h>
#include <Xm/DrawingAP.h>
#include <Xm/DrawnB.h>
#include <Xm/Frame.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/MainW.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/MenuShell.h>
#include <Xm/ScrolledW.h>
#include <Xm/SeparatoG.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/ToggleBG.h>
#include <Xm/VendorSEP.h>
#include <Dt/Icon.h>
#include <Dt/IconP.h>
#include <Dt/IconFile.h>
#include <Dt/Dnd.h>

#include <X11/ShellP.h>
#include <X11/Shell.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <Xm/Protocols.h>

#include <Dt/Action.h>
#include <Dt/DtP.h>                     /* required for DtDirPaths type */
#include <Dt/Connect.h>                 /* required for DtMakeConnect... */
#include <Dt/FileM.h>
#include <Dt/HourGlass.h>
#include <Dt/DtNlUtils.h>
#include <Dt/Dts.h>
#include <Dt/SharedProcs.h>
#include <Dt/Wsm.h>

#include <Xm/DragIcon.h>
#include <Xm/DragC.h>
#include <Dt/Dnd.h>

#include <Tt/tttk.h>

#include "Encaps.h"
#include "SharedProcs.h"
#include "Help.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "Common.h"
#include "Filter.h"
#include "Find.h"
#include "ChangeDir.h"
#include "ModAttr.h"
#include "Prefs.h"
#include "SharedMsgs.h"
#include "IconicPath.h"
#include "Shelf.h"
#include "DtSvcInternal.h"

#include <stdarg.h>

/*  Dialog classes installed by Main.c  */

extern int file_mgr_dialog;
extern int change_dir_dialog;
extern int preferences_dialog;
extern int filter_dialog;
extern int find_dialog;
extern int mod_attr_dialog;

extern int ReadInFiletypes(
                    FilterData *filter_data) ;

/* global varibles used within this function when we have to
 * put up a _DtMessage dialog
 */
static int          global_file_count;
static char         **global_file_set;
static char         **global_host_set;
static FileMgrData  *fm;
static int          view_type;
static FileViewData *fv;
static DesktopRec   *dtWindow;
static unsigned int mod;
static DirectorySet *dd;
static Pixmap       change_view_pixmap = XmUNSPECIFIED_PIXMAP;

/* Global variables used in Command.c, FileMgr.c and FileOp.c */
int     G_dropx, G_dropy;

/********    Static Function Declarations    ********/

static XmFontListEntry FontListDefaultEntry(
                        XmFontList font_list) ;
static void Create(
                        Display *display,
                        Widget parent,
                        Widget *return_widget,
                        XtPointer *dialog) ;
static XtPointer GetDefaultValues( void ) ;
static XtPointer GetResourceValues(
                        XrmDatabase data_base,
                        char **name_list) ;
static void SetValues(
                        FileMgrRec *file_mgr_rec,
                        FileMgrData *file_mgr_data) ;
static void WriteResourceValues(
                        DialogData *values,
                        int fd,
                        char **name_list) ;
static void FreeValues(
                        FileMgrData *file_mgr_data) ;
static void Destroy(
                        XtPointer recordPtr) ;
static void GetSessionDir(
                        FileMgrData *file_mgr_data) ;
static void BranchListToString(
                        int fd,
                        char ***value,
                        char *out_buf) ;
static void SelectionListToString(
                        int fd,
                        FileViewData ***value,
                        char *out_buf) ;
static Boolean StringToBranchList(
                        Display *display,
                        XrmValue *args,
                        Cardinal num_args,
                        XrmValue *from_val,
                        XrmValue *to_val,
                        XtPointer *converter_data );
static void StringToSelectionList(
                        XrmValue *args,
                        Cardinal num_args,
                        XrmValue *from_val,
                        XrmValue *to_val) ;
static void GetFileData(
                        FileMgrData *file_mgr_data,
                        Boolean valid,
                        char ** branch_list) ;
static FileViewData * GetTopInfo(
                        FileMgrData *file_mgr_data,
                        char *host_name,
                        char *directory_name,
                        char **branch_list) ;
static void CountDirectories(
                        FileViewData *ip,
                        int *np) ;
static DirectorySet * NewDirectorySet(
                        char *name,
                        FileViewData *ip,
                        FileMgrData *file_mgr_data) ;
static void MakeDirectorySets(
                        FileMgrData *file_mgr_data,
                        FileViewData *ip,
                        DirectorySet **directory_set,
                        int *index) ;
static void ReadTreeDirectory(
                        Widget w,
                        char *host_name,
                        char *directory_name,
                        FileMgrData *file_mgr_data,
                        char **branch_list,
                        DirectorySet ***directory_set,
                        int *directory_count) ;
static Bool IsShown(    FileMgrData *fmd,
                        FileViewData *ip);
static void SetDisplayedRecur(
                        FileMgrData *fmd,
                        FileViewData *dp,
                        int level) ;
static void SelectVisible (
                        FileMgrData *file_mgr_data) ;
static void FreeDirectorySet(
                        DirectorySet ** directory_set,
                        int directory_count) ;
static void FreeDirectoryData(
                        FileMgrData *file_mgr_data) ;
static void SystemClose(
                        Widget w,
                        XtPointer data) ;
static void SetIconAttributes(
                        FileMgrRec *file_mgr_rec,
                        FileMgrData *file_mgr_data,
                        char *directory_name) ;
static void SetFocus (
                        FileMgrRec  * file_mgr_rec,
                        FileMgrData * file_mgr_data) ;
static void MoveOkCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data ) ;
static void MoveCancelCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data ) ;
static void CreateFmPopup (
                        Widget w );
static void DoTheMove(
                        int type);
static void FMInput(
                        Widget wid,
                        XEvent *event,
                        String *params,
                        Cardinal *num_params );
static void ActivateClist(
                        Widget wid,
                        XEvent *event,
                        String *params,
                        Cardinal *num_params );
typedef struct
{
   FileMgrRec *file_mgr_rec;
   char *path;
   char *icon_name;
   int slot;
   Boolean drag_started;
   Boolean drag_possible;
   int drag_button;
   int press_x;
   int press_y;
   Boolean drop_registered;
   Boolean is_execute;
   Time last_click_time;
} ShelfItemData;

typedef struct _ShelfPopupData
{
   FileMgrRec *file_mgr_rec;
   FileMgrData *file_mgr_data;
   char *path;
   int slot;
   Boolean is_dir;
   Boolean is_execute;
   ShelfItemData *item_data;
} ShelfPopupData;

typedef struct _ShelfSlotData ShelfSlotData;

static void ShelfRegisterView(FileMgrRec *file_mgr_rec);
static void ShelfUnregisterView(FileMgrRec *file_mgr_rec);
static void ShelfPopupDataFree(ShelfPopupData *data);
static void ShelfItemPopup(
                        ShelfItemData *item_data,
                        XButtonEvent *button_event );
static void ShelfItemActivate(
                        ShelfItemData *item_data );
static void ShelfItemDefaultAction(
                        ShelfItemData *item_data );
static char *ShelfResolvePath(
                        FileMgrData *file_mgr_data,
                        const char *input_path,
                        char **host_out );
static int ShelfGetIconSize(
                        FileMgrData *file_mgr_data,
                        int *icon_px_out );
static PixmapData *ShelfGetPixmapData(
                        FileMgrRec *file_mgr_rec,
                        FileMgrData *file_mgr_data,
                        const char *resolved_path,
                        int icon_size,
                        char **data_type_out );
static Widget ShelfGetRowWidget(
                        FileMgrRec *file_mgr_rec );
static Widget ShelfFindWidgetForSlot(
                        Widget row,
                        int slot,
                        ShelfItemData **item_data_out );
static void ShelfUpdateSlotWidget(
                        FileMgrRec *file_mgr_rec,
                        int slot );
static void ShelfUpdateSlotWidgetWithData(
                        FileMgrRec *file_mgr_rec,
                        FileMgrData *file_mgr_data_in,
                        int slot );
static void ShelfUpdateSlotsRange(
                        FileMgrRec *file_mgr_rec,
                        int start_slot,
                        int end_slot );
static void ShelfUpdateSlotsRangeWithData(
                        FileMgrRec *file_mgr_rec,
                        FileMgrData *file_mgr_data_in,
                        int start_slot,
                        int end_slot );
static void ShelfUpdateSlotsRangeAllViews(
                        int start_slot,
                        int end_slot );
static void ShelfUpdateSlotAllViews(
                        int slot );
static void ShelfFreePixmapData(
                        PixmapData *pixmapData );
static void ShelfLog(const char *fmt, ...);
static void ShelfItemDestroyCB(
                        Widget wid,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfSlotDestroyCB(
                        Widget wid,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfRowDragEH(
                        Widget w,
                        XtPointer client_data,
                        XEvent *event,
                        Boolean *cont );
static void ShelfSlotDropCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static FileViewData *ShelfCreatePopupFileViewData(
                        FileMgrData *file_mgr_data,
                        const char *resolved_path );
static void ShelfFreePopupFileViewData(
                        FileViewData *file_view_data );
static void ShelfRunAction(
                        FileMgrRec *file_mgr_rec,
                        ShelfPopupData *popup_data,
                        const char *action );
static void ShelfDropContextDestroyCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfSlotRegisterDropSite(
                        Widget slot,
                        ShelfSlotData *slot_data );
static void ShelfEnforceSizeEH(
                        Widget w,
                        XtPointer client_data,
                        XEvent *event,
                        Boolean *cont );
static void ShelfPopupOpenCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfPopupOpenNewViewCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfPopupRunCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfPopupRunNoPromptCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfPopupRemoveCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfPopupMoveCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfPopupCopyCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfPopupLinkCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfPopupPopdownCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
void ShelfPopdownAllMenus(
                        FileMgrRec *file_mgr_rec );
static void FilePopupPopdownCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfItemDropCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static void ShelfItemCallback(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data );
static Pixmap ShelfCreateDragPixmap(
        Widget w,
        unsigned int *width_out,
        unsigned int *height_out,
        unsigned int *depth_out );
static Pixmap ShelfCreateDragMask(
        Widget w,
        unsigned int width,
        unsigned int height );

static void ShelfStartDrag(
                        Widget w,
                        ShelfItemData *item_data,
                        XEvent *event );
static void ShelfDebugDumpTimeout(
                        XtPointer client_data,
                        XtIntervalId *id );
static char *ShelfFitLabel(
                        const char *text,
                        XmFontList font_list,
                        int max_px );
static void ShelfRebuildAllTimeout(
                        XtPointer client_data,
                        XtIntervalId *id);

void ShelfRequestRebuild(Widget widget);

void ShelfRebuild(
                        FileMgrRec *file_mgr_rec,
                        FileMgrData *file_mgr_data );
void ShelfRebuildAll(void);

/********    End Static Function Declarations    ********/


#define TREE_DASH_WIDTH 1

/*  Resource read and write function  */

static char * FILEMGR = "FileMgr";

static char *
ShelfFitLabel(
        const char *text,
        XmFontList font_list,
        int max_px )
{
   XmString xm = NULL;
   Dimension w = 0, h = 0;
   char *fit = NULL;
   int len;

   if (text == NULL)
      return NULL;

   fit = XtNewString(text);
   xm = XmStringCreateLocalized(fit);
   XmStringExtent(font_list, xm, &w, &h);
   XmStringFree(xm);

   if ((int)w <= max_px)
      return fit;

   XtFree(fit);
   len = (int)strlen(text);
   while (len > 0)
   {
      char *tmp = XtMalloc((size_t)len + 4); /* space for "..." */
      memcpy(tmp, text, (size_t)(len));
      tmp[len] = '\0';
      strcat(tmp, "...");
      xm = XmStringCreateLocalized(tmp);
      XmStringExtent(font_list, xm, &w, &h);
      XmStringFree(xm);
      if ((int)w <= max_px)
         return tmp;
      XtFree(tmp);
      len--;
   }

   return XtNewString("...");
}

/* Forward declarations for helpers that use these globals. */
static FileMgrRec **shelf_views;
static int shelf_view_count;
static Dimension shelf_slot_width;

static Widget
ShelfGetRowWidget(FileMgrRec *file_mgr_rec)
{
   Widget *children = NULL;
   Cardinal num_children = 0;
   int i;

   if (file_mgr_rec == NULL || file_mgr_rec->shelf_frame == NULL)
      return NULL;

   XtVaGetValues(file_mgr_rec->shelf_frame,
                 XmNchildren, &children,
                 XmNnumChildren, &num_children,
                 NULL);
   if (children == NULL || num_children == 0)
      return NULL;

   for (i = 0; i < (int)num_children; i++)
   {
      if (children[i] && XtName(children[i]) &&
          strcmp(XtName(children[i]), "shelf_row") == 0)
         return children[i];
   }

   return children[0];
}

static Widget
ShelfFindWidgetForSlot(
        Widget row,
        int slot,
        ShelfItemData **item_data_out )
{
   Widget *children = NULL;
   Cardinal num_children = 0;
   int i;

   if (item_data_out)
      *item_data_out = NULL;

   if (row == NULL)
      return NULL;

   XtVaGetValues(row,
                 XmNchildren, &children,
                 XmNnumChildren, &num_children,
                 NULL);
   if (children == NULL || num_children == 0)
      return NULL;

   for (i = 0; i < (int)num_children; i++)
   {
      Widget child = children[i];
      ShelfItemData *item_data = NULL;

      if (child == NULL)
         continue;

      XtVaGetValues(child, XmNuserData, &item_data, NULL);
      if (item_data == NULL)
         continue;
      if (item_data->slot != slot)
         continue;

      if (item_data_out)
         *item_data_out = item_data;
      return child;
   }

   return NULL;
}

static void
ShelfUpdateSlotWidget(
        FileMgrRec *file_mgr_rec,
        int slot )
{
   ShelfUpdateSlotWidgetWithData(file_mgr_rec, NULL, slot);
}

static void
ShelfUpdateSlotWidgetWithData(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data_in,
        int slot )
{
   DialogData *dialog_data;
   FileMgrData *file_mgr_data;
   Widget row;
   Widget item;
   ShelfItemData *item_data = NULL;
   const char *path;
   XmFontList font_list = NULL;
   XmString label = NULL;
   char *label_text = NULL;
   char *fit_label = NULL;
   char *resolved_path = NULL;
   char *resolved_host = NULL;
   char *data_type = NULL;
   char *icon_name_owned = NULL;
   char *icon_name_dup = NULL;
   const char *icon_name = NULL;
   PixmapData *pixmapData = NULL;
   int shelf_icon_size;
   int shelf_icon_px = 0;
   int max_label_px;

   if (file_mgr_rec == NULL)
      return;

   file_mgr_data = file_mgr_data_in;
   if (file_mgr_data == NULL)
   {
      dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);
      if (dialog_data == NULL)
         return;
      file_mgr_data = (FileMgrData *) dialog_data->data;
      if (file_mgr_data == NULL)
         return;
   }

   row = ShelfGetRowWidget(file_mgr_rec);
   if (row == NULL)
      return;

   item = ShelfFindWidgetForSlot(row, slot, &item_data);
   if (item == NULL || item_data == NULL)
      return;

   path = ShelfGetPathForSlot(slot);
   if (getenv("DTFILE_SHELF_DEBUG") != NULL)
      ShelfLog("ShelfUpdateSlotWidget: slot %d path=%s\n",
               slot, path ? path : "(null)");

   XtVaGetValues(item, XmNfontList, &font_list, NULL);

   if (path == NULL)
   {
      if (item_data->path)
      {
         XtFree(item_data->path);
         item_data->path = NULL;
      }
      if (item_data->icon_name)
      {
         XtFree(item_data->icon_name);
         item_data->icon_name = NULL;
      }
      label = XmStringCreateLocalized("");
      XtVaSetValues(item,
                    XmNstring, label,
                    XmNimageName, (String)NULL,
                    NULL);
      XmStringFree(label);
      return;
   }

   if (slot == 0)
      label_text = XtNewString("Home");
   else
   {
      const char *base = strrchr(path, '/');
      if (base && *(base + 1) != '\0')
         label_text = XtNewString(base + 1);
      else
         label_text = XtNewString(path);
   }

   shelf_icon_size = ShelfGetIconSize(file_mgr_data, &shelf_icon_px);
   max_label_px = (int)shelf_slot_width - 8;
   if (font_list)
      fit_label = ShelfFitLabel(label_text, font_list, max_label_px);
   else
      fit_label = XtNewString(label_text);
   XtFree(label_text);
   label_text = NULL;

   resolved_path = ShelfResolvePath(file_mgr_data, path, &resolved_host);
   if (item_data)
   {
      struct stat st;
      item_data->is_execute = False;
      if (resolved_path &&
          stat(resolved_path, &st) == 0 &&
          !S_ISDIR(st.st_mode) &&
          (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
      {
         item_data->is_execute = True;
      }
   }
   pixmapData = ShelfGetPixmapData(file_mgr_rec, file_mgr_data,
                                   resolved_path, shelf_icon_size,
                                   &data_type);
   if (pixmapData && pixmapData->iconFileName && *pixmapData->iconFileName)
      icon_name = pixmapData->iconFileName;

   if (icon_name == NULL)
   {
      const char *fallback;
      int dt_size;
      if (data_type && strcmp(data_type, LT_DIRECTORY) == 0)
         fallback = "DtdirB";
      else
         fallback = "Dtdeflt";
      dt_size = (shelf_icon_size == EXTRA_LARGE) ? DtLARGE :
                  (shelf_icon_size == LARGE) ? DtMEDIUM : DtSMALL;
      icon_name_owned = _DtGetIconFileName(XtScreen(file_mgr_rec->shell),
                                           NULL, (String)fallback, NULL, dt_size);
      if (icon_name_owned && *icon_name_owned)
         icon_name = icon_name_owned;
   }

   if (icon_name)
      icon_name_dup = XtNewString(icon_name);

   if (item_data->path)
      XtFree(item_data->path);
   item_data->path = XtNewString(path);
   if (item_data->icon_name)
      XtFree(item_data->icon_name);
   item_data->icon_name = icon_name_dup;

   label = XmStringCreateLocalized(fit_label ? fit_label : "");
   XtVaSetValues(item,
                 XmNstring, label,
                 XmNimageName, icon_name_dup,
                 XmNpixmapPosition, XmPIXMAP_TOP,
                 XmNmaxPixmapWidth, shelf_icon_px,
                 XmNmaxPixmapHeight, shelf_icon_px,
                 XmNfillMode, XmFILL_TRANSPARENT,
                 NULL);
   XmStringFree(label);
   if (fit_label)
      XtFree(fit_label);

   if (pixmapData)
      ShelfFreePixmapData(pixmapData);
   if (data_type)
      DtDtsFreeDataType(data_type);
   if (resolved_path)
      XtFree(resolved_path);
   if (resolved_host)
      XtFree(resolved_host);
   if (icon_name_owned)
      XtFree(icon_name_owned);
}

static void
ShelfUpdateSlotsRange(
        FileMgrRec *file_mgr_rec,
        int start_slot,
        int end_slot )
{
   ShelfUpdateSlotsRangeWithData(file_mgr_rec, NULL, start_slot, end_slot);
}

static void
ShelfUpdateSlotsRangeWithData(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data_in,
        int start_slot,
        int end_slot )
{
   int i;

   if (file_mgr_rec == NULL)
      return;

   if (start_slot < 0)
      start_slot = 0;
   if (end_slot < start_slot)
      return;

   for (i = start_slot; i <= end_slot; i++)
      ShelfUpdateSlotWidgetWithData(file_mgr_rec, file_mgr_data_in, i);
}

static void
ShelfUpdateSlotsRangeAllViews(
        int start_slot,
        int end_slot )
{
   int i;

   for (i = 0; i < shelf_view_count; i++)
   {
      FileMgrRec *view = shelf_views[i];
      if (view == NULL)
         continue;
      ShelfUpdateSlotsRange(view, start_slot, end_slot);
   }
}

static void
ShelfUpdateSlotAllViews(
        int slot )
{
   ShelfUpdateSlotsRangeAllViews(slot, slot);
}

static FileMgrRec **shelf_views = NULL;
static int shelf_view_count = 0;
static Boolean shelf_drag_active = False;
static int shelf_drag_slot = -1;
static ShelfItemData *shelf_drag_candidate = NULL;
static Widget shelf_drag_widget = NULL;
static Widget shelf_drag_context = NULL;
static Boolean shelf_rebuild_after_drag = False;
static FileMgrRec *shelf_rebuild_rec = NULL;
static Boolean shelf_rebuild_pending = False;
static XtIntervalId shelf_rebuild_timer = 0;
static Dimension shelf_row_height = 0;
static Dimension shelf_slot_width = 140;
static Dimension shelf_slot_height = 0;
static Dimension shelf_slot_margin_w = 4;
static Dimension shelf_slot_margin_h = 4;
static Dimension shelf_slot_spacing = 4;
static int shelf_move_finish_slot = -1;

static void ShelfLog(const char *fmt, ...);

struct _ShelfSlotData {
   FileMgrRec *file_mgr_rec;
   int slot;
};

static const char shelf_frame_tag[] = "shelf_frame";
static const char shelf_row_tag[] = "shelf_row";

static void
ShelfLog(const char *fmt, ...)
{
   va_list ap;

   if (getenv("DTFILE_SHELF_DEBUG") == NULL)
      return;

   fprintf(stderr, "dtfile-shelf: ");
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fflush(stderr);
}

static void
ShelfLogWidget(const char *tag, Widget w)
{
   Dimension width = 0;
   Dimension height = 0;
   Position x = 0;
   Position y = 0;
   Widget parent = NULL;

   if (getenv("DTFILE_SHELF_DEBUG") == NULL)
      return;

   if (w == NULL)
   {
      ShelfLog("%s: (null)\n", tag);
      return;
   }

   parent = XtParent(w);
   XtVaGetValues(w,
                 XmNx, &x,
                 XmNy, &y,
                 XmNwidth, &width,
                 XmNheight, &height,
                 NULL);
   ShelfLog("%s: %p name=%s managed=%d realized=%d x=%d y=%d w=%u h=%u parent=%p\n",
            tag, (void *)w, XtName(w) ? XtName(w) : "(null)",
            XtIsManaged(w), XtIsRealized(w),
            (int)x, (int)y, (unsigned)width, (unsigned)height,
            (void *)parent);
}

static void
ShelfWidgetEventEH(
        Widget w,
        XtPointer client_data,
        XEvent *event,
        Boolean *cont )
{
   const char *tag = (const char *)client_data;

   (void) cont;

   if (getenv("DTFILE_SHELF_DEBUG") == NULL)
      return;

   if (event == NULL)
      return;

   if (event->type == MapNotify)
   {
      ShelfLog("ShelfWidgetEvent: %s map %p\n",
               tag ? tag : "(null)", (void *)w);
      ShelfLogWidget("ShelfWidgetEvent: map state", w);
   }
   else if (event->type == ConfigureNotify)
   {
      XConfigureEvent *cev = (XConfigureEvent *)event;
      ShelfLog("ShelfWidgetEvent: %s configure %p x=%d y=%d w=%d h=%d\n",
               tag ? tag : "(null)", (void *)w,
               cev->x, cev->y, cev->width, cev->height);
   }
   else if (event->type == UnmapNotify)
   {
      ShelfLog("ShelfWidgetEvent: %s unmap %p\n",
               tag ? tag : "(null)", (void *)w);
   }
}

static void
ShelfItemEventEH(
        Widget w,
        XtPointer client_data,
        XEvent *event,
        Boolean *cont )
{
   ShelfItemData *item_data = (ShelfItemData *)client_data;

   (void) cont;

   if (getenv("DTFILE_SHELF_DEBUG") == NULL)
      return;

   if (event == NULL)
      return;

   if (event->type == MapNotify)
   {
      ShelfLog("ShelfItemEvent: map item=%p slot=%d path=%s\n",
               (void *)w,
               item_data ? item_data->slot : -1,
               item_data && item_data->path ? item_data->path : "(null)");
      ShelfLogWidget("ShelfItemEvent: map state", w);
   }
   else if (event->type == ConfigureNotify)
   {
      XConfigureEvent *cev = (XConfigureEvent *)event;
      ShelfLog("ShelfItemEvent: configure item=%p slot=%d x=%d y=%d w=%d h=%d\n",
               (void *)w,
               item_data ? item_data->slot : -1,
               cev->x, cev->y, cev->width, cev->height);
   }
   else if (event->type == UnmapNotify)
   {
      ShelfLog("ShelfItemEvent: unmap item=%p slot=%d\n",
               (void *)w,
               item_data ? item_data->slot : -1);
   }
}

static Boolean
ShelfAllowBtn1Transfer(Widget w)
{
   static int adjustState = -1;
   XrmValue value_return;
   char *str_type_return;

   if (adjustState == -1)
   {
      if (XrmGetResource(XrmGetDatabase(XtDisplay(w)),
                         "*enableBtn1Transfer",
                         "*EnableBtn1Transfer",
                         &str_type_return,
                         &value_return) &&
          value_return.addr &&
          !strcmp(value_return.addr, "True"))
      {
         adjustState = True;
      }
      else
      {
         adjustState = False;
      }
   }

   return adjustState ? True : False;
}

static ShelfItemData *
ShelfItemAtPoint(Widget row, int x, int y, Widget *child_out)
{
   Widget *children = NULL;
   Cardinal num_children = 0;
   int i;

   if (child_out)
      *child_out = NULL;

   XtVaGetValues(row,
                 XmNchildren, &children,
                 XmNnumChildren, &num_children,
                 NULL);
   if (children == NULL || num_children == 0)
      return NULL;

   for (i = 0; i < (int)num_children; i++)
   {
      Widget child = children[i];
      Position cx = 0, cy = 0;
      Dimension cw = 0, ch = 0;
      ShelfItemData *item_data = NULL;

      if (child == NULL)
         continue;

      XtVaGetValues(child,
                    XmNx, &cx,
                    XmNy, &cy,
                    XmNwidth, &cw,
                    XmNheight, &ch,
                    XmNuserData, &item_data,
                    NULL);

      if (item_data == NULL || item_data->path == NULL)
         continue;

      if (x < cx || y < cy)
         continue;
      if ((Dimension)(x - cx) > cw || (Dimension)(y - cy) > ch)
         continue;

      if (child_out)
         *child_out = child;
      return item_data;
   }

   return NULL;
}

static void
ShelfRowDragEH(
        Widget w,
        XtPointer client_data,
        XEvent *event,
        Boolean *cont )
{
   (void) client_data;
   (void) cont;

   if (event == NULL)
      return;

   if (event->type == ButtonPress)
   {
      XButtonEvent *bev = (XButtonEvent *)event;
      ShelfItemData *item_data = NULL;
      Widget child = NULL;

      if (bev->button == bMenuButton)
         return;

      if (bev->button == Button1 || bev->button == Button2)
      {
         item_data = ShelfItemAtPoint(w, bev->x, bev->y, &child);
         if (item_data == NULL || child == NULL)
            return;

         item_data->drag_possible = True;
         item_data->drag_button = bev->button;
         item_data->press_x = bev->x;
         item_data->press_y = bev->y;
         item_data->drag_started = False;
         shelf_drag_candidate = item_data;
         shelf_drag_widget = child;
      }
      return;
   }

   if (event->type == MotionNotify)
   {
      XMotionEvent *mev = (XMotionEvent *)event;
      int dx, dy;

      if (shelf_drag_candidate == NULL || !shelf_drag_candidate->drag_possible)
         return;

      if (shelf_drag_candidate->drag_button == Button1 &&
          !(mev->state & Button1Mask))
         return;
      if (shelf_drag_candidate->drag_button == Button2 &&
          !(mev->state & Button2Mask))
         return;

      dx = shelf_drag_candidate->press_x - mev->x;
      dy = shelf_drag_candidate->press_y - mev->y;

      if (((dx < 0 ? -dx : dx) >= dragThreshold) ||
          ((dy < 0 ? -dy : dy) >= dragThreshold))
      {
         shelf_drag_candidate->drag_started = True;
         shelf_drag_candidate->drag_possible = False;
         if (shelf_drag_widget != NULL)
            ShelfStartDrag(shelf_drag_widget, shelf_drag_candidate, event);
         shelf_drag_candidate = NULL;
         shelf_drag_widget = NULL;
      }
      return;
   }

   if (event->type == ButtonRelease)
   {
      XButtonEvent *bev = (XButtonEvent *)event;
      if (shelf_drag_candidate != NULL &&
          bev->button == shelf_drag_candidate->drag_button)
      {
         shelf_drag_candidate->drag_possible = False;
         shelf_drag_candidate->drag_button = 0;
         shelf_drag_candidate = NULL;
         shelf_drag_widget = NULL;
      }
      return;
   }
}

static void
ShelfEnforceSizeEH(
        Widget w,
        XtPointer client_data,
        XEvent *event,
        Boolean *cont )
{
   XConfigureEvent *cev;

   (void) client_data;
   (void) cont;

   if (event == NULL || event->type != ConfigureNotify)
      return;

   if (shelf_row_height <= 0)
      return;

   cev = (XConfigureEvent *)event;
   if ((Dimension)cev->height != shelf_row_height)
      XtResizeWidget(w, (Dimension)cev->width, shelf_row_height, 0);
}


/* Action for osfMenu */
static XtActionsRec FMAction[] =
{ { "FMInput", FMInput},
  { "ActivateClist", ActivateClist}
};


/*  The resources set for the FileMgr dialog  */

static DialogResource resources[] =
{
   { "show_type", SHOW_TYPE, sizeof(unsigned char),
      XtOffset(FileMgrDataPtr, show_type),
      (XtPointer) SINGLE_DIRECTORY, ShowTypeToString },

   { "tree_files", TREE_FILES, sizeof(unsigned char),
      XtOffset(FileMgrDataPtr, tree_files),
      (XtPointer) TREE_FILES_NEVER, TreeFilesToString },

   { "view_single", VIEW, sizeof(unsigned char),
      XtOffset(FileMgrDataPtr, view_single),
      (XtPointer) BY_NAME_AND_ICON, ViewToString },

   { "view_tree", VIEW, sizeof(unsigned char),
      XtOffset(FileMgrDataPtr, view_tree),
      (XtPointer) BY_NAME_AND_SMALL_ICON, ViewToString },

   { "order", ORDER, sizeof(unsigned char),
      XtOffset(FileMgrDataPtr, order),
      (XtPointer) ORDER_BY_ALPHABETICAL, OrderToString },

   { "direction", DIRECTION_RESRC, sizeof(unsigned char),
      XtOffset(FileMgrDataPtr, direction),
      (XtPointer) DIRECTION_ASCENDING, DirectionToString },

   { "positionEnabled", RANDOM, sizeof(unsigned char),
      XtOffset(FileMgrDataPtr, positionEnabled),
      (XtPointer) RANDOM_OFF, RandomToString },

   { "host", XmRString, sizeof(String),
      XtOffset(FileMgrDataPtr, host), (XtPointer) NULL, _DtStringToString },

   { "current_directory", XmRString, sizeof(String),
      XtOffset(FileMgrDataPtr, current_directory),
      (XtPointer) "~", _DtStringToString },

   { "branch_list", BRANCH_LIST, sizeof(XtPointer),
      XtOffset(FileMgrDataPtr, branch_list),
      (XtPointer) NULL, BranchListToString },

   { "restricted_directory", XmRString, sizeof(String),
      XtOffset(FileMgrDataPtr, restricted_directory),
      (XtPointer) NULL, _DtStringToString },

   { "title", XmRString, sizeof(String),
      XtOffset(FileMgrDataPtr, title),
      (XtPointer) NULL, _DtStringToString },

   { "helpVol", XmRString, sizeof(String),
      XtOffset(FileMgrDataPtr, helpVol),
      (XtPointer) NULL, _DtStringToString },

   { "selection_list", SELECTION_LIST, sizeof(XtPointer),
      XtOffset(FileMgrDataPtr, selection_list),
      (XtPointer) NULL, SelectionListToString },

   { "show_shelf", XmRBoolean, sizeof(Boolean),
      XtOffset(FileMgrDataPtr, show_shelf),
      (XtPointer) True, _DtBooleanToString },

   { "show_iconic_path", XmRBoolean, sizeof(Boolean),
      XtOffset(FileMgrDataPtr, show_iconic_path),
      (XtPointer) True, _DtBooleanToString },

   { "show_current_directory", XmRBoolean, sizeof(Boolean),
      XtOffset(FileMgrDataPtr, show_current_dir),
      (XtPointer) False, _DtBooleanToString },

   { "show_status_line", XmRBoolean, sizeof(Boolean),
      XtOffset(FileMgrDataPtr, show_status_line),
      (XtPointer) True, _DtBooleanToString },

   { "fast_cd_enabled", XmRBoolean, sizeof(Boolean),
      XtOffset(FileMgrDataPtr, fast_cd_enabled),
      (XtPointer) False, _DtBooleanToString },

   { "toolbox", XmRBoolean, sizeof(Boolean),
      XtOffset(FileMgrDataPtr, toolbox),
      (XtPointer) False, _DtBooleanToString },

   { "show_hid_enabled", XmRBoolean, sizeof(Boolean),
      XtOffset(FileMgrDataPtr, show_hid_enabled),
      (XtPointer) False, _DtBooleanToString },

   { "secondaryHelpDialogCount", XmRInt, sizeof(int),
      XtOffset(FileMgrDataPtr, secondaryHelpDialogCount),
      (XtPointer) 0, _DtIntToString },

   { "attr_dialog_count", XmRInt, sizeof(int),
      XtOffset(FileMgrDataPtr, attr_dialog_count),
      (XtPointer) 0, _DtIntToString },

   { "trashcan", XmRBoolean, sizeof(Boolean),
     XtOffset(FileMgrDataPtr, IsTrashCan),
     (XtPointer) False, _DtBooleanToString },
};


/*
 *  The Dialog Class structure.
 */

static DialogClass fileMgrClassRec =
{
   resources,
   XtNumber(resources),
   Create,
   (DialogInstallChangeProc) NULL,
   (DialogInstallCloseProc) NULL,
   Destroy,
   (DialogGetValuesProc) NULL,
   GetDefaultValues,
   GetResourceValues,
   (DialogSetValuesProc) SetValues,
   WriteResourceValues,
   (DialogFreeValuesProc) FreeValues,
   NULL,
   (DialogSetFocusProc) SetFocus,
};

DialogClass * fileMgrClass = (DialogClass *) &fileMgrClassRec;

#ifdef _CHECK_FOR_SPACES
char translations_sp_esc[] = "<Key>space:Space()\n\
                             <Key>osfCancel:EscapeFM()";
char translations_space[] = "<Key>space:Space()";
#else
char translations_sp_esc[] = "Ctrl<Key>space:Space()\n\
                             <Key>osfCancel:EscapeFM()";
char translations_space[] = "Ctrl<Key>space:Space()";
#endif
char translations_escape[] = "<Key>osfCancel:EscapeFM()";

#define DOUBLE_CLICK_DRAG
#ifndef DOUBLE_CLICK_DRAG
char translations_da[] = "\
<Key>osfCancel:DrawingAreaInput() ManagerGadgetSelect()\n\
<Key>plus:DrawingAreaInput() ManagerGadgetSelect()\n\
<Key>minus:DrawingAreaInput() ManagerGadgetSelect()\n\
<Btn2Down>:DrawingAreaInput() ManagerGadgetArm()\n\
<Btn2Down>,<Btn2Up>:DrawingAreaInput() ManagerGadgetActivate()\n\
<Btn2Up>:DrawingAreaInput() ManagerGadgetActivate()\n\
<Btn2Down>(2+):DrawingAreaInput() ManagerGadgetMultiArm()\n\
<Btn2Up>(2+):DrawingAreaInput() ManagerGadgetMultiActivate()";

#else

extern _XmConst char _XmDrawingA_traversalTranslations[];

char translations_da[] = "\
<BtnMotion>:ManagerGadgetButtonMotion()\n\
<Btn1Down>:DrawingAreaInput() ManagerGadgetArm()\n\
<Btn1Down>,<Btn1Up>:DrawingAreaInput() ManagerGadgetActivate()\n\
<Btn1Up>:DrawingAreaInput() ManagerGadgetActivate()\n\
<Btn1Down>(2+):DrawingAreaInput() ManagerGadgetMultiArm() \
               ManagerGadgetMultiActivate()\n\
<Btn2Down>:DrawingAreaInput() ManagerGadgetArm()\n\
<Btn2Down>,<Btn2Up>:DrawingAreaInput() ManagerGadgetActivate()\n\
<Btn2Up>:DrawingAreaInput() ManagerGadgetActivate()\n\
<BtnDown>:DrawingAreaInput()\n\
<BtnUp>:DrawingAreaInput()\n\
:<Key>osfActivate:DrawingAreaInput() ManagerParentActivate()\n\
:<Key>osfCancel:DrawingAreaInput() ManagerGadgetSelect()\n\
:<Key>osfHelp:DrawingAreaInput() ManagerGadgetHelp()\n\
:<Key>osfSelect:DrawingAreaInput() ManagerGadgetSelect()\n\
:<Key>osfDelete:DrawingAreaInput() ManagerGadgetSelect()\n\
:<Key>osfMenu:FMInput(@)\n\
:<Key>osfEndLine:FMInput(@)\n\
<Key>osfBeginLine:FMInput(@)\n\
:<Key>osfPageDown:FMInput(@)\n\
:<Key>osfPageUp:FMInput(@)\n\
:<Key>F1:DrawingAreaInput() ManagerGadgetHelp()\n\
s c a <Key>c: ActivateClist(@)\n\
~s ~m ~a <Key>Return:DrawingAreaInput() ManagerParentActivate() ManagerGadgetSelect()\n\
<Key>space:DrawingAreaInput() ManagerGadgetSelect()\n\
:<Key>plus:DrawingAreaInput() ManagerGadgetSelect()\n\
:<Key>minus:DrawingAreaInput() ManagerGadgetSelect()\n\
<KeyDown>:DrawingAreaInput() ManagerGadgetKeyInput()\n\
<KeyUp>:DrawingAreaInput()";
#endif /* DOUBLE_CLICK_DRAG */

/************************************************************************
 *
 *  FontListDefaultEntry
 *
 *      Return the first entry in the font list with the tag
 *      XmFONTLIST_DEFAULT_TAG.  If there isn't one, just return the
 *      first entry in the font list.
 *
 ************************************************************************/

static XmFontListEntry
FontListDefaultEntry(XmFontList font_list)
{
    XmFontContext context;
    XmFontListEntry first_entry, entry;
    char *tag;

    if (!XmFontListInitFontContext(&context, font_list))
        return NULL;

    entry = first_entry = XmFontListNextEntry(context);
    while (entry) {
        tag = XmFontListEntryGetTag(entry);
        if (!strcmp(XmFONTLIST_DEFAULT_TAG, tag)) {
            XtFree(tag);
            break;
        }

        XtFree(tag);
        entry = XmFontListNextEntry(context);
    }

    XmFontListFreeFontContext(context);

    return entry ? entry : first_entry;
}

/************************************************************************
 *
 *  Create
 *
 ************************************************************************/

static void
Create(
        Display *display,
        Widget parent,
        Widget *return_widget,
        XtPointer *dialog )
{
   static Boolean first = True;
   FileMgrRec * file_mgr_rec;
   Widget shell;
   Widget mainWidget;
   Widget menu;
   Widget header_frame;
   Widget header_separator;
   Widget shelf_separator;
   Widget iconic_path_da;
   Widget current_directory_frame;
   Widget current_directory_drop;
   Widget current_directory_icon;
   Widget directory_list_form;
   FileMgrData *file_mgr_data;
   Widget work_frame;
   Widget status_form;
   Widget status_separator;
   Widget status_line;

   Pixel background;
   Pixel foreground;
   Pixel top_shadow;
   Pixel bottom_shadow;
   Pixel select;
   Colormap colormap;

   XmFontList font_list;
   XtPointer entry_font;
   XmFontType type;
   XFontSetExtents *extents;
   Atom delete_window;
   Arg args[32];
   int n;
   int font_height;
   int curdir_height;
   int temp;
   int icon_offset, cur_dir_offset;
   Dimension shadow_thickness;
   Dimension highlight_thickness;
   XtTranslations trans_table, trans_table1;
   Dimension shelf_height;
   int shelf_icon_px = 16;


   /*  Allocate the change directory dialog instance record.  */

   file_mgr_rec = (FileMgrRec *) XtMalloc (sizeof (FileMgrRec));
   file_mgr_rec->action_pane_file_type = NULL;
  file_mgr_rec->shelf_frame = NULL;
   file_mgr_rec->shelf_separator = NULL;
   file_mgr_rec->shelf_popup = NULL;
   file_mgr_rec->shelf_popup_data = NULL;
   ShelfRegisterView(file_mgr_rec);

   /* set up translations in main edit widget */
   trans_table = XtParseTranslationTable(translations_sp_esc);
   {
     char * resolvedTranslationString;

     resolvedTranslationString = ResolveTranslationString( translations_da,
                                                           (char *)file_mgr_rec );
     trans_table1 = XtParseTranslationTable( resolvedTranslationString );
     XtFree( resolvedTranslationString );
     resolvedTranslationString  = NULL;
   }

   /*  Create the shell and main window used for the view.  */

   XtSetArg (args[0], XmNallowShellResize, True);
   if(!first && (special_view == True && special_title != NULL && !TrashView))
   {
      file_mgr_rec->shell = shell =
         XtAppCreateShell (special_title, DTFILE_CLASS_NAME,
                        applicationShellWidgetClass, display, args, 1);
      XtFree(special_title);
      special_title = NULL;
   }
   else
      file_mgr_rec->shell = shell =
         XtAppCreateShell (application_name, DTFILE_CLASS_NAME,
                        applicationShellWidgetClass, display, args, 1);
   if(!TrashView)
      special_view = False;

   n = 0;
   XtSetArg (args[n], XmNdeleteResponse, XmUNMAP);      n++;

   if (first == False || TrashView)
   {
      XtSetArg (args[n], XmNgeometry, NULL);            n++;
   }
   else
      first = False;

   /* Set the useAsyncGeo on the shell */
   XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
   XtSetValues (shell, args, n);

   delete_window = XmInternAtom (XtDisplay (shell), "WM_DELETE_WINDOW", False);
   XmAddWMProtocolCallback (shell, delete_window, (XtCallbackProc)SystemClose,
                            (XtPointer)file_mgr_rec);

   file_mgr_rec->main = mainWidget = XmCreateMainWindow (shell, "main", args, 1);
   XtManageChild (mainWidget);
   XtAddCallback(mainWidget, XmNhelpCallback, (XtCallbackProc)HelpRequestCB,
                 HELP_FILE_MANAGER_VIEW_STR);


   /*  Get the select color  */

   XtSetArg (args[0], XmNbackground, &background);
   XtSetArg (args[1], XmNcolormap,  &colormap);
   XtGetValues (mainWidget, args, 2);

   XmGetColors (XtScreen (mainWidget), colormap, background,
                &foreground, &top_shadow, &bottom_shadow, &select);

   /*  Create the menu.  */


   if(TrashView)
   {
      file_mgr_rec->menuStates = 0;
      file_mgr_rec->menu = menu = CreateTrashMenu (mainWidget, file_mgr_rec);
   }
   else
   {
      file_mgr_rec->menuStates = (RENAME | MOVE | DUPLICATE | LINK | TRASH |
                                  MODIFY | CHANGEDIR | PREFERENCES | FILTER |
                                  FIND | CREATE_DIR | CREATE_FILE | SETTINGS |
                                  CLEAN_UP | MOVE_UP |
                                  HOME | CHANGE_DIR | TERMINAL);

      file_mgr_rec->menu = menu = CreateMenu (mainWidget, file_mgr_rec);
   }

   /*  Create the work area frame.  */

   XtSetArg (args[0], XmNshadowThickness, 1);
   XtSetArg (args[1], XmNshadowType, XmSHADOW_OUT);
   XtSetArg (args[2], XmNmarginWidth, 5);
   XtSetArg (args[3], XmNmarginHeight, 5);
   work_frame = XmCreateFrame (mainWidget, "work_frame", args, 4);
   XtManageChild (work_frame);


   /*  Create the current directory frame.  */

   n = 0;
   XtSetArg (args[n], XmNshadowThickness, 1);                   n++;
   XtSetArg (args[n], XmNshadowType, XmSHADOW_OUT);             n++;
   XtSetArg (args[n], XmNmarginWidth, 1);                       n++;
   XtSetArg (args[n], XmNmarginHeight, 1);                      n++;
   XtSetArg (args[n], XmNtextTranslations, trans_table);        n++;
   file_mgr_rec->header_frame = header_frame =
         XmCreateForm (mainWidget, "header_frame", args, n);

   XtAddCallback(header_frame, XmNhelpCallback,
                    (XtCallbackProc)HelpRequestCB,
                    HELP_FILE_MANAGER_VIEW_STR);

   /*  Create the current directory line only if not in showFilesystem.  */
   if (showFilesystem && !TrashView)
   {
      /* Create the shelf container (placeholder, no contents yet) */
      (void)ShelfGetIconSize(file_mgr_data, &shelf_icon_px);
      shelf_height = (Dimension)(shelf_icon_px + 32);
      n = 0;
      XtSetArg (args[n], XmNshadowThickness, 0);                 n++;
      XtSetArg (args[n], XmNmarginWidth, 0);                     n++;
      XtSetArg (args[n], XmNmarginHeight, 0);                    n++;
      XtSetArg (args[n], XmNheight, shelf_height);               n++;
      XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);       n++;
      XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);      n++;
      XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);     n++;
      file_mgr_rec->shelf_frame =
         _DtCreateShelf (header_frame, "shelf_frame", args, n);
      XtVaSetValues(file_mgr_rec->shelf_frame, XmNuserData, file_mgr_rec, NULL);
      XtManageChild (file_mgr_rec->shelf_frame);
      shelf_row_height = shelf_height;

      /* Separator between shelf and iconic path */
      n = 0;
      XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);    n++;
      XtSetArg (args[n], XmNtopWidget, file_mgr_rec->shelf_frame); n++;
      XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);     n++;
      XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);    n++;
      file_mgr_rec->shelf_separator =
         XmCreateSeparatorGadget(header_frame, "shelf_separator", args, n);
      XtManageChild(file_mgr_rec->shelf_separator);


      /* Create the iconic path */
      n = 0;
      XtSetArg (args[n], DtNfileMgrRec, file_mgr_rec);          n++;
      XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);    n++;
      XtSetArg (args[n], XmNtopWidget, file_mgr_rec->shelf_separator);n++;
      XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);     n++;
      XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);    n++;
      file_mgr_rec->iconic_path_da = iconic_path_da =
         _DtCreateIconicPath(header_frame, "iconic_path", args, n);
      XtManageChild (iconic_path_da);
      file_mgr_rec->iconic_path_width = 0;

      /* Create the separator between iconic path and current directory */
      n = 0;
      XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);    n++;
      XtSetArg (args[n], XmNtopWidget, iconic_path_da);         n++;
      XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);     n++;
      XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);    n++;
      file_mgr_rec->header_separator = header_separator =
         XmCreateSeparatorGadget(header_frame, "header_separator", args, n);
      XtManageChild (header_separator);

      /* Create a form for the current directory */
      n = 0;
      XtSetArg (args[n], XmNtextTranslations, trans_table);     n++;
      XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);    n++;
      XtSetArg (args[n], XmNtopWidget, header_separator);       n++;
      XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);     n++;
      XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);    n++;
      file_mgr_rec->current_directory_frame = current_directory_frame =
            XmCreateForm (header_frame, "current_directory_frame", args, n);
      XtManageChild (file_mgr_rec->current_directory_frame);

      /* Event handler for detecting drag threshold surpassed */
      XtAddEventHandler(current_directory_frame,
                        Button1MotionMask|Button2MotionMask,
                        False, (XtEventHandler)CurrentDirectoryIconMotion,
                        file_mgr_rec);

      /* Create the change directory drop zone. */
      if (showDropZone)
      {
         if (change_view_pixmap == XmUNSPECIFIED_PIXMAP)
         {
            change_view_pixmap =
              _DtGetPixmap(XtScreen(current_directory_frame),
                           CHANGE_VIEW_ICON_S, foreground, background);
         }
         n = 0;
         XtSetArg (args[n], XmNstring, NULL);                      n++;
         XtSetArg (args[n], XmNshadowThickness, 2);                n++;
         XtSetArg (args[n], XmNfillOnArm, False);                  n++;
         XtSetArg (args[n], XmNhighlightThickness, 0);             n++;
         XtSetArg (args[n], XmNpixmap, change_view_pixmap);        n++;
         XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);      n++;
         XtSetArg (args[n], XmNtopOffset, 2);                      n++;
         XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);     n++;
         XtSetArg (args[n], XmNleftOffset, 5);                     n++;
         XtSetArg (args[n], XmNbottomAttachment, XmATTACH_FORM);   n++;
         XtSetArg (args[n], XmNbottomOffset, 2);                   n++;
         XtSetArg (args[n], XmNtraversalOn, False);                n++;
         XtSetArg (args[n], XmNdropSiteOperations,
                      XmDROP_MOVE | XmDROP_COPY | XmDROP_LINK);    n++;
         file_mgr_rec->current_directory_drop = current_directory_drop =
            _DtCreateIcon (current_directory_frame, "drop_cd", args, n);

         XtAddCallback (current_directory_drop, XmNdropCallback,
                        DropOnChangeView, (XtPointer) file_mgr_rec);
      }
      else
        file_mgr_rec->current_directory_drop = current_directory_drop = NULL;

      /* Create the current directory icon. */
      n = 0;
      XtSetArg (args[n], XmNstring, NULL);                      n++;
      XtSetArg (args[n], XmNimageName, SMALL_DIRECTORY_ICON);   n++;
      XtSetArg (args[n], XmNshadowThickness, 0);                n++;
      XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);      n++;
      if (showDropZone)
      {
         XtSetArg (args[n], XmNleftAttachment, XmATTACH_WIDGET);    n++;
         XtSetArg (args[n], XmNleftWidget, current_directory_drop); n++;
         XtSetArg (args[n], XmNleftOffset, 0);                      n++;
      }
      else
      {
         XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);      n++;
         XtSetArg (args[n], XmNleftOffset, 5);                      n++;
      }
      XtSetArg (args[n], XmNtraversalOn, False);                    n++;
      file_mgr_rec->current_directory_icon = current_directory_icon =
         _DtCreateIcon (current_directory_frame, "current_directory_icon",
                        args, n);

      /* Create the current directory line */
      n = 0;
      XtSetArg (args[n], XmNshadowThickness, 0);             n++;
      XtSetArg (args[n], XmNmarginWidth, 0);                 n++;
      XtSetArg (args[n], XmNmarginHeight, 0);                n++;
      XtSetArg (args[n], XmNpushButtonEnabled, False);       n++;
      XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);   n++;
      XtSetArg (args[n], XmNtopOffset, 1);                   n++;
      XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
      XtSetArg (args[n], XmNleftOffset, 5);                  n++;
      XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM); n++;
      XtSetArg (args[n], XmNrightOffset, 1);                 n++;
      XtSetArg (args[n], XmNtraversalOn, True);              n++;
      file_mgr_rec->current_directory =
        XmCreateDrawnButton(current_directory_frame, "current_directory",
                                                                      args, n);
      XtManageChild (file_mgr_rec->current_directory);


      /* Create overlay text field, for typing in a new directory */
      n = 0;
      XtSetArg (args[n], XmNmarginHeight, 0);                         n++;
      XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);            n++;
      XtSetArg (args[n], XmNtopOffset, 1);                            n++;
      XtSetArg (args[n], XmNleftAttachment, XmATTACH_OPPOSITE_WIDGET);n++;
      XtSetArg (args[n], XmNleftWidget,
                         file_mgr_rec->current_directory);            n++;
      XtSetArg (args[n], XmNleftOffset, 0);                           n++;
      XtSetArg (args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET);n++;
      XtSetArg (args[n], XmNbottomWidget,
                         file_mgr_rec->current_directory);            n++;
      XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);          n++;
      XtSetArg (args[n], XmNrightOffset, 1);                          n++;
      XtSetArg (args[n], XmNtraversalOn, True);                       n++;
      file_mgr_rec->current_directory_text = XmCreateTextField(
                     current_directory_frame, FAST_RENAME, args, n);

      /*
       * Make sure the font & highlight thickness of the text field is the
       * same as the corresponding values for the current directory line.
       */
      XtSetArg (args[0], XmNhighlightThickness, &highlight_thickness);
      XtSetArg (args[1], XmNfontList, &font_list);
      XtGetValues (file_mgr_rec->current_directory, args, 2);

      XtSetArg (args[0], XmNhighlightThickness, highlight_thickness);
      XtSetArg (args[1], XmNfontList, font_list);
      XtSetValues (file_mgr_rec->current_directory_text, args, 2);

      /*
       * Calculate how high to make the current directory line by adding
       * up the font height and the highlight and shadow thickness
       */
      XtSetArg (args[0], XmNshadowThickness, &shadow_thickness);
      XtGetValues (file_mgr_rec->current_directory_text, args, 1);

      entry_font = XmFontListEntryGetFont(FontListDefaultEntry(font_list), &type);
      if(type == XmFONT_IS_FONTSET) {
          extents = XExtentsOfFontSet((XFontSet)entry_font);
          font_height = extents->max_logical_extent.height;
      }
      else {
          font_height = ((XFontStruct *)entry_font)->ascent +
                 ((XFontStruct *)entry_font)->descent;
      }
      curdir_height = font_height + 2*(highlight_thickness + shadow_thickness);

      if (file_mgr_rec->shelf_frame != NULL)
      {
         Dimension shelf_h = (Dimension)(shelf_icon_px + font_height +
            2*(highlight_thickness + shadow_thickness) + 8);
         XtSetArg (args[0], XmNheight, shelf_h);
         XtSetValues (file_mgr_rec->shelf_frame, args, 1);
         shelf_row_height = shelf_h;
      }

      XtSetArg (args[0], XmNtopOffset, &cur_dir_offset);
      XtGetValues (file_mgr_rec->current_directory, args, 1);

      XtSetArg (args[0], XmNheight, curdir_height);
      XtSetValues (file_mgr_rec->current_directory, args, 1);

      XtSetArg (args[0], XmNheight, curdir_height + 2*cur_dir_offset);
      XtSetValues (file_mgr_rec->current_directory_frame, args, 1);

      XtAddCallback(file_mgr_rec->current_directory_text, XmNactivateCallback,
                                 (XtCallbackProc)ChangeToNewDir, file_mgr_rec);
      XtAddCallback(file_mgr_rec->current_directory_text, XmNhelpCallback,
                    (XtCallbackProc)HelpRequestCB,
                    HELP_FILE_MANAGER_VIEW_STR);
#ifdef FOO
      /* This code is here so the Text Path (used to be Fast Change Dir
         text) will be checked if user is ever try to type on the portion
         before his restricted path.
         It's no longer needed because, there won't be anything since
         I'm tryting to treat restrictMode the same way I treat restricted
         directory (i.e. dtfile -restricted).
         Why I want to treat them the same way? Because they are the
         same sort of things and it, also, solves the iconic path problem.
         Note that user can double click on one of the icon and to a directory
         above the restricted directory.
      */
      if(restrictMode)
      {
         /* add the callbacks to make sure the user isn't able to
            go anywhere above their restricted directory */
         XtAddCallback (file_mgr_rec->current_directory_text,
                        XmNmodifyVerifyCallback, (XtCallbackProc)TextChange,
                        (XtPointer)NULL );
         XtAddCallback (file_mgr_rec->current_directory_text,
                        XmNmotionVerifyCallback, (XtCallbackProc)TextChange,
                        (XtPointer)NULL );
      }
#endif
   }

   /* create the status line */
   n = 0;
   XtSetArg (args[n], XmNshadowThickness, 1);           n++;
   XtSetArg (args[n], XmNshadowType, XmSHADOW_OUT);             n++;
   XtSetArg (args[n], XmNmarginWidth, 5);                       n++;
   XtSetArg (args[n], XmNmarginHeight, 1);                      n++;
   status_form = XmCreateForm (mainWidget, "status_form", args, n);
   XtManageChild (status_form);

   n = 0;
   XtSetArg (args[n], XmNalignment, XmALIGNMENT_BEGINNING);     n++;
   XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);         n++;
   XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);        n++;
   XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);       n++;
   file_mgr_rec->status_line =
         XmCreateLabelGadget (status_form, "status_line", args, n);
   XtManageChild (file_mgr_rec->status_line);

   /*  Associate the menu and frames with the appropriate  */
   /*  areas of the main windows.                          */

   XmMainWindowSetAreas (mainWidget, menu, file_mgr_rec->header_frame,
                            NULL, NULL, work_frame);
   XtSetArg (args[0], XmNmessageWindow, status_form);
   XtSetValues(mainWidget, args, 1);

   /*  Create the Scrolled Window for the file display area and  */
   /*  set the scrollbars colors correctly.                      */

   n = 0;
   XtSetArg (args[n], XmNspacing, 2);                           n++;
   XtSetArg (args[n], XmNscrollingPolicy, XmAUTOMATIC);         n++;
   XtSetArg (args[n], XmNscrollBarDisplayPolicy, XmAS_NEEDED);  n++;
   file_mgr_rec->scroll_window =
      XmCreateScrolledWindow (work_frame, "scroll_window", args, n);
   XtManageChild (file_mgr_rec->scroll_window);
   if(TrashView)
      XtAddCallback(file_mgr_rec->scroll_window, XmNhelpCallback,
                    (XtCallbackProc)HelpRequestCB,
                     HELP_TRASH_OVERVIEW_TOPIC_STR);
   else
      XtAddCallback(file_mgr_rec->scroll_window, XmNhelpCallback,
                    (XtCallbackProc)HelpRequestCB,
                    HELP_FILE_MANAGER_VIEW_STR);

   /*  Get the 2 scrollbars and reset their foreground to the proper color  */
   /*  Also turn their traversal off and set the highlight thickness to 0.  */

   XtSetArg (args[0], XmNhorizontalScrollBar, &(file_mgr_rec->horizontal_scroll_bar));
   XtSetArg (args[1], XmNverticalScrollBar, &(file_mgr_rec->vertical_scroll_bar));
   XtGetValues (file_mgr_rec->scroll_window, args, 2);

   XtSetArg (args[0], XmNforeground, background);
   XtSetArg (args[1], XmNtraversalOn, True);
   XtSetValues (file_mgr_rec->horizontal_scroll_bar, args, 2);
   XtSetValues (file_mgr_rec->vertical_scroll_bar, args, 2);


   /*  Add an event handler to catch resizes occurring on the scrolled  */
   /*  window, in order to force a relayout of the icons.              */

   XtAddEventHandler (file_mgr_rec->scroll_window, StructureNotifyMask,
                      False, (XtEventHandler)FileWindowResizeCallback,
                      file_mgr_rec);
   XtAddEventHandler (shell, StructureNotifyMask,
                      False, (XtEventHandler)FileWindowMapCallback,
                      file_mgr_rec);


   /*  Create a drawing area as a child of the scrolled window  */

   n = 0;
   XtSetArg (args[n], XmNmarginWidth, 0);               n++;
   XtSetArg (args[n], XmNmarginHeight, 0);              n++;
   XtSetArg (args[n], XmNresizePolicy, XmRESIZE_GROW);  n++;
   file_mgr_rec->file_window =
      XmCreateDrawingArea (file_mgr_rec->scroll_window, "file_window", args, n);
   XtAppAddActions( XtWidgetToApplicationContext(file_mgr_rec->file_window),
                    FMAction,
                    XtNumber(FMAction));
   XtManageChild (file_mgr_rec->file_window);

   XtManageChild (file_mgr_rec->header_frame);

   XtSetArg (args[0], XmNbackground, select);
   XtSetValues (file_mgr_rec->file_window, args, 1);

   XtSetArg (args[0], XmNbackground, select);
   XtSetValues (XtParent (file_mgr_rec->file_window), args, 1);

   if(fileMgrPopup.menu == NULL)
      CreateFmPopup(file_mgr_rec->file_window);
   else
      XmAddToPostFromList(fileMgrPopup.menu, file_mgr_rec->file_window);

   /* set up translations in main edit widget */
#ifdef DOUBLE_CLICK_DRAG
   XtUninstallTranslations(file_mgr_rec->file_window);
   XtOverrideTranslations(file_mgr_rec->file_window,
     (XtTranslations)
       ((XmManagerClassRec *)XtClass(file_mgr_rec->file_window))
                                      ->manager_class.translations);
   XtOverrideTranslations(file_mgr_rec->file_window, trans_table1);
   XtOverrideTranslations(file_mgr_rec->file_window,
        XtParseTranslationTable(_XmDrawingA_traversalTranslations));
#else
   XtOverrideTranslations(file_mgr_rec->file_window, trans_table1);
#endif

   /*  Add the callbacks for processing selections, exposures,  */
   /*  and geometry changes of the drawing area.                */

   XtAddCallback (file_mgr_rec->file_window, XmNexposeCallback,
                  FileWindowExposeCallback, file_mgr_rec);

   XtAddCallback (file_mgr_rec->file_window, XmNinputCallback,
                     FileWindowInputCallback, file_mgr_rec);

   /* Event handler for detecting drag threshold surpassed */
   XtAddEventHandler(file_mgr_rec->file_window,
                     Button1MotionMask|Button2MotionMask,
                     False, (XtEventHandler)FileIconMotion,
                     file_mgr_rec);

   /* Arrange for initial focus to be inside the file window */
   file_mgr_rec->focus_widget = file_mgr_rec->file_window;

   /*  Set the return values for the dialog widget and dialog instance.  */

   *return_widget = (Widget) mainWidget;
   *dialog = (XtPointer) file_mgr_rec;
}


/************************************************************************
 *
 *  GetDefaultValues
 *
 ************************************************************************/

static XtPointer
GetDefaultValues( void )
{
   FileMgrData * file_mgr_data;
   PreferencesData *preferences_data;
   unsigned char *viewP;


   /*  Allocate and initialize the default change dir dialog data.  */

   file_mgr_data = (FileMgrData *) XtMalloc (sizeof (FileMgrData));

   file_mgr_data->displayed = False;
   file_mgr_data->mapped = False;
   file_mgr_data->x = 0;
   file_mgr_data->y = 0;

   file_mgr_data->busy_status = not_busy;
   file_mgr_data->busy_detail = 0;

   file_mgr_data->num_objects = 0;
   file_mgr_data->object_positions = NULL;
   file_mgr_data->grid_height = 0;
   file_mgr_data->grid_width = 0;
   file_mgr_data->layout_data = NULL;

   file_mgr_data->special_msg = NULL;
   file_mgr_data->msg_timer_id = 0;

   file_mgr_data->show_type = SINGLE_DIRECTORY;
   file_mgr_data->tree_files = TREE_FILES_NEVER;
   file_mgr_data->view_single = BY_NAME_AND_ICON;
   file_mgr_data->view_tree = BY_NAME_AND_SMALL_ICON;
   file_mgr_data->tree_preread_level = 1;
   file_mgr_data->tree_show_level = 1;
   file_mgr_data->show_shelf = True;
   file_mgr_data->show_iconic_path = True;
   file_mgr_data->show_current_dir = True;
   file_mgr_data->show_status_line = True;

   file_mgr_data->scrollToThisFile = NULL;
   file_mgr_data->scrollToThisDirectory = NULL;

   file_mgr_data->renaming = NULL;

   if(special_view && !TrashView)
   {
      if(special_title != NULL)
      {
         file_mgr_data->title = XtNewString(special_title);
      }
      else
         file_mgr_data->title = NULL;

      if(special_helpVol != NULL)
      {
         file_mgr_data->helpVol = XtNewString(special_helpVol);
         XtFree(special_helpVol);
         special_helpVol = NULL;
      }
      else
         file_mgr_data->helpVol = XtNewString(fileMgrHelpVol);

      if(special_restricted != NULL)
        file_mgr_data->restricted_directory = special_restricted;
      else
        file_mgr_data->restricted_directory = NULL;

      if(special_treeType == UNSET_VALUE)
         file_mgr_data->show_type = SINGLE_DIRECTORY;
      else
         file_mgr_data->show_type = special_treeType;

      if(special_treeFiles == UNSET_VALUE)
         file_mgr_data->tree_files = TREE_FILES_NEVER;
      else
         file_mgr_data->tree_files = special_treeFiles;

      if(file_mgr_data->show_type == MULTIPLE_DIRECTORY)
        viewP = &file_mgr_data->view_tree;
      else
        viewP = &file_mgr_data->view_single;
      if(special_viewType == UNSET_VALUE)
         file_mgr_data->view = *viewP;
      else
         file_mgr_data->view = *viewP = special_viewType;

      if(special_orderType == UNSET_VALUE)
         file_mgr_data->order = ORDER_BY_ALPHABETICAL;
      else
         file_mgr_data->order = special_orderType;

      if(special_directionType == UNSET_VALUE)
         file_mgr_data->direction = DIRECTION_ASCENDING;
      else
         file_mgr_data->direction = special_directionType;

      if(special_randomType == UNSET_VALUE)
         file_mgr_data->positionEnabled = RANDOM_OFF;
      else
         file_mgr_data->positionEnabled = special_randomType;

   }
   else
   {
      if(fileMgrTitle == NULL)
         file_mgr_data->title = NULL;
      else
         file_mgr_data->title = XtNewString(fileMgrTitle);

      file_mgr_data->helpVol = XtNewString(fileMgrHelpVol);

      if(treeType == UNSET_VALUE || TrashView)
         file_mgr_data->show_type = SINGLE_DIRECTORY;
      else
         file_mgr_data->show_type = treeType;

      if(treeFiles == UNSET_VALUE)
         file_mgr_data->tree_files = TREE_FILES_NEVER;
      else
         file_mgr_data->tree_files = treeFiles;

      if(file_mgr_data->show_type == MULTIPLE_DIRECTORY)
        viewP = &file_mgr_data->view_tree;
      else
        viewP = &file_mgr_data->view_single;
      if(viewType == UNSET_VALUE)
         file_mgr_data->view = *viewP;
      else
         file_mgr_data->view = *viewP = viewType;

      if(orderType == UNSET_VALUE)
         file_mgr_data->order = ORDER_BY_ALPHABETICAL;
      else
         file_mgr_data->order = orderType;

      if(directionType == UNSET_VALUE)
         file_mgr_data->direction = DIRECTION_ASCENDING;
      else
         file_mgr_data->direction = directionType;

      if(randomType == UNSET_VALUE)
         file_mgr_data->positionEnabled = RANDOM_OFF;
      else
         file_mgr_data->positionEnabled = randomType;

      file_mgr_data->restricted_directory = NULL;
   }

   file_mgr_data->restoreKind = NORMAL_RESTORE;

   file_mgr_data->host = NULL;
   file_mgr_data->current_directory = NULL;
   file_mgr_data->branch_list = NULL;

   file_mgr_data->toolbox = False;
   file_mgr_data->dropSite = False;
   file_mgr_data->shelfDropSite = False;

   file_mgr_data->newSize = True;

   file_mgr_data->directory_count = 0;
   file_mgr_data->directory_set = NULL;
   file_mgr_data->tree_root = NULL;

   file_mgr_data->selection_list =
      (FileViewData **) XtMalloc (sizeof (FileViewData *));
   file_mgr_data->selection_list[0] = NULL;

   file_mgr_data->selection_table = NULL;
   file_mgr_data->selected_file_count = 0;

   file_mgr_data->fast_cd_enabled = False;
   file_mgr_data->show_hid_enabled = False;

   file_mgr_data->cd_inited = False;
   file_mgr_data->cd_font = NULL;
   file_mgr_data->cd_fontset = NULL;
   file_mgr_data->cd_normal_gc = 0;
   file_mgr_data->cd_select_gc = 0;
   file_mgr_data->cd_select = NULL;

   file_mgr_data->tree_solid_thin_gc = 0;
   file_mgr_data->tree_solid_thick_gc = 0;
   file_mgr_data->tree_dash_thin_gc = 0;
   file_mgr_data->tree_dash_thick_gc = 0;

   file_mgr_data->file_mgr_rec = NULL;
   file_mgr_data->popup_menu_icon = NULL;
   file_mgr_data->drag_file_view_data = NULL;

   file_mgr_data->change_dir = _DtGetDefaultDialogData (change_dir_dialog);
   file_mgr_data->preferences = _DtGetDefaultDialogData (preferences_dialog);

   /* now set up the preferences the way they really are */
   preferences_data = (PreferencesData *)file_mgr_data->preferences->data;
   preferences_data->show_type = file_mgr_data->show_type;
   preferences_data->tree_files = file_mgr_data->tree_files;
   preferences_data->view_single = file_mgr_data->view_single;
   preferences_data->view_tree = file_mgr_data->view_tree;
   preferences_data->order = file_mgr_data->order;
   preferences_data->direction = file_mgr_data->direction;
   preferences_data->positionEnabled = file_mgr_data->positionEnabled;
   preferences_data->show_shelf = file_mgr_data->show_shelf;
   preferences_data->show_iconic_path = file_mgr_data->show_iconic_path;
   preferences_data->show_current_dir = file_mgr_data->show_current_dir;
   preferences_data->show_status_line = file_mgr_data->show_status_line;

   file_mgr_data->filter_edit = _DtGetDefaultDialogData (filter_dialog);
   file_mgr_data->filter_active = _DtGetDefaultDialogData (filter_dialog);
   file_mgr_data->find = _DtGetDefaultDialogData (find_dialog);
   file_mgr_data->attr_dialog_list = NULL;
   file_mgr_data->attr_dialog_count = 0;
   file_mgr_data->secondaryHelpDialogList = NULL;
   file_mgr_data->secondaryHelpDialogCount = 0;
   file_mgr_data->primaryHelpDialog = NULL;

   return ((XtPointer) file_mgr_data);
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
   static Boolean first = True;
   FileMgrData * file_mgr_data;
   PreferencesData *preferences_data;
   char * host_name;
   char * directory_name;
   char * new_name_list[20];
   int name_list_count;
   struct passwd * pwInfo;
   char * homeDir;
   char number[10];
   int i, j, count;
   char * tmpStr;

   if (first)
   {
      first = False;

      XtAppAddConverter (XtWidgetToApplicationContext (toplevel), XmRString,
                         SHOW_TYPE, StringToShowType, NULL, 0);
      XtAppAddConverter (XtWidgetToApplicationContext (toplevel), XmRString,
                         TREE_FILES, StringToTreeFiles, NULL, 0);
      XtAppAddConverter (XtWidgetToApplicationContext (toplevel), XmRString,
                         VIEW, StringToView, NULL, 0);
      XtAppAddConverter (XtWidgetToApplicationContext (toplevel), XmRString,
                         ORDER, StringToOrder, NULL, 0);
      XtAppAddConverter (XtWidgetToApplicationContext (toplevel), XmRString,
                         DIRECTION_RESRC, StringToDirection, NULL, 0);
      XtAppAddConverter (XtWidgetToApplicationContext (toplevel), XmRString,
                         RANDOM, StringToRandom, NULL, 0);
      XtAppSetTypeConverter (XtWidgetToApplicationContext (toplevel), XmRString,
                             BRANCH_LIST, (XtTypeConverter)StringToBranchList,
                             NULL, 0, XtCacheNone, NULL);
      XtAppAddConverter (XtWidgetToApplicationContext (toplevel), XmRString,
                         SELECTION_LIST, (XtConverter)StringToSelectionList,
                         NULL, 0);
   }


   /*  Allocate and get the resources for change dir dialog data.  */

   file_mgr_data = (FileMgrData *) XtMalloc (sizeof (FileMgrData));

   file_mgr_data->busy_status = not_busy;
   file_mgr_data->busy_detail = 0;
   file_mgr_data->num_objects = 0;
   file_mgr_data->object_positions = NULL;
   file_mgr_data->grid_height = 0;
   file_mgr_data->grid_width = 0;
   file_mgr_data->layout_data = NULL;
   file_mgr_data->secondaryHelpDialogList = NULL;
   file_mgr_data->secondaryHelpDialogCount = 0;
   file_mgr_data->attr_dialog_count = 0;
   file_mgr_data->attr_dialog_list = NULL;
   file_mgr_data->primaryHelpDialog = NULL;
   file_mgr_data->popup_menu_icon = NULL;
   file_mgr_data->title = NULL;
   file_mgr_data->helpVol = NULL;
   file_mgr_data->restricted_directory = NULL;
   file_mgr_data->toolbox = False;
   file_mgr_data->dropSite = False;
   file_mgr_data->shelfDropSite = False;
   file_mgr_data->tree_preread_level = 1;  /* @@@ make these resources? */
   file_mgr_data->tree_show_level = 1;
   file_mgr_data->tree_files = TREE_FILES_NEVER;
   file_mgr_data->special_msg = NULL;
   file_mgr_data->msg_timer_id = 0;
   file_mgr_data->show_shelf = True;
   file_mgr_data->show_iconic_path = True;
   file_mgr_data->show_current_dir = True;
   file_mgr_data->show_status_line = True;
   file_mgr_data->scrollToThisFile = NULL;
   file_mgr_data->scrollToThisDirectory = NULL;
   file_mgr_data->renaming = NULL;

   _DtDialogGetResources (data_base, name_list, FILEMGR, (char *)file_mgr_data,
                       resources, fileMgrClass->resource_count);
   if (file_mgr_data->show_type == MULTIPLE_DIRECTORY)
     file_mgr_data->view = file_mgr_data->view_tree;
   else
     file_mgr_data->view = file_mgr_data->view_single;

   /* Bases on the type of restore session we are doing we must
    * adjust the host, current_directory, and selections to the
    * proper values (e.g. restore session or restore settings)
    */
    if (restoreType != NORMAL_RESTORE)
    {
      GetSessionDir(file_mgr_data);

      /* Adjust the selected files to null */
      file_mgr_data->selection_list = NULL;

      /* Set the Displayed to false so we do not use the x and y
         values when we place the window in showDialog
       */
      file_mgr_data->displayed = False;

      if(special_view && !TrashView)
      {
         if(special_title != NULL)
            file_mgr_data->title = XtNewString(special_title);
         else
            file_mgr_data->title = XtNewString(file_mgr_data->title);

         if(special_helpVol != NULL)
            file_mgr_data->helpVol = XtNewString(special_helpVol);
         else
            file_mgr_data->helpVol = XtNewString(file_mgr_data->helpVol);

         if(special_treeType != UNSET_VALUE)
            file_mgr_data->show_type = special_treeType;

         if(special_treeFiles != UNSET_VALUE)
            file_mgr_data->tree_files = special_treeFiles;

         if(special_viewType != UNSET_VALUE)
         {
           file_mgr_data->view = special_viewType;
           if(file_mgr_data->show_type == MULTIPLE_DIRECTORY)
              file_mgr_data->view_tree = special_viewType;
           else
              file_mgr_data->view_single = special_viewType;
         }

         if(special_orderType != UNSET_VALUE)
            file_mgr_data->order = special_orderType;

         if(special_directionType != UNSET_VALUE)
            file_mgr_data->direction = special_directionType;

         if(special_randomType != UNSET_VALUE)
            file_mgr_data->positionEnabled = special_randomType;
      }
      else
      {
         file_mgr_data->title = XtNewString(file_mgr_data->title);
         file_mgr_data->helpVol = XtNewString(file_mgr_data->helpVol);
      }
    }
    else
    {
      GetSessionDir(file_mgr_data);

      file_mgr_data->title = XtNewString(file_mgr_data->title);
      file_mgr_data->helpVol = XtNewString(file_mgr_data->helpVol);
    }

   /* The GetSessionDir() function depends on the hostname being set. */
   /* So, we'll do it earlier */
   if(file_mgr_data->host != NULL)
      file_mgr_data->host = XtNewString(file_mgr_data->host);
   else
      file_mgr_data->host = XtNewString(home_host_name);

   file_mgr_data->restoreKind = NORMAL_RESTORE;

   file_mgr_data->directory_count = 0;
   file_mgr_data->directory_set = NULL;
   file_mgr_data->tree_root = NULL;

   file_mgr_data->selected_file_count = 0;
   file_mgr_data->selection_table = (char **) file_mgr_data->selection_list;
   file_mgr_data->selection_list =
      (FileViewData **) XtMalloc (sizeof (FileViewData *));
   file_mgr_data->selection_list[0] = NULL;

   file_mgr_data->cd_inited = False;
   file_mgr_data->cd_font = NULL;
   file_mgr_data->cd_fontset = NULL;
   file_mgr_data->cd_normal_gc = 0;
   file_mgr_data->cd_select_gc = 0;
   file_mgr_data->cd_select = NULL;

   file_mgr_data->popup_menu_icon = NULL;
   file_mgr_data->drag_file_view_data = NULL;

   file_mgr_data->tree_solid_thin_gc = 0;
   file_mgr_data->tree_solid_thick_gc = 0;
   file_mgr_data->tree_dash_thin_gc = 0;
   file_mgr_data->tree_dash_thick_gc = 0;

   host_name = XtNewString (file_mgr_data->host);
   directory_name = XtNewString (file_mgr_data->current_directory);

   FileMgrBuildDirectories (file_mgr_data, host_name, directory_name);

   XtFree ((char *) host_name);
   host_name = NULL;
   XtFree ((char *) directory_name);

   if (restoreType == NORMAL_RESTORE &&
                       file_mgr_data->positionEnabled == RANDOM_ON)
   {
      /* Restore the optional positional data */
      RestorePositionalData(data_base, name_list, file_mgr_data, FILEMGR);
   }


   /*  Get a copy of the name list to be used to build new name lists.  */

   _DtDuplicateDialogNameList(name_list, new_name_list, 20, &name_list_count);

   /* Load sub-dialogs */

   /* Get secondary help dialogs */
   if(restoreType == NORMAL_RESTORE &&
                     file_mgr_data->secondaryHelpDialogCount > 0)
   {
      _DtLoadSubdialogArray(new_name_list, name_list_count,
                         &(file_mgr_data->secondaryHelpDialogList),
                         file_mgr_data->secondaryHelpDialogCount,
                         help_dialog, data_base, 1);
   }
   else
   {
      file_mgr_data->secondaryHelpDialogList = NULL;
      file_mgr_data->secondaryHelpDialogCount = 0;
   }

   /* Get primary help dialog */
   new_name_list[name_list_count] = number;
   new_name_list[name_list_count + 1] = NULL;
   sprintf(number, "%d", 0);
   if (restoreType == NORMAL_RESTORE)
   {
      file_mgr_data->primaryHelpDialog =
                _DtGetResourceDialogData(help_dialog, data_base, new_name_list);
      if(((DialogInstanceData *)
                 (file_mgr_data->primaryHelpDialog->data))->displayed == False)
      {
         _DtFreeDialogData( file_mgr_data->primaryHelpDialog );
         file_mgr_data->primaryHelpDialog = NULL;
      }
   }
   else
      file_mgr_data->primaryHelpDialog = NULL;

   if (restoreType == NORMAL_RESTORE &&
                     file_mgr_data->attr_dialog_count > 0)
   {
      /* Get properties dialogs */
      _DtLoadSubdialogArray(new_name_list, name_list_count,
                         &(file_mgr_data->attr_dialog_list),
                         file_mgr_data->attr_dialog_count,
                         mod_attr_dialog, data_base, 1);
   }
   else
   {
      file_mgr_data->attr_dialog_list = NULL;
      file_mgr_data->attr_dialog_count = 0;
   }
   new_name_list[name_list_count] = NULL;

   file_mgr_data->change_dir =
      _DtGetResourceDialogData (change_dir_dialog, data_base, name_list);

   file_mgr_data->preferences =
      _DtGetResourceDialogData (preferences_dialog, data_base, name_list);

   /* Set all dialogs displayed flag for change_dir, and preferences
    * to false if not NORMAL_RESTORE
    */
   if (restoreType != NORMAL_RESTORE)
   {
      ((DialogInstanceData *)
              (file_mgr_data->change_dir->data))->displayed = False;

      ((DialogInstanceData *)
              (file_mgr_data->preferences->data))->displayed = False;
   }

   /* now set up the preferences the way they really are */
   preferences_data = (PreferencesData *)file_mgr_data->preferences->data;
   preferences_data->show_type = file_mgr_data->show_type;
   preferences_data->tree_files = file_mgr_data->tree_files;
   preferences_data->view_single = file_mgr_data->view_single;
   preferences_data->view_tree = file_mgr_data->view_tree;
   preferences_data->order = file_mgr_data->order;
   preferences_data->direction = file_mgr_data->direction;
   preferences_data->positionEnabled = file_mgr_data->positionEnabled;
   preferences_data->show_shelf = file_mgr_data->show_shelf;
   preferences_data->show_iconic_path = file_mgr_data->show_iconic_path;
   preferences_data->show_current_dir = file_mgr_data->show_current_dir;
   preferences_data->show_status_line = file_mgr_data->show_status_line;

   /*  Build the name list for the file dialog that is displayed,  */
   /*  get resource data and perform and other initialization.     */

   tmpStr = GetSharedMessage(FILTER_EDIT_ID_STRING);
   new_name_list[name_list_count] = XtNewString (tmpStr);
   new_name_list[name_list_count + 1] = NULL;

   file_mgr_data->filter_edit =
      _DtGetResourceDialogData (filter_dialog, data_base, new_name_list);

   XtFree ((char *) new_name_list[name_list_count]);
   new_name_list[name_list_count] = NULL;


   /*  Build the name list for the filter dialog that is used for the  */
   /*  current processing values.  Get the resource data and perform   */
   /*  and other initialization.                                       */

   tmpStr = GetSharedMessage(FILTER_ACTIVE_ID_STRING);
   new_name_list[name_list_count] = XtNewString (tmpStr);
   new_name_list[name_list_count + 1] = NULL;

   file_mgr_data->filter_active =
      _DtGetResourceDialogData (filter_dialog, data_base, new_name_list);

   XtFree ((char *) new_name_list[name_list_count]);
   new_name_list[name_list_count] = NULL;

   /*  Get the resource data for the find dialog and perform  */
   /*  additional initialization.                             */

   file_mgr_data->find =
      _DtGetResourceDialogData (find_dialog, data_base, name_list);


   /* Set the FilterDialog and Find dialog displayed flag to false if
    * not NORMAL_RESTORE
    */

   if (restoreType != NORMAL_RESTORE)
   {
      ((DialogInstanceData *)
              (file_mgr_data->filter_edit->data))->displayed = False;

      ((DialogInstanceData *)
              (file_mgr_data->find->data))->displayed = False;

   }

   return ((XtPointer) file_mgr_data);
}

/************************************************************************
 *
 *  SetValues
 *
 ************************************************************************/

static void
SetValues(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data )
{
   Widget cd = file_mgr_rec->current_directory;
   Widget cdi = file_mgr_rec->current_directory_icon;
   Widget cdd = file_mgr_rec->current_directory_drop;
   Widget widg;
   XmFontList font_list;
   XtPointer entry_font;
   XmFontType type;
   Pixel foreground;
   Pixel background;
   XGCValues gc_values;
   int gc_mask;
   ChangeDirData * change_dir_data;
   ModAttrRec *modAttr_rec;
   DialogData *attr_dialog;
   ModAttrData *attr_data;
   Boolean loadOk;

   int i;
   int j;
   int k;
   DirectorySet ** directory_set;
   FileViewData ** order_list;
   int directory_count;
   char * file_name;
   char * realPath;
   char * textString;
   char *tmpStr, *tempStr;
   Arg args[8];
   PixmapData *pixmapData;

   file_mgr_data->file_mgr_rec = (XtPointer) file_mgr_rec;

   /* this is what we did for the ultrix port to get the name of the
      Application dir right, BUT it has some bugs */
   if(file_mgr_data->title != NULL)
   {
      XtSetArg (args[0], XmNtitle, file_mgr_data->title);
      XtSetValues(file_mgr_rec->shell, args, 1);
   }

   /*  Set up the callbacks for the current directory line only if
       not in showFilesystem.  */

   if(showFilesystem && !TrashView)
   {
      XtRemoveAllCallbacks (cd, XmNarmCallback);
      XtAddCallback (cd, XmNarmCallback, CurrentDirSelected, file_mgr_rec);

      XtRemoveAllCallbacks (cd, XmNexposeCallback);
      XtAddCallback (cd, XmNexposeCallback, CurrentDirExposed, file_mgr_rec);

      XtRemoveAllCallbacks (cdi, XmNcallback);
      XtAddCallback (cdi, XmNcallback, CurrentDirIconCallback, file_mgr_rec);

      if (cdd)
      {
         XtRemoveAllCallbacks (cdd, XmNcallback);
         XtAddCallback (cdd, XmNcallback, CurrentDirDropCallback, file_mgr_rec);
      }

      DtUpdateIconicPath(file_mgr_rec, file_mgr_data, False);
   }

   /*
    * To help our menu callbacks get the file_mgr_rec when they are
    * activated, we will register the file_mgr_rec pointer as the
    * user_data for the menubar.  That way, since the callbacks can
    * find out which menubar they were invoked from, they can easily
    * map this into the file_mgr_rec.
    */
   XtSetArg(args[0], XmNuserData, file_mgr_rec);
   XtSetValues(file_mgr_rec->menu, args, 1);
   if(fileMgrPopup.menu != NULL)
      XtSetValues(fileMgrPopup.menu, args, 1);


   /*
    * Adjust some widgets, in case this is a recycled dialog, which
    * previously had been used as a desktop view, or vice-versa.
    */

   XtManageChild(file_mgr_rec->header_frame);
   XtManageChild(file_mgr_rec->directoryBarBtn);


   if(showFilesystem && !TrashView)
   {
      widg = cd;
      XtSetArg (args[0], XmNallowShellResize, False);
      XtSetValues(file_mgr_rec->shell, args, 1);
   }
   else
      widg = file_mgr_rec->directoryBarBtn;


   XtSetArg (args[0], XmNforeground, &foreground);
   XtSetArg (args[1], XmNbackground, &background);
   XtSetArg (args[2], XmNfontList, &font_list);
   XtGetValues (widg, args, 3);

   /*  If the current_directory data has not been initialized  */
   /*  for this data record, do it now.                        */

   if (file_mgr_data->cd_inited == False)
   {
      file_mgr_data->cd_inited = True;

      entry_font = XmFontListEntryGetFont(FontListDefaultEntry(font_list), &type);
      if(type == XmFONT_IS_FONTSET) {
          file_mgr_data->cd_fontset = (XFontSet)entry_font;
          file_mgr_data->cd_fonttype = XmFONT_IS_FONTSET;
          gc_mask = GCForeground | GCBackground;
      }
      else {
          file_mgr_data->cd_font = (XFontStruct *)entry_font;
          file_mgr_data->cd_fonttype = XmFONT_IS_FONT;
          gc_values.font = ((XFontStruct *)entry_font)->fid;
          gc_mask = GCForeground | GCBackground | GCFont;
      }

      gc_values.foreground = foreground;
      gc_values.background = background;
      file_mgr_data->cd_normal_gc = XtGetGC (widg, gc_mask, &gc_values);

      gc_values.foreground = background;
      gc_values.background = foreground;
      file_mgr_data->cd_select_gc = XtGetGC (widg, gc_mask, &gc_values);

      /* graphics contexts for drawing tree lines */
      gc_values.foreground = foreground;
      XtSetArg (args[0], XmNbackground, &gc_values.background);
      XtGetValues (file_mgr_rec->file_window, args, 1);
      gc_values.line_width = 2;
      gc_values.line_style = LineOnOffDash;
      gc_values.cap_style = CapNotLast;
      gc_values.dashes = TREE_DASH_WIDTH;

      gc_mask |= GCCapStyle;

      file_mgr_data->tree_solid_thin_gc =
         XtGetGC (widg, gc_mask, &gc_values);
      file_mgr_data->tree_solid_thick_gc =
         XtGetGC (widg, gc_mask | GCLineWidth, &gc_values);
      file_mgr_data->tree_dash_thin_gc =
         XtGetGC (widg, gc_mask | GCLineStyle | GCDashList, &gc_values);
      file_mgr_data->tree_dash_thick_gc =
         XtGetGC (widg, gc_mask | GCLineWidth |
                  GCLineStyle | GCDashList, &gc_values);
   }


   if(showFilesystem && !TrashView)
   {
      /*  Set the current directory icon to normal colors  */
      SetToNormalColors (file_mgr_rec->current_directory_icon,
                         file_mgr_rec->file_window, file_mgr_rec->main, 0);


      /*  Update the Change Directory host name  */
      change_dir_data = (ChangeDirData *) file_mgr_data->change_dir->data;
      if (change_dir_data->host_name != NULL)
         XtFree ((char *) change_dir_data->host_name);
      change_dir_data->host_name = XtNewString (file_mgr_data->host);

      /* Display the correct small directory icon */
      pixmapData = GetPixmapData(file_mgr_rec,
                                 file_mgr_data,
                                 file_mgr_data->current_directory,
                                 SMALL);
      if (pixmapData)
        XtSetArg (args[0], XmNimageName, pixmapData->iconFileName);
      else
        XtSetArg (args[0], XmNimageName, NULL);
      XtSetValues(cdi, args, 1);

      _DtCheckAndFreePixmapData(
         GetDirectoryLogicalType(file_mgr_data,
                                 file_mgr_data->current_directory),
         file_mgr_rec->shell,
         (DtIconGadget) cdi,
         pixmapData);

      if(file_mgr_data->restricted_directory == NULL)
          textString = XtNewString(file_mgr_data->current_directory);
      else
      {
         char *ptr;

         ptr = file_mgr_data->current_directory +
                        strlen(file_mgr_data->restricted_directory);
         if(strcmp(ptr, "") == 0)
             textString = XtNewString( "/" );
         else
             textString = XtNewString( ptr );
      }
      XmTextFieldSetString(file_mgr_rec->current_directory_text, textString);

      /* Clear the change dir text field */
      if (file_mgr_data->fast_cd_enabled)
         XtManageChild(file_mgr_rec->current_directory_text);
      else
         XtUnmanageChild(file_mgr_rec->current_directory_text);
      XtSetArg (args[0], XmNallowShellResize, True);
      XtSetValues(file_mgr_rec->shell, args, 1);
      XtFree(textString);
   }


   /*  Get the file set displayed  */
   FileMgrRedisplayFiles (file_mgr_rec, file_mgr_data, True);


   /*  Set the icon name for this view to the directory name.  */
   SetIconAttributes ((FileMgrRec *)file_mgr_data->file_mgr_rec, file_mgr_data,
		      file_mgr_data->current_directory);


   /*  Process the selection table into a selection list  */

   if (file_mgr_data->selection_table != NULL)
   {
      DeselectAllFiles (file_mgr_data);

      directory_set = file_mgr_data->directory_set;

      if (file_mgr_data->show_type == SINGLE_DIRECTORY)
         directory_count = 1;
      else
         directory_count = file_mgr_data->directory_count;

      i = 0;
      while (file_mgr_data->selection_table[i] != NULL)
      {
         for (j = 0; j < directory_count; j++)
         {
            if (strncmp (directory_set[j]->name,
                         file_mgr_data->selection_table[i],
                         strlen (directory_set[j]->name)) == 0)
            {
               file_name = strrchr(file_mgr_data->selection_table[i], '/') +1;
               order_list = directory_set[j]->order_list;

               for (k = 0; k < directory_set[j]->file_count; k++)
               {
                  if (strcmp (file_name,
                      order_list[k]->file_data->file_name) == 0)
                  {
                     if (order_list[k]->filtered == False)
                        SelectFile (file_mgr_data, order_list[k]);
                     break;
                  }
               }
            }
         }

         i++;
      }

      file_mgr_data->selection_table = NULL;
   }

   /* Initially, all menubuttons are sensitive */

   file_mgr_rec->menuStates = (RENAME | MOVE | DUPLICATE | LINK | TRASH |
                               MODIFY | CHANGEDIR | PREFERENCES | FILTER |
                               FIND | CREATE_DIR | CREATE_FILE | SETTINGS |
                               CLEAN_UP | MOVE_UP |
                               HOME | CHANGE_DIR | TERMINAL);

   if(file_mgr_data != trashFileMgrData
      && file_mgr_data != NULL )
   {
      if (file_mgr_data->selected_file_count == 0)
         ActivateNoSelect (file_mgr_rec);
      else if (file_mgr_data->selected_file_count == 1)
         ActivateSingleSelect (file_mgr_rec,
           file_mgr_data->selection_list[0]->file_data->logical_type);
      else
         ActivateMultipleSelect (file_mgr_rec);
   }
   else
      SensitizeTrashBtns();

   /*  Display any dialogs that need to be displayed  */
   if (((DialogInstanceData *)
        (file_mgr_data->change_dir->data))->displayed == True)
   {
      ShowChangeDirDialog ((Widget)NULL, (XtPointer)file_mgr_rec,
			   (XtPointer)NULL);
   }
   else
      file_mgr_rec->menuStates |= CHANGEDIR;


   if (((DialogInstanceData *)
        (file_mgr_data->preferences->data))->displayed == True)
   {
      ShowPreferencesDialog ((Widget)NULL, (XtPointer)file_mgr_rec,
			     (XtPointer)NULL);
   }
   else
      file_mgr_rec->menuStates |= PREFERENCES;

   if (((DialogInstanceData *)
        (file_mgr_data->find->data))->displayed == True)
   {
      ShowFindDialog ((Widget)NULL, (XtPointer)file_mgr_rec, (XtPointer)NULL);
   }
   else
      file_mgr_rec->menuStates |= FIND;


   if (((DialogInstanceData *)
        (file_mgr_data->filter_edit->data))->displayed == True)
   {
      ShowFilterDialog ((Widget)NULL, (XtPointer)file_mgr_rec, (XtPointer)NULL);
   }
   else
      file_mgr_rec->menuStates |= FILTER;

   /* Display any properties dialogs */
   for (i = 0; i < file_mgr_data->attr_dialog_count; i++)
   {
      if (((DialogInstanceData *)
           (file_mgr_data->attr_dialog_list[i]->data))->displayed == False)
      {
         continue;
      }
      loadOk = True;

      attr_dialog = (DialogData *)file_mgr_data->attr_dialog_list[i];
      attr_data = (ModAttrData *) attr_dialog->data;

      loadOk =  LoadFileAttributes(attr_data->host, attr_data->directory,
                      attr_data->name, attr_data);


      if (loadOk)
      {
         /* We need the file_mgr_rec inside the create routine so
          * we attach it here for use later.
          */
         ((ModAttrData *) (file_mgr_data->attr_dialog_list[i]->data))->
                                           main_widget = file_mgr_rec->main;

         _DtShowDialog (file_mgr_rec->shell, NULL, (XtPointer)file_mgr_rec,
                     file_mgr_data->attr_dialog_list[i],
                     ModAttrChange, (XtPointer)file_mgr_rec, ModAttrClose,
                     (XtPointer)file_mgr_rec, NULL, False, False, NULL, NULL);

         modAttr_rec = (ModAttrRec *)_DtGetDialogInstance(
                                       file_mgr_data->attr_dialog_list[i]);

         if(file_mgr_data->title != NULL &&
                    strcmp(file_mgr_data->helpVol, DTFILE_HELP_NAME) != 0)
         {
            tmpStr = GETMESSAGE(21, 1, "File Permissions");
            tempStr = (char *)XtMalloc(strlen(tmpStr) +
                                     strlen(file_mgr_data->title) + 5);
            sprintf(tempStr, "%s - %s", file_mgr_data->title, tmpStr);
         }
         else
         {
            tmpStr = (GETMESSAGE(21,34, "File Manager - Permissions"));
            tempStr = XtNewString(tmpStr);
         }
         XtSetArg (args[0], XmNtitle, tempStr);
         XtSetValues (modAttr_rec->shell, args, 1);
         XtFree(tempStr);
      }
      else
      {
	/* The object would have got deleted and hence we are not able to
	   find it */
	if(attr_data)
	   ModAttrFreeValues(attr_data);
	XtFree((char *)attr_dialog);
	file_mgr_data->attr_dialog_count--;
	if(file_mgr_data->attr_dialog_count >0)
	    file_mgr_data->attr_dialog_list = (DialogData **) XtRealloc(
	          (char *) file_mgr_data->attr_dialog_list,
		  sizeof(DialogData *) * file_mgr_data->attr_dialog_count);
	else
	{
	   XtFree((char *)file_mgr_data->attr_dialog_list);
	   file_mgr_data->attr_dialog_list = NULL;
	}
      }
   }

   /* Display any secondary help dialogs */
   for (i = 0; i < file_mgr_data->secondaryHelpDialogCount; i++)
   {
      ShowHelpDialog(file_mgr_rec->shell, (XtPointer)file_mgr_rec,
		     HYPER_HELP_DIALOG,
                     file_mgr_data->secondaryHelpDialogList[i], NULL, NULL,
                     NULL, NULL, 0);
   }

   /* Display the primary help dialog, if active */
   if (file_mgr_data->primaryHelpDialog && ((DialogInstanceData *)
                 (file_mgr_data->primaryHelpDialog->data))->displayed == True)
   {
      ShowHelpDialog(file_mgr_rec->shell, (XtPointer)file_mgr_rec,
		     MAIN_HELP_DIALOG,
                     file_mgr_data->primaryHelpDialog, NULL, NULL, NULL,
                     NULL, 0);
   }

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
   FileMgrData * file_mgr_data = (FileMgrData *) values->data;
   FileMgrRec * file_mgr_rec;
   char * new_name_list[20];
   int name_list_count;
   Arg args[2];
   char number[10];
   int i;
   char * tmpStr;

   /*  If the dialog is currently displayed, update the geometry  */
   /*  fields to their current values.                            */

   if (file_mgr_data->displayed == True)
   {
      _DtGenericUpdateWindowPosition(values);
      file_mgr_rec = (FileMgrRec *) _DtGetDialogInstance (values);
   }


   /* save FILEMGR resources */
   /* store netfile version of current directory name */

   if ( file_mgr_data->current_directory && file_mgr_data->toolbox )
   {
      char *current_host;
      char *user_session_str = NULL;
      char *ptr = NULL;

      current_host = file_mgr_data->host;
      user_session_str = getenv("DTUSERSESSION");
      ptr = strstr(file_mgr_data->current_directory, user_session_str);
      if (ptr)
      {
         *ptr = '\0';
         file_mgr_data->host = XtNewString(file_mgr_data->current_directory);
         *ptr = user_session_str[0];
      }
      else
      {
         file_mgr_data->host = XtNewString(file_mgr_data->current_directory);
      }

      _DtDialogPutResources (fd, name_list, FILEMGR, values->data,
                             resources, fileMgrClass->resource_count);

      XtFree(file_mgr_data->host);
      file_mgr_data->host = current_host;
   }
   else
   {
      _DtDialogPutResources (fd, name_list, FILEMGR, values->data,
                             resources, fileMgrClass->resource_count);
   }


   /* Save the optional positional data if needed, if file_mgr_data->host
      is set to NULL, this is from a save settings, don't save the position
      information
   */
   SavePositionalData(fd, file_mgr_data, name_list, FILEMGR);


   /*  Get a copy of the name list to be used to build new name lists  */
   _DtDuplicateDialogNameList(name_list, new_name_list, 20, &name_list_count);

   /*  Write out all of the dialogs  */

   _DtWriteDialogData (file_mgr_data->change_dir, fd, name_list);
   _DtWriteDialogData (file_mgr_data->preferences, fd, name_list);


   /* Save any properties dialogs */
   _DtSaveSubdialogArray(new_name_list, name_list_count,
                      file_mgr_data->attr_dialog_list,
                      file_mgr_data->attr_dialog_count, fd, 1);

   /* Save any secondary help dialogs */
   _DtSaveSubdialogArray(new_name_list, name_list_count,
                      file_mgr_data->secondaryHelpDialogList,
                      file_mgr_data->secondaryHelpDialogCount, fd, 1);

   /* Save the primary help dialog, if active */
   if (file_mgr_data->primaryHelpDialog)
   {
      new_name_list[name_list_count] = number;
      new_name_list[name_list_count + 1] = NULL;
      sprintf(number, "%d", 0);
      _DtWriteDialogData(file_mgr_data->primaryHelpDialog, fd, new_name_list);
   }

   tmpStr = GetSharedMessage(FILTER_EDIT_ID_STRING);
   new_name_list[name_list_count] = XtNewString (tmpStr);
   new_name_list[name_list_count + 1] = NULL;
   _DtWriteDialogData (file_mgr_data->filter_edit, fd, new_name_list);
   XtFree ((char *) new_name_list[name_list_count]);
   new_name_list[name_list_count] = NULL;

   tmpStr = GetSharedMessage(FILTER_ACTIVE_ID_STRING);
   new_name_list[name_list_count] = XtNewString (tmpStr);
   new_name_list[name_list_count + 1] = NULL;
   _DtWriteDialogData (file_mgr_data->filter_active, fd, new_name_list);
   XtFree ((char *) new_name_list[name_list_count]);
   new_name_list[name_list_count] = NULL;

   _DtWriteDialogData (file_mgr_data->find, fd, name_list);
}




/************************************************************************
 *
 *  FreeValues
 *      Free up space allocated by the FileMgr dialog.  This
 *      includes sub directory information, and attached dialog data.
 *
 *      Note: this routine frees the FileMgrData, not the FileMgrRec
 *        (the FileMgrRec data structure will be reused by the dialog
 *        caching code for the next dtfile window the user might open)
 *
 ************************************************************************/

static void
FreeValues(
        FileMgrData *file_mgr_data )
{
   int i;

   FileMgrRec *file_mgr_rec;

   if (file_mgr_data == NULL)
      return;

   file_mgr_rec = (FileMgrRec *) file_mgr_data->file_mgr_rec;

   FreeDirectoryData (file_mgr_data);

   if (file_mgr_data->branch_list != NULL)
   {
      for (i = 0; file_mgr_data->branch_list[i]; i++)
        XtFree (file_mgr_data->branch_list[i]);
      XtFree ((char *) file_mgr_data->branch_list);
      file_mgr_data->branch_list = NULL;
   }

   if (file_mgr_data->selection_list != NULL)
   {
      XtFree ((char *) file_mgr_data->selection_list);
      file_mgr_data->selection_list = NULL;
   }

   if (file_mgr_data->cd_select != NULL)
   {
      XtFree ((char *) file_mgr_data->cd_select);
      file_mgr_data->cd_select = NULL;
   }

   if (file_mgr_data->desktop_file != NULL)
   {
       XtFree ((char *) file_mgr_data->desktop_file);
       file_mgr_data->desktop_file = NULL;
   }


   XtFree(file_mgr_data->special_msg);
   file_mgr_data->special_msg = NULL;
   if (file_mgr_data->msg_timer_id)
      XtRemoveTimeOut(file_mgr_data->msg_timer_id);

   _DtDestroySubdialog(file_mgr_data->change_dir);
   _DtDestroySubdialog(file_mgr_data->preferences);
   _DtDestroySubdialog(file_mgr_data->filter_edit);
   _DtDestroySubdialog(file_mgr_data->filter_active);
   _DtDestroySubdialog(file_mgr_data->find);
   _DtDestroySubdialogArray(file_mgr_data->attr_dialog_list,
                         file_mgr_data->attr_dialog_count);
   _DtDestroySubdialogArray(file_mgr_data->secondaryHelpDialogList,
                         file_mgr_data->secondaryHelpDialogCount);
   _DtDestroySubdialog(file_mgr_data->primaryHelpDialog);

   if(showFilesystem
      && file_mgr_data != trashFileMgrData
      && file_mgr_data != NULL )
   {
      /* Free up the GC's we got in SetValues */
      if(file_mgr_data->cd_normal_gc != NULL)
         XtReleaseGC(file_mgr_rec->current_directory,
                                       file_mgr_data->cd_normal_gc);
      if(file_mgr_data->cd_select_gc != NULL)
         XtReleaseGC(file_mgr_rec->current_directory_icon,
                                       file_mgr_data->cd_select_gc);

      if(file_mgr_data->tree_solid_thin_gc != NULL)
         XtReleaseGC(file_mgr_rec->current_directory_icon,
                                       file_mgr_data->tree_solid_thin_gc);
      if(file_mgr_data->tree_solid_thick_gc != NULL)
         XtReleaseGC(file_mgr_rec->current_directory_icon,
                                       file_mgr_data->tree_solid_thick_gc);
      if(file_mgr_data->tree_dash_thin_gc != NULL)
         XtReleaseGC(file_mgr_rec->current_directory_icon,
                                       file_mgr_data->tree_dash_thin_gc);
      if(file_mgr_data->tree_dash_thick_gc != NULL)
         XtReleaseGC(file_mgr_rec->current_directory_icon,
                                       file_mgr_data->tree_dash_thick_gc);
   }

   FreeLayoutData(file_mgr_data->layout_data);

   {    /* This block is added for rectifying memory leakages */
     XtFree(file_mgr_data->scrollToThisFile);
     XtFree(file_mgr_data->scrollToThisDirectory);
   }

   XtFree ((char *) file_mgr_data);
}


/************************************************************************
 *
 *  Destroy
 *      Destroy widgets and free up space allocated by the FileMgr dialog.
 *      Called when the dialog is destroyed.
 *
 ************************************************************************/

static void
Destroy(
        XtPointer recordPtr )
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *) recordPtr;

   ShelfUnregisterView(file_mgr_rec);
   XtDestroyWidget(file_mgr_rec->shell);

   XtFree(file_mgr_rec->action_pane_file_type);
   if (file_mgr_rec->shelf_popup_data)
      ShelfPopupDataFree((ShelfPopupData *)file_mgr_rec->shelf_popup_data);
   XtFree((char *)file_mgr_rec);
}


static void
ActivateClist(
        Widget wid,
        XEvent *event,
        String *params,
        Cardinal *num_params )
{
  XmDrawingAreaCallbackStruct cb ;
  FileMgrRec * fileMgrRec;
  int x, y ;
  Widget input_on_gadget ;

    if ((event->type == KeyPress)
        ||(event->type == KeyRelease))
    {
        x = event->xkey.x ;
        y = event->xkey.y ;
    }
    else
      return ;

    cb.reason = XmCR_INPUT;
    cb.event = event;

    if( *(params[0]) != '@' )
    {
      char * title = XtNewString( GETMESSAGE(12, 7, "File Manager") );
      char * tmp = "Gjmf Nbobhfs ufbn:\n\nTboez Bnjo\nEbo Ebp\nSbhiv Lspwwjej\nUfe Sbotpn\nMjoeb Sjfl\nGsbol Tdinvdl\nNvsbmj Tpnbspvuiv\n";
      char * message;
      int i, len = strlen( tmp );

      message = (char *)XtCalloc( 1, len + 1 );

      /* Not fancy, but serve the purpose */
      for( i = 0; i < len; ++i ){
        (( isalpha( tmp[i] ) ) ? (message[i] = (int)(tmp[i]) - 1) : (message[i] = tmp[i]));
      };

      sscanf( params[0], "%p", (void **) &fileMgrRec );
      _DtMessage(toplevel, title, message, NULL, HelpRequestCB);
      XtFree( title );
      XtFree( message );
    }

    return ;
}

/************************************************************************
 ************************************************************************
 *
 *   Resource converters
 *
 ************************************************************************
 ************************************************************************/


/************************************************************************
 *
 *  GetSessionDir
 *
 ************************************************************************/

static void
GetSessionDir(
         FileMgrData *file_mgr_data )
{
   if (restoreType == HOME_RESTORE)             /* home settings restore */
   {
      struct passwd * pwInfo;
      char * homeDir;

      /* Set current_directory to the home dir */
      if ((homeDir = (char *)getenv("HOME")) == NULL)
      {
         pwInfo = getpwuid (getuid());
         homeDir = pwInfo->pw_dir;
      }

      file_mgr_data->current_directory = XtNewString(homeDir);

      file_mgr_data->current_directory =
                     XtNewString(file_mgr_data->current_directory);

      if (file_mgr_data->restricted_directory)
         file_mgr_data->restricted_directory =
                   XtNewString(file_mgr_data->restricted_directory);
      else
         file_mgr_data->restricted_directory = NULL;
   }
   else if (restoreType == TOOL_RESTORE)        /* tool settings restore */
   {
      file_mgr_data->current_directory =
                     XtNewString(file_mgr_data->restricted_directory);

      if (file_mgr_data->restricted_directory)
         file_mgr_data->restricted_directory =
                   XtNewString(file_mgr_data->restricted_directory);
      else
         file_mgr_data->restricted_directory = NULL;
   }
   else                                         /* session restore */
   {
      if ((file_mgr_data->current_directory) &&
          (file_mgr_data->toolbox) &&
           (file_mgr_data->host)  &&
          (strcmp(file_mgr_data->host, file_mgr_data->current_directory) != 0))
      {
         char *root_toolbox;
         char *user_session_str;
         char *toolbox_dir;
         char *current_dir;
         size_t len = 0;

         root_toolbox = (file_mgr_data->host);

         user_session_str = getenv("DTUSERSESSION");
         /* the restricted directory for any object is a parent of that *
          * object; the restricted directory for toolboxes is the root  *
          * toolbox; the current toolbox dir is identified by the path  *
          * information past the restricted dir/root toolbox            */
         toolbox_dir = file_mgr_data->current_directory;
         toolbox_dir += strlen(file_mgr_data->restricted_directory);

         len = strlen(root_toolbox) +
             strlen(user_session_str) +
             strlen(toolbox_dir) + 1;
         current_dir = XtMalloc(len);
         if (current_dir)
         {
             snprintf(current_dir, len, "%s%s%s", root_toolbox,
                      user_session_str,
                      toolbox_dir);
             file_mgr_data->current_directory = current_dir;
         }

         len = strlen(root_toolbox) + strlen(user_session_str) + 1;
         file_mgr_data->restricted_directory = XtMalloc(len);
         if (file_mgr_data->restricted_directory)
         {
             snprintf(file_mgr_data->restricted_directory, len, "%s%s",
                      root_toolbox,
                      user_session_str);
         }
      }
      else
      {
         file_mgr_data->current_directory =
                        XtNewString(file_mgr_data->current_directory);

         if (file_mgr_data->restricted_directory)
            file_mgr_data->restricted_directory =
                   XtNewString(file_mgr_data->restricted_directory);
         else
            file_mgr_data->restricted_directory = NULL;
      }
   }
   file_mgr_data->host = NULL;
}


/************************************************************************
 *
 *  GetPixmapData
 *      Given a file name get the right icon name for it.
 *
 ************************************************************************/

PixmapData *
GetPixmapData(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data,
        char *path,
        int icon_size)
{
   char * full_name;
   char * short_name;
   char * ftype;
   char * icon_name;
   PixmapData * pixmapData;
   Tt_status tt_status;

   /* Display the correct small directory icon */
   ftype = GetDirectoryLogicalType(file_mgr_data, path);
   if (ftype == NULL)
      return NULL;

   full_name = ResolveLocalPathName(file_mgr_data->host,
                                    path,
                                    NULL,
                                    home_host_name,
                                    &tt_status);
   if( TT_OK != tt_status )
     return( NULL );
   if (full_name == NULL)
      return NULL;

   short_name = strrchr(full_name, '/');
   if (short_name == NULL)
      return NULL;
   if (strcmp(short_name, "/.") == 0)
   {
      if (short_name == full_name)
         short_name++;
      *short_name = '\0';
      short_name = strrchr(full_name, '/');
   }
   if (strcmp(full_name, "/") == 0)
      short_name = full_name;
   else
      *short_name++ = '\0';

   if (icon_size != LARGE && icon_size != SMALL && icon_size != EXTRA_LARGE)
      icon_size = SMALL;

   pixmapData = _DtRetrievePixmapData(ftype,
                                      short_name,
                                      full_name,
                                      file_mgr_rec->shell,
                                      icon_size);

   XtFree(full_name);
   return pixmapData;
}


/************************************************************************
 *
 *  BranchListToString
 *      Write out the array of strings to the file fd.
 *
 ************************************************************************/

static void
BranchListToString(
        int fd,
        char ***value,
        char *out_buf )
{
   int i;
   Boolean first = True;
   char * branch_name;
   int rv;

   if (*value != NULL)
   {
      rv = write (fd, out_buf, strlen (out_buf));

      i = 0;
      branch_name = (*value)[i];

      while (branch_name != NULL)
      {
         if (!first)
            rv = write (fd, ", ", strlen (", "));
         else
            first = False;

         rv = write (fd, branch_name, strlen (branch_name));

         i++;
         branch_name = (*value)[i];
      }

      rv = write (fd, "\n", strlen ("\n"));
   }
}



/************************************************************************
 *
 *  SelectionListToString
 *      Write out the array of strings to the file fd.
 *
 ************************************************************************/

static void
SelectionListToString(
        int fd,
        FileViewData ***value,
        char *out_buf )
{
   int i;
   Boolean first = True;
   FileViewData * file_view_data;
   DirectorySet * directory_set;
   int rv; /* probably should actually check this... */

   if (*value != NULL)
   {
      rv = write (fd, out_buf, strlen (out_buf));

      i = 0;
      file_view_data = (*value)[i];


      while (file_view_data != NULL)
      {
         directory_set  = (DirectorySet *) file_view_data->directory_set;

         if (!first)
             rv =  write (fd, ", ", strlen (", "));
         else
            first = False;


         rv = write (fd, directory_set->name, strlen (directory_set->name));

         if (strcmp (directory_set->name, "/") != 0)
            rv = write (fd, "/", strlen ("/"));

         rv = write (fd, file_view_data->file_data->file_name,
                     strlen (file_view_data->file_data->file_name));

         i++;
         file_view_data = (*value)[i];
      }

      rv = write (fd, "\n", strlen ("\n"));
   }

}



/************************************************************************
 *
 *  StringToBranchList
 *      Convert a string to a string array.
 *
 ************************************************************************/

static Boolean
StringToBranchList(
        Display *display,
        XrmValue *args,
        Cardinal num_args,
        XrmValue *from_val,
        XrmValue *to_val,
        XtPointer *converter_data)
{
   static char ** table_ptr;
   char * start;
   char * current;
   char ** table = NULL;
   int table_size = 0;

   table_ptr = NULL;

   to_val->size = sizeof (char **);
   to_val->addr = (XtPointer) &table_ptr;

   if (from_val->addr == NULL)
      return True;


   /*  Loop through the string extracting branch names  */
   /*  and placing them into an array of strings.       */

   current = start = (char *) from_val->addr;

   while (start != NULL)
   {
      current = DtStrchr (start, ',');
      if (current != NULL)
      {
         *current = '\0';
         current += 2;
      }

      table_size++;
      table = (char **) XtRealloc ((char *)table,
                                        sizeof (char *) * (table_size + 1));
      table[table_size] = NULL;

      table[table_size - 1] = XtNewString (start);
      start = current;
   }


   table_ptr = table;
   to_val->addr = (XtPointer ) &table_ptr;
   to_val->size = sizeof(XtPointer);
   return True;
}


/************************************************************************
 *
 *  StringToSelectionList
 *      Convert a string to a string array.
 *
 ************************************************************************/

static void
StringToSelectionList(
        XrmValue *args,
        Cardinal num_args,
        XrmValue *from_val,
        XrmValue *to_val )
{
   static char ** table_ptr;
   char * start;
   char * current;
   char ** table = NULL;
   int table_size = 0;

   table_ptr = NULL;

   to_val->size = sizeof (char **);
   to_val->addr = (XtPointer) &table_ptr;

   if (from_val->addr == NULL)
      return;


   /*  Loop through the string extracting file specifications  */
   /*  and placing them into an array of strings.              */

   current = start = (char *) from_val->addr;

   while (start != NULL)
   {
      current = DtStrchr (start, ',');
      if (current != NULL)
      {
         *current = '\0';
         current += 2;
      }

      table_size++;
      table = (char **) XtRealloc ((char *)table,
                                        sizeof (char *) * (table_size + 1));
      table[table_size] = NULL;

      table[table_size - 1] = XtNewString (start);
      start = current;
   }


   table_ptr = table;
   to_val->addr = (XtPointer ) &table_ptr;
   to_val->size = sizeof(XtPointer);
}




/************************************************************************
 ************************************************************************

        File Mgr file and directory processing functions.

 ************************************************************************
 ************************************************************************/


/************************************************************************
 *
 *  UpdateHeaders
 *      Update the iconic path and current directory line.
 *
 ************************************************************************/

/*
 * UpdateStatusLine:
 *   Update the status line label widget to show the right text.
 */
static void
UpdateStatusLine(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data)
{
   char buf[4*MAX_PATH];
   XmString label_string;
   Arg args[2];

   if (file_mgr_data->special_msg &&
       (file_mgr_data->busy_status == initiating_readdir ||
        file_mgr_data->busy_status == busy_readdir))
   {
      SetSpecialMsg( file_mgr_rec, file_mgr_data, NULL );
   }

   if (file_mgr_data->special_msg)
   {
      label_string = XmStringCreateLocalized(file_mgr_data->special_msg);
   }
   else
   {
      GetStatusMsg(file_mgr_data, buf);
      label_string = XmStringCreateLocalized(buf);
   }
   XtSetArg (args[0], XmNlabelString, label_string);
   XtSetValues(file_mgr_rec->status_line, args, 1);
   XmStringFree (label_string);
}


/*
 * MsgTimerEvent:
 *   Timeout routine that resets the status line after a
 *   special message was shown (see also SetSpecialMsg).
 */
static void
MsgTimerEvent(
        FileMgrData *file_mgr_data,
        XtIntervalId *id )
{
   FileMgrRec *file_mgr_rec;

   if (*id != file_mgr_data->msg_timer_id)
      return;

   file_mgr_data->msg_timer_id = 0;

   if (file_mgr_data->special_msg)
   {
      XtFree(file_mgr_data->special_msg);
      file_mgr_data->special_msg = NULL;
      if (file_mgr_data->show_status_line)
      {
         file_mgr_rec = (FileMgrRec *)file_mgr_data->file_mgr_rec;
         UpdateStatusLine(file_mgr_rec, file_mgr_data);
      }
   }
}


/*
 * SetSpecialMsg:
 *   Arrange for a special message to be shown in the status line.
 *   After 4 seconds the status line will revert back to the usual
 *   "x Files, y Hidden" message.
 */
void
SetSpecialMsg(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data,
        char *msg)
{
   if (file_mgr_data->special_msg)
   {
      XtFree(file_mgr_data->special_msg);
      file_mgr_data->special_msg = NULL;
   }
   if (file_mgr_data->msg_timer_id)
      XtRemoveTimeOut(file_mgr_data->msg_timer_id);

   if (msg)
   {
      file_mgr_data->special_msg = XtNewString(msg);
      file_mgr_data->msg_timer_id =
        XtAppAddTimeOut (XtWidgetToApplicationContext (file_mgr_rec->shell),
                         4000, (XtTimerCallbackProc) MsgTimerEvent,
                         (XtPointer) file_mgr_data);
   }
   else
   {
      file_mgr_data->special_msg = NULL;
      file_mgr_data->msg_timer_id = 0;
   }
}

/*
 * ShelfHomeButtonHandler:
 *   Open the Home directory in place when the shelf Home icon is clicked.
 */

static void
ShelfPopupDataFree(ShelfPopupData *data)
{
   if (data == NULL)
      return;
   XtFree(data->path);
   XtFree((char *)data);
}

static void
ShelfPopupDataSet(FileMgrRec *file_mgr_rec, ShelfPopupData *data)
{
   if (file_mgr_rec == NULL)
   {
      ShelfPopupDataFree(data);
      return;
   }

   if (file_mgr_rec->shelf_popup_data)
      ShelfPopupDataFree((ShelfPopupData *)file_mgr_rec->shelf_popup_data);
   file_mgr_rec->shelf_popup_data = (XtPointer)data;
}

static ShelfPopupData *
ShelfPopupDataGet(FileMgrRec *file_mgr_rec)
{
   if (file_mgr_rec == NULL)
      return NULL;
   return (ShelfPopupData *)file_mgr_rec->shelf_popup_data;
}

static void
ShelfMoveFinishCB(XtPointer client_data, int rc)
{
   (void) client_data;

   if (rc == 0 && shelf_move_finish_slot >= 0)
   {
      ShelfClearSlotSkipIgnore(shelf_move_finish_slot);
      ShelfUpdateSlotAllViews(shelf_move_finish_slot);
   }
   shelf_move_finish_slot = -1;
}

static void
ShelfMoveCopyLinkToCurrent(FileMgrRec *file_mgr_rec,
                           FileMgrData *file_mgr_data,
                           const char *path,
                           unsigned int modifiers,
                           int slot)
{
   char **file_set;
   char **host_set;
   Boolean result;
   void (*finish_cb)() = NULL;

   if (file_mgr_rec == NULL || file_mgr_data == NULL || path == NULL)
      return;

   file_set = (char **)XtMalloc(sizeof(char *));
   host_set = (char **)XtMalloc(sizeof(char *));
   file_set[0] = XtNewString(path);
   host_set[0] = home_host_name;

   if (modifiers == 0)
   {
      shelf_move_finish_slot = slot;
      finish_cb = (void (*)())ShelfMoveFinishCB;
   }

   result = FileMoveCopy(file_mgr_data,
                         NULL,
                         file_mgr_data->current_directory,
                         file_mgr_data->host,
                         host_set,
                         file_set,
                         1,
                         modifiers,
                         finish_cb,
                         NULL);

   if (!result)
      shelf_move_finish_slot = -1;

   XtFree(file_set[0]);
   XtFree((char *)file_set);
   XtFree((char *)host_set);
}

static void
ShelfOpenFile(FileMgrRec *file_mgr_rec,
              FileMgrData *file_mgr_data,
              const char *path)
{
   char *logical_type;
   char *action;
   DtActionArg action_arg;
   char dir_buf[MAX_PATH];
   char *file_name;

   if (file_mgr_rec == NULL || file_mgr_data == NULL || path == NULL)
      return;

   logical_type = (char *)DtDtsDataToDataType(path, NULL, 0,
                                              NULL, NULL, NULL, NULL);
   if (logical_type == NULL)
      return;

   action = _DtRetrieveDefaultAction(logical_type);
   DtDtsFreeDataType(logical_type);
   if (action == NULL)
      return;

   strncpy(dir_buf, path, MAX_PATH - 1);
   dir_buf[MAX_PATH - 1] = '\0';
   file_name = strrchr(dir_buf, '/');
   if (file_name == NULL)
   {
      XtFree(action);
      return;
   }
   if (file_name == dir_buf)
   {
      file_name++;
      dir_buf[1] = '\0';
   }
   else
   {
      *file_name = '\0';
      file_name++;
   }

   action_arg.argClass = DtACTION_FILE;
   action_arg.u.file.name = (char *)path;

   DtActionInvoke(file_mgr_rec->shell, action, &action_arg, 1,
                  NULL, NULL, dir_buf, True, NULL, NULL);
   XtFree(action);
}

static void
ShelfOpenNewView(FileMgrData *file_mgr_data, const char *path,
                 const char *host)
{
   if (file_mgr_data == NULL || path == NULL)
      return;

   const char *target_host = host ? host : file_mgr_data->host;

   initiating_view = (XtPointer) file_mgr_data;
   if (file_mgr_data->restricted_directory != NULL)
   {
      special_view = True;
      special_treeType = file_mgr_data->show_type;
      special_viewType = file_mgr_data->view;
      special_orderType = file_mgr_data->order;
      special_directionType = file_mgr_data->direction;
      special_randomType = file_mgr_data->positionEnabled;
      special_restricted = XtNewString(file_mgr_data->restricted_directory);
      if (file_mgr_data->title == NULL)
         special_title = NULL;
      else
         special_title = XtNewString(file_mgr_data->title);
      special_helpVol = XtNewString(file_mgr_data->helpVol);

      if (file_mgr_data->toolbox)
         GetNewView((char *)target_host, (char *)path,
                    file_mgr_data->restricted_directory, NULL, 0);
      else
         GetNewView((char *)target_host, (char *)path, NULL, NULL, 0);
   }
   else
   {
      GetNewView((char *)target_host, (char *)path, NULL, NULL, 0);
   }
   initiating_view = (XtPointer) NULL;
}

static void
ShelfPopupEnsureMenu(FileMgrRec *file_mgr_rec)
{
   Widget menu;
   Widget item;
   XmString label;
   Arg args[2];
   int n = 0;

   if (file_mgr_rec == NULL)
      return;

   if (file_mgr_rec->shelf_popup != NULL)
   {
      Widget menushell = XtParent(file_mgr_rec->shelf_popup);
      Widget parent = menushell ? XtParent(menushell) : NULL;

      if (parent == file_mgr_rec->shelf_frame)
         return;

      if (menushell)
         XtDestroyWidget(menushell);
      else
         XtDestroyWidget(file_mgr_rec->shelf_popup);
      file_mgr_rec->shelf_popup = NULL;
   }

   n = 0;
   XtSetArg(args[n], XmNmenuAccelerator, "   "); n++;
   XtSetArg(args[n], XmNwhichButton, bMenuButton); n++;
   menu = XmCreatePopupMenu(file_mgr_rec->shelf_frame, "shelfPopup", args, n);
   {
      /* Popdown callbacks live on the popup's MenuShell, not the RowColumn itself */
      Widget menushell = XtParent(menu);
      if (menushell)
         XtAddCallback(menushell, XmNpopdownCallback, ShelfPopupPopdownCB,
                       (XtPointer)file_mgr_rec);
   }

   label = XmStringCreateLocalized("");
  item = XmCreateLabelGadget(menu, "shelfName", NULL, 0);
   XtVaSetValues(item, XmNlabelString, label, NULL);
   XmStringFree(label);
   XtManageChild(item);

   item = XmCreateSeparatorGadget(menu, "shelfNameSeparator", NULL, 0);
   XtManageChild(item);

   label = XmStringCreateLocalized("Run");
   item = XmCreatePushButtonGadget(menu, "shelfRun", NULL, 0);
   XtVaSetValues(item, XmNlabelString, label, NULL);
   XmStringFree(label);
   XtAddCallback(item, XmNactivateCallback, ShelfPopupRunCB,
                 (XtPointer)file_mgr_rec);
   XtManageChild(item);

   label = XmStringCreateLocalized("Run...");
   item = XmCreatePushButtonGadget(menu, "shelfRunNoPrompt", NULL, 0);
   XtVaSetValues(item, XmNlabelString, label, NULL);
   XmStringFree(label);
   XtAddCallback(item, XmNactivateCallback, ShelfPopupRunNoPromptCB,
                 (XtPointer)file_mgr_rec);
   XtManageChild(item);

   label = XmStringCreateLocalized(GETMESSAGE(40, 3, "Open"));
   item = XmCreatePushButtonGadget(menu, "shelfOpen", NULL, 0);
   XtVaSetValues(item, XmNlabelString, label, NULL);
   XmStringFree(label);
   XtAddCallback(item, XmNactivateCallback, ShelfPopupOpenCB,
                 (XtPointer)file_mgr_rec);
   XtManageChild(item);

   label = XmStringCreateLocalized(GETMESSAGE(40, 4, "Open New View"));
   item = XmCreatePushButtonGadget(menu, "shelfOpenNewView", NULL, 0);
   XtVaSetValues(item, XmNlabelString, label, NULL);
   XmStringFree(label);
   XtAddCallback(item, XmNactivateCallback, ShelfPopupOpenNewViewCB,
                 (XtPointer)file_mgr_rec);
   XtManageChild(item);

   label = XmStringCreateLocalized(GETMESSAGE(40, 5, "Remove from Shelf"));
   item = XmCreatePushButtonGadget(menu, "shelfRemove", NULL, 0);
   XtVaSetValues(item, XmNlabelString, label, NULL);
   XmStringFree(label);
   XtAddCallback(item, XmNactivateCallback, ShelfPopupRemoveCB,
                 (XtPointer)file_mgr_rec);
   XtManageChild(item);

   item = XmCreateSeparatorGadget(menu, "separator", NULL, 0);
   XtManageChild(item);

   label = XmStringCreateLocalized(GETMESSAGE(40, 6, "Move to Current Directory"));
   item = XmCreatePushButtonGadget(menu, "shelfMove", NULL, 0);
   XtVaSetValues(item, XmNlabelString, label, NULL);
   XmStringFree(label);
   XtAddCallback(item, XmNactivateCallback, ShelfPopupMoveCB,
                 (XtPointer)file_mgr_rec);
   XtManageChild(item);

   label = XmStringCreateLocalized(GETMESSAGE(40, 7, "Copy to Current Directory"));
   item = XmCreatePushButtonGadget(menu, "shelfCopy", NULL, 0);
   XtVaSetValues(item, XmNlabelString, label, NULL);
   XmStringFree(label);
   XtAddCallback(item, XmNactivateCallback, ShelfPopupCopyCB,
                 (XtPointer)file_mgr_rec);
   XtManageChild(item);

   label = XmStringCreateLocalized(GETMESSAGE(40, 8, "Link to Current Directory"));
   item = XmCreatePushButtonGadget(menu, "shelfLink", NULL, 0);
   XtVaSetValues(item, XmNlabelString, label, NULL);
   XmStringFree(label);
   XtAddCallback(item, XmNactivateCallback, ShelfPopupLinkCB,
                 (XtPointer)file_mgr_rec);
   XtManageChild(item);

   file_mgr_rec->shelf_popup = menu;
}

static void
ShelfPopupUpdate(FileMgrRec *file_mgr_rec, ShelfPopupData *data)
{
   Widget menu;
   Widget child;
   Boolean can_remove;
   Boolean is_dir;
   XmString label;
   const char *name = "";

   if (file_mgr_rec == NULL || file_mgr_rec->shelf_popup == NULL)
      return;

   menu = file_mgr_rec->shelf_popup;
   is_dir = (data != NULL && data->is_dir);
   can_remove = (data != NULL && data->slot != 0);

   child = XtNameToWidget(menu, "shelfName");
   if (child)
   {
      if (data && data->path)
      {
         const char *base = strrchr(data->path, '/');
         if (base && *(base + 1) != '\0')
            name = base + 1;
         else
            name = data->path;
      }
      label = XmStringCreateLocalized((char *)name);
      XtVaSetValues(child, XmNlabelString, label, NULL);
      XmStringFree(label);
   }

   child = XtNameToWidget(menu, "shelfRun");
   if (child)
      XtSetSensitive(child, (data != NULL && data->is_execute));

   child = XtNameToWidget(menu, "shelfRunNoPrompt");
   if (child)
      XtSetSensitive(child, (data != NULL && data->is_execute));

   child = XtNameToWidget(menu, "shelfOpen");
   if (child)
      XtSetSensitive(child, (data != NULL && !data->is_execute));

   child = XtNameToWidget(menu, "shelfOpenNewView");
   if (child)
      XtSetSensitive(child, is_dir);

   child = XtNameToWidget(menu, "shelfRemove");
   if (child)
      XtSetSensitive(child, can_remove);
}

static FileViewData *
ShelfCreatePopupFileViewData(
        FileMgrData *file_mgr_data,
        const char *resolved_path )
{
   FileViewData *fvd = NULL;
   DirectorySet *ds = NULL;
   FileData *fd = NULL;
   char dir[MAX_PATH];
   const char *base;

   if (file_mgr_data == NULL || resolved_path == NULL || *resolved_path == '\0')
      return NULL;

   base = strrchr(resolved_path, '/');
   if (base == NULL)
      return NULL;

   if (base == resolved_path)
   {
      strcpy(dir, "/");
      base++;
   }
   else
   {
      size_t len = (size_t)(base - resolved_path);
      if (len >= sizeof(dir))
         return NULL;
      memcpy(dir, resolved_path, len);
      dir[len] = '\0';
      base++;
   }

   fd = ReadFileData(dir, (char *)base);
   if (fd == NULL)
      return NULL;

   ds = (DirectorySet *)XtNew(DirectorySet);
   memset(ds, 0, sizeof(DirectorySet));
   ds->name = XtNewString(dir);
   ds->file_count = 1;
   ds->file_view_data = (FileViewData **)XtMalloc(sizeof(FileViewData *));
   ds->order_list = (FileViewData **)XtMalloc(sizeof(FileViewData *));
   ds->filtered_file_count = 1;
   ds->invisible_file_count = 0;
   ds->file_mgr_data = (XtPointer)file_mgr_data;

   fvd = (FileViewData *)XtNew(FileViewData);
   memset(fvd, 0, sizeof(FileViewData));
   fvd->directory_set = (XtPointer)ds;
   fvd->file_data = fd;
   fvd->displayed = True;
   ds->file_view_data[0] = fvd;
   ds->order_list[0] = fvd;

   return fvd;
}

static void
ShelfFreePopupFileViewData(
        FileViewData *file_view_data )
{
   if (file_view_data == NULL)
      return;

   DirectorySet *ds = (DirectorySet *)file_view_data->directory_set;
   FileData *fd = file_view_data->file_data;

   if (fd)
      FreeFileData(fd, True);

   if (ds)
   {
      XtFree(ds->name);
      XtFree((char *)ds->file_view_data);
      XtFree((char *)ds->order_list);
      XtFree((char *)ds);
   }

   XtFree((char *)file_view_data);
}

static void
ShelfRunResolvedPath(
        FileMgrData *file_mgr_data,
        const char *resolved_path,
        const char *resolved_host,
        const char *action )
{
   char *dir_path = NULL;
   FileViewData *file_view_data = NULL;
   char *host_name = NULL;

   if (file_mgr_data == NULL || resolved_path == NULL || action == NULL)
      return;

   file_view_data = ShelfCreatePopupFileViewData(file_mgr_data, resolved_path);
   if (file_view_data == NULL)
      return;

   dir_path = XtNewString(resolved_path);
   char *slash = strrchr(dir_path, '/');
   if (slash == NULL)
      goto cleanup;
   if (slash == dir_path)
      slash[1] = '\0';
   else
      *slash = '\0';

   host_name = resolved_host ? (char *)resolved_host : file_mgr_data->host;
   if (host_name == NULL)
      host_name = home_host_name;

   ProcessAction((char *)action,
                 file_view_data,
                 NULL,
                 host_name,
                 dir_path,
                 file_mgr_data->restricted_directory,
                 ((FileMgrRec *)file_mgr_data->file_mgr_rec)->file_window);

cleanup:
   ShelfFreePopupFileViewData(file_view_data);
   if (dir_path)
      XtFree(dir_path);
}

static void
ShelfRunPath(
        FileMgrRec *file_mgr_rec,
        const char *path,
        const char *action )
{
   FileMgrData *file_mgr_data;
   DialogData *dialog_data;
   char *resolved_path = NULL;
   char *resolved_host = NULL;

   if (file_mgr_rec == NULL || path == NULL || action == NULL)
      return;

   dialog_data = _DtGetInstanceData((XtPointer)file_mgr_rec);
   if (dialog_data == NULL)
      return;
   file_mgr_data = (FileMgrData *)dialog_data->data;
   if (file_mgr_data == NULL)
      return;

   resolved_path = ShelfResolvePath(file_mgr_data, path, &resolved_host);
   if (resolved_path == NULL)
      return;

   ShelfRunResolvedPath(file_mgr_data, resolved_path, resolved_host, action);

   XtFree(resolved_path);
   if (resolved_host)
      XtFree(resolved_host);
}

static void
ShelfPopupRunCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;
   ShelfPopupData *data;

   (void) w;
   (void) call_data;

   data = ShelfPopupDataGet(file_mgr_rec);
   if (data == NULL || !data->is_execute)
      return;

   ShelfRunPath(file_mgr_rec, data->path, "RunNoPrompt");
}

static void
ShelfPopupRunNoPromptCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;
   ShelfPopupData *data;

   (void) w;
   (void) call_data;

   data = ShelfPopupDataGet(file_mgr_rec);
   if (data == NULL || !data->is_execute)
      return;

   ShelfRunPath(file_mgr_rec, data->path, "Run");
}

static void
ShelfItemDestroyCB(
        Widget wid,
        XtPointer client_data,
        XtPointer call_data )
{
   ShelfItemData *item_data = (ShelfItemData *) client_data;

   (void) wid;
   (void) call_data;

   ShelfLog("ShelfItemDestroyCB: item=%p data=%p slot=%d path=%s\n",
            (void *)wid, (void *)item_data,
            item_data ? item_data->slot : -1,
            (item_data && item_data->path) ? item_data->path : "(null)");

   if (item_data)
   {
      if (item_data->drop_registered)
      {
         ShelfLog("ShelfItemDestroyCB: unregister drop site item=%p slot=%d\n",
                  (void *)wid, item_data->slot);
         DtDndDropUnregister(wid);
         item_data->drop_registered = False;
      }
      XtFree(item_data->path);
      XtFree(item_data->icon_name);
      XtFree((char *)item_data);
   }
}

static void
ShelfSlotDestroyCB(
        Widget wid,
        XtPointer client_data,
        XtPointer call_data)
{
   ShelfSlotData *slot_data = (ShelfSlotData *)client_data;

   (void) wid;
   (void) call_data;

   if (slot_data)
      XtFree((char *)slot_data);
}

static void
ShelfDropContextDestroyCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;

   (void) w;
   (void) call_data;

   if (file_mgr_rec != NULL && file_mgr_rec->shelf_frame != NULL)
      ShelfRequestRebuild(file_mgr_rec->shelf_frame);
}

static void
ShelfDropIntoSlot(
        FileMgrRec *file_mgr_rec,
        int slot,
        DtDndDropCallbackStruct *dropInfo)
{
   int i;
   int first_slot;

   if (file_mgr_rec == NULL || dropInfo == NULL || dropInfo->dropData == NULL)
      return;

   if (slot <= 0)
   {
      dropInfo->status = DtDND_FAILURE;
      return;
   }

   first_slot = slot;
   ShelfInit();
   for (i = 0; i < (int)dropInfo->dropData->numItems; i++)
   {
      const char *path = dropInfo->dropData->data.files[i];
      if (path && *path)
      {
         ShelfSetSlot(slot, path);
         slot++;
      }
   }
   if (getenv("DTFILE_SHELF_DEBUG") != NULL)
   {
      ShelfLog("ShelfDropIntoSlot: dragContext=%p shelf_drag_context=%p active=%d\n",
               (void *)dropInfo->dragContext,
               (void *)shelf_drag_context,
               shelf_drag_active ? 1 : 0);
   }

   if (slot > first_slot)
      ShelfUpdateSlotsRangeAllViews(first_slot, slot - 1);

   if (shelf_drag_active && dropInfo->operation == XmDROP_MOVE &&
       shelf_drag_slot >= 0 && shelf_drag_slot != first_slot)
   {
      ShelfClearSlotSkipIgnore(shelf_drag_slot);
      ShelfUpdateSlotAllViews(shelf_drag_slot);
   }
   dropInfo->completeMove = False;
   dropInfo->status = DtDND_SUCCESS;
}

static void
ShelfSlotDropCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   ShelfSlotData *slot_data = (ShelfSlotData *)client_data;
   DtDndDropCallbackStruct *dropInfo =
      (DtDndDropCallbackStruct *) call_data;

   (void) w;

   if (slot_data == NULL || slot_data->file_mgr_rec == NULL)
      return;

   if (dropInfo == NULL || dropInfo->dropData == NULL)
      return;

   if (dropInfo->reason == DtCR_DND_DROP_ANIMATE)
      return;

   if (dropInfo->dropData->protocol != DtDND_FILENAME_TRANSFER ||
       dropInfo->dropData->numItems <= 0)
   {
      dropInfo->status = DtDND_FAILURE;
      return;
   }

   ShelfDropIntoSlot(slot_data->file_mgr_rec, slot_data->slot, dropInfo);
}


static void
ShelfItemRegisterDropSite(Widget item, ShelfItemData *item_data)
{
   static XtCallbackRec dropCB[] = { {ShelfItemDropCB, NULL},
                                     {NULL, NULL} };

   if (item == NULL || item_data == NULL)
      return;

   dropCB[0].closure = (XtPointer)item_data;
   ShelfLog("ShelfRebuild: register drop site for slot %d\n", item_data->slot);
   if (!item_data->drop_registered)
   {
      DtDndVaDropRegister(item,
                          DtDND_FILENAME_TRANSFER,
                          XmDROP_MOVE | XmDROP_COPY | XmDROP_LINK,
                          dropCB,
                          XmNanimationStyle, XmDRAG_UNDER_SHADOW_IN,
                          NULL);
      item_data->drop_registered = True;
   }
}

static void
ShelfSlotRegisterDropSite(Widget slot, ShelfSlotData *slot_data)
{
   static XtCallbackRec dropCB[] = { {ShelfSlotDropCB, NULL},
                                     {NULL, NULL} };

   if (slot == NULL || slot_data == NULL)
      return;

   dropCB[0].closure = (XtPointer)slot_data;
   DtDndVaDropRegister(slot,
                       DtDND_FILENAME_TRANSFER,
                       XmDROP_MOVE | XmDROP_COPY | XmDROP_LINK,
                       dropCB,
                       XmNanimationStyle, XmDRAG_UNDER_SHADOW_IN,
                       NULL);
}

static void
ShelfDebugDumpTimeout(
        XtPointer client_data,
        XtIntervalId *id)
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;
   XmManagerWidget mgr;
   Widget *children = NULL;
   Cardinal num_children = 0;
   int i;

   (void) id;

   if (getenv("DTFILE_SHELF_DEBUG") == NULL || file_mgr_rec == NULL)
      return;

   if (file_mgr_rec->shelf_frame == NULL)
      return;

   ShelfLogWidget("ShelfDebugDump: shelf_frame", file_mgr_rec->shelf_frame);

   if (!XtIsManaged(file_mgr_rec->shelf_frame))
      return;

   mgr = (XmManagerWidget)file_mgr_rec->shelf_frame;
   XtVaGetValues(file_mgr_rec->shelf_frame,
                 XmNchildren, &children,
                 XmNnumChildren, &num_children,
                 NULL);
   ShelfLog("ShelfDebugDump: shelf_frame children=%u\n",
            (unsigned)num_children);
   for (i = 0; i < (int)num_children; i++)
   {
      ShelfLogWidget("ShelfDebugDump: child", children[i]);
      if (XmIsManager(children[i]))
      {
         XmManagerWidget row_mgr = (XmManagerWidget)children[i];
         Widget *row_children = NULL;
         Cardinal row_num = 0;
         int j;
         XtVaGetValues(children[i],
                       XmNchildren, &row_children,
                       XmNnumChildren, &row_num,
                       NULL);
         ShelfLog("ShelfDebugDump: row children=%u\n", (unsigned)row_num);
         for (j = 0; j < (int)row_num && j < 8; j++)
         {
            ShelfLogWidget("ShelfDebugDump: row child", row_children[j]);
         }
      }
   }
}

static void
ShelfItemDropCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   ShelfItemData *item_data = (ShelfItemData *)client_data;
   DtDndDropCallbackStruct *dropInfo =
      (DtDndDropCallbackStruct *) call_data;
   DialogData *dialog_data;
   FileMgrData *file_mgr_data;
   struct stat st;

   (void) w;

   ShelfLog("ShelfItemDropCB: item=%p data=%p\n",
            (void *)w, (void *)item_data);

   if (item_data == NULL)
      return;

   if (dropInfo == NULL || dropInfo->dropData == NULL)
      return;

   if (dropInfo->reason == DtCR_DND_DROP_ANIMATE)
      return;

   if (dropInfo->dropData->protocol != DtDND_FILENAME_TRANSFER)
   {
      dropInfo->status = DtDND_FAILURE;
      return;
   }

   if (shelf_drag_active && shelf_drag_slot == item_data->slot)
   {
      dropInfo->status = DtDND_SUCCESS;
      dropInfo->completeMove = False;
      ShelfLog("ShelfItemDropCB: drop on same slot %d - no-op\n",
               item_data->slot);
      return;
   }

   if (item_data->path == NULL)
   {
      ShelfDropIntoSlot(item_data->file_mgr_rec, item_data->slot, dropInfo);
      return;
   }

   dialog_data = _DtGetInstanceData ((XtPointer)item_data->file_mgr_rec);
   if (dialog_data == NULL)
      return;
   file_mgr_data = (FileMgrData *) dialog_data->data;
   if (file_mgr_data == NULL)
      return;

   if (stat(item_data->path, &st) != 0)
   {
      dropInfo->status = DtDND_FAILURE;
      return;
   }

   if (S_ISDIR(st.st_mode))
   {
      char **file_set = NULL;
      char **host_set = NULL;
      unsigned int modifiers = 0;
      Boolean result;
      int i;
      void (*finish_cb)() = NULL;

      if (dropInfo->operation == XmDROP_COPY)
         modifiers = ControlMask;
      else if (dropInfo->operation == XmDROP_LINK)
         modifiers = ShiftMask;

      _DtSetDroppedFileInfo(dropInfo, &file_set, &host_set);

      if (shelf_drag_active && modifiers == 0)
      {
         shelf_move_finish_slot = shelf_drag_slot;
         finish_cb = (void (*)())ShelfMoveFinishCB;
      }

      result = FileMoveCopy(file_mgr_data,
                            NULL,
                            item_data->path,
                            file_mgr_data->host,
                            host_set,
                            file_set,
                            dropInfo->dropData->numItems,
                            modifiers,
                            finish_cb,
                            NULL);

      if (!result)
         shelf_move_finish_slot = -1;

      if (file_set && host_set)
         _DtFreeDroppedFileInfo(dropInfo->dropData->numItems, file_set, host_set);

      dropInfo->completeMove = False;
      dropInfo->status = result ? DtDND_SUCCESS : DtDND_FAILURE;
      return;
   }
   ShelfDropIntoSlot(item_data->file_mgr_rec, item_data->slot, dropInfo);
   return;
}

static void
ShelfItemCallback(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
  ShelfItemData *item_data = (ShelfItemData *) client_data;
  DtIconCallbackStruct *icon_cb = NULL;
  XEvent *event = NULL;
  int reason = 0;

   if (item_data == NULL || item_data->path == NULL)
      return;

   if (call_data == NULL)
      return;

   icon_cb = (DtIconCallbackStruct *)call_data;
   reason = icon_cb->reason;
   event = icon_cb->event;

   if (reason == XmCR_DRAG && event && event->type == ButtonPress)
   {
      XButtonEvent *button_event = (XButtonEvent *) event;
      if (button_event->button == bMenuButton)
         reason = XmCR_POPUP;
   }

   if (reason == XmCR_POPUP)
   {
      if (event && (event->type == ButtonPress || event->type == ButtonRelease))
      {
         ShelfItemPopup(item_data, (XButtonEvent *)event);
      }
      return;
   }

   if (reason == XmCR_ARM)
   {
      if (event && event->type == ButtonPress)
      {
         XButtonEvent *button_event = (XButtonEvent *) event;
         if (button_event->button != Button1)
            return;
         item_data->drag_started = False;
         item_data->press_x = button_event->x;
         item_data->press_y = button_event->y;
      }
      return;
   }

   if (reason == XmCR_DRAG)
   {
      if (event && event->type == MotionNotify)
      {
         XMotionEvent *motion_event = (XMotionEvent *) event;
         int dx = motion_event->x - item_data->press_x;
         int dy = motion_event->y - item_data->press_y;
         if (!item_data->drag_started && (dx*dx + dy*dy) > 9)
         {
            item_data->drag_started = True;
            ShelfStartDrag(w, item_data, event);
         }
      }
      return;
   }

   /* DtIcon doesn't reliably distinguish single vs double click for shelf
    * items via XmCR_DEFAULT_ACTION across environments, so we do our own
    * multi-click tracking and treat DEFAULT_ACTION as a normal click. */
   if (reason == XmCR_DEFAULT_ACTION)
      reason = XmCR_SELECT;

   if (reason == XmCR_SELECT || reason == XmCR_ACTIVATE)
   {
      if (event && (event->type == ButtonPress || event->type == ButtonRelease))
      {
         XButtonEvent *bev = (XButtonEvent *)event;
         if (bev->button == Button1)
         {
            int multi = XtGetMultiClickTime(XtDisplay(w));
            if (item_data->last_click_time != 0 &&
                (int)(bev->time - item_data->last_click_time) <= multi)
            {
               item_data->last_click_time = 0;
               if (item_data->drag_started)
                  return;
               ShelfItemDefaultAction(item_data);
               return;
            }
            item_data->last_click_time = bev->time;
         }
      }
      if (item_data->drag_started)
         return;
      ShelfItemActivate(item_data);
      return;
   }
}

static void
ShelfRowButtonEH(
        Widget w,
        XtPointer client_data,
        XEvent *event,
        Boolean *cont)
{
   XButtonEvent *bev = (XButtonEvent *)event;
   Widget *children = NULL;
   Cardinal num_children = 0;
   int i;

   (void) client_data;

   if (event == NULL || event->type != ButtonRelease)
      return;

   if (bev->button != bMenuButton)
      return;

   XtVaGetValues(w,
                 XmNchildren, &children,
                 XmNnumChildren, &num_children,
                 NULL);
   if (children == NULL || num_children == 0)
      return;

   for (i = 0; i < (int)num_children; i++)
   {
      Widget child = children[i];
      Position cx = 0, cy = 0;
      Dimension cw = 0, ch = 0;
      ShelfItemData *item_data = NULL;

      if (child == NULL)
         continue;

      XtVaGetValues(child,
                    XmNx, &cx,
                    XmNy, &cy,
                    XmNwidth, &cw,
                    XmNheight, &ch,
                    XmNuserData, &item_data,
                    NULL);

      if (item_data == NULL)
         continue;

      if (bev->x < cx || bev->y < cy)
         continue;
      if ((Dimension)(bev->x - cx) > cw || (Dimension)(bev->y - cy) > ch)
         continue;

      ShelfItemPopup(item_data, bev);
      if (cont)
         *cont = False;
      return;
   }
}

static void
ShelfPopupOpenCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;
   ShelfPopupData *data;

   (void) w;
   (void) call_data;

   data = ShelfPopupDataGet(file_mgr_rec);
   if (data == NULL || data->item_data == NULL)
      return;

   ShelfItemActivate(data->item_data);
}

static void
ShelfPopupOpenNewViewCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;
   ShelfPopupData *data;

   (void) w;
   (void) call_data;

   data = ShelfPopupDataGet(file_mgr_rec);
   if (data == NULL || data->file_mgr_data == NULL || data->path == NULL)
      return;

   if (!data->is_dir)
      return;

   {
      char *resolved_host = NULL;
      char *resolved_path = ShelfResolvePath(data->file_mgr_data, data->path,
                                             &resolved_host);

      if (resolved_path == NULL)
         return;

      ShelfOpenNewView(data->file_mgr_data, resolved_path, resolved_host);

      XtFree(resolved_path);
      if (resolved_host)
         XtFree(resolved_host);
   }
}

static void
ShelfPopupRemoveCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;
   ShelfPopupData *data;

   (void) w;
   (void) call_data;

   data = ShelfPopupDataGet(file_mgr_rec);
   if (data == NULL)
      return;

   if (data->slot == 0)
      return;

   ShelfClearSlot(data->slot);
   ShelfUpdateSlotAllViews(data->slot);
}

static void
ShelfPopupMoveCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;
   ShelfPopupData *data;

   (void) w;
   (void) call_data;

   data = ShelfPopupDataGet(file_mgr_rec);
   if (data == NULL || data->file_mgr_data == NULL || data->path == NULL)
      return;

   ShelfMoveCopyLinkToCurrent(file_mgr_rec, data->file_mgr_data,
                              data->path, 0, data->slot);
}

static void
ShelfPopupCopyCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;
   ShelfPopupData *data;

   (void) w;
   (void) call_data;

   data = ShelfPopupDataGet(file_mgr_rec);
   if (data == NULL || data->file_mgr_data == NULL || data->path == NULL)
      return;

   ShelfMoveCopyLinkToCurrent(file_mgr_rec, data->file_mgr_data,
                              data->path, ControlMask, data->slot);
}

static void
ShelfPopupLinkCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;
   ShelfPopupData *data;

   (void) w;
   (void) call_data;

   data = ShelfPopupDataGet(file_mgr_rec);
   if (data == NULL || data->file_mgr_data == NULL || data->path == NULL)
      return;

   ShelfMoveCopyLinkToCurrent(file_mgr_rec, data->file_mgr_data,
                              data->path, ShiftMask, data->slot);
}

static void
ShelfPopupPopdownCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   FileMgrRec *file_mgr_rec = (FileMgrRec *)client_data;

   (void) call_data;

   (void) file_mgr_rec;
   (void) w;
}

static void
FilePopupPopdownCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data)
{
   (void) client_data;
   (void) call_data;

   (void) w;
}

static void
ShelfDragFinishCB(
        Widget w,
        XtPointer client,
        XtPointer call)
{
   DtDndDragFinishCallback cb = (DtDndDragFinishCallback) call;
   int i;

   (void) w;
   (void) client;

   shelf_drag_active = False;
   shelf_drag_slot = -1;
   shelf_drag_candidate = NULL;
   shelf_drag_widget = NULL;
   shelf_drag_context = NULL;

   if (shelf_rebuild_after_drag)
   {
      shelf_rebuild_after_drag = False;
      if (shelf_rebuild_rec != NULL && shelf_rebuild_rec->shelf_frame != NULL)
         ShelfRequestRebuild(shelf_rebuild_rec->shelf_frame);
      else
         ShelfRebuildAll();
      shelf_rebuild_rec = NULL;
   }

   if (cb->sourceIcon)
      XtDestroyWidget(cb->sourceIcon);

   for (i = 0; i < cb->dragData->numItems; i++)
   {
      XtFree(cb->dragData->data.files[i]);
      cb->dragData->data.files[i] = NULL;
   }
}

static void
ShelfConvertCB(
        Widget w,
        XtPointer client,
        XtPointer call)
{
   ShelfItemData *item_data = (ShelfItemData *) client;
   DtDndConvertCallback cb = (DtDndConvertCallback) call;

   (void) w;

   if (item_data == NULL || item_data->path == NULL)
      return;

   if (cb->reason == DtCR_DND_CONVERT_DATA)
   {
      cb->dragData->data.files[0] = XtNewString(item_data->path);
      cb->dragData->numItems = 1;
   }
   else if (cb->reason == DtCR_DND_CONVERT_DELETE)
   {
      ShelfClearSlot(item_data->slot);
      ShelfUpdateSlotAllViews(item_data->slot);
   }
}

static Pixmap
ShelfCreateDragPixmap(
        Widget w,
        unsigned int *width_out,
        unsigned int *height_out,
        unsigned int *depth_out )
{
   Pixmap dragPixmap;
   Display *dpy;
   Window root;
   int x = 0, y = 0;
   unsigned int width = 0, height = 0, border = 0, depth = 0;

   if (w == NULL)
      return None;

   dpy = XtDisplay(w);
   dragPixmap = _DtIconDraw(w, 0, 0, 0, False);
   if (dragPixmap == None)
      return None;

   if (XGetGeometry(dpy, dragPixmap, &root, &x, &y,
                    &width, &height, &border, &depth) == 0)
   {
      width = height = depth = 0;
   }

   if (width_out)
      *width_out = width;
   if (height_out)
      *height_out = height;
   if (depth_out)
      *depth_out = depth;

   return dragPixmap;
}

static Pixmap
ShelfCreateDragMask(
        Widget w,
        unsigned int width,
        unsigned int height )
{
   Pixmap dragMask;
   Display *dpy;
   unsigned char flags;
   XRectangle pRect, lRect;
   GC fillGC;
   XGCValues values;
   Arg args[3];
   Dimension shadowThickness = 0;
   Dimension marginWidth = 0;
   Dimension marginHeight = 0;
   int minX = 0, minY = 0;
   Boolean minXUndefined = True, minYUndefined = True;

   if (w == NULL || width == 0 || height == 0)
      return None;

   dpy = XtDisplay(w);
   if (dpy == NULL)
      return None;

   dragMask = XCreatePixmap(dpy,
                            RootWindowOfScreen(XtScreenOfObject(w)),
                            width, height, 1);
   fillGC = XCreateGC(dpy, dragMask, 0, (XGCValues *) NULL);

   XFillRectangle(dpy, dragMask, fillGC, 0, 0, width, height);

   values.foreground = 1;
   XChangeGC(dpy, fillGC, GCForeground, &values);

   _DtIconGetIconRects((DtIconGadget) w, &flags, &pRect, &lRect);

   if (flags & XmPIXMAP_RECT)
   {
      minX = pRect.x;
      minY = pRect.y;
      minXUndefined = minYUndefined = False;
   }

   if (flags & XmLABEL_RECT)
   {
      if (lRect.x < minX || minXUndefined)
      {
         minX = lRect.x;
         minXUndefined = False;
      }
      if (lRect.y < minY || minYUndefined)
      {
         minY = lRect.y;
         minYUndefined = False;
      }
   }

   XtSetArg(args[0], XmNshadowThickness, &shadowThickness);
   XtSetArg(args[1], XmNmarginWidth, &marginWidth);
   XtSetArg(args[2], XmNmarginHeight, &marginHeight);
   XtGetValues(w, args, 3);

   if (flags & XmPIXMAP_RECT)
   {
      XFillRectangle(dpy, dragMask, fillGC,
                     pRect.x - minX + shadowThickness + marginWidth,
                     pRect.y - minY + shadowThickness + marginHeight,
                     pRect.width - 2 * marginWidth,
                     pRect.height - 2 * marginHeight);
   }
   if (flags & XmLABEL_RECT)
   {
      XFillRectangle(dpy, dragMask, fillGC,
                     lRect.x - minX + shadowThickness + marginWidth,
                     lRect.y - minY + shadowThickness + marginHeight,
                     lRect.width - 2 * marginWidth,
                     lRect.height - 2 * marginHeight);
   }

   XFreeGC(dpy, fillGC);

   return dragMask;
}

static void
ShelfStartDrag(
        Widget w,
        ShelfItemData *item_data,
        XEvent *event)
{
   static XtCallbackRec convertCB[] = { {ShelfConvertCB, NULL},
                                        {NULL, NULL} };
   static XtCallbackRec dragFinishCB[] = { {ShelfDragFinishCB, NULL},
                                          {NULL, NULL} };
   Arg args[2];
   int numArgs = 0;
   unsigned char operations;
   Widget drag_context;
   Widget drag_icon = NULL;
   Pixmap pm = None;
   Pixmap mask = None;
   Pixmap dragPixmap = None;
   Pixmap dragMask = None;
   unsigned int dragWidth = 0;
   unsigned int dragHeight = 0;
   unsigned int dragDepth = 0;

   if (item_data == NULL || item_data->path == NULL)
      return;

   convertCB[0].closure = (XtPointer) item_data;

   operations = XmDROP_MOVE | XmDROP_COPY | XmDROP_LINK;

   dragPixmap = ShelfCreateDragPixmap(w, &dragWidth, &dragHeight, &dragDepth);
   if (dragPixmap != None)
   {
      dragMask = ShelfCreateDragMask(w, dragWidth, dragHeight);
      if (dragMask != None)
         drag_icon = DtDndCreateSourceIcon(w, dragPixmap, dragMask);
      else
         drag_icon = DtDndCreateSourceIcon(w, dragPixmap, None);
   }
   else
   {
      XtVaGetValues(w,
                    XmNpixmap, &pm,
                    XmNmask, &mask,
                    NULL);
      if (pm != None && mask != None)
         drag_icon = DtDndCreateSourceIcon(w, pm, mask);
   }
   XtSetArg(args[numArgs], DtNsourceIcon, drag_icon); numArgs++;
   shelf_drag_active = True;
   shelf_drag_slot = item_data->slot;
   drag_context = DtDndDragStart(w, event, DtDND_FILENAME_TRANSFER, 1,
                                 operations,
                                 convertCB, dragFinishCB, args, numArgs);
   shelf_drag_context = drag_context;
   if (drag_context == NULL)
   {
      item_data->drag_started = False;
      shelf_drag_active = False;
      shelf_drag_slot = -1;
      shelf_drag_context = NULL;
   }
}

static void
ShelfItemPopup(
        ShelfItemData *item_data,
        XButtonEvent *button_event )
{
   FileMgrRec *file_mgr_rec;
   DialogData *dialog_data;
   FileMgrData *file_mgr_data;
   ShelfPopupData *popup_data;
   Boolean is_dir = False;
   Boolean is_execute = False;

   if (item_data == NULL || item_data->path == NULL || button_event == NULL)
      return;

   file_mgr_rec = item_data->file_mgr_rec;
   if (file_mgr_rec == NULL)
      return;

   dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);
   if (dialog_data == NULL)
      return;

   file_mgr_data = (FileMgrData *) dialog_data->data;
   if (file_mgr_data == NULL)
      return;

   {
      struct stat st;
      if (stat(item_data->path, &st) == 0)
      {
         if (S_ISDIR(st.st_mode))
            is_dir = True;
         else if (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
            is_execute = True;
      }
   }

   popup_data = XtNew(ShelfPopupData);
   popup_data->file_mgr_rec = file_mgr_rec;
   popup_data->file_mgr_data = file_mgr_data;
   popup_data->path = XtNewString(item_data->path);
   popup_data->slot = item_data->slot;
   popup_data->is_dir = is_dir;
   popup_data->is_execute = is_execute && !is_dir;
   popup_data->item_data = item_data;

   ShelfPopupDataSet(file_mgr_rec, popup_data);
   ShelfPopupEnsureMenu(file_mgr_rec);
   ShelfPopupUpdate(file_mgr_rec, popup_data);

  if (XtIsManaged(file_mgr_rec->shelf_popup))
     XtUnmanageChild(file_mgr_rec->shelf_popup);
   XmMenuPosition(file_mgr_rec->shelf_popup, button_event);
   XtManageChild(file_mgr_rec->shelf_popup);
}

/* Pop down any posted menus under this file manager shell to avoid grab conflicts */
void
ShelfPopdownAllMenus(FileMgrRec *file_mgr_rec)
{
   (void)file_mgr_rec;
}

static void
ShelfItemActivate(
        ShelfItemData *item_data )
{
   FileMgrRec *file_mgr_rec;
   DialogData *dialog_data;
   FileMgrData *file_mgr_data;
   struct stat st;
   char *resolved_path = NULL;
   char *resolved_host = NULL;
   char *file_name = NULL;
   char *dir_path = NULL;

   if (item_data == NULL || item_data->path == NULL)
      return;

   file_mgr_rec = item_data->file_mgr_rec;
   if (file_mgr_rec == NULL)
      return;

   dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);
   if (dialog_data == NULL)
      return;

   file_mgr_data = (FileMgrData *) dialog_data->data;
   if (file_mgr_data == NULL)
      return;

   resolved_path = ShelfResolvePath(file_mgr_data, item_data->path, &resolved_host);
   if (resolved_path == NULL)
   {
      if (resolved_host)
         XtFree(resolved_host);
      return;
   }

   if (resolved_host &&
       (strcmp(resolved_host, home_host_name) != 0) &&
       (file_mgr_data->host == NULL || strcmp(resolved_host, file_mgr_data->host) != 0))
   {
      ShowNewDirectory(file_mgr_data, resolved_host, resolved_path);
      XtFree(resolved_path);
      XtFree(resolved_host);
      return;
   }

   if (stat(resolved_path, &st) != 0)
   {
      XtFree(resolved_path);
      if (resolved_host)
         XtFree(resolved_host);
      return;
   }

   if (S_ISDIR(st.st_mode))
   {
      ShowNewDirectory(file_mgr_data,
                       resolved_host ? resolved_host : file_mgr_data->host,
                       resolved_path);
      XtFree(resolved_path);
      if (resolved_host)
         XtFree(resolved_host);
      return;
   }

   {
      char *slash = strrchr(resolved_path, '/');
      if (slash == NULL)
      {
         XtFree(resolved_path);
         if (resolved_host)
            XtFree(resolved_host);
         return;
      }

      if (slash == resolved_path)
      {
         file_name = XtNewString(slash + 1);
         dir_path = XtNewString("/");
      }
      else
      {
         file_name = XtNewString(slash + 1);
         *slash = '\0';
         dir_path = XtNewString(resolved_path);
      }
   }

   if (file_mgr_data->scrollToThisFile)
      XtFree(file_mgr_data->scrollToThisFile);
   if (file_mgr_data->scrollToThisDirectory)
      XtFree(file_mgr_data->scrollToThisDirectory);
   file_mgr_data->scrollToThisFile = file_name;
   file_mgr_data->scrollToThisDirectory = dir_path;

   ShowNewDirectory(file_mgr_data,
                    resolved_host ? resolved_host : file_mgr_data->host,
                    dir_path);
   XtFree(resolved_path);
   if (resolved_host)
      XtFree(resolved_host);
}

static void
ShelfItemDefaultAction(
        ShelfItemData *item_data )
{
   FileMgrRec *file_mgr_rec;
   DialogData *dialog_data;
   FileMgrData *file_mgr_data;
   char *resolved_path = NULL;
   char *resolved_host = NULL;
   struct stat st;
   Boolean is_dir = False;

   if (item_data == NULL || item_data->path == NULL)
      return;

   file_mgr_rec = item_data->file_mgr_rec;
   if (file_mgr_rec == NULL)
      return;

   dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);
   if (dialog_data == NULL)
      return;
   file_mgr_data = (FileMgrData *) dialog_data->data;
   if (file_mgr_data == NULL)
      return;

   resolved_path = ShelfResolvePath(file_mgr_data, item_data->path, &resolved_host);
   if (resolved_path && stat(resolved_path, &st) == 0)
      is_dir = S_ISDIR(st.st_mode);

   if (is_dir)
   {
      ShelfOpenNewView(file_mgr_data, resolved_path,
                       resolved_host ? resolved_host : file_mgr_data->host);
   }
   else if (item_data->is_execute)
   {
      ShelfRunPath(file_mgr_rec, item_data->path, "Run");
   }
   else
   {
      ShelfRunPath(file_mgr_rec, item_data->path, "Open");
   }

   if (resolved_path)
      XtFree(resolved_path);
   if (resolved_host)
      XtFree(resolved_host);
}

static char *
ShelfResolvePath(
        FileMgrData *file_mgr_data,
        const char *input_path,
        char **host_out )
{
   const char *host_default = home_host_name;
   const char *path = input_path;
   const char *colon;
   char *host = NULL;
   char *path_copy = NULL;
   char *tmp;
   Tt_status tt_status;

   if (host_out)
      *host_out = NULL;

   if (input_path == NULL || *input_path == '\0')
      return NULL;

   ShelfLog("ShelfResolvePath: input=%s cur_dir=%s host=%s\n",
            input_path,
            (file_mgr_data && file_mgr_data->current_directory)
               ? file_mgr_data->current_directory : "(null)",
            (file_mgr_data && file_mgr_data->host)
               ? file_mgr_data->host : "(null)");

   if (file_mgr_data && file_mgr_data->host)
      host_default = file_mgr_data->host;

   colon = strchr(input_path, ':');
   if (colon && *(colon + 1) == '/')
   {
      size_t host_len = (size_t)(colon - input_path);
      host = (char *)XtMalloc(host_len + 1);
      memcpy(host, input_path, host_len);
      host[host_len] = '\0';
      path = colon + 1;
   }
   else
   {
      host = XtNewString(host_default);
   }

   if (path == NULL || *path == '\0')
   {
      XtFree(host);
      return NULL;
   }

   if (*path != '/')
   {
      const char *base = (file_mgr_data && file_mgr_data->current_directory) ?
                         file_mgr_data->current_directory : "";
      size_t base_len = strlen(base);
      size_t path_len = strlen(path);
      if (base_len == 0 || (base_len == 1 && base[0] == '.'))
      {
         char cwd_buf[MAXPATHLEN];
         if (getcwd(cwd_buf, sizeof(cwd_buf)) != NULL)
         {
            base_len = strlen(cwd_buf);
            path_copy = (char *)XtMalloc(base_len + path_len + 2);
            strcpy(path_copy, cwd_buf);
            if (cwd_buf[base_len - 1] != '/')
               strcat(path_copy, "/");
            strcat(path_copy, path);
            ShelfLog("ShelfResolvePath: used cwd=%s\n", cwd_buf);
         }
         else
         {
            path_copy = XtNewString(path);
         }
      }
      else
      {
         path_copy = (char *)XtMalloc(base_len + path_len + 2);
         strcpy(path_copy, base);
         if (base[base_len - 1] != '/')
            strcat(path_copy, "/");
         strcat(path_copy, path);
      }
   }
   else
   {
      path_copy = XtNewString(path);
   }

   tmp = path_copy;
   path_copy = (char *)DtEliminateDots(path_copy);
   if (path_copy == NULL)
   {
      XtFree(tmp);
      XtFree(host);
      return NULL;
   }
   if (path_copy != tmp)
      XtFree(tmp);

   if (host && (strcmp(host, home_host_name) == 0 ||
                (file_mgr_data && file_mgr_data->host &&
                 strcmp(host, file_mgr_data->host) == 0)))
   {
      char *resolved = ResolveLocalPathName(host,
                                            path_copy,
                                            NULL,
                                            home_host_name,
                                            &tt_status);
      if (tt_status == TT_OK && resolved != NULL)
      {
         XtFree(path_copy);
         path_copy = resolved;
      }
   }

   if (host_out)
      *host_out = host;
   else
      XtFree(host);

   ShelfLog("ShelfResolvePath: resolved=%s host=%s\n",
            path_copy ? path_copy : "(null)",
            host ? host : "(null)");
   return path_copy;
}

static int
ShelfGetIconSize(
        FileMgrData *file_mgr_data,
        int *icon_px_out )
{
   int icon_size = SMALL;
   int icon_px = 16;

   if (file_mgr_data)
   {
      switch (file_mgr_data->view)
      {
         case BY_NAME_AND_EXTRA_LARGE_ICON:
            icon_size = EXTRA_LARGE;
            icon_px = 48;
            break;
         case BY_NAME_AND_ICON:
            icon_size = LARGE;
            icon_px = 32;
            break;
         case BY_NAME_AND_SMALL_ICON:
         default:
            icon_size = SMALL;
            icon_px = 16;
            break;
      }
   }

   if (icon_px_out)
      *icon_px_out = icon_px;
   return icon_size;
}

static PixmapData *
ShelfGetPixmapData(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data,
        const char *resolved_path,
        int icon_size,
        char **data_type_out )
{
   struct stat st;
   char *data_type = NULL;
   char *full_name = NULL;
   char *short_name;
   PixmapData *pixmapData = NULL;

   if (data_type_out)
      *data_type_out = NULL;

   if (file_mgr_rec == NULL || resolved_path == NULL || *resolved_path == '\0')
      return NULL;

   if (stat(resolved_path, &st) != 0)
   {
      if (lstat(resolved_path, &st) != 0)
      {
         ShelfLog("ShelfGetPixmapData: stat failed path=%s\n", resolved_path);
         return NULL;
      }
   }

   data_type = (char *)DtDtsDataToDataType(
                  resolved_path, NULL, 0, &st, NULL, NULL, NULL);
   if (data_type == NULL)
   {
      ShelfLog("ShelfGetPixmapData: no datatype path=%s\n", resolved_path);
      return NULL;
   }

   full_name = XtNewString(resolved_path);
   short_name = strrchr(full_name, '/');
   if (short_name == NULL)
   {
      ShelfLog("ShelfGetPixmapData: no '/' in path=%s\n", resolved_path);
      DtDtsFreeDataType(data_type);
      XtFree(full_name);
      return NULL;
   }

   if (strcmp(full_name, "/") == 0)
   {
      short_name = full_name;
   }
   else
   {
      *short_name++ = '\0';
      if (*short_name == '\0')
      {
         short_name = full_name;
      }
   }

   ShelfLog("ShelfGetPixmapData: path=%s full=%s short=%s type=%s\n",
            resolved_path, full_name, short_name, data_type);

   pixmapData = _DtRetrievePixmapData(
                   data_type,
                   short_name,
                   full_name,
                   file_mgr_rec->shell,
                   icon_size);

   XtFree(full_name);

   if (pixmapData == NULL)
      ShelfLog("ShelfGetPixmapData: no pixmapData path=%s type=%s\n",
               resolved_path, data_type);
   else
      ShelfLog("ShelfGetPixmapData: iconFile=%s iconName=%s inst=%s host=%s size=%d\n",
               pixmapData->iconFileName ? pixmapData->iconFileName : "(null)",
               pixmapData->iconName ? pixmapData->iconName : "(null)",
               pixmapData->instanceIconName ? pixmapData->instanceIconName : "(null)",
               pixmapData->hostPrefix ? pixmapData->hostPrefix : "(null)",
               pixmapData->size);

   if (data_type_out)
      *data_type_out = data_type;
   else
      DtDtsFreeDataType(data_type);

   (void) file_mgr_data;
   return pixmapData;
}

static void
ShelfFreePixmapData(
        PixmapData *pixmapData )
{
   if (!pixmapData)
      return;

   DtDtsFreeAttributeValue(pixmapData->instanceIconName);
   DtDtsFreeAttributeValue(pixmapData->iconName);
   DtDtsFreeAttributeValue(pixmapData->hostPrefix);
   if (pixmapData->iconFileName)
      XtFree(pixmapData->iconFileName);
   XtFree((char *)pixmapData);
}

void
ShelfRebuild(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data)
{
   XmManagerWidget manager;
   Widget *children = NULL;
   Widget *children_copy = NULL;
   Cardinal num_children = 0;
   Arg args[24];
   int n;
   int i;
   int slot_count = 0;
   int max_slot = -1;
   int visible_slots = 0;
   Widget row;
   Dimension slot_width = 0;
   Dimension slot_height = 0;
   int shelf_icon_px = 16;
   int shelf_icon_size = SMALL;
   XmFontList font_list = user_font;
   XmFontListEntry entry;
   XtPointer entry_font;
   XmFontType type;
   int font_height = 0;
   Dimension desired_row_height = 0;
   IconLayoutData *layout_data = NULL;
   Pixel bg = 0;
   Pixel fg = 0;
   Pixel pix_bg = 0;
   Pixel pix_fg = 0;
   Pixel top_shadow = 0;
   Pixel bottom_shadow = 0;
   Pixel select = 0;
   Colormap colormap = 0;

   if (file_mgr_rec == NULL || file_mgr_data == NULL)
      return;

   if (file_mgr_rec->shelf_frame == NULL)
      return;

   ShelfLog("ShelfRebuild: begin shelf_frame=%p\n", (void *)file_mgr_rec->shelf_frame);
   ShelfLogWidget("ShelfRebuild: shelf_frame(before)", file_mgr_rec->shelf_frame);
   if (file_mgr_rec->shell)
      XtVaSetValues(file_mgr_rec->shell, XmNallowShellResize, False, NULL);
   XtRemoveEventHandler(file_mgr_rec->shelf_frame,
                        StructureNotifyMask, False,
                        ShelfEnforceSizeEH, (XtPointer)file_mgr_rec);
   XtAddEventHandler(file_mgr_rec->shelf_frame,
                     StructureNotifyMask, False,
                     ShelfEnforceSizeEH, (XtPointer)file_mgr_rec);
   if (getenv("DTFILE_SHELF_DEBUG") != NULL)
      XtAddEventHandler(file_mgr_rec->shelf_frame,
                        StructureNotifyMask, False,
                        ShelfWidgetEventEH, (XtPointer)shelf_frame_tag);
   ShelfInit();
   ShelfLoadMounts();
   if (ShelfGetPathForSlot(0) == NULL)
      ShelfSetSlot(0, ShelfGetHomePath());

   DPRINTF(("ShelfRebuild: slots=%d\n", ShelfGetSlotCount()));
   ShelfLog("ShelfRebuild: slots=%d\n", ShelfGetSlotCount());

   manager = (XmManagerWidget) file_mgr_rec->shelf_frame;
   XtVaGetValues(file_mgr_rec->shelf_frame,
                 XmNchildren, &children,
                 XmNnumChildren, &num_children,
                 NULL);
   ShelfLog("ShelfRebuild: shelf_frame children=%u\n",
            (unsigned)num_children);
   if (num_children > 0 && children != NULL)
   {
      Widget popup_shell = NULL;
      if (file_mgr_rec->shelf_popup != NULL)
         popup_shell = XtParent(file_mgr_rec->shelf_popup);

      children_copy = (Widget *)XtMalloc(sizeof(Widget) * num_children);
      memcpy(children_copy, children, sizeof(Widget) * num_children);
      ShelfLog("ShelfRebuild: destroying %u children\n",
               (unsigned)num_children);
      for (i = (int)num_children - 1; i >= 0; i--)
      {
         ShelfLog("ShelfRebuild: destroy child[%d]=%p\n", i, (void *)children_copy[i]);
         if (children_copy[i] == popup_shell)
            continue;
         if (file_mgr_rec->shelf_popup == children_copy[i])
            file_mgr_rec->shelf_popup = NULL;
         XtDestroyWidget(children_copy[i]);
      }
      XtFree((char *)children_copy);
   }

   n = 0;
   XtSetArg (args[n], XmNorientation, XmHORIZONTAL); n++;
   XtSetArg (args[n], XmNpacking, XmPACK_TIGHT);     n++;
  XtSetArg (args[n], XmNspacing, 4);                n++;
  XtSetArg (args[n], XmNmarginWidth, 4);            n++;
  XtSetArg (args[n], XmNmarginHeight, 4);           n++;
   XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM); n++;
   XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM); n++;
   XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM); n++;
   XtSetArg (args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
   XtSetArg (args[n], XmNrecomputeSize, False);      n++;
   XtSetArg (args[n], XmNresizeWidth, False);        n++;
   XtSetArg (args[n], XmNresizeHeight, False);       n++;
   row = XmCreateRowColumn(file_mgr_rec->shelf_frame, "shelf_row", args, n);
   ShelfLog("ShelfRebuild: created shelf_row=%p\n", (void *)row);
   XtAddEventHandler(row, StructureNotifyMask, False,
                     ShelfEnforceSizeEH, (XtPointer)file_mgr_rec);
   if (getenv("DTFILE_SHELF_DEBUG") != NULL)
      XtAddEventHandler(row, StructureNotifyMask, False,
                        ShelfWidgetEventEH, (XtPointer)shelf_row_tag);
   XtAddEventHandler(row, ButtonReleaseMask, False,
                     ShelfRowButtonEH, (XtPointer)file_mgr_rec);
   XtAddEventHandler(row,
                     ButtonPressMask | ButtonReleaseMask |
                     Button1MotionMask | Button2MotionMask,
                     False, ShelfRowDragEH, (XtPointer)file_mgr_rec);
   ShelfLogWidget("ShelfRebuild: shelf_row(after)", row);

   XtVaGetValues(row,
                 XmNmarginWidth, &shelf_slot_margin_w,
                 XmNmarginHeight, &shelf_slot_margin_h,
                 XmNspacing, &shelf_slot_spacing,
                 NULL);
   if (shelf_slot_margin_w == 0)
      shelf_slot_margin_w = 4;
   if (shelf_slot_margin_h == 0)
      shelf_slot_margin_h = 4;

   shelf_icon_size = ShelfGetIconSize(file_mgr_data, &shelf_icon_px);
   entry = FontListDefaultEntry(font_list);
   if (entry)
   {
      entry_font = XmFontListEntryGetFont(entry, &type);
      if (type == XmFONT_IS_FONTSET)
      {
         XFontSetExtents *extents = XExtentsOfFontSet((XFontSet)entry_font);
         font_height = extents->max_logical_extent.height;
      }
      else
      {
         font_height = ((XFontStruct *)entry_font)->ascent +
            ((XFontStruct *)entry_font)->descent;
      }
   }
   if (font_height <= 0)
      font_height = 12;

   desired_row_height = (Dimension)(shelf_icon_px + font_height + 20);
   if (desired_row_height < (Dimension)(shelf_icon_px + 24))
      desired_row_height = (Dimension)(shelf_icon_px + 24);
   shelf_row_height = desired_row_height;
   ShelfLog("ShelfRebuild: font_height=%d shelf_icon_px=%d shelf_row_height=%u\n",
            font_height, shelf_icon_px, (unsigned)shelf_row_height);

   XtVaSetValues(file_mgr_rec->shelf_frame,
                 XmNheight, shelf_row_height,
                 XmNminHeight, shelf_row_height,
                 XmNmaxHeight, shelf_row_height,
                 XmNresizeHeight, False,
                 XmNresizeWidth, False,
                 NULL);

   layout_data = (IconLayoutData *)file_mgr_data->layout_data;
   if (layout_data)
   {
      bg = layout_data->background;
      fg = layout_data->foreground;
      pix_bg = layout_data->pixmap_back;
      pix_fg = layout_data->pixmap_fore;
      ShelfLog("ShelfRebuild: layout colors bg=%lu fg=%lu pix_bg=%lu pix_fg=%lu\n",
               (unsigned long)bg, (unsigned long)fg,
               (unsigned long)pix_bg, (unsigned long)pix_fg);
   }
   else
   {
      XtVaGetValues(file_mgr_rec->shelf_frame,
                    XmNbackground, &bg,
                    XmNcolormap, &colormap,
                    NULL);
      if (colormap != 0)
      {
         XmGetColors(XtScreen(file_mgr_rec->shelf_frame), colormap, bg,
                     &fg, &top_shadow, &bottom_shadow, &select);
         pix_bg = bg;
         pix_fg = fg;
         ShelfLog("ShelfRebuild: default colors bg=%lu fg=%lu pix_bg=%lu pix_fg=%lu\n",
                  (unsigned long)bg, (unsigned long)fg,
                  (unsigned long)pix_bg, (unsigned long)pix_fg);
      }
   }

   {
      Dimension sw = 0, sh = 0;
      XtVaGetValues(file_mgr_rec->shelf_frame, XmNwidth, &sw,
                    XmNheight, &sh, NULL);
      if (shelf_row_height > 0)
      {
         XtVaSetValues(row,
                       XmNheight, shelf_row_height,
                       XmNminHeight, shelf_row_height,
                       XmNmaxHeight, shelf_row_height,
                       XmNresizeHeight, False,
                       XmNresizeWidth, True,
                       XmNrecomputeSize, False,
                       NULL);
         ShelfLog("ShelfRebuild: set shelf_row height=%u\n",
                  (unsigned)shelf_row_height);
      }
      else if (sh > 0)
      {
         XtVaSetValues(row,
                       XmNheight, sh,
                       XmNresizeHeight, False,
                       XmNresizeWidth, True,
                       NULL);
         ShelfLog("ShelfRebuild: set shelf_row height=%u\n", (unsigned)sh);
      }
   }

   slot_count = ShelfGetSlotCount();
   if (slot_count <= 0)
      return;

   slot_height = (shelf_row_height > 0) ? shelf_row_height
                                        : (Dimension)(shelf_icon_px + font_height + 20);
   slot_width = (Dimension)(shelf_icon_px * 3);
   shelf_slot_width = slot_width;
   shelf_slot_height = slot_height;

   {
      const ShelfSlot *slots = NULL;
      int count = 0;
      slots = ShelfGetSlots(&count);
      for (i = 0; i < count; i++)
      {
         if (slots[i].slot > max_slot)
            max_slot = slots[i].slot;
      }
   }

   {
      Dimension frame_width = 0;
      Dimension avail = 0;
      Display *dpy = XtDisplay(file_mgr_rec->shelf_frame);
      if (dpy)
      {
         int screens = ScreenCount(dpy);
         int s;
         int max_width = 0;
         for (s = 0; s < screens; s++)
         {
            int w = DisplayWidth(dpy, s);
            if (w > max_width)
               max_width = w;
         }
         frame_width = (Dimension)max_width;
      }
      if (frame_width == 0)
         XtVaGetValues(file_mgr_rec->shelf_frame, XmNwidth, &frame_width, NULL);
      if (frame_width > 0)
      {
         if (frame_width > (shelf_slot_margin_w * 2))
            avail = frame_width - (shelf_slot_margin_w * 2);
         else
            avail = frame_width;
         if (slot_width + shelf_slot_spacing > 0)
            visible_slots = (int)((avail + slot_width + shelf_slot_spacing - 1) /
                                  (slot_width + shelf_slot_spacing));
      }
      if (visible_slots < 1)
         visible_slots = max_slot + 1;
   }

   if (max_slot < 0)
      max_slot = 0;
   if (visible_slots > 0 && max_slot < (visible_slots - 1))
      max_slot = visible_slots - 1;

   for (i = 0; i <= max_slot; i++)
   {
      XmString label;
      Widget item;
      ShelfItemData *item_data;

      label = XmStringCreateLocalized("");
      n = 0;
      XtSetArg (args[n], XmNstring, label);                 n++;
      XtSetArg (args[n], XmNimageName, (String)NULL);       n++;
      XtSetArg (args[n], XmNmaxPixmapWidth, shelf_icon_px); n++;
      XtSetArg (args[n], XmNmaxPixmapHeight, shelf_icon_px); n++;
      XtSetArg (args[n], XmNunderline, False);              n++;
      XtSetArg (args[n], XmNfillMode, XmFILL_TRANSPARENT);  n++;
      XtSetArg (args[n], XmNpixmapPosition, XmPIXMAP_TOP);  n++;
      XtSetArg (args[n], XmNstringPosition, XmSTRING_BOTTOM); n++;
      XtSetArg (args[n], XmNbehavior, XmICON_BUTTON);       n++;
      XtSetArg (args[n], XmNtraversalOn, False);            n++;
      XtSetArg (args[n], XmNrecomputeSize, False);          n++;
      XtSetArg (args[n], XmNresizeHeight, False);           n++;
      XtSetArg (args[n], XmNresizeWidth, False);            n++;
      XtSetArg (args[n], XmNhighlightThickness, 2);         n++;
      XtSetArg (args[n], XmNborderWidth, 0);                n++;
      XtSetArg (args[n], XmNbackground, bg);                n++;
      XtSetArg (args[n], XmNwidth, slot_width);             n++;
      XtSetArg (args[n], XmNheight, slot_height);           n++;
      XtSetArg (args[n], XmNminWidth, slot_width);          n++;
      XtSetArg (args[n], XmNmaxWidth, slot_width);          n++;
      XtSetArg (args[n], XmNminHeight, slot_height);        n++;
      XtSetArg (args[n], XmNmaxHeight, slot_height);        n++;
      item = _DtCreateIcon(row, "shelf_item", args, n);
      ShelfLog("ShelfRebuild: created shelf_item=%p slot=%d\n", (void *)item, i);
      if (item == NULL)
      {
         XmStringFree(label);
         continue;
      }
      XtManageChild(item);
      if (XtIsRealized(item))
         XtResizeWidget(item, slot_width, slot_height, 0);
      ShelfLogWidget("ShelfRebuild: shelf_item", item);
      if (getenv("DTFILE_SHELF_DEBUG") != NULL)
      {
         String image_name = NULL;
         XmString icon_string = NULL;
         Pixmap pm = None;
         unsigned char pix_pos = 0;
         unsigned char str_pos = 0;
         XtVaGetValues(item,
                       XmNimageName, &image_name,
                       XmNstring, &icon_string,
                       XmNpixmap, &pm,
                       XmNpixmapPosition, &pix_pos,
                       XmNstringPosition, &str_pos,
                       NULL);
         ShelfLog("ShelfRebuild: item=%p imageName=%s pixmap=%lu string=%p pixPos=%u strPos=%u\n",
                  (void *)item,
                  image_name ? image_name : "(null)",
                  (unsigned long)pm,
                  (void *)icon_string,
                  (unsigned)pix_pos,
                  (unsigned)str_pos);
      }
      XmStringFree(label);

      item_data = (ShelfItemData *)XtMalloc(sizeof(ShelfItemData));
      item_data->file_mgr_rec = file_mgr_rec;
      item_data->path = NULL;
      item_data->icon_name = NULL;
      item_data->slot = i;
      item_data->drag_started = False;
      item_data->drag_possible = False;
      item_data->drag_button = 0;
      item_data->press_x = 0;
      item_data->press_y = 0;
      item_data->drop_registered = False;
      item_data->is_execute = False;
      item_data->last_click_time = 0;
      ShelfLog("ShelfRebuild: attach handlers item=%p data=%p slot=%d path=%s\n",
               (void *)item, (void *)item_data, item_data->slot,
               item_data->path ? item_data->path : "(null)");
      ShelfLog("ShelfRebuild: add callback handler item=%p\n", (void *)item);
      XtAddCallback(item, XmNcallback, ShelfItemCallback,
                    (XtPointer)item_data);
      XtVaSetValues(item, XmNuserData, item_data, NULL);
      ShelfLog("ShelfRebuild: add destroy callback item=%p\n", (void *)item);
      XtAddCallback(item, XmNdestroyCallback, ShelfItemDestroyCB,
                    (XtPointer)item_data);
      ShelfItemRegisterDropSite(item, item_data);
      if (getenv("DTFILE_SHELF_DEBUG") != NULL)
      {
         Dimension sw = 0, sh = 0;
         XtVaGetValues(item, XmNwidth, &sw, XmNheight, &sh, NULL);
         ShelfLog("ShelfRebuild: slot %d widget=%p w=%u h=%u\n",
                  i, (void *)item, (unsigned)sw, (unsigned)sh);
      }
   }

   XtManageChild(row);
   if (XtIsRealized(row))
      XtResizeWidget(row, XtWidth(row), shelf_row_height, 0);

   ShelfLog("ShelfRebuild: end shelf_frame=%p\n", (void *)file_mgr_rec->shelf_frame);
   ShelfUpdateSlotsRangeWithData(file_mgr_rec, file_mgr_data, 0, max_slot);
   /* Re-assert fixed shelf sizing after children are created to avoid collapse */
   if (shelf_row_height > 0)
   {
      XtVaSetValues(file_mgr_rec->shelf_frame,
                    XmNheight, shelf_row_height,
                    XmNminHeight, shelf_row_height,
                    XmNmaxHeight, shelf_row_height,
                    XmNresizeHeight, False,
                    NULL);
      XtVaSetValues(row,
                    XmNheight, shelf_row_height,
                    XmNminHeight, shelf_row_height,
                    XmNmaxHeight, shelf_row_height,
                    XmNresizeHeight, False,
                    XmNrecomputeSize, False,
                    NULL);
      if (XtIsRealized(row))
         XtResizeWidget(row, XtWidth(row), shelf_row_height, 0);
      if (XtIsRealized(file_mgr_rec->shelf_frame))
         XtResizeWidget(file_mgr_rec->shelf_frame,
                        XtWidth(file_mgr_rec->shelf_frame),
                        shelf_row_height, 0);
   }
   if (getenv("DTFILE_SHELF_DEBUG") != NULL)
      XtAppAddTimeOut(XtWidgetToApplicationContext(file_mgr_rec->shelf_frame),
                      200, ShelfDebugDumpTimeout, (XtPointer)file_mgr_rec);
}

static void
ShelfRegisterView(FileMgrRec *file_mgr_rec)
{
   shelf_views = (FileMgrRec **)XtRealloc((char *)shelf_views,
                                         sizeof(FileMgrRec *) *
                                         (shelf_view_count + 1));
   shelf_views[shelf_view_count] = file_mgr_rec;
   shelf_view_count++;
}

static void
ShelfUnregisterView(FileMgrRec *file_mgr_rec)
{
   int i;

   for (i = 0; i < shelf_view_count; i++)
   {
      if (shelf_views[i] == file_mgr_rec)
      {
         int j;
         for (j = i; j < shelf_view_count - 1; j++)
            shelf_views[j] = shelf_views[j + 1];
         shelf_view_count--;
         break;
      }
   }
}

static void
ShelfRebuildAllTimeout(
        XtPointer client_data,
        XtIntervalId *id)
{
   (void) client_data;
   (void) id;
   shelf_rebuild_pending = False;
   shelf_rebuild_timer = 0;
   ShelfLog("ShelfRebuildAllTimeout: running\n");
   ShelfRebuildAll();
}

void
ShelfRequestRebuild(Widget widget)
{
   XtAppContext app;

   if (shelf_drag_active || shelf_drag_context != NULL)
   {
      shelf_rebuild_after_drag = True;
      shelf_rebuild_rec = NULL;
      return;
   }

   if (shelf_rebuild_pending)
      return;

   if (widget == NULL)
      widget = toplevel;

   if (widget == NULL)
   {
      ShelfRebuildAll();
      return;
   }

   app = XtWidgetToApplicationContext(widget);
   shelf_rebuild_pending = True;
   ShelfLog("ShelfRequestRebuild: schedule rebuild widget=%p\n", (void *)widget);
   shelf_rebuild_timer = XtAppAddTimeOut(app, 0,
                                        ShelfRebuildAllTimeout, NULL);
}

void
ShelfRebuildAll(void)
{
   int i;

   for (i = 0; i < shelf_view_count; i++)
   {
      FileMgrRec *view = shelf_views[i];
      DialogData *dialog_data;
      FileMgrData *file_mgr_data;

      if (view == NULL)
         continue;

      dialog_data = _DtGetInstanceData ((XtPointer)view);
      if (dialog_data == NULL)
         continue;
      file_mgr_data = (FileMgrData *) dialog_data->data;
      if (file_mgr_data == NULL || !file_mgr_data->show_shelf)
         continue;

      ShelfRebuild(view, file_mgr_data);
   }
}


/*
 * UpdateHeaders:
 *   Update the iconic path, current directory, and status lines.
 */
void
UpdateHeaders(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data,
        Boolean icons_changed)
{
   Widget manage[5];
   int nmanage;
   Widget cur_dir_manage[4];
   int cur_dir_nmanage;
   Arg args[32];
   int n;
   PixmapData *pixmapData;
   Boolean shelf_managed;

   /*
    * No headers on the trash can.
    */
   if (!showFilesystem)
   {
      if(file_mgr_data->show_status_line)
         UpdateStatusLine(file_mgr_rec, file_mgr_data);
      return;
   }
   else if(file_mgr_data == trashFileMgrData
           && file_mgr_data )
   {
      UpdateStatusLine(file_mgr_rec, file_mgr_data);
      return;
   }

   /*
    * Make sure the iconic path & current directory widgets are
    * correctly managed & attached.
    */
   shelf_managed = (file_mgr_rec->shelf_frame != NULL &&
                    XtIsManaged(file_mgr_rec->shelf_frame));

   if ((file_mgr_data->show_shelf == 0) != (shelf_managed == 0) ||
       (file_mgr_data->show_iconic_path == 0) !=
                            (XtIsManaged(file_mgr_rec->iconic_path_da) == 0) ||
       (file_mgr_data->show_current_dir == 0) !=
                   (XtIsManaged(file_mgr_rec->current_directory_frame) == 0))
   {
      icons_changed = True;

      DPRINTF((
         "UpdateHeaders: shelf %d, iconic_path %d, current_dir %d, status_line %d\n",
               file_mgr_data->show_shelf,
               file_mgr_data->show_iconic_path,
               file_mgr_data->show_current_dir,
               file_mgr_data->show_status_line));

      if ((!file_mgr_data->show_shelf || file_mgr_rec->shelf_frame == NULL) &&
          !file_mgr_data->show_iconic_path &&
          !file_mgr_data->show_current_dir)
         XtUnmanageChild(file_mgr_rec->header_frame);

      XtUnmanageChildren(
        ((XmManagerWidget)file_mgr_rec->header_frame)->composite.children,
        ((XmManagerWidget)file_mgr_rec->header_frame)->composite.num_children);
      XtUnmanageChildren(
        ((XmManagerWidget)file_mgr_rec->current_directory_frame)->
                                                     composite.children,
        ((XmManagerWidget)file_mgr_rec->current_directory_frame)->
                                                     composite.num_children);
      nmanage = 0;

      /* attach the shelf (placeholder container for now) */
      if (file_mgr_data->show_shelf && file_mgr_rec->shelf_frame != NULL)
      {
         n = 0;
        XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);      n++;
        XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);     n++;
        XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);    n++;
        if (file_mgr_data->show_iconic_path || file_mgr_data->show_current_dir)
        {
            XtSetArg (args[n], XmNbottomAttachment, XmATTACH_NONE);    n++;
         }
         else
         {
            XtSetArg (args[n], XmNbottomAttachment, XmATTACH_FORM);    n++;
        }
        XtSetValues(file_mgr_rec->shelf_frame, args, n);
         if (file_mgr_rec->shelf_separator)
            XtManageChild(file_mgr_rec->shelf_separator);
        manage[nmanage++] = file_mgr_rec->shelf_frame;
     }

      /* attach the iconic path */
      if (file_mgr_data->show_iconic_path)
      {
         n = 0;
         if (file_mgr_data->show_shelf && file_mgr_rec->shelf_frame != NULL)
         {
            XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);    n++;
            XtSetArg (args[n], XmNtopWidget,
                               file_mgr_rec->shelf_frame);            n++;
         }
         else
         {
            XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);      n++;
         }
         XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);     n++;
         XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);    n++;
         if (file_mgr_data->show_current_dir)
         {
            XtSetArg (args[n], XmNbottomAttachment, XmATTACH_NONE);    n++;
         }
         else
         {
            XtSetArg (args[n], XmNbottomAttachment, XmATTACH_FORM);    n++;
         }
         XtSetValues(file_mgr_rec->iconic_path_da, args, n);
         manage[nmanage++] = file_mgr_rec->iconic_path_da;
      }

      /* attach the separator */
      if (file_mgr_data->show_iconic_path && file_mgr_data->show_current_dir)
      {
         n = 0;
         XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);    n++;
         XtSetArg (args[n], XmNtopWidget,
                            file_mgr_rec->iconic_path_da);         n++;
         XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);     n++;
         XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);    n++;
         XtSetValues(file_mgr_rec->header_separator, args, n);
         manage[nmanage++] = file_mgr_rec->header_separator;
      }

      /* attach the current directory line */
      if (file_mgr_data->show_current_dir)
      {
         n = 0;
         if (file_mgr_data->show_iconic_path)
         {
            XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);    n++;
            XtSetArg (args[n], XmNtopWidget,
                               file_mgr_rec->header_separator);       n++;
         }
         else if (file_mgr_data->show_shelf && file_mgr_rec->shelf_frame != NULL)
         {
            XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);    n++;
            XtSetArg (args[n], XmNtopWidget,
                               file_mgr_rec->shelf_frame);            n++;
         }
         else
         {
            XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);      n++;
         }
         XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);        n++;
         XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);       n++;
/*
         XtSetArg (args[n], XmNbottomAttachment, XmATTACH_FORM);      n++;
*/
         XtSetValues(file_mgr_rec->current_directory_frame, args, n);
         manage[nmanage++] = file_mgr_rec->current_directory_frame;

         /*
          * If the iconic path is shown, show only the current directory on
          * the current directory line;
          * if the iconic path is not shown, also show the drop target and
          * the icon representing the current directory.
          */
         cur_dir_nmanage = 1;
         cur_dir_manage[0] = file_mgr_rec->current_directory;

         if (file_mgr_data->show_iconic_path)
         {
            n = 0;
            XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);    n++;
            XtSetArg (args[n], XmNleftOffset, 5);                    n++;
            XtSetValues(file_mgr_rec->current_directory, args, n);
         }
         else
         {
            if (showDropZone)
            {
               n = 0;
               XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);     n++;
               XtSetArg (args[n], XmNleftOffset, 5);                     n++;
               XtSetValues(file_mgr_rec->current_directory_drop, args, n);
               cur_dir_manage[cur_dir_nmanage++] =
                  file_mgr_rec->current_directory_drop;

               n = 0;
               XtSetArg (args[n], XmNleftAttachment, XmATTACH_WIDGET);    n++;
               XtSetArg (args[n], XmNleftWidget,
                                  file_mgr_rec->current_directory_drop);  n++;
               XtSetArg (args[n], XmNleftOffset, 0);                      n++;
            }
            else
            {
               n = 0;
               XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);      n++;
               XtSetArg (args[n], XmNleftOffset, 10);                     n++;
            }
            XtSetValues(file_mgr_rec->current_directory_icon, args, n);
            cur_dir_manage[cur_dir_nmanage++] =
               file_mgr_rec->current_directory_icon;

            n = 0;
            XtSetArg (args[n], XmNleftAttachment, XmATTACH_WIDGET);   n++;
            XtSetArg (args[n], XmNleftWidget,
                               file_mgr_rec->current_directory_icon);n++;
            XtSetArg (args[n], XmNleftOffset, 0);                     n++;
            XtSetValues(file_mgr_rec->current_directory, args, n);
         }

      }

      if (file_mgr_data->show_shelf ||
          file_mgr_data->show_iconic_path ||
          file_mgr_data->show_current_dir)
      {
         if (file_mgr_data->show_current_dir)
            XtManageChildren(cur_dir_manage, cur_dir_nmanage);
         XtManageChildren(manage, nmanage);
         if (!XtIsManaged(file_mgr_rec->header_frame))
            XtManageChild(file_mgr_rec->header_frame);
      }

      XtSetArg (args[0], XmNallowShellResize, True);
      XtSetValues(file_mgr_rec->shell, args, 1);
   }

   if (file_mgr_data->show_shelf && file_mgr_rec->shelf_frame != NULL)
   {
      XmManagerWidget shelf_mgr = (XmManagerWidget)file_mgr_rec->shelf_frame;
      if (XtIsManaged(file_mgr_rec->shelf_frame) &&
          shelf_mgr->composite.num_children == 0)
      {
         ShelfRebuild(file_mgr_rec, file_mgr_data);
      }
      if (file_mgr_rec->shelf_separator)
         XtManageChild(file_mgr_rec->shelf_separator);
   }
   else
   {
      if (file_mgr_rec->shelf_separator)
         XtUnmanageChild(file_mgr_rec->shelf_separator);
   }

   /*
    * Make sure the status line is correctly managed.
    */
   if (file_mgr_data->show_status_line &&
                            !XtIsManaged(XtParent(file_mgr_rec->status_line)))
   {
         XtManageChild(XtParent(file_mgr_rec->status_line));
   }
   else if (!file_mgr_data->show_status_line &&
                             XtIsManaged(XtParent(file_mgr_rec->status_line)))
   {
         XtUnmanageChild(XtParent(file_mgr_rec->status_line));
   }

   if (file_mgr_data->show_iconic_path)
      DtUpdateIconicPath(file_mgr_rec, file_mgr_data, icons_changed);

   if (file_mgr_data->show_current_dir)
   {
      if (icons_changed)
      {
         /* Display the correct small directory icon */
         pixmapData = GetPixmapData(file_mgr_rec,
                                    file_mgr_data,
                                    file_mgr_data->current_directory,
                                    SMALL);
         XtSetArg (args[0], XmNallowShellResize, False);
         XtSetValues(file_mgr_rec->shell, args, 1);

         if (pixmapData)
           XtSetArg (args[0], XmNimageName, pixmapData->iconFileName);
         else
           XtSetArg (args[0], XmNimageName, NULL);
         XtSetValues(file_mgr_rec->current_directory_icon, args, 1);
         _DtCheckAndFreePixmapData(
            GetDirectoryLogicalType(file_mgr_data,
                                    file_mgr_data->current_directory),
            file_mgr_rec->shell,
            (DtIconGadget) file_mgr_rec->current_directory_icon,
            pixmapData);

         XtSetArg (args[0], XmNallowShellResize, True);
         XtSetValues(file_mgr_rec->shell, args, 1);
      }
      if(XtIsRealized(file_mgr_rec->main))
         DrawCurrentDirectory (file_mgr_rec->current_directory,
                               file_mgr_rec, file_mgr_data);
   }

   if (file_mgr_data->show_status_line)
      UpdateStatusLine(file_mgr_rec, file_mgr_data);
}


/************************************************************************
 *
 *  FileMgrRedisplayFiles
 *      Reprocess and redisplay the files of a view.
 *
 ************************************************************************/

void
FileMgrRedisplayFiles(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data,
        Boolean new_directory)
{
   _DtTurnOnHourGlass (file_mgr_rec->shell);
   GetFileData (file_mgr_data, True, file_mgr_data->branch_list);
   XmDropSiteStartUpdate(file_mgr_rec->file_window);
   UpdateFileIcons(file_mgr_rec, file_mgr_data, new_directory);
   UpdateHeaders (file_mgr_rec, file_mgr_data, False);
   LayoutFileIcons(file_mgr_rec, file_mgr_data, False, False);
   XmDropSiteEndUpdate(file_mgr_rec->file_window);
   _DtTurnOffHourGlass (file_mgr_rec->shell);
}




/************************************************************************
 *
 *  ShowNewDirectory
 *      Update the view to the new directory.
 *
 ************************************************************************/

void
ShowNewDirectory(
        FileMgrData *file_mgr_data,
        char *host_name,
        char *directory_name )
{
   FileMgrRec * file_mgr_rec;
   Arg args[1];
   char   tmp_directory_name[MAX_PATH];
   char **branch_list;
   int i;
   char *tmp_type;
   char *title;

   tmp_type = DtDtsDataToDataType(directory_name, NULL, 0, NULL, NULL,
                                  NULL, NULL);
   if (( (strcmp(tmp_type, LT_AGROUP) == 0) ||
         (strstr(tmp_type, LT_AGROUP_SUBDIR)) )
       &&
       ( !(file_mgr_data->toolbox) ))
   {
      DtActionArg *action_args;
      char *pwd_dir;

      action_args = (DtActionArg *) XtCalloc(1, sizeof(DtActionArg));
      if (action_args)
      {
         action_args[0].argClass = DtACTION_FILE;
         action_args[0].u.file.name = directory_name;
      }

      pwd_dir = XtNewString(file_mgr_data->current_directory);
      DtActionInvoke(((FileMgrRec *) file_mgr_data->file_mgr_rec)->shell,
                     "OpenAppGroup", action_args, 1,
                     NULL, NULL, pwd_dir, True, NULL, NULL);
      DtDtsFreeDataType(tmp_type);
      XtFree(pwd_dir);
      return;
   }
   else if (strcmp(tmp_type, LT_TRASH) == 0)
   {
      DtActionArg *action_args;
      char *pwd_dir;

      pwd_dir = XtNewString(file_mgr_data->current_directory);
      DtActionInvoke(((FileMgrRec *) file_mgr_data->file_mgr_rec)->shell,
                     "Trash", NULL, 0,
                     NULL, NULL, pwd_dir, True, NULL, NULL);
      DtDtsFreeDataType(tmp_type);
      XtFree(pwd_dir);
      return;
   }
   DtDtsFreeDataType(tmp_type);

   if (openDirType == NEW)
   {
      DialogData *dialog_data;
      FileMgrData *fileMgrData;

      fileMgrData = CheckOpenDir(directory_name, host_name);
      if(fileMgrData != NULL)
      {
         dialog_data = _DtGetInstanceData(fileMgrData->file_mgr_rec);
         CloseView(dialog_data);
      }
      ForceMyIconClosed(file_mgr_data->host, file_mgr_data->current_directory);
   }

   file_mgr_rec = (FileMgrRec *) file_mgr_data->file_mgr_rec;

   /*  Set the icon name for this view to the directory name.  */

   SetIconAttributes ((FileMgrRec *)file_mgr_data->file_mgr_rec, file_mgr_data,
		      directory_name);

   _DtTurnOnHourGlass (file_mgr_rec->shell);

   DeselectAllFiles (file_mgr_data);
   ActivateNoSelect (file_mgr_rec);

   /* When leaving a directory, save any positional information */
   if(file_mgr_data->object_positions)
   {
      SavePositionInfo(file_mgr_data);
      FreePositionInfo(file_mgr_data);
   }

   /* save the current branch list */
   branch_list = file_mgr_data->branch_list;
   file_mgr_data->branch_list = NULL;

   FileMgrBuildDirectories (file_mgr_data, host_name, directory_name);

   GetFileData (file_mgr_data, True, branch_list);

   if (branch_list != NULL)
   {
      for (i = 0; branch_list[i]; i++)
        XtFree (branch_list[i]);
      XtFree ((char *) branch_list);
   }

   UpdateCachedDirectories (view_set, view_count);

   /* Inherit, or load positional data for this directory */
   XmDropSiteStartUpdate(file_mgr_rec->file_window);
   LoadPositionInfo(file_mgr_data);
   UpdateFileIcons(file_mgr_rec, file_mgr_data, True);

   if (showFilesystem && file_mgr_data != trashFileMgrData)
      UpdateHeaders (file_mgr_rec, file_mgr_data, True);
   else if(file_mgr_data == trashFileMgrData
           && file_mgr_data != NULL )
      UpdateStatusLine(file_mgr_rec, file_mgr_data);

   /* change the title */
   title = _DtBuildFMTitle(file_mgr_data);
   XtSetArg (args[0], XmNtitle, title);
   XtSetValues (file_mgr_rec->shell, args, 1);
   XtFree(title);

   if (file_mgr_data->show_type == SINGLE_DIRECTORY)
      LayoutFileIcons(file_mgr_rec, file_mgr_data, True, True);
   else
      LayoutFileIcons(file_mgr_rec, file_mgr_data, False, True);

   if (openDirType == NEW)
      ForceMyIconOpen (file_mgr_data->host, NULL);
   XmDropSiteEndUpdate(file_mgr_rec->file_window);
}




/************************************************************************
 *
 *  FileMgrReread
 *      This function causes a rescan of the directory for the view
 *      and a full redisplay of the file icons.
 *
 ************************************************************************/

void
FileMgrReread(
        FileMgrRec *file_mgr_rec )
{
   DialogData * dialog_data;
   FileMgrData * file_mgr_data;
   DirectorySet ** directory_set;
   int i;


   dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);
   file_mgr_data = (FileMgrData *) dialog_data->data;

   _DtTurnOnHourGlass (file_mgr_rec->shell);

   directory_set = file_mgr_data->directory_set;

   for (i = 0; i < file_mgr_data->directory_count; i++)
      RereadDirectory (file_mgr_rec->shell, file_mgr_data->host,
                       directory_set[i]->name);

   GetFileData (file_mgr_data, False, file_mgr_data->branch_list);
   XmDropSiteStartUpdate(file_mgr_rec->file_window);
   UpdateFileIcons(file_mgr_rec, file_mgr_data, False);
   UpdateHeaders (file_mgr_rec, file_mgr_data, False);
   LayoutFileIcons(file_mgr_rec, file_mgr_data, False, True);
   XmDropSiteEndUpdate(file_mgr_rec->file_window);
   /* _DtTurnOffHourGlass (file_mgr_rec->shell); */
}



/************************************************************************
 *
 *  FileMgrBuildDirectories
 *      Given a directory name, setup the directory set structure
 *      contained in the file mgr data stucture.
 *
 ************************************************************************/

void
FileMgrBuildDirectories(
        FileMgrData *file_mgr_data,
        char *host_name,
        char *directory_name )
{
   char current_directory[MAX_PATH];
   DirectorySet ** directory_set;

   /*  Inform Main that the directory is being changed and       */
   /*  free up the data contained in the current directory set.  */

   if (file_mgr_data->current_directory != NULL)
   {
      DirectoryChanged ((XtPointer)file_mgr_data, file_mgr_data->host, host_name,
                        file_mgr_data->current_directory, directory_name);
      FreeDirectoryData (file_mgr_data);
   }

   file_mgr_data->current_directory = (char *) XtNewString(directory_name);
   (void) strcpy (current_directory, file_mgr_data->current_directory);

   /* This code will solve the path problem if user has
      Dtfile*restrictMode resource set to True and
      also invoke dtfile with -restricted option
   */
   if( restrictMode
       && file_mgr_data->toolbox == False
       && file_mgr_data->restricted_directory
       && strncmp( file_mgr_data->restricted_directory, directory_name,
                   strlen( file_mgr_data->restricted_directory ) ) != 0
     )
   {
      XtFree( file_mgr_data->restricted_directory );
      file_mgr_data->restricted_directory = XtNewString( directory_name );
   }

   if (file_mgr_data->host != NULL)
      XtFree ((char *) file_mgr_data->host);

   file_mgr_data->host = (char *) XtNewString (host_name);


   /*  Allocate the data for one initial directory set  */

   file_mgr_data->directory_set = directory_set =
      ((DirectorySet **) XtMalloc (2 * sizeof(DirectorySet *))) + 1;
   file_mgr_data->directory_count = 1;

   directory_set[-1] = NULL;

   directory_set[0] = (DirectorySet *) XtMalloc (sizeof (DirectorySet));
   directory_set[0]->name = (char *) XtNewString (current_directory);
   directory_set[0]->sub_root = NULL;
   directory_set[0]->file_count = 0;
   directory_set[0]->file_view_data = NULL;
   directory_set[0]->order_list = NULL;
   directory_set[0]->filtered_file_count = 0;
   directory_set[0]->invisible_file_count = 0;
   directory_set[0]->file_mgr_data = (XtPointer) file_mgr_data;
}


Boolean
DropOnGadget (
   FileMgrData * file_mgr_data,
   Widget w,
   Position x,
   Position y)
{
   static Region r = NULL;
   unsigned char flags;
   XRectangle pRect, lRect;

   if ((file_mgr_data->show_type != SINGLE_DIRECTORY) ||
       (file_mgr_data->view == BY_ATTRIBUTES))
   {
      if (   x >= w->core.x
          && y >= w->core.y
          && x <  (Position)(w->core.x + w->core.width)
          && y <  (Position)(w->core.y + w->core.height)
         )
      {
         return(True);
      }
   }
   else
   {
      if (r == NULL)
         r = XCreateRegion();
      else
         XSubtractRegion(r, r, r);

      _DtIconGetIconRects((DtIconGadget)w, &flags, &pRect, &lRect);

      if (flags & XmPIXMAP_RECT)
         XUnionRectWithRegion(&pRect, r, r);

      if (flags & XmLABEL_RECT)
         XUnionRectWithRegion(&lRect, r, r);

      if (XPointInRegion(r, x, y))
         return (True);
   }

   return(False);
}


/*
 * Check to see if the drop occurred within one of the primary hotspots.
 * If this is the desktop, then we need to check the hotspots according
 * to their stacking order; all other views have their hotspots checked
 * according to the order the files were loaded, since overlapping hotspots
 * cannot occur.
 */

FileViewData *
DropOnPrimaryHotspot (
   FileMgrData * file_mgr_data,
   Position drop_x,
   Position drop_y,
   DirectorySet ** directory_data)
{
   int directory_count;
   int i, j;
   FileViewData * file_view_data;
   DirectorySet * dir_data;
   ObjectPtr top;

   if (PositioningEnabledInView(file_mgr_data))
   {
      top = GetTopOfStack(file_mgr_data);

      while (top)
      {
         file_view_data = top->file_view_data;

         if(file_view_data == NULL)
         {
            top = top->next;
            continue;
         }

         if (file_view_data->displayed &&
            DropOnGadget(file_mgr_data, file_view_data->widget, drop_x, drop_y))
         {
            *directory_data = file_mgr_data->directory_set[0];
            return(file_view_data);
         }

         top = top->next;
      }
   }
   else
   {
      if (file_mgr_data->show_type == MULTIPLE_DIRECTORY) {
         i = -1;
         directory_count = file_mgr_data->directory_count;

      } else {
         i = 0;
         directory_count = 1;
      }

      for (; i < directory_count; i++)
      {
         dir_data = file_mgr_data->directory_set[i];

         for (j = 0; j < dir_data->file_count; j++)
         {
            file_view_data = dir_data->file_view_data[j];

            if (!file_view_data->displayed)
               continue;

            if (DropOnGadget(file_mgr_data, file_view_data->widget, drop_x,
                             drop_y))
            {
               *directory_data = dir_data;
               return(file_view_data);
            }
         }
      }
   }

   *directory_data = NULL;
   return(False);
}



/************************************************************************
 *
 *  ProcessDropOnFileWindow
 *
 ************************************************************************/
static void
ProcessDropOnFileWindow (
     Widget w,
     DtDndDropCallbackStruct *dropInfo,
     FileMgrData *file_mgr_data)
{
   char *command = NULL;
   char *fileType;

  /******************/
  /* transfer phase */
  /******************/
  if(dropInfo->reason != DtCR_DND_DROP_ANIMATE)
  {

    Arg args[1];
    XmDragContext drag_context;

   /* Initiating view not valid when another client initiates drag */
   if (!dragActive)
      initiating_view = NULL;

    /* reject the drop if the Protocol is buffer and it was
       dropped on the Trash Can
    */
    if (dropInfo->dropData->protocol == DtDND_BUFFER_TRANSFER)
    {
      if (file_mgr_data == trashFileMgrData
          && file_mgr_data != NULL )
      {
        dropInfo->status = DtDND_FAILURE;
        DPRINTF (("ProcessDropOnFileWindow: Rejecting buffer drop on Trash Can\n"));
        return;
      }
    }


    /* if placement is 'As Placed', set blend model to BLEND_NONE */
    /* @@@...Need to check if this will work correctly for BUFFERS */
    if (PositioningEnabledInView(file_mgr_data))
    {
      drag_context = (XmDragContext)dropInfo->dragContext;

      if (drag_context)
      {
        XtSetArg(args[0], XmNblendModel, XmBLEND_NONE);
        XtSetValues((Widget)drag_context, args, 1);
      }
    }
    /* if placement is 'Grid' */
    else
    {
      /* if initiating view is current view, set status
         flag to failure
      */
      if (initiating_view)
      {
        if ((((FileMgrData *)initiating_view)->current_directory ==
             file_mgr_data->current_directory) &&
            (dropInfo->dropData->protocol == DtDND_FILENAME_TRANSFER))
         {
            /* we actually want to allow a copy or a link to the same directory
             * but not a move.  If it's a copy or link, we want the initiating
             * view to be NULL so that later we don't error out when it checks
             * to see if they are from the same view.  We will fail out if the
             * operation is a MOVE (causing the zoom back).
             */
            fileType = GetDirectoryLogicalType(file_mgr_data,
                                        file_mgr_data->current_directory);

            command = TypeToAction(dropInfo->operation, fileType);
            if( command )
            {
               if (strcmp(command, "FILESYSTEM_MOVE") == 0)
                 dropInfo->status = DtDND_FAILURE;
               else
                 initiating_view = NULL;
            }
         }
      }
    }

    /* set the complete move flag to False since the animate
       callback handles the deletion of the original file on the move
    */

    DPRINTF(("DropOnFileWindow: Transfer Callback - Setting Complete move flag to False\n"));

    if(dropInfo->dropData->protocol == DtDND_BUFFER_TRANSFER)
        dropInfo->completeMove = True;
    else if (shelf_drag_active && dropInfo->operation == XmDROP_MOVE)
        dropInfo->completeMove = True;
    else
        /* set the complete move flag to False since we will be handling */
        /* the deletion of the original file                             */

        dropInfo->completeMove = False;
  }

  /*****************************/
  /* animate phase, run action */
  /*****************************/
  else
  {
    char *command = NULL;
    char *fileType;

    DPRINTF(("DropOnFileWindow - Animate Callback."));

    fileType = GetDirectoryLogicalType(file_mgr_data,
                                       file_mgr_data->current_directory);

    command = TypeToAction(dropInfo->operation, fileType);

    if( command )
    {
      DirectorySet *directory_set;
      int i;

      /* retrieve the fileViewData for the current directory */
      directory_set = file_mgr_data->directory_set[0];
      for( i = 0; i < directory_set->file_count; ++i )
      {
        if( strcmp(directory_set->order_list[i]->file_data->file_name, "." )
            == 0 )
        {
          RunCommand( command,
                      file_mgr_data,
                      directory_set->order_list[i],
                      NULL,
                      dropInfo,
                      w );
          break;
        }
      }
      DtDtsFreeAttributeValue( command );
    }
  }
}


/************************************************************************
 *
 *  DropOnFileWindow
 *
 ************************************************************************/

void
DropOnFileWindow (
     Widget w,
     XtPointer client_data,
     XtPointer call_data)
{
   FileMgrData * file_mgr_data       = (FileMgrData *)client_data;
   DtDndDropCallbackStruct *dropInfo = (DtDndDropCallbackStruct *)call_data;

   switch (dropInfo->dropData->protocol)
     {
       case DtDND_FILENAME_TRANSFER:

            DPRINTF(("DropOnFileWindow: Number of Files dropped are %d\n",
                      dropInfo->dropData->numItems));
           ProcessDropOnFileWindow (w, dropInfo, file_mgr_data);
           break;
       case DtDND_BUFFER_TRANSFER:
	    DPRINTF (("DropOnFileWindow: Number of Buffers dropped are %d\n",
		      dropInfo->dropData->numItems));
	    ProcessDropOnFileWindow (w, dropInfo, file_mgr_data);
           break;
       default :
           dropInfo->status = DtDND_FAILURE;
     } /* endswitch */
}


/************************************************************************
 *
 *  ProcessDropOnObject
 *
 ************************************************************************/
static void
ProcessDropOnObject(
     Widget w,
     DtDndDropCallbackStruct *dropInfo,
     FileViewData *file_view_data)
{
   char *fileType;

   DirectorySet *directory_data =
                (DirectorySet *) file_view_data->directory_set;
   FileMgrData *file_mgr_data = (FileMgrData *) directory_data->file_mgr_data;
   char *command = NULL;

   /******************/
   /* transfer phase */
   /******************/
   if (dropInfo->reason != DtCR_DND_DROP_ANIMATE)
   {
      DPRINTF(("DropOnObject: Transfer Callback\n"));

      /* Initiating view not valid when another client initiates drag */
      if (!dragActive)
         initiating_view = NULL;

      /* check for invalid trash drop */
      if (FileFromTrash(dropInfo->dropData->data.files[0]))
      {
         if (InvalidTrashDragDrop(dropInfo->operation,
               FROM_TRASH,
               ((FileMgrRec *)file_mgr_data->file_mgr_rec)->file_window))
         {
            dropInfo->status = DtDND_FAILURE;
            return;
         }
      }
      command = TypeToAction(dropInfo->operation,
                             file_view_data->file_data->logical_type);
      if(command &&
         (strncmp("FILESYSTEM_", command, strlen("FILESYSTEM_")) != 0) &&
         dropInfo->dropData->protocol == DtDND_BUFFER_TRANSFER)
          dropInfo->completeMove = True;
      else if (shelf_drag_active && dropInfo->operation == XmDROP_MOVE)
          dropInfo->completeMove = True;
      else
          /* set the complete move flag to False since we will be handling */
          /* the deletion of the original file                             */
          dropInfo->completeMove = False;
   }

   /******************************************/
   /* animate phase, retrieve action and run */
   /******************************************/
   else
   {
      command = TypeToAction(dropInfo->operation,
                             file_view_data->file_data->logical_type);
      if (command)
      {
         RunCommand (command,
                     file_mgr_data,
                     file_view_data,
                     NULL,
                     dropInfo,
                     NULL);

         DtDtsFreeAttributeValue(command);
      }
   }
}


/************************************************************************
 *
 *  DropOnObject
 *
 ************************************************************************/

void
DropOnObject (
     Widget w,
     XtPointer client_data,
     XtPointer call_data)
{
   DtDndDropCallbackStruct *dropInfo = (DtDndDropCallbackStruct *)call_data;

   switch (dropInfo->dropData->protocol)
      {
        case DtDND_FILENAME_TRANSFER:
        case DtDND_BUFFER_TRANSFER:
             ProcessDropOnObject(w, dropInfo, (FileViewData *) client_data);
             break;
        default:
             dropInfo->status = DtDND_FAILURE;
      } /* endswitch */
}


/************************************************************************
 *
 *  FileMgrPropagateSettings
 *      Set a new (dst_data) file manager view settings to an
 *      old (src_data) settings.
 *
 ************************************************************************/

void
FileMgrPropagateSettings(
        FileMgrData *src_data,
        FileMgrData *dst_data )
{

   PreferencesData * src_preferences_data;
   PreferencesData * dst_preferences_data;

   FilterData * src_filter_active_data;
   FilterData * dst_filter_active_data;
   FilterData * src_filter_edit_data;
   FilterData * dst_filter_edit_data;

   Arg args[2];
   Dimension src_width;
   Dimension src_height;

   /* Copy the preferences data from src to dst data */

   src_preferences_data = (PreferencesData *) src_data->preferences->data;
   dst_preferences_data = (PreferencesData *) dst_data->preferences->data;

   dst_data->show_type = dst_preferences_data->show_type = src_data->show_type;
   dst_data->tree_files = dst_preferences_data->tree_files =
                                                          src_data->tree_files;
   dst_data->view = src_data->view;
   dst_data->view_single = dst_preferences_data->view_single =
                                                        src_data->view_single;
   dst_data->view_tree = dst_preferences_data->view_tree =
                                                        src_data->view_tree;
   dst_data->order = dst_preferences_data->order = src_data->order;
   dst_data->direction = dst_preferences_data->direction = src_data->direction;
   dst_data->positionEnabled = dst_preferences_data->positionEnabled =
                                                    src_data->positionEnabled;
   dst_data->show_shelf = dst_preferences_data->show_shelf =
                                                   src_data->show_shelf;
   dst_data->show_iconic_path = dst_preferences_data->show_iconic_path =
                                                    src_data->show_iconic_path;
   dst_data->show_current_dir = dst_preferences_data->show_current_dir =
                                                    src_data->show_current_dir;
   dst_data->show_status_line = dst_preferences_data->show_status_line =
                                                    src_data->show_status_line;


   /* Copy the Filter active info from src to dest data */
   src_filter_active_data = (FilterData *) src_data->filter_active->data;
   dst_filter_active_data = (FilterData *) dst_data->filter_active->data;

   dst_filter_active_data->match_flag = src_filter_active_data->match_flag;
   dst_filter_active_data->filter = XtNewString(src_filter_active_data->filter);
   dst_filter_active_data->show_hidden = src_filter_active_data->show_hidden;
   dst_filter_active_data->filetypesFilteredCount =
                                src_filter_active_data->filetypesFilteredCount;
   XtFree(dst_filter_active_data->filetypesFiltered);
   dst_filter_active_data->filetypesFiltered =
                        XtNewString(src_filter_active_data->filetypesFiltered);
   dst_filter_active_data->count = ReadInFiletypes(dst_filter_active_data);

   /* Copy the Filter edit info from src to dest data */
   src_filter_edit_data = (FilterData *) src_data->filter_edit->data;
   dst_filter_edit_data = (FilterData *) dst_data->filter_edit->data;

   dst_filter_edit_data->match_flag = src_filter_edit_data->match_flag;
   dst_filter_edit_data->filter = XtNewString(src_filter_edit_data->filter);
   dst_filter_edit_data->show_hidden = src_filter_edit_data->show_hidden;
   dst_filter_edit_data->filetypesFilteredCount =
                                src_filter_edit_data->filetypesFilteredCount;
   XtFree(dst_filter_edit_data->filetypesFiltered);
   dst_filter_edit_data->filetypesFiltered =
                        XtNewString(src_filter_edit_data->filetypesFiltered);
   dst_filter_edit_data->count = ReadInFiletypes(dst_filter_edit_data);

   /* Get the size info (e.g. X, Y) form src to dest data */
   XtSetArg (args[0], XmNwidth, &src_width);
   XtSetArg (args[1], XmNheight, &src_height);
   XtGetValues (((FileMgrRec *) src_data->file_mgr_rec)->shell, args, 2);

   dst_data->height = src_height;
   dst_data->width = src_width;

   /* need to propagate whether its a toolbox or not */
   dst_data->toolbox = src_data->toolbox;
}


/************************************************************************
 *
 *  UpdateBranchList
 *      Update the list of open tree branches
 *
 ************************************************************************/

void
UpdateBranchList(
        FileMgrData *file_mgr_data,
        DirectorySet *directory_set)
{
  int i;
  char ** table = NULL;
  int table_size = 0;
  char *branch_name;
  char *branch_entry;

  /* check if the given directory set is already in the table */
  if (directory_set && file_mgr_data->branch_list) {
    for (i = 0; file_mgr_data->branch_list[i]; i++)
      if (strcmp(file_mgr_data->branch_list[i] + 2, directory_set->name) == 0)
      {
        /* just update the TreeShow value for this branch and return */
        file_mgr_data->branch_list[i][0] = '0' + directory_set->sub_root->ts;
        return;
      }
  }

  /* free old branch_list, if any */
  if (file_mgr_data->branch_list) {
    for (i = 0; file_mgr_data->branch_list[i]; i++)
      XtFree (file_mgr_data->branch_list[i]);
    XtFree((char *)file_mgr_data->branch_list);
    file_mgr_data->branch_list = NULL;
  }

  /* create new brach list */
  for (i = 0; i < file_mgr_data->directory_count; i++) {

    if (file_mgr_data->directory_set[i]->sub_root->ts < tsNotRead)
      continue;

    /* get the name of the tree branch */
    branch_name = file_mgr_data->directory_set[i]->name;

    /* generate table entry */
    branch_entry = (char *)XtMalloc(2 + strlen(branch_name) + 1);
    branch_entry[0] = '0' + file_mgr_data->directory_set[i]->sub_root->ts;
    branch_entry[1] = ':';
    strcpy(branch_entry + 2, branch_name);

    /* add entry to the table */
    table_size++;
    table = (char **)XtRealloc((char *)table,
                                      sizeof(char *) * (table_size + 1));
    table[table_size - 1] = branch_entry;
    table[table_size] = NULL;
  }

  file_mgr_data->branch_list = table;
}


/************************************************************************
 *
 *  GetFileData
 *      Read the directory contained in host: current_directory and
 *      build the file data list and reset any related information
 *
 ************************************************************************/

static void
GetFileData(
        FileMgrData *file_mgr_data,
        Boolean valid,
        char **branch_list)
{
   FileMgrRec *file_mgr_rec;
   int directory_count;
   DirectorySet ** directory_set;
   DirectorySet ** new_dir_set;
   FileViewData ** new_view_data;
   FileViewData *new_renaming,*new_popup_menu_icon=NULL;
   FileViewData *file_view_data,*new_drag_file_view_data=NULL;
   int new_dir_count;
   int new_file_count;
   ObjectPtr position_info;
   int i;
   int j;
   int k;
   Boolean match;


   file_mgr_rec = (FileMgrRec *) file_mgr_data->file_mgr_rec;
   directory_count = file_mgr_data->directory_count;
   directory_set = file_mgr_data->directory_set;

   /*
    * Read the directory and subdirectories given by branch_list.
    * Note: if any directory we need isn't yet in the cache,
    *       ReadTreeDirectory will just kick off a background process
    *       to read the directory and return with
    *       file_mgr_data->busy_status set to busy_readdir.
    */
   ReadTreeDirectory (file_mgr_rec->shell, file_mgr_data->host,
                      file_mgr_data->current_directory,
                      file_mgr_data, branch_list,
                      &new_dir_set, &new_dir_count);

   /* if directories not ready yet, don't do anything */
   if (file_mgr_data->busy_status != not_busy)
   {
      return;
   }

   /*
    * Try to preserve the 'widget' and 'position_info' fields in the
    * file_view_data structure, for each file.  This will allow us to
    * re-use the same Icon widget, to reduce flashing.
    * Also preserve ndir & nfile counts (used in FilterFiles to decide
    * whether to expand or collapse a tree branch if ndir or nfile
    * becomes zero).
    */

   /* loop through the old directory set */
   new_renaming = NULL;
   for (i = 0; i < directory_count; i++)
   {
      /* find a directory with same name in the new directory set */
      for (j = 0; j < new_dir_count; j++)
         if (strcmp(directory_set[i]->name, new_dir_set[j]->name) == 0)
            break;

      /* if we couldn't find this directory, continue to the next one */
      if (j == new_dir_count)
         continue;

      new_view_data = new_dir_set[j]->file_view_data;
      new_file_count = new_dir_set[j]->file_count;

      /* loop throught the old file list */
      for (j = 0; j < directory_set[i]->file_count; j++)
      {
         file_view_data = directory_set[i]->file_view_data[j];
         position_info = file_view_data->position_info;

         /*
          * Find a file by the same name in the new file list.
          */
         for (k = 0; k < new_file_count; k++)
         {
            if (new_view_data[k]->file_data == file_view_data->file_data)
            {
                /* Fix for defect 5029    */
               if(file_mgr_data->popup_menu_icon && file_view_data->file_data==
                       file_mgr_data->popup_menu_icon->file_data)
                  new_popup_menu_icon = new_view_data[k];

               /* Fix for defect 5703 */
                if ( file_mgr_data->drag_file_view_data &&
                   file_mgr_data->drag_file_view_data->file_data ==
                     file_view_data->file_data)
                   new_drag_file_view_data = new_view_data[k];

               if (file_view_data == file_mgr_data->renaming)
                 new_renaming = new_view_data[k];

               /* re-use the old widgets */
               new_view_data[k]->widget = file_view_data->widget;
               new_view_data[k]->treebtn = file_view_data->treebtn;
               new_view_data[k]->registered = file_view_data->registered;

               /* preserve ndir, nfile counts */
               new_view_data[k]->ndir = file_view_data->ndir;
               new_view_data[k]->nfile = file_view_data->nfile;

               /* preserve the position info */
               if (position_info)
               {
                  new_view_data[k]->position_info = position_info;
                  position_info->file_view_data = new_view_data[k];
               }

               /* preserve icon_mtime */
               new_view_data[k]->icon_mtime = file_view_data->icon_mtime;

               break;
            }
         }

         /* if no file by the same name was found in the new file list,
            the file must have gone away ... lets eliminate the
            position infomation */
         if (position_info && k == new_file_count)
         {
            for (k = 0; k < file_mgr_data->num_objects; k++)
            {
               if (file_mgr_data->object_positions[k] == position_info)
               {
                  /* @@@ what does this do? */
                  position_info->late_bind = True;
                  position_info->y += (file_view_data->widget->core.height / 2);
               }
            }
         }
      }
   }

   /* replace the old directory set */
   file_mgr_data->directory_set = new_dir_set;
   file_mgr_data->directory_count = new_dir_count;
   file_mgr_data->renaming = new_renaming;
   file_mgr_data->popup_menu_icon = new_popup_menu_icon;
   file_mgr_data->drag_file_view_data = new_drag_file_view_data;

   /*  Get the file types and the files sorted and filtered  */
   for (i = 0; i < new_dir_count; i++)
   {
      OrderFiles (file_mgr_data, new_dir_set[i]);
      FilterFiles (file_mgr_data, new_dir_set[i]);
      file_mgr_data->tree_root->filtered =
         file_mgr_data->show_type != MULTIPLE_DIRECTORY;
   }
   SelectVisible(file_mgr_data);

   /* update the branch_list */
   UpdateBranchList(file_mgr_data, NULL);

   /*  Update the selection list */
   j = 0;
   while (j < file_mgr_data->selected_file_count)
   {
      file_view_data = file_mgr_data->selection_list[j];

      /*  See if the selected file is still around */
      match = False;
      for (i = 0; !match && i < new_dir_count; i++)
      {
         for (k = 0; k < new_dir_set[i]->file_count; k++)
         {
            if (file_view_data->file_data ==
                new_dir_set[i]->file_view_data[k]->file_data)
            {
                match = True;
                file_view_data =
                   file_mgr_data->selection_list[j] =
                      new_dir_set[i]->file_view_data[k];
                break;
            }
         }
      }
      /* Keep the file selected only if it was found in the new
       * directory set and if it is not filtered */
      if (match && !file_view_data->filtered)
         j++;
      else
         DeselectFile (file_mgr_data, file_view_data, False);
   }

   /* free the old directory set */
   FreeLayoutData(file_mgr_data->layout_data);
   file_mgr_data->layout_data = NULL;
   FreeDirectorySet(directory_set, directory_count);

   /*  Set the menu activation to reflect and changes in the selection.  */

   if(file_mgr_data != trashFileMgrData
      && file_mgr_data != NULL )
   {
      if (file_mgr_data->selected_file_count == 0)
         ActivateNoSelect ((FileMgrRec *) file_mgr_data->file_mgr_rec);
      else if (file_mgr_data->selected_file_count == 1)
         ActivateSingleSelect ((FileMgrRec *) file_mgr_data->file_mgr_rec,
                   file_mgr_data->selection_list[0]->file_data->logical_type);
      else
         ActivateMultipleSelect ((FileMgrRec *) file_mgr_data->file_mgr_rec);
   }
   else
      SensitizeTrashBtns();

   /* load positional data for this directory */
   if ( (file_mgr_data->positionEnabled == RANDOM_ON)
        && ( (file_mgr_rec->menuStates & CLEAN_UP_OP) == 0)
      )
      LoadPositionInfo(file_mgr_data);
}



/************************************************************************
 *
 *  QueryBranchList
 *      Search for a directory in branch list.
 *
 ************************************************************************/

Boolean
QueryBranchList(
        FileMgrData *file_mgr_data,
        char **branch_list,
        char *directory_name,
        TreeShow *tsp)
{
  int i;
  TreeShow ts;

  if (branch_list == NULL)
    return False;

  for (i = 0; branch_list[i]; i++)
    if (strcmp(branch_list[i] + 2, directory_name) == 0) {
      ts = branch_list[i][0] - '0';
      if (ts == tsNotRead)
        return False;

      if (ts == tsAll && file_mgr_data->tree_files == TREE_FILES_NEVER)
        *tsp = tsDirs;
      else if (ts == tsDirs && file_mgr_data->tree_files == TREE_FILES_ALWAYS)
        *tsp = tsAll;
      else
        *tsp = ts;

      return True;
    }

  return False;
}


/************************************************************************
 *
 *  ReadTreeDirectory
 *      Read a directory and sub directories.
 *
 ************************************************************************/

static FileViewData *
GetTopInfo(
        FileMgrData *file_mgr_data,
        char *host_name,
        char *directory_name,
        char **branch_list)
{
  FileData *fp;
  FileViewData *ip;
  char *p;
  TreeShow ts;

  /* allocate new FileData */
  fp = (FileData *) XtMalloc(sizeof(FileData));
  memset(fp, 0, sizeof(FileData));

  /* get the name */
  if (strcmp(directory_name, "/") == 0)
    fp->file_name = XtNewString("/");
  else
  {
    p = strrchr(directory_name, '/');
    if (p)
      fp->file_name = XtNewString(p + 1);
    else
      fp->file_name = XtNewString(directory_name);
  }

  /* assume it's a directory for now ... */
  fp->is_subdir = True;

  /* @@@ do a readlink here ... */

  /* allocate FileViewData */
  ip = (FileViewData *)XtMalloc(sizeof(FileViewData));
  memset(ip, 0, sizeof(FileViewData));
  ip->file_data = fp;

  if (QueryBranchList(file_mgr_data, branch_list, directory_name, &ts) &&
      ts >= tsNone)
  {
    ip->ts = ts;
  }
  else if (file_mgr_data->tree_files == TREE_FILES_ALWAYS)
    ip->ts = tsAll;
  else
    ip->ts = tsDirs;

  return ip;
}


static void
CountDirectories(
        FileViewData *ip,
        int *np)
/*
 * Recursively count the number of subdirectores we have read.
 */
{
  FileViewData *dp;

  if (ip->file_data->is_subdir && ip->ts != tsNotRead)
    (*np)++;

  for (dp = ip->desc; dp; dp = dp->next)
    CountDirectories(dp, np);
}


static DirectorySet *
NewDirectorySet(
        char *name,
        FileViewData *ip,
        FileMgrData *file_mgr_data)
{
  DirectorySet *ds;
  FileViewData *dp;
  int i;

  /* allocate a new directory set entry */
  ds = (DirectorySet *)XtMalloc(sizeof(DirectorySet));

  /* initialize the directory set entry */
  ds->name = XtNewString(name);
  ds->sub_root = ip;
  ds->file_count = 0;
  for (dp = ip->desc; dp; dp = dp->next)
    ds->file_count++;
  if (ds->file_count != 0)
    ds->file_view_data =
      (FileViewData **)XtMalloc(ds->file_count*sizeof(FileViewData *));
  else
    ds->file_view_data = NULL;

  for (dp = ip->desc, i = 0; dp; dp = dp->next, i++) {
    ds->file_view_data[i] = dp;
    dp->directory_set = (XtPointer)ds;
  }
  ds->order_list = NULL;
  ds->filtered_file_count = 0;
  ds->invisible_file_count = 0;
  ds->file_mgr_data = (XtPointer)file_mgr_data;

  return ds;
}


static void
MakeDirectorySets(
        FileMgrData *file_mgr_data,
        FileViewData *ip,
        DirectorySet **directory_set,
        int *index)
/*
 * Recursively add directores to the directory set array.
 */
{
  char fullname[MAX_PATH];
  FileViewData *dp;

  if (ip->file_data->is_subdir && ip->ts != tsNotRead)
  {
    /* add a new entry to the directory set array */
    GetFullName(file_mgr_data, ip, fullname);
    directory_set[*index] = NewDirectorySet(fullname, ip, file_mgr_data);
    (*index)++;
  }

  for (dp = ip->desc; dp; dp = dp->next)
    MakeDirectorySets(file_mgr_data, dp, directory_set, index);
}


static void
ReadTreeDirectory(
        Widget w,
        char *host_name,
        char *directory_name,
        FileMgrData *file_mgr_data,
        char **branch_list,
        DirectorySet ***directory_set,
        int *directory_count)
{
  FileViewData *ip, *dp;
  int i, rc;

  /* eliminate "/." */
  if (strcmp(directory_name, "/.") == 0)
    directory_name = "/";

  /* if not in tree mode, clear branch_list (@@@ really?) */
  if (file_mgr_data->show_type != MULTIPLE_DIRECTORY)
    branch_list = NULL;

  /* get a FileViewData for the tree root */
  file_mgr_data->tree_root = ip =
     GetTopInfo(file_mgr_data, host_name, directory_name, branch_list);

  /* read the directory tree */
  rc = ReadDir(w, file_mgr_data, host_name, directory_name, ip,
               0, file_mgr_data->tree_preread_level, branch_list);
  if (file_mgr_data->busy_status != not_busy)
  {
     /* No point in continuing any further, free up all 'ip' and return */
     XtFree(ip->file_data->file_name);
     XtFree((char *)ip->file_data);
     XtFree((char *)ip);
     file_mgr_data->tree_root = NULL;
     return;
  }
  if (rc)
    ip->ts = tsError;

  /* update root FileData from "." */
  for (dp = ip->desc; dp; dp = dp->next)
    if (strcmp(dp->file_data->file_name, ".") == 0) {
      ip->file_data->physical_type = dp->file_data->physical_type;
      ip->file_data->logical_type  =
                       GetDirectoryLogicalType(file_mgr_data, directory_name);
      ip->file_data->errnum        = dp->file_data->errnum;
      ip->file_data->stat          = dp->file_data->stat;
      ip->file_data->is_broken     = dp->file_data->is_broken;
      break;
    }

  *directory_count = 0;
  CountDirectories(ip, directory_count);

  /* allocate array of directory set pointers */
  *directory_set =
    (DirectorySet **) XtMalloc ((*directory_count + 1)*sizeof(DirectorySet *));

  /* make a fake directory set for the tree root */
  {
    char fullname[MAX_PATH];
    DirectorySet *ds;
    char *p;

    ds = (DirectorySet *)XtMalloc(sizeof(DirectorySet));
    ip->directory_set = (XtPointer)ds;

    strcpy(fullname, directory_name);
    p = strrchr(fullname, '/');
    if (p)
      *p = '\0';
    ds->name = XtNewString(fullname);
    ds->sub_root = NULL;
    ds->file_count = 1;
    ds->file_view_data =
      (FileViewData **)XtMalloc(sizeof(FileViewData *));
    ds->file_view_data[0] = ip;
    ds->order_list =
      (FileViewData **)XtMalloc(sizeof(FileViewData *));
    ds->order_list[0] = ip;
    ds->filtered_file_count = 1;
    ds->file_mgr_data = (XtPointer)file_mgr_data;

    (*directory_set)[0] = ds;
    (*directory_set)++;
  }

  /* make directory sets for the current dir and subdirs */
  i = 0;
  MakeDirectorySets(file_mgr_data, ip, *directory_set, &i);
}


/*--------------------------------------------------------------------
 * filtering
 *------------------------------------------------------------------*/

static Bool
IsShown(
        FileMgrData *fmd,
        FileViewData *ip)
/*
 * Decide if entry is currently displayed.
 */
{
  TreeShow ts;

  /* filtered files are not shown */
  if (ip->filtered)
    return False;

  /* in flat mode all un-filtered files are shown */
  if (fmd->show_type == SINGLE_DIRECTORY)
    return True;

  /* in tree mode an entry is shown only if user chooses to */
  ts = ip->parent? ip->parent->ts: tsDirs;
  if (ts == tsAll)
    return True;
  else if (ts == tsDirs)
    return ip->file_data->is_subdir;
  else
    return False;
}


static void
SetDisplayedRecur(
        FileMgrData *fmd,
        FileViewData *dp,     /* directory entry being searched */
        int level)            /* tree depth level of this entry */
/*
 * Recursively determine the display position of a given entry
 * Return false if the entry not currently displayed
 */
{
  FileViewData *ip;

  /* skip entries that are not displayed */
  if (level > 0 && !IsShown(fmd, dp))
    return;

  /* this entry is displayed */
  dp->displayed = True;

  /* traverse subtree */
  level++;

  for (ip = dp->desc; ip; ip = ip->next)
    SetDisplayedRecur(fmd, ip, level);

  return;
}


static void
SelectVisible (FileMgrData *file_mgr_data)
{
  int i, j;

  /* assume nothing displayed */
  for (i = 0; i < file_mgr_data->directory_count; i++)
    for (j = 0; j < file_mgr_data->directory_set[i]->file_count; j++)
      file_mgr_data->directory_set[i]->file_view_data[j]->displayed = False;

  /* set the displayed flag for all entries that are actually shown */
  SetDisplayedRecur(file_mgr_data, file_mgr_data->tree_root, 0);
  if (file_mgr_data->show_type == SINGLE_DIRECTORY)
    file_mgr_data->tree_root->displayed = False;
}


/*--------------------------------------------------------------------
 * expand tree branches
 *------------------------------------------------------------------*/

/*
 * UpdateBranchState:
 *   Determine new tree brach expansion state after a subdirectory has
 *   been re-read (op == BRANCH_UPDATE), or after the user has requested
 *   to expand (op == BRANCH_EXPAND) or collapse (op == BRANCH_COLLAPSE)
 *   a tree branch.
 */
void
UpdateBranchState(
        FileMgrData *file_mgr_data,
        FileViewData *ip,
        int op,
        Boolean busy)
{
  TreeShow old_ts = ip->ts;

  if (ip->ts == tsReading && op == BRANCH_UPDATE)
  {
     ip->ts = tsNone;
     op = BRANCH_EXPAND;
  }

  if (busy)
  {
    /* this is a new branch that's currently being read */
    ip->ts = tsReading;
  }

  else if (ip->ts == tsError)
  {
    /* can't expand or collaps this branch */
    ;
  }

  else if (op == BRANCH_UPDATE) /* update */
  {
  }

  else if (op == BRANCH_EXPAND) /* show more */
  {
    if (file_mgr_data->tree_files == TREE_FILES_NEVER)
    {
      if (ip->ts == tsNone || !showEmptySet && ip->ndir == 0)
        ip->ts = tsDirs;
      else
        ip->ts = tsNone;

    }
    else
    {
      if (ip->ndir == 0 && ip->nfile == 0)
      {
        /* the subdir is empty */
        if (!showEmptySet)
        ip->ts = tsDirs;
        else if (ip->ts == tsNone)
           ip->ts = tsDirs;
        else
           ip->ts = tsNone;
      }
      else if (ip->ts == tsAll)
        ip->ts = tsNone;
      else if (ip->ts == tsNone &&
               ip->ndir > 0 && ip->nfile > 0 &&
               file_mgr_data->tree_files == TREE_FILES_CHOOSE)
        ip->ts = tsDirs;
      else
        ip->ts = tsAll;
    }

    if (showEmptyMsg &&
        ip->ndir == 0 &&
        (ip->nfile == 0 || file_mgr_data->tree_files == TREE_FILES_NEVER) &&
        ip->ts == tsDirs)
    {
      DirectorySet *directory_set = (DirectorySet *)ip->directory_set;
      FileMgrData *file_mgr_data = (FileMgrData *)directory_set->file_mgr_data;
      FileMgrRec *file_mgr_rec = (FileMgrRec *)file_mgr_data->file_mgr_rec;
      char *msg;
      char buf[1024];

      if (ip->nfile == 0)
        msg = GETMESSAGE(12,18, "The folder %s is empty.");
      else
        msg = GETMESSAGE(12,19, "The folder %s contains no subdirectories.");

      sprintf(buf, msg, ip->file_data->file_name);
      SetSpecialMsg( file_mgr_rec, file_mgr_data, buf );
    }
  }

  else if (op == BRANCH_COLLAPSE) /* show less */
  {


    if (file_mgr_data->tree_files == TREE_FILES_NEVER)
    {
      if (ip->ts == tsNone || !showEmptySet && ip->ndir == 0)
        ip->ts = tsDirs;
      else
        ip->ts = tsNone;
    }
    else
    {
      if (ip->ndir == 0 && ip->nfile == 0)
      {
        /* the subdir is empty */
        if (!showEmptySet)
        ip->ts = tsDirs;
      else if (ip->ts == tsNone)
           ip->ts = tsDirs;
        else
           ip->ts = tsNone;
      }
      else if (ip->ts == tsNone)
        ip->ts = tsAll;
      else if (ip->ts == tsAll &&
               ip->ndir > 0 && ip->nfile > 0 &&
               file_mgr_data->tree_files == TREE_FILES_CHOOSE)
        ip->ts = tsDirs;
      else
        ip->ts = tsNone;
    }
  }
}


/*
 * DirTreeExpand:
 *   Expand (expand == True) or collpase (expand == False) a tree branch.
 */
void
DirTreeExpand(
        FileMgrData *file_mgr_data,
        FileViewData *ip,
        Boolean expand)
{
  FileMgrRec *file_mgr_rec = (FileMgrRec *) file_mgr_data->file_mgr_rec;
  DirectorySet *directory_set;
  int level, i, n, old_count, rc;
  char path[1024];
  Pixmap px;
  Arg args[20];
  Boolean new_branch;

  GetAncestorInfo(file_mgr_data, ip, &level, path, NULL);
  SetSpecialMsg( file_mgr_rec, file_mgr_data, NULL );

  if (expand) {
    /* show more */

    new_branch = (ip->ts == tsNotRead || ip->ts == tsError);
    if (new_branch) {

      /* we need to read the sub directory */
      _DtTurnOnHourGlass (file_mgr_rec->shell);
      if (ip->ts == tsError)
      {
        if (file_mgr_data->busy_status == not_busy)
        {
           file_mgr_data->busy_detail = 0;
           file_mgr_data->busy_status = initiating_readdir;
        }
        RereadDirectory (file_mgr_rec->shell, file_mgr_data->host, path);
        if (file_mgr_data->busy_status == initiating_readdir)
           file_mgr_data->busy_status = not_busy;
      }
      rc = ReadDir(file_mgr_rec->shell, file_mgr_data,
                   file_mgr_data->host, path, ip,
                   level, level, NULL);

      /* create new directory set entry */
      directory_set = NewDirectorySet(path, ip, file_mgr_data);

      /* if this is a new entry, add it to the directory set */
      if (ip->ts == tsNotRead)
      {
        file_mgr_data->directory_set =
         ((DirectorySet **)
           XtRealloc((char *)(file_mgr_data->directory_set - 1),
            (file_mgr_data->directory_count + 2)*sizeof(DirectorySet *))) + 1;
        file_mgr_data->directory_set[file_mgr_data->directory_count] =
            directory_set;
        file_mgr_data->directory_count++;
      }
      else
      {
        /* otherwise, replace the existing entry */
        for (i = 0; i < file_mgr_data->directory_count; i++)
          if (strcmp(file_mgr_data->directory_set[i]->name, path) == 0)
            break;
        XtFree(file_mgr_data->directory_set[i]->name);
        file_mgr_data->directory_set[i]->name = NULL;
	if (file_mgr_data->directory_set[i]->file_view_data != NULL)
          XtFree((char *)file_mgr_data->directory_set[i]->file_view_data);
        file_mgr_data->directory_set[i]->file_view_data = NULL;
        XtFree((char *)file_mgr_data->directory_set[i]);
        file_mgr_data->directory_set[i] = directory_set;
      }

      if (rc != 0 ||
          ip->ts == tsError && file_mgr_data->busy_status == busy_readdir)
         ip->ts = tsError;
      else
         ip->ts = tsNone;

      if (file_mgr_data->busy_status != busy_readdir)
      {
         OrderFiles (file_mgr_data, directory_set);
         FilterFiles (file_mgr_data, directory_set);
         file_mgr_data->newSize = True;
         AddFileIcons(file_mgr_rec, file_mgr_data, directory_set);
      }
    }

    /* expand the branch */
    UpdateBranchState(file_mgr_data, ip, BRANCH_EXPAND,
                    new_branch && file_mgr_data->busy_status == busy_readdir);
  }
  else
  {
    /* collaps the branch */
    UpdateBranchState(file_mgr_data, ip, BRANCH_COLLAPSE, False);
  }

  /* change tree button */
  if (file_mgr_data->busy_status != busy_readdir)
  {
     px = GetTreebtnPixmap(file_mgr_data, ip);
     XtSetArg(args[0], XmNlabelPixmap, px);
     XtSetValues (ip->treebtn, args, 1);
  }

  for (i = 0; i < file_mgr_data->directory_count; i++)
    if (strcmp(file_mgr_data->directory_set[i]->name, path) == 0)
      break;

  SelectVisible(file_mgr_data);
  UpdateBranchList(file_mgr_data, file_mgr_data->directory_set[i]);

  UnmanageFileIcons(file_mgr_rec, file_mgr_data, ip);
  EraseTreeLines(file_mgr_rec, file_mgr_data, ip);

  /* update the "Hidden" count */
  UpdateHeaders(file_mgr_rec, file_mgr_data, False);
}


/************************************************************************
 *
 *  FreeDirectorySet
 *      Free up the directory set data.
 *
 ************************************************************************/

static void
FreeDirectorySet(
        DirectorySet ** directory_set,
        int directory_count)
{
   int i, j;

   if (!directory_set)
     return;

   for (i = -1; i < directory_count; i++)
   {
      if (directory_set[i] == NULL)
        continue;

      XtFree ((char *) directory_set[i]->name);
      for (j = 0; j < directory_set[i]->file_count; j++)
      {
	 if( (char *) directory_set[i]->file_view_data[j]->label)
	 {
            XtFree ((char *) directory_set[i]->file_view_data[j]->label);
            directory_set[i]->file_view_data[j]->label = NULL;
	 }
         XtFree ((char *) directory_set[i]->file_view_data[j]);
         directory_set[i]->file_view_data[j] = NULL;
      }
      if (directory_set[i]->file_view_data != NULL)
         XtFree ((char *) directory_set[i]->file_view_data);
      XtFree ((char *) directory_set[i]->order_list);
      XtFree ((char *) directory_set[i]);
   }

   XtFree ((char *) (directory_set - 1));
}


/************************************************************************
 *
 *  FreeDirectoryData
 *      Free up the current directory and the directory set data.
 *
 ************************************************************************/

static void
FreeDirectoryData(
        FileMgrData *file_mgr_data )
{
   if(file_mgr_data->object_positions)
      FreePositionInfo(file_mgr_data);

   if (file_mgr_data->current_directory != NULL)
      XtFree ((char *) file_mgr_data->current_directory);
   file_mgr_data->current_directory = NULL;

   if (file_mgr_data->host != NULL)
      XtFree ((char *) file_mgr_data->host);
   file_mgr_data->host = NULL;

   FreeLayoutData(file_mgr_data->layout_data);
   file_mgr_data->layout_data = NULL;
   FreeDirectorySet(file_mgr_data->directory_set,
                    file_mgr_data->directory_count);
   file_mgr_data->directory_set = NULL;
}




/************************************************************************
 *
 *  Close
 *      Close (destroy) the file browser view.  This callback is issued
 *      from both the Close menu item and the Close system menu.
 *
 ************************************************************************/

void
Close(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
   FileMgrRec * file_mgr_rec;
   DialogData * dialog_data;
   Arg args[1];
   Widget mbar = XmGetPostedFromWidget(XtParent(w));


   XmUpdateDisplay (w);

   XtSetArg(args[0], XmNuserData, &file_mgr_rec);
   XtGetValues(mbar, args, 1);
   /* Ignore accelerator received after we're unposted */
   if ((dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec)) == NULL)
      return;

   CloseView (dialog_data);
}




/************************************************************************
 *
 *  SystemClose
 *      Function called from a close on the system menu.
 *
 ************************************************************************/

static void
SystemClose(
        Widget w,
        XtPointer data )
{
   FileMgrRec * file_mgr_rec;
   DialogData * dialog_data;


   file_mgr_rec = (FileMgrRec *) data;
   dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec);

   CloseView (dialog_data);
}



/************************************************************************
 *
 *  SetIconAttributes
 *      Set the icon name and icon to be used for a file manager view.
 *
 ************************************************************************/

static void
SetIconAttributes(
        FileMgrRec *file_mgr_rec,
        FileMgrData *file_mgr_data,
        char *directory_name )
{
   static Pixmap tool_icon = XmUNSPECIFIED_PIXMAP;
   static Pixmap dir_icon = XmUNSPECIFIED_PIXMAP;
   static Pixmap tool_mask = XmUNSPECIFIED_PIXMAP;
   static Pixmap dir_mask = XmUNSPECIFIED_PIXMAP;
   char * new_directory_name;
   Pixel background, foreground, top_shadow, bottom_shadow, select;
   Colormap colormap;
   unsigned int width;
   unsigned int height;
   Pixmap pixmap;
   Arg args[3];
   Boolean havePixmap = False;
   Boolean haveMask = False;
   Boolean root = False;
   char * tmpStr;
   char *ptr, *fileLabel, *fileName;

   if (tool_icon == XmUNSPECIFIED_PIXMAP)
   {
      XtSetArg (args[0], XmNbackground, &background);
      XtSetArg (args[1], XmNcolormap,  &colormap);
      XtGetValues (file_mgr_rec->main, args, 2);

      XmGetColors (XtScreen (file_mgr_rec->main), colormap, background,
                   &foreground, &top_shadow, &bottom_shadow, &select);

      /* first get the File Manager's Icon */
      pixmap = XmGetPixmap (XtScreen (file_mgr_rec->main), fileMgrIcon,
                                        foreground, background);
      if( pixmap != XmUNSPECIFIED_PIXMAP)
         dir_icon = pixmap;

      /* now let's get the mask for the File Manager */
      pixmap = _DtGetMask (XtScreen (file_mgr_rec->main), fileMgrIcon);
      if( pixmap != XmUNSPECIFIED_PIXMAP)
         dir_mask = pixmap;

      /* Let's get the Application Manager's Icon */
      pixmap = XmGetPixmap (XtScreen (file_mgr_rec->main), appMgrIcon,
                                        foreground, background);
      if( pixmap != XmUNSPECIFIED_PIXMAP)
         tool_icon = pixmap;

      /* now let's get the mask for the Application Manager */
      pixmap = _DtGetMask (XtScreen (file_mgr_rec->main), appMgrIcon);
      if( pixmap != XmUNSPECIFIED_PIXMAP)
         tool_mask = pixmap;
   }

   /* set icon name */

   if (fileLabel = DtDtsFileToAttributeValue(directory_name, DtDTS_DA_LABEL))
      ptr = fileLabel;
   else if (fileName = strrchr(directory_name, '/'))
      ptr = fileName + 1;
   else
      ptr = "";

   if(file_mgr_data->title)
   {
      if(file_mgr_data->toolbox)
      {
         if(strcmp(directory_name, file_mgr_data->restricted_directory) == 0)
         {
            new_directory_name =
              (char *)XtMalloc(strlen(file_mgr_data->title) + 1);
            strcpy( new_directory_name, file_mgr_data->title );
         }
         else
         {
            new_directory_name = (char *)XtMalloc( strlen(ptr) + 1 );
            sprintf(new_directory_name, "%s", ptr);
         }
      }
      else
      {
         new_directory_name = (char *)XtMalloc( strlen(ptr) + 1);
         sprintf(new_directory_name, "%s",  ptr);
      }
      root = True;
   }
   else
   {
     if (strcmp (directory_name, "/") == 0 && !fileLabel)
     {
       new_directory_name = (char *)XtMalloc(strlen(file_mgr_data->host) +
                                             strlen(root_title) + 3);
       sprintf( new_directory_name, "%s:%s", file_mgr_data->host, root_title );
       root = True;
     }
     else
       new_directory_name = ptr;
   }

   XtSetArg (args[0], XmNiconName, new_directory_name);

   if(file_mgr_data->toolbox && tool_icon != XmUNSPECIFIED_PIXMAP)
   {
      havePixmap = True;
      XtSetArg (args[1], XmNiconPixmap, tool_icon);
      if( tool_mask != XmUNSPECIFIED_PIXMAP)
      {
         haveMask = True;
         XtSetArg (args[2], XmNiconMask, tool_mask);
      }
   }
   else if (dir_icon != XmUNSPECIFIED_PIXMAP)
   {
      havePixmap = True;
      XtSetArg (args[1], XmNiconPixmap, dir_icon);
      if( dir_mask != XmUNSPECIFIED_PIXMAP)
      {
         haveMask = True;
         XtSetArg (args[2], XmNiconMask, dir_mask);
      }
   }

   if(havePixmap)
   {
      if(haveMask)
         XtSetValues (file_mgr_rec->shell, args, 3);
      else
         XtSetValues (file_mgr_rec->shell, args, 2);
   }
   else
      XtSetValues (file_mgr_rec->shell, args, 1);

   if (fileLabel)
      DtDtsFreeAttributeValue(fileLabel);
   if(root)
      XtFree(new_directory_name);
}



/*
 * Menu callback for the fast change directory toggle.  Toggles the text
 * field up and down.
 */
void
ShowChangeDirField (
   Widget  w,
   XtPointer client_data,
   XtPointer callback)
{
   FileMgrRec  * file_mgr_rec;
   DialogData  * dialog_data;
   FileMgrData * file_mgr_data;
   Arg args[1];
   Widget mbar;
   int begin_x;


   /*  Set the menu item to insensitive to prevent multiple  */
   /*  dialogs from being posted and get the area under the  */
   /*  menu pane redrawn.                                    */

   if (w)
   {
      if((intptr_t) client_data == FM_POPUP)
         mbar = XtParent(w);
      else
         mbar = XmGetPostedFromWidget(XtParent(w));
      XmUpdateDisplay (w);
      XtSetArg(args[0], XmNuserData, &file_mgr_rec);
      XtGetValues(mbar, args, 1);
   }
   else
   {
      /* Done only during a restore session */
      file_mgr_rec = (FileMgrRec *)client_data;
   }

   /* Got an accelerator after we were unposted */
   if ((dialog_data = _DtGetInstanceData ((XtPointer)file_mgr_rec)) == NULL)
      return;
   file_mgr_data = (FileMgrData *) dialog_data->data;

   /* Toggle the state of the text field */
   if (XtIsManaged(file_mgr_rec->current_directory_text))
   {
      XtSetArg (args[0], XmNallowShellResize, False);
      XtSetValues(file_mgr_rec->shell, args, 1);
      XtUnmanageChild(file_mgr_rec->current_directory_text);
      XtSetArg (args[0], XmNallowShellResize, True);
      XtSetValues(file_mgr_rec->shell, args, 1);
      file_mgr_data->fast_cd_enabled = False;
   }
   else
     ShowFastChangeDir(file_mgr_rec, file_mgr_data);
}



/*
 * Class function for forcing the focus to the text field, if visible, each
 * time the FileMgr dialog is posted.
 */

static void
SetFocus (
   FileMgrRec  * file_mgr_rec,
   FileMgrData * file_mgr_data)
{
   /* Force the focus to the text field */
   if (file_mgr_data->fast_cd_enabled)
   {
      XRaiseWindow(XtDisplay(file_mgr_rec->current_directory_text),
                   XtWindow(file_mgr_rec->current_directory_text));
      XmProcessTraversal(file_mgr_rec->current_directory_text,
                         XmTRAVERSE_CURRENT);
   }
}


/*
 * This function free up any position information associated with the
 * dtfile view, and nulls out the associated data fields.
 */

void
FreePositionInfo (
   FileMgrData * file_mgr_data)
{
   int i, j;
   FileViewData **file_view_data;

   /* free object positions */
   for (i = 0; i < file_mgr_data->num_objects; i++)
   {
      XtFree(file_mgr_data->object_positions[i]->name);
      XtFree((char *)file_mgr_data->object_positions[i]);
      file_mgr_data->object_positions[i] = NULL;
   }

   XtFree((char *)file_mgr_data->object_positions);

   file_mgr_data->object_positions = NULL;
   file_mgr_data->num_objects = 0;

   /* clear references to object positions in file_view_data */
   for (i = 0; i < file_mgr_data->directory_count; i++)
   {
      file_view_data = file_mgr_data->directory_set[i]->file_view_data;
      for (j = 0; j < file_mgr_data->directory_set[i]->file_count; j++)
         file_view_data[j]->position_info = NULL;
   }
}


/*
 * This function determines whether random positioning is currently enabled
 * for this view.  The decision is based upon they type of the view, and
 * the current preferences settings.
 */

Boolean
PositioningEnabledInView (
   FileMgrData * file_mgr_data)

{
   if ((file_mgr_data->show_type == SINGLE_DIRECTORY) &&
        (file_mgr_data->view != BY_ATTRIBUTES) &&
        (file_mgr_data->positionEnabled == RANDOM_ON))
   {
      return(True);
   }

   return(False);
}


/*
 * This function is similar to the above function, but is less restrictive;
 * this function does not require that the view currently have positioning
 * enabled; it simply returns whether the view has positioning info.
 */

Boolean
PositionFlagSet (
   FileMgrData * file_mgr_data)

{
   if ((file_mgr_data->object_positions) &&
        (file_mgr_data->positionEnabled == RANDOM_ON))
   {
      return(True);
   }

   return(False);
}


/*
 * When a new view of a directory is spun off of an existing view of that
 * directory, we want the new view to inherit the positioning information
 * associated with the original view.  This function takes care of that.
 */

void
InheritPositionInfo (
   FileMgrData * src_file_mgr_data,
   FileMgrData * dest_file_mgr_data)
{
   int i;
   ObjectPosition *ptr;
   ObjectPosition **temp_stack;

   if (!PositionFlagSet(src_file_mgr_data))
   {
      /* Nothing to inherit */
      dest_file_mgr_data->num_objects = 0;
      dest_file_mgr_data->object_positions = NULL;
      return;
   }

   dest_file_mgr_data->num_objects = src_file_mgr_data->num_objects;
   dest_file_mgr_data->object_positions = (ObjectPosition **) XtMalloc(
            sizeof(ObjectPosition *) * dest_file_mgr_data->num_objects);

   temp_stack = (ObjectPosition **) XtMalloc(
            sizeof(ObjectPosition *) * dest_file_mgr_data->num_objects);

   for (i = 0; i < dest_file_mgr_data->num_objects; i++)
   {
      ptr = dest_file_mgr_data->object_positions[i] = (ObjectPosition *)
           XtMalloc(sizeof(ObjectPosition));

      *ptr = *(src_file_mgr_data->object_positions[i]);
      ptr->name = XtNewString(ptr->name);
      ptr->file_view_data = NULL;
      ptr->next = NULL;
      ptr->prev = NULL;

      temp_stack[ptr->stacking_order - 1] = ptr;
   }

   for(i = 0; i < dest_file_mgr_data->num_objects; i++)
   {
      if(dest_file_mgr_data->object_positions[i]->stacking_order == 1)
      {
         dest_file_mgr_data->object_positions[i]->next =
            temp_stack[dest_file_mgr_data->object_positions[i]->stacking_order];

      }
      else if(dest_file_mgr_data->object_positions[i]->stacking_order ==
                                              dest_file_mgr_data->num_objects)
      {
         dest_file_mgr_data->object_positions[i]->prev =
            temp_stack[dest_file_mgr_data->object_positions[i]->
                                                            stacking_order - 2];
      }
      else
      {
         dest_file_mgr_data->object_positions[i]->prev =
            temp_stack[dest_file_mgr_data->object_positions[i]->
                                                            stacking_order - 2];
         dest_file_mgr_data->object_positions[i]->next =
            temp_stack[dest_file_mgr_data->object_positions[i]->stacking_order];
      }
   }

   XtFree((char *)temp_stack);
}


/*
 * This function saves the current position information (if any) in the
 * associated directory; if there is no positional data, then any old
 * position files in this directory are removed.  The entries are written
 * according to their position (left to right, top to bottom), not according
 * to their relative stacking order.
 */

void
SavePositionInfo (
   FileMgrData * file_mgr_data)

{
   PositionInfo *position_info;
   ObjectPosition * ptr;
   int i;

   /* Copy object positions into position info array */
   if (file_mgr_data->num_objects <= 0)
      position_info = NULL;
   else
   {
      position_info = (PositionInfo *)
                   XtMalloc(file_mgr_data->num_objects * sizeof(PositionInfo));
      for (i = 0; i < file_mgr_data->num_objects; i++)
      {
         ptr = file_mgr_data->object_positions[i];

         position_info[i].name = ptr->name;
         position_info[i].x = ptr->x;
         position_info[i].y = ptr->y;
         position_info[i].stacking_order = ptr->stacking_order;
      }
   }

   SetDirectoryPositionInfo(file_mgr_data->host,
                            file_mgr_data->current_directory,
                            file_mgr_data->num_objects, position_info);

   XtFree((char *)position_info);
}


/*
 * This function will attempt to load any positional data associated with
 * the directory to be viewed.  Within the positioning file, the entries
 * are order in left-to-right, top-to-bottom order, not according to the
 * stacking order.
 */

void
LoadPositionInfo (
   FileMgrData * file_mgr_data)
{
   PositionInfo *position_info;
   ObjectPosition * ptr;
   int numObjects;
   int i;

   /* don't do anything if we already have position information */
   if (file_mgr_data->object_positions != NULL)
      return;

   /* Load the number of entries */
   numObjects = GetDirectoryPositionInfo(file_mgr_data->host,
                             file_mgr_data->current_directory, &position_info);

   if (numObjects <= 0)
   {
      file_mgr_data->object_positions = NULL;
      file_mgr_data->num_objects = 0;
      return;
   }

   if (numObjects > 0)
   {
      /* Proceed with the loading */
      file_mgr_data->object_positions = (ObjectPosition **)XtMalloc(
              sizeof(ObjectPosition *) * numObjects);

      for (i = 0; i < numObjects; i++)
      {
         ptr = file_mgr_data->object_positions[i] = (ObjectPosition *)
                      XtMalloc(sizeof(ObjectPosition));
         ptr->name = XtNewString(position_info[i].name);
         ptr->x = position_info[i].x;
         ptr->y = position_info[i].y;
         ptr->in_use = False;
         ptr->late_bind = False;
         ptr->stacking_order = position_info[i].stacking_order;
         ptr->file_view_data = NULL;
         ptr->next = NULL;
         ptr->prev = NULL;
      }

      /* Repair all of the next and prev pointers */
      file_mgr_data->num_objects = i;
      RepairStackingPointers(file_mgr_data);
      /* OrderChildrenList(file_mgr_data); */
   }

   return;
}

/************************************************************************
 *
 *  MoveOkCB - the user wishes to actually do the move even though its
 *             not the desktop object that is actually getting moved.
 *             This function calls the routines which do the moves.  It
 *             depends on the view type to determine how it does it.
 *              view types are:
 *                     DESKTOP - the drop happened on a Desktop object
 *                              and the object was a directory
 *                     NOT_DESKTOP_DIR - the drop happened on a directory
 *                              but it wasn't a directory on the desktop.
 *                     NOT_DESKTOP - drop happened in a FileManager view
 *                              and not on a directory.
 *
 *
 ************************************************************************/
static void
MoveOkCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
   XtUnmanageChild((Widget)client_data);
   XmUpdateDisplay((Widget)client_data);
   XtDestroyWidget((Widget)client_data);

   DoTheMove(DESKTOP);
}

/************************************************************************
 *
 *  MoveCancelCB - function called when the user cancels out of the
 *                 Move file question dialog.  Just deleted the dialog.
 *
 ************************************************************************/
static void
MoveCancelCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
   FileMgrRec *file_mgr_rec;

   XtUnmanageChild((Widget)client_data);
   XmUpdateDisplay((Widget)client_data);
   XtDestroyWidget((Widget)client_data);

   if(view_type == NOT_DESKTOP)
   {
      file_mgr_rec = (FileMgrRec *)fm->file_mgr_rec;
      LayoutFileIcons(file_mgr_rec, fm, False, True);
   }
}

static void
RemoveIconInWorkspace(
         char * fileName,
         char * workspaceName )
{
  DesktopRec *desktopWin;
  int i, j;
  char iconName[MAX_PATH];


  for(i = 0; i < desktop_data->numIconsUsed; i++)
  {
    desktopWin = desktop_data->desktopWindows[i];

    if( strcmp( desktopWin->dir_linked_to, "/" ) == 0 )
      sprintf( iconName, "/%s", desktopWin->file_name );
    else
      sprintf( iconName, "%s/%s", desktopWin->dir_linked_to, desktopWin->file_name );

    DtEliminateDots( iconName );

    if( strcmp( fileName, iconName ) == 0
        && strcmp( workspaceName, desktopWin->workspace_name ) == 0 )
    {
      RemoveDT( desktopWin->shell, (XtPointer) desktopWin,
                (XtPointer)NULL );

    }
  }
}

/*************************************************************************
 *
 *  CheckMoveType - function used to determine:
 *                    1) is the file being moved have a reference to a
 *                       Desktop Object?
 *                    2) if yes: is the file being moved dragged from
 *                               the Desktop (check widget_dragged)?
 *                      2a) if yes: set up global varibles used by message
 *                                  dialog callback
 *                      2b) if no: execute the move, then if one of the objects
 *                                 is on the Desktop, move the link to the
 *                                 new directory.
 *                    3) question 1 answer is no:  just exectute the move
 *                                                 command.
 *                 Other information:
 *                         view types are:
 *                           DESKTOP - the drop happened on a Desktop object
 *                                    and the object was a directory
 *                           NOT_DESKTOP_DIR - the drop happened on a directory
 *                                    but it wasn't a directory on the desktop.
 *                           NOT_DESKTOP - drop happened in a FileManager view
 *                                    and not on a directory.
 *
 *
 **************************************************************************/
void
CheckMoveType(
        FileMgrData *file_mgr_data,
        FileViewData *file_view_data,
        DirectorySet * directory_data,
        DesktopRec * desktopWindow,
        char **file_set,
        char **host_set,
        unsigned int modifiers,
        int file_count,
        Position drop_x,
        Position drop_y,
        int view )
{
   char * tmpStr;
   char *Str;
   int number;
   int i, j;
   char *target_host;
   char directory[MAX_PATH];
   Window   rootWindow;
   Atom     pCurrent;
   Screen   *currentScreen;
   int      screen;
   char * workspace_name = NULL;
   Display  *display;
   Boolean value;

#ifdef _CHECK_FOR_SPACES
   if (_DtSpacesInFileNames(file_set, file_count))
   {
     char * tmpStr = (GETMESSAGE(27,94, "The File Manager does not support objects\nwith spaces in their names, so we cannot delete them.\nTo delete these objects:\n  - select 'Open Terminal' from the File Manager menu\n  - then issue the remove command to delete them."));
     char * msg = XtNewString(tmpStr);
     FileOperationError (toplevel, msg, NULL);
     XtFree(msg);
     return;
   }
#endif

   display = XtDisplay(toplevel);
   screen = XDefaultScreen(display);
   currentScreen = XScreenOfDisplay(display, screen);
   rootWindow = RootWindowOfScreen(currentScreen);

   if( DtWsmGetCurrentWorkspace(display,rootWindow,&pCurrent) == Success )
   {
     workspace_name = XGetAtomName (display, pCurrent);
     CleanUpWSName(workspace_name);
   }

   /* Fix for the case, when you drop an object from App. Manager on to desktop
      and try to move it to File Manager.  It must copied for this operation
      (modifers == ControlMask). */
   {
     Boolean IsToolBox;

     Str = (char *)IsAFileOnDesktop2(file_set, file_count, &number,&IsToolBox);
     if(IsToolBox)
       modifiers = ControlMask;
   }

   /*
    * If number is comes back greater than 0 then at least one of the files
    * dropped has a reference to a Desktop object
    *
    * If widget_dragged is != NULL then the files dragged are from the
    * desktop; in this case set up globals, and post a confirmation dialog.
    */
   modifiers &= ~Button2Mask;
   modifiers &= ~Button1Mask;
   if (number > 0 &&
       modifiers != ControlMask  && modifiers != ShiftMask &&
       widget_dragged != NULL)
   {
      char *message = NULL;
      char *title;
      char *from, *to, *filename;

      global_file_count = file_count;
      _DtCopyDroppedFileInfo(file_count, file_set, host_set,
                             &global_file_set, &global_host_set);
      G_dropx = drop_x;
      G_dropy = drop_y;
      fv = file_view_data;
      view_type = view;
      mod = modifiers;
      switch( view )
      {
          case DESKTOP:
              /* desktop object dropped on a desktop directory */
              dtWindow = desktopWindow;
              fm = (FileMgrData *)NULL;
              dd = (DirectorySet *)NULL;
              to = (char *)XtMalloc(strlen(directory_data->name) +
                                         strlen(fv->file_data->file_name)+ 2);
              sprintf(to, "%s/%s", directory_data->name,
                                                   fv->file_data->file_name );
              DtEliminateDots(to);
              break;
          case NOT_DESKTOP_DIR:
              /* desktop object dropped on a file view directory icon */
              dtWindow = (DesktopRec *)NULL;
              fm = file_mgr_data;
              dd = directory_data;
              to = (char *)XtMalloc(strlen(dd->name) +
                                    strlen(fv->file_data->file_name) + 2);
              sprintf( to, "%s/%s",dd->name , fv->file_data->file_name );
              (void) DtEliminateDots(to);
              break;
          default:/* view == NOT_DESKTOP */
              /* desktop object dropped in a file manager view */
              dtWindow = (DesktopRec *)NULL;
              fm = file_mgr_data;
              dd = (DirectorySet *)NULL;
              to = (char *)XtMalloc(strlen(file_mgr_data->current_directory)
                                    + 1);
              strcpy(to, file_mgr_data->current_directory);
              break;
      }

      for(i=0; i<file_count; i++)
      {
         if(strcmp(file_set[i], to) == 0)
         {
            char *msg;
            FileMgrRec *file_mgr_rec;
            tmpStr =
                  GETMESSAGE(11,16,"A folder cannot be moved into itself.\n%s");
            msg = XtNewString(tmpStr);
            if(file_view_data == NULL)
            {
               file_mgr_rec = (FileMgrRec *)file_mgr_data->file_mgr_rec;
               FileOperationError(file_mgr_rec->shell, msg, file_set[i]);
            }
            else
            {
               FileOperationError(file_view_data->widget, msg, file_set[i]);
            }
            XtFree(msg);
            XtFree(Str);
            XtFree(to);
            XtFree(workspace_name);
            return;
         }
      }

      tmpStr = (GETMESSAGE(12,3, "Move File(s)"));
      title = XtNewString(tmpStr);
      if(number == 1)
      {
         int len = strlen(to);
         char *ptr, *ptr1;

         if( file_mgr_data
             && file_mgr_data->toolbox )
         {
           ptr = strrchr(file_set[0], '/');
           ptr1 = (char *)XtMalloc(strlen(to) + strlen(ptr) + 1);
           sprintf(ptr1, "%s%s", to, ptr);
           ptr1 = _DtResolveAppManPath( ptr1,
                                  file_mgr_data->restricted_directory);

           if (strncmp(ptr1, file_set[0], len) == 0)
           {
             DoTheMove(DESKTOP);
             XtFree(ptr1);
             XtFree(title);
             XtFree(to);
             XtFree(Str);
             return;
           }
           from = XtNewString( file_set[0] );
           ptr = strrchr( from, '/' );
           *ptr = 0x0;
           filename = XtNewString("");
         }
         else
         {
           ptr = strrchr(file_set[0], '/');
           if (ptr)
           {
             *ptr = '\0';
             from = (char *)XtMalloc(strlen(file_set[0]) + 1);
             strcpy(from, file_set[0]);
             *ptr = '/';
             ptr++;
             filename = (char *)XtMalloc(strlen(ptr) + 1);
             strcpy(filename, ptr);
           }
           else
           {
             from = (char *)XtMalloc(strlen(ptr) + 1);
             strcpy(from, ptr);
             filename = XtNewString("");
           }
         }

         if( strcmp( from, to ) == 0 )
         {
           RemoveIconInWorkspace( file_set[0], workspace_name );
           XtFree(title);
           XtFree(to);
           XtFree(Str);
           XtFree(workspace_name);
           return;
         }

#ifdef sun /* This format statement core dumps on SunOS 4.0.3 and 4.1 */
         tmpStr = (GETMESSAGE(12,4, "The object %s is currently in folder %s.\nYou are moving the object to folder %s.\nIs this what you want to do?"));
#else
         tmpStr = (GETMESSAGE(12,5, "The object %1$s is currently in folder %2$s.\nYou are moving the object to folder %3$s.\nIs this what you want to do?"));
#endif
         message = (char *)XtMalloc(strlen(tmpStr) + strlen(filename) +
                                            strlen(to) + strlen(from) + 1);

         sprintf(message, tmpStr, filename, from, to);
         XtFree(filename);
         XtFree(to);
         XtFree(from);
      }
      else
      {
        int len = strlen(to);
        char notHere = 0x0;
        int workspace_num = 1;

        for( i = 0; i < desktop_data->numWorkspaces; ++i )
        {
          if( strcmp( workspace_name, desktop_data->workspaceData[i]->name) == 0 )
          {
            workspace_num = desktop_data->workspaceData[i]->number;
            break;
          }
        }

        DeselectAllDTFiles( desktop_data->workspaceData[workspace_num -1] );

        {
           char *ptr;

           if( file_mgr_data
               && file_mgr_data->toolbox)
           {
             to = _DtResolveAppManPath( to,
                          file_mgr_data->restricted_directory );
             for( i = 0; i < number; ++i )
             {
               char *ptr, *ptr1;

               from = (char *)XtNewString( file_set[i] );
               ptr = strrchr( from, '/' );
               *ptr = 0x0;
               from = _DtResolveAppManPath( from,
                              file_mgr_data->restricted_directory );


               if( strcmp( from, to ) == 0 )
                 RemoveIconInWorkspace( file_set[i], workspace_name );
               else
                 notHere = 0x1;
               XtFree( from );
             }
           }
           else
           {
              for( i = 0; i < number; ++i )
              {
                ptr = strrchr(file_set[i], '/');
                if (ptr)
                {
                  *ptr = '\0';
                  from = (char *)XtMalloc(strlen(file_set[i]) + 1);
                  strcpy(from, file_set[i]);
                  *ptr = '/';
                }
                else
                {
                  from = (char *)XtMalloc(strlen(ptr) + 1);
                  strcpy(from, ptr);
                }

                if( strcmp( to, from ) == 0 )
                {
                  RemoveIconInWorkspace( file_set[i], workspace_name );
                }
                else
                {
                  notHere = 0x1;
                }
                XtFree(from);
              }
           }
        }

        if( notHere )
        {
          tmpStr = (GETMESSAGE(12,6, "At least one of the files you dropped is from a different\nfolder.  You are moving all these files to %s.\nIs this what you want to do?"));
          message = (char *)XtMalloc(strlen(tmpStr) + strlen(to) + 1);

          sprintf(message, tmpStr, to);
          XtFree(to);
        }
        else
        {
        }
      }

      if( message )
      {
        _DtMessageDialog(toplevel, title, message, NULL, TRUE,
                         MoveCancelCB, MoveOkCB, NULL, HelpRequestCB, False,
                         QUESTION_DIALOG);

        XtFree(message);
      }
      XtFree(title);
      XtFree(Str);
      XtFree(workspace_name);
      return;
   }

    XtFree(Str);  /* No longer used, so free it up */
    XtFree(workspace_name);
   /*
    * Files are not dragged from the desktop.
    *
    * Just execute the operation.  The target directory dependings on
    * the view type (i.e. DESKTOP, NOT_DESKTOP, or NOT_DESKTOP_DIR).
    */
   switch( view )
   {
       case DESKTOP:
           target_host = desktopWindow->host;
           sprintf( directory, "%s/%s", directory_data->name,
                    file_view_data->file_data->file_name );
           DtEliminateDots(directory);
           value = FileMoveCopyDesktop (file_view_data, directory,
                                        host_set, file_set, file_count,
                                        modifiers, desktopWindow,
                                        NULL, NULL);
           break;

       case NOT_DESKTOP_DIR:
           target_host = file_mgr_data->host;
           sprintf( directory, "%s/%s", directory_data->name,
                    file_view_data->file_data->file_name );
           DtEliminateDots(directory);
           value = FileMoveCopy (file_mgr_data,
                                 NULL, directory, target_host,
                                 host_set, file_set, file_count,
                                 modifiers, NULL, NULL);
           break;

       default:/* view == NOT_DESKTOP */
           target_host = file_mgr_data->host;
           strcpy(directory, file_mgr_data->current_directory);
           G_dropx = drop_x;
           G_dropy = drop_y;
/*
           RepositionIcons(file_mgr_data, file_set, file_count, drop_x,
                           drop_y, True);
*/
           value = FileMoveCopy(file_mgr_data,
                                NULL, directory, target_host,
                                host_set, file_set, file_count,
                                modifiers, NULL, NULL);
           break;
   }


#ifdef FOO
   /*
    * If this was a move and any of the files is referenced by a
    * Desktop object we need to change the reference to the new
    * location that the file moved to.
    *
    * @@@ This should really be done in the pipe callback!
    */
   if (number > 0 &&
       value &&
       modifiers != ControlMask  && modifiers != ShiftMask)
   {
      Tt_status tt_status;

      for(j = 0; j < file_count; j++)
      {
         char *fileName;
         DesktopRec *desktopWin;

         /*
          * loop through each desktop object to see if the file
          * being operated on has a reference to a desktop object.
          * If it does change the reference in the desktop object
          * structure.
          * NOTE:  if we find a desktop object the file references,
          *        after we've changed the DT object we can't break
          *        because there might be more than one DT object which
          *        references it.
          */
         for(i = 0; i < desktop_data->numIconsUsed; i++)
         {
            desktopWin = desktop_data->desktopWindows[i];
            fileName = ResolveLocalPathName( desktopWin->host, desktopWin->dir_linked_to,
                                             desktopWin->file_name, home_host_name, &tt_status);
            if( TT_OK == tt_status && strcmp(fileName, file_set[j]) == 0 )
            {
              /*
               * if fileName == file_set[j] then the file is on the
               * Desktop so change the dir_linked to reference.  What the
               * new reference is depends on which view type is being
               * exectuted.
               */
              XtFree(desktopWin->host);
              desktopWin->host = XtNewString(target_host);

              XtFree(desktopWin->dir_linked_to);
              desktopWin->dir_linked_to = XtNewString(directory);

              XtFree(desktopWin->restricted_directory);
              if(file_mgr_data->restricted_directory != NULL)
                desktopWin->restricted_directory =
                  XtNewString(file_mgr_data->restricted_directory);
              else
                desktopWin->restricted_directory = NULL;

              XtFree(desktopWin->title);
              if(file_mgr_data->title != NULL)
                desktopWin->title = XtNewString(file_mgr_data->title);
              else
                desktopWin->title = NULL;

              XtFree(desktopWin->helpVol);
              if(file_mgr_data->helpVol != NULL)
                desktopWin->helpVol = XtNewString(file_mgr_data->helpVol);
              else
                desktopWin->helpVol = NULL;

              desktopWin->toolbox = file_mgr_data->toolbox;
              XtFree(fileName);
            }
         } /* for ( i = 0 ...) */
      }  /* for (j = 0 ...) */

      /* since at least one of the file has been moved lets update
         the !dtdesktop file */
      SaveDesktopInfo(NORMAL_RESTORE);
   }
#endif
}


/*
 * This function returns the current working directory settings, used when
 * invoking an action.  The caller passes in the current directory for the
 * command, and we will simply copy it and return new pointers, unless the
 * path specifies a directory which is part of the tool/apps area; in this
 * case, the cwd is set to the user's home directory.
 *
 * The application must free up these two return pointers.
 */

void
SetPWD(
        char *viewHost,
        char *viewDir,
        char **pwdHostRet,
        char **pwdDirRet,
        char *type )

{
   if ((strcmp(home_host_name, viewHost) == 0) && type != NULL)
   {
      char *ptr;

      /* Force app dir and any of its subdirs to use $HOME */
      if(strcmp(users_home_dir, "/") != 0)
      {
         ptr = strrchr(users_home_dir, '/');
         *ptr = '\0';
         *pwdDirRet = XtNewString(users_home_dir);
         *ptr = '/';
      }
      else
         *pwdDirRet = XtNewString(users_home_dir);
      *pwdHostRet = XtNewString(home_host_name);
   }
   else
   {
      /* Simply use the passed in cwd */
      *pwdHostRet = XtNewString(viewHost);
      *pwdDirRet = XtNewString(viewDir);
   }
}

static void
CreateFmPopup (Widget w)
{
   Widget popupBtns[FM_POPUP_CHILDREN];
   XmString label_string;
   char * mnemonic;
   Arg args[2];
   int n;
   int i = 0;


   /* Create file manager popup menu */
   n = 0;
   XtSetArg(args[n],XmNmenuAccelerator,"   "); n++;
   XtSetArg(args[n],XmNwhichButton, bMenuButton); n++;
   fileMgrPopup.menu = XmCreatePopupMenu(w, "FMPopup", args, n);
   /* Track popdown to release grabs */
   XtAddCallback(XtParent(fileMgrPopup.menu), XmNpopdownCallback,
                 FilePopupPopdownCB, NULL);
   XtAddCallback(fileMgrPopup.menu, XmNhelpCallback,
                       (XtCallbackProc)HelpRequestCB,
                       HELP_POPUP_MENU_STR);


   /* Create file manager title */
   fileMgrPopup.title = popupBtns[i] =
     (Widget)XmCreateLabelGadget(fileMgrPopup.menu, "fmtitle", NULL, 0);
   i++;


   /* Create title separators */
   popupBtns[i++] = XmCreateSeparatorGadget(fileMgrPopup.menu, "sep2",
                                                                     NULL, 0);


   /* Create 'Properties' option - white space popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 150, "Change Permissions...")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 17, "P"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.wsPopup[BTN_PROPERTIES] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "permissions", args, 2);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                  (XtCallbackProc)HelpRequestCB,
                                  HELP_POPUP_MENU_STR);

   XmStringFree (label_string);


   /* Create 'Find' option -- white space popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 18, "Find...")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 19, "F"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.wsPopup[BTN_FIND] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "find", args, 2);
   XtAddCallback (popupBtns[i], XmNactivateCallback,
                                ShowFindDialog, (XtPointer) FM_POPUP);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                  (XtCallbackProc)HelpRequestCB,
                                  HELP_FILE_MENU_STR);

   XmStringFree (label_string);


   /* Create 'Clean Up' option -- white space popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 99, "Clean Up")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 100, "C"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.wsPopup[BTN_CLEANUP] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "cleanUp", args, 2);
   XtAddCallback (popupBtns[i], XmNactivateCallback,
                                CleanUp, (XtPointer) FM_POPUP);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                  (XtCallbackProc)HelpRequestCB,
                                  HELP_CLEAN_UP_COMMAND_STR);

   XmStringFree (label_string);


   /* Create 'Select All' option -- white space popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 48, "Select All")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 51, "S"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.wsPopup[BTN_SELECTALL] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "selectAll", args, 2);
   XtAddCallback (popupBtns[i], XmNactivateCallback,
                                SelectAll, (XtPointer) FM_POPUP);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                  (XtCallbackProc)HelpRequestCB,
                                  HELP_SELECT_ALL_COMMAND_STR);

   XmStringFree (label_string);


   /* Create 'Unselect All' option -- white space popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 52, "Deselect All")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 55, "D"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.wsPopup[BTN_UNSELECTALL] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "deselectAll", args, 2);
   XtAddCallback (popupBtns[i], XmNactivateCallback,
                                UnselectAll, (XtPointer) FM_POPUP);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                  (XtCallbackProc)HelpRequestCB,
                                  HELP_UNSELECT_ALL_COMMAND_STR);

   XmStringFree (label_string);


   /* Create 'Show Hidden Files' option -- white space popup */
   label_string = XmStringCreateLocalized((GETMESSAGE(20, 156, "Show Hidden Objects")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 102, "H"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.wsPopup[BTN_SHOWHIDDEN] = popupBtns[i] =
          XmCreateToggleButtonGadget (fileMgrPopup.menu, "showHiddenObjects", args, 2);
   XtAddCallback (popupBtns[i], XmNvalueChangedCallback,
                                ShowHiddenFiles, (XtPointer) FM_POPUP);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                  (XtCallbackProc)HelpRequestCB,
                                  HELP_SHOW_HIDDEN_COMMAND_STR);

   XmStringFree (label_string);


   /* Manage white space popup buttons -- since XmNadjustMargin defaults */
   /* to true, we manage these buttons now so that they will be aligned  */
   /* according to the 'ShowHiddenFiles' toggle button                   */
   XtManageChildren(fileMgrPopup.wsPopup, WS_BTNS);


   /* Adjust XmNadjustMargin such that the rest of the popup buttons will */
   /* NOT be forced to align with the 'ShowHiddenFiles' toggle button     */
   XtSetArg(args[0], XmNadjustMargin, False);
   XtSetValues(fileMgrPopup.menu, args, 1);


   /* Create 'Properties' option - object popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 150, "Change Permissions...")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 17, "P"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.objPopup[BTN_PROPERTIES] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "permissions", args, 2);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                  (XtCallbackProc)HelpRequestCB,
                                  HELP_POPUP_MENU_STR);

   XmStringFree (label_string);


   /* Create 'Storage Size' option -- object popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(35, 1, "Storage Size...")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(35, 2, "S"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.objPopup[BTN_STORAGE_SIZE] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "storageSize", args, 2);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                  (XtCallbackProc)HelpRequestCB,
                                  HELP_POPUP_MENU_STR);

   XmStringFree (label_string);


   /* Create 'Put In Workspace' option -- object popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 84, "Put in Workspace")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 85, "W"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.objPopup[BTN_PUTON] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "putInWorkspace", args, 2);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                  (XtCallbackProc)HelpRequestCB,
                                  HELP_POPUP_MENU_STR);

   XmStringFree (label_string);


   /* Create 'Delete To Trash' option -- object popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 151, "Put in Trash")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 91, "T"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.objPopup[BTN_TRASH] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "trash", args, 2);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                 (XtCallbackProc)HelpRequestCB,
                                 HELP_POPUP_MENU_STR);

   XmStringFree (label_string);

   /* Create 'Add to Shelf' option -- object popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 200, "Add to Shelf")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 201, "A"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.objPopup[BTN_ADD_TO_SHELF] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "addToShelf", args, 2);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                 (XtCallbackProc)HelpRequestCB,
                                 HELP_POPUP_MENU_STR);

   XmStringFree (label_string);

   /* Create 'Help' option -- object popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(20, 123, "Help")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(20, 102, "H"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);

   fileMgrPopup.objPopup[BTN_HELP] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "popupHelp", args, 2);
   XtAddCallback (popupBtns[i++], XmNhelpCallback,
                                 (XtCallbackProc)HelpRequestCB,
                                 HELP_HELP_MENU_STR);

   XmStringFree (label_string);

   /* Create 'Restore' option -- trash popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(27, 24, "Put back")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(27, 26, "P"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);
   fileMgrPopup.trash_objPopup[BTN_RESTORE] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "putBack", args, 2);
   XtAddCallback(popupBtns[i++], XmNhelpCallback,
                                 (XtCallbackProc)HelpRequestCB,
                                 HELP_TRASH_DIALOG_STR);
   XmStringFree (label_string);


   /* Create 'Remove' option -- trash popup */
   label_string = XmStringCreateLocalized ((GETMESSAGE(27, 28, "Shred")));
   XtSetArg (args[0], XmNlabelString, label_string);
   mnemonic = ((char *)GETMESSAGE(27, 30, "h"));
   XtSetArg (args[1], XmNmnemonic, mnemonic[0]);
   fileMgrPopup.trash_objPopup[BTN_REMOVE] = popupBtns[i] =
          XmCreatePushButtonGadget (fileMgrPopup.menu, "shred", args, 2);
   XtAddCallback(popupBtns[i++], XmNhelpCallback,
                                 (XtCallbackProc)HelpRequestCB,
                                 HELP_TRASH_DIALOG_STR);
   XmStringFree (label_string);


   /* Create separator -- white space popup and object popup */
   fileMgrPopup.action_separator = popupBtns[i++] =
       XmCreateSeparatorGadget(fileMgrPopup.menu, "sep2", NULL, 0);


   /* Manage popup buttons */
   XtManageChildren(popupBtns, i);

}


/************************************************************************
 *
 *  DoTheMove - the user wishes to actually do the move even though its
 *             not the desktop object that is actually getting moved.
 *             This function calls the routines which do the moves.  It
 *             depends on the view type to determine how it does it.
 *              view types are:
 *                     DESKTOP - the drop happened on a Desktop object
 *                              and the object was a directory
 *                     NOT_DESKTOP_DIR - the drop happened on a directory
 *                              but it wasn't a directory on the desktop.
 *                     NOT_DESKTOP - drop happened in a FileManager view
 *                              and not on a directory.
 *
 *             if the type passed in is DESKTOP, then remove the corresponding
 *             desktop object.
 *
 ************************************************************************/
static void
DoTheMove(
   int type)
{
   int file_count;
   char **file_set;
   char **host_set;
   int i, j;
   char *file, *next, *end;
   char directory[MAX_PATH];
   Boolean result = True;

   file_count = global_file_count;
   file_set = global_file_set;
   host_set = global_host_set;

   switch( view_type )
   {
       case NOT_DESKTOP:
/*
           RepositionIcons(fm, file_set, file_count, dx, dy, True);
*/
           result = FileMoveCopy(fm,
                                 NULL, fm->current_directory, fm->host,
                                 host_set, file_set, file_count,
                                 (unsigned int)0, NULL, NULL);
#ifdef FOO
           CheckDesktop();
           if(type == DESKTOP)
           {
             for(j = 0; j < file_count; j++)
             {
               /* remove the Desktop object which corresponds to this move */
               for(i = 0; i < desktop_data->numIconsUsed; i++)
               {
                 char *fileName;
                 DesktopRec *desktopWin;

                 desktopWin = desktop_data->desktopWindows[i];
                 if (strcmp(desktopWin->host, host_set[j]) != 0)
                   continue;

                 fileName = (char *)XtMalloc(strlen(desktopWin->dir_linked_to)+
                                             strlen(desktopWin->file_name) + 3);
                 sprintf( fileName, "%s/%s", desktopWin->dir_linked_to,
                          desktopWin->file_name );
                 /*
                  * if fileName == file_set[j] then the file is on the
                  * Desktop so change the dir_linked to reference.  What the
                  * new reference is depends on which view type is being
                  * exectuted.
                  */
                 if(strcmp(fileName, file_set[j]) == 0)
                 {
                   Window   rootWindow;
                   Atom     pCurrent;
                   Screen   *currentScreen;
                   int      screen;
                   char     *workspace_name;

                   screen = XDefaultScreen(XtDisplay(desktopWin->shell));
                   currentScreen =
                     XScreenOfDisplay(XtDisplay(desktopWin->shell), screen);
                   rootWindow = RootWindowOfScreen(currentScreen);

                   if(DtWsmGetCurrentWorkspace(XtDisplay(desktopWin->shell),
                                             rootWindow, &pCurrent) == Success)
                     workspace_name =
                       XGetAtomName (XtDisplay(desktopWin->shell), pCurrent);
                   else
                     workspace_name = XtNewString("One");
                   if (strcmp(workspace_name, desktopWin->workspace_name) == 0)
                   {
                     RemoveDT (desktopWin->shell, (XtPointer) desktopWin,
                               (XtPointer)NULL);
                     XtFree(workspace_name);
                     workspace_name = NULL;
                     break;
                   }
                   XtFree(workspace_name);
                   workspace_name = NULL;
                 }
               }
             }
           }
#endif
           break;

       case NOT_DESKTOP_DIR:
           sprintf( directory, "%s/%s", dd->name, fv->file_data->file_name );

           result = FileMoveCopy (fm,
                                  NULL, directory, fm->host,
                                  host_set, file_set, file_count,
                                  (unsigned int) 0, NULL, NULL);

           CheckDesktop();
           break;

       default:
           {
             DirectorySet * directory_data = (DirectorySet *)fv->directory_set;

             sprintf( directory, "%s/%s", directory_data->name,
                      fv->file_data->file_name );

             result = FileMoveCopyDesktop (fv, directory, host_set, file_set,
                                           file_count, mod, dtWindow,
                                           NULL, NULL);
           }
           break;
   }

   _DtFreeDroppedFileInfo(file_count, file_set, host_set);
}

static void
FMInput(
        Widget wid,
        XEvent *event,
        String *params,
        Cardinal *num_params )
{
  XmDrawingAreaCallbackStruct cb ;
  FileMgrRec * fileMgrRec;
  int x, y ;
  Widget input_on_gadget ;

    if ((event->type == KeyPress)
        ||(event->type == KeyRelease))
    {
        x = event->xkey.x ;
        y = event->xkey.y ;
    }
    else
      return ;

    cb.reason = XmCR_INPUT ;
    cb.event = event ;

    if( *(params[0]) != '@' )
    {
        sscanf( params[0], "%p", (void **) &fileMgrRec );
      FileWindowInputCallback( wid, (XtPointer)fileMgrRec, (XtPointer)&cb );
    }

    return ;
}
