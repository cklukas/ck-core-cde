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
/* $TOG: StorageSize.c /main/1 2024/01/01 00:00:00 user $ */
/*****************************************************************************
 *
 *   FILE:           StorageSize.c
 *
 *   COMPONENT_NAME: Desktop File Manager
 *
 *   DESCRIPTION:    Provides the Storage Size dialog and background
 *                   traversal that computes recursive folder storage usage.
 *
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/LabelG.h>
#include <Xm/PushBG.h>

#include "FileMgr.h"
#include "Main.h"
#include "SharedProcs.h"
#include "StorageSize.h"

typedef struct DirStackEntry {
    DIR *dir;
    char *path;
    struct DirStackEntry *next;
} DirStackEntry;

typedef struct {
    Widget shell;
    Widget status_label;
    Widget size_label;
    Widget storage_label;
    Widget files_label;
    Widget dirs_label;
    Widget action_button;
    XtAppContext app;
    XtIntervalId timer_id;
    XtWorkProcId work_proc_id;
    DirStackEntry *stack;
    Boolean scanning;
    Boolean done;
    Boolean closing;
    Boolean cancel_request;
    Boolean closed;
    char *path;
    char *title;
    char *error_message;
    unsigned long long total_size;
    unsigned long long total_blocks;
    unsigned long file_count;
    unsigned long dir_count;
} StorageSizeData;

static void StorageSizeTimer(XtPointer client_data, XtIntervalId *);
static Boolean StorageSizeWorkProc(XtPointer client_data);
static void StorageSizeRequestClose(StorageSizeData *data);
static void StorageSizeClose(StorageSizeData *data);
static void StorageSizeCancelCB(Widget w, XtPointer client_data, XtPointer call_data);
static void StorageSizeDestroyCB(Widget w, XtPointer client_data, XtPointer call_data);
static void SetLabelText(Widget widget, const char *text);
static void StorageSizeUpdateButton(StorageSizeData *data);
static void StorageSizeFreeStack(StorageSizeData *data);
static Boolean StorageSizePushDir(StorageSizeData *data, const char *path);
static void StorageSizeSetError(StorageSizeData *data, const char *path, const char *reason);

static const int StorageBatch = 250;

static void
SetLabelText(Widget widget, const char *text)
{
    XmString str = XmStringCreateLocalized(text ? text : "");
    XtVaSetValues(widget, XmNlabelString, str, NULL);
    XmStringFree(str);
}

static void
StorageSizeSetError(StorageSizeData *data, const char *path, const char *reason)
{
    if (data->error_message)
        XtFree(data->error_message);
    if (path && reason)
    {
        int len = strlen(path) + strlen(reason) + 5;
        data->error_message = XtMalloc(len);
        snprintf(data->error_message, len, "%s: %s", path, reason);
    }
    else if (reason)
        data->error_message = XtNewString(reason);
    else
        data->error_message = NULL;
}

static Boolean
StorageSizePushDir(StorageSizeData *data, const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
    {
        StorageSizeSetError(data, path, strerror(errno));
        return False;
    }

    DirStackEntry *entry = XtCalloc(1, sizeof(DirStackEntry));
    entry->dir = dir;
    entry->path = XtNewString(path);
    entry->next = data->stack;
    data->stack = entry;

    return True;
}

static void
StorageSizeFreeStack(StorageSizeData *data)
{
    while (data->stack)
    {
        DirStackEntry *entry = data->stack;
        data->stack = entry->next;
        if (entry->dir)
            closedir(entry->dir);
        XtFree(entry->path);
        XtFree(entry);
    }
}

static Boolean
StorageSizeWorkProc(XtPointer client_data)
{
    StorageSizeData *data = (StorageSizeData *)client_data;

    int processed = 0;
    while (processed < StorageBatch && data->stack && !data->cancel_request)
    {
        DirStackEntry *entry = data->stack;
        struct dirent *direntp = readdir(entry->dir);

        if (!direntp)
        {
            closedir(entry->dir);
            entry->dir = NULL;
            data->stack = entry->next;
            XtFree(entry->path);
            XtFree(entry);
            continue;
        }

        if (direntp->d_name[0] == '.' &&
            (direntp->d_name[1] == '\0' ||
             (direntp->d_name[1] == '.' && direntp->d_name[2] == '\0')))
            continue;

        char child[PATH_MAX];
        int written = snprintf(child, sizeof(child), "%s/%s", entry->path, direntp->d_name);
        if (written >= (int)sizeof(child))
            continue;

        struct stat st;
        if (lstat(child, &st) != 0)
        {
            StorageSizeSetError(data, child, strerror(errno));
            continue;
        }

        data->total_size += (unsigned long long)st.st_size;
        data->total_blocks += (unsigned long long)st.st_blocks * 512ULL;

        if (S_ISDIR(st.st_mode))
        {
            data->dir_count++;
            StorageSizePushDir(data, child);
        }
        else
        {
            data->file_count++;
        }

        processed++;
    }

    if ((!data->stack && !data->cancel_request) || data->cancel_request)
    {
        data->scanning = False;
        data->done = True;
        return False;
    }

    return True;
}

static void
StorageSizeUpdateButton(StorageSizeData *data)
{
    const char *label = data->done ? GETMESSAGE(35, 11, "OK")
                                    : GETMESSAGE(35, 10, "Cancel");
    SetLabelText(data->action_button, label);
}

static void
StorageSizeTimer(XtPointer client_data, XtIntervalId *id)
{
    StorageSizeData *data = (StorageSizeData *)client_data;
    if (data->closed)
        return;

    if (data->timer_id && id && *id != data->timer_id)
        return;

    if (data->timer_id)
    {
        XtRemoveTimeOut(data->timer_id);
        data->timer_id = 0;
    }

    if (data->closing && data->done)
    {
        StorageSizeClose(data);
        return;
    }

    char buf[256];
    char human_size[64];
    char human_storage[64];

    double size = (double)data->total_size;
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int index = 0;
    while (size >= 1024.0 && index < (int)(sizeof(units)/sizeof(units[0])) - 1)
    {
        size /= 1024.0;
        index++;
    }
    snprintf(human_size, sizeof(human_size), "%.1f %s", size, units[index]);

    double storage = (double)data->total_blocks;
    index = 0;
    while (storage >= 1024.0 && index < (int)(sizeof(units)/sizeof(units[0])) - 1)
    {
        storage /= 1024.0;
        index++;
    }
    snprintf(human_storage, sizeof(human_storage), "%.1f %s", storage, units[index]);

    snprintf(buf, sizeof(buf), GETMESSAGE(35, 5, "Total size: %s"), human_size);
    SetLabelText(data->size_label, buf);

    snprintf(buf, sizeof(buf), GETMESSAGE(35, 6, "Storage used: %s"), human_storage);
    SetLabelText(data->storage_label, buf);

    snprintf(buf, sizeof(buf), GETMESSAGE(35, 7, "Files scanned: %lu"), data->file_count);
    SetLabelText(data->files_label, buf);

    snprintf(buf, sizeof(buf), GETMESSAGE(35, 8, "Directories scanned: %lu"), data->dir_count);
    SetLabelText(data->dirs_label, buf);

    if (data->error_message)
    {
        SetLabelText(data->status_label, data->error_message);
    }
    else if (data->cancel_request && !data->done)
    {
        SetLabelText(data->status_label, GETMESSAGE(35, 12, "Canceling..."));
    }
    else if (!data->done)
    {
        SetLabelText(data->status_label, GETMESSAGE(35, 3, "One moment..."));
    }
    else if (data->cancel_request && data->done)
    {
        SetLabelText(data->status_label, GETMESSAGE(35, 13, "Storage size scan canceled."));
    }
    else
    {
        SetLabelText(data->status_label, GETMESSAGE(35, 9, "Storage size calculation complete."));
    }

    StorageSizeUpdateButton(data);

    if (!data->closed)
        data->timer_id = XtAppAddTimeOut(data->app, 1000, StorageSizeTimer, data);
}

static void
StorageSizeRequestClose(StorageSizeData *data)
{
    if (data->closing)
        return;

    data->closing = True;
    data->cancel_request = True;
    if (!data->scanning)
        StorageSizeClose(data);
}

static void
StorageSizeClose(StorageSizeData *data)
{
    if (data->closed)
        return;
    data->closed = True;

    if (data->timer_id)
    {
        XtRemoveTimeOut(data->timer_id);
        data->timer_id = 0;
    }

    if (data->work_proc_id)
    {
        XtRemoveWorkProc(data->work_proc_id);
        data->work_proc_id = 0;
    }

    StorageSizeFreeStack(data);

    if (data->shell)
    {
        XtPopdown(data->shell);
        XtDestroyWidget(data->shell);
    }

    XtFree(data->path);
    XtFree(data->title);
    XtFree(data->error_message);
    XtFree(data);
}

static void
StorageSizeCancelCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    StorageSizeData *data = (StorageSizeData *)client_data;
    if (data->done)
    {
        StorageSizeClose(data);
        return;
    }

    StorageSizeRequestClose(data);
}

static void
StorageSizeDestroyCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    StorageSizeData *data = (StorageSizeData *)client_data;
    StorageSizeRequestClose(data);
}

void
ShowStorageSizeDialog(FileViewData *file_view_data)
{
    if (!file_view_data)
        return;

    DirectorySet *directory_set = (DirectorySet *)file_view_data->directory_set;
    FileMgrData *file_mgr_data = (FileMgrData *)directory_set->file_mgr_data;
    FileMgrRec *file_mgr_rec = (FileMgrRec *)file_mgr_data->file_mgr_rec;

    StorageSizeData *data = XtCalloc(1, sizeof(StorageSizeData));
    data->app = XtWidgetToApplicationContext(file_mgr_rec->shell);
    data->scanning = True;

    data->path = _DtGetSelectedFilePath(file_view_data);
    if (!data->path)
    {
        XtFree(data);
        return;
    }

    struct stat st;
    if (lstat(data->path, &st) != 0)
    {
        StorageSizeSetError(data, data->path, strerror(errno));
        data->scanning = False;
        data->done = True;
    }
    else
    {
        data->total_size = (unsigned long long)st.st_size;
        data->total_blocks = (unsigned long long)st.st_blocks * 512ULL;
        if (S_ISDIR(st.st_mode))
            data->dir_count = 1;
        else
            data->file_count = 1;

        if (S_ISDIR(st.st_mode))
        {
            if (!StorageSizePushDir(data, data->path))
            {
                data->scanning = False;
                data->done = True;
            }
        }
        else
            data->scanning = False;
    }

    char title_buf[PATH_MAX + 64];
    snprintf(title_buf, sizeof(title_buf), GETMESSAGE(35, 4, "Storage size for %s"), data->path);
    data->title = XtNewString(title_buf);

    Arg args[5];
    int n = 0;
    XtSetArg(args[n], XmNallowShellResize, True); n++;
    XtSetArg(args[n], XmNtitle, data->title); n++;
    data->shell = XmCreateFormDialog(file_mgr_rec->shell, "storageSizeDialog", args, n);
    XtAddCallback(data->shell, XmNdestroyCallback, StorageSizeDestroyCB, data);

    Widget row = XmCreateRowColumn(data->shell, "storageSizeRowColumn", NULL, 0);
    XtVaSetValues(row, XmNorientation, XmVERTICAL, XmNspacing, 6, NULL);

    data->status_label = XmCreateLabelGadget(row, "storageSizeStatus", NULL, 0);
    data->size_label = XmCreateLabelGadget(row, "storageSizeSize", NULL, 0);
    data->storage_label = XmCreateLabelGadget(row, "storageSizeStorage", NULL, 0);
    data->files_label = XmCreateLabelGadget(row, "storageSizeFiles", NULL, 0);
    data->dirs_label = XmCreateLabelGadget(row, "storageSizeDirs", NULL, 0);

    XtManageChild(data->status_label);
    XtManageChild(data->size_label);
    XtManageChild(data->storage_label);
    XtManageChild(data->files_label);
    XtManageChild(data->dirs_label);

    Widget button_row = XmCreateRowColumn(data->shell, "storageSizeButtonRow", NULL, 0);
    XtVaSetValues(button_row, XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_COLUMN, XmNspacing, 12, NULL);

    data->action_button = XmCreatePushButtonGadget(button_row, "storageSizeAction", NULL, 0);
    XmString button_label = XmStringCreateLocalized(GETMESSAGE(35, 10, "Cancel"));
    XtVaSetValues(data->action_button, XmNlabelString, button_label, NULL);
    XmStringFree(button_label);

    XtAddCallback(data->action_button, XmNactivateCallback, StorageSizeCancelCB, data);

    XtManageChild(data->action_button);
    XtManageChild(button_row);
    XtManageChild(row);

    data->timer_id = XtAppAddTimeOut(data->app, 100, StorageSizeTimer, data);

    if (data->scanning && !data->work_proc_id)
        data->work_proc_id = XtAppAddWorkProc(data->app, StorageSizeWorkProc, data);

    XtManageChild(data->shell);
}
