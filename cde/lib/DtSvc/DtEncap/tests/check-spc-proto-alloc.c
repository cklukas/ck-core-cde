/*
 * Harness for check-spc-proto-alloc.py: sscan_logfile_data() and
 * sscan_register_data() of spc-proto.c with every allocation failing in
 * turn, including the copy of the default host information.  The runner
 * puts this file's prologue in front of the production functions, which it
 * extracts verbatim, and appends the body.  malloc, calloc, strdup and free
 * are redirected to wrappers that keep a table of the live blocks, so that
 * the Nth allocation can fail and a leak is observable.
 *
 * The runner splits this file at its two @@ marker comments.
 */

/* @@PROLOGUE@@ */
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char XeChar;
typedef char *XeString;
#define XeString_NULL ((XeString) 0)
#define Space ' '
#define SPC_ERROR 0
#define TRUE 1
#define SPC_Out_Of_Memory 100
#define SPC_UNKNOWN_HOSTINFO_STR "unknown"

static int last_error;
static void SPC_Error(int code, ...) { last_error = code; }

#define CK_MAX_BLOCKS 256
static void *ck_blocks[CK_MAX_BLOCKS];
static int ck_live;
static int ck_calls;
static int ck_fail_at;		/* 1-based allocation that fails; 0 = never */

static void *ck_track(void *p)
{
    int i;

    for (i = 0; p && i < CK_MAX_BLOCKS; i++)
        if (ck_blocks[i] == NULL) {
            ck_blocks[i] = p;
            ck_live++;
            return p;
        }
    assert(p == NULL);
    return p;
}

static int ck_fails(void)
{
    return ck_fail_at && ++ck_calls == ck_fail_at;
}

static void *ck_malloc(size_t n) { return ck_fails() ? NULL : ck_track(malloc(n)); }
static void *ck_calloc(size_t n, size_t s) { return ck_fails() ? NULL : ck_track(calloc(n, s)); }
static char *ck_strdup(const char *s) { return ck_fails() ? NULL : ck_track(strdup(s)); }

static void ck_free(void *p)
{
    int i;

    for (i = 0; p && i < CK_MAX_BLOCKS; i++)
        if (ck_blocks[i] == p) {
            ck_blocks[i] = NULL;
            ck_live--;
            break;
        }
    free(p);
}

static void arm(int fail_at)
{
    ck_calls = 0;
    ck_fail_at = fail_at;
    last_error = 0;
}

#define malloc ck_malloc
#define calloc ck_calloc
#define strdup ck_strdup
#define free ck_free

/* @@BODY@@ */
/* "<count in hex> NUL string NUL string NUL ...", as the peer sends it. */
static const char logfile_old[] = "1\0/tmp/log\0";
static const char logfile_new[] = "3\0/tmp/log\0" "A.02\0" "hp-ux 9.0\0";
static const char register_old[] = "2\0user\0secret\0";
static const char register_new[] = "4\0user\0secret\0" "A.02\0" "host info\0";

static void test_logfile(void)
{
    XeString logfile, proto_ver, hostinfo;
    int k, total;

    /* A peer that sends no host information gets the default. */
    arm(1000000);
    assert(sscan_logfile_data((XeString) logfile_old, (XeString) logfile_old + sizeof logfile_old,
                              &logfile, &proto_ver, &hostinfo) == TRUE);
    total = ck_calls;
    assert(strcmp(logfile, "/tmp/log") == 0 && proto_ver == NULL);
    assert(strcmp(hostinfo, "unknown") == 0);
    free(logfile);
    free(hostinfo);
    assert(ck_live == 0);

    /* Every allocation fails in turn: an error, no strings, no leak. */
    for (k = 1; k <= total; k++) {
        arm(k);
        assert(sscan_logfile_data((XeString) logfile_old,
                                  (XeString) logfile_old + sizeof logfile_old,
                                  &logfile, &proto_ver, &hostinfo) == SPC_ERROR);
        assert(ck_live == 0);
        if (k == total) {
            /* the copy of the default host information */
            assert(last_error == SPC_Out_Of_Memory);
            assert(logfile == NULL && proto_ver == NULL);
        }
    }

    /* A full answer needs no default. */
    arm(0);
    assert(sscan_logfile_data((XeString) logfile_new, (XeString) logfile_new + sizeof logfile_new,
                              &logfile, &proto_ver, &hostinfo) == TRUE);
    assert(strcmp(proto_ver, "A.02") == 0 && strcmp(hostinfo, "hp-ux 9.0") == 0);
    free(logfile); free(proto_ver); free(hostinfo);
    assert(ck_live == 0);
}

static void test_register(void)
{
    XeString user, passwd, proto_ver, hostinfo;
    int k, total;

    arm(1000000);
    assert(sscan_register_data((XeString) register_old, (XeString) register_old + sizeof register_old,
                               &user, &passwd, &proto_ver, &hostinfo) == TRUE);
    total = ck_calls;
    assert(strcmp(user, "user") == 0 && strcmp(passwd, "secret") == 0);
    assert(proto_ver == NULL && strcmp(hostinfo, "unknown") == 0);
    free(user); free(passwd); free(hostinfo);
    assert(ck_live == 0);

    for (k = 1; k <= total; k++) {
        arm(k);
        assert(sscan_register_data((XeString) register_old,
                                   (XeString) register_old + sizeof register_old,
                                   &user, &passwd, &proto_ver, &hostinfo) == SPC_ERROR);
        assert(ck_live == 0);
        if (k == total) {
            assert(last_error == SPC_Out_Of_Memory);
            assert(user == NULL && passwd == NULL && proto_ver == NULL);
        }
    }

    arm(0);
    assert(sscan_register_data((XeString) register_new, (XeString) register_new + sizeof register_new,
                               &user, &passwd, &proto_ver, &hostinfo) == TRUE);
    assert(strcmp(hostinfo, "host info") == 0);
    free(user); free(passwd); free(proto_ver); free(hostinfo);
    assert(ck_live == 0);
}

int main(void)
{
    test_logfile();
    test_register();
    puts("SPC protocol: a failed copy of the host information is reported without a leak");
    return 0;
}
