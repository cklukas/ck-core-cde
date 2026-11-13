/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef int Boolean;
#define True 1
#define False 0
typedef void *XtPointer;
typedef void *XtAppContext;
typedef void *Widget;
typedef unsigned long XtSignalId;
typedef int DtEditorErrorCode;
typedef struct Editor {
    struct Editor *pNextPad;
    Boolean inUse;
    Widget editor;
    struct { char *fileName; Boolean saveWithNewlines; } fileStuff;
    struct { Boolean wordWrap; } xrdb;
} Editor;
static Editor pad;
static Editor *pPadList = &pad;

/* Recorders.  A call counts as "in signal context" when SIGUSR1 is blocked,
 * which is true only while the handlers under test run. */
static int ck_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGUSR1) == 1;
}

static int ck_save_calls, ck_save_blocked, ck_save_raise;
static int ck_notice_calls, ck_add_calls, ck_order;
static int ck_add_order, ck_first_sigaction_order;
static XtAppContext ck_add_app;
static int ck_exit_calls, ck_exit_code, ck_exit_blocked;
static int ck__exit_calls, ck__exit_code;
static sigjmp_buf ck_jump;
static int ck_jump_armed;

static Boolean DtEditorCheckForUnsavedChanges(Widget w) { (void) w; return True; }

static DtEditorErrorCode DtEditorSaveContentsToFile(Widget w, char *name, Boolean a, Boolean b, Boolean c)
{
    (void) w; (void) name; (void) a; (void) b; (void) c;
    ++ck_save_calls;
    if (ck_blocked())
        ck_save_blocked = 1;
    if (ck_save_raise) {
        ck_save_raise = 0;
        raise(SIGUSR2);		/* a fault inside the save */
    }
    return 0;
}

static char *AddPound(char *name)
{
    (void) name;
    return (char *) "/nonexistent-ck-dir/#name#";
}

static XtSignalId XtAppAddSignal(XtAppContext app, void (*proc)(XtPointer, XtSignalId *), XtPointer data)
{
    (void) proc; (void) data;
    ++ck_add_calls;
    ck_add_app = app;
    ck_add_order = ++ck_order;
    return 55;
}

static void XtNoticeSignal(XtSignalId id)
{
    (void) id;
    ++ck_notice_calls;
}

static int dtpadXErrorHandler(void *d, void *e) { (void) d; (void) e; return 0; }
static int dtpadXIOErrorHandler(void *d) { (void) d; return 0; }
static void XSetErrorHandler(int (*h)(void *, void *)) { (void) h; }
static void XSetIOErrorHandler(int (*h)(void *)) { (void) h; }

/* sigaction() calls made by setPanicConditions() are recorded, not installed. */
static struct { int sig; int flags; void (*handler)(int); sigset_t mask; } ck_table[64];
static int ck_table_n;

static int ck_record(int sig, const struct sigaction *act)
{
    if (ck_first_sigaction_order == 0)
        ck_first_sigaction_order = ++ck_order;
    ck_table[ck_table_n].sig = sig;
    ck_table[ck_table_n].flags = act->sa_flags;
    ck_table[ck_table_n].handler = act->sa_handler;
    ck_table[ck_table_n].mask = act->sa_mask;
    ++ck_table_n;
    return 0;
}

static void ck_leave(void)
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
    abort();
}

[[noreturn]] static void ck__exit(int code)
{
    ++ck__exit_calls;
    ck__exit_code = code;
    ck_leave();
    abort();
}

#define exit ck_exit
#define _exit ck__exit
#define sigaction(sig, act, old) ck_record(sig, act)
/* @@BODY@@ */
#undef sigaction
#undef exit
#undef _exit

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    ck_save_calls = ck_save_blocked = ck_save_raise = 0;
    ck_notice_calls = 0;
    ck_exit_calls = ck_exit_code = ck_exit_blocked = 0;
    ck__exit_calls = ck__exit_code = 0;
    ck_jump_armed = 0;
}

static void install(int sig, void (*handler)(int), int flags)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR1);
    sa.sa_flags = flags;
    sa.sa_handler = handler;
    CHECK(sigaction(sig, &sa, nullptr) == 0);
}

static void raise_guarded(int sig)
{
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        raise(sig);
    ck_jump_armed = 0;
}

#ifdef CK_HAS_FATAL_SPLIT

static int find(int sig)
{
    for (int i = 0; i < ck_table_n; i++)
        if (ck_table[i].sig == sig)
            return i;
    return -1;
}

int main(void)
{
    static const int term[] = { SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM };
    static const int fatal[] = { SIGILL, SIGABRT, SIGFPE, SIGBUS, SIGSEGV };
    char name[] = "file";

    pad.inUse = True;
    pad.editor = &pad;
    pad.fileStuff.fileName = name;

    /* The signal procedure exists before the first handler is installed. */
    setPanicConditions((XtAppContext) 7);
    CHECK(ck_add_calls == 1 && ck_add_app == (XtAppContext) 7);
    CHECK(ck_add_order < ck_first_sigaction_order);
    for (unsigned i = 0; i < sizeof term / sizeof term[0]; i++) {
        int k = find(term[i]);
        CHECK(k >= 0 && ck_table[k].handler == panicTerminateHandler);
        CHECK(k >= 0 && !(ck_table[k].flags & (SA_RESETHAND | SA_NODEFER)));
    }
    for (unsigned i = 0; i < sizeof fatal / sizeof fatal[0]; i++) {
        int k = find(fatal[i]);
        CHECK(k >= 0 && ck_table[k].handler == panicFatalHandler);
        CHECK(k >= 0 && (ck_table[k].flags & SA_RESETHAND) && (ck_table[k].flags & SA_NODEFER));
        CHECK(k >= 0 && sigismember(&ck_table[k].mask, fatal[i]) == 0);
        CHECK(k >= 0 && sigismember(&ck_table[k].mask, SIGTERM) == 1);
    }

    /* A termination signal only wakes the loop, and keeps errno. */
    install(SIGUSR1, panicTerminateHandler, 0);
    reset_records();
    errno = 4321;
    raise_guarded(SIGUSR1);
    CHECK(errno == 4321);
    CHECK(ck_notice_calls == 1);
    CHECK(ck_save_calls == 0 && ck_exit_calls == 0 && ck__exit_calls == 0);

    /* A second one while the save is pending leaves at once. */
    reset_records();
    raise_guarded(SIGUSR1);
    CHECK(ck__exit_calls == 1 && ck__exit_code == 1);
    CHECK(ck_notice_calls == 0 && ck_save_calls == 0);

    /* The save and the exit run from the signal procedure, outside the handler. */
    reset_records();
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        panicSignalProc(nullptr, nullptr);
    ck_jump_armed = 0;
    CHECK(ck_save_calls == 1 && !ck_save_blocked);
    CHECK(ck_exit_calls == 1 && ck_exit_code == 1 && !ck_exit_blocked);

    /* Fatal signal: best-effort save in the handler, then _exit(1). */
    panicSaving = 0;
    install(SIGUSR2, panicFatalHandler, SA_NODEFER);
    reset_records();
    raise_guarded(SIGUSR2);
    CHECK(ck_save_calls == 1);
    CHECK(ck__exit_calls == 1 && ck__exit_code == 1 && ck_exit_calls == 0);

    /* A fault inside the save does not recurse: the second entry leaves at once. */
    panicSaving = 0;
    reset_records();
    ck_save_raise = 1;
    raise_guarded(SIGUSR2);
    CHECK(ck_save_calls == 1);
    CHECK(ck__exit_calls == 1 && ck__exit_code == 1);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("dtpad: termination signals defer the save to the event loop, fatal signals keep a guarded best-effort save");
    return 0;
}

#else /* a revision whose handler saves the files itself */

int main(void)
{
    char name[] = "file";

    pad.inUse = True;
    pad.editor = &pad;
    pad.fileStuff.fileName = name;
    install(SIGUSR1, panicSignalHandler, 0);
    reset_records();
    raise_guarded(SIGUSR1);
    if (ck_save_calls) {
        fprintf(stderr, "FAIL: the save ran in signal context (signal blocked: %d)\n",
                ck_save_blocked);
        ++failures;
    }
    return failures ? 1 : 0;
}

#endif
