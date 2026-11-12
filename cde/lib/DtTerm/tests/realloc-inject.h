/*
 * Allocation wrappers for the realloc failure tests.
 *
 * The runner puts this header in front of a production source file, so that
 * every malloc(), realloc() and free() of that file goes through the
 * wrappers below.  The wrappers keep a table of the live blocks, which
 * makes a leak on the error path observable without a leak sanitizer, and
 * can make the Nth realloc() since ck_arm() fail, leaving the old block
 * valid as libc does.
 */
#ifndef CK_REALLOC_INJECT_H
#define CK_REALLOC_INJECT_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK_MAX_BLOCKS 4096

static void *ck_blocks[CK_MAX_BLOCKS];
static int ck_live;		/* blocks allocated here and not freed */
static int ck_realloc_calls;
static int ck_realloc_fail_at;	/* 1-based call number that fails; 0 = never */

static void ck_track(void *p)
{
    int i;

    for (i = 0; i < CK_MAX_BLOCKS; i++) {
	if (ck_blocks[i] == NULL) {
	    ck_blocks[i] = p;
	    ck_live++;
	    return;
	}
    }
    abort();
}

static void ck_untrack(void *p)
{
    int i;

    for (i = 0; i < CK_MAX_BLOCKS; i++) {
	if (ck_blocks[i] == p) {
	    ck_blocks[i] = NULL;
	    ck_live--;
	    return;
	}
    }
    /* a block that libc allocated on its own, e.g. strdup() */
}

static void *ck_malloc(size_t n)
{
    void *p = malloc(n);

    if (p != NULL)
	ck_track(p);
    return p;
}

static void *ck_realloc(void *p, size_t n)
{
    void *q;

    ++ck_realloc_calls;
    if (ck_realloc_fail_at && ck_realloc_calls == ck_realloc_fail_at)
	return NULL;		/* the old block stays valid, as in libc */
    q = realloc(p, n);
    if (q != NULL) {
	if (p != NULL)
	    ck_untrack(p);
	ck_track(q);
    }
    return q;
}

static void ck_free(void *p)
{
    if (p != NULL)
	ck_untrack(p);
    free(p);
}

static void ck_arm(int fail_at)
{
    ck_realloc_calls = 0;
    ck_realloc_fail_at = fail_at;
}

#ifdef CK_FROM_ENVIRONMENT
/*
 * For a whole program: CK_REALLOC_FAIL_AT selects the failing realloc() and
 * the number of realloc() calls is written to the file named by
 * CK_REALLOC_COUNT_FILE when the program ends.
 */
static void ck_report(void)
{
    const char *name = getenv("CK_REALLOC_COUNT_FILE");
    FILE *f = name ? fopen(name, "w") : NULL;

    if (f != NULL) {
	fprintf(f, "%d\n", ck_realloc_calls);
	fclose(f);
    }
}

__attribute__((constructor)) static void ck_from_environment(void)
{
    const char *at = getenv("CK_REALLOC_FAIL_AT");

    if (at != NULL)
	ck_realloc_fail_at = atoi(at);
    atexit(ck_report);
}
#endif

#define malloc ck_malloc
#define realloc ck_realloc
#define free ck_free

#endif /* CK_REALLOC_INJECT_H */
