/*
 * CDE - Common Desktop Environment
 * dtwm-wayland: Wayland compositor core with server-side CDE 3D bevel frames
 * and rootless XWayland integration.
 */

#ifndef DTWM_WAYLAND_H
#define DTWM_WAYLAND_H

#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE 1
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/xwayland/xwayland.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#define DTWM_BORDER_WIDTH 6
#define DTWM_TITLE_HEIGHT 24
#define DTWM_CORNER_HANDLE_LEN 24
#define DTWM_GRID_SIZE 16
#define DTWM_SNAP_THRESHOLD 24
#define DTWM_MAX_WORKSPACES 8
#define DTWM_MENU_ITEM_COUNT 8
#define DTWM_MENU_WIDTH 216
#define DTWM_MENU_ITEM_H 24
#define DTWM_MENU_PAD 4
#define DTWM_PANEL_WIDTH 840
#define DTWM_PANEL_HEIGHT 74
#define DTWM_PANEL_STRUT 76

typedef struct {
    double r, g, b, a;
} DtwmRgba;

typedef struct {
    DtwmRgba bg;
    DtwmRgba fg;
    DtwmRgba top_shadow;
    DtwmRgba bottom_shadow;
} DtwmColorSet;

typedef struct {
    DtwmColorSet active;
    DtwmColorSet inactive;
    DtwmColorSet menu;
    DtwmRgba desktop_bg;
} DtwmPalette;

typedef enum {
    DTWM_PART_NONE = 0,
    DTWM_PART_CLIENT,
    DTWM_PART_TITLE,
    DTWM_PART_BTN_MENU,
    DTWM_PART_BTN_MINIMIZE,
    DTWM_PART_BTN_MAXIMIZE,
    DTWM_PART_RESIZE_NW,
    DTWM_PART_RESIZE_N,
    DTWM_PART_RESIZE_NE,
    DTWM_PART_RESIZE_E,
    DTWM_PART_RESIZE_SE,
    DTWM_PART_RESIZE_S,
    DTWM_PART_RESIZE_SW,
    DTWM_PART_RESIZE_W,
} DtwmFramePart;

typedef enum {
    DTWM_SNAP_NONE = 0,
    DTWM_SNAP_LEFT,
    DTWM_SNAP_RIGHT,
    DTWM_SNAP_NW,
    DTWM_SNAP_NE,
    DTWM_SNAP_SW,
    DTWM_SNAP_SE,
} DtwmSnapZone;

typedef enum {
    DTWM_CURSOR_PASSTHROUGH = 0,
    DTWM_CURSOR_MOVE,
    DTWM_CURSOR_RESIZE,
} DtwmCursorMode;

typedef enum {
    DTWM_VIEW_XDG = 0,
    DTWM_VIEW_XWAYLAND = 1,
} DtwmViewKind;

typedef struct DtwmServer DtwmServer;
typedef struct DtwmView DtwmView;

struct DtwmView {
    struct wl_list link; /* DtwmServer.views */
    DtwmServer *server;
    DtwmViewKind kind;

    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_xwayland_surface *xwayland_surface;

    /* Scene nodes:
     * frame_tree sits at (x, y) in workspace coordinates.
     * frame_buf renders the server-side CDE 3D bevel border & titlebar.
     * content_tree sits at (left_inset, top_inset) inside frame_tree.
     */
    struct wlr_scene_tree *frame_tree;
    struct wlr_scene_buffer *frame_buf;
    struct wlr_scene_tree *content_tree;

    int x, y;
    int client_w, client_h;
    int saved_x, saved_y, saved_w, saved_h;
    bool has_saved_geom;

    bool mapped;
    bool active;
    bool maximized;
    bool minimized;
    bool is_desktop_pin;    /* dtfile Desktop.c pinned workspace object */
    bool is_swallowed;      /* Front Panel TYPE client swallowed window */
    uint32_t workspace_mask;

    /* Desktop icon state (open-window icon + minimized icon + live pixmap) */
    struct wlr_scene_buffer *desktop_icon_buf;
    cairo_surface_t *icon_surface;
    int icon_x, icon_y, icon_w, icon_h;
    int icon_box_w, icon_box_h;

    /* Decoration flags (honoring xdg-decoration + _MOTIF_WM_HINTS) */
    bool decorated;
    bool has_title;
    bool has_resize_handles;
    bool has_menu_btn;
    bool has_min_btn;
    bool has_max_btn;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_minimize;
    struct wl_listener request_activate;
    struct wl_listener request_close;
    struct wl_listener request_configure;
    struct wl_listener set_title;
    struct wl_listener set_decorations;
};

typedef struct {
    char id_name[32];        /* "ws0", "ws1", ... */
    char title[64];          /* "One", "Two", ... */
    char backdrop_name[512]; /* "SkyDark", "Pebbles", or explicit path */
    int color_set_id;
    int image_type;          /* 0=tiled, 1=center, 2=scaled */
    DtwmColorSet colors;
    xcb_atom_t x11_atom;
    xcb_atom_t info_atom;
    xcb_atom_t backdrop_atom;
} DtwmWorkspace;

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_rect *bg_rect;
    struct wlr_scene_buffer *backdrop_buf;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
} DtwmOutput;

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
} DtwmKeyboard;

struct DtwmServer {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_session *wlr_session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *output_layout;

    /* Scene Z-order layers */
    struct wlr_scene_tree *bg_tree;
    struct wlr_scene_tree *layer_bg_tree;
    struct wlr_scene_tree *layer_bottom_tree;
    struct wlr_scene_tree *desktop_icon_tree;
    struct wlr_scene_tree *workspace_tree;
    struct wlr_scene_tree *unmanaged_xwayland_tree;
    struct wlr_scene_tree *layer_top_tree;
    struct wlr_scene_tree *panel_swallow_tree;
    struct wlr_scene_tree *popup_tree;
    struct wlr_scene_tree *overlay_tree;
    struct wlr_scene_tree *layer_overlay_tree;

    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wlr_xwayland *xwayland;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wlr_seat *seat;
    struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
    struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;

    struct wl_list outputs;
    struct wl_list views;
    struct wl_list keyboards;

    DtwmPalette palette;
    DtwmView *focused_view;
    uint32_t current_workspace;
    uint32_t num_workspaces;
    uint32_t next_ws_serial;
    DtwmWorkspace workspaces[DTWM_MAX_WORKSPACES];

    /* Docked CDE Front Panel & TYPE client swallowing */
    bool show_front_panel;
    bool show_open_window_icons;
    bool large_icons;
    int panel_x, panel_y, panel_w, panel_h;
    int clock_slot_x, clock_slot_y, clock_slot_w, clock_slot_h;
    char swallow_client_name[64];
    DtwmView *swallowed_clock_view;
    struct wlr_scene_buffer *panel_buf;

    /* XWayland CDE Workspace Manager (_MOTIF_WM_INFO / _DT_WORKSPACE_*) XCB bridge */
    xcb_connection_t *xcb_conn;
    xcb_window_t xcb_root;
    xcb_window_t xcb_wm_win;
    struct wl_event_source *xcb_event_source;
    xcb_atom_t atom_mwm_info;
    xcb_atom_t atom_ws_list;
    xcb_atom_t atom_ws_current;
    xcb_atom_t atom_ws_hints;
    xcb_atom_t atom_ws_presence;
    xcb_atom_t atom_wm_request;
    xcb_atom_t atom_ws_all;
    xcb_atom_t atom_wm_settings_v1;
    xcb_atom_t atom_wm_state;
    xcb_atom_t atom_net_wm_icon_geom;

    /* Native Wayland ext-workspace-v1 & xdg-toplevel-icon-v1 protocols */
    struct wl_global *ext_ws_global;
    struct wl_list ext_ws_managers;
    struct wl_global *toplevel_icon_global;

    /* Interactive move / resize / snap state */
    DtwmCursorMode cursor_mode;
    DtwmView *grabbed_view;
    double grab_x, grab_y;
    struct wlr_box grab_orig_box;
    uint32_t resize_edges;
    DtwmSnapZone active_snap;
    struct wlr_scene_buffer *snap_preview_buf;

    /* Double-click titlebar detection */
    uint32_t last_title_click_ms;
    DtwmView *last_title_click_view;

    /* CDE Window Menu overlay */
    bool menu_visible;
    DtwmView *menu_target;
    int menu_x, menu_y;
    int menu_hover_idx;
    struct wlr_scene_buffer *menu_buf;

    /* CDE Keyboard Task Switcher overlay */
    bool switcher_visible;
    int switcher_index;
    struct wlr_scene_buffer *switcher_buf;

    /* Listeners */
    struct wl_listener new_output;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_xdg_decoration;
    struct wl_listener new_layer_surface;
    struct wl_listener new_xwayland_surface;
    struct wl_listener xwayland_ready;

    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wl_listener new_input;
    struct wl_listener new_virtual_pointer;
    struct wl_listener new_virtual_keyboard;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    struct wl_listener request_start_drag;

    struct wl_event_source *sigint_source;
    struct wl_event_source *sigterm_source;
    const char *wayland_socket;
    const char *status_file;
};

/* cde_bevel.c */
struct wlr_buffer *dtwm_cairo_buffer_begin(int width, int height, cairo_t **out_cr, cairo_surface_t **out_surf);
void dtwm_cairo_buffer_finish(cairo_t *cr, cairo_surface_t *surf);
void dtwm_draw_bevel(cairo_t *cr, double x, double y, double w, double h, double t, DtwmRgba tl_col, DtwmRgba br_col);
void dtwm_draw_label(cairo_t *cr, const char *text, double x, double y, double w, double h, DtwmRgba fg, bool centered, bool bold);
void dtwm_palette_init_default(DtwmPalette *pal);
void dtwm_view_insets(const DtwmView *view, int *left, int *top, int *right, int *bottom);
void dtwm_view_outer_size(const DtwmView *view, int *out_w, int *out_h);
const char *dtwm_view_title(const DtwmView *view);
void dtwm_view_update_frame(DtwmView *view);
DtwmFramePart dtwm_frame_hit_test(const DtwmView *view, int local_x, int local_y);
uint32_t dtwm_part_to_wlr_edges(DtwmFramePart part);

struct wlr_buffer *dtwm_render_window_menu(const DtwmPalette *pal, const DtwmView *view, int hover_idx, int *out_w, int *out_h);
struct wlr_buffer *dtwm_render_snap_preview(const DtwmPalette *pal, int w, int h);
struct wlr_buffer *dtwm_render_task_switcher(DtwmServer *server, int *out_w, int *out_h);

/* cde_workspace.c */
bool dtwm_view_visible_on_current_ws(const DtwmView *view);
void dtwm_workspaces_init(DtwmServer *server);
void dtwm_workspaces_finish(DtwmServer *server);
void dtwm_workspaces_on_xwayland_ready(DtwmServer *server);
void dtwm_workspaces_on_xwayland_view_map(DtwmView *view);
void dtwm_view_set_workspace_mask(DtwmView *view, uint32_t mask);
void dtwm_switch_workspace(DtwmServer *server, uint32_t ws_idx);
int dtwm_add_workspace(DtwmServer *server, const char *title);
bool dtwm_delete_workspace(DtwmServer *server, uint32_t ws_idx);
void dtwm_rename_workspace(DtwmServer *server, uint32_t ws_idx, const char *title);
void dtwm_set_workspace_backdrop(DtwmServer *server, uint32_t ws_idx, const char *path, int image_type);
void dtwm_update_output_backdrop(DtwmOutput *output);
void dtwm_update_all_backdrops(DtwmServer *server);
void dtwm_update_front_panel(DtwmServer *server);
bool dtwm_front_panel_click(DtwmServer *server, double lx, double ly, uint32_t button);
void dtwm_get_usable_output_box(DtwmServer *server, double ref_x, double ref_y, struct wlr_box *out_box);

/* cde_icons.c */
void dtwm_icons_init(DtwmServer *server);
void dtwm_icons_finish(DtwmServer *server);
void dtwm_icons_on_xwayland_ready(DtwmServer *server);
bool dtwm_icons_classify_xwayland_map(DtwmView *view);
void dtwm_icons_on_view_map(DtwmView *view);
void dtwm_icons_on_view_unmap_or_destroy(DtwmView *view, bool destroying);
void dtwm_icons_on_xcb_property(DtwmServer *server, xcb_window_t win, xcb_atom_t atom);
void dtwm_icons_set_wm_state(DtwmView *view);
void dtwm_icons_relayout(DtwmServer *server);
void dtwm_icons_update_view(DtwmView *view);
DtwmView *dtwm_icons_view_at(DtwmServer *server, double lx, double ly);
void dtwm_icons_set_open_window_icons(DtwmServer *server, bool enabled);
void dtwm_icons_set_large_icons(DtwmServer *server, bool large);
void dtwm_icons_sync_xwayland_pointer(DtwmServer *server, double lx, double ly, struct wlr_surface *focused_surf);
void dtwm_icons_reposition_swallowed(DtwmServer *server);

#endif /* DTWM_WAYLAND_H */
