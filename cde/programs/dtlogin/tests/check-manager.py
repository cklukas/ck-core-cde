#!/usr/bin/env python3
"""Exercise the login manager's signal handling and the input it takes from other processes.

The production code of dm.c (signal handlers), xdmcp.c (the wait loop),
session.c (resource loader, greeter protocol) and choose.c (chooser socket)
is compiled with stand-ins for what lies outside it, under ASan and UBSan.
Every scenario runs in its own process and must neither crash nor be flagged
by a sanitizer.  Set DTLOGIN_SOURCE to test another copy of the sources.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTLOGIN_SOURCE") or Path(__file__).resolve().parents[1])
read = lambda name: (source / name).read_text()

def block(text, start, end="\n}\n"):
    first = text.index(start)
    return text[first:text.index(end, first) + len(end)]

def until(text, start, stop):
    first = text.index(start)
    return text[first:text.index(stop, first)]

dm, xdmcp, session, choose, util, server = (read(n) for n in ("dm.c", "xdmcp.c", "session.c", "choose.c", "util.c", "server.c"))
deferred = "ShutdownManager" in dm
loader_start = "/*\n * Load the resources in file into the X server" if "RunXrdb" in session \
               else "int\nLoadXloginResources( struct display *d )"
chooser_start = "/* seconds an accepted chooser connection may stay silent */" if "CHOOSER_REPLY_TIMEOUT" in choose \
                else "int\nProcessChooserSocket ("

# the notes the session process's handlers leave and the waits that look at them (not in older sources)
signal_start = "/*\n * The signals that reach the session process only leave a note."
signal_helpers = until(session, signal_start, "#if defined(__STDC__)\nstatic int\nFileNameCompare") if signal_start in session else ""

if "SetSessionGroups" in session:
    groups_function = block(session, "static int\nSetSessionGroups(")
else:
    # sources from before the function existed: the statements sit inline in StartClient
    groups_function = ("static int\nSetSessionGroups( struct verify_info *verify )\n{\n" +
                       until(session, "setgroups (verify->ngroups, verify->groups);", "#    else  /* ! NGROUPS */") +
                       "    return 1;\n}\n")

harness = r'''
#include "dm.h"
#include "vgmsg.h"
#include "vgproto.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

extern int violations, destroyed, stopped, start_displays, registered, mktemp_used, parent_pid_value;
extern int response[2], request[2];
int in_handler(void);
char *StubMktemp(char *template);
void install_handlers(void);
void set_parent_pid(int pid);
int dt_shutdown_value(void);
int rescan_value(void);
int stop_noted(void);
void run_deferred_shutdown(void);
void block_and_wait_for_stop(void);
void watch_pipe(int fd);
void test_WaitForSomething(void);
int LoadXloginResources(struct display *d);
int test_Ask(char *buffer, int length);
int test_Chooser(int fd);
int test_StartServer(struct display *d);
extern struct passwd puser;
extern int drop_fails;
int StubSetuid(uid_t uid);
int StubSetgid(gid_t gid);
uid_t StubGeteuid(void);
int StubSetgroups(size_t count, const gid_t *list);
int test_Groups(struct verify_info *verify);
int test_SetTitle(char *area, char *name);
extern int groups_fail;
extern uid_t effective_uid;
extern char call_log[16];
'''

dm_unit = r'''
#include "harness.h"
''' + ("#define HAVE_DEFERRED 1\nvolatile sig_atomic_t Rescan, StopRequested;\n" if deferred else "int Rescan;\n") + r'''
static int parent_pid = -1;
char *Title;
static void MarkShutdownTime(void) { if (in_handler()) ++violations; }
''' + block(dm, "static void\nTerminateProcess(int pid, int sig )") \
    + until(dm, "static int    dt_shutdown = 0;", "/*\n    In HP-UX") \
    + block(dm, "static SIGVAL\nRescanNotify( int arg )") \
    + block(dm, re.search(r"^int\s*\nSetTitle", dm, re.M).group(0)) + r'''
int test_SetTitle(char *area, char *name) { Title = area; return SetTitle(name, "chooser"); }
int dt_shutdown_value(void) { return dt_shutdown; }
int rescan_value(void) { return Rescan; }
void set_parent_pid(int pid) { parent_pid = parent_pid_value = pid; }
void install_handlers(void) { signal(SIGTERM, StopAll); signal(SIGHUP, RescanNotify); }
#ifdef HAVE_DEFERRED
int stop_noted(void) { return StopRequested; }
void run_deferred_shutdown(void) { StopRequested = 0; ShutdownManager(); }
void block_and_wait_for_stop(void)
{
    sigset_t previous;
    BlockRequestSignals(&previous);
    kill(getpid(), SIGTERM);
    if (!RequestPending()) sigsuspend(&previous);
    sigprocmask(SIG_SETMASK, &previous, NULL);
}
#else
int stop_noted(void) { return dt_shutdown; }
void run_deferred_shutdown(void) { }
void block_and_wait_for_stop(void) { kill(getpid(), SIGTERM); }
#endif
'''

wait_unit = r'''
#include "harness.h"
#include <sys/select.h>
int xdmcpFd = -1, chooserFd = -1, wakeupTime = -1, WellKnownSocketsMax, start_displays;
FD_TYPE WellKnownSocketsMask;
int AnyWellKnownSockets(void) { return xdmcpFd != -1 || chooserFd != -1; }
void StartDisplays(void) { ++start_displays; }
void WaitForChild(void) { }
int ProcessChooserSocket(int fd);
static void ProcessRequestSocket(void) { }
''' + block(xdmcp, "void WaitForSomething (void)") + r'''
void test_WaitForSomething(void) { WaitForSomething(); }
void watch_pipe(int fd)
{
    xdmcpFd = fd;
    FD_SET(fd, &WellKnownSocketsMask);
    WellKnownSocketsMax = fd;
}
'''

session_unit = r'''
#include "harness.h"
#include <ctype.h>
#include <stdint.h>
#define mktemp StubMktemp
#define geteuid StubGeteuid
#define setgroups StubSetgroups
#define setgid StubSetgid
#undef isblank
#define isblank(c) ((c) == ' ' || (c) == '\t')
int response[2], request[2];
XrmDatabase XresourceDB;
static void LoadAltDtsResources(struct display *d) { (void)d; }
char *_ExpandLang(char *string, char *lang) { (void)lang; return strdup(string); }
''' + block(util, "int\nappendArg(") + block(util, "int\nparseArgs(") + block(util, "void\nfreeEnv (") \
    + until(session, loader_start, "/***************************************************************************\n *\n *  LoadAltDtsResources"
            ).replace('"/var/dt/dtlogin_XXXXXX"', '"dtlogin_XXXXXX"') \
    + signal_helpers + block(session, "static void\nTellGreeter(") + block(session, "static int\nAskGreeter(") + groups_function + r'''
int test_Ask(char *buffer, int length) { return AskGreeter(NULL, buffer, length); }
int test_Groups(struct verify_info *verify) { return SetSessionGroups(verify); }
'''

choose_unit = r'''
#include "harness.h"
#define CHOOSER_REPLY_TIMEOUT 1
int registered;
static int RegisterIndirectChoice(ARRAY8Ptr clientAddress, CARD16 connectionType, ARRAY8Ptr choice)
{ (void)clientAddress; (void)connectionType; (void)choice; ++registered; return 1; }
''' + until(choose, chooser_start, "\nvoid\nRunChooser") + r'''
int test_Chooser(int fd) { return ProcessChooserSocket(fd); }
'''

server_unit = r'''
#include "harness.h"
#define setuid StubSetuid
#define setgid StubSetgid
static int receivedUsr1;
static void CatchUsr1(int arg) { (void)arg; }
static void CatchSignal(int sig, void (*handler)(int), struct sigaction *previous) { (void)sig; (void)handler; (void)previous; }
static int serverPause(unsigned t, int serverPid) { (void)t; (void)serverPid; return 0; }
struct passwd puser;
void CleanUpChild(void) { }
char **parseEnv(char **e, char *string) { (void)string; return e; }
''' + block(server, re.search(r"^int\s*\nStartServerOnce", server, re.M).group(0)) + r'''
int test_StartServer(struct display *d) { return StartServerOnce(d); }
'''

stubs = r'''
#define _GNU_SOURCE
#include "harness.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>

int violations, destroyed, stopped, mktemp_used, parent_pid_value, debugLevel, drop_fails, groups_fail;
uid_t effective_uid;
char call_log[16];

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

/* inside a signal handler the signal being handled is blocked */
int in_handler(void)
{
    sigset_t current;
    sigprocmask(SIG_BLOCK, NULL, &current);
    return sigismember(&current, SIGTERM) || sigismember(&current, SIGHUP);
}
void Debug(char *fmt, ...)
{
    (void)fmt;
    if (in_handler()) { ++violations; if (getpid() != parent_pid_value) _exit(97); }
}
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
char *StubMktemp(char *template) { ++mktemp_used; return template; }
int StubSetuid(uid_t uid) { (void)uid; if (drop_fails) { errno = EPERM; return -1; } return 0; }
int StubSetgid(gid_t gid)
{ (void)gid; strcat(call_log, "G"); if (drop_fails) { errno = EPERM; return -1; } return 0; }
uid_t StubGeteuid(void) { return effective_uid; }
int StubSetgroups(size_t count, const gid_t *list)
{ (void)count; (void)list; strcat(call_log, "g"); if (groups_fail) { errno = EPERM; return -1; } return 0; }
void DestroyWellKnownSockets(void) { if (in_handler()) ++violations; ++destroyed; }
void StopDisplay(struct display *d) { (void)d; if (in_handler()) ++violations; ++stopped; }
void ForEachDisplay(void (*f)(struct display *))
{
    static struct display d;
    if (in_handler()) ++violations;
    f(&d);
}

static void Watchdog(int sig)
{
    static const char message[] = "FAIL blocked for more than the time allowed\n";
    (void)sig;
    if (write(2, message, sizeof message - 1)) { }
    _exit(1);
}

static void StopSignal(void)
{
    install_handlers();
    set_parent_pid(getpid());
    raise(SIGTERM);
    /* a handler may not call what the main loop calls: it only leaves a note */
    CHECK(violations == 0);
    CHECK(stop_noted());
    run_deferred_shutdown();
    CHECK(destroyed == 1 && stopped == 1 && dt_shutdown_value() == 1 && violations == 0);
}

static void RescanSignal(void)
{
    install_handlers();
    set_parent_pid(getpid());
    raise(SIGHUP);
    CHECK(violations == 0);
    CHECK(rescan_value() == 1);
}

static void StrayChild(void)
{
    pid_t pid = fork();
    int status;
    if (pid == 0) {
        install_handlers();
        set_parent_pid(getppid());
        raise(SIGTERM);
        _exit(96);
    }
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
    /* a child that was signalled before it could reset the handler just dies of it */
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
}

static void RequestWhileBlocked(void)
{
    signal(SIGALRM, Watchdog);
    alarm(5);
    install_handlers();
    set_parent_pid(getpid());
    /* a request that arrives before the manager sleeps wakes it instead of being lost */
    block_and_wait_for_stop();
    alarm(0);
    CHECK(stop_noted());
}

static void RaiseTerm(int sig) { (void)sig; kill(getpid(), SIGTERM); }

static void WaitInterrupted(void)
{
    int fds[2];
    struct itimerval timer = { {0, 0}, {0, 100000} };
    install_handlers();
    set_parent_pid(getpid());
    CHECK(pipe(fds) == 0);
    watch_pipe(fds[0]);
    signal(SIGALRM, RaiseTerm);
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    /* shutdown arrives while the manager waits for a packet: it must not start displays first */
    test_WaitForSomething();
    CHECK(start_displays == 0);
}

static char *Slurp(const char *path)
{
    static char text[4096];
    FILE *f = fopen(path, "r");
    CHECK(f);
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = '\0';
    fclose(f);
    return text;
}

static int Leftovers(void)
{
    int count = 0;
    DIR *dir = opendir(".");
    CHECK(dir);
    for (struct dirent *e; (e = readdir(dir)); )
        if (strncmp(e->d_name, "dtlogin_", 8) == 0) ++count;
    closedir(dir);
    return count;
}

static void ResourceLoader(void)
{
    struct display d;
    char cwd[512], script[600], expected[1024];
    const char *hostile = "x;touch INJECTED $(touch INJECTED2) `touch INJECTED3`:0";
    memset(&d, 0, sizeof d);
    CHECK(getcwd(cwd, sizeof cwd));
    FILE *f = fopen("resources", "w");
    CHECK(f && fputs("Dtlogin*greeting: hello\n", f) >= 0 && fclose(f) == 0);
    snprintf(script, sizeof script, "%s/fake-xrdb", cwd);
    f = fopen(script, "w");
    CHECK(f);
    fputs("#!/bin/sh\n"
          "{ for a in \"$@\"; do printf '%s\\n' \"$a\"; done; printf 'XAUTHORITY=%s\\n' \"$XAUTHORITY\"; } > xrdb.args\n"
          "cp \"$4\" xrdb.resources\n"
          "ls -ln \"$4\" | cut -c1-10 > xrdb.mode\n", f);
    CHECK(fclose(f) == 0 && chmod(script, 0700) == 0);
    d.name = (char *)hostile;
    d.resources = "resources";
    d.language = "C";
    d.xrdb = script;
    d.authFile = "auth-file";
    CHECK(LoadXloginResources(&d) == 0);
    /* the display name reaches the loader as one argument; no shell sees it */
    CHECK(access("INJECTED", F_OK) != 0 && access("INJECTED2", F_OK) != 0 && access("INJECTED3", F_OK) != 0);
    snprintf(expected, sizeof expected, "-display\n%s\n-load\n", hostile);
    char *args = Slurp("xrdb.args");
    CHECK(strncmp(args, expected, strlen(expected)) == 0);
    CHECK(strstr(args, "\nXAUTHORITY=auth-file\n") != NULL);
    CHECK(strstr(Slurp("xrdb.resources"), "greeting") != NULL);
    /* the file was created for this run alone, readable by no one else, and removed afterwards */
    CHECK(strcmp(Slurp("xrdb.mode"), "-rw-------\n") == 0);
    CHECK(mktemp_used == 0 && Leftovers() == 0);
}

static void GreeterLength(void)
{
    char *buffer = malloc(512);
    ResponseHeader header = { REQUEST_OP_CHALLENGE, 0, 20 };
    char *payload = calloc(1, 40000);

    CHECK(pipe(response) == 0);
    CHECK(write(response[1], &header, sizeof header) == sizeof header);
    CHECK(write(response[1], payload, 16) == 16);
    CHECK(test_Ask(buffer, 512) == 20);
    close(response[0]); close(response[1]);

    /* the length is the greeter's claim: more than the buffer holds ends the exchange */
    header.length = 40000;
    CHECK(pipe(response) == 0);
    CHECK(write(response[1], &header, sizeof header) == sizeof header);
    CHECK(write(response[1], payload, 40000 - sizeof header) == (ssize_t)(40000 - sizeof header));
    CHECK(test_Ask(buffer, 512) == 0);
    free(buffer); free(payload);
}

static int Connect(const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    strcpy(address.sun_path, path);
    CHECK(fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
    return fd;
}

static void SilentChooser(void)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    static const unsigned char reply[] = { 0, 4, 10, 0, 0, 5, 0, 0, 0, 4, 10, 0, 0, 9 };
    strcpy(address.sun_path, "chooser.sock");
    CHECK(listener >= 0 && bind(listener, (struct sockaddr *)&address, sizeof address) == 0);
    CHECK(listen(listener, 5) == 0);
    signal(SIGALRM, Watchdog);
    alarm(8);

    /* a chooser that answers is heard */
    int client = Connect("chooser.sock");
    CHECK(write(client, reply, sizeof reply) == sizeof reply);
    CHECK(test_Chooser(listener) == 1 && registered == 1);
    close(client);

    /* a peer that says nothing must not hold the manager up */
    client = Connect("chooser.sock");
    CHECK(test_Chooser(listener) == 1 && registered == 1);
    alarm(0);
    close(client);
}

static void ServerPrivileges(void)
{
    struct display d;
    char cwd[512], script[600], *argv[2] = { script, NULL };
    int status;
    FILE *f;

    memset(&d, 0, sizeof d);
    CHECK(getcwd(cwd, sizeof cwd));
    snprintf(script, sizeof script, "%s/fake-server", cwd);
    f = fopen(script, "w");
    CHECK(f && fputs("#!/bin/sh\necho started > server.marker\n", f) >= 0);
    CHECK(fclose(f) == 0 && chmod(script, 0700) == 0);
    d.name = ":0";
    d.argv = argv;
    puser.pw_uid = 12345;
    puser.pw_gid = 12345;

    /* when the server cannot be given up the manager's privileges, it is not started */
    drop_fails = 1;
    CHECK(test_StartServer(&d) == TRUE);
    CHECK(waitpid(d.serverPid, &status, 0) == d.serverPid);
    CHECK(access("server.marker", F_OK) != 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);

    drop_fails = 0;
    CHECK(test_StartServer(&d) == TRUE);
    CHECK(waitpid(d.serverPid, &status, 0) == d.serverPid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0 && access("server.marker", F_OK) == 0);
}

static void SessionGroups(void)
{
    struct verify_info v;
    memset(&v, 0, sizeof v);
    v.ngroups = 2; v.groups[0] = 100; v.groups[1] = 200;

    /* the groups are set before the primary group, and each failure is reported */
    effective_uid = 0; drop_fails = 0; groups_fail = 0; call_log[0] = '\0';
    CHECK(test_Groups(&v) == 1 && strcmp(call_log, "gG") == 0);
    groups_fail = 1; call_log[0] = '\0';
    CHECK(test_Groups(&v) == 0 && strcmp(call_log, "g") == 0);
    groups_fail = 0; drop_fails = 1; call_log[0] = '\0';
    CHECK(test_Groups(&v) == 0);
    /* a process that has already given up its privileges has nothing left to set */
    effective_uid = 1000; groups_fail = 1; call_log[0] = '\0';
    CHECK(test_Groups(&v) == 1 && call_log[0] == '\0');
}

static void ProcessTitle(void)
{
    char area[64] = "/usr/b/dtlogin";
    /* the display name and the program name are moved within their own strings */
    CHECK(test_SetTitle(area, "a.b:0.0") == 1);
    CHECK(strcmp(area, "dtlogin <a:0> ") == 0);
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"stop-signal", StopSignal},
    {"rescan-signal", RescanSignal},
    {"stray-child", StrayChild},
    {"request-while-blocked", RequestWhileBlocked},
    {"wait-interrupted", WaitInterrupted},
    {"resource-loader", ResourceLoader},
    {"greeter-length", GreeterLength},
    {"silent-chooser", SilentChooser},
    {"server-privileges", ServerPrivileges},
    {"session-groups", SessionGroups},
    {"process-title", ProcessTitle},
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

def run_all(prefix, files, units, libs, scenarios):
    with tempfile.TemporaryDirectory(prefix=prefix, dir=os.environ.get("TMPDIR")) as work:
        work = Path(work)
        for name, text in files.items():
            (work / name).write_text(text)
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-std=c23", "-D_DEFAULT_SOURCE", "-DUNIXCONN", "-DTCPCONN", "-DXDMCP", "-DHASXDMAUTH",
                 "-DANSICPP", "-I", str(work), "-I", str(source), "-I", str(source.parents[1] / "include"),
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                 "-g", "-O1"]
        objects = []
        for unit, extra in units:
            obj = work / (unit + ".o")
            subprocess.run(cc + flags + extra + ["-c", str(work / (unit + ".c")), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = work / "scenarios"
        subprocess.run(cc + flags + objects + ["-o", str(binary)] + libs, check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0",
                   UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
        failed = 0
        for name in scenarios:
            scratch = work / name
            scratch.mkdir()
            try:
                done = subprocess.run([str(binary), name], cwd=scratch, env=env, capture_output=True,
                                      text=True, timeout=60)
            except subprocess.TimeoutExpired:
                failed += 1
                print("FAIL %s: did not finish within 60 seconds" % name)
                continue
            lines = (done.stdout + done.stderr).splitlines()
            reason = next((l for l in lines if l.startswith("FAIL") or "ERROR:" in l or "runtime error" in l),
                          lines[0] if lines else "exit status %d" % done.returncode)
            frame = next((l.strip() for l in lines if re.match(r"\s*#[0-4] .* in \w+ .*(_unit|stubs)\.c", l)), "")
            if done.returncode:
                failed += 1
                print("FAIL %s: %s %s" % (name, reason.strip(), frame))
            else:
                print("ok   %s" % name)
        return failed

names = ["stop-signal", "rescan-signal", "stray-child", "request-while-blocked", "wait-interrupted",
         "resource-loader", "greeter-length", "silent-chooser", "server-privileges", "session-groups", "process-title"]
failed = run_all("login-manager-",
                 {"harness.h": harness, "dm_unit.c": dm_unit, "wait_unit.c": wait_unit,
                  "session_unit.c": session_unit, "choose_unit.c": choose_unit, "server_unit.c": server_unit,
                  "stubs.c": stubs},
                 [("dm_unit", ["-w"]), ("wait_unit", ["-w"]), ("session_unit", ["-w"]), ("choose_unit", ["-w"]),
                  ("server_unit", ["-w"]), ("stubs", ["-Wall", "-Wextra", "-Wno-unused-parameter"])],
                 ["-lXdmcp", "-lX11"], names)
if failed:
    sys.exit("Login manager: %d scenario(s) failed" % failed)
print("Login manager: signal handlers, wait loop, resource loader, greeter and chooser input passed")
