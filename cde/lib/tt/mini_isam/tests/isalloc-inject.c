/*
 * isalloc.c compiled with an allocator that can be made to fail.  The test
 * program check-isam-oom links this object instead of isalloc.o of the
 * library, so that the library's own allocation helpers are the code under
 * test.
 */

#include <stdlib.h>
#include <string.h>

size_t ck_oom_calls;		/* allocations requested since the last arm */
size_t ck_oom_fail_at;		/* number of the allocation to fail; 0: none */
size_t ck_oom_failures;		/* allocations that were failed */

static int
ck_oom_fails(void)
{
    ck_oom_calls++;
    if (ck_oom_fail_at != 0 && ck_oom_calls == ck_oom_fail_at) {
        ck_oom_failures++;
        return 1;
    }
    return 0;
}

static void *
ck_oom_malloc(size_t n)
{
    if (ck_oom_fails())
        return nullptr;
    return malloc(n);
}

static char *
ck_oom_strdup(const char *s)
{
    if (ck_oom_fails())
        return nullptr;
    return strdup(s);
}

#define malloc ck_oom_malloc
#define strdup ck_oom_strdup
#include "../isalloc.c"
