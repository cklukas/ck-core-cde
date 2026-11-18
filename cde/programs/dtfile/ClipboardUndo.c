/*
 * ClipboardUndo.c - Standard File Clipboard (Ctrl+C/X/V: x-special/gnome-copied-files
 *                   & text/uri-list) and Bounded Session Undo Journal (Ctrl+Z)
 *                   for CK-Core CDE File Manager (WP-17).
 */

#define _GNU_SOURCE

#include "ClipboardUndo.h"
#ifndef DTFILE_CLIPBOARD_UNIT_TEST
#include <Xm/Xm.h>
#include <Xm/RowColumn.h>
#include "Encaps.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "Help.h"
#include "SharedMsgs.h"
#include "XdgTrashInfo.h"
#include "XdgTrashSync.h"
#include "BrowserErgonomics.h"
#endif
#include <X11/Xatom.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef DTFILE_CLIPBOARD_UNIT_TEST
#define DTFILE_UNDO_CAP 64

typedef struct {
    DtfileUndoKind kind;
    int count;
    char **src_paths;
    char **dst_paths;
    char summary[256];
} DtfileUndoEntry;

static Widget g_clip_widget = nullptr;
static Boolean g_clip_owned = False;
static Boolean g_clip_is_cut = False;
static Time g_clip_timestamp = CurrentTime;
static char **g_clip_paths = nullptr;
static int g_clip_count = 0;

static DtfileUndoEntry g_undo_stack[DTFILE_UNDO_CAP];
static int g_undo_len = 0;
static Boolean g_undo_in_progress = False;

/* Optional notification hooks defined in FileOp.c */
extern void (*DtfileUndoRenameNotify)(const char *old_path, const char *new_path);
extern void (*DtfileUndoTransferNotify)(int mode, const char *src_path, const char *dst_path);
#endif

static Boolean
is_uri_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
}

static int
hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

char *
DtfilePathToFileUri(const char *abs_path)
{
    if (!abs_path || abs_path[0] != '/')
        return nullptr;

    size_t len = strlen(abs_path);
    size_t cap = 8 + len * 3 + 1;
    char *out = (char *)XtMalloc((Cardinal)cap);
    if (!out)
        return nullptr;

    static const char hex[] = "0123456789ABCDEF";
    memcpy(out, "file://", 7);
    size_t pos = 7;
    for (size_t i = 0; i < len; ++i)
    {
        unsigned char c = (unsigned char)abs_path[i];
        if (is_uri_unreserved(c))
        {
            out[pos++] = (char)c;
        }
        else
        {
            out[pos++] = '%';
            out[pos++] = hex[(c >> 4) & 0xFu];
            out[pos++] = hex[c & 0xFu];
        }
    }
    out[pos] = '\0';
    return out;
}

char *
DtfileFileUriToPath(const char *uri)
{
    if (!uri)
        return nullptr;
    while (*uri == ' ' || *uri == '\t')
        ++uri;

    Boolean is_uri = True;
    const char *p = nullptr;
    if (strncmp(uri, "file://localhost/", 17) == 0)
        p = uri + 16;
    else if (strncmp(uri, "file:///", 8) == 0)
        p = uri + 7;
    else if (uri[0] == '/')
    {
        p = uri;
        is_uri = False;
    }
    else
        return nullptr;

    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n' ||
                       (!is_uri && (p[len - 1] == ' ' || p[len - 1] == '\t')) ||
                       (is_uri && (p[len - 1] == ' ' || p[len - 1] == '\t'))))
    {
        --len;
    }
    if (len == 0)
        return nullptr;

    char *out = (char *)XtMalloc((Cardinal)(len + 1));
    if (!out)
        return nullptr;

    size_t o = 0;
    for (size_t i = 0; i < len; ++i)
    {
        if (is_uri && p[i] == '%')
        {
            if (i + 2 >= len)
            {
                XtFree(out);
                return nullptr;
            }
            int hi = hex_val(p[i + 1]);
            int lo = hex_val(p[i + 2]);
            if (hi < 0 || lo < 0)
            {
                XtFree(out);
                return nullptr;
            }
            unsigned char b = (unsigned char)((hi << 4) | lo);
            if (b == 0)
            {
                XtFree(out);
                return nullptr;
            }
            out[o++] = (char)b;
            i += 2;
            continue;
        }
        out[o++] = p[i];
    }
    out[o] = '\0';
    if (out[0] != '/')
    {
        XtFree(out);
        return nullptr;
    }
    return out;
}

void
DtfileFreeStringArray(char **arr, int count)
{
    if (!arr)
        return;
    for (int i = 0; i < count; ++i)
        XtFree(arr[i]);
    XtFree((char *)arr);
}

Boolean
DtfileParseClipboardPayload(
    const char *mime_type,
    const char *data,
    size_t data_len,
    Boolean *out_is_cut,
    char ***out_paths,
    int *out_count)
{
    if (out_is_cut) *out_is_cut = False;
    if (out_paths) *out_paths = nullptr;
    if (out_count) *out_count = 0;
    if (!data || data_len == 0 || !out_paths || !out_count)
        return False;

    Boolean is_gnome = (mime_type && strcmp(mime_type, "x-special/gnome-copied-files") == 0);
    Boolean is_cut = False;
    Boolean first_line = True;

    char *buf = (char *)XtMalloc((Cardinal)(data_len + 1));
    if (!buf)
        return False;
    memcpy(buf, data, data_len);
    buf[data_len] = '\0';

    int cap = 16;
    int cnt = 0;
    char **paths = (char **)XtCalloc((Cardinal)cap, sizeof(char *));

    char *save = nullptr;
    for (char *line = strtok_r(buf, "\r\n", &save);
         line != nullptr;
         line = strtok_r(nullptr, "\r\n", &save))
    {
        while (*line == ' ' || *line == '\t')
            ++line;
        if (*line == '\0')
            continue;

        if (first_line && is_gnome)
        {
            first_line = False;
            if (strcmp(line, "cut") == 0)
            {
                is_cut = True;
                continue;
            }
            if (strcmp(line, "copy") == 0)
            {
                is_cut = False;
                continue;
            }
        }
        first_line = False;
        if (line[0] == '#')
            continue;

        char *decoded = DtfileFileUriToPath(line);
        if (!decoded)
            continue;

        if (cnt >= cap)
        {
            cap *= 2;
            paths = (char **)XtRealloc((char *)paths, (Cardinal)(cap * sizeof(char *)));
        }
        paths[cnt++] = decoded;
    }

    XtFree(buf);
    if (cnt == 0)
    {
        XtFree((char *)paths);
        return False;
    }
    if (out_is_cut) *out_is_cut = is_cut;
    *out_paths = paths;
    *out_count = cnt;
    return True;
}

#ifndef DTFILE_CLIPBOARD_UNIT_TEST

static void
free_undo_entry(DtfileUndoEntry *e)
{
    if (!e)
        return;
    DtfileFreeStringArray(e->src_paths, e->count);
    DtfileFreeStringArray(e->dst_paths, e->count);
    memset(e, 0, sizeof(*e));
}

static DtfileUndoEntry *
push_undo_slot(void)
{
    if (g_undo_len >= DTFILE_UNDO_CAP)
    {
        free_undo_entry(&g_undo_stack[0]);
        memmove(&g_undo_stack[0], &g_undo_stack[1],
                (size_t)(DTFILE_UNDO_CAP - 1) * sizeof(DtfileUndoEntry));
        g_undo_len = DTFILE_UNDO_CAP - 1;
    }
    DtfileUndoEntry *slot = &g_undo_stack[g_undo_len++];
    memset(slot, 0, sizeof(*slot));
    return slot;
}

static const char *
path_basename_cstr(const char *path)
{
    if (!path)
        return "";
    const char *s = strrchr(path, '/');
    return (s && s[1]) ? (s + 1) : path;
}

static void
extract_parent_dir(const char *full_path, char *out_dir, size_t cap)
{
    if (!full_path || cap == 0)
    {
        if (cap > 0) out_dir[0] = '\0';
        return;
    }
    const char *slash = strrchr(full_path, '/');
    if (!slash || slash == full_path)
    {
        snprintf(out_dir, cap, "/");
        return;
    }
    size_t dlen = (size_t)(slash - full_path);
    if (dlen >= cap) dlen = cap - 1;
    memcpy(out_dir, full_path, dlen);
    out_dir[dlen] = '\0';
}

static void
refresh_dir_views(const char *dir_path)
{
    if (!dir_path || !*dir_path)
        return;
    RereadDirectory(toplevel, home_host_name, (char *)dir_path);
    UpdateDirectory(toplevel, home_host_name, (char *)dir_path);
}

static void
set_view_status_msg(FileMgrData *fmd, const char *msg)
{
    if (!fmd || !msg || !fmd->file_mgr_rec)
        return;
    FileMgrRec *rec = (FileMgrRec *)fmd->file_mgr_rec;
    if (rec->status_line)
    {
        XmString xs = XmStringCreateLocalized((char *)msg);
        XtVaSetValues(rec->status_line, XmNlabelString, xs, nullptr);
        XmStringFree(xs);
    }
}

void
DtfileUndoRecordRename(const char *old_path, const char *new_path)
{
    if (g_undo_in_progress || !old_path || !new_path || strcmp(old_path, new_path) == 0)
        return;
    DtfileUndoEntry *slot = push_undo_slot();
    slot->kind = DTFILE_UNDO_RENAME;
    slot->count = 1;
    slot->src_paths = (char **)XtCalloc(1, sizeof(char *));
    slot->dst_paths = (char **)XtCalloc(1, sizeof(char *));
    slot->src_paths[0] = XtNewString((char *)old_path);
    slot->dst_paths[0] = XtNewString((char *)new_path);
    snprintf(slot->summary, sizeof(slot->summary), "Rename %s -> %s",
             path_basename_cstr(old_path), path_basename_cstr(new_path));
    DtfileErgonomicsPublishState(nullptr);
}

void
DtfileUndoRecordMoveBatch(
    const char *const *src_paths,
    const char *const *dst_paths,
    int count)
{
    if (g_undo_in_progress || !src_paths || !dst_paths || count <= 0)
        return;
    DtfileUndoEntry *slot = push_undo_slot();
    slot->kind = DTFILE_UNDO_MOVE;
    slot->count = count;
    slot->src_paths = (char **)XtCalloc((Cardinal)count, sizeof(char *));
    slot->dst_paths = (char **)XtCalloc((Cardinal)count, sizeof(char *));
    for (int i = 0; i < count; ++i)
    {
        slot->src_paths[i] = XtNewString((char *)src_paths[i]);
        slot->dst_paths[i] = XtNewString((char *)dst_paths[i]);
    }
    if (count == 1)
        snprintf(slot->summary, sizeof(slot->summary), "Move %s", path_basename_cstr(dst_paths[0]));
    else
        snprintf(slot->summary, sizeof(slot->summary), "Move %d items", count);
    DtfileErgonomicsPublishState(nullptr);
}

void
DtfileUndoRecordCopyBatch(const char *const *dst_paths, int count)
{
    if (g_undo_in_progress || !dst_paths || count <= 0)
        return;
    DtfileUndoEntry *slot = push_undo_slot();
    slot->kind = DTFILE_UNDO_COPY;
    slot->count = count;
    slot->dst_paths = (char **)XtCalloc((Cardinal)count, sizeof(char *));
    for (int i = 0; i < count; ++i)
        slot->dst_paths[i] = XtNewString((char *)dst_paths[i]);
    if (count == 1)
        snprintf(slot->summary, sizeof(slot->summary), "Copy %s", path_basename_cstr(dst_paths[0]));
    else
        snprintf(slot->summary, sizeof(slot->summary), "Copy %d items", count);
    DtfileErgonomicsPublishState(nullptr);
}

void
DtfileUndoRecordTrashBatch(const char *const *orig_paths, int count)
{
    if (g_undo_in_progress || !orig_paths || count <= 0)
        return;
    DtfileUndoEntry *slot = push_undo_slot();
    slot->kind = DTFILE_UNDO_TRASH;
    slot->count = count;
    slot->src_paths = (char **)XtCalloc((Cardinal)count, sizeof(char *));
    for (int i = 0; i < count; ++i)
        slot->src_paths[i] = XtNewString((char *)orig_paths[i]);
    if (count == 1)
        snprintf(slot->summary, sizeof(slot->summary), "Trash %s", path_basename_cstr(orig_paths[0]));
    else
        snprintf(slot->summary, sizeof(slot->summary), "Trash %d items", count);
    DtfileErgonomicsPublishState(nullptr);
}

int
DtfileUndoDepth(void)
{
    return g_undo_len;
}

const char *
DtfileUndoTopSummary(void)
{
    if (g_undo_len <= 0)
        return "";
    return g_undo_stack[g_undo_len - 1].summary;
}

/* Safe recursive copy/move helpers for clipboard paste and cross-FS undo */
static Boolean
copy_tree_noreplace(const char *src, const char *dst, int depth)
{
    if (!src || !dst || depth > 256)
        return False;

    struct stat st;
    if (lstat(src, &st) != 0)
        return False;

    struct stat dst_st;
    if (lstat(dst, &dst_st) == 0)
        return False;

    if (S_ISLNK(st.st_mode))
    {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof(target) - 1);
        if (n < 0)
            return False;
        target[n] = '\0';
        return symlink(target, dst) == 0;
    }

    if (S_ISDIR(st.st_mode))
    {
        if (mkdir(dst, (st.st_mode & 0777) | 0700) != 0)
            return False;
        DIR *dp = opendir(src);
        if (!dp)
            return False;
        struct dirent *de;
        Boolean ok = True;
        while ((de = readdir(dp)) != nullptr)
        {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char csrc[PATH_MAX], cdst[PATH_MAX];
            if (snprintf(csrc, sizeof(csrc), "%s/%s", src, de->d_name) >= (int)sizeof(csrc) ||
                snprintf(cdst, sizeof(cdst), "%s/%s", dst, de->d_name) >= (int)sizeof(cdst))
            {
                ok = False;
                break;
            }
            if (!copy_tree_noreplace(csrc, cdst, depth + 1))
            {
                ok = False;
                break;
            }
        }
        closedir(dp);
        (void)chmod(dst, st.st_mode & 07777);
        struct timespec ts[2] = {st.st_atim, st.st_mtim};
        (void)utimensat(AT_FDCWD, dst, ts, AT_SYMLINK_NOFOLLOW);
        return ok;
    }

    if (S_ISREG(st.st_mode))
    {
        int sfd = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (sfd < 0)
            return False;
        int dfd = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                       st.st_mode & 0777);
        if (dfd < 0)
        {
            close(sfd);
            return False;
        }
        unsigned char buf[65536];
        Boolean ok = True;
        for (;;)
        {
            ssize_t r = read(sfd, buf, sizeof(buf));
            if (r < 0)
            {
                if (errno == EINTR) continue;
                ok = False;
                break;
            }
            if (r == 0)
                break;
            ssize_t off = 0;
            while (off < r)
            {
                ssize_t w = write(dfd, buf + off, (size_t)(r - off));
                if (w < 0)
                {
                    if (errno == EINTR) continue;
                    ok = False;
                    break;
                }
                off += w;
            }
            if (!ok)
                break;
        }
        if (ok)
        {
            (void)fchmod(dfd, st.st_mode & 0777);
            struct timespec ts[2] = {st.st_atim, st.st_mtim};
            (void)futimens(dfd, ts);
        }
        close(dfd);
        close(sfd);
        if (!ok)
            unlink(dst);
        return ok;
    }

    return False;
}

static Boolean
remove_copied_source_tree(const char *path, int depth)
{
    if (!path || depth > 256)
        return False;
    struct stat st;
    if (lstat(path, &st) != 0)
        return False;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
    {
        DIR *dp = opendir(path);
        if (!dp)
            return False;
        struct dirent *de;
        Boolean ok = True;
        while ((de = readdir(dp)) != nullptr)
        {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char child[PATH_MAX];
            if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child) ||
                !remove_copied_source_tree(child, depth + 1))
            {
                ok = False;
                break;
            }
        }
        closedir(dp);
        if (!ok)
            return False;
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

static Boolean
move_path_noreplace(const char *src, const char *dst)
{
    if (!src || !dst)
        return False;
    struct stat st;
    if (lstat(dst, &st) == 0)
        return False;
    if (errno != ENOENT)
        return False;

#ifdef RENAME_NOREPLACE
    if (renameat2(AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE) == 0)
        return True;
    if (errno != EINVAL && errno != ENOSYS && errno != EXDEV)
        return False;
#endif
    if (rename(src, dst) == 0)
        return True;
    if (errno == EXDEV)
    {
        if (copy_tree_noreplace(src, dst, 0))
            return remove_copied_source_tree(src, 0);
    }
    return False;
}

static Boolean
is_same_or_descendant_dir(const char *src_dir, const char *dst_dir)
{
    char rsrc[PATH_MAX], rdst[PATH_MAX];
    const char *a = realpath(src_dir, rsrc) ? rsrc : src_dir;
    const char *b = realpath(dst_dir, rdst) ? rdst : dst_dir;
    size_t alen = strlen(a);
    while (alen > 1 && a[alen - 1] == '/')
        --alen;
    if (strncmp(a, b, alen) != 0)
        return False;
    return b[alen] == '\0' || b[alen] == '/';
}

static Boolean
make_unique_copy_dest(const char *dst_dir, const char *base_name, char *out_path, size_t cap)
{
    if (snprintf(out_path, cap, "%s/%s",
                 strcmp(dst_dir, "/") == 0 ? "" : dst_dir, base_name) >= (int)cap)
        return False;
    struct stat st;
    if (lstat(out_path, &st) != 0 && errno == ENOENT)
        return True;

    const char *dot = strrchr(base_name, '.');
    char stem[NAME_MAX + 1];
    char ext[NAME_MAX + 1];
    if (dot && dot != base_name)
    {
        size_t slen = (size_t)(dot - base_name);
        if (slen >= sizeof(stem)) slen = sizeof(stem) - 1;
        memcpy(stem, base_name, slen);
        stem[slen] = '\0';
        snprintf(ext, sizeof(ext), "%s", dot);
    }
    else
    {
        snprintf(stem, sizeof(stem), "%s", base_name);
        ext[0] = '\0';
    }

    for (int n = 1; n <= 999; ++n)
    {
        if (n == 1)
        {
            if (snprintf(out_path, cap, "%s/%s (copy)%s",
                         strcmp(dst_dir, "/") == 0 ? "" : dst_dir, stem, ext) >= (int)cap)
                return False;
        }
        else
        {
            if (snprintf(out_path, cap, "%s/%s (copy %d)%s",
                         strcmp(dst_dir, "/") == 0 ? "" : dst_dir, stem, n, ext) >= (int)cap)
                return False;
        }
        if (lstat(out_path, &st) != 0 && errno == ENOENT)
            return True;
    }
    return False;
}

static Boolean
restore_single_trashed_item(const char *orig_path)
{
    if (!orig_path || !*orig_path)
        return False;

    (void)DtfileXdgTrashReconcileNow();

    int match_idx = -1;
    int total = DtfileTrashBackendCount();
    const char *payload_c = nullptr;
    const char *orig_c = nullptr;
    const char *leaf_c = nullptr;

    /* Search from newest to oldest */
    for (int i = total - 1; i >= 0; --i)
    {
        const char *p = nullptr, *o = nullptr, *l = nullptr;
        if (DtfileTrashBackendGet(i, &p, &o, &l) && o && p && strcmp(o, orig_path) == 0)
        {
            struct stat pst;
            if (lstat(p, &pst) == 0)
            {
                match_idx = i;
                payload_c = p;
                orig_c = o;
                leaf_c = l;
                break;
            }
        }
    }

    if (match_idx >= 0 && payload_c)
    {
        char *payload = XtNewString((char *)payload_c);
        char *leaf = leaf_c ? XtNewString((char *)leaf_c) : nullptr;
        (void)orig_c;

        Boolean ok = move_path_noreplace(payload, orig_path);
        if (ok)
        {
            /* Remove companion XDG .trashinfo if payload lived under .../files/<name> */
            const char *files_seg = strstr(payload, "/files/");
            if (files_seg)
            {
                size_t prefix_len = (size_t)(files_seg - payload);
                const char *tname = files_seg + 7;
                if (*tname && !strchr(tname, '/'))
                {
                    char info_path[PATH_MAX];
                    if (snprintf(info_path, sizeof(info_path), "%.*s/info/%s.trashinfo",
                                 (int)prefix_len, payload, tname) < (int)sizeof(info_path))
                    {
                        unlink(info_path);
                    }
                }
            }
            if (leaf && trash_dir)
            {
                char dt_proxy[PATH_MAX];
                if (snprintf(dt_proxy, sizeof(dt_proxy), "%s/%s", trash_dir, leaf) < (int)sizeof(dt_proxy))
                {
                    struct stat lst;
                    if (lstat(dt_proxy, &lst) == 0 && S_ISLNK(lst.st_mode))
                        unlink(dt_proxy);
                }
            }
            DtfileTrashBackendRemoveAt(match_idx);
            DtfileTrashBackendCommit(True);
        }

        XtFree(payload);
        XtFree(leaf);
        return ok;
    }

    /* Fallback when dtfile runs standalone without TrashInitialized:
     * scan $XDG_DATA_HOME/Trash/info (or $HOME/.local/share/Trash/info). */
    char xdg_base[PATH_MAX];
    const char *xdg_data = getenv("XDG_DATA_HOME");
    const char *home_env = getenv("HOME");
    if (xdg_data && xdg_data[0] == '/')
        snprintf(xdg_base, sizeof(xdg_base), "%s/Trash", xdg_data);
    else if (home_env && home_env[0] == '/')
        snprintf(xdg_base, sizeof(xdg_base), "%s/.local/share/Trash", home_env);
    else
        return False;

    char info_dir[PATH_MAX];
    if (snprintf(info_dir, sizeof(info_dir), "%s/info", xdg_base) >= (int)sizeof(info_dir))
        return False;

    DIR *dp = opendir(info_dir);
    if (!dp)
        return False;

    char best_payload[PATH_MAX] = "";
    char best_info[PATH_MAX] = "";
    char best_stem[256] = "";
    char best_date[64] = "";

    struct dirent *de = nullptr;
    while ((de = readdir(dp)) != nullptr)
    {
        size_t nlen = strlen(de->d_name);
        if (nlen <= 10 || strcmp(de->d_name + nlen - 10, ".trashinfo") != 0)
            continue;
        size_t slen = nlen - 10;
        if (slen >= sizeof(best_stem))
            continue;

        char stem[256];
        memcpy(stem, de->d_name, slen);
        stem[slen] = '\0';

        char ipath[PATH_MAX], fpath[PATH_MAX];
        if (snprintf(ipath, sizeof(ipath), "%s/info/%s", xdg_base, de->d_name) >= (int)sizeof(ipath) ||
            snprintf(fpath, sizeof(fpath), "%s/files/%s", xdg_base, stem) >= (int)sizeof(fpath))
            continue;

        struct stat fst;
        if (lstat(fpath, &fst) != 0)
            continue;

        FILE *fp = fopen(ipath, "r");
        if (!fp)
            continue;
        DtfileXdgTrashInfo tinfo = {nullptr, nullptr};
        int rrc = DtfileReadXdgTrashInfo(fp, "/", &tinfo);
        if (fclose(fp) == 0 && rrc == DTFILE_XDG_INFO_OK && tinfo.path &&
            strcmp(tinfo.path, orig_path) == 0)
        {
            const char *dd = tinfo.deletion_date ? tinfo.deletion_date : "";
            if (!best_payload[0] || strcmp(dd, best_date) >= 0)
            {
                snprintf(best_payload, sizeof(best_payload), "%s", fpath);
                snprintf(best_info, sizeof(best_info), "%s", ipath);
                snprintf(best_stem, sizeof(best_stem), "%s", stem);
                snprintf(best_date, sizeof(best_date), "%s", dd);
            }
        }
        DtfileFreeXdgTrashInfo(&tinfo);
    }
    closedir(dp);

    if (!best_payload[0])
        return False;

    Boolean ok = move_path_noreplace(best_payload, orig_path);
    if (ok)
    {
        unlink(best_info);
        if (trash_dir && best_stem[0])
        {
            char dt_proxy[PATH_MAX];
            if (snprintf(dt_proxy, sizeof(dt_proxy), "%s/%s", trash_dir, best_stem) < (int)sizeof(dt_proxy))
            {
                struct stat lst;
                if (lstat(dt_proxy, &lst) == 0 && S_ISLNK(lst.st_mode))
                    unlink(dt_proxy);
            }
        }
    }
    return ok;
}

Boolean
DtfileUndoExecuteLast(FileMgrData *active_fmd)
{
    if (g_undo_len <= 0)
    {
        set_view_status_msg(active_fmd, "Nothing to undo");
        return False;
    }

    DtfileUndoEntry entry = g_undo_stack[--g_undo_len];
    memset(&g_undo_stack[g_undo_len], 0, sizeof(DtfileUndoEntry));

    g_undo_in_progress = True;
    Boolean all_ok = True;

    switch (entry.kind)
    {
    case DTFILE_UNDO_RENAME:
        if (entry.count >= 1 && entry.src_paths && entry.dst_paths)
        {
            if (move_path_noreplace(entry.dst_paths[0], entry.src_paths[0]))
            {
                char pdir[PATH_MAX];
                extract_parent_dir(entry.src_paths[0], pdir, sizeof(pdir));
                refresh_dir_views(pdir);
            }
            else
            {
                all_ok = False;
            }
        }
        break;

    case DTFILE_UNDO_MOVE:
        for (int i = entry.count - 1; i >= 0; --i)
        {
            if (move_path_noreplace(entry.dst_paths[i], entry.src_paths[i]))
            {
                char sdir[PATH_MAX], ddir[PATH_MAX];
                extract_parent_dir(entry.src_paths[i], sdir, sizeof(sdir));
                extract_parent_dir(entry.dst_paths[i], ddir, sizeof(ddir));
                refresh_dir_views(sdir);
                if (strcmp(sdir, ddir) != 0)
                    refresh_dir_views(ddir);
            }
            else
            {
                all_ok = False;
            }
        }
        break;

    case DTFILE_UNDO_COPY:
        for (int i = entry.count - 1; i >= 0; --i)
        {
            const char *created = entry.dst_paths[i];
            if (!created)
                continue;
            char *payload_out = nullptr;
            if (trash_dir &&
                DtfileXdgTrashMoveItem(created, path_basename_cstr(created), trash_dir, &payload_out))
            {
                XtFree(payload_out);
                (void)DtfileXdgTrashReconcileNow();
            }
            else if (TrashIsInitialized())
            {
                const char *one[1] = {created};
                DtfileTrashBackendMovePaths(one, 1);
            }
            else
            {
                all_ok = False;
            }
            char ddir[PATH_MAX];
            extract_parent_dir(created, ddir, sizeof(ddir));
            refresh_dir_views(ddir);
        }
        break;

    case DTFILE_UNDO_TRASH:
        for (int i = entry.count - 1; i >= 0; --i)
        {
            const char *orig = entry.src_paths[i];
            if (restore_single_trashed_item(orig))
            {
                char odir[PATH_MAX];
                extract_parent_dir(orig, odir, sizeof(odir));
                refresh_dir_views(odir);
            }
            else
            {
                all_ok = False;
            }
        }
        break;

    default:
        all_ok = False;
        break;
    }

    g_undo_in_progress = False;

    char msg[320];
    snprintf(msg, sizeof(msg), "%s: %s",
             all_ok ? "Undid" : "Undo incomplete", entry.summary);
    set_view_status_msg(active_fmd, msg);
    free_undo_entry(&entry);
    DtfileErgonomicsPublishState(active_fmd);
    return all_ok;
}

/* --- X11 / Wayland CLIPBOARD Selection Owner & Requestor --- */

static Boolean
ClipboardConvertProc(
    Widget w,
    Atom *selection,
    Atom *target,
    Atom *type_return,
    XtPointer *value_return,
    unsigned long *length_return,
    int *format_return)
{
    (void)selection;
    if (!g_clip_owned || g_clip_count <= 0 || !g_clip_paths)
        return False;

    Display *dpy = XtDisplay(w);
    Atom a_targets = XInternAtom(dpy, "TARGETS", False);
    Atom a_timestamp = XInternAtom(dpy, "TIMESTAMP", False);
    Atom a_gnome = XInternAtom(dpy, "x-special/gnome-copied-files", False);
    Atom a_uri_list = XInternAtom(dpy, "text/uri-list", False);
    Atom a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom a_plain_utf8 = XInternAtom(dpy, "text/plain;charset=utf-8", False);

    if (*target == a_targets)
    {
        Atom *tlist = (Atom *)XtMalloc(7 * sizeof(Atom));
        tlist[0] = a_targets;
        tlist[1] = a_timestamp;
        tlist[2] = a_gnome;
        tlist[3] = a_uri_list;
        tlist[4] = a_utf8;
        tlist[5] = a_plain_utf8;
        tlist[6] = XA_STRING;
        *type_return = XA_ATOM;
        *value_return = (XtPointer)tlist;
        *length_return = 7;
        *format_return = 32;
        return True;
    }

    if (*target == a_timestamp)
    {
        Time *ts = (Time *)XtMalloc(sizeof(Time));
        *ts = g_clip_timestamp;
        *type_return = XA_INTEGER;
        *value_return = (XtPointer)ts;
        *length_return = 1;
        *format_return = 32;
        return True;
    }

    if (*target == a_gnome || *target == a_uri_list)
    {
        Boolean with_header = (*target == a_gnome);
        size_t total = with_header ? 8 : 0;
        char **uris = (char **)XtCalloc((Cardinal)g_clip_count, sizeof(char *));
        for (int i = 0; i < g_clip_count; ++i)
        {
            uris[i] = DtfilePathToFileUri(g_clip_paths[i]);
            if (uris[i])
                total += strlen(uris[i]) + 2;
        }
        char *payload = (char *)XtMalloc((Cardinal)(total + 1));
        size_t off = 0;
        if (with_header)
        {
            const char *hdr = g_clip_is_cut ? "cut\n" : "copy\n";
            size_t hlen = strlen(hdr);
            memcpy(payload + off, hdr, hlen);
            off += hlen;
        }
        for (int i = 0; i < g_clip_count; ++i)
        {
            if (!uris[i])
                continue;
            size_t ulen = strlen(uris[i]);
            memcpy(payload + off, uris[i], ulen);
            off += ulen;
            if (with_header)
            {
                if (i + 1 < g_clip_count)
                    payload[off++] = '\n';
            }
            else
            {
                payload[off++] = '\r';
                payload[off++] = '\n';
            }
        }
        payload[off] = '\0';
        DtfileFreeStringArray(uris, g_clip_count);

        *type_return = *target;
        *value_return = (XtPointer)payload;
        *length_return = (unsigned long)off;
        *format_return = 8;
        return True;
    }

    if (*target == a_utf8 || *target == a_plain_utf8 || *target == XA_STRING)
    {
        size_t total = 0;
        for (int i = 0; i < g_clip_count; ++i)
            total += strlen(g_clip_paths[i]) + 1;
        char *payload = (char *)XtMalloc((Cardinal)(total + 1));
        size_t off = 0;
        for (int i = 0; i < g_clip_count; ++i)
        {
            size_t plen = strlen(g_clip_paths[i]);
            memcpy(payload + off, g_clip_paths[i], plen);
            off += plen;
            if (i + 1 < g_clip_count)
                payload[off++] = '\n';
        }
        payload[off] = '\0';
        *type_return = *target;
        *value_return = (XtPointer)payload;
        *length_return = (unsigned long)off;
        *format_return = 8;
        return True;
    }

    return False;
}

static void
ClipboardLoseProc(Widget w, Atom *selection)
{
    (void)w;
    (void)selection;
    g_clip_owned = False;
    DtfileErgonomicsPublishState(nullptr);
}

Boolean
DtfileClipboardHasLocalOwnership(Boolean *out_is_cut, int *out_count)
{
    if (out_is_cut) *out_is_cut = g_clip_owned ? g_clip_is_cut : False;
    if (out_count) *out_count = g_clip_owned ? g_clip_count : 0;
    return g_clip_owned;
}

Boolean
DtfileClipboardCopyOrCutSelection(
    FileMgrData *fmd,
    Boolean is_cut,
    Time event_time)
{
    if (!fmd || fmd == trashFileMgrData || !g_clip_widget)
        return False;

    int sel_count = fmd->selected_file_count;
    FileViewData *single_popup = nullptr;
    if (sel_count <= 0 && fmd->popup_menu_icon)
    {
        single_popup = fmd->popup_menu_icon;
        sel_count = 1;
    }
    if (sel_count <= 0)
        return False;

    char **paths = (char **)XtCalloc((Cardinal)sel_count, sizeof(char *));
    int valid = 0;
    for (int i = 0; i < sel_count; ++i)
    {
        FileViewData *fvd = single_popup ? single_popup : fmd->selection_list[i];
        if (!fvd || !fvd->file_data || !fvd->file_data->file_name)
            continue;
        const char *fname = fvd->file_data->file_name;
        if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0)
            continue;
        DirectorySet *dset = (DirectorySet *)fvd->directory_set;
        const char *dir = (dset && dset->name) ? dset->name : fmd->current_directory;
        if (!dir || dir[0] != '/')
            continue;
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s",
                     strcmp(dir, "/") == 0 ? "" : dir, fname) >= (int)sizeof(full))
            continue;
        paths[valid++] = XtNewString(full);
    }

    if (valid == 0)
    {
        XtFree((char *)paths);
        return False;
    }

    DtfileFreeStringArray(g_clip_paths, g_clip_count);
    g_clip_paths = paths;
    g_clip_count = valid;
    g_clip_is_cut = is_cut;
    if (event_time == CurrentTime && XtDisplay(g_clip_widget))
        event_time = XtLastTimestampProcessed(XtDisplay(g_clip_widget));
    g_clip_timestamp = event_time;

    Atom a_clip = XInternAtom(XtDisplay(g_clip_widget), "CLIPBOARD", False);
    g_clip_owned = XtOwnSelection(g_clip_widget, a_clip, event_time,
                                  ClipboardConvertProc, ClipboardLoseProc, nullptr);

    char msg[256];
    snprintf(msg, sizeof(msg), "%s %d item%s to clipboard",
             is_cut ? "Cut" : "Copied", valid, valid == 1 ? "" : "s");
    set_view_status_msg(fmd, msg);
    DtfileErgonomicsPublishState(fmd);
    return g_clip_owned;
}

static void
execute_paste_paths(FileMgrData *fmd, Boolean is_cut, char **paths, int count)
{
    if (!fmd || !fmd->current_directory || fmd == trashFileMgrData || count <= 0)
        return;

    const char *dst_dir = fmd->current_directory;
    char **ok_srcs = (char **)XtCalloc((Cardinal)count, sizeof(char *));
    char **ok_dsts = (char **)XtCalloc((Cardinal)count, sizeof(char *));
    int ok_count = 0;

    for (int i = 0; i < count; ++i)
    {
        const char *src = paths[i];
        struct stat st;
        if (!src || lstat(src, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) &&
            is_same_or_descendant_dir(src, dst_dir))
        {
            continue;
        }

        const char *base = path_basename_cstr(src);
        char dst[PATH_MAX];
        if (is_cut)
        {
            if (snprintf(dst, sizeof(dst), "%s/%s",
                         strcmp(dst_dir, "/") == 0 ? "" : dst_dir, base) >= (int)sizeof(dst))
                continue;
            if (strcmp(src, dst) == 0)
                continue;
            if (move_path_noreplace(src, dst))
            {
                ok_srcs[ok_count] = XtNewString((char *)src);
                ok_dsts[ok_count] = XtNewString(dst);
                ++ok_count;
                char sdir[PATH_MAX];
                extract_parent_dir(src, sdir, sizeof(sdir));
                if (strcmp(sdir, dst_dir) != 0)
                    refresh_dir_views(sdir);
            }
        }
        else
        {
            if (!make_unique_copy_dest(dst_dir, base, dst, sizeof(dst)))
                continue;
            if (copy_tree_noreplace(src, dst, 0))
            {
                ok_srcs[ok_count] = XtNewString((char *)src);
                ok_dsts[ok_count] = XtNewString(dst);
                ++ok_count;
            }
        }
    }

    if (ok_count > 0)
    {
        if (is_cut)
        {
            DtfileUndoRecordMoveBatch((const char *const *)ok_srcs,
                                      (const char *const *)ok_dsts, ok_count);
            if (g_clip_owned && g_clip_is_cut)
            {
                Atom a_clip = XInternAtom(XtDisplay(g_clip_widget), "CLIPBOARD", False);
                XtDisownSelection(g_clip_widget, a_clip, CurrentTime);
                g_clip_owned = False;
                DtfileFreeStringArray(g_clip_paths, g_clip_count);
                g_clip_paths = nullptr;
                g_clip_count = 0;
            }
        }
        else
        {
            DtfileUndoRecordCopyBatch((const char *const *)ok_dsts, ok_count);
        }

        refresh_dir_views(dst_dir);
        char msg[256];
        snprintf(msg, sizeof(msg), "Pasted %d item%s", ok_count, ok_count == 1 ? "" : "s");
        set_view_status_msg(fmd, msg);
    }

    DtfileFreeStringArray(ok_srcs, ok_count);
    DtfileFreeStringArray(ok_dsts, ok_count);
    DtfileErgonomicsPublishState(fmd);
}

typedef struct {
    FileMgrData *fmd;
    Time time;
} PasteReqCtx;

static void
PasteUriListSelectionCB(
    Widget w,
    XtPointer client_data,
    Atom *selection,
    Atom *type,
    XtPointer value,
    unsigned long *length,
    int *format)
{
    (void)w;
    (void)selection;
    PasteReqCtx *ctx = (PasteReqCtx *)client_data;
    if (value != nullptr && *type != XT_CONVERT_FAIL && *format == 8 && *length > 0)
    {
        Boolean is_cut = False;
        char **paths = nullptr;
        int count = 0;
        if (DtfileParseClipboardPayload("text/uri-list", (const char *)value,
                                        (size_t)*length, &is_cut, &paths, &count))
        {
            execute_paste_paths(ctx->fmd, False, paths, count);
            DtfileFreeStringArray(paths, count);
        }
    }
    XtFree((char *)value);
    XtFree((char *)ctx);
}

static void
PasteGnomeSelectionCB(
    Widget w,
    XtPointer client_data,
    Atom *selection,
    Atom *type,
    XtPointer value,
    unsigned long *length,
    int *format)
{
    (void)selection;
    PasteReqCtx *ctx = (PasteReqCtx *)client_data;
    if (value != nullptr && *type != XT_CONVERT_FAIL && *format == 8 && *length > 0)
    {
        Boolean is_cut = False;
        char **paths = nullptr;
        int count = 0;
        if (DtfileParseClipboardPayload("x-special/gnome-copied-files",
                                        (const char *)value, (size_t)*length,
                                        &is_cut, &paths, &count))
        {
            execute_paste_paths(ctx->fmd, is_cut, paths, count);
            DtfileFreeStringArray(paths, count);
            XtFree((char *)value);
            XtFree((char *)ctx);
            return;
        }
    }
    XtFree((char *)value);

    /* Fallback to standard text/uri-list (copy semantics) */
    Display *dpy = XtDisplay(w);
    Atom a_clip = XInternAtom(dpy, "CLIPBOARD", False);
    Atom a_uri = XInternAtom(dpy, "text/uri-list", False);
    XtGetSelectionValue(w, a_clip, a_uri, PasteUriListSelectionCB,
                        (XtPointer)ctx, ctx->time);
}

void
DtfileClipboardPasteIntoView(FileMgrData *fmd, Time event_time)
{
    if (!fmd || fmd == trashFileMgrData || !g_clip_widget)
        return;

    /* Fast path if this dtfile process currently owns CLIPBOARD */
    if (g_clip_owned && g_clip_count > 0 && g_clip_paths)
    {
        int cnt = g_clip_count;
        Boolean is_cut = g_clip_is_cut;
        char **copy_paths = (char **)XtCalloc((Cardinal)cnt, sizeof(char *));
        for (int i = 0; i < cnt; ++i)
            copy_paths[i] = XtNewString(g_clip_paths[i]);
        execute_paste_paths(fmd, is_cut, copy_paths, cnt);
        DtfileFreeStringArray(copy_paths, cnt);
        return;
    }

    if (event_time == CurrentTime && XtDisplay(g_clip_widget))
        event_time = XtLastTimestampProcessed(XtDisplay(g_clip_widget));

    PasteReqCtx *ctx = (PasteReqCtx *)XtMalloc(sizeof(PasteReqCtx));
    ctx->fmd = fmd;
    ctx->time = event_time;

    Display *dpy = XtDisplay(g_clip_widget);
    Atom a_clip = XInternAtom(dpy, "CLIPBOARD", False);
    Atom a_gnome = XInternAtom(dpy, "x-special/gnome-copied-files", False);
    XtGetSelectionValue(g_clip_widget, a_clip, a_gnome,
                        PasteGnomeSelectionCB, (XtPointer)ctx, event_time);
}

static void
on_fileop_rename_notify(const char *old_path, const char *new_path)
{
    DtfileUndoRecordRename(old_path, new_path);
}

static void
on_fileop_transfer_notify(int mode, const char *src_path, const char *dst_path)
{
    if (g_undo_in_progress || !dst_path)
        return;
    if (mode == MOVE_FILE && src_path)
    {
        const char *s[1] = {src_path};
        const char *d[1] = {dst_path};
        DtfileUndoRecordMoveBatch(s, d, 1);
    }
    else if (mode == COPY_FILE)
    {
        const char *d[1] = {dst_path};
        DtfileUndoRecordCopyBatch(d, 1);
    }
}

void
DtfileClipboardUndoInit(Widget toplevel_widget)
{
    g_clip_widget = toplevel_widget;
    DtfileUndoRenameNotify = on_fileop_rename_notify;
    DtfileUndoTransferNotify = on_fileop_transfer_notify;
}

static FileMgrData *
menu_widget_to_fmd(Widget w, XtPointer client_data)
{
    FileMgrRec *fmr = nullptr;
    Widget mbar = nullptr;
    if (client_data != nullptr)
        mbar = XtParent(w);
    else
        mbar = XmGetPostedFromWidget(XtParent(w));
    if (!mbar)
        return nullptr;
    XtVaGetValues(mbar, XmNuserData, &fmr, nullptr);
    if (!fmr)
        return nullptr;
    DialogData *dd = _DtGetInstanceData((XtPointer)fmr);
    return dd ? (FileMgrData *)dd->data : nullptr;
}

void
DtfileCopyMenuCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    XmAnyCallbackStruct *cbs = (XmAnyCallbackStruct *)call_data;
    Time t = (cbs && cbs->event && cbs->event->type == ButtonRelease)
                 ? cbs->event->xbutton.time
                 : CurrentTime;
    FileMgrData *fmd = menu_widget_to_fmd(w, client_data);
    if (fmd)
        (void)DtfileClipboardCopyOrCutSelection(fmd, False, t);
}

void
DtfileCutMenuCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    XmAnyCallbackStruct *cbs = (XmAnyCallbackStruct *)call_data;
    Time t = (cbs && cbs->event && cbs->event->type == ButtonRelease)
                 ? cbs->event->xbutton.time
                 : CurrentTime;
    FileMgrData *fmd = menu_widget_to_fmd(w, client_data);
    if (fmd)
        (void)DtfileClipboardCopyOrCutSelection(fmd, True, t);
}

void
DtfilePasteMenuCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    XmAnyCallbackStruct *cbs = (XmAnyCallbackStruct *)call_data;
    Time t = (cbs && cbs->event && cbs->event->type == ButtonRelease)
                 ? cbs->event->xbutton.time
                 : CurrentTime;
    FileMgrData *fmd = menu_widget_to_fmd(w, client_data);
    if (fmd)
        DtfileClipboardPasteIntoView(fmd, t);
}

void
DtfileUndoMenuCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    FileMgrData *fmd = menu_widget_to_fmd(w, client_data);
    (void)DtfileUndoExecuteLast(fmd);
}

#endif /* !DTFILE_CLIPBOARD_UNIT_TEST */
