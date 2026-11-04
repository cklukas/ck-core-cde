#!/usr/bin/env python3
"""Run the greeter's message catalog handling with stand-ins for the catalog calls.

The greeter closes its catalog when it exits.  CloseCatalog() used to call
catclose() whatever the state: with no catalog opened (no message was read, or
no language is set) the handle is a null pointer, and after a failed open it is
-1; the C library crashes on both.  The functions of vgutil.c are compiled
with catalog calls that abort on an invalid handle, and every state the
greeter can be in at exit is run in its own process (OpenCatalog() opens once
per process).

DTLOGIN_SOURCE names another copy of the sources to test: a tree from before
the fix must fail.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTLOGIN_SOURCE") or Path(__file__).resolve().parents[1])
text = (source / "vgutil.c").read_text()
decl_start = text.index("static nl_catd\tnl_fd;")
decl = text[decl_start:text.index("\n", decl_start) + 1]
start = text.index("void\nCloseCatalog(void)")
end = text.rindex("/***", 0, text.index("XmString\nReadCatalogXms("))
header = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef void *nl_catd;
#define NLS_CATALOG "dtlogin.cat"
#define NL_CAT_LOCALE 0
#define NLSPATH_ENV "/nonexistent/%N"
#define MC_DEF_LOG_NO_MSGCAT "no catalog for %s"
static char *langenv;
static int opens, closes, reads, logged;
static void *open_result;
static void *last_closed;
static nl_catd Open(const char *name, int flag)
{
    (void) name; (void) flag;
    opens++;
    return (nl_catd) open_result;
}
static void Close(nl_catd catd)
{
    closes++;
    last_closed = (void *) catd;
    if (catd == NULL || catd == (nl_catd) -1) {
        fputs("catclose() called with an invalid handle\n", stderr);
        abort();
    }
}
static char *Gets(nl_catd catd, int set, int msg, char *dflt)
{
    (void) set; (void) msg;
    reads++;
    if (catd == last_closed) {
        fputs("catgets() on a closed catalog\n", stderr);
        abort();
    }
    return dflt;
}
static void Log(unsigned char *fmt, const char *arg) { (void) fmt; (void) arg; logged++; }
#define CATOPEN(name, flag) Open(name, flag)
#define CATCLOSE(catd) Close(catd)
#define CATGETS(catd, set, msg, dflt) Gets(catd, set, msg, dflt)
#define LogError(fmt, arg) Log(fmt, arg)
'''
main = r'''
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "failed: %s (line %d)\n", #cond, __LINE__); return 1; } } while (0)
int main(int argc, char **argv)
{
    static char marker;
    const char *scenario = argc > 1 ? argv[1] : "";
    if (!strcmp(scenario, "never-opened")) {
        /* no message was read: the handle is still null at exit */
        langenv = "C";
        CloseCatalog();
        CHECK(closes == 0 && opens == 0);
    } else if (!strcmp(scenario, "no-language")) {
        langenv = NULL;
        CHECK(!strcmp((char *) ReadCatalog(1, 1, "default"), "default"));
        CloseCatalog();
        CHECK(opens == 0 && closes == 0);
    } else if (!strcmp(scenario, "failed-open")) {
        langenv = "de_DE";
        open_result = (void *) -1;
        CHECK(!strcmp((char *) ReadCatalog(1, 1, "default"), "default"));
        CHECK(opens == 1 && logged == 1);
        CloseCatalog();
        CHECK(closes == 0);
    } else if (!strcmp(scenario, "opened")) {
        langenv = "de_DE";
        open_result = &marker;
        CHECK(!strcmp((char *) ReadCatalog(1, 1, "default"), "default"));
        CHECK(opens == 1 && reads == 1 && logged == 0);
        CloseCatalog();
        CHECK(closes == 1 && last_closed == &marker);
        /* a closed catalog is not used again and not closed twice */
        CHECK(!strcmp((char *) ReadCatalog(1, 1, "again"), "again"));
        CHECK(reads == 1);
        CloseCatalog();
        CHECK(closes == 1);
    } else {
        fprintf(stderr, "unknown scenario %s\n", scenario);
        return 2;
    }
    return 0;
}
'''
scenarios = ["never-opened", "no-language", "failed-open", "opened"]
failed = []
with tempfile.TemporaryDirectory(prefix="greeter-catalog-", dir=os.environ.get("TMPDIR")) as work:
    c, binary = Path(work) / "catalog.c", Path(work) / "catalog"
    c.write_text(header + decl + text[start:end] + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Wno-unused-function",
                    "-fsanitize=address,undefined", "-g", str(c), "-o", str(binary)], check=True)
    for name in scenarios:
        r = subprocess.run([str(binary), name], capture_output=True, text=True)
        if r.returncode:
            failed.append(name)
            print("%s failed: %s" % (name, (r.stderr or "exit %d" % r.returncode).strip().splitlines()[0]))
if failed:
    sys.exit("Greeter catalog: %d of %d scenarios failed" % (len(failed), len(scenarios)))
print("Greeter catalog: %d scenarios passed" % len(scenarios))
