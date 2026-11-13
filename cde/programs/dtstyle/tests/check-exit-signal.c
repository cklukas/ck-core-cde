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

/* Recorders for the calls the handler and the signal procedure make. */
static int ck_exitbtn_calls, ck_exitbtn_blocked;
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

typedef void *Widget;
[[noreturn]] static void ck_exit(int code);

/* The Motif exit callback; it ends in exit(). */
static void activateCB_exitBtn(Widget w, XtPointer client, XtPointer call)
{
    (void) w; (void) client; (void) call;
    ++ck_exitbtn_calls;
    if (ck_blocked())
        ck_exitbtn_blocked = 1;
    ck_exit(0);
}

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

#define _exit ck__exit
/* @@BODY@@ */
#undef _exit

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    ck_exitbtn_calls = ck_exitbtn_blocked = 0;
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
    sa.sa_handler = ExitSignalHandler;
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

    /* The handler only notifies Xt and keeps errno. */
    exitSignalId = 55;
    reset_records();
    errno = 4321;
    raise_guarded();
    CHECK(errno == 4321);
    CHECK(ck_notice_calls == 1 && ck_notice_id == 55);
    CHECK(ck_exitbtn_calls == 0);
    CHECK(ck_exit_calls == 0 && ck__exit_calls == 0);
    CHECK(exitRequested);

    /* The exit callback runs from the procedure, signal unblocked. */
    reset_records();
    XtSignalId id = 55;
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        ExitSignalProc(nullptr, &id);
    ck_jump_armed = 0;
    CHECK(ck_exitbtn_calls == 1 && !ck_exitbtn_blocked);
    CHECK(ck_exit_calls == 1 && ck_exit_code == 0 && !ck_exit_blocked);

    /* A second signal while the request is pending leaves at once. */
    reset_records();
    raise_guarded();
    CHECK(ck__exit_calls == 1 && ck__exit_code == 0);
    CHECK(ck_exit_calls == 0 && ck_exitbtn_calls == 0);
    CHECK(ck_notice_calls == 0);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("dtstyle: INT and TERM handler defers the exit callback to the event loop");
    return 0;
}

#else /* a revision whose handler runs the exit callback itself */

int main(void)
{
    install_handler();
    reset_records();
    raise_guarded();
    if (ck_exitbtn_calls) {
        fprintf(stderr, "FAIL: activateCB_exitBtn() ran in signal context (signal blocked: %d)\n",
                ck_exitbtn_blocked);
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
