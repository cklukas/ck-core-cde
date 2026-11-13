/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *XtPointer;
typedef unsigned long XtSignalId;
#define DebugF(...)

/* Recorders; the order of the cleanups is part of the contract. */
static char ck_order[16];
static int ck_blocked_any;
static int ck_notice_calls, ck_notice_id;
static int ck_exit_calls, ck_exit_code, ck_exit_blocked;
static int ck__exit_calls, ck__exit_code;
static sigjmp_buf ck_jump;
static int ck_jump_armed;

static int ck_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGUSR1) == 1;
}

static void note(char c)
{
    size_t n = strlen(ck_order);

    if (n < sizeof ck_order - 1)
        ck_order[n] = c;
    if (ck_blocked())
        ck_blocked_any = 1;
}

void _DtTermPrimPtyCleanup(void) { note('p'); }
void _DtTermPrimUtmpCleanup(void) { note('u'); }
void _DtTermPrimLogFileCleanup(void) { note('l'); }

static void XtNoticeSignal(XtSignalId id)
{
    ++ck_notice_calls;
    ck_notice_id = (int) id;
}

[[noreturn]] static void ck_leave(void)
{
    if (ck_jump_armed)
        siglongjmp(ck_jump, 1);
    fputs("exit called outside a guarded call\n", stderr);
    _exit(3);
}

[[noreturn]] static void ck_exit(int code)
{
    ++ck_exit_calls;
    ck_exit_code = code;
    ck_exit_blocked = ck_blocked();
    ck_leave();
}

[[noreturn]] static void ck__exit(int code)
{
    ++ck__exit_calls;
    ck__exit_code = code;
    ck_leave();
}

#define exit ck_exit
#define _exit ck__exit
/* @@BODY@@ */
#undef exit
#undef _exit

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    memset(ck_order, 0, sizeof ck_order);
    ck_blocked_any = 0;
    ck_notice_calls = ck_notice_id = 0;
    ck_exit_calls = ck_exit_code = ck_exit_blocked = 0;
    ck__exit_calls = ck__exit_code = 0;
    ck_jump_armed = 0;
}

static void install_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SignalHandler;
    CHECK(sigaction(SIGUSR1, &sa, nullptr) == 0);
}

static void raise_guarded(void)
{
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        raise(SIGUSR1);
    ck_jump_armed = 0;
}

#ifdef CK_HAS_SIGNAL_PROC

int main(void)
{
    install_handler();

    /* Early window, no signal id yet: the old sequence runs in the handler. */
    cleanupSignalId = 0;
    reset_records();
    raise_guarded();
    CHECK(strcmp(ck_order, "pul") == 0);
    CHECK(ck_exit_calls == 1 && ck_exit_code == 0);
    CHECK(ck_notice_calls == 0);
    CHECK(!cleanupRequested);

    /* With the signal id the handler only notifies Xt and keeps errno. */
    cleanupSignalId = 55;
    reset_records();
    errno = 4321;
    raise_guarded();
    CHECK(errno == 4321);
    CHECK(ck_notice_calls == 1 && ck_notice_id == 55);
    CHECK(ck_order[0] == 0);
    CHECK(ck_exit_calls == 0 && ck__exit_calls == 0);
    CHECK(cleanupRequested);

    /* The cleanups and exit(0) run from the signal procedure, signal unblocked. */
    reset_records();
    XtSignalId id = 55;
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        CleanupSignalProc(nullptr, &id);
    ck_jump_armed = 0;
    CHECK(strcmp(ck_order, "pul") == 0);
    CHECK(!ck_blocked_any);
    CHECK(ck_exit_calls == 1 && ck_exit_code == 0 && !ck_exit_blocked);

    /* A second signal while the request is pending leaves at once. */
    reset_records();
    raise_guarded();
    CHECK(ck__exit_calls == 1 && ck__exit_code == 0);
    CHECK(ck_exit_calls == 0);
    CHECK(ck_notice_calls == 0);
    CHECK(ck_order[0] == 0);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("dtterm main: handler defers the cleanup to the event loop, early window keeps the old sequence");
    return 0;
}

#else /* a revision whose handler does the cleanup itself */

int main(void)
{
    install_handler();
    reset_records();
    raise_guarded();
    if (ck_order[0]) {
        fprintf(stderr, "FAIL: cleanups \"%s\" ran in signal context (signal blocked: %d)\n",
                ck_order, ck_blocked_any);
        ++failures;
    }
    if (ck_exit_calls) {
        fprintf(stderr, "FAIL: exit(%d) ran in signal context (signal blocked: %d)\n",
                ck_exit_code, ck_exit_blocked);
        ++failures;
    }
    return failures ? 1 : 0;
}

#endif
