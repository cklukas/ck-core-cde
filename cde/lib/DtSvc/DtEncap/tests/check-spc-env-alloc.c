/*
 * Behavioural test for SPC_Create_Default_Envp() in spc-env.c.
 *
 * The runner (check-spc-env-alloc.py) prepends this file's prologue to the
 * production function, which it extracts verbatim from spc-env.c, and appends
 * the test body (after the body marker).  malloc and realloc are redirected
 * to wrappers sharing one call counter, so that the Nth allocation made by
 * the function can be made to fail.  SPC_Putenv() and SPC_Add_Env_File() are
 * stubs that record what they are given.
 *
 * The runner splits this file at its two @@ marker comments.
 */

/* @@PROLOGUE@@ */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char XeChar;
typedef char *XeString;
#define XeChar_NULL 0
#define DEFAULT_ENVP_SIZE 8
#define SPCD_ENV_INSTALL_DIRECTORY "/install/dir"
#define SPCD_ENV_CONFIG_DIRECTORY "/config/dir"
#define SPCD_ENV_FILE "env.file"
#define XeMalloc malloc
#define _DtSvcProcessLock()
#define _DtSvcProcessUnlock()

static char *official_hostname = "testhost";
static char *spc_user_environment_file = "/user/env.file";

static int alloc_calls;
static int alloc_fail_at;	/* 1-based call number that fails; 0 = never */

static void *ck_malloc(size_t n)
{
    ++alloc_calls;
    if (alloc_fail_at && alloc_calls == alloc_fail_at)
        return NULL;
    return malloc(n);
}

static void *ck_realloc(void *p, size_t n)
{
    ++alloc_calls;
    if (alloc_fail_at && alloc_calls == alloc_fail_at)
        return NULL;		/* the old block stays valid, as in libc */
    return realloc(p, n);
}
#define malloc ck_malloc
#define realloc ck_realloc

/* Recorded calls, in order: "P:<string>" for putenv, "F:<path>" for files. */
static char recorded[16][96];
static int nrecorded;

static void record(const char *kind, const char *s)
{
    assert(nrecorded < 16);
    assert(strlen(s) + 3 < sizeof recorded[0]);
    snprintf(recorded[nrecorded++], sizeof recorded[0], "%s:%s", kind, s);
}

static XeString *SPC_Putenv(XeString s, XeString *envp)
{
    record("P", s);
    return envp;
}

static XeString *SPC_Add_Env_File(XeString path, XeString *envp)
{
    record("F", path);
    return envp;
}

/* @@BODY@@ */
static int has(const char *entry)
{
    for (int i = 0; i < nrecorded; ++i)
        if (strcmp(recorded[i], entry) == 0)
            return 1;
    return 0;
}

static void run(int fail_at)
{
    static XeString envp_storage[DEFAULT_ENVP_SIZE];
    nrecorded = 0;
    alloc_calls = 0;
    alloc_fail_at = fail_at;
    XeString *r = SPC_Create_Default_Envp(envp_storage);
    assert(r == envp_storage);
}

int main(void)
{
    /* DISPLAY set and LANG unset: allocation 1 is the DISPLAY string,
     * 2 the install-directory path, 3 the configured-directory path. */
    setenv("DISPLAY", "disp:1", 1);
    unsetenv("LANG");

    const char *install = "F:/install/dir/env.file";
    const char *config = "F:/config/dir/env.file";
    const char *user = "F:/user/env.file";

    /* No injection: both paths added, in order, then the user file. */
    run(0);
    assert(alloc_calls == 3);
    assert(nrecorded == 4);
    assert(strcmp(recorded[0], "P:DISPLAY=disp:1") == 0);
    assert(strcmp(recorded[1], install) == 0);
    assert(strcmp(recorded[2], config) == 0);
    assert(strcmp(recorded[3], user) == 0);
    puts("ok: no failure, install, config and user files added in order");

    /* Install-directory allocation fails: that file is skipped, the
     * configured one and the user file are still added. */
    run(2);
    assert(nrecorded == 3);
    assert(strcmp(recorded[0], "P:DISPLAY=disp:1") == 0);
    assert(!has(install));
    assert(strcmp(recorded[1], config) == 0);
    assert(strcmp(recorded[2], user) == 0);
    puts("ok: install path allocation failure skips only that file");

    /* Configured-directory allocation fails: install file stays added,
     * no entry for the configured one. */
    run(3);
    assert(nrecorded == 3);
    assert(strcmp(recorded[0], "P:DISPLAY=disp:1") == 0);
    assert(strcmp(recorded[1], install) == 0);
    assert(!has(config));
    assert(strcmp(recorded[2], user) == 0);
    puts("ok: config path allocation failure skips only that file");

    return 0;
}
