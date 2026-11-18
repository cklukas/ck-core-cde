/*
 * FileManagerService.c - org.freedesktop.FileManager1 Session D-Bus Service
 *                        for CK-Core CDE File Manager (Wayland & X11, WP-16).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/Xm.h>
#include <Dt/Wsm.h>

#include "Encaps.h"
#include "SharedProcs.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "Help.h"
#include "ModAttr.h"
#include "Archive.h"
#include "Devices.h"
#include "XdgTrashSync.h"
#include "FileManagerService.h"

extern int mod_attr_dialog;
extern void ModAttrChange(XtPointer, DialogData *, DialogData *, XtPointer);
extern void ModAttrClose(XtPointer, DialogData *, DialogData *);

#define FM1_BUS_NAME    "org.freedesktop.FileManager1"
#define FM1_OBJECT_PATH "/org/freedesktop/FileManager1"
#define FM1_INTERFACE   "org.freedesktop.FileManager1"

static const char fm1_introspect_xml[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node name=\"/org/freedesktop/FileManager1\">\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"data\" direction=\"out\" type=\"s\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"property_name\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"value\" direction=\"out\" type=\"v\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"props\" direction=\"out\" type=\"a{sv}\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.FileManager1\">\n"
    "    <method name=\"ShowFolders\">\n"
    "      <arg name=\"URIs\" direction=\"in\" type=\"as\"/>\n"
    "      <arg name=\"StartupId\" direction=\"in\" type=\"s\"/>\n"
    "    </method>\n"
    "    <method name=\"ShowItems\">\n"
    "      <arg name=\"URIs\" direction=\"in\" type=\"as\"/>\n"
    "      <arg name=\"StartupId\" direction=\"in\" type=\"s\"/>\n"
    "    </method>\n"
    "    <method name=\"ShowItemProperties\">\n"
    "      <arg name=\"URIs\" direction=\"in\" type=\"as\"/>\n"
    "      <arg name=\"StartupId\" direction=\"in\" type=\"s\"/>\n"
    "    </method>\n"
    "    <property name=\"Hostname\" type=\"s\" access=\"read\"/>\n"
    "  </interface>\n"
    "</node>\n";

static DBusConnection *g_fm1_conn = nullptr;
static Widget g_fm1_toplevel = nullptr;
static XtInputId g_fm1_input_id = 0;
static int g_fm1_watch_fd = -1;
static XtIntervalId g_fm1_timer_id = 0;

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

Boolean DtfileDecodeFileUri(const char *uri, char *path_out, size_t path_sz) {
    if (!uri || !path_out || path_sz < 2)
        return False;

    const char *raw = nullptr;
    if (uri[0] == '/') {
        raw = uri;
    } else if (strncasecmp(uri, "file://", 7) == 0) {
        const char *rest = uri + 7;
        if (rest[0] == '/') {
            raw = rest;
        } else if (strncasecmp(rest, "localhost/", 10) == 0) {
            raw = rest + 9;
        } else {
            const char *slash = strchr(rest, '/');
            if (!slash)
                return False;
            size_t hlen = (size_t)(slash - rest);
            if (home_host_name[0] != '\0' &&
                strlen(home_host_name) == hlen &&
                strncasecmp(rest, home_host_name, hlen) == 0) {
                raw = slash;
            } else {
                char sys_host[256];
                if (gethostname(sys_host, sizeof(sys_host) - 1) == 0) {
                    sys_host[sizeof(sys_host) - 1] = '\0';
                    if (strlen(sys_host) == hlen && strncasecmp(rest, sys_host, hlen) == 0)
                        raw = slash;
                }
            }
            if (!raw)
                return False;
        }
    } else {
        return False;
    }

    size_t pos = 0;
    for (const char *p = raw; *p && *p != '?' && *p != '#'; ++p) {
        if (pos + 2 > path_sz)
            return False;
        if (*p == '%') {
            int hi = hex_val(p[1]);
            int lo = (hi >= 0) ? hex_val(p[2]) : -1;
            if (hi < 0 || lo < 0)
                return False;
            unsigned char ch = (unsigned char)((hi << 4) | lo);
            if (ch == '\0')
                return False;
            path_out[pos++] = (char)ch;
            p += 2;
        } else {
            path_out[pos++] = *p;
        }
    }
    if (pos == 0)
        return False;
    while (pos > 1 && path_out[pos - 1] == '/')
        pos--;
    path_out[pos] = '\0';
    return path_out[0] == '/' ? True : False;
}

static void raise_and_activate_shell(Widget shell) {
    if (!shell || !XtIsRealized(shell))
        return;
    Display *dpy = XtDisplay(shell);
    Window win = XtWindow(shell);
    if (!dpy || win == None)
        return;

    Window root = DefaultRootWindow(dpy);

    /* Ensure window occupies current CDE workspace */
    Atom cur_ws = None;
    if (DtWsmGetCurrentWorkspace(dpy, root, &cur_ws) == Success && cur_ws != None) {
        Atom *ws_list = nullptr;
        unsigned long num_ws = 0;
        if (DtWsmGetWorkspacesOccupied(dpy, win, &ws_list, &num_ws) == Success) {
            bool present = false;
            for (unsigned long i = 0; i < num_ws; ++i) {
                if (ws_list[i] == cur_ws) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                Atom *ext = (Atom *)XtRealloc((char *)ws_list, sizeof(Atom) * (num_ws + 1));
                ws_list = ext;
                ws_list[num_ws] = cur_ws;
                DtWsmSetWorkspacesOccupied(dpy, win, ws_list, num_ws + 1);
            }
            XtFree((char *)ws_list);
        }
    }

    XMapRaised(dpy, win);

    Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = net_active;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2; /* pager / user-initiated service request */
    ev.xclient.data.l[1] = CurrentTime;
    ev.xclient.data.l[2] = None;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

static void publish_fm1_observability(
    Widget shell,
    const char *method,
    const char *dir_path,
    const char *item_name)
{
    if (!g_fm1_toplevel)
        return;
    Display *dpy = XtDisplay(g_fm1_toplevel);
    if (!dpy)
        return;
    Window root = DefaultRootWindow(dpy);
    Window win = (shell && XtIsRealized(shell)) ? XtWindow(shell) : None;

    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom prop = XInternAtom(dpy, "_DT_FILEMGR_LAST_SHOW", False);
    char buf[PATH_MAX * 2 + 128];
    int n = snprintf(buf, sizeof(buf), "%s\t%s\t%s\t0x%lx",
                     method ? method : "",
                     dir_path ? dir_path : "",
                     item_name ? item_name : "",
                     (unsigned long)win);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        XChangeProperty(dpy, root, prop, utf8, 8, PropModeReplace,
                        (const unsigned char *)buf, (int)len);
        if (win != None) {
            XChangeProperty(dpy, win, prop, utf8, 8, PropModeReplace,
                            (const unsigned char *)buf, (int)len);
        }
        XFlush(dpy);
    }
}

typedef struct {
    FileMgrData *fmd;
    char *expected_dir;
    char *expected_item;
    int retries_left;
} SelectionVerifyCtx;

static void verify_selection_timer_cb(XtPointer client_data, XtIntervalId *id) {
    (void)id;
    SelectionVerifyCtx *ctx = (SelectionVerifyCtx *)client_data;
    if (!ctx)
        return;

    FileMgrData *fmd = ctx->fmd;
    bool valid_view = false;
    for (int i = 0; i < view_count; ++i) {
        if (view_set && view_set[i] && view_set[i]->dialog_data) {
            DialogData *dd = (DialogData *)view_set[i]->dialog_data;
            if (dd && (FileMgrData *)dd->data == fmd) {
                valid_view = true;
                break;
            }
        }
    }

    if (!valid_view) {
        XtFree(ctx->expected_dir);
        XtFree(ctx->expected_item);
        XtFree((char *)ctx);
        return;
    }

    /* Check if directory loading has finished */
    if (fmd->busy_status != not_busy && ctx->retries_left > 0) {
        ctx->retries_left--;
        XtAppAddTimeOut(XtWidgetToApplicationContext(g_fm1_toplevel),
                        100, verify_selection_timer_cb, (XtPointer)ctx);
        return;
    }

    /* Ensure item is selected in FileMgrData if present */
    bool selected_match = (fmd->selected_file_count == 1 &&
                           fmd->selection_list &&
                           fmd->selection_list[0] &&
                           fmd->selection_list[0]->file_data &&
                           strcmp(fmd->selection_list[0]->file_data->file_name,
                                  ctx->expected_item) == 0);

    if (!selected_match) {
        for (int d = 0; d < fmd->directory_count; ++d) {
            DirectorySet *ds = (DirectorySet *)fmd->directory_set[d];
            if (!ds || strcmp(ds->name, ctx->expected_dir) != 0)
                continue;
            for (int f = 0; f < ds->file_count; ++f) {
                FileViewData *fvd = ds->order_list[f];
                if (fvd && fvd->file_data && !fvd->filtered &&
                    strcmp(fvd->file_data->file_name, ctx->expected_item) == 0) {
                    FileMgrRec *fmr = (FileMgrRec *)fmd->file_mgr_rec;
                    DeselectAllFiles(fmd);
                    SelectFile(fmd, fvd);
                    if (fmr && fmd->selection_list && fmd->selection_list[0])
                        ActivateSingleSelect(fmr, fmd->selection_list[0]->file_data->logical_type);
                    PositionFileView(fvd, fmd);
                    selected_match = true;
                    break;
                }
            }
        }
    }

    if (!selected_match && ctx->retries_left > 0) {
        ctx->retries_left--;
        XtAppAddTimeOut(XtWidgetToApplicationContext(g_fm1_toplevel),
                        120, verify_selection_timer_cb, (XtPointer)ctx);
        return;
    }

    FileMgrRec *fmr = (FileMgrRec *)fmd->file_mgr_rec;
    if (fmr && fmr->shell && XtIsRealized(fmr->shell)) {
        Display *dpy = XtDisplay(fmr->shell);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        Atom sel_prop = XInternAtom(dpy, "_DT_FILEMGR_SELECTION", False);
        const char *sel_name = (fmd->selected_file_count > 0 &&
                                fmd->selection_list &&
                                fmd->selection_list[0] &&
                                fmd->selection_list[0]->file_data)
                                   ? fmd->selection_list[0]->file_data->file_name
                                   : "";
        char sbuf[PATH_MAX + 64];
        int sn = snprintf(sbuf, sizeof(sbuf), "%s\t%s\t%d",
                          fmd->current_directory ? fmd->current_directory : "",
                          sel_name,
                          fmd->selected_file_count);
        if (sn > 0) {
            size_t slen = (size_t)sn < sizeof(sbuf) ? (size_t)sn : sizeof(sbuf) - 1;
            XChangeProperty(dpy, DefaultRootWindow(dpy), sel_prop, utf8, 8,
                            PropModeReplace, (const unsigned char *)sbuf, (int)slen);
            XChangeProperty(dpy, XtWindow(fmr->shell), sel_prop, utf8, 8,
                            PropModeReplace, (const unsigned char *)sbuf, (int)slen);
            XFlush(dpy);
        }
    }

    XtFree(ctx->expected_dir);
    XtFree(ctx->expected_item);
    XtFree((char *)ctx);
}

static FileMgrData *find_or_open_view_for_dir(const char *dir_path) {
    if (!dir_path || dir_path[0] != '/')
        return nullptr;

    /* 1. Prefer an existing view already showing `dir_path` */
    for (int i = 0; i < view_count; ++i) {
        if (!view_set || !view_set[i] || !view_set[i]->dialog_data)
            continue;
        DialogData *dd = (DialogData *)view_set[i]->dialog_data;
        FileMgrData *fmd = dd ? (FileMgrData *)dd->data : nullptr;
        if (fmd && !fmd->toolbox && !fmd->IsTrashCan &&
            fmd->current_directory && strcmp(fmd->current_directory, dir_path) == 0) {
            return fmd;
        }
    }

    /* 2. If openDirType == CURRENT and a normal File Manager view exists, navigate it */
    if (openDirType == CURRENT) {
        for (int i = 0; i < view_count; ++i) {
            if (!view_set || !view_set[i] || !view_set[i]->dialog_data)
                continue;
            DialogData *dd = (DialogData *)view_set[i]->dialog_data;
            FileMgrData *fmd = dd ? (FileMgrData *)dd->data : nullptr;
            if (fmd && !fmd->toolbox && !fmd->IsTrashCan && !fmd->restricted_directory) {
                ShowNewDirectory(fmd, home_host_name, (char *)dir_path);
                return fmd;
            }
        }
    }

    /* 3. Otherwise open a brand-new File Manager view (works even in `dtfile -noview`!) */
    char dir_buf[PATH_MAX];
    snprintf(dir_buf, sizeof(dir_buf), "%s", dir_path);
    initiating_view = nullptr;
    special_view = False;
    DialogData *dd = GetNewView(home_host_name, dir_buf, nullptr, nullptr, 0);
    if (!dd && view_count > 0 && view_set && view_set[view_count - 1])
        dd = (DialogData *)view_set[view_count - 1]->dialog_data;
    return dd ? (FileMgrData *)dd->data : nullptr;
}

static void handle_show_folders(DBusMessage *msg) {
    DBusMessageIter args, arr;
    if (!dbus_message_iter_init(msg, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY)
        return;

    dbus_message_iter_recurse(&args, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char *uri = nullptr;
        dbus_message_iter_get_basic(&arr, &uri);
        char path[PATH_MAX];
        if (DtfileDecodeFileUri(uri, path, sizeof(path))) {
            struct stat st;
            if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
                char *slash = strrchr(path, '/');
                if (slash == path)
                    slash[1] = '\0';
                else if (slash)
                    *slash = '\0';
            }
            FileMgrData *fmd = find_or_open_view_for_dir(path);
            if (fmd && fmd->file_mgr_rec) {
                FileMgrRec *fmr = (FileMgrRec *)fmd->file_mgr_rec;
                raise_and_activate_shell(fmr->shell);
                publish_fm1_observability(fmr->shell, "ShowFolders", path, "");
            }
        }
        dbus_message_iter_next(&arr);
    }
}

static void select_item_in_view(FileMgrData *fmd, const char *dir_path, const char *item_name) {
    if (!fmd || !dir_path || !item_name || !item_name[0])
        return;

    XtFree(fmd->scrollToThisFile);
    XtFree(fmd->scrollToThisDirectory);
    fmd->scrollToThisFile = XtNewString((char *)item_name);
    fmd->scrollToThisDirectory = XtNewString((char *)dir_path);

    /* Try immediate selection if directory is already loaded, otherwise reread/navigate */
    bool found_now = false;
    if (fmd->current_directory && strcmp(fmd->current_directory, dir_path) == 0 &&
        fmd->busy_status == not_busy) {
        for (int d = 0; d < fmd->directory_count; ++d) {
            DirectorySet *ds = (DirectorySet *)fmd->directory_set[d];
            if (!ds || strcmp(ds->name, dir_path) != 0)
                continue;
            for (int f = 0; f < ds->file_count; ++f) {
                FileViewData *fvd = ds->order_list[f];
                if (fvd && fvd->file_data && !fvd->filtered &&
                    strcmp(fvd->file_data->file_name, item_name) == 0) {
                    FileMgrRec *fmr = (FileMgrRec *)fmd->file_mgr_rec;
                    DeselectAllFiles(fmd);
                    SelectFile(fmd, fvd);
                    if (fmr && fmd->selection_list && fmd->selection_list[0])
                        ActivateSingleSelect(fmr, fmd->selection_list[0]->file_data->logical_type);
                    PositionFileView(fvd, fmd);
                    XtFree(fmd->scrollToThisFile);
                    fmd->scrollToThisFile = nullptr;
                    XtFree(fmd->scrollToThisDirectory);
                    fmd->scrollToThisDirectory = nullptr;
                    found_now = true;
                    break;
                }
            }
        }
        if (!found_now) {
            /* Item may have been created just milliseconds ago; refresh directory */
            UpdateDirectory(nullptr, fmd->host ? fmd->host : home_host_name, (char *)dir_path);
        }
    } else if (!fmd->current_directory || strcmp(fmd->current_directory, dir_path) != 0) {
        ShowNewDirectory(fmd, fmd->host ? fmd->host : home_host_name, (char *)dir_path);
    }

    SelectionVerifyCtx *vctx = (SelectionVerifyCtx *)XtCalloc(1, sizeof(SelectionVerifyCtx));
    vctx->fmd = fmd;
    vctx->expected_dir = XtNewString((char *)dir_path);
    vctx->expected_item = XtNewString((char *)item_name);
    vctx->retries_left = 25;
    XtAppAddTimeOut(XtWidgetToApplicationContext(g_fm1_toplevel),
                    found_now ? 20 : 120, verify_selection_timer_cb, (XtPointer)vctx);
}

static void handle_show_items(DBusMessage *msg) {
    DBusMessageIter args, arr;
    if (!dbus_message_iter_init(msg, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY)
        return;

    dbus_message_iter_recurse(&args, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char *uri = nullptr;
        dbus_message_iter_get_basic(&arr, &uri);
        char path[PATH_MAX];
        if (DtfileDecodeFileUri(uri, path, sizeof(path))) {
            if (strcmp(path, "/") == 0) {
                FileMgrData *fmd = find_or_open_view_for_dir("/");
                if (fmd && fmd->file_mgr_rec) {
                    FileMgrRec *fmr = (FileMgrRec *)fmd->file_mgr_rec;
                    raise_and_activate_shell(fmr->shell);
                    publish_fm1_observability(fmr->shell, "ShowItems", "/", "");
                }
            } else {
                char dir_path[PATH_MAX];
                char item_name[NAME_MAX + 1];
                const char *slash = strrchr(path, '/');
                if (slash) {
                    snprintf(item_name, sizeof(item_name), "%s", slash + 1);
                    if (slash == path) {
                        strcpy(dir_path, "/");
                    } else {
                        size_t dlen = (size_t)(slash - path);
                        memcpy(dir_path, path, dlen);
                        dir_path[dlen] = '\0';
                    }
                    FileMgrData *fmd = find_or_open_view_for_dir(dir_path);
                    if (fmd && fmd->file_mgr_rec) {
                        FileMgrRec *fmr = (FileMgrRec *)fmd->file_mgr_rec;
                        select_item_in_view(fmd, dir_path, item_name);
                        raise_and_activate_shell(fmr->shell);
                        publish_fm1_observability(fmr->shell, "ShowItems", dir_path, item_name);
                    }
                }
            }
        }
        dbus_message_iter_next(&arr);
    }
}

static void open_properties_for_item(FileMgrData *fmd, const char *dir_path, const char *item_name) {
    if (!fmd || !fmd->file_mgr_rec || !dir_path || !item_name || !item_name[0])
        return;

    FileMgrRec *fmr = (FileMgrRec *)fmd->file_mgr_rec;
    DialogData *attr_dialog = _DtGetDefaultDialogData(mod_attr_dialog);
    if (!attr_dialog)
        return;

    Boolean loadOk = LoadFileAttributes(
        fmd->host ? fmd->host : home_host_name,
        (char *)dir_path,
        (char *)item_name,
        (ModAttrData *)attr_dialog->data);

    if (!loadOk) {
        _DtFreeDialogData(attr_dialog);
        return;
    }

    _DtAddOneSubdialog(attr_dialog, &fmd->attr_dialog_list, &fmd->attr_dialog_count);
    ((ModAttrData *)attr_dialog->data)->main_widget = fmr->main;
    _DtShowDialog(fmr->shell, nullptr, (XtPointer)fmr, attr_dialog,
                  ModAttrChange, (XtPointer)fmr,
                  ModAttrClose, (XtPointer)fmr,
                  nullptr, False, False, nullptr, nullptr);
}

static void handle_show_item_properties(DBusMessage *msg) {
    DBusMessageIter args, arr;
    if (!dbus_message_iter_init(msg, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY)
        return;

    dbus_message_iter_recurse(&args, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char *uri = nullptr;
        dbus_message_iter_get_basic(&arr, &uri);
        char path[PATH_MAX];
        if (DtfileDecodeFileUri(uri, path, sizeof(path)) && strcmp(path, "/") != 0) {
            char dir_path[PATH_MAX];
            char item_name[NAME_MAX + 1];
            const char *slash = strrchr(path, '/');
            if (slash && slash[1] != '\0') {
                snprintf(item_name, sizeof(item_name), "%s", slash + 1);
                if (slash == path) {
                    strcpy(dir_path, "/");
                } else {
                    size_t dlen = (size_t)(slash - path);
                    memcpy(dir_path, path, dlen);
                    dir_path[dlen] = '\0';
                }
                FileMgrData *fmd = find_or_open_view_for_dir(dir_path);
                if (fmd && fmd->file_mgr_rec) {
                    FileMgrRec *fmr = (FileMgrRec *)fmd->file_mgr_rec;
                    select_item_in_view(fmd, dir_path, item_name);
                    raise_and_activate_shell(fmr->shell);
                    open_properties_for_item(fmd, dir_path, item_name);
                    publish_fm1_observability(fmr->shell, "ShowItemProperties", dir_path, item_name);
                }
            }
        }
        dbus_message_iter_next(&arr);
    }
}

static DBusHandlerResult fm1_message_handler(
    DBusConnection *conn,
    DBusMessage *msg,
    void *user_data)
{
    (void)user_data;

    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            const char *xml = fm1_introspect_xml;
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Get")) {
        const char *iface = nullptr, *prop = nullptr;
        if (dbus_message_get_args(msg, nullptr,
                                  DBUS_TYPE_STRING, &iface,
                                  DBUS_TYPE_STRING, &prop,
                                  DBUS_TYPE_INVALID) &&
            prop && strcmp(prop, "Hostname") == 0) {
            char host[256];
            if (gethostname(host, sizeof(host) - 1) != 0)
                snprintf(host, sizeof(host), "%s", home_host_name[0] ? home_host_name : "localhost");
            host[sizeof(host) - 1] = '\0';
            const char *hptr = host;

            DBusMessage *reply = dbus_message_new_method_return(msg);
            if (reply) {
                DBusMessageIter it, var;
                dbus_message_iter_init_append(reply, &it);
                dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &hptr);
                dbus_message_iter_close_container(&it, &var);
                dbus_connection_send(conn, reply, nullptr);
                dbus_message_unref(reply);
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "GetAll")) {
        char host[256];
        if (gethostname(host, sizeof(host) - 1) != 0)
            snprintf(host, sizeof(host), "%s", home_host_name[0] ? home_host_name : "localhost");
        host[sizeof(host) - 1] = '\0';
        const char *hptr = host;
        const char *key = "Hostname";

        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            DBusMessageIter it, dict, entry, var;
            dbus_message_iter_init_append(reply, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
            dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &hptr);
            dbus_message_iter_close_container(&entry, &var);
            dbus_message_iter_close_container(&dict, &entry);
            dbus_message_iter_close_container(&it, &dict);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, FM1_INTERFACE, "ShowFolders")) {
        handle_show_folders(msg);
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, FM1_INTERFACE, "ShowItems")) {
        handle_show_items(msg);
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, FM1_INTERFACE, "ShowItemProperties")) {
        handle_show_item_properties(msg);
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.ck_core.Dtfile", "ActivateDevice")) {
        const char *tok = nullptr;
        if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &tok, DBUS_TYPE_INVALID) && tok) {
            FileMgrData *fmd = find_or_open_view_for_dir(users_home_dir[0] ? users_home_dir : "/");
            FileMgrRec *fmr = fmd ? (FileMgrRec *)fmd->file_mgr_rec : nullptr;
            (void)DtfileDevicesActivatePath(fmr, fmd, tok);
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.ck_core.Dtfile", "UnmountDevice") ||
        dbus_message_is_method_call(msg, "org.ck_core.Dtfile", "EjectDevice")) {
        Boolean eject = dbus_message_is_method_call(msg, "org.ck_core.Dtfile", "EjectDevice") ? True : False;
        const char *tok = nullptr;
        if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &tok, DBUS_TYPE_INVALID) && tok) {
            FileMgrData *fmd = (view_count > 0 && view_set && view_set[0] && view_set[0]->dialog_data)
                                   ? (FileMgrData *)((DialogData *)view_set[0]->dialog_data)->data
                                   : nullptr;
            Widget sh = (fmd && fmd->file_mgr_rec) ? ((FileMgrRec *)fmd->file_mgr_rec)->shell : g_fm1_toplevel;
            DtfileDevicesUnmountOrEject(sh, fmd, tok, eject);
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.ck_core.Dtfile", "OpenConnectServerDialog")) {
        ShowConnectServerDialog(nullptr, nullptr, nullptr);
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.ck_core.Dtfile", "TrashPaths")) {
        DBusMessageIter args, arr;
        if (dbus_message_iter_init(msg, &args) &&
            dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY) {
            const char *paths[64];
            int count = 0;
            dbus_message_iter_recurse(&args, &arr);
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING && count < 64) {
                const char *p = nullptr;
                dbus_message_iter_get_basic(&arr, &p);
                if (p && p[0] == '/') paths[count++] = p;
                dbus_message_iter_next(&arr);
            }
            if (count > 0)
                DtfileTrashBackendMovePaths(paths, count);
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.ck_core.Dtfile", "CreateArchive")) {
        DBusMessageIter args, arr;
        const char *inputs[64];
        int in_count = 0;
        const char *out_arc = nullptr;
        const char *fmt_str = nullptr;
        if (dbus_message_iter_init(msg, &args) &&
            dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&args, &arr);
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING && in_count < 64) {
                const char *p = nullptr;
                dbus_message_iter_get_basic(&arr, &p);
                if (p && p[0] == '/') inputs[in_count++] = p;
                dbus_message_iter_next(&arr);
            }
            if (dbus_message_iter_next(&args) && dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&args, &out_arc);
                if (dbus_message_iter_next(&args) && dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING)
                    dbus_message_iter_get_basic(&args, &fmt_str);
            }
        }
        char err_buf[512] = {0};
        DtfileArchiveStatus st = DtfileArchiveCreate(
            inputs, in_count, out_arc,
            DtfileArchiveDetectFormat(fmt_str ? fmt_str : out_arc),
            err_buf, sizeof(err_buf));
        if (st != DTFILE_ARCHIVE_OK) {
            DBusMessage *er = dbus_message_new_error(msg, "org.ck_core.Dtfile.ArchiveError",
                                                     err_buf[0] ? err_buf : "Archive create failed");
            if (er) { dbus_connection_send(conn, er, nullptr); dbus_message_unref(er); }
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (out_arc) {
            char dcopy[PATH_MAX];
            snprintf(dcopy, sizeof(dcopy), "%s", out_arc);
            char *sl = strrchr(dcopy, '/');
            if (sl) { if (sl == dcopy) sl[1] = '\0'; else *sl = '\0'; UpdateDirectory(nullptr, home_host_name, dcopy); }
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.ck_core.Dtfile", "ExtractArchive")) {
        const char *arc_path = nullptr, *dest_dir = nullptr;
        dbus_bool_t smart = TRUE;
        if (!dbus_message_get_args(msg, nullptr,
                                   DBUS_TYPE_STRING, &arc_path,
                                   DBUS_TYPE_STRING, &dest_dir,
                                   DBUS_TYPE_BOOLEAN, &smart,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *er = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid args");
            if (er) { dbus_connection_send(conn, er, nullptr); dbus_message_unref(er); }
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        char created[PATH_MAX] = {0};
        char err_buf[512] = {0};
        DtfileArchiveStatus st = DtfileArchiveExtract(
            arc_path, dest_dir, smart ? 1 : 0,
            created, sizeof(created), err_buf, sizeof(err_buf));
        if (st != DTFILE_ARCHIVE_OK) {
            DBusMessage *er = dbus_message_new_error(msg, "org.ck_core.Dtfile.ArchiveError",
                                                     err_buf[0] ? err_buf : "Archive extract failed");
            if (er) { dbus_connection_send(conn, er, nullptr); dbus_message_unref(er); }
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        UpdateDirectory(nullptr, home_host_name, (char *)dest_dir);
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            const char *cptr = created;
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &cptr, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void fm1_pump_dbus(void) {
    if (!g_fm1_conn)
        return;
    dbus_connection_read_write(g_fm1_conn, 0);
    while (dbus_connection_dispatch(g_fm1_conn) == DBUS_DISPATCH_DATA_REMAINS) {
    }
}

static void fm1_xt_input_cb(XtPointer client_data, int *source, XtInputId *id) {
    (void)client_data;
    (void)source;
    (void)id;
    fm1_pump_dbus();
}

static void fm1_timer_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data;
    (void)id;
    g_fm1_timer_id = 0;
    if (!g_fm1_conn || !g_fm1_toplevel)
        return;
    fm1_pump_dbus();
    g_fm1_timer_id = XtAppAddTimeOut(XtWidgetToApplicationContext(g_fm1_toplevel),
                                     250, fm1_timer_cb, nullptr);
}

void DtfileFileManagerServiceInit(Widget toplevel_widget) {
    if (g_fm1_conn || !toplevel_widget)
        return;
    if (getenv("DTFILE_DISABLE_FILEMANAGER1") != nullptr)
        return;

    g_fm1_toplevel = toplevel_widget;

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err) || !conn) {
        dbus_error_free(&err);
        return;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    int rc = dbus_bus_request_name(conn, FM1_BUS_NAME,
                                   DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                   &err);
    if (dbus_error_is_set(&err) ||
        (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
         rc != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER)) {
        dbus_error_free(&err);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return;
    }

    static const DBusObjectPathVTable vtable = {
        .unregister_function = nullptr,
        .message_function = fm1_message_handler,
    };
    if (!dbus_connection_register_object_path(conn, FM1_OBJECT_PATH, &vtable, nullptr)) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return;
    }

    g_fm1_conn = conn;
    int fd = -1;
    if (dbus_connection_get_unix_fd(conn, &fd) && fd >= 0) {
        g_fm1_watch_fd = fd;
        g_fm1_input_id = XtAppAddInput(XtWidgetToApplicationContext(toplevel_widget),
                                       fd, (XtPointer)XtInputReadMask,
                                       fm1_xt_input_cb, nullptr);
    }
    g_fm1_timer_id = XtAppAddTimeOut(XtWidgetToApplicationContext(toplevel_widget),
                                     250, fm1_timer_cb, nullptr);
    dbus_connection_flush(conn);
}

void DtfileFileManagerServiceShutdown(void) {
    if (g_fm1_timer_id != 0) {
        XtRemoveTimeOut(g_fm1_timer_id);
        g_fm1_timer_id = 0;
    }
    if (g_fm1_input_id != 0) {
        XtRemoveInput(g_fm1_input_id);
        g_fm1_input_id = 0;
    }
    g_fm1_watch_fd = -1;
    if (g_fm1_conn) {
        dbus_connection_close(g_fm1_conn);
        dbus_connection_unref(g_fm1_conn);
        g_fm1_conn = nullptr;
    }
}
