/*
 * CDE - Common Desktop Environment
 * dtwm-media-ctl: Shared Wayland + X11 hardware multimedia, volume, mic-mute,
 * backlight (/sys/class/backlight + systemd-logind SetBrightness), keyboard
 * backlight, and MPRIS transport controller for dtwm-wayland and classic dtwm.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool has_prog_in_path(const char *prog) {
    const char *path = getenv("PATH");
    if (!path || !path[0]) path = "/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy) return false;
    bool found = false;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char cand[PATH_MAX];
        snprintf(cand, sizeof(cand), "%s/%s", dir, prog);
        if (access(cand, X_OK) == 0) {
            found = true;
            break;
        }
    }
    free(copy);
    return found;
}

static bool read_int_file(const char *path, int *out_val) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    char buf[64] = {0};
    bool ok = false;
    if (fgets(buf, sizeof(buf), fp)) {
        char *end = NULL;
        long v = strtol(buf, &end, 10);
        if (end != buf) {
            *out_val = (int)v;
            ok = true;
        }
    }
    fclose(fp);
    return ok;
}

static bool write_int_file(const char *path, int val) {
    char tmp[PATH_MAX];
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d\n", val);
        ssize_t wr = write(fd, buf, (size_t)n);
        close(fd);
        return wr == n;
    }
    /* For regular state files that do not exist yet, write atomically */
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    FILE *fp = fopen(tmp, "w");
    if (!fp) return false;
    fprintf(fp, "%d\n", val);
    if (fclose(fp) != 0) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static void state_dir_path(char *out, size_t out_sz) {
    const char *override_dir = getenv("CK_CDE_MEDIA_STATE_DIR");
    if (override_dir && override_dir[0]) {
        snprintf(out, out_sz, "%s", override_dir);
        mkdir(out, 0700);
        return;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) {
        snprintf(out, out_sz, "%s/cde-media-state", rt);
        mkdir(out, 0700);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, out_sz, "%s/.dt/cde-media-state", (home && home[0]) ? home : "/var/tmp");
    mkdir(out, 0700);
}

static bool run_capture(char *const argv[], char *out, size_t out_sz) {
    int pfd[2];
    if (pipe(pfd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > 2) close(dn);
        }
        close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t used = 0;
    if (out && out_sz > 0) out[0] = '\0';
    while (out && used + 1 < out_sz) {
        ssize_t r = read(pfd[0], out + used, out_sz - 1 - used);
        if (r > 0) used += (size_t)r;
        else if (r == -1 && errno == EINTR) continue;
        else break;
    }
    if (out && out_sz > 0) out[used] = '\0';
    close(pfd[0]);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* -------------------------------------------------------------------------
 * Optional ALSA libasound fallback via dlopen("libasound.so.2")
 * ------------------------------------------------------------------------- */
typedef struct _snd_mixer snd_mixer_t;
typedef struct _snd_mixer_elem snd_mixer_elem_t;
typedef struct _snd_mixer_selem_id snd_mixer_selem_id_t;

static bool alsa_audio_op(const char *op, int step, int *out_vol, bool *out_muted) {
    void *lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (!lib) return false;

    int (*p_open)(snd_mixer_t **, int) = dlsym(lib, "snd_mixer_open");
    int (*p_attach)(snd_mixer_t *, const char *) = dlsym(lib, "snd_mixer_attach");
    int (*p_reg)(snd_mixer_t *, void *, void *) = dlsym(lib, "snd_mixer_selem_register");
    int (*p_load)(snd_mixer_t *) = dlsym(lib, "snd_mixer_load");
    int (*p_close)(snd_mixer_t *) = dlsym(lib, "snd_mixer_close");
    int (*p_id_malloc)(snd_mixer_selem_id_t **) = dlsym(lib, "snd_mixer_selem_id_malloc");
    void (*p_id_free)(snd_mixer_selem_id_t *) = dlsym(lib, "snd_mixer_selem_id_free");
    void (*p_id_set_idx)(snd_mixer_selem_id_t *, unsigned int) = dlsym(lib, "snd_mixer_selem_id_set_index");
    void (*p_id_set_name)(snd_mixer_selem_id_t *, const char *) = dlsym(lib, "snd_mixer_selem_id_set_name");
    snd_mixer_elem_t *(*p_find)(snd_mixer_t *, snd_mixer_selem_id_t *) = dlsym(lib, "snd_mixer_find_selem");
    int (*p_get_range)(snd_mixer_elem_t *, long *, long *) = dlsym(lib, "snd_mixer_selem_get_playback_volume_range");
    int (*p_get_vol)(snd_mixer_elem_t *, int, long *) = dlsym(lib, "snd_mixer_selem_get_playback_volume");
    int (*p_set_vol)(snd_mixer_elem_t *, long) = dlsym(lib, "snd_mixer_selem_set_playback_volume_all");
    int (*p_has_pswitch)(snd_mixer_elem_t *) = dlsym(lib, "snd_mixer_selem_has_playback_switch");
    int (*p_get_pswitch)(snd_mixer_elem_t *, int, int *) = dlsym(lib, "snd_mixer_selem_get_playback_switch");
    int (*p_set_pswitch)(snd_mixer_elem_t *, int) = dlsym(lib, "snd_mixer_selem_set_playback_switch_all");
    int (*p_has_cswitch)(snd_mixer_elem_t *) = dlsym(lib, "snd_mixer_selem_has_capture_switch");
    int (*p_get_cswitch)(snd_mixer_elem_t *, int, int *) = dlsym(lib, "snd_mixer_selem_get_capture_switch");
    int (*p_set_cswitch)(snd_mixer_elem_t *, int) = dlsym(lib, "snd_mixer_selem_set_capture_switch_all");

    if (!p_open || !p_attach || !p_reg || !p_load || !p_close || !p_id_malloc ||
        !p_id_free || !p_id_set_idx || !p_id_set_name || !p_find || !p_get_range ||
        !p_get_vol || !p_set_vol || !p_has_pswitch || !p_get_pswitch || !p_set_pswitch) {
        dlclose(lib);
        return false;
    }

    snd_mixer_t *mix = NULL;
    if (p_open(&mix, 0) < 0) {
        dlclose(lib);
        return false;
    }
    if (p_attach(mix, "default") < 0 || p_reg(mix, NULL, NULL) < 0 || p_load(mix) < 0) {
        p_close(mix);
        dlclose(lib);
        return false;
    }

    snd_mixer_selem_id_t *sid = NULL;
    p_id_malloc(&sid);
    if (!sid) {
        p_close(mix);
        dlclose(lib);
        return false;
    }

    bool ok = false;
    if (strcmp(op, "mic-mute") == 0 && p_has_cswitch && p_get_cswitch && p_set_cswitch) {
        const char *cap_names[] = {"Capture", "Mic", "Internal Mic", NULL};
        for (int i = 0; cap_names[i]; ++i) {
            p_id_set_idx(sid, 0);
            p_id_set_name(sid, cap_names[i]);
            snd_mixer_elem_t *el = p_find(mix, sid);
            if (el && p_has_cswitch(el)) {
                int sw = 1;
                p_get_cswitch(el, 0, &sw);
                int nsw = sw ? 0 : 1;
                p_set_cswitch(el, nsw);
                *out_vol = nsw ? 100 : 0;
                *out_muted = (nsw == 0);
                ok = true;
                break;
            }
        }
    } else {
        const char *play_names[] = {"Master", "Speaker", "PCM", "Headphone", NULL};
        for (int i = 0; play_names[i]; ++i) {
            p_id_set_idx(sid, 0);
            p_id_set_name(sid, play_names[i]);
            snd_mixer_elem_t *el = p_find(mix, sid);
            if (!el) continue;
            long vmin = 0, vmax = 0, vcur = 0;
            if (p_get_range(el, &vmin, &vmax) < 0 || vmax <= vmin) continue;
            p_get_vol(el, 0, &vcur);
            int pct = (int)lround(((double)(vcur - vmin) * 100.0) / (double)(vmax - vmin));
            pct = clamp_int(pct, 0, 100);
            int sw = 1;
            if (p_has_pswitch(el)) p_get_pswitch(el, 0, &sw);

            if (strcmp(op, "vol-up") == 0) {
                pct = clamp_int(pct + step, 0, 100);
                long nv = vmin + (long)lround(((double)pct / 100.0) * (double)(vmax - vmin));
                p_set_vol(el, nv);
                if (p_has_pswitch(el) && sw == 0) {
                    sw = 1;
                    p_set_pswitch(el, 1);
                }
            } else if (strcmp(op, "vol-down") == 0) {
                pct = clamp_int(pct - step, 0, 100);
                long nv = vmin + (long)lround(((double)pct / 100.0) * (double)(vmax - vmin));
                p_set_vol(el, nv);
            } else if (strcmp(op, "vol-mute") == 0 && p_has_pswitch(el)) {
                sw = sw ? 0 : 1;
                p_set_pswitch(el, sw);
            }
            *out_vol = pct;
            *out_muted = (sw == 0);
            ok = true;
            break;
        }
    }

    p_id_free(sid);
    p_close(mix);
    dlclose(lib);
    return ok;
}

static void handle_audio_action(const char *op, int step) {
    const char *mock_dir = getenv("CK_CDE_MEDIA_STATE_DIR");
    bool force_mock = (mock_dir && mock_dir[0]);

    int vol = 50;
    bool muted = false;

    if (!force_mock && has_prog_in_path("wpctl")) {
        const char *target = (strcmp(op, "mic-mute") == 0) ? "@DEFAULT_AUDIO_SOURCE@" : "@DEFAULT_AUDIO_SINK@";
        if (strcmp(op, "vol-up") == 0) {
            char delta[32];
            snprintf(delta, sizeof(delta), "%d%%+", step);
            char *av1[] = {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "0", NULL};
            char *av2[] = {"wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", delta, NULL};
            (void)run_capture(av1, NULL, 0);
            (void)run_capture(av2, NULL, 0);
        } else if (strcmp(op, "vol-down") == 0) {
            char delta[32];
            snprintf(delta, sizeof(delta), "%d%%-", step);
            char *av[] = {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", delta, NULL};
            (void)run_capture(av, NULL, 0);
        } else if (strcmp(op, "vol-mute") == 0 || strcmp(op, "mic-mute") == 0) {
            char *av[] = {"wpctl", "set-mute", (char *)target, "toggle", NULL};
            (void)run_capture(av, NULL, 0);
        }
        char out[256];
        char *qav[] = {"wpctl", "get-volume", (char *)target, NULL};
        if (run_capture(qav, out, sizeof(out)) && strncmp(out, "Volume:", 7) == 0) {
            double f = strtod(out + 7, NULL);
            vol = clamp_int((int)lround(f * 100.0), 0, 150);
            muted = (strstr(out, "[MUTED]") != NULL);
            if (strcmp(op, "mic-mute") == 0) {
                printf("OSD|mic|%d|%d|Microphone: %s\n", muted ? 0 : vol, muted ? 1 : 0, muted ? "Muted" : "Active");
            } else if (muted) {
                printf("OSD|volume|%d|1|Volume: Muted (%d%%)\n", vol, vol);
            } else {
                printf("OSD|volume|%d|0|Volume: %d%%\n", vol, vol);
            }
            return;
        }
    }

    if (!force_mock && has_prog_in_path("pactl")) {
        if (strcmp(op, "mic-mute") == 0) {
            char *av[] = {"pactl", "set-source-mute", "@DEFAULT_SOURCE@", "toggle", NULL};
            char out[256];
            if (run_capture(av, NULL, 0)) {
                char *qav[] = {"pactl", "get-source-mute", "@DEFAULT_SOURCE@", NULL};
                if (run_capture(qav, out, sizeof(out))) {
                    muted = (strstr(out, "yes") != NULL);
                    printf("OSD|mic|%d|%d|Microphone: %s\n", muted ? 0 : 100, muted ? 1 : 0, muted ? "Muted" : "Active");
                    return;
                }
            }
        } else {
            if (strcmp(op, "vol-up") == 0) {
                char delta[32];
                snprintf(delta, sizeof(delta), "+%d%%", step);
                char *av1[] = {"pactl", "set-sink-mute", "@DEFAULT_SINK@", "0", NULL};
                char *av2[] = {"pactl", "set-sink-volume", "@DEFAULT_SINK@", delta, NULL};
                (void)run_capture(av1, NULL, 0);
                (void)run_capture(av2, NULL, 0);
            } else if (strcmp(op, "vol-down") == 0) {
                char delta[32];
                snprintf(delta, sizeof(delta), "-%d%%", step);
                char *av[] = {"pactl", "set-sink-volume", "@DEFAULT_SINK@", delta, NULL};
                (void)run_capture(av, NULL, 0);
            } else if (strcmp(op, "vol-mute") == 0) {
                char *av[] = {"pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle", NULL};
                (void)run_capture(av, NULL, 0);
            }
            char vout[256], mout[128];
            char *qv[] = {"pactl", "get-sink-volume", "@DEFAULT_SINK@", NULL};
            char *qm[] = {"pactl", "get-sink-mute", "@DEFAULT_SINK@", NULL};
            if (run_capture(qv, vout, sizeof(vout)) && run_capture(qm, mout, sizeof(mout))) {
                const char *pct_p = strchr(vout, '%');
                if (pct_p) {
                    while (pct_p > vout && isdigit((unsigned char)*(pct_p - 1))) pct_p--;
                    vol = clamp_int(atoi(pct_p), 0, 100);
                }
                muted = (strstr(mout, "yes") != NULL);
                if (muted) printf("OSD|volume|%d|1|Volume: Muted (%d%%)\n", vol, vol);
                else printf("OSD|volume|%d|0|Volume: %d%%\n", vol, vol);
                return;
            }
        }
    }

    if (!force_mock && alsa_audio_op(op, step, &vol, &muted)) {
        if (strcmp(op, "mic-mute") == 0) {
            printf("OSD|mic|%d|%d|Microphone: %s\n", muted ? 0 : 100, muted ? 1 : 0, muted ? "Muted" : "Active");
        } else if (muted) {
            printf("OSD|volume|%d|1|Volume: Muted (%d%%)\n", vol, vol);
        } else {
            printf("OSD|volume|%d|0|Volume: %d%%\n", vol, vol);
        }
        return;
    }

    /* Deterministic state file (used in headless tests & hardwareless VMs) */
    char sdir[PATH_MAX], vfile[PATH_MAX], mfile[PATH_MAX], micfile[PATH_MAX];
    state_dir_path(sdir, sizeof(sdir));
    snprintf(vfile, sizeof(vfile), "%s/volume", sdir);
    snprintf(mfile, sizeof(mfile), "%s/muted", sdir);
    snprintf(micfile, sizeof(micfile), "%s/mic_muted", sdir);

    int m_int = 0, mic_int = 0;
    (void)read_int_file(vfile, &vol);
    (void)read_int_file(mfile, &m_int);
    (void)read_int_file(micfile, &mic_int);
    vol = clamp_int(vol, 0, 100);

    if (strcmp(op, "mic-mute") == 0) {
        mic_int = mic_int ? 0 : 1;
        (void)write_int_file(micfile, mic_int);
        printf("OSD|mic|%d|%d|Microphone: %s\n", mic_int ? 0 : 100, mic_int, mic_int ? "Muted" : "Active");
        return;
    }

    if (strcmp(op, "vol-up") == 0) {
        vol = clamp_int(vol + step, 0, 100);
        m_int = 0;
    } else if (strcmp(op, "vol-down") == 0) {
        vol = clamp_int(vol - step, 0, 100);
    } else if (strcmp(op, "vol-mute") == 0) {
        m_int = m_int ? 0 : 1;
    }
    (void)write_int_file(vfile, vol);
    (void)write_int_file(mfile, m_int);

    if (m_int) printf("OSD|volume|%d|1|Volume: Muted (%d%%)\n", vol, vol);
    else printf("OSD|volume|%d|0|Volume: %d%%\n", vol, vol);
}

static bool find_sysfs_backlight(bool is_kbd, char *out_dir, size_t out_sz, char *out_name, size_t name_sz) {
    const char *env_override = getenv(is_kbd ? "CK_CDE_KBD_BACKLIGHT_DIR" : "CK_CDE_BACKLIGHT_DIR");
    if (env_override && env_override[0]) {
        char maxp[PATH_MAX];
        snprintf(maxp, sizeof(maxp), "%s/max_brightness", env_override);
        if (access(maxp, R_OK) == 0) {
            snprintf(out_dir, out_sz, "%s", env_override);
            const char *slash = strrchr(env_override, '/');
            snprintf(out_name, name_sz, "%s", slash ? (slash + 1) : env_override);
            return true;
        }
    }

    const char *root = is_kbd ? "/sys/class/leds" : "/sys/class/backlight";
    DIR *dp = opendir(root);
    if (!dp) return false;
    struct dirent *de;
    bool found = false;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (is_kbd && !strstr(de->d_name, "kbd_backlight")) continue;
        char maxp[PATH_MAX], curp[PATH_MAX];
        snprintf(maxp, sizeof(maxp), "%s/%s/max_brightness", root, de->d_name);
        snprintf(curp, sizeof(curp), "%s/%s/brightness", root, de->d_name);
        if (access(maxp, R_OK) == 0 && access(curp, R_OK) == 0) {
            snprintf(out_dir, out_sz, "%s/%s", root, de->d_name);
            snprintf(out_name, name_sz, "%s", de->d_name);
            found = true;
            break;
        }
    }
    closedir(dp);
    return found;
}

static void handle_brightness_action(bool is_kbd, int delta_pct, bool get_only) {
    const char *kind = is_kbd ? "kbd_brightness" : "brightness";
    const char *prefix = is_kbd ? "Keyboard Light" : "Brightness";
    int min_pct = is_kbd ? 0 : 5;

    char bdir[PATH_MAX] = {0}, devname[256] = {0};
    if (find_sysfs_backlight(is_kbd, bdir, sizeof(bdir), devname, sizeof(devname))) {
        char curp[PATH_MAX], maxp[PATH_MAX];
        snprintf(curp, sizeof(curp), "%s/brightness", bdir);
        snprintf(maxp, sizeof(maxp), "%s/max_brightness", bdir);
        int cur = 0, maxv = 0;
        if (read_int_file(curp, &cur) && read_int_file(maxp, &maxv) && maxv > 0) {
            int pct = clamp_int((int)lround(((double)cur * 100.0) / (double)maxv), 0, 100);
            if (!get_only) {
                int target_pct = clamp_int(pct + delta_pct, min_pct, 100);
                int target_raw = clamp_int((int)lround(((double)target_pct / 100.0) * (double)maxv), 0, maxv);
                if (delta_pct > 0 && target_raw == cur && cur < maxv) target_raw = cur + 1;
                if (delta_pct < 0 && target_raw == cur && cur > 0) {
                    int min_raw = clamp_int(
                        (int)lround(((double)min_pct / 100.0) * (double)maxv),
                        (!is_kbd && maxv > 0) ? 1 : 0, maxv);
                    if (cur > min_raw) target_raw = cur - 1;
                }
                if (!write_int_file(curp, target_raw) && has_prog_in_path("dbus-send")) {
                    char subsys_arg[64], name_arg[300], val_arg[64];
                    snprintf(subsys_arg, sizeof(subsys_arg), "string:%s", is_kbd ? "leds" : "backlight");
                    snprintf(name_arg, sizeof(name_arg), "string:%s", devname);
                    snprintf(val_arg, sizeof(val_arg), "uint32:%d", target_raw);
                    char *av[] = {
                        "dbus-send", "--system", "--print-reply=literal",
                        "--dest=org.freedesktop.login1",
                        "/org/freedesktop/login1/session/auto",
                        "org.freedesktop.login1.Session.SetBrightness",
                        subsys_arg, name_arg, val_arg, NULL
                    };
                    (void)run_capture(av, NULL, 0);
                }
                pct = clamp_int((int)lround(((double)target_raw * 100.0) / (double)maxv), 0, 100);
            }
            printf("OSD|%s|%d|0|%s: %d%%\n", kind, pct, prefix, pct);
            return;
        }
    }

    /* Fallback state file when no sysfs backlight device is present */
    char sdir[PATH_MAX], bfile[PATH_MAX];
    state_dir_path(sdir, sizeof(sdir));
    snprintf(bfile, sizeof(bfile), "%s/%s", sdir, is_kbd ? "kbd_brightness" : "brightness");
    int pct = is_kbd ? 50 : 80;
    (void)read_int_file(bfile, &pct);
    pct = clamp_int(pct, min_pct, 100);
    if (!get_only) {
        pct = clamp_int(pct + delta_pct, min_pct, 100);
        (void)write_int_file(bfile, pct);
    }
    printf("OSD|%s|%d|0|%s: %d%%\n", kind, pct, prefix, pct);
}

static void handle_mpris_action(const char *method, const char *human_label) {
    const char *mock_dir = getenv("CK_CDE_MEDIA_STATE_DIR");
    if (mock_dir && mock_dir[0]) {
        char lpath[PATH_MAX];
        snprintf(lpath, sizeof(lpath), "%s/mpris.log", mock_dir);
        FILE *fp = fopen(lpath, "a");
        if (fp) {
            fprintf(fp, "%s\n", method);
            fclose(fp);
        }
    }

    if (getenv("DBUS_SESSION_BUS_ADDRESS") && has_prog_in_path("dbus-send")) {
        char names_out[4096];
        char *list_av[] = {
            "dbus-send", "--session", "--print-reply=literal",
            "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus.ListNames", NULL
        };
        if (run_capture(list_av, names_out, sizeof(names_out))) {
            char *save = NULL;
            for (char *tok = strtok_r(names_out, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
                if (strncmp(tok, "org.mpris.MediaPlayer2.", 23) == 0) {
                    char dest_arg[320], call_arg[128];
                    snprintf(dest_arg, sizeof(dest_arg), "--dest=%s", tok);
                    snprintf(call_arg, sizeof(call_arg), "org.mpris.MediaPlayer2.Player.%s", method);
                    char *call_av[] = {
                        "dbus-send", "--session", "--type=method_call",
                        dest_arg, "/org/mpris/MediaPlayer2", call_arg, NULL
                    };
                    (void)run_capture(call_av, NULL, 0);
                    break;
                }
            }
        }
    }

    printf("OSD|media|100|0|Media: %s\n", human_label);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: dtwm-media-ctl <vol-up|vol-down|vol-mute|vol-get|mic-mute|"
                        "bright-up|bright-down|bright-get|kbd-bright-up|kbd-bright-down|"
                        "media-play|media-pause|media-next|media-prev|media-stop> [step]\n");
        return 1;
    }

    const char *act = argv[1];
    int step = (argc >= 3) ? clamp_int(atoi(argv[2]), 1, 50) : 0;

    if (strcmp(act, "vol-up") == 0) {
        handle_audio_action("vol-up", step > 0 ? step : 5);
    } else if (strcmp(act, "vol-down") == 0) {
        handle_audio_action("vol-down", step > 0 ? step : 5);
    } else if (strcmp(act, "vol-mute") == 0) {
        handle_audio_action("vol-mute", 0);
    } else if (strcmp(act, "vol-get") == 0) {
        handle_audio_action("vol-get", 0);
    } else if (strcmp(act, "mic-mute") == 0) {
        handle_audio_action("mic-mute", 0);
    } else if (strcmp(act, "bright-up") == 0) {
        handle_brightness_action(false, step > 0 ? step : 10, false);
    } else if (strcmp(act, "bright-down") == 0) {
        handle_brightness_action(false, -(step > 0 ? step : 10), false);
    } else if (strcmp(act, "bright-get") == 0) {
        handle_brightness_action(false, 0, true);
    } else if (strcmp(act, "kbd-bright-up") == 0) {
        handle_brightness_action(true, step > 0 ? step : 25, false);
    } else if (strcmp(act, "kbd-bright-down") == 0) {
        handle_brightness_action(true, -(step > 0 ? step : 25), false);
    } else if (strcmp(act, "media-play") == 0) {
        handle_mpris_action("PlayPause", "Play / Pause");
    } else if (strcmp(act, "media-pause") == 0) {
        handle_mpris_action("Pause", "Pause");
    } else if (strcmp(act, "media-next") == 0) {
        handle_mpris_action("Next", "Next Track");
    } else if (strcmp(act, "media-prev") == 0) {
        handle_mpris_action("Previous", "Previous Track");
    } else if (strcmp(act, "media-stop") == 0) {
        handle_mpris_action("Stop", "Stop");
    } else {
        fprintf(stderr, "dtwm-media-ctl: unknown action '%s'\n", act);
        return 2;
    }
    return 0;
}
