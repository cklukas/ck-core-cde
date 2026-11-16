#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef char Boolean;
typedef void *Widget;
typedef void *XtAppContext;
typedef unsigned long XtIntervalId;
typedef void *XtPointer;
typedef char *String;

#ifndef True
#define True 1
#endif
#ifndef False
#define False 0
#endif

static char *
XtMalloc(size_t size)
{
    return (char *)malloc(size ? size : 1);
}

static char *
XtCalloc(size_t count, size_t size)
{
    return (char *)calloc(count ? count : 1, size ? size : 1);
}

static char *
XtRealloc(char *ptr, size_t size)
{
    return (char *)realloc(ptr, size ? size : 1);
}

static void
XtFree(char *ptr)
{
    free(ptr);
}

static char *
XtNewString(const char *str)
{
    if (str == NULL)
        return NULL;
    return strdup(str);
}

static const int StorageBatch = 8000;

#include "dtfile_storage_size_extracted.inc"

static void
test_hardlink_block_dedup_synthetic(void)
{
    StorageSizeData data;
    memset(&data, 0, sizeof(data));

    /*
     * 1. Two hardlinked names to the same (st_dev=10, st_ino=1001) with
     *    st_nlink=2, st_size=4096, st_blocks=8 (4096 bytes on disk).
     */
    struct stat st_link;
    memset(&st_link, 0, sizeof(st_link));
    st_link.st_dev = (dev_t)10;
    st_link.st_ino = (ino_t)1001;
    st_link.st_mode = S_IFREG | 0644;
    st_link.st_nlink = 2;
    st_link.st_size = 4096;
    st_link.st_blocks = 8;

    StorageSizeAddEntrySize(&data, &st_link, False);
    StorageSizeAddEntrySize(&data, &st_link, False);

    /* Apparent size counts every regular filename: 2 * 4096 = 8192 */
    assert(data.total_size == 8192ULL);
    assert(data.regular_size == 8192ULL);

    /* Actual disk usage counts once per unique (st_dev, st_ino): 1 * 4096 = 4096 */
    assert(data.total_blocks == 4096ULL);
    assert(data.regular_blocks == 4096ULL);

#if HAVE_HARDLINK_FIELDS
    assert(data.hardlink_name_count == 2UL);
    assert(data.hardlink_unique_inode_count == 1UL);
    assert(data.hardlink_extra_name_count == 1UL);
#endif

    /*
     * 2. Same inode number (1001) on a different device (st_dev=11) must be
     *    counted as a distinct inode on disk!
     */
    struct stat st_other_dev = st_link;
    st_other_dev.st_dev = (dev_t)11;
    st_other_dev.st_nlink = 1;
    StorageSizeAddEntrySize(&data, &st_other_dev, False);

    assert(data.total_size == 12288ULL);
    assert(data.total_blocks == 8192ULL);

    /*
     * 3. Exercise hash table growth (> 350 unique inodes) and verify all
     *    previously recorded inodes remain deduplicated after rehash.
     */
    for (unsigned int i = 0; i < 400; ++i)
    {
        struct stat st_many;
        memset(&st_many, 0, sizeof(st_many));
        st_many.st_dev = (dev_t)20;
        st_many.st_ino = (ino_t)(5000 + i);
        st_many.st_mode = S_IFREG | 0644;
        st_many.st_nlink = 3;
        st_many.st_size = 1000;
        st_many.st_blocks = 2; /* 1024 bytes */

        /* Add first time */
        StorageSizeAddEntrySize(&data, &st_many, False);
        /* Add second time (duplicate hardlink name) */
        StorageSizeAddEntrySize(&data, &st_many, False);
    }

    /* Re-adding the very first hardlink (dev=10, ino=1001) still must not add blocks */
    unsigned long long blocks_before = data.total_blocks;
    StorageSizeAddEntrySize(&data, &st_link, False);
    assert(data.total_blocks == blocks_before);

#if HAVE_HARDLINK_FIELDS
    assert(data.hardlink_unique_inode_count == 1UL + 400UL);
    assert(data.hardlink_extra_name_count == 2UL + 400UL);
    StorageSizeFreeSeenInodes(&data);
    assert(data.seen_inodes == NULL);
    assert(data.seen_inode_capacity == 0);
    assert(data.seen_inode_count == 0);
#endif
}

static void
test_hardlink_directory_traversal(const char *scratch_dir)
{
    char test_root[PATH_MAX - 64];
    snprintf(test_root, sizeof(test_root), "%s/scan_tree", scratch_dir);
    mkdir(test_root, 0700);

    char file_a[PATH_MAX];
    char file_b[PATH_MAX];
    char file_c[PATH_MAX];
    char single_file[PATH_MAX];
    snprintf(file_a, sizeof(file_a), "%s/hardlink_a.dat", test_root);
    snprintf(file_b, sizeof(file_b), "%s/hardlink_b.dat", test_root);
    snprintf(file_c, sizeof(file_c), "%s/hardlink_c.dat", test_root);
    snprintf(single_file, sizeof(single_file), "%s/unique.dat", test_root);

    unlink(file_a);
    unlink(file_b);
    unlink(file_c);
    unlink(single_file);

    int fd = open(file_a, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0);
    char payload[8192];
    memset(payload, 'A', sizeof(payload));
    assert(write(fd, payload, sizeof(payload)) == (ssize_t)sizeof(payload));
    close(fd);

    assert(link(file_a, file_b) == 0);
    assert(link(file_a, file_c) == 0);

    fd = open(single_file, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0);
    memset(payload, 'B', 4096);
    assert(write(fd, payload, 4096) == 4096);
    close(fd);

    struct stat st_hl;
    struct stat st_uni;
    assert(lstat(file_a, &st_hl) == 0);
    assert(lstat(single_file, &st_uni) == 0);

    StorageSizeData data;
    memset(&data, 0, sizeof(data));
    data.scanning = True;
    assert(StorageSizePushDir(&data, test_root) == True);
    StorageSizeProcessBatch(&data);
    assert(data.done == True);

    /* 4 regular filenames total: 3 hardlinked + 1 unique */
    assert(data.regular_file_count == 4UL);
    unsigned long long expected_apparent =
        3ULL * (unsigned long long)st_hl.st_size + (unsigned long long)st_uni.st_size;
    unsigned long long expected_blocks =
        ((unsigned long long)st_hl.st_blocks + (unsigned long long)st_uni.st_blocks) * 512ULL;

    assert(data.total_size == expected_apparent);
    assert(data.total_blocks == expected_blocks);

#if HAVE_HARDLINK_FIELDS
    assert(data.hardlink_name_count == 3UL);
    assert(data.hardlink_unique_inode_count == 1UL);
    assert(data.hardlink_extra_name_count == 2UL);
    StorageSizeFreeSeenInodes(&data);
#endif
    StorageSizeFreeStack(&data);
    XtFree(data.error_message);

    unlink(file_a);
    unlink(file_b);
    unlink(file_c);
    unlink(single_file);
    rmdir(test_root);
}

static void
test_shared_procs_hardening(const char *scratch_dir)
{
    /* 1. _DtPName bounds & nullptr checks */
    assert(strcmp(_DtPName(NULL), ".") == 0);
    assert(strcmp(_DtPName("relative_name"), ".") == 0);
    assert(strcmp(_DtPName("/usr/dt/bin/dtfile"), "/usr/dt/bin") == 0);
    assert(strcmp(_DtPName("/root_child"), "/") == 0);

    char overlong[MAXPATHLEN * 2];
    memset(overlong, 'x', sizeof(overlong) - 1);
    overlong[0] = '/';
    overlong[10] = '/';
    overlong[sizeof(overlong) - 1] = '\0';
    char *parent = _DtPName(overlong);
    assert(parent != NULL);
    assert(strlen(parent) < MAXPATHLEN);

    /* 2. _DtBuildPath bounds checks */
    char built[MAXPATHLEN];
    _DtBuildPath(built, overlong, "leaf");
    assert(strlen(built) == MAXPATHLEN - 1);

    /* 3. _DtFollowLink relative symlink without '/' in link name */
    char cwd_save[PATH_MAX];
    assert(getcwd(cwd_save, sizeof(cwd_save)) != NULL);
    assert(chdir(scratch_dir) == 0);

    unlink("rel_target.txt");
    unlink("rel_link_no_slash");
    int fd = open("rel_target.txt", O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0);
    close(fd);
    assert(symlink("rel_target.txt", "rel_link_no_slash") == 0);

    char *resolved = _DtFollowLink("rel_link_no_slash");
    assert(resolved != NULL);
    assert(strcmp(resolved, "rel_target.txt") == 0);

    unlink("rel_link_no_slash");
    unlink("rel_target.txt");
    assert(chdir(cwd_save) == 0);

#if HAVE_EXPAND_ENV_VARS
    /* 4. _DtExpandPathEnvVars expands $VAR and ${VAR} without invoking /bin/sh */
    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/pwned_marker", scratch_dir);
    unlink(marker);

    setenv("DTFILE_TEST_DIR", "/opt/dt/custom", 1);
    char injection_input[PATH_MAX + 128];
    snprintf(injection_input, sizeof(injection_input),
             "$DTFILE_TEST_DIR/${DTFILE_TEST_DIR}/sub;touch %s", marker);

    char *expanded = _DtExpandPathEnvVars(injection_input);
    assert(expanded != NULL);
    assert(strstr(expanded, "/opt/dt/custom//opt/dt/custom/sub;touch ") == expanded);
    /* Marker file must NOT exist because no shell was invoked */
    struct stat st_marker;
    assert(lstat(marker, &st_marker) != 0);
    XtFree(expanded);
#endif
}

int
main(int argc, char **argv)
{
    const char *scratch_dir = (argc > 1) ? argv[1] : ".";
    test_hardlink_block_dedup_synthetic();
    test_hardlink_directory_traversal(scratch_dir);
    test_shared_procs_hardening(scratch_dir);
    printf("check-storage-size-hardlinks: all tests passed\n");
    return 0;
}
