/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define _DtActCHILD_FAILED 7

/* The previous revision selects the handler prototype by platform; the
 * signal handler takes an int everywhere this test runs. */
#if !defined(__linux__) && !defined(__aix) && !defined(CSRG_BASED)
#define __linux__ 1
#endif

/* Recorders; blocked means the SIGUSR1 stand-in is blocked, which is true
 * only while its handler runs. */
static char *dtSvcProcIdG;
static int tmpFileCntG;
static char **tmpFilesG;
static int ck_done_calls, ck_done_code, ck_done_blocked;
static int ck_chmod_calls, ck_unlink_calls, ck_file_blocked;
static int ck__exit_calls, ck__exit_code, ck__exit_blocked;
static sigjmp_buf ck_jump;
static int ck_jump_armed;

static int ck_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGUSR1) == 1;
}

void DoneRequest(int code)
{
    ++ck_done_calls;
    ck_done_code = code;
    if (ck_blocked())
        ck_done_blocked = 1;
}

static int ck_chmod(const char *path, mode_t mode)
{
    (void) path; (void) mode;
    ++ck_chmod_calls;
    if (ck_blocked())
        ck_file_blocked = 1;
    return 0;
}

static int ck_unlink(const char *path)
{
    (void) path;
    ++ck_unlink_calls;
    if (ck_blocked())
        ck_file_blocked = 1;
    return 0;
}

[[noreturn]] static void ck__exit(int code)
{
    ++ck__exit_calls;
    ck__exit_code = code;
    ck__exit_blocked = ck_blocked();
    if (ck_jump_armed)
        siglongjmp(ck_jump, 1);
    fputs("_exit called outside a guarded call\n", stderr);
    _exit(3);
}

#define chmod ck_chmod
#define unlink ck_unlink
#define _exit ck__exit
/* @@BODY@@ */
#undef chmod
#undef unlink
#undef _exit

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    ck_done_calls = ck_done_code = ck_done_blocked = 0;
    ck_chmod_calls = ck_unlink_calls = ck_file_blocked = 0;
    ck__exit_calls = ck__exit_code = ck__exit_blocked = 0;
    ck_jump_armed = 0;
}

static void install_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = PanicSignal;
    CHECK(sigaction(SIGUSR1, &sa, nullptr) == 0);
}

static void raise_guarded(void)
{
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        raise(SIGUSR1);
    ck_jump_armed = 0;
}

#ifdef CK_HAS_PANIC_SHUTDOWN

int main(void)
{
    char *files[2] = { (char *) "a.tmp", (char *) "b.tmp" };
    char byte = 0;

    tmpFilesG = files;
    tmpFileCntG = 2;
    dtSvcProcIdG = (char *) "procid";
    CHECK(pipe(panicPipeG) == 0);
    CHECK(fcntl(panicPipeG[0], F_SETFL, O_NONBLOCK) == 0);
    install_handler();

    /* The handler only records the request, wakes select() and keeps errno. */
    reset_records();
    errno = 4321;
    raise_guarded();
    CHECK(errno == 4321);
    CHECK(panicRequestedG);
    CHECK(read(panicPipeG[0], &byte, 1) == 1 && byte == 1);
    CHECK(ck_done_calls == 0 && ck_chmod_calls == 0 && ck_unlink_calls == 0);
    CHECK(ck__exit_calls == 0);

    /* A second signal while the request is pending leaves at once. */
    reset_records();
    raise_guarded();
    CHECK(ck__exit_calls == 1 && ck__exit_code == 0);
    CHECK(ck_done_calls == 0 && ck_chmod_calls == 0);
    CHECK(read(panicPipeG[0], &byte, 1) == -1);

    /* Without the pipe the flag alone carries the request. */
    panicRequestedG = 0;
    panicPipeG[1] = -1;
    reset_records();
    raise_guarded();
    CHECK(panicRequestedG && ck__exit_calls == 0 && ck_done_calls == 0);

    /* The shutdown runs in normal context: reports to the caller, then leaves. */
    reset_records();
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        PanicShutdown();
    ck_jump_armed = 0;
    CHECK(ck_done_calls == 1 && ck_done_code == _DtActCHILD_FAILED && !ck_done_blocked);
    CHECK(ck_unlink_calls == 0 && ck_chmod_calls == 0);
    CHECK(ck__exit_calls == 1 && ck__exit_code == 0 && !ck__exit_blocked);

    /* Without a caller the temporary files are removed instead. */
    dtSvcProcIdG = nullptr;
    reset_records();
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        PanicShutdown();
    ck_jump_armed = 0;
    CHECK(ck_done_calls == 0);
    CHECK(ck_chmod_calls == 2 && ck_unlink_calls == 2 && !ck_file_blocked);
    CHECK(ck__exit_calls == 1 && !ck__exit_blocked);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("dtexec: the SIGINT handler only wakes the select loop, the shutdown runs outside it");
    return 0;
}

#else /* a revision whose handler reports to the caller itself */

int main(void)
{
    dtSvcProcIdG = (char *) "procid";
    install_handler();
    reset_records();
    raise_guarded();
    if (ck_done_calls) {
        fprintf(stderr, "FAIL: DoneRequest(%d) ran in signal context (signal blocked: %d)\n",
                ck_done_code, ck_done_blocked);
        ++failures;
    }
    return failures ? 1 : 0;
}

#endif
