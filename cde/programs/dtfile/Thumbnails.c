/*
 * Thumbnails.c - FreeDesktop Shared Thumbnail Cache & PNG Raster Preview
 *                Resolver for CK-Core CDE File Manager (WP-16).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <Xm/Xm.h>

#include "Encaps.h"
#include "SharedProcs.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "Thumbnails.h"

#define DTFILE_MAX_PNG_BYTES (8U * 1024U * 1024U)
#define DTFILE_MAX_PNG_DIM   2048U

/* ---- Minimal RFC 1321 MD5 implementation (zero external crypto dep) ---- */

typedef struct {
    uint32_t state[4];
    uint64_t bit_count;
    unsigned char buffer[64];
} DtMd5Ctx;

static uint32_t md5_rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32U - n));
}

static void md5_transform(uint32_t state[4], const unsigned char block[64]) {
    static const uint32_t k[64] = {
        0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU,
        0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
        0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
        0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
        0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
        0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
        0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
        0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
        0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
        0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
        0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U,
        0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
        0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U,
        0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
        0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
        0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U
    };
    static const uint32_t s[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };

    uint32_t m[16];
    for (unsigned int i = 0; i < 16; ++i) {
        m[i] = ((uint32_t)block[i * 4]) |
               ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    for (unsigned int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5U * i + 1U) % 16U;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3U * i + 5U) % 16U;
        } else {
            f = c ^ (b | (~d));
            g = (7U * i) % 16U;
        }
        f = f + a + k[i] + m[g];
        a = d;
        d = c;
        c = b;
        b = b + md5_rotl(f, s[i]);
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(DtMd5Ctx *ctx) {
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU;
    ctx->state[3] = 0x10325476U;
    ctx->bit_count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void md5_update(DtMd5Ctx *ctx, const unsigned char *data, size_t len) {
    size_t idx = (size_t)((ctx->bit_count >> 3) & 0x3FU);
    ctx->bit_count += (uint64_t)len * 8ULL;

    size_t i = 0;
    if (idx > 0) {
        size_t need = 64U - idx;
        if (len < need) {
            memcpy(ctx->buffer + idx, data, len);
            return;
        }
        memcpy(ctx->buffer + idx, data, need);
        md5_transform(ctx->state, ctx->buffer);
        i = need;
    }
    for (; i + 64U <= len; i += 64U) {
        md5_transform(ctx->state, data + i);
    }
    if (i < len) {
        memcpy(ctx->buffer, data + i, len - i);
    }
}

static void md5_final_hex(DtMd5Ctx *ctx, char hex_out[33]) {
    unsigned char pad[64];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80U;

    uint64_t bits = ctx->bit_count;
    size_t idx = (size_t)((bits >> 3) & 0x3FU);
    size_t pad_len = (idx < 56U) ? (56U - idx) : (120U - idx);
    md5_update(ctx, pad, pad_len);

    unsigned char len_bytes[8];
    for (unsigned int i = 0; i < 8; ++i) {
        len_bytes[i] = (unsigned char)((bits >> (i * 8U)) & 0xFFU);
    }
    md5_update(ctx, len_bytes, 8);

    static const char hex_digits[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 4; ++i) {
        uint32_t w = ctx->state[i];
        for (unsigned int b = 0; b < 4; ++b) {
            unsigned char byte = (unsigned char)((w >> (b * 8U)) & 0xFFU);
            hex_out[(i * 4U + b) * 2U] = hex_digits[(byte >> 4) & 0x0FU];
            hex_out[(i * 4U + b) * 2U + 1U] = hex_digits[byte & 0x0FU];
        }
    }
    hex_out[32] = '\0';
}

/* RFC 3986 path character set used by GLib g_filename_to_uri / FreeDesktop Thumbnails */
static int is_uri_path_unescaped(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    switch (c) {
        case '-': case '_': case '.': case '~': case '/':
        case '!': case '$': case '&': case '\'': case '(':
        case ')': case '*': case '+': case ',': case ';':
        case '=': case ':': case '@':
            return 1;
        default:
            return 0;
    }
}

/* Strict RFC 3986 unreserved + '/' (used by Python Path.as_uri() / QUrl) */
static int is_uri_strict_unreserved(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    switch (c) {
        case '-': case '_': case '.': case '~': case '/':
            return 1;
        default:
            return 0;
    }
}

static Boolean encode_file_uri_mode(const char *abs_path, int strict_mode, char *uri_out, size_t uri_sz) {
    static const char hex_upper[] = "0123456789ABCDEF";
    if (!abs_path || abs_path[0] != '/' || !uri_out || uri_sz < 10)
        return False;

    const char *prefix = "file://";
    size_t pos = strlen(prefix);
    memcpy(uri_out, prefix, pos);

    for (const unsigned char *p = (const unsigned char *)abs_path; *p; ++p) {
        int keep = strict_mode ? is_uri_strict_unreserved(*p) : is_uri_path_unescaped(*p);
        if (keep) {
            if (pos + 2 > uri_sz) return False;
            uri_out[pos++] = (char)*p;
        } else {
            if (pos + 4 > uri_sz) return False;
            uri_out[pos++] = '%';
            uri_out[pos++] = hex_upper[(*p >> 4) & 0x0FU];
            uri_out[pos++] = hex_upper[*p & 0x0FU];
        }
    }
    uri_out[pos] = '\0';
    return True;
}

static Boolean encode_file_uri(const char *abs_path, char *uri_out, size_t uri_sz) {
    return encode_file_uri_mode(abs_path, 0, uri_out, uri_sz);
}

Boolean DtfileComputeThumbnailUriAndMd5(
    const char *full_path,
    char *uri_out,
    size_t uri_sz,
    char md5_hex_out[33])
{
    char local_uri[PATH_MAX * 3 + 16];
    char resolved[PATH_MAX];
    const char *use_path = full_path;

    if (!full_path || full_path[0] != '/' || !md5_hex_out)
        return False;

    if (realpath(full_path, resolved) != nullptr)
        use_path = resolved;

    char *dst_uri = uri_out ? uri_out : local_uri;
    size_t dst_sz = uri_out ? uri_sz : sizeof(local_uri);
    if (!encode_file_uri(use_path, dst_uri, dst_sz))
        return False;

    DtMd5Ctx ctx;
    md5_init(&ctx);
    md5_update(&ctx, (const unsigned char *)dst_uri, strlen(dst_uri));
    md5_final_hex(&ctx, md5_hex_out);
    return True;
}

/* Inspect PNG header, dimensions, and optional tEXt Thumb::MTime metadata */
static Boolean inspect_png_file(
    const char *png_path,
    unsigned int *out_w,
    unsigned int *out_h,
    Boolean *has_thumb_mtime,
    long long *thumb_mtime_out,
    time_t *file_mtime_out)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (has_thumb_mtime) *has_thumb_mtime = False;
    if (thumb_mtime_out) *thumb_mtime_out = 0;
    if (file_mtime_out) *file_mtime_out = 0;

    if (!png_path || !png_path[0])
        return False;

    int fd = open(png_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return False;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 33 || (uintmax_t)st.st_size > DTFILE_MAX_PNG_BYTES) {
        close(fd);
        return False;
    }
    if (file_mtime_out)
        *file_mtime_out = st.st_mtime;

    unsigned char hdr[24];
    ssize_t nr = read(fd, hdr, sizeof(hdr));
    if (nr != (ssize_t)sizeof(hdr)) {
        close(fd);
        return False;
    }

    static const unsigned char png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (memcmp(hdr, png_sig, 8) != 0 || memcmp(hdr + 12, "IHDR", 4) != 0) {
        close(fd);
        return False;
    }

    uint32_t w = ((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) |
                 ((uint32_t)hdr[18] << 8)  | (uint32_t)hdr[19];
    uint32_t h = ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) |
                 ((uint32_t)hdr[22] << 8)  | (uint32_t)hdr[23];
    if (w == 0 || h == 0 || w > DTFILE_MAX_PNG_DIM || h > DTFILE_MAX_PNG_DIM) {
        close(fd);
        return False;
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;

    /* Scan pre-IDAT chunks (up to 16 chunks) for tEXt Thumb::MTime */
    if (has_thumb_mtime && thumb_mtime_out) {
        uint32_t ihdr_len = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                            ((uint32_t)hdr[10] << 8) | (uint32_t)hdr[11];
        off_t next_off = (off_t)8 + 8 + (off_t)ihdr_len + 4;
        if (ihdr_len <= 64 && lseek(fd, next_off, SEEK_SET) == next_off) {
            for (int ci = 0; ci < 16; ++ci) {
                unsigned char chdr[8];
                if (read(fd, chdr, 8) != 8)
                    break;
                uint32_t clen = ((uint32_t)chdr[0] << 24) | ((uint32_t)chdr[1] << 16) |
                                ((uint32_t)chdr[2] << 8)  | (uint32_t)chdr[3];
                if (memcmp(chdr + 4, "IDAT", 4) == 0 || memcmp(chdr + 4, "IEND", 4) == 0)
                    break;
                if (memcmp(chdr + 4, "tEXt", 4) == 0 && clen > 13 && clen <= 512) {
                    char tbuf[513];
                    if (read(fd, tbuf, clen) == (ssize_t)clen) {
                        tbuf[clen] = '\0';
                        if (strcmp(tbuf, "Thumb::MTime") == 0) {
                            const char *val = tbuf + 13;
                            char *endp = nullptr;
                            errno = 0;
                            long long mt = strtoll(val, &endp, 10);
                            if (errno == 0 && endp && endp != val) {
                                *has_thumb_mtime = True;
                                *thumb_mtime_out = mt;
                            }
                            break;
                        }
                    } else {
                        break;
                    }
                    if (lseek(fd, 4, SEEK_CUR) < 0)
                        break;
                } else {
                    if (clen > DTFILE_MAX_PNG_BYTES)
                        break;
                    if (lseek(fd, (off_t)clen + 4, SEEK_CUR) < 0)
                        break;
                }
            }
        }
    }

    close(fd);
    return True;
}

Boolean DtfileLooksLikePngIcon(
    const char *path,
    unsigned int *out_w,
    unsigned int *out_h)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!path || !path[0])
        return False;
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".png") != 0)
        return False;
    return inspect_png_file(path, out_w, out_h, nullptr, nullptr, nullptr);
}

static void get_thumbnail_cache_root(char *out, size_t out_sz) {
    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    if (xdg_cache && xdg_cache[0] == '/') {
        snprintf(out, out_sz, "%s/thumbnails", xdg_cache);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = users_home_dir;
    size_t hlen = home ? strlen(home) : 0;
    while (hlen > 1 && home[hlen - 1] == '/')
        hlen--;
    if (hlen > 0)
        snprintf(out, out_sz, "%.*s/.cache/thumbnails", (int)hlen, home);
    else
        out[0] = '\0';
}

static void log_resolved_thumbnail(const char *src_path, const char *thumb_path) {
    const char *log_file = getenv("DTFILE_THUMBNAIL_LOG");
    if (!log_file || !log_file[0] || !src_path || !thumb_path)
        return;
    int fd = open(log_file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    char line[PATH_MAX * 2 + 16];
    int n = snprintf(line, sizeof(line), "%s\t%s\n", src_path, thumb_path);
    if (n > 0) {
        size_t wn = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        ssize_t wr = write(fd, line, wn);
        (void)wr;
    }
    close(fd);
}

static char *try_resolve_md5_thumbnail(
    const char *src_path,
    const struct stat *src_st,
    const char *cache_root,
    const char *md5_hex,
    Boolean prefer_large,
    unsigned int *out_w,
    unsigned int *out_h)
{
    const char *dirs[2] = {
        prefer_large ? "large" : "normal",
        prefer_large ? "normal" : "large"
    };

    for (int i = 0; i < 2; ++i) {
        char candidate[PATH_MAX];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s/%s.png",
                         cache_root, dirs[i], md5_hex);
        if (n <= 0 || (size_t)n >= sizeof(candidate))
            continue;

        unsigned int w = 0, h = 0;
        Boolean has_tm = False;
        long long tm_val = 0;
        time_t file_mtime = 0;
        if (!inspect_png_file(candidate, &w, &h, &has_tm, &tm_val, &file_mtime))
            continue;

        /* Verify freshness against source mtime */
        if (has_tm) {
            if (tm_val != (long long)src_st->st_mtime)
                continue;
        } else {
            if (file_mtime < src_st->st_mtime)
                continue;
        }

        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        log_resolved_thumbnail(src_path, candidate);
        return XtNewString(candidate);
    }
    return nullptr;
}

char *DtfileResolveImageOrThumbnail(
    const char *full_path,
    Boolean enabled,
    int icon_size,
    unsigned int *out_w,
    unsigned int *out_h)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!enabled || !full_path || full_path[0] != '/')
        return nullptr;

    struct stat src_st;
    if (stat(full_path, &src_st) != 0 || S_ISDIR(src_st.st_mode))
        return nullptr;

    /* 1. Direct XPM/XBM/PM/BM or PNG instance icons (for small raster/icon files) */
    if (DtfileLooksLikeImageIcon(full_path, out_w, out_h))
        return XtNewString(full_path);

    /* 2. Check FreeDesktop shared thumbnail cache first so large photos/PDFs/videos/SVGs
     *    use pre-scaled 128x128 or 256x256 cached thumbnails when available! */
    char cache_root[PATH_MAX];
    get_thumbnail_cache_root(cache_root, sizeof(cache_root));
    if (cache_root[0] != '\0' && strncmp(full_path, cache_root, strlen(cache_root)) != 0) {
        Boolean prefer_large = (icon_size == LARGE || icon_size == EXTRA_LARGE);
        char md5_canon[33] = {0};
        if (DtfileComputeThumbnailUriAndMd5(full_path, nullptr, 0, md5_canon)) {
            char *hit = try_resolve_md5_thumbnail(full_path, &src_st, cache_root,
                                                  md5_canon, prefer_large, out_w, out_h);
            if (hit)
                return hit;
        }
        /* Also check strict RFC-3986 unreserved URI digest (Python Path.as_uri / QUrl) */
        char resolved_p[PATH_MAX];
        const char *canon_p = realpath(full_path, resolved_p) ? resolved_p : full_path;
        char strict_uri[PATH_MAX * 3 + 16];
        if (encode_file_uri_mode(canon_p, 1, strict_uri, sizeof(strict_uri))) {
            char md5_strict[33];
            DtMd5Ctx sctx;
            md5_init(&sctx);
            md5_update(&sctx, (const unsigned char *)strict_uri, strlen(strict_uri));
            md5_final_hex(&sctx, md5_strict);
            if (strcmp(md5_strict, md5_canon) != 0) {
                char *hit = try_resolve_md5_thumbnail(full_path, &src_st, cache_root,
                                                      md5_strict, prefer_large, out_w, out_h);
                if (hit)
                    return hit;
            }
        }
        /* Fallback: literal non-realpath URI if full_path is a symlink */
        char lit_uri[PATH_MAX * 3 + 16];
        if (encode_file_uri(full_path, lit_uri, sizeof(lit_uri))) {
            char md5_lit[33];
            DtMd5Ctx ctx;
            md5_init(&ctx);
            md5_update(&ctx, (const unsigned char *)lit_uri, strlen(lit_uri));
            md5_final_hex(&ctx, md5_lit);
            if (strcmp(md5_lit, md5_canon) != 0) {
                char *hit = try_resolve_md5_thumbnail(full_path, &src_st, cache_root,
                                                      md5_lit, prefer_large, out_w, out_h);
                if (hit)
                    return hit;
            }
        }
    }

    /* 3. Direct PNG image preview if bounded */
    if (DtfileLooksLikePngIcon(full_path, out_w, out_h))
        return XtNewString(full_path);

    return nullptr;
}
