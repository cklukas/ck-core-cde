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
typedef void *XtAppContext;
typedef struct { int unused; } Display;
typedef int Boolean;
#define True 1
#define False 0
#define PointerRoot 1L
#define RevertToPointerRoot 1
#define CurrentTime 0L
#define WM_SHOW_FB_KILL 0x4
#define QUIT_MWM_ACTION 7
#define ACTIVE_PSD ((void *) 0)

static struct { int showFeedback; Display *display; } wmGD;
#define DISPLAY (wmGD.display)

/* Recorders: every call that is not async-signal-safe notes whether SIGUSR1
 * is blocked, which is so only inside the signal handler. */
static int ck_blocked_any;
static int ck_notice_calls, ck_notice_id;
static int ck_confirm_calls, ck_flush_calls, ck_quit_calls;
static int ck_focus_calls, ck_sync_calls, ck_close_calls, ck_kill_calls, ck_kill_sig;
static int ck__exit_calls, ck__exit_code;
static void (*ck_proc)(XtPointer, XtSignalId *);
static sigjmp_buf ck_jump;
static int ck_jump_armed;

static int ck_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGUSR1) == 1;
}

static void note(void)
{
    if (ck_blocked())
        ck_blocked_any = 1;
}

static void ConfirmAction(void *psd, int nbr) { (void) psd; (void) nbr; ++ck_confirm_calls; note(); }
static void XFlush(Display *d) { (void) d; ++ck_flush_calls; note(); }
static void Do_Quit_Mwm(Boolean died) { (void) died; ++ck_quit_calls; note(); }
static void XSetInputFocus(Display *d, long w, int r, long t)
{ (void) d; (void) w; (void) r; (void) t; ++ck_focus_calls; note(); }
static void XSync(Display *d, int discard) { (void) d; (void) discard; ++ck_sync_calls; note(); }
static void XCloseDisplay(Display *d) { (void) d; ++ck_close_calls; note(); }

static void XtNoticeSignal(XtSignalId id)
{
    ++ck_notice_calls;
    ck_notice_id = (int) id;
}

static XtSignalId XtAppAddSignal(XtAppContext app, void (*proc)(XtPointer, XtSignalId *), XtPointer data)
{
    (void) app; (void) data;
    ck_proc = proc;
    return 77;
}

static int ck_kill(pid_t pid, int sig)
{
    (void) pid;
    ++ck_kill_calls;
    ck_kill_sig = sig;
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

#define _exit ck__exit
#define kill ck_kill
/* @@BODY@@ */
#undef _exit
#undef kill

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    ck_blocked_any = 0;
    ck_notice_calls = ck_notice_id = 0;
    ck_confirm_calls = ck_flush_calls = ck_quit_calls = 0;
    ck_focus_calls = ck_sync_calls = ck_close_calls = ck_kill_calls = ck_kill_sig = 0;
    ck__exit_calls = ck__exit_code = 0;
    ck_jump_armed = 0;
}

static void install_handler(void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    CHECK(sigaction(SIGUSR1, &sa, nullptr) == 0);
}

/* Raises SIGUSR1 with the handler guarded against _exit(). */
static void raise_guarded(void)
{
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        raise(SIGUSR1);
    ck_jump_armed = 0;
}

#ifdef CK_NEW

/* macOS does not report SA_RESETHAND when the disposition is queried. */
#ifdef __APPLE__
#define CK_RESET_FLAGS SA_NODEFER
#else
#define CK_RESET_FLAGS (SA_RESETHAND | SA_NODEFER)
#endif

static const int fatal[] = { SIGILL, SIGFPE, SIGBUS, SIGSEGV };
static const int quitting[] = { SIGINT, SIGHUP, SIGQUIT, SIGTERM };

int main(void)
{
    install_handler(QuitWmSignalHandler);

    /* Before the signal procedure exists nothing is open: leave at once. */
    reset_records();
    raise_guarded();
    CHECK(ck__exit_calls == 1 && ck__exit_code == 0);
    CHECK(ck_notice_calls == 0 && ck_quit_calls == 0 && ck_confirm_calls == 0);

    SetupWmSignalDeferral((XtAppContext) 1);
    CHECK(ck_proc != nullptr);

    /* Afterwards the handler only notifies Xt and keeps errno. */
    reset_records();
    errno = 4321;
    raise_guarded();
    CHECK(errno == 4321);
    CHECK(ck_notice_calls == 1 && ck_notice_id == 77);
    CHECK(ck__exit_calls == 0);
    CHECK(ck_quit_calls == 0 && ck_confirm_calls == 0 && ck_flush_calls == 0 && ck_close_calls == 0);

    /* A second signal while the request is pending leaves at once. */
    reset_records();
    raise_guarded();
    CHECK(ck__exit_calls == 1 && ck__exit_code == 0);
    CHECK(ck_notice_calls == 0);

    /* The quit runs from the signal procedure, signal unblocked. */
    reset_records();
    XtSignalId id = 77;
    wmGD.showFeedback = 0;
    ck_proc(nullptr, &id);
    CHECK(ck_quit_calls == 1 && ck_confirm_calls == 0);
    wmGD.showFeedback = WM_SHOW_FB_KILL;
    wmGD.display = (Display *) &id;
    ck_proc(nullptr, &id);
    CHECK(ck_confirm_calls == 1 && ck_flush_calls == 1 && ck_quit_calls == 1);
    CHECK(!ck_blocked_any);

    /* After the procedure ran, a new signal is a new request. */
    reset_records();
    raise_guarded();
    CHECK(ck_notice_calls == 1 && ck__exit_calls == 0);

    /* The fatal handler is best effort, leaves with _exit() when entered
     * twice and ends with the default action of the signal. */
    reset_records();
    AbortWmSignalHandler(SIGUSR2);
    CHECK(ck_focus_calls == 1 && ck_sync_calls == 1 && ck_close_calls == 1);
    CHECK(ck_kill_calls == 1 && ck_kill_sig == SIGUSR2);
    signal(SIGUSR2, SIG_DFL);
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        AbortWmSignalHandler(SIGUSR2);
    ck_jump_armed = 0;
    CHECK(ck__exit_calls == 1 && ck__exit_code == 1);
    CHECK(ck_focus_calls == 1 && ck_close_calls == 1 && ck_kill_calls == 1);

    /* The installed dispositions. */
    struct sigaction saved[64];
    int all[] = { SIGINT, SIGHUP, SIGQUIT, SIGTERM, SIGCHLD, SIGILL, SIGFPE, SIGBUS, SIGSEGV };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++)
        sigaction(all[i], nullptr, &saved[i]);
    SetupWmSignalHandlers(0);
    for (size_t i = 0; i < sizeof fatal / sizeof *fatal; i++) {
        struct sigaction sa;
        sigaction(fatal[i], nullptr, &sa);
        CHECK(sa.sa_handler == AbortWmSignalHandler);
        CHECK((sa.sa_flags & CK_RESET_FLAGS) == CK_RESET_FLAGS);
    }
    for (size_t i = 0; i < sizeof quitting / sizeof *quitting; i++) {
        struct sigaction sa;
        sigaction(quitting[i], nullptr, &sa);
        CHECK(sa.sa_handler == QuitWmSignalHandler || sa.sa_handler == SIG_IGN);
    }
    for (size_t i = 0; i < sizeof all / sizeof *all; i++)
        sigaction(all[i], &saved[i], nullptr);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("dtwm: the quit signals defer to the event loop, the fatal signals are re-entrancy safe");
    return 0;
}

#else /* a revision whose handlers do their work in the signal */

#ifdef __APPLE__
#define CK_RESET_FLAGS SA_NODEFER
#else
#define CK_RESET_FLAGS (SA_RESETHAND | SA_NODEFER)
#endif

int main(void)
{
    install_handler(QuitWmSignalHandler);
    reset_records();
    raise_guarded();
    if (ck_quit_calls || ck_confirm_calls || ck_flush_calls) {
        fprintf(stderr, "FAIL: window manager shutdown ran in signal context (signal blocked: %d)\n",
                ck_blocked_any);
        ++failures;
    }

    struct sigaction sa;
    sigaction(SIGSEGV, nullptr, &sa);
    SetupWmSignalHandlers(0);
    sigaction(SIGSEGV, nullptr, &sa);
    if ((sa.sa_flags & CK_RESET_FLAGS) != CK_RESET_FLAGS) {
        fprintf(stderr, "FAIL: the fatal handler is not installed with SA_RESETHAND and SA_NODEFER\n");
        ++failures;
    }
    signal(SIGSEGV, SIG_DFL);
    return failures ? 1 : 0;
}

#endif
