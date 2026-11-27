/*
 * Wayland local-seat display class, unprivileged _dtgreeter session management,
 * FreeDesktop session menu discovery, and VT/seat handoff for dtlogin.
 */

#include "wayland_seat.h"
#include "vgmsg.h"
#include "vgproto.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>

#if !defined(sun) && defined(HAS_PAM_LIBRARY)
#include <security/pam_appl.h>
#include <Dt/SvcPam.h>
#endif

#define DTLOGIN_MAX_DESKTOP_SESSIONS 64

typedef struct {
    char filename[128];
    char path[PATH_MAX];
    char name[128];
    char exec_cmd[PATH_MAX];
    char desktop_names[128];
    int is_wayland;
    int rank;
} DesktopSessionEntry;

static volatile sig_atomic_t g_greeter_forward_pid = 0;

static void forward_greeter_signal(int sig)
{
    pid_t pid = (pid_t)g_greeter_forward_pid;
    if (pid > 1) {
        kill(pid, sig);
    }
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

static char **unsetEnvKey(char **e, const char *name)
{
    if (!e || !name || !name[0]) return e;
    size_t len = strlen(name);
    size_t out = 0;
    for (size_t in = 0; e[in]; ++in) {
        if (strncmp(e[in], name, len) == 0 && e[in][len] == '=') {
            free(e[in]);
        } else {
            e[out++] = e[in];
        }
    }
    e[out] = nullptr;
    return e;
}

static char **setEnvPair(char **e, const char *pair)
{
    if (!pair) return e;
    const char *eq = strchr(pair, '=');
    if (!eq || eq == pair) return e;
    size_t klen = (size_t)(eq - pair);
    char *key = malloc(klen + 1);
    if (!key) return e;
    memcpy(key, pair, klen);
    key[klen] = '\0';
    e = setEnv(e, key, (char *)(eq + 1));
    free(key);
    return e;
}

static void sanitize_token(const char *src, char *dst, size_t dst_sz)
{
    if (!dst || dst_sz == 0) return;
    size_t o = 0;
    if (src) {
        for (size_t i = 0; src[i] && o + 1 < dst_sz; ++i) {
            unsigned char c = (unsigned char)src[i];
            dst[o++] = (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
        }
    }
    if (o == 0 && dst_sz > 1) {
        dst[o++] = '0';
    }
    dst[o] = '\0';
}

static int parse_vtnr(const char *tty)
{
    if (!tty) return 0;
    if (strncmp(tty, "/dev/", 5) == 0) tty += 5;
    /* FreeBSD vt(4) / syscons: ttyv0..ttyvf map to 1-based VT numbers 1..16 */
    if (strncmp(tty, "ttyv", 4) == 0 && isxdigit((unsigned char)tty[4])) {
        char *endp = nullptr;
        long idx = strtol(tty + 4, &endp, 16);
        if (endp && *endp == '\0' && idx >= 0 && idx < 64) {
            return (int)idx + 1;
        }
    }
    if (strncmp(tty, "tty", 3) == 0 && isdigit((unsigned char)tty[3])) {
        return atoi(tty + 3);
    }
    if (strncmp(tty, "vt", 2) == 0 && isdigit((unsigned char)tty[2])) {
        return atoi(tty + 2);
    }
    return 0;
}

static const char *detect_seatd_socket(char *buf, size_t buf_sz)
{
    struct stat st;
    const char *explicit_lb = getenv("LIBSEAT_BACKEND");
    if (explicit_lb && explicit_lb[0] && strcmp(explicit_lb, "seatd") != 0) {
        return nullptr;
    }
    const char *sd_seats = getenv("DTLOGIN_SYSTEMD_SEATS_DIR");
    if (!sd_seats || !sd_seats[0]) {
        sd_seats = "/run/systemd/seats";
    }
    if ((!explicit_lb || !explicit_lb[0]) &&
        stat(sd_seats, &st) == 0 && S_ISDIR(st.st_mode)) {
        return nullptr;
    }
    const char *explicit_sock = getenv("SEATD_SOCK");
    if (explicit_sock && explicit_sock[0] &&
        stat(explicit_sock, &st) == 0 && S_ISSOCK(st.st_mode)) {
        snprintf(buf, buf_sz, "%s", explicit_sock);
        return buf;
    }
    static const char *const candidates[] = {
        "/var/run/seatd.sock",
        "/run/seatd.sock",
        nullptr
    };
    for (int i = 0; candidates[i]; ++i) {
        if (stat(candidates[i], &st) == 0 && S_ISSOCK(st.st_mode)) {
            snprintf(buf, buf_sz, "%s", candidates[i]);
            return buf;
        }
    }
    return nullptr;
}

static int ensure_runtime_base(char *out_base, size_t out_sz)
{
    const char *env_base = getenv("DTLOGIN_WAYLAND_RUNTIME_BASE");
    const char *candidates[4];
    int ncand = 0;
    if (env_base && env_base[0]) candidates[ncand++] = env_base;
    candidates[ncand++] = "/run/dtlogin";
    candidates[ncand++] = "/var/run/dtlogin";
    candidates[ncand++] = "/var/dt";

    for (int i = 0; i < ncand; ++i) {
        struct stat st;
        if (stat(candidates[i], &st) != 0) {
            (void)mkdir(candidates[i], 0755);
        }
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode) &&
            access(candidates[i], W_OK | X_OK) == 0) {
            snprintf(out_base, out_sz, "%s", candidates[i]);
            return 1;
        }
    }
    return 0;
}

static int read_status_env(const char *path, char *wl_out, size_t wl_sz, char *x11_out, size_t x11_sz)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    wl_out[0] = '\0';
    x11_out[0] = '\0';
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (strncmp(line, "WAYLAND_DISPLAY=", 16) == 0) {
            snprintf(wl_out, wl_sz, "%s", line + 16);
        } else if (strncmp(line, "DISPLAY=", 8) == 0) {
            snprintf(x11_out, x11_sz, "%s", line + 8);
        }
    }
    fclose(fp);
    return (wl_out[0] != '\0' && x11_out[0] != '\0' && strcmp(x11_out, "(none)") != 0);
}

static void clean_directory_contents(const char *dir_path)
{
    if (!dir_path || !dir_path[0]) return;
    DIR *dp = opendir(dir_path);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name) < (int)sizeof(full)) {
            (void)unlink(full);
        }
    }
    closedir(dp);
}

#if !defined(sun) && defined(HAS_PAM_LIBRARY)
static int greeter_null_conv(int num_msg, const struct pam_message **msg,
                             struct pam_response **resp, void *appdata)
{
    (void)msg;
    (void)appdata;
    if (num_msg <= 0 || !resp) return PAM_CONV_ERR;
    *resp = calloc((size_t)num_msg, sizeof(struct pam_response));
    return *resp ? PAM_SUCCESS : PAM_BUF_ERR;
}
#endif

int DtloginStartWaylandGreeter(struct display *d)
{
    if (!d) return FALSE;

    const char *guser = getenv("DTLOGIN_GREETER_USER");
    if (!guser || !guser[0]) {
        guser = (d->greeterUser && d->greeterUser[0]) ? d->greeterUser : "_dtgreeter";
    }

    uid_t target_uid = geteuid();
    gid_t target_gid = getegid();
    char resolved_user[64];
    snprintf(resolved_user, sizeof(resolved_user), "%s", guser);

    struct passwd *gpw = nullptr;
    if (geteuid() == 0) {
        gpw = getpwnam(guser);
        if (!gpw) {
            gpw = getpwnam("nobody");
        }
        if (gpw) {
            target_uid = gpw->pw_uid;
            target_gid = gpw->pw_gid;
            snprintf(resolved_user, sizeof(resolved_user), "%s", gpw->pw_name);
        }
    } else {
        gpw = getpwuid(geteuid());
        if (gpw && gpw->pw_name) {
            snprintf(resolved_user, sizeof(resolved_user), "%s", gpw->pw_name);
        }
    }

    char rt_base[PATH_MAX];
    if (!ensure_runtime_base(rt_base, sizeof(rt_base))) {
        LogError((unsigned char *)"Wayland greeter: no writable runtime base for %s\n", d->name);
        return FALSE;
    }

    char tag[64];
    sanitize_token(d->name, tag, sizeof(tag));
    char rt_dir[PATH_MAX];
    if (snprintf(rt_dir, sizeof(rt_dir), "%s/greeter-%s", rt_base, tag) >= (int)sizeof(rt_dir)) {
        return FALSE;
    }

    (void)mkdir(rt_dir, 0700);
    clean_directory_contents(rt_dir);
    (void)chmod(rt_dir, 0700);
    if (geteuid() == 0) {
        (void)chown(rt_dir, target_uid, target_gid);
    }

    char status_path[PATH_MAX];
    if (snprintf(status_path, sizeof(status_path), "%s/status.env", rt_dir) >= (int)sizeof(status_path)) {
        return FALSE;
    }
    (void)unlink(status_path);

    char **cmd_argv = nullptr;
    int has_greeter_flag = 0;
    if (d->argv && d->argv[0]) {
        for (int i = 0; d->argv[i]; ++i) {
            if (strcmp(d->argv[i], "--greeter") == 0 || strcmp(d->argv[i], "-g") == 0) {
                has_greeter_flag = 1;
            }
            if (!appendArg(&cmd_argv, d->argv[i])) {
                freeEnv(cmd_argv);
                return FALSE;
            }
        }
    } else {
        if (!appendArg(&cmd_argv, CDE_INSTALLATION_TOP "/bin/dtwm-wayland")) {
            return FALSE;
        }
    }
    if (!has_greeter_flag && !appendArg(&cmd_argv, "--greeter")) {
        freeEnv(cmd_argv);
        return FALSE;
    }
    if (!appendArg(&cmd_argv, "--status-file") || !appendArg(&cmd_argv, status_path)) {
        freeEnv(cmd_argv);
        return FALSE;
    }

    pid_t sup_pid = fork();
    if (sup_pid < 0) {
        freeEnv(cmd_argv);
        return FALSE;
    }

    if (sup_pid == 0) {
        CleanUpChild();

#if !defined(sun) && defined(HAS_PAM_LIBRARY)
        pam_handle_t *gpam = nullptr;
        int gpam_opened = 0;
        if (geteuid() == 0) {
            struct pam_conv gconv = { greeter_null_conv, nullptr };
            const char *svc = getenv("DTLOGIN_GREETER_PAM_SERVICE");
            if (!svc || !svc[0]) {
                svc = (access("/etc/pam.d/dtlogin-greeter", R_OK) == 0)
                      ? "dtlogin-greeter" : "dtlogin";
            }
            if (pam_start(svc, resolved_user, &gconv, &gpam) == PAM_SUCCESS) {
                const char *tty_item = (d->gettyLine && d->gettyLine[0] && strcmp(d->gettyLine, "??") != 0)
                                       ? d->gettyLine : d->name;
                (void)pam_set_item(gpam, PAM_TTY, tty_item);
                (void)pam_putenv(gpam, "XDG_SESSION_CLASS=greeter");
                (void)pam_putenv(gpam, "XDG_SESSION_TYPE=wayland");
                (void)pam_putenv(gpam, "XDG_SEAT=seat0");
                int vtnr = parse_vtnr(d->gettyLine);
                if (vtnr > 0) {
                    char vtbuf[32];
                    snprintf(vtbuf, sizeof(vtbuf), "XDG_VTNR=%d", vtnr);
                    (void)pam_putenv(gpam, vtbuf);
                }
                if (pam_open_session(gpam, PAM_SILENT) == PAM_SUCCESS) {
                    gpam_opened = 1;
                }
            }
        }
#endif

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = forward_greeter_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGHUP, &sa, nullptr);

        pid_t comp_pid = fork();
        if (comp_pid == 0) {
            char **env = nullptr;
            static const char *const passthrough_vars[] = {
                "PATH", "LD_LIBRARY_PATH", "WLR_BACKENDS", "WLR_RENDERER",
                "WLR_LIBINPUT_NO_DEVICES", "WLR_HEADLESS_OUTPUTS",
                "LIBSEAT_BACKEND", "SEATD_SOCK", "XKB_DEFAULT_RULES",
                "XKB_DEFAULT_MODEL", "XKB_DEFAULT_LAYOUT", "LANG", nullptr
            };
            for (int i = 0; passthrough_vars[i]; ++i) {
                const char *v = getenv(passthrough_vars[i]);
                if (v) env = setEnv(env, (char *)passthrough_vars[i], (char *)v);
            }
            if (!getEnv(env, "PATH")) {
                env = setEnv(env, "PATH", CDE_INSTALLATION_TOP "/bin:/usr/local/bin:/usr/bin:/bin");
            }
            {
                char seatd_sock[PATH_MAX];
                if (detect_seatd_socket(seatd_sock, sizeof(seatd_sock))) {
                    if (!getEnv(env, "SEATD_SOCK")) {
                        env = setEnv(env, "SEATD_SOCK", seatd_sock);
                    }
                    if (!getEnv(env, "LIBSEAT_BACKEND")) {
                        env = setEnv(env, "LIBSEAT_BACKEND", "seatd");
                    }
                }
            }
            if (d->environStr && d->environStr[0]) {
                env = parseEnv(env, d->environStr);
            }
#if !defined(sun) && defined(HAS_PAM_LIBRARY)
            if (gpam) {
                char **penv = pam_getenvlist(gpam);
                if (penv) {
                    for (int i = 0; penv[i]; ++i) {
                        env = setEnvPair(env, penv[i]);
                        free(penv[i]);
                    }
                    free(penv);
                }
            }
#endif
            if (geteuid() == 0) {
                if (setgid(target_gid) != 0) _exit(1);
                (void)initgroups(resolved_user, target_gid);
                if (setuid(target_uid) != 0) _exit(1);
            }
            /* Always bind the greeter Wayland socket inside rt_dir so dtlogin knows its exact location */
            env = setEnv(env, "XDG_RUNTIME_DIR", rt_dir);
            env = setEnv(env, "XDG_SESSION_CLASS", "greeter");
            env = setEnv(env, "XDG_SESSION_TYPE", "wayland");
            execve(cmd_argv[0], cmd_argv, env);
            _exit(127);
        }

        if (comp_pid > 0) {
            g_greeter_forward_pid = comp_pid;
            int cst = 0;
            while (waitpid(comp_pid, &cst, 0) < 0 && errno == EINTR) {
            }
#if !defined(sun) && defined(HAS_PAM_LIBRARY)
            if (gpam) {
                if (gpam_opened) (void)pam_close_session(gpam, PAM_SILENT);
                (void)pam_end(gpam, PAM_SUCCESS);
            }
#endif
            _exit(WIFEXITED(cst) ? WEXITSTATUS(cst) : 1);
        }

#if !defined(sun) && defined(HAS_PAM_LIBRARY)
        if (gpam) {
            if (gpam_opened) (void)pam_close_session(gpam, PAM_SILENT);
            (void)pam_end(gpam, PAM_SUCCESS);
        }
#endif
        _exit(1);
    }

    freeEnv(cmd_argv);

    int timeout_sec = (d->openTimeout > 0) ? d->openTimeout : 15;
    int max_iters = timeout_sec * 40;
    char wl_sock[128] = "";
    char x11_dpy[128] = "";

    for (int iter = 0; iter < max_iters; ++iter) {
        int st = 0;
        pid_t r = waitpid(sup_pid, &st, WNOHANG);
        if (r == sup_pid) {
            LogError((unsigned char *)"Wayland greeter compositor exited prematurely on %s\n", d->name);
            return FALSE;
        }
        if (read_status_env(status_path, wl_sock, sizeof(wl_sock), x11_dpy, sizeof(x11_dpy))) {
            free(d->waylandRuntimeDir);
            free(d->waylandSocket);
            free(d->xwaylandDisplay);
            d->waylandRuntimeDir = strdup(rt_dir);
            d->waylandSocket = strdup(wl_sock);
            d->xwaylandDisplay = strdup(x11_dpy);
            d->serverPid = sup_pid;
            Debug("Wayland greeter ready on %s: pid=%d WAYLAND_DISPLAY=%s DISPLAY=%s\n",
                  d->name, (int)sup_pid, wl_sock, x11_dpy);
            return TRUE;
        }
        sleep_ms(25);
    }

    kill(sup_pid, SIGTERM);
    (void)waitpid(sup_pid, nullptr, 0);
    LogError((unsigned char *)"Timed out waiting for Wayland greeter on %s\n", d->name);
    return FALSE;
}

void DtloginPrepareWaylandGreeterChild(struct display *d)
{
    if (!d || !d->isWaylandSeat) return;

    d->authorize = FALSE;
    if (d->authFile) {
        free(d->authFile);
        d->authFile = nullptr;
    }
    if (!d->seatConfigName && d->name) {
        d->seatConfigName = strdup(d->name);
    }
    if (d->xwaylandDisplay && d->xwaylandDisplay[0]) {
        char *new_name = strdup(d->xwaylandDisplay);
        if (new_name) {
            free(d->name);
            d->name = new_name;
        }
        setenv("DISPLAY", d->xwaylandDisplay, 1);
    }
    if (d->waylandRuntimeDir && d->waylandRuntimeDir[0]) {
        setenv("DTLOGIN_GREETER_RUNTIME_DIR", d->waylandRuntimeDir, 1);
    }
    if (d->waylandSocket && d->waylandSocket[0]) {
        setenv("DTLOGIN_GREETER_WAYLAND_DISPLAY", d->waylandSocket, 1);
    }
}

static int executable_usable(const char *cmd_or_path)
{
    if (!cmd_or_path || !cmd_or_path[0]) return 0;
    char tok[PATH_MAX];
    size_t i = 0;
    while (cmd_or_path[i] && isspace((unsigned char)cmd_or_path[i])) ++i;
    size_t o = 0;
    while (cmd_or_path[i] && !isspace((unsigned char)cmd_or_path[i]) && o + 1 < sizeof(tok)) {
        tok[o++] = cmd_or_path[i++];
    }
    tok[o] = '\0';
    if (tok[0] == '\0') return 0;
    if (strchr(tok, '/')) {
        return access(tok, X_OK) == 0;
    }
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0]) {
        path_env = CDE_INSTALLATION_TOP "/bin:/usr/local/bin:/usr/bin:/bin";
    }
    const char *p = path_env;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        char cand[PATH_MAX];
        if (dlen > 0 && dlen + 1 + strlen(tok) < sizeof(cand)) {
            memcpy(cand, p, dlen);
            cand[dlen] = '/';
            strcpy(cand + dlen + 1, tok);
            if (access(cand, X_OK) == 0) return 1;
        }
        if (!colon) break;
        p = colon + 1;
    }
    return 0;
}

static void strip_desktop_field_codes(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_sz; ++i) {
        if (src[i] == '%' && src[i + 1] != '\0') {
            if (src[i + 1] == '%') {
                dst[o++] = '%';
            }
            ++i;
            continue;
        }
        dst[o++] = src[i];
    }
    while (o > 0 && isspace((unsigned char)dst[o - 1])) {
        --o;
    }
    dst[o] = '\0';
}

static int parse_desktop_file(const char *dir, const char *fname, int is_wayland, DesktopSessionEntry *out)
{
    memset(out, 0, sizeof(*out));
    if (snprintf(out->path, sizeof(out->path), "%s/%s", dir, fname) >= (int)sizeof(out->path)) {
        return 0;
    }
    FILE *fp = fopen(out->path, "r");
    if (!fp) return 0;

    int in_desktop_entry = 0;
    int hidden = 0;
    char try_exec[PATH_MAX] = "";
    char raw_exec[PATH_MAX] = "";
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        char *s = line;
        while (*s && isspace((unsigned char)*s)) ++s;
        if (*s == '\0' || *s == '#') continue;
        if (*s == '[') {
            in_desktop_entry = (strncmp(s, "[Desktop Entry]", 15) == 0);
            continue;
        }
        if (!in_desktop_entry) continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = s;
        char *val = eq + 1;
        char *kend = eq;
        while (kend > key && isspace((unsigned char)kend[-1])) *--kend = '\0';
        while (*val && isspace((unsigned char)*val)) ++val;

        if (strcmp(key, "Name") == 0) {
            snprintf(out->name, sizeof(out->name), "%s", val);
        } else if (strcmp(key, "Exec") == 0) {
            snprintf(raw_exec, sizeof(raw_exec), "%s", val);
        } else if (strcmp(key, "TryExec") == 0) {
            snprintf(try_exec, sizeof(try_exec), "%s", val);
        } else if (strcmp(key, "DesktopNames") == 0) {
            snprintf(out->desktop_names, sizeof(out->desktop_names), "%s", val);
            char *semi = strchr(out->desktop_names, ';');
            if (semi) *semi = '\0';
        } else if (strcmp(key, "Hidden") == 0 || strcmp(key, "NoDisplay") == 0) {
            if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0) {
                hidden = 1;
            }
        }
    }
    fclose(fp);

    if (hidden || out->name[0] == '\0' || raw_exec[0] == '\0') return 0;
    strip_desktop_field_codes(raw_exec, out->exec_cmd, sizeof(out->exec_cmd));
    if (out->exec_cmd[0] == '\0') return 0;
    if (try_exec[0] != '\0' && !executable_usable(try_exec)) return 0;
    if (!executable_usable(out->exec_cmd)) return 0;

    snprintf(out->filename, sizeof(out->filename), "%s", fname);
    out->is_wayland = is_wayland;
    if (out->desktop_names[0] == '\0') {
        snprintf(out->desktop_names, sizeof(out->desktop_names), "CDE");
    }

    if (strcmp(fname, "cde-wayland.desktop") == 0) {
        out->rank = 0;
    } else if (strcmp(fname, "cde.desktop") == 0) {
        out->rank = 1;
    } else if (is_wayland) {
        out->rank = 2;
    } else {
        out->rank = 3;
    }
    return 1;
}

static int cmp_session_entries(const void *a, const void *b)
{
    const DesktopSessionEntry *sa = (const DesktopSessionEntry *)a;
    const DesktopSessionEntry *sb = (const DesktopSessionEntry *)b;
    if (sa->rank != sb->rank) return (sa->rank < sb->rank) ? -1 : 1;
    int c = strcasecmp(sa->name, sb->name);
    if (c != 0) return c;
    return strcmp(sa->filename, sb->filename);
}

static void scan_desktop_dir(const char *dir, int is_wayland,
                             DesktopSessionEntry *entries, int *count)
{
    if (!dir || !dir[0] || *count >= DTLOGIN_MAX_DESKTOP_SESSIONS) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != nullptr && *count < DTLOGIN_MAX_DESKTOP_SESSIONS) {
        size_t nlen = strlen(de->d_name);
        if (nlen <= 8 || strcmp(de->d_name + nlen - 8, ".desktop") != 0) continue;
        if (strchr(de->d_name, ' ') != nullptr) continue;
        DesktopSessionEntry item;
        if (parse_desktop_file(dir, de->d_name, is_wayland, &item)) {
            int dup_found = 0;
            for (int k = 0; k < *count; ++k) {
                if (entries[k].is_wayland == is_wayland &&
                    strcmp(entries[k].filename, item.filename) == 0) {
                    dup_found = 1;
                    break;
                }
            }
            if (!dup_found) {
                entries[(*count)++] = item;
            }
        }
    }
    closedir(dp);
}

void DtloginPopulateDesktopSessions(struct display *d, XrmDatabase *db)
{
    if (!d || !d->isWaylandSeat || !db) return;

    const char *wl_dir = getenv("DTLOGIN_WAYLAND_SESSIONS_DIR");
    const char *x11_dir = getenv("DTLOGIN_XSESSIONS_DIR");

    DesktopSessionEntry entries[DTLOGIN_MAX_DESKTOP_SESSIONS];
    int count = 0;

    if (wl_dir && wl_dir[0]) {
        scan_desktop_dir(wl_dir, 1, entries, &count);
    } else {
        scan_desktop_dir("/usr/share/wayland-sessions", 1, entries, &count);
        scan_desktop_dir("/usr/local/share/wayland-sessions", 1, entries, &count);
    }
    if (x11_dir && x11_dir[0]) {
        scan_desktop_dir(x11_dir, 0, entries, &count);
    } else {
        scan_desktop_dir("/usr/share/xsessions", 0, entries, &count);
        scan_desktop_dir("/usr/local/share/xsessions", 0, entries, &count);
    }

    /* Ensure CDE (Wayland) and CDE (X11 / XLibre) are present if installed in CDE_INSTALLATION_TOP */
    int has_cde_wl = 0, has_cde_x11 = 0;
    for (int i = 0; i < count; ++i) {
        if (entries[i].rank == 0) has_cde_wl = 1;
        if (entries[i].rank == 1) has_cde_x11 = 1;
    }
    if (!has_cde_wl && count < DTLOGIN_MAX_DESKTOP_SESSIONS &&
        access(CDE_INSTALLATION_TOP "/bin/Xsession.wayland", X_OK) == 0) {
        DesktopSessionEntry *e = &entries[count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->filename, sizeof(e->filename), "cde-wayland.desktop");
        snprintf(e->path, sizeof(e->path), "%s", CDE_INSTALLATION_TOP "/bin/Xsession.wayland");
        snprintf(e->name, sizeof(e->name), "CDE (Wayland)");
        snprintf(e->exec_cmd, sizeof(e->exec_cmd), "%s", CDE_INSTALLATION_TOP "/bin/Xsession.wayland");
        snprintf(e->desktop_names, sizeof(e->desktop_names), "CDE");
        e->is_wayland = 1;
        e->rank = 0;
    }
    if (!has_cde_x11 && count < DTLOGIN_MAX_DESKTOP_SESSIONS &&
        access(CDE_INSTALLATION_TOP "/bin/Xsession", X_OK) == 0) {
        DesktopSessionEntry *e = &entries[count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->filename, sizeof(e->filename), "cde.desktop");
        snprintf(e->path, sizeof(e->path), "%s", CDE_INSTALLATION_TOP "/bin/Xsession");
        snprintf(e->name, sizeof(e->name), "CDE (X11 / XLibre)");
        snprintf(e->exec_cmd, sizeof(e->exec_cmd), "%s", CDE_INSTALLATION_TOP "/bin/Xsession");
        snprintf(e->desktop_names, sizeof(e->desktop_names), "CDE");
        e->is_wayland = 0;
        e->rank = 1;
    }

    if (count == 0) return;

    qsort(entries, (size_t)count, sizeof(entries[0]), cmp_session_entries);

    /* Disambiguate identical session names across Wayland and X11 directories */
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (strcmp(entries[i].name, entries[j].name) == 0 &&
                entries[i].is_wayland != entries[j].is_wayland) {
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "%s (%s)", entries[i].name,
                         entries[i].is_wayland ? "Wayland" : "X11");
                snprintf(entries[i].name, sizeof(entries[i].name), "%s", tmp);
                snprintf(tmp, sizeof(tmp), "%s (%s)", entries[j].name,
                         entries[j].is_wayland ? "Wayland" : "X11");
                snprintf(entries[j].name, sizeof(entries[j].name), "%s", tmp);
            }
        }
    }

    char key[128];
    char val[32];
    snprintf(val, sizeof(val), "%d", count);
    XrmPutStringResource(db, "Dtlogin*altDts", val);

    for (int i = 0; i < count; ++i) {
        int idx = i + 1;
        snprintf(key, sizeof(key), "Dtlogin*altDtName%d", idx);
        XrmPutStringResource(db, key, entries[i].name);

        snprintf(key, sizeof(key), "Dtlogin*altDtKey%d", idx);
        XrmPutStringResource(db, key, entries[i].path);

        snprintf(key, sizeof(key), "Dtlogin*altDtStart%d", idx);
        XrmPutStringResource(db, key, entries[i].exec_cmd);

        snprintf(key, sizeof(key), "Dtlogin*altDtProtocol%d", idx);
        XrmPutStringResource(db, key, entries[i].is_wayland ? "wayland" : "x11");

        snprintf(key, sizeof(key), "Dtlogin*altDtDesktop%d", idx);
        XrmPutStringResource(db, key, entries[i].desktop_names);
    }

    /* Record default session first token so dtgreet highlights slot 1 (CDE Wayland) */
    char first_tok[PATH_MAX];
    size_t o = 0;
    while (entries[0].exec_cmd[o] && !isspace((unsigned char)entries[0].exec_cmd[o]) &&
           o + 1 < sizeof(first_tok)) {
        first_tok[o] = entries[0].exec_cmd[o];
        ++o;
    }
    first_tok[o] = '\0';
    free(d->defaultWaylandSession);
    d->defaultWaylandSession = strdup(first_tok);
}

int DtloginResolveDefaultWaylandSession(struct display *d, XrmDatabase db, char ***argv_out)
{
    if (!d || !d->isWaylandSeat || !db || !argv_out) return 0;
    char *rmtype = nullptr;
    XrmValue rmval = {0, nullptr};
    if (XrmGetResource(db, "Dtlogin*altDtStart1", "Dtlogin*AltDtStart1", &rmtype, &rmval) == True &&
        rmval.addr && rmval.addr[0]) {
        if (parseArgs(argv_out, rmval.addr)) {
            return 1;
        }
    }
    return 0;
}

static int find_free_x11_display(char *dpy_out, size_t dpy_sz)
{
    char sock_dir[32];
    snprintf(sock_dir, sizeof(sock_dir), "/%s/.X11-unix", "tmp");
    for (int n = 1; n < 64; ++n) {
        char spath[64];
        char lpath[64];
        snprintf(spath, sizeof(spath), "%s/X%d", sock_dir, n);
        snprintf(lpath, sizeof(lpath), "/%s/.X%d-lock", "tmp", n);
        if (access(spath, F_OK) != 0 && access(lpath, F_OK) != 0) {
            snprintf(dpy_out, dpy_sz, ":%d", n);
            return 1;
        }
    }
    snprintf(dpy_out, dpy_sz, ":1");
    return 1;
}

static int spawn_seat_x11_server(struct display *d, char *dpy_out, size_t dpy_sz)
{
    if (!find_free_x11_display(dpy_out, dpy_sz)) return 0;

    const char *custom_cmd = getenv("DTLOGIN_X11_SERVER_CMD");
    char **xargv = nullptr;
    if (custom_cmd && custom_cmd[0]) {
        if (!parseArgs(&xargv, (char *)custom_cmd) || !appendArg(&xargv, dpy_out)) {
            freeEnv(xargv);
            return 0;
        }
    } else {
        const char *xbin = (access("/usr/bin/Xorg", X_OK) == 0) ? "/usr/bin/Xorg" : "/usr/bin/X";
        if (!appendArg(&xargv, xbin) || !appendArg(&xargv, dpy_out)) {
            freeEnv(xargv);
            return 0;
        }
        int vtnr = parse_vtnr(d->gettyLine);
        if (vtnr > 0) {
            char vtarg[16];
            snprintf(vtarg, sizeof(vtarg), "vt%d", vtnr);
            if (!appendArg(&xargv, vtarg)) {
                freeEnv(xargv);
                return 0;
            }
        }
    }

    pid_t xpid = fork();
    if (xpid < 0) {
        freeEnv(xargv);
        return 0;
    }
    if (xpid == 0) {
        CleanUpChild();
        execvp(xargv[0], xargv);
        _exit(127);
    }
    freeEnv(xargv);

    for (int iter = 0; iter < 400; ++iter) {
        int st = 0;
        if (waitpid(xpid, &st, WNOHANG) == xpid) {
            LogError((unsigned char *)"Seat X11 server exited early for %s\n", dpy_out);
            return 0;
        }
        Display *test_dpy = XOpenDisplay(dpy_out);
        if (test_dpy) {
            XCloseDisplay(test_dpy);
            d->seatXserverPid = (int)xpid;
            Debug("Started seat X11 server %s (pid %d)\n", dpy_out, (int)xpid);
            return 1;
        }
        sleep_ms(25);
    }

    kill(xpid, SIGTERM);
    (void)waitpid(xpid, nullptr, 0);
    return 0;
}

int DtloginHandoffWaylandSeat(struct display *d, struct verify_info *verify, const char *pam_service)
{
    if (!d || !d->isWaylandSeat || !verify) return 1;

    extern XrmDatabase XresourceDB;

    /* 1. Cleanly terminate greeter compositor so DRM/libseat/VT ownership is released */
    if (d->serverPid > 1) {
        pid_t gpid = (pid_t)d->serverPid;
        Debug("Wayland seat handoff: stopping greeter compositor pid %d\n", (int)gpid);
        kill(gpid, SIGTERM);
        for (int i = 0; i < 160; ++i) {
            if (kill(gpid, 0) != 0 && errno == ESRCH) {
                break;
            }
            sleep_ms(25);
        }
        if (kill(gpid, 0) == 0) {
            kill(gpid, SIGKILL);
            for (int i = 0; i < 40; ++i) {
                if (kill(gpid, 0) != 0 && errno == ESRCH) break;
                sleep_ms(25);
            }
        }
        d->serverPid = -1;
    }
    if (d->waylandRuntimeDir) {
        clean_directory_contents(d->waylandRuntimeDir);
    }

    /* 2. Restore configured seat name for PAM/accounting */
    if (d->seatConfigName && d->seatConfigName[0]) {
        char *restored = strdup(d->seatConfigName);
        if (restored) {
            free(d->name);
            d->name = restored;
        }
    }

    /* 3. Resolve target protocol (wayland vs x11) and DesktopNames */
    const char *proto = "wayland";
    const char *desktop_names = "CDE";
    char *rmtype = nullptr;
    XrmValue rmval = {0, nullptr};

    if (d->sessionType == NOTIFY_FAILSAFE) {
        proto = "x11";
    } else if (XresourceDB) {
        int slot = (d->sessionType > NOTIFY_ALT_DTS) ? (d->sessionType - NOTIFY_ALT_DTS) : 1;
        char pkey[64], pcls[64], dkey[64], dcls[64];
        snprintf(pkey, sizeof(pkey), "Dtlogin*altDtProtocol%d", slot);
        snprintf(pcls, sizeof(pcls), "Dtlogin*AltDtProtocol%d", slot);
        if (XrmGetResource(XresourceDB, pkey, pcls, &rmtype, &rmval) == True && rmval.addr) {
            proto = rmval.addr;
        } else if (verify->argv && verify->argv[0]) {
            /* Match NOTIFY_LAST_DT command against populated altDtStart<N> */
            for (int i = 1; i <= DTLOGIN_MAX_DESKTOP_SESSIONS; ++i) {
                char skey[64], scls[64];
                snprintf(skey, sizeof(skey), "Dtlogin*altDtStart%d", i);
                snprintf(scls, sizeof(scls), "Dtlogin*AltDtStart%d", i);
                XrmValue sval = {0, nullptr};
                if (XrmGetResource(XresourceDB, skey, scls, &rmtype, &sval) != True || !sval.addr) {
                    break;
                }
                if (strncmp(sval.addr, verify->argv[0], strlen(verify->argv[0])) == 0) {
                    slot = i;
                    snprintf(pkey, sizeof(pkey), "Dtlogin*altDtProtocol%d", slot);
                    snprintf(pcls, sizeof(pcls), "Dtlogin*AltDtProtocol%d", slot);
                    if (XrmGetResource(XresourceDB, pkey, pcls, &rmtype, &rmval) == True && rmval.addr) {
                        proto = rmval.addr;
                    }
                    break;
                }
            }
        }
        snprintf(dkey, sizeof(dkey), "Dtlogin*altDtDesktop%d", slot);
        snprintf(dcls, sizeof(dcls), "Dtlogin*AltDtDesktop%d", slot);
        XrmValue dval = {0, nullptr};
        if (XrmGetResource(XresourceDB, dkey, dcls, &rmtype, &dval) == True && dval.addr && dval.addr[0]) {
            desktop_names = dval.addr;
        }
    }

    int is_wayland_session = (strcmp(proto, "wayland") == 0);
    d->waylandSessionActive = is_wayland_session ? 1 : 0;
    d->pingInterval = 0;

    char *user = getEnv(verify->userEnviron, "USER");
    int vtnr = parse_vtnr(d->gettyLine);

    if (is_wayland_session) {
        verify->userEnviron = unsetEnvKey(verify->userEnviron, "DISPLAY");
        verify->userEnviron = unsetEnvKey(verify->userEnviron, "WAYLAND_DISPLAY");
        verify->userEnviron = unsetEnvKey(verify->userEnviron, "XAUTHORITY");
        verify->systemEnviron = unsetEnvKey(verify->systemEnviron, "DISPLAY");
        verify->systemEnviron = unsetEnvKey(verify->systemEnviron, "WAYLAND_DISPLAY");

        verify->userEnviron = setEnv(verify->userEnviron, "XDG_SESSION_TYPE", "wayland");
        verify->userEnviron = setEnv(verify->userEnviron, "XDG_SESSION_CLASS", "user");
        verify->userEnviron = setEnv(verify->userEnviron, "XDG_SEAT", "seat0");
        verify->userEnviron = setEnv(verify->userEnviron, "XDG_CURRENT_DESKTOP", (char *)desktop_names);
        {
            char seatd_sock[PATH_MAX];
            if (detect_seatd_socket(seatd_sock, sizeof(seatd_sock))) {
                if (!getEnv(verify->userEnviron, "SEATD_SOCK")) {
                    verify->userEnviron = setEnv(verify->userEnviron, "SEATD_SOCK", seatd_sock);
                }
                if (!getEnv(verify->userEnviron, "LIBSEAT_BACKEND")) {
                    verify->userEnviron = setEnv(verify->userEnviron, "LIBSEAT_BACKEND", "seatd");
                }
            }
        }
    } else {
        char seat_dpy[64];
        if (!spawn_seat_x11_server(d, seat_dpy, sizeof(seat_dpy))) {
            LogError((unsigned char *)"Failed to start X11 server for X11 session on Wayland seat %s\n", d->name);
            return 0;
        }
        verify->userEnviron = unsetEnvKey(verify->userEnviron, "WAYLAND_DISPLAY");
        verify->userEnviron = setEnv(verify->userEnviron, "DISPLAY", seat_dpy);
        verify->systemEnviron = setEnv(verify->systemEnviron, "DISPLAY", seat_dpy);
        verify->userEnviron = setEnv(verify->userEnviron, "XDG_SESSION_TYPE", "x11");
        verify->userEnviron = setEnv(verify->userEnviron, "XDG_SESSION_CLASS", "user");
        verify->userEnviron = setEnv(verify->userEnviron, "XDG_SEAT", "seat0");
        verify->userEnviron = setEnv(verify->userEnviron, "XDG_CURRENT_DESKTOP", (char *)desktop_names);
    }

    if (vtnr > 0) {
        char vts[16];
        snprintf(vts, sizeof(vts), "%d", vtnr);
        verify->userEnviron = setEnv(verify->userEnviron, "XDG_VTNR", vts);
    }

#if !defined(sun) && defined(HAS_PAM_LIBRARY)
    if (pam_service && user) {
        char nv[192];
        snprintf(nv, sizeof(nv), "XDG_SESSION_TYPE=%s", is_wayland_session ? "wayland" : "x11");
        (void)_DtSvcPamPutenv(pam_service, user, d->name, nv);
        (void)_DtSvcPamPutenv(pam_service, user, d->name, "XDG_SESSION_CLASS=user");
        (void)_DtSvcPamPutenv(pam_service, user, d->name, "XDG_SEAT=seat0");
        snprintf(nv, sizeof(nv), "XDG_CURRENT_DESKTOP=%s", desktop_names);
        (void)_DtSvcPamPutenv(pam_service, user, d->name, nv);
        if (vtnr > 0) {
            snprintf(nv, sizeof(nv), "XDG_VTNR=%d", vtnr);
            (void)_DtSvcPamPutenv(pam_service, user, d->name, nv);
        }
    }
#else
    (void)pam_service;
    (void)user;
#endif

    return 1;
}

void DtloginSyncPamSessionEnv(struct display *d, struct verify_info *verify)
{
    if (!verify) return;

#if !defined(sun) && defined(HAS_PAM_LIBRARY)
    char **penv = _DtSvcPamGetenvlist();
    if (penv) {
        for (int i = 0; penv[i]; ++i) {
            verify->userEnviron = setEnvPair(verify->userEnviron, penv[i]);
            free(penv[i]);
        }
        free(penv);
    }
#endif

    if (d && d->isWaylandSeat && d->waylandSessionActive) {
        char *xrt = getEnv(verify->userEnviron, "XDG_RUNTIME_DIR");
        if (!xrt || !xrt[0]) {
            const char *ubase = getenv("DTLOGIN_USER_RUNTIME_BASE");
            char dir_buf[PATH_MAX];
            if (ubase && ubase[0]) {
                snprintf(dir_buf, sizeof(dir_buf), "%s/user-%d", ubase, verify->uid);
            } else if (access("/run/user", W_OK | X_OK) == 0 ||
                       (geteuid() == 0 && access("/run", W_OK | X_OK) == 0)) {
                (void)mkdir("/run/user", 0755);
                snprintf(dir_buf, sizeof(dir_buf), "/run/user/%d", verify->uid);
            } else if (access("/var/run/user", W_OK | X_OK) == 0 ||
                       (geteuid() == 0 && access("/var/run", W_OK | X_OK) == 0)) {
                (void)mkdir("/var/run/user", 0755);
                snprintf(dir_buf, sizeof(dir_buf), "/var/run/user/%d", verify->uid);
            } else {
                snprintf(dir_buf, sizeof(dir_buf), "/var/dt/user-%d", verify->uid);
            }
            (void)mkdir(dir_buf, 0700);
            (void)chmod(dir_buf, 0700);
            if (geteuid() == 0) {
#ifdef NGROUPS
                gid_t ugid = verify->groups[0];
#else
                gid_t ugid = (gid_t)verify->gid;
#endif
                (void)chown(dir_buf, (uid_t)verify->uid, ugid);
            }
            verify->userEnviron = setEnv(verify->userEnviron, "XDG_RUNTIME_DIR", dir_buf);
        }
    }
}

void DtloginCleanupWaylandSeat(struct display *d)
{
    if (!d || !d->isWaylandSeat) return;

    if (d->seatXserverPid > 1) {
        pid_t xpid = (pid_t)d->seatXserverPid;
        Debug("Stopping seat X11 server pid %d\n", (int)xpid);
        kill(xpid, SIGTERM);
        for (int i = 0; i < 120; ++i) {
            int st = 0;
            pid_t r = waitpid(xpid, &st, WNOHANG);
            if (r == xpid || (r < 0 && errno == ECHILD)) break;
            sleep_ms(25);
        }
        if (kill(xpid, 0) == 0) {
            kill(xpid, SIGKILL);
            (void)waitpid(xpid, nullptr, 0);
        }
        d->seatXserverPid = -1;
    }

    if (d->waylandRuntimeDir && d->waylandRuntimeDir[0]) {
        clean_directory_contents(d->waylandRuntimeDir);
        (void)rmdir(d->waylandRuntimeDir);
    }
}
