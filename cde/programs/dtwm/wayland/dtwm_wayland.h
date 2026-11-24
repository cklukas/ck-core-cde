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
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#if defined(__has_include)
#if __has_include(<GLES2/gl2.h>)
#include <wlr/render/gles2.h>
#define DTWM_HAVE_WLR_GLES2_H 1
#endif
#if __has_include(<vulkan/vulkan_core.h>)
#include <wlr/render/vulkan.h>
#define DTWM_HAVE_WLR_VULKAN_H 1
#endif
#endif
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/region.h>
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
#define DTWM_ROOT_MENU_ITEM_COUNT 13
#define DTWM_ROOT_MENU_WIDTH 236
#define DTWM_ROOT_MENU_HEADER_H 26
#define DTWM_PANEL_WIDTH 840
#define DTWM_PANEL_HEIGHT 74
#define DTWM_PANEL_STRUT 76
#define DTWM_MAX_NATIVE_DPI_PIDS 256

typedef struct {
    int dpi;
    double scale;
    int border_w;
    int panel_border_w;
    int title_h;
    int corner_len;
    int outer_bevel;
    int inner_bevel;
    int menu_w;
    int root_menu_w;
    int menu_item_h;
    int root_header_h;
    int menu_pad;
    int menu_sep_h;
    int grid_sz;
    int snap_thresh;
} DtwmMetrics;

typedef struct {
    double r, g, b, a;
} DtwmRgba;

typedef struct {
    DtwmRgba bg;
    DtwmRgba fg;
    DtwmRgba top_shadow;
    DtwmRgba bottom_shadow;
} DtwmColorSet;

typedef enum {
    DTWM_PROFILE_CLASSIC = 0,
    DTWM_PROFILE_MODERN_LIGHT = 1,
    DTWM_PROFILE_MODERN_DARK = 2,
    DTWM_PROFILE_HIGH_CONTRAST = 3,
} DtwmAppearanceProfile;

typedef struct {
    DtwmColorSet active;
    DtwmColorSet inactive;
    DtwmColorSet menu;
    DtwmColorSet window_bg;
    DtwmRgba desktop_bg;
    DtwmAppearanceProfile profile;
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

typedef enum {
    DTWM_ICON_PLACE_TOP_LEFT = 0,
    DTWM_ICON_PLACE_LEFT_TOP,
    DTWM_ICON_PLACE_BOTTOM_LEFT,
    DTWM_ICON_PLACE_LEFT_BOTTOM,
    DTWM_ICON_PLACE_TOP_RIGHT,
    DTWM_ICON_PLACE_RIGHT_TOP,
    DTWM_ICON_PLACE_BOTTOM_RIGHT,
    DTWM_ICON_PLACE_RIGHT_BOTTOM,
} DtwmIconPlacement;

typedef enum {
    DTWM_ANIM_STYLE_AUTO = 0,
    DTWM_ANIM_STYLE_NONE,
    DTWM_ANIM_STYLE_BEVEL,
    DTWM_ANIM_STYLE_TITLEBAR,
    DTWM_ANIM_STYLE_WIREFRAME,
} DtwmAnimStyle;

typedef enum {
    DTWM_ANIM_SPEED_VERY_QUICK = 0,
    DTWM_ANIM_SPEED_STANDARD = 1,
    DTWM_ANIM_SPEED_SLOW = 2,
} DtwmAnimSpeed;

typedef enum {
    DTWM_ANIM_KIND_NONE = 0,
    DTWM_ANIM_KIND_MINIMIZE,
    DTWM_ANIM_KIND_RESTORE,
    DTWM_ANIM_KIND_MAXIMIZE,
    DTWM_ANIM_KIND_UNMAXIMIZE,
} DtwmAnimKind;

typedef struct DtwmServer DtwmServer;
typedef struct DtwmView DtwmView;
typedef struct DtwmForeignToplevel DtwmForeignToplevel;
typedef struct DtwmImeState DtwmImeState;
typedef struct DtwmPowerDisplayState DtwmPowerDisplayState;
typedef struct DtwmA11yState DtwmA11yState;

struct DtwmView {
    struct wl_list link; /* DtwmServer.views */
    DtwmServer *server;
    DtwmViewKind kind;

    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_xwayland_surface *xwayland_surface;
    DtwmForeignToplevel *foreign_toplevel;

    /* Scene nodes:
     * frame_tree sits at (x, y) in workspace coordinates.
     * client_bg_rect fills the client box behind content_tree.
     * content_tree sits at (left_inset, top_inset) clipped to client_w × client_h.
     * frame_{top,bottom,left,right}_buf render 4 tight opaque XRGB8888 bevel strips.
     * popup_anchor_tree sits unclipped above the frame strips for xdg_popup children.
     */
    struct wlr_scene_tree *frame_tree;
    struct wlr_scene_rect *client_bg_rect;
    struct wlr_scene_tree *content_tree;
    struct wlr_scene_buffer *frame_top_buf;
    struct wlr_scene_buffer *frame_bottom_buf;
    struct wlr_scene_buffer *frame_left_buf;
    struct wlr_scene_buffer *frame_right_buf;
    struct wlr_scene_tree *popup_anchor_tree;

    int x, y;
    int client_w, client_h;
    int saved_x, saved_y, saved_w, saved_h;
    bool has_saved_geom;
    int tile_state;
    int tile_saved_x, tile_saved_y, tile_saved_w, tile_saved_h;
    bool has_tile_saved_geom;
    bool has_pending_geom;
    bool pending_pos_set;
    bool resize_inflight;
    bool has_queued_resize;
    int queued_x, queued_y, queued_w, queued_h;
    uint32_t active_resize_edges;
    int resize_anchor_right;
    int resize_anchor_bottom;
    uint32_t resize_sent_ms;
    uint32_t pending_resize_serial;

    bool mapped;
    bool active;
    bool pending_activation;
    bool maximized;
    bool minimized;
    bool is_panel;          /* Genuine CDE Front Panel (DtWM_BEHAVIOR_PANEL) */
    bool is_subpanel;       /* Genuine CDE Subpanel (DtWM_BEHAVIOR_SUBPANEL) */
    bool subpanel_torn_off; /* Subpanel torn off / dragged away into a Tool Window */
    bool is_desktop_pin;    /* dtfile Desktop.c pinned workspace object */
    bool is_swallowed;      /* Front Panel TYPE client swallowed window */
    bool is_lock_dialog;    /* dtsession PAM lock dialog / cover shell */
    bool is_native_dpi;     /* CK Core Motif / CDE client rendering 1:1 physical pixels */
    double external_client_scale; /* Scale S = dpi/96 for external non-Motif Wayland clients */
    uint32_t workspace_mask;
    bool is_sandboxed;
    char sandbox_engine[64];
    char sandbox_app_id[128];
    char toplevel_tag[128];
    char toplevel_description[256];
    bool session_rule_applied;

    /* Desktop icon state (open-window icon + minimized icon + live pixmap) */
    struct wlr_scene_buffer *desktop_icon_buf;
    cairo_surface_t *icon_surface;
    bool icon_from_named_image;
    bool has_icon_pos_hint;
    int icon_hint_x, icon_hint_y;
    char icon_title[256];
    int icon_x, icon_y, icon_w, icon_h;
    int icon_box_w, icon_box_h;
    bool icon_clipped_out;
    bool icon_latched;

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
    struct wl_listener set_app_id;
    struct wl_listener set_decorations;
    struct wl_listener set_hints;
    struct wl_listener set_geometry;
    struct wl_listener set_override_redirect;
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
    uint32_t output_idx;
    int dpi;
    bool vrr_enabled;
    struct wlr_scene_rect *bg_rect;
    struct wlr_scene_rect *lock_bg_rect;
    struct wlr_scene_buffer *backdrop_buf;
    struct wlr_scene_buffer *night_light_buf;
    struct wlr_swapchain *mag_swapchain;
    struct wlr_buffer *mag_unmag_buf;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
} DtwmOutput;

typedef struct {
    struct wl_list link;
    DtwmServer *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener keymap;
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
    struct wlr_linux_dmabuf_v1 *linux_dmabuf;
    struct wlr_linux_drm_syncobj_manager_v1 *drm_syncobj_mgr;
    struct wlr_presentation *presentation;
    struct wlr_single_pixel_buffer_manager_v1 *single_pixel_mgr;
    struct wlr_alpha_modifier_v1 *alpha_modifier_mgr;
    char renderer_name[32];
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
    struct wlr_scene_tree *lock_tree;
    struct wlr_scene_tree *lock_cover_tree;
    struct wlr_scene_tree *lock_dialog_tree;

    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
    struct wlr_xdg_activation_v1 *xdg_activation;
    struct wl_listener xdg_activation_new_token;
    struct wl_listener xdg_activation_request_activate;
    struct wl_list activation_tokens;
    uint32_t pending_activation_tokens;
    uint32_t last_typing_ms;
    DtwmView *last_typing_view;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wlr_xwayland *xwayland;
    struct wlr_session_lock_manager_v1 *session_lock_mgr;
    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wl_listener new_session_lock;
    bool session_locked;

    /* Hardware / dtstyle IPC state */
    double ptr_accel_speed;
    int ptr_accel_thresh;
    bool left_handed;
    bool tp_tap_to_click;
    bool tp_natural_scroll;
    bool tp_clickfinger;
    bool tp_dwt;
    bool tp_disable_with_ext_mouse;
    bool key_repeat_enabled;
    int key_repeat_delay;
    int key_repeat_rate;
    char xkb_layout[64];
    int bell_percent;
    int bell_pitch;
    int bell_duration;
    int saver_timeout_sec;
    int lock_timeout_sec;
    xcb_atom_t atom_session_lock_dlg;
    xcb_atom_t atom_dt_sm_win_info;
    xcb_atom_t atom_wayland_input_state;
    xcb_atom_t atom_wayland_outputs;
    xcb_atom_t atom_osd_state;
    xcb_atom_t atom_accessibility_state;
    DtwmA11yState *a11y;
    bool reduced_motion;

    /* Output management, portals, foreign-toplevel & CJK IME globals (WP-10 / WP-11) */
    struct wlr_output_manager_v1 *output_mgr;
    struct wlr_output_power_manager_v1 *output_power_mgr;
    struct wlr_export_dmabuf_manager_v1 *export_dmabuf_mgr;
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;
    DtwmImeState *ime;
    DtwmPowerDisplayState *power_display;
    struct wl_listener output_mgr_apply;
    struct wl_listener output_mgr_test;
    struct wl_listener output_power_set_mode;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *active_pointer_constraint;
    struct wl_listener new_pointer_constraint;
    struct wl_list tracked_pointer_constraints;
    struct wlr_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit_mgr;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *active_shortcuts_inhibitor;
    struct wl_listener new_shortcuts_inhibitor;
    struct wl_list tracked_shortcuts_inhibitors;
    DtwmView *input_escaped_view;
    bool ctrl_alt_chord_pending;
    struct wlr_seat *seat;
    struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
    struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;

    struct wl_list outputs;
    struct wl_list views;
    struct wl_list popups;
    struct wl_list keyboards;
    struct wl_list pointers;
    struct wl_list touches;
    struct wl_list tablets;
    struct wl_list tablet_pads;
    struct wl_list tablet_tools;
    struct wlr_tablet_manager_v2 *tablet_v2_mgr;
    bool touch_grab_active;
    int32_t touch_grab_id;

    DtwmPalette palette;
    DtwmView *focused_view;
    uint32_t current_workspace;
    uint32_t num_workspaces;
    uint32_t next_ws_serial;
    DtwmWorkspace workspaces[DTWM_MAX_WORKSPACES];

    /* Docked genuine CDE Front Panel (dtwm-panel) & TYPE client swallowing */
    bool no_xwayland;
    int native_ipc_fd;
    char native_ipc_path[512];
    struct wl_event_source *native_ipc_source;
    bool greeter_mode;
    bool super_tap_pending;
    uint32_t super_tap_press_ms;
    bool show_front_panel;
    bool show_open_window_icons;
    bool use_icon_box;
    DtwmIconPlacement icon_placement;
    int icon_placement_margin;
    bool large_icons;
    bool move_opaque;
    bool focus_follows_mouse;
    bool focus_auto_raise;
    bool allow_primary_on_top;
    bool legacy_animate_minimize;
    DtwmAnimStyle minimize_anim_style;
    DtwmAnimStyle maximize_anim_style;
    DtwmAnimSpeed anim_speed;
    bool show_task_switcher;

    /* Interactive CDE Icon Box window state (when use_icon_box is enabled) */
    bool icon_box_active;
    bool icon_box_maximized;
    bool icon_box_pos_initialized;
    int icon_box_x, icon_box_y, icon_box_w, icon_box_h;
    int icon_box_saved_x, icon_box_saved_y, icon_box_saved_w, icon_box_saved_h;
    int icon_box_scroll_y;
    int icon_box_max_scroll_y;
    bool icon_box_dragging;
    bool icon_box_resizing;
    uint32_t icon_box_resize_edges;
    double icon_box_grab_x, icon_box_grab_y;
    struct wlr_box icon_box_orig_box;
    uint32_t icon_box_last_title_ms;
    struct wlr_scene_tree *icon_box_tree;
    struct wlr_scene_buffer *icon_box_bg_buf;
    bool panel_user_moved;
    pid_t panel_pid;
    DtwmView *panel_view;
    int panel_x, panel_y, panel_w, panel_h;
    int clock_slot_x, clock_slot_y, clock_slot_w, clock_slot_h;
    char swallow_client_name[64];
    char exe_dir[1024];
    DtwmView *swallowed_clock_view;

    /* XWayland CDE Workspace Manager (_MOTIF_WM_INFO / _DT_WORKSPACE_*) XCB bridge */
    xcb_connection_t *xcb_conn;
    xcb_window_t xcb_root;
    xcb_window_t xcb_wm_win;
    xcb_window_t xcb_popup_sink_win;
    DtwmView *popup_sink_view;
    struct wl_event_source *xcb_event_source;
    xcb_atom_t atom_mwm_info;
    xcb_atom_t atom_mwm_hints;
    xcb_atom_t atom_dt_wm_hints;
    xcb_atom_t atom_ws_list;
    xcb_atom_t atom_ws_current;
    xcb_atom_t atom_ws_hints;
    xcb_atom_t atom_ws_presence;
    xcb_atom_t atom_wm_request;
    xcb_atom_t atom_panel_busy_start_seq;
    xcb_atom_t atom_panel_busy_stop_seq;
    uint32_t panel_busy_start_seq;
    uint32_t panel_busy_stop_seq;
    xcb_atom_t atom_ws_all;
    xcb_atom_t atom_wm_settings_v1;
    xcb_atom_t atom_wm_state;
    xcb_atom_t atom_net_wm_icon_geom;
    xcb_atom_t atom_net_wm_icon;
    xcb_atom_t atom_net_wm_icon_name;
    xcb_atom_t atom_net_wm_pid;
    xcb_atom_t atom_wm_command;
    xcb_atom_t atom_active_win_box;
    xcb_atom_t atom_client_ids;
    xcb_atom_t atom_randr_dpi_opt_in;
    xcb_atom_t atom_dpi;
    int initial_output_dpi[16];
    char cli_output_dpi_names[16][64];
    int cli_output_dpi_vals[16];
    int cli_output_dpi_count;
    pid_t native_dpi_pids[DTWM_MAX_NATIVE_DPI_PIDS];
    size_t native_dpi_pid_count;
    uint32_t next_output_idx;
    struct wlr_scene_tree *drag_icon_tree;
    struct wl_listener drag_destroy;
    struct wl_listener drag_icon_destroy;
    bool drag_destroy_attached;
    bool drag_icon_destroy_attached;

    /* Native Wayland ext-workspace-v1 & xdg-toplevel-icon-v1 protocols */
    struct wl_global *ext_ws_global;
    struct wl_list ext_ws_managers;
    struct wl_global *toplevel_icon_global;
    struct wl_global *toplevel_tag_global;
    struct wlr_security_context_manager_v1 *security_context_mgr;

    /* Interactive move / resize / snap / wireframe state */
    DtwmCursorMode cursor_mode;
    DtwmView *grabbed_view;
    DtwmView *subpanel_drag_candidate;
    double subpanel_drag_start_x, subpanel_drag_start_y;
    double grab_x, grab_y;
    struct wlr_box grab_orig_box;
    struct wlr_box pending_grab_box;
    uint32_t resize_edges;
    DtwmSnapZone active_snap;
    struct wlr_scene_buffer *snap_preview_buf;
    bool wireframe_active;
    bool grab_ignore_next_release;
    int wireframe_w, wireframe_h;
    struct wlr_scene_buffer *wireframe_buf;

    /* Window minimize / restore & maximize / unmaximize vsync animation engine */
    struct wl_event_source *anim_timer;
    struct wl_event_source *resize_flush_timer;
    struct wlr_scene_tree *anim_tree;
    struct wlr_scene_buffer *anim_trail_buf;
    struct wlr_scene_buffer *anim_client_buf;
    struct wlr_scene_buffer *anim_icon_buf;
    struct wlr_scene_buffer *anim_frame_buf;
    DtwmView *anim_view;
    DtwmAnimKind anim_kind;
    DtwmAnimStyle anim_active_style;
    uint64_t anim_start_ns;
    uint32_t anim_duration_ms;
    bool anim_in_latch_phase;
    bool anim_hides_view_frame;
    struct wlr_box anim_from_box;
    struct wlr_box anim_to_box;
    struct wlr_box anim_cur_box;
    struct wlr_box anim_prev_box;
    int anim_client_src_w;
    int anim_client_src_h;

    /* Double-click titlebar / window-menu button detection & live XCursor theme/size */
    uint32_t double_click_ms;
    uint32_t last_title_click_ms;
    DtwmView *last_title_click_view;
    uint32_t last_menu_btn_ms;
    DtwmView *last_menu_btn_view;
    char cursor_theme[128];
    int cursor_size;

    /* CDE Window Menu & Desktop Workspace Menu overlay */
    bool menu_visible;
    bool menu_is_root;
    DtwmView *menu_target;
    int menu_x, menu_y;
    int menu_w, menu_h;
    int menu_hover_idx;
    uint32_t menu_open_ms;
    double menu_open_x, menu_open_y;
    bool menu_drag_moved;
    struct wlr_scene_buffer *menu_buf;

    /* CDE Keyboard Task Switcher & Hardware OSD overlays */
    bool switcher_visible;
    int switcher_index;
    struct wlr_scene_buffer *switcher_buf;

    bool osd_visible;
    char osd_kind[32];
    int osd_level;
    bool osd_muted;
    char osd_label[128];
    struct wlr_scene_buffer *osd_buf;
    struct wl_event_source *osd_timer;

    struct wlr_pointer_gestures_v1 *pointer_gestures;
    bool swipe_active;
    uint32_t swipe_fingers;
    double swipe_dx, swipe_dy;
    struct wl_listener cursor_swipe_begin;
    struct wl_listener cursor_swipe_update;
    struct wl_listener cursor_swipe_end;
    struct wl_listener cursor_pinch_begin;
    struct wl_listener cursor_pinch_update;
    struct wl_listener cursor_pinch_end;
    struct wl_listener cursor_hold_begin;
    struct wl_listener cursor_hold_end;

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
    struct wl_listener cursor_touch_down;
    struct wl_listener cursor_touch_up;
    struct wl_listener cursor_touch_motion;
    struct wl_listener cursor_touch_cancel;
    struct wl_listener cursor_touch_frame;
    struct wl_listener cursor_tablet_tool_axis;
    struct wl_listener cursor_tablet_tool_proximity;
    struct wl_listener cursor_tablet_tool_tip;
    struct wl_listener cursor_tablet_tool_button;

    struct wl_listener new_input;
    struct wl_listener backend_destroy;
    struct wl_listener new_virtual_pointer;
    struct wl_listener new_virtual_keyboard;
    struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
    struct wl_listener request_cursor;
    struct wl_listener request_cursor_shape;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    struct wl_listener request_start_drag;
    struct wl_listener start_drag;
    int pointer_buttons_down;
    uint32_t pointer_buttons_mask;
    uint32_t last_pointer_button;
    bool wheel_natural_scroll;
    int wheel_scroll_speed;
    bool wheel_smooth_scroll;

    struct wl_event_source *sigint_source;
    struct wl_event_source *sigterm_source;
    struct wl_event_source *status_fallback_timer;
    bool status_written;
    bool shutting_down;
    const char *wayland_socket;
    const char *status_file;
};

/* cde_bevel.c */
int dtwm_primary_output_dpi(const DtwmServer *server);
int dtwm_output_dpi_at(const DtwmServer *server, double lx, double ly);
DtwmMetrics dtwm_metrics_for_dpi(int dpi);
DtwmMetrics dtwm_metrics_for_view(const DtwmView *view);
struct wlr_buffer *dtwm_cairo_buffer_begin(int width, int height, cairo_t **out_cr, cairo_surface_t **out_surf);
void dtwm_cairo_buffer_finish(cairo_t *cr, cairo_surface_t *surf);
void dtwm_draw_bevel(cairo_t *cr, double x, double y, double w, double h, double t, DtwmRgba tl_col, DtwmRgba br_col);
void dtwm_draw_label(cairo_t *cr, const char *text, double x, double y, double w, double h, DtwmRgba fg, bool centered, bool bold);
void dtwm_draw_label_dpi(cairo_t *cr, int dpi, const char *text, double x, double y, double w, double h, DtwmRgba fg, bool centered, bool bold);
void dtwm_palette_init_default(DtwmPalette *pal);
DtwmAppearanceProfile dtwm_parse_appearance_profile(const char *name);
void dtwm_palette_init_profile(DtwmPalette *pal, DtwmAppearanceProfile profile);
void dtwm_server_apply_appearance_profile(DtwmServer *server, DtwmAppearanceProfile profile);
void dtwm_view_insets(const DtwmView *view, int *left, int *top, int *right, int *bottom);
void dtwm_view_outer_size(const DtwmView *view, int *out_w, int *out_h);
const char *dtwm_view_title(const DtwmView *view);
void dtwm_view_update_frame(DtwmView *view);
DtwmFramePart dtwm_frame_hit_test(const DtwmView *view, int local_x, int local_y);
uint32_t dtwm_part_to_wlr_edges(DtwmFramePart part);

struct wlr_buffer *dtwm_render_window_menu(const DtwmPalette *pal, const DtwmView *view, int hover_idx, int dpi, int *out_w, int *out_h);
struct wlr_buffer *dtwm_render_root_menu(
    const DtwmPalette *pal,
    bool panel_minimized,
    bool has_panel,
    int hover_idx,
    int dpi,
    int *out_w,
    int *out_h
);
int dtwm_root_menu_item_at(int rx, int ry, int menu_w, int menu_h, int dpi);
struct wlr_buffer *dtwm_render_snap_preview(const DtwmPalette *pal, int w, int h);
struct wlr_buffer *dtwm_render_wireframe_outline(int w, int h);
struct wlr_buffer *dtwm_render_palette_wireframe_outline(
    const DtwmColorSet *cs, int w, int h, double alpha
);
struct wlr_buffer *dtwm_render_anim_bevel_frame(
    const DtwmPalette *pal,
    const DtwmView *view,
    DtwmAnimStyle style,
    DtwmAnimKind kind,
    int w,
    int h,
    double progress
);
struct wlr_buffer *dtwm_render_task_switcher(DtwmServer *server, int *out_w, int *out_h);
struct wlr_buffer *dtwm_render_osd_gauge(
    const DtwmPalette *pal,
    const char *kind,
    int level,
    bool muted,
    const char *label,
    int dpi,
    int *out_w,
    int *out_h
);

bool dtwm_view_wants_opaque_client_bg(const DtwmView *view);
bool dtwm_view_has_input_capture(const DtwmView *view);

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
void dtwm_begin_move_panel(DtwmServer *server);
void dtwm_notify_panel_mapped(DtwmServer *server);
void dtwm_notify_panel_busy_start(DtwmServer *server);
void dtwm_notify_panel_busy_stop(DtwmServer *server);
void dtwm_sync_xwayland_randr_dpi(DtwmServer *server);
void dtwm_set_output_dpi(DtwmServer *server, uint32_t output_idx, int dpi);
void dtwm_publish_active_window_box(DtwmServer *server);
void dtwm_publish_client_ids(DtwmServer *server);
bool dtwm_recall_client_by_name(DtwmServer *server, const char *name);
void dtwm_get_usable_output_box(DtwmServer *server, double ref_x, double ref_y, struct wlr_box *out_box);
void dtwm_reflow_views_for_outputs(DtwmServer *server);
void dtwm_reload_cursor_theme(DtwmServer *server, const char *theme, int size);

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
void dtwm_icons_pack(DtwmServer *server);
bool dtwm_icons_handle_desktop_button(DtwmServer *server, DtwmView *icon_v, uint32_t btn, bool pressed);
bool dtwm_icons_handle_drag_motion(DtwmServer *server);
void dtwm_icons_update_view(DtwmView *view);
struct wlr_buffer *dtwm_icons_render_tile_buffer(DtwmView *view, int *out_w, int *out_h);
DtwmView *dtwm_icons_view_at(DtwmServer *server, double lx, double ly);
void dtwm_icons_set_open_window_icons(DtwmServer *server, bool enabled);
void dtwm_icons_set_use_icon_box(DtwmServer *server, bool enabled);
void dtwm_icons_set_placement(DtwmServer *server, const char *spec);
void dtwm_icons_set_placement_margin(DtwmServer *server, int margin);
void dtwm_icons_set_large_icons(DtwmServer *server, bool large);
void dtwm_icons_refresh_focus_indicators(DtwmServer *server);
void dtwm_icons_invalidate_theme_icons(DtwmServer *server);
void dtwm_icons_record_icon_image_rule(const char *client_key, const char *icon_name);
bool dtwm_icon_box_is_topmost_at(DtwmServer *server, double lx, double ly);
DtwmFramePart dtwm_icon_box_part_at(DtwmServer *server, double lx, double ly);
bool dtwm_icon_box_handle_button(DtwmServer *server, uint32_t btn, bool pressed, uint32_t t_ms);
bool dtwm_icon_box_handle_motion(DtwmServer *server);
bool dtwm_icon_box_handle_axis(DtwmServer *server, double delta, int32_t discrete);
void dtwm_icon_box_deactivate(DtwmServer *server);
void dtwm_icons_sync_xwayland_pointer(DtwmServer *server, double lx, double ly, struct wlr_surface *focused_surf);
void dtwm_icons_reposition_swallowed(DtwmServer *server);
void dtwm_recheck_cursor_focus(DtwmServer *server);

/* cde_xdnd_bridge.c */
void dtwm_xdnd_on_xwayland_ready(DtwmServer *server);
void dtwm_xdnd_finish(void);
bool dtwm_xdnd_handle_xcb_event(DtwmServer *server, xcb_generic_event_t *ev);
void dtwm_xdnd_on_button_state(DtwmServer *server, uint32_t button, bool pressed);
void dtwm_xdnd_on_cursor_motion(DtwmServer *server, struct wlr_surface *surface, double sx, double sy, uint32_t time_msec);

/* cde_session_lock.c */
void dtwm_lock_init(DtwmServer *server);
void dtwm_lock_finish(DtwmServer *server);
void dtwm_lock_on_xwayland_ready(DtwmServer *server);
void dtwm_lock_sync_outputs(DtwmServer *server);
void dtwm_lock_notify_activity(DtwmServer *server);
bool dtwm_lock_is_sm_lock_window(DtwmServer *server, xcb_window_t win);
void dtwm_lock_focus_surface(DtwmServer *server, struct wlr_surface *surf);
struct wlr_surface *dtwm_lock_surface_at(DtwmServer *server, double lx, double ly, double *sx, double *sy);
void dtwm_hw_apply_input_device(DtwmServer *server, struct wlr_input_device *dev);
bool dtwm_hw_pointer_needs_hires_boost(struct wlr_pointer *wlr_ptr);
bool dtwm_hw_pointer_is_touchpad(struct wlr_pointer *wlr_ptr);
void dtwm_hw_publish_wheel_ipc(DtwmServer *server);
void dtwm_hw_reapply_all_pointers(DtwmServer *server);
void dtwm_hw_apply_keyboard_defaults(DtwmServer *server, struct wlr_keyboard *wlr_kbd);
bool dtwm_hw_handle_wm_request(DtwmServer *server, const char *cmd);
void dtwm_hw_publish_state(DtwmServer *server);
void dtwm_handle_touchpad_swipe(DtwmServer *server, uint32_t fingers, double dx, double dy);
void dtwm_restore_view_focus(DtwmServer *server, DtwmView *view);
void dtwm_open_window_menu_for_view(DtwmServer *server, DtwmView *view, int x, int y);
void dtwm_view_set_maximized(DtwmView *view, bool maximized);
void dtwm_view_set_minimized(DtwmView *view, bool minimized);
void dtwm_view_apply_geometry(DtwmView *view, int x, int y, int client_w, int client_h);
void dtwm_view_close(DtwmView *view);
DtwmAnimStyle dtwm_parse_anim_style(const char *val);
DtwmAnimSpeed dtwm_parse_anim_speed(const char *val);
DtwmAnimStyle dtwm_resolve_anim_style(const DtwmServer *server, bool is_maximize);
void dtwm_anim_cancel_all(DtwmServer *server);
void dtwm_anim_cancel_for_view(DtwmServer *server, DtwmView *view);

/* cde_output_mgmt.c */
void dtwm_output_mgmt_init(DtwmServer *server);
void dtwm_output_mgmt_finish(DtwmServer *server);
void dtwm_output_mgmt_sync(DtwmServer *server);
void dtwm_output_mgmt_after_reconfigure(DtwmServer *server);
void dtwm_output_mgmt_publish_x11(DtwmServer *server);
bool dtwm_output_mgmt_handle_wm_request(DtwmServer *server, const char *cmd);
void dtwm_foreign_toplevel_sync(DtwmView *view);
void dtwm_foreign_toplevel_destroy(DtwmView *view);

/* cde_ime.c */
void dtwm_ime_init(DtwmServer *server);
void dtwm_ime_finish(DtwmServer *server);
void dtwm_ime_set_focus(DtwmServer *server, struct wlr_surface *surface);
void dtwm_ime_set_keyboard(DtwmServer *server, struct wlr_keyboard *wlr_kbd);
void dtwm_ime_on_session_lock_changed(DtwmServer *server, bool locked);
bool dtwm_ime_forward_key(
    DtwmServer *server,
    struct wlr_keyboard *wlr_kbd,
    const struct wlr_keyboard_key_event *ev
);
bool dtwm_ime_forward_modifiers(DtwmServer *server, struct wlr_keyboard *wlr_kbd);

/* cde_power_display.c */
void dtwm_kelvin_to_rgb(int temp_k, double *out_r, double *out_g, double *out_b);
void dtwm_power_display_init(DtwmServer *server);
void dtwm_power_display_finish(DtwmServer *server);
void dtwm_power_display_on_xwayland_ready(DtwmServer *server);
void dtwm_power_display_on_new_output(DtwmServer *server, DtwmOutput *output);
void dtwm_power_display_on_output_destroy(DtwmServer *server, DtwmOutput *output);
void dtwm_power_display_sync_outputs(DtwmServer *server);
void dtwm_power_display_record_dpi(DtwmServer *server, const char *name, int dpi);
int dtwm_power_display_lookup_dpi(const DtwmServer *server, const char *name);
void dtwm_power_display_on_new_input(DtwmServer *server, struct wlr_input_device *dev);
void dtwm_power_display_notify_activity(DtwmServer *server);
bool dtwm_power_display_handle_wm_request(DtwmServer *server, const char *cmd);
bool dtwm_power_display_handle_keybinding(DtwmServer *server, uint32_t mods, uint32_t keysym);

/* cde_accessibility.c */
void dtwm_a11y_init(DtwmServer *server);
void dtwm_a11y_finish(DtwmServer *server);
void dtwm_a11y_on_xwayland_ready(DtwmServer *server);
bool dtwm_a11y_handle_xcb_event(DtwmServer *server, xcb_generic_event_t *ev);
bool dtwm_a11y_handle_wm_request(DtwmServer *server, const char *cmd);
void dtwm_a11y_publish_state(DtwmServer *server);
void dtwm_a11y_trigger_visual_bell(DtwmServer *server);
bool dtwm_a11y_sticky_keys_enabled(const DtwmServer *server);
uint32_t dtwm_a11y_effective_modifiers(const DtwmServer *server, struct wlr_keyboard *wlr_kbd);
void dtwm_a11y_notify_seat_modifiers(DtwmServer *server, struct wlr_keyboard *wlr_kbd);
void dtwm_a11y_notify_keyboard_enter(
    DtwmServer *server, struct wlr_surface *surface, struct wlr_keyboard *wlr_kbd
);
bool dtwm_a11y_filter_key(
    DtwmServer *server,
    struct wlr_keyboard *wlr_kbd,
    const struct wlr_keyboard_key_event *ev,
    uint32_t time_msec
);
bool dtwm_a11y_handle_shortcut(DtwmServer *server, uint32_t mods, xkb_keysym_t sym);
bool dtwm_a11y_handle_axis(
    DtwmServer *server,
    uint32_t orientation,
    double delta,
    int32_t discrete
);
int dtwm_a11y_get_zoom_pct(const DtwmServer *server);
void dtwm_a11y_on_cursor_motion(DtwmServer *server);
void dtwm_a11y_on_output_destroy(DtwmOutput *output);
bool dtwm_a11y_render_output_frame(DtwmOutput *output, struct wlr_scene_output *scene_output);
bool dtwm_server_dispatch_keybinding(DtwmServer *server, uint32_t mods, xkb_keysym_t sym);
void dtwm_spawn_front_panel(DtwmServer *server);
void dtwm_apply_resource_manager_string(DtwmServer *server, const char *data, size_t len);
void dtwm_dispatch_wm_request_bytes(DtwmServer *server, const char *data, size_t len);
void dtwm_native_ipc_publish_prop(DtwmServer *server, const char *atom_name, const void *data, size_t len);
void dtwm_sync_resource_manager_dpi(DtwmServer *server);
void dtwm_sync_external_view_scene(DtwmView *view);
void dtwm_sync_all_external_client_scales(DtwmServer *server);
void dtwm_workspace_sync_ipc(DtwmServer *server);
bool dtwm_wayland_session_handle_wm_request(DtwmServer *server, const char *cmd);

#endif /* DTWM_WAYLAND_H */
