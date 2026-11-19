/*
 * CDE - Common Desktop Environment
 * cde_accessibility.c: Keyboard Accessibility (AccessX: Sticky Keys, Slow Keys,
 * Bounce Keys), Visual Bell, Reduced Motion, and Cursor-Tracking Screen
 * Magnification for dtwm-wayland (WP-20).
 */

#define _GNU_SOURCE
#include "dtwm_wayland.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_texture.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-system-bell-v1-protocol.h"

#define DTWM_A11Y_KEY_SLOTS 64
#define DTWM_A11Y_SLOW_SLOTS 16
#define DTWM_A11Y_HELD_MODS_MAX 16

typedef enum {
    DTWM_VBELL_OFF = 0,
    DTWM_VBELL_WINDOW = 1,
    DTWM_VBELL_SCREEN = 2,
} DtwmVbellMode;

typedef struct {
    bool in_use;
    uint32_t keycode;
    bool has_release;
    uint32_t last_release_ms;
    bool bounce_rejected;
    bool consumed_by_shortcut;
} DtwmKeyTrackSlot;

typedef struct {
    DtwmServer *server;
    struct wlr_keyboard *wlr_kbd;
    struct wl_event_source *timer;
    uint32_t keycode;
    bool active;
    bool accepted;
} DtwmSlowKeySlot;

typedef struct {
    bool held;
    uint32_t keycode;
    uint32_t mod_bit;
} DtwmHeldModKey;

struct DtwmA11yState {
    DtwmServer *server;

    /* AccessX configuration & live modifier state */
    bool sticky_keys;
    bool sticky_two_keys_off;
    uint32_t phys_mods;
    uint32_t latched_mods;
    uint32_t locked_mods;
    uint32_t mod_handled_on_press;
    bool chord_used_while_mod_held;
    DtwmHeldModKey held_mod_keys[DTWM_A11Y_HELD_MODS_MAX];

    bool slow_keys;
    int slow_keys_ms;
    DtwmSlowKeySlot slow_slots[DTWM_A11Y_SLOW_SLOTS];

    bool bounce_keys;
    int bounce_keys_ms;
    DtwmKeyTrackSlot key_slots[DTWM_A11Y_KEY_SLOTS];

    /* Visual Bell & Reduced Motion */
    DtwmVbellMode vbell_mode;
    uint32_t vbell_count;
    struct wlr_scene_rect *vbell_rect;
    struct wl_event_source *vbell_timer;
    struct wl_global *xdg_bell_global;

    /* XWayland XKB BellNotify subscription (raw XCB wire protocol) */
    bool xkb_bell_subscribed;
    uint8_t xkb_first_event;

    /* Cursor-tracking Screen Magnifier */
    int configured_zoom_pct; /* 125..400 remembered when toggled on */
    int active_zoom_pct;     /* 100 when off, 125..400 when active */
};

static uint32_t a11y_now_msec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static const char *vbell_mode_to_str(DtwmVbellMode mode) {
    switch (mode) {
    case DTWM_VBELL_WINDOW:
        return "window";
    case DTWM_VBELL_SCREEN:
        return "screen";
    default:
        return "off";
    }
}

static DtwmVbellMode parse_vbell_mode(const char *s) {
    if (!s) return DTWM_VBELL_OFF;
    if (strcasecmp(s, "window") == 0 || strcasecmp(s, "active") == 0 || strcmp(s, "1") == 0) {
        return DTWM_VBELL_WINDOW;
    }
    if (strcasecmp(s, "screen") == 0 || strcasecmp(s, "fullscreen") == 0 || strcmp(s, "2") == 0) {
        return DTWM_VBELL_SCREEN;
    }
    return DTWM_VBELL_OFF;
}

static bool parse_bool_str(const char *s, bool fallback) {
    if (!s || !*s) return fallback;
    if (strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 ||
        strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0) {
        return true;
    }
    if (strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0 ||
        strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0) {
        return false;
    }
    return fallback;
}

static void schedule_all_outputs(DtwmServer *server, bool force_scene_damage) {
    if (!server) return;
    DtwmOutput *dout;
    wl_list_for_each(dout, &server->outputs, link) {
        if (force_scene_damage && dout->bg_rect) {
            wlr_scene_node_set_enabled(&dout->bg_rect->node, false);
            wlr_scene_node_set_enabled(&dout->bg_rect->node, true);
        }
        if (dout->wlr_output) {
            wlr_output_schedule_frame(dout->wlr_output);
        }
    }
}

static void set_active_zoom_pct(DtwmServer *server, int new_zoom_pct) {
    if (!server || !server->a11y) return;
    DtwmA11yState *a11y = server->a11y;
    if (new_zoom_pct < 125) {
        new_zoom_pct = 100;
    } else {
        new_zoom_pct = clamp_int(((new_zoom_pct + 12) / 25) * 25, 125, 400);
        a11y->configured_zoom_pct = new_zoom_pct;
    }
    if (a11y->active_zoom_pct == new_zoom_pct) {
        return;
    }
    bool was_mag = (a11y->active_zoom_pct > 100);
    bool now_mag = (new_zoom_pct > 100);
    a11y->active_zoom_pct = new_zoom_pct;

    if (!now_mag) {
        DtwmOutput *dout;
        wl_list_for_each(dout, &server->outputs, link) {
            if (dout->mag_unmag_buf) {
                wlr_buffer_unlock(dout->mag_unmag_buf);
                dout->mag_unmag_buf = nullptr;
            }
        }
    }
    /* Force full scene damage when transitioning into or out of magnification */
    schedule_all_outputs(server, was_mag != now_mag);
}

static void apply_reduced_motion(DtwmServer *server, bool reduced) {
    if (!server) return;
    server->reduced_motion = reduced;
    if (reduced) {
        server->anim_step = 0;
        if (server->anim_timer) {
            wl_event_source_timer_update(server->anim_timer, 0);
        }
        if (server->anim_buf) {
            wlr_scene_buffer_set_buffer(server->anim_buf, nullptr);
            wlr_scene_node_set_enabled(&server->anim_buf->node, false);
        }
    }
}

static void load_accessibility_conf(DtwmServer *server) {
    if (!server || !server->a11y) return;
    DtwmA11yState *a11y = server->a11y;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/.dt/accessibility.conf", home);
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    bool mag_enabled = false;
    int cfg_zoom = a11y->configured_zoom_pct;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#' || *p == '!') continue;
        char *nl = strpbrk(p, "\r\n");
        if (nl) *nl = '\0';
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        while (*val && isspace((unsigned char)*val)) val++;
        size_t klen = strlen(key);
        while (klen > 0 && isspace((unsigned char)key[klen - 1])) {
            key[--klen] = '\0';
        }

        if (strcasecmp(key, "sticky_keys") == 0 || strcasecmp(key, "sticky") == 0) {
            a11y->sticky_keys = parse_bool_str(val, a11y->sticky_keys);
        } else if (strcasecmp(key, "sticky_two_keys_off") == 0 || strcasecmp(key, "two_keys_off") == 0) {
            a11y->sticky_two_keys_off = parse_bool_str(val, a11y->sticky_two_keys_off);
        } else if (strcasecmp(key, "slow_keys") == 0 || strcasecmp(key, "slow") == 0) {
            a11y->slow_keys = parse_bool_str(val, a11y->slow_keys);
        } else if (strcasecmp(key, "slow_keys_ms") == 0 || strcasecmp(key, "slow_ms") == 0) {
            a11y->slow_keys_ms = clamp_int(atoi(val), 50, 2000);
        } else if (strcasecmp(key, "bounce_keys") == 0 || strcasecmp(key, "bounce") == 0) {
            a11y->bounce_keys = parse_bool_str(val, a11y->bounce_keys);
        } else if (strcasecmp(key, "bounce_keys_ms") == 0 || strcasecmp(key, "bounce_ms") == 0) {
            a11y->bounce_keys_ms = clamp_int(atoi(val), 50, 2000);
        } else if (strcasecmp(key, "visual_bell") == 0 || strcasecmp(key, "vbell") == 0) {
            a11y->vbell_mode = parse_vbell_mode(val);
        } else if (strcasecmp(key, "reduced_motion") == 0) {
            apply_reduced_motion(server, parse_bool_str(val, server->reduced_motion));
        } else if (strcasecmp(key, "magnifier") == 0) {
            mag_enabled = parse_bool_str(val, false);
        } else if (strcasecmp(key, "zoom_pct") == 0) {
            int z = atoi(val);
            if (z >= 125) {
                cfg_zoom = clamp_int(z, 125, 400);
            }
        }
    }
    fclose(fp);

    a11y->configured_zoom_pct = cfg_zoom;
    if (!a11y->sticky_keys) {
        a11y->latched_mods = 0;
        a11y->locked_mods = 0;
    }
    set_active_zoom_pct(server, mag_enabled ? cfg_zoom : 100);
}

static xcb_atom_t intern_xcb_atom(xcb_connection_t *conn, const char *name) {
    if (!conn || !name) return XCB_ATOM_NONE;
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, nullptr);
    if (!rep) return XCB_ATOM_NONE;
    xcb_atom_t a = rep->atom;
    free(rep);
    return a;
}

void dtwm_a11y_publish_state(DtwmServer *server) {
    if (!server || !server->a11y || !server->xcb_conn || server->xcb_root == XCB_WINDOW_NONE) {
        return;
    }
    if (server->atom_accessibility_state == XCB_ATOM_NONE) {
        server->atom_accessibility_state = intern_xcb_atom(server->xcb_conn, "_DT_ACCESSIBILITY_STATE");
        if (server->atom_accessibility_state == XCB_ATOM_NONE) return;
    }
    const DtwmA11yState *a11y = server->a11y;
    char buf[512];
    int n = snprintf(
        buf,
        sizeof(buf),
        "sticky=%d latched=%u locked=%u slow=%d slow_ms=%d bounce=%d bounce_ms=%d "
        "vbell=%s vbell_count=%u reduced_motion=%d zoom_pct=%d",
        a11y->sticky_keys ? 1 : 0,
        a11y->latched_mods,
        a11y->locked_mods,
        a11y->slow_keys ? 1 : 0,
        a11y->slow_keys_ms,
        a11y->bounce_keys ? 1 : 0,
        a11y->bounce_keys_ms,
        vbell_mode_to_str(a11y->vbell_mode),
        a11y->vbell_count,
        server->reduced_motion ? 1 : 0,
        a11y->active_zoom_pct
    );
    if (n > 0) {
        xcb_change_property(
            server->xcb_conn,
            XCB_PROP_MODE_REPLACE,
            server->xcb_root,
            server->atom_accessibility_state,
            XCB_ATOM_STRING,
            8,
            (uint32_t)n,
            buf
        );
        xcb_flush(server->xcb_conn);
    }
}

/* --- Visual Bell & Wayland xdg-system-bell-v1 / XWayland XKB BellNotify --- */

static int handle_vbell_timer(void *data) {
    DtwmServer *server = data;
    if (!server || !server->a11y || !server->a11y->vbell_rect) return 0;
    wlr_scene_node_set_enabled(&server->a11y->vbell_rect->node, false);
    if (server->a11y->active_zoom_pct > 100) {
        schedule_all_outputs(server, false);
    }
    return 0;
}

void dtwm_a11y_trigger_visual_bell(DtwmServer *server) {
    if (!server || !server->a11y || server->shutting_down) return;
    DtwmA11yState *a11y = server->a11y;
    if (a11y->vbell_mode == DTWM_VBELL_OFF || !a11y->vbell_rect) {
        return;
    }

    int fx = 0, fy = 0, fw = 0, fh = 0;
    if (a11y->vbell_mode == DTWM_VBELL_WINDOW &&
        server->focused_view && server->focused_view->mapped &&
        !server->focused_view->minimized) {
        DtwmView *v = server->focused_view;
        int ow = 0, oh = 0;
        dtwm_view_outer_size(v, &ow, &oh);
        fx = v->x;
        fy = v->y;
        fw = ow;
        fh = oh;
    } else {
        struct wlr_output *wo = server->cursor
                                    ? wlr_output_layout_output_at(
                                          server->output_layout, server->cursor->x, server->cursor->y
                                      )
                                    : nullptr;
        if (!wo && !wl_list_empty(&server->outputs)) {
            DtwmOutput *first = wl_container_of(server->outputs.next, first, link);
            wo = first->wlr_output;
        }
        struct wlr_box obox = {0, 0, 1280, 900};
        if (wo) {
            wlr_output_layout_get_box(server->output_layout, wo, &obox);
        }
        fx = obox.x;
        fy = obox.y;
        fw = obox.width > 0 ? obox.width : 1280;
        fh = obox.height > 0 ? obox.height : 900;
    }

    if (fw <= 0 || fh <= 0) return;

    a11y->vbell_count++;
    wlr_scene_node_set_position(&a11y->vbell_rect->node, fx, fy);
    wlr_scene_rect_set_size(a11y->vbell_rect, fw, fh);
    wlr_scene_node_raise_to_top(&a11y->vbell_rect->node);
    wlr_scene_node_set_enabled(&a11y->vbell_rect->node, true);
    if (a11y->vbell_timer) {
        wl_event_source_timer_update(a11y->vbell_timer, 120);
    }
    dtwm_a11y_publish_state(server);
    if (a11y->active_zoom_pct > 100) {
        schedule_all_outputs(server, false);
    }
}

static void dtwm_ring_audible_bell(DtwmServer *server, const xcb_generic_event_t *xkb_ev) {
    if (!server) return;
    const char *dis = getenv("DTWM_DISABLE_AUDIBLE_BELL");
    if (dis && strcmp(dis, "1") == 0) return;

    /* Rate-limit audible bells to at most one every 75 ms */
    static uint64_t last_bell_ms = 0;
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
    if (now_ms - last_bell_ms < 75ULL) return;
    last_bell_ms = now_ms;

    int pct = server->bell_percent > 0 ? server->bell_percent : 50;
    int pitch = server->bell_pitch > 0 ? server->bell_pitch : 400;
    int dur_ms = server->bell_duration > 0 ? server->bell_duration : 100;

    if (xkb_ev) {
        const uint8_t *raw = (const uint8_t *)xkb_ev;
        uint8_t ev_pct = raw[11];
        int16_t ev_pitch = 0, ev_dur = 0;
        memcpy(&ev_pitch, raw + 12, sizeof(int16_t));
        memcpy(&ev_dur, raw + 14, sizeof(int16_t));
        if (ev_pct == 0) return; /* Explicitly muted bell */
        if (ev_pct <= 100) pct = (int)ev_pct;
        if (ev_pitch >= 50 && ev_pitch <= 9000) pitch = (int)ev_pitch;
        if (ev_dur >= 40 && ev_dur <= 2500) dur_ms = (int)ev_dur;
    } else if (server->bell_percent == 0) {
        return;
    }

    static const char *const candidates[] = {
        "/usr/local/bin/ck-sound",
        "/usr/dt/bin/ck-sound",
        "/usr/bin/ck-sound",
        nullptr,
    };
    const char *cksound_bin = nullptr;
    for (int i = 0; candidates[i]; ++i) {
        if (access(candidates[i], X_OK) == 0) {
            cksound_bin = candidates[i];
            break;
        }
    }

    const char *home = getenv("HOME");
    bool have_beep_conf = false;
    if (home && home[0]) {
        char conf_path[1024];
        snprintf(conf_path, sizeof(conf_path), "%s/.dt/beep.conf", home);
        have_beep_conf = (access(conf_path, R_OK) == 0);
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            if (cksound_bin) {
                if (have_beep_conf) {
                    char *argv[] = { (char *)cksound_bin, (char *)"--bell", nullptr };
                    execv(cksound_bin, argv);
                } else {
                    char pct_s[16], pitch_s[16], dur_s[16];
                    snprintf(pct_s, sizeof(pct_s), "%d", pct);
                    snprintf(pitch_s, sizeof(pitch_s), "%d", pitch);
                    snprintf(dur_s, sizeof(dur_s), "%d", dur_ms);
                    char *argv[] = { (char *)cksound_bin, (char *)"--bell", pct_s, pitch_s, dur_s, nullptr };
                    execv(cksound_bin, argv);
                }
            } else {
                execlp("ck-sound", "ck-sound", "--bell", (char *)nullptr);
            }
            _exit(0);
        }
        _exit(0);
    }
    if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
}

static void xdg_bell_destroy_req(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_bell_ring_req(
    struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface
) {
    (void)client;
    (void)surface;
    DtwmServer *server = wl_resource_get_user_data(resource);
    dtwm_a11y_trigger_visual_bell(server);
    dtwm_ring_audible_bell(server, nullptr);
}

static const struct xdg_system_bell_v1_interface xdg_bell_impl = {
    .destroy = xdg_bell_destroy_req,
    .ring = xdg_bell_ring_req,
};

static void bind_xdg_system_bell(
    struct wl_client *client, void *data, uint32_t version, uint32_t id
) {
    DtwmServer *server = data;
    struct wl_resource *res =
        wl_resource_create(client, &xdg_system_bell_v1_interface, (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &xdg_bell_impl, server, nullptr);
}

static void subscribe_xwayland_xkb_bell(DtwmServer *server) {
    if (!server || !server->a11y || !server->xcb_conn) return;
    DtwmA11yState *a11y = server->a11y;
    a11y->xkb_bell_subscribed = false;

    xcb_query_extension_cookie_t qck = xcb_query_extension(server->xcb_conn, 9, "XKEYBOARD");
    xcb_query_extension_reply_t *qrep = xcb_query_extension_reply(server->xcb_conn, qck, nullptr);
    if (!qrep || !qrep->present) {
        free(qrep);
        return;
    }
    uint8_t major_op = qrep->major_opcode;
    a11y->xkb_first_event = qrep->first_event;
    free(qrep);

    static xcb_extension_t xkb_ext_id = { "XKEYBOARD", 0 };

    /* 1. X_kbUseExtension (opcode 0, length 2 words = 8 bytes, version 1.0) */
    struct {
        uint8_t reqType;
        uint8_t xkbReqType;
        uint16_t length;
        uint16_t wantedMajor;
        uint16_t wantedMinor;
    } use_req = { major_op, 0, 2, 1, 0 };

    struct iovec use_iov[2] = {
        { .iov_base = &use_req, .iov_len = sizeof(use_req) },
        { .iov_base = nullptr, .iov_len = 0 },
    };
    static const xcb_protocol_request_t use_proto = {
        .count = 1,
        .ext = &xkb_ext_id,
        .opcode = 0,
        .isvoid = 0,
    };
    unsigned int seq = xcb_send_request(
        server->xcb_conn, XCB_REQUEST_CHECKED, &use_iov[0], &use_proto
    );
    void *use_rep = xcb_wait_for_reply(server->xcb_conn, seq, nullptr);
    if (!use_rep) {
        return;
    }
    uint8_t supported = *((const uint8_t *)use_rep + 1);
    free(use_rep);
    if (!supported) {
        return;
    }

    /* 2. X_kbSelectEvents (opcode 1, length 4 words = 16 bytes):
     *    deviceSpec = XkbUseCoreKbd (0x0100), affectWhich = XkbBellNotifyMask (0x0100),
     *    selectAll = XkbBellNotifyMask (0x0100). */
    struct {
        uint8_t reqType;
        uint8_t xkbReqType;
        uint16_t length;
        uint16_t deviceSpec;
        uint16_t affectWhich;
        uint16_t clear;
        uint16_t selectAll;
        uint16_t affectMap;
        uint16_t map;
    } sel_req = { major_op, 1, 4, 0x0100, 0x0100, 0, 0x0100, 0, 0 };

    struct iovec sel_iov[2] = {
        { .iov_base = &sel_req, .iov_len = sizeof(sel_req) },
        { .iov_base = nullptr, .iov_len = 0 },
    };
    static const xcb_protocol_request_t sel_proto = {
        .count = 1,
        .ext = &xkb_ext_id,
        .opcode = 1,
        .isvoid = 1,
    };
    xcb_send_request(server->xcb_conn, 0, &sel_iov[0], &sel_proto);
    xcb_flush(server->xcb_conn);
    a11y->xkb_bell_subscribed = true;
}

bool dtwm_a11y_handle_xcb_event(DtwmServer *server, xcb_generic_event_t *ev) {
    if (!server || !server->a11y || !ev || !server->a11y->xkb_bell_subscribed) {
        return false;
    }
    uint8_t rt = ev->response_type & 0x7fu;
    if (rt == server->a11y->xkb_first_event && ev->pad0 == 8 /* XkbBellNotify */) {
        dtwm_a11y_trigger_visual_bell(server);
        dtwm_ring_audible_bell(server, ev);
        return true;
    }
    return false;
}

/* --- AccessX Keyboard Filtering (Sticky Keys, Slow Keys, Bounce Keys) --- */

static DtwmKeyTrackSlot *get_key_slot(DtwmA11yState *a11y, uint32_t keycode, bool create) {
    int free_idx = -1;
    int oldest_idx = 0;
    uint32_t oldest_ms = UINT32_MAX;
    for (int i = 0; i < DTWM_A11Y_KEY_SLOTS; ++i) {
        if (a11y->key_slots[i].in_use) {
            if (a11y->key_slots[i].keycode == keycode) {
                return &a11y->key_slots[i];
            }
            if (a11y->key_slots[i].last_release_ms < oldest_ms) {
                oldest_ms = a11y->key_slots[i].last_release_ms;
                oldest_idx = i;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (!create) return nullptr;
    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    memset(&a11y->key_slots[idx], 0, sizeof(a11y->key_slots[idx]));
    a11y->key_slots[idx].in_use = true;
    a11y->key_slots[idx].keycode = keycode;
    return &a11y->key_slots[idx];
}

static uint32_t classify_modifier_keycode(struct wlr_keyboard *wlr_kbd, uint32_t raw_keycode) {
    if (!wlr_kbd || !wlr_kbd->keymap || !wlr_kbd->xkb_state) return 0;
    xkb_keycode_t xkc = raw_keycode + 8;
    xkb_layout_index_t layout = xkb_state_key_get_layout(wlr_kbd->xkb_state, xkc);
    const xkb_keysym_t *syms = nullptr;
    int nsyms = xkb_keymap_key_get_syms_by_level(wlr_kbd->keymap, xkc, layout, 0, &syms);
    for (int i = 0; i < nsyms; ++i) {
        switch (syms[i]) {
        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R:
            return WLR_MODIFIER_SHIFT;
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R:
            return WLR_MODIFIER_CTRL;
        case XKB_KEY_Alt_L:
        case XKB_KEY_Alt_R:
        case XKB_KEY_Meta_L:
        case XKB_KEY_Meta_R:
            return WLR_MODIFIER_ALT;
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R:
        case XKB_KEY_Hyper_L:
        case XKB_KEY_Hyper_R:
            return WLR_MODIFIER_LOGO;
        default:
            break;
        }
    }
    return 0;
}

static void update_held_mod_keys(DtwmA11yState *a11y, uint32_t keycode, uint32_t mod_bit, bool pressed) {
    if (pressed) {
        int free_idx = -1;
        for (int i = 0; i < DTWM_A11Y_HELD_MODS_MAX; ++i) {
            if (a11y->held_mod_keys[i].held && a11y->held_mod_keys[i].keycode == keycode) {
                a11y->held_mod_keys[i].mod_bit = mod_bit;
                free_idx = -2;
                break;
            }
            if (!a11y->held_mod_keys[i].held && free_idx == -1) {
                free_idx = i;
            }
        }
        if (free_idx >= 0) {
            a11y->held_mod_keys[free_idx].held = true;
            a11y->held_mod_keys[free_idx].keycode = keycode;
            a11y->held_mod_keys[free_idx].mod_bit = mod_bit;
        }
    } else {
        for (int i = 0; i < DTWM_A11Y_HELD_MODS_MAX; ++i) {
            if (a11y->held_mod_keys[i].held && a11y->held_mod_keys[i].keycode == keycode) {
                a11y->held_mod_keys[i].held = false;
            }
        }
    }
    uint32_t mask = 0;
    for (int i = 0; i < DTWM_A11Y_HELD_MODS_MAX; ++i) {
        if (a11y->held_mod_keys[i].held) {
            mask |= a11y->held_mod_keys[i].mod_bit;
        }
    }
    a11y->phys_mods = mask;
}

bool dtwm_a11y_sticky_keys_enabled(const DtwmServer *server) {
    return server && server->a11y && server->a11y->sticky_keys;
}

uint32_t dtwm_a11y_effective_modifiers(const DtwmServer *server, struct wlr_keyboard *wlr_kbd) {
    uint32_t base = wlr_kbd ? wlr_keyboard_get_modifiers(wlr_kbd) : 0u;
    if (!server || !server->a11y || !server->a11y->sticky_keys) {
        return base;
    }
    const uint32_t managed =
        WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
    return (base & ~managed) | server->a11y->phys_mods | server->a11y->latched_mods |
           server->a11y->locked_mods;
}

static void build_seat_modifiers(
    const DtwmServer *server,
    struct wlr_keyboard *wlr_kbd,
    struct wlr_keyboard_modifiers *out_mods
) {
    memset(out_mods, 0, sizeof(*out_mods));
    if (wlr_kbd) {
        *out_mods = wlr_kbd->modifiers;
    }
    if (!server || !server->a11y || !server->a11y->sticky_keys || !wlr_kbd || !wlr_kbd->keymap) {
        return;
    }

    /* Map WLR_MODIFIER_* bits to the keyboard's real xkb_mod_mask_t bit indices so both
     * native Wayland clients and XWayland see exact depressed/locked modifier masks. */
    static const struct {
        uint32_t wlr_bit;
        const char *xkb_name;
    } mod_map[] = {
        { WLR_MODIFIER_SHIFT, XKB_MOD_NAME_SHIFT },
        { WLR_MODIFIER_CTRL, XKB_MOD_NAME_CTRL },
        { WLR_MODIFIER_ALT, XKB_MOD_NAME_ALT },
        { WLR_MODIFIER_LOGO, XKB_MOD_NAME_LOGO },
    };

    xkb_mod_mask_t managed_xkb_mask = 0;
    xkb_mod_mask_t active_xkb_mask = 0;
    uint32_t eff = server->a11y->phys_mods | server->a11y->latched_mods | server->a11y->locked_mods;

    for (size_t i = 0; i < sizeof(mod_map) / sizeof(mod_map[0]); ++i) {
        xkb_mod_index_t idx = xkb_keymap_mod_get_index(wlr_kbd->keymap, mod_map[i].xkb_name);
        if (idx != XKB_MOD_INVALID) {
            xkb_mod_mask_t bit = (xkb_mod_mask_t)1u << idx;
            managed_xkb_mask |= bit;
            if (eff & mod_map[i].wlr_bit) {
                active_xkb_mask |= bit;
            }
        }
    }

    out_mods->depressed = (out_mods->depressed & ~managed_xkb_mask) | active_xkb_mask;
    out_mods->latched &= ~managed_xkb_mask;
}

void dtwm_a11y_notify_seat_modifiers(DtwmServer *server, struct wlr_keyboard *wlr_kbd) {
    if (!server || !server->seat || !wlr_kbd) return;
    struct wlr_keyboard_modifiers mods;
    build_seat_modifiers(server, wlr_kbd, &mods);
    if (!dtwm_ime_forward_modifiers(server, wlr_kbd)) {
        wlr_seat_keyboard_notify_modifiers(server->seat, &mods);
    }
}

void dtwm_a11y_notify_keyboard_enter(
    DtwmServer *server, struct wlr_surface *surface, struct wlr_keyboard *wlr_kbd
) {
    if (!server || !server->seat || !surface) return;
    if (!wlr_kbd) {
        wlr_seat_keyboard_notify_enter(server->seat, surface, nullptr, 0, nullptr);
        return;
    }
    struct wlr_keyboard_modifiers mods;
    build_seat_modifiers(server, wlr_kbd, &mods);
    wlr_seat_keyboard_notify_enter(
        server->seat, surface, wlr_kbd->keycodes, wlr_kbd->num_keycodes, &mods
    );
}

static void deliver_accepted_nonmod_press(
    DtwmServer *server, struct wlr_keyboard *wlr_kbd, uint32_t raw_keycode, uint32_t time_msec
) {
    if (!server || !server->a11y || !wlr_kbd) return;
    DtwmA11yState *a11y = server->a11y;
    DtwmKeyTrackSlot *slot = get_key_slot(a11y, raw_keycode, true);

    dtwm_a11y_notify_seat_modifiers(server, wlr_kbd);
    uint32_t eff_mods = dtwm_a11y_effective_modifiers(server, wlr_kbd);

    bool handled = false;
    if (wlr_kbd->keymap && wlr_kbd->xkb_state) {
        xkb_keycode_t xkc = raw_keycode + 8;
        xkb_layout_index_t layout = xkb_state_key_get_layout(wlr_kbd->xkb_state, xkc);
        xkb_level_index_t level = (eff_mods & WLR_MODIFIER_SHIFT) ? 1 : 0;
        const xkb_keysym_t *syms = nullptr;
        int nsyms = xkb_keymap_key_get_syms_by_level(wlr_kbd->keymap, xkc, layout, level, &syms);
        if (nsyms <= 0 && level != 0) {
            nsyms = xkb_keymap_key_get_syms_by_level(wlr_kbd->keymap, xkc, layout, 0, &syms);
        }
        for (int i = 0; i < nsyms; ++i) {
            if (dtwm_server_dispatch_keybinding(server, eff_mods, syms[i])) {
                handled = true;
                break;
            }
        }
    }

    if (handled) {
        if (slot) slot->consumed_by_shortcut = true;
    } else {
        if (slot) slot->consumed_by_shortcut = false;
        struct wlr_keyboard_key_event kev = {
            .time_msec = time_msec,
            .keycode = raw_keycode,
            .update_state = true,
            .state = WL_KEYBOARD_KEY_STATE_PRESSED,
        };
        wlr_seat_set_keyboard(server->seat, wlr_kbd);
        if (!dtwm_ime_forward_key(server, wlr_kbd, &kev)) {
            wlr_seat_keyboard_notify_key(
                server->seat, time_msec, raw_keycode, WL_KEYBOARD_KEY_STATE_PRESSED
            );
        }
    }

    /* Sticky Keys unlatches latched modifiers immediately after the next non-modifier press */
    if (a11y->sticky_keys && a11y->latched_mods != 0) {
        a11y->latched_mods = 0;
        dtwm_a11y_notify_seat_modifiers(server, wlr_kbd);
        dtwm_a11y_publish_state(server);
    }
}

static int handle_slow_key_timer(void *data) {
    DtwmSlowKeySlot *ps = data;
    if (!ps || !ps->active || ps->accepted || !ps->server) return 0;
    ps->accepted = true;
    deliver_accepted_nonmod_press(ps->server, ps->wlr_kbd, ps->keycode, a11y_now_msec());
    return 0;
}

static DtwmSlowKeySlot *find_slow_slot(DtwmA11yState *a11y, uint32_t keycode, bool alloc_free) {
    int free_idx = -1;
    for (int i = 0; i < DTWM_A11Y_SLOW_SLOTS; ++i) {
        if (a11y->slow_slots[i].active && a11y->slow_slots[i].keycode == keycode) {
            return &a11y->slow_slots[i];
        }
        if (!a11y->slow_slots[i].active && free_idx < 0) {
            free_idx = i;
        }
    }
    if (!alloc_free || free_idx < 0) return nullptr;
    return &a11y->slow_slots[free_idx];
}

bool dtwm_a11y_filter_key(
    DtwmServer *server,
    struct wlr_keyboard *wlr_kbd,
    const struct wlr_keyboard_key_event *ev,
    uint32_t time_msec
) {
    if (!server || !server->a11y || !wlr_kbd || !ev) return false;
    DtwmA11yState *a11y = server->a11y;
    if (!a11y->sticky_keys && !a11y->slow_keys && !a11y->bounce_keys) {
        return false;
    }

    uint32_t mod_bit = classify_modifier_keycode(wlr_kbd, ev->keycode);
    DtwmKeyTrackSlot *slot = get_key_slot(a11y, ev->keycode, true);

    /* 1. Bounce Keys filter */
    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (a11y->bounce_keys && a11y->bounce_keys_ms > 0 && slot && slot->has_release) {
            uint32_t elapsed = time_msec - slot->last_release_ms;
            if (elapsed < (uint32_t)a11y->bounce_keys_ms) {
                slot->bounce_rejected = true;
                return true;
            }
        }
        if (slot) slot->bounce_rejected = false;
    } else if (ev->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (slot && slot->bounce_rejected) {
            slot->bounce_rejected = false;
            return true;
        }
    }

    /* 2. Modifier keys (Sticky Keys state machine) */
    if (mod_bit != 0) {
        uint32_t prior_phys = a11y->phys_mods;
        update_held_mod_keys(
            a11y, ev->keycode, mod_bit, ev->state == WL_KEYBOARD_KEY_STATE_PRESSED
        );

        if (!a11y->sticky_keys) {
            if (ev->state == WL_KEYBOARD_KEY_STATE_RELEASED && slot) {
                slot->last_release_ms = time_msec;
                slot->has_release = true;
            }
            wlr_seat_set_keyboard(server->seat, wlr_kbd);
            if (!dtwm_ime_forward_key(server, wlr_kbd, ev)) {
                wlr_seat_keyboard_notify_key(server->seat, time_msec, ev->keycode, ev->state);
            }
            return true;
        }

        server->super_tap_pending = false;

        if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            if ((prior_phys & ~mod_bit) != 0) {
                a11y->chord_used_while_mod_held = true;
                if (a11y->sticky_two_keys_off) {
                    a11y->sticky_keys = false;
                    a11y->latched_mods = 0;
                    a11y->locked_mods = 0;
                    dtwm_a11y_notify_seat_modifiers(server, wlr_kbd);
                    dtwm_a11y_publish_state(server);
                    return true;
                }
            } else if (prior_phys == 0) {
                a11y->chord_used_while_mod_held = false;
            }

            if (a11y->locked_mods & mod_bit) {
                a11y->locked_mods &= ~mod_bit;
                a11y->latched_mods &= ~mod_bit;
                a11y->mod_handled_on_press |= mod_bit;
            } else if (a11y->latched_mods & mod_bit) {
                a11y->latched_mods &= ~mod_bit;
                a11y->locked_mods |= mod_bit;
                a11y->mod_handled_on_press |= mod_bit;
            } else {
                a11y->mod_handled_on_press &= ~mod_bit;
            }

            dtwm_a11y_notify_seat_modifiers(server, wlr_kbd);
            dtwm_a11y_publish_state(server);
            return true;
        } else {
            if (slot) {
                slot->last_release_ms = time_msec;
                slot->has_release = true;
            }
            if (!(a11y->mod_handled_on_press & mod_bit) && !a11y->chord_used_while_mod_held) {
                a11y->latched_mods |= mod_bit;
            }
            a11y->mod_handled_on_press &= ~mod_bit;
            if (a11y->phys_mods == 0) {
                a11y->chord_used_while_mod_held = false;
            }
            dtwm_a11y_notify_seat_modifiers(server, wlr_kbd);
            dtwm_a11y_publish_state(server);
            return true;
        }
    }

    /* 3. Non-modifier keys (Sticky Keys chord detection + Slow Keys hold filter) */
    server->super_tap_pending = false;

    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (a11y->sticky_keys && a11y->phys_mods != 0) {
            a11y->chord_used_while_mod_held = true;
            if (a11y->sticky_two_keys_off) {
                a11y->sticky_keys = false;
                a11y->latched_mods = 0;
                a11y->locked_mods = 0;
                dtwm_a11y_notify_seat_modifiers(server, wlr_kbd);
                dtwm_a11y_publish_state(server);
            }
        }

        if (a11y->slow_keys && a11y->slow_keys_ms > 0) {
            DtwmSlowKeySlot *ps = find_slow_slot(a11y, ev->keycode, true);
            if (ps && ps->timer) {
                ps->wlr_kbd = wlr_kbd;
                ps->keycode = ev->keycode;
                ps->active = true;
                ps->accepted = false;
                wl_event_source_timer_update(ps->timer, a11y->slow_keys_ms);
                return true;
            }
        }

        deliver_accepted_nonmod_press(server, wlr_kbd, ev->keycode, time_msec);
        return true;
    }

    if (ev->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        DtwmSlowKeySlot *ps = find_slow_slot(a11y, ev->keycode, false);
        if (ps && ps->active) {
            bool was_accepted = ps->accepted;
            wl_event_source_timer_update(ps->timer, 0);
            ps->active = false;
            ps->accepted = false;
            if (!was_accepted) {
                /* Key released before slow_keys_ms acceptance delay -> cancel press */
                return true;
            }
        }

        if (slot) {
            slot->last_release_ms = time_msec;
            slot->has_release = true;
            if (slot->consumed_by_shortcut) {
                slot->consumed_by_shortcut = false;
                return true;
            }
        }

        wlr_seat_set_keyboard(server->seat, wlr_kbd);
        if (!dtwm_ime_forward_key(server, wlr_kbd, ev)) {
            wlr_seat_keyboard_notify_key(
                server->seat, time_msec, ev->keycode, WL_KEYBOARD_KEY_STATE_RELEASED
            );
        }
        return true;
    }

    return true;
}

/* --- Magnifier Shortcuts, Super+Scroll & Output Render Pass --- */

static void step_magnifier_zoom(DtwmServer *server, int delta_pct) {
    if (!server || !server->a11y) return;
    int cur = server->a11y->active_zoom_pct;
    int next = cur + delta_pct;
    if (next < 125) next = 100;
    if (next > 400) next = 400;
    set_active_zoom_pct(server, next);
    dtwm_a11y_publish_state(server);
}

bool dtwm_a11y_handle_shortcut(DtwmServer *server, uint32_t mods, xkb_keysym_t sym) {
    if (!server || !server->a11y || !(mods & WLR_MODIFIER_LOGO)) {
        return false;
    }
    switch (sym) {
    case XKB_KEY_equal:
    case XKB_KEY_plus:
    case XKB_KEY_KP_Add:
        step_magnifier_zoom(server, 25);
        return true;
    case XKB_KEY_minus:
    case XKB_KEY_underscore:
    case XKB_KEY_KP_Subtract:
        step_magnifier_zoom(server, -25);
        return true;
    case XKB_KEY_0:
    case XKB_KEY_KP_0:
        set_active_zoom_pct(server, 100);
        dtwm_a11y_publish_state(server);
        return true;
    default:
        return false;
    }
}

bool dtwm_a11y_handle_axis(
    DtwmServer *server,
    uint32_t orientation,
    double delta,
    int32_t discrete
) {
    if (!server || !server->a11y || server->session_locked || server->greeter_mode) {
        return false;
    }
    if (orientation != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return false;
    }
    struct wlr_keyboard *kbd = server->seat ? wlr_seat_get_keyboard(server->seat) : nullptr;
    uint32_t mods = dtwm_a11y_effective_modifiers(server, kbd);
    if (!(mods & WLR_MODIFIER_LOGO)) {
        return false;
    }
    double dir = (discrete != 0) ? (double)discrete : delta;
    if (dir < 0.0) {
        step_magnifier_zoom(server, 25);
    } else if (dir > 0.0) {
        step_magnifier_zoom(server, -25);
    }
    return true;
}

int dtwm_a11y_get_zoom_pct(const DtwmServer *server) {
    return (server && server->a11y) ? server->a11y->active_zoom_pct : 100;
}

void dtwm_a11y_on_cursor_motion(DtwmServer *server) {
    if (!server || !server->a11y || server->a11y->active_zoom_pct <= 100) {
        return;
    }
    schedule_all_outputs(server, false);
}

void dtwm_a11y_on_output_destroy(DtwmOutput *output) {
    if (!output) return;
    if (output->mag_unmag_buf) {
        wlr_buffer_unlock(output->mag_unmag_buf);
        output->mag_unmag_buf = nullptr;
    }
    if (output->mag_swapchain) {
        wlr_swapchain_destroy(output->mag_swapchain);
        output->mag_swapchain = nullptr;
    }
}

bool dtwm_a11y_render_output_frame(DtwmOutput *output, struct wlr_scene_output *scene_output) {
    if (!output || !scene_output || !output->server || !output->server->a11y) {
        return false;
    }
    DtwmServer *server = output->server;
    int zoom_pct = server->a11y->active_zoom_pct;
    if (zoom_pct <= 100) {
        return false;
    }

    struct wlr_output *wlr_output = output->wlr_output;
    if (!wlr_output_configure_primary_swapchain(wlr_output, nullptr, &output->mag_swapchain) ||
        !output->mag_swapchain) {
        return false;
    }

    if (!output->mag_unmag_buf && output->bg_rect) {
        wlr_scene_node_set_enabled(&output->bg_rect->node, false);
        wlr_scene_node_set_enabled(&output->bg_rect->node, true);
    }

    struct wlr_output_state off_state;
    wlr_output_state_init(&off_state);
    struct wlr_scene_output_state_options opts = {
        .swapchain = output->mag_swapchain,
    };
    if (wlr_scene_output_build_state(scene_output, &off_state, &opts) && off_state.buffer) {
        if (output->mag_unmag_buf) {
            wlr_buffer_unlock(output->mag_unmag_buf);
        }
        output->mag_unmag_buf = wlr_buffer_lock(off_state.buffer);
    }
    wlr_output_state_finish(&off_state);

    if (!output->mag_unmag_buf) {
        return false;
    }

    struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, output->mag_unmag_buf);
    if (!tex) {
        return false;
    }

    int bw = output->mag_unmag_buf->width;
    int bh = output->mag_unmag_buf->height;
    struct wlr_box obox = {0, 0, bw, bh};
    wlr_output_layout_get_box(server->output_layout, wlr_output, &obox);

    double rel_x = (double)bw * 0.5;
    double rel_y = (double)bh * 0.5;
    if (server->cursor && obox.width > 0 && obox.height > 0) {
        rel_x = (server->cursor->x - (double)obox.x) * (double)bw / (double)obox.width;
        rel_y = (server->cursor->y - (double)obox.y) * (double)bh / (double)obox.height;
    }
    if (rel_x < 0.0) rel_x = 0.0;
    if (rel_x > (double)bw) rel_x = (double)bw;
    if (rel_y < 0.0) rel_y = 0.0;
    if (rel_y > (double)bh) rel_y = (double)bh;

    double z = (double)zoom_pct / 100.0;
    double src_w = (double)bw / z;
    double src_h = (double)bh / z;
    double src_x = rel_x - src_w * 0.5;
    double src_y = rel_y - src_h * 0.5;
    if (src_x < 0.0) src_x = 0.0;
    if (src_y < 0.0) src_y = 0.0;
    if (src_x + src_w > (double)bw) src_x = (double)bw - src_w;
    if (src_y + src_h > (double)bh) src_y = (double)bh - src_h;

    bool committed = false;
    struct wlr_output_state out_state;
    wlr_output_state_init(&out_state);
    struct wlr_render_pass *pass = wlr_output_begin_render_pass(wlr_output, &out_state, nullptr);
    if (pass) {
        wlr_render_pass_add_texture(
            pass,
            &(struct wlr_render_texture_options){
                .texture = tex,
                .src_box = { .x = src_x, .y = src_y, .width = src_w, .height = src_h },
                .dst_box = { .x = 0, .y = 0, .width = bw, .height = bh },
                .filter_mode = WLR_SCALE_FILTER_NEAREST,
                .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
            }
        );
        if (wlr_render_pass_submit(pass)) {
            committed = wlr_output_commit_state(wlr_output, &out_state);
        }
    }
    wlr_output_state_finish(&out_state);
    wlr_texture_destroy(tex);
    return committed;
}

/* --- _DT_WM_REQUEST IPC Handler --- */

bool dtwm_a11y_handle_wm_request(DtwmServer *server, const char *cmd) {
    if (!server || !server->a11y || !cmd) return false;
    DtwmA11yState *a11y = server->a11y;

    if (strncmp(cmd, "f.visual_bell", 13) == 0 &&
        (cmd[13] == '\0' || isspace((unsigned char)cmd[13]))) {
        dtwm_a11y_trigger_visual_bell(server);
        return true;
    }

    if (strncmp(cmd, "f.reload_accessibility", 22) == 0 &&
        (cmd[22] == '\0' || isspace((unsigned char)cmd[22]))) {
        load_accessibility_conf(server);
        struct wlr_keyboard *kbd = server->seat ? wlr_seat_get_keyboard(server->seat) : nullptr;
        if (kbd) dtwm_a11y_notify_seat_modifiers(server, kbd);
        dtwm_a11y_publish_state(server);
        return true;
    }

    if (strncmp(cmd, "f.magnifier_zoom", 16) == 0 &&
        (cmd[16] == '\0' || isspace((unsigned char)cmd[16]))) {
        const char *arg = cmd + 16;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        if (strcasecmp(arg, "in") == 0) {
            step_magnifier_zoom(server, 25);
        } else if (strcasecmp(arg, "out") == 0) {
            step_magnifier_zoom(server, -25);
        } else if (strcasecmp(arg, "reset") == 0) {
            set_active_zoom_pct(server, 100);
            dtwm_a11y_publish_state(server);
        } else if (*arg) {
            set_active_zoom_pct(server, atoi(arg));
            dtwm_a11y_publish_state(server);
        }
        return true;
    }

    if (strncmp(cmd, "f.set_accessibility", 19) == 0 &&
        (cmd[19] == '\0' || isspace((unsigned char)cmd[19]))) {
        char copy[512];
        snprintf(copy, sizeof(copy), "%s", cmd + 19);

        bool saw_mag_flag = false;
        bool mag_flag_val = false;
        bool saw_zoom = false;
        int zoom_val = a11y->active_zoom_pct;

        char *saveptr = nullptr;
        for (char *tok = strtok_r(copy, " \t\r\n", &saveptr); tok;
             tok = strtok_r(nullptr, " \t\r\n", &saveptr)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *k = tok;
            const char *v = eq + 1;

            if (strcasecmp(k, "sticky") == 0 || strcasecmp(k, "sticky_keys") == 0) {
                a11y->sticky_keys = parse_bool_str(v, a11y->sticky_keys);
                if (!a11y->sticky_keys) {
                    a11y->latched_mods = 0;
                    a11y->locked_mods = 0;
                    a11y->mod_handled_on_press = 0;
                }
            } else if (strcasecmp(k, "two_keys_off") == 0 ||
                       strcasecmp(k, "sticky_two_keys_off") == 0) {
                a11y->sticky_two_keys_off = parse_bool_str(v, a11y->sticky_two_keys_off);
            } else if (strcasecmp(k, "clear_latches") == 0 && parse_bool_str(v, false)) {
                a11y->latched_mods = 0;
                a11y->locked_mods = 0;
                a11y->mod_handled_on_press = 0;
            } else if (strcasecmp(k, "slow") == 0 || strcasecmp(k, "slow_keys") == 0) {
                a11y->slow_keys = parse_bool_str(v, a11y->slow_keys);
                if (!a11y->slow_keys) {
                    for (int i = 0; i < DTWM_A11Y_SLOW_SLOTS; ++i) {
                        if (a11y->slow_slots[i].active && a11y->slow_slots[i].timer) {
                            wl_event_source_timer_update(a11y->slow_slots[i].timer, 0);
                        }
                        a11y->slow_slots[i].active = false;
                        a11y->slow_slots[i].accepted = false;
                    }
                }
            } else if (strcasecmp(k, "slow_ms") == 0 || strcasecmp(k, "slow_keys_ms") == 0) {
                a11y->slow_keys_ms = clamp_int(atoi(v), 50, 2000);
            } else if (strcasecmp(k, "bounce") == 0 || strcasecmp(k, "bounce_keys") == 0) {
                a11y->bounce_keys = parse_bool_str(v, a11y->bounce_keys);
                if (!a11y->bounce_keys) {
                    memset(a11y->key_slots, 0, sizeof(a11y->key_slots));
                }
            } else if (strcasecmp(k, "bounce_ms") == 0 || strcasecmp(k, "bounce_keys_ms") == 0) {
                a11y->bounce_keys_ms = clamp_int(atoi(v), 50, 2000);
            } else if (strcasecmp(k, "vbell") == 0 || strcasecmp(k, "visual_bell") == 0) {
                a11y->vbell_mode = parse_vbell_mode(v);
            } else if (strcasecmp(k, "reduced_motion") == 0) {
                apply_reduced_motion(server, parse_bool_str(v, server->reduced_motion));
            } else if (strcasecmp(k, "magnifier") == 0) {
                saw_mag_flag = true;
                mag_flag_val = parse_bool_str(v, false);
            } else if (strcasecmp(k, "zoom_pct") == 0) {
                saw_zoom = true;
                zoom_val = atoi(v);
            }
        }

        if (saw_mag_flag) {
            if (saw_zoom && zoom_val >= 125) {
                a11y->configured_zoom_pct = clamp_int(zoom_val, 125, 400);
            }
            set_active_zoom_pct(server, mag_flag_val ? a11y->configured_zoom_pct : 100);
        } else if (saw_zoom) {
            set_active_zoom_pct(server, zoom_val);
        }

        struct wlr_keyboard *kbd = server->seat ? wlr_seat_get_keyboard(server->seat) : nullptr;
        if (kbd) {
            dtwm_a11y_notify_seat_modifiers(server, kbd);
        }
        dtwm_a11y_publish_state(server);
        return true;
    }

    return false;
}

/* --- Lifecycle --- */

void dtwm_a11y_on_xwayland_ready(DtwmServer *server) {
    if (!server || !server->a11y || !server->xcb_conn) return;
    server->atom_accessibility_state = intern_xcb_atom(server->xcb_conn, "_DT_ACCESSIBILITY_STATE");
    subscribe_xwayland_xkb_bell(server);
    dtwm_a11y_publish_state(server);
}

void dtwm_a11y_init(DtwmServer *server) {
    if (!server) return;
    DtwmA11yState *a11y = calloc(1, sizeof(*a11y));
    if (!a11y) return;
    a11y->server = server;
    a11y->sticky_two_keys_off = true;
    a11y->slow_keys_ms = 300;
    a11y->bounce_keys_ms = 300;
    a11y->vbell_mode = DTWM_VBELL_OFF;
    a11y->configured_zoom_pct = 200;
    a11y->active_zoom_pct = 100;
    server->a11y = a11y;

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    for (int i = 0; i < DTWM_A11Y_SLOW_SLOTS; ++i) {
        a11y->slow_slots[i].server = server;
        a11y->slow_slots[i].timer =
            wl_event_loop_add_timer(loop, handle_slow_key_timer, &a11y->slow_slots[i]);
    }

    if (server->layer_overlay_tree) {
        const float flash_rgba[4] = { 1.0f, 0.96f, 0.78f, 0.88f };
        a11y->vbell_rect = wlr_scene_rect_create(server->layer_overlay_tree, 1, 1, flash_rgba);
        if (a11y->vbell_rect) {
            wlr_scene_node_set_enabled(&a11y->vbell_rect->node, false);
        }
    }
    a11y->vbell_timer = wl_event_loop_add_timer(loop, handle_vbell_timer, server);
    a11y->xdg_bell_global = wl_global_create(
        server->wl_display, &xdg_system_bell_v1_interface, 1, server, bind_xdg_system_bell
    );

    load_accessibility_conf(server);
}

void dtwm_a11y_finish(DtwmServer *server) {
    if (!server || !server->a11y) return;
    DtwmA11yState *a11y = server->a11y;
    server->a11y = nullptr;

    for (int i = 0; i < DTWM_A11Y_SLOW_SLOTS; ++i) {
        if (a11y->slow_slots[i].timer) {
            wl_event_source_remove(a11y->slow_slots[i].timer);
        }
    }
    if (a11y->vbell_timer) {
        wl_event_source_remove(a11y->vbell_timer);
    }
    if (a11y->xdg_bell_global) {
        wl_global_destroy(a11y->xdg_bell_global);
    }
    DtwmOutput *dout;
    wl_list_for_each(dout, &server->outputs, link) {
        dtwm_a11y_on_output_destroy(dout);
    }
    free(a11y);
}
