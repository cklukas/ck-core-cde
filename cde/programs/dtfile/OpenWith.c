/*
 * OpenWith.c - Motif "Open With..." Dialog & XDG mimeapps.list Integration
 *              for CK-Core CDE File Manager (Wayland & X11/XLibre, WP-12).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Separator.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <Xm/Wayland.h>

#include <Dt/Dts.h>

#include "Encaps.h"
#include "SharedProcs.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "OpenWith.h"

#define OW_MAX_APPS 384

typedef struct {
    char desktop_id[128];
    char name[128];
    char comment[160];
    char raw_exec[512];
    char work_dir[256];
    bool terminal;
    bool mime_compatible;
    bool is_current_default;
} OpenWithApp;

typedef struct {
    Widget shell;
    Widget list_w;
    Widget cmd_tf;
    Widget def_toggle;
    char mime_type[128];
    int file_count;
    char **file_paths;
    int app_count;
    OpenWithApp apps[OW_MAX_APPS];
    bool updating_cmd;
} OpenWithDialogCtx;

static void ow_copy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void ow_trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void ow_append_quoted(char *dst, size_t dst_sz, const char *arg) {
    if (!dst || dst_sz == 0 || !arg) return;
    size_t cur = strlen(dst);
    if (cur + 3 >= dst_sz) return;
    dst[cur++] = '\'';
    for (const char *p = arg; *p; ++p) {
        if (*p == '\'') {
            if (cur + 5 >= dst_sz) break;
            memcpy(dst + cur, "'\\''", 4);
            cur += 4;
        } else {
            if (cur + 2 >= dst_sz) break;
            dst[cur++] = *p;
        }
    }
    if (cur + 1 < dst_sz) dst[cur++] = '\'';
    dst[cur] = '\0';
}

static char *ow_build_full_path(FileViewData *fvd) {
    if (!fvd || !fvd->file_data) return nullptr;
    DirectorySet *ds = (DirectorySet *)fvd->directory_set;
    if (!ds || !ds->name || !fvd->file_data->file_name) return nullptr;
    const char *dir = ds->name;
    const char *fn = fvd->file_data->file_name;
    if (strcmp(fn, ".") == 0) return XtNewString(dir);
    if (strcmp(fn, "..") == 0) return nullptr;
    size_t dlen = strlen(dir);
    char *out = (char *)XtMalloc(dlen + strlen(fn) + 2);
    strcpy(out, dir);
    if (dlen == 0 || out[dlen - 1] != '/') strcat(out, "/");
    strcat(out, fn);
    return out;
}

void DtfileDetectMimeType(const char *file_path, char *out_mime, size_t out_sz) {
    if (!out_mime || out_sz == 0) return;
    ow_copy(out_mime, out_sz, "application/octet-stream");
    if (!file_path || !file_path[0]) return;

    FILE *fp = fopen(file_path, "rb");
    if (fp) {
        unsigned char hdr[64];
        size_t nr = fread(hdr, 1, sizeof(hdr), fp);
        fclose(fp);
        if (nr >= 5 && memcmp(hdr, "%PDF-", 5) == 0) {
            ow_copy(out_mime, out_sz, "application/pdf");
            return;
        }
        if (nr >= 8 && memcmp(hdr, "\x89PNG\r\n\x1a\n", 8) == 0) {
            ow_copy(out_mime, out_sz, "image/png");
            return;
        }
        if (nr >= 3 && hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF) {
            ow_copy(out_mime, out_sz, "image/jpeg");
            return;
        }
        if (nr >= 6 && (memcmp(hdr, "GIF87a", 6) == 0 || memcmp(hdr, "GIF89a", 6) == 0)) {
            ow_copy(out_mime, out_sz, "image/gif");
            return;
        }
        if (nr >= 12 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WEBP", 4) == 0) {
            ow_copy(out_mime, out_sz, "image/webp");
            return;
        }
        if (nr >= 4 && memcmp(hdr, "%!PS", 4) == 0) {
            ow_copy(out_mime, out_sz, "application/postscript");
            return;
        }
    }

    const char *dot = strrchr(file_path, '.');
    if (dot && dot[1]) {
        static const struct { const char *ext; const char *mime; } map[] = {
            {".pdf",  "application/pdf"},
            {".png",  "image/png"},
            {".jpg",  "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".gif",  "image/gif"},
            {".webp", "image/webp"},
            {".svg",  "image/svg+xml"},
            {".bmp",  "image/bmp"},
            {".tif",  "image/tiff"},
            {".tiff", "image/tiff"},
            {".xpm",  "image/x-xpixmap"},
            {".pm",   "image/x-xpixmap"},
            {".ps",   "application/postscript"},
            {".eps",  "application/postscript"},
            {".html", "text/html"},
            {".htm",  "text/html"},
            {".txt",  "text/plain"},
            {".md",   "text/markdown"},
            {".c",    "text/x-csrc"},
            {".h",    "text/x-chdr"},
            {".cpp",  "text/x-c++src"},
            {".py",   "text/x-python"},
            {".sh",   "application/x-shellscript"},
            {".json", "application/json"},
            {".xml",  "application/xml"},
            {".mp4",  "video/mp4"},
            {".mkv",  "video/x-matroska"},
            {".webm", "video/webm"},
            {".mov",  "video/quicktime"},
            {".avi",  "video/x-msvideo"},
            {".mp3",  "audio/mpeg"},
            {".flac", "audio/flac"},
            {".ogg",  "audio/ogg"},
            {".wav",  "audio/x-wav"},
            {".zip",  "application/zip"},
            {".gz",   "application/gzip"},
            {".xz",   "application/x-xz"},
            {".zst",  "application/zstd"},
            {".7z",   "application/x-7z-compressed"},
            {".odt",  "application/vnd.oasis.opendocument.text"},
            {".ods",  "application/vnd.oasis.opendocument.spreadsheet"},
            {".odp",  "application/vnd.oasis.opendocument.presentation"},
            {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
            {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
            {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        };
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
            if (strcasecmp(dot, map[i].ext) == 0) {
                ow_copy(out_mime, out_sz, map[i].mime);
                return;
            }
        }
    }

    char *dts_type = DtDtsFileToDataType((char *)file_path);
    if (dts_type) {
        char *dts_mime = DtDtsDataTypeToAttributeValue(dts_type, DtDTS_DA_MIME_TYPE, nullptr);
        if (dts_mime && dts_mime[0] && strchr(dts_mime, '/')) {
            ow_copy(out_mime, out_sz, dts_mime);
        }
        if (dts_mime) DtDtsFreeAttributeValue(dts_mime);
        DtDtsFreeDataType(dts_type);
        if (strcmp(out_mime, "application/octet-stream") != 0) return;
    }

    /* If text-like ASCII/UTF-8 bytes, classify as text/plain */
    fp = fopen(file_path, "rb");
    if (fp) {
        unsigned char buf[128];
        size_t nr = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        if (nr > 0) {
            bool binary = false;
            for (size_t i = 0; i < nr; ++i) {
                if (buf[i] == 0) { binary = true; break; }
            }
            if (!binary) ow_copy(out_mime, out_sz, "text/plain");
        }
    }
}

static void get_mimeapps_path(char *out, size_t out_sz) {
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (cfg && cfg[0]) {
        mkdir(cfg, 0755);
        snprintf(out, out_sz, "%s/mimeapps.list", cfg);
    } else if (home && home[0]) {
        char d[PATH_MAX];
        snprintf(d, sizeof(d), "%s/.config", home);
        mkdir(d, 0755);
        snprintf(out, out_sz, "%s/mimeapps.list", d);
    } else {
        ow_copy(out, out_sz, "/tmp/mimeapps.list");
    }
}

static void read_current_default_desktop_id(const char *mime_type, char *out_id, size_t out_sz) {
    out_id[0] = '\0';
    if (!mime_type || !mime_type[0]) return;

    char mpath[PATH_MAX];
    get_mimeapps_path(mpath, sizeof(mpath));
    FILE *fp = fopen(mpath, "r");
    if (!fp) return;

    bool in_def = false;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        ow_trim(line);
        if (line[0] == '[') {
            in_def = (strcmp(line, "[Default Applications]") == 0);
            continue;
        }
        if (!in_def || line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        ow_trim(k);
        ow_trim(v);
        if (strcasecmp(k, mime_type) == 0 && v[0]) {
            char *semi = strchr(v, ';');
            if (semi) *semi = '\0';
            ow_trim(v);
            ow_copy(out_id, out_sz, v);
            break;
        }
    }
    fclose(fp);
}

bool DtfileSetMimeAppsDefault(const char *mime_type, const char *desktop_id) {
    if (!mime_type || !mime_type[0] || !desktop_id || !desktop_id[0]) return false;

    char norm_id[128];
    if (strlen(desktop_id) > 8 && strcmp(desktop_id + strlen(desktop_id) - 8, ".desktop") == 0) {
        ow_copy(norm_id, sizeof(norm_id), desktop_id);
    } else {
        snprintf(norm_id, sizeof(norm_id), "%s.desktop", desktop_id);
    }

    char mpath[PATH_MAX], tmppath[PATH_MAX + 32];
    get_mimeapps_path(mpath, sizeof(mpath));
    snprintf(tmppath, sizeof(tmppath), "%s.%d.tmp", mpath, (int)getpid());

    /* Read existing lines, updating [Default Applications] and [Added Associations] cleanly */
    char **lines = nullptr;
    size_t line_cnt = 0, line_cap = 0;
    FILE *in = fopen(mpath, "r");
    if (in) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), in)) {
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
            if (line_cnt >= line_cap) {
                line_cap = line_cap ? line_cap * 2 : 64;
                lines = (char **)realloc(lines, line_cap * sizeof(char *));
            }
            lines[line_cnt++] = strdup(buf);
        }
        fclose(in);
    }

    FILE *out = fopen(tmppath, "w");
    if (!out) {
        for (size_t i = 0; i < line_cnt; ++i) free(lines[i]);
        free(lines);
        return false;
    }

    int cur_sec = 0; /* 0=other, 1=Default Applications, 2=Added Associations */
    bool seen_def_sec = false, wrote_def_key = false;
    bool seen_add_sec = false, wrote_add_key = false;
    size_t mlen = strlen(mime_type);

    for (size_t i = 0; i < line_cnt; ++i) {
        const char *l = lines[i];
        if (l[0] == '[') {
            if (cur_sec == 1 && !wrote_def_key) {
                fprintf(out, "%s=%s\n", mime_type, norm_id);
                wrote_def_key = true;
            } else if (cur_sec == 2 && !wrote_add_key) {
                fprintf(out, "%s=%s;\n", mime_type, norm_id);
                wrote_add_key = true;
            }
            if (strcmp(l, "[Default Applications]") == 0) {
                cur_sec = 1;
                seen_def_sec = true;
            } else if (strcmp(l, "[Added Associations]") == 0) {
                cur_sec = 2;
                seen_add_sec = true;
            } else {
                cur_sec = 0;
            }
            fprintf(out, "%s\n", l);
            continue;
        }

        if ((cur_sec == 1 || cur_sec == 2) && strncasecmp(l, mime_type, mlen) == 0 && l[mlen] == '=') {
            if (cur_sec == 1) {
                fprintf(out, "%s=%s\n", mime_type, norm_id);
                wrote_def_key = true;
            } else {
                fprintf(out, "%s=%s;\n", mime_type, norm_id);
                wrote_add_key = true;
            }
            continue;
        }
        fprintf(out, "%s\n", l);
    }

    if (cur_sec == 1 && !wrote_def_key) {
        fprintf(out, "%s=%s\n", mime_type, norm_id);
        wrote_def_key = true;
    } else if (cur_sec == 2 && !wrote_add_key) {
        fprintf(out, "%s=%s;\n", mime_type, norm_id);
        wrote_add_key = true;
    }

    if (!seen_def_sec) {
        if (line_cnt > 0) fprintf(out, "\n");
        fprintf(out, "[Default Applications]\n%s=%s\n", mime_type, norm_id);
    }
    if (!seen_add_sec) {
        fprintf(out, "\n[Added Associations]\n%s=%s;\n", mime_type, norm_id);
    }

    fclose(out);
    for (size_t i = 0; i < line_cnt; ++i) free(lines[i]);
    free(lines);

    return rename(tmppath, mpath) == 0;
}

static bool mime_list_matches(const char *semi_list, const char *mime_type) {
    if (!semi_list || !mime_type || !mime_type[0]) return false;
    const char *slash = strchr(mime_type, '/');
    size_t major_len = slash ? (size_t)(slash - mime_type) : 0;

    const char *p = semi_list;
    while (*p) {
        while (*p == ';' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != ';') end++;
        size_t len = (size_t)(end - p);
        while (len > 0 && isspace((unsigned char)p[len - 1])) len--;

        if (len == strlen(mime_type) && strncasecmp(p, mime_type, len) == 0) return true;
        if (major_len > 0 && len == major_len + 2 &&
            strncasecmp(p, mime_type, major_len) == 0 &&
            p[major_len] == '/' && p[major_len + 1] == '*') {
            return true;
        }
        p = (*end == ';') ? end + 1 : end;
    }
    return false;
}

static void expand_exec_preview(const OpenWithApp *app, char **files, int file_count, char *out, size_t out_sz) {
    out[0] = '\0';
    if (!app || !app->raw_exec[0]) return;
    size_t pos = 0;
    bool used_files = false;

    for (const char *p = app->raw_exec; *p && pos + 1 < out_sz; ++p) {
        if (*p == '%' && p[1]) {
            char c = *++p;
            if (c == '%') {
                out[pos++] = '%';
                out[pos] = '\0';
            } else if (c == 'f' || c == 'u') {
                if (file_count > 0 && files[0]) {
                    out[pos] = '\0';
                    ow_append_quoted(out, out_sz, files[0]);
                    pos = strlen(out);
                    used_files = true;
                }
            } else if (c == 'F' || c == 'U') {
                for (int i = 0; i < file_count; ++i) {
                    if (!files[i]) continue;
                    if (pos > 0 && out[pos - 1] != ' ' && pos + 1 < out_sz) {
                        out[pos++] = ' ';
                    }
                    out[pos] = '\0';
                    ow_append_quoted(out, out_sz, files[i]);
                    pos = strlen(out);
                }
                used_files = true;
            } else if (c == 'c') {
                out[pos] = '\0';
                ow_append_quoted(out, out_sz, app->name);
                pos = strlen(out);
            }
        } else {
            out[pos++] = *p;
            out[pos] = '\0';
        }
    }

    if (!used_files && file_count > 0) {
        for (int i = 0; i < file_count; ++i) {
            if (!files[i]) continue;
            if (pos + 2 < out_sz) {
                out[pos++] = ' ';
                out[pos] = '\0';
                ow_append_quoted(out, out_sz, files[i]);
                pos = strlen(out);
            }
        }
    }
    ow_trim(out);
}

static void scan_desktop_dir(
    OpenWithDialogCtx *ctx,
    const char *dir_path,
    const char *rel_prefix,
    const char *cur_default_id,
    int depth
) {
    if (!ctx || !dir_path || depth > 3 || ctx->app_count >= OW_MAX_APPS) return;
    char full_dir[PATH_MAX];
    if (rel_prefix && rel_prefix[0]) snprintf(full_dir, sizeof(full_dir), "%s/%s", dir_path, rel_prefix);
    else ow_copy(full_dir, sizeof(full_dir), dir_path);

    DIR *d = opendir(full_dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != nullptr && ctx->app_count < OW_MAX_APPS) {
        if (de->d_name[0] == '.') continue;
        char fpath[PATH_MAX];
        snprintf(fpath, sizeof(fpath), "%s/%s", full_dir, de->d_name);
        struct stat st;
        if (stat(fpath, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            char sub[256];
            if (rel_prefix && rel_prefix[0]) snprintf(sub, sizeof(sub), "%s/%s", rel_prefix, de->d_name);
            else ow_copy(sub, sizeof(sub), de->d_name);
            scan_desktop_dir(ctx, dir_path, sub, cur_default_id, depth + 1);
            continue;
        }
        size_t nlen = strlen(de->d_name);
        if (nlen <= 8 || strcmp(de->d_name + nlen - 8, ".desktop") != 0) continue;

        char did[128];
        if (rel_prefix && rel_prefix[0]) {
            char flat[96];
            ow_copy(flat, sizeof(flat), rel_prefix);
            for (char *c = flat; *c; ++c) if (*c == '/') *c = '-';
            snprintf(did, sizeof(did), "%s-%s", flat, de->d_name);
        } else {
            ow_copy(did, sizeof(did), de->d_name);
        }

        bool exists = false;
        for (int i = 0; i < ctx->app_count; ++i) {
            if (strcmp(ctx->apps[i].desktop_id, did) == 0) { exists = true; break; }
        }
        if (exists) continue;

        FILE *fp = fopen(fpath, "r");
        if (!fp) continue;
        bool in_ent = false, hidden = false, nodisp = false, term = false;
        char type_v[32] = "", name_v[128] = "", comm_v[160] = "", exec_v[512] = "", mimes_v[1024] = "", wdir[256] = "";
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            ow_trim(line);
            if (line[0] == '[') {
                in_ent = (strcmp(line, "[Desktop Entry]") == 0);
                continue;
            }
            if (!in_ent || line[0] == '#' || line[0] == '\0') continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = line, *v = eq + 1;
            ow_trim(k); ow_trim(v);
            if (strcmp(k, "Type") == 0) ow_copy(type_v, sizeof(type_v), v);
            else if (strcmp(k, "Hidden") == 0) hidden = (strcasecmp(v, "true") == 0);
            else if (strcmp(k, "NoDisplay") == 0) nodisp = (strcasecmp(v, "true") == 0);
            else if (strcmp(k, "Terminal") == 0) term = (strcasecmp(v, "true") == 0);
            else if (strcmp(k, "Name") == 0 && !name_v[0]) ow_copy(name_v, sizeof(name_v), v);
            else if (strcmp(k, "Comment") == 0 && !comm_v[0]) ow_copy(comm_v, sizeof(comm_v), v);
            else if (strcmp(k, "GenericName") == 0 && !comm_v[0]) ow_copy(comm_v, sizeof(comm_v), v);
            else if (strcmp(k, "Exec") == 0) ow_copy(exec_v, sizeof(exec_v), v);
            else if (strcmp(k, "MimeType") == 0) ow_copy(mimes_v, sizeof(mimes_v), v);
            else if (strcmp(k, "Path") == 0) ow_copy(wdir, sizeof(wdir), v);
        }
        fclose(fp);

        if (hidden || !name_v[0] || !exec_v[0]) continue;
        if (type_v[0] && strcmp(type_v, "Application") != 0) continue;

        bool compat = mime_list_matches(mimes_v, ctx->mime_type);
        bool is_def = (cur_default_id && cur_default_id[0] && strcasecmp(did, cur_default_id) == 0);
        if (nodisp && !compat && !is_def) continue;

        OpenWithApp *a = &ctx->apps[ctx->app_count++];
        memset(a, 0, sizeof(*a));
        ow_copy(a->desktop_id, sizeof(a->desktop_id), did);
        ow_copy(a->name, sizeof(a->name), name_v);
        ow_copy(a->comment, sizeof(a->comment), comm_v);
        ow_copy(a->raw_exec, sizeof(a->raw_exec), exec_v);
        ow_copy(a->work_dir, sizeof(a->work_dir), wdir);
        a->terminal = term;
        a->mime_compatible = compat || is_def;
        a->is_current_default = is_def;
    }
    closedir(d);
}

static int cmp_ow_apps(const void *pa, const void *pb) {
    const OpenWithApp *a = (const OpenWithApp *)pa;
    const OpenWithApp *b = (const OpenWithApp *)pb;
    if (a->is_current_default != b->is_current_default) return a->is_current_default ? -1 : 1;
    if (a->mime_compatible != b->mime_compatible) return a->mime_compatible ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static void populate_ow_apps(OpenWithDialogCtx *ctx) {
    char cur_def[128] = "";
    read_current_default_desktop_id(ctx->mime_type, cur_def, sizeof(cur_def));

    const char *xdg_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char uapps[PATH_MAX] = "";
    if (xdg_home && xdg_home[0]) snprintf(uapps, sizeof(uapps), "%s/applications", xdg_home);
    else if (home && home[0]) snprintf(uapps, sizeof(uapps), "%s/.local/share/applications", home);
    if (uapps[0]) scan_desktop_dir(ctx, uapps, "", cur_def, 0);

    const char *xdg_dirs = getenv("XDG_DATA_DIRS");
    char dbuf[2048];
    ow_copy(dbuf, sizeof(dbuf), (xdg_dirs && xdg_dirs[0])
            ? xdg_dirs
            : "/usr/local/share:/usr/share:/var/lib/flatpak/exports/share:/var/lib/snapd/desktop");

    char *sp = nullptr;
    for (char *tok = strtok_r(dbuf, ":", &sp); tok; tok = strtok_r(nullptr, ":", &sp)) {
        if (!tok[0]) continue;
        char adir[PATH_MAX];
        snprintf(adir, sizeof(adir), "%s/applications", tok);
        scan_desktop_dir(ctx, adir, "", cur_def, 0);
    }

    /* Always provide built-in CDE Text Editor (dtpad) as a fallback option */
    if (ctx->app_count < OW_MAX_APPS) {
        OpenWithApp *a = &ctx->apps[ctx->app_count++];
        memset(a, 0, sizeof(*a));
        ow_copy(a->desktop_id, sizeof(a->desktop_id), "cde-dtpad.desktop");
        ow_copy(a->name, sizeof(a->name), "CDE Text Editor (dtpad)");
        ow_copy(a->comment, sizeof(a->comment), "Open file in CDE Motif Text Editor");
        ow_copy(a->raw_exec, sizeof(a->raw_exec), CDE_INSTALLATION_TOP "/bin/dtpad %F");
        a->mime_compatible = (strncmp(ctx->mime_type, "text/", 5) == 0);
    }

    if (ctx->app_count > 1) {
        qsort(ctx->apps, (size_t)ctx->app_count, sizeof(OpenWithApp), cmp_ow_apps);
    }
}

static void free_ow_ctx(OpenWithDialogCtx *ctx) {
    if (!ctx) return;
    if (ctx->file_paths) {
        for (int i = 0; i < ctx->file_count; ++i) {
            if (ctx->file_paths[i]) XtFree(ctx->file_paths[i]);
        }
        XtFree((char *)ctx->file_paths);
    }
    XtFree((char *)ctx);
}

static void ow_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    free_ow_ctx((OpenWithDialogCtx *)client_data);
}

static void ow_list_select_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    OpenWithDialogCtx *ctx = (OpenWithDialogCtx *)client_data;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    if (!ctx || !cbs || cbs->item_position < 1 || cbs->item_position > ctx->app_count) return;

    const OpenWithApp *app = &ctx->apps[cbs->item_position - 1];
    char cmd_preview[1024];
    expand_exec_preview(app, ctx->file_paths, ctx->file_count, cmd_preview, sizeof(cmd_preview));
    ctx->updating_cmd = true;
    XmTextFieldSetString(ctx->cmd_tf, cmd_preview);
    ctx->updating_cmd = false;
}

static void ow_cancel_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    OpenWithDialogCtx *ctx = (OpenWithDialogCtx *)client_data;
    if (ctx && ctx->shell) XtDestroyWidget(ctx->shell);
}

static void ow_execute_and_close(OpenWithDialogCtx *ctx) {
    if (!ctx) return;

    int *sel_pos = nullptr;
    int sel_cnt = 0;
    int chosen_idx = -1;
    if (XmListGetSelectedPos(ctx->list_w, &sel_pos, &sel_cnt) && sel_cnt > 0 && sel_pos) {
        chosen_idx = sel_pos[0] - 1;
        XtFree((char *)sel_pos);
    }

    Boolean set_def = XmToggleButtonGetState(ctx->def_toggle);
    if (set_def && chosen_idx >= 0 && chosen_idx < ctx->app_count &&
        ctx->apps[chosen_idx].desktop_id[0]) {
        DtfileSetMimeAppsDefault(ctx->mime_type, ctx->apps[chosen_idx].desktop_id);
    }

    char *cmd_str = XmTextFieldGetString(ctx->cmd_tf);
    bool is_term = (chosen_idx >= 0 && chosen_idx < ctx->app_count) ? ctx->apps[chosen_idx].terminal : false;
    const char *wdir = (chosen_idx >= 0 && chosen_idx < ctx->app_count) ? ctx->apps[chosen_idx].work_dir : "";
    const char *title = (chosen_idx >= 0 && chosen_idx < ctx->app_count) ? ctx->apps[chosen_idx].name : "Application";

    if (cmd_str && cmd_str[0]) {
        char *wl_tok = nullptr;
        const char *did = (chosen_idx >= 0 && chosen_idx < ctx->app_count)
                              ? ctx->apps[chosen_idx].desktop_id
                              : nullptr;
        Display *ow_dpy = ctx->shell ? XtDisplay(ctx->shell) : nullptr;
        Window ow_win = (ctx->shell && XtIsRealized(ctx->shell)) ? XtWindow(ctx->shell) : None;
        (void)XmWaylandRequestActivationToken(ow_dpy, ow_win, did, &wl_tok);

        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            pid_t pid2 = fork();
            if (pid2 == 0) {
                if (wl_tok && wl_tok[0]) {
                    setenv("XDG_ACTIVATION_TOKEN", wl_tok, 1);
                    setenv("DESKTOP_STARTUP_ID", wl_tok, 1);
                }
                if (wdir && wdir[0] && chdir(wdir) != 0) {}
                int dn = open("/dev/null", O_RDONLY);
                if (dn >= 0) {
                    dup2(dn, STDIN_FILENO);
                    if (dn > 2) close(dn);
                }
                if (is_term && access(CDE_INSTALLATION_TOP "/bin/dtterm", X_OK) == 0) {
                    execl(CDE_INSTALLATION_TOP "/bin/dtterm", "dtterm", "-title", title,
                          "-e", "/bin/sh", "-c", cmd_str, (char *)nullptr);
                }
                execl("/bin/sh", "sh", "-c", cmd_str, (char *)nullptr);
                _exit(127);
            }
            _exit(0);
        } else if (pid > 0) {
            int st = 0;
            (void)waitpid(pid, &st, 0);
        }
        if (wl_tok) XtFree(wl_tok);
    }
    if (cmd_str) XtFree(cmd_str);
    if (ctx->shell) XtDestroyWidget(ctx->shell);
}

static void ow_ok_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    ow_execute_and_close((OpenWithDialogCtx *)client_data);
}

static void show_open_with_for_paths(Widget parent_shell, char **paths, int count) {
    int valid = 0;
    for (int i = 0; i < count; ++i) {
        if (paths[i] && paths[i][0]) paths[valid++] = paths[i];
        else if (paths[i]) XtFree(paths[i]);
    }
    if (valid <= 0) {
        XtFree((char *)paths);
        return;
    }

    OpenWithDialogCtx *ctx = (OpenWithDialogCtx *)XtCalloc(1, sizeof(OpenWithDialogCtx));
    ctx->file_paths = paths;
    ctx->file_count = valid;
    DtfileDetectMimeType(paths[0], ctx->mime_type, sizeof(ctx->mime_type));
    populate_ow_apps(ctx);

    ctx->shell = XmCreateDialogShell(parent_shell, "openWithDialog", nullptr, 0);
    XtVaSetValues(
        ctx->shell,
        XmNtitle, "Open With",
        XmNdeleteResponse, XmDESTROY,
        XmNminWidth, 580,
        XmNminHeight, 390,
        NULL
    );
    XtAddCallback(ctx->shell, XmNdestroyCallback, ow_destroy_cb, (XtPointer)ctx);

    Widget form = XtVaCreateWidget(
        "openWithForm", xmFormWidgetClass, ctx->shell,
        XmNmarginWidth, 12,
        XmNmarginHeight, 12,
        XmNheight, 400,
        NULL
    );

    const char *base = strrchr(paths[0], '/');
    base = base ? (base + 1) : paths[0];
    char hdr_buf[384];
    if (valid == 1) {
        snprintf(hdr_buf, sizeof(hdr_buf), "Choose an application to open \"%s\"  (%s):",
                 base, ctx->mime_type);
    } else {
        snprintf(hdr_buf, sizeof(hdr_buf), "Choose an application to open %d selected files  (%s):",
                 valid, ctx->mime_type);
    }
    XmString xs_hdr = XmStringCreateLocalized(hdr_buf);
    Widget hdr_lbl = XtVaCreateManagedWidget(
        "owHeader", xmLabelWidgetClass, form,
        XmNlabelString, xs_hdr,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    XmStringFree(xs_hdr);

    Widget btn_rc = XtVaCreateManagedWidget(
        "owButtons", xmRowColumnWidgetClass, form,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_COLUMN,
        XmNspacing, 12,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );

    XmString xs_ok = XmStringCreateLocalized("OK");
    Widget ok_btn = XtVaCreateManagedWidget("owOk", xmPushButtonWidgetClass, btn_rc,
                                            XmNlabelString, xs_ok, NULL);
    XmStringFree(xs_ok);
    XtAddCallback(ok_btn, XmNactivateCallback, ow_ok_cb, (XtPointer)ctx);

    XmString xs_cancel = XmStringCreateLocalized("Cancel");
    Widget cancel_btn = XtVaCreateManagedWidget("owCancel", xmPushButtonWidgetClass, btn_rc,
                                                XmNlabelString, xs_cancel, NULL);
    XmStringFree(xs_cancel);
    XtAddCallback(cancel_btn, XmNactivateCallback, ow_cancel_cb, (XtPointer)ctx);

    Widget sep = XtVaCreateManagedWidget(
        "owSep", xmSeparatorWidgetClass, form,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, btn_rc,
        XmNbottomOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );

    char def_lbl_buf[256];
    snprintf(def_lbl_buf, sizeof(def_lbl_buf),
             "Set as default application for %s files",
             ctx->mime_type);
    XmString xs_def = XmStringCreateLocalized(def_lbl_buf);
    ctx->def_toggle = XtVaCreateManagedWidget(
        "owDefaultToggle", xmToggleButtonWidgetClass, form,
        XmNlabelString, xs_def,
        XmNset, False,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, sep,
        XmNbottomOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    XmStringFree(xs_def);

    ctx->cmd_tf = XtVaCreateManagedWidget(
        "owCommandField", xmTextFieldWidgetClass, form,
        XmNcolumns, 56,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, ctx->def_toggle,
        XmNbottomOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );

    XmString xs_clbl = XmStringCreateLocalized("Command:");
    Widget cmd_lbl = XtVaCreateManagedWidget(
        "owCommandLabel", xmLabelWidgetClass, form,
        XmNlabelString, xs_clbl,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, ctx->cmd_tf,
        XmNbottomOffset, 2,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    XmStringFree(xs_clbl);

    Arg largs[8];
    int n = 0;
    XtSetArg(largs[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(largs[n], XmNvisibleItemCount, 9); n++;
    XtSetArg(largs[n], XmNlistSizePolicy, XmCONSTANT); n++;
    ctx->list_w = XmCreateScrolledList(form, "owAppList", largs, n);
    XtVaSetValues(
        XtParent(ctx->list_w),
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, hdr_lbl,
        XmNtopOffset, 8,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, cmd_lbl,
        XmNbottomOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    XtManageChild(ctx->list_w);
    XtAddCallback(ctx->list_w, XmNbrowseSelectionCallback, ow_list_select_cb, (XtPointer)ctx);
    XtAddCallback(ctx->list_w, XmNdefaultActionCallback, ow_ok_cb, (XtPointer)ctx);

    if (ctx->app_count > 0) {
        XmString *xitems = (XmString *)XtCalloc((Cardinal)ctx->app_count, sizeof(XmString));
        for (int i = 0; i < ctx->app_count; ++i) {
            const OpenWithApp *a = &ctx->apps[i];
            char row[256];
            const char *tag = a->is_current_default ? "[Default] " : (a->mime_compatible ? "[Recommended] " : "");
            if (a->comment[0]) {
                snprintf(row, sizeof(row), "%s%s  --  %.90s", tag, a->name, a->comment);
            } else {
                snprintf(row, sizeof(row), "%s%s", tag, a->name);
            }
            xitems[i] = XmStringCreateLocalized(row);
        }
        XmListAddItemsUnselected(ctx->list_w, xitems, ctx->app_count, 1);
        for (int i = 0; i < ctx->app_count; ++i) XmStringFree(xitems[i]);
        XtFree((char *)xitems);

        XmListSelectPos(ctx->list_w, 1, True);
    }

    XtVaSetValues(form, XmNdefaultButton, ok_btn, XmNcancelButton, cancel_btn, NULL);
    XtManageChild(form);
    XtPopup(ctx->shell, XtGrabNone);
}

void OpenWithMenuCB(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data; (void)call_data;
    Widget mbar = XmGetPostedFromWidget(XtParent(w));
    if (!mbar) return;

    FileMgrRec *file_mgr_rec = nullptr;
    Arg args[1];
    XtSetArg(args[0], XmNuserData, &file_mgr_rec);
    XtGetValues(mbar, args, 1);
    if (!file_mgr_rec) return;

    DialogData *dialog_data = _DtGetInstanceData((XtPointer)file_mgr_rec);
    if (!dialog_data) return;
    FileMgrData *file_mgr_data = (FileMgrData *)dialog_data->data;
    if (!file_mgr_data || file_mgr_data->selected_file_count <= 0) return;

    int count = file_mgr_data->selected_file_count;
    char **paths = (char **)XtCalloc((Cardinal)count, sizeof(char *));
    for (int i = 0; i < count; ++i) {
        paths[i] = ow_build_full_path(file_mgr_data->selection_list[i]);
    }
    show_open_with_for_paths(file_mgr_rec->shell, paths, count);
}

void OpenWithPopupCB(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    FileViewData *file_view_data = (FileViewData *)client_data;
    if (!file_view_data) return;

    DirectorySet *ds = (DirectorySet *)file_view_data->directory_set;
    if (!ds || !ds->file_mgr_data) return;
    FileMgrData *file_mgr_data = (FileMgrData *)ds->file_mgr_data;
    FileMgrRec *file_mgr_rec = (FileMgrRec *)file_mgr_data->file_mgr_rec;
    if (!file_mgr_rec) return;

    int count = 0;
    char **paths = nullptr;
    if (file_mgr_data->selected_file_count > 1) {
        count = file_mgr_data->selected_file_count;
        paths = (char **)XtCalloc((Cardinal)count, sizeof(char *));
        for (int i = 0; i < count; ++i) {
            paths[i] = ow_build_full_path(file_mgr_data->selection_list[i]);
        }
    } else {
        count = 1;
        paths = (char **)XtCalloc(1, sizeof(char *));
        paths[0] = ow_build_full_path(file_view_data);
    }
    show_open_with_for_paths(file_mgr_rec->shell, paths, count);
}
