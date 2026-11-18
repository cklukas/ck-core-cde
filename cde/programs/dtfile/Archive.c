/*
 * Archive.c - Built-in Archive Extract & Compress Actions + Zip-Slip Hardening
 *             for CK-Core CDE File Manager (Wayland & X11/XLibre, WP-16).
 */

#define _GNU_SOURCE
#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "Archive.h"

#ifndef DTFILE_ARCHIVE_CORE_ONLY
#include <Xm/DialogS.h>
#include <Xm/FileSB.h>
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
#endif

#define ARCHIVE_MAX_PATH_DEPTH 128

typedef struct {
    locale_t utf8_loc;
    locale_t prev_loc;
    bool active;
} ScopedUtf8Locale;

static ScopedUtf8Locale enter_utf8_thread_locale(void) {
    ScopedUtf8Locale s = { (locale_t)0, (locale_t)0, false };
    static const char *const candidates[] = { "C.UTF-8", "en_US.UTF-8", "UTF-8", NULL };
    for (int i = 0; candidates[i]; ++i) {
        s.utf8_loc = newlocale(LC_CTYPE_MASK, candidates[i], (locale_t)0);
        if (s.utf8_loc != (locale_t)0) {
            s.prev_loc = uselocale(s.utf8_loc);
            s.active = true;
            break;
        }
    }
    return s;
}

static void leave_utf8_thread_locale(ScopedUtf8Locale *s) {
    if (!s || !s->active)
        return;
    uselocale(s->prev_loc);
    freelocale(s->utf8_loc);
    s->active = false;
}

static void arc_set_err(char *err_out, size_t err_sz, const char *msg) {
    if (!err_out || err_sz == 0)
        return;
    if (!msg) {
        err_out[0] = '\0';
        return;
    }
    snprintf(err_out, err_sz, "%s", msg);
}

static bool has_suffix_ci(const char *name, const char *sfx) {
    size_t nlen = strlen(name);
    size_t slen = strlen(sfx);
    if (nlen <= slen)
        return false;
    return strcasecmp(name + nlen - slen, sfx) == 0;
}

DtfileArchiveFormat DtfileArchiveDetectFormat(const char *path) {
    if (!path || !path[0])
        return DTFILE_ARCHIVE_FMT_NONE;
    const char *base = strrchr(path, '/');
    base = base ? (base + 1) : path;
    if (!base[0])
        return DTFILE_ARCHIVE_FMT_NONE;

    if (has_suffix_ci(base, ".tar.gz") || has_suffix_ci(base, ".tgz"))
        return DTFILE_ARCHIVE_FMT_TAR_GZ;
    if (has_suffix_ci(base, ".tar.xz") || has_suffix_ci(base, ".txz"))
        return DTFILE_ARCHIVE_FMT_TAR_XZ;
    if (has_suffix_ci(base, ".tar.zst") || has_suffix_ci(base, ".tzst"))
        return DTFILE_ARCHIVE_FMT_TAR_ZST;
    if (has_suffix_ci(base, ".tar.bz2") || has_suffix_ci(base, ".tbz2") || has_suffix_ci(base, ".tbz"))
        return DTFILE_ARCHIVE_FMT_TAR_BZ2;
    if (has_suffix_ci(base, ".zip"))
        return DTFILE_ARCHIVE_FMT_ZIP;
    if (has_suffix_ci(base, ".7z"))
        return DTFILE_ARCHIVE_FMT_7Z;
    if (has_suffix_ci(base, ".tar"))
        return DTFILE_ARCHIVE_FMT_TAR;
    return DTFILE_ARCHIVE_FMT_NONE;
}

const char *DtfileArchiveFormatExtension(DtfileArchiveFormat fmt) {
    switch (fmt) {
        case DTFILE_ARCHIVE_FMT_ZIP:     return ".zip";
        case DTFILE_ARCHIVE_FMT_TAR_GZ:  return ".tar.gz";
        case DTFILE_ARCHIVE_FMT_TAR_XZ:  return ".tar.xz";
        case DTFILE_ARCHIVE_FMT_TAR_ZST: return ".tar.zst";
        case DTFILE_ARCHIVE_FMT_TAR_BZ2: return ".tar.bz2";
        case DTFILE_ARCHIVE_FMT_TAR:     return ".tar";
        case DTFILE_ARCHIVE_FMT_7Z:      return ".7z";
        default:                         return ".zip";
    }
}

static void strip_archive_stem(const char *archive_path, char *stem_out, size_t stem_sz) {
    const char *base = strrchr(archive_path, '/');
    base = base ? (base + 1) : archive_path;
    snprintf(stem_out, stem_sz, "%s", base);

    static const char *const suffixes[] = {
        ".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2",
        ".tgz", ".txz", ".tzst", ".tbz2", ".tbz",
        ".zip", ".7z", ".tar", NULL
    };
    size_t len = strlen(stem_out);
    for (int i = 0; suffixes[i]; ++i) {
        size_t slen = strlen(suffixes[i]);
        if (len > slen && strcasecmp(stem_out + len - slen, suffixes[i]) == 0) {
            stem_out[len - slen] = '\0';
            break;
        }
    }
    if (stem_out[0] == '\0' || strcmp(stem_out, ".") == 0 || strcmp(stem_out, "..") == 0)
        snprintf(stem_out, stem_sz, "Archive");
}

/*
 * Normalize and strictly validate an archive entry relative path:
 * - Reject empty, leading '/', Windows drive prefix ("C:..."), '.' or '..' components,
 *   or paths exceeding PATH_MAX / ARCHIVE_MAX_PATH_DEPTH.
 * - Allows spaces, UTF-8, quotes, punctuation, and arbitrary non-'/'/'\0' characters.
 * - Outputs normalized '/' separated relative path without leading/trailing '/'.
 */
static DtfileArchiveStatus validate_and_normalize_relpath(
    const char *raw_path,
    char *norm_out,
    size_t norm_sz,
    int *depth_out,
    char *first_comp_out,
    size_t first_comp_sz)
{
    if (!raw_path || !raw_path[0] || !norm_out || norm_sz < 4)
        return DTFILE_ARCHIVE_ERR_UNSAFE_PATH;

    /* Reject leading '/' or '\' or Windows drive letter "C:" */
    if (raw_path[0] == '/' || raw_path[0] == '\\')
        return DTFILE_ARCHIVE_ERR_UNSAFE_PATH;
    if (isalpha((unsigned char)raw_path[0]) && raw_path[1] == ':')
        return DTFILE_ARCHIVE_ERR_UNSAFE_PATH;

    size_t out_pos = 0;
    int depth = 0;
    if (first_comp_out && first_comp_sz > 0)
        first_comp_out[0] = '\0';

    const char *p = raw_path;
    /* Allow benign leading "./" prefix commonly emitted by `tar cf archive.tar .` */
    while (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) {
        p += 2;
        while (*p == '/' || *p == '\\')
            p++;
    }
    if (*p == '\0' || (p[0] == '.' && p[1] == '\0')) {
        /* Benign "." root directory record */
        norm_out[0] = '\0';
        if (depth_out) *depth_out = 0;
        return DTFILE_ARCHIVE_OK;
    }

    while (*p) {
        if (*p == '/' || *p == '\\') {
            /* Collapse multiple slashes, but reject leading slash already checked */
            while (*p == '/' || *p == '\\')
                p++;
            if (*p == '\0')
                break;
        }
        const char *start = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        size_t clen = (size_t)(p - start);
        if (clen == 0)
            continue;
        if ((clen == 1 && start[0] == '.') ||
            (clen == 2 && start[0] == '.' && start[1] == '.')) {
            return DTFILE_ARCHIVE_ERR_UNSAFE_PATH;
        }
        if (clen > NAME_MAX)
            return DTFILE_ARCHIVE_ERR_UNSAFE_PATH;

        if (depth == 0 && first_comp_out && first_comp_sz > 0) {
            size_t cp = clen < (first_comp_sz - 1) ? clen : (first_comp_sz - 1);
            memcpy(first_comp_out, start, cp);
            first_comp_out[cp] = '\0';
        }
        depth++;
        if (depth > ARCHIVE_MAX_PATH_DEPTH)
            return DTFILE_ARCHIVE_ERR_UNSAFE_PATH;

        size_t need = (out_pos > 0 ? 1 : 0) + clen + 1;
        if (out_pos + need > norm_sz)
            return DTFILE_ARCHIVE_ERR_UNSAFE_PATH;
        if (out_pos > 0)
            norm_out[out_pos++] = '/';
        memcpy(norm_out + out_pos, start, clen);
        out_pos += clen;
    }

    if (out_pos == 0)
        return DTFILE_ARCHIVE_ERR_UNSAFE_PATH;
    norm_out[out_pos] = '\0';
    if (depth_out)
        *depth_out = depth;
    return DTFILE_ARCHIVE_OK;
}

/*
 * Verify that a symlink target resolved relative to an entry at `entry_depth`
 * (where depth 1 means top-level file inside root, so parent depth is `entry_depth - 1`)
 * never escapes above depth 0 (the extraction root) and is not absolute.
 */
static DtfileArchiveStatus validate_symlink_target(const char *target, int entry_depth) {
    if (!target || !target[0])
        return DTFILE_ARCHIVE_ERR_UNSAFE_LINK;
    if (target[0] == '/' || target[0] == '\\')
        return DTFILE_ARCHIVE_ERR_UNSAFE_LINK;
    if (isalpha((unsigned char)target[0]) && target[1] == ':')
        return DTFILE_ARCHIVE_ERR_UNSAFE_LINK;

    int cur_depth = entry_depth - 1;
    if (cur_depth < 0)
        return DTFILE_ARCHIVE_ERR_UNSAFE_LINK;

    const char *p = target;
    while (*p) {
        while (*p == '/' || *p == '\\')
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        size_t clen = (size_t)(p - start);
        if (clen == 1 && start[0] == '.') {
            continue;
        } else if (clen == 2 && start[0] == '.' && start[1] == '.') {
            cur_depth--;
            if (cur_depth < 0)
                return DTFILE_ARCHIVE_ERR_UNSAFE_LINK;
        } else {
            cur_depth++;
            if (cur_depth > ARCHIVE_MAX_PATH_DEPTH * 2)
                return DTFILE_ARCHIVE_ERR_UNSAFE_LINK;
        }
    }
    return DTFILE_ARCHIVE_OK;
}

/* Open/create intermediate directories under `root_fd` using O_NOFOLLOW so a
 * previously extracted symlink can NEVER redirect subsequent file writes! */
static int open_parent_dir_nofollow(int root_fd, const char *rel_path, const char **leaf_out) {
    const char *slash = strrchr(rel_path, '/');
    if (!slash) {
        int dup_fd = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
        if (leaf_out) *leaf_out = rel_path;
        return dup_fd;
    }

    int cur_fd = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (cur_fd < 0)
        return -1;

    const char *p = rel_path;
    while (p < slash) {
        const char *next_slash = strchr(p, '/');
        if (!next_slash || next_slash > slash)
            next_slash = slash;
        size_t clen = (size_t)(next_slash - p);
        if (clen == 0 || clen > NAME_MAX) {
            close(cur_fd);
            errno = EINVAL;
            return -1;
        }
        char comp[NAME_MAX + 1];
        memcpy(comp, p, clen);
        comp[clen] = '\0';

        if (mkdirat(cur_fd, comp, 0755) != 0 && errno != EEXIST) {
            close(cur_fd);
            return -1;
        }
        int next_fd = openat(cur_fd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(cur_fd);
        if (next_fd < 0)
            return -1;
        cur_fd = next_fd;
        p = next_slash + 1;
    }

    if (leaf_out)
        *leaf_out = slash + 1;
    return cur_fd;
}

static bool choose_disambiguated_path(
    const char *parent_dir,
    const char *desired_name,
    char *out_path,
    size_t out_sz,
    char *out_leaf,
    size_t out_leaf_sz)
{
    for (int idx = 1; idx < 10000; ++idx) {
        char candidate_leaf[NAME_MAX + 1];
        if (idx == 1) {
            snprintf(candidate_leaf, sizeof(candidate_leaf), "%s", desired_name);
        } else {
            const char *dot = strrchr(desired_name, '.');
            if (dot && dot != desired_name && strlen(dot) <= 10) {
                int stem_len = (int)(dot - desired_name);
                snprintf(candidate_leaf, sizeof(candidate_leaf), "%.*s (%d)%s",
                         stem_len, desired_name, idx, dot);
            } else {
                snprintf(candidate_leaf, sizeof(candidate_leaf), "%s (%d)",
                         desired_name, idx);
            }
        }
        int n = snprintf(out_path, out_sz, "%s/%s", parent_dir, candidate_leaf);
        if (n <= 0 || (size_t)n >= out_sz)
            return false;
        struct stat st;
        if (lstat(out_path, &st) != 0 && errno == ENOENT) {
            if (out_leaf && out_leaf_sz > 0)
                snprintf(out_leaf, out_leaf_sz, "%s", candidate_leaf);
            return true;
        }
    }
    return false;
}

static void remove_tree_at(const char *path) {
    struct stat st;
    if (!path || !path[0] || lstat(path, &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char child[PATH_MAX];
            if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) < (int)sizeof(child))
                remove_tree_at(child);
        }
        closedir(d);
    }
    rmdir(path);
}

DtfileArchiveStatus DtfileArchiveExtract(
    const char *archive_path,
    const char *dest_dir,
    int smart_subfolder,
    char *created_path_out,
    size_t created_path_sz,
    char *err_out,
    size_t err_sz)
{
    if (created_path_out && created_path_sz > 0)
        created_path_out[0] = '\0';
    arc_set_err(err_out, err_sz, NULL);

    if (!archive_path || archive_path[0] != '/' || !dest_dir || dest_dir[0] != '/') {
        arc_set_err(err_out, err_sz, "Invalid archive or destination path.");
        return DTFILE_ARCHIVE_ERR_INVALID;
    }

    struct stat dst_st;
    if (stat(dest_dir, &dst_st) != 0 || !S_ISDIR(dst_st.st_mode)) {
        arc_set_err(err_out, err_sz, "Destination directory does not exist.");
        return DTFILE_ARCHIVE_ERR_IO;
    }

    ScopedUtf8Locale utf8_guard = enter_utf8_thread_locale();
#define RETURN_EXTRACT(code) do { leave_utf8_thread_locale(&utf8_guard); return (code); } while (0)

    /* ---- Pass 1: Strict pre-flight validation of EVERY entry before touching disk ---- */
    struct archive *ar = archive_read_new();
    if (!ar) {
        arc_set_err(err_out, err_sz, "Out of memory initializing libarchive.");
        RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_IO);
    }
    archive_read_support_format_all(ar);
    archive_read_support_filter_all(ar);

    if (archive_read_open_filename(ar, archive_path, 65536) != ARCHIVE_OK) {
        arc_set_err(err_out, err_sz, archive_error_string(ar));
        archive_read_free(ar);
        RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_FORMAT);
    }

    char common_root[NAME_MAX + 1];
    common_root[0] = '\0';
    bool multiple_roots = false;
    int entry_count = 0;

    struct archive_entry *entry = NULL;
    int r;
    while ((r = archive_read_next_header(ar, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        const char *raw_path = archive_entry_pathname_utf8(entry);
        if (!raw_path)
            raw_path = archive_entry_pathname(entry);
        if (!raw_path) {
            arc_set_err(err_out, err_sz, "Archive contains an entry with an unreadable pathname.");
            archive_read_free(ar);
            RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_UNSAFE_PATH);
        }
        char norm[PATH_MAX];
        char first_comp[NAME_MAX + 1];
        int depth = 0;
        DtfileArchiveStatus st = validate_and_normalize_relpath(
            raw_path, norm, sizeof(norm), &depth, first_comp, sizeof(first_comp));
        if (st != DTFILE_ARCHIVE_OK) {
            arc_set_err(err_out, err_sz, "Rejected archive entry with unsafe path traversal.");
            archive_read_free(ar);
            RETURN_EXTRACT(st);
        }
        if (depth == 0)
            continue;

        if (common_root[0] == '\0') {
            snprintf(common_root, sizeof(common_root), "%s", first_comp);
        } else if (strcmp(common_root, first_comp) != 0) {
            multiple_roots = true;
        }

        mode_t ftype = archive_entry_filetype(entry);
        if (ftype != AE_IFREG && ftype != AE_IFDIR && ftype != AE_IFLNK) {
            const char *hl = archive_entry_hardlink_utf8(entry);
            if (!hl) hl = archive_entry_hardlink(entry);
            if (!hl) {
                arc_set_err(err_out, err_sz, "Rejected unsupported special device or FIFO entry in archive.");
                archive_read_free(ar);
                RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_UNSUPPORTED_ENTRY);
            }
        }

        const char *hl_target = archive_entry_hardlink_utf8(entry);
        if (!hl_target)
            hl_target = archive_entry_hardlink(entry);
        if (hl_target) {
            char hl_norm[PATH_MAX];
            int hl_depth = 0;
            if (validate_and_normalize_relpath(hl_target, hl_norm, sizeof(hl_norm),
                                               &hl_depth, NULL, 0) != DTFILE_ARCHIVE_OK ||
                hl_depth <= 0) {
                arc_set_err(err_out, err_sz, "Rejected hardlink target escaping extraction root.");
                archive_read_free(ar);
                RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_UNSAFE_LINK);
            }
        }

        if (ftype == AE_IFLNK) {
            const char *sym_target = archive_entry_symlink_utf8(entry);
            if (!sym_target)
                sym_target = archive_entry_symlink(entry);
            if (validate_symlink_target(sym_target, depth) != DTFILE_ARCHIVE_OK) {
                arc_set_err(err_out, err_sz, "Rejected symlink target escaping extraction root.");
                archive_read_free(ar);
                RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_UNSAFE_LINK);
            }
        }
        entry_count++;
    }

    if (r != ARCHIVE_EOF) {
        arc_set_err(err_out, err_sz, archive_error_string(ar));
        archive_read_free(ar);
        RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_FORMAT);
    }
    archive_read_free(ar);

    /* ---- Pass 2: Stage extraction inside a private directory on the destination filesystem ---- */
    char stage_dir[PATH_MAX];
    if (snprintf(stage_dir, sizeof(stage_dir), "%s/.dtfile-extract-XXXXXX", dest_dir) >= (int)sizeof(stage_dir)) {
        arc_set_err(err_out, err_sz, "Destination path is too long.");
        RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_INVALID);
    }
    if (!mkdtemp(stage_dir)) {
        arc_set_err(err_out, err_sz, strerror(errno));
        RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_IO);
    }

    int root_fd = open(stage_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) {
        remove_tree_at(stage_dir);
        arc_set_err(err_out, err_sz, strerror(errno));
        RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_IO);
    }

    ar = archive_read_new();
    archive_read_support_format_all(ar);
    archive_read_support_filter_all(ar);
    if (archive_read_open_filename(ar, archive_path, 65536) != ARCHIVE_OK) {
        close(root_fd);
        remove_tree_at(stage_dir);
        arc_set_err(err_out, err_sz, archive_error_string(ar));
        archive_read_free(ar);
        RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_FORMAT);
    }

    DtfileArchiveStatus final_status = DTFILE_ARCHIVE_OK;
    while ((r = archive_read_next_header(ar, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        char norm[PATH_MAX];
        int depth = 0;
        const char *epath = archive_entry_pathname_utf8(entry);
        if (!epath)
            epath = archive_entry_pathname(entry);
        if (validate_and_normalize_relpath(epath, norm, sizeof(norm),
                                           &depth, NULL, 0) != DTFILE_ARCHIVE_OK) {
            final_status = DTFILE_ARCHIVE_ERR_UNSAFE_PATH;
            arc_set_err(err_out, err_sz, "Unsafe entry path encountered during extraction.");
            break;
        }
        if (depth == 0)
            continue;

        const char *leaf = NULL;
        int parent_fd = open_parent_dir_nofollow(root_fd, norm, &leaf);
        if (parent_fd < 0 || !leaf || !leaf[0]) {
            if (parent_fd >= 0) close(parent_fd);
            final_status = (errno == ELOOP || errno == ENOTDIR)
                               ? DTFILE_ARCHIVE_ERR_UNSAFE_LINK
                               : DTFILE_ARCHIVE_ERR_IO;
            arc_set_err(err_out, err_sz,
                        final_status == DTFILE_ARCHIVE_ERR_UNSAFE_LINK
                            ? "Blocked write through symlinked parent directory."
                            : "Failed creating parent directory.");
            break;
        }

        mode_t perm = (archive_entry_perm(entry) & 0777);
        const char *hl_target = archive_entry_hardlink_utf8(entry);
        if (!hl_target)
            hl_target = archive_entry_hardlink(entry);
        mode_t ftype = archive_entry_filetype(entry);

        if (hl_target) {
            char hl_norm[PATH_MAX];
            int hl_depth = 0;
            if (validate_and_normalize_relpath(hl_target, hl_norm, sizeof(hl_norm),
                                               &hl_depth, NULL, 0) != DTFILE_ARCHIVE_OK ||
                hl_depth <= 0) {
                close(parent_fd);
                final_status = DTFILE_ARCHIVE_ERR_UNSAFE_LINK;
                break;
            }
            const char *src_leaf = NULL;
            int src_parent_fd = open_parent_dir_nofollow(root_fd, hl_norm, &src_leaf);
            if (src_parent_fd < 0) {
                close(parent_fd);
                final_status = DTFILE_ARCHIVE_ERR_UNSAFE_LINK;
                break;
            }
            if (linkat(src_parent_fd, src_leaf, parent_fd, leaf, 0) != 0) {
                close(src_parent_fd);
                close(parent_fd);
                final_status = DTFILE_ARCHIVE_ERR_IO;
                arc_set_err(err_out, err_sz, "Failed creating hardlink inside archive.");
                break;
            }
            close(src_parent_fd);
        } else if (ftype == AE_IFDIR) {
            if (perm == 0) perm = 0755;
            perm |= 0700;
            if (mkdirat(parent_fd, leaf, perm) != 0 && errno != EEXIST) {
                close(parent_fd);
                final_status = DTFILE_ARCHIVE_ERR_IO;
                arc_set_err(err_out, err_sz, strerror(errno));
                break;
            }
            /* Verify existing entry is a real directory, not a symlink */
            int dfd = openat(parent_fd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (dfd < 0) {
                close(parent_fd);
                final_status = DTFILE_ARCHIVE_ERR_UNSAFE_LINK;
                arc_set_err(err_out, err_sz, "Directory entry collided with non-directory.");
                break;
            }
            fchmod(dfd, perm);
            close(dfd);
        } else if (ftype == AE_IFLNK) {
            const char *sym_target = archive_entry_symlink_utf8(entry);
            if (!sym_target)
                sym_target = archive_entry_symlink(entry);
            if (symlinkat(sym_target, parent_fd, leaf) != 0) {
                close(parent_fd);
                final_status = DTFILE_ARCHIVE_ERR_IO;
                arc_set_err(err_out, err_sz, strerror(errno));
                break;
            }
        } else if (ftype == AE_IFREG) {
            if (perm == 0) perm = 0644;
            perm |= 0600;
            int out_fd = openat(parent_fd, leaf,
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, perm);
            if (out_fd < 0) {
                close(parent_fd);
                final_status = DTFILE_ARCHIVE_ERR_IO;
                arc_set_err(err_out, err_sz, strerror(errno));
                break;
            }
            const void *buff = NULL;
            size_t size = 0;
            la_int64_t offset = 0;
            int dr;
            while ((dr = archive_read_data_block(ar, &buff, &size, &offset)) == ARCHIVE_OK) {
                if (lseek(out_fd, (off_t)offset, SEEK_SET) < 0) {
                    final_status = DTFILE_ARCHIVE_ERR_IO;
                    break;
                }
                const unsigned char *bp = (const unsigned char *)buff;
                size_t rem = size;
                while (rem > 0) {
                    ssize_t wn = write(out_fd, bp, rem);
                    if (wn < 0 && errno == EINTR)
                        continue;
                    if (wn <= 0) {
                        final_status = DTFILE_ARCHIVE_ERR_IO;
                        break;
                    }
                    bp += wn;
                    rem -= (size_t)wn;
                }
                if (final_status != DTFILE_ARCHIVE_OK)
                    break;
            }
            if (final_status == DTFILE_ARCHIVE_OK && dr != ARCHIVE_EOF) {
                final_status = DTFILE_ARCHIVE_ERR_FORMAT;
                arc_set_err(err_out, err_sz, archive_error_string(ar));
            }
            if (final_status == DTFILE_ARCHIVE_OK && archive_entry_mtime_is_set(entry)) {
                struct timespec ts[2];
                ts[0].tv_sec = archive_entry_mtime(entry);
                ts[0].tv_nsec = archive_entry_mtime_nsec(entry);
                ts[1] = ts[0];
                futimens(out_fd, ts);
            }
            fchmod(out_fd, perm);
            close(out_fd);
        }

        close(parent_fd);
        if (final_status != DTFILE_ARCHIVE_OK)
            break;
    }

    if (final_status == DTFILE_ARCHIVE_OK && r != ARCHIVE_EOF) {
        final_status = DTFILE_ARCHIVE_ERR_FORMAT;
        arc_set_err(err_out, err_sz, archive_error_string(ar));
    }
    archive_read_free(ar);
    close(root_fd);

    if (final_status != DTFILE_ARCHIVE_OK) {
        remove_tree_at(stage_dir);
        RETURN_EXTRACT(final_status);
    }

    /* ---- Finalize staged contents into `dest_dir` cleanly ---- */
    if (entry_count == 0) {
        char stem[NAME_MAX + 1];
        strip_archive_stem(archive_path, stem, sizeof(stem));
        char final_dir[PATH_MAX];
        if (!choose_disambiguated_path(dest_dir, stem, final_dir, sizeof(final_dir), NULL, 0) ||
            rename(stage_dir, final_dir) != 0) {
            remove_tree_at(stage_dir);
            arc_set_err(err_out, err_sz, "Failed finalizing empty archive folder.");
            RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_IO);
        }
        chmod(final_dir, 0755);
        if (created_path_out && created_path_sz > 0)
            snprintf(created_path_out, created_path_sz, "%s", final_dir);
        RETURN_EXTRACT(DTFILE_ARCHIVE_OK);
    }

    if (smart_subfolder) {
        if (!multiple_roots && common_root[0] != '\0') {
            char staged_item[PATH_MAX + NAME_MAX + 2];
            char final_item[PATH_MAX];
            snprintf(staged_item, sizeof(staged_item), "%s/%s", stage_dir, common_root);
            if (!choose_disambiguated_path(dest_dir, common_root,
                                           final_item, sizeof(final_item), NULL, 0) ||
                rename(staged_item, final_item) != 0) {
                remove_tree_at(stage_dir);
                arc_set_err(err_out, err_sz, "Failed moving extracted item into destination.");
                RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_IO);
            }
            rmdir(stage_dir);
            if (created_path_out && created_path_sz > 0)
                snprintf(created_path_out, created_path_sz, "%s", final_item);
            RETURN_EXTRACT(DTFILE_ARCHIVE_OK);
        } else {
            char stem[NAME_MAX + 1];
            strip_archive_stem(archive_path, stem, sizeof(stem));
            char final_dir[PATH_MAX];
            if (!choose_disambiguated_path(dest_dir, stem, final_dir, sizeof(final_dir), NULL, 0) ||
                rename(stage_dir, final_dir) != 0) {
                remove_tree_at(stage_dir);
                arc_set_err(err_out, err_sz, "Failed moving extracted folder into destination.");
                RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_IO);
            }
            chmod(final_dir, 0755);
            if (created_path_out && created_path_sz > 0)
                snprintf(created_path_out, created_path_sz, "%s", final_dir);
            RETURN_EXTRACT(DTFILE_ARCHIVE_OK);
        }
    } else {
        /* Direct extraction into `dest_dir` (Extract To...): move each top-level staged item,
         * disambiguating collisions rather than clobbering existing files. */
        DIR *d = opendir(stage_dir);
        if (!d) {
            remove_tree_at(stage_dir);
            arc_set_err(err_out, err_sz, strerror(errno));
            RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_IO);
        }
        char first_created[PATH_MAX];
        first_created[0] = '\0';
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char src_item[PATH_MAX + NAME_MAX + 2];
            char dst_item[PATH_MAX];
            snprintf(src_item, sizeof(src_item), "%s/%s", stage_dir, de->d_name);
            if (!choose_disambiguated_path(dest_dir, de->d_name, dst_item, sizeof(dst_item), NULL, 0) ||
                rename(src_item, dst_item) != 0) {
                closedir(d);
                remove_tree_at(stage_dir);
                arc_set_err(err_out, err_sz, "Failed moving extracted entry to target folder.");
                RETURN_EXTRACT(DTFILE_ARCHIVE_ERR_IO);
            }
            if (first_created[0] == '\0')
                snprintf(first_created, sizeof(first_created), "%s", dst_item);
        }
        closedir(d);
        rmdir(stage_dir);
        if (created_path_out && created_path_sz > 0)
            snprintf(created_path_out, created_path_sz, "%s",
                     first_created[0] ? first_created : dest_dir);
        RETURN_EXTRACT(DTFILE_ARCHIVE_OK);
    }
#undef RETURN_EXTRACT
}

/* ---- Archive Creation (Compress...) ---- */

static DtfileArchiveStatus add_path_recursive(
    struct archive *aw,
    const char *disk_path,
    const char *arc_rel_path,
    dev_t skip_dev,
    ino_t skip_ino,
    int depth,
    char *err_out,
    size_t err_sz)
{
    if (depth > ARCHIVE_MAX_PATH_DEPTH) {
        arc_set_err(err_out, err_sz, "Directory nesting is too deep to archive.");
        return DTFILE_ARCHIVE_ERR_INVALID;
    }

    struct stat st;
    if (lstat(disk_path, &st) != 0) {
        arc_set_err(err_out, err_sz, strerror(errno));
        return DTFILE_ARCHIVE_ERR_IO;
    }

    /* Never recurse into the temporary output archive itself */
    if (st.st_dev == skip_dev && st.st_ino == skip_ino)
        return DTFILE_ARCHIVE_OK;

    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
        return DTFILE_ARCHIVE_OK;

    struct archive_entry *ae = archive_entry_new();
    if (!ae)
        return DTFILE_ARCHIVE_ERR_IO;

    archive_entry_set_pathname_utf8(ae, arc_rel_path);
    archive_entry_copy_stat(ae, &st);
    archive_entry_set_perm(ae, st.st_mode & 0777);

    if (S_ISLNK(st.st_mode)) {
        char link_buf[PATH_MAX];
        ssize_t ln = readlink(disk_path, link_buf, sizeof(link_buf) - 1);
        if (ln < 0) {
            archive_entry_free(ae);
            arc_set_err(err_out, err_sz, strerror(errno));
            return DTFILE_ARCHIVE_ERR_IO;
        }
        link_buf[ln] = '\0';
        archive_entry_set_filetype(ae, AE_IFLNK);
        archive_entry_set_symlink_utf8(ae, link_buf);
        archive_entry_set_size(ae, 0);
    } else if (S_ISDIR(st.st_mode)) {
        archive_entry_set_filetype(ae, AE_IFDIR);
        archive_entry_set_size(ae, 0);
    } else {
        archive_entry_set_filetype(ae, AE_IFREG);
    }

    int hdr_rc = archive_write_header(aw, ae);
    if (hdr_rc != ARCHIVE_OK && hdr_rc != ARCHIVE_WARN) {
        arc_set_err(err_out, err_sz, archive_error_string(aw));
        archive_entry_free(ae);
        return DTFILE_ARCHIVE_ERR_FORMAT;
    }
    archive_entry_free(ae);

    if (S_ISREG(st.st_mode)) {
        int fd = open(disk_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            arc_set_err(err_out, err_sz, strerror(errno));
            return DTFILE_ARCHIVE_ERR_IO;
        }
        unsigned char buf[65536];
        for (;;) {
            ssize_t nr = read(fd, buf, sizeof(buf));
            if (nr < 0 && errno == EINTR)
                continue;
            if (nr < 0) {
                close(fd);
                arc_set_err(err_out, err_sz, strerror(errno));
                return DTFILE_ARCHIVE_ERR_IO;
            }
            if (nr == 0)
                break;
            if (archive_write_data(aw, buf, (size_t)nr) != nr) {
                close(fd);
                arc_set_err(err_out, err_sz, archive_error_string(aw));
                return DTFILE_ARCHIVE_ERR_IO;
            }
        }
        close(fd);
    } else if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(disk_path);
        if (!d) {
            arc_set_err(err_out, err_sz, strerror(errno));
            return DTFILE_ARCHIVE_ERR_IO;
        }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char child_disk[PATH_MAX];
            char child_rel[PATH_MAX];
            if (snprintf(child_disk, sizeof(child_disk), "%s/%s", disk_path, de->d_name) >= (int)sizeof(child_disk) ||
                snprintf(child_rel, sizeof(child_rel), "%s/%s", arc_rel_path, de->d_name) >= (int)sizeof(child_rel)) {
                closedir(d);
                arc_set_err(err_out, err_sz, "Path inside directory is too long.");
                return DTFILE_ARCHIVE_ERR_INVALID;
            }
            DtfileArchiveStatus cs = add_path_recursive(
                aw, child_disk, child_rel, skip_dev, skip_ino, depth + 1, err_out, err_sz);
            if (cs != DTFILE_ARCHIVE_OK) {
                closedir(d);
                return cs;
            }
        }
        closedir(d);
    }

    return DTFILE_ARCHIVE_OK;
}

DtfileArchiveStatus DtfileArchiveCreate(
    const char *const *input_paths,
    int input_count,
    const char *output_archive_path,
    DtfileArchiveFormat fmt,
    char *err_out,
    size_t err_sz)
{
    arc_set_err(err_out, err_sz, NULL);
    if (!input_paths || input_count <= 0 ||
        !output_archive_path || output_archive_path[0] != '/') {
        arc_set_err(err_out, err_sz, "Invalid input selection or output archive path.");
        return DTFILE_ARCHIVE_ERR_INVALID;
    }

    ScopedUtf8Locale utf8_guard = enter_utf8_thread_locale();
#define RETURN_CREATE(code) do { leave_utf8_thread_locale(&utf8_guard); return (code); } while (0)

    if (fmt == DTFILE_ARCHIVE_FMT_NONE)
        fmt = DtfileArchiveDetectFormat(output_archive_path);
    if (fmt == DTFILE_ARCHIVE_FMT_NONE)
        fmt = DTFILE_ARCHIVE_FMT_ZIP;

    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.part.XXXXXX", output_archive_path) >= (int)sizeof(tmp_path)) {
        arc_set_err(err_out, err_sz, "Output archive path is too long.");
        RETURN_CREATE(DTFILE_ARCHIVE_ERR_INVALID);
    }

    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        arc_set_err(err_out, err_sz, strerror(errno));
        RETURN_CREATE(DTFILE_ARCHIVE_ERR_IO);
    }
    fchmod(tmp_fd, 0644);
    struct stat tmp_st;
    memset(&tmp_st, 0, sizeof(tmp_st));
    fstat(tmp_fd, &tmp_st);

    struct archive *aw = archive_write_new();
    if (!aw) {
        close(tmp_fd);
        unlink(tmp_path);
        RETURN_CREATE(DTFILE_ARCHIVE_ERR_IO);
    }

    int fmt_rc = ARCHIVE_OK;
    int flt_rc = ARCHIVE_OK;
    switch (fmt) {
        case DTFILE_ARCHIVE_FMT_ZIP:
            fmt_rc = archive_write_set_format_zip(aw);
            break;
        case DTFILE_ARCHIVE_FMT_7Z:
            fmt_rc = archive_write_set_format_7zip(aw);
            break;
        case DTFILE_ARCHIVE_FMT_TAR_GZ:
            fmt_rc = archive_write_set_format_pax_restricted(aw);
            flt_rc = archive_write_add_filter_gzip(aw);
            break;
        case DTFILE_ARCHIVE_FMT_TAR_XZ:
            fmt_rc = archive_write_set_format_pax_restricted(aw);
            flt_rc = archive_write_add_filter_xz(aw);
            break;
        case DTFILE_ARCHIVE_FMT_TAR_ZST:
            fmt_rc = archive_write_set_format_pax_restricted(aw);
            flt_rc = archive_write_add_filter_zstd(aw);
            break;
        case DTFILE_ARCHIVE_FMT_TAR_BZ2:
            fmt_rc = archive_write_set_format_pax_restricted(aw);
            flt_rc = archive_write_add_filter_bzip2(aw);
            break;
        case DTFILE_ARCHIVE_FMT_TAR:
            fmt_rc = archive_write_set_format_pax_restricted(aw);
            flt_rc = archive_write_add_filter_none(aw);
            break;
        default:
            fmt_rc = ARCHIVE_FATAL;
            break;
    }

    if ((fmt_rc != ARCHIVE_OK && fmt_rc != ARCHIVE_WARN) ||
        (flt_rc != ARCHIVE_OK && flt_rc != ARCHIVE_WARN)) {
        arc_set_err(err_out, err_sz, archive_error_string(aw));
        archive_write_free(aw);
        close(tmp_fd);
        unlink(tmp_path);
        RETURN_CREATE(DTFILE_ARCHIVE_ERR_FORMAT);
    }

    if (archive_write_open_fd(aw, tmp_fd) != ARCHIVE_OK) {
        arc_set_err(err_out, err_sz, archive_error_string(aw));
        archive_write_free(aw);
        close(tmp_fd);
        unlink(tmp_path);
        RETURN_CREATE(DTFILE_ARCHIVE_ERR_IO);
    }

    DtfileArchiveStatus status = DTFILE_ARCHIVE_OK;
    for (int i = 0; i < input_count; ++i) {
        const char *p = input_paths[i];
        if (!p || p[0] != '/') {
            status = DTFILE_ARCHIVE_ERR_INVALID;
            break;
        }
        char clean_p[PATH_MAX];
        snprintf(clean_p, sizeof(clean_p), "%s", p);
        size_t plen = strlen(clean_p);
        while (plen > 1 && clean_p[plen - 1] == '/')
            clean_p[--plen] = '\0';

        const char *leaf = strrchr(clean_p, '/');
        leaf = leaf ? (leaf + 1) : clean_p;
        if (!leaf[0] || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
            status = DTFILE_ARCHIVE_ERR_INVALID;
            break;
        }

        status = add_path_recursive(aw, clean_p, leaf,
                                    tmp_st.st_dev, tmp_st.st_ino, 1, err_out, err_sz);
        if (status != DTFILE_ARCHIVE_OK)
            break;
    }

    if (archive_write_close(aw) != ARCHIVE_OK && status == DTFILE_ARCHIVE_OK) {
        arc_set_err(err_out, err_sz, archive_error_string(aw));
        status = DTFILE_ARCHIVE_ERR_IO;
    }
    archive_write_free(aw);
    fsync(tmp_fd);
    close(tmp_fd);

    if (status != DTFILE_ARCHIVE_OK) {
        unlink(tmp_path);
        RETURN_CREATE(status);
    }

    if (rename(tmp_path, output_archive_path) != 0) {
        arc_set_err(err_out, err_sz, strerror(errno));
        unlink(tmp_path);
        RETURN_CREATE(DTFILE_ARCHIVE_ERR_IO);
    }

    RETURN_CREATE(DTFILE_ARCHIVE_OK);
#undef RETURN_CREATE
}

/* =========================================================================
 * Motif UI Callbacks (Extract Here, Extract To..., Compress...)
 * ========================================================================= */
#ifndef DTFILE_ARCHIVE_CORE_ONLY

Boolean DtfileIsArchiveFile(const char *path) {
    return DtfileArchiveDetectFormat(path) != DTFILE_ARCHIVE_FMT_NONE ? True : False;
}

static char *arc_build_fvd_path(FileViewData *fvd) {
    if (!fvd || !fvd->file_data)
        return nullptr;
    DirectorySet *ds = (DirectorySet *)fvd->directory_set;
    if (!ds || !ds->name || !fvd->file_data->file_name)
        return nullptr;
    const char *dir = ds->name;
    const char *fn = fvd->file_data->file_name;
    if (strcmp(fn, ".") == 0 || strcmp(fn, "..") == 0)
        return nullptr;
    size_t dlen = strlen(dir);
    char *out = (char *)XtMalloc(dlen + strlen(fn) + 2);
    strcpy(out, dir);
    if (dlen == 0 || out[dlen - 1] != '/')
        strcat(out, "/");
    strcat(out, fn);
    return out;
}

static FileMgrData *arc_resolve_fmd_from_menu(Widget w) {
    Widget mbar = XmGetPostedFromWidget(XtParent(w));
    if (!mbar)
        return nullptr;
    FileMgrRec *fmr = nullptr;
    Arg args[1];
    XtSetArg(args[0], XmNuserData, &fmr);
    XtGetValues(mbar, args, 1);
    if (!fmr)
        return nullptr;
    DialogData *dd = _DtGetInstanceData((XtPointer)fmr);
    return dd ? (FileMgrData *)dd->data : nullptr;
}

static int arc_collect_selection(FileMgrData *fmd, FileViewData *popup_fvd, char ***paths_out) {
    *paths_out = nullptr;
    if (!fmd)
        return 0;

    bool use_multi = false;
    if (popup_fvd) {
        for (int i = 0; i < fmd->selected_file_count; ++i) {
            if (fmd->selection_list[i] == popup_fvd) {
                use_multi = (fmd->selected_file_count > 1);
                break;
            }
        }
        if (!use_multi) {
            char *p = arc_build_fvd_path(popup_fvd);
            if (!p) return 0;
            char **arr = (char **)XtCalloc(1, sizeof(char *));
            arr[0] = p;
            *paths_out = arr;
            return 1;
        }
    }

    if (fmd->selected_file_count <= 0)
        return 0;
    char **arr = (char **)XtCalloc((Cardinal)fmd->selected_file_count, sizeof(char *));
    int n = 0;
    for (int i = 0; i < fmd->selected_file_count; ++i) {
        char *p = arc_build_fvd_path(fmd->selection_list[i]);
        if (p)
            arr[n++] = p;
    }
    if (n == 0) {
        XtFree((char *)arr);
        return 0;
    }
    *paths_out = arr;
    return n;
}

static void arc_free_paths(char **paths, int count) {
    if (!paths) return;
    for (int i = 0; i < count; ++i)
        XtFree(paths[i]);
    XtFree((char *)paths);
}

static void arc_refresh_and_select(FileMgrData *fmd, const char *target_dir, const char *created_path) {
    if (!target_dir || !target_dir[0])
        return;
    if (fmd && created_path && created_path[0] == '/') {
        const char *slash = strrchr(created_path, '/');
        if (slash && slash[1] != '\0') {
            size_t dlen = (slash == created_path) ? 1 : (size_t)(slash - created_path);
            if (fmd->current_directory &&
                strlen(fmd->current_directory) == dlen &&
                strncmp(fmd->current_directory, created_path, dlen) == 0) {
                XtFree(fmd->scrollToThisFile);
                XtFree(fmd->scrollToThisDirectory);
                fmd->scrollToThisFile = XtNewString(slash + 1);
                fmd->scrollToThisDirectory = XtNewString(target_dir);
            }
        }
    }
    UpdateDirectory(nullptr, (fmd && fmd->host) ? fmd->host : home_host_name, (char *)target_dir);
}

static void arc_post_error(Widget parent, const char *title_txt, const char *err_txt) {
    Widget ref = parent ? parent : toplevel;
    char *title = XtNewString(title_txt ? title_txt : "Archive Error");
    char *msg = XtNewString((err_txt && err_txt[0]) ? err_txt : "Archive operation failed.");
    _DtMessage(ref, title, msg, nullptr, HelpRequestCB);
    XtFree(title);
    XtFree(msg);
}

static void run_extract_here(FileMgrData *fmd, const char *archive_path) {
    if (!archive_path)
        return;
    char dest_dir[PATH_MAX];
    const char *slash = strrchr(archive_path, '/');
    if (slash == archive_path) {
        strcpy(dest_dir, "/");
    } else if (slash) {
        size_t len = (size_t)(slash - archive_path);
        if (len >= sizeof(dest_dir)) return;
        memcpy(dest_dir, archive_path, len);
        dest_dir[len] = '\0';
    } else {
        return;
    }

    char created[PATH_MAX];
    char err[512];
    DtfileArchiveStatus st = DtfileArchiveExtract(
        archive_path, dest_dir, 1, created, sizeof(created), err, sizeof(err));
    if (st != DTFILE_ARCHIVE_OK) {
        Widget shell = (fmd && fmd->file_mgr_rec) ? ((FileMgrRec *)fmd->file_mgr_rec)->shell : toplevel;
        arc_post_error(shell, "Extract Archive Error", err);
        return;
    }
    arc_refresh_and_select(fmd, dest_dir, created);
}

void ExtractHereMenuCB(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;
    FileMgrData *fmd = arc_resolve_fmd_from_menu(w);
    char **paths = nullptr;
    int count = arc_collect_selection(fmd, nullptr, &paths);
    if (count == 1 && DtfileIsArchiveFile(paths[0]))
        run_extract_here(fmd, paths[0]);
    arc_free_paths(paths, count);
}

void ExtractHerePopupCB(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    FileViewData *fvd = (FileViewData *)client_data;
    if (!fvd || !fvd->directory_set)
        return;
    FileMgrData *fmd = (FileMgrData *)((DirectorySet *)fvd->directory_set)->file_mgr_data;
    if (fmd)
        fmd->popup_menu_icon = nullptr;
    char *p = arc_build_fvd_path(fvd);
    if (p) {
        if (DtfileIsArchiveFile(p))
            run_extract_here(fmd, p);
        XtFree(p);
    }
}

/* ---- Extract To... directory picker ---- */

typedef struct {
    FileMgrData *fmd;
    char *archive_path;
} ExtractToCtx;

static void extract_to_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    ExtractToCtx *ctx = (ExtractToCtx *)client_data;
    if (ctx) {
        XtFree(ctx->archive_path);
        XtFree((char *)ctx);
    }
}

static void extract_to_cancel_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;
    XtDestroyWidget(XtParent(w));
}

static void extract_to_ok_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    ExtractToCtx *ctx = (ExtractToCtx *)client_data;
    XmFileSelectionBoxCallbackStruct *cbs = (XmFileSelectionBoxCallbackStruct *)call_data;
    if (!ctx || !cbs) {
        XtDestroyWidget(XtParent(w));
        return;
    }

    char *chosen_dir = nullptr;
    if (!XmStringGetLtoR(cbs->value, XmFONTLIST_DEFAULT_TAG, &chosen_dir) ||
        !chosen_dir || chosen_dir[0] != '/') {
        XtFree(chosen_dir);
        return;
    }

    char created[PATH_MAX];
    char err[512];
    DtfileArchiveStatus st = DtfileArchiveExtract(
        ctx->archive_path, chosen_dir, 0, created, sizeof(created), err, sizeof(err));
    if (st != DTFILE_ARCHIVE_OK) {
        arc_post_error(w, "Extract Archive Error", err);
        XtFree(chosen_dir);
        return;
    }

    arc_refresh_and_select(ctx->fmd, chosen_dir, created);
    if (ctx->fmd)
        ShowNewDirectory(ctx->fmd, ctx->fmd->host ? ctx->fmd->host : home_host_name, chosen_dir);
    XtFree(chosen_dir);
    XtDestroyWidget(XtParent(w));
}

static void show_extract_to_dialog(FileMgrData *fmd, const char *archive_path) {
    if (!archive_path)
        return;
    Widget parent = (fmd && fmd->file_mgr_rec) ? ((FileMgrRec *)fmd->file_mgr_rec)->shell : toplevel;
    ExtractToCtx *ctx = (ExtractToCtx *)XtCalloc(1, sizeof(ExtractToCtx));
    ctx->fmd = fmd;
    ctx->archive_path = XtNewString(archive_path);

    Arg args[8];
    Cardinal n = 0;
    XmString dir_str = XmStringCreateLocalized(
        (fmd && fmd->current_directory) ? fmd->current_directory : "/");
    XmString ok_str = XmStringCreateLocalized("Extract");
    XtSetArg(args[n], XmNfileTypeMask, XmFILE_DIRECTORY); n++;
    XtSetArg(args[n], XmNdirectory, dir_str); n++;
    XtSetArg(args[n], XmNokLabelString, ok_str); n++;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    Widget dlg = XmCreateFileSelectionDialog(parent, "extractToDialog", args, n);
    XmStringFree(dir_str);
    XmStringFree(ok_str);

    XtVaSetValues(XtParent(dlg), XmNtitle, "Extract Archive To Folder", nullptr);
    Widget help_btn = XmFileSelectionBoxGetChild(dlg, XmDIALOG_HELP_BUTTON);
    if (help_btn)
        XtUnmanageChild(help_btn);

    XtAddCallback(dlg, XmNokCallback, extract_to_ok_cb, (XtPointer)ctx);
    XtAddCallback(dlg, XmNcancelCallback, extract_to_cancel_cb, (XtPointer)ctx);
    XtAddCallback(dlg, XmNdestroyCallback, extract_to_destroy_cb, (XtPointer)ctx);
    XtManageChild(dlg);
}

void ExtractToMenuCB(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;
    FileMgrData *fmd = arc_resolve_fmd_from_menu(w);
    char **paths = nullptr;
    int count = arc_collect_selection(fmd, nullptr, &paths);
    if (count == 1 && DtfileIsArchiveFile(paths[0]))
        show_extract_to_dialog(fmd, paths[0]);
    arc_free_paths(paths, count);
}

void ExtractToPopupCB(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    FileViewData *fvd = (FileViewData *)client_data;
    if (!fvd || !fvd->directory_set)
        return;
    FileMgrData *fmd = (FileMgrData *)((DirectorySet *)fvd->directory_set)->file_mgr_data;
    if (fmd)
        fmd->popup_menu_icon = nullptr;
    char *p = arc_build_fvd_path(fvd);
    if (p) {
        if (DtfileIsArchiveFile(p))
            show_extract_to_dialog(fmd, p);
        XtFree(p);
    }
}

/* ---- Compress... Motif Dialog ---- */

typedef struct {
    Widget shell;
    Widget name_tf;
    DtfileArchiveFormat selected_fmt;
    FileMgrData *fmd;
    char *dest_dir;
    char **input_paths;
    int input_count;
} CompressDialogCtx;

static void compress_dlg_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    CompressDialogCtx *ctx = (CompressDialogCtx *)client_data;
    if (!ctx) return;
    XtFree(ctx->dest_dir);
    arc_free_paths(ctx->input_paths, ctx->input_count);
    XtFree((char *)ctx);
}

static void compress_fmt_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)call_data;
    CompressDialogCtx *ctx = (CompressDialogCtx *)client_data;
    if (!ctx) return;
    XtPointer ud = nullptr;
    XtVaGetValues(w, XmNuserData, &ud, nullptr);
    ctx->selected_fmt = (DtfileArchiveFormat)(uintptr_t)ud;
}

static void compress_cancel_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    CompressDialogCtx *ctx = (CompressDialogCtx *)client_data;
    if (ctx && ctx->shell)
        XtDestroyWidget(ctx->shell);
}

static void compress_ok_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    CompressDialogCtx *ctx = (CompressDialogCtx *)client_data;
    if (!ctx) return;

    char *raw_name = XmTextFieldGetString(ctx->name_tf);
    if (!raw_name || !raw_name[0] || strchr(raw_name, '/') != nullptr ||
        strcmp(raw_name, ".") == 0 || strcmp(raw_name, "..") == 0) {
        XtFree(raw_name);
        arc_post_error(ctx->shell, "Compress Error",
                       "Please enter a valid archive file name without '/'.");
        return;
    }

    const char *ext = DtfileArchiveFormatExtension(ctx->selected_fmt);
    char full_leaf[NAME_MAX + 1];
    if (has_suffix_ci(raw_name, ext)) {
        snprintf(full_leaf, sizeof(full_leaf), "%s", raw_name);
    } else {
        snprintf(full_leaf, sizeof(full_leaf), "%s%s", raw_name, ext);
    }
    XtFree(raw_name);

    char out_path[PATH_MAX];
    if (!choose_disambiguated_path(ctx->dest_dir, full_leaf, out_path, sizeof(out_path), NULL, 0)) {
        arc_post_error(ctx->shell, "Compress Error", "Could not construct output archive path.");
        return;
    }

    char err[512];
    DtfileArchiveStatus st = DtfileArchiveCreate(
        (const char *const *)ctx->input_paths,
        ctx->input_count,
        out_path,
        ctx->selected_fmt,
        err,
        sizeof(err));
    if (st != DTFILE_ARCHIVE_OK) {
        arc_post_error(ctx->shell, "Compress Error", err);
        return;
    }

    arc_refresh_and_select(ctx->fmd, ctx->dest_dir, out_path);
    XtDestroyWidget(ctx->shell);
}

static void show_compress_dialog(FileMgrData *fmd, char **paths, int count) {
    if (!paths || count <= 0) {
        arc_free_paths(paths, count);
        return;
    }

    Widget parent = (fmd && fmd->file_mgr_rec) ? ((FileMgrRec *)fmd->file_mgr_rec)->shell : toplevel;
    CompressDialogCtx *ctx = (CompressDialogCtx *)XtCalloc(1, sizeof(CompressDialogCtx));
    ctx->fmd = fmd;
    ctx->input_paths = paths;
    ctx->input_count = count;
    ctx->selected_fmt = DTFILE_ARCHIVE_FMT_ZIP;

    if (fmd && fmd->current_directory && fmd->current_directory[0] == '/') {
        ctx->dest_dir = XtNewString(fmd->current_directory);
    } else {
        const char *sl = strrchr(paths[0], '/');
        if (sl && sl != paths[0]) {
            size_t dl = (size_t)(sl - paths[0]);
            ctx->dest_dir = (char *)XtMalloc((Cardinal)dl + 1);
            memcpy(ctx->dest_dir, paths[0], dl);
            ctx->dest_dir[dl] = '\0';
        } else {
            ctx->dest_dir = XtNewString("/");
        }
    }

    char default_stem[NAME_MAX + 1];
    if (count == 1) {
        strip_archive_stem(paths[0], default_stem, sizeof(default_stem));
    } else {
        const char *dsl = strrchr(ctx->dest_dir, '/');
        if (dsl && dsl[1] != '\0')
            snprintf(default_stem, sizeof(default_stem), "%s", dsl + 1);
        else
            snprintf(default_stem, sizeof(default_stem), "Archive");
    }

    Arg args[12];
    Cardinal n = 0;
    XtSetArg(args[n], XmNtitle, "Create Archive"); n++;
    XtSetArg(args[n], XmNdeleteResponse, XmDESTROY); n++;
    XtSetArg(args[n], XmNallowShellResize, True); n++;
    ctx->shell = XmCreateDialogShell(parent, "compressDialogShell", args, n);
    XtAddCallback(ctx->shell, XmNdestroyCallback, compress_dlg_destroy_cb, (XtPointer)ctx);

    n = 0;
    XtSetArg(args[n], XmNmarginWidth, 14); n++;
    XtSetArg(args[n], XmNmarginHeight, 12); n++;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    Widget form = XmCreateForm(ctx->shell, "compressForm", args, n);

    char hdr_buf[384];
    snprintf(hdr_buf, sizeof(hdr_buf), "Compress %d selected item%s into %s:",
             count, count == 1 ? "" : "s", ctx->dest_dir);
    XmString hdr_xm = XmStringCreateLocalized(hdr_buf);
    Widget hdr_lbl = XtVaCreateManagedWidget(
        "compressHeader", xmLabelWidgetClass, form,
        XmNlabelString, hdr_xm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(hdr_xm);

    XmString name_xm = XmStringCreateLocalized("Archive Name:");
    Widget name_lbl = XtVaCreateManagedWidget(
        "compressNameLabel", xmLabelWidgetClass, form,
        XmNlabelString, name_xm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, hdr_lbl,
        XmNtopOffset, 12,
        XmNleftAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(name_xm);

    ctx->name_tf = XtVaCreateManagedWidget(
        "compressNameText", xmTextFieldWidgetClass, form,
        XmNcolumns, 32,
        XmNvalue, default_stem,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, name_lbl,
        XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    Widget pulldown = XmCreatePulldownMenu(form, "compressFmtPulldown", nullptr, 0);
    static const struct {
        const char *label;
        DtfileArchiveFormat fmt;
    } fmt_opts[] = {
        { "ZIP Archive (.zip)",            DTFILE_ARCHIVE_FMT_ZIP },
        { "Tar + Gzip (.tar.gz)",          DTFILE_ARCHIVE_FMT_TAR_GZ },
        { "Tar + XZ (.tar.xz)",            DTFILE_ARCHIVE_FMT_TAR_XZ },
        { "Tar + Zstandard (.tar.zst)",    DTFILE_ARCHIVE_FMT_TAR_ZST },
        { "7-Zip Archive (.7z)",           DTFILE_ARCHIVE_FMT_7Z },
    };
    Widget first_item = nullptr;
    for (size_t fi = 0; fi < sizeof(fmt_opts) / sizeof(fmt_opts[0]); ++fi) {
        XmString fxm = XmStringCreateLocalized((char *)fmt_opts[fi].label);
        Widget item = XtVaCreateManagedWidget(
            "fmtItem", xmPushButtonGadgetClass, pulldown,
            XmNlabelString, fxm,
            XmNuserData, (XtPointer)(uintptr_t)fmt_opts[fi].fmt,
            nullptr);
        XmStringFree(fxm);
        XtAddCallback(item, XmNactivateCallback, compress_fmt_cb, (XtPointer)ctx);
        if (fi == 0) first_item = item;
    }

    XmString fmt_xm = XmStringCreateLocalized("Format:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, fmt_xm); n++;
    XtSetArg(args[n], XmNsubMenuId, pulldown); n++;
    XtSetArg(args[n], XmNmenuHistory, first_item); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, ctx->name_tf); n++;
    XtSetArg(args[n], XmNtopOffset, 10); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    Widget opt_menu = XmCreateOptionMenu(form, "compressFormatMenu", args, n);
    XmStringFree(fmt_xm);
    XtManageChild(opt_menu);

    Widget sep = XtVaCreateManagedWidget(
        "compressSep", xmSeparatorWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, opt_menu,
        XmNtopOffset, 12,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    XmString ok_xm = XmStringCreateLocalized("Create");
    Widget ok_btn = XtVaCreateManagedWidget(
        "compressCreateBtn", xmPushButtonWidgetClass, form,
        XmNlabelString, ok_xm,
        XmNwidth, 96,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, sep,
        XmNtopOffset, 10,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(ok_xm);
    XtAddCallback(ok_btn, XmNactivateCallback, compress_ok_cb, (XtPointer)ctx);

    XmString cancel_xm = XmStringCreateLocalized("Cancel");
    Widget cancel_btn = XtVaCreateManagedWidget(
        "compressCancelBtn", xmPushButtonWidgetClass, form,
        XmNlabelString, cancel_xm,
        XmNwidth, 96,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, sep,
        XmNtopOffset, 10,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(cancel_xm);
    XtAddCallback(cancel_btn, XmNactivateCallback, compress_cancel_cb, (XtPointer)ctx);

    XtVaSetValues(form,
                  XmNdefaultButton, ok_btn,
                  XmNcancelButton, cancel_btn,
                  nullptr);
    XtManageChild(form);
}

void CompressMenuCB(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;
    FileMgrData *fmd = arc_resolve_fmd_from_menu(w);
    char **paths = nullptr;
    int count = arc_collect_selection(fmd, nullptr, &paths);
    if (count > 0)
        show_compress_dialog(fmd, paths, count);
}

void CompressPopupCB(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    FileViewData *fvd = (FileViewData *)client_data;
    if (!fvd || !fvd->directory_set)
        return;
    FileMgrData *fmd = (FileMgrData *)((DirectorySet *)fvd->directory_set)->file_mgr_data;
    if (fmd)
        fmd->popup_menu_icon = nullptr;
    char **paths = nullptr;
    int count = arc_collect_selection(fmd, fvd, &paths);
    if (count > 0)
        show_compress_dialog(fmd, paths, count);
}

#endif /* !DTFILE_ARCHIVE_CORE_ONLY */
