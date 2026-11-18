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
/* $XConsortium: ChangeDir.h /main/3 1995/11/02 14:34:52 rswiston $ */
/************************************<+>*************************************
 ****************************************************************************
 *
 *   FILE:           ChangeDir.h
 *
 *   COMPONENT_NAME: Desktop File Manager
 *
 *   DESCRIPTION:    Public include file for the change directory dialog.
 *
 *   (c) Copyright 1993, 1994, 1995 Hewlett-Packard Company
 *   (c) Copyright 1993, 1994, 1995 International Business Machines Corp.
 *   (c) Copyright 1993, 1994, 1995 Sun Microsystems, Inc.
 *   (c) Copyright 1993, 1994, 1995 Novell, Inc.
 *
 ****************************************************************************
 ************************************<+>*************************************/

#ifndef _ChangeDir_h
#define _ChangeDir_h

#include "Mounts.h"

extern DialogClass * changeDirClass;


typedef struct
{
   Boolean       displayed;
   Position      x;
   Position      y;
   Dimension     width;
   Dimension     height;
   char        * host_name;
   XmString      string_path;
   XmStringTable history_list;
   int           visible_count;
   int           list_count;
   XtPointer     file_mgr_rec;
} ChangeDirData, * ChangeDirDataPtr;


typedef struct
{
   XtCallbackProc  callback;
   XtPointer       change_dir_rec;
   XtPointer       client_data;
} ChangeDirApply;


typedef struct
{
   Widget shell;
   Widget change_dir;
   ChangeDirApply * apply_data;
   Widget tab_stack;
   Widget history_page;
   Widget history_host_label;
   Widget history_tab_list;
   Widget mounts_page;
   Widget mounts_tab_list;
   Widget mounts_info_label;
   Widget mounts_connect_btn;
   Widget mounts_unmount_btn;
   Widget mounts_refresh_btn;
   DtfileMountNavEntry *mount_entries;
   int mount_entry_count;
   XtPointer active_file_mgr_rec;
} ChangeDirRec;

extern void DtfileOpenGoToDialogTab(FileMgrRec *file_mgr_rec, int tab_index);


#endif /* _ChangeDir_h */
/* DON'T ADD ANYTHING AFTER THIS #endif */
