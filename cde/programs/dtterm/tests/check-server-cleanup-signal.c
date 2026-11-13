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
typedef unsigned long XtIntervalId;
typedef unsigned long XtSignalId;
typedef void *XtAppContext;
typedef struct ck_widget *Widget;
typedef void (*XtTimerCallbackProc)(XtPointer, XtIntervalId *);
typedef unsigned char Boolean;
#define True 1
#define False 0

static struct ck_widget { int x; } ck_top;
static Widget refWidget = &ck_top;

/* Recorders. */
static int ck_timeout_calls, ck_timeout_blocked;
static int ck_notice_calls, ck_notice_id;
static int ck_nice_calls, ck_nice_blocked;
static XtPointer ck_nice_data;
static int ck_exit_calls, ck_exit_code, ck_exit_blocked;
static int ck__exit_calls, ck__exit_code, ck__exit_blocked;
static sigjmp_buf ck_jump;
static int ck_jump_armed;

static int ck_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGUSR1) == 1;
}

static XtAppContext XtWidgetToApplicationContext(Widget w)
{
    (void) w;
    return nullptr;
}

static XtIntervalId XtAppAddTimeOut(XtAppContext app, unsigned long interval,
                                    XtTimerCallbackProc proc, XtPointer data)
{
    (void) app; (void) interval; (void) proc; (void) data;
    ++ck_timeout_calls;
    ck_timeout_blocked = ck_blocked();
    return 1;
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
    ck__exit_blocked = ck_blocked();
    ck_leave();
}

/* The real NiceCleanUp() is not extracted; it ends in exit(1). */
static void NiceCleanUp(XtPointer clientData, XtIntervalId *id)
{
    (void) id;
    ++ck_nice_calls;
    ck_nice_data = clientData;
    ck_nice_blocked = ck_blocked();
    ck_exit(1);
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
    ck_timeout_calls = ck_timeout_blocked = 0;
    ck_notice_calls = ck_notice_id = 0;
    ck_nice_calls = ck_nice_blocked = 0;
    ck_nice_data = nullptr;
    ck_exit_calls = ck_exit_code = ck_exit_blocked = 0;
    ck__exit_calls = ck__exit_code = ck__exit_blocked = 0;
    ck_jump_armed = 0;
}

static void install_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = CleanUp;
    CHECK(sigaction(SIGUSR1, &sa, nullptr) == 0);
}

/* raise() the signal; a guarded exit leaves by a jump. */
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
    cleanUpSignalId = 77;

    /* The first signal only notifies Xt and keeps errno. */
    reset_records();
    errno = 4321;
    raise_guarded();
    CHECK(errno == 4321);
    CHECK(ck_notice_calls == 1 && ck_notice_id == 77);
    CHECK(ck_timeout_calls == 0);
    CHECK(ck_nice_calls == 0);
    CHECK(ck_exit_calls == 0 && ck__exit_calls == 0);
    CHECK(cleanUpRequested);

    /* The cleanup runs later from the signal procedure, outside signal context. */
    reset_records();
    XtSignalId id = 77;
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        CleanUpSignalProc((XtPointer) refWidget, &id);
    ck_jump_armed = 0;
    CHECK(ck_nice_calls == 1);
    CHECK(ck_nice_data == (XtPointer) refWidget);
    CHECK(!ck_nice_blocked);
    CHECK(ck_exit_calls == 1 && ck_exit_code == 1 && !ck_exit_blocked);

    /* A second signal while the request is pending leaves at once. */
    reset_records();
    raise_guarded();
    CHECK(ck__exit_calls == 1 && ck__exit_code == 1);
    CHECK(ck__exit_blocked);
    CHECK(ck_exit_calls == 0);
    CHECK(ck_notice_calls == 0);
    CHECK(ck_timeout_calls == 0);
    CHECK(ck_nice_calls == 0);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("dtterm server: handler only calls XtNoticeSignal, cleanup runs from the signal procedure");
    return 0;
}

#else /* a revision whose handler does the work itself */

int main(void)
{
    install_handler();
    reset_records();
    raise_guarded();
    if (ck_timeout_calls) {
        fprintf(stderr, "FAIL: XtAppAddTimeOut ran in signal context (signal blocked: %d)\n",
                ck_timeout_blocked);
        ++failures;
    }
    reset_records();
    raise_guarded();	/* the second signal */
    if (ck_exit_calls) {
        fprintf(stderr, "FAIL: exit(%d) ran in signal context (signal blocked: %d)\n",
                ck_exit_code, ck_exit_blocked);
        ++failures;
    }
    return failures ? 1 : 0;
}

#endif
