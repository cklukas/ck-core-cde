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
#define SIGVAL void
#define NOTIFY_ABORT 1

static int ck_notice_calls, ck_notice_id;
static int ck_cleanup_calls, ck_cleanup_code, ck_cleanup_blocked;
static int ck_write_calls, ck_write_blocked;
static int ck__exit_calls, ck__exit_code;
static sigjmp_buf ck_jump;
static int ck_jump_armed;

static int ck_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGTERM) == 1;
}

static void XtNoticeSignal(XtSignalId id)
{
    ++ck_notice_calls;
    ck_notice_id = (int) id;
    errno = 0; /* a toolkit call may clobber errno; the handler must restore it */
}

[[noreturn]] static void ck_leave(void)
{
    if (ck_jump_armed)
        siglongjmp(ck_jump, 1);
    fputs("exit called outside a guarded call\n", stderr);
    _exit(3);
}

static void CleanupAndExit(void *display, int code)
{
    (void) display;
    ++ck_cleanup_calls;
    ck_cleanup_code = code;
    ck_cleanup_blocked = ck_blocked();
    ck_leave();
}

static ssize_t ck_write(int fd, const void *buf, size_t n)
{
    (void) fd;
    (void) buf;
    ++ck_write_calls;
    ck_write_blocked = ck_blocked();
    return (ssize_t) n;
}

static void ck__exit(int code)
{
    ++ck__exit_calls;
    ck__exit_code = code;
    ck_leave();
}

#define write ck_write
#define _exit ck__exit
/* @@BODY@@ */
#undef write
#undef _exit

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    ck_notice_calls = ck_notice_id = 0;
    ck_cleanup_calls = ck_cleanup_code = ck_cleanup_blocked = 0;
    ck_write_calls = ck_write_blocked = 0;
    ck__exit_calls = ck__exit_code = 0;
    ck_jump_armed = 0;
}

static void install_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = Terminate;
    CHECK(sigaction(SIGTERM, &sa, nullptr) == 0);
}

static void raise_guarded(void)
{
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        raise(SIGTERM);
    ck_jump_armed = 0;
}

#ifdef CK_HAS_SIGNAL_PROC

int main(void)
{
    install_handler();

    /* Early window, no signal id yet: the old sequence runs in the handler. */
    terminateSignalId = 0;
    reset_records();
    raise_guarded();
    CHECK(ck_write_calls == 1);
    CHECK(ck_cleanup_calls == 1 && ck_cleanup_code == NOTIFY_ABORT);
    CHECK(ck_notice_calls == 0);
    CHECK(!terminateRequested);

    /* With the signal id the handler only notifies Xt and keeps errno. */
    terminateSignalId = 77;
    reset_records();
    errno = 4321;
    raise_guarded();
    CHECK(errno == 4321);
    CHECK(ck_notice_calls == 1 && ck_notice_id == 77);
    CHECK(ck_write_calls == 0 && ck_cleanup_calls == 0 && ck__exit_calls == 0);
    CHECK(terminateRequested);

    /* The cleanup runs from the signal procedure, with the signal unblocked. */
    reset_records();
    XtSignalId id = 77;
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        TerminateSignalProc(nullptr, &id);
    ck_jump_armed = 0;
    CHECK(ck_cleanup_calls == 1 && ck_cleanup_code == NOTIFY_ABORT && !ck_cleanup_blocked);

    /* A second signal while the request is pending leaves at once. */
    reset_records();
    raise_guarded();
    CHECK(ck__exit_calls == 1 && ck__exit_code == NOTIFY_ABORT);
    CHECK(ck_cleanup_calls == 0 && ck_notice_calls == 0 && ck_write_calls == 0);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("dtlogin Terminate: handler defers the cleanup to the event loop, early window keeps the old sequence");
    return 0;
}

#else /* a revision whose handler does the cleanup itself */

int main(void)
{
    install_handler();
    reset_records();
    raise_guarded();
    if (ck_cleanup_calls) {
        fprintf(stderr, "FAIL: CleanupAndExit(%d) ran in signal context (signal blocked: %d)\n",
                ck_cleanup_code, ck_cleanup_blocked);
        ++failures;
    }
    return failures ? 1 : 0;
}

#endif
