/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TRUE 1
#define FALSE 0
#define PROGNAME "TOMITA"
#define MS_tomita 29
#define CATGETS(cat, set, num, text) (text)
static int dtsearch_catd;

/* Recorders: every call that is not async-signal-safe notes whether SIGUSR1
 * is blocked, which is so only inside the signal handler. */
static int ck_unsafe_calls, ck_blocked_any;
static int ck__exit_calls, ck__exit_code;
static sigjmp_buf ck_jump;
static int ck_jump_armed;

static int ck_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGUSR1) == 1;
}

static int ck_printf(const char *fmt, ...)
{
    (void) fmt;
    ++ck_unsafe_calls;
    if (ck_blocked())
        ck_blocked_any = 1;
    return 0;
}

[[noreturn]] static void ck__exit(int code)
{
    ++ck__exit_calls;
    ck__exit_code = code;
    if (ck_jump_armed)
        siglongjmp(ck_jump, 1);
    fputs("_exit called outside a guarded call\n", stderr);
    abort();
}

#define printf ck_printf
#define _exit ck__exit
/* @@BODY@@ */
#undef printf
#undef _exit

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    ck_unsafe_calls = ck_blocked_any = 0;
    ck__exit_calls = ck__exit_code = 0;
    ck_jump_armed = 0;
}

static void raise_guarded(void)
{
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        raise(SIGUSR1);
    ck_jump_armed = 0;
}

#ifdef CK_NEW

int main(void)
{
    struct sigaction sa, query;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = flag_shutdown_signal;
    CHECK(sigaction(SIGUSR1, &sa, nullptr) == 0);

    /* The handler only records the signal and keeps errno. */
    reset_records();
    errno = 4321;
    raise_guarded();
    CHECK(errno == 4321);
    CHECK(shutdown_signal == SIGUSR1);
    CHECK(ck_unsafe_calls == 0 && ck__exit_calls == 0);

    /* A second signal before the main flow noticed the first leaves at once. */
    reset_records();
    raise_guarded();
    CHECK(ck__exit_calls == 1 && ck__exit_code == SIGUSR1);
    CHECK(ck_unsafe_calls == 0);

    /* The installer: the handler is flag_shutdown_signal and a blocked read is
     * interrupted unless the caller asks to restart. */
    catch_shutdown_signal(SIGUSR2, FALSE);
    sigaction(SIGUSR2, nullptr, &query);
    CHECK(query.sa_handler == flag_shutdown_signal);
    CHECK(!(query.sa_flags & SA_RESTART));
    catch_shutdown_signal(SIGUSR2, TRUE);
    sigaction(SIGUSR2, nullptr, &query);
    CHECK(query.sa_handler == flag_shutdown_signal);
    CHECK(query.sa_flags & SA_RESTART);
    signal(SIGUSR2, SIG_DFL);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("dtsr tomita: the handler only sets the flag, a second signal calls _exit()");
    return 0;
}

#else /* a revision whose handler prints from the signal */

int main(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = kill_delete;
    CHECK(sigaction(SIGUSR1, &sa, nullptr) == 0);
    reset_records();
    raise_guarded();
    if (ck_unsafe_calls) {
        fprintf(stderr, "FAIL: printf ran in signal context (signal blocked: %d)\n", ck_blocked_any);
        ++failures;
    }
    return failures ? 1 : 0;
}

#endif
