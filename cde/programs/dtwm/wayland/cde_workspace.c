/*
 * CDE - Common Desktop Environment
 * cde_workspace.c: Multi-workspace model, per-workspace CDE XPM/XBM backdrops,
 * XWayland _DT_WORKSPACE_* / _MOTIF_WM_INFO libDtSvc bridge, native Wayland
 * ext-workspace-v1 protocol server, and docked interactive CDE Front Panel.
 */

#include "dtwm_wayland.h"
#include "ext-workspace-v1-protocol.h"

#include <ctype.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

/* --- CDE WSM Wire Constants (matching <Dt/WsmP.h>) --- */
#define DTWM_WS_HINTS_WSFLAGS    (1u << 0)
#define DTWM_WS_HINTS_WORKSPACES (1u << 1)
#define DTWM_WS_FLAG_OCCUPY_ALL  (1u << 0)

/* --- Native Wayland ext-workspace-v1 State --- */
typedef struct DtwmExtWsManager DtwmExtWsManager;

typedef struct {
    struct wl_list link; /* DtwmExtWsManager.handles */
    DtwmExtWsManager *mgr;
    struct wl_resource *resource;
    uint32_t ws_idx;
    bool pending_activate;
    bool pending_remove;
} DtwmExtWsHandle;

struct DtwmExtWsManager {
    struct wl_list link; /* DtwmServer.ext_ws_managers */
    DtwmServer *server;
    struct wl_resource *mgr_res;
    struct wl_resource *group_res;
    struct wl_list handles;
    char pending_create_name[64];
    bool has_pending_create;
};

static void xcb_sync_wm_properties(DtwmServer *server);
static void xcb_sync_view_presence(DtwmView *view);
static void ext_ws_broadcast_sync(DtwmServer *server);

/* --- Default Per-Workspace CDE Color Sets & Backdrops (Profile-Aware) --- */
static DtwmColorSet make_ws_colorset(DtwmAppearanceProfile profile, uint32_t idx) {
    if (profile == DTWM_PROFILE_MODERN_LIGHT) {
        static const DtwmColorSet ml[4] = {
            {.bg = {0.494, 0.557, 0.643, 1.0}, .fg = {0.96, 0.97, 0.99, 1.0}, .top_shadow = {0.72, 0.77, 0.84, 1.0}, .bottom_shadow = {0.28, 0.33, 0.40, 1.0}},
            {.bg = {0.322, 0.478, 0.580, 1.0}, .fg = {0.96, 0.98, 0.99, 1.0}, .top_shadow = {0.56, 0.70, 0.79, 1.0}, .bottom_shadow = {0.16, 0.26, 0.33, 1.0}},
            {.bg = {0.549, 0.588, 0.651, 1.0}, .fg = {0.10, 0.13, 0.17, 1.0}, .top_shadow = {0.77, 0.80, 0.85, 1.0}, .bottom_shadow = {0.31, 0.35, 0.41, 1.0}},
            {.bg = {0.486, 0.549, 0.635, 1.0}, .fg = {0.96, 0.97, 0.99, 1.0}, .top_shadow = {0.71, 0.76, 0.83, 1.0}, .bottom_shadow = {0.27, 0.32, 0.39, 1.0}},
        };
        return ml[idx % 4];
    }
    if (profile == DTWM_PROFILE_MODERN_DARK) {
        static const DtwmColorSet md[4] = {
            {.bg = {0.106, 0.133, 0.173, 1.0}, .fg = {0.92, 0.94, 0.97, 1.0}, .top_shadow = {0.23, 0.28, 0.35, 1.0}, .bottom_shadow = {0.05, 0.06, 0.09, 1.0}},
            {.bg = {0.157, 0.165, 0.176, 1.0}, .fg = {0.92, 0.93, 0.95, 1.0}, .top_shadow = {0.29, 0.30, 0.32, 1.0}, .bottom_shadow = {0.07, 0.08, 0.09, 1.0}},
            {.bg = {0.122, 0.161, 0.216, 1.0}, .fg = {0.92, 0.95, 0.98, 1.0}, .top_shadow = {0.25, 0.31, 0.40, 1.0}, .bottom_shadow = {0.06, 0.08, 0.11, 1.0}},
            {.bg = {0.145, 0.153, 0.169, 1.0}, .fg = {0.92, 0.93, 0.95, 1.0}, .top_shadow = {0.28, 0.29, 0.32, 1.0}, .bottom_shadow = {0.07, 0.07, 0.08, 1.0}},
        };
        return md[idx % 4];
    }
    if (profile == DTWM_PROFILE_HIGH_CONTRAST) {
        return (DtwmColorSet){
            .bg = {0.0, 0.0, 0.0, 1.0},
            .fg = {1.0, 1.0, 1.0, 1.0},
            .top_shadow = {1.0, 1.0, 1.0, 1.0},
            .bottom_shadow = {0.8, 0.8, 0.8, 1.0},
        };
    }
    static const DtwmColorSet sets[4] = {
        /* Workspace 0 ("One"): Default.dp ColorSet 2 (#89559808aa00) + KnitLight */
        {
            .bg = {0.538, 0.596, 0.667, 1.0},
            .fg = {1.000, 1.000, 1.000, 1.0},
            .top_shadow = {0.793, 0.819, 0.850, 1.0},
            .bottom_shadow = {0.281, 0.311, 0.348, 1.0},
        },
        /* Workspace 1 ("Two"): Default.dp ColorSet 7 (#938eab73bf00) + WaterDrops */
        {
            .bg = {0.576, 0.671, 0.749, 1.0},
            .fg = {1.000, 1.000, 1.000, 1.0},
            .top_shadow = {0.818, 0.859, 0.892, 1.0},
            .bottom_shadow = {0.315, 0.367, 0.410, 1.0},
        },
        /* Workspace 2 ("Three"): Default.dp ColorSet 5 (#49009200a700) + SkyDark */
        {
            .bg = {0.286, 0.573, 0.655, 1.0},
            .fg = {1.000, 1.000, 1.000, 1.0},
            .top_shadow = {0.669, 0.802, 0.840, 1.0},
            .bottom_shadow = {0.138, 0.277, 0.316, 1.0},
        },
        /* Workspace 3 ("Four"): Default.dp ColorSet 6 (#b70087008d00) + Paver */
        {
            .bg = {0.718, 0.529, 0.553, 1.0},
            .fg = {1.000, 1.000, 1.000, 1.0},
            .top_shadow = {0.874, 0.790, 0.800, 1.0},
            .bottom_shadow = {0.376, 0.277, 0.290, 1.0},
        },
    };
    return sets[idx % 4];
}

static uint32_t rgba_to_argb32(DtwmRgba c) {
    uint32_t a = (uint32_t)fmin(255.0, fmax(0.0, round(c.a * 255.0)));
    uint32_t r = (uint32_t)fmin(255.0, fmax(0.0, round(c.r * 255.0)));
    uint32_t g = (uint32_t)fmin(255.0, fmax(0.0, round(c.g * 255.0)));
    uint32_t b = (uint32_t)fmin(255.0, fmax(0.0, round(c.b * 255.0)));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t rgba_to_rgb24(DtwmRgba c) {
    return rgba_to_argb32(c) & 0x00FFFFFFu;
}

/* --- CDE Backdrop Resolver & XPM / XBM Parser --- */
static bool resolve_backdrop_file(const char *name, char *out_path, size_t out_sz) {
    if (!name || !name[0] || strcasecmp(name, "NoBackdrop") == 0) {
        return false;
    }
    if (strchr(name, '/')) {
        if (access(name, R_OK) == 0) {
            snprintf(out_path, out_sz, "%s", name);
            return true;
        }
        snprintf(out_path, out_sz, "%s.pm", name);
        if (access(out_path, R_OK) == 0) return true;
        snprintf(out_path, out_sz, "%s.bm", name);
        if (access(out_path, R_OK) == 0) return true;
        return false;
    }

    const char *env_dir = getenv("CDE_BACKDROP_DIR");
    const char *dirs[4] = {
        env_dir,
#ifdef CDE_INSTALLATION_TOP
        CDE_INSTALLATION_TOP "/backdrops",
#else
        "/usr/dt/backdrops",
#endif
        "/usr/dt/backdrops",
        "cde/programs/backdrops",
    };
    const char *exts[3] = {"", ".pm", ".bm"};
    for (size_t d = 0; d < 4; ++d) {
        if (!dirs[d] || !dirs[d][0]) continue;
        for (size_t e = 0; e < 3; ++e) {
            snprintf(out_path, out_sz, "%s/%s%s", dirs[d], name, exts[e]);
            if (access(out_path, R_OK) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool parse_hex_color(const char *s, DtwmRgba *out) {
    if (!s || s[0] != '#') return false;
    size_t len = strlen(s + 1);
    unsigned int r = 0, g = 0, b = 0;
    if (len == 6 && sscanf(s + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        *out = (DtwmRgba){r / 255.0, g / 255.0, b / 255.0, 1.0};
        return true;
    }
    if (len == 12 && sscanf(s + 1, "%04x%04x%04x", &r, &g, &b) == 3) {
        *out = (DtwmRgba){r / 65535.0, g / 65535.0, b / 65535.0, 1.0};
        return true;
    }
    if (len == 3 && sscanf(s + 1, "%01x%01x%01x", &r, &g, &b) == 3) {
        *out = (DtwmRgba){r / 15.0, g / 15.0, b / 15.0, 1.0};
        return true;
    }
    return false;
}

static DtwmRgba resolve_xpm_color_spec(const char *spec, const DtwmColorSet *cs) {
    /* Prefer CDE symbolic color ('s ...') when present so workspace palettes tint backdrops! */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);

    char *save = nullptr;
    char *tok = strtok_r(buf, " \t", &save);
    const char *sym_val = nullptr;
    const char *col_val = nullptr;
    while (tok) {
        if (strcmp(tok, "s") == 0) {
            sym_val = strtok_r(nullptr, " \t", &save);
        } else if (strcmp(tok, "c") == 0) {
            col_val = strtok_r(nullptr, " \t", &save);
        } else if (strcmp(tok, "m") == 0 || strcmp(tok, "g") == 0 || strcmp(tok, "g4") == 0) {
            (void)strtok_r(nullptr, " \t", &save);
        }
        tok = strtok_r(nullptr, " \t", &save);
    }

    if (sym_val) {
        if (strcasecmp(sym_val, "background") == 0) return cs->bg;
        if (strcasecmp(sym_val, "foreground") == 0) return cs->fg;
        if (strcasecmp(sym_val, "topShadowColor") == 0) return cs->top_shadow;
        if (strcasecmp(sym_val, "bottomShadowColor") == 0) return cs->bottom_shadow;
        if (strcasecmp(sym_val, "selectColor") == 0) {
            return (DtwmRgba){cs->bg.r * 0.85, cs->bg.g * 0.85, cs->bg.b * 0.85, 1.0};
        }
    }

    if (col_val) {
        DtwmRgba parsed;
        if (parse_hex_color(col_val, &parsed)) {
            /* Tint neutral XPM grays gently toward the workspace palette hue */
            double lum = 0.299 * parsed.r + 0.587 * parsed.g + 0.114 * parsed.b;
            if (fabs(parsed.r - parsed.g) < 0.03 && fabs(parsed.g - parsed.b) < 0.03) {
                return (DtwmRgba){
                    fmin(1.0, cs->bg.r * (0.45 + lum * 0.9)),
                    fmin(1.0, cs->bg.g * (0.45 + lum * 0.9)),
                    fmin(1.0, cs->bg.b * (0.45 + lum * 0.9)),
                    1.0,
                };
            }
            return parsed;
        }
        if (strcasecmp(col_val, "white") == 0) return (DtwmRgba){1.0, 1.0, 1.0, 1.0};
        if (strcasecmp(col_val, "black") == 0) return (DtwmRgba){0.0, 0.0, 0.0, 1.0};
        if (strcasecmp(col_val, "none") == 0) return cs->bg;
    }

    return cs->bg;
}

static cairo_surface_t *load_xpm_tile(FILE *fp, const DtwmColorSet *cs) {
    int width = 0, height = 0, ncolors = 0, cpp = 0;
    bool got_header = false;

    typedef struct {
        char key[8];
        uint32_t argb;
    } XpmEntry;
    XpmEntry *cmap = nullptr;
    int cmap_read = 0;

    cairo_surface_t *surf = nullptr;
    uint32_t *pixels = nullptr;
    int stride_u32 = 0;
    int row = 0;

    char line[4096];
    bool in_comment = false;
    while (fgets(line, sizeof(line), fp)) {
        /* Track multi-line C comments outside quoted strings */
        char *p = line;
        if (in_comment) {
            char *endc = strstr(p, "*/");
            if (!endc) continue;
            in_comment = false;
            p = endc + 2;
        }
        while (*p && *p != '"') {
            if (p[0] == '/' && p[1] == '*') {
                char *endc = strstr(p + 2, "*/");
                if (!endc) {
                    in_comment = true;
                    break;
                }
                p = endc + 2;
                continue;
            }
            p++;
        }
        if (in_comment || *p != '"') continue;

        char *start = p + 1;
        char *end = strrchr(start, '"');
        if (!end) continue;
        *end = '\0';

        if (!got_header) {
            if (sscanf(start, "%d %d %d %d", &width, &height, &ncolors, &cpp) == 4 &&
                width > 0 && width <= 4096 && height > 0 && height <= 4096 &&
                ncolors > 0 && ncolors <= 1024 && cpp >= 1 && cpp <= 4) {
                got_header = true;
                cmap = calloc((size_t)ncolors, sizeof(*cmap));
                surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
                if (!cmap || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
                    free(cmap);
                    if (surf) cairo_surface_destroy(surf);
                    return nullptr;
                }
                pixels = (uint32_t *)cairo_image_surface_get_data(surf);
                stride_u32 = cairo_image_surface_get_stride(surf) / 4;
            }
            continue;
        }

        if (cmap_read < ncolors) {
            if ((int)strlen(start) >= cpp) {
                memcpy(cmap[cmap_read].key, start, (size_t)cpp);
                cmap[cmap_read].key[cpp] = '\0';
                DtwmRgba col = resolve_xpm_color_spec(start + cpp, cs);
                cmap[cmap_read].argb = rgba_to_argb32(col);
                cmap_read++;
            }
            continue;
        }

        if (row < height) {
            size_t slen = strlen(start);
            uint32_t fallback = rgba_to_argb32(cs->bg);
            for (int x = 0; x < width; ++x) {
                uint32_t pix = fallback;
                if ((size_t)((x + 1) * cpp) <= slen) {
                    const char *k = start + x * cpp;
                    for (int ci = 0; ci < ncolors; ++ci) {
                        if (memcmp(cmap[ci].key, k, (size_t)cpp) == 0) {
                            pix = cmap[ci].argb;
                            break;
                        }
                    }
                }
                pixels[row * stride_u32 + x] = pix;
            }
            row++;
            if (row >= height) break;
        }
    }

    free(cmap);
    if (!surf || row == 0) {
        if (surf) cairo_surface_destroy(surf);
        return nullptr;
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

static cairo_surface_t *load_xbm_tile(FILE *fp, const DtwmColorSet *cs) {
    int width = 0, height = 0;
    char line[1024];
    bool in_bits = false;

    cairo_surface_t *surf = nullptr;
    uint32_t *pixels = nullptr;
    int stride_u32 = 0;
    int byte_idx = 0;
    int bytes_per_row = 0;
    uint32_t fg = rgba_to_argb32(cs->top_shadow);
    uint32_t bg = rgba_to_argb32(cs->bg);

    while (fgets(line, sizeof(line), fp)) {
        if (!in_bits) {
            char name[128];
            int val = 0;
            if (sscanf(line, "#define %127s %d", name, &val) == 2) {
                if (strstr(name, "_width")) width = val;
                else if (strstr(name, "_height")) height = val;
            }
            if (strchr(line, '{') && width > 0 && width <= 2048 && height > 0 && height <= 2048) {
                in_bits = true;
                surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
                if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
                    cairo_surface_destroy(surf);
                    return nullptr;
                }
                pixels = (uint32_t *)cairo_image_surface_get_data(surf);
                stride_u32 = cairo_image_surface_get_stride(surf) / 4;
                bytes_per_row = (width + 7) / 8;
                char *brace = strchr(line, '{');
                memmove(line, brace + 1, strlen(brace + 1) + 1);
            } else {
                continue;
            }
        }
        char *p = line;
        while (*p) {
            while (*p && !isxdigit((unsigned char)*p) && *p != 'x' && *p != 'X') p++;
            if (!*p) break;
            char *endp = nullptr;
            unsigned long bval = strtoul(p, &endp, 0);
            if (endp == p) {
                p++;
                continue;
            }
            p = endp;
            int ry = byte_idx / bytes_per_row;
            int rx_base = (byte_idx % bytes_per_row) * 8;
            if (ry < height) {
                for (int bit = 0; bit < 8 && (rx_base + bit) < width; ++bit) {
                    pixels[ry * stride_u32 + rx_base + bit] = ((bval >> bit) & 1u) ? fg : bg;
                }
            }
            byte_idx++;
        }
    }
    if (!surf || byte_idx == 0) {
        if (surf) cairo_surface_destroy(surf);
        return nullptr;
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

static cairo_surface_t *load_backdrop_tile(const char *backdrop_name, const DtwmColorSet *cs) {
    char path[1024];
    if (!resolve_backdrop_file(backdrop_name, path, sizeof(path))) {
        return nullptr;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) return nullptr;

    char first[128] = {0};
    if (!fgets(first, sizeof(first), fp)) {
        fclose(fp);
        return nullptr;
    }
    rewind(fp);
    bool is_xpm = (strstr(first, "XPM") != nullptr) ||
                  (strlen(path) >= 3 && strcasecmp(path + strlen(path) - 3, ".pm") == 0);
    cairo_surface_t *tile = is_xpm ? load_xpm_tile(fp, cs) : load_xbm_tile(fp, cs);
    fclose(fp);
    return tile;
}

/* --- Backdrop Scene Rendering --- */
void dtwm_update_output_backdrop(DtwmOutput *output) {
    if (!output || !output->server) return;
    DtwmServer *server = output->server;
    DtwmWorkspace *ws = &server->workspaces[server->current_workspace];

    struct wlr_box obox;
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &obox);
    if (obox.width <= 0 || obox.height <= 0) return;

    if (output->bg_rect) {
        float bg[4] = {(float)ws->colors.bg.r, (float)ws->colors.bg.g, (float)ws->colors.bg.b, 1.0f};
        wlr_scene_rect_set_color(output->bg_rect, bg);
        wlr_scene_rect_set_size(output->bg_rect, obox.width, obox.height);
        wlr_scene_node_set_position(&output->bg_rect->node, obox.x, obox.y);
    }

    if (!output->backdrop_buf) {
        output->backdrop_buf = wlr_scene_buffer_create(server->bg_tree, nullptr);
    }
    wlr_scene_node_set_position(&output->backdrop_buf->node, obox.x, obox.y);

    cairo_surface_t *tile = load_backdrop_tile(ws->backdrop_name, &ws->colors);
    if (!tile) {
        wlr_scene_buffer_set_buffer(output->backdrop_buf, nullptr);
        wlr_scene_node_set_enabled(&output->backdrop_buf->node, false);
        return;
    }

    cairo_t *cr = nullptr;
    cairo_surface_t *surf = nullptr;
    struct wlr_buffer *buf = dtwm_cairo_buffer_begin(obox.width, obox.height, &cr, &surf);
    if (!buf) {
        cairo_surface_destroy(tile);
        return;
    }

    cairo_set_source_rgba(cr, ws->colors.bg.r, ws->colors.bg.g, ws->colors.bg.b, 1.0);
    cairo_paint(cr);

    int tw = cairo_image_surface_get_width(tile);
    int th = cairo_image_surface_get_height(tile);
    int tile_scale = (output->dpi >= 144) ? (int)lround((double)output->dpi / 96.0) : 1;
    if (tile_scale < 1) tile_scale = 1;
    if (ws->image_type == 1 && tw > 0 && th > 0) {
        /* Centered */
        double dw = (double)tw * tile_scale;
        double dh = (double)th * tile_scale;
        cairo_save(cr);
        cairo_translate(cr, floor((obox.width - dw) * 0.5), floor((obox.height - dh) * 0.5));
        cairo_scale(cr, (double)tile_scale, (double)tile_scale);
        cairo_set_source_surface(cr, tile, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_restore(cr);
    } else if (ws->image_type >= 2 && tw > 0 && th > 0) {
        /* Scaled to output */
        cairo_save(cr);
        cairo_scale(cr, (double)obox.width / tw, (double)obox.height / th);
        cairo_set_source_surface(cr, tile, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        /* Tiled (default CDE backdrop mode, crisp integer pixel-art scaling on Retina outputs) */
        cairo_pattern_t *pat = cairo_pattern_create_for_surface(tile);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        if (tile_scale > 1) {
            cairo_matrix_t mat;
            cairo_matrix_init_scale(&mat, 1.0 / (double)tile_scale, 1.0 / (double)tile_scale);
            cairo_pattern_set_matrix(pat, &mat);
            cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);
        }
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
    }

    cairo_surface_destroy(tile);
    dtwm_cairo_buffer_finish(cr, surf);

    wlr_scene_buffer_set_buffer(output->backdrop_buf, buf);
    wlr_scene_buffer_set_dest_size(output->backdrop_buf, obox.width, obox.height);
    wlr_scene_node_set_enabled(&output->backdrop_buf->node, true);
    wlr_buffer_drop(buf);
}

void dtwm_update_all_backdrops(DtwmServer *server) {
    DtwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        dtwm_update_output_backdrop(out);
    }
}

static struct wlr_output *get_panel_output(DtwmServer *server) {
    struct wlr_output *out = wlr_output_layout_output_at(server->output_layout, 1, 1);
    if (!out && !wl_list_empty(&server->outputs)) {
        DtwmOutput *first = wl_container_of(server->outputs.next, first, link);
        out = first->wlr_output;
    }
    return out;
}

/* --- Usable Output Box & Docked Genuine CDE Front Panel (dtwm-panel) --- */
void dtwm_get_usable_output_box(DtwmServer *server, double ref_x, double ref_y, struct wlr_box *out_box) {
    struct wlr_output *out = wlr_output_layout_output_at(server->output_layout, ref_x, ref_y);
    if (!out) {
        out = get_panel_output(server);
    }
    if (out) {
        wlr_output_layout_get_box(server->output_layout, out, out_box);
    } else {
        *out_box = (struct wlr_box){.x = 0, .y = 0, .width = 1280, .height = 900};
    }
    int dpi = dtwm_output_dpi_at(
        server, out_box->x + out_box->width * 0.5, out_box->y + out_box->height * 0.5
    );
    double s = (double)(dpi > 0 ? dpi : 96) / 96.0;
    int min_clear = (int)lround(160.0 * s);
    int strut = (server->panel_view && server->panel_view->mapped && !server->panel_view->minimized &&
                 server->panel_h > 0)
                    ? (server->panel_h + (int)lround(2.0 * s))
                    : (server->show_front_panel ? (int)lround((double)DTWM_PANEL_STRUT * s) : 0);
    if (server->show_front_panel && strut > 0 && out == get_panel_output(server) &&
        out_box->height > strut + min_clear) {
        out_box->height -= strut;
    }
}

void dtwm_update_front_panel(DtwmServer *server) {
    if (!server) return;
    DtwmView *pv = server->panel_view;
    if (!pv || !pv->mapped) {
        return;
    }
    if (!server->show_front_panel) {
        if (pv->frame_tree) {
            wlr_scene_node_set_enabled(&pv->frame_tree->node, false);
        }
        return;
    }

    int outer_w = 0, outer_h = 0;
    dtwm_view_outer_size(pv, &outer_w, &outer_h);
    int l, t, r, b;
    dtwm_view_insets(pv, &l, &t, &r, &b);

    struct wlr_output *panel_out = get_panel_output(server);
    if (panel_out) {
        struct wlr_box obox;
        wlr_output_layout_get_box(server->output_layout, panel_out, &obox);
        if (obox.width > 0 && obox.height > 0) {
            if (!server->panel_user_moved) {
                pv->x = obox.x + (obox.width - outer_w) / 2;
                pv->y = obox.y + obox.height - outer_h;
            } else {
                if (pv->x + outer_w > obox.x + obox.width) pv->x = obox.x + obox.width - outer_w;
                if (pv->x < obox.x) pv->x = obox.x;
                if (pv->y + outer_h > obox.y + obox.height) pv->y = obox.y + obox.height - outer_h;
                if (pv->y < obox.y) pv->y = obox.y;
            }
        }
    }

    server->panel_x = pv->x;
    server->panel_y = pv->y;
    server->panel_w = outer_w;
    server->panel_h = outer_h;

    if (pv->frame_tree) {
        wlr_scene_node_set_position(&pv->frame_tree->node, pv->x, pv->y);
        wlr_scene_node_set_enabled(&pv->frame_tree->node, !pv->minimized);
    }
    if (pv->xwayland_surface) {
        wlr_xwayland_surface_configure(
            pv->xwayland_surface,
            (int16_t)(pv->x + l),
            (int16_t)(pv->y + t),
            (uint16_t)pv->client_w,
            (uint16_t)pv->client_h
        );
    }
    dtwm_view_update_frame(pv);
    dtwm_icons_reposition_swallowed(server);
}

static const char *profile_cli_name(DtwmAppearanceProfile prof) {
    switch (prof) {
    case DTWM_PROFILE_MODERN_LIGHT:  return "modern_light";
    case DTWM_PROFILE_MODERN_DARK:   return "modern_dark";
    case DTWM_PROFILE_HIGH_CONTRAST: return "high_contrast";
    case DTWM_PROFILE_CLASSIC:
    default:                         return "classic";
    }
}

static void spawn_genuine_front_panel(DtwmServer *server) {
    if (!server || !server->show_front_panel || server->panel_pid > 0) return;
    if (!server->no_xwayland && (!server->xwayland || !server->xwayland->display_name)) return;
    if (server->no_xwayland && !server->wayland_socket) return;

    char cand_libs[1152] = {0};
    char cand1[1152] = {0};
    const char *panel_bin = nullptr;
    struct stat st;
    if (server->exe_dir[0] != '\0') {
        snprintf(cand_libs, sizeof(cand_libs), "%s/.libs/dtwm-panel", server->exe_dir);
        if (stat(cand_libs, &st) == 0 && (st.st_mode & S_IXUSR)) {
            panel_bin = cand_libs;
        } else {
            snprintf(cand1, sizeof(cand1), "%s/dtwm-panel", server->exe_dir);
            if (stat(cand1, &st) == 0 && (st.st_mode & S_IXUSR)) {
                panel_bin = cand1;
            }
        }
    }
    if (!panel_bin && stat(CDE_INSTALLATION_TOP "/bin/dtwm-panel", &st) == 0 &&
        (st.st_mode & S_IXUSR)) {
        panel_bin = CDE_INSTALLATION_TOP "/bin/dtwm-panel";
    }
    if (!panel_bin) {
        /* Fallback: write status file immediately if dtwm-panel binary is absent */
        dtwm_notify_panel_mapped(server);
        return;
    }

    const char *prof_str = profile_cli_name(server->palette.profile);
    pid_t pid = fork();
    if (pid == 0) {
        /* Unmask signals blocked by wl_event_loop_add_signal in parent */
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
        if (server->xwayland && server->xwayland->display_name) {
            setenv("DISPLAY", server->xwayland->display_name, 1);
        } else {
            unsetenv("DISPLAY");
            setenv("CK_MOTIF_BACKEND", "wayland", 1);
        }
        if (server->wayland_socket) {
            setenv("WAYLAND_DISPLAY", server->wayland_socket, 1);
        }
        char *const args[] = {
            (char *)panel_bin,
            (char *)"--appearance-profile",
            (char *)prof_str,
            nullptr,
        };
        execv(panel_bin, args);
        _exit(127);
    }
    if (pid > 0) {
        server->panel_pid = pid;
    }
}

static pid_t wait_front_panel(pid_t pid, int options) {
    pid_t result;
    do {
        result = waitpid(pid, nullptr, options);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void stop_genuine_front_panel(DtwmServer *server) {
    pid_t pid = server->panel_pid;
    if (pid <= 0) return;
    server->panel_pid = 0;

    /* Check that the recorded process is still our child before signaling it. */
    if (wait_front_panel(pid, WNOHANG) != 0) return;
    kill(pid, SIGTERM);
    const struct timespec interval = { .tv_nsec = 10000000 };
    for (int i = 0; i < 200; i++) {
        nanosleep(&interval, nullptr);
        if (wait_front_panel(pid, WNOHANG) != 0) return;
    }
    /* A panel blocked in a display or ToolTalk call cannot handle SIGTERM. */
    fprintf(stderr, "dtwm-wayland: Front Panel did not stop after SIGTERM\n");
    kill(pid, SIGKILL);
    (void)wait_front_panel(pid, 0);
}

void dtwm_spawn_front_panel(DtwmServer *server) {
    spawn_genuine_front_panel(server);
}

/* --- Workspace Occupancy & Switching --- */
bool dtwm_view_visible_on_current_ws(const DtwmView *view) {
    if (!view || !view->mapped || view->minimized) {
        return false;
    }
    if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface &&
        view->xwayland_surface->override_redirect) {
        return true;
    }
    return (view->workspace_mask & (1u << view->server->current_workspace)) != 0;
}

static void refresh_workspace_scene_visibility(DtwmServer *server) {
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (!v->frame_tree) continue;
        if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface &&
            v->xwayland_surface->override_redirect) {
            continue;
        }
        bool vis = dtwm_view_visible_on_current_ws(v);
        wlr_scene_node_set_enabled(&v->frame_tree->node, vis);
    }
}

void dtwm_view_set_workspace_mask(DtwmView *view, uint32_t mask) {
    if (!view || !view->server) return;
    DtwmServer *server = view->server;
    uint32_t valid = (1u << server->num_workspaces) - 1u;
    mask &= valid;
    if (mask == 0) {
        mask = 1u << server->current_workspace;
    }
    view->workspace_mask = mask;
    if (view->frame_tree) {
        wlr_scene_node_set_enabled(&view->frame_tree->node, dtwm_view_visible_on_current_ws(view));
    }
    xcb_sync_view_presence(view);
    dtwm_icons_relayout(server);
}

void dtwm_switch_workspace(DtwmServer *server, uint32_t ws_idx) {
    if (!server || ws_idx >= server->num_workspaces) {
        return;
    }
    if (server->current_workspace == ws_idx) {
        return;
    }
    dtwm_anim_cancel_all(server);
    server->current_workspace = ws_idx;
    server->palette.desktop_bg = server->workspaces[ws_idx].colors.bg;

    refresh_workspace_scene_visibility(server);

    /* Ensure focus moves to top-most visible window on the newly active workspace */
    if (server->focused_view && !dtwm_view_visible_on_current_ws(server->focused_view)) {
        DtwmView *prev = server->focused_view;
        server->focused_view = nullptr;
        prev->active = false;
        if (prev->kind == DTWM_VIEW_XDG && prev->xdg_toplevel && prev->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
        } else if (prev->kind == DTWM_VIEW_XWAYLAND && prev->xwayland_surface) {
            wlr_xwayland_surface_activate(prev->xwayland_surface, false);
        }
        dtwm_view_update_frame(prev);
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        dtwm_ime_set_focus(server, nullptr);
    }
    if (!server->focused_view) {
        DtwmView *cand;
        wl_list_for_each(cand, &server->views, link) {
            if (dtwm_view_visible_on_current_ws(cand) &&
                !cand->is_swallowed && !cand->is_desktop_pin &&
                !cand->is_panel && !cand->is_subpanel) {
                cand->active = true;
                server->focused_view = cand;
                struct wlr_surface *surf = nullptr;
                if (cand->kind == DTWM_VIEW_XDG && cand->xdg_toplevel) {
                    surf = cand->xdg_toplevel->base->surface;
                    if (cand->xdg_toplevel->base->initialized) {
                        wlr_xdg_toplevel_set_activated(cand->xdg_toplevel, true);
                    }
                } else if (cand->kind == DTWM_VIEW_XWAYLAND && cand->xwayland_surface) {
                    surf = cand->xwayland_surface->surface;
                    wlr_xwayland_surface_activate(cand->xwayland_surface, true);
                }
                dtwm_view_update_frame(cand);
                if (surf) {
                    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
                    if (kbd) {
                        wlr_seat_keyboard_notify_enter(
                            server->seat, surf, kbd->keycodes, kbd->num_keycodes, &kbd->modifiers
                        );
                    } else {
                        wlr_seat_keyboard_notify_enter(server->seat, surf, nullptr, 0, nullptr);
                    }
                    dtwm_ime_set_focus(server, surf);
                }
                break;
            }
        }
    }

    dtwm_update_all_backdrops(server);
    dtwm_update_front_panel(server);
    dtwm_icons_relayout(server);
    dtwm_icons_refresh_focus_indicators(server);
    dtwm_publish_active_window_box(server);
    xcb_sync_wm_properties(server);
    ext_ws_broadcast_sync(server);
    if (server->overview_visible) {
        dtwm_overview_refresh(server);
    }
}

/* --- XCB Helper Intern & Property Sync --- */
static xcb_atom_t intern_atom_sync(xcb_connection_t *conn, const char *name) {
    if (!conn || !name || !name[0]) return XCB_ATOM_NONE;
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, nullptr);
    if (!rep) return XCB_ATOM_NONE;
    xcb_atom_t a = rep->atom;
    free(rep);
    return a;
}

static void ensure_ws_xcb_atoms(DtwmServer *server, DtwmWorkspace *ws) {
    if (!server->xcb_conn) return;
    if (ws->x11_atom == XCB_ATOM_NONE) {
        ws->x11_atom = intern_atom_sync(server->xcb_conn, ws->id_name);
    }
    if (ws->info_atom == XCB_ATOM_NONE) {
        char info_name[96];
        snprintf(info_name, sizeof(info_name), "_DT_WORKSPACE_INFO_%s", ws->id_name);
        ws->info_atom = intern_atom_sync(server->xcb_conn, info_name);
    }
    const char *bname = ws->backdrop_name[0] ? ws->backdrop_name : "NoBackdrop";
    ws->backdrop_atom = intern_atom_sync(server->xcb_conn, bname);
}

static void xcb_write_ws_info(DtwmServer *server, DtwmWorkspace *ws) {
    if (!server->xcb_conn || server->xcb_wm_win == XCB_WINDOW_NONE) return;
    ensure_ws_xcb_atoms(server, ws);

    /* 7 NUL-separated strings expected by DtWsmGetWorkspaceInfo():
     * title \0 colorSetId \0 bgHex \0 fgHex \0 backdropAtom \0 imageType \0 numBackdropWindows \0
     */
    char buf[1024];
    int n1 = snprintf(buf, sizeof(buf), "%s", ws->title);
    int off = n1 + 1;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%d", ws->color_set_id) + 1;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "0x%x", rgba_to_rgb24(ws->colors.bg)) + 1;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "0x%x", rgba_to_rgb24(ws->colors.fg)) + 1;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%lu", (unsigned long)ws->backdrop_atom) + 1;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%d", ws->image_type) + 1;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "0") + 1;

    xcb_change_property(
        server->xcb_conn,
        XCB_PROP_MODE_REPLACE,
        server->xcb_wm_win,
        ws->info_atom,
        XCB_ATOM_STRING,
        8,
        (uint32_t)off,
        buf
    );
}

void dtwm_workspace_sync_ipc(DtwmServer *server) {
    if (!server || server->num_workspaces == 0) return;

    char list_buf[512];
    size_t loff = 0;
    for (uint32_t i = 0; i < server->num_workspaces; ++i) {
        DtwmWorkspace *ws = &server->workspaces[i];
        if (!ws->id_name[0]) continue;
        size_t idlen = strlen(ws->id_name);
        if (loff + idlen + 1 < sizeof(list_buf)) {
            memcpy(list_buf + loff, ws->id_name, idlen + 1);
            loff += idlen + 1;
        }

        const char *bname = ws->backdrop_name[0] ? ws->backdrop_name : "NoBackdrop";
        char info_buf[1024];
        size_t off = 0;
        off += (size_t)snprintf(info_buf + off, sizeof(info_buf) - off, "%s", ws->title) + 1;
        off += (size_t)snprintf(info_buf + off, sizeof(info_buf) - off, "%d", ws->color_set_id) + 1;
        off += (size_t)snprintf(info_buf + off, sizeof(info_buf) - off, "0x%x", rgba_to_rgb24(ws->colors.bg)) + 1;
        off += (size_t)snprintf(info_buf + off, sizeof(info_buf) - off, "0x%x", rgba_to_rgb24(ws->colors.fg)) + 1;
        off += (size_t)snprintf(info_buf + off, sizeof(info_buf) - off, "%s", bname) + 1;
        off += (size_t)snprintf(info_buf + off, sizeof(info_buf) - off, "%d", ws->image_type) + 1;
        off += (size_t)snprintf(info_buf + off, sizeof(info_buf) - off, "0") + 1;

        char prop_name[128];
        snprintf(prop_name, sizeof(prop_name), "_DT_WORKSPACE_INFO_RAW_%s", ws->id_name);
        dtwm_native_ipc_publish_prop(server, prop_name, info_buf, off);
    }

    if (loff > 0) {
        dtwm_native_ipc_publish_prop(server, "_DT_WORKSPACE_LIST_NAMES", list_buf, loff);
    }
    if (server->current_workspace < server->num_workspaces) {
        const char *cur_name = server->workspaces[server->current_workspace].id_name;
        if (cur_name[0]) {
            dtwm_native_ipc_publish_prop(
                server, "_DT_WORKSPACE_CURRENT_NAME", cur_name, strlen(cur_name) + 1
            );
        }
    }
}

static void xcb_sync_wm_properties(DtwmServer *server) {
    dtwm_workspace_sync_ipc(server);
    if (!server || !server->xcb_conn || server->xcb_wm_win == XCB_WINDOW_NONE) return;

    xcb_atom_t list[DTWM_MAX_WORKSPACES];
    for (uint32_t i = 0; i < server->num_workspaces; ++i) {
        ensure_ws_xcb_atoms(server, &server->workspaces[i]);
        list[i] = server->workspaces[i].x11_atom;
        xcb_write_ws_info(server, &server->workspaces[i]);
    }

    xcb_change_property(
        server->xcb_conn,
        XCB_PROP_MODE_REPLACE,
        server->xcb_wm_win,
        server->atom_ws_list,
        XCB_ATOM_ATOM,
        32,
        server->num_workspaces,
        list
    );

    xcb_atom_t cur = list[server->current_workspace];
    xcb_change_property(
        server->xcb_conn,
        XCB_PROP_MODE_REPLACE,
        server->xcb_wm_win,
        server->atom_ws_current,
        XCB_ATOM_ATOM,
        32,
        1,
        &cur
    );
    xcb_flush(server->xcb_conn);
}

static void xcb_sync_view_presence(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return;
    DtwmServer *server = view->server;
    if (!server->xcb_conn || server->atom_ws_presence == XCB_ATOM_NONE) return;

    xcb_atom_t occupied[DTWM_MAX_WORKSPACES];
    uint32_t n = 0;
    for (uint32_t i = 0; i < server->num_workspaces; ++i) {
        if (view->workspace_mask & (1u << i)) {
            ensure_ws_xcb_atoms(server, &server->workspaces[i]);
            occupied[n++] = server->workspaces[i].x11_atom;
        }
    }
    xcb_change_property(
        server->xcb_conn,
        XCB_PROP_MODE_REPLACE,
        view->xwayland_surface->window_id,
        server->atom_ws_presence,
        server->atom_ws_presence,
        32,
        n,
        occupied
    );
    xcb_flush(server->xcb_conn);
}

static void xcb_apply_client_ws_hints(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return;
    DtwmServer *server = view->server;
    if (!server->xcb_conn || server->atom_ws_hints == XCB_ATOM_NONE) return;

    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn,
        0,
        view->xwayland_surface->window_id,
        server->atom_ws_hints,
        server->atom_ws_hints,
        0,
        64
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return;

    int len = xcb_get_property_value_length(rep);
    if (rep->format == 32 && len >= 16) {
        const uint32_t *vals = (const uint32_t *)xcb_get_property_value(rep);
        uint32_t count_u32 = (uint32_t)(len / 4);
        uint32_t version = vals[0];
        uint32_t flags = vals[1];
        uint32_t wsflags = vals[2];
        uint32_t num_ws = vals[3];
        if (version == 1) {
            uint32_t new_mask = 0;
            if ((flags & DTWM_WS_HINTS_WSFLAGS) && (wsflags & DTWM_WS_FLAG_OCCUPY_ALL)) {
                new_mask = (1u << server->num_workspaces) - 1u;
            } else if (flags & DTWM_WS_HINTS_WORKSPACES) {
                for (uint32_t k = 0; k < num_ws && (4 + k) < count_u32; ++k) {
                    xcb_atom_t a = vals[4 + k];
                    if (a == server->atom_ws_all) {
                        new_mask = (1u << server->num_workspaces) - 1u;
                        break;
                    }
                    for (uint32_t i = 0; i < server->num_workspaces; ++i) {
                        if (server->workspaces[i].x11_atom == a) {
                            new_mask |= (1u << i);
                        }
                    }
                }
            }
            if (new_mask != 0) {
                dtwm_view_set_workspace_mask(view, new_mask);
            }
        }
    }
    free(rep);
}

int dtwm_add_workspace(DtwmServer *server, const char *title) {
    if (!server || server->num_workspaces >= DTWM_MAX_WORKSPACES) {
        return -1;
    }
    uint32_t idx = server->num_workspaces++;
    uint32_t serial = server->next_ws_serial++;
    DtwmWorkspace *ws = &server->workspaces[idx];
    memset(ws, 0, sizeof(*ws));
    snprintf(ws->id_name, sizeof(ws->id_name), "ws%u", serial);
    snprintf(ws->title, sizeof(ws->title), "%s", (title && title[0]) ? title : ws->id_name);
    snprintf(ws->backdrop_name, sizeof(ws->backdrop_name), "RicePaper");
    ws->color_set_id = (int)(idx % 8) + 1;
    ws->image_type = 0;
    ws->colors = make_ws_colorset(server->palette.profile, idx);

    ensure_ws_xcb_atoms(server, ws);
    /* Propagate occupy-all views onto the newly created workspace */
    uint32_t prev_all = (idx > 0) ? ((1u << idx) - 1u) : 0;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (prev_all != 0 && (v->workspace_mask & prev_all) == prev_all) {
            v->workspace_mask |= (1u << idx);
            xcb_sync_view_presence(v);
        }
    }

    dtwm_update_front_panel(server);
    xcb_sync_wm_properties(server);
    ext_ws_broadcast_sync(server);
    return (int)idx;
}

bool dtwm_delete_workspace(DtwmServer *server, uint32_t ws_idx) {
    if (!server || server->num_workspaces <= 1 || ws_idx >= server->num_workspaces) {
        return false;
    }
    for (uint32_t i = ws_idx; i + 1 < server->num_workspaces; ++i) {
        server->workspaces[i] = server->workspaces[i + 1];
    }
    server->num_workspaces--;

    /* Shift view workspace_mask bits above ws_idx */
    uint32_t low_mask = (1u << ws_idx) - 1u;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        uint32_t lo = v->workspace_mask & low_mask;
        uint32_t hi = (v->workspace_mask >> (ws_idx + 1)) << ws_idx;
        uint32_t merged = lo | hi;
        if (merged == 0) merged = 1u;
        v->workspace_mask = merged;
        xcb_sync_view_presence(v);
    }

    if (server->current_workspace >= server->num_workspaces) {
        server->current_workspace = server->num_workspaces - 1;
    }
    refresh_workspace_scene_visibility(server);
    dtwm_update_all_backdrops(server);
    dtwm_update_front_panel(server);
    xcb_sync_wm_properties(server);
    ext_ws_broadcast_sync(server);
    return true;
}

void dtwm_rename_workspace(DtwmServer *server, uint32_t ws_idx, const char *title) {
    if (!server || ws_idx >= server->num_workspaces || !title || !title[0]) return;
    snprintf(server->workspaces[ws_idx].title, sizeof(server->workspaces[ws_idx].title), "%s", title);
    dtwm_update_front_panel(server);
    xcb_sync_wm_properties(server);
    ext_ws_broadcast_sync(server);
}

void dtwm_set_workspace_backdrop(DtwmServer *server, uint32_t ws_idx, const char *path, int image_type) {
    if (!server || ws_idx >= server->num_workspaces || !path) return;
    DtwmWorkspace *ws = &server->workspaces[ws_idx];
    snprintf(ws->backdrop_name, sizeof(ws->backdrop_name), "%s", path);
    ws->image_type = image_type;
    ensure_ws_xcb_atoms(server, ws);
    xcb_sync_wm_properties(server);
    if (ws_idx == server->current_workspace) {
        dtwm_update_all_backdrops(server);
    }
}

static int find_ws_by_name_or_title(const DtwmServer *server, const char *tok) {
    if (!tok || !tok[0]) return -1;
    char *endp = nullptr;
    unsigned long atom_num = strtoul(tok, &endp, 0);
    bool is_atom_num = (endp && *endp == '\0' && atom_num > 0);
    for (uint32_t i = 0; i < server->num_workspaces; ++i) {
        if (is_atom_num && server->workspaces[i].x11_atom == (xcb_atom_t)atom_num) {
            return (int)i;
        }
        if (strcmp(server->workspaces[i].id_name, tok) == 0 ||
            strcasecmp(server->workspaces[i].title, tok) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void spawn_color_sync_for_profile(DtwmServer *server, DtwmAppearanceProfile profile);

static bool dtwm_server_apply_palette_file(DtwmServer *server, const char *raw_name);

static void handle_dt_wm_request_item(DtwmServer *server, const char *cmd) {
    while (*cmd && isspace((unsigned char)*cmd)) cmd++;
    if (!*cmd) return;
    if (dtwm_hw_handle_wm_request(server, cmd)) return;
    if (dtwm_output_mgmt_handle_wm_request(server, cmd)) return;
    if (dtwm_power_display_handle_wm_request(server, cmd)) return;
    if (dtwm_a11y_handle_wm_request(server, cmd)) return;
    if (dtwm_wayland_session_handle_wm_request(server, cmd)) return;
    if (dtwm_portal_handle_wm_request(server, cmd)) return;

    if (strncmp(cmd, "f.set_minimize_animation", 24) == 0 && isspace((unsigned char)cmd[24])) {
        server->minimize_anim_style = dtwm_parse_anim_style(cmd + 25);
        return;
    }
    if (strncmp(cmd, "f.set_maximize_animation", 24) == 0 && isspace((unsigned char)cmd[24])) {
        server->maximize_anim_style = dtwm_parse_anim_style(cmd + 25);
        return;
    }
    if (strncmp(cmd, "f.set_animation_speed", 21) == 0 && isspace((unsigned char)cmd[21])) {
        server->anim_speed = dtwm_parse_anim_speed(cmd + 22);
        return;
    }

    if (strncmp(cmd, "f.change_backdrop", 17) == 0 && isspace((unsigned char)cmd[17])) {
        char path[512] = {0};
        unsigned long pix = 0;
        int img_type = 0;
        int n = sscanf(cmd + 18, "%511s %lx %d", path, &pix, &img_type);
        if (n >= 1) {
            dtwm_set_workspace_backdrop(server, server->current_workspace, path, (n >= 3) ? img_type : 0);
        }
        return;
    }
    if (strncmp(cmd, "f.goto_workspace", 16) == 0 && isspace((unsigned char)cmd[16])) {
        char target[64] = {0};
        if (sscanf(cmd + 17, "%63s", target) == 1) {
            int idx = find_ws_by_name_or_title(server, target);
            if (idx >= 0) {
                dtwm_switch_workspace(server, (uint32_t)idx);
            }
        }
        return;
    }
    if (strncmp(cmd, "f.add_workspace", 15) == 0) {
        const char *arg = cmd + 15;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        dtwm_add_workspace(server, *arg ? arg : "New");
        return;
    }
    if (strncmp(cmd, "f.delete_workspace", 18) == 0 && isspace((unsigned char)cmd[18])) {
        char target[64] = {0};
        if (sscanf(cmd + 19, "%63s", target) == 1) {
            int idx = find_ws_by_name_or_title(server, target);
            if (idx >= 0) {
                dtwm_delete_workspace(server, (uint32_t)idx);
            }
        }
        return;
    }
    if (strncmp(cmd, "f.set_workspace_title", 21) == 0 && isspace((unsigned char)cmd[21])) {
        const char *p = cmd + 22;
        while (*p && isspace((unsigned char)*p)) p++;
        char ws_tok[64] = {0};
        size_t ti = 0;
        while (*p && !isspace((unsigned char)*p) && ti + 1 < sizeof(ws_tok)) {
            ws_tok[ti++] = *p++;
        }
        while (*p && isspace((unsigned char)*p)) p++;
        int idx = find_ws_by_name_or_title(server, ws_tok);
        if (idx >= 0 && *p) {
            dtwm_rename_workspace(server, (uint32_t)idx, p);
        }
        return;
    }
    if (strncmp(cmd, "f.pack_icons", 12) == 0) {
        dtwm_icons_pack(server);
        return;
    }
    if (strncmp(cmd, "f.set_open_window_icons", 23) == 0) {
        int val = 1;
        if (sscanf(cmd + 23, "%d", &val) == 1) {
            dtwm_icons_set_open_window_icons(server, val != 0);
        }
        return;
    }
    if (strncmp(cmd, "f.set_use_icon_box", 18) == 0) {
        int val = 0;
        if (sscanf(cmd + 18, "%d", &val) == 1) {
            dtwm_icons_set_use_icon_box(server, val != 0);
        }
        return;
    }
    if (strncmp(cmd, "f.set_large_icons", 17) == 0) {
        int val = 0;
        if (sscanf(cmd + 17, "%d", &val) == 1) {
            dtwm_icons_set_large_icons(server, val != 0);
        }
        return;
    }
    if (strncmp(cmd, "f.set_appearance_profile", 24) == 0) {
        const char *arg = cmd + 24;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        dtwm_server_apply_appearance_profile(server, dtwm_parse_appearance_profile(arg));
        return;
    }
    if (strncmp(cmd, "f.set_palette", 13) == 0) {
        const char *arg = cmd + 13;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        (void)dtwm_server_apply_palette_file(server, arg);
        return;
    }
    if (strncmp(cmd, "f.move_panel", 12) == 0) {
        dtwm_begin_move_panel(server);
        return;
    }
    if (strncmp(cmd, "f.quit_mwm", 10) == 0 &&
        (cmd[10] == '\0' || isspace((unsigned char)cmd[10]))) {
        wl_display_terminate(server->wl_display);
        return;
    }
    if (strncmp(cmd, "f.restart", 9) == 0) {
        stop_genuine_front_panel(server);
        server->panel_view = nullptr;
        spawn_genuine_front_panel(server);
        return;
    }
    if (strncmp(cmd, "f.set_output_dpi", 16) == 0) {
        unsigned int out_idx = 0;
        int dpi_val = 96;
        if (sscanf(cmd + 16, "%u %d", &out_idx, &dpi_val) == 2) {
            dtwm_set_output_dpi(server, (uint32_t)out_idx, dpi_val);
        }
        return;
    }
    if (strncmp(cmd, "f.recall_client", 15) == 0 && isspace((unsigned char)cmd[15])) {
        const char *arg = cmd + 16;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        char target[128] = {0};
        size_t ti = 0;
        while (*arg && !isspace((unsigned char)*arg) && ti + 1 < sizeof(target)) {
            target[ti++] = *arg++;
        }
        if (target[0]) {
            (void)dtwm_recall_client_by_name(server, target);
        }
        return;
    }
    if (strncmp(cmd, "f.move_active_to", 16) == 0) {
        int nx = 0, ny = 0;
        if (sscanf(cmd + 16, "%d %d", &nx, &ny) == 2 && server->focused_view) {
            DtwmView *v = server->focused_view;
            v->x = nx;
            v->y = ny;
            if (v->frame_tree) {
                wlr_scene_node_set_position(&v->frame_tree->node, v->x, v->y);
            }
            if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface) {
                int l, t, r, b;
                dtwm_view_insets(v, &l, &t, &r, &b);
                wlr_xwayland_surface_configure(
                    v->xwayland_surface,
                    (int16_t)(v->x + l),
                    (int16_t)(v->y + t),
                    (uint16_t)v->client_w,
                    (uint16_t)v->client_h
                );
            }
            dtwm_publish_active_window_box(server);
            dtwm_sync_xwayland_randr_dpi(server);
        }
        return;
    }
}

void dtwm_publish_active_window_box(DtwmServer *server) {
    if (!server) return;
    DtwmView *v = server->focused_view;
    if (!v || !v->mapped || v->is_panel || v->is_desktop_pin || v->is_swallowed) {
        dtwm_native_ipc_publish_prop(server, "_DT_ACTIVE_WINDOW_BOX", nullptr, 0);
        if (server->xcb_conn && server->xcb_root != XCB_WINDOW_NONE &&
            server->atom_active_win_box != XCB_ATOM_NONE) {
            xcb_delete_property(server->xcb_conn, server->xcb_root, server->atom_active_win_box);
            xcb_flush(server->xcb_conn);
        }
        return;
    }
    int l, t, r, b, outer_w, outer_h;
    dtwm_view_insets(v, &l, &t, &r, &b);
    dtwm_view_outer_size(v, &outer_w, &outer_h);
    uint32_t box[8] = {
        (uint32_t)v->x,
        (uint32_t)v->y,
        (uint32_t)(outer_w > 0 ? outer_w : 1),
        (uint32_t)(outer_h > 0 ? outer_h : 1),
        (uint32_t)(v->x + l),
        (uint32_t)(v->y + t),
        (uint32_t)(v->client_w > 0 ? v->client_w : 1),
        (uint32_t)(v->client_h > 0 ? v->client_h : 1),
    };
    dtwm_native_ipc_publish_prop(server, "_DT_ACTIVE_WINDOW_BOX", box, sizeof(box));
    if (server->xcb_conn && server->xcb_root != XCB_WINDOW_NONE &&
        server->atom_active_win_box != XCB_ATOM_NONE) {
        xcb_change_property(
            server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_root,
            server->atom_active_win_box, XCB_ATOM_CARDINAL, 32, 8, box
        );
        xcb_flush(server->xcb_conn);
    }
}

static bool view_eligible_for_recall(const DtwmView *v) {
    if (!v || !v->mapped) return false;
    if (v->is_panel || v->is_subpanel || v->is_desktop_pin || v->is_swallowed || v->is_lock_dialog) {
        return false;
    }
    if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface &&
        v->xwayland_surface->override_redirect) {
        return false;
    }
    return true;
}

static bool view_matches_recall_instance(const DtwmView *v, const char *name) {
    if (!view_eligible_for_recall(v) || !name || !name[0]) return false;
    if (v->kind == DTWM_VIEW_XDG && v->xdg_toplevel && v->xdg_toplevel->app_id &&
        v->xdg_toplevel->app_id[0]) {
        const char *aid = v->xdg_toplevel->app_id;
        if (strcasecmp(aid, name) == 0) return true;
        const char *dot = strrchr(aid, '.');
        if (dot && dot[1] && strcasecmp(dot + 1, name) == 0) return true;
    }
    if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface) {
        const struct wlr_xwayland_surface *xs = v->xwayland_surface;
        if (xs->instance && xs->instance[0] && strcasecmp(xs->instance, name) == 0) return true;
    }
    return false;
}

static bool view_matches_recall_class(const DtwmView *v, const char *name) {
    if (!view_eligible_for_recall(v) || !name || !name[0]) return false;
    if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface) {
        const struct wlr_xwayland_surface *xs = v->xwayland_surface;
        if (xs->class && xs->class[0] && strcasecmp(xs->class, name) == 0) return true;
    }
    return false;
}

static void append_client_id_token(char *buf, size_t cap, size_t *len, const char *tok) {
    if (!tok || !tok[0] || !len) return;
    size_t tlen = strlen(tok);
    if (*len + tlen + 1 > cap) return;
    memcpy(buf + *len, tok, tlen + 1);
    *len += tlen + 1;
}

void dtwm_publish_client_ids(DtwmServer *server) {
    if (!server) return;
    char buf[8192];
    buf[0] = '\0';
    size_t len = 0;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (!view_eligible_for_recall(v)) continue;
        if (v->kind == DTWM_VIEW_XDG && v->xdg_toplevel && v->xdg_toplevel->app_id &&
            v->xdg_toplevel->app_id[0]) {
            const char *aid = v->xdg_toplevel->app_id;
            append_client_id_token(buf, sizeof(buf), &len, aid);
            const char *dot = strrchr(aid, '.');
            if (dot && dot[1]) {
                append_client_id_token(buf, sizeof(buf), &len, dot + 1);
            }
        } else if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface) {
            if (v->xwayland_surface->instance && v->xwayland_surface->instance[0]) {
                append_client_id_token(buf, sizeof(buf), &len, v->xwayland_surface->instance);
            }
            if (v->xwayland_surface->class && v->xwayland_surface->class[0]) {
                append_client_id_token(buf, sizeof(buf), &len, v->xwayland_surface->class);
            }
        }
    }
    dtwm_native_ipc_publish_prop(server, "_DT_WAYLAND_CLIENT_IDS", buf, len);
    if (server->xcb_conn && server->xcb_root != XCB_WINDOW_NONE &&
        server->atom_client_ids != XCB_ATOM_NONE) {
        xcb_change_property(
            server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_root,
            server->atom_client_ids, XCB_ATOM_STRING, 8, (uint32_t)len, buf
        );
        xcb_flush(server->xcb_conn);
    }
}

bool dtwm_recall_client_by_name(DtwmServer *server, const char *name) {
    if (!server || !name || !name[0] || server->session_locked) return false;
    DtwmView *matched = nullptr;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (view_matches_recall_instance(v, name)) {
            matched = v;
            break;
        }
    }
    if (!matched) {
        wl_list_for_each(v, &server->views, link) {
            if (view_matches_recall_class(v, name)) {
                matched = v;
                break;
            }
        }
    }
    if (matched) {
        if ((matched->workspace_mask & (1u << server->current_workspace)) == 0) {
            dtwm_view_set_workspace_mask(matched, 1u << server->current_workspace);
        }
        dtwm_restore_view_focus(server, matched);
        return true;
    }
    return false;
}

static void notify_wayland_fractional_scale_for_outputs(DtwmServer *server) {
    dtwm_sync_all_external_client_scales(server);
}

void dtwm_sync_xwayland_randr_dpi(DtwmServer *server) {
    if (!server) return;
    notify_wayland_fractional_scale_for_outputs(server);
    if (!server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE ||
        server->atom_dpi == XCB_ATOM_NONE) {
        return;
    }

    /* Opt XWayland into CK Core Motif's per-output RandR DPI observer */
    if (server->atom_randr_dpi_opt_in != XCB_ATOM_NONE) {
        uint32_t opt_in = 1u;
        xcb_change_property(
            server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_root,
            server->atom_randr_dpi_opt_in, XCB_ATOM_CARDINAL, 32, 1, &opt_in
        );
    }

    xcb_randr_get_screen_resources_current_cookie_t rck =
        xcb_randr_get_screen_resources_current(server->xcb_conn, server->xcb_root);
    xcb_randr_get_screen_resources_current_reply_t *rrep =
        xcb_randr_get_screen_resources_current_reply(server->xcb_conn, rck, nullptr);
    if (!rrep) {
        xcb_flush(server->xcb_conn);
        return;
    }

    int nout = xcb_randr_get_screen_resources_current_outputs_length(rrep);
    xcb_randr_output_t *routs = xcb_randr_get_screen_resources_current_outputs(rrep);

    for (int i = 0; i < nout; ++i) {
        int matched_dpi = 96;
        float matched_scale = 1.0f;
        bool matched = false;

        xcb_randr_get_output_info_cookie_t ock =
            xcb_randr_get_output_info(server->xcb_conn, routs[i], rrep->config_timestamp);
        xcb_randr_get_output_info_reply_t *orep =
            xcb_randr_get_output_info_reply(server->xcb_conn, ock, nullptr);
        if (orep) {
            if (orep->crtc != XCB_NONE) {
                xcb_randr_get_crtc_info_cookie_t cck =
                    xcb_randr_get_crtc_info(server->xcb_conn, orep->crtc, rrep->config_timestamp);
                xcb_randr_get_crtc_info_reply_t *crep =
                    xcb_randr_get_crtc_info_reply(server->xcb_conn, cck, nullptr);
                if (crep) {
                    DtwmOutput *dout;
                    wl_list_for_each(dout, &server->outputs, link) {
                        struct wlr_box obox;
                        wlr_output_layout_get_box(server->output_layout, dout->wlr_output, &obox);
                        if (obox.x == crep->x && obox.y == crep->y) {
                            matched_dpi = dout->dpi > 0 ? dout->dpi : 96;
                            matched_scale = (dout->wlr_output && dout->wlr_output->scale > 0.25f)
                                                ? dout->wlr_output->scale
                                                : 1.0f;
                            matched = true;
                            break;
                        }
                    }
                    free(crep);
                }
            }
            free(orep);
        }

        if (!matched) {
            DtwmOutput *dout;
            wl_list_for_each(dout, &server->outputs, link) {
                if ((int)dout->output_idx == i) {
                    matched_dpi = dout->dpi > 0 ? dout->dpi : 96;
                    matched_scale = (dout->wlr_output && dout->wlr_output->scale > 0.25f)
                                        ? dout->wlr_output->scale
                                        : 1.0f;
                    break;
                }
            }
        }

        int logical_dpi = (int)lround((double)matched_dpi / (double)matched_scale);
        int32_t dpi_i32 = (logical_dpi >= 1 && logical_dpi <= 4096) ? logical_dpi : 96;
        xcb_randr_change_output_property(
            server->xcb_conn,
            routs[i],
            server->atom_dpi,
            XCB_ATOM_INTEGER,
            32,
            XCB_PROP_MODE_REPLACE,
            1,
            &dpi_i32
        );
    }

    free(rrep);
    xcb_flush(server->xcb_conn);
}

void dtwm_set_output_dpi(DtwmServer *server, uint32_t output_idx, int dpi) {
    if (!server || dpi < 1 || dpi > 4096) return;
    if (output_idx < 16) {
        server->initial_output_dpi[output_idx] = dpi;
    }
    DtwmOutput *dout;
    wl_list_for_each(dout, &server->outputs, link) {
        if (dout->output_idx == output_idx) {
            dout->dpi = dpi;
            if (dout->wlr_output && dout->wlr_output->name) {
                dtwm_power_display_record_dpi(server, dout->wlr_output->name, dpi);
            }
        }
    }
    dtwm_output_mgmt_after_reconfigure(server);
}

static bool parse_dp_hex_color(const char *line, DtwmRgba *out_rgb) {
    if (!line || !out_rgb) return false;
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line != '#') return false;
    line++;
    size_t n = 0;
    while (isxdigit((unsigned char)line[n])) n++;
    unsigned int r = 0, g = 0, b = 0;
    double denom = 255.0;
    if (n >= 12) {
        if (sscanf(line, "%4x%4x%4x", &r, &g, &b) != 3) return false;
        denom = 65535.0;
    } else if (n >= 6) {
        if (sscanf(line, "%2x%2x%2x", &r, &g, &b) != 3) return false;
        denom = 255.0;
    } else {
        return false;
    }
    *out_rgb = (DtwmRgba){(double)r / denom, (double)g / denom, (double)b / denom, 1.0};
    return true;
}

static DtwmColorSet build_colorset_from_bg(DtwmRgba bg) {
    /* Exact Motif Color.c / ColorP.h Brightness() & XmDEFAULT_FOREGROUND_THRESHOLD (70%) */
    double intensity = (bg.r + bg.g + bg.b) / 3.0;
    double luminosity = 0.30 * bg.r + 0.59 * bg.g + 0.11 * bg.b;
    double brightness = 0.75 * intensity + 0.25 * luminosity;
    DtwmRgba fg =
        (brightness > 0.70) ? (DtwmRgba){0.0, 0.0, 0.0, 1.0} : (DtwmRgba){1.0, 1.0, 1.0, 1.0};

    DtwmRgba ts, bs;
    if (brightness > 0.85) {
        /* XmCOLOR_LITE model */
        ts = (DtwmRgba){bg.r * 0.80, bg.g * 0.80, bg.b * 0.80, 1.0};
        bs = (DtwmRgba){bg.r * 0.60, bg.g * 0.60, bg.b * 0.60, 1.0};
    } else if (brightness < 0.15) {
        /* XmCOLOR_DARK model */
        ts = (DtwmRgba){bg.r + (1.0 - bg.r) * 0.50, bg.g + (1.0 - bg.g) * 0.50, bg.b + (1.0 - bg.b) * 0.50, 1.0};
        bs = (DtwmRgba){bg.r + (1.0 - bg.r) * 0.30, bg.g + (1.0 - bg.g) * 0.30, bg.b + (1.0 - bg.b) * 0.30, 1.0};
    } else {
        /* XmCOLOR_STD model (interpolates LO..HI shadow factors across brightness) */
        double ts_f = 0.60 - brightness * (0.60 - 0.50);
        double bs_f = 0.60 - brightness * (0.60 - 0.40);
        ts = (DtwmRgba){
            fmin(1.0, bg.r + (1.0 - bg.r) * ts_f),
            fmin(1.0, bg.g + (1.0 - bg.g) * ts_f),
            fmin(1.0, bg.b + (1.0 - bg.b) * ts_f),
            1.0,
        };
        bs = (DtwmRgba){bg.r * (1.0 - bs_f), bg.g * (1.0 - bs_f), bg.b * (1.0 - bs_f), 1.0};
    }
    return (DtwmColorSet){.bg = bg, .fg = fg, .top_shadow = ts, .bottom_shadow = bs};
}

static void spawn_color_sync_for_scheme(DtwmServer *server, const char *sync_arg);

static bool parse_dp_color_entry(const char *line, DtwmRgba *out_rgb) {
    if (!line || !out_rgb) return false;
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0' || *line == '!') return false;
    if (parse_dp_hex_color(line, out_rgb)) return true;
    if (strncasecmp(line, "black", 5) == 0 &&
        (line[5] == '\0' || isspace((unsigned char)line[5]))) {
        *out_rgb = (DtwmRgba){0.0, 0.0, 0.0, 1.0};
        return true;
    }
    if (strncasecmp(line, "white", 5) == 0 &&
        (line[5] == '\0' || isspace((unsigned char)line[5]))) {
        *out_rgb = (DtwmRgba){1.0, 1.0, 1.0, 1.0};
        return true;
    }
    return false;
}

static bool dtwm_server_apply_palette_and_profile(
    DtwmServer *server,
    const char *raw_args,
    bool has_explicit_prof,
    DtwmAppearanceProfile explicit_prof
) {
    if (!server || !raw_args) return false;
    while (*raw_args && (isspace((unsigned char)*raw_args) || *raw_args == '"' || *raw_args == '\'')) {
        raw_args++;
    }
    char name[128] = {0};
    size_t o = 0;
    while (raw_args[o] && !isspace((unsigned char)raw_args[o]) && raw_args[o] != '"' &&
           raw_args[o] != '\'' && o + 1 < sizeof(name)) {
        name[o] = raw_args[o];
        o++;
    }
    name[o] = '\0';
    if (o == 0) return false;

    const char *rest = raw_args + o;
    while (*rest && (isspace((unsigned char)*rest) || *rest == '"' || *rest == '\'')) rest++;
    if (*rest && !has_explicit_prof) {
        explicit_prof = dtwm_parse_appearance_profile(rest);
        has_explicit_prof = true;
    }

    size_t nlen = strlen(name);
    if (nlen > 3 && strcasecmp(name + nlen - 3, ".dp") == 0) {
        name[nlen - 3] = '\0';
    }

    /* Pure profile-named palettes with no overriding style behave identically to before */
    DtwmAppearanceProfile named_prof = dtwm_parse_appearance_profile(name);
    if (!has_explicit_prof && named_prof != DTWM_PROFILE_CLASSIC) {
        dtwm_server_apply_appearance_profile(server, named_prof);
        return true;
    }

    const char *home = getenv("HOME");
    char cand[4][512];
    int ncand = 0;
    if (home && home[0]) {
        snprintf(cand[ncand++], sizeof(cand[0]), "%s/.dt/palettes/%s.dp", home, name);
    }
    snprintf(cand[ncand++], sizeof(cand[0]), "/etc/dt/palettes/%s.dp", name);
    snprintf(cand[ncand++], sizeof(cand[0]), CDE_INSTALLATION_TOP "/palettes/%s.dp", name);
    snprintf(cand[ncand++], sizeof(cand[0]), CDE_INSTALLATION_TOP "/share/palettes/%s.dp", name);

    FILE *fp = nullptr;
    for (int i = 0; i < ncand && !fp; ++i) {
        fp = fopen(cand[i], "r");
    }
    if (!fp) {
        if (has_explicit_prof) {
            dtwm_server_apply_appearance_profile(server, explicit_prof);
            return true;
        }
        return false;
    }

    DtwmColorSet sets[8];
    int count = 0;
    char line[256];
    while (count < 8 && fgets(line, sizeof(line), fp)) {
        DtwmRgba bg;
        if (parse_dp_color_entry(line, &bg)) {
            sets[count++] = build_colorset_from_bg(bg);
        }
    }
    fclose(fp);
    if (count < 2) return false;
    while (count < 8) {
        sets[count] = sets[count % 2];
        count++;
    }

    DtwmAppearanceProfile final_prof = has_explicit_prof ? explicit_prof : server->palette.profile;
    if (strcasecmp(name, "HighContrast") == 0) {
        final_prof = DTWM_PROFILE_HIGH_CONTRAST;
    }
    server->palette.profile = final_prof;
    server->palette.active = sets[0];
    server->palette.inactive = sets[1];
    server->palette.menu = sets[1];
    server->palette.window_bg = sets[4];

    static const int ws_cs_map[4] = {2, 7, 5, 6};
    for (uint32_t i = 0; i < server->num_workspaces; ++i) {
        server->workspaces[i].colors = sets[ws_cs_map[i % 4]];
        if (final_prof == DTWM_PROFILE_HIGH_CONTRAST) {
            snprintf(server->workspaces[i].backdrop_name, sizeof(server->workspaces[i].backdrop_name), "NoBackdrop");
        }
    }
    server->palette.desktop_bg = server->workspaces[server->current_workspace].colors.bg;

    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped) {
            dtwm_view_update_frame(v);
        }
    }
    dtwm_icons_invalidate_theme_icons(server);
    dtwm_update_all_backdrops(server);
    dtwm_update_front_panel(server);
    dtwm_icons_relayout(server);
    xcb_sync_wm_properties(server);

    double l1 = 0.2126 * sets[1].bg.r + 0.7152 * sets[1].bg.g + 0.0722 * sets[1].bg.b;
    double l4 = 0.2126 * sets[4].bg.r + 0.7152 * sets[4].bg.g + 0.0722 * sets[4].bg.b;
    double l3 = 0.2126 * sets[3].bg.r + 0.7152 * sets[3].bg.g + 0.0722 * sets[3].bg.b;
    double avg_lum = 0.45 * l1 + 0.35 * l4 + 0.20 * l3;
    const char *scheme = "light";
    if (final_prof == DTWM_PROFILE_HIGH_CONTRAST) {
        scheme = "high_contrast";
    } else if (final_prof == DTWM_PROFILE_MODERN_DARK || avg_lum < 0.30) {
        scheme = "dark";
    }
    spawn_color_sync_for_scheme(server, scheme);
    return true;
}

static bool dtwm_server_apply_palette_file(DtwmServer *server, const char *raw_name) {
    return dtwm_server_apply_palette_and_profile(server, raw_name, false, DTWM_PROFILE_CLASSIC);
}

static void spawn_color_sync_for_scheme(DtwmServer *server, const char *sync_arg) {
    if (!server || server->greeter_mode || !sync_arg) return;
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            if (server->wayland_socket) setenv("WAYLAND_DISPLAY", server->wayland_socket, 1);
            if (server->xwayland && server->xwayland->display_name) {
                setenv("DISPLAY", server->xwayland->display_name, 1);
            }
            if (server->exe_dir[0]) {
                char local_bin[1152];
                snprintf(local_bin, sizeof(local_bin), "%s/cde-color-sync", server->exe_dir);
                execl(local_bin, "cde-color-sync", "--apply", sync_arg, (char *)nullptr);
            }
            execlp("cde-color-sync", "cde-color-sync", "--apply", sync_arg, (char *)nullptr);
            execl(CDE_INSTALLATION_TOP "/bin/cde-color-sync", "cde-color-sync", "--apply", sync_arg, (char *)nullptr);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
    }
}

static void spawn_color_sync_for_profile(DtwmServer *server, DtwmAppearanceProfile profile) {
    const char *sync_arg =
        (profile == DTWM_PROFILE_MODERN_DARK) ? "dark" :
        (profile == DTWM_PROFILE_HIGH_CONTRAST) ? "high_contrast" : "light";
    spawn_color_sync_for_scheme(server, sync_arg);
}

void dtwm_server_apply_appearance_profile(DtwmServer *server, DtwmAppearanceProfile profile) {
    if (!server) return;
    dtwm_palette_init_profile(&server->palette, profile);
    for (uint32_t i = 0; i < server->num_workspaces; ++i) {
        server->workspaces[i].colors = make_ws_colorset(profile, i);
        if (profile == DTWM_PROFILE_HIGH_CONTRAST) {
            snprintf(server->workspaces[i].backdrop_name, sizeof(server->workspaces[i].backdrop_name), "NoBackdrop");
        }
    }
    server->palette.desktop_bg = server->workspaces[server->current_workspace].colors.bg;

    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped) {
            dtwm_view_update_frame(v);
        }
    }
    dtwm_icons_invalidate_theme_icons(server);
    dtwm_update_all_backdrops(server);
    dtwm_update_front_panel(server);
    dtwm_icons_relayout(server);
    xcb_sync_wm_properties(server);
    spawn_color_sync_for_profile(server, profile);
}

void dtwm_dispatch_wm_request_bytes(DtwmServer *server, const char *data, size_t len) {
    if (!server || !data || len == 0) return;
    size_t pos = 0;
    while (pos < len) {
        size_t slen = strnlen(data + pos, len - pos);
        if (slen > 0) {
            char item[2048];
            size_t cp = slen < sizeof(item) - 1 ? slen : sizeof(item) - 1;
            memcpy(item, data + pos, cp);
            item[cp] = '\0';
            handle_dt_wm_request_item(server, item);
        }
        pos += slen + 1;
    }
}

static void xcb_drain_dt_wm_requests(DtwmServer *server, xcb_window_t req_win) {
    if (!server->xcb_conn || req_win == XCB_WINDOW_NONE) return;
    /* Atomically read & delete _DT_WM_REQUEST (same pattern as classic dtwm WmIPC.c) */
    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn,
        1, /* delete = True */
        req_win,
        server->atom_wm_request,
        XCB_GET_PROPERTY_TYPE_ANY,
        0,
        4096
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return;

    int len = xcb_get_property_value_length(rep);
    if (len > 0) {
        dtwm_dispatch_wm_request_bytes(
            server, (const char *)xcb_get_property_value(rep), (size_t)len
        );
    }
    free(rep);
}

static DtwmView *find_xwayland_view_by_win(DtwmServer *server, xcb_window_t win) {
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->kind == DTWM_VIEW_XWAYLAND && v->xwayland_surface &&
            v->xwayland_surface->window_id == win) {
            return v;
        }
    }
    return nullptr;
}

static bool xrdb_key_matches(const char *lhs, size_t lhs_len, const char *suffix) {
    while (lhs_len > 0 && isspace((unsigned char)lhs[lhs_len - 1])) {
        lhs_len--;
    }
    size_t slen = strlen(suffix);
    if (lhs_len < slen) {
        return false;
    }
    if (strncasecmp(lhs + (lhs_len - slen), suffix, slen) != 0) {
        return false;
    }
    if (lhs_len == slen) {
        return true;
    }
    char sep = lhs[lhs_len - slen - 1];
    return sep == '.' || sep == '*' || isspace((unsigned char)sep);
}

static bool parse_xrdb_bool(const char *val, bool fallback) {
    if (!val) return fallback;
    while (*val && isspace((unsigned char)*val)) val++;
    if (strncasecmp(val, "true", 4) == 0 || strncasecmp(val, "on", 2) == 0 ||
        strncasecmp(val, "yes", 3) == 0 || val[0] == '1') {
        return true;
    }
    if (strncasecmp(val, "false", 5) == 0 || strncasecmp(val, "off", 3) == 0 ||
        strncasecmp(val, "no", 2) == 0 || val[0] == '0') {
        return false;
    }
    return fallback;
}

void dtwm_apply_resource_manager_string(DtwmServer *server, const char *raw_data, size_t len) {
    if (!server || !raw_data || len == 0) return;
    char *txt = calloc(len + 1, 1);
    if (!txt) return;
    memcpy(txt, raw_data, len);
    {
        if (txt) {
            const char *last_prof = nullptr;
            const char *last_pal = nullptr;
            char xcursor_theme[128] = {0};
            int xcursor_size = server->cursor_size > 0 ? server->cursor_size : 24;
            bool cursor_changed = false;
            bool tp_changed = false;
            bool wheel_changed = false;
            bool has_auto_raise = false;
            snprintf(xcursor_theme, sizeof(xcursor_theme), "%s", server->cursor_theme);

            char *saveptr = nullptr;
            for (char *line = strtok_r(txt, "\r\n", &saveptr); line;
                 line = strtok_r(nullptr, "\r\n", &saveptr)) {
                while (*line && isspace((unsigned char)*line)) line++;
                if (*line == '\0' || *line == '!') continue;
                char *colon = strchr(line, ':');
                if (!colon) continue;
                size_t klen = (size_t)(colon - line);
                const char *val = colon + 1;
                while (*val && isspace((unsigned char)*val)) val++;

                if (xrdb_key_matches(line, klen, "appearanceProfile")) {
                    last_prof = val;
                } else if (xrdb_key_matches(line, klen, "ColorPalette")) {
                    last_pal = val;
                } else if (xrdb_key_matches(line, klen, "moveOpaque")) {
                    server->move_opaque = parse_xrdb_bool(val, server->move_opaque);
                } else if (xrdb_key_matches(line, klen, "keyboardFocusPolicy")) {
                    server->focus_follows_mouse = (strncasecmp(val, "pointer", 7) == 0);
                    if (!has_auto_raise) {
                        server->focus_auto_raise = !server->focus_follows_mouse;
                    }
                } else if (xrdb_key_matches(line, klen, "focusAutoRaise")) {
                    server->focus_auto_raise = parse_xrdb_bool(val, server->focus_auto_raise);
                    has_auto_raise = true;
                } else if (xrdb_key_matches(line, klen, "secondariesOnTop")) {
                    server->allow_primary_on_top = !parse_xrdb_bool(val, true);
                } else if (xrdb_key_matches(line, klen, "animateMinimizeRestore")) {
                    server->legacy_animate_minimize = parse_xrdb_bool(val, server->legacy_animate_minimize);
                } else if (xrdb_key_matches(line, klen, "minimizeAnimation")) {
                    server->minimize_anim_style = dtwm_parse_anim_style(val);
                } else if (xrdb_key_matches(line, klen, "maximizeAnimation")) {
                    server->maximize_anim_style = dtwm_parse_anim_style(val);
                } else if (xrdb_key_matches(line, klen, "animationSpeed")) {
                    server->anim_speed = dtwm_parse_anim_speed(val);
                } else if (xrdb_key_matches(line, klen, "showTaskSwitcher")) {
                    server->show_task_switcher = parse_xrdb_bool(val, server->show_task_switcher);
                } else if (xrdb_key_matches(line, klen, "useIconBox")) {
                    dtwm_icons_set_use_icon_box(server, parse_xrdb_bool(val, server->use_icon_box));
                } else if (xrdb_key_matches(line, klen, "iconPlacement")) {
                    dtwm_icons_set_placement(server, val);
                } else if (xrdb_key_matches(line, klen, "iconPlacementMargin")) {
                    dtwm_icons_set_placement_margin(server, atoi(val));
                } else if (xrdb_key_matches(line, klen, "doubleIconSize")) {
                    dtwm_icons_set_large_icons(server, parse_xrdb_bool(val, server->large_icons));
                } else if (xrdb_key_matches(line, klen, "showOpenWindowIcons")) {
                    dtwm_icons_set_open_window_icons(
                        server, parse_xrdb_bool(val, server->show_open_window_icons)
                    );
                } else if (xrdb_key_matches(line, klen, "iconImage")) {
                    /* Parse Dtwm*<ClientClassOrName>*iconImage: <IconName> */
                    size_t endk = klen;
                    while (endk > 0 && isspace((unsigned char)line[endk - 1])) endk--;
                    if (endk > 9) {
                        endk -= 9; /* strip "iconImage" */
                        while (endk > 0 && (line[endk - 1] == '*' || line[endk - 1] == '.')) endk--;
                        size_t startk = endk;
                        while (startk > 0 && line[startk - 1] != '*' && line[startk - 1] != '.' &&
                               !isspace((unsigned char)line[startk - 1])) {
                            startk--;
                        }
                        if (endk > startk && (endk - startk) < 120) {
                            char ckey[128], iname[128];
                            memcpy(ckey, line + startk, endk - startk);
                            ckey[endk - startk] = '\0';
                            size_t vi = 0;
                            while (val[vi] && !isspace((unsigned char)val[vi]) && vi + 1 < sizeof(iname)) {
                                iname[vi] = val[vi];
                                vi++;
                            }
                            iname[vi] = '\0';
                            if (strcasecmp(ckey, "Dtwm") != 0 && iname[0]) {
                                dtwm_icons_record_icon_image_rule(ckey, iname);
                            }
                        }
                    }
                } else if (xrdb_key_matches(line, klen, "ws0*backdrop*image") ||
                           xrdb_key_matches(line, klen, "ws0.backdrop.image")) {
                    char bname[256] = {0};
                    size_t bi = 0;
                    while (val[bi] && !isspace((unsigned char)val[bi]) && bi + 1 < sizeof(bname)) {
                        bname[bi] = val[bi];
                        bi++;
                    }
                    if (bname[0] && server->num_workspaces > 0) {
                        dtwm_set_workspace_backdrop(server, 0, bname, server->workspaces[0].image_type);
                    }
                } else if (xrdb_key_matches(line, klen, "ws1*backdrop*image") ||
                           xrdb_key_matches(line, klen, "ws1.backdrop.image")) {
                    char bname[256] = {0};
                    size_t bi = 0;
                    while (val[bi] && !isspace((unsigned char)val[bi]) && bi + 1 < sizeof(bname)) {
                        bname[bi] = val[bi];
                        bi++;
                    }
                    if (bname[0] && server->num_workspaces > 1) {
                        dtwm_set_workspace_backdrop(server, 1, bname, server->workspaces[1].image_type);
                    }
                } else if (xrdb_key_matches(line, klen, "ws2*backdrop*image") ||
                           xrdb_key_matches(line, klen, "ws2.backdrop.image")) {
                    char bname[256] = {0};
                    size_t bi = 0;
                    while (val[bi] && !isspace((unsigned char)val[bi]) && bi + 1 < sizeof(bname)) {
                        bname[bi] = val[bi];
                        bi++;
                    }
                    if (bname[0] && server->num_workspaces > 2) {
                        dtwm_set_workspace_backdrop(server, 2, bname, server->workspaces[2].image_type);
                    }
                } else if (xrdb_key_matches(line, klen, "ws3*backdrop*image") ||
                           xrdb_key_matches(line, klen, "ws3.backdrop.image")) {
                    char bname[256] = {0};
                    size_t bi = 0;
                    while (val[bi] && !isspace((unsigned char)val[bi]) && bi + 1 < sizeof(bname)) {
                        bname[bi] = val[bi];
                        bi++;
                    }
                    if (bname[0] && server->num_workspaces > 3) {
                        dtwm_set_workspace_backdrop(server, 3, bname, server->workspaces[3].image_type);
                    }
                } else if (xrdb_key_matches(line, klen, "multiClickTime")) {
                    int ms = atoi(val);
                    if (ms >= 100 && ms <= 2000) {
                        server->double_click_ms = (uint32_t)ms;
                    }
                } else if (xrdb_key_matches(line, klen, "Xcursor.theme")) {
                    size_t n = 0;
                    while (val[n] && !isspace((unsigned char)val[n]) && n + 1 < sizeof(xcursor_theme)) {
                        xcursor_theme[n] = val[n];
                        n++;
                    }
                    xcursor_theme[n] = '\0';
                    cursor_changed = true;
                } else if (xrdb_key_matches(line, klen, "Xcursor.size")) {
                    int sz = atoi(val);
                    if (sz >= 12 && sz <= 128) {
                        xcursor_size = sz;
                        cursor_changed = true;
                    }
                } else if (xrdb_key_matches(line, klen, "touchpadTapToClick")) {
                    server->tp_tap_to_click = parse_xrdb_bool(val, server->tp_tap_to_click);
                    tp_changed = true;
                } else if (xrdb_key_matches(line, klen, "touchpadNaturalScroll")) {
                    server->tp_natural_scroll = parse_xrdb_bool(val, server->tp_natural_scroll);
                    tp_changed = true;
                } else if (xrdb_key_matches(line, klen, "touchpadClickfinger")) {
                    server->tp_clickfinger = parse_xrdb_bool(val, server->tp_clickfinger);
                    tp_changed = true;
                } else if (xrdb_key_matches(line, klen, "touchpadDisableWhileTyping")) {
                    server->tp_dwt = parse_xrdb_bool(val, server->tp_dwt);
                    tp_changed = true;
                } else if (xrdb_key_matches(line, klen, "touchpadDisableWithExternalMouse")) {
                    server->tp_disable_with_ext_mouse = parse_xrdb_bool(val, server->tp_disable_with_ext_mouse);
                    tp_changed = true;
                } else if (xrdb_key_matches(line, klen, "mouseWheelNatural")) {
                    server->wheel_natural_scroll = parse_xrdb_bool(val, server->wheel_natural_scroll);
                    wheel_changed = true;
                } else if (xrdb_key_matches(line, klen, "mouseWheelSpeed")) {
                    int spd = atoi(val);
                    if (spd >= 1 && spd <= 10) {
                        server->wheel_scroll_speed = spd;
                        wheel_changed = true;
                    }
                } else if (xrdb_key_matches(line, klen, "mouseWheelSmooth")) {
                    server->wheel_smooth_scroll = parse_xrdb_bool(val, server->wheel_smooth_scroll);
                    wheel_changed = true;
                }
            }
            if (cursor_changed) {
                dtwm_reload_cursor_theme(server, xcursor_theme, xcursor_size);
            }
            if (wheel_changed) {
                dtwm_hw_publish_wheel_ipc(server);
                dtwm_hw_publish_state(server);
            }
            if (tp_changed) {
                dtwm_hw_reapply_all_pointers(server);
                dtwm_hw_publish_state(server);
            }
            if (last_pal && last_prof) {
                dtwm_server_apply_palette_and_profile(
                    server, last_pal, true, dtwm_parse_appearance_profile(last_prof)
                );
            } else if (last_pal) {
                dtwm_server_apply_palette_file(server, last_pal);
            } else if (last_prof) {
                dtwm_server_apply_appearance_profile(server, dtwm_parse_appearance_profile(last_prof));
            }
            free(txt);
        }
    }
}

static void sync_xcb_resource_manager_profile(DtwmServer *server) {
    if (!server || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) return;
    xcb_get_property_cookie_t ck = xcb_get_property(
        server->xcb_conn, 0, server->xcb_root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 65536
    );
    xcb_get_property_reply_t *rep = xcb_get_property_reply(server->xcb_conn, ck, nullptr);
    if (!rep) return;
    int len = xcb_get_property_value_length(rep);
    if (len > 0) {
        const char *val = (const char *)xcb_get_property_value(rep);
        dtwm_apply_resource_manager_string(server, val, (size_t)len);
        dtwm_native_ipc_publish_prop(server, "RESOURCE_MANAGER", val, (size_t)len);
        dtwm_sync_resource_manager_dpi(server);
    }
    free(rep);
}

static int handle_xcb_event_fd(int fd, uint32_t mask, void *data) {
    (void)fd;
    (void)mask;
    DtwmServer *server = data;
    if (!server->xcb_conn) return 0;

    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(server->xcb_conn)) != nullptr) {
        if (dtwm_xdnd_handle_xcb_event(server, ev)) {
            free(ev);
            continue;
        }
        if (dtwm_a11y_handle_xcb_event(server, ev)) {
            free(ev);
            continue;
        }
        uint8_t rt = ev->response_type & ~0x80u;
        if (rt == XCB_PROPERTY_NOTIFY) {
            xcb_property_notify_event_t *pev = (xcb_property_notify_event_t *)ev;
            if ((pev->window == server->xcb_wm_win || pev->window == server->xcb_root) &&
                pev->atom == server->atom_wm_request &&
                pev->state == XCB_PROPERTY_NEW_VALUE) {
                xcb_drain_dt_wm_requests(server, pev->window);
            } else if (pev->window == server->xcb_root &&
                       pev->atom == XCB_ATOM_RESOURCE_MANAGER &&
                       pev->state == XCB_PROPERTY_NEW_VALUE) {
                sync_xcb_resource_manager_profile(server);
            } else if (pev->atom == server->atom_ws_hints &&
                       pev->state == XCB_PROPERTY_NEW_VALUE) {
                DtwmView *v = find_xwayland_view_by_win(server, pev->window);
                if (v) {
                    xcb_apply_client_ws_hints(v);
                }
            } else if (pev->state == XCB_PROPERTY_NEW_VALUE) {
                dtwm_icons_on_xcb_property(server, pev->window, pev->atom);
            }
        }
        free(ev);
    }
    xcb_flush(server->xcb_conn);
    return 0;
}

void dtwm_workspaces_on_xwayland_ready(DtwmServer *server) {
    if (!server->xwayland || !server->xwayland->display_name) return;

    server->xcb_conn = xcb_connect(server->xwayland->display_name, nullptr);
    if (!server->xcb_conn || xcb_connection_has_error(server->xcb_conn)) {
        if (server->xcb_conn) {
            xcb_disconnect(server->xcb_conn);
            server->xcb_conn = nullptr;
        }
        return;
    }

    const xcb_setup_t *setup = xcb_get_setup(server->xcb_conn);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    server->xcb_root = screen->root;

    server->atom_mwm_info = intern_atom_sync(server->xcb_conn, "_MOTIF_WM_INFO");
    server->atom_mwm_hints = intern_atom_sync(server->xcb_conn, "_MOTIF_WM_HINTS");
    server->atom_dt_wm_hints = intern_atom_sync(server->xcb_conn, "_DT_WM_HINTS");
    server->atom_active_win_box = intern_atom_sync(server->xcb_conn, "_DT_ACTIVE_WINDOW_BOX");
    server->atom_client_ids = intern_atom_sync(server->xcb_conn, "_DT_WAYLAND_CLIENT_IDS");
    server->atom_randr_dpi_opt_in = intern_atom_sync(server->xcb_conn, "_MOTIF_RANDR_OUTPUT_DPI");
    server->atom_dpi = intern_atom_sync(server->xcb_conn, "DPI");
    server->atom_ws_list = intern_atom_sync(server->xcb_conn, "_DT_WORKSPACE_LIST");
    server->atom_ws_current = intern_atom_sync(server->xcb_conn, "_DT_WORKSPACE_CURRENT");
    server->atom_ws_hints = intern_atom_sync(server->xcb_conn, "_DT_WORKSPACE_HINTS");
    server->atom_ws_presence = intern_atom_sync(server->xcb_conn, "_DT_WORKSPACE_PRESENCE");
    server->atom_wm_request = intern_atom_sync(server->xcb_conn, "_DT_WM_REQUEST");
    server->atom_panel_busy_start_seq = intern_atom_sync(server->xcb_conn, "_DT_PANEL_BUSY_START_SEQ");
    server->atom_panel_busy_stop_seq = intern_atom_sync(server->xcb_conn, "_DT_PANEL_BUSY_STOP_SEQ");
    server->atom_ws_all = intern_atom_sync(server->xcb_conn, "all");

    /* Unmapped child of root for _MOTIF_WM_INFO (_DtGetMwmWindow checks parent == root) */
    server->xcb_wm_win = xcb_generate_id(server->xcb_conn);
    uint32_t cw_vals[2] = {1u, XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_create_window(
        server->xcb_conn,
        XCB_COPY_FROM_PARENT,
        server->xcb_wm_win,
        server->xcb_root,
        -100, -100, 1, 1, 0,
        XCB_WINDOW_CLASS_INPUT_ONLY,
        XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
        cw_vals
    );

    uint32_t mwm_info[2] = {2u /* MWM_INFO_STARTUP_CUSTOM */, server->xcb_wm_win};
    xcb_change_property(
        server->xcb_conn,
        XCB_PROP_MODE_REPLACE,
        server->xcb_root,
        server->atom_mwm_info,
        server->atom_mwm_info,
        32,
        2,
        mwm_info
    );

    /* Hidden 1x1 override-redirect InputOutput window (excluded from the Wayland scene graph)
     * used to deliver outside ButtonPress/Release events to XWayland's active pointer grab
     * when the user clicks a server-side window titlebar, border, native Wayland client, or
     * desktop background while an X11/Motif popup menu is posted. */
    server->xcb_popup_sink_win = xcb_generate_id(server->xcb_conn);
    uint32_t sink_vals[4] = {
        screen->black_pixel,
        1u,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
    };
    xcb_create_window(
        server->xcb_conn,
        screen->root_depth,
        server->xcb_popup_sink_win,
        server->xcb_root,
        0, 0, 1, 1, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_DONT_PROPAGATE,
        sink_vals
    );
    xcb_map_window(server->xcb_conn, server->xcb_popup_sink_win);
    uint32_t stack_below = XCB_STACK_MODE_BELOW;
    xcb_configure_window(
        server->xcb_conn, server->xcb_popup_sink_win, XCB_CONFIG_WINDOW_STACK_MODE, &stack_below
    );

    /* Watch RESOURCE_MANAGER on root so xrdb/dtstyle *appearanceProfile updates apply live */
    uint32_t root_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(server->xcb_conn, server->xcb_root, XCB_CW_EVENT_MASK, &root_mask);
    sync_xcb_resource_manager_profile(server);
    xcb_sync_wm_properties(server);
    dtwm_sync_xwayland_randr_dpi(server);
    dtwm_publish_active_window_box(server);
    dtwm_publish_client_ids(server);

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    int xfd = xcb_get_file_descriptor(server->xcb_conn);
    server->xcb_event_source = wl_event_loop_add_fd(
        loop, xfd, WL_EVENT_READABLE, handle_xcb_event_fd, server
    );

    spawn_genuine_front_panel(server);
}

void dtwm_workspaces_on_xwayland_view_map(DtwmView *view) {
    if (!view || view->kind != DTWM_VIEW_XWAYLAND || !view->xwayland_surface) return;
    if (view->xwayland_surface->override_redirect) return;
    DtwmServer *server = view->server;
    if (!server->xcb_conn) return;

    /* Subscribe to PropertyNotify on the X11 client window so DtWsmSetWorkspacesOccupied works live */
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(
        server->xcb_conn, view->xwayland_surface->window_id, XCB_CW_EVENT_MASK, &mask
    );
    xcb_apply_client_ws_hints(view);
    xcb_sync_view_presence(view);
}

void dtwm_notify_panel_busy_start(DtwmServer *server) {
    if (!server) {
        return;
    }
    uint32_t seq = ++server->panel_busy_start_seq;
    dtwm_native_ipc_publish_prop(server, "_DT_PANEL_BUSY_START_SEQ", &seq, sizeof(seq));
    if (server->xcb_conn && server->xcb_wm_win != XCB_NONE &&
        server->atom_panel_busy_start_seq != XCB_NONE) {
        xcb_change_property(
            server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_wm_win,
            server->atom_panel_busy_start_seq, XCB_ATOM_CARDINAL, 32, 1, &seq
        );
        xcb_flush(server->xcb_conn);
    }
}

void dtwm_notify_panel_busy_stop(DtwmServer *server) {
    if (!server) {
        return;
    }
    uint32_t seq = ++server->panel_busy_stop_seq;
    dtwm_native_ipc_publish_prop(server, "_DT_PANEL_BUSY_STOP_SEQ", &seq, sizeof(seq));
    if (server->xcb_conn && server->xcb_wm_win != XCB_NONE &&
        server->atom_panel_busy_stop_seq != XCB_NONE) {
        xcb_change_property(
            server->xcb_conn, XCB_PROP_MODE_REPLACE, server->xcb_wm_win,
            server->atom_panel_busy_stop_seq, XCB_ATOM_CARDINAL, 32, 1, &seq
        );
        xcb_flush(server->xcb_conn);
    }
}

/* --- Native Wayland ext-workspace-v1 Protocol Implementation --- */
static void ext_ws_handle_destroy_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void ext_ws_handle_activate_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    DtwmExtWsHandle *wh = wl_resource_get_user_data(resource);
    if (wh) wh->pending_activate = true;
}

static void ext_ws_handle_deactivate_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    (void)resource;
}

static void ext_ws_handle_assign_req(
    struct wl_client *client, struct wl_resource *resource, struct wl_resource *workspace_group
) {
    (void)client;
    (void)resource;
    (void)workspace_group;
}

static void ext_ws_handle_remove_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    DtwmExtWsHandle *wh = wl_resource_get_user_data(resource);
    if (wh) wh->pending_remove = true;
}

static const struct ext_workspace_handle_v1_interface ext_ws_handle_impl = {
    .destroy = ext_ws_handle_destroy_req,
    .activate = ext_ws_handle_activate_req,
    .deactivate = ext_ws_handle_deactivate_req,
    .assign = ext_ws_handle_assign_req,
    .remove = ext_ws_handle_remove_req,
};

static void ext_ws_handle_res_destroy(struct wl_resource *resource) {
    DtwmExtWsHandle *wh = wl_resource_get_user_data(resource);
    if (!wh) return;
    wl_list_remove(&wh->link);
    free(wh);
}

static void ext_ws_group_create_workspace_req(
    struct wl_client *client, struct wl_resource *resource, const char *workspace
) {
    (void)client;
    DtwmExtWsManager *mgr = wl_resource_get_user_data(resource);
    if (!mgr || !workspace) return;
    snprintf(mgr->pending_create_name, sizeof(mgr->pending_create_name), "%s", workspace);
    mgr->has_pending_create = true;
}

static void ext_ws_group_destroy_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct ext_workspace_group_handle_v1_interface ext_ws_group_impl = {
    .create_workspace = ext_ws_group_create_workspace_req,
    .destroy = ext_ws_group_destroy_req,
};

static void ext_ws_group_res_destroy(struct wl_resource *resource) {
    DtwmExtWsManager *mgr = wl_resource_get_user_data(resource);
    if (mgr && mgr->group_res == resource) {
        mgr->group_res = nullptr;
    }
}

static void ext_ws_mgr_commit_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    DtwmExtWsManager *mgr = wl_resource_get_user_data(resource);
    if (!mgr || !mgr->server) return;
    DtwmServer *server = mgr->server;

    if (mgr->has_pending_create) {
        mgr->has_pending_create = false;
        dtwm_add_workspace(server, mgr->pending_create_name);
    }

    int target_activate = -1;
    int target_remove = -1;
    DtwmExtWsHandle *wh;
    wl_list_for_each(wh, &mgr->handles, link) {
        if (wh->pending_activate) {
            wh->pending_activate = false;
            target_activate = (int)wh->ws_idx;
        }
        if (wh->pending_remove) {
            wh->pending_remove = false;
            target_remove = (int)wh->ws_idx;
        }
    }
    if (target_activate >= 0 && (uint32_t)target_activate < server->num_workspaces) {
        dtwm_switch_workspace(server, (uint32_t)target_activate);
    }
    if (target_remove >= 0 && (uint32_t)target_remove < server->num_workspaces) {
        dtwm_delete_workspace(server, (uint32_t)target_remove);
    }
}

static void ext_ws_mgr_stop_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    ext_workspace_manager_v1_send_finished(resource);
    wl_resource_destroy(resource);
}

static const struct ext_workspace_manager_v1_interface ext_ws_mgr_impl = {
    .commit = ext_ws_mgr_commit_req,
    .stop = ext_ws_mgr_stop_req,
};

static void send_ws_handle_details(DtwmServer *server, DtwmExtWsHandle *wh) {
    if (wh->ws_idx >= server->num_workspaces) return;
    const DtwmWorkspace *ws = &server->workspaces[wh->ws_idx];
    ext_workspace_handle_v1_send_name(wh->resource, ws->title);

    struct wl_array coords;
    wl_array_init(&coords);
    uint32_t *pos = wl_array_add(&coords, sizeof(uint32_t) * 2);
    if (pos) {
        pos[0] = wh->ws_idx % 2u;
        pos[1] = wh->ws_idx / 2u;
    }
    ext_workspace_handle_v1_send_coordinates(wh->resource, &coords);
    wl_array_release(&coords);

    uint32_t st = (wh->ws_idx == server->current_workspace) ? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0u;
    ext_workspace_handle_v1_send_state(wh->resource, st);
    ext_workspace_handle_v1_send_capabilities(
        wh->resource,
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE |
            (server->num_workspaces > 1 ? EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_REMOVE : 0u)
    );
}

static DtwmExtWsHandle *create_ext_ws_handle_for_mgr(DtwmExtWsManager *mgr, uint32_t ws_idx) {
    struct wl_client *client = wl_resource_get_client(mgr->mgr_res);
    struct wl_resource *wres = wl_resource_create(
        client, &ext_workspace_handle_v1_interface, wl_resource_get_version(mgr->mgr_res), 0
    );
    if (!wres) return nullptr;

    DtwmExtWsHandle *wh = calloc(1, sizeof(*wh));
    wh->mgr = mgr;
    wh->resource = wres;
    wh->ws_idx = ws_idx;
    wl_resource_set_implementation(wres, &ext_ws_handle_impl, wh, ext_ws_handle_res_destroy);
    wl_list_insert(mgr->handles.prev, &wh->link);

    ext_workspace_manager_v1_send_workspace(mgr->mgr_res, wres);
    ext_workspace_handle_v1_send_id(wres, mgr->server->workspaces[ws_idx].id_name);
    send_ws_handle_details(mgr->server, wh);
    if (mgr->group_res) {
        ext_workspace_group_handle_v1_send_workspace_enter(mgr->group_res, wres);
    }
    return wh;
}

static void sync_single_ext_ws_mgr(DtwmExtWsManager *mgr) {
    DtwmServer *server = mgr->server;
    /* Remove excess handles if a workspace was deleted */
    DtwmExtWsHandle *wh, *tmp;
    wl_list_for_each_safe(wh, tmp, &mgr->handles, link) {
        if (wh->ws_idx >= server->num_workspaces) {
            if (mgr->group_res) {
                ext_workspace_group_handle_v1_send_workspace_leave(mgr->group_res, wh->resource);
            }
            ext_workspace_handle_v1_send_removed(wh->resource);
            wl_resource_set_user_data(wh->resource, nullptr);
            wl_list_remove(&wh->link);
            free(wh);
        }
    }

    /* Update existing handles and create handles for newly added workspaces */
    for (uint32_t i = 0; i < server->num_workspaces; ++i) {
        DtwmExtWsHandle *found = nullptr;
        wl_list_for_each(wh, &mgr->handles, link) {
            if (wh->ws_idx == i) {
                found = wh;
                break;
            }
        }
        if (found) {
            send_ws_handle_details(server, found);
        } else {
            create_ext_ws_handle_for_mgr(mgr, i);
        }
    }
    ext_workspace_manager_v1_send_done(mgr->mgr_res);
}

static void ext_ws_broadcast_sync(DtwmServer *server) {
    DtwmExtWsManager *mgr;
    wl_list_for_each(mgr, &server->ext_ws_managers, link) {
        sync_single_ext_ws_mgr(mgr);
    }
}

static void ext_ws_mgr_res_destroy(struct wl_resource *resource) {
    DtwmExtWsManager *mgr = wl_resource_get_user_data(resource);
    if (!mgr) return;
    DtwmExtWsHandle *wh, *tmp;
    wl_list_for_each_safe(wh, tmp, &mgr->handles, link) {
        wl_resource_set_user_data(wh->resource, nullptr);
        wl_list_remove(&wh->link);
        free(wh);
    }
    if (mgr->group_res) {
        wl_resource_set_user_data(mgr->group_res, nullptr);
    }
    wl_list_remove(&mgr->link);
    free(mgr);
}

static void bind_ext_ws_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    DtwmServer *server = data;
    struct wl_resource *mres = wl_resource_create(client, &ext_workspace_manager_v1_interface, (int)version, id);
    if (!mres) {
        wl_client_post_no_memory(client);
        return;
    }
    DtwmExtWsManager *mgr = calloc(1, sizeof(*mgr));
    mgr->server = server;
    mgr->mgr_res = mres;
    wl_list_init(&mgr->handles);
    wl_resource_set_implementation(mres, &ext_ws_mgr_impl, mgr, ext_ws_mgr_res_destroy);
    wl_list_insert(&server->ext_ws_managers, &mgr->link);

    struct wl_resource *gres = wl_resource_create(
        client, &ext_workspace_group_handle_v1_interface, (int)version, 0
    );
    if (gres) {
        mgr->group_res = gres;
        wl_resource_set_implementation(gres, &ext_ws_group_impl, mgr, ext_ws_group_res_destroy);
        ext_workspace_manager_v1_send_workspace_group(mres, gres);
        ext_workspace_group_handle_v1_send_capabilities(
            gres, EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE
        );
    }

    for (uint32_t i = 0; i < server->num_workspaces; ++i) {
        create_ext_ws_handle_for_mgr(mgr, i);
    }
    ext_workspace_manager_v1_send_done(mres);
}

/* --- Initialization & Cleanup --- */
void dtwm_workspaces_init(DtwmServer *server) {
    static const char *const default_titles[4] = {"One", "Two", "Three", "Four"};
    static const char *const default_backdrops[4] = {"KnitLight", "WaterDrops", "SkyDark", "Paver"};

    wl_list_init(&server->ext_ws_managers);
    server->num_workspaces = 4;
    server->current_workspace = 0;
    server->next_ws_serial = 4;
    server->show_front_panel = true;

    for (uint32_t i = 0; i < 4; ++i) {
        DtwmWorkspace *ws = &server->workspaces[i];
        snprintf(ws->id_name, sizeof(ws->id_name), "ws%u", i);
        snprintf(ws->title, sizeof(ws->title), "%s", default_titles[i]);
        snprintf(ws->backdrop_name, sizeof(ws->backdrop_name), "%s", default_backdrops[i]);
        ws->color_set_id = (int)i + 1;
        ws->image_type = 0;
        ws->colors = make_ws_colorset(server->palette.profile, i);
    }
    server->palette.desktop_bg = server->workspaces[0].colors.bg;

    server->ext_ws_global = wl_global_create(
        server->wl_display, &ext_workspace_manager_v1_interface, 1, server, bind_ext_ws_manager
    );
}

void dtwm_workspaces_finish(DtwmServer *server) {
    stop_genuine_front_panel(server);
    if (server->xcb_event_source) {
        wl_event_source_remove(server->xcb_event_source);
        server->xcb_event_source = nullptr;
    }
    if (server->xcb_conn) {
        xcb_disconnect(server->xcb_conn);
        server->xcb_conn = nullptr;
    }
    if (server->ext_ws_global) {
        wl_global_destroy(server->ext_ws_global);
        server->ext_ws_global = nullptr;
    }
}
