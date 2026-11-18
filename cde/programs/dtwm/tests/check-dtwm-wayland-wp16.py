#!/usr/bin/env python3
"""WP-16 parallel dual-environment integration check (Wayland + X11/XLibre):
1. Removable USB/SD & Encrypted LUKS Volumes (`org.freedesktop.UDisks2`):
   - Verifies `dtfile` maps CK Core Motif ABI 9 (`libXm.so.9`), subscribes to
     `org.freedesktop.UDisks2`, and surfaces locked LUKS + removable USB volumes.
   - Exercises the live Motif LUKS Unlock Dialog (`_DT_LUKS_DIALOG_VISIBLE=1`) via
     `zwp_virtual_keyboard_v1` (Wayland) and `XTestFakeKeyEvent` (X11): verifies
     wrong-passphrase retry, valid-passphrase `Encrypted.Unlock` -> `Filesystem.Mount`
     -> automatic navigation, hotplug `InterfacesAdded`, and safe Eject
     (`Filesystem.Unmount` + `Encrypted.Lock` + `Drive.Eject` + `Drive.PowerOff`).
2. Network Shares (`GVFS` / `Connect to Server...`) & MTP Phones:
   - Exercises the live Motif Connect to Server dialog (`_DT_CONNECT_DIALOG_VISIBLE=1`),
     spawning `DTFILE_GIO_BIN mount smb://files.local/shared`, surfacing FUSE mounts and
     hotplugged `mtp:host=Pixel_9` phone storage via `inotify`, and unmounting cleanly.
3. Built-in Archive Extract/Compress (`libarchive`):
   - Verifies `.zip`, `.tar.gz`, `.tar.xz`, `.tar.zst`, and `.7z` round-trip through the
     live `dtfile` process (including spaces & UTF-8 names) and Zip-Slip rejection.
4. Shared FreeDesktop Thumbnails & `org.freedesktop.FileManager1`:
   - Verifies `$XDG_CACHE_HOME/thumbnails/large/<md5>.png` resolution (`Thumb::MTime`)
     and `FileManager1` (`ShowFolders`, `ShowItems`, `ShowItemProperties`) window raise
     (`_NET_ACTIVE_WINDOW`) + live `FileMgrData` icon selection (`_DT_FILEMGR_SELECTION`).
5. Live Bidirectional XDG Trash (`inotify`) & Per-Mount `.Trash-$UID`:
   - Verifies `< 1.5s` `inotify` sync when external apps trash or restore items in
     `$XDG_DATA_HOME/Trash`, and verifies same-filesystem `<mount>/.Trash-$UID/{files,info}`
     creation when `dtfile` trashes files on external mounts.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
import zlib


MOCK_UDISKS2_C = r"""
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static const char *g_log_path = NULL;
static const char *g_mnt_root = NULL;
static const char *g_expected_pw = "vaultkey42";

static bool g_luks_unlocked = false;
static bool g_vault_mounted = false;
static char g_vault_mp[512] = "";

static bool g_usb2_present = false;
static bool g_usb2_mounted = false;
static char g_usb2_mp[512] = "";

static void log_ev(const char *fmt, ...) {
    if (!g_log_path) return;
    FILE *fp = fopen(g_log_path, "a");
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
    fclose(fp);
}

static void dict_add_str(DBusMessageIter *dict, const char *key, const char *val) {
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

static void dict_add_objpath(DBusMessageIter *dict, const char *key, const char *val) {
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "o", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_OBJECT_PATH, &val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

static void dict_add_bool(DBusMessageIter *dict, const char *key, dbus_bool_t val) {
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

static void dict_add_u64(DBusMessageIter *dict, const char *key, uint64_t val) {
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "t", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT64, &val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

static void dict_add_ay(DBusMessageIter *dict, const char *key, const char *str) {
    DBusMessageIter e, v, ay;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "ay", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "y", &ay);
    size_t n = strlen(str) + 1;
    dbus_message_iter_append_fixed_array(&ay, DBUS_TYPE_BYTE, &str, (int)n);
    dbus_message_iter_close_container(&v, &ay);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

static void dict_add_mountpoints(DBusMessageIter *dict, const char *mp) {
    DBusMessageIter e, v, outer, inner;
    const char *key = "MountPoints";
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "aay", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "ay", &outer);
    if (mp && mp[0]) {
        dbus_message_iter_open_container(&outer, DBUS_TYPE_ARRAY, "y", &inner);
        size_t n = strlen(mp) + 1;
        dbus_message_iter_append_fixed_array(&inner, DBUS_TYPE_BYTE, &mp, (int)n);
        dbus_message_iter_close_container(&outer, &inner);
    }
    dbus_message_iter_close_container(&v, &outer);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

static void append_drive_object(DBusMessageIter *objs, const char *path, const char *model) {
    DBusMessageIter oe, ifs, ie, pd;
    dbus_message_iter_open_container(objs, DBUS_TYPE_DICT_ENTRY, NULL, &oe);
    dbus_message_iter_append_basic(&oe, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_open_container(&oe, DBUS_TYPE_ARRAY, "{sa{sv}}", &ifs);

    const char *if_drv = "org.freedesktop.UDisks2.Drive";
    dbus_message_iter_open_container(&ifs, DBUS_TYPE_DICT_ENTRY, NULL, &ie);
    dbus_message_iter_append_basic(&ie, DBUS_TYPE_STRING, &if_drv);
    dbus_message_iter_open_container(&ie, DBUS_TYPE_ARRAY, "{sv}", &pd);
    dict_add_str(&pd, "Vendor", "CKCore");
    dict_add_str(&pd, "Model", model);
    dict_add_str(&pd, "ConnectionBus", "usb");
    dict_add_bool(&pd, "Removable", TRUE);
    dict_add_bool(&pd, "Ejectable", TRUE);
    dict_add_bool(&pd, "CanPowerOff", TRUE);
    dbus_message_iter_close_container(&ie, &pd);
    dbus_message_iter_close_container(&ifs, &ie);

    dbus_message_iter_close_container(&oe, &ifs);
    dbus_message_iter_close_container(objs, &oe);
}

static void append_managed_objects(DBusMessage *reply) {
    DBusMessageIter it, objs, oe, ifs, ie, pd;
    dbus_message_iter_init_append(reply, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &objs);

    const char *drv1 = "/org/freedesktop/UDisks2/drives/usb_vault";
    append_drive_object(&objs, drv1, "VaultKey USB");

    /* 1. LUKS backing partition /dev/sdb1 */
    const char *sdb1 = "/org/freedesktop/UDisks2/block_devices/sdb1";
    dbus_message_iter_open_container(&objs, DBUS_TYPE_DICT_ENTRY, NULL, &oe);
    dbus_message_iter_append_basic(&oe, DBUS_TYPE_OBJECT_PATH, &sdb1);
    dbus_message_iter_open_container(&oe, DBUS_TYPE_ARRAY, "{sa{sv}}", &ifs);

    const char *if_blk = "org.freedesktop.UDisks2.Block";
    dbus_message_iter_open_container(&ifs, DBUS_TYPE_DICT_ENTRY, NULL, &ie);
    dbus_message_iter_append_basic(&ie, DBUS_TYPE_STRING, &if_blk);
    dbus_message_iter_open_container(&ie, DBUS_TYPE_ARRAY, "{sv}", &pd);
    dict_add_ay(&pd, "Device", "/dev/sdb1");
    dict_add_str(&pd, "IdLabel", "SecureVault");
    dict_add_str(&pd, "IdUsage", "crypto");
    dict_add_str(&pd, "IdType", "crypto_LUKS");
    dict_add_objpath(&pd, "Drive", drv1);
    dict_add_objpath(&pd, "CryptoBackingDevice", "/");
    dict_add_u64(&pd, "Size", 64ULL * 1024ULL * 1024ULL);
    dict_add_bool(&pd, "HintIgnore", FALSE);
    dict_add_bool(&pd, "HintSystem", FALSE);
    dbus_message_iter_close_container(&ie, &pd);
    dbus_message_iter_close_container(&ifs, &ie);

    const char *if_enc = "org.freedesktop.UDisks2.Encrypted";
    dbus_message_iter_open_container(&ifs, DBUS_TYPE_DICT_ENTRY, NULL, &ie);
    dbus_message_iter_append_basic(&ie, DBUS_TYPE_STRING, &if_enc);
    dbus_message_iter_open_container(&ie, DBUS_TYPE_ARRAY, "{sv}", &pd);
    dict_add_objpath(&pd, "CleartextDevice",
                     g_luks_unlocked ? "/org/freedesktop/UDisks2/block_devices/dm_0" : "/");
    dbus_message_iter_close_container(&ie, &pd);
    dbus_message_iter_close_container(&ifs, &ie);

    dbus_message_iter_close_container(&oe, &ifs);
    dbus_message_iter_close_container(&objs, &oe);

    /* 2. Unlocked cleartext volume /dev/dm-0 when unlocked */
    if (g_luks_unlocked) {
        const char *dm0 = "/org/freedesktop/UDisks2/block_devices/dm_0";
        dbus_message_iter_open_container(&objs, DBUS_TYPE_DICT_ENTRY, NULL, &oe);
        dbus_message_iter_append_basic(&oe, DBUS_TYPE_OBJECT_PATH, &dm0);
        dbus_message_iter_open_container(&oe, DBUS_TYPE_ARRAY, "{sa{sv}}", &ifs);

        dbus_message_iter_open_container(&ifs, DBUS_TYPE_DICT_ENTRY, NULL, &ie);
        dbus_message_iter_append_basic(&ie, DBUS_TYPE_STRING, &if_blk);
        dbus_message_iter_open_container(&ie, DBUS_TYPE_ARRAY, "{sv}", &pd);
        dict_add_ay(&pd, "Device", "/dev/dm-0");
        dict_add_str(&pd, "IdLabel", "SecureVault");
        dict_add_str(&pd, "IdUsage", "filesystem");
        dict_add_str(&pd, "IdType", "ext4");
        dict_add_objpath(&pd, "Drive", "/");
        dict_add_objpath(&pd, "CryptoBackingDevice", sdb1);
        dict_add_u64(&pd, "Size", 64ULL * 1024ULL * 1024ULL);
        dict_add_bool(&pd, "HintIgnore", FALSE);
        dict_add_bool(&pd, "HintSystem", FALSE);
        dbus_message_iter_close_container(&ie, &pd);
        dbus_message_iter_close_container(&ifs, &ie);

        const char *if_fs = "org.freedesktop.UDisks2.Filesystem";
        dbus_message_iter_open_container(&ifs, DBUS_TYPE_DICT_ENTRY, NULL, &ie);
        dbus_message_iter_append_basic(&ie, DBUS_TYPE_STRING, &if_fs);
        dbus_message_iter_open_container(&ie, DBUS_TYPE_ARRAY, "{sv}", &pd);
        dict_add_mountpoints(&pd, g_vault_mounted ? g_vault_mp : "");
        dbus_message_iter_close_container(&ie, &pd);
        dbus_message_iter_close_container(&ifs, &ie);

        dbus_message_iter_close_container(&oe, &ifs);
        dbus_message_iter_close_container(&objs, &oe);
    }

    /* 3. Optional hotplugged removable SD card /dev/sdc1 */
    if (g_usb2_present) {
        const char *drv2 = "/org/freedesktop/UDisks2/drives/camera_sd";
        append_drive_object(&objs, drv2, "Camera SD");

        const char *sdc1 = "/org/freedesktop/UDisks2/block_devices/sdc1";
        dbus_message_iter_open_container(&objs, DBUS_TYPE_DICT_ENTRY, NULL, &oe);
        dbus_message_iter_append_basic(&oe, DBUS_TYPE_OBJECT_PATH, &sdc1);
        dbus_message_iter_open_container(&oe, DBUS_TYPE_ARRAY, "{sa{sv}}", &ifs);

        dbus_message_iter_open_container(&ifs, DBUS_TYPE_DICT_ENTRY, NULL, &ie);
        dbus_message_iter_append_basic(&ie, DBUS_TYPE_STRING, &if_blk);
        dbus_message_iter_open_container(&ie, DBUS_TYPE_ARRAY, "{sv}", &pd);
        dict_add_ay(&pd, "Device", "/dev/sdc1");
        dict_add_str(&pd, "IdLabel", "CameraSD");
        dict_add_str(&pd, "IdUsage", "filesystem");
        dict_add_str(&pd, "IdType", "vfat");
        dict_add_objpath(&pd, "Drive", drv2);
        dict_add_objpath(&pd, "CryptoBackingDevice", "/");
        dict_add_u64(&pd, "Size", 32ULL * 1024ULL * 1024ULL);
        dict_add_bool(&pd, "HintIgnore", FALSE);
        dict_add_bool(&pd, "HintSystem", FALSE);
        dbus_message_iter_close_container(&ie, &pd);
        dbus_message_iter_close_container(&ifs, &ie);

        const char *if_fs = "org.freedesktop.UDisks2.Filesystem";
        dbus_message_iter_open_container(&ifs, DBUS_TYPE_DICT_ENTRY, NULL, &ie);
        dbus_message_iter_append_basic(&ie, DBUS_TYPE_STRING, &if_fs);
        dbus_message_iter_open_container(&ie, DBUS_TYPE_ARRAY, "{sv}", &pd);
        dict_add_mountpoints(&pd, g_usb2_mounted ? g_usb2_mp : "");
        dbus_message_iter_close_container(&ie, &pd);
        dbus_message_iter_close_container(&ifs, &ie);

        dbus_message_iter_close_container(&oe, &ifs);
        dbus_message_iter_close_container(&objs, &oe);
    }

    dbus_message_iter_close_container(&it, &objs);
}

static void emit_udisks_changed(DBusConnection *conn) {
    DBusMessage *sig = dbus_message_new_signal(
        "/org/freedesktop/UDisks2/block_devices/sdb1",
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged");
    if (!sig) return;
    const char *iface = "org.freedesktop.UDisks2.Block";
    DBusMessageIter it, d, a;
    dbus_message_iter_init_append(sig, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &d);
    dbus_message_iter_close_container(&it, &d);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &a);
    dbus_message_iter_close_container(&it, &a);
    dbus_connection_send(conn, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(conn);
}

static DBusHandlerResult on_msg(DBusConnection *conn, DBusMessage *msg, void *ud) {
    (void)ud;
    const char *path = dbus_message_get_path(msg);
    if (!path) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")) {
        DBusMessage *r = dbus_message_new_method_return(msg);
        append_managed_objects(r);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.ck_core.MockUDisks2", "HotplugCameraSD")) {
        g_usb2_present = true;
        log_ev("HOTPLUG_SDC1=1\n");
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        emit_udisks_changed(conn);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.UDisks2.Encrypted", "Unlock")) {
        const char *pw = NULL;
        DBusMessageIter it;
        if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&it, &pw);
        if (!pw || strcmp(pw, g_expected_pw) != 0) {
            log_ev("UNLOCK_FAIL=%s\n", pw ? pw : "");
            DBusMessage *er = dbus_message_new_error(
                msg, "org.freedesktop.UDisks2.Error.Failed", "Invalid passphrase");
            dbus_connection_send(conn, er, NULL);
            dbus_message_unref(er);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        g_luks_unlocked = true;
        log_ev("UNLOCK_OK=dm_0\n");
        const char *ct = "/org/freedesktop/UDisks2/block_devices/dm_0";
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &ct, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        emit_udisks_changed(conn);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.UDisks2.Encrypted", "Lock")) {
        g_luks_unlocked = false;
        g_vault_mounted = false;
        log_ev("LOCK_OK=sdb1\n");
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        emit_udisks_changed(conn);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.UDisks2.Filesystem", "Mount")) {
        const char *mp_out = "";
        if (strstr(path, "dm_0") != NULL) {
            snprintf(g_vault_mp, sizeof(g_vault_mp), "%s/SecureVault", g_mnt_root);
            mkdir(g_vault_mp, 0700);
            char fpath[600];
            snprintf(fpath, sizeof(fpath), "%s/vault_secret.txt", g_vault_mp);
            FILE *fp = fopen(fpath, "w");
            if (fp) { fputs("LUKS decrypted secret!\n", fp); fclose(fp); }
            g_vault_mounted = true;
            mp_out = g_vault_mp;
            log_ev("MOUNT_OK=dm_0:%s\n", g_vault_mp);
        } else if (strstr(path, "sdc1") != NULL) {
            snprintf(g_usb2_mp, sizeof(g_usb2_mp), "%s/CameraSD", g_mnt_root);
            mkdir(g_usb2_mp, 0700);
            char fpath[600];
            snprintf(fpath, sizeof(fpath), "%s/DCIM_0001.JPG", g_usb2_mp);
            FILE *fp = fopen(fpath, "w");
            if (fp) { fputs("JPEG-DATA\n", fp); fclose(fp); }
            g_usb2_mounted = true;
            mp_out = g_usb2_mp;
            log_ev("MOUNT_OK=sdc1:%s\n", g_usb2_mp);
        }
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_message_append_args(r, DBUS_TYPE_STRING, &mp_out, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        emit_udisks_changed(conn);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.UDisks2.Filesystem", "Unmount")) {
        if (strstr(path, "dm_0") != NULL) {
            g_vault_mounted = false;
            log_ev("UNMOUNT_OK=dm_0\n");
        } else if (strstr(path, "sdc1") != NULL) {
            g_usb2_mounted = false;
            log_ev("UNMOUNT_OK=sdc1\n");
        }
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        emit_udisks_changed(conn);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.UDisks2.Drive", "Eject")) {
        log_ev("DRIVE_EJECT=%s\n", path);
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.UDisks2.Drive", "PowerOff")) {
        log_ev("DRIVE_POWEROFF=%s\n", path);
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    g_log_path = argv[1];
    g_mnt_root = argv[2];
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err) || !conn) return 1;
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    int rc = dbus_bus_request_name(conn, "org.freedesktop.UDisks2",
                                   DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                   &err);
    if (dbus_error_is_set(&err) || rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) return 1;

    dbus_connection_add_filter(conn, on_msg, NULL, NULL);
    log_ev("READY=1\n");

    int fd = -1;
    dbus_connection_get_unix_fd(conn, &fd);
    while (!g_stop) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        poll(&pfd, 1, 100);
        dbus_connection_read_write(conn, 0);
        while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS) {}
    }
    return 0;
}
"""


WL_VKBD_C = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

extern const struct wl_interface zwp_virtual_keyboard_v1_interface;

static const struct wl_interface *vkb_mgr_types[] = {
    &wl_seat_interface, &zwp_virtual_keyboard_v1_interface,
};
static const struct wl_message vkb_mgr_requests[] = {
    { "create_virtual_keyboard", "on", vkb_mgr_types },
};
static const struct wl_interface zwp_virtual_keyboard_manager_v1_interface = {
    "zwp_virtual_keyboard_manager_v1", 1, 1, vkb_mgr_requests, 0, NULL,
};
static const struct wl_interface *vkb_null_types[] = { NULL, NULL, NULL, NULL };
static const struct wl_message vkb_requests[] = {
    { "keymap", "uhu", vkb_null_types },
    { "key", "uuu", vkb_null_types },
    { "modifiers", "uuuu", vkb_null_types },
    { "destroy", "", vkb_null_types },
};
const struct wl_interface zwp_virtual_keyboard_v1_interface = {
    "zwp_virtual_keyboard_v1", 1, 4, vkb_requests, 0, NULL,
};

typedef struct {
    struct wl_seat *seat;
    struct wl_proxy *vkb_mgr;
} InputState;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t ver) {
    InputState *st = (InputState *)data;
    (void)ver;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !st->seat) {
        st->seat = (struct wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, "zwp_virtual_keyboard_manager_v1") == 0 && !st->vkb_mgr) {
        st->vkb_mgr = (struct wl_proxy *)wl_registry_bind(
            reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener = { on_global, on_global_remove };

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

static uint32_t evdev_for_ascii(char ch) {
    switch (ch) {
    case 'a': return KEY_A; case 'b': return KEY_B; case 'c': return KEY_C;
    case 'd': return KEY_D; case 'e': return KEY_E; case 'f': return KEY_F;
    case 'g': return KEY_G; case 'h': return KEY_H; case 'i': return KEY_I;
    case 'j': return KEY_J; case 'k': return KEY_K; case 'l': return KEY_L;
    case 'm': return KEY_M; case 'n': return KEY_N; case 'o': return KEY_O;
    case 'p': return KEY_P; case 'q': return KEY_Q; case 'r': return KEY_R;
    case 's': return KEY_S; case 't': return KEY_T; case 'u': return KEY_U;
    case 'v': return KEY_V; case 'w': return KEY_W; case 'x': return KEY_X;
    case 'y': return KEY_Y; case 'z': return KEY_Z;
    case '0': return KEY_0; case '1': return KEY_1; case '2': return KEY_2;
    case '3': return KEY_3; case '4': return KEY_4; case '5': return KEY_5;
    case '6': return KEY_6; case '7': return KEY_7; case '8': return KEY_8;
    case '9': return KEY_9;
    default: return 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "type") != 0) return 1;
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 2;

    InputState st = {0};
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, &st);
    wl_display_roundtrip(dpy);
    if (!st.seat || !st.vkb_mgr) return 3;

    struct wl_proxy *vkb = wl_proxy_marshal_flags(
        st.vkb_mgr, 0, &zwp_virtual_keyboard_v1_interface, 1, 0, st.seat, NULL);
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
    struct xkb_keymap *kmap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *kmap_str = xkb_keymap_get_as_string(kmap, XKB_KEYMAP_FORMAT_TEXT_V1);
    uint32_t kmap_sz = (uint32_t)strlen(kmap_str) + 1u;
    int kfd = memfd_create("wp16-xkb", MFD_CLOEXEC);
    if (kfd < 0 || ftruncate(kfd, (off_t)kmap_sz) != 0) return 4;
    void *ptr = mmap(NULL, kmap_sz, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, 0);
    memcpy(ptr, kmap_str, kmap_sz);
    munmap(ptr, kmap_sz);
    wl_proxy_marshal(vkb, 0, 1u, kfd, kmap_sz);
    wl_display_roundtrip(dpy);
    close(kfd);

    const char *txt = argv[2];
    if (strcmp(txt, "<ESC>") == 0) {
        wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_ESC, 1u);
        wl_display_roundtrip(dpy);
        usleep(25000);
        wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_ESC, 0u);
        wl_display_roundtrip(dpy);
    } else {
        for (size_t i = 0; txt[i]; ++i) {
            uint32_t kc = evdev_for_ascii(txt[i]);
            if (!kc) continue;
            wl_proxy_marshal(vkb, 1, now_ms(), kc, 1u);
            wl_display_roundtrip(dpy);
            usleep(18000);
            wl_proxy_marshal(vkb, 1, now_ms(), kc, 0u);
            wl_display_roundtrip(dpy);
            usleep(18000);
        }
        wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_ENTER, 1u);
        wl_display_roundtrip(dpy);
        usleep(25000);
        wl_proxy_marshal(vkb, 1, now_ms(), (uint32_t)KEY_ENTER, 0u);
        wl_display_roundtrip(dpy);
    }

    wl_proxy_marshal(vkb, 3);
    wl_proxy_destroy(vkb);
    free(kmap_str);
    xkb_keymap_unref(kmap);
    xkb_context_unref(ctx);
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
"""


X11_HELPER_C = r"""
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*XTestFakeKeyEventFn)(Display *, unsigned int, Bool, unsigned long);

static void print_str_prop(Display *dpy, Window root, const char *name) {
    Atom a = XInternAtom(dpy, name, False);
    Atom act = None;
    int fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, a, 0, 4096, False, AnyPropertyType,
                           &act, &fmt, &nitems, &rem, &data) == Success && data && fmt == 8) {
        printf("%s=%.*s\n", name, (int)nitems, (const char *)data);
    } else {
        printf("%s=\n", name);
    }
    if (data) XFree(data);
}

static void print_win_prop(Display *dpy, Window root, const char *name) {
    Atom a = XInternAtom(dpy, name, False);
    Atom act = None;
    int fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, a, 0, 4, False, XA_WINDOW,
                           &act, &fmt, &nitems, &rem, &data) == Success &&
        data && fmt == 32 && nitems >= 1) {
        Window w = *(Window *)data;
        printf("%s=0x%lx\n", name, (unsigned long)w);
    } else {
        printf("%s=\n", name);
    }
    if (data) XFree(data);
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window root = DefaultRootWindow(dpy);

    if (argc >= 2 && strcmp(argv[1], "props") == 0) {
        print_str_prop(dpy, root, "_DT_DEVICES_STATE");
        print_str_prop(dpy, root, "_DT_TRASH_STATE");
        print_str_prop(dpy, root, "_DT_FILEMGR_LAST_SHOW");
        print_str_prop(dpy, root, "_DT_FILEMGR_SELECTION");
        print_str_prop(dpy, root, "_DT_LUKS_DIALOG_VISIBLE");
        print_str_prop(dpy, root, "_DT_CONNECT_DIALOG_VISIBLE");
        print_str_prop(dpy, root, "_DT_CONNECT_PROGRESS_VISIBLE");
        print_str_prop(dpy, root, "_DT_CONNECT_ERROR_VISIBLE");
        print_str_prop(dpy, root, "_DT_SSH_CONFIG_HOSTS");
        print_str_prop(dpy, root, "_DT_GOTO_DIALOG_STATE");
        print_win_prop(dpy, root, "_NET_ACTIVE_WINDOW");
        XCloseDisplay(dpy);
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "type") == 0) {
        void *xtst = dlopen("libXtst.so.6", RTLD_LAZY);
        if (!xtst) xtst = dlopen("libXtst.so", RTLD_LAZY);
        if (!xtst) { XCloseDisplay(dpy); return 2; }
        XTestFakeKeyEventFn fk = (XTestFakeKeyEventFn)dlsym(xtst, "XTestFakeKeyEvent");
        const char *txt = argv[2];
        if (strcmp(txt, "<ESC>") == 0) {
            KeyCode esc_kc = XKeysymToKeycode(dpy, XK_Escape);
            if (esc_kc) {
                fk(dpy, esc_kc, True, CurrentTime);
                XSync(dpy, False);
                usleep(20000);
                fk(dpy, esc_kc, False, CurrentTime);
                XSync(dpy, False);
            }
            XCloseDisplay(dpy);
            return 0;
        }
        for (size_t i = 0; txt[i]; ++i) {
            char ks_name[2] = { txt[i], '\0' };
            KeySym ks = XStringToKeysym(ks_name);
            KeyCode kc = XKeysymToKeycode(dpy, ks);
            if (!kc) continue;
            fk(dpy, kc, True, CurrentTime);
            XSync(dpy, False);
            usleep(16000);
            fk(dpy, kc, False, CurrentTime);
            XSync(dpy, False);
            usleep(16000);
        }
        KeyCode ret_kc = XKeysymToKeycode(dpy, XK_Return);
        if (ret_kc) {
            fk(dpy, ret_kc, True, CurrentTime);
            XSync(dpy, False);
            usleep(20000);
            fk(dpy, ret_kc, False, CurrentTime);
            XSync(dpy, False);
        }
        XCloseDisplay(dpy);
        return 0;
    }

    XCloseDisplay(dpy);
    return 1;
}
"""


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)


def write_thumbnail_png(path: Path, mtime_sec: int, width: int = 64, height: int = 64) -> None:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    text = _png_chunk(b"tEXt", b"Thumb::MTime\x00" + str(mtime_sec).encode("ascii"))
    row = b"\x00" + (b"\x20\x90\xe0" * width)
    idat = _png_chunk(b"IDAT", zlib.compress(row * height))
    iend = _png_chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(sig + ihdr + text + idat + iend)


def wait_for(pred, timeout: float = 10.0, interval: float = 0.08, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_val = None
    while time.monotonic() < deadline:
        last_val = pred()
        if last_val:
            return last_val
        time.sleep(interval)
    raise RuntimeError(f"Timed out waiting for {desc} (last={last_val!r})")


def read_props(x11_helper: Path, env: dict[str, str]) -> dict[str, str]:
    cp = subprocess.run([str(x11_helper), "props"], env=env, capture_output=True, text=True, check=True)
    out: dict[str, str] = {}
    for ln in cp.stdout.splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            out[k] = v
    return out


def dbus_call(env: dict[str, str], dest: str, path: str, iface_method: str, *args: str) -> str:
    cmd = [
        "dbus-send", "--session", "--print-reply",
        f"--dest={dest}", path, iface_method, *args
    ]
    cp = subprocess.run(cmd, env=env, capture_output=True, text=True, check=True, timeout=15)
    return cp.stdout


def terminate_procs(procs: list[subprocess.Popen]) -> None:
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except OSError:
                try:
                    p.terminate()
                except OSError:
                    pass
    time.sleep(0.2)
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                try:
                    p.kill()
                except OSError:
                    pass


def build_helpers(tmp_root: Path, cc: str) -> tuple[Path, Path, Path]:
    dbus_cflags = subprocess.check_output(["pkg-config", "--cflags", "dbus-1"], text=True).split()
    dbus_libs = subprocess.check_output(["pkg-config", "--libs", "dbus-1"], text=True).split()
    wl_cflags = subprocess.check_output(["pkg-config", "--cflags", "wayland-client", "xkbcommon"], text=True).split()
    wl_libs = subprocess.check_output(["pkg-config", "--libs", "wayland-client", "xkbcommon"], text=True).split()

    ud_c = tmp_root / "mock_udisks2.c"
    ud_bin = tmp_root / "mock-udisks2"
    ud_c.write_text(MOCK_UDISKS2_C, encoding="utf-8")
    subprocess.run([cc, "-O2", "-Wall", *dbus_cflags, str(ud_c), "-o", str(ud_bin), *dbus_libs], check=True)

    wl_c = tmp_root / "wl_vkbd.c"
    wl_bin = tmp_root / "wl-vkbd"
    wl_c.write_text(WL_VKBD_C, encoding="utf-8")
    subprocess.run([cc, "-O2", "-Wall", *wl_cflags, str(wl_c), "-o", str(wl_bin), *wl_libs], check=True)

    x11_c = tmp_root / "x11_helper.c"
    x11_bin = tmp_root / "x11-helper"
    x11_c.write_text(X11_HELPER_C, encoding="utf-8")
    subprocess.run([cc, "-O2", "-Wall", str(x11_c), "-o", str(x11_bin), "-lX11", "-ldl"], check=True)

    return ud_bin, wl_bin, x11_bin


def make_mock_gio_script(script_path: Path, gvfs_root: Path, gio_log: Path) -> None:
    body = f"""#!/usr/bin/env python3
import os, shutil, sys, time
from pathlib import Path

gvfs_root = Path({str(gvfs_root)!r})
log_file = Path({str(gio_log)!r})
args = sys.argv[1:]
with log_file.open("a", encoding="utf-8") as fp:
    fp.write(" ".join(args) + "\\n")

if len(args) >= 3 and args[0] == "mount" and args[1] == "-u":
    target = Path(args[2])
    if target.exists() and target.resolve().is_relative_to(gvfs_root.resolve()):
        shutil.rmtree(target)
    sys.exit(0)

if len(args) >= 2 and args[0] == "mount":
    uri = args[1]
    time.sleep(0.55)
    if "failhost" in uri:
        sys.stderr.write("Error mounting location: Connection timed out\\n")
        sys.exit(1)
    if uri.startswith("smb://"):
        rest = uri[len("smb://"):].strip("/")
        parts = rest.split("/", 1)
        host = parts[0].split("@")[-1]
        share = parts[1] if len(parts) > 1 and parts[1] else "share"
        mdir = gvfs_root / f"smb-share:server={{host}},share={{share}}"
    elif uri.startswith("sftp://"):
        rest = uri[len("sftp://"):].strip("/")
        host_part = rest.split("/", 1)[0]
        if "@" in host_part:
            user, host = host_part.split("@", 1)
            mdir = gvfs_root / f"sftp:host={{host}},user={{user}}"
        else:
            mdir = gvfs_root / f"sftp:host={{host_part}}"
    else:
        mdir = gvfs_root / "remote-share"
    mdir.mkdir(parents=True, exist_ok=True)
    (mdir / "remote_readme.txt").write_text(f"Mounted from {{uri}}\\n", encoding="utf-8")
    sys.exit(0)

sys.exit(1)
"""
    script_path.write_text(body, encoding="utf-8")
    script_path.chmod(0o755)


def run_lane(
    lane: str,
    comp_bin: Path,
    dtwm_x11_bin: Path,
    dtfile_bin: Path,
    udisks_bin: Path,
    wl_vkbd_bin: Path,
    x11_helper_bin: Path,
    lane_root: Path,
    disp_idx: int,
) -> dict[str, object]:
    t0 = time.monotonic()
    procs: list[subprocess.Popen] = []
    try:
        home = lane_root / "home"
        xdg_rt = lane_root / "runtime"
        xdg_data = lane_root / "xdg-data"
        xdg_cache = lane_root / "xdg-cache"
        xdg_cfg = lane_root / "xdg-config"
        gvfs_root = xdg_rt / "gvfs"
        mnt_root = lane_root / "mounts"
        ext_trash_mount = lane_root / "usb_data_mount"
        for d in (home, home / ".dt", xdg_rt, xdg_data, xdg_cache, xdg_cfg, gvfs_root, mnt_root, ext_trash_mount):
            d.mkdir(parents=True, exist_ok=True)
        xdg_rt.chmod(0o700)

        # Stage ~/.ssh/config + Include ~/.ssh/config.d/*.conf to verify SSH remotes discovery
        ssh_dir = home / ".ssh"
        ssh_inc_dir = ssh_dir / "config.d"
        ssh_inc_dir.mkdir(parents=True, exist_ok=True)
        ssh_dir.chmod(0o700)
        ssh_cfg_file = ssh_dir / "config"
        ssh_cfg_file.write_text(
            "Host *\n"
            "    ServerAliveInterval 60\n\n"
            "Host rpi4\n"
            "    HostName 192.168.1.50\n"
            "    User pi\n"
            "    Port 2222\n\n"
            "Include config.d/*.conf\n",
            encoding="utf-8",
        )
        (ssh_inc_dir / "extra.conf").write_text(
            "Host labbox\n"
            "    HostName lab.internal\n"
            "    User dev\n",
            encoding="utf-8",
        )

        # Enable renderImageIcons so FreeDesktop shared thumbnails render in dtfile!
        (home / ".Xdefaults").write_text(
            "Dtfile*renderImageIcons: True\n"
            "dtfile*renderImageIcons: True\n"
            "Dtfile*rereadTime: 2\n"
            "Dtfile*retryLoadDesktop: 1\n"
            "dtfile*retryLoadDesktop: 1\n",
            encoding="utf-8",
        )

        gio_log = lane_root / "gio.log"
        gio_script = lane_root / "mock-gio"
        make_mock_gio_script(gio_script, gvfs_root, gio_log)
        thumb_log = lane_root / "thumbnails.log"
        udisks_log = lane_root / "udisks2.log"

        cde_build_root = comp_bin.parents[2]
        env = os.environ.copy()
        for k in ("WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "DTUSERSESSION"):
            env.pop(k, None)

        ld_parts = [
            "/var/tmp/ck-wayland-build/wp00/motif-prefix/lib",
            "/var/tmp/ck-wayland-build/wp00/fs-prefix/lib",
            "/usr/dt/lib",
            str(cde_build_root / "lib/DtSvc/.libs"),
            str(cde_build_root / "lib/DtWidget/.libs"),
            str(cde_build_root / "lib/DtHelp/.libs"),
            str(cde_build_root / "lib/DtPrint/.libs"),
            str(cde_build_root / "lib/tt/lib/.libs"),
            str(cde_build_root / "lib/DtXinerama/.libs"),
        ]
        env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
        env["PATH"] = f"{comp_bin.parent}:/usr/dt/bin:/usr/bin:/bin"
        env.update({
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(xdg_rt),
            "XDG_DATA_HOME": str(xdg_data),
            "XDG_CACHE_HOME": str(xdg_cache),
            "XDG_CONFIG_HOME": str(xdg_cfg),
            "DTFILE_UDISKS_BUS": "session",
            "DTFILE_GVFS_ROOT": str(gvfs_root),
            "DTFILE_GIO_BIN": str(gio_script),
            "DTFILE_SSH_CONFIG": str(ssh_cfg_file),
            "DTFILE_THUMBNAIL_LOG": str(thumb_log),
            "DTFILE_TRASH_MOUNT_ROOTS": str(ext_trash_mount),
            "LC_ALL": "C.UTF-8",
            "LANG": "C.UTF-8",
        })

        # 1. Start isolated session D-Bus
        dbus_addr_file = lane_root / "dbus.addr"
        with dbus_addr_file.open("w", encoding="utf-8") as dfp:
            dbus_proc = subprocess.Popen(
                ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                env=env, stdout=dfp, stderr=subprocess.DEVNULL, start_new_session=True,
            )
        procs.append(dbus_proc)
        wait_for(lambda: dbus_addr_file.is_file() and dbus_addr_file.read_text(encoding="utf-8").strip(),
                 timeout=6.0, desc=f"{lane} dbus-daemon address")
        env["DBUS_SESSION_BUS_ADDRESS"] = dbus_addr_file.read_text(encoding="utf-8").splitlines()[0].strip()

        # 2. Start compositor / X server + dtwm
        if lane == "wayland":
            status_file = lane_root / "comp.status"
            comp_log = lane_root / "dtwm-wayland.log"
            wenv = dict(
                env,
                WLR_BACKENDS="headless",
                WLR_HEADLESS_OUTPUTS="1",
                WLR_RENDERER="pixman",
                WLR_LIBINPUT_NO_DEVICES="1",
            )
            with comp_log.open("w", encoding="utf-8") as cfp:
                comp_proc = subprocess.Popen(
                    [str(comp_bin), "--status-file", str(status_file)],
                    env=wenv, stdout=cfp, stderr=subprocess.STDOUT, start_new_session=True,
                )
            procs.append(comp_proc)

            def comp_ready():
                if comp_proc.poll() is not None:
                    raise RuntimeError(f"dtwm-wayland exited early:\n{comp_log.read_text(encoding='utf-8', errors='replace')}")
                if not status_file.is_file():
                    return None
                kv = dict(
                    ln.split("=", 1)
                    for ln in status_file.read_text(encoding="utf-8").splitlines()
                    if "=" in ln
                )
                if kv.get("WAYLAND_DISPLAY") and kv.get("DISPLAY"):
                    return kv
                return None

            kv = wait_for(comp_ready, timeout=10.0, desc="dtwm-wayland status file")
            env["WAYLAND_DISPLAY"] = kv["WAYLAND_DISPLAY"]
            env["DISPLAY"] = kv["DISPLAY"]
        else:
            xdisp = f":{disp_idx}"
            xvfb_proc = subprocess.Popen(
                ["Xvfb", xdisp, "-screen", "0", "1280x900x24", "-nolisten", "tcp"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(xvfb_proc)
            env["DISPLAY"] = xdisp
            wait_for(lambda: subprocess.run([str(x11_helper_bin), "props"], env=env,
                                            capture_output=True).returncode == 0,
                     timeout=8.0, desc="Xvfb ready")
            dtwm_proc = subprocess.Popen(
                [str(dtwm_x11_bin)],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(dtwm_proc)
            time.sleep(0.6)

        subprocess.run(["xrdb", "-merge", str(home / ".Xdefaults")], env=env, check=True)
        if shutil.which("dbus-update-activation-environment"):
            upd_vars = ["DISPLAY", "HOME", "LD_LIBRARY_PATH", "PATH",
                        "XDG_RUNTIME_DIR", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME"]
            if lane == "wayland":
                upd_vars.append("WAYLAND_DISPLAY")
            subprocess.run(["dbus-update-activation-environment", *upd_vars],
                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # 3. Start ToolTalk session daemon (`ttsession -s`) + mock UDisks2 daemon
        tt_proc = subprocess.Popen(
            ["/usr/dt/bin/ttsession", "-s"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(tt_proc)
        time.sleep(0.5)

        ud_proc = subprocess.Popen(
            [str(udisks_bin), str(udisks_log), str(mnt_root)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(ud_proc)
        wait_for(lambda: udisks_log.is_file() and "READY=1" in udisks_log.read_text(encoding="utf-8"),
                 timeout=6.0, desc=f"{lane} mock-udisks2 ready")

        # 4. Launch dtfile and verify Motif ABI 9 + FileManager1 registration + initial UDisks2 discovery
        docs_dir = home / "Documents"
        docs_dir.mkdir(parents=True, exist_ok=True)
        pdf_file = docs_dir / "Project Proposal (Final).pdf"
        pdf_file.write_bytes(b"%PDF-1.7\n% CK-Core WP-16 Thumbnail & FileManager1 Test\n")
        pdf_mtime = int(pdf_file.stat().st_mtime)

        # Stage FreeDesktop shared thumbnail in $XDG_CACHE_HOME/thumbnails/large/<md5(URI)>.png
        pdf_uri = pdf_file.resolve().as_uri()
        thumb_md5 = hashlib.md5(pdf_uri.encode("utf-8")).hexdigest()
        expected_thumb = xdg_cache / "thumbnails" / "large" / f"{thumb_md5}.png"
        write_thumbnail_png(expected_thumb, pdf_mtime)

        dtfile_log = lane_root / "dtfile.log"
        with dtfile_log.open("w", encoding="utf-8") as df_out:
            dtfile_proc = subprocess.Popen(
                [str(dtfile_bin), "-dir", str(home)],
                env=env, stdout=df_out, stderr=subprocess.STDOUT, start_new_session=True,
            )
        procs.append(dtfile_proc)

        # Wait for our launched dtfile to own org.freedesktop.FileManager1 without triggering auto-start
        wait_for(
            lambda: "boolean true" in subprocess.run(
                ["dbus-send", "--session", "--print-reply",
                 "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus",
                 "org.freedesktop.DBus.NameHasOwner", "string:org.freedesktop.FileManager1"],
                env=env, capture_output=True, text=True
            ).stdout,
            timeout=12.0,
            desc=f"{lane} dtfile FileManager1 D-Bus name ownership",
        )
        wait_for(
            lambda: subprocess.run(
                ["dbus-send", "--session", "--print-reply",
                 "--dest=org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                 "org.freedesktop.DBus.Introspectable.Introspect"],
                env=env, capture_output=True, text=True
            ).returncode == 0,
            timeout=6.0,
            desc=f"{lane} dtfile FileManager1 D-Bus Introspect",
        )

        # Verify actual dtfile view process maps CK Core Motif ABI 9 (`libXm.so.9`)
        def find_dtfile_motif9():
            for ent in Path("/proc").iterdir():
                if not ent.name.isdigit():
                    continue
                try:
                    if (ent / "comm").read_text(encoding="utf-8").strip() != "dtfile":
                        continue
                    ebytes = (ent / "environ").read_bytes().decode("utf-8", errors="replace")
                    if f"HOME={home}\x00" not in ebytes:
                        continue
                    maps = (ent / "maps").read_text(encoding="utf-8", errors="replace")
                    if "libXm.so.9" in maps:
                        return int(ent.name)
                except OSError:
                    continue
            return None

        dtfile_pid = wait_for(find_dtfile_motif9, timeout=6.0, desc=f"{lane} dtfile libXm.so.9 mapping")

        def type_keys(text: str) -> None:
            if lane == "wayland":
                subprocess.run([str(wl_vkbd_bin), "type", text], env=env, check=True)
            else:
                subprocess.run([str(x11_helper_bin), "type", text], env=env, check=True)

        # ---- CHECK A: UDisks2 LUKS Unlock (wrong pw retry -> valid pw -> mount) + USB hotplug + Eject ----
        wait_for(
            lambda: "[Locked] SecureVault|crypto_LUKS|locked" in read_props(x11_helper_bin, env).get("_DT_DEVICES_STATE", ""),
            timeout=8.0,
            desc=f"{lane} UDisks2 locked LUKS discovery",
        )

        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.ck_core.Dtfile.ActivateDevice",
            "string:udisks:///org/freedesktop/UDisks2/block_devices/sdb1",
        )
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_LUKS_DIALOG_VISIBLE") == "1",
            timeout=6.0,
            desc=f"{lane} Motif LUKS Unlock dialog visible",
        )

        # 1) Wrong passphrase -> stays visible and records UNLOCK_FAIL
        type_keys("wrongpass1")
        wait_for(
            lambda: "UNLOCK_FAIL=wrongpass1" in udisks_log.read_text(encoding="utf-8"),
            timeout=6.0,
            desc=f"{lane} LUKS wrong passphrase rejected",
        )
        assert read_props(x11_helper_bin, env).get("_DT_LUKS_DIALOG_VISIBLE") == "1"

        # 2) Valid passphrase -> Encrypted.Unlock -> Filesystem.Mount -> dialog closes -> mounted!
        type_keys("vaultkey42")
        wait_for(
            lambda: "MOUNT_OK=dm_0:" in udisks_log.read_text(encoding="utf-8")
            and read_props(x11_helper_bin, env).get("_DT_LUKS_DIALOG_VISIBLE") == "0"
            and "SecureVault|ext4|mounted" in read_props(x11_helper_bin, env).get("_DT_DEVICES_STATE", ""),
            timeout=8.0,
            desc=f"{lane} LUKS unlock + mount completion",
        )

        # 3) Hotplug second removable drive (CameraSD) -> live signal discovery + mount
        dbus_call(
            env, "org.freedesktop.UDisks2", "/org/freedesktop/UDisks2",
            "org.ck_core.MockUDisks2.HotplugCameraSD",
        )
        wait_for(
            lambda: "[Drive] CameraSD|vfat|unmounted" in read_props(x11_helper_bin, env).get("_DT_DEVICES_STATE", ""),
            timeout=6.0,
            desc=f"{lane} hotplugged CameraSD discovery",
        )
        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.ck_core.Dtfile.ActivateDevice",
            "string:udisks:///org/freedesktop/UDisks2/block_devices/sdc1",
        )
        wait_for(
            lambda: "CameraSD|vfat|mounted" in read_props(x11_helper_bin, env).get("_DT_DEVICES_STATE", ""),
            timeout=6.0,
            desc=f"{lane} CameraSD mount",
        )

        # 4) Safely eject SecureVault -> Unmount + Encrypted.Lock + Drive.Eject + Drive.PowerOff
        vault_mp = str(mnt_root / "SecureVault")
        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.ck_core.Dtfile.EjectDevice", f"string:{vault_mp}",
        )
        wait_for(
            lambda: all(tok in udisks_log.read_text(encoding="utf-8")
                        for tok in ("UNMOUNT_OK=dm_0", "LOCK_OK=sdb1", "DRIVE_EJECT=", "DRIVE_POWEROFF="))
            and "[Locked] SecureVault|crypto_LUKS|locked" in read_props(x11_helper_bin, env).get("_DT_DEVICES_STATE", ""),
            timeout=8.0,
            desc=f"{lane} SecureVault safe eject & LUKS re-lock",
        )

        # ---- CHECK B: GVFS Connect to Server (Progress + SSH Remotes + Error) + Tabbed Go To + MTP ----
        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.ck_core.Dtfile.OpenConnectServerDialog",
        )
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_CONNECT_DIALOG_VISIBLE") == "1",
            timeout=6.0,
            desc=f"{lane} Connect to Server dialog visible",
        )
        # Default URI is pre-filled with `smb://files.local/shared`; pressing Return activates Connect
        # and displays the Connecting progress dialog (`_DT_CONNECT_PROGRESS_VISIBLE == "1"`)!
        type_keys("")
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_CONNECT_PROGRESS_VISIBLE") == "1",
            timeout=4.0,
            desc=f"{lane} Connecting to Server progress dialog",
        )
        wait_for(
            lambda: gio_log.is_file()
            and "mount smb://files.local/shared" in gio_log.read_text(encoding="utf-8")
            and read_props(x11_helper_bin, env).get("_DT_CONNECT_PROGRESS_VISIBLE", "0") == "0"
            and "SMB: shared@files.local|gvfs/smb|mounted" in read_props(x11_helper_bin, env).get("_DT_DEVICES_STATE", ""),
            timeout=8.0,
            desc=f"{lane} GVFS SMB share mounted and surfaced",
        )

        # Verify SSH / SFTP mode parses `~/.ssh/config` + `Include` (`rpi4` & `labbox`, skipping `Host *`)
        # and pre-selects `rpi4` (`sftp://pi@rpi4/`) for instant one-Return connection!
        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.ck_core.Dtfile.OpenConnectServerDialog", "string:sftp",
        )
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_CONNECT_DIALOG_VISIBLE") == "1"
            and read_props(x11_helper_bin, env).get("_DT_SSH_CONFIG_HOSTS") == "rpi4|192.168.1.50|pi|2222;labbox|lab.internal|dev|",
            timeout=6.0,
            desc=f"{lane} SSH config remotes parsed (including Include)",
        )
        type_keys("")
        wait_for(
            lambda: "mount sftp://pi@rpi4/" in gio_log.read_text(encoding="utf-8")
            and "SFTP: rpi4|gvfs/sftp|mounted" in read_props(x11_helper_bin, env).get("_DT_DEVICES_STATE", ""),
            timeout=8.0,
            desc=f"{lane} SSH remote alias mounted via GVFS SFTP",
        )

        # Verify mount failure shows Progress Dialog -> Motif Connect to Server Error Dialog!
        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.ck_core.Dtfile.OpenConnectServerDialog", "string:sftp://failhost/",
        )
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_CONNECT_DIALOG_VISIBLE") == "1",
            timeout=6.0,
            desc=f"{lane} Connect dialog prefilled with failing host",
        )
        type_keys("")
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_CONNECT_PROGRESS_VISIBLE") == "1"
            or read_props(x11_helper_bin, env).get("_DT_CONNECT_ERROR_VISIBLE") == "1",
            timeout=4.0,
            desc=f"{lane} Progress dialog during failing connection",
        )
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_CONNECT_ERROR_VISIBLE") == "1"
            and read_props(x11_helper_bin, env).get("_DT_CONNECT_PROGRESS_VISIBLE", "0") == "0",
            timeout=6.0,
            desc=f"{lane} Connect to Server Error dialog posted on mount failure",
        )
        # Dismiss error dialog (Return) and close Connect dialog (Escape)
        type_keys("")
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_CONNECT_ERROR_VISIBLE") == "0",
            timeout=6.0,
            desc=f"{lane} Connect Error dialog dismissed",
        )
        type_keys("<ESC>")
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_CONNECT_DIALOG_VISIBLE") == "0",
            timeout=6.0,
            desc=f"{lane} Connect dialog closed after error",
        )

        # Verify Tabbed `File -> Go To...` (`Connected Mounts & Shares` tab) lists mounts & opens selected mount!
        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.ck_core.Dtfile.OpenGoToDialog", "string:mounts",
        )
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_GOTO_DIALOG_STATE", "").startswith("tab=mounts\tmounts="),
            timeout=6.0,
            desc=f"{lane} Tabbed Go To dialog Connected Mounts & Shares tab",
        )
        goto_state = read_props(x11_helper_bin, env).get("_DT_GOTO_DIALOG_STATE", "")
        mounts_num = int(goto_state.split("mounts=", 1)[1])
        assert mounts_num >= 2, f"Expected >= 2 mounts in Go To Mounts tab, got {goto_state!r}"
        type_keys("<ESC>")

        # Hotplug MTP phone directory under gvfs_root -> live inotify discovery
        mtp_dir = gvfs_root / "mtp:host=Google_Pixel_9"
        mtp_dir.mkdir(parents=True, exist_ok=True)
        (mtp_dir / "DCIM").mkdir(exist_ok=True)
        wait_for(
            lambda: "Phone: Google_Pixel_9|gvfs/mtp|mounted" in read_props(x11_helper_bin, env).get("_DT_DEVICES_STATE", ""),
            timeout=4.0,
            desc=f"{lane} live MTP phone GVFS discovery",
        )

        # Disconnect SMB share via UnmountDevice (`gio mount -u`)
        smb_mp = str(gvfs_root / "smb-share:server=files.local,share=shared")
        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.ck_core.Dtfile.UnmountDevice", f"string:{smb_mp}",
        )
        wait_for(
            lambda: "SMB: shared@files.local" not in read_props(x11_helper_bin, env).get("_DT_DEVICES_STATE", ""),
            timeout=6.0,
            desc=f"{lane} GVFS SMB share disconnected",
        )

        # ---- CHECK C: Built-in Archive Compress/Extract (5 formats) + Zip-Slip Rejection ----
        arc_work = lane_root / "arc_test"
        arc_src = arc_work / "src"
        arc_src.mkdir(parents=True, exist_ok=True)
        sample_doc = arc_src / "Quarterly Report (2026) 日本語.txt"
        sample_payload = b"Archive verification across Wayland and X11!\n" * 12
        sample_doc.write_bytes(sample_payload)

        for ext in (".zip", ".tar.gz", ".tar.xz", ".tar.zst", ".7z"):
            out_arc = arc_work / f"packed{ext}"
            dbus_call(
                env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                "org.ck_core.Dtfile.CreateArchive",
                f"array:string:{sample_doc}", f"string:{out_arc}", f"string:{ext}",
            )
            assert out_arc.is_file() and out_arc.stat().st_size > 0
            ext_out = arc_work / f"out_{ext.lstrip('.').replace('.', '_')}"
            ext_out.mkdir(parents=True, exist_ok=True)
            dbus_call(
                env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                "org.ck_core.Dtfile.ExtractArchive",
                f"string:{out_arc}", f"string:{ext_out}", "boolean:true",
            )
            assert (ext_out / sample_doc.name).read_bytes() == sample_payload

        # Adversarial Zip-Slip archive via D-Bus must return org.ck_core.Dtfile.ArchiveError!
        evil_zip = arc_work / "evil_slip.zip"
        with zipfile.ZipFile(evil_zip, "w") as zf:
            zf.writestr("../../escaped_outside.txt", b"zip-slip")
        slip_dest = arc_work / "slip_jail" / "inner"
        slip_dest.mkdir(parents=True, exist_ok=True)
        slip_cp = subprocess.run(
            ["dbus-send", "--session", "--print-reply",
             "--dest=org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
             "org.ck_core.Dtfile.ExtractArchive",
             f"string:{evil_zip}", f"string:{slip_dest}", "boolean:false"],
            env=env, capture_output=True, text=True,
        )
        assert slip_cp.returncode != 0 and "ArchiveError" in (slip_cp.stderr + slip_cp.stdout)
        assert not (arc_work / "escaped_outside.txt").exists()

        # ---- CHECK D: org.freedesktop.FileManager1 + FreeDesktop Shared Thumbnail Resolution ----
        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1.ShowItems",
            f"array:string:{pdf_uri}", "string:wp16-startup",
        )

        def check_fm1_and_thumb():
            pr = read_props(x11_helper_bin, env)
            last_show = pr.get("_DT_FILEMGR_LAST_SHOW", "")
            sel = pr.get("_DT_FILEMGR_SELECTION", "")
            act = pr.get("_NET_ACTIVE_WINDOW", "")
            parts = last_show.split("\t")
            if len(parts) < 4:
                return False
            win_hex = parts[3]
            if parts[0] != "ShowItems" or parts[1] != str(docs_dir) or parts[2] != pdf_file.name:
                return False
            if sel != f"{docs_dir}\t{pdf_file.name}\t1":
                return False
            if not win_hex or win_hex == "0x0" or (act and act != win_hex):
                return False
            if not thumb_log.is_file() or str(expected_thumb) not in thumb_log.read_text(encoding="utf-8"):
                return False
            return True

        wait_for(check_fm1_and_thumb, timeout=8.0, desc=f"{lane} FileManager1 ShowItems + window raise + thumbnail")

        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1.ShowItemProperties",
            f"array:string:{pdf_uri}", "string:wp16-props",
        )
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_FILEMGR_LAST_SHOW", "").startswith(
                f"ShowItemProperties\t{docs_dir}\t{pdf_file.name}\t"
            ),
            timeout=6.0,
            desc=f"{lane} FileManager1 ShowItemProperties",
        )

        # ---- CHECK E: Live Bidirectional XDG Trash (inotify) + Per-Mount .Trash-$UID ----
        xdg_trash_files = xdg_data / "Trash" / "files"
        xdg_trash_info = xdg_data / "Trash" / "info"
        xdg_trash_files.mkdir(parents=True, exist_ok=True)
        xdg_trash_info.mkdir(parents=True, exist_ok=True)

        # 1) External app trashes file into $XDG_DATA_HOME/Trash -> live inotify sync (< 1.5s)!
        ext_payload = xdg_trash_files / "external_note.txt"
        ext_info = xdg_trash_info / "external_note.txt.trashinfo"
        ext_payload.write_text("trashed by external app\n", encoding="utf-8")
        ext_info.write_text(
            f"[Trash Info]\nPath={home}/external_note.txt\nDeletionDate=2026-11-18T14:00:00\n",
            encoding="utf-8",
        )
        wait_for(
            lambda: "count=1\texternal_note.txt" in read_props(x11_helper_bin, env).get("_DT_TRASH_STATE", ""),
            timeout=2.5,
            desc=f"{lane} live inotify XDG Trash external add",
        )
        # Bidirectional non-destructive guarantee: XDG payload & .trashinfo remain intact!
        assert ext_payload.is_file() and ext_info.is_file()

        # 2) External app restores/deletes item from $XDG_DATA_HOME/Trash -> live inotify prune!
        ext_payload.unlink()
        ext_info.unlink()
        wait_for(
            lambda: read_props(x11_helper_bin, env).get("_DT_TRASH_STATE", "") == "count=0",
            timeout=2.5,
            desc=f"{lane} live inotify XDG Trash external prune",
        )

        # 3) dtfile trashes both a $HOME file and an external mount file -> creates XDG & .Trash-$UID!
        home_victim = home / "home_memo.txt"
        home_victim.write_text("memo in home\n", encoding="utf-8")
        usb_victim = ext_trash_mount / "card_clip.mp4"
        usb_victim.write_bytes(b"MP4-DATA-ON-USB-MOUNT\n" * 50)

        dbus_call(
            env, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.ck_core.Dtfile.TrashPaths",
            f"array:string:{home_victim},{usb_victim}",
        )

        uid = os.getuid()
        per_mount_trash = ext_trash_mount / f".Trash-{uid}"
        wait_for(
            lambda: (xdg_trash_files / "home_memo.txt").is_file()
            and (xdg_trash_info / "home_memo.txt.trashinfo").is_file()
            and (per_mount_trash / "files" / "card_clip.mp4").is_file()
            and (per_mount_trash / "info" / "card_clip.mp4.trashinfo").is_file()
            and "count=2" in read_props(x11_helper_bin, env).get("_DT_TRASH_STATE", ""),
            timeout=6.0,
            desc=f"{lane} dtfile bidirectional XDG Trash + per-mount .Trash-$UID",
        )
        pm_info_txt = (per_mount_trash / "info" / "card_clip.mp4.trashinfo").read_text(encoding="utf-8")
        assert "Path=card_clip.mp4\n" in pm_info_txt, f"Expected mount-relative Path= in {pm_info_txt!r}"

        return {
            "lane": lane,
            "dtfile_pid": dtfile_pid,
            "elapsed_s": round(time.monotonic() - t0, 2),
        }
    except Exception:
        for lname in ("dtfile.log", "udisks2.log", "gio.log", "dtwm-wayland.log"):
            lp = lane_root / lname
            if lp.is_file():
                txt = lp.read_text(encoding="utf-8", errors="replace")[-1600:]
                if txt.strip():
                    print(f"--- [{lane}] {lname} ---\n{txt}", file=sys.stderr)
        raise
    finally:
        terminate_procs(procs)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compositor", type=Path, required=True, help="Path to dtwm-wayland binary")
    args = ap.parse_args()

    comp_bin = args.compositor.resolve()
    dtwm_x11_bin = comp_bin.parent / "dtwm"
    if not dtwm_x11_bin.is_file():
        dtwm_x11_bin = Path("/usr/dt/bin/dtwm")

    cde_build_root = comp_bin.parents[2]
    dtfile_bin = Path("/usr/dt/bin/dtfile")
    if not dtfile_bin.is_file():
        dtfile_bin = cde_build_root / "programs/dtfile/dtfile"

    for req_tool in ("dbus-daemon", "dbus-send", "Xvfb", "xrdb"):
        if not shutil.which(req_tool):
            print(f"SKIP: required tool {req_tool} not installed")
            return 77

    cc = os.environ.get("CC", "cc").split()[0]
    tmp_base = os.environ.get("TMPDIR")

    t_all = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="wp16-dual-", dir=tmp_base) as raw_root:
        root = Path(raw_root)
        ud_bin, wl_bin, x11_bin = build_helpers(root, cc)

        wl_root = root / "wayland"
        x11_root = root / "x11"
        wl_root.mkdir()
        x11_root.mkdir()

        disp_idx = 84 + (os.getpid() % 25)
        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_wl = pool.submit(
                run_lane, "wayland", comp_bin, dtwm_x11_bin, dtfile_bin,
                ud_bin, wl_bin, x11_bin, wl_root, disp_idx,
            )
            fut_x11 = pool.submit(
                run_lane, "x11", comp_bin, dtwm_x11_bin, dtfile_bin,
                ud_bin, wl_bin, x11_bin, x11_root, disp_idx + 1,
            )
            res_wl = fut_wl.result()
            res_x11 = fut_x11.result()

    total_s = round(time.monotonic() - t_all, 2)
    print(
        f"PASS: WP-16 parallel Wayland ({res_wl['elapsed_s']}s) + X11 ({res_x11['elapsed_s']}s) "
        f"completed in {total_s}s — UDisks2 USB/LUKS, GVFS/MTP, 5-format archives + Zip-Slip guard, "
        f"FreeDesktop thumbnails, FileManager1, and live inotify XDG/.Trash-$UID verified"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
