#!/usr/bin/env python3
"""Exercise the XDMCP request handlers with hostile requests under ASan and UBSan.

The production policy, protocol display, XDM authorization and address
conversion code is compiled with stubs for everything outside of it and
driven with the arrays a remote display can send.  Every scenario runs in its
own process and must neither crash nor be flagged by a sanitizer.  Set
DTLOGIN_SOURCE to test another copy of the sources.
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

policy = read("policy.c")
protodpy = read("protodpy.c")
xdmauth = read("xdmauth.c")
auth = read("auth.c")
xdmcp = read("xdmcp.c")

authorization = until(auth, "struct AuthProtocol {", "static Xauth *\nGenerateAuthorization")
authorization += block(auth, "#if NeedWidePrototypes\nvoid\nSetProtoDisplayAuthorization")
address = re.search(r"^#define getString\(name,len\).*$", xdmcp, re.M).group(0) + "\n"
address += block(xdmcp, "#if NeedWidePrototypes\nchar *\nNetworkAddressToName")
lookup_start = "/*\n * The name of a host is asked of the resolver"
if lookup_start in xdmcp:
    address += until(xdmcp, lookup_start, "#if NeedWidePrototypes\nchar *\nNetworkAddressToHostname").replace(
        "#define HOSTNAME_LOOKUP_TIMEOUT\t\t2", "#define HOSTNAME_LOOKUP_TIMEOUT\t\t1")
address += block(xdmcp, "#if NeedWidePrototypes\nchar *\nNetworkAddressToHostname")

harness = r'''
#include "dm.h"
#include <netdb.h>

/* heap bytes in use, from the sanitizer runtime (gcc ships no header for it) */
extern size_t __sanitizer_get_current_allocated_bytes(void);
extern int mode_popen_fails;
extern const char *uptime_output;
extern int used_tmpnam, used_system, used_popen;
FILE *StubPopen(const char *command, const char *type);
int StubPclose(FILE *stream);
int StubSystem(const char *command);
char *StubTmpnam(char *name);
struct hostent *StubGethostbyaddr(const void *addr, socklen_t length, int type);
char *test_WillingMsg(void);
'''

SELECT_CALL = "SelectConnectionTypeIndex(types, addresses, peer)" if "peerAddress" in policy else "(void)peer, SelectConnectionTypeIndex(types, addresses)"

production = r'''
#include "harness.h"
#include <sys/mman.h>
#define popen StubPopen
#define pclose StubPclose
#define system StubSystem
#define tmpnam StubTmpnam
#define gethostbyaddr StubGethostbyaddr
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
''' + "\n".join((policy, protodpy, xdmauth, authorization, address)) + r'''
char *test_WillingMsg(void) { return WillingMsg(); }
'''

stubs = r'''
#define _GNU_SOURCE
#include "harness.h"
#include <sys/mman.h>
#include <signal.h>
#include <time.h>

int mode_popen_fails;
const char *uptime_output = "";
int used_tmpnam, used_system, used_popen;
char *keyFile = "/nonexistent";
int removeDomainname;
int debugLevel;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
int addressEqual(XdmcpNetaddr a1, int len1, XdmcpNetaddr a2, int len2)
{ return len1 == len2 && memcmp(a1, a2, len1) == 0; }
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

void MitInitAuth(unsigned short length, char *name) { (void)length; (void)name; }
Xauth *MitGetAuth(unsigned short length, char *name)
{
    Xauth *a = calloc(1, sizeof *a);
    a->name = malloc(length); memcpy(a->name, name, length); a->name_length = length;
    a->data = malloc(16); memset(a->data, 0x33, 16); a->data_length = 16;
    return a;
}

/* uptime is replaced by canned output, and tmpnam/system by a shell stand-in */
static FILE *Canned(void)
{
    FILE *f = tmpfile();
    CHECK(f && fputs(uptime_output, f) >= 0 && fseek(f, 0, SEEK_SET) == 0);
    return f;
}
FILE *StubPopen(const char *command, const char *type)
{
    ++used_popen;
    CHECK(strstr(command, "uptime") && strcmp(type, "r") == 0);
    return mode_popen_fails ? NULL : Canned();
}
int StubPclose(FILE *stream) { return fclose(stream); }
char *StubTmpnam(char *name) { used_tmpnam = 1; strcpy(name, "tmpnam.out"); return name; }
int StubSystem(const char *command)
{
    const char *target = strstr(command, "> ");
    ++used_system;
    CHECK(target);
    FILE *out = fopen(target + 2, "w");
    CHECK(out);
    fputs(uptime_output, out);
    fclose(out);
    return 0;
}

/* 10.0.0.5 is host5.example, 10.0.0.9 takes half a minute to answer, the others have no name */
static int *lookups;
struct hostent *StubGethostbyaddr(const void *addr, socklen_t length, int type)
{
    static struct hostent h;
    const unsigned char *a = addr;
    (void)length; (void)type;
    if (lookups) ++*lookups;
    if (a[3] == 9) sleep(30);
    if (a[3] == 5) { h.h_name = "host5.example"; return &h; }
    return NULL;
}

static ARRAY8 Make(const void *bytes, int length)
{
    ARRAY8 a = { length, length ? memcpy(malloc(length), bytes, length) : NULL };
    return a;
}

/* the selection of the connection address, with the sender's address when the policy asks for it */
static int Select(ARRAY16Ptr types, ARRAYofARRAY8Ptr addresses, ARRAY8Ptr peer)
{
    return @SELECT@;
}

static void SelectIndex(void)
{
    static const unsigned char loopback[4] = {127, 0, 0, 1}, routable[4] = {10, 1, 2, 3};
    ARRAY8 peer = { 4, (CARD8Ptr)routable };
    CARD16 types[2] = {FamilyInternet, FamilyInternet};
    ARRAY16 typeList = {2, types};
    ARRAY8 addresses[2];
    ARRAYofARRAY8 addressList = {2, addresses};

    /* a lone loopback address leaves nothing to select; the old index was the array length */
    typeList.length = 1; addressList.length = 1;
    addresses[0] = Make(loopback, 4);
    CHECK(Select(&typeList, &addressList, &peer) == -1);
    free(addresses[0].data);

    /* the first routable IPv4 address is chosen */
    typeList.length = 2; addressList.length = 2;
    addresses[0] = Make(loopback, 4);
    addresses[1] = Make(routable, 4);
    CHECK(Select(&typeList, &addressList, &peer) == 1);
    free(addresses[0].data); free(addresses[1].data);

    /* an address of the wrong size for its family is no address */
    typeList.length = 1; addressList.length = 1;
    addresses[0] = Make(routable, 2);
    CHECK(Select(&typeList, &addressList, &peer) == -1);
    free(addresses[0].data);
    addresses[0] = Make(routable, 0);
    CHECK(Select(&typeList, &addressList, &peer) == -1);

    /* only the Internet family can be named */
    types[0] = FamilyChaos; types[1] = FamilyInternet;
    typeList.length = 2; addressList.length = 2;
    addresses[0] = Make(routable, 4);
    addresses[1] = Make(routable, 4);
    CHECK(Select(&typeList, &addressList, &peer) == 1);
    free(addresses[0].data); free(addresses[1].data);
}

static void RequestNamesTheSender(void)
{
    static const unsigned char loopback[4] = {127, 0, 0, 1}, sender[4] = {10, 1, 2, 3}, other[4] = {10, 9, 9, 9};
    CARD16 types[2] = {FamilyInternet, FamilyInternet};
    ARRAY16 typeList = {2, types};
    ARRAY8 addresses[2];
    ARRAYofARRAY8 addressList = {2, addresses};
    ARRAY8 peer = { 4, (CARD8Ptr)sender }, local = { 4, (CARD8Ptr)loopback }, odd = { 2, (CARD8Ptr)sender };

    /* a display is reached at the address it names: the request names the one it comes from */
    addresses[0] = Make(other, 4);
    addresses[1] = Make(sender, 4);
    CHECK(Select(&typeList, &addressList, &peer) == 1);
    free(addresses[1].data);
    addressList.length = 1; typeList.length = 1;
    CHECK(Select(&typeList, &addressList, &peer) == -1);   /* only another host's address */
    CHECK(Select(&typeList, &addressList, &odd) == -1);
    CHECK(Select(&typeList, &addressList, NULL) == -1);
    /* a request from this host may name any address of it */
    CHECK(Select(&typeList, &addressList, &local) == 0);
    free(addresses[0].data);
}

static void Watchdog(int sig)
{
    static const char m[] = "FAIL blocked for more than the time allowed\n";
    (void)sig;
    if (write(2, m, sizeof m - 1)) { }
    _exit(1);
}

static double Now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void HostnameLookup(void)
{
    static const unsigned char named[4] = {10, 0, 0, 5}, slow[4] = {10, 0, 0, 9}, nameless[4] = {10, 0, 0, 7};
    ARRAY8 address;
    char *name;
    double start;

    lookups = mmap(NULL, sizeof *lookups, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(lookups != MAP_FAILED);
    signal(SIGALRM, Watchdog);
    alarm(10);

    address = Make(named, 4);
    name = NetworkAddressToHostname(FamilyInternet, &address);
    CHECK(name && strcmp(name, "host5.example") == 0);
    free(name);
    CHECK(*lookups == 1);
    name = NetworkAddressToHostname(FamilyInternet, &address);        /* kept: the resolver is not asked again */
    CHECK(name && strcmp(name, "host5.example") == 0 && *lookups == 1);
    free(name);
    free(address.data);

    /* a resolver that does not answer does not hold the manager up: the address stands for the name */
    address = Make(slow, 4);
    start = Now();
    name = NetworkAddressToHostname(FamilyInternet, &address);
    CHECK(name && strcmp(name, "10.0.0.9") == 0);
    CHECK(Now() - start < 4.0);
    free(name);
    start = Now();
    name = NetworkAddressToHostname(FamilyInternet, &address);        /* and it is not asked again for a while */
    CHECK(name && strcmp(name, "10.0.0.9") == 0 && Now() - start < 0.5);
    free(name);
    free(address.data);

    address = Make(nameless, 4);
    name = NetworkAddressToHostname(FamilyInternet, &address);
    CHECK(name && strcmp(name, "10.0.0.7") == 0);
    free(name);
    free(address.data);
    alarm(0);
}

static void DisplayName(void)
{
    static const unsigned char routable[4] = {10, 0, 0, 5};
    ARRAY8 address;
    char *name;

    address = Make(routable, 2);
    CHECK(NetworkAddressToName(FamilyInternet, &address, 3) == NULL);
    CHECK(NetworkAddressToHostname(FamilyInternet, &address) == NULL);
    free(address.data);
    address = Make(routable, 0);
    CHECK(NetworkAddressToName(FamilyInternet, &address, 3) == NULL);
    CHECK(NetworkAddressToHostname(FamilyInternet, &address) == NULL);
    address = Make(routable, 4);
    name = NetworkAddressToName(FamilyInternet, &address, 3);
    CHECK(name && strcmp(name, "host5.example:3") == 0);
    free(name);
    name = NetworkAddressToHostname(FamilyInternet, &address);
    CHECK(name && strcmp(name, "host5.example") == 0);
    free(name);
    free(address.data);
}

static void AuthenticationName(void)
{
    static const char exact[] = "XDM-AUTHENTICATION-1";
    ARRAY8 name, data = Make("", 0);

    /* only the exact name selects XDM-AUTHENTICATION-1 (no key file: refused) */
    name = Make(exact, 3);
    CHECK(CheckAuthentication(NULL, &data, &name, &data));
    free(name.data);
    name = Make("XDM-AUTHENTICATION-1xyz", 23);
    CHECK(CheckAuthentication(NULL, &data, &name, &data));
    free(name.data);
    name = Make(exact, 20);
    CHECK(!CheckAuthentication(NULL, &data, &name, &data));
    free(name.data);
}

static struct protoDisplay *NewPending(void)
{
    struct sockaddr_in peer = { .sin_family = AF_INET };
    ARRAY8 address = Make("\n\0\0\5", 4);
    struct protoDisplay *pdpy = NewProtoDisplay((struct sockaddr *)&peer, sizeof peer, 0,
                                                FamilyInternet, &address, 1);
    free(address.data);
    CHECK(pdpy);
    return pdpy;
}

static void FreshDisplay(void)
{
    static const char zero[sizeof(XdmAuthKeyRec)];
    size_t before = __sanitizer_get_current_allocated_bytes();
    struct protoDisplay *pdpy = NewPending();

    CHECK(pdpy->authenticationData.length == 0 && pdpy->authenticationData.data == NULL);
    CHECK(pdpy->authenticationName.length == 0 && pdpy->authenticationName.data == NULL);
    CHECK(memcmp(&pdpy->key, zero, sizeof zero) == 0);
    /* what the XDM handshake leaves behind is released with the display */
    CHECK(XdmcpCopyARRAY8(&(ARRAY8){8, (CARD8 *)"12345678"}, &pdpy->authenticationData));
    DisposeProtoDisplay(pdpy);
    CHECK(__sanitizer_get_current_allocated_bytes() == before);
}

static void XdmAuthorizationWithoutAuthentication(void)
{
    struct protoDisplay *pdpy = NewPending();

    SetProtoDisplayAuthorization(pdpy, 19, "XDM-AUTHORIZATION-1");
    CHECK(pdpy->fileAuthorization == NULL && pdpy->xdmcpAuthorization == NULL);
    /* after the handshake the authorization is made from the shared number */
    CHECK(XdmcpCopyARRAY8(&(ARRAY8){8, (CARD8 *)"12345678"}, &pdpy->authenticationData));
    SetProtoDisplayAuthorization(pdpy, 19, "XDM-AUTHORIZATION-1");
    CHECK(pdpy->fileAuthorization && pdpy->xdmcpAuthorization);
    CHECK(pdpy->fileAuthorization->data_length == 16 &&
          memcmp(pdpy->fileAuthorization->data, "12345678", 8) == 0);
    DisposeProtoDisplay(pdpy);
}

static void RepeatedRequest(void)
{
    struct protoDisplay *pdpy = NewPending();
    size_t before, after;

    SetProtoDisplayAuthorization(pdpy, 18, "MIT-MAGIC-COOKIE-1");
    CHECK(pdpy->fileAuthorization);
    before = __sanitizer_get_current_allocated_bytes();
    for (int i = 0; i < 5; ++i)
        SetProtoDisplayAuthorization(pdpy, 18, "MIT-MAGIC-COOKIE-1");
    after = __sanitizer_get_current_allocated_bytes();
    CHECK(after == before);
    DisposeProtoDisplay(pdpy);
}

static void ExpectStatus(const char *output, const char *expected)
{
    uptime_output = output;
    used_popen = used_system = used_tmpnam = 0;
    CHECK(strcmp(test_WillingMsg(), expected) == 0);
}

static void WillingStatus(void)
{
    /* a command that prints nothing keeps the default and copies no command line */
    ExpectStatus("", "Willing to Manage");
    ExpectStatus("3 users  load: 0.1, 0.2, 0.3\n", "3 users  load: 0.1, 0.2, 0.3");
    ExpectStatus("short\n", "Willing to Manage");
    ExpectStatus("a line that is a good deal longer than the status field\n", "a line that is a good deal long");
    mode_popen_fails = 1;
    ExpectStatus("", "Willing to Manage");
}

static void NoTemporaryFile(void)
{
    ExpectStatus("3 users  load: 0.1, 0.2, 0.3\n", "3 users  load: 0.1, 0.2, 0.3");
    /* the output must not travel through a file named in a shared directory */
    CHECK(!used_tmpnam && !used_system);
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"select-index", SelectIndex},
    {"request-names-the-sender", RequestNamesTheSender},
    {"hostname-lookup", HostnameLookup},
    {"display-name", DisplayName},
    {"authentication-name", AuthenticationName},
    {"fresh-display", FreshDisplay},
    {"xdm-authorization-without-authentication", XdmAuthorizationWithoutAuthentication},
    {"repeated-request", RepeatedRequest},
    {"willing-status", WillingStatus},
    {"willing-no-temporary-file", NoTemporaryFile},
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

stubs = stubs.replace("@SELECT@", SELECT_CALL)

def run_all(prefix, harness_h, production_c, stubs_c, libs, defines, scenarios):
    with tempfile.TemporaryDirectory(prefix=prefix, dir=os.environ.get("TMPDIR")) as work:
        work = Path(work)
        (work / "harness.h").write_text(harness_h)
        (work / "production.c").write_text(production_c)
        (work / "stubs.c").write_text(stubs_c)
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-std=c23", "-D_DEFAULT_SOURCE", "-DUNIXCONN", "-DTCPCONN", "-DXDMCP", "-DHASXDMAUTH",
                 "-DANSICPP", "-I", str(work), "-I", str(source), "-I", str(source.parents[1] / "include"),
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                 "-g", "-O1"] + defines
        objects = []
        for unit, extra in (("production", ["-w"]), ("stubs", ["-Wall", "-Wextra", "-Wno-unused-parameter"])):
            obj = work / (unit + ".o")
            subprocess.run(cc + flags + extra + ["-c", str(work / (unit + ".c")), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = work / "scenarios"
        subprocess.run(cc + flags + objects + ["-o", str(binary)] + libs, check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0",
                   UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
        failed = 0
        for name in scenarios:
            done = subprocess.run([str(binary), name], cwd=work, env=env, capture_output=True, text=True)
            lines = (done.stdout + done.stderr).splitlines()
            reason = next((l for l in lines if l.startswith("FAIL") or "ERROR:" in l or "runtime error" in l),
                          lines[0] if lines else "exit status %d" % done.returncode)
            frame = next((l.strip() for l in lines if re.match(r"\s*#[0-4] .* in \w+ .*(production|stubs)\.c", l)), "")
            if done.returncode:
                failed += 1
                print("FAIL %s: %s %s" % (name, reason.strip(), frame))
            else:
                print("ok   %s" % name)
        return failed

names = ["select-index", "request-names-the-sender", "hostname-lookup", "display-name", "authentication-name", "fresh-display",
         "xdm-authorization-without-authentication", "repeated-request",
         "willing-status", "willing-no-temporary-file"]
failed = run_all("login-xdmcp-", harness, production, stubs, ["-lXdmcp", "-lXau"], [], names)
if failed:
    sys.exit("XDMCP requests: %d scenario(s) failed" % failed)
print("XDMCP requests: connection selection, address names, authentication, protocol displays and status passed")
