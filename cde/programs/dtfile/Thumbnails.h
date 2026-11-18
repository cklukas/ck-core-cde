/*
 * Thumbnails.h - FreeDesktop Shared Thumbnail Cache & PNG Raster Preview
 *                Resolver for CK-Core CDE File Manager (WP-16).
 */

#ifndef _Dtfile_Thumbnails_h
#define _Dtfile_Thumbnails_h

#include <X11/Intrinsic.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Canonicalize an absolute filesystem path to a FreeDesktop "file:///..." URI
 * and compute its lowercase 32-hex RFC 1321 MD5 digest.
 */
Boolean DtfileComputeThumbnailUriAndMd5(
    const char *full_path,
    char *uri_out,
    size_t uri_sz,
    char md5_hex_out[33]);

/*
 * Check whether `path` is a safe, bounded PNG image file (1..2048 px, <= 8 MiB).
 */
Boolean DtfileLooksLikePngIcon(
    const char *path,
    unsigned int *out_w,
    unsigned int *out_h);

/*
 * Resolve either a direct raster/vector icon (.pm, .bm, .xpm, .xbm, .png) or a
 * fresh FreeDesktop shared thumbnail ($XDG_CACHE_HOME/thumbnails/{large,normal}/<md5>.png)
 * when `enabled` (render_image_icons) is True.
 *
 * Returns a newly XtMalloc'd path string owned by the caller, or NULL.
 */
char *DtfileResolveImageOrThumbnail(
    const char *full_path,
    Boolean enabled,
    int icon_size,
    unsigned int *out_w,
    unsigned int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* _Dtfile_Thumbnails_h */
