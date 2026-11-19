/*
 * CK Core CDE - Wayland Session Bootstrap, ext-session-lock-v1, and
 * ext-idle-notify-v1 integration for dtsession (WP-06)
 */

#define _GNU_SOURCE

#include "SmWayland.h"
#include "Sm.h"
#include "SmGlobals.h"
#include "SmLock.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>

#ifdef BUILD_DTWM_WAYLAND
#include <wayland-client.h>
#include "ext-session-lock-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"

typedef struct SmWlOutputNode {
    uint32_t global_name;
    struct wl_output *wl_output;
    struct ext_session_lock_surface_v1 *lock_surface;
    struct wl_surface *surface;
    struct wl_buffer *buffer;
    uint32_t width;
    uint32_t height;
    struct SmWlOutputNode *next;
} SmWlOutputNode;

static struct {
    Boolean wayland_mode;
    pid_t compositor_pid;
    char status_path[512];

    XtAppContext app;
    Display *x_dpy;
    XtInputId xt_input_id;

    struct wl_display *wl_dpy;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct ext_session_lock_manager_v1 *lock_mgr;
    struct ext_idle_notifier_v1 *idle_mgr;

    SmWlOutputNode *outputs;

    struct ext_session_lock_v1 *active_lock;
    Boolean lock_ack_locked;
    Boolean lock_finished;

    struct ext_idle_notification_v1 *idle_notif;
} g_smwl;

static int create_anon_shm_fd(size_t size) {
    char name[64];
    snprintf(name, sizeof(name), "/dtsession-lock-%d-%ld", (int)getpid(), (long)random());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static struct wl_buffer *create_black_shm_buffer(uint32_t width, uint32_t height) {
    if (!g_smwl.shm || width == 0 || height == 0) return nullptr;
    size_t stride = (size_t)width * 4u;
    size_t size = stride * (size_t)height;
    int fd = create_anon_shm_fd(size);
    if (fd < 0) return nullptr;

    uint32_t *px = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) {
        px[i] = 0xFF000000u;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(g_smwl.shm, fd, (int32_t)size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, (int32_t)width, (int32_t)height, (int32_t)stride, WL_SHM_FORMAT_ARGB8888
    );
    wl_shm_pool_destroy(pool);
    munmap(px, size);
    close(fd);
    return buf;
}

static void lock_surf_configure(
    void *data, struct ext_session_lock_surface_v1 *lock_surf,
    uint32_t serial, uint32_t width, uint32_t height
) {
    SmWlOutputNode *node = data;
    ext_session_lock_surface_v1_ack_configure(lock_surf, serial);
    if (!node || !node->surface) return;

    if (width == 0) width = 1280;
    if (height == 0) height = 900;
    if (node->buffer && node->width == width && node->height == height) {
        wl_surface_attach(node->surface, node->buffer, 0, 0);
        wl_surface_commit(node->surface);
        return;
    }
    if (node->buffer) {
        wl_buffer_destroy(node->buffer);
        node->buffer = nullptr;
    }
    node->width = width;
    node->height = height;
    node->buffer = create_black_shm_buffer(width, height);
    if (node->buffer) {
        wl_surface_attach(node->surface, node->buffer, 0, 0);
        wl_surface_damage_buffer(node->surface, 0, 0, (int32_t)width, (int32_t)height);
        wl_surface_commit(node->surface);
    }
}

static const struct ext_session_lock_surface_v1_listener g_lock_surf_listener = {
    .configure = lock_surf_configure,
};

static void attach_lock_surface_for_output(SmWlOutputNode *node) {
    if (!g_smwl.active_lock || !g_smwl.compositor || !node || node->lock_surface) {
        return;
    }
    node->surface = wl_compositor_create_surface(g_smwl.compositor);
    if (!node->surface) return;
    node->lock_surface = ext_session_lock_v1_get_lock_surface(
        g_smwl.active_lock, node->surface, node->wl_output
    );
    if (node->lock_surface) {
        ext_session_lock_surface_v1_add_listener(node->lock_surface, &g_lock_surf_listener, node);
    }
}

static void destroy_output_lock_surface(SmWlOutputNode *node) {
    if (!node) return;
    if (node->lock_surface) {
        ext_session_lock_surface_v1_destroy(node->lock_surface);
        node->lock_surface = nullptr;
    }
    if (node->surface) {
        wl_surface_destroy(node->surface);
        node->surface = nullptr;
    }
    if (node->buffer) {
        wl_buffer_destroy(node->buffer);
        node->buffer = nullptr;
    }
    node->width = 0;
    node->height = 0;
}

static void session_lock_locked(void *data, struct ext_session_lock_v1 *lock) {
    (void)data;
    (void)lock;
    g_smwl.lock_ack_locked = True;
}

static void session_lock_finished(void *data, struct ext_session_lock_v1 *lock) {
    (void)data;
    (void)lock;
    g_smwl.lock_finished = True;
    g_smwl.lock_ack_locked = False;
}

static const struct ext_session_lock_v1_listener g_session_lock_listener = {
    .locked = session_lock_locked,
    .finished = session_lock_finished,
};

static void idle_handle_idled(void *data, struct ext_idle_notification_v1 *notif) {
    (void)data;
    (void)notif;
    if (smGD.lockedState == UNLOCKED) {
        LockDisplay(False);
    }
}

static void idle_handle_resumed(void *data, struct ext_idle_notification_v1 *notif) {
    (void)data;
    (void)notif;
}

static const struct ext_idle_notification_v1_listener g_idle_listener = {
    .idled = idle_handle_idled,
    .resumed = idle_handle_resumed,
};

static void reg_global(
    void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver
) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_smwl.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_smwl.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        if (!g_smwl.seat) {
            g_smwl.seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
        }
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        SmWlOutputNode *node = calloc(1, sizeof(*node));
        if (node) {
            node->global_name = name;
            node->wl_output = wl_registry_bind(reg, name, &wl_output_interface, ver < 3 ? ver : 3);
            node->next = g_smwl.outputs;
            g_smwl.outputs = node;
            if (g_smwl.active_lock && !g_smwl.lock_finished) {
                attach_lock_surface_for_output(node);
                wl_display_flush(g_smwl.wl_dpy);
            }
        }
    } else if (strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
        g_smwl.lock_mgr = wl_registry_bind(reg, name, &ext_session_lock_manager_v1_interface, 1);
    } else if (strcmp(iface, ext_idle_notifier_v1_interface.name) == 0) {
        g_smwl.idle_mgr = wl_registry_bind(reg, name, &ext_idle_notifier_v1_interface, 1);
    }
}

static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data;
    (void)reg;
    SmWlOutputNode **pp = &g_smwl.outputs;
    while (*pp) {
        if ((*pp)->global_name == name) {
            SmWlOutputNode *victim = *pp;
            *pp = victim->next;
            destroy_output_lock_surface(victim);
            if (victim->wl_output) wl_output_destroy(victim->wl_output);
            free(victim);
            return;
        }
        pp = &(*pp)->next;
    }
}

static const struct wl_registry_listener g_reg_listener = {
    .global = reg_global,
    .global_remove = reg_global_remove,
};

static void xt_wayland_fd_cb(XtPointer closure, int *source, XtInputId *id) {
    (void)closure;
    (void)source;
    (void)id;
    if (!g_smwl.wl_dpy) return;
    if (wl_display_dispatch(g_smwl.wl_dpy) < 0) {
        if (g_smwl.xt_input_id != (XtInputId)0) {
            XtRemoveInput(g_smwl.xt_input_id);
            g_smwl.xt_input_id = (XtInputId)0;
        }
    } else {
        wl_display_flush(g_smwl.wl_dpy);
    }
}

static void ensure_ttsession_started(void) {
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    Atom sun_tt = XInternAtom(dpy, "_SUN_TT_SESSION", False);
    Atom dt_tt = XInternAtom(dpy, "_DT_TT_SESSION", False);
    Atom act = None;
    int fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *val = nullptr;

    if (XGetWindowProperty(dpy, root, sun_tt, 0, 256, False, AnyPropertyType,
                           &act, &fmt, &nitems, &rem, &val) != Success || !val || nitems == 0) {
        if (val) XFree(val);
        val = nullptr;
        /* Start ttsession -s so ToolTalk notices (DtActivity_Lock, ExitSession) work */
        if (!getenv("DTDATABASESEARCHPATH") || !getenv("DTDATABASESEARCHPATH")[0]) {
            setenv("DTDATABASESEARCHPATH",
                   CDE_INSTALLATION_TOP "/appconfig/types/%L," CDE_INSTALLATION_TOP "/appconfig/types/C",
                   1);
        }
        if (!getenv("DTHELPSEARCHPATH") || !getenv("DTHELPSEARCHPATH")[0]) {
            setenv("DTHELPSEARCHPATH",
                   CDE_INSTALLATION_TOP "/appconfig/help/%L/%H:"
                   CDE_INSTALLATION_TOP "/appconfig/help/%L/%H.sdl:"
                   CDE_INSTALLATION_TOP "/appconfig/help/%L/%H.hv:"
                   CDE_INSTALLATION_TOP "/appconfig/help/C/%H:"
                   CDE_INSTALLATION_TOP "/appconfig/help/C/%H.sdl:"
                   CDE_INSTALLATION_TOP "/appconfig/help/C/%H.hv",
                   1);
        }
        if (!getenv("DTINFOLIBSEARCHPATH") || !getenv("DTINFOLIBSEARCHPATH")[0]) {
            setenv("DTINFOLIBSEARCHPATH",
                   CDE_INSTALLATION_TOP "/infolib/%L/%I.dti:" CDE_INSTALLATION_TOP "/infolib/C/%I.dti",
                   1);
        }
        if (!getenv("DTINFOLIBDEFAULT") || !getenv("DTINFOLIBDEFAULT")[0]) {
            setenv("DTINFOLIBDEFAULT", "cde", 1);
        }
        const char *tt_bin = CDE_INSTALLATION_TOP "/bin/ttsession";
        if (access(tt_bin, X_OK) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                execl(tt_bin, "ttsession", "-s", (char *)nullptr);
                _exit(127);
            } else if (pid > 0) {
                int st = 0;
                (void)waitpid(pid, &st, 0);
            }
        }
        (void)XGetWindowProperty(dpy, root, sun_tt, 0, 256, False, AnyPropertyType,
                                 &act, &fmt, &nitems, &rem, &val);
        if ((!val || nitems == 0) && dt_tt != None) {
            if (val) XFree(val);
            val = nullptr;
            (void)XGetWindowProperty(dpy, root, dt_tt, 0, 256, False, AnyPropertyType,
                                     &act, &fmt, &nitems, &rem, &val);
            if (val && nitems > 0) {
                XChangeProperty(dpy, root, sun_tt, XA_STRING, 8, PropModeReplace, val, (int)nitems);
                XFlush(dpy);
            }
        }
    }

    if (val && nitems > 0) {
        char sess_buf[512];
        size_t cp = nitems < sizeof(sess_buf) - 1 ? (size_t)nitems : sizeof(sess_buf) - 1;
        memcpy(sess_buf, val, cp);
        sess_buf[cp] = '\0';
        setenv("TT_SESSION", sess_buf, 1);
    }
    if (val) XFree(val);
    XCloseDisplay(dpy);
}

static void spawn_compositor_if_needed(void) {
    const char *wl_env = getenv("WAYLAND_DISPLAY");
    const char *x_env = getenv("DISPLAY");
    if (wl_env && *wl_env && x_env && *x_env) {
        return;
    }

    const char *rt = getenv("XDG_RUNTIME_DIR");
    const char *home = getenv("HOME");
    char dir_buf[384];
    if (rt && *rt) {
        snprintf(dir_buf, sizeof(dir_buf), "%s", rt);
    } else if (home && *home) {
        snprintf(dir_buf, sizeof(dir_buf), "%s/.dt", home);
        (void)mkdir(dir_buf, 0700);
    } else {
        return;
    }

    snprintf(g_smwl.status_path, sizeof(g_smwl.status_path),
             "%s/dtsession-wayland-XXXXXX", dir_buf);
    int sfd = mkstemp(g_smwl.status_path);
    if (sfd < 0) {
        g_smwl.status_path[0] = '\0';
        return;
    }
    close(sfd);
    unlink(g_smwl.status_path);

    const char *comp_bin = getenv("DTWM_WAYLAND_BIN");
    if (!comp_bin || !*comp_bin) {
        comp_bin = CDE_INSTALLATION_TOP "/bin/dtwm-wayland";
    }

    pid_t pid = fork();
    if (pid == 0) {
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, nullptr);
        execl(comp_bin, "dtwm-wayland", "--status-file", g_smwl.status_path, (char *)nullptr);
        _exit(127);
    }
    if (pid < 0) return;
    g_smwl.compositor_pid = pid;

    struct timespec slp = {.tv_sec = 0, .tv_nsec = 100000000L};
    for (int i = 0; i < 100; ++i) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            g_smwl.compositor_pid = 0;
            return;
        }
        FILE *fp = fopen(g_smwl.status_path, "r");
        if (fp) {
            char line[256];
            char got_wl[128] = {0};
            char got_x[128] = {0};
            while (fgets(line, sizeof(line), fp)) {
                size_t l = strlen(line);
                while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
                if (strncmp(line, "WAYLAND_DISPLAY=", 16) == 0) {
                    snprintf(got_wl, sizeof(got_wl), "%s", line + 16);
                } else if (strncmp(line, "DISPLAY=", 8) == 0) {
                    snprintf(got_x, sizeof(got_x), "%s", line + 8);
                }
            }
            fclose(fp);
            if (got_wl[0] && got_x[0]) {
                setenv("WAYLAND_DISPLAY", got_wl, 1);
                setenv("DISPLAY", got_x, 1);
                setenv("XDG_SESSION_TYPE", "wayland", 1);
                if (!getenv("XDG_CURRENT_DESKTOP")) {
                    setenv("XDG_CURRENT_DESKTOP", "CDE", 1);
                }
                if (!getenv("XMODIFIERS")) {
                    if (access("/usr/bin/fcitx5", X_OK) == 0) {
                        setenv("XMODIFIERS", "@im=fcitx", 1);
                        if (!getenv("CDE_WAYLAND_IMS")) setenv("CDE_WAYLAND_IMS", "fcitx5", 1);
                    } else if (access("/usr/bin/ibus-daemon", X_OK) == 0) {
                        setenv("XMODIFIERS", "@im=ibus", 1);
                        if (!getenv("CDE_WAYLAND_IMS")) setenv("CDE_WAYLAND_IMS", "ibus", 1);
                    }
                }
                unlink(g_smwl.status_path);
                g_smwl.status_path[0] = '\0';
                /* Publish live WAYLAND_DISPLAY / DISPLAY to D-Bus & systemd user activation
                 * so xdg-desktop-portal / xdg-desktop-portal-wlr (WebRTC screen sharing),
                 * Secret Service (gnome-keyring-daemon), Polkit authentication (ck-polkit-agent),
                 * Wayland/XIM Input Methods (fcitx5 / ibus), and ck-status --daemon connect
                 * to the active Wayland & XWayland sockets. */
                pid_t spid = fork();
                if (spid == 0) {
                    if (fork() == 0) {
                        setsid();
                        execl(
                            "/bin/sh", "sh", "-c",
                            "if command -v gnome-keyring-daemon >/dev/null 2>&1; then "
                            "eval \"$(gnome-keyring-daemon --start --components=secrets,ssh 2>/dev/null)\" || true; "
                            "export SSH_AUTH_SOCK; "
                            "fi; "
                            "if command -v dbus-update-activation-environment >/dev/null 2>&1; then "
                            "dbus-update-activation-environment --systemd WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE XMODIFIERS GTK_IM_MODULE QT_IM_MODULE SSH_AUTH_SOCK >/dev/null 2>&1 || "
                            "dbus-update-activation-environment WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE XMODIFIERS GTK_IM_MODULE QT_IM_MODULE SSH_AUTH_SOCK >/dev/null 2>&1 || true; "
                            "fi; "
                            "case \"${CDE_WAYLAND_IMS:-}\" in "
                            "fcitx5|fcitx) command -v fcitx5 >/dev/null 2>&1 && fcitx5 -d >/dev/null 2>&1 & ;; "
                            "ibus) command -v ibus-daemon >/dev/null 2>&1 && ibus-daemon -drx >/dev/null 2>&1 & ;; "
                            "esac; "
                            "if command -v ck-polkit-agent >/dev/null 2>&1; then "
                            "ck-polkit-agent >/dev/null 2>&1 & "
                            "elif [ -x " CDE_INSTALLATION_TOP "/bin/ck-polkit-agent ]; then "
                            CDE_INSTALLATION_TOP "/bin/ck-polkit-agent >/dev/null 2>&1 & "
                            "fi; "
                            "if command -v ck-status >/dev/null 2>&1; then "
                            "ck-status --daemon >/dev/null 2>&1 & "
                            "fi; "
                            "_midi_cfg=\"${XDG_CONFIG_HOME:-$HOME/.config}/ck-core/midi.conf\"; "
                            "if [ -f \"$_midi_cfg\" ] && grep -q '^virtual_port=1' \"$_midi_cfg\" && command -v ck-midi >/dev/null 2>&1; then "
                            "ck-midi --daemon >/dev/null 2>&1 & "
                            "fi",
                            (char *)nullptr
                        );
                        _exit(0);
                    }
                    _exit(0);
                } else if (spid > 0) {
                    int sst = 0;
                    (void)waitpid(spid, &sst, 0);
                }
                break;
            }
        }
        nanosleep(&slp, nullptr);
    }
}

static void ensure_appmanager_gathered_wl(void) {
    const char *home = getenv("HOME");
    if (!getenv("DTUSERSESSION") || !getenv("DTUSERSESSION")[0]) {
        const char *u = getenv("USER");
        if (!u || !u[0]) u = getenv("LOGNAME");
        struct passwd *pw = (!u || !u[0]) ? getpwuid(getuid()) : nullptr;
        if (pw && pw->pw_name && pw->pw_name[0]) u = pw->pw_name;
        if (!u || !u[0]) u = "user";
        char usess[256];
        snprintf(usess, sizeof(usess), "%s-wayland", u);
        for (char *p = usess; *p; ++p) {
            if (*p == '/') *p = '_';
        }
        setenv("DTUSERSESSION", usess, 1);
    }
    if (!getenv("DTAPPSEARCHPATH") || !getenv("DTAPPSEARCHPATH")[0]) {
        char apppath[PATH_MAX * 2];
        if (home && home[0]) {
            snprintf(apppath, sizeof(apppath),
                     "%s/.dt/appmanager:" CDE_INSTALLATION_TOP "/appconfig/appmanager/%%L:"
                     CDE_INSTALLATION_TOP "/appconfig/appmanager/C",
                     home);
        } else {
            snprintf(apppath, sizeof(apppath),
                     CDE_INSTALLATION_TOP "/appconfig/appmanager/%%L:"
                     CDE_INSTALLATION_TOP "/appconfig/appmanager/C");
        }
        setenv("DTAPPSEARCHPATH", apppath, 1);
    }
    const char *usess = getenv("DTUSERSESSION");
    if (!usess || !usess[0] || strchr(usess, '/')) {
        return;
    }
    char check_path[PATH_MAX];
    snprintf(check_path, sizeof(check_path),
             "/var/dt/appconfig/appmanager/%s/Desktop_Apps", usess);
    struct stat st;
    if (stat(check_path, &st) != 0) {
        const char *gather_bin = CDE_INSTALLATION_TOP "/bin/dtappgather";
        if (access(gather_bin, X_OK) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                execl(gather_bin, "dtappgather", (char *)nullptr);
                _exit(127);
            } else if (pid > 0) {
                int status = 0;
                (void)waitpid(pid, &status, 0);
            }
        }
    }

    const char *xdg_bin = CDE_INSTALLATION_TOP "/bin/dtappintegrate";
    if (access(xdg_bin, X_OK) == 0) {
        pid_t xpid = fork();
        if (xpid == 0) {
            execl(xdg_bin, "dtappintegrate", "--xdg", (char *)nullptr);
            _exit(127);
        } else if (xpid > 0) {
            int status = 0;
            (void)waitpid(xpid, &status, 0);
        }

        /* Start live inotify watcher if not already running for this session */
        const char *rdir = getenv("XDG_RUNTIME_DIR");
        const char *hdir = getenv("HOME");
        char pidfile[PATH_MAX];
        pidfile[0] = '\0';
        if (rdir && rdir[0] == '/') {
            snprintf(pidfile, sizeof(pidfile), "%s/ck-xdg-watch-wayland.pid", rdir);
        } else if (hdir && hdir[0] == '/') {
            snprintf(pidfile, sizeof(pidfile), "%s/.dt/ck-xdg-watch-wayland.pid", hdir);
        }
        bool already_watching = (pidfile[0] == '\0');
        if (!already_watching) {
            FILE *pf = fopen(pidfile, "r");
            if (pf) {
                long wpid = 0;
                if (fscanf(pf, "%ld", &wpid) == 1 && wpid > 1 && kill((pid_t)wpid, 0) == 0) {
                    already_watching = true;
                }
                fclose(pf);
            }
        }
        if (!already_watching) {
            pid_t wpid = fork();
            if (wpid == 0) {
                setsid();
                execl(xdg_bin, "dtappintegrate", "--xdg", "--watch", "--pidfile", pidfile, (char *)nullptr);
                _exit(127);
            }
        }
    }

    const char *csync_bin = CDE_INSTALLATION_TOP "/bin/cde-color-sync";
    if (access(csync_bin, X_OK) == 0) {
        pid_t cpid = fork();
        if (cpid == 0) {
            setsid();
            execl(csync_bin, "cde-color-sync", "--daemon", (char *)nullptr);
            _exit(127);
        }
    }
}

void SmWaylandPreInit(int *argc, char **argv) {
    if (!argc || !argv) return;
    int out = 1;
    Boolean req_wayland = False;

    for (int i = 1; i < *argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-help") == 0) {
            printf("Usage: %s [--wayland] [-norestore] [-session name]\n", argv[0]);
            fflush(stdout);
            _exit(0);
        }
        if (strcmp(argv[i], "--wayland") == 0) {
            req_wayland = True;
            continue;
        }
        argv[out++] = argv[i];
    }
    argv[out] = nullptr;
    *argc = out;

    const char *wl_env = getenv("WAYLAND_DISPLAY");
    if (req_wayland || (wl_env && *wl_env)) {
        g_smwl.wayland_mode = True;
        ensure_appmanager_gathered_wl();
        if (req_wayland) {
            spawn_compositor_if_needed();
        }
        ensure_ttsession_started();
    }
}

Boolean SmWaylandActive(void) {
    const char *wl_env = getenv("WAYLAND_DISPLAY");
    return (g_smwl.wayland_mode || (wl_env && *wl_env)) ? True : False;
}

Boolean SmWaylandManagedCompositor(void) {
    return g_smwl.compositor_pid > 0 ? True : False;
}

Boolean SmWaylandInitXtIntegration(XtAppContext app, Display *dpy) {
    if (!SmWaylandActive()) return False;
    if (g_smwl.wl_dpy) return True;

    g_smwl.app = app;
    g_smwl.x_dpy = dpy;
    g_smwl.wl_dpy = wl_display_connect(nullptr);
    if (!g_smwl.wl_dpy) return False;

    g_smwl.registry = wl_display_get_registry(g_smwl.wl_dpy);
    wl_registry_add_listener(g_smwl.registry, &g_reg_listener, nullptr);
    wl_display_roundtrip(g_smwl.wl_dpy);
    wl_display_roundtrip(g_smwl.wl_dpy);

    if (app) {
        g_smwl.xt_input_id = XtAppAddInput(
            app, wl_display_get_fd(g_smwl.wl_dpy), (XtPointer)XtInputReadMask,
            xt_wayland_fd_cb, nullptr
        );
    }
    return (g_smwl.lock_mgr != nullptr) ? True : False;
}

void SmWaylandUpdateIdleTimeout(int timeout_seconds) {
    if (!g_smwl.wl_dpy || !g_smwl.idle_mgr || !g_smwl.seat) return;
    if (g_smwl.idle_notif) {
        ext_idle_notification_v1_destroy(g_smwl.idle_notif);
        g_smwl.idle_notif = nullptr;
    }
    if (timeout_seconds > 0) {
        uint32_t timeout_ms = (uint32_t)timeout_seconds * 1000u;
        g_smwl.idle_notif = ext_idle_notifier_v1_get_idle_notification(
            g_smwl.idle_mgr, timeout_ms, g_smwl.seat
        );
        if (g_smwl.idle_notif) {
            ext_idle_notification_v1_add_listener(g_smwl.idle_notif, &g_idle_listener, nullptr);
        }
    }
    wl_display_flush(g_smwl.wl_dpy);
}

void SmWaylandMarkLockWindow(Display *dpy, Window win) {
    if (!dpy || win == None || !SmWaylandActive()) return;
    Atom a = XInternAtom(dpy, "_DT_SESSION_LOCK_DIALOG", False);
    unsigned long one = 1UL;
    XChangeProperty(dpy, win, a, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
    XFlush(dpy);
}

Boolean SmWaylandAcquireSessionLock(void) {
    if (!g_smwl.wl_dpy || !g_smwl.lock_mgr) return False;
    if (g_smwl.active_lock && g_smwl.lock_ack_locked && !g_smwl.lock_finished) {
        return True;
    }
    if (g_smwl.active_lock) {
        SmWaylandReleaseSessionLock();
    }

    g_smwl.lock_ack_locked = False;
    g_smwl.lock_finished = False;
    g_smwl.active_lock = ext_session_lock_manager_v1_lock(g_smwl.lock_mgr);
    if (!g_smwl.active_lock) return False;

    ext_session_lock_v1_add_listener(g_smwl.active_lock, &g_session_lock_listener, nullptr);
    for (SmWlOutputNode *n = g_smwl.outputs; n != nullptr; n = n->next) {
        attach_lock_surface_for_output(n);
    }
    wl_display_flush(g_smwl.wl_dpy);

    for (int i = 0; i < 40 && !g_smwl.lock_ack_locked && !g_smwl.lock_finished; ++i) {
        if (wl_display_roundtrip(g_smwl.wl_dpy) < 0) break;
    }

    if (!g_smwl.lock_ack_locked || g_smwl.lock_finished) {
        for (SmWlOutputNode *n = g_smwl.outputs; n != nullptr; n = n->next) {
            destroy_output_lock_surface(n);
        }
        if (g_smwl.active_lock) {
            ext_session_lock_v1_destroy(g_smwl.active_lock);
            g_smwl.active_lock = nullptr;
        }
        wl_display_flush(g_smwl.wl_dpy);
        return False;
    }

    /* Roundtrip once more so initial black SHM buffers commit on all outputs */
    wl_display_roundtrip(g_smwl.wl_dpy);
    return True;
}

Boolean SmWaylandSessionIsLocked(void) {
    return (g_smwl.active_lock != nullptr && g_smwl.lock_ack_locked && !g_smwl.lock_finished)
               ? True
               : False;
}

void SmWaylandReleaseSessionLock(void) {
    if (!g_smwl.wl_dpy || !g_smwl.active_lock) return;

    if (g_smwl.lock_ack_locked && !g_smwl.lock_finished) {
        ext_session_lock_v1_unlock_and_destroy(g_smwl.active_lock);
    } else {
        ext_session_lock_v1_destroy(g_smwl.active_lock);
    }
    g_smwl.active_lock = nullptr;
    g_smwl.lock_ack_locked = False;
    g_smwl.lock_finished = False;

    for (SmWlOutputNode *n = g_smwl.outputs; n != nullptr; n = n->next) {
        destroy_output_lock_surface(n);
    }
    wl_display_roundtrip(g_smwl.wl_dpy);
}

static int ignore_smwl_x_error(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    return 0;
}

static void request_external_compositor_quit(void) {
    Display *dpy = g_smwl.x_dpy ? g_smwl.x_dpy : smGD.display;
    Boolean opened_local = False;
    if (!dpy) {
        dpy = XOpenDisplay(nullptr);
        opened_local = (dpy != nullptr) ? True : False;
    }
    if (!dpy) return;

    int (*prev_handler)(Display *, XErrorEvent *) = XSetErrorHandler(ignore_smwl_x_error);
    Window root = DefaultRootWindow(dpy);
    Atom mwm_info = XInternAtom(dpy, "_MOTIF_WM_INFO", True);
    Atom wm_req = XInternAtom(dpy, "_DT_WM_REQUEST", False);
    if (mwm_info != None && wm_req != None) {
        Atom actual = None;
        int fmt = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char *data = nullptr;
        Window wm_win = None;
        if (XGetWindowProperty(dpy, root, mwm_info, 0, 2, False, mwm_info,
                               &actual, &fmt, &nitems, &after, &data) == Success && data) {
            if (fmt == 32 && nitems >= 2) {
                const long *vals = (const long *)data;
                wm_win = (Window)vals[1];
            }
            XFree(data);
        }
        if (wm_win != None) {
            static const char quit_cmd[] = "f.quit_mwm";
            XChangeProperty(dpy, wm_win, wm_req, XA_STRING, 8, PropModeAppend,
                            (const unsigned char *)quit_cmd, (int)sizeof(quit_cmd));
            XSync(dpy, False);
        }
    }
    XSetErrorHandler(prev_handler);

    if (opened_local) {
        XCloseDisplay(dpy);
    }
}

void SmWaylandShutdown(void) {
    if (g_smwl.active_lock) {
        SmWaylandReleaseSessionLock();
    }
    if (g_smwl.compositor_pid <= 0 && SmWaylandActive() && smGD.loggingOut) {
        request_external_compositor_quit();
    }
    if (g_smwl.idle_notif) {
        ext_idle_notification_v1_destroy(g_smwl.idle_notif);
        g_smwl.idle_notif = nullptr;
    }
    if (g_smwl.xt_input_id != (XtInputId)0) {
        XtRemoveInput(g_smwl.xt_input_id);
        g_smwl.xt_input_id = (XtInputId)0;
    }
    while (g_smwl.outputs) {
        SmWlOutputNode *victim = g_smwl.outputs;
        g_smwl.outputs = victim->next;
        destroy_output_lock_surface(victim);
        if (victim->wl_output) wl_output_destroy(victim->wl_output);
        free(victim);
    }
    if (g_smwl.lock_mgr) {
        ext_session_lock_manager_v1_destroy(g_smwl.lock_mgr);
        g_smwl.lock_mgr = nullptr;
    }
    if (g_smwl.idle_mgr) {
        ext_idle_notifier_v1_destroy(g_smwl.idle_mgr);
        g_smwl.idle_mgr = nullptr;
    }
    if (g_smwl.seat) {
        wl_seat_destroy(g_smwl.seat);
        g_smwl.seat = nullptr;
    }
    if (g_smwl.shm) {
        wl_shm_destroy(g_smwl.shm);
        g_smwl.shm = nullptr;
    }
    if (g_smwl.compositor) {
        wl_compositor_destroy(g_smwl.compositor);
        g_smwl.compositor = nullptr;
    }
    if (g_smwl.registry) {
        wl_registry_destroy(g_smwl.registry);
        g_smwl.registry = nullptr;
    }
    if (g_smwl.wl_dpy) {
        wl_display_disconnect(g_smwl.wl_dpy);
        g_smwl.wl_dpy = nullptr;
    }
    if (g_smwl.status_path[0]) {
        unlink(g_smwl.status_path);
        g_smwl.status_path[0] = '\0';
    }
    if (g_smwl.compositor_pid > 0) {
        pid_t cpid = g_smwl.compositor_pid;
        g_smwl.compositor_pid = 0;
        kill(cpid, SIGTERM);
        struct timespec slp = {.tv_sec = 0, .tv_nsec = 50000000L};
        for (int i = 0; i < 40; ++i) {
            int st = 0;
            if (waitpid(cpid, &st, WNOHANG) == cpid) break;
            nanosleep(&slp, nullptr);
        }
    }
}

#else /* !BUILD_DTWM_WAYLAND */

void SmWaylandPreInit(int *argc, char **argv) { (void)argc; (void)argv; }
Boolean SmWaylandActive(void) { return False; }
Boolean SmWaylandManagedCompositor(void) { return False; }
Boolean SmWaylandInitXtIntegration(XtAppContext app, Display *dpy) { (void)app; (void)dpy; return False; }
void SmWaylandUpdateIdleTimeout(int timeout_seconds) { (void)timeout_seconds; }
void SmWaylandMarkLockWindow(Display *dpy, Window win) { (void)dpy; (void)win; }
Boolean SmWaylandAcquireSessionLock(void) { return False; }
Boolean SmWaylandSessionIsLocked(void) { return False; }
void SmWaylandReleaseSessionLock(void) {}
void SmWaylandShutdown(void) {}

#endif /* BUILD_DTWM_WAYLAND */
