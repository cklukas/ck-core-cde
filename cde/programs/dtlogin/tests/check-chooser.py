#!/usr/bin/env python3
"""Exercise the chooser socket and the indirect-query bookkeeping of the login manager.

The production socket.c, netaddr.c and the chooser part of choose.c are compiled with stand-ins
for what lies outside them, under ASan and UBSan, and driven the way a chooser and a flood of
indirect queries drive them.  Every scenario runs in its own process.  Set DTLOGIN_SOURCE to
test another copy of the sources.
"""
import sys
from scenarios import block, finish, read, run_all, until

choose = read("choose.c")

chooser_code = until(choose, "static int\nFormatBytes (", "\nvoid\nRunChooser") + block(choose, "static char *\nPrint8Address (")

harness = r'''
#include "dm.h"
#include <netdb.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern int registered_fds[16], nregistered;
extern char last_error[256];
extern time_t now_value;
time_t test_time(time_t *t);
int test_CreateSockets(void);
int test_FormatArgument(char *buf, int len);
int test_Chooser(int fd);
void test_DestroySockets(void);
int RememberIndirectClient(ARRAY8Ptr clientAddress, CARD16 connectionType);
int IsIndirectClient(ARRAY8Ptr clientAddress, CARD16 connectionType);
ARRAY8Ptr IndirectChoice(ARRAY8Ptr clientAddress, CARD16 connectionType);
int AwaitIndirectChoice(ARRAY8Ptr clientAddress, CARD16 connectionType) __attribute__((weak));
void DestroyChooserSocket(void) __attribute__((weak));
'''

socket_unit = r'''
#include "harness.h"
#include "socket.c"
int test_CreateSockets(void) { return CreateWellKnownSockets(); }
void test_DestroySockets(void)
{
    if (DestroyChooserSocket) DestroyChooserSocket();
    else if (chooserFd != -1) close(chooserFd);
}
'''

netaddr_unit = r'''
#include "harness.h"
#include "netaddr.c"
'''

choose_unit = r'''
#include "harness.h"
#define time test_time
int LoadXloginResources(struct display *d);
int GetChooserAddr(char *addr, int *lenp);
int NetaddrFamily(XdmcpNetaddr netaddrp);
static char *Print8Address (ARRAY8Ptr Address);
''' + chooser_code + r'''
#undef time
int test_FormatArgument(char *buf, int len) { return FormatChooserArgument(buf, len); }
int test_Chooser(int fd) { return ProcessChooserSocket(fd); }
'''

stubs = r'''
#define _GNU_SOURCE
#include "harness.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdarg.h>

int request_port, xdmcpFd = -1, chooserFd = -1, WellKnownSocketsMax, debugLevel;
FD_TYPE WellKnownSocketsMask;
char *authDir;
int registered_fds[16], nregistered;
char last_error[256];
time_t now_value = 1000;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

time_t test_time(time_t *t) { if (t) *t = now_value; return now_value; }
void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof last_error, (char *)fmt, ap);
    va_end(ap);
}
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
char *localHostname(void) { return "thishost"; }
void registerHostname(char *name, int namelen) { (void)name; (void)namelen; }
void RegisterCloseOnFork(int fd) { if (nregistered < 16) registered_fds[nregistered++] = fd; }
ARRAY8Ptr getLocalAddress(void)
{
    static unsigned char address[4] = {10, 9, 8, 7};
    static ARRAY8 a = {4, address};
    return &a;
}
int parseArgs(char ***argv, const char *string) { (void)argv; (void)string; return 1; }
void freeEnv(char **env) { (void)env; }

static int Registered(int fd)
{
    for (int i = 0; i < nregistered; ++i) if (registered_fds[i] == fd) return 1;
    return 0;
}

static ARRAY8 Make(const void *bytes, int length)
{
    ARRAY8 a = { length, length ? memcpy(malloc(length), bytes, length) : NULL };
    return a;
}

static char scratch[512];

/* the path of a socket is short: use a directory of its own with a short name */
static void RemoveScratch(void)
{
    char path[600];
    snprintf(path, sizeof path, "%s/dtlogin-chooser-%ld", scratch, (long)getpid());
    unlink(path);
    rmdir(scratch);
}

static void Setup(void)
{
    const char *base = getenv("TMPDIR");
    if (!base || strlen(base) > 40) base = "/tmp";
    snprintf(scratch, sizeof scratch, "%s/dtlc-XXXXXX", base);
    CHECK(mkdtemp(scratch));
    atexit(RemoveScratch);
    authDir = scratch;
    request_port = 30000 + (getpid() % 20000);
    if (test_CreateSockets() != 1) { fprintf(stderr, "FAIL sockets: %s (errno %d)\n", last_error, errno); exit(1); }
}

static void Connect(const char *path, const void *data, size_t length)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0 && strlen(path) < sizeof address.sun_path);
    strcpy(address.sun_path, path);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
    CHECK(write(fd, data, length) == (ssize_t)length);
    close(fd);
}

static void Unreachable(int sig) { (void)sig; static const char m[] = "FAIL blocked\n"; if (write(2, m, sizeof m - 1)) { } _exit(1); }

/* The chooser reports the host it chose for a client: client address, connection type, host address. */
static size_t Report(unsigned char *out, const unsigned char *client, int clientLength, int type,
                     const unsigned char *host, int hostLength)
{
    size_t n = 0;
    out[n++] = clientLength >> 8; out[n++] = clientLength;
    memcpy(out + n, client, clientLength); n += clientLength;
    out[n++] = type >> 8; out[n++] = type;
    out[n++] = hostLength >> 8; out[n++] = hostLength;
    memcpy(out + n, host, hostLength); n += hostLength;
    return n;
}

static char *SocketPath(void)
{
    static char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un address;
    socklen_t length = sizeof address;
    CHECK(getsockname(chooserFd, (struct sockaddr *)&address, &length) == 0);
    CHECK(address.sun_family == AF_UNIX);
    snprintf(path, sizeof path, "%s", address.sun_path);
    return path;
}

static void SocketOfTheManager(void)
{
    struct stat st;
    int type;
    socklen_t length = sizeof type;

    Setup();
    /* the socket is a file that only the manager's user can use, not a port on all interfaces */
    CHECK(stat(SocketPath(), &st) == 0);
    CHECK(S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600);
    CHECK(getsockopt(chooserFd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_STREAM);
    /* the sessions started later must not inherit it */
    CHECK(Registered(chooserFd));
}

static void SocketFileRemoved(void)
{
    char path[512];
    struct stat st;

    Setup();
    snprintf(path, sizeof path, "%s", SocketPath());
    CHECK(stat(path, &st) == 0);
    close(chooserFd);
    chooserFd = -1;
    test_DestroySockets();
    CHECK(stat(path, &st) != 0);
}

static void ArgumentNamesTheSocket(void)
{
    char argument[512], expected[512];
    const char *path;
    size_t n = 0;

    Setup();
    path = SocketPath();
    CHECK(test_FormatArgument(argument, sizeof argument) == 1);
    /* the family, then the path of the socket file, as hexadecimal digits */
    n += sprintf(expected + n, "%04x", AF_UNIX);
    for (const char *p = path; *p; ++p) n += sprintf(expected + n, "%02x", (unsigned char)*p);
    CHECK(strcmp(argument, expected) == 0);
}

static void ChoiceNeedsAChooser(void)
{
    static const unsigned char client[4] = {10, 1, 2, 3}, other[4] = {10, 1, 2, 4}, host[4] = {10, 9, 9, 9};
    unsigned char report[64];
    ARRAY8 a = Make(client, 4), b = Make(other, 4);
    size_t n;

    Setup();
    /* nobody started a chooser for this client: the report is not from one of ours */
    n = Report(report, client, 4, FamilyInternet, host, 4);
    Connect(SocketPath(), report, n);
    CHECK(test_Chooser(chooserFd) == 1);
    CHECK(IndirectChoice(&a, FamilyInternet) == NULL);

    /* a chooser was started for the client: its report is kept, for that client only */
    CHECK(AwaitIndirectChoice != NULL);
    CHECK(AwaitIndirectChoice(&a, FamilyInternet) == 1);
    CHECK(IndirectChoice(&a, FamilyInternet) == NULL);
    Connect(SocketPath(), report, n);
    CHECK(test_Chooser(chooserFd) == 1);
    ARRAY8Ptr choice = IndirectChoice(&a, FamilyInternet);
    CHECK(choice && choice->length == 4 && memcmp(choice->data, host, 4) == 0);
    CHECK(IndirectChoice(&b, FamilyInternet) == NULL);
    CHECK(IndirectChoice(&a, FamilyChaos) == NULL);

    /* the choice is for the query that follows; later it is forgotten */
    now_value += 16;
    CHECK(IndirectChoice(&a, FamilyInternet) == NULL);
}

static void LongAddressReported(void)
{
    unsigned char client[255], host[4] = {10, 9, 9, 9}, report[600];
    ARRAY8 a;
    size_t n;

    Setup();
    memset(client, 0xdd, sizeof client);
    a = Make(client, sizeof client);
    CHECK(AwaitIndirectChoice != NULL);
    CHECK(AwaitIndirectChoice(&a, FamilyInternet) == 1);
    /* the address is printed to the debug log: it must fit however long it is */
    n = Report(report, client, sizeof client, FamilyInternet, host, 4);
    Connect(SocketPath(), report, n);
    CHECK(test_Chooser(chooserFd) == 1);
    CHECK(IndirectChoice(&a, FamilyInternet) != NULL);
}

static void IndirectClientsAreBounded(void)
{
    ARRAY8 a;
    int accepted = 0;

    /* anybody can send indirect queries from any address: the list does not grow without limit */
    for (int i = 0; i < 1000; ++i) {
        unsigned char address[4] = {10, 2, i >> 8, i & 255};
        a = Make(address, 4);
        accepted += RememberIndirectClient(&a, FamilyInternet);
        free(a.data);
    }
    CHECK(accepted > 0 && accepted < 1000);
    /* the first clients are kept, those that did not fit were refused */
    unsigned char first[4] = {10, 2, 0, 0}, last[4] = {10, 2, 3, 231};
    a = Make(first, 4);
    CHECK(IsIndirectClient(&a, FamilyInternet));
    free(a.data);
    a = Make(last, 4);
    CHECK(!IsIndirectClient(&a, FamilyInternet));
    free(a.data);
}

static void IndirectClientsExpire(void)
{
    unsigned char address[4] = {10, 3, 0, 1};
    ARRAY8 a = Make(address, 4);

    CHECK(RememberIndirectClient(&a, FamilyInternet) == 1);
    CHECK(IsIndirectClient(&a, FamilyInternet));
    /* a client that never follows up is dropped when the next one arrives */
    now_value += 11 * 60;
    unsigned char other[4] = {10, 3, 0, 2};
    ARRAY8 b = Make(other, 4);
    CHECK(RememberIndirectClient(&b, FamilyInternet) == 1);
    CHECK(!IsIndirectClient(&a, FamilyInternet));
    CHECK(IsIndirectClient(&b, FamilyInternet));
}

static void SilentPeer(void)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int fd;

    Setup();
    signal(SIGALRM, Unreachable);
    alarm(8);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    strcpy(address.sun_path, SocketPath());
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
    /* a connection that says nothing does not hold the manager up for long */
    CHECK(test_Chooser(chooserFd) == 1);
    alarm(0);
    close(fd);
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"socket-of-the-manager", SocketOfTheManager},
    {"socket-file-removed", SocketFileRemoved},
    {"argument-names-the-socket", ArgumentNamesTheSocket},
    {"choice-needs-a-chooser", ChoiceNeedsAChooser},
    {"long-address-reported", LongAddressReported},
    {"indirect-clients-are-bounded", IndirectClientsAreBounded},
    {"indirect-clients-expire", IndirectClientsExpire},
    {"silent-peer", SilentPeer},
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

names = ["socket-of-the-manager", "socket-file-removed", "argument-names-the-socket", "choice-needs-a-chooser",
         "long-address-reported", "indirect-clients-are-bounded", "indirect-clients-expire", "silent-peer"]
failed = run_all("login-chooser-",
                 {"harness.h": harness, "socket_unit.c": socket_unit, "netaddr_unit.c": netaddr_unit,
                  "choose_unit.c": choose_unit, "stubs.c": stubs},
                 [("socket_unit", ["-w"]), ("netaddr_unit", ["-w"]), ("choose_unit", ["-w"]),
                  ("stubs", ["-Wall", "-Wextra", "-Wno-unused-parameter"])],
                 ["-lXdmcp"], names)
finish(failed, "Chooser", "socket, choices and indirect clients")
