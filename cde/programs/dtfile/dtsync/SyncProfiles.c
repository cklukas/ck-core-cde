/* SPDX-License-Identifier: LGPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "SyncProfiles.h"
#if defined(__APPLE__) && defined(SYNC_PROFILES_TEST)
#include <sys/acl.h>
static acl_t test_storage_acl_get(int fd);
#define SYNC_STORAGE_ACL_GET(fd) test_storage_acl_get(fd)
#endif
#include "SyncPrivateStorage.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STORE_DIR ".dtfile-sync"
#define STORE_FILE "profiles.v1"
#define STORE_LOCK ".profiles.lock"
#define STORE_BYTES_MAX (9u * 1024u * 1024u)

typedef struct { uint32_t count; SyncProfile *items[SYNC_PROFILES_MAX]; } ProfileDB;
typedef struct {
    SyncProfilesResult result;
    char text[512];
} ProfileError;
typedef struct { unsigned char *bytes; size_t length, offset; } ProfileBytes;
typedef struct { int present; struct stat stat; } StoreIdentity;
struct SyncProfiles {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    atomic_int stop, finished;
    char *home;
    int busy, reload, deleting;
    char deletion[SYNC_PROFILE_NAME_MAX + 1];
    SyncProfile *pending;
    ProfileDB *db;
    SyncProfilesSnapshot snapshot;
};

static void profile_error(ProfileError *e, SyncProfilesResult result, const char *text)
{
    e->result = result;
    snprintf(e->text, sizeof(e->text), "%s", text);
}
static void profile_errno(ProfileError *e, const char *operation)
{
    int code = errno;
    e->result = SYNC_PROFILES_IO;
    snprintf(e->text, sizeof(e->text), "%s: %s", operation, strerror(code));
}
static void db_free(ProfileDB *db)
{
    if (!db) return;
    for (uint32_t i = 0; i < db->count; ++i) free(db->items[i]);
    free(db);
}
static int bounded_length(const char *s, size_t maximum, size_t *length)
{
    *length = strnlen(s, maximum + 1);
    if (*length > maximum) return 0;
    return 1;
}
/* UTF-8 names are UI labels. Filesystem paths intentionally have no UTF-8 test. */
static int valid_name(const char *name)
{
    size_t n;
    if (!bounded_length(name, SYNC_PROFILE_NAME_MAX, &n) || !n) return 0;
    const unsigned char *p = (const unsigned char *)name;
    for (size_t i = 0; i < n;) {
        uint32_t c = p[i++], minimum;
        unsigned trailing;
        if (c < 0x80) { if (c < 0x20 || c == 0x7f) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { trailing = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { trailing = 2; minimum = 0x800; c &= 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { trailing = 3; minimum = 0x10000; c &= 7; }
        else return 0;
        if (trailing > n - i) return 0;
        while (trailing--) {
            if ((p[i] & 0xc0) != 0x80) return 0;
            c = (c << 6) | (p[i++] & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
            (c >= 0x80 && c <= 0x9f)) return 0;
    }
    return 1;
}
static int time_before(const ckfsp_timestamp *a, const ckfsp_timestamp *b)
{
    return a->seconds < b->seconds ||
           (a->seconds == b->seconds && a->nanoseconds < b->nanoseconds);
}
/* Wire-format validation only: selecting a preference never qualifies a rule
 * for a native operation. That is always the fresh backend review's job. */
static SyncProfilesResult profile_validate(const SyncProfile *p)
{
    size_t n;
    if (!p || p->version != SYNC_PROFILES_VERSION) return SYNC_PROFILES_INVALID;
    if (!bounded_length(p->name, SYNC_PROFILE_NAME_MAX, &n)) return SYNC_PROFILES_LIMIT;
    if (!valid_name(p->name)) return SYNC_PROFILES_INVALID;
    if (!bounded_length(p->source, SYNC_PROFILE_PATH_MAX, &n) ||
        !bounded_length(p->destination, SYNC_PROFILE_PATH_MAX, &n))
        return SYNC_PROFILES_LIMIT;
    if (p->source[0] != '/' || p->destination[0] != '/') return SYNC_PROFILES_INVALID;
    if (p->profile > CKFSP_COMPARE || p->metadata_scope > 1) return SYNC_PROFILES_INVALID;
    const ckfsp_filters *f = &p->filters;
    if (f->has_minimum_size > 1 || f->has_maximum_size > 1 ||
        f->has_modified_not_before > 1 || f->has_modified_before > 1 ||
        f->modified_not_before.nanoseconds >= 1000000000 ||
        f->modified_before.nanoseconds >= 1000000000 ||
        f->modified_not_before.reserved || f->modified_before.reserved ||
        (f->allowed_kind_mask & ~CKFSP_ALL_KINDS)) return SYNC_PROFILES_INVALID;
    if (f->maximum_depth > SYNC_PROFILE_DEPTH_MAX || p->rule_count > SYNC_PROFILE_RULES_MAX)
        return SYNC_PROFILES_LIMIT;
    if ((f->has_minimum_size && f->has_maximum_size && f->minimum_size > f->maximum_size) ||
        (f->has_modified_not_before && f->has_modified_before &&
         !time_before(&f->modified_not_before, &f->modified_before)))
        return SYNC_PROFILES_INVALID;
    for (uint32_t i = 0; i < p->rule_count; ++i) {
        const SyncProfileRule *r = &p->rules[i];
        if (r->kind > CKFET_CONFLICT_CAPACITY || r->choice > CKFET_DELETE_EDITED ||
            r->tie > CKFET_DELETE_EDITED || r->protect_nonempty > 1 || r->retain_replaced > 1)
            return SYNC_PROFILES_INVALID;
    }
    return SYNC_PROFILES_OK;
}
static int bytes_take(ProfileBytes *b, void *out, size_t n)
{
    if (n > b->length - b->offset) return 0;
    if (n) memcpy(out, b->bytes + b->offset, n);
    b->offset += n;
    return 1;
}
static int get32(ProfileBytes *b, uint32_t *v)
{
    unsigned char p[4];
    if (!bytes_take(b, p, 4)) return 0;
    *v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return 1;
}
static int get64(ProfileBytes *b, uint64_t *v)
{
    uint32_t hi, lo;
    if (!get32(b, &hi) || !get32(b, &lo)) return 0;
    *v = ((uint64_t)hi << 32) | lo;
    return 1;
}
static int get_time(ProfileBytes *b, ckfsp_timestamp *t)
{
    uint64_t seconds;
    if (!get64(b, &seconds) || !get32(b, &t->nanoseconds)) return 0;
    t->seconds = seconds <= INT64_MAX ? (int64_t)seconds : -1 - (int64_t)(~seconds);
    t->reserved = 0;
    return 1;
}
static int bytes_put(ProfileBytes *b, const void *in, size_t n)
{
    if (n > b->length - b->offset) return 0;
    if (n) memcpy(b->bytes + b->offset, in, n);
    b->offset += n;
    return 1;
}
static int put32(ProfileBytes *b, uint32_t v)
{
    unsigned char p[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16),
                           (unsigned char)(v >> 8), (unsigned char)v };
    return bytes_put(b, p, 4);
}
static int put64(ProfileBytes *b, uint64_t v)
{
    return put32(b, (uint32_t)(v >> 32)) && put32(b, (uint32_t)v);
}
static int put_time(ProfileBytes *b, const ckfsp_timestamp *t)
{
    return put64(b, (uint64_t)t->seconds) && put32(b, t->nanoseconds);
}
static ProfileDB *db_decode(ProfileBytes *b, ProfileError *error)
{
    unsigned char magic[8];
    uint32_t version, count;
    if (!bytes_take(b, magic, 8) || memcmp(magic, "DTSYNCP1", 8) ||
        !get32(b, &version) || version != SYNC_PROFILES_VERSION ||
        !get32(b, &count) || count > SYNC_PROFILES_MAX) goto corrupt;
    ProfileDB *db = calloc(1, sizeof(*db));
    if (!db) { profile_error(error, SYNC_PROFILES_NOMEM, "Cannot allocate profile database"); return NULL; }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t name, source, destination, rules;
        if (!get32(b, &name) || !name || name > SYNC_PROFILE_NAME_MAX ||
            !get32(b, &source) || !source || source > SYNC_PROFILE_PATH_MAX ||
            !get32(b, &destination) || !destination || destination > SYNC_PROFILE_PATH_MAX ||
            !get32(b, &rules) || rules > SYNC_PROFILE_RULES_MAX) goto bad_db;
        SyncProfile *p = calloc(1, sizeof(*p));
        if (!p) { db_free(db); profile_error(error, SYNC_PROFILES_NOMEM, "Cannot allocate saved profile"); return NULL; }
        db->items[db->count++] = p;
        p->version = SYNC_PROFILES_VERSION;
        p->rule_count = rules;
        ckfsp_filters *f = &p->filters;
        if (!get32(b, &p->profile) || !get32(b, &p->metadata_scope) ||
            !get32(b, &f->has_minimum_size) || !get32(b, &f->has_maximum_size) ||
            !get64(b, &f->minimum_size) || !get64(b, &f->maximum_size) ||
            !get32(b, &f->has_modified_not_before) || !get32(b, &f->has_modified_before) ||
            !get_time(b, &f->modified_not_before) || !get_time(b, &f->modified_before) ||
            !get64(b, &f->allowed_kind_mask) || !get64(b, &f->maximum_depth) ||
            !bytes_take(b, p->name, name) || !bytes_take(b, p->source, source) ||
            !bytes_take(b, p->destination, destination) ||
            memchr(p->name, 0, name) || memchr(p->source, 0, source) ||
            memchr(p->destination, 0, destination)) goto bad_db;
        for (uint32_t j = 0; j < rules; ++j) {
            SyncProfileRule *r = &p->rules[j];
            if (!get32(b, &r->kind) || !get32(b, &r->choice) || !get32(b, &r->tie) ||
                !get32(b, &r->protect_nonempty) || !get32(b, &r->retain_replaced)) goto bad_db;
        }
        if (profile_validate(p) != SYNC_PROFILES_OK) goto bad_db;
        for (uint32_t j = 0; j < i; ++j)
            if (!strcmp(p->name, db->items[j]->name)) goto bad_db;
    }
    if (b->offset != b->length) goto bad_db;
    return db;
bad_db:
    db_free(db);
corrupt:
    profile_error(error, SYNC_PROFILES_CORRUPT,
                  "Saved profiles have an unsupported version or invalid/truncated data; store retained unchanged");
    return NULL;
}
static int db_encode(const ProfileDB *db, ProfileBytes *b, ProfileError *error)
{
    /* Maximum resident serialization is bounded independently of file contents. */
    b->length = STORE_BYTES_MAX;
    b->offset = 0;
    b->bytes = malloc(b->length);
    if (!b->bytes) { profile_error(error, SYNC_PROFILES_NOMEM, "Cannot serialize saved profiles"); return 0; }
    if (!bytes_put(b, "DTSYNCP1", 8) || !put32(b, SYNC_PROFILES_VERSION) || !put32(b, db->count)) goto limit;
    for (uint32_t i = 0; i < db->count; ++i) {
        const SyncProfile *p = db->items[i];
        const ckfsp_filters *f = &p->filters;
        size_t name = strlen(p->name), source = strlen(p->source), destination = strlen(p->destination);
        if (!put32(b, (uint32_t)name) || !put32(b, (uint32_t)source) ||
            !put32(b, (uint32_t)destination) || !put32(b, p->rule_count) ||
            !put32(b, p->profile) || !put32(b, p->metadata_scope) ||
            !put32(b, f->has_minimum_size) || !put32(b, f->has_maximum_size) ||
            !put64(b, f->minimum_size) || !put64(b, f->maximum_size) ||
            !put32(b, f->has_modified_not_before) || !put32(b, f->has_modified_before) ||
            !put_time(b, &f->modified_not_before) || !put_time(b, &f->modified_before) ||
            !put64(b, f->allowed_kind_mask) || !put64(b, f->maximum_depth) ||
            !bytes_put(b, p->name, name) || !bytes_put(b, p->source, source) ||
            !bytes_put(b, p->destination, destination)) goto limit;
        for (uint32_t j = 0; j < p->rule_count; ++j) {
            const SyncProfileRule *r = &p->rules[j];
            if (!put32(b, r->kind) || !put32(b, r->choice) || !put32(b, r->tie) ||
                !put32(b, r->protect_nonempty) || !put32(b, r->retain_replaced)) goto limit;
        }
    }
    b->length = b->offset;
    b->offset = 0;
    return 1;
limit:
    free(b->bytes); b->bytes = NULL;
    profile_error(error, SYNC_PROFILES_LIMIT, "Saved profile payload exceeds the store limit; no profiles dropped");
    return 0;
}

/* Translate the shared storage policy into the preferences API result domain. */
static int storage_result(SyncStorageStatus status, ProfileError *error)
{
    if (status == SYNC_STORAGE_OK) return 1;
    error->result = status == SYNC_STORAGE_NOMEM ? SYNC_PROFILES_NOMEM :
        status == SYNC_STORAGE_IO ? SYNC_PROFILES_IO : SYNC_PROFILES_UNSAFE;
    return 0;
}
static int private_stat(int fd, int directory, ProfileError *error)
{
    return storage_result(SyncStorageCheck(fd, directory ? SYNC_STORAGE_DIRECTORY : SYNC_STORAGE_FILE,
                                         error->text, sizeof(error->text)), error);
}
static int open_home(SyncProfiles *s, ProfileError *error)
{
    int fd = -1;
    storage_result(SyncStorageOpenHome(s->home, &fd, error->text, sizeof(error->text)), error);
    return fd;
}
#ifdef SYNC_PROFILES_TEST
static int test_home_flush(int fd, int created);
#endif
static int flush_home(int fd, int created)
{
#ifdef SYNC_PROFILES_TEST
    return test_home_flush(fd, created);
#else
    (void)created;
    return fsync(fd);
#endif
}
static int profile_directory(SyncProfiles *s, ProfileError *error)
{
    int home = open_home(s, error);
    if (home < 0) return -1;
    int created = mkdirat(home, STORE_DIR, 0700) == 0;
    if (!created && errno != EEXIST) { profile_errno(error, "Create private profile directory"); close(home); return -1; }
    int dir = openat(home, STORE_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) { profile_errno(error, "Open private profile directory without following links"); close(home); return -1; }
    if (!private_stat(dir, 1, error)) { close(home); close(dir); return -1; }
    /* EEXIST can mean another creator has not flushed this entry yet. Every
     * adopter establishes parent durability independently, before store I/O. */
    if (flush_home(home, created) < 0) {
        profile_errno(error, "Flush profile directory entry in home"); close(home); close(dir); return -1;
    }
    close(home);
    return dir;
}
static int same_identity(const struct stat *a, const struct stat *b)
{
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino || a->st_size != b->st_size ||
        a->st_uid != b->st_uid || a->st_mode != b->st_mode || a->st_nlink != b->st_nlink) return 0;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}
static int store_unchanged(int dir, const char *name, const StoreIdentity *identity,
                           ProfileError *error)
{
    struct stat st;
    int result = fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW);
    if (result < 0 && errno == ENOENT && !identity->present) return 1;
    if (result < 0) { profile_errno(error, "Recheck profile store entry"); return 0; }
    if (!identity->present || !same_identity(&st, &identity->stat)) {
        profile_error(error, SYNC_PROFILES_UNSAFE,
                      "Profile store changed outside the cooperative lock; existing data retained");
        return 0;
    }
    return 1;
}
static int profile_lock(SyncProfiles *s, int dir, ProfileError *error)
{
    /* flock is per open description: independent workers in this process also
     * serialize, unlike process-scoped POSIX record locks. No global lock state. */
    int fd = -1;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        fd = openat(dir, STORE_LOCK, O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd >= 0) break;
        if (errno == EINTR) continue;
        if (errno != ENOENT) break;
        /* Exclusive creation distinguishes a competing creator from an invalid
         * existing entry. Reopen the winner without following symbolic links. */
        fd = openat(dir, STORE_LOCK,
                    O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST && errno != EINTR) break;
    }
    if (fd < 0) { profile_errno(error, "Open cooperative profile lock without following links"); return -1; }
    if (!private_stat(fd, 0, error)) { close(fd); return -1; }
    while (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
            profile_errno(error, "Acquire cooperative profile lock"); close(fd); return -1;
        }
        if (atomic_load(&s->stop)) {
            profile_error(error, SYNC_PROFILES_CANCELLED, "Profile operation cancelled while waiting for another writer");
            close(fd); return -1;
        }
        struct timespec delay = { 0, 20000000 };
        nanosleep(&delay, NULL);
    }
    StoreIdentity identity = { 0 };
    identity.present = 1;
    if (fstat(fd, &identity.stat) < 0) { profile_errno(error, "Inspect held profile lock"); close(fd); return -1; }
    if (!private_stat(fd, 0, error) || !store_unchanged(dir, STORE_LOCK, &identity, error)) {
        close(fd); return -1;
    }
    return fd;
}
static ProfileDB *store_read(SyncProfiles *s, int dir, StoreIdentity *identity, ProfileError *error)
{
    int fd = openat(dir, STORE_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    memset(identity, 0, sizeof(*identity));
    if (fd < 0) {
        if (errno != ENOENT) { profile_errno(error, "Read saved profiles without following links"); return NULL; }
        ProfileDB *empty = calloc(1, sizeof(*empty));
        if (!empty) profile_error(error, SYNC_PROFILES_NOMEM, "Cannot allocate empty profile list");
        return empty;
    }
    identity->present = 1;
    if (!private_stat(fd, 0, error)) { close(fd); return NULL; }
    if (fstat(fd, &identity->stat) < 0) { profile_errno(error, "Inspect saved profile size"); close(fd); return NULL; }
    if (identity->stat.st_size < 16 || identity->stat.st_size > STORE_BYTES_MAX) {
        profile_error(error, SYNC_PROFILES_CORRUPT, "Saved profile store is empty, truncated or exceeds its bounded payload; retained unchanged");
        close(fd); return NULL;
    }
    ProfileBytes bytes = { NULL, (size_t)identity->stat.st_size, 0 };
    bytes.bytes = malloc(bytes.length);
    if (!bytes.bytes) { profile_error(error, SYNC_PROFILES_NOMEM, "Cannot read bounded profile store"); close(fd); return NULL; }
    size_t received = 0;
    while (received < bytes.length) {
        if (atomic_load(&s->stop)) {
            profile_error(error, SYNC_PROFILES_CANCELLED, "Profile read cancelled"); goto failure;
        }
        ssize_t n = read(fd, bytes.bytes + received, bytes.length - received);
        if (n > 0) received += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else {
            if (n < 0) profile_errno(error, "Read saved profile payload");
            else profile_error(error, SYNC_PROFILES_CORRUPT, "Saved profile payload is truncated; retained unchanged");
            goto failure;
        }
    }
    struct stat after;
    if (fstat(fd, &after) < 0) { profile_errno(error, "Recheck saved profile payload"); goto failure; }
    if (!same_identity(&identity->stat, &after) || !store_unchanged(dir, STORE_FILE, identity, error)) {
        if (error->result == SYNC_PROFILES_OK)
            profile_error(error, SYNC_PROFILES_UNSAFE, "Saved profiles changed during read; retained unchanged");
        goto failure;
    }
    if (close(fd) < 0) { fd = -1; profile_errno(error, "Close saved profile input"); goto failure; }
    ProfileDB *db = db_decode(&bytes, error);
    free(bytes.bytes);
    return db;
failure:
    if (fd >= 0) close(fd);
    free(bytes.bytes);
    return NULL;
}
static int temporary_name(char name[64], ProfileError *error)
{
    unsigned char random[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { profile_errno(error, "Open profile temporary-name entropy"); return 0; }
    size_t have = 0;
    while (have < sizeof(random)) {
        ssize_t n = read(fd, random + have, sizeof(random) - have);
        if (n > 0) have += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else {
            if (n < 0) profile_errno(error, "Read profile temporary-name entropy");
            else profile_error(error, SYNC_PROFILES_IO, "Profile temporary-name entropy ended unexpectedly");
            close(fd); return 0;
        }
    }
    close(fd);
    static const char hex[] = "0123456789abcdef";
    memcpy(name, ".profiles-new-", 14);
    for (size_t i = 0; i < sizeof(random); ++i) {
        name[14 + i * 2] = hex[random[i] >> 4];
        name[15 + i * 2] = hex[random[i] & 15];
    }
    name[46] = 0;
    return 1;
}
static void temporary_remove(int dir, const char *name, ProfileError *error)
{
    if (unlinkat(dir, name, 0) < 0) {
        int code = errno;
        size_t used = strlen(error->text);
        if (error->result == SYNC_PROFILES_OK) error->result = SYNC_PROFILES_IO;
        snprintf(error->text + used, sizeof(error->text) - used,
                 "; cannot remove private staging file %s: %s", name, strerror(code));
    }
}
/* Returns true after rename, even if directory fsync fails. In that case the
 * caller publishes the visible DB but reports DURABILITY, never false success. */
static int store_write(SyncProfiles *s, int dir, const StoreIdentity *identity,
                       const ProfileDB *db, ProfileError *error)
{
    ProfileBytes bytes = { 0 };
    if (!db_encode(db, &bytes, error)) return 0;
    char name[64];
    if (!temporary_name(name, error)) { free(bytes.bytes); return 0; }
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { profile_errno(error, "Create private staging file in profile directory"); free(bytes.bytes); return 0; }
    int committed = 0;
    if (fchmod(fd, 0600) < 0) { profile_errno(error, "Set private staging file permissions"); goto done; }
    if (!private_stat(fd, 0, error)) goto done;
    size_t sent = 0;
    while (sent < bytes.length) {
        if (atomic_load(&s->stop)) { profile_error(error, SYNC_PROFILES_CANCELLED, "Profile save cancelled before commit"); goto done; }
        ssize_t n = write(fd, bytes.bytes + sent, bytes.length - sent);
        if (n > 0) sent += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { if (!n) errno = EIO; profile_errno(error, "Write profile staging payload"); goto done; }
    }
    if (fsync(fd) < 0) { profile_errno(error, "Flush profile staging payload"); goto done; }
    if (!store_unchanged(dir, STORE_FILE, identity, error)) goto done;
    StoreIdentity staging = { 0 };
    staging.present = 1;
    if (fstat(fd, &staging.stat) < 0) { profile_errno(error, "Inspect flushed profile staging file"); goto done; }
    if (!private_stat(fd, 0, error) || !store_unchanged(dir, name, &staging, error)) goto done;
    if (close(fd) < 0) { fd = -1; profile_errno(error, "Close flushed profile staging file"); goto done; }
    fd = -1;
    if (atomic_load(&s->stop)) { profile_error(error, SYNC_PROFILES_CANCELLED, "Profile save cancelled before commit"); goto done; }
    if (renameat(dir, name, dir, STORE_FILE) < 0) { profile_errno(error, "Commit saved profile replacement"); goto done; }
    committed = 1;
    if (fsync(dir) < 0) {
        int code = errno;
        error->result = SYNC_PROFILES_DURABILITY;
        snprintf(error->text, sizeof(error->text),
                 "Profile rename committed but directory flush failed; durability is uncertain: %s", strerror(code));
    }
done:
    if (fd >= 0 && close(fd) < 0 && error->result == SYNC_PROFILES_OK)
        profile_errno(error, "Close profile staging file");
    if (!committed) temporary_remove(dir, name, error);
    free(bytes.bytes);
    return committed;
}
static ProfileDB *profile_operation(SyncProfiles *s, const SyncProfile *save,
                                    const char *deletion, ProfileError *error)
{
    int dir = profile_directory(s, error);
    if (dir < 0) return NULL;
    int lock = profile_lock(s, dir, error);
    if (lock < 0) { close(dir); return NULL; }
    StoreIdentity identity;
    ProfileDB *db = store_read(s, dir, &identity, error);
    if (db && save) {
        uint32_t index;
        for (index = 0; index < db->count; ++index)
            if (!strcmp(save->name, db->items[index]->name)) break;
        if (index == db->count && db->count == SYNC_PROFILES_MAX) {
            profile_error(error, SYNC_PROFILES_LIMIT, "Profile store already contains 64 names; no existing profile was discarded");
            db_free(db); db = NULL;
        } else {
            SyncProfile *copy = malloc(sizeof(*copy));
            if (!copy) {
                profile_error(error, SYNC_PROFILES_NOMEM, "Cannot merge saved profile"); db_free(db); db = NULL;
            } else {
                *copy = *save;
                if (index == db->count) ++db->count;
                else free(db->items[index]);
                db->items[index] = copy;
                if (!store_write(s, dir, &identity, db, error)) { db_free(db); db = NULL; }
            }
        }
    }
    if (db && deletion) {
        uint32_t index;
        for (index = 0; index < db->count; ++index)
            if (!strcmp(deletion, db->items[index]->name)) break;
        if (index == db->count) {
            profile_error(error, SYNC_PROFILES_NOT_FOUND, "Named preference no longer exists; no other profile was removed");
        } else {
            free(db->items[index]);
            memmove(db->items + index, db->items + index + 1,
                    (db->count - index - 1) * sizeof(db->items[0]));
            db->items[--db->count] = NULL;
            if (!store_write(s, dir, &identity, db, error)) { db_free(db); db = NULL; }
        }
    }
    /* Closing the independent lock description releases it for other helpers. */
    if (close(lock) < 0 && error->result == SYNC_PROFILES_OK)
        profile_errno(error, "Release cooperative profile lock");
    if (close(dir) < 0 && error->result == SYNC_PROFILES_OK)
        profile_errno(error, "Close private profile directory");
    return db;
}

static void publish(SyncProfiles *s, ProfileDB *db, const ProfileError *error, uint64_t serial)
{
    /* Assemble the bounded name list before taking the UI-facing mutex. */
    SyncProfilesSnapshot next;
    memset(&next, 0, sizeof(next));
    next.completed = serial;
    next.result = error->result;
    next.state = error->result == SYNC_PROFILES_OK ? SYNC_PROFILES_READY : SYNC_PROFILES_ERROR;
    snprintf(next.error, sizeof(next.error), "%s", error->text);
    if (db) {
        next.count = db->count;
        for (uint32_t i = 0; i < db->count; ++i)
            memcpy(next.names[i], db->items[i]->name, sizeof(next.names[i]));
    }
    pthread_mutex_lock(&s->mutex);
    ProfileDB *old = NULL;
    if (db) { old = s->db; s->db = db; }
    else { next.count = s->snapshot.count; memcpy(next.names, s->snapshot.names, sizeof(next.names)); }
    next.submitted = s->snapshot.submitted;
    next.revision = s->snapshot.revision + 1;
    s->snapshot = next;
    s->busy = 0;
    pthread_mutex_unlock(&s->mutex);
    db_free(old);
}
static void *profiles_worker(void *arg)
{
    SyncProfiles *s = arg;
    ProfileError error = { SYNC_PROFILES_OK, { 0 } };
    ProfileDB *db = profile_operation(s, NULL, NULL, &error);
    publish(s, db, &error, 1);
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        while (!s->pending && !s->reload && !s->deleting && !atomic_load(&s->stop)) {
            /* Stop does not acquire the mutex on the UI thread. Timed waiting
             * bounds a signal lost between the predicate and cond_wait. */
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 100000000;
            if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&s->wake, &s->mutex, &until);
        }
        if (atomic_load(&s->stop)) {
            if (s->pending || s->reload || s->deleting) {
                s->snapshot.result = SYNC_PROFILES_CANCELLED;
                snprintf(s->snapshot.error, sizeof(s->snapshot.error), "Pending profile operation cancelled");
                s->snapshot.completed = s->snapshot.submitted;
            }
            s->snapshot.state = SYNC_PROFILES_FINISHED;
            ++s->snapshot.revision;
            pthread_mutex_unlock(&s->mutex);
            break;
        }
        SyncProfile *save = s->pending;
        char deletion[SYNC_PROFILE_NAME_MAX + 1] = { 0 };
        if (s->deleting) memcpy(deletion, s->deletion, sizeof(deletion));
        s->pending = NULL;
        s->reload = s->deleting = 0;
        uint64_t serial = s->snapshot.submitted;
        pthread_mutex_unlock(&s->mutex);
        memset(&error, 0, sizeof(error));
        db = profile_operation(s, save, deletion[0] ? deletion : NULL, &error);
        free(save);
        publish(s, db, &error, serial);
    }
    /* No native I/O or access to s follows this release publication. */
    atomic_store_explicit(&s->finished, 1, memory_order_release);
    return NULL;
}
SyncProfiles *SyncProfilesStart(const char *home, char *error, size_t error_size)
{
    size_t length;
    int code = 0;
    if (!home || home[0] != '/' || !bounded_length(home, SYNC_PROFILE_PATH_MAX, &length)) {
        if (error && error_size) snprintf(error, error_size, "Profile home must be an absolute bounded path");
        return NULL;
    }
    SyncProfiles *s = calloc(1, sizeof(*s));
    if (!s) goto no_memory;
    s->home = malloc(length + 1);
    if (!s->home) { free(s); goto no_memory; }
    memcpy(s->home, home, length + 1);
    while (length > 1 && s->home[length - 1] == '/') s->home[--length] = 0;
    atomic_init(&s->stop, 0);
    atomic_init(&s->finished, 0);
    s->busy = 1;
    s->snapshot.revision = 1;
    s->snapshot.submitted = 1;
    s->snapshot.state = SYNC_PROFILES_LOADING;
    code = pthread_mutex_init(&s->mutex, NULL);
    if (code) goto thread_error;
    code = pthread_cond_init(&s->wake, NULL);
    if (code) { pthread_mutex_destroy(&s->mutex); goto thread_error; }
    code = pthread_create(&s->thread, NULL, profiles_worker, s);
    if (code) { pthread_cond_destroy(&s->wake); pthread_mutex_destroy(&s->mutex); goto thread_error; }
    if (error && error_size) error[0] = 0;
    return s;
thread_error:
    if (error && error_size) snprintf(error, error_size, "Start profile worker: %s", strerror(code));
    free(s->home); free(s);
    return NULL;
no_memory:
    if (error && error_size) snprintf(error, error_size, "Cannot allocate profile worker");
    return NULL;
}
SyncProfilesResult SyncProfilesGetSnapshot(SyncProfiles *s, SyncProfilesSnapshot *out)
{
    if (!s || !out) return SYNC_PROFILES_INVALID;
    if (pthread_mutex_trylock(&s->mutex)) return SYNC_PROFILES_BUSY;
    *out = s->snapshot;
    pthread_mutex_unlock(&s->mutex);
    return SYNC_PROFILES_OK;
}
SyncProfilesResult SyncProfilesGetProfile(SyncProfiles *s, const char *name, SyncProfile *out)
{
    if (!s || !name || !out || !valid_name(name)) return SYNC_PROFILES_INVALID;
    if (pthread_mutex_trylock(&s->mutex)) return SYNC_PROFILES_BUSY;
    SyncProfilesResult result = SYNC_PROFILES_NOT_FOUND;
    if (atomic_load(&s->stop)) result = SYNC_PROFILES_STOPPED;
    else if (!s->db) result = SYNC_PROFILES_NOT_READY;
    else for (uint32_t i = 0; i < s->db->count; ++i) {
        if (!strcmp(name, s->db->items[i]->name)) { *out = *s->db->items[i]; result = SYNC_PROFILES_OK; break; }
    }
    pthread_mutex_unlock(&s->mutex);
    return result;
}
static SyncProfilesResult submit(SyncProfiles *s, SyncProfile *save, const char *deletion)
{
    if (!s) { free(save); return SYNC_PROFILES_INVALID; }
    if (pthread_mutex_trylock(&s->mutex)) { free(save); return SYNC_PROFILES_BUSY; }
    SyncProfilesResult result = SYNC_PROFILES_OK;
    if (atomic_load(&s->stop)) result = SYNC_PROFILES_STOPPED;
    else if (s->busy) result = SYNC_PROFILES_BUSY;
    else {
        s->pending = save;
        s->reload = save == NULL && deletion == NULL;
        s->deleting = deletion != NULL;
        if (deletion) strcpy(s->deletion, deletion);
        s->busy = 1;
        ++s->snapshot.submitted;
        ++s->snapshot.revision;
        s->snapshot.state = save || deletion ? SYNC_PROFILES_SAVING : SYNC_PROFILES_LOADING;
        s->snapshot.error[0] = 0;
        s->snapshot.result = SYNC_PROFILES_OK;
        pthread_cond_signal(&s->wake);
    }
    pthread_mutex_unlock(&s->mutex);
    if (result != SYNC_PROFILES_OK) free(save);
    return result;
}
SyncProfilesResult SyncProfilesSave(SyncProfiles *s, const SyncProfile *profile)
{
    if (!s) return SYNC_PROFILES_INVALID;
    SyncProfilesResult result = profile_validate(profile);
    if (result != SYNC_PROFILES_OK) return result;
    SyncProfile *copy = malloc(sizeof(*copy));
    if (!copy) return SYNC_PROFILES_NOMEM;
    *copy = *profile;
    return submit(s, copy, NULL);
}
SyncProfilesResult SyncProfilesDelete(SyncProfiles *s, const char *name)
{
    size_t length;
    if (!name) return SYNC_PROFILES_INVALID;
    if (!bounded_length(name, SYNC_PROFILE_NAME_MAX, &length)) return SYNC_PROFILES_LIMIT;
    if (!valid_name(name)) return SYNC_PROFILES_INVALID;
    return submit(s, NULL, name);
}
SyncProfilesResult SyncProfilesReload(SyncProfiles *s)
{
    return submit(s, NULL, NULL);
}
void SyncProfilesStop(SyncProfiles *s)
{
    if (!s) return;
    atomic_store(&s->stop, 1);
    pthread_cond_signal(&s->wake);
}
int SyncProfilesDestroy(SyncProfiles *s)
{
    if (!s) return 1;
    if (!atomic_load_explicit(&s->finished, memory_order_acquire)) return 0;
    pthread_join(s->thread, NULL);
    pthread_cond_destroy(&s->wake);
    pthread_mutex_destroy(&s->mutex);
    db_free(s->db);
    free(s->pending);
    free(s->home);
    free(s);
    return 1;
}

#ifdef SYNC_PROFILES_TEST
/* Standalone real-filesystem regression, built without Xm or a backend link:
 * cc -std=c11 -Wall -Wextra -Werror -pthread -DSYNC_PROFILES_TEST \
 *    -I<installed-fs-headers> SyncProfiles.c -o <approved-build-dir>/profiles-test
 * profiles-test <unique-owned-0700-fixture-root> [<noowners-fixture-root>]
 * Every fixture must be under an explicitly selected task workspace. Positive
 * permission/privacy coverage must run on an ownership-enforcing filesystem.
 * All atomic staging remains inside each fixture's private config directory.
 */
#include <assert.h>
#include <dirent.h>
#include <sys/wait.h>

static void test_pause(void)
{
    struct timespec delay = { 0, 1000000 };
    nanosleep(&delay, NULL);
}
/* Deterministic initialization fault: hold the creator immediately before its
 * parent flush, then make an EEXIST adopter's own parent flush fail. Real mkdir,
 * locks, parsing and commits remain in use; only the selected fsync is faulted.
 */
static struct {
    dev_t device;
    ino_t inode;
    atomic_int enabled, creator_waiting, release_creator, fail_adopter, adoption_flushes;
} test_flush;
static int test_home_flush(int fd, int created)
{
    struct stat st;
    if (atomic_load(&test_flush.enabled) && fstat(fd, &st) == 0 &&
        st.st_dev == test_flush.device && st.st_ino == test_flush.inode) {
        if (created) {
            atomic_store(&test_flush.creator_waiting, 1);
            while (!atomic_load(&test_flush.release_creator)) test_pause();
        } else {
            atomic_fetch_add(&test_flush.adoption_flushes, 1);
            if (atomic_load(&test_flush.fail_adopter)) { errno = EIO; return -1; }
        }
    }
    return fsync(fd);
}
static SyncProfilesSnapshot test_settled(SyncProfiles *s)
{
    SyncProfilesSnapshot snapshot;
    for (unsigned i = 0; i < 20000; ++i) {
        if (SyncProfilesGetSnapshot(s, &snapshot) == SYNC_PROFILES_OK &&
            snapshot.completed == snapshot.submitted) return snapshot;
        test_pause();
    }
    fprintf(stderr, "Profile worker did not settle\n"); abort();
}
static SyncProfiles *test_start(const char *home)
{
    char error[512];
    SyncProfiles *s = SyncProfilesStart(home, error, sizeof(error));
    if (!s) { fprintf(stderr, "Start: %s\n", error); abort(); }
    return s;
}
static void test_stop(SyncProfiles *s)
{
    SyncProfilesStop(s);
    for (unsigned i = 0; i < 20000; ++i) {
        if (SyncProfilesDestroy(s)) return;
        test_pause();
    }
    fprintf(stderr, "Profile worker did not stop\n"); abort();
}
static void test_queue(SyncProfiles *s, const SyncProfile *p)
{
    for (unsigned i = 0; i < 20000; ++i) {
        SyncProfilesResult r = SyncProfilesSave(s, p);
        if (r == SYNC_PROFILES_OK) return;
        assert(r == SYNC_PROFILES_BUSY);
        test_pause();
    }
    abort();
}
static void test_reload(SyncProfiles *s)
{
    for (unsigned i = 0; i < 20000; ++i) {
        SyncProfilesResult r = SyncProfilesReload(s);
        if (r == SYNC_PROFILES_OK) return;
        assert(r == SYNC_PROFILES_BUSY);
        test_pause();
    }
    abort();
}
static void test_delete(SyncProfiles *s, const char *name)
{
    for (unsigned i = 0; i < 20000; ++i) {
        SyncProfilesResult r = SyncProfilesDelete(s, name);
        if (r == SYNC_PROFILES_OK) return;
        assert(r == SYNC_PROFILES_BUSY);
        test_pause();
    }
    abort();
}
static void test_get(SyncProfiles *s, const char *name, SyncProfile *out)
{
    for (unsigned i = 0; i < 20000; ++i) {
        SyncProfilesResult r = SyncProfilesGetProfile(s, name, out);
        if (r == SYNC_PROFILES_OK) return;
        assert(r == SYNC_PROFILES_BUSY);
        test_pause();
    }
    abort();
}
static SyncProfile *test_profile(const char *name)
{
    SyncProfile *p = calloc(1, sizeof(*p)); assert(p);
    p->version = SYNC_PROFILES_VERSION;
    assert(strlen(name) <= SYNC_PROFILE_NAME_MAX);
    strcpy(p->name, name);
    /* Non-UTF8, whitespace and metacharacters are exact path bytes, not code. */
    strcpy(p->source, "/source/\xff\xfe; $`\n");
    strcpy(p->destination, "/destination/\x80\x81\t");
    p->profile = CKFSP_UPDATE;
    p->metadata_scope = 1;
    p->filters.allowed_kind_mask = CKFSP_ALL_KINDS;
    p->filters.maximum_depth = 1024;
    p->filters.has_minimum_size = p->filters.has_maximum_size = 1;
    p->filters.minimum_size = 0;
    p->filters.maximum_size = UINT64_MAX;
    p->filters.has_modified_not_before = p->filters.has_modified_before = 1;
    p->filters.modified_not_before.seconds = INT64_MIN;
    p->filters.modified_not_before.nanoseconds = 123456789;
    p->filters.modified_before.seconds = INT64_MAX;
    p->filters.modified_before.nanoseconds = 999999999;
    p->rule_count = SYNC_PROFILE_RULES_MAX;
    for (uint32_t i = 0; i < p->rule_count; ++i) {
        p->rules[i].kind = i % (CKFET_CONFLICT_CAPACITY + 1);
        p->rules[i].choice = i % (CKFET_DELETE_EDITED + 1);
        p->rules[i].tie = (i + 1) % (CKFET_DELETE_EDITED + 1);
        p->rules[i].protect_nonempty = i % 2;
        p->rules[i].retain_replaced = (i + 1) % 2;
    }
    return p;
}
static void test_assert_profile(const SyncProfile *a, const SyncProfile *b)
{
    assert(a->version == b->version && !strcmp(a->name, b->name));
    assert(!strcmp(a->source, b->source) && !strcmp(a->destination, b->destination));
    assert(a->profile == b->profile && a->metadata_scope == b->metadata_scope);
    assert(!memcmp(&a->filters, &b->filters, sizeof(a->filters)));
    assert(a->rule_count == b->rule_count);
    assert(!memcmp(a->rules, b->rules, a->rule_count * sizeof(a->rules[0])));
}
static char *test_path(const char *root, const char *suffix)
{
    size_t n = strlen(root) + strlen(suffix) + 2;
    char *p = malloc(n); assert(p);
    snprintf(p, n, "%s/%s", root, suffix);
    return p;
}
static char *test_home(const char *root, const char *name)
{
    char *p = test_path(root, name);
    assert(mkdir(p, 0700) == 0 && chmod(p, 0700) == 0);
    return p;
}
static void test_write_file(const char *path, const void *bytes, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600); assert(fd >= 0);
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = write(fd, (const char *)bytes + sent, length - sent);
        assert(n > 0); sent += (size_t)n;
    }
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
}
static unsigned char *test_read_file(const char *path, size_t *size)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); assert(fd >= 0);
    assert(fstat(fd, &st) == 0 && st.st_size >= 0 && st.st_size <= STORE_BYTES_MAX);
    *size = (size_t)st.st_size;
    unsigned char *p = malloc(*size ? *size : 1); assert(p);
    size_t got = 0;
    while (got < *size) {
        ssize_t n = read(fd, p + got, *size - got);
        assert(n > 0); got += (size_t)n;
    }
    assert(close(fd) == 0);
    return p;
}
static void test_refused_save(SyncProfiles *s, const SyncProfile *p, const char *file,
                              SyncProfilesResult expected)
{
    size_t before_size, after_size;
    unsigned char *before = test_read_file(file, &before_size);
    test_queue(s, p);
    SyncProfilesSnapshot snapshot = test_settled(s);
    assert(snapshot.result == expected && snapshot.error[0]);
    unsigned char *after = test_read_file(file, &after_size);
    assert(before_size == after_size && !memcmp(before, after, before_size));
    free(before); free(after);
}
static void test_no_staging(const char *home)
{
    char *path = test_path(home, STORE_DIR);
    DIR *dir = opendir(path); assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        assert(strncmp(entry->d_name, ".profiles-new-", 14));
    assert(closedir(dir) == 0); free(path);
}
static void test_remove_tree(const char *path)
{
    /* Fixture-only teardown, never traversing symbolic links. */
    struct stat st; assert(lstat(path, &st) == 0);
    if (!S_ISDIR(st.st_mode)) { assert(unlink(path) == 0); return; }
    DIR *dir = opendir(path); assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char *child = test_path(path, entry->d_name);
        test_remove_tree(child); free(child);
    }
    assert(closedir(dir) == 0); assert(rmdir(path) == 0);
}
static void test_adoption_durability(const char *root)
{
    char *home = test_home(root, "adoption-durability");
    struct stat st; assert(stat(home, &st) == 0);
    test_flush.device = st.st_dev; test_flush.inode = st.st_ino;
    atomic_store(&test_flush.creator_waiting, 0);
    atomic_store(&test_flush.release_creator, 0);
    atomic_store(&test_flush.adoption_flushes, 0);
    atomic_store(&test_flush.fail_adopter, 1);
    atomic_store(&test_flush.enabled, 1);
    SyncProfiles *creator = test_start(home);
    unsigned waited;
    for (waited = 0; waited < 20000 && !atomic_load(&test_flush.creator_waiting); ++waited) test_pause();
    assert(waited < 20000);
    SyncProfiles *adopter = test_start(home);
    SyncProfilesSnapshot snapshot = test_settled(adopter);
    assert(snapshot.result == SYNC_PROFILES_IO && strstr(snapshot.error, "Flush profile directory entry"));
    SyncProfile *p = test_profile("adopted");
    test_queue(adopter, p);
    assert(test_settled(adopter).result == SYNC_PROFILES_IO);
    char *file = test_path(home, STORE_DIR "/" STORE_FILE);
    struct stat missing;
    assert(lstat(file, &missing) < 0 && errno == ENOENT);
    assert(atomic_load(&test_flush.adoption_flushes) >= 2);
    atomic_store(&test_flush.fail_adopter, 0);
    test_queue(adopter, p);
    snapshot = test_settled(adopter);
    assert(snapshot.result == SYNC_PROFILES_OK && snapshot.count == 1);
    /* The adopter has made the entry durable independently of the paused creator. */
    atomic_store(&test_flush.release_creator, 1);
    snapshot = test_settled(creator);
    assert(snapshot.result == SYNC_PROFILES_OK && snapshot.count == 1);
    test_stop(creator); test_stop(adopter);
    atomic_store(&test_flush.enabled, 0);
    test_remove_tree(home); free(home); free(file); free(p);
    puts("Adoption durability: paused creator / adopter flush failure / independent successful flush passed");
}
#ifdef __APPLE__
#include <pwd.h>
static struct {
    dev_t device;
    ino_t inode;
    atomic_int enabled;
} test_acl_failure;
static acl_t test_storage_acl_get(int fd)
{
    struct stat st;
    if (atomic_load(&test_acl_failure.enabled) && fstat(fd, &st) == 0 &&
        st.st_dev == test_acl_failure.device && st.st_ino == test_acl_failure.inode) {
        errno = EIO; return NULL;
    }
    return acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
}
static acl_t test_grant(int fd, uid_t uid, acl_perm_t permission, int inherit, int deny)
{
    acl_t original = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (!original && errno == ENOENT) original = acl_init(0);
    assert(original);
    acl_t changed = acl_dup(original); assert(changed);
    acl_entry_t entry;
    assert(acl_create_entry(&changed, &entry) == 0);
    assert(acl_set_tag_type(entry, deny ? ACL_EXTENDED_DENY : ACL_EXTENDED_ALLOW) == 0);
    uuid_t identity; assert(mbr_uid_to_uuid(uid, identity) == 0);
    assert(acl_set_qualifier(entry, identity) == 0);
    acl_permset_t permissions; assert(acl_get_permset(entry, &permissions) == 0);
    assert(acl_clear_perms(permissions) == 0 && acl_add_perm(permissions, permission) == 0);
    if (inherit) {
        acl_flagset_t flags; assert(acl_get_flagset_np(entry, &flags) == 0);
        assert(acl_add_flag_np(flags, ACL_ENTRY_FILE_INHERIT) == 0);
        assert(acl_add_flag_np(flags, ACL_ENTRY_DIRECTORY_INHERIT) == 0);
        assert(acl_add_flag_np(flags, ACL_ENTRY_ONLY_INHERIT) == 0);
    }
    assert(acl_set_fd_np(fd, changed, ACL_TYPE_EXTENDED) == 0);
    acl_free(changed);
    return original;
}
static void test_restore_acl(int fd, acl_t original)
{
    assert(acl_set_fd_np(fd, original, ACL_TYPE_EXTENDED) == 0);
    acl_free(original); assert(close(fd) == 0);
}
static void test_acl_privacy(const char *root)
{
    struct passwd *nobody = getpwnam("nobody"); assert(nobody);
    uid_t other = nobody->pw_uid; assert(other != 0 && other != geteuid());
    char *home = test_home(root, "acl-privacy");
    SyncProfiles *s = test_start(home); assert(test_settled(s).result == SYNC_PROFILES_OK);
    SyncProfile *p = test_profile("private"); test_queue(s, p);
    assert(test_settled(s).result == SYNC_PROFILES_OK);
    char *file = test_path(home, STORE_DIR "/" STORE_FILE);
    const char *suffixes[] = { "", STORE_DIR, STORE_DIR "/" STORE_LOCK, STORE_DIR "/" STORE_FILE };
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        char *path = test_path(home, suffixes[i]);
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
        if (i < 2) flags |= O_DIRECTORY;
        int fd = open(path, flags); assert(fd >= 0);
        /* chmod alone cannot remove an ACL grant to another identity. */
        acl_t original = test_grant(fd, other, ACL_READ_DATA, 0, 0);
        assert(fchmod(fd, i < 2 ? 0700 : 0600) == 0);
        test_refused_save(s, p, file, SYNC_PROFILES_UNSAFE);
        test_no_staging(home);
        test_restore_acl(fd, original); free(path);
    }
    /* Unknown ACL retrieval cannot be mistaken for an absent ACL. */
    struct stat input; assert(stat(file, &input) == 0);
    test_acl_failure.device = input.st_dev; test_acl_failure.inode = input.st_ino;
    atomic_store(&test_acl_failure.enabled, 1);
    test_refused_save(s, p, file, SYNC_PROFILES_UNSAFE);
    atomic_store(&test_acl_failure.enabled, 0);
    test_no_staging(home);
    /* Explicit grants only to the caller and restrictive denies remain usable. */
    for (int deny = 0; deny < 2; ++deny) {
        int fd = open(file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); assert(fd >= 0);
        acl_t original = test_grant(fd, deny ? other : geteuid(), ACL_READ_DATA, 0, deny);
        test_reload(s); assert(test_settled(s).result == SYNC_PROFILES_OK);
        test_restore_acl(fd, original);
    }
    test_stop(s); free(file); free(p); test_remove_tree(home); free(home);
    /* Inherit-only read grants on HOME are refused before creating private children. */
    home = test_home(root, "acl-inheritance");
    int fd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); assert(fd >= 0);
    acl_t original = test_grant(fd, other, ACL_READ_DATA, 1, 0);
    s = test_start(home); assert(test_settled(s).result == SYNC_PROFILES_UNSAFE); test_stop(s);
    char *config = test_path(home, STORE_DIR);
    struct stat st; assert(lstat(config, &st) < 0 && errno == ENOENT);
    test_restore_acl(fd, original); free(config); test_remove_tree(home); free(home);
    /* An unsafe intermediate component cannot hide behind a private final HOME. */
    char *parent = test_home(root, "acl-ancestor"); home = test_home(parent, "home");
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); assert(fd >= 0);
    original = test_grant(fd, other, ACL_DELETE_CHILD, 0, 0);
    s = test_start(home); assert(test_settled(s).result == SYNC_PROFILES_UNSAFE); test_stop(s);
    config = test_path(home, STORE_DIR); assert(lstat(config, &st) < 0 && errno == ENOENT);
    test_restore_acl(fd, original); free(config);
    test_remove_tree(parent); free(parent); free(home);
    puts("Mac ACL privacy: exposed home/directory/lock/data, inheritance, ancestor mutation, unknown ACL refusal and safe grants/denies passed");
}
#endif
static void test_access_path(const char *root)
{
    char *parent = test_home(root, "access-path"), *home = test_home(parent, "home");
    char *link = test_path(root, "access-alias"); assert(symlink(parent, link) == 0);
    char *alias = test_path(link, "home");
    SyncProfiles *s = test_start(alias); assert(test_settled(s).result != SYNC_PROFILES_OK); test_stop(s);
    assert(unlink(link) == 0);
    assert(chmod(parent, 0777) == 0);
    s = test_start(home); assert(test_settled(s).result == SYNC_PROFILES_UNSAFE); test_stop(s);
    assert(chmod(parent, 0700) == 0);
    char *config = test_path(home, STORE_DIR); struct stat st;
    assert(lstat(config, &st) < 0 && errno == ENOENT);
    free(config); free(alias); free(link); free(home); test_remove_tree(parent); free(parent);
}
static void test_storage_metadata_policy(void)
{
    struct stat st = { 0 };
    uid_t caller = geteuid(), other = caller == 1 ? 2 : 1;
    st.st_mode = S_IFDIR | 0755;
    st.st_uid = other;
    assert(SyncStorageMetadataSafe(&st, SYNC_STORAGE_ROOT, caller));
    assert(!SyncStorageMetadataSafe(&st, SYNC_STORAGE_ANCESTOR, caller));
    st.st_mode = S_IFDIR | 0775;
    assert(!SyncStorageMetadataSafe(&st, SYNC_STORAGE_ROOT, caller));
    st.st_mode = S_IFDIR | 0700;
    assert(!SyncStorageMetadataSafe(&st, SYNC_STORAGE_HOME, caller));
    st.st_uid = caller;
    assert(SyncStorageMetadataSafe(&st, SYNC_STORAGE_HOME, caller));
    puts("Private storage metadata: unmapped filesystem root and strict ordinary ancestors passed");
}
static int test_writer(const char *home, const char *name)
{
    SyncProfiles *s = test_start(home);
    assert(test_settled(s).result == SYNC_PROFILES_OK);
    SyncProfile *p = test_profile(name);
    test_queue(s, p); assert(test_settled(s).result == SYNC_PROFILES_OK);
    test_stop(s); free(p);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--writer")) return test_writer(argv[2], argv[3]);
    test_storage_metadata_policy();
    assert(argc == 2 || argc == 3);
    assert(argv[1][0] == '/');
    ProfileError safety = { 0 };
    int fixture = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(fixture >= 0);
    int fixture_safe = private_stat(fixture, 1, &safety);
    if (!fixture_safe) fprintf(stderr, "Unsafe test fixture: %s\n", safety.text);
    assert(fixture_safe);
    assert(close(fixture) == 0);
    char *home = test_home(argv[1], "roundtrip");
    SyncProfiles *a = test_start(home), *b = test_start(home);
    SyncProfilesSnapshot initial_a = test_settled(a), initial_b = test_settled(b);
    if (initial_a.result != SYNC_PROFILES_OK || initial_b.result != SYNC_PROFILES_OK)
        fprintf(stderr, "Initial profile loads failed: a=%d %s; b=%d %s\n",
                initial_a.result, initial_a.error, initial_b.result, initial_b.error);
    assert(initial_a.result == SYNC_PROFILES_OK && initial_b.result == SYNC_PROFILES_OK);
    SyncProfile *p = test_profile("profile \xc3\xa9"), *out = calloc(1, sizeof(*out)); assert(out);
    test_queue(a, p);
    /* Caller ownership: mutating the caller after submission cannot affect save. */
    p->source[1] = 'X';
    assert(test_settled(a).result == SYNC_PROFILES_OK);
    test_get(a, p->name, out); assert(out->source[1] == 's'); p->source[1] = 's';
    test_assert_profile(p, out);
    struct stat st;
    char *file = test_path(home, STORE_DIR "/" STORE_FILE);
    assert(lstat(file, &st) == 0 && (st.st_mode & 07777) == 0600 && st.st_nlink == 1 && st.st_uid == geteuid());
    SyncProfile *q = test_profile("from-stale-reader");
    test_queue(b, q); assert(test_settled(b).result == SYNC_PROFILES_OK);
    assert(test_settled(b).count == 2); /* Merged disk DB, not b's stale empty cache. */
    p->profile = CKFSP_COMPARE;
    test_queue(a, p); assert(test_settled(a).result == SYNC_PROFILES_OK);
    test_reload(b); assert(test_settled(b).count == 2);
    test_get(b, p->name, out); test_assert_profile(p, out);
    test_stop(a); test_stop(b);
    a = test_start(home); assert(test_settled(a).count == 2);
    test_get(a, p->name, out); test_assert_profile(p, out);
    /* Boundary path length and full 64 rules round trip without truncation. */
    memset(p->source, 'x', SYNC_PROFILE_PATH_MAX); p->source[0] = '/'; p->source[SYNC_PROFILE_PATH_MAX] = 0;
    test_queue(a, p); assert(test_settled(a).result == SYNC_PROFILES_OK);
    test_get(a, p->name, out); test_assert_profile(p, out);
    p->source[SYNC_PROFILE_PATH_MAX] = 'x'; assert(SyncProfilesSave(a, p) == SYNC_PROFILES_LIMIT);
    p->source[SYNC_PROFILE_PATH_MAX] = 0;
    p->rule_count = 65; assert(SyncProfilesSave(a, p) == SYNC_PROFILES_LIMIT); p->rule_count = 64;
    p->version = 2; assert(SyncProfilesSave(a, p) == SYNC_PROFILES_INVALID); p->version = 1;
    p->rules[0].kind = UINT32_MAX; assert(SyncProfilesSave(a, p) == SYNC_PROFILES_INVALID); p->rules[0].kind = 0;
    p->filters.has_minimum_size = 2; assert(SyncProfilesSave(a, p) == SYNC_PROFILES_INVALID); p->filters.has_minimum_size = 1;
    p->filters.modified_before.nanoseconds = 1000000000; assert(SyncProfilesSave(a, p) == SYNC_PROFILES_INVALID);
    p->filters.modified_before.nanoseconds = 999999999;
    p->filters.maximum_size = 1; p->filters.minimum_size = 2;
    assert(SyncProfilesSave(a, p) == SYNC_PROFILES_INVALID);
    p->filters.maximum_size = UINT64_MAX; p->filters.minimum_size = 0;
    p->filters.modified_before = p->filters.modified_not_before;
    assert(SyncProfilesSave(a, p) == SYNC_PROFILES_INVALID);
    p->filters.modified_before.seconds = INT64_MAX; p->filters.modified_before.nanoseconds = 999999999;
    /* Corrupt stores and malformed bounds are retained byte-for-byte on save. */
    size_t valid_size;
    unsigned char *valid = test_read_file(file, &valid_size);
    test_write_file(file, "corrupt", 7);
    test_reload(a); assert(test_settled(a).result == SYNC_PROFILES_CORRUPT);
    test_refused_save(a, p, file, SYNC_PROFILES_CORRUPT);
    test_delete(a, p->name); assert(test_settled(a).result == SYNC_PROFILES_CORRUPT);
    size_t corrupt_size; unsigned char *corrupt_bytes = test_read_file(file, &corrupt_size);
    assert(corrupt_size == 7 && !memcmp(corrupt_bytes, "corrupt", 7)); free(corrupt_bytes);
    test_write_file(file, valid, valid_size);
    unsigned char *bad = malloc(valid_size); assert(bad); memcpy(bad, valid, valid_size);
    /* Record layout: 16-byte store header then name/source/destination/rules. */
    memset(bad + 20, 0xff, 4); test_write_file(file, bad, valid_size);
    test_refused_save(a, p, file, SYNC_PROFILES_CORRUPT);
    memcpy(bad, valid, valid_size); bad[11] = 2; test_write_file(file, bad, valid_size);
    test_refused_save(a, p, file, SYNC_PROFILES_CORRUPT);
    memcpy(bad, valid, valid_size); memset(bad + 28, 0xff, 4); test_write_file(file, bad, valid_size);
    test_refused_save(a, p, file, SYNC_PROFILES_CORRUPT);
    /* Reject unknown enum/boolean values, invalid nanoseconds/depth and NUL
     * payloads read from an actual file, not only through API validation. */
    const size_t malformed_offsets[] = { 32, 40, 80, 104, 112 };
    for (size_t i = 0; i < sizeof(malformed_offsets) / sizeof(malformed_offsets[0]); ++i) {
        memcpy(bad, valid, valid_size);
        size_t offset = malformed_offsets[i];
        if (offset == 112) bad[offset] = 0;
        else if (offset == 104) { memset(bad + offset, 0xff, 8); }
        else { memset(bad + offset, 0xff, 4); }
        test_write_file(file, bad, valid_size);
        test_refused_save(a, p, file, SYNC_PROFILES_CORRUPT);
    }
    test_write_file(file, valid, valid_size);
    assert(chmod(file, 0400) == 0); test_refused_save(a, p, file, SYNC_PROFILES_UNSAFE);
    assert(chmod(file, 0600) == 0);
    char *alias = test_path(home, "hardlink"); assert(link(file, alias) == 0);
    test_refused_save(a, p, file, SYNC_PROFILES_UNSAFE); assert(unlink(alias) == 0);
    char *retained = test_path(home, "retained"); assert(rename(file, retained) == 0);
    assert(symlink(retained, file) == 0);
    test_queue(a, p); assert(test_settled(a).result != SYNC_PROFILES_OK);
    assert(lstat(file, &st) == 0 && S_ISLNK(st.st_mode));
    size_t retained_size; unsigned char *retained_bytes = test_read_file(retained, &retained_size);
    assert(retained_size == valid_size && !memcmp(valid, retained_bytes, valid_size)); free(retained_bytes);
    assert(unlink(file) == 0 && rename(retained, file) == 0);
    test_no_staging(home);
    test_stop(a);
    /* Both separate workers and separate helper processes must merge safely. */
    char *lockpath = test_path(home, STORE_DIR "/" STORE_LOCK);
    int lock = open(lockpath, O_RDWR | O_NOFOLLOW | O_CLOEXEC); assert(lock >= 0);
    assert(flock(lock, LOCK_EX | LOCK_NB) == 0);
    pid_t first = fork(); assert(first >= 0);
    if (!first) { execl(argv[0], argv[0], "--writer", home, "process-a", (char *)NULL); _exit(127); }
    pid_t second = fork(); assert(second >= 0);
    if (!second) { execl(argv[0], argv[0], "--writer", home, "process-b", (char *)NULL); _exit(127); }
    assert(close(lock) == 0);
    int status;
    assert(waitpid(first, &status, 0) == first && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(waitpid(second, &status, 0) == second && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    a = test_start(home); assert(test_settled(a).count == 4);
    test_delete(a, "missing-profile");
    assert(test_settled(a).result == SYNC_PROFILES_NOT_FOUND && test_settled(a).count == 4);
    test_delete(a, "process-a"); assert(test_settled(a).result == SYNC_PROFILES_OK);
    assert(test_settled(a).count == 3); test_stop(a);
    a = test_start(home); assert(test_settled(a).count == 3); test_stop(a);
    /* Stress concurrent initial creation: every iteration starts with no config
     * directory or lock, so an open/create race cannot hide behind an old lock. */
    for (unsigned i = 0; i < 24; ++i) {
        char name[32]; snprintf(name, sizeof(name), "creation-%02u", i);
        char *concurrent = test_home(argv[1], name);
        a = test_start(concurrent); b = test_start(concurrent);
        assert(test_settled(a).result == SYNC_PROFILES_OK);
        assert(test_settled(b).result == SYNC_PROFILES_OK);
        test_stop(a); test_stop(b); test_remove_tree(concurrent); free(concurrent);
    }
    /* Cancellation while a different helper holds the lock has bounded exit. */
    lock = open(lockpath, O_RDWR | O_NOFOLLOW | O_CLOEXEC); assert(lock >= 0);
    assert(flock(lock, LOCK_EX | LOCK_NB) == 0);
    a = test_start(home); SyncProfilesStop(a); test_stop(a); assert(close(lock) == 0);
    /* Never silently evict an existing named profile at the 64-profile limit. */
    char *capacity = test_home(argv[1], "capacity"); a = test_start(capacity);
    assert(test_settled(a).result == SYNC_PROFILES_OK);
    for (unsigned i = 0; i < SYNC_PROFILES_MAX; ++i) {
        snprintf(q->name, sizeof(q->name), "profile-%02u", i);
        test_queue(a, q); assert(test_settled(a).result == SYNC_PROFILES_OK);
    }
    strcpy(q->name, "one-too-many"); test_queue(a, q);
    assert(test_settled(a).result == SYNC_PROFILES_LIMIT && test_settled(a).count == 64);
    strcpy(q->name, "profile-00"); test_queue(a, q); assert(test_settled(a).result == SYNC_PROFILES_OK);
    for (unsigned i = 0; i < SYNC_PROFILES_MAX; ++i) {
        snprintf(q->name, sizeof(q->name), "profile-%02u", i);
        test_delete(a, q->name); assert(test_settled(a).result == SYNC_PROFILES_OK);
    }
    assert(test_settled(a).count == 0); test_stop(a);
    a = test_start(capacity); assert(test_settled(a).result == SYNC_PROFILES_OK && test_settled(a).count == 0);
    test_no_staging(capacity); test_stop(a);
    /* Refuse a symlinked private config directory and a nonprivate directory. */
    char *unsafe = test_home(argv[1], "unsafe"), *config = test_path(unsafe, STORE_DIR);
    assert(symlink(capacity, config) == 0);
    a = test_start(unsafe); assert(test_settled(a).result != SYNC_PROFILES_OK); test_stop(a);
    assert(unlink(config) == 0 && mkdir(config, 0755) == 0 && chmod(config, 0755) == 0);
    a = test_start(unsafe); assert(test_settled(a).result == SYNC_PROFILES_UNSAFE); test_stop(a);
    assert(chmod(config, 0700) == 0);
    char *unsafe_lock = test_path(config, STORE_LOCK);
    assert(symlink(file, unsafe_lock) == 0);
    a = test_start(unsafe); assert(test_settled(a).result != SYNC_PROFILES_OK); test_stop(a);
#ifdef __APPLE__
    if (argc == 3) {
        char *ignored = test_home(argv[2], "noowners-refusal");
        a = test_start(ignored);
        SyncProfilesSnapshot refusal = test_settled(a);
        assert(refusal.result == SYNC_PROFILES_UNSAFE && strstr(refusal.error, "noowners"));
        test_stop(a); test_remove_tree(ignored); free(ignored);
    }
#else
    (void)argc;
#endif
    test_adoption_durability(argv[1]);
    test_access_path(argv[1]);
#ifdef __APPLE__
    test_acl_privacy(argv[1]);
#endif
    test_remove_tree(home); test_remove_tree(capacity); test_remove_tree(unsafe);
    free(home); free(capacity); free(unsafe); free(config); free(unsafe_lock);
    free(p); free(q); free(out); free(file); free(alias); free(retained);
    free(lockpath); free(valid); free(bad);
    puts("SyncProfiles: roundtrip/update/delete/merge/concurrent writers/bounds/corrupt preservation/privacy/cancellation/access path/adoption durability passed");
    return 0;
}
#endif
