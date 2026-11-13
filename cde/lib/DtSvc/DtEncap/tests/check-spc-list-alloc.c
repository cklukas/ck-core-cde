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
#include <dirent.h>
#include <limits.h>
#include <unistd.h>

typedef char *XeString;
typedef struct { char *logfile; } SPC_Channel, *SPC_Channel_Ptr;
#define SPC_ERROR 0
#define SPC_Out_Of_Memory 100
#define SPC_Cannot_Open_Log 149
#define SPCIO_USE_LOGFILE 1
#define IS_SPCIO_USE_LOGFILE(a) ((a) & SPCIO_USE_LOGFILE)
#define SPCD_DEAD_PROCESS (-1)
#define call_parent_method(channel, func, args, result) ((result) = (channel))
#define _DtSvcProcessLock()
#define _DtSvcProcessUnlock()

char **SPC_logfile_list = nullptr;
pid_t *SPC_pid_list = nullptr;
/* The directory of the logfiles: a real one, as the files are created. */
static char ck_dir[PATH_MAX];
static char *SPCD_Authentication_Dir = ck_dir;
static int last_error;
static void SPC_Error(int code, ...) { last_error = code; }
static char *get_tmp_dir(void) { return strdup(ck_dir); }

static int alloc_calls;
static int alloc_fail_at;	/* 1-based call number that fails; 0 = never */

static void *ck_malloc(size_t n)
{
    ++alloc_calls;
    if (alloc_fail_at && alloc_calls == alloc_fail_at)
        return nullptr;
    return malloc(n);
}

static void *ck_realloc(void *p, size_t n)
{
    ++alloc_calls;
    if (alloc_fail_at && alloc_calls == alloc_fail_at)
        return nullptr;		/* the old block stays valid, as in libc */
    return realloc(p, n);
}

static int strdup_fail;		/* n > 0: the nth strdup from now on fails */

static char *ck_strdup(const char *s)
{
    if (strdup_fail && --strdup_fail == 0)
        return nullptr;
    return strdup(s);
}
#define malloc ck_malloc
#define realloc ck_realloc
#define strdup ck_strdup

/* @@BODY@@ */
static void arm(int fail_at)
{
    alloc_calls = 0;
    alloc_fail_at = fail_at;
}

static SPC_Channel_Ptr open_channel(SPC_Channel *c)
{
    c->logfile = nullptr;
    last_error = 0;
    return open_noio_channel_object(c, SPCIO_USE_LOGFILE, nullptr);
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
    assert(SPC_logfile_list[1] == nullptr);
    assert(open_channel(&b) == &b && b.logfile);
    assert(alloc_calls == 2);
    assert(strcmp(SPC_logfile_list[1], b.logfile) == 0);
    assert(SPC_logfile_list[2] == nullptr);

    /*
     * The realloc fails: the channel is refused with an out-of-memory
     * error, and the list keeps its old block with both names.
     */
    old = SPC_logfile_list;
    arm(1);
    assert(open_channel(&c) == SPC_ERROR);
    assert(alloc_calls == 1);
    assert(last_error == SPC_Out_Of_Memory);
    assert(c.logfile == nullptr);
    assert(SPC_logfile_list == old);
    assert(strcmp(SPC_logfile_list[0], a.logfile) == 0);
    assert(strcmp(SPC_logfile_list[1], b.logfile) == 0);
    assert(SPC_logfile_list[2] == nullptr);

    /* Without injection the list grows again. */
    arm(0);
    assert(open_channel(&d) == &d && d.logfile);
    assert(strcmp(SPC_logfile_list[2], d.logfile) == 0);
    assert(SPC_logfile_list[3] == nullptr);
}

static int count_files(void)
{
    DIR *d = opendir(ck_dir);
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.')
            n++;
    if (d)
        closedir(d);
    return n;
}

/*
 * The copy of the logfile name for the list cannot be made: the channel is
 * refused, the file is removed again, and the list stays terminated with the
 * names it had.
 */
static void test_logfile_name_copy(void)
{
    SPC_Channel e, f;
    int files;

    arm(0);
    assert(open_channel(&e) == &e && e.logfile);
    files = count_files();
    arm(0);
    strdup_fail = 2;		/* the first copy is the file name itself */
    assert(open_channel(&f) == SPC_ERROR);
    assert(strdup_fail == 0);
    assert(last_error == SPC_Out_Of_Memory);
    assert(f.logfile == nullptr);
    assert(count_files() == files);
    assert(SPC_logfile_list[0] && SPC_logfile_list[1] && SPC_logfile_list[2]);
    assert(strcmp(SPC_logfile_list[3], e.logfile) == 0);
    assert(SPC_logfile_list[4] == nullptr);
}

static void test_logfile_list_first_block(void)
{
    SPC_Channel a;

    /* The list does not exist yet and its first block cannot be made. */
    free(SPC_logfile_list);
    SPC_logfile_list = nullptr;
    arm(1);
    assert(open_channel(&a) == SPC_ERROR);
    assert(last_error == SPC_Out_Of_Memory);
    assert(a.logfile == nullptr);
    assert(SPC_logfile_list == nullptr);
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
    SPC_pid_list = nullptr;
    arm(1);
    assert(reserve_pid_slot(&slot) == 0);
    assert(SPC_pid_list == nullptr);
}
#endif

static void remove_dir(void)
{
    DIR *d = opendir(ck_dir);
    struct dirent *e;
    char path[PATH_MAX];

    while (d && (e = readdir(d))) {
        snprintf(path, sizeof path, "%s/%s", ck_dir, e->d_name);
        unlink(path);
    }
    if (d)
        closedir(d);
    rmdir(ck_dir);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(ck_dir, sizeof ck_dir, "%s/list-XXXXXX", argv[1]);
    assert(mkdtemp(ck_dir));
    test_logfile_list();
    test_logfile_name_copy();
    test_logfile_list_first_block();
    puts("SPC logfile list: allocation failures leave the list valid");
#ifdef TEST_PID_LIST
    test_pid_list();
    puts("SPC pid list: allocation failures leave the list valid");
#endif
    remove_dir();
    return 0;
}
