/*
 * Fuzz target: the Xaccess file reader of dtlogin and the access checks the
 * XDMCP code makes against it (dtlogin/access.c).
 *
 * run-fuzz.py includes access.c whole, as dtlogin/tests/check-config.py does.
 * The stand-ins are the logging functions, the catalog, the resolver
 * (gethostbyname: names that start with "bad" do not resolve, a dotted number
 * is its own address, any other name is 10.1.1.1), the reverse lookup of a
 * client address (it returns the client name of the input), the chooser's
 * choice, and the globals of the display manager.
 *
 * Input layout: byte 0 is the length N (0-15) of the client host name, then N
 * bytes of that name (the reverse lookup of an attacker's address can return
 * anything), then the content of the Xaccess file.  The harness reads the
 * file with ScanAccessDatabase and then asks, as the XDMCP handlers do:
 * AcceptableDisplayAddress for a query and a direct query,
 * ForEachMatchingIndirectHost, ForEachChooserHost and UseChooser for the
 * client 10.0.0.2 and for a client of 4 bytes with the name of the input.
 */
#include <assert.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "dm.h"

int debugLevel, wakeupTime = -1, errors;
char *accessFile, *keyFile;
static char client_name[16];
static struct hostent *fuzz_gethostbyname(const char *name);

#include FUZZ_TYPES_H
#include FUZZ_PRODUCTION_H

void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; ++errors; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
char *localHostname(void) { return "thishost"; }
char *NetworkAddressToHostname(CARD16 type, ARRAY8Ptr address)
{
    (void)type; (void)address;
    return strdup(client_name);
}
int addressEqual(XdmcpNetaddr a1, int len1, XdmcpNetaddr a2, int len2)
{ return len1 == len2 && memcmp(a1, a2, len1) == 0; }
ARRAY8Ptr IndirectChoice(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; return NULL; }

static struct hostent *fuzz_gethostbyname(const char *name)
{
    static struct hostent h;
    static unsigned char address[4];
    static char *list[2] = { (char *)address, NULL };
    unsigned a, b, c, d;

    if (strncmp(name, "bad", 3) == 0) return NULL;
    if (sscanf(name, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        address[0] = a; address[1] = b; address[2] = c; address[3] = d;
    } else {
        address[0] = 10; address[1] = 1; address[2] = 1; address[3] = 1;
    }
    h.h_name = (char *)name; h.h_addrtype = AF_INET; h.h_length = 4; h.h_addr_list = list;
    return &h;
}

static void Count(CARD16 type, ARRAY8Ptr host, char *closure) { (void)type; (void)host; (void)closure; }

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    const char *tmp = getenv("TMPDIR");
    static char path[1024];
    snprintf(path, sizeof path, "%s/Xaccess.%d", tmp ? tmp : ".", (int)getpid());
    accessFile = path;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) return 0;
    size_t n = data[0] & 15;
    if (size < 1 + n) return 0;
    memset(client_name, 0, sizeof client_name);
    memcpy(client_name, data + 1, n);          /* cut at the first NUL */
    data += 1 + n; size -= 1 + n;

    FILE *f = fopen(accessFile, "w");
    if (!f) abort();
    if (size) fwrite(data, 1, size, f);
    fclose(f);

    ScanAccessDatabase();
    unsigned char a1[4] = { 10, 0, 0, 2 }, a2[4] = { 127, 0, 0, 1 };
    ARRAY8 clients[2] = { { 4, a1 }, { 4, a2 } };
    for (int i = 0; i < 2; i++) {
        AcceptableDisplayAddress(&clients[i], FamilyInternet, QUERY);
        AcceptableDisplayAddress(&clients[i], FamilyInternet, BROADCAST_QUERY);
        ForEachMatchingIndirectHost(&clients[i], FamilyInternet, Count, NULL);
        ForEachChooserHost(&clients[i], FamilyInternet, Count, NULL);
        UseChooser(&clients[i], FamilyInternet);
    }
    return 0;
}
