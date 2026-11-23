/* Virtual pointer input for isolated headless CDE integration tests. */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

extern const struct wl_interface zwlr_virtual_pointer_v1_interface;
static const struct wl_interface *vptr_mgr_types[] = {
    &wl_seat_interface, &zwlr_virtual_pointer_v1_interface,
};
static const struct wl_message vptr_mgr_requests[] = {
    { "create_virtual_pointer", "?on", vptr_mgr_types },
};
static const struct wl_interface zwlr_virtual_pointer_manager_v1_interface = {
    "zwlr_virtual_pointer_manager_v1", 1, 1, vptr_mgr_requests, 0, NULL,
};
static const struct wl_interface *vptr_null_types[] = { NULL, NULL, NULL, NULL, NULL };
static const struct wl_message vptr_requests[] = {
    { "motion", "uff", vptr_null_types },
    { "motion_absolute", "uuuuu", vptr_null_types },
    { "button", "uuu", vptr_null_types },
    { "axis", "uuf", vptr_null_types },
    { "frame", "", vptr_null_types },
    { "axis_source", "u", vptr_null_types },
    { "axis_stop", "uu", vptr_null_types },
    { "axis_discrete", "uufi", vptr_null_types },
    { "destroy", "", vptr_null_types },
};
const struct wl_interface zwlr_virtual_pointer_v1_interface = {
    "zwlr_virtual_pointer_v1", 1, 9, vptr_requests, 0, NULL,
};

typedef struct {
    struct wl_seat *seat;
    struct wl_proxy *vptr_mgr;
} InputState;

static void on_global(void *d, struct wl_registry *r, uint32_t n,
                      const char *iface, uint32_t v) {
    InputState *st = (InputState *)d;
    (void)v;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !st->seat) {
        st->seat = (struct wl_seat *)wl_registry_bind(r, n, &wl_seat_interface, 1);
    } else if (strcmp(iface, "zwlr_virtual_pointer_manager_v1") == 0 && !st->vptr_mgr) {
        st->vptr_mgr = (struct wl_proxy *)wl_registry_bind(
            r, n, &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}
static void on_rem(void *d, struct wl_registry *r, uint32_t n) {
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener rl = { on_global, on_rem };

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

static void vp_move(struct wl_proxy *vp, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h) {
    wl_proxy_marshal(vp, 1, now_ms(), x, y, w, h);
    wl_proxy_marshal(vp, 4);
}

static void vp_btn(struct wl_proxy *vp, uint32_t b, uint32_t s) {
    wl_proxy_marshal(vp, 2, now_ms(), b, s);
    wl_proxy_marshal(vp, 4);
}

int main(void) {
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) return 2;
    InputState st = {0};
    struct wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &rl, &st);
    wl_display_roundtrip(d);
    if (!st.seat || !st.vptr_mgr) return 3;
    struct wl_proxy *vp = wl_proxy_marshal_constructor(
        st.vptr_mgr, 0, &zwlr_virtual_pointer_v1_interface, st.seat, NULL);
    wl_display_roundtrip(d);
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned a = 0, b = 0, c = 0, e = 0;
        if (sscanf(line, "move %u %u %u %u", &a, &b, &c, &e) == 4) {
            vp_move(vp, a, b, c, e);
        } else if (sscanf(line, "down %u", &a) == 1) {
            vp_btn(vp, a, 1);
        } else if (sscanf(line, "up %u", &a) == 1) {
            vp_btn(vp, a, 0);
        } else if (sscanf(line, "sleep %u", &a) == 1) {
            usleep(a * 1000);
        } else if (strncmp(line, "quit", 4) == 0) {
            break;
        } else {
            return 4;
        }
        if (wl_display_roundtrip(d) < 0) return 5;
        printf("OK\n");
        fflush(stdout);
    }
    return 0;
}
