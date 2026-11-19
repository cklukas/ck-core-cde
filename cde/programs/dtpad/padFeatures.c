#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#include "padFeatures.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t fnv1a_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    return h;
}

static void canonicalize_or_copy(const char *in_path, char *out_path, size_t out_len)
{
    if (!out_path || out_len == 0) return;
    out_path[0] = '\0';
    if (!in_path || !in_path[0]) return;

    char resolved[PATH_MAX];
    if (realpath(in_path, resolved)) {
        snprintf(out_path, out_len, "%s", resolved);
        return;
    }
    if (in_path[0] == '/') {
        snprintf(out_path, out_len, "%s", in_path);
        return;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(out_path, out_len, "%s/%s", cwd, in_path);
    } else {
        snprintf(out_path, out_len, "%s", in_path);
    }
}

static void ensure_parent_dir(const char *file_path)
{
    if (!file_path || !file_path[0]) return;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", file_path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = '\0';

    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *pslash = strrchr(parent, '/');
    if (pslash && pslash != parent) {
        *pslash = '\0';
        (void)mkdir(parent, 0700);
    }
    (void)mkdir(dir, 0700);
}

const char *dtpad_default_recent_path(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return NULL;
    const char *env = getenv("DTPAD_RECENT_FILE");
    if (env && env[0]) {
        snprintf(buf, buf_len, "%s", env);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(buf, buf_len, "%s/.dt/dtpad.recent", home);
    return buf;
}

const char *dtpad_default_autosave_dir(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return NULL;
    const char *env = getenv("DTPAD_AUTOSAVE_DIR");
    if (env && env[0]) {
        snprintf(buf, buf_len, "%s", env);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(buf, buf_len, "%s/.dt/dtpad-autosave", home);
    return buf;
}

int dtpad_recent_load(const char *store_path,
                      char out_paths[DTPAD_MAX_RECENT][PATH_MAX])
{
    if (!out_paths) return 0;
    for (int i = 0; i < DTPAD_MAX_RECENT; ++i) out_paths[i][0] = '\0';

    char def_buf[PATH_MAX];
    if (!store_path || !store_path[0]) {
        store_path = dtpad_default_recent_path(def_buf, sizeof(def_buf));
    }
    if (!store_path) return 0;

    FILE *fp = fopen(store_path, "r");
    if (!fp) return 0;

    int count = 0;
    char line[PATH_MAX + 16];
    while (count < DTPAD_MAX_RECENT && fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        int dup = 0;
        for (int j = 0; j < count; ++j) {
            if (strcmp(out_paths[j], line) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            snprintf(out_paths[count++], PATH_MAX, "%s", line);
        }
    }
    fclose(fp);
    return count;
}

int dtpad_recent_record(const char *store_path, const char *file_path)
{
    if (!file_path || !file_path[0]) return -1;
    char def_buf[PATH_MAX];
    if (!store_path || !store_path[0]) {
        store_path = dtpad_default_recent_path(def_buf, sizeof(def_buf));
    }
    if (!store_path) return -1;

    char canon[PATH_MAX];
    canonicalize_or_copy(file_path, canon, sizeof(canon));
    if (!canon[0]) return -1;

    char existing[DTPAD_MAX_RECENT][PATH_MAX];
    int old_cnt = dtpad_recent_load(store_path, existing);

    char updated[DTPAD_MAX_RECENT][PATH_MAX];
    int new_cnt = 0;
    snprintf(updated[new_cnt++], PATH_MAX, "%s", canon);
    for (int i = 0; i < old_cnt && new_cnt < DTPAD_MAX_RECENT; ++i) {
        if (strcmp(existing[i], canon) == 0) continue;
        snprintf(updated[new_cnt++], PATH_MAX, "%s", existing[i]);
    }

    ensure_parent_dir(store_path);
    char tmp_path[PATH_MAX + 32];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", store_path, (int)getpid());
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    for (int i = 0; i < new_cnt; ++i) {
        fprintf(fp, "%s\n", updated[i]);
    }
    if (fclose(fp) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, store_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

void dtpad_autosave_slot_path(const char *autosave_dir,
                              const char *target_path,
                              int pid,
                              int tab_id,
                              char *out_path,
                              size_t out_len)
{
    if (!out_path || out_len == 0) return;
    char def_dir[PATH_MAX];
    if (!autosave_dir || !autosave_dir[0]) {
        autosave_dir = dtpad_default_autosave_dir(def_dir, sizeof(def_dir));
    }
    if (target_path && target_path[0]) {
        char canon[PATH_MAX];
        canonicalize_or_copy(target_path, canon, sizeof(canon));
        uint64_t h = fnv1a_hash(canon);
        snprintf(out_path, out_len, "%s/file-%016llx.autosave",
                 autosave_dir, (unsigned long long)h);
    } else {
        snprintf(out_path, out_len, "%s/untitled-%d-%d.autosave",
                 autosave_dir, pid, tab_id);
    }
}

int dtpad_autosave_write(const char *autosave_dir,
                         const char *target_path,
                         int pid,
                         int tab_id,
                         const char *contents,
                         size_t len,
                         char *out_swap_path,
                         size_t swap_len)
{
    if (!contents) return -1;
    char def_dir[PATH_MAX];
    if (!autosave_dir || !autosave_dir[0]) {
        autosave_dir = dtpad_default_autosave_dir(def_dir, sizeof(def_dir));
    }

    char slot[PATH_MAX];
    dtpad_autosave_slot_path(autosave_dir, target_path, pid, tab_id, slot, sizeof(slot));
    ensure_parent_dir(slot);

    char canon[PATH_MAX] = "";
    if (target_path && target_path[0]) {
        canonicalize_or_copy(target_path, canon, sizeof(canon));
    }

    char tmp_path[PATH_MAX + 32];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", slot, pid);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;

    char hdr[PATH_MAX + 128];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "#DTPAD_AUTOSAVE_V1\nPID=%d\nPATH=%s\n---\n",
                        pid, canon);
    if (hlen < 0 || write(fd, hdr, (size_t)hlen) != (ssize_t)hlen) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t wr = write(fd, contents + off, len - off);
        if (wr <= 0) {
            close(fd);
            unlink(tmp_path);
            return -1;
        }
        off += (size_t)wr;
    }
    close(fd);

    if (rename(tmp_path, slot) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (out_swap_path && swap_len > 0) {
        snprintf(out_swap_path, swap_len, "%s", slot);
    }
    return 0;
}

int dtpad_autosave_remove_file(const char *swap_path)
{
    if (!swap_path || !swap_path[0]) return 0;
    if (unlink(swap_path) == 0 || errno == ENOENT) return 0;
    return -1;
}

int dtpad_autosave_remove_for_path(const char *autosave_dir,
                                   const char *target_path)
{
    if (!target_path || !target_path[0]) return 0;
    char slot[PATH_MAX];
    dtpad_autosave_slot_path(autosave_dir, target_path, 0, 0, slot, sizeof(slot));
    return dtpad_autosave_remove_file(slot);
}

static int parse_autosave_file(const char *swap_path,
                               int *out_pid,
                               char *out_target,
                               size_t target_len,
                               char **out_body,
                               size_t *out_len)
{
    FILE *fp = fopen(swap_path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz < 0 || sz > 64L * 1024L * 1024L) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    char *raw = (char *)malloc((size_t)sz + 1U);
    if (!raw) {
        fclose(fp);
        return -1;
    }
    if (sz > 0 && fread(raw, 1, (size_t)sz, fp) != (size_t)sz) {
        free(raw);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    raw[sz] = '\0';

    if (strncmp(raw, "#DTPAD_AUTOSAVE_V1\n", 19) != 0) {
        free(raw);
        return -1;
    }
    char *sep = strstr(raw + 19, "\n---\n");
    if (!sep) {
        free(raw);
        return -1;
    }

    int pid = 0;
    if (out_target && target_len > 0) out_target[0] = '\0';

    char *p = raw + 19;
    while (p < sep) {
        char *nl = strchr(p, '\n');
        if (!nl || nl > sep) nl = sep;
        size_t llen = (size_t)(nl - p);
        if (llen > 4 && strncmp(p, "PID=", 4) == 0) {
            pid = atoi(p + 4);
        } else if (llen >= 5 && strncmp(p, "PATH=", 5) == 0 && out_target && target_len > 0) {
            size_t plen = llen - 5;
            if (plen >= target_len) plen = target_len - 1;
            memcpy(out_target, p + 5, plen);
            out_target[plen] = '\0';
        }
        p = nl + 1;
    }

    char *body_start = sep + 5;
    size_t body_len = (size_t)(raw + sz - body_start);
    char *body = (char *)malloc(body_len + 1U);
    if (!body) {
        free(raw);
        return -1;
    }
    if (body_len > 0) memcpy(body, body_start, body_len);
    body[body_len] = '\0';
    free(raw);

    if (out_pid) *out_pid = pid;
    if (out_body) *out_body = body;
    else free(body);
    if (out_len) *out_len = body_len;
    return 0;
}

static int pid_is_alive(int pid)
{
    if (pid <= 1) return 0;
    if (pid == (int)getpid()) return 1;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return (errno == EPERM) ? 1 : 0;
}

int dtpad_autosave_find_recovery(const char *autosave_dir,
                                 const char *target_path,
                                 char **out_contents,
                                 size_t *out_len,
                                 char *out_swap_path,
                                 size_t swap_len)
{
    if (!out_contents || !out_len) return -1;
    *out_contents = NULL;
    *out_len = 0;

    char def_dir[PATH_MAX];
    if (!autosave_dir || !autosave_dir[0]) {
        autosave_dir = dtpad_default_autosave_dir(def_dir, sizeof(def_dir));
    }

    if (target_path && target_path[0]) {
        char slot[PATH_MAX];
        dtpad_autosave_slot_path(autosave_dir, target_path, 0, 0, slot, sizeof(slot));
        struct stat st_swap;
        if (stat(slot, &st_swap) != 0) return -1;

        int owner_pid = 0;
        char recorded_path[PATH_MAX];
        char *body = NULL;
        size_t blen = 0;
        if (parse_autosave_file(slot, &owner_pid, recorded_path, sizeof(recorded_path),
                                &body, &blen) != 0) {
            return -1;
        }
        if (pid_is_alive(owner_pid)) {
            free(body);
            return -1;
        }

        struct stat st_target;
        if (stat(target_path, &st_target) == 0 && st_target.st_mtime > st_swap.st_mtime) {
            /* Disk file was saved more recently than the orphaned swap file */
            free(body);
            unlink(slot);
            return -1;
        }

        *out_contents = body;
        *out_len = blen;
        if (out_swap_path && swap_len > 0) snprintf(out_swap_path, swap_len, "%s", slot);
        return 0;
    }

    /* Untitled crash recovery: scan untitled-*.autosave from dead PIDs */
    DIR *dp = opendir(autosave_dir);
    if (!dp) return -1;

    char best_slot[PATH_MAX + 512] = "";
    time_t best_mtime = 0;

    struct dirent *ent = NULL;
    while ((ent = readdir(dp)) != NULL) {
        if (strncmp(ent->d_name, "untitled-", 9) != 0) continue;
        size_t dlen = strlen(ent->d_name);
        if (dlen < 18 || strcmp(ent->d_name + dlen - 9, ".autosave") != 0) continue;

        char full[PATH_MAX + 512];
        snprintf(full, sizeof(full), "%s/%s", autosave_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        int owner_pid = 0;
        if (parse_autosave_file(full, &owner_pid, NULL, 0, NULL, NULL) != 0) continue;
        if (pid_is_alive(owner_pid)) continue;

        if (!best_slot[0] || st.st_mtime >= best_mtime) {
            best_mtime = st.st_mtime;
            snprintf(best_slot, sizeof(best_slot), "%s", full);
        }
    }
    closedir(dp);

    if (!best_slot[0]) return -1;
    int owner_pid = 0;
    char *body = NULL;
    size_t blen = 0;
    if (parse_autosave_file(best_slot, &owner_pid, NULL, 0, &body, &blen) != 0) {
        return -1;
    }
    *out_contents = body;
    *out_len = blen;
    if (out_swap_path && swap_len > 0) snprintf(out_swap_path, swap_len, "%s", best_slot);
    return 0;
}
