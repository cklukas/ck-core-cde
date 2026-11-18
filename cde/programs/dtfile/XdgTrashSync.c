/*
 * XdgTrashSync.c - Live Bidirectional XDG Trash (inotify) & Per-Mount .Trash-$UID
 *                  Integration for CK-Core CDE File Manager (WP-16).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/Xm.h>

#include "Encaps.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "XdgTrashInfo.h"
#include "XdgTrashSync.h"

#define MAX_TRASH_REPOS   32
#define MAX_INOTIFY_WATCH 96

typedef struct {
    char trash_root[PATH_MAX]; /* e.g. ~/.local/share/Trash or /media/usb/.Trash-1000 */
    char mount_base[PATH_MAX]; /* original_base for relative Path= in .trashinfo */
    bool is_home_trash;
} XdgTrashRepo;

static Widget g_trash_toplevel = nullptr;
static char g_dt_trash_dir[PATH_MAX];
static int g_inotify_fd = -1;
static XtInputId g_inotify_input = 0;
static XtIntervalId g_debounce_timer = 0;
static int g_watch_fds[MAX_INOTIFY_WATCH];
static int g_watch_count = 0;
static bool g_reconciling = false;

static void current_iso_deletion_date(char out[32]) {
    time_t now = time(nullptr);
    struct tm tm_buf;
    if (localtime_r(&now, &tm_buf) &&
        strftime(out, 32, "%Y-%m-%dT%H:%M:%S", &tm_buf) > 0) {
        return;
    }
    snprintf(out, 32, "1970-01-01T00:00:00");
}

static int is_trashinfo_unescaped(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    switch (c) {
        case '-': case '_': case '.': case '~': case '/':
            return 1;
        default:
            return 0;
    }
}

Boolean DtfileFormatXdgTrashInfoContent(
    const char *raw_path,
    const char *deletion_date,
    char *buf_out,
    size_t buf_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!raw_path || !raw_path[0] || !buf_out || buf_sz < 64)
        return False;

    char date_local[32];
    if (!deletion_date || !deletion_date[0]) {
        current_iso_deletion_date(date_local);
        deletion_date = date_local;
    }

    char enc[PATH_MAX * 3 + 4];
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)raw_path; *p; ++p) {
        if (is_trashinfo_unescaped(*p)) {
            if (pos + 2 > sizeof(enc)) return False;
            enc[pos++] = (char)*p;
        } else {
            if (pos + 4 > sizeof(enc)) return False;
            enc[pos++] = '%';
            enc[pos++] = hex[(*p >> 4) & 0x0FU];
            enc[pos++] = hex[*p & 0x0FU];
        }
    }
    enc[pos] = '\0';

    int n = snprintf(buf_out, buf_sz,
                     "[Trash Info]\nPath=%s\nDeletionDate=%s\n",
                     enc, deletion_date);
    return (n > 0 && (size_t)n < buf_sz) ? True : False;
}

static bool ensure_safe_private_dir(const char *dir_path) {
    if (!dir_path || dir_path[0] != '/')
        return false;
    if (mkdir(dir_path, 0700) != 0 && errno != EEXIST)
        return false;
    struct stat st;
    if (lstat(dir_path, &st) != 0)
        return false;
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
        return false;
    if (st.st_uid != getuid())
        return false;
    return true;
}

static bool ensure_xdg_trash_repo_dirs(const char *trash_root) {
    if (!ensure_safe_private_dir(trash_root))
        return false;
    char sub[PATH_MAX];
    if (snprintf(sub, sizeof(sub), "%s/files", trash_root) >= (int)sizeof(sub) ||
        !ensure_safe_private_dir(sub))
        return false;
    if (snprintf(sub, sizeof(sub), "%s/info", trash_root) >= (int)sizeof(sub) ||
        !ensure_safe_private_dir(sub))
        return false;
    return true;
}

static void get_home_xdg_trash_root(char *trash_root_out, size_t tr_sz,
                                    char *base_out, size_t base_sz) {
    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] == '/') {
        size_t xlen = strlen(xdg_data);
        while (xlen > 1 && xdg_data[xlen - 1] == '/')
            xlen--;
        snprintf(base_out, base_sz, "%.*s", (int)xlen, xdg_data);
        snprintf(trash_root_out, tr_sz, "%.*s/Trash", (int)xlen, xdg_data);
        return;
    }

    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = users_home_dir;
    size_t hlen = home ? strlen(home) : 0;
    while (hlen > 1 && home[hlen - 1] == '/')
        hlen--;
    snprintf(base_out, base_sz, "%.*s/.local/share", (int)hlen, home);
    snprintf(trash_root_out, tr_sz, "%.*s/.local/share/Trash", (int)hlen, home);
}

static void ensure_parent_dirs_0700(const char *dir_path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

static bool is_skip_pseudo_fs(const char *fstype, const char *mnt_dir) {
    if (!mnt_dir || mnt_dir[0] != '/')
        return true;
    if (strcmp(mnt_dir, "/") == 0 ||
        strncmp(mnt_dir, "/proc", 5) == 0 ||
        strncmp(mnt_dir, "/sys", 4) == 0 ||
        strncmp(mnt_dir, "/dev", 4) == 0 ||
        strncmp(mnt_dir, "/boot", 5) == 0)
        return true;
    if (!fstype)
        return true;
    if (strcmp(fstype, "proc") == 0 || strcmp(fstype, "sysfs") == 0 ||
        strcmp(fstype, "devtmpfs") == 0 || strcmp(fstype, "devpts") == 0 ||
        strcmp(fstype, "cgroup") == 0 || strcmp(fstype, "cgroup2") == 0 ||
        strcmp(fstype, "pstore") == 0 || strcmp(fstype, "securityfs") == 0 ||
        strcmp(fstype, "debugfs") == 0 || strcmp(fstype, "tracefs") == 0 ||
        strcmp(fstype, "configfs") == 0 || strcmp(fstype, "fusectl") == 0 ||
        strcmp(fstype, "mqueue") == 0 || strcmp(fstype, "hugetlbfs") == 0 ||
        strcmp(fstype, "autofs") == 0 || strcmp(fstype, "portal") == 0 ||
        strcmp(fstype, "fuse.portal") == 0)
        return true;
    return false;
}

static bool path_has_dir_prefix(const char *path, const char *prefix) {
    size_t plen = strlen(prefix);
    while (plen > 1 && prefix[plen - 1] == '/')
        plen--;
    if (plen == 1 && prefix[0] == '/')
        return path[0] == '/';
    if (strncmp(path, prefix, plen) != 0)
        return false;
    return (path[plen] == '\0' || path[plen] == '/');
}

/* Enumerate active Trash repositories:
 * [0] = $XDG_DATA_HOME/Trash (home XDG trash)
 * [1..N] = existing <mount>/.Trash-$UID repositories on mounted volumes + DTFILE_TRASH_MOUNT_ROOTS */
static int collect_trash_repos(XdgTrashRepo repos[MAX_TRASH_REPOS], bool create_home) {
    int count = 0;
    char home_tr[PATH_MAX], home_base[PATH_MAX];
    get_home_xdg_trash_root(home_tr, sizeof(home_tr), home_base, sizeof(home_base));
    if (create_home) {
        ensure_parent_dirs_0700(home_base);
        ensure_xdg_trash_repo_dirs(home_tr);
    }

    snprintf(repos[count].trash_root, sizeof(repos[count].trash_root), "%s", home_tr);
    snprintf(repos[count].mount_base, sizeof(repos[count].mount_base), "%s", home_base);
    repos[count].is_home_trash = true;
    count++;

    uid_t uid = getuid();

    /* Explicit test / user mount roots via DTFILE_TRASH_MOUNT_ROOTS (colon-separated) */
    const char *extra_roots = getenv("DTFILE_TRASH_MOUNT_ROOTS");
    if (extra_roots && extra_roots[0]) {
        char copy[PATH_MAX * 4];
        snprintf(copy, sizeof(copy), "%s", extra_roots);
        char *saveptr = nullptr;
        for (char *tok = strtok_r(copy, ":", &saveptr);
             tok && count < MAX_TRASH_REPOS;
             tok = strtok_r(nullptr, ":", &saveptr)) {
            if (tok[0] != '/') continue;
            size_t tlen = strlen(tok);
            while (tlen > 1 && tok[tlen - 1] == '/')
                tok[--tlen] = '\0';
            char tr[PATH_MAX];
            if (snprintf(tr, sizeof(tr), "%s/.Trash-%u", tok, (unsigned)uid) >= (int)sizeof(tr))
                continue;
            struct stat st;
            if (lstat(tr, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_uid == uid) {
                snprintf(repos[count].trash_root, sizeof(repos[count].trash_root), "%s", tr);
                snprintf(repos[count].mount_base, sizeof(repos[count].mount_base), "%s", tok);
                repos[count].is_home_trash = false;
                count++;
            }
        }
    }

    /* System mounts from /proc/mounts */
    FILE *fp = setmntent("/proc/mounts", "r");
    if (fp) {
        struct mntent *ent;
        while ((ent = getmntent(fp)) != nullptr && count < MAX_TRASH_REPOS) {
            if (is_skip_pseudo_fs(ent->mnt_type, ent->mnt_dir))
                continue;
            char tr[PATH_MAX];
            if (snprintf(tr, sizeof(tr), "%s/.Trash-%u", ent->mnt_dir, (unsigned)uid) >= (int)sizeof(tr))
                continue;
            bool dup = false;
            for (int i = 0; i < count; ++i) {
                if (strcmp(repos[i].trash_root, tr) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            struct stat st;
            if (lstat(tr, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_uid == uid) {
                snprintf(repos[count].trash_root, sizeof(repos[count].trash_root), "%s", tr);
                snprintf(repos[count].mount_base, sizeof(repos[count].mount_base), "%s", ent->mnt_dir);
                repos[count].is_home_trash = false;
                count++;
            }
        }
        endmntent(fp);
    }

    return count;
}

/* Determine whether `src_path` belongs to a separate mount root from home XDG trash */
static bool find_per_mount_root_for_path(
    const char *src_path,
    dev_t home_dev,
    char *mount_root_out,
    size_t mount_root_sz)
{
    mount_root_out[0] = '\0';
    size_t best_len = 0;

    /* 1. Check DTFILE_TRASH_MOUNT_ROOTS first (allows testing per-mount .Trash-$UID even on tmpfs/bind dirs!) */
    const char *extra_roots = getenv("DTFILE_TRASH_MOUNT_ROOTS");
    if (extra_roots && extra_roots[0]) {
        char copy[PATH_MAX * 4];
        snprintf(copy, sizeof(copy), "%s", extra_roots);
        char *saveptr = nullptr;
        for (char *tok = strtok_r(copy, ":", &saveptr);
             tok;
             tok = strtok_r(nullptr, ":", &saveptr)) {
            if (tok[0] != '/') continue;
            size_t tlen = strlen(tok);
            while (tlen > 1 && tok[tlen - 1] == '/')
                tok[--tlen] = '\0';
            if (path_has_dir_prefix(src_path, tok) && strcmp(src_path, tok) != 0 && tlen > best_len) {
                best_len = tlen;
                snprintf(mount_root_out, mount_root_sz, "%s", tok);
            }
        }
        if (best_len > 0)
            return true;
    }

    /* 2. Check st_dev against home_dev and find deepest matching mountpoint in /proc/mounts */
    struct stat src_st;
    if (lstat(src_path, &src_st) != 0 || src_st.st_dev == home_dev)
        return false;

    FILE *fp = setmntent("/proc/mounts", "r");
    if (!fp)
        return false;
    struct mntent *ent;
    while ((ent = getmntent(fp)) != nullptr) {
        if (is_skip_pseudo_fs(ent->mnt_type, ent->mnt_dir))
            continue;
        size_t mlen = strlen(ent->mnt_dir);
        if (path_has_dir_prefix(src_path, ent->mnt_dir) &&
            strcmp(src_path, ent->mnt_dir) != 0 &&
            mlen > best_len) {
            struct stat mst;
            if (stat(ent->mnt_dir, &mst) == 0 && mst.st_dev == src_st.st_dev) {
                best_len = mlen;
                snprintf(mount_root_out, mount_root_sz, "%s", ent->mnt_dir);
            }
        }
    }
    endmntent(fp);
    return best_len > 0;
}

/* Given a payload path `<repo>/files/<stem>`, derive `<repo>/info/<stem>.trashinfo` */
static bool payload_to_info_path(const char *payload_path, char *info_out, size_t info_sz) {
    if (!payload_path)
        return false;
    const char *marker = strstr(payload_path, "/files/");
    if (!marker)
        return false;
    const char *stem = marker + 7;
    if (!stem[0] || strchr(stem, '/') != nullptr)
        return false;
    size_t root_len = (size_t)(marker - payload_path);
    int n = snprintf(info_out, info_sz, "%.*s/info/%s.trashinfo", (int)root_len, payload_path, stem);
    return (n > 0 && (size_t)n < info_sz);
}

static bool choose_unique_dt_proxy_leaf(const char *dt_trash_dir, const char *base_name,
                                        char *leaf_out, size_t leaf_sz,
                                        char *proxy_path_out, size_t proxy_sz) {
    for (int idx = 1; idx < 10000; ++idx) {
        if (idx == 1)
            snprintf(leaf_out, leaf_sz, "%s", base_name);
        else
            snprintf(leaf_out, leaf_sz, "%s.%d", base_name, idx);

        if (strcmp(leaf_out, ".trashinfo") == 0 || leaf_out[0] == '\0')
            continue;

        int n = snprintf(proxy_path_out, proxy_sz, "%s/%s", dt_trash_dir, leaf_out);
        if (n <= 0 || (size_t)n >= proxy_sz)
            return false;

        struct stat st;
        if (lstat(proxy_path_out, &st) != 0 && errno == ENOENT)
            return true;
    }
    return false;
}

Boolean DtfileXdgTrashMoveItem(
    const char *src_path,
    const char *base_name,
    const char *dt_trash_dir,
    char **out_payload_path)
{
    if (out_payload_path)
        *out_payload_path = nullptr;
    if (!src_path || src_path[0] != '/' || !base_name || !base_name[0] ||
        !dt_trash_dir || dt_trash_dir[0] != '/' || !out_payload_path)
        return False;

    char home_tr[PATH_MAX], home_base[PATH_MAX];
    get_home_xdg_trash_root(home_tr, sizeof(home_tr), home_base, sizeof(home_base));
    ensure_parent_dirs_0700(home_base);
    ensure_xdg_trash_repo_dirs(home_tr);

    struct stat home_st;
    dev_t home_dev = 0;
    if (stat(home_tr, &home_st) == 0)
        home_dev = home_st.st_dev;

    char target_repo[PATH_MAX];
    char info_rel_or_abs[PATH_MAX];
    char mount_root[PATH_MAX];

    if (find_per_mount_root_for_path(src_path, home_dev, mount_root, sizeof(mount_root))) {
        /* Per-mount .Trash-$UID on external/removable filesystem */
        if (snprintf(target_repo, sizeof(target_repo), "%s/.Trash-%u",
                     mount_root, (unsigned)getuid()) >= (int)sizeof(target_repo))
            return False;
        if (!ensure_xdg_trash_repo_dirs(target_repo))
            return False;

        size_t mlen = strlen(mount_root);
        while (mlen > 1 && mount_root[mlen - 1] == '/')
            mlen--;
        const char *rel = src_path + mlen;
        while (*rel == '/')
            rel++;
        if (!rel[0])
            return False;
        snprintf(info_rel_or_abs, sizeof(info_rel_or_abs), "%s", rel);
    } else {
        /* Home XDG Trash ($XDG_DATA_HOME/Trash) */
        struct stat src_st;
        if (lstat(src_path, &src_st) != 0 || src_st.st_dev != home_dev)
            return False;
        snprintf(target_repo, sizeof(target_repo), "%s", home_tr);
        snprintf(info_rel_or_abs, sizeof(info_rel_or_abs), "%s", src_path);
    }

    char info_content[PATH_MAX * 3 + 128];
    if (!DtfileFormatXdgTrashInfoContent(info_rel_or_abs, nullptr,
                                         info_content, sizeof(info_content)))
        return False;

    /* Atomically reserve `<target_repo>/info/<unique>.trashinfo` with O_CREAT | O_EXCL */
    char chosen_stem[NAME_MAX + 1];
    char info_path[PATH_MAX];
    char payload_path[PATH_MAX];
    int info_fd = -1;

    for (int idx = 1; idx < 10000; ++idx) {
        if (idx == 1) {
            snprintf(chosen_stem, sizeof(chosen_stem), "%s", base_name);
        } else {
            const char *dot = strrchr(base_name, '.');
            if (dot && dot != base_name && strlen(dot) <= 10) {
                int sl = (int)(dot - base_name);
                snprintf(chosen_stem, sizeof(chosen_stem), "%.*s.%d%s", sl, base_name, idx, dot);
            } else {
                snprintf(chosen_stem, sizeof(chosen_stem), "%s.%d", base_name, idx);
            }
        }
        if (snprintf(info_path, sizeof(info_path), "%s/info/%s.trashinfo",
                     target_repo, chosen_stem) >= (int)sizeof(info_path) ||
            snprintf(payload_path, sizeof(payload_path), "%s/files/%s",
                     target_repo, chosen_stem) >= (int)sizeof(payload_path))
            return False;

        struct stat pst;
        if (lstat(payload_path, &pst) == 0 || errno != ENOENT)
            continue;

        info_fd = open(info_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (info_fd >= 0)
            break;
        if (errno != EEXIST)
            return False;
    }

    if (info_fd < 0)
        return False;

    size_t clen = strlen(info_content);
    const char *cp = info_content;
    while (clen > 0) {
        ssize_t wn = write(info_fd, cp, clen);
        if (wn < 0 && errno == EINTR)
            continue;
        if (wn <= 0) {
            close(info_fd);
            unlink(info_path);
            return False;
        }
        cp += wn;
        clen -= (size_t)wn;
    }
    fsync(info_fd);
    close(info_fd);

    /* Same-filesystem O(1) rename into `<target_repo>/files/<chosen_stem>` */
    if (rename(src_path, payload_path) != 0) {
        unlink(info_path);
        return False;
    }

    /* Create proxy symlink in `~/.dt/Trash/<chosen_stem>` so dtfile's Trash view renders it */
    char proxy_leaf[NAME_MAX + 1];
    char proxy_path[PATH_MAX];
    snprintf(proxy_path, sizeof(proxy_path), "%s/%s", dt_trash_dir, chosen_stem);
    struct stat ex_st;
    if (lstat(proxy_path, &ex_st) == 0) {
        /* If a stale dangling symlink occupied that leaf, replace or disambiguate */
        if (S_ISLNK(ex_st.st_mode)) {
            struct stat tgt_st;
            if (stat(proxy_path, &tgt_st) != 0 && errno == ENOENT)
                unlink(proxy_path);
        }
    }
    if (lstat(proxy_path, &ex_st) != 0 && errno == ENOENT) {
        (void)symlink(payload_path, proxy_path);
    } else if (choose_unique_dt_proxy_leaf(dt_trash_dir, chosen_stem,
                                           proxy_leaf, sizeof(proxy_leaf),
                                           proxy_path, sizeof(proxy_path))) {
        (void)symlink(payload_path, proxy_path);
    }

    *out_payload_path = XtNewString(payload_path);
    return True;
}

static bool parse_trashinfo_file(
    const char *info_path,
    const char *original_base,
    char **orig_out,
    char **date_out)
{
    *orig_out = nullptr;
    *date_out = nullptr;

    int fd = open(info_path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || (uintmax_t)st.st_size > DTFILE_XDG_INFO_FILE_LIMIT) {
        close(fd);
        return false;
    }
    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return false;
    }
    DtfileXdgTrashInfo rec = {0};
    int rc = DtfileReadXdgTrashInfo(fp, original_base, &rec);
    if (fclose(fp) != 0 && rc == DTFILE_XDG_INFO_OK)
        rc = DTFILE_XDG_INFO_IO_ERROR;

    bool ok = false;
    if (rc == DTFILE_XDG_INFO_OK && rec.path && rec.deletion_date) {
        *orig_out = XtNewString(rec.path);
        *date_out = XtNewString(rec.deletion_date);
        ok = true;
    }
    DtfileFreeXdgTrashInfo(&rec);
    return ok;
}

static void publish_trash_root_property(void) {
    if (!g_trash_toplevel)
        return;
    Display *dpy = XtDisplay(g_trash_toplevel);
    if (!dpy)
        return;
    Window root = DefaultRootWindow(dpy);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom prop = XInternAtom(dpy, "_DT_TRASH_STATE", False);

    int count = DtfileTrashBackendCount();
    char buf[1024];
    int pos = snprintf(buf, sizeof(buf), "count=%d", count);
    for (int i = 0; i < count && pos + 4 < (int)sizeof(buf); ++i) {
        const char *in_new = nullptr, *in_orig = nullptr, *fn = nullptr;
        if (DtfileTrashBackendGet(i, &in_new, &in_orig, &fn) && fn) {
            int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\t%s", fn);
            if (n <= 0 || pos + n >= (int)sizeof(buf))
                break;
            pos += n;
        }
    }
    XChangeProperty(dpy, root, prop, utf8, 8, PropModeReplace,
                    (const unsigned char *)buf, pos);
    XFlush(dpy);
}

Boolean DtfileXdgTrashReconcileNow(void) {
    if (g_reconciling || g_dt_trash_dir[0] == '\0')
        return False;

    g_reconciling = true;
    bool changed = false;

    /* 1. Upgrade any proxy symlinks loaded at startup (`~/.dt/Trash/<name>` -> `<repo>/files/<name>`)
     *    so `trashCan[i].intNew` points directly to the real XDG payload! */
    int n_items = DtfileTrashBackendCount();
    for (int i = 0; i < n_items; ++i) {
        const char *int_new = nullptr, *int_orig = nullptr, *fn = nullptr;
        if (!DtfileTrashBackendGet(i, &int_new, &int_orig, &fn) || !int_new)
            continue;
        if (strncmp(int_new, g_dt_trash_dir, strlen(g_dt_trash_dir)) == 0) {
            char target[PATH_MAX];
            ssize_t ln = readlink(int_new, target, sizeof(target) - 1);
            if (ln > 0) {
                target[ln] = '\0';
                if (target[0] == '/' && strstr(target, "/files/") != nullptr)
                    DtfileTrashBackendUpdatePayload(i, target);
            }
        }
    }

    /* 2. Prune entries whose payload was restored or deleted externally (or via dtfile Restore/Empty),
     *    and clean up any orphaned `.trashinfo` or `~/.dt/Trash/<leaf>` proxy symlinks. */
    for (int i = DtfileTrashBackendCount() - 1; i >= 0; --i) {
        const char *int_new = nullptr, *int_orig = nullptr, *fn = nullptr;
        if (!DtfileTrashBackendGet(i, &int_new, &int_orig, &fn) || !int_new)
            continue;
        struct stat pst;
        if (lstat(int_new, &pst) != 0 && errno == ENOENT) {
            char info_path[PATH_MAX];
            if (payload_to_info_path(int_new, info_path, sizeof(info_path)))
                unlink(info_path);
            if (fn && fn[0]) {
                char proxy[PATH_MAX];
                if (snprintf(proxy, sizeof(proxy), "%s/%s", g_dt_trash_dir, fn) < (int)sizeof(proxy)) {
                    struct stat ls;
                    if (lstat(proxy, &ls) == 0 && S_ISLNK(ls.st_mode))
                        unlink(proxy);
                }
            }
            DtfileTrashBackendRemoveAt(i);
            changed = true;
        }
    }

    /* 2b. Deduplicate any trashCan records sharing the exact same payload path (`intNew`) */
    for (int i = 0; i < DtfileTrashBackendCount(); ++i) {
        const char *a_new = nullptr, *a_orig = nullptr, *a_fn = nullptr;
        if (!DtfileTrashBackendGet(i, &a_new, &a_orig, &a_fn) || !a_new)
            continue;
        for (int j = DtfileTrashBackendCount() - 1; j > i; --j) {
            const char *b_new = nullptr, *b_orig = nullptr, *b_fn = nullptr;
            if (!DtfileTrashBackendGet(j, &b_new, &b_orig, &b_fn) || !b_new)
                continue;
            if (strcmp(a_new, b_new) == 0) {
                if (b_fn && b_fn[0] && (!a_fn || strcmp(b_fn, a_fn) != 0)) {
                    char dup_proxy[PATH_MAX];
                    struct stat ls;
                    if (snprintf(dup_proxy, sizeof(dup_proxy), "%s/%s", g_dt_trash_dir, b_fn) < (int)sizeof(dup_proxy) &&
                        lstat(dup_proxy, &ls) == 0 && S_ISLNK(ls.st_mode))
                        unlink(dup_proxy);
                }
                DtfileTrashBackendRemoveAt(j);
                changed = true;
            }
        }
    }

    /* Also sweep `~/.dt/Trash` for dangling proxy symlinks left after dtfile's own Restore/Shred */
    DIR *dt_d = opendir(g_dt_trash_dir);
    if (dt_d) {
        struct dirent *de;
        while ((de = readdir(dt_d)) != nullptr) {
            if (de->d_name[0] == '.')
                continue;
            char ppath[PATH_MAX];
            if (snprintf(ppath, sizeof(ppath), "%s/%s", g_dt_trash_dir, de->d_name) >= (int)sizeof(ppath))
                continue;
            struct stat lst;
            if (lstat(ppath, &lst) != 0 || !S_ISLNK(lst.st_mode))
                continue;
            char link_tgt[PATH_MAX];
            ssize_t ln = readlink(ppath, link_tgt, sizeof(link_tgt) - 1);
            if (ln <= 0)
                continue;
            link_tgt[ln] = '\0';
            if (link_tgt[0] == '/' && strstr(link_tgt, "/files/") != nullptr) {
                struct stat tst;
                if (lstat(link_tgt, &tst) != 0 && errno == ENOENT) {
                    char info_path[PATH_MAX];
                    if (payload_to_info_path(link_tgt, info_path, sizeof(info_path)))
                        unlink(info_path);
                    unlink(ppath);
                    changed = true;
                }
            }
        }
        closedir(dt_d);
    }

    /* 3. Scan XDG Home Trash and all active per-mount `.Trash-$UID` repos:
     *    - Remove orphaned `.trashinfo` whose `files/<stem>` was removed (e.g. via dtfile Restore/Shred)
     *    - Import newly trashed external items non-destructively (keeping `.trashinfo` + `files/<stem>`!) */
    XdgTrashRepo repos[MAX_TRASH_REPOS];
    int repo_count = collect_trash_repos(repos, false);
    for (int r = 0; r < repo_count; ++r) {
        char info_dir[PATH_MAX], files_dir[PATH_MAX];
        if (snprintf(info_dir, sizeof(info_dir), "%s/info", repos[r].trash_root) >= (int)sizeof(info_dir) ||
            snprintf(files_dir, sizeof(files_dir), "%s/files", repos[r].trash_root) >= (int)sizeof(files_dir))
            continue;

        DIR *id = opendir(info_dir);
        if (!id)
            continue;
        struct dirent *de;
        while ((de = readdir(id)) != nullptr) {
            if (de->d_name[0] == '.')
                continue;
            const char *dot = strrchr(de->d_name, '.');
            if (!dot || strcmp(dot, ".trashinfo") != 0 || dot == de->d_name)
                continue;

            size_t stem_len = (size_t)(dot - de->d_name);
            if (stem_len > NAME_MAX)
                continue;
            char stem[NAME_MAX + 1];
            memcpy(stem, de->d_name, stem_len);
            stem[stem_len] = '\0';

            char info_path[PATH_MAX], payload_path[PATH_MAX];
            if (snprintf(info_path, sizeof(info_path), "%s/%s", info_dir, de->d_name) >= (int)sizeof(info_path) ||
                snprintf(payload_path, sizeof(payload_path), "%s/%s", files_dir, stem) >= (int)sizeof(payload_path))
                continue;

            struct stat pst;
            if (lstat(payload_path, &pst) != 0) {
                if (errno == ENOENT) {
                    struct stat ist;
                    if (lstat(info_path, &ist) == 0 && (time(nullptr) - ist.st_mtime) >= 3)
                        unlink(info_path);
                }
                continue;
            }

            /* Is this payload already tracked in dtfile's trashCan? */
            bool known = false;
            int cur_n = DtfileTrashBackendCount();
            for (int k = 0; k < cur_n; ++k) {
                const char *in_new = nullptr, *in_orig = nullptr, *fn = nullptr;
                if (DtfileTrashBackendGet(k, &in_new, &in_orig, &fn) && in_new &&
                    strcmp(in_new, payload_path) == 0) {
                    known = true;
                    /* Ensure proxy symlink in ~/.dt/Trash still exists */
                    if (fn && fn[0]) {
                        char ppath[PATH_MAX];
                        struct stat ls;
                        if (snprintf(ppath, sizeof(ppath), "%s/%s", g_dt_trash_dir, fn) < (int)sizeof(ppath) &&
                            lstat(ppath, &ls) != 0 && errno == ENOENT) {
                            if (symlink(payload_path, ppath) == 0)
                                changed = true;
                        }
                    }
                    break;
                }
            }
            if (known)
                continue;

            /* Check whether MoveToTrashProcess already created a proxy symlink in ~/.dt/Trash
             * pointing to `payload_path`: if recent (< 3s), MoveToTrashPipeCB is still in flight;
             * if older, adopt the existing proxy symlink without creating a `.2` duplicate! */
            char existing_leaf[NAME_MAX + 1] = {0};
            time_t existing_mtime = 0;
            DIR *chk_d = opendir(g_dt_trash_dir);
            if (chk_d) {
                struct dirent *cde;
                while ((cde = readdir(chk_d)) != nullptr) {
                    if (cde->d_name[0] == '.')
                        continue;
                    char ppath[PATH_MAX], ltgt[PATH_MAX];
                    struct stat lst;
                    if (snprintf(ppath, sizeof(ppath), "%s/%s", g_dt_trash_dir, cde->d_name) >= (int)sizeof(ppath))
                        continue;
                    if (lstat(ppath, &lst) != 0 || !S_ISLNK(lst.st_mode))
                        continue;
                    ssize_t ln = readlink(ppath, ltgt, sizeof(ltgt) - 1);
                    if (ln > 0) {
                        ltgt[ln] = '\0';
                        if (strcmp(ltgt, payload_path) == 0) {
                            snprintf(existing_leaf, sizeof(existing_leaf), "%s", cde->d_name);
                            existing_mtime = lst.st_mtime;
                            break;
                        }
                    }
                }
                closedir(chk_d);
            }
            if (existing_leaf[0] != '\0') {
                if ((time(nullptr) - existing_mtime) < 3)
                    continue;
                char *orig_path = nullptr;
                char *del_date = nullptr;
                if (parse_trashinfo_file(info_path, repos[r].mount_base, &orig_path, &del_date)) {
                    if (DtfileTrashBackendAddExternal(payload_path, orig_path, existing_leaf, del_date))
                        changed = true;
                    XtFree(orig_path);
                    XtFree(del_date);
                }
                continue;
            }

            char *orig_path = nullptr;
            char *del_date = nullptr;
            if (!parse_trashinfo_file(info_path, repos[r].mount_base, &orig_path, &del_date))
                continue;

            char dt_leaf[NAME_MAX + 1];
            char proxy_path[PATH_MAX];
            if (choose_unique_dt_proxy_leaf(g_dt_trash_dir, stem,
                                            dt_leaf, sizeof(dt_leaf),
                                            proxy_path, sizeof(proxy_path)) &&
                symlink(payload_path, proxy_path) == 0) {
                if (DtfileTrashBackendAddExternal(payload_path, orig_path, dt_leaf, del_date))
                    changed = true;
                else
                    unlink(proxy_path);
            }
            XtFree(orig_path);
            XtFree(del_date);
        }
        closedir(id);
    }

    if (changed)
        DtfileTrashBackendCommit(True);
    publish_trash_root_property();

    g_reconciling = false;
    return changed ? True : False;
}

static void add_inotify_watch_dir(const char *dir_path) {
    if (g_inotify_fd < 0 || !dir_path || g_watch_count >= MAX_INOTIFY_WATCH)
        return;
    int wd = inotify_add_watch(g_inotify_fd, dir_path,
                               IN_CREATE | IN_DELETE | IN_MOVED_TO |
                               IN_MOVED_FROM | IN_CLOSE_WRITE | IN_ATTRIB);
    if (wd >= 0)
        g_watch_fds[g_watch_count++] = wd;
}

static void rebuild_inotify_watches(void) {
    if (g_inotify_fd < 0)
        return;
    for (int i = 0; i < g_watch_count; ++i)
        inotify_rm_watch(g_inotify_fd, g_watch_fds[i]);
    g_watch_count = 0;

    if (g_dt_trash_dir[0] == '/')
        add_inotify_watch_dir(g_dt_trash_dir);

    XdgTrashRepo repos[MAX_TRASH_REPOS];
    int n = collect_trash_repos(repos, true);
    for (int i = 0; i < n; ++i) {
        char sub[PATH_MAX];
        add_inotify_watch_dir(repos[i].trash_root);
        if (snprintf(sub, sizeof(sub), "%s/info", repos[i].trash_root) < (int)sizeof(sub))
            add_inotify_watch_dir(sub);
        if (snprintf(sub, sizeof(sub), "%s/files", repos[i].trash_root) < (int)sizeof(sub))
            add_inotify_watch_dir(sub);
    }
}

static void debounce_reconcile_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data;
    (void)id;
    g_debounce_timer = 0;
    DtfileXdgTrashReconcileNow();
}

static void inotify_xt_cb(XtPointer client_data, int *source, XtInputId *id) {
    (void)client_data;
    (void)id;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (read(*source, buf, sizeof(buf)) > 0) {
    }
    if (g_reconciling || !g_trash_toplevel)
        return;
    if (g_debounce_timer == 0) {
        g_debounce_timer = XtAppAddTimeOut(
            XtWidgetToApplicationContext(g_trash_toplevel),
            80, debounce_reconcile_cb, nullptr);
    }
}

void DtfileXdgTrashSyncRefreshMounts(void) {
    rebuild_inotify_watches();
    DtfileXdgTrashReconcileNow();
}

void DtfileXdgTrashSyncInit(Widget toplevel_widget, const char *dt_trash_dir) {
    if (!toplevel_widget || !dt_trash_dir || dt_trash_dir[0] != '/')
        return;

    g_trash_toplevel = toplevel_widget;
    snprintf(g_dt_trash_dir, sizeof(g_dt_trash_dir), "%s", dt_trash_dir);
    size_t dlen = strlen(g_dt_trash_dir);
    while (dlen > 1 && g_dt_trash_dir[dlen - 1] == '/')
        g_dt_trash_dir[--dlen] = '\0';

    if (g_inotify_fd < 0) {
        g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (g_inotify_fd >= 0) {
            g_inotify_input = XtAppAddInput(
                XtWidgetToApplicationContext(toplevel_widget),
                g_inotify_fd, (XtPointer)XtInputReadMask,
                inotify_xt_cb, nullptr);
        }
    }

    rebuild_inotify_watches();
    DtfileXdgTrashReconcileNow();
}
