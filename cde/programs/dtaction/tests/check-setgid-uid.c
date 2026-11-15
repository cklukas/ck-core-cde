/* Harness for check-setgid-uid.py: SetGidUid() of dtaction against a fake
 * kernel.  The production function is pasted between the markers below. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(TEST_VARIANT_SVR4)
#define SVR4 1
#else
#define __linux__ 1
#endif

/* what the fake kernel was asked to do, in order, and which call fails */
static char trace[256];
static const char *failing = "";

static int step(const char *name, long a, long b)
{
    char entry[64];
    snprintf(entry, sizeof entry, "%s(%ld,%ld) ", name, a, b);
    strncat(trace, entry, sizeof trace - strlen(trace) - 1);
    if (strcmp(failing, name) == 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int stub_setgid(gid_t g) { return step("setgid", g, 0); }
static int stub_setuid(uid_t u) { return step("setuid", u, 0); }
static int stub_setregid(gid_t r, gid_t e) { return step("setregid", r, e); }
static int stub_setreuid(uid_t r, uid_t e) { return step("setreuid", r, e); }
static int stub_initgroups(const char *u, gid_t g)
{
    return step(u != nullptr && strcmp(u, "alice") == 0 ? "initgroups" : "initgroups-other", g, 0);
}

#define setgid stub_setgid
#define setuid stub_setuid
#define setregid stub_setregid
#define setreuid stub_setreuid
#define initgroups stub_initgroups
/* @@BODY@@ */

#if defined(TEST_VARIANT_SVR4)
#define SET_GROUP "setgid(100,0) "
#define SET_USER  "setuid(200,0) "
#else
#define SET_GROUP "setregid(100,100) "
#define SET_USER  "setreuid(200,200) "
#endif

static int run(const char *fail, const char *user)
{
    int rc;
    trace[0] = '\0';
    failing = fail;
    rc = SetGidUid(100, 200, user);
    return rc;
}

static void expect(const char *what, int got, int want, const char *want_trace)
{
    if (got != want || strcmp(trace, want_trace) != 0) {
        fprintf(stderr, "FAIL %s: returned %d (want %d), trace \"%s\" (want \"%s\")\n",
                what, got, want, trace, want_trace);
        exit(1);
    }
}

int main(void)
{
    int rc;

    rc = run("", nullptr);
    expect("own ids", rc, 0, SET_GROUP SET_USER);

    rc = run("", "alice");
    expect("other user", rc, 0, SET_GROUP "initgroups(100,0) " SET_USER);

    /* a refused step must stop the later ones: no user ID change after a
     * failed group change, or the process would run as the wrong identity */
    rc = run(strstr(SET_GROUP, "setgid") ? "setgid" : "setregid", "alice");
    expect("group refused", rc, -1, SET_GROUP);

    rc = run("initgroups", "alice");
    expect("supplementary groups refused", rc, -1, SET_GROUP "initgroups(100,0) ");

    rc = run(strstr(SET_USER, "setuid") ? "setuid" : "setreuid", "alice");
    expect("user refused", rc, -1, SET_GROUP "initgroups(100,0) " SET_USER);

    puts("ok");
    return 0;
}
