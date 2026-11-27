/*
 * CDE - Common Desktop Environment
 *
 * dtportal.h: Native Motif XDG Desktop Portal backend (xdg-desktop-portal-cde)
 * providing FileChooser, AppChooser, Inhibit, Wallpaper, GlobalShortcuts,
 * RemoteDesktop (Notify* + EIS socket), and Settings for CDE (Wayland & X11).
 */

#ifndef CDE_DTPORTAL_H
#define CDE_DTPORTAL_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <X11/Intrinsic.h>
#include <Xm/Xm.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#define DTPORTAL_MAX_REQUESTS 64
#define DTPORTAL_MAX_SESSIONS 32
#define DTPORTAL_MAX_SHORTCUTS_PER_SESSION 32
#define DTPORTAL_MAX_CHOICES 16
#define DTPORTAL_MAX_FILTERS 16
#define DTPORTAL_MAX_EIS_CLIENTS 16

typedef enum {
    DTPORTAL_REQ_NONE = 0,
    DTPORTAL_REQ_FILE_OPEN,
    DTPORTAL_REQ_FILE_SAVE,
    DTPORTAL_REQ_FILE_SAVE_MULTI,
    DTPORTAL_REQ_APP_CHOOSER,
    DTPORTAL_REQ_WALLPAPER,
    DTPORTAL_REQ_INHIBIT,
    DTPORTAL_REQ_REMOTEDESKTOP_START,
} DtPortalReqKind;

typedef enum {
    DTPORTAL_SESS_NONE = 0,
    DTPORTAL_SESS_INHIBIT_MONITOR,
    DTPORTAL_SESS_GLOBAL_SHORTCUTS,
    DTPORTAL_SESS_REMOTE_DESKTOP,
} DtPortalSessionKind;

typedef struct DtPortalCtx DtPortalCtx;

typedef struct {
    char id[96];
    char label[128];
    char selected_id[96];
    bool is_boolean;
    Widget widget; /* XmToggleButton or XmOptionMenu */
    int num_opts;
    char opt_ids[16][96];
    char opt_labels[16][128];
} DtPortalChoiceItem;

typedef struct {
    char name[128];
    char first_glob[128];
    GVariant *raw_tuple; /* (sa(us)) ref */
} DtPortalFilterItem;

typedef struct {
    bool in_use;
    DtPortalCtx *ctx;
    DtPortalReqKind kind;
    char handle_path[256];
    guint reg_id;
    char sender[128];
    guint watch_id;
    char app_id[128];
    GDBusMethodInvocation *invocation;

    /* Motif dialog widgets */
    Widget shell;
    Widget dialog;
    Widget list_widget;
    Widget filter_menu;

    /* FileChooser state */
    bool directory_mode;
    bool multiple_mode;
    char save_folder[1024];
    int save_files_count;
    char save_files_names[16][256];
    int choice_count;
    DtPortalChoiceItem choices[DTPORTAL_MAX_CHOICES];
    int filter_count;
    int active_filter_idx;
    DtPortalFilterItem filters[DTPORTAL_MAX_FILTERS];

    /* AppChooser state */
    int app_count;
    char app_ids[64][128];
    char app_labels[64][256];

    /* Wallpaper state */
    char wallpaper_uri[1024];
    char wallpaper_set_on[32];

    /* Inhibit state */
    uint32_t inhibit_flags;
    char inhibit_reason[256];

    /* RemoteDesktop Start state */
    char session_path[256];
    Widget remember_toggle;
} DtPortalRequest;

typedef struct {
    char id[96];
    char description[256];
    char trigger[128];
} DtPortalShortcutEntry;

typedef struct {
    bool in_use;
    int fd;
    guint io_watch_id;
    DtPortalCtx *ctx;
    char session_path[256];
    bool handshake_done;
    bool device_emulating;
    uint32_t next_serial;
    uint8_t rx_buf[4096];
    size_t rx_len;
} DtPortalEisClient;

typedef struct {
    bool in_use;
    DtPortalCtx *ctx;
    DtPortalSessionKind kind;
    char session_path[256];
    char session_key[96];
    guint reg_id;
    char sender[128];
    guint watch_id;
    char app_id[128];

    /* GlobalShortcuts session */
    int shortcut_count;
    DtPortalShortcutEntry shortcuts[DTPORTAL_MAX_SHORTCUTS_PER_SESSION];

    /* RemoteDesktop session */
    uint32_t device_types; /* 1=KEYBOARD, 2=POINTER, 4=TOUCHSCREEN */
    bool persist_mode_allow;
    bool started;
    char restore_token[96];
} DtPortalSession;

struct DtPortalCtx {
    XtAppContext app;
    Widget top_shell;
    Display *dpy;
    bool is_wayland;

    GDBusConnection *bus;
    guint bus_owner_id;

    /* Compositor IPC socket (dtwm-wayland .ipc) */
    int wl_ipc_fd;
    char wl_ipc_rx[8192];
    size_t wl_ipc_rx_len;
    uint32_t last_shortcut_seq;

    /* Wayland virtual input client for RemoteDesktop & EIS */
    struct wl_display *wl_dpy;
    struct wl_registry *wl_reg;
    struct wl_seat *wl_seat;
    void *vptr_mgr; /* zwlr_virtual_pointer_manager_v1 proxy */
    void *vptr;     /* zwlr_virtual_pointer_v1 proxy */
    void *vkbd_mgr; /* zwp_virtual_keyboard_manager_v1 proxy */
    void *vkbd;     /* zwp_virtual_keyboard_v1 proxy */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    uint32_t output_w;
    uint32_t output_h;

    /* Settings cache */
    uint32_t color_scheme; /* 1=prefer-dark, 2=prefer-light */
    uint32_t contrast;     /* 1=high, 0=normal */

    /* Test/Introspection files (optional via env) */
    const char *test_state_path;
    const char *test_cmd_path;

    DtPortalRequest requests[DTPORTAL_MAX_REQUESTS];
    DtPortalSession sessions[DTPORTAL_MAX_SESSIONS];
    DtPortalEisClient eis_clients[DTPORTAL_MAX_EIS_CLIENTS];
};

/* Core request/session lifecycle & IPC helpers (dtportal_main.c) */
DtPortalRequest *dtportal_alloc_request(
    DtPortalCtx *ctx,
    DtPortalReqKind kind,
    const char *handle_path,
    const char *app_id,
    GDBusMethodInvocation *invocation
);
void dtportal_free_request(DtPortalRequest *req);
DtPortalRequest *dtportal_find_request(DtPortalCtx *ctx, const char *handle_path);

DtPortalSession *dtportal_alloc_session(
    DtPortalCtx *ctx,
    DtPortalSessionKind kind,
    const char *session_path,
    const char *app_id,
    const char *sender
);
void dtportal_close_session(DtPortalSession *sess, bool emit_closed_signal);
DtPortalSession *dtportal_find_session(DtPortalCtx *ctx, const char *session_path);
DtPortalSession *dtportal_find_session_by_key(DtPortalCtx *ctx, const char *session_key);

void dtportal_send_wm_request(DtPortalCtx *ctx, const char *cmd);
void dtportal_write_test_state(DtPortalCtx *ctx);
char *dtportal_mint_activation_token(DtPortalCtx *ctx, const char *app_id);

/* Dialogs & desktop portals (portal_dialogs.c) */
void dtportal_handle_file_chooser(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
);
void dtportal_handle_app_chooser(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
);
void dtportal_handle_wallpaper(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
);
void dtportal_handle_global_shortcuts(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
);
void dtportal_on_shortcut_ipc_event(DtPortalCtx *ctx, const char *payload);
void dtportal_test_accept_active_dialog(DtPortalCtx *ctx, const char *arg);
void dtportal_test_cancel_active_dialog(DtPortalCtx *ctx);

/* Inhibit, Settings, RemoteDesktop & EIS (portal_remote_eis.c) */
void dtportal_handle_inhibit(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
);
void dtportal_recompute_inhibit_state(DtPortalCtx *ctx);
void dtportal_handle_settings(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
);
void dtportal_sync_settings_from_appearance(DtPortalCtx *ctx);
void dtportal_on_appearance_signal(
    DtPortalCtx *ctx, uint32_t color_scheme, uint32_t contrast
);
void dtportal_handle_remote_desktop(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
);
void dtportal_cleanup_remote_input(DtPortalCtx *ctx);

#endif /* CDE_DTPORTAL_H */
