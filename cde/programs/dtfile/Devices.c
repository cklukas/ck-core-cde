/*
 * Devices.c - UDisks2 Removable/LUKS Volumes & GVFS Network Shares / MTP
 *             Integration for CK-Core CDE File Manager (Wayland & X11, WP-16).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/MessageB.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/Separator.h>
#include <Xm/TextF.h>
#include <Dt/SharedProcs.h>

#include "Encaps.h"
#include "SharedProcs.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "Help.h"
#include "Shelf.h"
#include "Mounts.h"
#include "XdgTrashSync.h"
#include "Devices.h"

#define UDISKS_BUS_NAME   "org.freedesktop.UDisks2"
#define UDISKS_ROOT_PATH  "/org/freedesktop/UDisks2"
#define MAX_DRIVES        64
#define MAX_DEVICES       96
#define MAX_SECRET_LEN    512

typedef struct {
    char obj_path[256];
    char vendor[64];
    char model[64];
    char connection_bus[32];
    bool removable;
    bool ejectable;
    bool can_power_off;
} UDisksDriveRec;

typedef struct {
    char obj_path[256];
    char device[256];
    char id_label[128];
    char id_uuid[128];
    char id_usage[64];
    char id_type[64];
    char hint_name[128];
    char drive_path[256];
    char crypto_backing[256];
    char cleartext_device[256];
    char mount_point[MAX_PATH];
    unsigned long long size_bytes;
    bool hint_ignore;
    bool hint_system;
    bool has_filesystem;
    bool has_encrypted;
} UDisksBlockRec;

static Widget g_dev_toplevel = nullptr;
static DBusConnection *g_udisks_conn = nullptr;
static XtInputId g_udisks_input = 0;
static XtIntervalId g_udisks_timer = 0;
static XtIntervalId g_udisks_rescan_debounce = 0;

static int g_gvfs_inotify_fd = -1;
static XtInputId g_gvfs_input = 0;
static int g_gvfs_watch_wd = -1;
static XtIntervalId g_gvfs_debounce = 0;

static DtfileDeviceEntry g_devices[MAX_DEVICES];
static int g_device_count = 0;

/* Track dynamic Shelf tokens managed by Devices.c so unmounted ones are removed cleanly */
static char g_managed_tokens[MAX_DEVICES][MAX_PATH];
static int g_managed_token_count = 0;

static void secure_zero(void *ptr, size_t len) {
    if (!ptr || len == 0)
        return;
    volatile unsigned char *vp = (volatile unsigned char *)ptr;
    while (len--)
        *vp++ = 0;
}

static void dev_copy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_sz, "%s", src);
}

static void get_gvfs_root_dir(char *out, size_t out_sz) {
    const char *override_root = getenv("DTFILE_GVFS_ROOT");
    if (override_root && override_root[0] == '/') {
        size_t len = strlen(override_root);
        while (len > 1 && override_root[len - 1] == '/')
            len--;
        snprintf(out, out_sz, "%.*s", (int)len, override_root);
        return;
    }
    const char *xdg_rt = getenv("XDG_RUNTIME_DIR");
    if (xdg_rt && xdg_rt[0] == '/') {
        snprintf(out, out_sz, "%s/gvfs", xdg_rt);
        return;
    }
    snprintf(out, out_sz, "/run/user/%u/gvfs", (unsigned)getuid());
}

Boolean DtfileDevicesIsSyntheticPath(const char *path) {
    return (path && strncmp(path, "udisks://", 9) == 0) ? True : False;
}

Boolean DtfileDevicesIsGvfsPath(const char *path) {
    if (!path || path[0] != '/')
        return False;
    char gvfs_root[PATH_MAX];
    get_gvfs_root_dir(gvfs_root, sizeof(gvfs_root));
    size_t rlen = strlen(gvfs_root);
    if (rlen == 0 || strncmp(path, gvfs_root, rlen) != 0)
        return False;
    return (path[rlen] == '/' && path[rlen + 1] != '\0') ? True : False;
}

/* ---- D-Bus variant helpers for UDisks2 ObjectManager ---- */

static void extract_byte_array_string(DBusMessageIter *variant_iter, char *out, size_t out_sz) {
    out[0] = '\0';
    if (dbus_message_iter_get_arg_type(variant_iter) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter arr;
    dbus_message_iter_recurse(variant_iter, &arr);
    if (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_BYTE)
        return;
    const unsigned char *bytes = nullptr;
    int n_elem = 0;
    dbus_message_iter_get_fixed_array(&arr, &bytes, &n_elem);
    if (bytes && n_elem > 0) {
        size_t len = (size_t)n_elem;
        while (len > 0 && bytes[len - 1] == '\0')
            len--;
        if (len >= out_sz)
            len = out_sz - 1;
        memcpy(out, bytes, len);
        out[len] = '\0';
    }
}

static void extract_first_mount_point(DBusMessageIter *variant_iter, char *out, size_t out_sz) {
    out[0] = '\0';
    if (dbus_message_iter_get_arg_type(variant_iter) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter outer;
    dbus_message_iter_recurse(variant_iter, &outer);
    if (dbus_message_iter_get_arg_type(&outer) == DBUS_TYPE_ARRAY) {
        extract_byte_array_string(&outer, out, out_sz);
    }
}

static void parse_drive_props(DBusMessageIter *dict_iter, UDisksDriveRec *drv) {
    while (dbus_message_iter_get_arg_type(dict_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, var;
        const char *key = nullptr;
        dbus_message_iter_recurse(dict_iter, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
            if (dbus_message_iter_next(&entry) &&
                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &var);
                int vtype = dbus_message_iter_get_arg_type(&var);
                if (strcmp(key, "Vendor") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(drv->vendor, sizeof(drv->vendor), s);
                } else if (strcmp(key, "Model") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(drv->model, sizeof(drv->model), s);
                } else if (strcmp(key, "ConnectionBus") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(drv->connection_bus, sizeof(drv->connection_bus), s);
                } else if ((strcmp(key, "Removable") == 0 || strcmp(key, "MediaRemovable") == 0) &&
                           vtype == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b = FALSE;
                    dbus_message_iter_get_basic(&var, &b);
                    if (b) drv->removable = true;
                } else if (strcmp(key, "Ejectable") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b = FALSE;
                    dbus_message_iter_get_basic(&var, &b);
                    drv->ejectable = (b != FALSE);
                } else if (strcmp(key, "CanPowerOff") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b = FALSE;
                    dbus_message_iter_get_basic(&var, &b);
                    drv->can_power_off = (b != FALSE);
                }
            }
        }
        dbus_message_iter_next(dict_iter);
    }
}

static void parse_block_props(DBusMessageIter *dict_iter, UDisksBlockRec *blk) {
    while (dbus_message_iter_get_arg_type(dict_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, var;
        const char *key = nullptr;
        dbus_message_iter_recurse(dict_iter, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
            if (dbus_message_iter_next(&entry) &&
                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &var);
                int vtype = dbus_message_iter_get_arg_type(&var);
                if (strcmp(key, "Device") == 0) {
                    if (vtype == DBUS_TYPE_STRING) {
                        const char *s = nullptr;
                        dbus_message_iter_get_basic(&var, &s);
                        dev_copy(blk->device, sizeof(blk->device), s);
                    } else {
                        extract_byte_array_string(&var, blk->device, sizeof(blk->device));
                    }
                } else if (strcmp(key, "IdLabel") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(blk->id_label, sizeof(blk->id_label), s);
                } else if (strcmp(key, "IdUUID") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(blk->id_uuid, sizeof(blk->id_uuid), s);
                } else if (strcmp(key, "IdUsage") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(blk->id_usage, sizeof(blk->id_usage), s);
                } else if (strcmp(key, "IdType") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(blk->id_type, sizeof(blk->id_type), s);
                } else if (strcmp(key, "HintName") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(blk->hint_name, sizeof(blk->hint_name), s);
                } else if (strcmp(key, "Drive") == 0 && vtype == DBUS_TYPE_OBJECT_PATH) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(blk->drive_path, sizeof(blk->drive_path), s);
                } else if (strcmp(key, "CryptoBackingDevice") == 0 && vtype == DBUS_TYPE_OBJECT_PATH) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(blk->crypto_backing, sizeof(blk->crypto_backing), s);
                } else if (strcmp(key, "Size") == 0 && vtype == DBUS_TYPE_UINT64) {
                    uint64_t sz = 0;
                    dbus_message_iter_get_basic(&var, &sz);
                    blk->size_bytes = (unsigned long long)sz;
                } else if (strcmp(key, "HintIgnore") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b = FALSE;
                    dbus_message_iter_get_basic(&var, &b);
                    blk->hint_ignore = (b != FALSE);
                } else if (strcmp(key, "HintSystem") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b = FALSE;
                    dbus_message_iter_get_basic(&var, &b);
                    blk->hint_system = (b != FALSE);
                }
            }
        }
        dbus_message_iter_next(dict_iter);
    }
}

static void parse_fs_props(DBusMessageIter *dict_iter, UDisksBlockRec *blk) {
    blk->has_filesystem = true;
    while (dbus_message_iter_get_arg_type(dict_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, var;
        const char *key = nullptr;
        dbus_message_iter_recurse(dict_iter, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
            if (strcmp(key, "MountPoints") == 0 &&
                dbus_message_iter_next(&entry) &&
                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &var);
                if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter arr;
                    dbus_message_iter_recurse(&var, &arr);
                    if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                        const char *s = nullptr;
                        dbus_message_iter_get_basic(&arr, &s);
                        dev_copy(blk->mount_point, sizeof(blk->mount_point), s);
                    } else {
                        extract_first_mount_point(&var, blk->mount_point, sizeof(blk->mount_point));
                    }
                }
            }
        }
        dbus_message_iter_next(dict_iter);
    }
}

static void parse_encrypted_props(DBusMessageIter *dict_iter, UDisksBlockRec *blk) {
    blk->has_encrypted = true;
    while (dbus_message_iter_get_arg_type(dict_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, var;
        const char *key = nullptr;
        dbus_message_iter_recurse(dict_iter, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
            if (strcmp(key, "CleartextDevice") == 0 &&
                dbus_message_iter_next(&entry) &&
                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &var);
                if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_OBJECT_PATH ||
                    dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
                    const char *s = nullptr;
                    dbus_message_iter_get_basic(&var, &s);
                    dev_copy(blk->cleartext_device, sizeof(blk->cleartext_device), s);
                }
            }
        }
        dbus_message_iter_next(dict_iter);
    }
}

static const UDisksDriveRec *find_drive(const UDisksDriveRec *drives, int n_drives, const char *path) {
    if (!path || !path[0] || strcmp(path, "/") == 0)
        return nullptr;
    for (int i = 0; i < n_drives; ++i) {
        if (strcmp(drives[i].obj_path, path) == 0)
            return &drives[i];
    }
    return nullptr;
}

static const UDisksBlockRec *find_block(const UDisksBlockRec *blocks, int n_blocks, const char *path) {
    if (!path || !path[0] || strcmp(path, "/") == 0)
        return nullptr;
    for (int i = 0; i < n_blocks; ++i) {
        if (strcmp(blocks[i].obj_path, path) == 0)
            return &blocks[i];
    }
    return nullptr;
}

static void scan_udisks_devices(void) {
    if (!g_udisks_conn)
        return;

    DBusMessage *req = dbus_message_new_method_call(
        UDISKS_BUS_NAME, UDISKS_ROOT_PATH,
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!req)
        return;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(g_udisks_conn, req, 1500, &err);
    dbus_message_unref(req);
    if (dbus_error_is_set(&err) || !reply) {
        dbus_error_free(&err);
        return;
    }

    UDisksDriveRec *drives = (UDisksDriveRec *)calloc(MAX_DRIVES, sizeof(UDisksDriveRec));
    UDisksBlockRec *blocks = (UDisksBlockRec *)calloc(MAX_DEVICES, sizeof(UDisksBlockRec));
    int n_drives = 0, n_blocks = 0;

    DBusMessageIter root_iter, obj_arr;
    if (drives && blocks &&
        dbus_message_iter_init(reply, &root_iter) &&
        dbus_message_iter_get_arg_type(&root_iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&root_iter, &obj_arr);
        while (dbus_message_iter_get_arg_type(&obj_arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter obj_entry, iface_arr;
            const char *obj_path = nullptr;
            dbus_message_iter_recurse(&obj_arr, &obj_entry);
            if (dbus_message_iter_get_arg_type(&obj_entry) == DBUS_TYPE_OBJECT_PATH) {
                dbus_message_iter_get_basic(&obj_entry, &obj_path);
                if (obj_path && dbus_message_iter_next(&obj_entry) &&
                    dbus_message_iter_get_arg_type(&obj_entry) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&obj_entry, &iface_arr);

                    UDisksDriveRec *drv_rec = nullptr;
                    UDisksBlockRec *blk_rec = nullptr;

                    while (dbus_message_iter_get_arg_type(&iface_arr) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter if_entry, prop_dict;
                        const char *if_name = nullptr;
                        dbus_message_iter_recurse(&iface_arr, &if_entry);
                        if (dbus_message_iter_get_arg_type(&if_entry) == DBUS_TYPE_STRING) {
                            dbus_message_iter_get_basic(&if_entry, &if_name);
                            if (if_name && dbus_message_iter_next(&if_entry) &&
                                dbus_message_iter_get_arg_type(&if_entry) == DBUS_TYPE_ARRAY) {
                                dbus_message_iter_recurse(&if_entry, &prop_dict);
                                if (strcmp(if_name, "org.freedesktop.UDisks2.Drive") == 0) {
                                    if (!drv_rec && n_drives < MAX_DRIVES) {
                                        drv_rec = &drives[n_drives++];
                                        dev_copy(drv_rec->obj_path, sizeof(drv_rec->obj_path), obj_path);
                                    }
                                    if (drv_rec)
                                        parse_drive_props(&prop_dict, drv_rec);
                                } else if (strcmp(if_name, "org.freedesktop.UDisks2.Block") == 0 ||
                                           strcmp(if_name, "org.freedesktop.UDisks2.Filesystem") == 0 ||
                                           strcmp(if_name, "org.freedesktop.UDisks2.Encrypted") == 0) {
                                    if (!blk_rec && n_blocks < MAX_DEVICES) {
                                        blk_rec = &blocks[n_blocks++];
                                        dev_copy(blk_rec->obj_path, sizeof(blk_rec->obj_path), obj_path);
                                    }
                                    if (blk_rec) {
                                        if (strcmp(if_name, "org.freedesktop.UDisks2.Block") == 0)
                                            parse_block_props(&prop_dict, blk_rec);
                                        else if (strcmp(if_name, "org.freedesktop.UDisks2.Filesystem") == 0)
                                            parse_fs_props(&prop_dict, blk_rec);
                                        else if (strcmp(if_name, "org.freedesktop.UDisks2.Encrypted") == 0)
                                            parse_encrypted_props(&prop_dict, blk_rec);
                                    }
                                }
                            }
                        }
                        dbus_message_iter_next(&iface_arr);
                    }
                }
            }
            dbus_message_iter_next(&obj_arr);
        }
    }
    dbus_message_unref(reply);

    bool show_all = (getenv("DTFILE_UDISKS_SHOW_ALL") != nullptr);

    for (int i = 0; i < n_blocks && g_device_count < MAX_DEVICES; ++i) {
        const UDisksBlockRec *b = &blocks[i];
        if (b->hint_ignore)
            continue;
        if (!b->has_filesystem && !b->has_encrypted)
            continue;

        /* Skip root / boot / swap system partitions */
        if (strcmp(b->mount_point, "/") == 0 ||
            strncmp(b->mount_point, "/boot", 5) == 0 ||
            strcmp(b->id_usage, "other") == 0)
            continue;

        /* If this is an unlocked LUKS container (`CleartextDevice != "/"`), skip the raw container
         * because its unlocked cleartext block device will represent the volume! */
        if (b->has_encrypted && b->cleartext_device[0] == '/' && strcmp(b->cleartext_device, "/") != 0)
            continue;

        const UDisksBlockRec *backing = find_block(blocks, n_blocks, b->crypto_backing);
        const char *eff_drive_path = (b->drive_path[0] && strcmp(b->drive_path, "/") != 0)
                                         ? b->drive_path
                                         : (backing ? backing->drive_path : "");
        const UDisksDriveRec *drv = find_drive(drives, n_drives, eff_drive_path);

        bool is_removable_bus = false;
        if (drv) {
            if (drv->removable || drv->ejectable || drv->can_power_off ||
                strcasecmp(drv->connection_bus, "usb") == 0 ||
                strcasecmp(drv->connection_bus, "sdio") == 0 ||
                strcasecmp(drv->connection_bus, "mmc") == 0 ||
                strcasecmp(drv->connection_bus, "ieee1394") == 0) {
                is_removable_bus = true;
            }
        }
        bool is_luks = (b->has_encrypted || backing != nullptr ||
                        strcmp(b->id_type, "crypto_LUKS") == 0);

        if (!show_all && b->hint_system && !is_removable_bus && !is_luks)
            continue;

        DtfileDeviceEntry *de = &g_devices[g_device_count++];
        memset(de, 0, sizeof(*de));
        de->is_udisks = True;
        de->size_bytes = b->size_bytes ? b->size_bytes : (backing ? backing->size_bytes : 0);
        dev_copy(de->block_obj_path, sizeof(de->block_obj_path), b->obj_path);
        dev_copy(de->crypto_backing_path, sizeof(de->crypto_backing_path),
                 backing ? backing->obj_path : (b->has_encrypted ? b->obj_path : ""));
        dev_copy(de->drive_obj_path, sizeof(de->drive_obj_path), eff_drive_path);
        dev_copy(de->device_node, sizeof(de->device_node),
                 b->device[0] ? b->device : b->obj_path);
        dev_copy(de->fs_type, sizeof(de->fs_type),
                 b->id_type[0] ? b->id_type : (b->has_encrypted ? "crypto_LUKS" : "filesystem"));

        const char *base_name = b->id_label[0] ? b->id_label :
                                (b->hint_name[0] ? b->hint_name :
                                 ((drv && drv->model[0]) ? drv->model : nullptr));
        if (!base_name || !base_name[0]) {
            const char *sl = strrchr(de->device_node, '/');
            base_name = (sl && sl[1]) ? (sl + 1) : "Volume";
        }

        if (b->has_encrypted && (!b->cleartext_device[0] || strcmp(b->cleartext_device, "/") == 0)) {
            de->is_luks_locked = True;
            de->is_mounted = False;
            de->can_mount = True;
            de->can_unmount = False;
            de->can_eject = (drv && (drv->ejectable || drv->can_power_off || drv->removable)) ? True : False;
            snprintf(de->shelf_token, sizeof(de->shelf_token), "udisks://%s", b->obj_path);
            snprintf(de->display_label, sizeof(de->display_label), "[Locked] %s", base_name);
        } else if (b->mount_point[0] == '/') {
            de->is_luks_locked = False;
            de->is_mounted = True;
            de->can_mount = False;
            de->can_unmount = True;
            de->can_eject = ((drv && (drv->ejectable || drv->can_power_off || drv->removable)) ||
                             de->crypto_backing_path[0] != '\0') ? True : False;
            dev_copy(de->mount_point, sizeof(de->mount_point), b->mount_point);
            dev_copy(de->shelf_token, sizeof(de->shelf_token), b->mount_point);
            snprintf(de->display_label, sizeof(de->display_label), "%s", base_name);
        } else {
            de->is_luks_locked = False;
            de->is_mounted = False;
            de->can_mount = True;
            de->can_unmount = False;
            de->can_eject = ((drv && (drv->ejectable || drv->can_power_off || drv->removable)) ||
                             de->crypto_backing_path[0] != '\0') ? True : False;
            snprintf(de->shelf_token, sizeof(de->shelf_token), "udisks://%s", b->obj_path);
            snprintf(de->display_label, sizeof(de->display_label), "[Drive] %s", base_name);
        }
    }

    free(drives);
    free(blocks);
}

/* ---- GVFS /run/user/$UID/gvfs FUSE mount & MTP phone discovery ---- */

static void extract_gvfs_kv(const char *entry_name, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = entry_name;
    while ((p = strstr(p, key)) != nullptr) {
        if ((p == entry_name || p[-1] == ':' || p[-1] == ',') && p[klen] == '=') {
            const char *val = p + klen + 1;
            const char *end = strchr(val, ',');
            size_t vlen = end ? (size_t)(end - val) : strlen(val);
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return;
        }
        p += klen;
    }
}

static void format_gvfs_entry_label(const char *d_name, char *label_out, size_t label_sz,
                                    char *fstype_out, size_t fs_sz, Boolean *is_mtp_out) {
    *is_mtp_out = False;
    char host[64], share[64];
    extract_gvfs_kv(d_name, "server", host, sizeof(host));
    if (!host[0])
        extract_gvfs_kv(d_name, "host", host, sizeof(host));
    extract_gvfs_kv(d_name, "share", share, sizeof(share));

    if (strncmp(d_name, "smb-share:", 10) == 0) {
        snprintf(fstype_out, fs_sz, "gvfs/smb");
        if (share[0] && host[0])
            snprintf(label_out, label_sz, "SMB: %s@%s", share, host);
        else
            snprintf(label_out, label_sz, "SMB: %s", host[0] ? host : d_name + 10);
    } else if (strncmp(d_name, "sftp:", 5) == 0) {
        snprintf(fstype_out, fs_sz, "gvfs/sftp");
        snprintf(label_out, label_sz, "SFTP: %s", host[0] ? host : d_name + 5);
    } else if (strncmp(d_name, "davs:", 5) == 0 || strncmp(d_name, "dav:", 4) == 0) {
        snprintf(fstype_out, fs_sz, "gvfs/webdav");
        snprintf(label_out, label_sz, "WebDAV: %s", host[0] ? host : d_name);
    } else if (strncmp(d_name, "nfs:", 4) == 0) {
        snprintf(fstype_out, fs_sz, "gvfs/nfs");
        snprintf(label_out, label_sz, "NFS: %s", host[0] ? host : d_name + 4);
    } else if (strncmp(d_name, "mtp:", 4) == 0 || strncmp(d_name, "gphoto2:", 8) == 0) {
        *is_mtp_out = True;
        snprintf(fstype_out, fs_sz, "gvfs/mtp");
        snprintf(label_out, label_sz, "Phone: %s", host[0] ? host : d_name);
    } else {
        snprintf(fstype_out, fs_sz, "gvfs/fuse");
        snprintf(label_out, label_sz, "%s", d_name);
    }
}

static void scan_gvfs_mounts(void) {
    char gvfs_root[PATH_MAX];
    get_gvfs_root_dir(gvfs_root, sizeof(gvfs_root));

    DIR *d = opendir(gvfs_root);
    if (!d)
        return;

    struct dirent *de;
    while ((de = readdir(d)) != nullptr && g_device_count < MAX_DEVICES) {
        if (de->d_name[0] == '.')
            continue;
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", gvfs_root, de->d_name) >= (int)sizeof(full))
            continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        DtfileDeviceEntry *ent = &g_devices[g_device_count++];
        memset(ent, 0, sizeof(*ent));
        ent->is_gvfs = True;
        ent->is_mounted = True;
        ent->can_unmount = True;
        ent->can_eject = False;
        dev_copy(ent->mount_point, sizeof(ent->mount_point), full);
        dev_copy(ent->shelf_token, sizeof(ent->shelf_token), full);
        dev_copy(ent->device_node, sizeof(ent->device_node), de->d_name);
        format_gvfs_entry_label(de->d_name,
                                ent->display_label, sizeof(ent->display_label),
                                ent->fs_type, sizeof(ent->fs_type),
                                &ent->is_mtp);
    }
    closedir(d);
}

/* Synchronize discovered UDisks2 + GVFS items into Shelf slots and publish X11 state */
static void sync_devices_to_shelf_and_root(void) {
    ShelfInit();

    /* 1. Remove previously managed tokens that are no longer active */
    for (int m = 0; m < g_managed_token_count; ++m) {
        const char *old_tok = g_managed_tokens[m];
        bool still_present = false;
        for (int i = 0; i < g_device_count; ++i) {
            if (strcmp(g_devices[i].shelf_token, old_tok) == 0) {
                still_present = true;
                break;
            }
        }
        if (!still_present) {
            int sc = 0;
            const ShelfSlot *slots = ShelfGetSlots(&sc);
            for (int s = 0; s < sc; ++s) {
                if (slots[s].slot > 0 && slots[s].path &&
                    strcmp(slots[s].path, old_tok) == 0) {
                    ShelfClearSlotSkipIgnore(slots[s].slot);
                }
            }
        }
    }

    /* 2. Add any newly discovered device/share token unless ignored by user */
    g_managed_token_count = 0;
    for (int i = 0; i < g_device_count; ++i) {
        const char *tok = g_devices[i].shelf_token;
        if (!tok[0])
            continue;
        if (g_managed_token_count < MAX_DEVICES)
            dev_copy(g_managed_tokens[g_managed_token_count++], MAX_PATH, tok);

        if (ShelfIsIgnoredMount(tok))
            continue;

        bool exists = false;
        int sc = 0;
        const ShelfSlot *slots = ShelfGetSlots(&sc);
        for (int s = 0; s < sc; ++s) {
            if (slots[s].path && strcmp(slots[s].path, tok) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            int slot = ShelfNextEmptySlot();
            ShelfSetSlot(slot, tok);
        }
    }

    ShelfRefreshAllViews();
    DtfileXdgTrashSyncRefreshMounts();

    /* 3. Publish `_DT_DEVICES_STATE` on root window for deterministic Wayland/X11 observability */
    if (g_dev_toplevel) {
        Display *dpy = XtDisplay(g_dev_toplevel);
        if (dpy) {
            Window root = DefaultRootWindow(dpy);
            Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
            Atom prop = XInternAtom(dpy, "_DT_DEVICES_STATE", False);
            char buf[4096];
            int pos = snprintf(buf, sizeof(buf), "count=%d", g_device_count);
            for (int i = 0; i < g_device_count && pos + 8 < (int)sizeof(buf); ++i) {
                const char *state = g_devices[i].is_luks_locked ? "locked" :
                                    (g_devices[i].is_mounted ? "mounted" : "unmounted");
                int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\t%s|%s|%s|%s",
                                 g_devices[i].shelf_token,
                                 g_devices[i].display_label,
                                 g_devices[i].fs_type,
                                 state);
                if (n <= 0 || pos + n >= (int)sizeof(buf))
                    break;
                pos += n;
            }
            XChangeProperty(dpy, root, prop, utf8, 8, PropModeReplace,
                            (const unsigned char *)buf, pos);
            XFlush(dpy);
        }
    }
}

void DtfileDevicesRescan(void) {
    g_device_count = 0;
    scan_udisks_devices();
    scan_gvfs_mounts();
    sync_devices_to_shelf_and_root();
}

int DtfileDevicesGetCount(void) {
    return g_device_count;
}

Boolean DtfileDevicesGetEntry(int index, DtfileDeviceEntry *out_entry) {
    if (index < 0 || index >= g_device_count || !out_entry)
        return False;
    *out_entry = g_devices[index];
    return True;
}

Boolean DtfileDevicesLookupByPath(const char *path, DtfileDeviceEntry *out_entry) {
    if (!path || !path[0])
        return False;
    for (int i = 0; i < g_device_count; ++i) {
        if (strcmp(g_devices[i].shelf_token, path) == 0 ||
            (g_devices[i].mount_point[0] && strcmp(g_devices[i].mount_point, path) == 0)) {
            if (out_entry)
                *out_entry = g_devices[i];
            return True;
        }
    }
    return False;
}

Boolean DtfileDevicesDescribeShelfSlot(
    const char *path,
    char **label_out,
    const char **icon_fallback_out)
{
    if (label_out) *label_out = nullptr;
    if (icon_fallback_out) *icon_fallback_out = nullptr;

    DtfileDeviceEntry ent;
    if (!DtfileDevicesLookupByPath(path, &ent))
        return False;

    if (label_out)
        *label_out = XtNewString(ent.display_label);
    if (icon_fallback_out) {
        if (ent.is_luks_locked)
            *icon_fallback_out = "FpblnkS";
        else if (ent.is_gvfs && !ent.is_mtp)
            *icon_fallback_out = "Dtdirnw";
        else
            *icon_fallback_out = "DtdirB";
    }
    return True;
}

static void dev_post_error(Widget parent, const char *title_txt, const char *msg_txt) {
    Widget ref = parent ? parent : (g_dev_toplevel ? g_dev_toplevel : toplevel);
    char *t = XtNewString((char *)(title_txt ? title_txt : "Devices & Mounts Error"));
    char *m = XtNewString((char *)((msg_txt && msg_txt[0]) ? msg_txt : "Device operation failed."));
    _DtMessage(ref, t, m, nullptr, HelpRequestCB);
    XtFree(t);
    XtFree(m);
}

static void publish_dialog_visibility(const char *atom_name, const char *val) {
    if (!g_dev_toplevel)
        return;
    Display *dpy = XtDisplay(g_dev_toplevel);
    if (!dpy)
        return;
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom prop = XInternAtom(dpy, atom_name, False);
    XChangeProperty(dpy, DefaultRootWindow(dpy), prop, utf8, 8,
                    PropModeReplace, (const unsigned char *)val, (int)strlen(val));
    XFlush(dpy);
}

static void request_net_active_window(Widget shell) {
    if (!shell || !XtIsRealized(shell))
        return;
    Display *dpy = XtDisplay(shell);
    Window win = XtWindow(shell);
    if (!dpy || win == None)
        return;
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

/* ---- Asynchronous UDisks2 Mount / Unlock / Unmount / Eject ---- */

static void append_empty_asv_options(DBusMessage *msg) {
    DBusMessageIter it, dict;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&it, &dict);
}

typedef struct {
    Widget parent;
    FileMgrData *fmd;
    char obj_path[256];
} MountPendingCtx;

static void udisks_mount_reply_cb(DBusPendingCall *pending, void *user_data) {
    MountPendingCtx *ctx = (MountPendingCtx *)user_data;
    DBusMessage *reply = dbus_pending_call_steal_reply(pending);
    dbus_pending_call_unref(pending);

    if (!reply)
        return;

    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *ename = dbus_message_get_error_name(reply);
        const char *edetail = nullptr;
        dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &edetail, DBUS_TYPE_INVALID);
        dev_post_error(ctx ? ctx->parent : nullptr, "Mount Volume Error",
                       edetail ? edetail : (ename ? ename : "Failed mounting volume."));
        dbus_message_unref(reply);
        return;
    }

    const char *mount_path = nullptr;
    dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &mount_path, DBUS_TYPE_INVALID);
    char saved_mp[PATH_MAX];
    dev_copy(saved_mp, sizeof(saved_mp), mount_path);
    dbus_message_unref(reply);

    DtfileDevicesRescan();

    if (saved_mp[0] == '/' && ctx && ctx->fmd) {
        ShowNewDirectory(ctx->fmd,
                         ctx->fmd->host ? ctx->fmd->host : home_host_name,
                         saved_mp);
    }
}

static void start_udisks_filesystem_mount(Widget parent, FileMgrData *fmd, const char *block_obj_path) {
    if (!g_udisks_conn || !block_obj_path || block_obj_path[0] != '/')
        return;

    DBusMessage *msg = dbus_message_new_method_call(
        UDISKS_BUS_NAME, block_obj_path,
        "org.freedesktop.UDisks2.Filesystem", "Mount");
    if (!msg)
        return;
    append_empty_asv_options(msg);

    MountPendingCtx *ctx = (MountPendingCtx *)XtCalloc(1, sizeof(MountPendingCtx));
    ctx->parent = parent;
    ctx->fmd = fmd;
    dev_copy(ctx->obj_path, sizeof(ctx->obj_path), block_obj_path);

    DBusPendingCall *pending = nullptr;
    if (dbus_connection_send_with_reply(g_udisks_conn, msg, &pending, 60000) && pending) {
        dbus_pending_call_set_notify(pending, udisks_mount_reply_cb, ctx, (DBusFreeFunction)XtFree);
        dbus_connection_flush(g_udisks_conn);
    } else {
        XtFree((char *)ctx);
    }
    dbus_message_unref(msg);
}

/* ---- Motif LUKS Passphrase Unlock Dialog ---- */

struct LuksPendingCallCtx;

typedef struct {
    Widget shell;
    Widget pass_tf;
    Widget status_lbl;
    FileMgrData *fmd;
    char luks_obj_path[256];
    char secret[MAX_SECRET_LEN];
    size_t secret_len;
    bool modifying_mask;
    bool busy;
    struct LuksPendingCallCtx *active_pending;
} LuksUnlockCtx;

typedef struct LuksPendingCallCtx {
    LuksUnlockCtx *owner;
    FileMgrData *fmd;
} LuksPendingCallCtx;

static void luks_dlg_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    publish_dialog_visibility("_DT_LUKS_DIALOG_VISIBLE", "0");
    LuksUnlockCtx *ctx = (LuksUnlockCtx *)client_data;
    if (ctx) {
        if (ctx->active_pending)
            ctx->active_pending->owner = nullptr;
        secure_zero(ctx->secret, sizeof(ctx->secret));
        XtFree((char *)ctx);
    }
}

static void luks_map_eh(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)cont;
    LuksUnlockCtx *ctx = (LuksUnlockCtx *)client_data;
    if (ev && ev->type == MapNotify) {
        request_net_active_window(w);
        if (ctx && ctx->pass_tf)
            XmProcessTraversal(ctx->pass_tf, XmTRAVERSE_CURRENT);
        publish_dialog_visibility("_DT_LUKS_DIALOG_VISIBLE", "1");
    }
}

static void luks_mask_modify_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    LuksUnlockCtx *ctx = (LuksUnlockCtx *)client_data;
    XmTextVerifyCallbackStruct *cbs = (XmTextVerifyCallbackStruct *)call_data;
    if (!ctx || !cbs || ctx->modifying_mask)
        return;

    long start = cbs->startPos;
    long end = cbs->endPos;
    if (start < 0) start = 0;
    if (end < start) end = start;
    if ((size_t)start > ctx->secret_len) start = (long)ctx->secret_len;
    if ((size_t)end > ctx->secret_len) end = (long)ctx->secret_len;

    int ins_len = (cbs->text && cbs->text->ptr && cbs->text->length > 0) ? cbs->text->length : 0;
    size_t rem_len = (size_t)(end - start);
    if (ctx->secret_len - rem_len + (size_t)ins_len >= sizeof(ctx->secret)) {
        cbs->doit = False;
        return;
    }

    memmove(ctx->secret + start + ins_len,
            ctx->secret + end,
            ctx->secret_len - (size_t)end + 1);
    if (ins_len > 0) {
        memcpy(ctx->secret + start, cbs->text->ptr, (size_t)ins_len);
        for (int i = 0; i < ins_len; ++i)
            cbs->text->ptr[i] = '*';
    }
    ctx->secret_len = ctx->secret_len - rem_len + (size_t)ins_len;
    ctx->secret[ctx->secret_len] = '\0';
}

static void udisks_unlock_reply_cb(DBusPendingCall *pending, void *user_data) {
    LuksPendingCallCtx *pctx = (LuksPendingCallCtx *)user_data;
    LuksUnlockCtx *owner = pctx ? pctx->owner : nullptr;
    if (owner) {
        owner->active_pending = nullptr;
        owner->busy = false;
    }

    DBusMessage *reply = dbus_pending_call_steal_reply(pending);
    dbus_pending_call_unref(pending);
    if (!reply)
        return;

    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *edetail = nullptr;
        dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &edetail, DBUS_TYPE_INVALID);
        if (owner && owner->status_lbl) {
            XmString xm = XmStringCreateLocalized(
                (char *)(edetail ? edetail : "Incorrect passphrase or unlock denied."));
            XtVaSetValues(owner->status_lbl, XmNlabelString, xm, nullptr);
            XmStringFree(xm);
        } else {
            dev_post_error(nullptr, "Unlock Encrypted Volume Error",
                           edetail ? edetail : "Unlock failed.");
        }
        dbus_message_unref(reply);
        return;
    }

    const char *cleartext_obj = nullptr;
    dbus_message_get_args(reply, nullptr, DBUS_TYPE_OBJECT_PATH, &cleartext_obj, DBUS_TYPE_INVALID);
    char saved_ct[256];
    dev_copy(saved_ct, sizeof(saved_ct), cleartext_obj);
    dbus_message_unref(reply);

    if (owner && owner->shell)
        XtDestroyWidget(owner->shell);

    DtfileDevicesRescan();
    if (saved_ct[0] == '/' && strcmp(saved_ct, "/") != 0)
        start_udisks_filesystem_mount(nullptr, pctx ? pctx->fmd : nullptr, saved_ct);
}

static void submit_luks_unlock(
    LuksUnlockCtx *owner,
    FileMgrData *fmd,
    const char *luks_obj_path,
    char *passphrase_buf,
    size_t passphrase_cap)
{
    if (!g_udisks_conn || !luks_obj_path || luks_obj_path[0] != '/') {
        secure_zero(passphrase_buf, passphrase_cap);
        return;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        UDISKS_BUS_NAME, luks_obj_path,
        "org.freedesktop.UDisks2.Encrypted", "Unlock");
    if (!msg) {
        secure_zero(passphrase_buf, passphrase_cap);
        return;
    }

    const char *pass_ptr = passphrase_buf;
    DBusMessageIter it, dict;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &pass_ptr);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&it, &dict);

    /* Zero passphrase immediately after marshalling into DBusMessage */
    secure_zero(passphrase_buf, passphrase_cap);

    LuksPendingCallCtx *pctx = (LuksPendingCallCtx *)XtCalloc(1, sizeof(LuksPendingCallCtx));
    pctx->owner = owner;
    pctx->fmd = fmd;

    DBusPendingCall *pending = nullptr;
    if (dbus_connection_send_with_reply(g_udisks_conn, msg, &pending, 60000) && pending) {
        if (owner) {
            owner->busy = true;
            owner->active_pending = pctx;
        }
        dbus_pending_call_set_notify(pending, udisks_unlock_reply_cb, pctx, (DBusFreeFunction)XtFree);
        dbus_connection_flush(g_udisks_conn);
    } else {
        XtFree((char *)pctx);
    }
    dbus_message_unref(msg);
}

static void luks_ok_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    LuksUnlockCtx *ctx = (LuksUnlockCtx *)client_data;
    if (!ctx || ctx->busy) return;

    char pass_copy[MAX_SECRET_LEN];
    memcpy(pass_copy, ctx->secret, sizeof(pass_copy));
    ctx->modifying_mask = true;
    XmTextFieldSetString(ctx->pass_tf, "");
    ctx->modifying_mask = false;
    secure_zero(ctx->secret, sizeof(ctx->secret));
    ctx->secret_len = 0;

    submit_luks_unlock(ctx, ctx->fmd,
                       ctx->luks_obj_path, pass_copy, sizeof(pass_copy));
}

static void luks_cancel_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    LuksUnlockCtx *ctx = (LuksUnlockCtx *)client_data;
    if (ctx && ctx->shell)
        XtDestroyWidget(ctx->shell);
}

static void show_luks_unlock_dialog(Widget parent, FileMgrData *fmd, const DtfileDeviceEntry *ent) {
    /* Optional headless test-lane passphrase injection when explicitly requested */
    const char *test_pw = getenv("DTFILE_LUKS_TEST_PASSPHRASE");
    if (test_pw && test_pw[0]) {
        char pw_buf[MAX_SECRET_LEN];
        dev_copy(pw_buf, sizeof(pw_buf), test_pw);
        submit_luks_unlock(nullptr, fmd, ent->block_obj_path, pw_buf, sizeof(pw_buf));
        return;
    }

    Widget ref = parent ? parent : (g_dev_toplevel ? g_dev_toplevel : toplevel);
    LuksUnlockCtx *ctx = (LuksUnlockCtx *)XtCalloc(1, sizeof(LuksUnlockCtx));
    ctx->fmd = fmd;
    dev_copy(ctx->luks_obj_path, sizeof(ctx->luks_obj_path), ent->block_obj_path);

    Arg args[10];
    Cardinal n = 0;
    XtSetArg(args[n], XmNtitle, "Unlock Encrypted Volume (LUKS)"); n++;
    XtSetArg(args[n], XmNdeleteResponse, XmDESTROY); n++;
    XtSetArg(args[n], XmNallowShellResize, True); n++;
    ctx->shell = XmCreateDialogShell(ref, "luksUnlockShell", args, n);
    XtAddCallback(ctx->shell, XmNdestroyCallback, luks_dlg_destroy_cb, (XtPointer)ctx);

    n = 0;
    XtSetArg(args[n], XmNmarginWidth, 14); n++;
    XtSetArg(args[n], XmNmarginHeight, 12); n++;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    Widget form = XmCreateForm(ctx->shell, "luksUnlockForm", args, n);

    char prompt_txt[320];
    snprintf(prompt_txt, sizeof(prompt_txt),
             "Enter passphrase to unlock encrypted volume\n%s (%s):",
             ent->display_label, ent->device_node);
    XmString pxm = XmStringCreateLocalized(prompt_txt);
    Widget plbl = XtVaCreateManagedWidget(
        "luksPrompt", xmLabelWidgetClass, form,
        XmNlabelString, pxm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(pxm);

    ctx->pass_tf = XtVaCreateManagedWidget(
        "luksPassphraseText", xmTextFieldWidgetClass, form,
        XmNcolumns, 32,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, plbl,
        XmNtopOffset, 10,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XtAddCallback(ctx->pass_tf, XmNmodifyVerifyCallback, luks_mask_modify_cb, (XtPointer)ctx);

    XmString sxm = XmStringCreateLocalized("");
    ctx->status_lbl = XtVaCreateManagedWidget(
        "luksStatus", xmLabelWidgetClass, form,
        XmNlabelString, sxm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, ctx->pass_tf,
        XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(sxm);

    Widget sep = XtVaCreateManagedWidget(
        "luksSep", xmSeparatorWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, ctx->status_lbl,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    XmString ok_xm = XmStringCreateLocalized("Unlock");
    Widget ok_btn = XtVaCreateManagedWidget(
        "luksUnlockBtn", xmPushButtonWidgetClass, form,
        XmNlabelString, ok_xm,
        XmNwidth, 96,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, sep,
        XmNtopOffset, 10,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(ok_xm);
    XtAddCallback(ok_btn, XmNactivateCallback, luks_ok_cb, (XtPointer)ctx);

    XmString cancel_xm = XmStringCreateLocalized("Cancel");
    Widget cancel_btn = XtVaCreateManagedWidget(
        "luksCancelBtn", xmPushButtonWidgetClass, form,
        XmNlabelString, cancel_xm,
        XmNwidth, 96,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, sep,
        XmNtopOffset, 10,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(cancel_xm);
    XtAddCallback(cancel_btn, XmNactivateCallback, luks_cancel_cb, (XtPointer)ctx);

    XtVaSetValues(form, XmNdefaultButton, ok_btn, XmNcancelButton, cancel_btn, nullptr);
    XtAddEventHandler(ctx->shell, StructureNotifyMask, False, luks_map_eh, (XtPointer)ctx);
    XtManageChild(form);
    XmProcessTraversal(ctx->pass_tf, XmTRAVERSE_CURRENT);
}

Boolean DtfileDevicesActivatePath(
    FileMgrRec *file_mgr_rec,
    FileMgrData *file_mgr_data,
    const char *path)
{
    if (!path || !path[0])
        return False;

    DtfileDeviceEntry ent;
    if (DtfileDevicesLookupByPath(path, &ent)) {
        Widget shell = file_mgr_rec ? file_mgr_rec->shell : nullptr;
        if (ent.is_luks_locked) {
            show_luks_unlock_dialog(shell, file_mgr_data, &ent);
            return True;
        }
        if (ent.is_udisks && !ent.is_mounted) {
            start_udisks_filesystem_mount(shell, file_mgr_data, ent.block_obj_path);
            return True;
        }
        return False;
    }

    if (DtfileDevicesIsSyntheticPath(path)) {
        const char *obj_path = path + 8; /* keep leading '/' of /org/freedesktop/UDisks2/... */
        start_udisks_filesystem_mount(file_mgr_rec ? file_mgr_rec->shell : nullptr,
                                      file_mgr_data, obj_path);
        return True;
    }
    return False;
}

/* ---- Unmount / Lock / Eject (UDisks2 + GVFS `gio mount -u`) ---- */

typedef struct {
    int read_fd;
    pid_t pid;
    XtInputId input_id;
    Widget dlg_shell;
    Widget status_lbl;
    FileMgrData *fmd;
    bool navigate_on_mount;
    char prev_mounts[32][MAX_PATH];
    int prev_mount_count;
    char output_buf[1024];
    size_t output_len;
} GioJobCtx;

static void collect_current_gvfs_dirs(char out[32][MAX_PATH], int *count_out) {
    *count_out = 0;
    char gvfs_root[PATH_MAX];
    get_gvfs_root_dir(gvfs_root, sizeof(gvfs_root));
    DIR *d = opendir(gvfs_root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr && *count_out < 32) {
        if (de->d_name[0] == '.') continue;
        snprintf(out[(*count_out)++], MAX_PATH, "%s/%s", gvfs_root, de->d_name);
    }
    closedir(d);
}

static void gio_job_input_cb(XtPointer client_data, int *source, XtInputId *id) {
    (void)id;
    GioJobCtx *job = (GioJobCtx *)client_data;
    if (!job) return;

    char buf[256];
    ssize_t nr = read(*source, buf, sizeof(buf) - 1);
    if (nr > 0) {
        size_t avail = sizeof(job->output_buf) - 1 - job->output_len;
        if ((size_t)nr < avail) avail = (size_t)nr;
        if (avail > 0) {
            memcpy(job->output_buf + job->output_len, buf, avail);
            job->output_len += avail;
            job->output_buf[job->output_len] = '\0';
        }
        return;
    }
    if (nr < 0 && (errno == EINTR || errno == EAGAIN))
        return;

    /* EOF */
    XtRemoveInput(job->input_id);
    close(job->read_fd);

    int status = 0;
    waitpid(job->pid, &status, 0);
    bool ok = (WIFEXITED(status) && WEXITSTATUS(status) == 0);

    DtfileDevicesRescan();

    if (!ok) {
        if (job->status_lbl) {
            XmString xm = XmStringCreateLocalized(
                job->output_buf[0] ? job->output_buf : "Connection failed.");
            XtVaSetValues(job->status_lbl, XmNlabelString, xm, nullptr);
            XmStringFree(xm);
        } else {
            dev_post_error(nullptr, "Network Share Error",
                           job->output_buf[0] ? job->output_buf : "Mount/unmount command failed.");
        }
        XtFree((char *)job);
        return;
    }

    if (job->dlg_shell)
        XtDestroyWidget(job->dlg_shell);

    if (job->navigate_on_mount && job->fmd) {
        char cur_mounts[32][MAX_PATH];
        int cur_count = 0;
        collect_current_gvfs_dirs(cur_mounts, &cur_count);
        const char *chosen = nullptr;
        for (int i = 0; i < cur_count; ++i) {
            bool existed = false;
            for (int j = 0; j < job->prev_mount_count; ++j) {
                if (strcmp(cur_mounts[i], job->prev_mounts[j]) == 0) {
                    existed = true;
                    break;
                }
            }
            if (!existed) {
                chosen = cur_mounts[i];
                break;
            }
        }
        if (!chosen && cur_count > 0)
            chosen = cur_mounts[cur_count - 1];
        if (chosen) {
            ShowNewDirectory(job->fmd,
                             job->fmd->host ? job->fmd->host : home_host_name,
                             (char *)chosen);
        }
    }

    XtFree((char *)job);
}

static void spawn_gio_mount_cmd(
    const char *const argv[],
    const char *optional_secret,
    Widget dlg_shell,
    Widget status_lbl,
    FileMgrData *fmd,
    bool navigate_on_mount)
{
    if (!g_dev_toplevel)
        return;

    int out_pipe[2] = {-1, -1};
    int in_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0)
        return;
    if (pipe(in_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return;
    }

    GioJobCtx *job = (GioJobCtx *)XtCalloc(1, sizeof(GioJobCtx));
    job->dlg_shell = dlg_shell;
    job->status_lbl = status_lbl;
    job->fmd = fmd;
    job->navigate_on_mount = navigate_on_mount;
    if (navigate_on_mount)
        collect_current_gvfs_dirs(job->prev_mounts, &job->prev_mount_count);

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(in_pipe[0]);  close(in_pipe[1]);
        XtFree((char *)job);
        return;
    }
    if (pid == 0) {
        close(out_pipe[0]);
        close(in_pipe[1]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);
        execvp(argv[0], (char *const *)argv);
        const char *emsg = "Failed executing gio mount helper.\n";
        ssize_t wr = write(STDERR_FILENO, emsg, strlen(emsg));
        (void)wr;
        _exit(127);
    }

    close(out_pipe[1]);
    close(in_pipe[0]);
    if (optional_secret && optional_secret[0]) {
        ssize_t w1 = write(in_pipe[1], optional_secret, strlen(optional_secret));
        ssize_t w2 = write(in_pipe[1], "\n", 1);
        (void)w1;
        (void)w2;
    }
    close(in_pipe[1]);

    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    job->read_fd = out_pipe[0];
    job->pid = pid;
    job->input_id = XtAppAddInput(
        XtWidgetToApplicationContext(g_dev_toplevel),
        out_pipe[0], (XtPointer)XtInputReadMask,
        gio_job_input_cb, (XtPointer)job);
}

static void call_udisks_simple_sync(const char *obj_path, const char *iface, const char *method,
                                    char *err_buf, size_t err_sz) {
    if (err_buf && err_sz > 0) err_buf[0] = '\0';
    if (!g_udisks_conn || !obj_path || obj_path[0] != '/')
        return;
    DBusMessage *msg = dbus_message_new_method_call(UDISKS_BUS_NAME, obj_path, iface, method);
    if (!msg) return;
    append_empty_asv_options(msg);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(g_udisks_conn, msg, 10000, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) {
        if (err_buf && err_sz > 0)
            snprintf(err_buf, err_sz, "%s", err.message ? err.message : err.name);
        dbus_error_free(&err);
    }
    if (reply)
        dbus_message_unref(reply);
}

void DtfileDevicesUnmountOrEject(
    Widget parent,
    FileMgrData *file_mgr_data,
    const char *path,
    Boolean eject_drive)
{
    if (!path || !path[0])
        return;

    /* Move view back to $HOME if currently browsing inside the volume being unmounted */
    if (file_mgr_data && file_mgr_data->current_directory && path[0] == '/') {
        size_t plen = strlen(path);
        if (strncmp(file_mgr_data->current_directory, path, plen) == 0 &&
            (file_mgr_data->current_directory[plen] == '\0' ||
             file_mgr_data->current_directory[plen] == '/')) {
            ShowNewDirectory(file_mgr_data,
                             file_mgr_data->host ? file_mgr_data->host : home_host_name,
                             (char *)ShelfGetHomePath());
        }
    }

    DtfileDeviceEntry ent;
    bool found = DtfileDevicesLookupByPath(path, &ent) ? true : false;

    if ((found && ent.is_gvfs) || DtfileDevicesIsGvfsPath(path)) {
        const char *gio_bin = getenv("DTFILE_GIO_BIN");
        if (!gio_bin || !gio_bin[0])
            gio_bin = "gio";
        const char *argv[] = { gio_bin, "mount", "-u", path, nullptr };
        spawn_gio_mount_cmd(argv, nullptr, nullptr, nullptr, file_mgr_data, false);
        return;
    }

    if (found && ent.is_udisks) {
        char err_buf[256];
        err_buf[0] = '\0';
        if (ent.is_mounted && ent.block_obj_path[0] == '/') {
            call_udisks_simple_sync(ent.block_obj_path,
                                    "org.freedesktop.UDisks2.Filesystem", "Unmount",
                                    err_buf, sizeof(err_buf));
            if (err_buf[0]) {
                dev_post_error(parent, "Unmount Volume Error", err_buf);
                return;
            }
        }
        if (eject_drive) {
            if (ent.crypto_backing_path[0] == '/' && strcmp(ent.crypto_backing_path, "/") != 0 &&
                !ent.is_luks_locked) {
                call_udisks_simple_sync(ent.crypto_backing_path,
                                        "org.freedesktop.UDisks2.Encrypted", "Lock",
                                        nullptr, 0);
            }
            if (ent.drive_obj_path[0] == '/' && strcmp(ent.drive_obj_path, "/") != 0) {
                call_udisks_simple_sync(ent.drive_obj_path,
                                        "org.freedesktop.UDisks2.Drive", "Eject",
                                        nullptr, 0);
                call_udisks_simple_sync(ent.drive_obj_path,
                                        "org.freedesktop.UDisks2.Drive", "PowerOff",
                                        nullptr, 0);
            }
        }
        DtfileDevicesRescan();
    }
}

/* ---- File -> Connect to Server... Motif Dialog ---- */

typedef struct {
    Widget shell;
    Widget host_tf;
    Widget share_tf;
    Widget user_tf;
    Widget pass_tf;
    Widget uri_tf;
    Widget status_lbl;
    FileMgrData *fmd;
    const char *scheme;
    bool custom_uri_mode;
    bool syncing_uri;
    char secret[MAX_SECRET_LEN];
    size_t secret_len;
    bool modifying_mask;
} ConnectServerCtx;

static void conn_srv_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    publish_dialog_visibility("_DT_CONNECT_DIALOG_VISIBLE", "0");
    ConnectServerCtx *ctx = (ConnectServerCtx *)client_data;
    if (ctx) {
        secure_zero(ctx->secret, sizeof(ctx->secret));
        XtFree((char *)ctx);
    }
}

static void conn_srv_map_eh(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)cont;
    ConnectServerCtx *ctx = (ConnectServerCtx *)client_data;
    if (ev && ev->type == MapNotify) {
        request_net_active_window(w);
        if (ctx && ctx->uri_tf)
            XmProcessTraversal(ctx->uri_tf, XmTRAVERSE_CURRENT);
        publish_dialog_visibility("_DT_CONNECT_DIALOG_VISIBLE", "1");
    }
}

static void conn_srv_rebuild_uri(ConnectServerCtx *ctx) {
    if (!ctx || ctx->custom_uri_mode || ctx->syncing_uri)
        return;
    char *host = XmTextFieldGetString(ctx->host_tf);
    char *share = XmTextFieldGetString(ctx->share_tf);
    char *user = XmTextFieldGetString(ctx->user_tf);

    const char *h = (host && host[0]) ? host : "server.local";
    const char *s = (share && share[0]) ? share : "";
    while (*s == '/') s++;

    char uri[1024];
    if (user && user[0]) {
        if (s[0])
            snprintf(uri, sizeof(uri), "%s://%s@%s/%s", ctx->scheme, user, h, s);
        else
            snprintf(uri, sizeof(uri), "%s://%s@%s/", ctx->scheme, user, h);
    } else {
        if (s[0])
            snprintf(uri, sizeof(uri), "%s://%s/%s", ctx->scheme, h, s);
        else
            snprintf(uri, sizeof(uri), "%s://%s/", ctx->scheme, h);
    }

    ctx->syncing_uri = true;
    XmTextFieldSetString(ctx->uri_tf, uri);
    ctx->syncing_uri = false;

    XtFree(host);
    XtFree(share);
    XtFree(user);
}

static void conn_srv_field_changed_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    conn_srv_rebuild_uri((ConnectServerCtx *)client_data);
}

static void conn_srv_proto_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)call_data;
    ConnectServerCtx *ctx = (ConnectServerCtx *)client_data;
    if (!ctx) return;
    XtPointer ud = nullptr;
    XtVaGetValues(w, XmNuserData, &ud, nullptr);
    const char *proto = (const char *)ud;
    if (!proto || strcmp(proto, "custom") == 0) {
        ctx->custom_uri_mode = true;
    } else {
        ctx->custom_uri_mode = false;
        ctx->scheme = proto;
        conn_srv_rebuild_uri(ctx);
    }
}

static void conn_srv_mask_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    ConnectServerCtx *ctx = (ConnectServerCtx *)client_data;
    XmTextVerifyCallbackStruct *cbs = (XmTextVerifyCallbackStruct *)call_data;
    if (!ctx || !cbs || ctx->modifying_mask)
        return;

    long start = cbs->startPos < 0 ? 0 : cbs->startPos;
    long end = cbs->endPos < start ? start : cbs->endPos;
    if ((size_t)start > ctx->secret_len) start = (long)ctx->secret_len;
    if ((size_t)end > ctx->secret_len) end = (long)ctx->secret_len;
    int ins_len = (cbs->text && cbs->text->ptr && cbs->text->length > 0) ? cbs->text->length : 0;
    size_t rem_len = (size_t)(end - start);
    if (ctx->secret_len - rem_len + (size_t)ins_len >= sizeof(ctx->secret)) {
        cbs->doit = False;
        return;
    }
    memmove(ctx->secret + start + ins_len, ctx->secret + end, ctx->secret_len - (size_t)end + 1);
    if (ins_len > 0) {
        memcpy(ctx->secret + start, cbs->text->ptr, (size_t)ins_len);
        for (int i = 0; i < ins_len; ++i)
            cbs->text->ptr[i] = '*';
    }
    ctx->secret_len = ctx->secret_len - rem_len + (size_t)ins_len;
    ctx->secret[ctx->secret_len] = '\0';
}

static bool is_supported_remote_uri(const char *uri) {
    if (!uri || !uri[0])
        return false;
    return (strncasecmp(uri, "smb://", 6) == 0 ||
            strncasecmp(uri, "sftp://", 7) == 0 ||
            strncasecmp(uri, "ssh://", 6) == 0 ||
            strncasecmp(uri, "davs://", 7) == 0 ||
            strncasecmp(uri, "dav://", 6) == 0 ||
            strncasecmp(uri, "nfs://", 6) == 0 ||
            strncasecmp(uri, "ftp://", 6) == 0 ||
            strncasecmp(uri, "mtp://", 6) == 0);
}

static void conn_srv_connect_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    ConnectServerCtx *ctx = (ConnectServerCtx *)client_data;
    if (!ctx) return;

    char *uri = XmTextFieldGetString(ctx->uri_tf);
    if (!is_supported_remote_uri(uri)) {
        XmString xm = XmStringCreateLocalized(
            "Enter a valid smb://, sftp://, davs://, nfs://, or mtp:// URI.");
        XtVaSetValues(ctx->status_lbl, XmNlabelString, xm, nullptr);
        XmStringFree(xm);
        XtFree(uri);
        return;
    }

    XmString busy_xm = XmStringCreateLocalized("Connecting to server via GVFS...");
    XtVaSetValues(ctx->status_lbl, XmNlabelString, busy_xm, nullptr);
    XmStringFree(busy_xm);

    const char *gio_bin = getenv("DTFILE_GIO_BIN");
    if (!gio_bin || !gio_bin[0])
        gio_bin = "gio";

    char secret_copy[MAX_SECRET_LEN];
    memcpy(secret_copy, ctx->secret, sizeof(secret_copy));
    ctx->modifying_mask = true;
    XmTextFieldSetString(ctx->pass_tf, "");
    ctx->modifying_mask = false;
    secure_zero(ctx->secret, sizeof(ctx->secret));
    ctx->secret_len = 0;

    const char *argv[] = { gio_bin, "mount", uri, nullptr };
    spawn_gio_mount_cmd(argv, secret_copy[0] ? secret_copy : nullptr,
                        ctx->shell, ctx->status_lbl, ctx->fmd, true);
    secure_zero(secret_copy, sizeof(secret_copy));
    XtFree(uri);
}

static void conn_srv_cancel_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    ConnectServerCtx *ctx = (ConnectServerCtx *)client_data;
    if (ctx && ctx->shell)
        XtDestroyWidget(ctx->shell);
}

void ShowConnectServerDialog(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;

    FileMgrRec *fmr = nullptr;
    FileMgrData *fmd = nullptr;
    if (w) {
        Widget mbar = XmGetPostedFromWidget(XtParent(w));
        if (mbar) {
            Arg a[1];
            XtSetArg(a[0], XmNuserData, &fmr);
            XtGetValues(mbar, a, 1);
        }
    }
    if (fmr) {
        DialogData *dd = _DtGetInstanceData((XtPointer)fmr);
        if (dd) fmd = (FileMgrData *)dd->data;
    }

    /* Headless automation fast-path when DTFILE_CONNECT_URI_AUTO is set */
    const char *auto_uri = getenv("DTFILE_CONNECT_URI_AUTO");
    if (auto_uri && is_supported_remote_uri(auto_uri)) {
        const char *gio_bin = getenv("DTFILE_GIO_BIN");
        if (!gio_bin || !gio_bin[0]) gio_bin = "gio";
        const char *argv[] = { gio_bin, "mount", auto_uri, nullptr };
        spawn_gio_mount_cmd(argv, nullptr, nullptr, nullptr, fmd, true);
        return;
    }

    Widget parent = (fmr && fmr->shell) ? fmr->shell : (g_dev_toplevel ? g_dev_toplevel : toplevel);
    ConnectServerCtx *ctx = (ConnectServerCtx *)XtCalloc(1, sizeof(ConnectServerCtx));
    ctx->fmd = fmd;
    ctx->scheme = "smb";

    Arg args[10];
    Cardinal n = 0;
    XtSetArg(args[n], XmNtitle, "Connect to Remote Server (GVFS)"); n++;
    XtSetArg(args[n], XmNdeleteResponse, XmDESTROY); n++;
    XtSetArg(args[n], XmNallowShellResize, True); n++;
    ctx->shell = XmCreateDialogShell(parent, "connectServerShell", args, n);
    XtAddCallback(ctx->shell, XmNdestroyCallback, conn_srv_destroy_cb, (XtPointer)ctx);

    n = 0;
    XtSetArg(args[n], XmNmarginWidth, 14); n++;
    XtSetArg(args[n], XmNmarginHeight, 12); n++;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    Widget form = XmCreateForm(ctx->shell, "connectServerForm", args, n);

    Widget pulldown = XmCreatePulldownMenu(form, "protoPulldown", nullptr, 0);
    static const struct { const char *label; const char *scheme; } protos[] = {
        { "SMB / Windows Share (smb://)", "smb" },
        { "SSH / SFTP (sftp://)",         "sftp" },
        { "WebDAV HTTPS (davs://)",       "davs" },
        { "NFS Share (nfs://)",           "nfs" },
        { "Custom Server URI",            "custom" },
    };
    Widget first_item = nullptr;
    for (size_t i = 0; i < sizeof(protos) / sizeof(protos[0]); ++i) {
        XmString pxm = XmStringCreateLocalized((char *)protos[i].label);
        Widget item = XtVaCreateManagedWidget(
            "protoItem", xmPushButtonGadgetClass, pulldown,
            XmNlabelString, pxm,
            XmNuserData, (XtPointer)protos[i].scheme,
            nullptr);
        XmStringFree(pxm);
        XtAddCallback(item, XmNactivateCallback, conn_srv_proto_cb, (XtPointer)ctx);
        if (i == 0) first_item = item;
    }

    XmString pr_lbl = XmStringCreateLocalized("Protocol:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, pr_lbl); n++;
    XtSetArg(args[n], XmNsubMenuId, pulldown); n++;
    XtSetArg(args[n], XmNmenuHistory, first_item); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    Widget proto_menu = XmCreateOptionMenu(form, "protocolOptionMenu", args, n);
    XmStringFree(pr_lbl);
    XtManageChild(proto_menu);

    XmString h_xm = XmStringCreateLocalized("Server / Host:");
    Widget h_lbl = XtVaCreateManagedWidget(
        "hostLabel", xmLabelWidgetClass, form,
        XmNlabelString, h_xm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, proto_menu, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(h_xm);

    ctx->host_tf = XtVaCreateManagedWidget(
        "serverHostText", xmTextFieldWidgetClass, form,
        XmNcolumns, 34,
        XmNvalue, "files.local",
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, h_lbl, XmNtopOffset, 3,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XtAddCallback(ctx->host_tf, XmNvalueChangedCallback, conn_srv_field_changed_cb, (XtPointer)ctx);

    XmString sh_xm = XmStringCreateLocalized("Share / Remote Folder:");
    Widget sh_lbl = XtVaCreateManagedWidget(
        "shareLabel", xmLabelWidgetClass, form,
        XmNlabelString, sh_xm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, ctx->host_tf, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(sh_xm);

    ctx->share_tf = XtVaCreateManagedWidget(
        "serverShareText", xmTextFieldWidgetClass, form,
        XmNcolumns, 34,
        XmNvalue, "shared",
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, sh_lbl, XmNtopOffset, 3,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XtAddCallback(ctx->share_tf, XmNvalueChangedCallback, conn_srv_field_changed_cb, (XtPointer)ctx);

    XmString u_xm = XmStringCreateLocalized("Username (optional):");
    Widget u_lbl = XtVaCreateManagedWidget(
        "userLabel", xmLabelWidgetClass, form,
        XmNlabelString, u_xm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, ctx->share_tf, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(u_xm);

    ctx->user_tf = XtVaCreateManagedWidget(
        "serverUserText", xmTextFieldWidgetClass, form,
        XmNcolumns, 18,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, u_lbl, XmNtopOffset, 3,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 48,
        nullptr);
    XtAddCallback(ctx->user_tf, XmNvalueChangedCallback, conn_srv_field_changed_cb, (XtPointer)ctx);

    XmString pw_xm = XmStringCreateLocalized("Password (optional):");
    Widget pw_lbl = XtVaCreateManagedWidget(
        "passLabel", xmLabelWidgetClass, form,
        XmNlabelString, pw_xm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, ctx->share_tf, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 52,
        nullptr);
    XmStringFree(pw_xm);

    ctx->pass_tf = XtVaCreateManagedWidget(
        "serverPassText", xmTextFieldWidgetClass, form,
        XmNcolumns, 18,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pw_lbl, XmNtopOffset, 3,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 52,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XtAddCallback(ctx->pass_tf, XmNmodifyVerifyCallback, conn_srv_mask_cb, (XtPointer)ctx);

    XmString uri_xm = XmStringCreateLocalized("Server URI:");
    Widget uri_lbl = XtVaCreateManagedWidget(
        "uriLabel", xmLabelWidgetClass, form,
        XmNlabelString, uri_xm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, ctx->user_tf, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(uri_xm);

    ctx->uri_tf = XtVaCreateManagedWidget(
        "serverUriText", xmTextFieldWidgetClass, form,
        XmNcolumns, 38,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, uri_lbl, XmNtopOffset, 3,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    XmString st_xm = XmStringCreateLocalized("Mounts under /run/user/$UID/gvfs for all CDE apps.");
    ctx->status_lbl = XtVaCreateManagedWidget(
        "connectStatusLabel", xmLabelWidgetClass, form,
        XmNlabelString, st_xm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, ctx->uri_tf, XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(st_xm);

    Widget sep = XtVaCreateManagedWidget(
        "connectSep", xmSeparatorWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, ctx->status_lbl, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    XmString ok_xm = XmStringCreateLocalized("Connect");
    Widget ok_btn = XtVaCreateManagedWidget(
        "connectBtn", xmPushButtonWidgetClass, form,
        XmNlabelString, ok_xm,
        XmNwidth, 100,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, sep, XmNtopOffset, 10,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(ok_xm);
    XtAddCallback(ok_btn, XmNactivateCallback, conn_srv_connect_cb, (XtPointer)ctx);

    XmString cancel_xm = XmStringCreateLocalized("Cancel");
    Widget cancel_btn = XtVaCreateManagedWidget(
        "connectCancelBtn", xmPushButtonWidgetClass, form,
        XmNlabelString, cancel_xm,
        XmNwidth, 100,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, sep, XmNtopOffset, 10,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(cancel_xm);
    XtAddCallback(cancel_btn, XmNactivateCallback, conn_srv_cancel_cb, (XtPointer)ctx);

    conn_srv_rebuild_uri(ctx);
    XtVaSetValues(form, XmNdefaultButton, ok_btn, XmNcancelButton, cancel_btn, nullptr);
    XtAddEventHandler(ctx->shell, StructureNotifyMask, False, conn_srv_map_eh, (XtPointer)ctx);
    XtManageChild(form);
    XmProcessTraversal(ctx->uri_tf, XmTRAVERSE_CURRENT);
}

/* ---- UDisks2 Signal Filter + GVFS inotify Event Loop Integration ---- */

static void udisks_rescan_debounce_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data;
    (void)id;
    g_udisks_rescan_debounce = 0;
    DtfileDevicesRescan();
}

static DBusHandlerResult udisks_signal_filter(
    DBusConnection *conn,
    DBusMessage *msg,
    void *user_data)
{
    (void)conn;
    (void)user_data;
    if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL) {
        const char *path = dbus_message_get_path(msg);
        if (path && strncmp(path, "/org/freedesktop/UDisks2", 24) == 0) {
            if (dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded") ||
                dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved") ||
                dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
                if (g_dev_toplevel && g_udisks_rescan_debounce == 0) {
                    g_udisks_rescan_debounce = XtAppAddTimeOut(
                        XtWidgetToApplicationContext(g_dev_toplevel),
                        20, udisks_rescan_debounce_cb, nullptr);
                }
            }
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void pump_udisks_dbus(void) {
    if (!g_udisks_conn)
        return;
    dbus_connection_read_write(g_udisks_conn, 0);
    while (dbus_connection_dispatch(g_udisks_conn) == DBUS_DISPATCH_DATA_REMAINS) {
    }
}

static void udisks_xt_input_cb(XtPointer client_data, int *source, XtInputId *id) {
    (void)client_data;
    (void)source;
    (void)id;
    pump_udisks_dbus();
}

static void udisks_timer_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data;
    (void)id;
    g_udisks_timer = 0;
    if (!g_udisks_conn || !g_dev_toplevel)
        return;
    pump_udisks_dbus();
    g_udisks_timer = XtAppAddTimeOut(XtWidgetToApplicationContext(g_dev_toplevel),
                                     350, udisks_timer_cb, nullptr);
}

static void gvfs_debounce_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data;
    (void)id;
    g_gvfs_debounce = 0;
    DtfileDevicesRescan();
}

static void gvfs_inotify_cb(XtPointer client_data, int *source, XtInputId *id) {
    (void)client_data;
    (void)id;
    char buf[2048] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (read(*source, buf, sizeof(buf)) > 0) {
    }
    if (g_dev_toplevel && g_gvfs_debounce == 0) {
        g_gvfs_debounce = XtAppAddTimeOut(
            XtWidgetToApplicationContext(g_dev_toplevel),
            120, gvfs_debounce_cb, nullptr);
    }
}

void DtfileDevicesInit(Widget toplevel_widget) {
    if (!toplevel_widget || g_dev_toplevel)
        return;
    g_dev_toplevel = toplevel_widget;

    /* 1. Connect to UDisks2 (system bus by default; session bus when DTFILE_UDISKS_BUS=session) */
    const char *bus_env = getenv("DTFILE_UDISKS_BUS");
    DBusBusType bus_type = (bus_env && strcmp(bus_env, "session") == 0)
                               ? DBUS_BUS_SESSION
                               : DBUS_BUS_SYSTEM;

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get_private(bus_type, &err);
    if (!dbus_error_is_set(&err) && conn) {
        dbus_connection_set_exit_on_disconnect(conn, FALSE);
        dbus_bus_add_match(conn,
            "type='signal',interface='org.freedesktop.DBus.ObjectManager',"
            "path_namespace='/org/freedesktop/UDisks2'", &err);
        dbus_error_free(&err);
        dbus_error_init(&err);
        dbus_bus_add_match(conn,
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "path_namespace='/org/freedesktop/UDisks2'", &err);
        dbus_error_free(&err);
        dbus_connection_add_filter(conn, udisks_signal_filter, nullptr, nullptr);
        dbus_connection_flush(conn);

        g_udisks_conn = conn;
        int fd = -1;
        if (dbus_connection_get_unix_fd(conn, &fd) && fd >= 0) {
            g_udisks_input = XtAppAddInput(
                XtWidgetToApplicationContext(toplevel_widget),
                fd, (XtPointer)XtInputReadMask,
                udisks_xt_input_cb, nullptr);
        }
        g_udisks_timer = XtAppAddTimeOut(
            XtWidgetToApplicationContext(toplevel_widget),
            350, udisks_timer_cb, nullptr);
    } else {
        dbus_error_free(&err);
    }

    /* 2. Watch GVFS FUSE mount directory (/run/user/$UID/gvfs or DTFILE_GVFS_ROOT) */
    char gvfs_root[PATH_MAX];
    get_gvfs_root_dir(gvfs_root, sizeof(gvfs_root));
    if (getenv("DTFILE_GVFS_ROOT") != nullptr)
        mkdir(gvfs_root, 0700);

    g_gvfs_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_gvfs_inotify_fd >= 0) {
        g_gvfs_watch_wd = inotify_add_watch(
            g_gvfs_inotify_fd, gvfs_root,
            IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
        g_gvfs_input = XtAppAddInput(
            XtWidgetToApplicationContext(toplevel_widget),
            g_gvfs_inotify_fd, (XtPointer)XtInputReadMask,
            gvfs_inotify_cb, nullptr);
    }

    DtfileDevicesRescan();
}

void DtfileDevicesShutdown(void) {
    if (g_udisks_rescan_debounce != 0) {
        XtRemoveTimeOut(g_udisks_rescan_debounce);
        g_udisks_rescan_debounce = 0;
    }
    if (g_udisks_timer != 0) {
        XtRemoveTimeOut(g_udisks_timer);
        g_udisks_timer = 0;
    }
    if (g_udisks_input != 0) {
        XtRemoveInput(g_udisks_input);
        g_udisks_input = 0;
    }
    if (g_udisks_conn) {
        dbus_connection_close(g_udisks_conn);
        dbus_connection_unref(g_udisks_conn);
        g_udisks_conn = nullptr;
    }
    if (g_gvfs_debounce != 0) {
        XtRemoveTimeOut(g_gvfs_debounce);
        g_gvfs_debounce = 0;
    }
    if (g_gvfs_input != 0) {
        XtRemoveInput(g_gvfs_input);
        g_gvfs_input = 0;
    }
    if (g_gvfs_inotify_fd >= 0) {
        if (g_gvfs_watch_wd >= 0)
            inotify_rm_watch(g_gvfs_inotify_fd, g_gvfs_watch_wd);
        close(g_gvfs_inotify_fd);
        g_gvfs_inotify_fd = -1;
        g_gvfs_watch_wd = -1;
    }
    g_dev_toplevel = nullptr;
}
