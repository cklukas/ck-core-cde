/*
 * CDE - Common Desktop Environment
 * cde_bevel.c: Server-side CDE 3D mitered bevel window frames, hit-testing,
 * window menu, snap feedback preview, and keyboard task switcher.
 */

#include "dtwm_wayland.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    struct wlr_buffer base;
    cairo_surface_t *surface;
    void *data;
    size_t stride;
} DtwmCairoBuffer;

static void cairo_buf_destroy(struct wlr_buffer *wlr_buffer) {
    DtwmCairoBuffer *buf = wl_container_of(wlr_buffer, buf, base);
    if (buf->surface) {
        cairo_surface_destroy(buf->surface);
    }
    free(buf);
}

static bool cairo_buf_begin_data_ptr_access(
    struct wlr_buffer *wlr_buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride
) {
    (void)flags;
    DtwmCairoBuffer *buf = wl_container_of(wlr_buffer, buf, base);
    *data = buf->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = buf->stride;
    return true;
}

static void cairo_buf_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    (void)wlr_buffer;
}

static const struct wlr_buffer_impl cairo_buf_impl = {
    .destroy = cairo_buf_destroy,
    .begin_data_ptr_access = cairo_buf_begin_data_ptr_access,
    .end_data_ptr_access = cairo_buf_end_data_ptr_access,
};

static DtwmCairoBuffer *dtwm_cairo_buffer_create(int width, int height, cairo_t **out_cr) {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    DtwmCairoBuffer *buf = calloc(1, sizeof(*buf));
    if (!buf) {
        return nullptr;
    }
    buf->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(buf->surface);
        free(buf);
        return nullptr;
    }
    buf->data = cairo_image_surface_get_data(buf->surface);
    buf->stride = (size_t)cairo_image_surface_get_stride(buf->surface);
    wlr_buffer_init(&buf->base, &cairo_buf_impl, width, height);
    *out_cr = cairo_create(buf->surface);
    return buf;
}

struct wlr_buffer *dtwm_cairo_buffer_begin(
    int width, int height, cairo_t **out_cr, cairo_surface_t **out_surf
) {
    DtwmCairoBuffer *buf = dtwm_cairo_buffer_create(width, height, out_cr);
    if (!buf) {
        return nullptr;
    }
    if (out_surf) {
        *out_surf = buf->surface;
    }
    return &buf->base;
}

void dtwm_cairo_buffer_finish(cairo_t *cr, cairo_surface_t *surf) {
    if (surf) {
        cairo_surface_flush(surf);
    }
    if (cr) {
        cairo_destroy(cr);
    }
}

static inline void set_rgba(cairo_t *cr, DtwmRgba c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

/* Mitered 3D Motif/CDE bevel matching WmGraphics.c BevelRectangle() */
static void draw_bevel(
    cairo_t *cr, double x, double y, double w, double h, double t, DtwmRgba tl_col, DtwmRgba br_col
) {
    if (w <= 0.0 || h <= 0.0 || t <= 0.0) {
        return;
    }
    /* Top + Left mitered shadow */
    set_rgba(cr, tl_col);
    cairo_new_path(cr);
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + w, y);
    cairo_line_to(cr, x + w - t, y + t);
    cairo_line_to(cr, x + t, y + t);
    cairo_line_to(cr, x + t, y + h - t);
    cairo_line_to(cr, x, y + h);
    cairo_close_path(cr);
    cairo_fill(cr);

    /* Bottom + Right mitered shadow */
    set_rgba(cr, br_col);
    cairo_new_path(cr);
    cairo_move_to(cr, x + w, y);
    cairo_line_to(cr, x + w, y + h);
    cairo_line_to(cr, x, y + h);
    cairo_line_to(cr, x + t, y + h - t);
    cairo_line_to(cr, x + w - t, y + h - t);
    cairo_line_to(cr, x + w - t, y + t);
    cairo_close_path(cr);
    cairo_fill(cr);
}

void dtwm_draw_bevel(
    cairo_t *cr, double x, double y, double w, double h, double t, DtwmRgba tl_col, DtwmRgba br_col
) {
    draw_bevel(cr, x, y, w, h, t, tl_col, br_col);
}

static void draw_groove_line_v(
    cairo_t *cr, double x, double y1, double y2, DtwmRgba shadow, DtwmRgba hilite
) {
    set_rgba(cr, shadow);
    cairo_rectangle(cr, x, y1, 1.0, y2 - y1);
    cairo_fill(cr);
    set_rgba(cr, hilite);
    cairo_rectangle(cr, x + 1.0, y1, 1.0, y2 - y1);
    cairo_fill(cr);
}

static void draw_groove_line_h(
    cairo_t *cr, double x1, double x2, double y, DtwmRgba shadow, DtwmRgba hilite
) {
    set_rgba(cr, shadow);
    cairo_rectangle(cr, x1, y, x2 - x1, 1.0);
    cairo_fill(cr);
    set_rgba(cr, hilite);
    cairo_rectangle(cr, x1, y + 1.0, x2 - x1, 1.0);
    cairo_fill(cr);
}

DtwmAppearanceProfile dtwm_parse_appearance_profile(const char *name) {
    if (!name || !name[0]) return DTWM_PROFILE_CLASSIC;
    while (*name == ' ' || *name == '\t' || *name == '"' || *name == '\'') name++;
    if (strncasecmp(name, "modern_light", 12) == 0 || strncasecmp(name, "modern-light", 12) == 0 ||
        strncasecmp(name, "ModernLight", 11) == 0) {
        return DTWM_PROFILE_MODERN_LIGHT;
    }
    if (strncasecmp(name, "modern_dark", 11) == 0 || strncasecmp(name, "modern-dark", 11) == 0 ||
        strncasecmp(name, "ModernDark", 10) == 0) {
        return DTWM_PROFILE_MODERN_DARK;
    }
    if (strncasecmp(name, "high_contrast", 13) == 0 || strncasecmp(name, "high-contrast", 13) == 0 ||
        strncasecmp(name, "HighContrast", 12) == 0) {
        return DTWM_PROFILE_HIGH_CONTRAST;
    }
    return DTWM_PROFILE_CLASSIC;
}

void dtwm_palette_init_profile(DtwmPalette *pal, DtwmAppearanceProfile profile) {
    pal->profile = profile;
    switch (profile) {
    case DTWM_PROFILE_MODERN_LIGHT:
        /* Matches cde/programs/palettes/ModernLight.dp (#5b84b8 / #9aa3b0 / #c8ccd4) */
        pal->active = (DtwmColorSet){
            .bg = {0.357, 0.518, 0.722, 1.0},            /* #5B84B8 */
            .fg = {1.000, 1.000, 1.000, 1.0},
            .top_shadow = {0.612, 0.729, 0.875, 1.0},
            .bottom_shadow = {0.188, 0.286, 0.420, 1.0},
        };
        pal->inactive = (DtwmColorSet){
            .bg = {0.604, 0.639, 0.690, 1.0},            /* #9AA3B0 */
            .fg = {0.106, 0.133, 0.173, 1.0},
            .top_shadow = {0.824, 0.847, 0.882, 1.0},
            .bottom_shadow = {0.337, 0.373, 0.424, 1.0},
        };
        pal->menu = (DtwmColorSet){
            .bg = {0.784, 0.800, 0.831, 1.0},            /* #C8CCD4 */
            .fg = {0.090, 0.114, 0.149, 1.0},
            .top_shadow = {0.925, 0.937, 0.957, 1.0},
            .bottom_shadow = {0.435, 0.463, 0.514, 1.0},
        };
        pal->desktop_bg = (DtwmRgba){0.494, 0.557, 0.643, 1.0}; /* #7E8EA4 */
        return;

    case DTWM_PROFILE_MODERN_DARK:
        /* Matches cde/programs/palettes/ModernDark.dp (#283952 / #25272b / #1b222c) */
        pal->active = (DtwmColorSet){
            .bg = {0.157, 0.224, 0.322, 1.0},            /* #283952 */
            .fg = {0.949, 0.965, 0.988, 1.0},
            .top_shadow = {0.310, 0.404, 0.537, 1.0},
            .bottom_shadow = {0.075, 0.110, 0.165, 1.0},
        };
        pal->inactive = (DtwmColorSet){
            .bg = {0.145, 0.153, 0.169, 1.0},            /* #25272B */
            .fg = {0.847, 0.867, 0.898, 1.0},
            .top_shadow = {0.278, 0.294, 0.325, 1.0},
            .bottom_shadow = {0.067, 0.071, 0.082, 1.0},
        };
        pal->menu = (DtwmColorSet){
            .bg = {0.125, 0.129, 0.141, 1.0},            /* #202124 */
            .fg = {0.910, 0.925, 0.949, 1.0},
            .top_shadow = {0.267, 0.278, 0.306, 1.0},
            .bottom_shadow = {0.055, 0.059, 0.067, 1.0},
        };
        pal->desktop_bg = (DtwmRgba){0.106, 0.133, 0.173, 1.0}; /* #1B222C */
        return;

    case DTWM_PROFILE_HIGH_CONTRAST:
        /* Matches cde/programs/palettes/HighContrast.dp (#ffff00 active / #000000 surfaces) */
        pal->active = (DtwmColorSet){
            .bg = {1.000, 1.000, 0.000, 1.0},            /* #FFFF00 */
            .fg = {0.000, 0.000, 0.000, 1.0},            /* #000000 */
            .top_shadow = {1.000, 1.000, 1.000, 1.0},
            .bottom_shadow = {0.600, 0.600, 0.000, 1.0},
        };
        pal->inactive = (DtwmColorSet){
            .bg = {0.000, 0.000, 0.000, 1.0},            /* #000000 */
            .fg = {1.000, 1.000, 1.000, 1.0},            /* #FFFFFF */
            .top_shadow = {1.000, 1.000, 1.000, 1.0},
            .bottom_shadow = {0.800, 0.800, 0.800, 1.0},
        };
        pal->menu = pal->inactive;
        pal->desktop_bg = (DtwmRgba){0.000, 0.000, 0.000, 1.0};
        return;

    case DTWM_PROFILE_CLASSIC:
    default:
        /* Classic CDE Default Crimson active window frame + Slate inactive frame */
        pal->active = (DtwmColorSet){
            .bg = {0.698, 0.302, 0.478, 1.0},            /* #B24D7A */
            .fg = {1.000, 1.000, 1.000, 1.0},            /* #FFFFFF */
            .top_shadow = {0.886, 0.608, 0.745, 1.0},    /* #E29BBE */
            .bottom_shadow = {0.357, 0.137, 0.235, 1.0}, /* #5B233C */
        };
        pal->inactive = (DtwmColorSet){
            .bg = {0.682, 0.698, 0.765, 1.0},            /* #AEB2C3 */
            .fg = {0.090, 0.100, 0.130, 1.0},            /* #171A21 */
            .top_shadow = {0.878, 0.890, 0.933, 1.0},    /* #E0E3EE */
            .bottom_shadow = {0.345, 0.357, 0.408, 1.0}, /* #585B68 */
        };
        pal->menu = (DtwmColorSet){
            .bg = {0.682, 0.698, 0.765, 1.0},
            .fg = {0.070, 0.080, 0.110, 1.0},
            .top_shadow = {0.878, 0.890, 0.933, 1.0},
            .bottom_shadow = {0.345, 0.357, 0.408, 1.0},
        };
        /* Classic CDE Teal/Slate backdrop */
        pal->desktop_bg = (DtwmRgba){0.235, 0.388, 0.463, 1.0}; /* #3C6376 */
        return;
    }
}

void dtwm_palette_init_default(DtwmPalette *pal) {
    dtwm_palette_init_profile(pal, DTWM_PROFILE_CLASSIC);
}

void dtwm_view_insets(const DtwmView *view, int *left, int *top, int *right, int *bottom) {
    if (!view->decorated || view->maximized) {
        if (!view->decorated) {
            *left = *top = *right = *bottom = 0;
            return;
        }
    }
    int b = view->decorated ? (view->is_panel ? 4 : DTWM_BORDER_WIDTH) : 0;
    int t = (view->decorated && view->has_title) ? DTWM_TITLE_HEIGHT : 0;
    *left = b;
    *right = b;
    *bottom = b;
    *top = b + t;
}

void dtwm_view_outer_size(const DtwmView *view, int *out_w, int *out_h) {
    int l, t, r, b;
    dtwm_view_insets(view, &l, &t, &r, &b);
    *out_w = view->client_w + l + r;
    *out_h = view->client_h + t + b;
}

const char *dtwm_view_title(const DtwmView *view) {
    const char *title = nullptr;
    if (view->kind == DTWM_VIEW_XDG && view->xdg_toplevel) {
        title = view->xdg_toplevel->title;
        if (!title || !title[0]) {
            title = view->xdg_toplevel->app_id;
        }
    } else if (view->kind == DTWM_VIEW_XWAYLAND && view->xwayland_surface) {
        title = view->xwayland_surface->title;
        if (!title || !title[0]) {
            title = view->xwayland_surface->class;
        }
    }
    return (title && title[0]) ? title : "Untitled";
}

static void draw_pango_label(
    cairo_t *cr, const char *text, double x, double y, double w, double h, DtwmRgba fg, bool centered, bool bold
) {
    if (w <= 4.0 || h <= 4.0) {
        return;
    }
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(
        bold ? "Sans Bold 9.5" : "Sans 9.5"
    );
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text ? text : "", -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, (int)((w - 8.0) * PANGO_SCALE));

    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    double tx = centered ? (x + (w - tw) * 0.5) : (x + 6.0);
    if (tx < x + 4.0) {
        tx = x + 4.0;
    }
    double ty = y + (h - th) * 0.5;

    set_rgba(cr, fg);
    cairo_move_to(cr, floor(tx), floor(ty));
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

void dtwm_draw_label(
    cairo_t *cr, const char *text, double x, double y, double w, double h, DtwmRgba fg, bool centered, bool bold
) {
    draw_pango_label(cr, text, x, y, w, h, fg, centered, bold);
}

void dtwm_view_update_frame(DtwmView *view) {
    if (!view || !view->frame_buf) {
        return;
    }
    if (!view->decorated || view->client_w <= 0 || view->client_h <= 0) {
        wlr_scene_buffer_set_buffer(view->frame_buf, nullptr);
        wlr_scene_node_set_position(&view->content_tree->node, 0, 0);
        return;
    }

    int left, top, right, bottom;
    dtwm_view_insets(view, &left, &top, &right, &bottom);
    wlr_scene_node_set_position(&view->content_tree->node, left, top);

    int outer_w = view->client_w + left + right;
    int outer_h = view->client_h + top + bottom;

    cairo_t *cr = nullptr;
    DtwmCairoBuffer *buf = dtwm_cairo_buffer_create(outer_w, outer_h, &cr);
    if (!buf) {
        return;
    }

    static const DtwmColorSet classic_panel_border = {
        .bg = {0.929, 0.659, 0.439, 1.0},            /* Default.dp #EDA870 */
        .fg = {1.000, 1.000, 1.000, 1.0},
        .top_shadow = {0.976, 0.843, 0.725, 1.0},
        .bottom_shadow = {0.490, 0.345, 0.227, 1.0},
    };
    const DtwmColorSet *cs = view->active ? &view->server->palette.active : &view->server->palette.inactive;
    if (view->is_panel) {
        cs = (view->server->palette.profile == DTWM_PROFILE_CLASSIC)
                 ? &classic_panel_border
                 : &view->server->palette.inactive;
    }

    /* 1. Fill frame background */
    set_rgba(cr, cs->bg);
    cairo_rectangle(cr, 0, 0, outer_w, outer_h);
    cairo_fill(cr);

    /* 2. Outer raised 2px bevel */
    draw_bevel(cr, 0, 0, outer_w, outer_h, 2.0, cs->top_shadow, cs->bottom_shadow);

    /* 3. Inner sunken 1px bevel framing the titlebar + client area */
    int b_in = left > 1 ? (left - 1) : 1;
    draw_bevel(
        cr,
        b_in,
        b_in,
        outer_w - 2 * b_in,
        outer_h - 2 * b_in,
        1.0,
        cs->bottom_shadow,
        cs->top_shadow
    );

    /* 4. Iconic CDE/MWM corner resize handle notches */
    if (view->has_resize_handles) {
        int c = DTWM_CORNER_HANDLE_LEN;
        if (outer_w > 2 * c + 16) {
            draw_groove_line_v(cr, c, 1, DTWM_BORDER_WIDTH - 1, cs->bottom_shadow, cs->top_shadow);
            draw_groove_line_v(cr, outer_w - c - 2, 1, DTWM_BORDER_WIDTH - 1, cs->bottom_shadow, cs->top_shadow);
            draw_groove_line_v(cr, c, outer_h - DTWM_BORDER_WIDTH + 1, outer_h - 1, cs->bottom_shadow, cs->top_shadow);
            draw_groove_line_v(cr, outer_w - c - 2, outer_h - DTWM_BORDER_WIDTH + 1, outer_h - 1, cs->bottom_shadow, cs->top_shadow);
        }
        if (outer_h > 2 * c + 16) {
            draw_groove_line_h(cr, 1, DTWM_BORDER_WIDTH - 1, c, cs->bottom_shadow, cs->top_shadow);
            draw_groove_line_h(cr, outer_w - DTWM_BORDER_WIDTH + 1, outer_w - 1, c, cs->bottom_shadow, cs->top_shadow);
            draw_groove_line_h(cr, 1, DTWM_BORDER_WIDTH - 1, outer_h - c - 2, cs->bottom_shadow, cs->top_shadow);
            draw_groove_line_h(cr, outer_w - DTWM_BORDER_WIDTH + 1, outer_w - 1, outer_h - c - 2, cs->bottom_shadow, cs->top_shadow);
        }
    }

    /* 5. Titlebar and MWM buttons */
    if (view->has_title) {
        double tx = DTWM_BORDER_WIDTH;
        double ty = DTWM_BORDER_WIDTH;
        double tw = view->client_w;
        double th = DTWM_TITLE_HEIGHT;
        double btn_w = DTWM_TITLE_HEIGHT;

        double title_x = tx;
        double title_w = tw;

        /* Left Window Menu Button [ - ] */
        if (view->has_menu_btn && title_w >= btn_w * 2) {
            draw_bevel(cr, tx, ty, btn_w, th, 1.0, cs->top_shadow, cs->bottom_shadow);
            /* Recessed horizontal bar glyph */
            double gx = tx + 5.0;
            double gy = ty + floor((th - 4.0) * 0.5);
            double gw = btn_w - 10.0;
            draw_bevel(cr, gx, gy, gw, 4.0, 1.0, cs->top_shadow, cs->bottom_shadow);
            title_x += btn_w;
            title_w -= btn_w;
        }

        /* Right Maximize Button [ □ ] */
        double right_edge = tx + tw;
        if (view->has_max_btn && title_w >= btn_w * 2) {
            right_edge -= btn_w;
            title_w -= btn_w;
            if (view->maximized) {
                draw_bevel(cr, right_edge, ty, btn_w, th, 1.0, cs->bottom_shadow, cs->top_shadow);
            } else {
                draw_bevel(cr, right_edge, ty, btn_w, th, 1.0, cs->top_shadow, cs->bottom_shadow);
            }
            double sq = 10.0;
            double sx = right_edge + floor((btn_w - sq) * 0.5);
            double sy = ty + floor((th - sq) * 0.5);
            draw_bevel(cr, sx, sy, sq, sq, 1.0, cs->top_shadow, cs->bottom_shadow);
        }

        /* Minimize Button [ . ] */
        if (view->has_min_btn && title_w >= btn_w * 1.5) {
            right_edge -= btn_w;
            title_w -= btn_w;
            draw_bevel(cr, right_edge, ty, btn_w, th, 1.0, cs->top_shadow, cs->bottom_shadow);
            double sq = 5.0;
            double sx = right_edge + floor((btn_w - sq) * 0.5);
            double sy = ty + floor((th - sq) * 0.5);
            draw_bevel(cr, sx, sy, sq, sq, 1.0, cs->top_shadow, cs->bottom_shadow);
        }

        /* Titlebar slab + centered window title */
        if (title_w > 4.0) {
            draw_bevel(cr, title_x, ty, title_w, th, 1.0, cs->top_shadow, cs->bottom_shadow);
            draw_pango_label(cr, dtwm_view_title(view), title_x, ty, title_w, th, cs->fg, true, true);
        }
    }

    /* 6. Cut out transparent client area so client surface shows cleanly underneath/within */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, left, top, view->client_w, view->client_h);
    cairo_fill(cr);
    cairo_restore(cr);

    cairo_surface_flush(buf->surface);
    cairo_destroy(cr);

    wlr_scene_buffer_set_buffer(view->frame_buf, &buf->base);
    wlr_buffer_drop(&buf->base);
}

DtwmFramePart dtwm_frame_hit_test(const DtwmView *view, int lx, int ly) {
    int left, top, right, bottom;
    dtwm_view_insets(view, &left, &top, &right, &bottom);
    int ow = view->client_w + left + right;
    int oh = view->client_h + top + bottom;

    if (lx < 0 || ly < 0 || lx >= ow || ly >= oh) {
        return DTWM_PART_NONE;
    }
    if (!view->decorated) {
        return DTWM_PART_CLIENT;
    }
    if (lx >= left && lx < left + view->client_w && ly >= top && ly < top + view->client_h) {
        return DTWM_PART_CLIENT;
    }

    /* Outer border & corner resize handles */
    int b = DTWM_BORDER_WIDTH;
    int c = DTWM_CORNER_HANDLE_LEN;
    bool in_outer_border = (lx < b || lx >= ow - b || ly < b || ly >= oh - b);
    if (in_outer_border) {
        if (!view->has_resize_handles) {
            return DTWM_PART_TITLE;
        }
        bool near_l = lx < c;
        bool near_r = lx >= ow - c;
        bool near_t = ly < c;
        bool near_b = ly >= oh - c;
        if (near_t && near_l) return DTWM_PART_RESIZE_NW;
        if (near_t && near_r) return DTWM_PART_RESIZE_NE;
        if (near_b && near_l) return DTWM_PART_RESIZE_SW;
        if (near_b && near_r) return DTWM_PART_RESIZE_SE;
        if (ly < b) return DTWM_PART_RESIZE_N;
        if (ly >= oh - b) return DTWM_PART_RESIZE_S;
        if (lx < b) return DTWM_PART_RESIZE_W;
        return DTWM_PART_RESIZE_E;
    }

    /* Titlebar region */
    if (view->has_title && ly >= b && ly < b + DTWM_TITLE_HEIGHT) {
        int tx = b;
        int tw = view->client_w;
        int btn_w = DTWM_TITLE_HEIGHT;
        int rem_w = tw;
        if (view->has_menu_btn && rem_w >= btn_w * 2) {
            if (lx >= tx && lx < tx + btn_w) {
                return DTWM_PART_BTN_MENU;
            }
            rem_w -= btn_w;
        }
        int right_edge = b + tw;
        if (view->has_max_btn && rem_w >= btn_w * 2) {
            right_edge -= btn_w;
            rem_w -= btn_w;
            if (lx >= right_edge && lx < right_edge + btn_w) {
                return DTWM_PART_BTN_MAXIMIZE;
            }
        }
        if (view->has_min_btn && rem_w >= (btn_w * 3) / 2) {
            right_edge -= btn_w;
            if (lx >= right_edge && lx < right_edge + btn_w) {
                return DTWM_PART_BTN_MINIMIZE;
            }
        }
        return DTWM_PART_TITLE;
    }

    return DTWM_PART_CLIENT;
}

uint32_t dtwm_part_to_wlr_edges(DtwmFramePart part) {
    switch (part) {
    case DTWM_PART_RESIZE_NW: return WLR_EDGE_TOP | WLR_EDGE_LEFT;
    case DTWM_PART_RESIZE_N:  return WLR_EDGE_TOP;
    case DTWM_PART_RESIZE_NE: return WLR_EDGE_TOP | WLR_EDGE_RIGHT;
    case DTWM_PART_RESIZE_E:  return WLR_EDGE_RIGHT;
    case DTWM_PART_RESIZE_SE: return WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
    case DTWM_PART_RESIZE_S:  return WLR_EDGE_BOTTOM;
    case DTWM_PART_RESIZE_SW: return WLR_EDGE_BOTTOM | WLR_EDGE_LEFT;
    case DTWM_PART_RESIZE_W:  return WLR_EDGE_LEFT;
    default:                  return WLR_EDGE_NONE;
    }
}

struct wlr_buffer *dtwm_render_window_menu(
    const DtwmPalette *pal, const DtwmView *view, int hover_idx, int *out_w, int *out_h
) {
    uint32_t all_mask = 0xFu;
    if (view && view->server && view->server->num_workspaces > 0) {
        all_mask = (1u << view->server->num_workspaces) - 1u;
    }
    bool occupies_all = view && ((view->workspace_mask & all_mask) == all_mask);
    const char *items[DTWM_MENU_ITEM_COUNT] = {
        "Restore",
        "Move",
        "Size",
        "Minimize",
        "Maximize",
        "Lower",
        occupies_all ? "Unoccupy Other Workspaces" : "Occupy All Workspaces",
        "Close          Alt+F4",
    };
    int w = DTWM_MENU_WIDTH;
    int h = DTWM_MENU_PAD * 2 + DTWM_MENU_ITEM_COUNT * DTWM_MENU_ITEM_H + 6;
    *out_w = w;
    *out_h = h;

    cairo_t *cr = nullptr;
    DtwmCairoBuffer *buf = dtwm_cairo_buffer_create(w, h, &cr);
    if (!buf) {
        return nullptr;
    }

    const DtwmColorSet *cs = &pal->menu;
    set_rgba(cr, cs->bg);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    draw_bevel(cr, 0, 0, w, h, 2.0, cs->top_shadow, cs->bottom_shadow);

    double y = DTWM_MENU_PAD;
    for (int i = 0; i < DTWM_MENU_ITEM_COUNT; ++i) {
        if (i == DTWM_MENU_ITEM_COUNT - 1) {
            draw_groove_line_h(cr, 4, w - 4, y + 2, cs->bottom_shadow, cs->top_shadow);
            y += 6;
        }
        if (i == hover_idx) {
            draw_bevel(cr, 3, y, w - 6, DTWM_MENU_ITEM_H, 2.0, cs->top_shadow, cs->bottom_shadow);
        }
        bool enabled = true;
        if (view) {
            if (i == 0 && !view->maximized) enabled = false;
            if (i == 4 && view->maximized) enabled = false;
        }
        DtwmRgba fg = enabled ? cs->fg : cs->bottom_shadow;
        draw_pango_label(cr, items[i], 8, y, w - 16, DTWM_MENU_ITEM_H, fg, false, false);
        y += DTWM_MENU_ITEM_H;
    }

    cairo_surface_flush(buf->surface);
    cairo_destroy(cr);
    return &buf->base;
}

struct wlr_buffer *dtwm_render_snap_preview(const DtwmPalette *pal, int w, int h) {
    cairo_t *cr = nullptr;
    DtwmCairoBuffer *buf = dtwm_cairo_buffer_create(w, h, &cr);
    if (!buf) {
        return nullptr;
    }
    DtwmRgba fill = {pal->active.bg.r, pal->active.bg.g, pal->active.bg.b, 0.28};
    set_rgba(cr, fill);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    draw_bevel(cr, 0, 0, w, h, 3.0, pal->active.top_shadow, pal->active.bottom_shadow);
    cairo_surface_flush(buf->surface);
    cairo_destroy(cr);
    return &buf->base;
}

struct wlr_buffer *dtwm_render_wireframe_outline(int w, int h) {
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;

    cairo_t *cr = nullptr;
    DtwmCairoBuffer *buf = dtwm_cairo_buffer_create(w, h, &cr);
    if (!buf) {
        return nullptr;
    }

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    /* Classic CDE / Motif 3x3 rubberband outline (WmFeedback.c DrawOutline),
     * rendered with high-contrast dark halo + bright core so it remains crisp
     * over both dark and light application surfaces without X11 GXxor. */
    double x1 = floor((double)w / 3.0) + 0.5;
    double x2 = floor((2.0 * (double)w) / 3.0) + 0.5;
    double y1 = floor((double)h / 3.0) + 0.5;
    double y2 = floor((2.0 * (double)h) / 3.0) + 0.5;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

    /* Dark outer/inner contrast backing */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.88);
    cairo_set_line_width(cr, 3.0);
    cairo_rectangle(cr, 1.5, 1.5, (double)w - 3.0, (double)h - 3.0);
    cairo_stroke(cr);

    if (w >= 48 && h >= 48) {
        cairo_set_line_width(cr, 3.0);
        cairo_move_to(cr, x1, 2.0);
        cairo_line_to(cr, x1, (double)h - 2.0);
        cairo_move_to(cr, x2, 2.0);
        cairo_line_to(cr, x2, (double)h - 2.0);
        cairo_move_to(cr, 2.0, y1);
        cairo_line_to(cr, (double)w - 2.0, y1);
        cairo_move_to(cr, 2.0, y2);
        cairo_line_to(cr, (double)w - 2.0, y2);
        cairo_stroke(cr);
    }

    /* Bright white/silver core lines */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 1.5, 1.5, (double)w - 3.0, (double)h - 3.0);
    cairo_stroke(cr);

    if (w >= 48 && h >= 48) {
        cairo_move_to(cr, x1, 2.0);
        cairo_line_to(cr, x1, (double)h - 2.0);
        cairo_move_to(cr, x2, 2.0);
        cairo_line_to(cr, x2, (double)h - 2.0);
        cairo_move_to(cr, 2.0, y1);
        cairo_line_to(cr, (double)w - 2.0, y1);
        cairo_move_to(cr, 2.0, y2);
        cairo_line_to(cr, (double)w - 2.0, y2);
        cairo_stroke(cr);
    }

    cairo_surface_flush(buf->surface);
    cairo_destroy(cr);
    return &buf->base;
}

struct wlr_buffer *dtwm_render_task_switcher(DtwmServer *server, int *out_w, int *out_h) {
    int count = 0;
    DtwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (dtwm_view_visible_on_current_ws(v) && !v->is_swallowed && !v->is_desktop_pin &&
            !v->is_panel && !v->is_subpanel) {
            count++;
        }
    }
    if (count == 0) {
        return nullptr;
    }
    int w = 340;
    int row_h = 28;
    int h = 38 + count * row_h + 10;
    *out_w = w;
    *out_h = h;

    cairo_t *cr = nullptr;
    DtwmCairoBuffer *buf = dtwm_cairo_buffer_create(w, h, &cr);
    if (!buf) {
        return nullptr;
    }

    const DtwmColorSet *cs = &server->palette.menu;
    set_rgba(cr, cs->bg);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    draw_bevel(cr, 0, 0, w, h, 3.0, cs->top_shadow, cs->bottom_shadow);

    /* Header bar in active CDE Crimson */
    set_rgba(cr, server->palette.active.bg);
    cairo_rectangle(cr, 4, 4, w - 8, 26);
    cairo_fill(cr);
    draw_bevel(cr, 4, 4, w - 8, 26, 1.0, server->palette.active.top_shadow, server->palette.active.bottom_shadow);
    draw_pango_label(cr, "Switch Window", 8, 4, w - 16, 26, server->palette.active.fg, true, true);

    int idx = 0;
    double y = 36;
    wl_list_for_each(v, &server->views, link) {
        if (!dtwm_view_visible_on_current_ws(v) || v->is_swallowed || v->is_desktop_pin ||
            v->is_panel || v->is_subpanel) {
            continue;
        }
        bool sel = (idx == (server->switcher_index % count));
        if (sel) {
            set_rgba(cr, server->palette.active.bg);
            cairo_rectangle(cr, 8, y, w - 16, row_h - 2);
            cairo_fill(cr);
            draw_bevel(cr, 8, y, w - 16, row_h - 2, 2.0, server->palette.active.top_shadow, server->palette.active.bottom_shadow);
            draw_pango_label(cr, dtwm_view_title(v), 14, y, w - 28, row_h - 2, server->palette.active.fg, false, true);
        } else {
            draw_pango_label(cr, dtwm_view_title(v), 14, y, w - 28, row_h - 2, cs->fg, false, false);
        }
        y += row_h;
        idx++;
    }

    cairo_surface_flush(buf->surface);
    cairo_destroy(cr);
    return &buf->base;
}
