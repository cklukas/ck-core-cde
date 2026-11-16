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
#define DTWM_MENU_ITEM_COUNT 7
#define DTWM_MENU_WIDTH 196
#define DTWM_MENU_ITEM_H 24
#define DTWM_MENU_PAD 4

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
    uint32_t workspace_mask;

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
    struct wl_list link;
    DtwmServer *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_rect *bg_rect;
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
    struct wlr_scene_tree *workspace_tree;
    struct wlr_scene_tree *unmanaged_xwayland_tree;
    struct wlr_scene_tree *layer_top_tree;
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

#endif /* DTWM_WAYLAND_H */
