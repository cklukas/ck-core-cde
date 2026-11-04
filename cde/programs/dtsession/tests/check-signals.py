#!/usr/bin/env python3
"""Check how dtsession handles SIGTERM, from the signal to the shutdown.

The handler used to call ImmediateExit(), which sends ToolTalk messages, saves
the session, allocates memory and runs a nested X event loop, all from inside
the signal handler of a process that may be interrupted in malloc() or Xlib.
The handler now leaves a byte in a pipe that the event loop reads.

The production code is cut out of the sources and compiled, under
AddressSanitizer and UndefinedBehaviorSanitizer: StopAll, StopRequested,
CatchStopSignal and WatchStopSignal of SmMain.c and the real ImmediateExit of
SmUI.c, with stand-ins for ToolTalk, the session save and the X event loop.
ImmediateExit holds SIGTERM while it runs, so the checks cover the whole life
of a request: nothing of the shutdown runs inside the handler, repeated
signals cause one shutdown, a shutdown that is cancelled gives the signal mask
back so that the next signal starts a new one, a signal that arrives during a
shutdown is not lost and a request made while one is under way is dropped.
Every scenario runs in its own process.

DTSESSION_SOURCE names another copy of the sources to test, for example an
older revision.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
main_c = (source / "SmMain.c").read_text()
ui_c = (source / "SmUI.c").read_text()


def function(text, name, kind="static void\n"):
    match = re.search(r"^" + re.escape(kind) + re.escape(name) + r"\s*\(", text, re.M)
    assert match, name
    return text[match.start():text.index("\n}", match.start()) + 3] + "\n"


code = "static int stopPipe[2] = { -1, -1 };\n"
if "static Boolean stopping" in main_c:
    code += "static Boolean stopping = False;\n"
code += function(main_c, "StopAll") + function(main_c, "StopRequested") + function(main_c, "CatchStopSignal")
if "StopPipeFailed" in main_c:
    code = "static void StopPipeFailed(void);\n" + code + function(main_c, "StopPipeFailed") + function(main_c, "WatchStopSignal")
    install = "CatchStopSignal(); WatchStopSignal();"
else:
    install = "CatchStopSignal(); if (stopPipe[0] != -1) XtAppAddInput(0, stopPipe[0], 0, StopRequested, 0);"
code += function(ui_c, "ImmediateExit", "void\n")

harness = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Intrinsic.h>

typedef void *Tt_message;
#define TT_NOTICE 1
#define TT_SESSION 2
#define SmSaveLocal 0
#define SmSaveGlobal 1
#define DtSM_CURRENT_STATE 1
#define DtSM_HOME_STATE 2
#define DEFAULT_INTERACT_STYLE 0
#define DEFAULT_FAST 1
#define DEFAULT_GLOBAL 1
#define DEFAULT_SESSION 0
#define HOME_SESSION 1
#define CURRENT_SESSION 2
#define SM_RUNNING_LOCK "lock"
#define DtError 1
#define GETMESSAGE(set, n, s) (s)

static struct {
    Boolean bmsDead, compatMode, ExitComplete, loggingOut;
    int sessionType;
    void *display, *appCon;
} smGD;
static struct { int startState; } smSettings;
static struct { int saveYourselfTimeout; } smRes;
static struct { struct { Boolean saveComplete, shutdownCanceled; } saveState; } smXSMP;
static Boolean saveTimeout;
void ImmediateExit(int, Tt_message, Boolean);

/* what the code under test reaches */
static int notices, exited, saves, errors, fail_pipe, fail_fcntl, fail_sigaction;
static void (*during_save)(void);
static void (*input_proc)(XtPointer, int *, XtInputId *);
static int input_fd = -1;

static Tt_message tttk_message_create(void *a, int b, int c, void *d, const char *op, void *e) { (void) a; (void) b; (void) c; (void) d; (void) op; (void) e; return (Tt_message) 1; }
static void tt_message_send(Tt_message m) { (void) m; notices++; }
static void tt_message_destroy(Tt_message m) { (void) m; }
static void tt_message_reply(Tt_message m) { (void) m; }
static void SaveState(int a, int b, int c, int d, int e, int f, int g) { (void) a; (void) b; (void) c; (void) d; (void) e; (void) f; (void) g; saves++; if (during_save) during_save(); }
static void SaveTimeout(XtPointer p, XtIntervalId *i) { (void) p; (void) i; saveTimeout = True; }
static void StartEtc(Boolean exiting) { (void) exiting; }
static void _DtReleaseLock(void *d, const char *name) { (void) d; (void) name; }
static void SmExitStub(int status) { (void) status; exited++; }
static void PrintErrnoError(int type, const char *message) { (void) type; (void) message; errors++; }
#define XSync(d, b) 0
/* the Xt event loop: callbacks (input, timers) run inside it; XtAppNextEvent returns only with an X event,
   and no X event comes in these scenarios, so it would wait for ever */
static void (*timeout_proc)(XtPointer, XtIntervalId *);
static void (*pending_callback)(void);
static void fake_process_event(void)
{
    XtIntervalId id = 1;
    if (pending_callback) { void (*cb)(void) = pending_callback; pending_callback = NULL; cb(); }
    else if (timeout_proc) { void (*proc)(XtPointer, XtIntervalId *) = timeout_proc; timeout_proc = NULL; proc(NULL, &id); }
    else _exit(98);
}
static void fake_next_event(void) { _exit(99); }
#define XtAppAddTimeOut(app, ms, proc, data) (timeout_proc = (proc), (XtIntervalId) 1)
#define XtAppNextEvent(app, ev) fake_next_event()
#define XtAppProcessEvent(app, mask) fake_process_event()
#define XtIMAll 7
#define XtDispatchEvent(ev) ((void) 0)
#define SM_EXIT(status) SmExitStub(status)
#define XtAppAddInput(app, fd, mask, proc, data) fake_AddInput(fd, proc)
static void fake_AddInput(int fd, void (*proc)(XtPointer, int *, XtInputId *)) { input_fd = fd; input_proc = proc; }
static int fake_pipe(int fds[2]) { if (fail_pipe) { errno = EMFILE; return -1; } return pipe(fds); }
static int fake_fcntl(int fd, int cmd, long arg)
{
    if (fail_fcntl && cmd == F_SETFD) { errno = EBADF; return -1; }
    return fcntl(fd, cmd, arg);
}
static int fake_sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    if (fail_sigaction) { errno = EINVAL; return -1; }
    return sigaction(sig, act, old);
}
#define pipe(...) fake_pipe(__VA_ARGS__)
#define fcntl(...) fake_fcntl(__VA_ARGS__)
#define sigaction(...) fake_sigaction(__VA_ARGS__)

''' + code + r'''
#undef pipe
#undef fcntl
#undef sigaction

static void install_handler(void)
{
    ''' + install + r'''
}

/* the event loop: when the input pipe is readable, call the procedure */
static void event_loop(void)
{
    struct pollfd p = { input_fd, POLLIN, 0 };
    int source = input_fd;
    XtInputId id = 0;
    if (input_proc == NULL) return;
    while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN))
        input_proc(NULL, &source, &id);
}
static int term_blocked(void)
{
    sigset_t current;
    sigprocmask(SIG_BLOCK, NULL, &current);
    return sigismember(&current, SIGTERM);
}
static void cancelled(void) { smXSMP.saveState.shutdownCanceled = True; }

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int handler_defers(void)
{
    cancelled();
    install_handler();
    raise(SIGTERM);
    CHECK(saves == 0);             /* nothing of the shutdown runs in the handler */
    event_loop();
    CHECK(saves == 1);
    return 0;
}
static int repeated_signals(void)
{
    cancelled();
    install_handler();
    raise(SIGTERM); raise(SIGTERM); raise(SIGTERM);
    event_loop();
    CHECK(saves == 1);
    return 0;
}
static int cancelled_shutdown_restores_the_mask(void)
{
    cancelled();
    install_handler();
    raise(SIGTERM);
    event_loop();
    CHECK(saves == 1);
    CHECK(!term_blocked());          /* the signal can arrive again */
    return 0;
}
static int cancelled_then_repeated(void)
{
    cancelled();
    install_handler();
    raise(SIGTERM); event_loop();
    CHECK(saves == 1);
    raise(SIGTERM); event_loop();
    CHECK(saves == 2);             /* the second signal starts a new shutdown */
    smXSMP.saveState.shutdownCanceled = False;
    smXSMP.saveState.saveComplete = True;
    raise(SIGTERM); event_loop();
    CHECK(saves == 3 && exited == 1);                 /* and the third one is not cancelled */
    return 0;
}
static void late_cancel(void) { smXSMP.saveState.shutdownCanceled = True; }
static int cancel_that_arrives_as_a_callback_ends_the_wait(void)
{
    /* the clients' replies are input callbacks: the wait ends when one of them cancels, without an X event */
    smXSMP.saveState.shutdownCanceled = False;
    smXSMP.saveState.saveComplete = False;
    pending_callback = late_cancel;
    ImmediateExit(-1, 0, True);
    CHECK(saves == 1 && exited == 0 && !term_blocked());
    return 0;
}
static int timeout_ends_the_wait(void)
{
    smXSMP.saveState.shutdownCanceled = False;
    smXSMP.saveState.saveComplete = False;
    ImmediateExit(-1, 0, True);                          /* the timer procedure ends the wait; the session ends */
    CHECK(saves == 1 && exited == 1);
    return 0;
}
static void signal_during_save(void) { static int raised; if (!raised++) raise(SIGTERM); }
static int signal_during_a_shutdown_is_held(void)
{
    cancelled();
    install_handler();
    during_save = signal_during_save;
    raise(SIGTERM); event_loop();
    /* the signal that came in during the first shutdown is held, and starts the second one when the first has ended */
    CHECK(saves == 2);
    return 0;
}
static void nested_request(void) { ImmediateExit(-1, 0, True); }
static int request_during_a_shutdown_is_dropped(void)
{
    cancelled();
    install_handler();
    during_save = nested_request;
    raise(SIGTERM); event_loop();
    CHECK(saves == 1);
    CHECK(!term_blocked());
    return 0;
}
static int pipe_flags(void)
{
    int i;
    install_handler();
    CHECK(input_fd != -1);
    for (i = 0; i < 2; i++) {
        int fd = i ? input_fd + 1 : input_fd;
        CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
    }
    return 0;
}
static int full_pipe_never_blocks(void)
{
    int i;
    cancelled();
    install_handler();
    alarm(10);
    for (i = 0; i < 200000; i++) raise(SIGTERM);   /* far more than the pipe holds */
    alarm(0);
    event_loop();
    CHECK(saves == 1);
    return 0;
}
static int errno_kept(void)
{
    int i;
    install_handler();
    for (i = 0; i < 100000; i++) raise(SIGTERM);   /* the last writes find the pipe full */
    errno = 1234;
    raise(SIGTERM);
    CHECK(errno == 1234);
    return 0;
}
static void ignore_term(void) { struct sigaction ignore; memset(&ignore, 0, sizeof ignore); ignore.sa_handler = SIG_IGN; sigaction(SIGTERM, &ignore, NULL); }
static int set_up_failures(void)
{
    struct sigaction current;
    int variant;
    for (variant = 0; variant < 3; variant++) {
        errors = 0; input_fd = -1; input_proc = NULL;
        fail_pipe = variant == 0; fail_fcntl = variant == 1; fail_sigaction = variant == 2;
        ignore_term();                               /* started with SIGTERM ignored */
        stopPipe[0] = stopPipe[1] = -1;
        install_handler();
        CHECK(errors == 1);                          /* the failure is logged */
        CHECK(input_fd == -1 && input_proc == NULL); /* nothing is watched */
        sigaction(SIGTERM, NULL, &current);
        if (variant != 2) CHECK(current.sa_handler == SIG_DFL);   /* and the signal ends the process */
    }
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"cancel_that_arrives_as_a_callback_ends_the_wait", cancel_that_arrives_as_a_callback_ends_the_wait},
    {"timeout_ends_the_wait", timeout_ends_the_wait},
    {"handler_defers", handler_defers},
    {"repeated_signals", repeated_signals},
    {"cancelled_shutdown_restores_the_mask", cancelled_shutdown_restores_the_mask},
    {"cancelled_then_repeated", cancelled_then_repeated},
    {"signal_during_a_shutdown_is_held", signal_during_a_shutdown_is_held},
    {"request_during_a_shutdown_is_dropped", request_during_a_shutdown_is_dropped},
    {"pipe_flags", pipe_flags},
    {"full_pipe_never_blocks", full_pipe_never_blocks},
    {"errno_kept", errno_kept},
#ifdef WATCHED
    {"set_up_failures", set_up_failures},
#endif
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    if (argc < 2) { for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
with tempfile.TemporaryDirectory(prefix="dtsession-signals-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    c = work / "signals.c"
    c.write_text(harness)
    binary = work / "signals"
    define = ["-DWATCHED"] if "StopPipeFailed" in main_c else []
    build = cc + ["-std=gnu23", "-D_GNU_SOURCE", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-w"] + define + [str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr[:6000])
        sys.exit("cannot compile the signal harness")
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True, env=env).stdout.split()
    for scenario in names:
        run = subprocess.run([str(binary), scenario], capture_output=True, text=True, env=env, timeout=60)
        if run.returncode:
            failed.append(scenario)
            lines = [l for l in run.stderr.strip().splitlines() if l.strip()]
            sys.stderr.write("%s failed: %s\n" % (scenario, lines[0] if lines else "exit %d (signal %d)" % (run.returncode, -run.returncode)))
if failed:
    sys.exit("%d of %d signal scenarios failed: %s" % (len(failed), len(names), ", ".join(failed)))
print("Signals: %d scenarios passed" % len(names))
