/*
 * Behavioural test for resolve_variable_reference() in spc-exec.c.
 *
 * The runner (check-spc-exec-realloc.py) prepends this file's prologue to the
 * production functions remove_variable() and resolve_variable_reference(),
 * which it extracts verbatim from spc-exec.c, and appends the test body
 * (the second half of this file, after the body marker).  realloc is
 * redirected to a wrapper that can be told to fail on the Nth call.
 *
 * The runner splits this file at its two @@ marker comments.
 */

/* @@PROLOGUE@@ */
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;
typedef unsigned char u_char;
#define SPC_REMOVE_VAR "unset"

static int realloc_calls;
static int realloc_fail_at;	/* 1-based call number that fails; 0 = never */

static void *ck_realloc(void *p, size_t n)
{
    ++realloc_calls;
    if (realloc_fail_at && realloc_calls == realloc_fail_at)
        return NULL;		/* the old block stays valid, as in libc */
    return realloc(p, n);
}
#define realloc ck_realloc

/* @@BODY@@ */
static char *own(const char *s)
{
    char *p = malloc(strlen(s) + 1);
    assert(p);
    return strcpy(p, s);
}

static void arm(int fail_at)
{
    realloc_calls = 0;
    realloc_fail_at = fail_at;
}

/*
 * Strings handed to resolve_variable_reference() are deliberately never
 * freed: putenv() leaves them referenced by environ.
 */
int main(void)
{
    char *s, *orig;

    setenv("CKT_BASE", "/base", 1);

    /* Control: without injection the reference is expanded. */
    arm(0);
    s = own("CKT_A=$CKT_BASE/foo");
    resolve_variable_reference(&s);
    assert(strcmp(s, "CKT_A=/base/foo") == 0);
    assert(strcmp(getenv("CKT_A"), "/base/foo") == 0);
    assert(realloc_calls == 1);

    /* The only realloc fails: the caller keeps its original string. */
    arm(1);
    orig = s = own("CKT_B=$CKT_BASE/foo");
    resolve_variable_reference(&s);
    assert(realloc_calls == 1);
    assert(s != NULL);
    assert(s == orig);
    assert(strcmp(s, "CKT_B=$CKT_BASE/foo") == 0);
    /* The unexpanded value is still exported. */
    assert(strcmp(getenv("CKT_B"), "$CKT_BASE/foo") == 0);

    /* Second of two substitutions fails: the first one is kept intact. */
    arm(2);
    s = own("CKT_C=$CKT_BASE:${CKT_BASE}/x");
    resolve_variable_reference(&s);
    assert(realloc_calls == 2);
    assert(s != NULL);
    assert(strcmp(s, "CKT_C=/base:${CKT_BASE}/x") == 0);
    assert(strcmp(getenv("CKT_C"), "/base:${CKT_BASE}/x") == 0);

    /* Recovery: later calls work once the injection is off. */
    arm(0);
    s = own("CKT_D=${CKT_BASE}/bar");
    resolve_variable_reference(&s);
    assert(strcmp(s, "CKT_D=/base/bar") == 0);

    puts("spc-exec realloc: failed growth keeps the caller's string and never writes through NULL");
    return 0;
}
