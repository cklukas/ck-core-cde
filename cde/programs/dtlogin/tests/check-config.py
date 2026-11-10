#!/usr/bin/env python3
"""Feed the configuration readers of the login manager lines they must not trip over.

The production access.c (the Xaccess file), file.c (the Xservers file), qualify.c (file search
paths) and dpylist.c are compiled as they are, the language expansion and the loader of the
alternate desktop resources are cut out of session.c, and all of them run with stand-ins for the
resolver and the allocator under ASan and UBSan.  Every scenario runs in a process of its own.
Set DTLOGIN_SOURCE to test another copy of the sources.
"""
from scenarios import block, finish, read, run_all, until

session = read("session.c")
dm = read("dm.c")
resource = read("resource.c")
util = read("util.c")

expand_lang = block(session, "char *\n_ExpandLang(")
alt_start = "/* the most alternate desktops the resource files can add to the login screen */" \
    if "MAX_ALT_DESKTOPS" in session else "static void\nLoadAltDtsResources("
alt_resources = until(session, alt_start, "/******************\n *\n * Function Name:  _ExpandLang")
file_compare = block(session, "#if defined(__STDC__)\nstatic int\nFileNameCompare", "\n}\n")

get_resource = block(resource, "int \nGetResource(")
scan_servers = block(dm, "static int \nScanServers( void )")

harness = r'''
#include "dm.h"
#include <netdb.h>
#include <signal.h>
#include <unistd.h>

extern size_t __sanitizer_get_current_allocated_bytes(void);
extern int fail_size, fail_next;
extern const char *hostname_value;
extern int errors;
void *test_malloc(size_t size);
void *test_realloc(void *p, size_t size);
char *test_strdup(const char *s);
struct hostent *test_gethostbyname(const char *name);
int test_gethostname(char *name, size_t length);
int test_scan_access(const char *path);
int test_match(char *string, char *pattern);
int test_hosts_for(unsigned char *client, int length);
ARRAY8Ptr test_local_address(void);
int test_parse(char *line);
int test_scan_servers(void);
int test_get_resource(char *name, char *class, char **valuep, char *default_value);
void test_resource_db(const char *text);
extern char *servers;
char *test_qualify(char *file, char *path);
char *test_expand(char *string, char *lang);
void test_alt_desktops(struct display *d);
'''

access_unit = r'''
#include "harness.h"
#define gethostbyname test_gethostbyname
#include "access.c"

int test_scan_access(const char *path)
{
    free(accessFile);
    accessFile = strdup(path);
    return ScanAccessDatabase();
}
int test_match(char *string, char *pattern) { return patternMatch(string, pattern); }
ARRAY8Ptr test_local_address(void) { return getLocalAddress(); }

static int hosts_seen;
static void Count(CARD16 type, ARRAY8Ptr host, char *closure)
{ (void)type; (void)closure; hosts_seen += host->length == 4 && host->data[0] == 10; }

/* how many hosts does the database name for this client? */
int test_hosts_for(unsigned char *client, int length)
{
    ARRAY8 a = { length, client };
    hosts_seen = 0;
    ForEachMatchingIndirectHost(&a, FamilyInternet, Count, NULL);
    return hosts_seen;
}
'''

file_unit = r'''
#include "harness.h"
#define gethostname test_gethostname
#define gethostbyname test_gethostbyname
#include "file.c"

struct passwd puser;
int test_parse(char *line)
{
    static DisplayType acceptable[] = { { Local, Permanent, FromFile }, { Foreign, Permanent, FromFile } };
    return ParseDisplay(line, acceptable, 2, &puser);
}
'''

resource_unit = r'''
#include "harness.h"
#include "vgmsg.h"
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xmu/CharSet.h>
#define DM_STRING 0
#define DM_INT 1
#define DM_BOOL 2
#define DM_ARGV 3
XrmDatabase DmResourceDB;
''' + get_resource + r'''
void test_resource_db(const char *text) { DmResourceDB = XrmGetStringDatabase(text); }
int test_get_resource(char *name, char *class, char **valuep, char *default_value)
{ return GetResource(name, class, DM_STRING, valuep, default_value); }
'''

servers_unit = r'''
#include "harness.h"
#include "vgmsg.h"
#include <sys/stat.h>
extern struct passwd puser;
char *servers;
time_t ServersModTime;
''' + scan_servers + r'''
int test_scan_servers(void) { return ScanServers(); }
'''

dpylist_unit = r'''
#include "harness.h"
#include "dpylist.c"
'''

qualify_unit = r'''
#include "harness.h"
#define malloc test_malloc
#define strdup test_strdup
#include "qualify.c"
char *test_qualify(char *file, char *path) { return qualifyWithFirst(file, path); }
'''

session_unit = r'''
#include "harness.h"
#define malloc test_malloc
#define realloc test_realloc
#include <dirent.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#define DOT "."
#define DOTDOT ".."
XrmDatabase XresourceDB;
''' + file_compare + expand_lang + alt_resources + r'''
char *test_expand(char *string, char *lang) { return _ExpandLang(string, lang); }
void test_alt_desktops(struct display *d) { LoadAltDtsResources(d); }
'''

stubs = r'''
#define _GNU_SOURCE
#include "harness.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>

int debugLevel, wakeupTime = -1, errors;
int fail_size, fail_next;
char *accessFile, *keyFile;
const char *hostname_value = "thishost";
extern XrmDatabase XresourceDB;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

int parseArgs(char ***argv, const char *string) { (void)argv; (void)string; return 1; }
void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; ++errors; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
char *localHostname(void) { return (char *)hostname_value; }
char *NetworkAddressToHostname(CARD16 type, ARRAY8Ptr address) { (void)type; (void)address; return NULL; }
int addressEqual(XdmcpNetaddr a1, int len1, XdmcpNetaddr a2, int len2)
{ return len1 == len2 && memcmp(a1, a2, len1) == 0; }
ARRAY8Ptr IndirectChoice(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; return NULL; }
''' + block(util, "void\nfreeEnv (") + r'''
/* an allocation of the size in fail_size fails once */
void *test_malloc(size_t size)
{
    if (fail_next) { fail_next = 0; return NULL; }
    if (fail_size && size == (size_t)fail_size) { fail_size = 0; return NULL; }
    return malloc(size);
}
void *test_realloc(void *p, size_t size)
{
    if (fail_size && size == (size_t)fail_size) { fail_size = 0; return NULL; }
    return realloc(p, size);
}
char *test_strdup(const char *s)
{
    if (fail_size && strlen(s) + 1 == (size_t)fail_size) { fail_size = 0; return NULL; }
    return strdup(s);
}

/* names that start with "bad" do not resolve; a number is its own address; every other name is 10.1.1.1 */
struct hostent *test_gethostbyname(const char *name)
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
int test_gethostname(char *name, size_t length)
{
    snprintf(name, length, "%s", hostname_value);
    return 0;
}

static void Watchdog(int sig)
{
    static const char m[] = "FAIL blocked for more than the time allowed\n";
    (void)sig;
    if (write(2, m, sizeof m - 1)) { }
    _exit(1);
}

static void WriteFile(const char *name, const char *text)
{
    FILE *f = fopen(name, "w");
    CHECK(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static char *Heap(const char *s) { char *p = strdup(s); CHECK(p); return p; }

static void LongWord(void)
{
    char *line = malloc(1200), *text;
    unsigned char client[4] = {10, 0, 0, 2};

    hostname_value = "10.8.8.8";		/* this host is not one of the hosts of the file */

    /* a word of any length is cut off, not written over the buffer that holds it */
    memset(line, 'x', 1000);
    line[1000] = '\0';
    CHECK(asprintf(&text, "10.0.0.2 %s 10.0.0.7\n", line) > 0);
    WriteFile("Xaccess", text);
    CHECK(test_scan_access("Xaccess") == 1);
    CHECK(test_hosts_for(client, 4) == 2);
}

static void UnknownDisplay(void)
{
    unsigned char client[4] = {10, 0, 0, 2};

    /* a display that does not resolve costs its own entry, not the ones after it */
    WriteFile("Xaccess", "bad-display 10.0.0.9\n10.0.0.2 10.0.0.7\n");
    CHECK(test_scan_access("Xaccess") == 1);
    CHECK(test_hosts_for(client, 4) == 1);
}

static void PatternTime(void)
{
    char subject[120], pattern[120];
    int n;

    signal(SIGALRM, Watchdog);
    alarm(5);
    /* nested wildcards must not take longer than the strings are long */
    memset(subject, 'a', 60); subject[60] = '\0';
    n = 0;
    for (int i = 0; i < 12; ++i) { pattern[n++] = '*'; pattern[n++] = 'a'; }
    pattern[n++] = 'b'; pattern[n] = '\0';
    CHECK(test_match(subject, pattern) == 0);
    pattern[n - 1] = 'a';
    CHECK(test_match(subject, pattern) == 1);
    alarm(0);
}

static void PatternMeaning(void)
{
    char *p;

    CHECK(test_match("abc", "abc") && !test_match("abc", "abd") && !test_match("abc", "ab"));
    CHECK(test_match("abc", "a?c") && !test_match("ac", "a?c"));
    CHECK(test_match("abc", "a*") && test_match("a", "a*") && test_match("abc", "*c") && test_match("abc", "*"));
    CHECK(test_match("axxb", "a*b") && test_match("ab", "a*b") && !test_match("axxc", "a*b"));
    CHECK(test_match("a*c", "a\\*c") && !test_match("abc", "a\\*c"));
    CHECK(test_match(NULL, "*") && test_match("", "") && !test_match("", "a"));
    /* a backslash that ends the pattern quotes nothing: it matches nothing, and reads nothing beyond it */
    p = Heap("abc\\");
    CHECK(!test_match("abc", p));
    CHECK(!test_match("abcd", p));
    free(p);
}

static void LocalAddress(void)
{
    size_t before;
    ARRAY8Ptr a;

    /* a host whose name does not resolve has no address that could be compared */
    hostname_value = "bad-host";
    a = test_local_address();
    CHECK(a && a->length == 0);

    /* the address of a host that does resolve is looked up once and kept */
    hostname_value = "thishost";
    a = test_local_address();
    CHECK(a->length == 4 && a->data[0] == 10);
    before = __sanitizer_get_current_allocated_bytes();
    for (int i = 0; i < 20; ++i) test_local_address();
    CHECK(__sanitizer_get_current_allocated_bytes() == before);
}

static void ServersLocalUid(void)
{
    extern struct passwd puser;

    /* "local_uid" at the end of the line has no user name to read */
    CHECK(test_parse(Heap(":0 local_uid")) == 1);
    CHECK(FindDisplayByName(":0") != NULL);
    CHECK(test_parse(Heap(":1 local_uid root /usr/bin/X :1")) == 1);
    CHECK(FindDisplayByName(":1")->argv[0] != NULL && strcmp(FindDisplayByName(":1")->argv[0], "/usr/bin/X") == 0);
    (void) puser;
}

static void ServersClassAdded(void)
{
    /* a display that was listed without a class and is listed again with one */
    CHECK(test_parse(Heap(":0 local /usr/bin/X :0")) == 1);
    CHECK(test_parse(Heap(":0 CLASSNAME local /usr/bin/X :0")) == 1);
    CHECK(FindDisplayByName(":0")->class && strcmp(FindDisplayByName(":0")->class, "CLASSNAME") == 0);
}

static void ServersHostName(void)
{
    char name[400];

    /* the display "*" is this host's name and ":0"; both must fit however long the name is */
    memset(name, 'h', 127); name[127] = '\0';
    hostname_value = name;
    CHECK(test_parse(Heap("* local /usr/bin/X")) == 1);
}

static void ServersFile(void)
{
    FILE *f;

    /* a line that starts with a NUL byte, a line that does not fit and the lines after them */
    f = fopen("Xservers", "w");
    CHECK(f);
    fputs(":0 local /usr/bin/X :0\n", f);
    fputc('\0', f); fputs("garbage\n", f);
    for (int i = 0; i < 12000; ++i) fputc('x', f);
    fputs("\n:1 local /usr/bin/X :1\n", f);
    CHECK(fclose(f) == 0);
    {
        char cwd[256], path[400];
        CHECK(getcwd(cwd, sizeof cwd));
        snprintf(path, sizeof path, "%s/Xservers", cwd);
        servers = Heap(path);
    }
    CHECK(test_scan_servers() == 1);
    CHECK(FindDisplayByName(":0") != NULL && FindDisplayByName(":1") != NULL);
    /* the piece of the long line is not a display of its own */
    CHECK(FindDisplayByName("xxxxxxxxxxxxxxxxxxxx") == NULL);
}

static void ResourceUnchanged(void)
{
    char *value = NULL, *kept;

    test_resource_db("Dtlogin.session: /usr/dt/bin/Xsession\n");
    CHECK(test_get_resource("Dtlogin.session", "Dtlogin.Session", &value, "default") == 1);
    CHECK(value && strcmp(value, "/usr/dt/bin/Xsession") == 0);
    kept = value;
    /* a resource that did not change is left as it is (the size of a value counts its NUL) */
    CHECK(test_get_resource("Dtlogin.session", "Dtlogin.Session", &value, "default") == 0);
    CHECK(value == kept);
    /* a new value replaces it, a missing one gives the default, and the default alone gives nothing when it is empty */
    test_resource_db("Dtlogin.session: other\n");
    CHECK(test_get_resource("Dtlogin.session", "Dtlogin.Session", &value, "default") == 1 && strcmp(value, "other") == 0);
    test_resource_db("");
    CHECK(test_get_resource("Dtlogin.session", "Dtlogin.Session", &value, "default") == 1 && strcmp(value, "default") == 0);
    CHECK(test_get_resource("Dtlogin.setup", "Dtlogin.Setup", &value, "another") == 1 && strcmp(value, "another") == 0);
    value = NULL;
    CHECK(test_get_resource("Dtlogin.setup", "Dtlogin.Setup", &value, "") == 1 && value == NULL);
}

static void ServersMissing(void)
{
    /* a name that leads to no file leaves no servers file to read */
    servers = NULL;
    CHECK(test_scan_servers() == 0);
}

static void SearchPath(void)
{
    char *found, path[600];
    char cwd[200];

    CHECK(getcwd(cwd, sizeof cwd));
    CHECK(mkdir("dir", 0755) == 0);
    WriteFile("dir/Xconfig", "x");
    snprintf(path, sizeof path, "%s/missing:%s/dir", cwd, cwd);
    found = test_qualify("Xconfig", path);
    CHECK(found && strstr(found, "/dir/Xconfig"));
    free(found);

    /* only a file counts, and looking for it must not wait for a writer to open a pipe */
    CHECK(mkdir("pipes", 0755) == 0 && mkfifo("pipes/Xfifo", 0644) == 0);
    snprintf(path, sizeof path, "%s/pipes", cwd);
    signal(SIGALRM, Watchdog);
    alarm(5);
    CHECK(test_qualify("Xfifo", path) == NULL);
    alarm(0);
}

static void SearchPathNoMemory(void)
{
    char path[] = "/nonexistent-a:/nonexistent-b";

    fail_size = strlen("/nonexistent-a") + strlen("Xconfig") + 2;
    CHECK(test_qualify("Xconfig", path) == NULL);
    fail_size = sizeof path;
    CHECK(test_qualify("Xconfig", path) == NULL);
}

static void ExpandLang(void)
{
    char *s;

    s = test_expand("/etc/%L/Xresources", "de_DE");
    CHECK(strcmp(s, "/etc/de_DE/Xresources") == 0);
    free(s);
    s = test_expand("/etc/%L/Xresources", "");
    CHECK(strcmp(s, "/etc/Xresources") == 0);
    free(s);
    /* a percent sign at the end, or before something else, stands for itself */
    s = test_expand(Heap("100%"), "C");
    CHECK(strcmp(s, "100%") == 0);
    free(s);
    s = test_expand(Heap("a%Xb%"), "C");
    CHECK(strcmp(s, "a%Xb%") == 0);
    free(s);
    s = test_expand(Heap("%L"), "fr");
    CHECK(strcmp(s, "fr") == 0);
    free(s);
}

static void ExpandLangNoMemory(void)
{
    fail_next = 1;
    CHECK(test_expand("/etc/%L", "C") == NULL);
}

static void AltDesktops(void)
{
    struct display d;
    XrmValue value;
    char *type;

    memset(&d, 0, sizeof d);
    d.language = "C";
    CHECK(mkdir("dtinst", 0755) == 0 && mkdir("dtinst/config", 0755) == 0);
    CHECK(mkdir("dtinst/config/C", 0755) == 0 && mkdir("dtinst/config/C/Xresources.d", 0755) == 0);
    WriteFile("dtinst/config/C/Xresources.d/10.res",
              "Dtlogin*altDtsIncrement: True \nDtlogin*altDtKey: First\nDtlogin*altDtName: One\n");
    WriteFile("dtinst/config/C/Xresources.d/20.res",
              "Dtlogin*altDtsIncrement: True\nDtlogin*altDtKey: Second\nDtlogin*altDtName: Two\n");
    XresourceDB = XrmGetStringDatabase("Dtlogin*altDts: 4000000000\n");
    test_alt_desktops(&d);
    /* the count the resource file asks for is limited; the two files add to it */
    CHECK(XrmGetResource(XresourceDB, "Dtlogin*altDts", "Dtlogin*AltDts", &type, &value));
    CHECK(atoi(value.addr) == 100);
}

static void AltDesktopsNoMemory(void)
{
    struct display d;
    XrmValue value;
    char *type;

    memset(&d, 0, sizeof d);
    d.language = "C";
    CHECK(mkdir("dtinst", 0755) == 0 && mkdir("dtinst/config", 0755) == 0);
    CHECK(mkdir("dtinst/config/C", 0755) == 0 && mkdir("dtinst/config/C/Xresources.d", 0755) == 0);
    WriteFile("dtinst/config/C/Xresources.d/10.res", "Dtlogin*altDtsIncrement: True\n");
    XresourceDB = XrmGetStringDatabase("Dtlogin*altDts: 1\n");
    /* the list of files cannot be made: the login screen goes on without the alternate desktops */
    fail_size = 10 * sizeof(char *);
    test_alt_desktops(&d);
    CHECK(XrmGetResource(XresourceDB, "Dtlogin*altDts", "Dtlogin*AltDts", &type, &value));
    CHECK(atoi(value.addr) == 1);
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"xaccess-long-word", LongWord},
    {"xaccess-unknown-display", UnknownDisplay},
    {"xaccess-pattern-time", PatternTime},
    {"xaccess-pattern-meaning", PatternMeaning},
    {"local-address", LocalAddress},
    {"xservers-local-uid", ServersLocalUid},
    {"xservers-class-added", ServersClassAdded},
    {"xservers-host-name", ServersHostName},
    {"xservers-file", ServersFile},
    {"xservers-missing", ServersMissing},
    {"resource-unchanged", ResourceUnchanged},
    {"search-path", SearchPath},
    {"search-path-no-memory", SearchPathNoMemory},
    {"expand-lang", ExpandLang},
    {"expand-lang-no-memory", ExpandLangNoMemory},
    {"alt-desktops", AltDesktops},
    {"alt-desktops-no-memory", AltDesktopsNoMemory},
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

names = ["xaccess-long-word", "xaccess-unknown-display", "xaccess-pattern-time", "xaccess-pattern-meaning",
         "local-address", "xservers-local-uid", "xservers-class-added", "xservers-host-name", "xservers-file", "xservers-missing", "resource-unchanged", "search-path",
         "search-path-no-memory", "expand-lang", "expand-lang-no-memory", "alt-desktops", "alt-desktops-no-memory"]
failed = run_all("login-config-",
                 {"harness.h": harness, "access_unit.c": access_unit, "file_unit.c": file_unit,
                  "dpylist_unit.c": dpylist_unit, "servers_unit.c": servers_unit, "resource_unit.c": resource_unit, "qualify_unit.c": qualify_unit,
                  "session_unit.c": session_unit, "stubs.c": stubs},
                 [("access_unit", ["-w"]), ("file_unit", ["-w"]), ("dpylist_unit", ["-w"]), ("servers_unit", ["-w"]), ("resource_unit", ["-w"]),
                  ("qualify_unit", ["-w"]), ("session_unit", ["-w"]),
                  ("stubs", ["-Wall", "-Wextra", "-Wno-unused-parameter"])],
                 ["-lXdmcp", "-lXau", "-lXmu", "-lXt", "-lX11"], names,
                 defines=['-DCDE_INSTALLATION_TOP="dtinst"', '-DCDE_CONFIGURATION_TOP="dtconf"'])
finish(failed, "Configuration readers", "Xaccess, Xservers, search paths and resource directories")
