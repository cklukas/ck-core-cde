/*
 * Behavioural check of the alias expansion of the Xaccess reader (access.c).
 *
 * An Xaccess entry may list "%name" aliases, and alias entries list hosts and
 * further aliases.  The expansion used to stop only at a depth of 32 levels,
 * so aliases that list each other, or a ladder of entries that list the next
 * one twice, were expanded an exponential number of times for one query.
 *
 * The runner (check-xaccess-alias-cycle.py) compiles the production access.c
 * whole, with stand-ins for the resolver, the logging functions and the
 * display manager's globals, and runs one scenario per process (argv[1]).
 * Hosts are 10.1.0.N and are recorded by N, in the order of the callbacks.
 *
 *   acyclic   an alias file without a cycle gives the same hosts, in the same
 *             order, with the shared alias expanded at each place it is listed
 *   cycle     two aliases that list each other, an alias that lists itself and
 *             an entry whose pattern matches every alias: the entry that is
 *             being expanded is not expanded again
 *   ladder    40 entries that each list the next one twice: the lookup ends
 *             in a bounded number of expansions and logs that it stopped
 *   clique    eight aliases that each list all of them: the same
 *   chooser   the clique through ForEachChooserHost
 *
 * The process ends itself after 10 seconds, which fails a scenario that does
 * not finish.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dm.h"

int debugLevel, wakeupTime = -1, errors;
char *accessFile, *keyFile;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

static struct hostent *test_gethostbyname(const char *name)
{
    static struct hostent h;
    static unsigned char address[4];
    static char *list[2] = { (char *)address, NULL };
    unsigned a, b, c, d;

    if (sscanf(name, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
	address[0] = a; address[1] = b; address[2] = c; address[3] = d;
    } else {
	address[0] = 10; address[1] = 9; address[2] = 9; address[3] = 9;
    }
    h.h_name = (char *)name; h.h_addrtype = AF_INET; h.h_length = 4; h.h_addr_list = list;
    return &h;
}
#define gethostbyname test_gethostbyname
#include "access.c"

void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; ++errors; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
char *localHostname(void) { return "thishost"; }
char *NetworkAddressToHostname(CARD16 type, ARRAY8Ptr address) { (void)type; (void)address; return NULL; }
int addressEqual(XdmcpNetaddr a1, int len1, XdmcpNetaddr a2, int len2)
{ return len1 == len2 && memcmp(a1, a2, len1) == 0; }
ARRAY8Ptr IndirectChoice(ARRAY8Ptr a, CARD16 t) { (void)a; (void)t; return NULL; }

static char seen[65536];
static long calls;

static void Record(CARD16 type, ARRAY8Ptr host, char *closure)
{
    char item[16];

    (void)type; (void)closure;
    ++calls;
    if (host->length == 4 && host->data[0] == 10 && host->data[1] == 1) {
	snprintf(item, sizeof item, "%s%d", seen[0] ? " " : "", host->data[3]);
	if (strlen(seen) + strlen(item) < sizeof seen)
	    strcat(seen, item);
    }
}

static void load(const char *text)
{
    static char path[256];
    FILE *f;

    snprintf(path, sizeof path, "Xaccess.%d", (int)getpid());
    free(accessFile);
    accessFile = strdup(path);
    f = fopen(path, "w");
    CHECK(f != NULL);
    fputs(text, f);
    fclose(f);
    ScanAccessDatabase();
    remove(path);
    seen[0] = '\0';
    calls = 0;
    errors = 0;
}

static unsigned char client_address[4] = { 10, 0, 0, 2 };
static ARRAY8 client = { 4, client_address };

static void query(void)
{
    ForEachMatchingIndirectHost(&client, FamilyInternet, Record, NULL);
}

static void acyclic(void)
{
    load("%a %b %c\n"
	 "%b 10.1.0.1 10.1.0.2\n"
	 "%c %b 10.1.0.3\n"
	 "10.0.0.2 %a 10.1.0.4\n");
    query();
    CHECK(strcmp(seen, "1 2 1 2 3 4") == 0);
    CHECK(errors == 0);
}

static void cycle(void)
{
    load("%a %b 10.1.0.1\n"
	 "%b %a 10.1.0.2\n"
	 "10.0.0.2 %a\n");
    query();
    CHECK(strcmp(seen, "2 1") == 0);

    load("%a %a 10.1.0.1\n"
	 "10.0.0.2 %a\n");
    query();
    CHECK(strcmp(seen, "1") == 0);

    /* an entry that matches every alias, itself included */
    load("%* %a 10.1.0.1\n"
	 "10.0.0.2 %a\n");
    query();
    CHECK(strcmp(seen, "1") == 0);
    CHECK(errors == 0);
}

static void ladder(void)
{
    char text[8192] = "";
    char line[128];
    int i;

    for (i = 0; i < 40; i++) {
	snprintf(line, sizeof line, "%%l%d %%l%d %%l%d 10.1.0.1\n", i, i + 1, i + 1);
	strcat(text, line);
    }
    strcat(text, "10.0.0.2 %l0\n");
    load(text);
    query();
    CHECK(calls > 0 && calls <= 4096);
    CHECK(errors >= 1);
}

static void clique_text(char *text, int chooser)
{
    char line[256];
    int i, j;

    text[0] = '\0';
    for (i = 0; i < 8; i++) {
	snprintf(line, sizeof line, "%%n%d", i);
	strcat(text, line);
	for (j = 0; j < 8; j++) {
	    snprintf(line, sizeof line, " %%n%d", j);
	    strcat(text, line);
	}
	strcat(text, " 10.1.0.1\n");
    }
    strcat(text, chooser ? "10.0.0.2 CHOOSER %n0\n" : "10.0.0.2 %n0\n");
}

static void clique(void)
{
    char text[8192];

    clique_text(text, 0);
    load(text);
    query();
    CHECK(calls > 0 && calls <= 4096);
}

static void chooser(void)
{
    char text[8192];

    clique_text(text, 1);
    load(text);
    ForEachChooserHost(&client, FamilyInternet, Record, NULL);
    CHECK(calls > 0 && calls <= 4096);
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*run)(void); } scenarios[] = {
	{ "acyclic", acyclic }, { "cycle", cycle }, { "ladder", ladder },
	{ "clique", clique }, { "chooser", chooser },
    };
    size_t i;

    if (argc != 2)
	return 2;
    alarm(10);
    for (i = 0; i < sizeof scenarios / sizeof *scenarios; i++)
	if (strcmp(argv[1], scenarios[i].name) == 0) {
	    scenarios[i].run();
	    printf("ok %s\n", argv[1]);
	    return 0;
	}
    return 2;
}
