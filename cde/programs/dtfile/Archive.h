/*
 * Archive.h - Built-in Archive Extract & Compress Actions + Zip-Slip Hardening
 *             for CK-Core CDE File Manager (Wayland & X11/XLibre, WP-16).
 */

#ifndef _Dtfile_Archive_h
#define _Dtfile_Archive_h

#include <stddef.h>

#ifndef DTFILE_ARCHIVE_CORE_ONLY
#include <X11/Intrinsic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DTFILE_ARCHIVE_FMT_NONE = 0,
    DTFILE_ARCHIVE_FMT_ZIP,
    DTFILE_ARCHIVE_FMT_TAR_GZ,
    DTFILE_ARCHIVE_FMT_TAR_XZ,
    DTFILE_ARCHIVE_FMT_TAR_ZST,
    DTFILE_ARCHIVE_FMT_TAR_BZ2,
    DTFILE_ARCHIVE_FMT_TAR,
    DTFILE_ARCHIVE_FMT_7Z
} DtfileArchiveFormat;

typedef enum {
    DTFILE_ARCHIVE_OK = 0,
    DTFILE_ARCHIVE_ERR_INVALID = -1,
    DTFILE_ARCHIVE_ERR_IO = -2,
    DTFILE_ARCHIVE_ERR_FORMAT = -3,
    DTFILE_ARCHIVE_ERR_UNSAFE_PATH = -4,
    DTFILE_ARCHIVE_ERR_UNSAFE_LINK = -5,
    DTFILE_ARCHIVE_ERR_UNSUPPORTED_ENTRY = -6
} DtfileArchiveStatus;

/* Return non-zero format enum if `path` has a supported archive extension. */
DtfileArchiveFormat DtfileArchiveDetectFormat(const char *path);

/* Return canonical extension (e.g. ".zip", ".tar.gz", ".7z") for `fmt`. */
const char *DtfileArchiveFormatExtension(DtfileArchiveFormat fmt);

/*
 * Safely extract `archive_path` into `dest_dir`.
 * - Rejects empty paths, leading '/', '.', '..', NUL bytes, Windows drive
 *   prefixes, device/FIFO/socket nodes, and symlinks/hardlinks escaping root.
 * - Uses descriptor-relative openat/mkdirat/symlinkat with O_NOFOLLOW so
 *   intra-archive symlinks can never redirect writes outside the target tree.
 * - When `smart_subfolder` is non-zero (Extract Here), multi-root archives
 *   extract into a disambiguated `<stem>` subdirectory and single-root
 *   archives disambiguate `<root> (2)` instead of overwriting existing files.
 * - Writes the created top-level path into `created_path_out` (if non-NULL).
 */
DtfileArchiveStatus DtfileArchiveExtract(
    const char *archive_path,
    const char *dest_dir,
    int smart_subfolder,
    char *created_path_out,
    size_t created_path_sz,
    char *err_out,
    size_t err_sz);

/*
 * Compress `input_count` files/directories (`input_paths`) into `output_archive_path`
 * using `fmt` (.zip, .tar.gz, .tar.xz, .tar.zst, .tar.bz2, .tar, .7z).
 * Stages atomically into a sibling `.part.XXXXXX` file and renames into place.
 */
DtfileArchiveStatus DtfileArchiveCreate(
    const char *const *input_paths,
    int input_count,
    const char *output_archive_path,
    DtfileArchiveFormat fmt,
    char *err_out,
    size_t err_sz);

#ifndef DTFILE_ARCHIVE_CORE_ONLY
Boolean DtfileIsArchiveFile(const char *path);

void ExtractHereMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void ExtractHerePopupCB(Widget w, XtPointer client_data, XtPointer call_data);

void ExtractToMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void ExtractToPopupCB(Widget w, XtPointer client_data, XtPointer call_data);

void CompressMenuCB(Widget w, XtPointer client_data, XtPointer call_data);
void CompressPopupCB(Widget w, XtPointer client_data, XtPointer call_data);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _Dtfile_Archive_h */
