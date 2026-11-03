/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef DTFILE_SYNC_PRIVATE_STORAGE_H
#define DTFILE_SYNC_PRIVATE_STORAGE_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/acl.h>
#include <membership.h>
#ifndef SYNC_STORAGE_ACL_GET
#define SYNC_STORAGE_ACL_GET(fd) acl_get_fd_np((fd), ACL_TYPE_EXTENDED)
#endif
#endif

/* Shared held-descriptor policy for private preferences and operation journals.
 * Define _POSIX_C_SOURCE=200809L and _XOPEN_SOURCE=700 before system headers;
 * on Darwin also define _DARWIN_C_SOURCE.
 * Call on the I/O worker. Caller owns descriptors and the error buffer. Unknown
 * ownership/ACL state is refused. No pathname checks authorize a later reopen.
 * As with the filesystem operations, root and the current UID are trusted;
 * root-owned group-zero ancestors are system administration namespaces.
 * Group ACL grants are conservatively refused, without membership inference.
 */
typedef enum { SYNC_STORAGE_OK, SYNC_STORAGE_IO, SYNC_STORAGE_UNSAFE, SYNC_STORAGE_NOMEM } SyncStorageStatus;
typedef enum { SYNC_STORAGE_ANCESTOR, SYNC_STORAGE_HOME, SYNC_STORAGE_DIRECTORY, SYNC_STORAGE_FILE } SyncStorageKind;

/* ACLs are independent of mode bits on Darwin. Refuse uninspectable ACLs,
 * unknown grants and group grants rather than infer group membership. Denies
 * only reduce access. Only root and the caller are trusted grant recipients.
 * Ancestors may be publicly readable; their namespace must not be mutable by
 * an ACL recipient outside that trust boundary. Home/storage also reject read
 * and inheritable grants, before creating any private child or payload.
 */
static inline SyncStorageStatus SyncStorageACL(int fd, int ancestor, char *error, size_t capacity)
{
#ifdef __APPLE__
    acl_t acl = SYNC_STORAGE_ACL_GET(fd);
    if (!acl && errno == ENOENT) return SYNC_STORAGE_OK; /* Held fd has no extended ACL. */
    if (!acl) {
        snprintf(error, capacity, "Cannot inspect extended ACL on private storage access path: %s", strerror(errno));
        return SYNC_STORAGE_UNSAFE;
    }
    const acl_permset_mask_t known = ACL_READ_DATA | ACL_WRITE_DATA | ACL_EXECUTE |
        ACL_DELETE | ACL_APPEND_DATA | ACL_DELETE_CHILD | ACL_READ_ATTRIBUTES |
        ACL_WRITE_ATTRIBUTES | ACL_READ_EXTATTRIBUTES | ACL_WRITE_EXTATTRIBUTES |
        ACL_READ_SECURITY | ACL_WRITE_SECURITY | ACL_CHANGE_OWNER | ACL_SYNCHRONIZE;
    const acl_permset_mask_t mutation = ACL_WRITE_DATA | ACL_DELETE | ACL_APPEND_DATA |
        ACL_DELETE_CHILD | ACL_WRITE_ATTRIBUTES | ACL_WRITE_EXTATTRIBUTES |
        ACL_WRITE_SECURITY | ACL_CHANGE_OWNER;
    if (acl_valid(acl) < 0) {
        acl_free(acl);
        snprintf(error, capacity, "Cannot validate extended ACL on private storage access path");
        return SYNC_STORAGE_UNSAFE;
    }
    acl_entry_t entry;
    int which = ACL_FIRST_ENTRY, result, safe = 1;
    while ((result = acl_get_entry(acl, which, &entry)) == 0) {
        which = ACL_NEXT_ENTRY;
        acl_tag_t tag;
        acl_permset_mask_t permissions;
        if (acl_get_tag_type(entry, &tag) < 0 ||
            (tag != ACL_EXTENDED_ALLOW && tag != ACL_EXTENDED_DENY)) { safe = 0; break; }
        if (tag == ACL_EXTENDED_DENY) continue;
        if (acl_get_permset_mask_np(entry, &permissions) < 0 || (permissions & ~known)) { safe = 0; break; }
        if (!(permissions & (ancestor ? mutation : known))) continue;
        void *qualifier = acl_get_qualifier(entry);
        uuid_t caller, root;
        if (!qualifier) { safe = 0; break; }
        int identified = mbr_uid_to_uuid(geteuid(), caller) == 0 && mbr_uid_to_uuid(0, root) == 0;
        safe = identified && (!memcmp(qualifier, caller, sizeof(caller)) || !memcmp(qualifier, root, sizeof(root)));
        acl_free(qualifier);
        if (!safe) break;
    }
    /* Darwin returns -1/EINVAL at the end of an ACL, not POSIX's 0. */
    if (safe && result != -1) safe = 0;
    if (safe && errno != EINVAL) safe = 0;
    acl_free(acl);
    if (!safe) snprintf(error, capacity,
        "Extended ACL grants or unknown ACL state prevent private storage");
    return safe ? SYNC_STORAGE_OK : SYNC_STORAGE_UNSAFE;
#else
    (void)fd; (void)ancestor; (void)error; (void)capacity;
    return SYNC_STORAGE_OK;
#endif
}

/* Inspect the actual held object, including volume ownership and Darwin ACLs.
 * HOME excludes writes by others; DIRECTORY/FILE additionally require exact
 * 0700/0600 modes and files require a single link. Ancestors can be readable.
 */
static inline SyncStorageStatus SyncStorageCheck(int fd, SyncStorageKind kind, char *error, size_t capacity)
{
    struct stat st;
    if (fstat(fd, &st) < 0) {
        snprintf(error, capacity, "Inspect private storage: %s", strerror(errno)); return SYNC_STORAGE_IO;
    }
#ifdef __APPLE__
    struct statfs fs;
    if (fstatfs(fd, &fs) < 0) {
        snprintf(error, capacity, "Inspect private storage volume: %s", strerror(errno)); return SYNC_STORAGE_IO;
    }
    if (fs.f_flags & MNT_IGNORE_OWNERSHIP) {
        snprintf(error, capacity, "Private storage requires an ownership-enforcing volume; noowners volumes are refused");
        return SYNC_STORAGE_UNSAFE;
    }
#endif
    int safe;
    if (kind == SYNC_STORAGE_ANCESTOR)
        safe = S_ISDIR(st.st_mode) && (st.st_uid == 0 || st.st_uid == geteuid()) &&
            (!(st.st_mode & 0022) || (st.st_mode & S_ISVTX) ||
             (st.st_uid == 0 && st.st_gid == 0 && !(st.st_mode & 0002)));
    else if (kind == SYNC_STORAGE_HOME)
        safe = S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0022);
    else if (kind == SYNC_STORAGE_DIRECTORY)
        safe = S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 07777) == 0700;
    else if (kind == SYNC_STORAGE_FILE)
        safe = S_ISREG(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 07777) == 0600 && st.st_nlink == 1;
    else safe = 0;
    if (!safe) {
        snprintf(error, capacity, "Unsafe private storage ownership, permissions, type or link count");
        return SYNC_STORAGE_UNSAFE;
    }
    return SyncStorageACL(fd, kind == SYNC_STORAGE_ANCESTOR, error, capacity);
}

/* Resolve from / using openat+O_NOFOLLOW on every component, including home.
 * On success *out is an owned, checked HOME fd. Use mkdirat/openat relative to
 * this fd, check each resulting private object with SyncStorageCheck, and fsync
 * this HOME fd after creating OR adopting a private subdirectory before reporting
 * durable saves. Do not close and reopen a validated directory by pathname.
 */
static inline SyncStorageStatus SyncStorageOpenHome(const char *home, int *out, char *error, size_t capacity)
{
    *out = -1;
    if (!home || home[0] != '/' || strnlen(home, 65537) > 65536) {
        snprintf(error, capacity, "Private storage home must be an absolute bounded path"); return SYNC_STORAGE_UNSAFE;
    }
    char *path = strdup(home);
    if (!path) { snprintf(error, capacity, "Cannot inspect private storage access path"); return SYNC_STORAGE_NOMEM; }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    SyncStorageStatus status = SYNC_STORAGE_OK;
    if (fd < 0) { snprintf(error, capacity, "Open private storage root: %s", strerror(errno)); status = SYNC_STORAGE_IO; }
    char *cursor = path + 1;
    while (fd >= 0 && *cursor) {
        char *component = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        if (*cursor) *cursor++ = 0;
        if (!*component || !strcmp(component, ".")) continue;
        if (!strcmp(component, "..")) {
            snprintf(error, capacity, "Parent traversal is not allowed in private storage home");
            status = SYNC_STORAGE_UNSAFE; break;
        }
        status = SyncStorageCheck(fd, SYNC_STORAGE_ANCESTOR, error, capacity);
        if (status != SYNC_STORAGE_OK) break;
        int next = openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd); fd = next;
        if (fd < 0) { snprintf(error, capacity, "Open private storage path without following links: %s", strerror(errno)); status = SYNC_STORAGE_IO; }
    }
    if (fd >= 0 && status == SYNC_STORAGE_OK) status = SyncStorageCheck(fd, SYNC_STORAGE_HOME, error, capacity);
    free(path);
    if (status != SYNC_STORAGE_OK) { if (fd >= 0) close(fd); return status; }
    *out = fd; return SYNC_STORAGE_OK;
}
#endif
