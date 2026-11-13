/*
 * Fuzz target: the XDMCP request handlers of dtlogin (dtlogin/xdmcp.c).
 *
 * run-fuzz.py extracts ProcessRequestSocket and the handlers it dispatches to
 * (query, broadcast, indirect, forward, request, manage, keep-alive) and the
 * reply writers from xdmcp.c, and compiles the whole of netaddr.c, policy.c,
 * protodpy.c and xdmauth.c and the authorization code of auth.c with them.
 * The packet decoding is done by libXdmcp (XQuartz's copy, which is not
 * instrumented: only the handlers are).
 *
 * What is stubbed: the UDP socket (XdmcpFill hands out the fuzz bytes and
 * XdmcpFlush discards the reply), the display list, the indirect-client and
 * chooser tables, the resolver (the display name is built from the address),
 * the logs and the authorization key generators.
 *
 * Input layout: byte 0 selects the peer and the state of the display list,
 * the rest is the datagram.
 *   bits 0-1  peer address: 10.1.2.3, 127.0.0.1, 0.0.0.0 or 255.255.255.255
 *   bit 2     FindDisplayBySessionID and FindDisplayByAddress find a display
 *   bit 3     FindDisplayByName finds a display of XDMCP origin
 *   bit 4     the peer is a known indirect client
 *   bit 5     the display found is running
 */
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <arpa/inet.h>
#include "dm.h"

extern size_t __sanitizer_get_current_allocated_bytes(void);

/* the stand-ins must exist before the production text refers to them */
int xdmcpFd = -1;
int chooserFd = -1;
char *keyFile = "/nonexistent";
int removeDomainname;
int debugLevel;

static uint8_t state_flags;
static const uint8_t *datagram;
static size_t datagram_size;
static struct sockaddr_storage peer_address;
static int peer_length;

#define XdmcpFill fuzz_XdmcpFill
#define XdmcpFlush fuzz_XdmcpFlush
/* the willing message asks "uptime" of a shell; nothing is run here */
static FILE *no_popen(const char *command, const char *type) { (void)command; (void)type; return NULL; }
#define popen no_popen

static int fuzz_XdmcpFill(int fd, XdmcpBufferPtr b, XdmcpNetaddr from, int *fromlen)
{
    (void)fd;
    if (!b->data) { b->data = calloc(1, XDM_MAX_MSGLEN); b->size = XDM_MAX_MSGLEN; }
    memcpy(b->data, datagram, datagram_size);
    b->count = (int)datagram_size;
    b->pointer = 0;
    memcpy(from, &peer_address, peer_length < *fromlen ? (size_t)peer_length : (size_t)*fromlen);
    *fromlen = peer_length;
    return 1;
}
static int fuzz_XdmcpFlush(int fd, XdmcpBufferPtr b, XdmcpNetaddr to, int tolen)
{ (void)fd; (void)b; (void)to; (void)tolen; return 1; }

void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
int AcceptableDisplayAddress(ARRAY8Ptr a,
#if NeedWidePrototypes
                             int type,
#else
                             CARD16 type,
#endif
                             xdmOpCode op)
{ (void)a; (void)type; (void)op; return 1; }
char *localHostname(void) { return "thishost"; }
int GenerateAuthData(char *data, int len) { memset(data, 0x5a, len); return 1; }
void MitInitAuth(
#if NeedWidePrototypes
    unsigned int length,
#else
    unsigned short length,
#endif
    char *name) { (void)length; (void)name; }
Xauth *MitGetAuth(
#if NeedWidePrototypes
    unsigned int length,
#else
    unsigned short length,
#endif
    char *name)
{
    Xauth *a = calloc(1, sizeof *a);
    a->name = malloc(length ? length : 1); memcpy(a->name, name, length); a->name_length = length;
    a->data = malloc(16); memset(a->data, 0x33, 16); a->data_length = 16;
    return a;
}

/* the display list: one display, created on demand, and the ones manage() makes */
static struct display found_display;
static struct display *created[8];
static int created_count;
struct display *FindDisplayBySessionID(CARD32 id)
{ (void)id; return (state_flags & 4) ? &found_display : NULL; }
struct display *FindDisplayByAddress(struct sockaddr *a, int len,
#if NeedWidePrototypes
                                     int n)
#else
                                     CARD16 n)
#endif
{ (void)a; (void)len; (void)n; return (state_flags & 4) ? &found_display : NULL; }
struct display *FindDisplayByName(char *name)
{ (void)name; return (state_flags & 8) ? &found_display : NULL; }
struct display *NewDisplay(char *name, char *class)
{
    struct display *d;
    if (created_count == 8) return NULL;
    d = calloc(1, sizeof *d);
    d->name = strdup(name);
    d->class = class ? strdup(class) : NULL;
    created[created_count++] = d;
    return d;
}
void RemoveDisplay(struct display *d)
{
    for (int i = 0; i < created_count; i++)
        if (created[i] == d) { created[i] = created[--created_count]; break; }
    free(d->name); free(d->class); free(d->from); free(d->clientAddr.data);
    free(d->authorizations);
    free(d);
}
int StartDisplay(struct display *d) { (void)d; return 0; }
void StopDisplay(struct display *d) { (void)d; }

/* indirect clients and the chooser */
int RememberIndirectClient(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; return 1; }
void ForgetIndirectClient(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; }
int IsIndirectClient(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; return (state_flags & 0x10) != 0; }
int AwaitIndirectChoice(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; return 1; }
int ForEachMatchingIndirectHost(ARRAY8Ptr a,
#if NeedWidePrototypes
                                int t,
#else
                                CARD16 t,
#endif
                                void (*f)(CARD16, struct _ARRAY8 *, char *), char *closure)
{
    static unsigned char other[4] = {10, 9, 8, 7};
    ARRAY8 target = {4, other};
    (void)a; (void)t;
    f(FamilyInternet, &target, closure);
    return 1;
}
int UseChooser(ARRAY8Ptr a,
#if NeedWidePrototypes
               int t)
#else
               CARD16 t)
#endif
{ (void)a; (void)t; return 1; }

/* the resolver: the display name is built from the address */
char *NetworkAddressToName(
#if NeedWidePrototypes
    int connectionType,
#else
    CARD16 connectionType,
#endif
    ARRAY8Ptr connectionAddress,
#if NeedWidePrototypes
    int displayNumber)
#else
    CARD16 displayNumber)
#endif
{
    char *name = malloc(16 * 4 + 32);
    size_t used = 0;
    (void)connectionType;
    for (int i = 0; i < connectionAddress->length && i < 16; i++)
        used += (size_t)sprintf(name + used, "%s%d", i ? "." : "", connectionAddress->data[i]);
    sprintf(name + used, ":%d", displayNumber);
    return name;
}
char *NetworkAddressToHostname(
#if NeedWidePrototypes
    int connectionType,
#else
    CARD16 connectionType,
#endif
    ARRAY8Ptr connectionAddress)
{ (void)connectionType; (void)connectionAddress; return strdup("peer.example"); }

#include FUZZ_PRODUCTION_H

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    static unsigned char hostname[] = "fuzzhost";
    (void)argc; (void)argv;
    Hostname.length = sizeof hostname - 1;
    Hostname.data = hostname;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) return 0;
    state_flags = data[0];
    datagram = data + 1;
    datagram_size = size - 1;

    /* the request socket is a UDP socket of the Internet family: the peer is an IPv4 address */
    static const uint32_t peers[4] = {0x0a010203, 0x7f000001, 0x00000000, 0xffffffff};
    memset(&peer_address, 0, sizeof peer_address);
    struct sockaddr_in *in = (struct sockaddr_in *)&peer_address;
    in->sin_family = AF_INET;
    in->sin_port = htons(40000);
    in->sin_addr.s_addr = htonl(peers[state_flags & 3]);
    peer_length = sizeof *in;
    memset(&found_display, 0, sizeof found_display);
    found_display.name = "found:0";
    found_display.sessionID = 1;
    found_display.status = (state_flags & 0x20) ? running : notRunning;
    found_display.displayType.origin = FromXDMCP;
    found_display.from = (struct sockaddr *)&peer_address;
    found_display.fromlen = peer_length;

    ProcessRequestSocket();

    while (created_count > 0) RemoveDisplay(created[0]);
    return 0;
}
