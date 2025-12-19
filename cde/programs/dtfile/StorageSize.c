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
#include <sys/statfs.h>
#include <dirent.h>

#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/LabelG.h>
#include <Xm/PushBG.h>
#include <Xm/Protocols.h>

#include "Encaps.h"
#include "Desktop.h"
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
    Widget form;
    Widget dialog_shell;
    Widget status_label;
    Widget size_desc_label;
    Widget size_value_label;
    Widget storage_desc_label;
    Widget storage_value_label;
    Widget files_desc_label;
    Widget files_value_label;
    Widget dirs_desc_label;
    Widget dirs_value_label;
    Widget permission_desc_label;
    Widget permission_value_label;
    Widget metadata_desc_label;
    Widget metadata_value_label;
    Widget virtual_desc_label;
    Widget virtual_value_label;
    Widget symlinks_desc_label;
    Widget symlinks_value_label;
    Widget special_desc_label;
    Widget special_value_label;
    Widget action_button;
    XtAppContext app;
    XtIntervalId timer_id;
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
    unsigned long regular_file_count;
    unsigned long dir_count;
    unsigned long permission_denied_dir_count;
    unsigned long virtual_entry_count;
    unsigned long symlink_count;
    unsigned long special_file_count;
    unsigned long long regular_size;
    unsigned long long regular_blocks;
    unsigned long long metadata_size;
    unsigned long long metadata_blocks;
} StorageSizeData;

static void StorageSizeTimer(XtPointer client_data, XtIntervalId *);
static void StorageSizeProcessBatch(StorageSizeData *data);
static void StorageSizeRequestClose(StorageSizeData *data);
static void StorageSizeClose(StorageSizeData *data);
static void StorageSizeCancelCB(Widget w, XtPointer client_data, XtPointer call_data);
static void StorageSizeDestroyCB(Widget w, XtPointer client_data, XtPointer call_data);
static void StorageSizeWMCloseCB(Widget w, XtPointer client_data, XtPointer call_data);
static void StorageSizeMotifWMCloseCB(Widget w, XtPointer client_data, XtPointer call_data);
static Widget CreateInfoRow(Widget parent, Widget prev_row, const char *row_name,
                            const char *desc_name, const char *value_name,
                            int desc_right_pos, int value_left_pos, int top_offset,
                            Widget *desc_label, Widget *value_label);
static void FormatThousands(unsigned long long value, char *buf, size_t bufsize);
static void StorageSizeAddEntrySize(StorageSizeData *data, const struct stat *st, Boolean metadata);
static void SetLabelText(Widget widget, const char *text);
static void StorageSizeUpdateButton(StorageSizeData *data);
static void StorageSizeFreeStack(StorageSizeData *data);
static Boolean StorageSizePushDir(StorageSizeData *data, const char *path);
static void StorageSizeSetError(StorageSizeData *data, const char *path, const char *reason);

static const int StorageBatch = 8000;

static void
SetLabelText(Widget widget, const char *text)
{
    XmString str = XmStringCreateLocalized((String)(text ? text : ""));
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

static void
FormatThousands(unsigned long long value, char *buf, size_t bufsize)
{
    if (bufsize == 0)
        return;

    char digits[32];
    int digit_count = 0;
    if (value == 0)
        digits[digit_count++] = '0';
    else
    {
        while (value > 0 && digit_count < (int)sizeof(digits))
        {
            digits[digit_count++] = '0' + (value % 10);
            value /= 10;
        }
    }

    char result[64];
    int res_index = 0;
    int group = 0;
    for (int i = 0; i < digit_count; ++i)
    {
        if (group == 3)
        {
            result[res_index++] = ',';
            group = 0;
        }
        result[res_index++] = digits[i];
        group++;
    }
    result[res_index] = '\0';

    int len = res_index;
    for (int i = 0; i < len / 2; ++i)
    {
        char tmp = result[i];
        result[i] = result[len - 1 - i];
        result[len - 1 - i] = tmp;
    }

    size_t copy_len = (size_t)len;
    if (copy_len >= bufsize)
        copy_len = bufsize - 1;
    memcpy(buf, result, copy_len);
    buf[copy_len] = '\0';
}

static void
StorageSizeAddEntrySize(StorageSizeData *data, const struct stat *st, Boolean metadata)
{
    unsigned long long size = (unsigned long long)st->st_size;
    unsigned long long blocks = (unsigned long long)st->st_blocks * 512ULL;
    if (metadata)
    {
        data->metadata_size += size;
        data->metadata_blocks += blocks;
    }
    else
    {
        data->total_size += size;
        data->total_blocks += blocks;
        data->regular_size += size;
        data->regular_blocks += blocks;
    }
}

static Boolean
StorageSizeIsVirtualFSType(long type)
{
    switch (type)
    {
#ifdef PROC_SUPER_MAGIC
    case PROC_SUPER_MAGIC:
        return True;
#endif
#ifdef SYSFS_MAGIC
    case SYSFS_MAGIC:
        return True;
#endif
#ifdef DEBUGFS_MAGIC
    case DEBUGFS_MAGIC:
        return True;
#endif
#ifdef TRACEFS_MAGIC
    case TRACEFS_MAGIC:
        return True;
#endif
#ifdef SECURITYFS_MAGIC
    case SECURITYFS_MAGIC:
        return True;
#endif
#ifdef BPF_FS_MAGIC
    case BPF_FS_MAGIC:
        return True;
#endif
#ifdef CGROUP_SUPER_MAGIC
    case CGROUP_SUPER_MAGIC:
        return True;
#endif
#ifdef CGROUP2_SUPER_MAGIC
    case CGROUP2_SUPER_MAGIC:
        return True;
#endif
#ifdef DEVPTS_SUPER_MAGIC
    case DEVPTS_SUPER_MAGIC:
        return True;
#endif
#ifdef MQUEUE_MAGIC
    case MQUEUE_MAGIC:
        return True;
#endif
#ifdef CONFIGFS_MAGIC
    case CONFIGFS_MAGIC:
        return True;
#endif
#ifdef SOCKFS_MAGIC
    case SOCKFS_MAGIC:
        return True;
#endif
#ifdef PIPEFS_MAGIC
    case PIPEFS_MAGIC:
        return True;
#endif
    default:
        return False;
    }
}

static Boolean
StorageSizeIsVirtualFS(const char *path)
{
    struct statfs fs;
    if (statfs(path, &fs) != 0)
        return False;
    return StorageSizeIsVirtualFSType(fs.f_type);
}

static Widget
CreateInfoRow(Widget parent, Widget prev_row, const char *row_name,
              const char *desc_name, const char *value_name,
              int desc_right_pos, int value_left_pos, int top_offset,
              Widget *desc_label, Widget *value_label)
{
    Arg args[10];
    int n = 0;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNfractionBase, 100); n++;
    if (prev_row)
    {
        XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
        XtSetArg(args[n], XmNtopWidget, prev_row); n++;
        XtSetArg(args[n], XmNtopOffset, top_offset); n++;
    }
    else
    {
        XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
        XtSetArg(args[n], XmNtopOffset, top_offset); n++;
    }

    Widget row_form = XmCreateForm(parent, (String)row_name, args, n);

    *desc_label = XmCreateLabelGadget(row_form, (String)desc_name, NULL, 0);
    XtVaSetValues(*desc_label,
                  XmNalignment, XmALIGNMENT_END,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, desc_right_pos,
                  NULL);
    XtManageChild(*desc_label);

    *value_label = XmCreateLabelGadget(row_form, (String)value_name, NULL, 0);
    XtVaSetValues(*value_label,
                  XmNalignment, XmALIGNMENT_BEGINNING,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, value_left_pos,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(*value_label);

    XtManageChild(row_form);
    return row_form;
}

static Boolean
StorageSizePushDir(StorageSizeData *data, const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
    {
        if (errno == EACCES)
            data->permission_denied_dir_count++;
        StorageSizeSetError(data, path, strerror(errno));
        return False;
    }

    DirStackEntry *entry = (DirStackEntry *)XtCalloc(1, sizeof(DirStackEntry));
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
        XtFree((char *)entry);
    }
}

static void
StorageSizeProcessBatch(StorageSizeData *data)
{
    if (!data->scanning)
        return;

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
            XtFree((char *)entry);
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

        if (StorageSizeIsVirtualFS(child))
        {
            data->virtual_entry_count++;
            continue;
        }

        if (S_ISDIR(st.st_mode))
        {
            data->dir_count++;
            StorageSizeAddEntrySize(data, &st, True);
            StorageSizePushDir(data, child);
        }
        else if (S_ISREG(st.st_mode))
        {
            data->regular_file_count++;
            StorageSizeAddEntrySize(data, &st, False);
        }
        else if (S_ISLNK(st.st_mode))
        {
            data->symlink_count++;
            StorageSizeAddEntrySize(data, &st, True);
        }
        else
        {
            data->special_file_count++;
            StorageSizeAddEntrySize(data, &st, True);
        }

        processed++;
    }

    if ((!data->stack && !data->cancel_request) || data->cancel_request)
    {
        data->scanning = False;
        data->done = True;
    }
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

    StorageSizeProcessBatch(data);

    char human_size[64];
    char human_storage[64];
    char size_value[256];
    char storage_value[256];
    char size_bytes[64];
    char storage_bytes[64];
    char size_sep[64];
    char storage_sep[64];
    char file_value[32];
    char dir_value[32];
    char permission_value[32];
    char virtual_value[64];
    char metadata_value[256];
    char metadata_bytes[64];
    char metadata_sep[64];
    char metadata_human[64];
    char symlink_value[64];
    char special_value[64];
    const char *byte_format = GETMESSAGE(35, 18, "(%s bytes)");

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

    FormatThousands(data->total_size, size_sep, sizeof(size_sep));
    snprintf(size_bytes, sizeof(size_bytes), byte_format, size_sep);
    snprintf(size_value, sizeof(size_value), "%s %s", human_size, size_bytes);
    SetLabelText(data->size_value_label, size_value);

    FormatThousands(data->total_blocks, storage_sep, sizeof(storage_sep));
    snprintf(storage_bytes, sizeof(storage_bytes), byte_format, storage_sep);
    snprintf(storage_value, sizeof(storage_value), "%s %s", human_storage, storage_bytes);
    SetLabelText(data->storage_value_label, storage_value);

    double metadata = (double)data->metadata_size;
    int metadata_index = 0;
    while (metadata >= 1024.0 && metadata_index < (int)(sizeof(units)/sizeof(units[0])) - 1)
    {
        metadata /= 1024.0;
        metadata_index++;
    }
    snprintf(metadata_human, sizeof(metadata_human), "%.1f %s", metadata, units[metadata_index]);
    FormatThousands(data->metadata_size, metadata_sep, sizeof(metadata_sep));
    snprintf(metadata_bytes, sizeof(metadata_bytes), byte_format, metadata_sep);
    snprintf(metadata_value, sizeof(metadata_value), "%s %s", metadata_human, metadata_bytes);
    SetLabelText(data->metadata_value_label, metadata_value);

    snprintf(file_value, sizeof(file_value), "%lu", data->regular_file_count);
    SetLabelText(data->files_value_label, file_value);

    snprintf(dir_value, sizeof(dir_value), "%lu", data->dir_count);
    SetLabelText(data->dirs_value_label, dir_value);

    snprintf(permission_value, sizeof(permission_value), "%lu", data->permission_denied_dir_count);
    SetLabelText(data->permission_value_label, permission_value);

    snprintf(special_value, sizeof(special_value), "%lu %s",
             data->special_file_count,
             GETMESSAGE(35, 22, "(ignored in totals)"));
    SetLabelText(data->special_value_label, special_value);

    snprintf(virtual_value, sizeof(virtual_value), "%lu %s",
             data->virtual_entry_count,
             GETMESSAGE(35, 22, "(ignored in totals)"));
    SetLabelText(data->virtual_value_label, virtual_value);

    snprintf(symlink_value, sizeof(symlink_value), "%lu %s",
             data->symlink_count,
             GETMESSAGE(35, 21, "(not followed)"));
    SetLabelText(data->symlinks_value_label, symlink_value);

    if (data->error_message && !data->done)
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
        SetLabelText(data->status_label, "");
    }

    StorageSizeUpdateButton(data);

    if (data->closing && data->done)
    {
        StorageSizeClose(data);
        return;
    }

    if (!data->closed && (data->scanning || (data->cancel_request && !data->done)))
    {
        int interval = data->scanning ? 1 : 1000;
        data->timer_id = XtAppAddTimeOut(data->app, interval, StorageSizeTimer, data);
    }
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
    data->closing = True;
    data->closed = True;

    if (data->timer_id)
    {
        XtRemoveTimeOut(data->timer_id);
        data->timer_id = 0;
    }

    StorageSizeFreeStack(data);

    if (data->dialog_shell)
    {
        XtPopdown(data->dialog_shell);
        XtDestroyWidget(data->dialog_shell);
        data->dialog_shell = NULL;
        data->form = NULL;
    }

    XtFree(data->path);
    XtFree(data->title);
    XtFree(data->error_message);
    XtFree((char *)data);
}

static void
StorageSizeCancelCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    StorageSizeData *data = (StorageSizeData *)client_data;
    if (data->closing)
        return;
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

static void
StorageSizeWMCloseCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    StorageSizeData *data = (StorageSizeData *)client_data;
    (void)w;
    (void)call_data;
    StorageSizeRequestClose(data);
}

static void
StorageSizeMotifWMCloseCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    StorageSizeData *data = (StorageSizeData *)client_data;
    XmAnyCallbackStruct *cb = (XmAnyCallbackStruct *)call_data;
    if (!cb || !cb->event || data->closing)
        return;

    if (cb->event->type == ClientMessage)
    {
        StorageSizeRequestClose(data);
    }
}

void
ShowStorageSizeDialog(Widget w, XtPointer client_data, XtPointer call_data)
{
    FileViewData *file_view_data = (FileViewData *)client_data;
    if (!file_view_data)
        return;

    DirectorySet *directory_set = (DirectorySet *)file_view_data->directory_set;
    FileMgrData *file_mgr_data = (FileMgrData *)directory_set->file_mgr_data;
    FileMgrRec *file_mgr_rec = (FileMgrRec *)file_mgr_data->file_mgr_rec;

    StorageSizeData *data = (StorageSizeData *)XtCalloc(1, sizeof(StorageSizeData));
    data->app = XtWidgetToApplicationContext(file_mgr_rec->shell);
    data->scanning = True;

    data->path = _DtGetSelectedFilePath(file_view_data);
    if (!data->path)
    {
        XtFree((char *)data);
        return;
    }

    struct stat st;
    if (lstat(data->path, &st) != 0)
    {
        StorageSizeSetError(data, data->path, strerror(errno));
        data->scanning = False;
        data->done = True;
    }
    else if (StorageSizeIsVirtualFS(data->path))
    {
        data->virtual_entry_count = 1;
        data->scanning = False;
        data->done = True;
    }
    else if (S_ISDIR(st.st_mode))
    {
        data->dir_count = 1;
        StorageSizeAddEntrySize(data, &st, True);
        if (!StorageSizePushDir(data, data->path))
        {
            data->scanning = False;
            data->done = True;
        }
    }
    else if (S_ISREG(st.st_mode))
    {
        data->regular_file_count = 1;
        StorageSizeAddEntrySize(data, &st, False);
        data->scanning = False;
    }
    else if (S_ISLNK(st.st_mode))
    {
        data->symlink_count = 1;
        StorageSizeAddEntrySize(data, &st, True);
        data->scanning = False;
    }
    else
    {
        data->special_file_count = 1;
        StorageSizeAddEntrySize(data, &st, True);
        data->scanning = False;
    }

    char title_buf[PATH_MAX + 64];
    snprintf(title_buf, sizeof(title_buf), GETMESSAGE(35, 4, "Storage size for %s"), data->path);
    data->title = XtNewString(title_buf);

    Arg args[7];
    int n = 0;
    XtSetArg(args[n], XmNallowShellResize, True); n++;
    XtSetArg(args[n], XmNtitle, data->title); n++;
    XtSetArg(args[n], XmNmarginWidth, 16); n++;
    XtSetArg(args[n], XmNmarginHeight, 12); n++;
    XtSetArg(args[n], XmNdeleteResponse, XmDO_NOTHING); n++;
    data->form = XmCreateFormDialog(file_mgr_rec->shell, "storageSizeDialog", args, n);
    data->dialog_shell = XtParent(data->form);
    XtAddCallback(data->dialog_shell, XmNdestroyCallback, StorageSizeDestroyCB, data);

    Atom wm_delete = XmInternAtom(XtDisplay(data->dialog_shell), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(data->dialog_shell, wm_delete, StorageSizeWMCloseCB, data);

    Atom motif_messages = XmInternAtom(XtDisplay(data->dialog_shell), "_MOTIF_WM_MESSAGES", False);
    if (motif_messages != None && wm_delete != None)
    {
        Atom protocols[] = {wm_delete};
        XmAddProtocols(data->dialog_shell, motif_messages, protocols, 1);
        XmAddProtocolCallback(data->dialog_shell, motif_messages, wm_delete,
                              StorageSizeMotifWMCloseCB, data);
    }

    data->status_label = XmCreateLabelGadget(data->form, "storageSizeStatus", NULL, 0);
    XtVaSetValues(data->status_label,
                  XmNalignment, XmALIGNMENT_BEGINNING,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopOffset, 4,
                  NULL);
    XtManageChild(data->status_label);

    Widget info_form = XmCreateForm(data->form, "storageSizeInfoForm", NULL, 0);
    XtVaSetValues(info_form,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, data->status_label,
                  XmNtopOffset, 12,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNfractionBase, 100,
                  NULL);

    const int desc_right_pos = 40;
    const int value_left_pos = 42;
    const int row_spacing = 6;
    Widget prev_row = NULL;
    Widget row_form;

    row_form = CreateInfoRow(info_form, prev_row, "storageSizeSizeRow",
                             "storageSizeSizeDesc", "storageSizeSizeValue",
                             desc_right_pos, value_left_pos, 0,
                             &data->size_desc_label, &data->size_value_label);
    SetLabelText(data->size_desc_label, GETMESSAGE(35, 14, "Total size:"));
    prev_row = row_form;

    row_form = CreateInfoRow(info_form, prev_row, "storageSizeStorageRow",
                             "storageSizeStorageDesc", "storageSizeStorageValue",
                             desc_right_pos, value_left_pos, row_spacing,
                             &data->storage_desc_label, &data->storage_value_label);
    SetLabelText(data->storage_desc_label, GETMESSAGE(35, 15, "Storage used:"));
    prev_row = row_form;

    row_form = CreateInfoRow(info_form, prev_row, "storageSizeMetadataRow",
                             "storageSizeMetadataDesc", "storageSizeMetadataValue",
                             desc_right_pos, value_left_pos, row_spacing,
                             &data->metadata_desc_label, &data->metadata_value_label);
    SetLabelText(data->metadata_desc_label, GETMESSAGE(35, 25, "Metadata size (dirs/special/symlinks):"));
    prev_row = row_form;

    row_form = CreateInfoRow(info_form, prev_row, "storageSizeFilesRow",
                             "storageSizeFilesDesc", "storageSizeFilesValue",
                             desc_right_pos, value_left_pos, row_spacing,
                             &data->files_desc_label, &data->files_value_label);
    SetLabelText(data->files_desc_label, GETMESSAGE(35, 16, "Files:"));
    prev_row = row_form;

    row_form = CreateInfoRow(info_form, prev_row, "storageSizeDirsRow",
                             "storageSizeDirsDesc", "storageSizeDirsValue",
                             desc_right_pos, value_left_pos, row_spacing,
                             &data->dirs_desc_label, &data->dirs_value_label);
    SetLabelText(data->dirs_desc_label, GETMESSAGE(35, 17, "Directories:"));
    prev_row = row_form;

    row_form = CreateInfoRow(info_form, prev_row, "storageSizePermissionRow",
                             "storageSizePermissionDesc", "storageSizePermissionValue",
                             desc_right_pos, value_left_pos, row_spacing,
                             &data->permission_desc_label, &data->permission_value_label);
    SetLabelText(data->permission_desc_label, GETMESSAGE(35, 23, "Permission denied directories:"));
    prev_row = row_form;

    row_form = CreateInfoRow(info_form, prev_row, "storageSizeSpecialRow",
                             "storageSizeSpecialDesc", "storageSizeSpecialValue",
                             desc_right_pos, value_left_pos, row_spacing,
                             &data->special_desc_label, &data->special_value_label);
    SetLabelText(data->special_desc_label, GETMESSAGE(35, 20, "Special files:"));
    prev_row = row_form;

    row_form = CreateInfoRow(info_form, prev_row, "storageSizeVirtualRow",
                             "storageSizeVirtualDesc", "storageSizeVirtualValue",
                             desc_right_pos, value_left_pos, row_spacing,
                             &data->virtual_desc_label, &data->virtual_value_label);
    SetLabelText(data->virtual_desc_label, GETMESSAGE(35, 24, "Virtual filesystem entries:"));
    prev_row = row_form;

    row_form = CreateInfoRow(info_form, prev_row, "storageSizeSymlinkRow",
                             "storageSizeSymlinkDesc", "storageSizeSymlinkValue",
                             desc_right_pos, value_left_pos, row_spacing,
                             &data->symlinks_desc_label, &data->symlinks_value_label);
    SetLabelText(data->symlinks_desc_label, GETMESSAGE(35, 19, "Symlinks:"));

    XtManageChild(info_form);

    Widget button_row = XmCreateRowColumn(data->form, "storageSizeButtonRow", NULL, 0);
    XtVaSetValues(button_row,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_COLUMN,
                  XmNspacing, 12,
                  XmNalignment, XmALIGNMENT_CENTER,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, data->symlinks_value_label,
                  XmNtopOffset, 16,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);

    data->action_button = XmCreatePushButtonGadget(button_row, "storageSizeAction", NULL, 0);
    XmString button_label = XmStringCreateLocalized(GETMESSAGE(35, 10, "Cancel"));
    XtVaSetValues(data->action_button, XmNlabelString, button_label, NULL);
    XmStringFree(button_label);
    XtAddCallback(data->action_button, XmNactivateCallback, StorageSizeCancelCB, data);

    XtManageChild(data->action_button);
    XtManageChild(button_row);

    StorageSizeTimer(data, NULL);

    XtManageChild(data->form);
}
