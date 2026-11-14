/*
 * Exercise the operations that derive file names from an ISAM file name:
 * rename, repair and garbage collection, and the rejection of names that
 * are too long to carry a suffix.
 */

#include "isam_impl.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: %s (iserrno %d)\n", __FILE__, __LINE__, \
                    #cond, iserrno); \
            failures++; \
        } \
    } while (0)

#define RECLEN 16
#define NRECS 20

static void
make_key(struct keydesc *key)
{
    memset(key, 0, sizeof(*key));
    key->k_flags = ISNODUPS;
    key->k_nparts = 1;
    key->k_part[0].kp_start = 0;
    key->k_part[0].kp_leng = 8;
    key->k_part[0].kp_type = CHARTYPE;
}

static int
fill(const char *name)
{
    struct keydesc key;
    char record[RECLEN];
    int fd;
    int i;

    make_key(&key);
    fd = isbuild((char *)name, RECLEN, &key, ISINOUT + ISFIXLEN + ISEXCLLOCK);
    if (fd < 0)
        return -1;
    for (i = 0; i < NRECS; i++) {
        memset(record, 0, sizeof(record));
        snprintf(record, sizeof(record), "k%07d-v", i);
        if (iswrite(fd, record) != 0)
            return -1;
    }
    return isclose(fd);
}

static int
count(const char *name)
{
    struct keydesc key;
    char record[RECLEN];
    int fd;
    int n = 0;

    fd = isopen((char *)name, ISINPUT + ISMANULOCK);
    if (fd < 0)
        return -1;
    make_key(&key);
    if (isstart(fd, &key, 0, record, ISFIRST) != 0)
        n = -1;
    else
        while (isread(fd, record, ISNEXT) == 0)
            n++;
    isclose(fd);
    return n;
}

int
main(void)
{
    const char *tmp = getenv("TMPDIR");
    char dir[MAXPATHLEN / 2];
    char a[MAXPATHLEN], b[MAXPATHLEN], longname[MAXPATHLEN + 16];

    snprintf(dir, sizeof(dir), "%s/check-isam-files-XXXXXX",
             tmp != nullptr && *tmp != '\0' ? tmp : "/tmp");
    if (mkdtemp(dir) == nullptr) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(a, sizeof(a), "%s/a", dir);
    snprintf(b, sizeof(b), "%s/b", dir);

    CHECK(fill(a) == 0);
    CHECK(count(a) == NRECS);

    CHECK(isrename(a, b) == 0);
    CHECK(count(b) == NRECS);
    CHECK(isopen(a, ISINPUT) < 0);

    CHECK(isgarbage(b) == 0);
    CHECK(count(b) == NRECS);

    CHECK(isrepair(b, 0) == 0);
    CHECK(count(b) == NRECS);

    memset(longname, 'x', sizeof(longname) - 1);
    longname[0] = '/';
    longname[sizeof(longname) - 1] = '\0';
    CHECK(isopen(longname, ISINPUT) < 0 && iserrno == EFNAME);
    CHECK(isgarbage(longname) < 0 && iserrno == EFNAME);
    CHECK(isrepair(longname, 0) < 0 && iserrno == EFNAME);
    CHECK(isrename(b, longname) < 0 && iserrno == EFNAME);
    CHECK(count(b) == NRECS);

    CHECK(iserase(b) == 0);
    rmdir(dir);

    return failures ? 1 : 0;
}
