#!/usr/bin/env python3
"""Exercise the display list and the XDMCP Manage request with failing allocations and rival peers.

The production dpylist.c and netaddr.c are compiled as they are; the Manage handler of xdmcp.c is cut
out of its file and wrapped with stand-ins.  The scenarios run under ASan and UBSan, each in a
process of its own.  Set DTLOGIN_SOURCE to test another copy of the sources.
"""
import re
from scenarios import block, finish, read, run_all

xdmcp = read("xdmcp.c")
util = read("util.c")


def first_block(text, starts):
    """The first of several spellings of a function (the sources of before and after a change)."""
    for start in starts:
        if start in text:
            return block(text, start)
    return ""


client_address = first_block(xdmcp, ["static int\nClientAddress (", "static void\nClientAddress ("])
same_host = first_block(xdmcp, ["/* are the two network addresses those of one host, whatever their ports? */\nstatic int\nSameHost"])
manage = block(xdmcp, "void\nmanage (struct sockaddr *from, int fromlen, int length)\n{")

harness = r'''
#include "dm.h"
#include <netdb.h>
#include <sys/mman.h>
#include <arpa/inet.h>

extern size_t __sanitizer_get_current_allocated_bytes(void);
extern int fail_size, live_maps, stopped, started, failed_sent, refused_sent, disposed;
extern struct protoDisplay *current_pdpy;
extern struct display *displays;
extern const char *display_name;
void *test_malloc(size_t size);
void *test_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int test_munmap(void *addr, size_t length);
void test_prepare(unsigned sessionID, unsigned short number, const char *class, int *length);
void test_manage(struct sockaddr *from, int fromlen, int length);
'''

dpylist_unit = r'''
#include "harness.h"
#define malloc test_malloc
#define mmap test_mmap
#define munmap test_munmap
#include "dpylist.c"
'''

netaddr_unit = r'''
#include "harness.h"
#include "netaddr.c"
'''

manage_unit = r'''
#include "harness.h"
#define malloc test_malloc
static XdmcpBuffer buffer;
int NetaddrFamily(XdmcpNetaddr netaddrp);
void SendFailed(struct display *d, char *reason);
static void send_failed (struct sockaddr *from, int fromlen, char *name, CARD32 sessionID, char *reason)
{ (void)from; (void)fromlen; (void)name; (void)sessionID; (void)reason; ++failed_sent; }
static void send_refuse (struct sockaddr *from, int fromlen, CARD32 sessionID)
{ (void)from; (void)fromlen; (void)sessionID; ++refused_sent; }
char *NetworkAddressToName(CARD16 connectionType, ARRAY8Ptr connectionAddress, CARD16 displayNumber)
{ (void)connectionType; (void)connectionAddress; (void)displayNumber; return strdup(display_name); }
struct protoDisplay *FindProtoDisplay(struct sockaddr *from, int fromlen, CARD16 displayNumber)
{ (void)from; (void)fromlen; (void)displayNumber; return current_pdpy; }
int DisposeProtoDisplay(struct protoDisplay *pdpy)
{ if (pdpy == current_pdpy) current_pdpy = NULL; ++disposed; return 0; }
static int IsIndirectClient(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; return 0; }
static void ForgetIndirectClient(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; }
int UseChooser(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; return 0; }
static int AwaitIndirectChoice(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; return 0; }
''' + client_address + same_host + manage + r'''
void test_prepare(unsigned sessionID, unsigned short number, const char *class, int *length)
{
    static BYTE bytes[256];
    ARRAY8 name = { (CARD16)strlen(class), (CARD8Ptr)class };

    buffer.data = bytes; buffer.size = sizeof bytes; buffer.pointer = 0; buffer.count = 0;
    XdmcpWriteCARD32(&buffer, sessionID);
    XdmcpWriteCARD16(&buffer, number);
    XdmcpWriteARRAY8(&buffer, &name);
    *length = buffer.pointer;
    buffer.count = buffer.pointer;
    buffer.pointer = 0;
}
void test_manage(struct sockaddr *from, int fromlen, int length) { manage(from, fromlen, length); }
'''

stubs = r'''
#include "harness.h"
#include <stdarg.h>

int debugLevel, wakeupTime = -1;
int fail_size, live_maps, stopped, started, failed_sent, refused_sent, disposed;
struct protoDisplay *current_pdpy;
const char *display_name = "10.0.0.5:0";

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }

/* an allocation of the size in fail_size fails once */
void *test_malloc(size_t size)
{
    if (fail_size && size == (size_t)fail_size) { fail_size = 0; return NULL; }
    return malloc(size);
}
void *test_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    void *p = mmap(addr, length, prot, flags, fd, offset);
    if (p != MAP_FAILED) ++live_maps;
    return p;
}
int test_munmap(void *addr, size_t length) { --live_maps; return munmap(addr, length); }

''' + block(util, "void\nfreeEnv (") + r'''
/* the manager of the display list asks for these when it starts a display */
void StopDisplay(struct display *d) { (void)d; ++stopped; }
int StartDisplay(struct display *d)
{
    /* every display of the list must still be there to be walked */
    for (struct display *p = displays; p; p = p->next) CHECK(p->name && p->name[0]);
    CHECK(d == displays);
    ++started;
    return 1;
}

static struct sockaddr_in Peer(const char *host, int port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    CHECK(inet_pton(AF_INET, host, &a.sin_addr) == 1);
    return a;
}

static struct protoDisplay *Pending(unsigned sessionID, int withAuthorization)
{
    static const unsigned char address[4] = {10, 0, 0, 5};
    struct protoDisplay *pdpy = calloc(1, sizeof *pdpy);
    CHECK(pdpy);
    pdpy->connectionType = FamilyInternet;
    pdpy->connectionAddress.length = 4;
    pdpy->connectionAddress.data = malloc(4);
    memcpy(pdpy->connectionAddress.data, address, 4);
    pdpy->sessionID = sessionID;
    if (withAuthorization) {
        pdpy->fileAuthorization = calloc(1, sizeof(Xauth));
        CHECK(pdpy->fileAuthorization);
    }
    return pdpy;
}

static void Request(const char *host, int port, unsigned sessionID)
{
    struct sockaddr_in from = Peer(host, port);
    int length;

    test_prepare(sessionID, 0, "XEPHYR", &length);
    test_manage((struct sockaddr *)&from, sizeof from, length);
}

/* an XDMCP display of the list, started from the given peer */
static struct display *Existing(const char *host, int port, int origin)
{
    struct sockaddr_in *from = malloc(sizeof *from);
    struct display *d = NewDisplay((char *)display_name, "XEPHYR");

    CHECK(d && from);
    *from = Peer(host, port);
    d->from = (struct sockaddr *)from;
    d->fromlen = sizeof *from;
    d->displayType.origin = origin;
    d->sessionID = 77;
    return d;
}

static void NewDisplayLeaks(void)
{
    /* each way NewDisplay can run out of memory gives back the shared page it mapped */
    int sizes[] = { (int)sizeof(struct display), (int)strlen("name") + 1, (int)strlen("class") + 1 };

    for (int i = 0; i < 3; ++i) {
        fail_size = sizes[i];
        CHECK(NewDisplay("name", "class") == NULL);
        CHECK(live_maps == 0);
        CHECK(displays == NULL);
    }
}

static void DisplayRelease(void)
{
    size_t before = __sanitizer_get_current_allocated_bytes();
    struct display *d = NewDisplay("host:0", "CLASS");

    CHECK(d);
    d->pmSearchPath = strdup("/pixmaps");
    d->bmSearchPath = strdup("/bitmaps");
    d->authNames = calloc(3, sizeof(char *));
    d->authNames[0] = strdup("MIT-MAGIC-COOKIE-1");
    d->authNames[1] = strdup("XDM-AUTHORIZATION-1");
    d->authNameLens = malloc(2 * sizeof(unsigned short));
    RemoveDisplay(d);
    CHECK(displays == NULL && live_maps == 0);
    /* everything the display held is released with it */
    CHECK(__sanitizer_get_current_allocated_bytes() == before);
}

static void ManageNoMemory(void)
{
    /* the display that cannot get its authorization is taken out of the list, not just freed */
    current_pdpy = Pending(55, 1);
    fail_size = sizeof(Xauth *);
    Request("10.0.0.5", 4000, 55);
    CHECK(failed_sent == 1 && started == 0);
    CHECK(displays == NULL);
    CHECK(FindDisplayByName("10.0.0.5:0") == NULL);
    /* the list is usable afterwards */
    CHECK(NewDisplay("other:0", NULL) != NULL);
    CHECK(displays && displays->next == NULL);
}

static void ManageStarts(void)
{
    struct display *d;

    current_pdpy = Pending(55, 1);
    Request("10.0.0.5", 4000, 55);
    CHECK(failed_sent == 0 && started == 1 && stopped == 0);
    d = FindDisplayByName("10.0.0.5:0");
    CHECK(d && d->displayType.origin == FromXDMCP && d->sessionID == 55 && d->authNum == 1);
}

static void ManageRestart(void)
{
    /* a display that comes back from the same host, from another port, replaces its old session */
    Existing("10.0.0.5", 4000, FromXDMCP);
    current_pdpy = Pending(55, 0);
    Request("10.0.0.5", 4001, 55);
    CHECK(stopped == 1 && failed_sent == 0);
}

static void ManageOtherHost(void)
{
    struct display *old = Existing("10.0.0.5", 4000, FromXDMCP);

    /* another host cannot end the session of a display by naming it */
    current_pdpy = Pending(55, 0);
    Request("10.0.0.9", 4000, 55);
    CHECK(stopped == 0 && started == 0 && failed_sent == 1);
    CHECK(FindDisplayByName("10.0.0.5:0") == old);
}

static void ManageConfiguredDisplay(void)
{
    struct display *old = Existing("10.0.0.5", 4000, FromFile);

    /* nor can it end a display of the configuration file */
    current_pdpy = Pending(55, 0);
    Request("10.0.0.5", 4000, 55);
    CHECK(stopped == 0 && started == 0 && failed_sent == 1);
    CHECK(FindDisplayByName("10.0.0.5:0") == old);
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"new-display-leaks", NewDisplayLeaks},
    {"display-release", DisplayRelease},
    {"manage-no-memory", ManageNoMemory},
    {"manage-starts", ManageStarts},
    {"manage-restart", ManageRestart},
    {"manage-other-host", ManageOtherHost},
    {"manage-configured-display", ManageConfiguredDisplay},
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

names = ["new-display-leaks", "display-release", "manage-no-memory", "manage-starts", "manage-restart",
         "manage-other-host", "manage-configured-display"]
failed = run_all("login-displays-",
                 {"harness.h": harness, "dpylist_unit.c": dpylist_unit, "netaddr_unit.c": netaddr_unit,
                  "manage_unit.c": manage_unit, "stubs.c": stubs},
                 [("dpylist_unit", ["-w"]), ("netaddr_unit", ["-w"]), ("manage_unit", ["-w"]),
                  ("stubs", ["-Wall", "-Wextra", "-Wno-unused-parameter"])],
                 ["-lXdmcp", "-lXau"], names)
finish(failed, "Display list", "allocation failures and Manage requests")
