#!/usr/bin/env python3
"""Signals that reach the session process of the login manager.

The session process waits for the greeter, for the greeter to end, for the user's session and
for the X server, and the manager asks it to stop or to start again with SIGTERM and SIGHUP.  The
waits and the signal handling of session.c (AskGreeter, AbortClient and what they use) and of
server.c (the pause for the server) are cut out of the sources and run with real signals, pipes
and child processes under ASan and UBSan.  A handler may only leave a note: it must not call
what the waiting code calls and must not jump out of the code it interrupted, a request that
arrives before a wait begins must not be lost, and the code that was waiting does what the
request asks.  Every scenario runs in a process of its own.  Set DTLOGIN_SOURCE to test another
copy of the sources.
"""
import re
from scenarios import block, finish, read, run_all, until

session = read("session.c")
server = read("server.c")
new = "NoteSignal" in session

signal_start = "/*\n * The signals that reach the session process only leave a note."
if new:
    session_signals = until(session, signal_start, "#if defined(__STDC__)\nstatic int\nFileNameCompare")
    session_unit_extra = ""
else:
    # the sources from before the change: handlers that jump or do the work themselves
    session_signals = (until(session, "static sigjmp_buf\tabortSession;", "#if defined(__STDC__)\nstatic int\nFileNameCompare")
                       + "static int greeterPid = 0;\nstatic struct display *greeter_d = NULL;\n"
                       + block(session, "static SIGVAL\ncatchHUP(int arg)")
                       + "static sigjmp_buf\ttenaciousClient;\n"
                       + block(session, "static SIGVAL\nwaitAbort( int arg )"))
abort_client = block(session, "#if defined(SYSV) || defined(SVR4)\n# include\t<ctype.h>", "\n}\n")
abort_client = abort_client[:abort_client.index("static int \nAbortClient")] + block(session, "static int \nAbortClient( int pid )")
if new:
    abort_client = abort_client.replace("#define ABORT_WAIT\t10", "#define ABORT_WAIT\t1")
else:
    abort_client = abort_client.replace("(unsigned) 10", "(unsigned) 1")
ask_greeter = block(session, "static int\nAskGreeter(")
ask_greeter = ask_greeter.replace("RequestHeader *preqhdr", "RequestHeader *preqhdr")

if "receivedUsr1;\t/* SIGUSR1" in server:
    server_head = until(server, "static volatile sig_atomic_t receivedUsr1", "static const char * \n_SysErrorMsg")
else:
    server_head = ("static int receivedUsr1;\n"
                   + until(server, "static SIGVAL\nCatchUsr1( int arg )", "static const char * \n_SysErrorMsg"))
pause_start = "/*\n * sleep for t seconds, return 1 if the server is dead when"
server_pause = (until(server, pause_start, "/*\n * this code is complicated by some TCP failings"))
if not new:
    server_pause = "static SIGVAL serverPauseAbort( int arg ) ;\nstatic SIGVAL serverPauseUsr1( int arg ) ;\n" + server_pause

harness = r'''
#define _GNU_SOURCE
#include "dm.h"
#include "vgproto.h"
#include "vgmsg.h"
#include <setjmp.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/time.h>

extern int violations, session_exits, exit_status_seen, response[2], request[2];
int in_handler(void);
void install_session_handlers(void);
int session_noted(int *term, int *hup);
int test_ask(int *jumped);
int test_abort(int pid);
int test_await(int pid, unsigned seconds, int notice, int *status);
int test_pause(unsigned seconds, int server_pid, int *jumped);
void test_server_handlers(void);
int test_usr1(void);
'''

session_unit = r'''
#include "harness.h"
#include <fcntl.h>
#include <sys/select.h>
int response[2], request[2];
struct display *TestDisplay;
static void SessionExit(struct display *d, int status);
static int AbortClient(int pid);
''' + session_signals + abort_client + r'''
static void TellGreeter(RequestHeader *phdr) { (void)phdr; }
''' + ask_greeter + (r'''
void install_session_handlers(void)
{
    CatchSignal(SIGTERM, NoteSignal, NULL);
    CatchSignal(SIGHUP, NoteSignal, NULL);
}
int session_noted(int *term, int *hup) { *term = termRequested; *hup = hupRequested; return 1; }
int test_ask(int *jumped)
{
    char buffer[REQUEST_LIM_MAXLEN];
    *jumped = 0;
    return AskGreeter(NULL, buffer, sizeof buffer);
}
int test_await(int pid, unsigned seconds, int notice, int *status) { return AwaitChild(pid, seconds, notice, status); }
''' if new else r'''
void install_session_handlers(void)
{
    greeter_d = TestDisplay;
    signal(SIGTERM, catchTerm);
    signal(SIGHUP, catchHUP);
}
int session_noted(int *term, int *hup) { *term = *hup = 0; return 0; }
int test_ask(int *jumped)
{
    char buffer[REQUEST_LIM_MAXLEN];
    *jumped = 0;
    if (sigsetjmp(abortSession, 1)) { *jumped = 1; return -1; }
    return AskGreeter(NULL, buffer, sizeof buffer);
}
int test_await(int pid, unsigned seconds, int notice, int *status) { (void)pid; (void)seconds; (void)notice; (void)status; return -99; }
''') + r'''
int test_abort(int pid) { return AbortClient(pid); }
'''

server_unit = r'''
#include "harness.h"
#include <errno.h>
''' + server_head + r'''
''' + server_pause + (r'''
void test_server_handlers(void) { CatchSignal(SIGUSR1, CatchUsr1, NULL); }
int test_usr1(void) { return receivedUsr1; }
int test_pause(unsigned seconds, int server_pid, int *jumped) { *jumped = 0; return serverPause(seconds, server_pid); }
''' if new else r'''
void test_server_handlers(void) { signal(SIGUSR1, CatchUsr1); }
int test_usr1(void) { return receivedUsr1; }
static sigjmp_buf *outer;
int test_pause(unsigned seconds, int server_pid, int *jumped) { *jumped = 0; return serverPause(seconds, server_pid); }
''')

stubs = r'''
#define _GNU_SOURCE
#include "harness.h"
#include <stdarg.h>

int violations, session_exits, exit_status_seen, debugLevel;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

/* a handler runs with its own signal blocked and the others not; a waiter blocks several of them */
int in_handler(void)
{
    sigset_t current;
    int blocked = 0;
    sigprocmask(SIG_BLOCK, NULL, &current);
    blocked += sigismember(&current, SIGTERM) == 1;
    blocked += sigismember(&current, SIGHUP) == 1;
    blocked += sigismember(&current, SIGUSR1) == 1;
    blocked += sigismember(&current, SIGALRM) == 1;
    return blocked == 1;
}
void Debug(char *fmt, ...) { (void)fmt; if (in_handler()) ++violations; }
void LogError(unsigned char *fmt, ...) { (void)fmt; if (in_handler()) ++violations; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
void SessionExit(struct display *d, int status) { (void)d; exit_status_seen = status; ++session_exits; if (in_handler()) ++violations; }

static void Watchdog(int sig)
{
    static const char m[] = "FAIL blocked for more than the time allowed\n";
    (void)sig;
    if (write(2, m, sizeof m - 1)) { }
    _exit(1);
}
static void Arm(unsigned seconds) { signal(SIGALRM, Watchdog); alarm(seconds); }

/* a helper process that sends SIGNAL to its parent after MS milliseconds */
static pid_t Later(int signal_number, int ms)
{
    pid_t pid = fork();
    if (pid == 0) { usleep(ms * 1000); kill(getppid(), signal_number); _exit(0); }
    CHECK(pid > 0);
    return pid;
}

static pid_t Child(const char *behaviour)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        setpgid(0, 0);
        if (!strcmp(behaviour, "exit")) _exit(7);
        if (!strcmp(behaviour, "sleep")) { sleep(30); _exit(0); }
        if (!strcmp(behaviour, "short")) { usleep(200000); _exit(3); }
        if (!strcmp(behaviour, "stubborn")) { signal(SIGTERM, SIG_IGN); for (;;) sleep(1); }
        _exit(0);
    }
    setpgid(pid, pid);
    return pid;
}

static void Reset(void)
{
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    violations = session_exits = 0;
    CHECK(pipe(response) == 0);
    install_session_handlers();
}

static void TermWhileWaiting(void)
{
    int jumped, term, hup, answer;

    Reset();
    Arm(8);
    Later(SIGTERM, 150);
    answer = test_ask(&jumped);
    /* the request ends the wait for the greeter; the handler left a note and did nothing else */
    CHECK(!jumped);
    CHECK(answer == 0);
    session_noted(&term, &hup);
    CHECK(term == 1 && hup == 0);
    CHECK(violations == 0 && session_exits == 0);
}

static void HupWhileWaiting(void)
{
    int jumped, term, hup, answer;

    Reset();
    Arm(8);
    Later(SIGHUP, 150);
    answer = test_ask(&jumped);
    CHECK(!jumped && answer == 0);
    session_noted(&term, &hup);
    CHECK(term == 0 && hup == 1);
    /* ending the display is for the code that was waiting, not for the handler */
    CHECK(violations == 0 && session_exits == 0);
}

static void RequestBeforeWait(void)
{
    int jumped, term, hup;

    Reset();
    Arm(8);
    raise(SIGTERM);                                 /* arrives while nothing waits */
    CHECK(violations == 0);
    CHECK(test_ask(&jumped) == 0 && !jumped);       /* the wait does not begin: nothing is lost */
    session_noted(&term, &hup);
    CHECK(term == 1);
}

static void GreeterAnswers(void)
{
    int jumped;
    static const unsigned char message[4] = { REQUEST_OP_EXIT, 0, 4, 0 };

    Reset();
    Arm(8);
    CHECK(write(response[1], message, sizeof message) == (ssize_t)sizeof message);
    CHECK(test_ask(&jumped) == 4);
}

static void AbortEndsTheGroup(void)
{
    int status;
    pid_t pid;

    Reset();
    Arm(15);
    /* the request that led to the abort is still pending: it must not cut the wait short */
    raise(SIGTERM);
    pid = Child("sleep");
    CHECK(test_abort(pid) == 1);
    CHECK(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);     /* reaped by the abort */
}

static void AbortStubbornClient(void)
{
    int status;
    pid_t pid;

    Reset();
    Arm(15);
    pid = Child("stubborn");
    usleep(100000);
    /* a process group that ignores SIGTERM is killed when the time is over */
    CHECK(test_abort(pid) == 1);
    CHECK(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
}

static void AwaitTimeout(void)
{
    int status = 0, result;
    pid_t pid;

    Reset();
    Arm(15);
    pid = Child("sleep");
    result = test_await(pid, 1, 0, &status);
    CHECK(result == 0);                               /* the time is over, the child is still there */
    CHECK(kill(pid, 0) == 0);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static void AwaitStatus(void)
{
    int status = 0, result;
    pid_t pid, other;

    Reset();
    Arm(15);
    other = Child("exit");
    pid = Child("short");
    result = test_await(pid, 0, 0, &status);
    CHECK(result == 1 && WIFEXITED(status) && WEXITSTATUS(status) == 3);
    /* the other child that ended meanwhile was reaped as well */
    CHECK(waitpid(other, &status, WNOHANG) == -1 && errno == ECHILD);
}

static void AwaitRequest(void)
{
    int status = 0, result;
    pid_t pid;

    Reset();
    Arm(15);
    pid = Child("sleep");
    Later(SIGTERM, 150);
    result = test_await(pid, 0, 1, &status);
    CHECK(result == -1);                               /* the request ends the wait, the child is left */
    CHECK(violations == 0 && kill(pid, 0) == 0);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static void AwaitRequestPending(void)
{
    int status = 0;
    pid_t pid;

    Reset();
    Arm(15);
    pid = Child("sleep");
    raise(SIGHUP);                                      /* before the wait begins */
    CHECK(test_await(pid, 0, 1, &status) == -1);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static void AwaitNoChild(void)
{
    int status = 0;

    Reset();
    Arm(8);
    CHECK(test_await(99999, 0, 0, &status) == -2);      /* no process to wait for: no endless wait */
}

static void ServerReady(void)
{
    int jumped;
    pid_t server;

    Reset();
    Arm(10);
    test_server_handlers();
    server = Child("sleep");
    Later(SIGUSR1, 150);
    CHECK(test_pause(8, server, &jumped) == 0);
    CHECK(!jumped && test_usr1() == 1 && violations == 0);
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

static void ServerReadyBefore(void)
{
    int jumped;
    pid_t server;

    Reset();
    Arm(10);
    test_server_handlers();
    server = Child("sleep");
    raise(SIGUSR1);                                    /* the server was quick */
    CHECK(violations == 0 && test_usr1() == 1);
    CHECK(test_pause(8, server, &jumped) == 0);
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

static void ServerDies(void)
{
    int jumped;
    pid_t server;

    Reset();
    Arm(10);
    test_server_handlers();
    server = Child("exit");
    CHECK(test_pause(8, server, &jumped) == 1);
}

static void ServerSlow(void)
{
    int jumped;
    pid_t server;

    Reset();
    Arm(10);
    test_server_handlers();
    server = Child("sleep");
    CHECK(test_pause(1, server, &jumped) == 0);        /* no word from the server: the time is over */
    CHECK(violations == 0);
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"term-while-waiting", TermWhileWaiting},
    {"hup-while-waiting", HupWhileWaiting},
    {"request-before-wait", RequestBeforeWait},
    {"greeter-answers", GreeterAnswers},
    {"abort-ends-the-group", AbortEndsTheGroup},
    {"abort-stubborn-client", AbortStubbornClient},
    {"await-timeout", AwaitTimeout},
    {"await-status", AwaitStatus},
    {"await-request", AwaitRequest},
    {"await-request-pending", AwaitRequestPending},
    {"await-no-child", AwaitNoChild},
    {"server-ready", ServerReady},
    {"server-ready-before", ServerReadyBefore},
    {"server-dies", ServerDies},
    {"server-slow", ServerSlow},
};

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; ++i)
        if (strcmp(argv[1], scenarios[i].name) == 0) { scenarios[i].run(); puts("ok"); return 0; }
    fprintf(stderr, "FAIL unknown scenario %s\n", argv[1]);
    return 2;
}
'''

names = ["term-while-waiting", "hup-while-waiting", "request-before-wait", "greeter-answers",
         "abort-ends-the-group", "abort-stubborn-client", "await-timeout", "await-status", "await-request",
         "await-request-pending", "await-no-child", "server-ready", "server-ready-before", "server-dies",
         "server-slow"]
failed = run_all("login-session-signals-",
                 {"harness.h": harness, "session_unit.c": session_unit, "server_unit.c": server_unit, "stubs.c": stubs},
                 [("session_unit", ["-w"]), ("server_unit", ["-w"]), ("stubs", ["-Wall", "-Wextra", "-Wno-unused-parameter"])],
                 [], names)
finish(failed, "Session signals", "greeter waits, aborts of clients and the pause for the server")
