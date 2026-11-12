/*
 * Behavioural test for SPCD_Initialize() in dtspcd/main.c under allocation
 * failure.
 *
 * The runner (check-env-file-alloc.py) prepends this file's prologue to the
 * production function SPCD_Initialize(), which it extracts verbatim from
 * main.c, and appends the test body (the second half of this file, after the
 * body marker).  malloc, realloc and free are redirected to wrappers that
 * count live blocks and can be told to fail chosen allocations, numbered
 * from 1 in call order: 1 is the default environment array, 2 the
 * install-directory path, 3 the configured-directory path.  SPC_Initialize()
 * and SPC_Add_Env_File() are stubs; the latter records each file name it is
 * given and appends it to the environment list, as the real one does.
 *
 * The runner splits this file at its two @@ marker comments.
 */

/* @@PROLOGUE@@ */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char *XeString;
#define SPC_ERROR (-1)
#define TRUE 1
#define DEFAULT_ENVP_SIZE 50
#define CDE_INSTALLATION_TOP "/usr/dt"
#define CDE_CONFIGURATION_TOP "/etc/dt"
#define SPCD_ENV_INSTALL_DIRECTORY CDE_INSTALLATION_TOP "/config"
#define SPCD_ENV_CONFIG_DIRECTORY CDE_CONFIGURATION_TOP "/config"
#define SPCD_ENV_FILE "dtspcdenv"
#define SPCD_PROG_NAME "dtspcd"

static XeString XeProgName;
static XeString *default_environment;

static unsigned alloc_calls;	/* malloc and realloc calls so far */
static unsigned fail_mask;	/* bit N set: allocation number N fails */
static long live_blocks;	/* blocks obtained and not yet freed */

static int failing(void)
{
    ++alloc_calls;
    return alloc_calls < 32 && (fail_mask >> alloc_calls & 1);
}

static void *ck_malloc(size_t n)
{
    void *p;
    if (failing())
        return NULL;
    p = malloc(n);
    if (p)
        ++live_blocks;
    return p;
}

static void *ck_realloc(void *old, size_t n)
{
    void *p;
    if (failing())
        return NULL;		/* the old block stays valid, as in libc */
    p = realloc(old, n);
    if (p && !old)
        ++live_blocks;
    return p;
}

static void ck_free(void *p)
{
    if (p)
        --live_blocks;
    free(p);
}
#define malloc ck_malloc
#define realloc ck_realloc
#define free ck_free

static int SPC_Initialize(void)
{
    return 0;
}

static XeString added[8];	/* file names SPC_Add_Env_File() received */
static int added_count;

static XeString *SPC_Add_Env_File(XeString file, XeString *envp)
{
    int n = 0;
    char *copy = strdup(file);
    assert(copy && added_count < 8);
    added[added_count++] = copy;
    while (envp[n])
        ++n;
    assert(n + 1 < DEFAULT_ENVP_SIZE);
    envp[n] = copy;
    envp[n + 1] = NULL;
    return envp;
}

/* @@BODY@@ */
static const char *const install_path = CDE_INSTALLATION_TOP "/config/" SPCD_ENV_FILE;
static const char *const config_path = CDE_CONFIGURATION_TOP "/config/" SPCD_ENV_FILE;

/*
 * Run SPCD_Initialize() with the allocations in mask failing and check that
 * the environment list holds exactly the files whose buffers were allocated.
 *
 * The recorded names and the environment array are deliberately not freed
 * here; the runner disables leak detection for that reason.  The path
 * buffers owned by SPCD_Initialize() are accounted for by live_blocks.
 */
static void run(unsigned mask, int want_install, int want_config)
{
    int n = 0, i;
    const char *want[2];

    if (want_install)
        want[n++] = install_path;
    if (want_config)
        want[n++] = config_path;

    alloc_calls = 0;
    fail_mask = mask;
    live_blocks = 0;
    added_count = 0;
    default_environment = NULL;

    assert(SPCD_Initialize() == TRUE);

    assert(default_environment != NULL);
    for (i = 0; i < n; ++i) {
        assert(default_environment[i] != NULL);
        assert(strcmp(default_environment[i], want[i]) == 0);
    }
    assert(default_environment[n] == NULL);
    assert(added_count == n);
    /* Only the environment array may remain allocated. */
    assert(live_blocks == 1);
    printf("mask 0x%x: %d file(s) added, no leaked path buffer\n", mask, n);
}

int main(void)
{
    run(0, 1, 1);		/* no failure: both files, in order */
    run(1u << 2, 0, 1);		/* install-directory path fails */
    run(1u << 3, 1, 0);		/* configured-directory path fails */
    run((1u << 2) | (1u << 3), 0, 0);
    return 0;
}
