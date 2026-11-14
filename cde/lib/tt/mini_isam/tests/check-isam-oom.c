/*
 * Run the ISAM operations that allocate memory with each of their
 * allocations failing in turn.  Every operation must then return -1 with
 * iserrno set to ENOMEM, leave the file as it was, and must not exit or
 * crash; the same operation must work again afterwards.  Under a leak
 * checker, a failure path that does not release what it allocated shows up
 * as a nonzero exit status of the child process.
 *
 * Each injected run is made in a child process: the allocator is the one of
 * isalloc-inject.c, and the children report through their exit status
 * (0: the operation failed with ENOMEM and recovered, 2: the operation made
 * fewer allocations than the failing one and succeeded, other: error).
 */

#include "isam_impl.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <unistd.h>

extern size_t ck_oom_calls;
extern size_t ck_oom_fail_at;
extern size_t ck_oom_failures;

#define RECLEN 16
#define NRECS 20
#define MAX_POINTS 200

enum variant { V_NODUPS, V_DUPS, V_NOKEY, V_VAR };

static const char *const variant_name[] = {
    "nodups", "dups", "nokey", "varlen"
};

static int failures;
static const char *phase_name = "";
static const char *variant_label = "";

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: %s %s: %s (iserrno %d)\n", __FILE__, \
                    __LINE__, phase_name, variant_label, #cond, iserrno); \
            failures++; \
        } \
    } while (0)

static void
arm(size_t fail_at)
{
    ck_oom_calls = 0;
    ck_oom_failures = 0;
    ck_oom_fail_at = fail_at;
}

static void
disarm(void)
{
    ck_oom_fail_at = 0;
}

static void
make_key(struct keydesc *key, enum variant v)
{
    memset(key, 0, sizeof(*key));
    key->k_flags = (v == V_DUPS) ? ISDUPS : ISNODUPS;
    key->k_nparts = (v == V_NOKEY) ? 0 : 1;
    key->k_part[0].kp_start = 0;
    key->k_part[0].kp_leng = 8;
    key->k_part[0].kp_type = CHARTYPE;
}

static void
make_alt_key(struct keydesc *key)
{
    memset(key, 0, sizeof(*key));
    key->k_flags = ISDUPS;
    key->k_nparts = 1;
    key->k_part[0].kp_start = 7;
    key->k_part[0].kp_leng = 3;
    key->k_part[0].kp_type = CHARTYPE;
}

static void
make_record(char *record, int i, char tail)
{
    memset(record, 0, RECLEN);
    snprintf(record, RECLEN, "k%07d-%c", i, tail);
}

static int
build_mode(enum variant v)
{
    return ISINOUT + ISEXCLLOCK + (v == V_VAR ? ISVARLEN : ISFIXLEN);
}

/* Build a file of NRECS records.  Returns 0, or -1 with iserrno set. */
static int
fill(const char *name, enum variant v)
{
    struct keydesc key;
    char record[RECLEN];
    int fd;
    int i;

    make_key(&key, v);
    isreclen = RECLEN;
    fd = isbuild((char *)name, RECLEN, &key, build_mode(v));
    if (fd < 0)
        return -1;
    for (i = 0; i < NRECS; i++) {
        make_record(record, i, 'v');
        if (iswrite(fd, record) != 0) {
            isclose(fd);
            return -1;
        }
    }
    return isclose(fd);
}

/* Number of records that are read in order of the primary key. */
static int
count(const char *name, enum variant v)
{
    struct keydesc key;
    char record[RECLEN];
    int fd;
    int n = 0;

    fd = isopen((char *)name, ISINPUT + ISMANULOCK);
    if (fd < 0)
        return -1;
    make_key(&key, v);
    if (isstart(fd, &key, 0, record, ISFIRST) != 0)
        n = -1;
    else
        while (isread(fd, record, ISNEXT) == 0)
            n++;
    isclose(fd);
    return n;
}

/* The operation under test is armed; what it returned is checked here. */
static int
injected(void)
{
    return ck_oom_failures != 0;
}

static void
expect_enomem_or_ok(int result, int ok_value)
{
    if (injected()) {
        CHECK(result == -1);
        CHECK(iserrno == ENOMEM);
    } else {
        CHECK(result == ok_value);
    }
}

static void
expect_fd(int fd)
{
    if (injected()) {
        CHECK(fd == -1);
        CHECK(iserrno == ENOMEM);
    } else {
        CHECK(fd >= 0);
    }
}

static int
open_start(const char *name, enum variant v)
{
    struct keydesc key;
    char record[RECLEN];
    int fd = isopen((char *)name, ISINOUT + ISMANULOCK);

    if (fd < 0)
        return -1;
    make_key(&key, v);
    if (isstart(fd, &key, 0, record, ISFIRST) != 0) {
        isclose(fd);
        return -1;
    }
    return fd;
}

typedef void (*phase_fn)(const char *name, enum variant v, size_t fail_at);

static void
phase_build(const char *name, enum variant v, size_t fail_at)
{
    struct keydesc key;
    int fd;

    make_key(&key, v);
    isreclen = RECLEN;
    arm(fail_at);
    fd = isbuild((char *)name, RECLEN, &key, build_mode(v));
    expect_fd(fd);
    disarm();
    if (fd >= 0)
        CHECK(isclose(fd) == 0);
    if (fd < 0) {
        /* Nothing of the failed file remains: it can be built now. */
        CHECK(fill(name, v) == 0);
        CHECK(count(name, v) == NRECS);
    }
}

static void
phase_open(const char *name, enum variant v, size_t fail_at)
{
    int fd;

    CHECK(fill(name, v) == 0);
    arm(fail_at);
    fd = isopen((char *)name, ISINOUT + ISMANULOCK);
    expect_fd(fd);
    disarm();
    if (fd >= 0)
        CHECK(isclose(fd) == 0);
    CHECK(count(name, v) == NRECS);
}

static void
phase_start(const char *name, enum variant v, size_t fail_at)
{
    struct keydesc key;
    char record[RECLEN];
    int fd;
    int n = 0;

    CHECK(fill(name, v) == 0);
    fd = isopen((char *)name, ISINOUT + ISMANULOCK);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    make_key(&key, v);
    arm(fail_at);
    expect_enomem_or_ok(isstart(fd, &key, 0, record, ISFIRST), 0);
    disarm();
    CHECK(isstart(fd, &key, 0, record, ISFIRST) == 0);
    while (isread(fd, record, ISNEXT) == 0)
        n++;
    CHECK(n == NRECS);
    CHECK(isclose(fd) == 0);
}

/* isread allocates nothing; it must keep working while the allocator fails. */
static void
phase_read(const char *name, enum variant v, size_t fail_at)
{
    char record[RECLEN];
    int fd;
    int n = 0;

    CHECK(fill(name, v) == 0);
    fd = open_start(name, v);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    arm(fail_at);
    while (isread(fd, record, ISNEXT) == 0)
        n++;
    CHECK(n == NRECS);
    CHECK(!injected());
    CHECK(isclose(fd) == 0);
}

static void
phase_write(const char *name, enum variant v, size_t fail_at)
{
    char record[RECLEN];
    int fd;

    CHECK(fill(name, v) == 0);
    fd = isopen((char *)name, ISINOUT + ISMANULOCK);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    isreclen = RECLEN;
    make_record(record, NRECS, 'v');
    arm(fail_at);
    expect_enomem_or_ok(iswrite(fd, record), 0);
    disarm();
    CHECK(isclose(fd) == 0);
    CHECK(count(name, v) == (injected() ? NRECS : NRECS + 1));
}

static void
phase_rewcurr(const char *name, enum variant v, size_t fail_at)
{
    char record[RECLEN];
    char first[RECLEN];
    int fd;

    CHECK(fill(name, v) == 0);
    fd = open_start(name, v);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    CHECK(isread(fd, record, ISNEXT) == 0);
    make_record(record, 0, 'w');
    isreclen = RECLEN;
    arm(fail_at);
    expect_enomem_or_ok(isrewcurr(fd, record), 0);
    disarm();
    CHECK(isclose(fd) == 0);

    fd = open_start(name, v);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    CHECK(isread(fd, first, ISNEXT) == 0);
    CHECK(first[8] == '-' && first[9] == (injected() ? 'v' : 'w'));
    CHECK(isclose(fd) == 0);
    CHECK(count(name, v) == NRECS);
}

static void
phase_getcurpos(const char *name, enum variant v, size_t fail_at)
{
    char *buf = nullptr;
    int len = 0;
    int fd;

    CHECK(fill(name, v) == 0);
    fd = open_start(name, v);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    arm(fail_at);
    expect_enomem_or_ok(isgetcurpos(fd, &len, &buf), 0);
    disarm();
    if (injected())
        CHECK(buf == nullptr);
    else
        CHECK(buf != nullptr && len > 0);
    free(buf);
    buf = nullptr;
    CHECK(isgetcurpos(fd, &len, &buf) == 0);
    free(buf);
    CHECK(isclose(fd) == 0);
}

static void
phase_setcurpos(const char *name, enum variant v, size_t fail_at)
{
    char record[RECLEN];
    char *buf = nullptr;
    int len = 0;
    int fd;

    CHECK(fill(name, v) == 0);
    fd = open_start(name, v);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    CHECK(isread(fd, record, ISNEXT) == 0);
    CHECK(isgetcurpos(fd, &len, &buf) == 0);
    arm(fail_at);
    expect_enomem_or_ok(issetcurpos(fd, buf), 0);
    disarm();
    CHECK(issetcurpos(fd, buf) == 0);
    CHECK(isread(fd, record, ISNEXT) == 0);
    free(buf);
    CHECK(isclose(fd) == 0);
}

static void
phase_addindex(const char *name, enum variant v, size_t fail_at)
{
    struct keydesc key;
    char record[RECLEN];
    int fd;
    int n = 0;

    CHECK(fill(name, v) == 0);
    fd = isopen((char *)name, ISINOUT + ISEXCLLOCK);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    make_alt_key(&key);
    arm(fail_at);
    expect_enomem_or_ok(isaddindex(fd, &key), 0);
    disarm();
    if (injected())
        CHECK(isaddindex(fd, &key) == 0);
    CHECK(isstart(fd, &key, 0, record, ISFIRST) == 0);
    while (isread(fd, record, ISNEXT) == 0)
        n++;
    CHECK(n == NRECS);
    CHECK(isclose(fd) == 0);
    CHECK(count(name, v) == NRECS);
}

static void
phase_addprimary(const char *name, enum variant v, size_t fail_at)
{
    struct keydesc key;
    char record[RECLEN];
    int fd;
    int n = 0;

    CHECK(fill(name, V_NOKEY) == 0);
    fd = isopen((char *)name, ISINOUT + ISEXCLLOCK);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    make_key(&key, v);
    arm(fail_at);
    expect_enomem_or_ok(isaddprimary(fd, &key), 0);
    disarm();
    if (injected())
        CHECK(isaddprimary(fd, &key) == 0);
    CHECK(isstart(fd, &key, 0, record, ISFIRST) == 0);
    while (isread(fd, record, ISNEXT) == 0)
        n++;
    CHECK(n == NRECS);
    CHECK(isclose(fd) == 0);
}

struct phase {
    const char *name;
    phase_fn fn;
    int allocates;		/* the operation must reach an allocation */
    unsigned variants;		/* bit mask of the variants it is run with */
};

#define ALL ((1u << V_NODUPS) | (1u << V_DUPS) | (1u << V_NOKEY) | (1u << V_VAR))
#define KEYED_OR_NOKEY ((1u << V_NODUPS) | (1u << V_DUPS) | (1u << V_NOKEY))

static const struct phase phases[] = {
    { "isbuild", phase_build, 1, ALL },
    { "isopen", phase_open, 1, ALL },
    { "isstart", phase_start, 1, ALL },
    { "isread", phase_read, 0, ALL },
    { "iswrite", phase_write, 1, ALL },
    { "isrewcurr", phase_rewcurr, 1, KEYED_OR_NOKEY },
    { "isgetcurpos", phase_getcurpos, 1, KEYED_OR_NOKEY },
    { "issetcurpos", phase_setcurpos, 1, KEYED_OR_NOKEY },
    { "isaddindex", phase_addindex, 1, (1u << V_NODUPS) | (1u << V_DUPS) },
    { "isaddprimary", phase_addprimary, 1,
      (1u << V_NODUPS) | (1u << V_DUPS) },
};

static char dir[MAXPATHLEN / 2];

static void
remove_files(void)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[MAXPATHLEN];

    if (d == nullptr)
        return;
    while ((e = readdir(d)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
}

/*
 * Run one phase with the nth allocation failing, in a child process.
 * Returns the exit status of the child, or -1 if it did not exit.
 */
static int
run_child(const struct phase *ph, enum variant v, size_t n, int serial)
{
    char name[MAXPATHLEN];
    int status;
    pid_t pid;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        snprintf(name, sizeof(name), "%s/f%d", dir, serial);
        failures = 0;
        phase_name = ph->name;
        variant_label = variant_name[v];
        ph->fn(name, v, n);
        if (failures)
            exit(1);
        exit(ck_oom_failures != 0 ? 0 : 2);
    }
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    fprintf(stderr, "%s %s, allocation %zu failing: child killed by "
            "signal %d\n", ph->name, variant_name[v], n, WTERMSIG(status));
    return -1;
}

int
main(void)
{
    const char *tmp = getenv("TMPDIR");
    size_t p;
    int serial = 0;
    int v;

    snprintf(dir, sizeof(dir), "%s/check-isam-oom-XXXXXX",
             tmp != nullptr && *tmp != '\0' ? tmp : "/tmp");
    if (mkdtemp(dir) == nullptr) {
        perror("mkdtemp");
        return 1;
    }

    for (p = 0; p < sizeof(phases) / sizeof(phases[0]); p++) {
        for (v = V_NODUPS; v <= V_VAR; v++) {
            size_t n;
            int status = 0;

            if (!(phases[p].variants & (1u << v)))
                continue;
            for (n = 1; n <= MAX_POINTS; n++) {
                status = run_child(&phases[p], (enum variant)v, n, serial++);
                if (status != 0)
                    break;
            }
            if (status == 2 && (n > 1 || !phases[p].allocates)) {
                printf("%s %s: %zu allocation points\n", phases[p].name,
                       variant_name[v], n - 1);
            } else {
                fprintf(stderr, "%s %s: failed at allocation %zu (status "
                        "%d)\n", phases[p].name, variant_name[v], n, status);
                failures++;
            }
            remove_files();
        }
    }

    remove_files();
    rmdir(dir);
    return failures ? 1 : 0;
}
