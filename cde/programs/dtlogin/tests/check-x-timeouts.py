#!/usr/bin/env python3
"""The waits of the login manager for the X server and the scan of the locale directory, without timer signals.

Opening the display (WaitForServer), the pseudo reset of the server (pseudoReset), the ping of the
server and the scan of the NLS directory (MakeLangList) used SIGALRM and jumped out of what they were
doing when the time was up.  They now use a thread with a time limit (BoundedCall), a look at the
descriptor, and a deadline between directory entries.  The production code of util.c, server.c and
reset.c is cut out and run under ASan and UBSan with an X library whose requests do not answer.
Every scenario runs in a process of its own.  Set DTLOGIN_SOURCE to test another copy of the sources.
"""
from scenarios import block, finish, read, run_all, until

util = read("util.c")
server = read("server.c")
reset = read("reset.c")
new = "BoundedCall" in util

bounded = (until(util, "struct bounded_call {", "#if !defined (ENABLE_DYNAMIC_LANGLIST)") if new else "")
lang_start = "/*\n * The scan of the NLS directory is given langListTimeout seconds." if new else "static sigjmp_buf\tlangJump;"
lang_end = "#endif /* ENABLE_DYNAMIC_LANGLIST */"
language = until(util, lang_start, lang_end)
# the generic scan of one directory
language = language.replace("#if defined(_AIX)", "#if 0")

harness = r'''
#define _GNU_SOURCE
#include "dm.h"
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <setjmp.h>
#include <time.h>
#include <X11/Xlib.h>
#undef ConnectionNumber
#define ConnectionNumber(d) 3

extern int errors, hung_logged, opens, now_offset;
extern int hang_open, open_fails;
extern int open_timeout_seconds;
int test_bounded(unsigned seconds, int mode, int *result_out);
int test_wait_for_server(struct display *d);
int test_connection_lost(int fd);
int test_can_write(int fd);
void test_make_lang_list(void);
void test_pseudo_reset(void);
time_t test_time(time_t *t);
extern int hang_reset;
'''

util_unit = r'''
#include "harness.h"
#include <stdlib.h>
''' + bounded + r'''
static void *Job(void *arg)
{
    long mode = (long)arg;
    sigset_t current;
    if (mode == 1) usleep(100000);
    if (mode == 2) { for (;;) usleep(100000); }
    if (mode == 3) { sigprocmask(SIG_BLOCK, NULL, &current); return (void *)(long)(sigismember(&current, SIGTERM) + 10 * sigismember(&current, SIGALRM)); }
    return (void *)7;
}
int test_bounded(unsigned seconds, int mode, int *result_out)
{
    void *result = NULL;
    int status = BoundedCall(seconds, Job, (void *)(long)mode, &result);
    *result_out = (int)(long)result;
    return status;
}
''' if new else r'''
int test_bounded(unsigned seconds, int mode, int *result_out) { (void)seconds; (void)mode; *result_out = 0; return -99; }
'''

lang_unit = r'''
#include "harness.h"
#include "vgmsg.h"
#define time test_time
char languageList[LANGLISTSIZE];
int langListTimeout;
#define DEF_NLS_DIR_TEST "nls"
#undef DEF_NLS_DIR
#define DEF_NLS_DIR "nls"
#undef ENABLE_DYNAMIC_LANGLIST
static void ScanNLSDir(char *dirname);
''' + language + r'''
#undef time
void test_make_lang_list(void) { MakeLangList(); }
'''

server_unit = r'''
#include "harness.h"
#include "vgmsg.h"
#include <fcntl.h>
Display *dpy;
#define XOpenDisplay test_XOpenDisplay
Display *test_XOpenDisplay(char *name);
void RegisterCloseOnFork(int fd) { (void)fd; }
int LogOpenError(int count) { (void)count; return 1; }
static const char *_SysErrorMsg(int n) { return strerror(n); }
''' + (until(server, "static void *\nOpenDisplayJob", "int \nLogOpenError") if new else "static sigjmp_buf openAbort;\nstatic SIGVAL abortOpen( int arg ) { siglongjmp (openAbort, 1); }\nstatic void GetRemoteAddress(struct display *d, int fd) { (void)d; (void)fd; }\n") + block(server, "int \nWaitForServer( struct display *d )") + (r'''
''' + block(server, "static int\nConnectionLost( int fd )") + block(server, "static int\nCanWrite( int fd )") + r'''
int test_connection_lost(int fd) { return ConnectionLost(fd); }
int test_can_write(int fd) { return CanWrite(fd); }
''' if "ConnectionLost" in server else r'''
int test_connection_lost(int fd) { (void)fd; return -99; }
int test_can_write(int fd) { (void)fd; return -99; }
''') + r'''
int test_wait_for_server(struct display *d) { return WaitForServer(d); }
'''

reset_unit = r'''
#include "harness.h"
#include "vgmsg.h"
#undef ScreenCount
#undef RootWindow
#define ScreenCount(d) 1
#define RootWindow(d, s) 1UL
#define XQueryTree test_XQueryTree
#define XKillClient test_XKillClient
#define XSync test_XSync
#define XSetErrorHandler test_XSetErrorHandler
#define XFree test_XFree
#define RESET_TIMEOUT 1
Status test_XQueryTree(Display *, Window, Window *, Window *, Window **, unsigned int *);
int test_XKillClient(Display *, XID);
int test_XSync(Display *, Bool);
XErrorHandler test_XSetErrorHandler(XErrorHandler);
int test_XFree(void *);
''' + (block(reset, "static int \nignoreErrors") if False else "") + r'''
''' + "\n".join([]) + r'''
#include "reset.c"
void test_pseudo_reset(void) { pseudoReset((Display *)0x1000); }
'''

stubs = r'''
#define _GNU_SOURCE
#include "harness.h"
#include <stdarg.h>
#include <sys/wait.h>

int errors, hung_logged, opens, now_offset, hang_open, open_fails, hang_reset, debugLevel;
time_t clock_offset, clock_advance;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; ++errors; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
time_t test_time(time_t *t) { clock_offset += clock_advance; time_t now = time(NULL) + clock_offset; if (t) *t = now; return now; }

Display *test_XOpenDisplay(char *name)
{
    (void)name;
    ++opens;
    if (hang_open) { for (;;) { struct timespec t = {1, 0}; nanosleep(&t, NULL); } }
    return open_fails ? NULL : (Display *)0x2000;
}
Status test_XQueryTree(Display *d, Window w, Window *r, Window *p, Window **c, unsigned int *n)
{
    (void)d; (void)w; (void)r; (void)p;
    if (hang_reset) { for (;;) { struct timespec t = {1, 0}; nanosleep(&t, NULL); } }
    *c = NULL; *n = 0;
    return 1;
}
int test_XKillClient(Display *d, XID x) { (void)d; (void)x; return 0; }
int test_XSync(Display *d, Bool b) { (void)d; (void)b; return 0; }
XErrorHandler test_XSetErrorHandler(XErrorHandler h) { (void)h; return NULL; }
int test_XFree(void *p) { (void)p; return 0; }

static double Now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int alarms_seen;
static void Alarm(int sig) { (void)sig; ++alarms_seen; }

static void BoundedNormal(void)
{
    int result = 0;
    CHECK(test_bounded(5, 0, &result) == 1 && result == 7);
    CHECK(test_bounded(5, 1, &result) == 1 && result == 7);       /* a job that takes a little while */
    CHECK(test_bounded(0, 1, &result) == 1 && result == 7);       /* no limit */
}

static void BoundedTimeout(void)
{
    int result = 0;
    double start = Now();
    CHECK(test_bounded(1, 2, &result) == 0);                      /* a job that never returns */
    CHECK(Now() - start > 0.8 && Now() - start < 4.0);
}

static void BoundedSignals(void)
{
    int result = 0;
    /* the job runs with every signal blocked: the signals are for the caller */
    CHECK(test_bounded(5, 3, &result) == 1);
    CHECK(result == 11);
}

static void OpenWorks(void)
{
    struct display d;
    memset(&d, 0, sizeof d);
    d.name = ":0"; d.openTimeout = 2; d.openRepeat = 1;
    CHECK(test_wait_for_server(&d) == 1);
}

static void OpenHangs(void)
{
    struct display d;
    double start = Now();
    memset(&d, 0, sizeof d);
    d.name = ":0"; d.openTimeout = 1; d.openRepeat = 3;
    hang_open = 1;
    /* the connection never comes: the manager gives up after the time it was given, once */
    CHECK(test_wait_for_server(&d) == 0);
    CHECK(Now() - start > 0.8 && Now() - start < 4.0);
    CHECK(errors >= 1);
}

static void OpenFails(void)
{
    struct display d;
    memset(&d, 0, sizeof d);
    d.name = ":0"; d.openTimeout = 2; d.openRepeat = 2; d.openDelay = 0;
    open_fails = 1;
    CHECK(test_wait_for_server(&d) == 0);
    CHECK(opens == 2);
}

static void OpenLeavesTheTimerAlone(void)
{
    struct display d;
    struct sigaction sa, old;
    memset(&d, 0, sizeof d);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = Alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm(20);                                                    /* the timer of somebody else */
    d.name = ":0"; d.openTimeout = 2; d.openRepeat = 1;
    CHECK(test_wait_for_server(&d) == 1);
    sigaction(SIGALRM, NULL, &old);
    CHECK(old.sa_handler == Alarm);                               /* its handler is still there */
    CHECK(alarm(0) > 15);                                         /* and its timer still running */
}

static void ResetNormal(void)
{
    test_pseudo_reset();
    CHECK(errors == 0);
}

static void ResetHangs(void)
{
    double start = Now();
    hang_reset = 1;
    test_pseudo_reset();                                          /* the server does not answer: the reset is given up */
    CHECK(Now() - start > 0.8 && Now() - start < 4.0);
    CHECK(errors == 1);
}

static void ResetLeavesTheTimerAlone(void)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = Alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm(20);
    test_pseudo_reset();
    sigaction(SIGALRM, NULL, &old);
    CHECK(old.sa_handler == Alarm && alarm(0) > 15);
}

static void ConnectionState(void)
{
    int fds[2];
    char byte = 'x';

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(test_connection_lost(fds[0]) == 0);
    CHECK(test_can_write(fds[0]) == 1);
    CHECK(write(fds[1], &byte, 1) == 1);
    CHECK(test_connection_lost(fds[0]) == 0);                     /* an answer is waiting: the server is there */
    CHECK(read(fds[0], &byte, 1) == 1);
    close(fds[1]);
    CHECK(test_connection_lost(fds[0]) == 1);                     /* the server closed the connection */
}

static void ConnectionBlocked(void)
{
    int fds[2], flags;
    char block[4096];

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    flags = fcntl(fds[0], F_GETFL);
    fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    memset(block, 0, sizeof block);
    while (write(fds[0], block, sizeof block) > 0) ;
    CHECK(test_can_write(fds[0]) == 0);                           /* a server that does not read: nothing more is written */
    CHECK(test_connection_lost(fds[0]) == 0);
}

static void LanguageList(void)
{
    char name[64];

    CHECK(mkdir("nls", 0755) == 0);
    for (int i = 0; i < 26; ++i)
        for (int j = 0; j < 26; ++j) {
            snprintf(name, sizeof name, "nls/%c%c", 'a' + i, 'a' + j);
            CHECK(mkdir(name, 0755) == 0);
        }
    /* more locales than the sort array holds: the list is cut, nothing is written beyond the array */
    test_make_lang_list();
    CHECK(strncmp(languageList, "C aa ab ", 8) == 0 || strncmp(languageList, "C ", 2) == 0);
    CHECK(errors == 0);
}

static void LanguageListTimeout(void)
{
    CHECK(mkdir("nls", 0755) == 0 && mkdir("nls/de_DE", 0755) == 0 && mkdir("nls/fr_FR", 0755) == 0);
    langListTimeout = 5;
    clock_advance = 10;                                           /* the clock runs fast: the deadline passes during the scan */
    test_make_lang_list();
    /* the scan stopped when the time was up and said so */
    CHECK(errors == 1);
}

static void LanguageListFast(void)
{
    CHECK(mkdir("nls", 0755) == 0 && mkdir("nls/de_DE", 0755) == 0 && mkdir("nls/fr_FR", 0755) == 0);
    langListTimeout = 30;
    test_make_lang_list();
    CHECK(strstr(languageList, "de_DE") && strstr(languageList, "fr_FR") && errors == 0);
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"bounded-normal", BoundedNormal}, {"bounded-timeout", BoundedTimeout}, {"bounded-signals", BoundedSignals},
    {"open-works", OpenWorks}, {"open-hangs", OpenHangs}, {"open-fails", OpenFails},
    {"open-leaves-the-timer-alone", OpenLeavesTheTimerAlone},
    {"reset-normal", ResetNormal}, {"reset-hangs", ResetHangs}, {"reset-leaves-the-timer-alone", ResetLeavesTheTimerAlone},
    {"connection-state", ConnectionState}, {"connection-blocked", ConnectionBlocked},
    {"language-list", LanguageList}, {"language-list-timeout", LanguageListTimeout}, {"language-list-fast", LanguageListFast},
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

names = ["bounded-normal", "bounded-timeout", "bounded-signals", "open-works", "open-hangs", "open-fails",
         "open-leaves-the-timer-alone", "reset-normal", "reset-hangs", "reset-leaves-the-timer-alone",
         "connection-state", "connection-blocked", "language-list", "language-list-timeout", "language-list-fast"]
failed = run_all("x-timeouts-",
                 {"harness.h": harness, "util_unit.c": util_unit, "lang_unit.c": lang_unit, "server_unit.c": server_unit,
                  "reset_unit.c": reset_unit, "stubs.c": stubs},
                 [("util_unit", ["-w"]), ("lang_unit", ["-w"]), ("server_unit", ["-w"]), ("reset_unit", ["-w"]),
                  ("stubs", ["-Wall", "-Wextra", "-Wno-unused-parameter"])],
                 ["-pthread", "-lX11"], names, defines=["-pthread"])
finish(failed, "X server timeouts", "bounded calls, opening the display, the reset, the ping and the locale scan")
