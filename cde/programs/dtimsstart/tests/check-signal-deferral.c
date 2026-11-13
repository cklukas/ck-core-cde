/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DPR(msg)
enum { ErrSignaled = -194, ErrImsAborted = -178, ErrImsWaiting = -164,
       ErrImsConnecting = -162 };
typedef struct { int status; int wait_status; pid_t pid; } RunEnv;
static struct { RunEnv *renv; char *name; } userSel;
static const char *sig_name(int sig) { (void) sig; return "SIG"; }
#define _DtSignal(sig, handler) signal(sig, handler)

/* Recorders.  A call counts as "in signal context" when the signal that
 * triggered the handler is blocked, which is true only while its handler runs. */
static int ck_exit_calls, ck_exit_code, ck_exit_blocked;
static int ck__exit_calls, ck__exit_code;
static int ck_log_calls, ck_log_blocked;
static int ck_waitdone_calls, ck_waitdone_blocked;
static int ck_wake_calls, ck_wake_blocked;
static sigjmp_buf ck_jump;
static int ck_jump_armed;

static int ck_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGUSR1) == 1 || sigismember(&cur, SIGCHLD) == 1;
}

static void ck_leave(void)
{
    if (ck_jump_armed)
        siglongjmp(ck_jump, 1);
    fputs("exit called outside a guarded call\n", stderr);
    _exit(3);
}

[[noreturn]] static void Exit(int code)
{
    ++ck_exit_calls;
    ck_exit_code = code;
    ck_exit_blocked = ck_blocked();
    ck_leave();
    abort();
}

[[noreturn]] static void ck__exit(int code)
{
    ++ck__exit_calls;
    ck__exit_code = code;
    ck_leave();
    abort();
}

static int put_xims_log(char *fmt, void *a, void *b, void *c)
{
    (void) fmt; (void) a; (void) b; (void) c;
    ++ck_log_calls;
    if (ck_blocked())
        ck_log_blocked = 1;
    return 0;
}

static void ximsWaitDone(void)
{
    ++ck_waitdone_calls;
    if (ck_blocked())
        ck_waitdone_blocked = 1;
}

static void ximsWakeEventLoop(void)
{
    ++ck_wake_calls;
    if (ck_blocked())
        ck_wake_blocked = 1;
}

#define _exit ck__exit
/* @@BODY@@ */
#undef _exit

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    ck_exit_calls = ck_exit_code = ck_exit_blocked = 0;
    ck__exit_calls = ck__exit_code = 0;
    ck_log_calls = ck_log_blocked = 0;
    ck_waitdone_calls = ck_waitdone_blocked = 0;
    ck_wake_calls = ck_wake_blocked = 0;
    ck_jump_armed = 0;
}

static void install(int sig, void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    CHECK(sigaction(sig, &sa, nullptr) == 0);
}

static void raise_guarded(void)
{
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        raise(SIGUSR1);
    ck_jump_armed = 0;
}

/* Fork a child that exits at once and run the SIGCHLD handler for it. */
static pid_t run_child(void)
{
    sigset_t block, none;
    pid_t pid;

    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, nullptr);
    pid = fork();
    if (pid == 0)
        _Exit(0);
    sigemptyset(&none);
    sigsuspend(&none);		/* the handler runs here */
    sigprocmask(SIG_UNBLOCK, &block, nullptr);
    return pid;
}

#ifdef CK_HAS_DEFERRAL

int main(void)
{
    RunEnv renv = { ErrImsWaiting, 0, 0 };
    pid_t pid;

    install(SIGUSR1, sigExit);
    install(SIGCHLD, on_sig_chld);
    userSel.renv = &renv;
    userSel.name = (char *) "ims";

    /* The termination handler records the signal, wakes the loop, keeps errno. */
    reset_records();
    errno = 4321;
    raise_guarded();
    CHECK(errno == 4321);
    CHECK(exitSignalNumber == SIGUSR1);
    CHECK(ck_wake_calls == 1);
    CHECK(ck_exit_calls == 0 && ck__exit_calls == 0);

    /* A second signal while the exit is pending leaves at once. */
    reset_records();
    raise_guarded();
    CHECK(ck__exit_calls == 1 && ck__exit_code == 1);
    CHECK(ck_exit_calls == 0 && ck_wake_calls == 0);

    /* Exit() runs from the deferred call, outside signal context. */
    reset_records();
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        ximsHandleDeferredSignals();
    ck_jump_armed = 0;
    CHECK(ck_exit_calls == 1 && ck_exit_code == ErrSignaled && !ck_exit_blocked);
    exitSignalNumber = 0;

    /* A child that is not the one waited for is reaped and ignored. */
    reset_records();
    renv.pid = 1;
    pid = run_child();
    CHECK(ck_log_calls == 0 && ck_waitdone_calls == 0 && ck_wake_calls == 0);
    CHECK(waitpid(pid, nullptr, WNOHANG) == -1 && errno == ECHILD);
    ximsHandleDeferredSignals();
    CHECK(ck_log_calls == 0 && ck_waitdone_calls == 0);

    /* The child handler reaps and records; it reports nothing itself. */
    reset_records();
    /* Start the child ourselves so renv.pid is known before the handler runs. */
    {
        sigset_t block, none;

        sigemptyset(&block);
        sigaddset(&block, SIGCHLD);
        sigprocmask(SIG_BLOCK, &block, nullptr);
        pid = fork();
        if (pid == 0)
            _Exit(0);
        renv.pid = pid;
        sigemptyset(&none);
        sigsuspend(&none);
        sigprocmask(SIG_UNBLOCK, &block, nullptr);
    }
    CHECK(ck_wake_calls == 1);
    CHECK(ck_log_calls == 0 && ck_waitdone_calls == 0);
    CHECK(renv.status == ErrImsWaiting);

    /* The deferred call aborts the wait in normal context. */
    reset_records();
    ximsHandleDeferredSignals();
    CHECK(renv.status == ErrImsAborted && renv.wait_status == 0);
    CHECK(ck_log_calls == 1 && !ck_log_blocked);
    CHECK(ck_waitdone_calls == 1 && !ck_waitdone_blocked);

    /* Nothing pending: nothing happens. */
    reset_records();
    ximsHandleDeferredSignals();
    CHECK(ck_log_calls == 0 && ck_waitdone_calls == 0 && ck_exit_calls == 0);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("dtimsstart: the termination and child handlers only record and wake, the work runs outside them");
    return 0;
}

#else /* a revision whose handlers do the work themselves */

int main(void)
{
    RunEnv renv = { ErrImsWaiting, 0, 0 };
    sigset_t block, none;
    pid_t pid;

    install(SIGUSR1, sigExit);
    install(SIGCHLD, on_sig_chld);
    userSel.renv = &renv;
    userSel.name = (char *) "ims";

    reset_records();
    raise_guarded();
    if (ck_exit_calls) {
        fprintf(stderr, "FAIL: Exit(%d) ran in signal context (signal blocked: %d)\n",
                ck_exit_code, ck_exit_blocked);
        ++failures;
    }

    reset_records();
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, nullptr);
    pid = fork();
    if (pid == 0)
        _Exit(0);
    renv.pid = pid;
    sigemptyset(&none);
    sigsuspend(&none);
    if (ck_log_calls || ck_waitdone_calls) {
        fprintf(stderr, "FAIL: put_xims_log() x%d and ximsWaitDone() x%d ran in signal context (blocked: %d %d)\n",
                ck_log_calls, ck_waitdone_calls, ck_log_blocked, ck_waitdone_blocked);
        ++failures;
    }
    return failures ? 1 : 0;
}

#endif
