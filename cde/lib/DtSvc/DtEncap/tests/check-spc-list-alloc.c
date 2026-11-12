/*
 * Behavioural test for the lists of the SPC daemon that grow with realloc:
 * SPC_logfile_list, filled by open_noio_channel_object() in noio.c, and
 * SPC_pid_list, filled by reserve_pid_slot() in spc-exec.c.
 *
 * The runner (check-spc-list-alloc.py) prepends this file's prologue to the
 * production code, which it extracts verbatim from the two sources, and
 * appends the test body (after the body marker).  malloc and realloc are
 * redirected to wrappers sharing one call counter, so that the Nth
 * allocation made by the code under test can be made to fail.  The pid
 * tests are compiled only when the runner defines TEST_PID_LIST.
 *
 * The runner splits this file at its two @@ marker comments.
 */

/* @@PROLOGUE@@ */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef char *XeString;
typedef struct { char *logfile; } SPC_Channel, *SPC_Channel_Ptr;
#define SPC_ERROR 0
#define SPC_Out_Of_Memory 100
#define SPCIO_USE_LOGFILE 1
#define IS_SPCIO_USE_LOGFILE(a) ((a) & SPCIO_USE_LOGFILE)
#define SPCD_DEAD_PROCESS (-1)
#define call_parent_method(channel, func, args, result) ((result) = (channel))
#define _DtSvcProcessLock()
#define _DtSvcProcessUnlock()

char **SPC_logfile_list = NULL;
pid_t *SPC_pid_list = NULL;
static char *SPCD_Authentication_Dir = "/nonexistent-spc-dir";
static int last_error;
static void SPC_Error(int code, ...) { last_error = code; }
static char *get_tmp_dir(void) { return strdup("/nonexistent-spc-dir"); }

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

/* @@BODY@@ */
static void arm(int fail_at)
{
    alloc_calls = 0;
    alloc_fail_at = fail_at;
}

static SPC_Channel_Ptr open_channel(SPC_Channel *c)
{
    c->logfile = NULL;
    last_error = 0;
    return open_noio_channel_object(c, SPCIO_USE_LOGFILE, NULL);
}

static void test_logfile_list(void)
{
    SPC_Channel a, b, c, d;
    char **old;

    /* Control: the first channel creates the list, the second extends it. */
    arm(0);
    assert(open_channel(&a) == &a && a.logfile);
    assert(alloc_calls == 1);
    assert(SPC_logfile_list[0] && strcmp(SPC_logfile_list[0], a.logfile) == 0);
    assert(SPC_logfile_list[1] == NULL);
    assert(open_channel(&b) == &b && b.logfile);
    assert(alloc_calls == 2);
    assert(strcmp(SPC_logfile_list[1], b.logfile) == 0);
    assert(SPC_logfile_list[2] == NULL);

    /*
     * The realloc fails: the channel is refused with an out-of-memory
     * error, and the list keeps its old block with both names.
     */
    old = SPC_logfile_list;
    arm(1);
    assert(open_channel(&c) == SPC_ERROR);
    assert(alloc_calls == 1);
    assert(last_error == SPC_Out_Of_Memory);
    assert(c.logfile == NULL);
    assert(SPC_logfile_list == old);
    assert(strcmp(SPC_logfile_list[0], a.logfile) == 0);
    assert(strcmp(SPC_logfile_list[1], b.logfile) == 0);
    assert(SPC_logfile_list[2] == NULL);

    /* Without injection the list grows again. */
    arm(0);
    assert(open_channel(&d) == &d && d.logfile);
    assert(strcmp(SPC_logfile_list[2], d.logfile) == 0);
    assert(SPC_logfile_list[3] == NULL);
}

static void test_logfile_list_first_block(void)
{
    SPC_Channel a;

    /* The list does not exist yet and its first block cannot be made. */
    free(SPC_logfile_list);
    SPC_logfile_list = NULL;
    arm(1);
    assert(open_channel(&a) == SPC_ERROR);
    assert(last_error == SPC_Out_Of_Memory);
    assert(a.logfile == NULL);
    assert(SPC_logfile_list == NULL);
}

#ifdef TEST_PID_LIST
static void test_pid_list(void)
{
    int slot, i;
    pid_t *old;

    /* Control: the first claim creates the list with its terminator. */
    arm(0);
    assert(reserve_pid_slot(&slot) == 1 && slot == 0);
    assert(alloc_calls == 1);
    assert(SPC_pid_list[0] == SPC_PID_RESERVED && SPC_pid_list[1] == 0);
    SPC_pid_list[0] = 100;
    assert(reserve_pid_slot(&slot) == 1 && slot == 1);
    assert(alloc_calls == 2);
    SPC_pid_list[1] = 101;
    assert(SPC_pid_list[2] == 0);

    /* The list cannot grow: it is unchanged and still valid. */
    old = SPC_pid_list;
    arm(1);
    assert(reserve_pid_slot(&slot) == 0);
    assert(alloc_calls == 1);
    assert(SPC_pid_list == old);
    assert(SPC_pid_list[0] == 100 && SPC_pid_list[1] == 101 &&
           SPC_pid_list[2] == 0);

    /* A dead entry is reused without allocating, so no failure can occur. */
    SPC_pid_list[0] = SPCD_DEAD_PROCESS;
    arm(1);
    assert(reserve_pid_slot(&slot) == 1 && slot == 0);
    assert(alloc_calls == 0);
    assert(SPC_pid_list[0] == SPC_PID_RESERVED && SPC_pid_list[1] == 101);

    /* A claimed entry is not handed out twice. */
    arm(0);
    assert(reserve_pid_slot(&slot) == 1 && slot == 2);
    assert(SPC_pid_list[3] == 0);
    for (i = 0; i < 3; i++)
        assert(SPC_pid_list[i] != SPCD_DEAD_PROCESS);

    /* The first block of a list that does not exist yet cannot be made. */
    free(SPC_pid_list);
    SPC_pid_list = NULL;
    arm(1);
    assert(reserve_pid_slot(&slot) == 0);
    assert(SPC_pid_list == NULL);
}
#endif

int main(void)
{
    test_logfile_list();
    test_logfile_list_first_block();
    puts("SPC logfile list: allocation failures leave the list valid");
#ifdef TEST_PID_LIST
    test_pid_list();
    puts("SPC pid list: allocation failures leave the list valid");
#endif
    return 0;
}
