#!/usr/bin/env python3
"""Run functions of DtsMM.c under the sanitizers: the expansion of long values,
the name of the cache file for display names of all kinds, and the check of
the header of a cache file that is mapped.

The functions are cut out of DtUtil1/DtsMM.c and compiled with stubs for what
they take from the rest of the library.  DTSMM_SOURCE names another copy of
DtsMM.c to test, e.g. an older revision.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
path = Path(os.environ.get("DTSMM_SOURCE", here.parent / "DtUtil1" / "DtsMM.c"))
match = re.search(r"^char \*\n_DtDtsMMExpandValue\(const char \*value\)\n\{.*?\n\}\n", path.read_text(), re.M | re.S)
assert match, "_DtDtsMMExpandValue not found in " + str(path)

program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fills;
void _DtDbFillVariables(char **line) { (void)line; fills++; }

''' + match.group(0) + r'''
int main(void)
{
    /* below, at and above the 1024 bytes the buffer used to have */
    static const size_t sizes[] = { 0, 1, 1022, 1023, 1024, 1025, 5000, 100000 };
    assert(_DtDtsMMExpandValue(NULL) == NULL);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char *value = malloc(sizes[i] + 1), *copy;
        memset(value, 'a', sizes[i]);
        value[sizes[i]] = '\0';
        copy = _DtDtsMMExpandValue(value);
        assert(copy && strcmp(copy, value) == 0);
        free(copy);
        free(value);
    }
    assert(fills == (int)(sizeof(sizes) / sizeof(sizes[0])));
    puts("Dts value expansion: lengths from 0 to 100000 bytes passed");
    return 0;
}
'''
cc = shlex.split(os.environ.get("CC", "cc"))
with tempfile.TemporaryDirectory(prefix="dts-mm-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    (work / "test.c").write_text(program)
    subprocess.run(cc + ["-std=c99", "-D_DEFAULT_SOURCE", "-UNDEBUG", "-g", "-O1", "-fno-omit-frame-pointer",
                         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                         str(work / "test.c"), "-o", str(work / "test")], check=True)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:" + os.environ.get("ASAN_OPTIONS", ""))
    run = subprocess.run([str(work / "test")], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if run.returncode:
        keep = [l for l in run.stdout.splitlines() if "ERROR:" in l or "SUMMARY:" in l or "Assertion" in l]
        sys.exit("Dts value expansion failed (exit %d)\n%s" % (run.returncode, "\n".join(keep[:6])))
    print(run.stdout.strip())


def function(name, required=True):
    """The definition of NAME in DtsMM.c, with the lines above it that belong to it."""
    m = re.search(r"^(?:static )?(?:char \*|int|void)\n" + re.escape(name) + r"\(.*?\n\}\n", path.read_text(), re.M | re.S)
    assert m or not required, name + " not found in " + str(path)
    return m.group(0) if m else ""


header = here.parents[2] / "include" / "Dt" / "DtsMM.h"
htext = header.read_text()
types = "".join(re.findall(r"^typedef\s+int\s+\w+;.*\n", htext, re.M))
for name in ("DtDtsMMField", "DtDtsMMRecord", "DtDtsMMDatabase", "DtDtsMMHeader"):
    types += re.search(r"typedef\s+struct\s*\{[^{}]*\}\s*" + name + r";", htext).group(0) + "\n"

cache_name = r'''
#include <assert.h>
#include <ctype.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define _DTDTSMMTEMPDIR "/tmp"
#define _DTDTSMMTEMPFILE "dtdbcache_"
#define _DtSvcProcessLock() ((void)0)
#define _DtSvcProcessUnlock() ((void)0)
''' + "".join(re.findall(r"^#define\s+DTS_MAX_DISPLAY_NAME\s+\d+\s*$\n", path.read_text(), re.M)) + function("_DtDtsMMCacheName") + r'''
static char *name_for(const char *display)
{
    if (display)
        setenv("DISPLAY", display, 1);
    else
        unsetenv("DISPLAY");
    return _DtDtsMMCacheName(1);
}

int main(void)
{
    static const char *const odd[] = { "hostonly", "", ":", "no-colon.with.dots", "../../etc/passwd:0",
        "/private/tmp/socket:0", "a b\nc:0", "host:0.0.0", "\xff\xfe:1", "...", "x:y:z.1.2" };
    char *name, *dir;
    size_t i;

    name = name_for(":0");
    assert(name && strcmp(name, "/tmp/dtdbcache_:0") == 0);
    free(name);
    name = name_for("host.example.org:12.1");
    assert(name && strcmp(name, "/tmp/dtdbcache_host.example.org:12") == 0);
    free(name);
    name = name_for("unix:0.0");
    assert(name && strcmp(name, "/tmp/dtdbcache_unix:0") == 0);
    free(name);

    /* what a display name may look like, and what it may not */
    for (i = 0; i < sizeof odd / sizeof odd[0]; i++) {
        name = name_for(odd[i]);
        assert(name && strncmp(name, "/tmp/dtdbcache_", 15) == 0);
        assert(strchr(name + 5, '/') == NULL);
        assert(strpbrk(name, " \n\xff") == NULL);
        free(name);
    }
    dir = malloc(100001);
    memset(dir, 'h', 100000);
    dir[100000] = '\0';
    name = name_for(dir);
    assert(name && strlen(name) < 200);
    free(name);
    free(dir);

    /* without a display, or without a shared cache: a name of this process */
    name = name_for(NULL);
    assert(name && strncmp(name, "/tmp/dtdbcache_", 15) == 0 && strchr(name + 5, '/') == NULL);
    dir = _DtDtsMMCacheName(0);
    assert(dir && strcmp(name, dir) != 0);
    free(dir);
    free(name);
    puts("Dts cache names: display names of all kinds passed");
    return 0;
}
'''

cache_header = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef int DtShmBoson;
''' + types + function("cache_size_fits", False) + function("index_inside", False) + function("cache_header_valid", False) + r'''
int main(void)
{
    enum { INTS = 4096 };
    size_t size = sizeof(DtDtsMMHeader) + INTS * sizeof(int);
    DtDtsMMHeader h;

    assert(!cache_size_fits(0) && !cache_size_fits(sizeof(DtDtsMMHeader) - 1));
    assert(cache_size_fits(sizeof(DtDtsMMHeader)) && cache_size_fits(size));
    assert(!cache_size_fits((off_t)INT_MAX + 1));

    memset(&h, 0, sizeof h);
    h.num_db = 2;
    h.db_offset = 0;
    h.files_count = 3;
    h.files_offset = 1000;
    h.mtimes_offset = 2000;
    h.str_tbl_offset = 3000;
    h.name_list_offset = 3500;
    h.no_name_offset = -1;
    assert(cache_header_valid(&h, size));

    h.no_name_offset = 3600;
    assert(cache_header_valid(&h, size));
    h.no_name_offset = INTS + 1;
    assert(!cache_header_valid(&h, size));
    h.no_name_offset = -1;

    /* every offset and count of the header beyond the file, or negative */
    h.db_offset = INTS; assert(!cache_header_valid(&h, size)); h.db_offset = 0;
    h.db_offset = INT_MAX; assert(!cache_header_valid(&h, size)); h.db_offset = 0;
    h.num_db = INT_MAX; assert(!cache_header_valid(&h, size)); h.num_db = 2;
    h.num_db = -1; assert(!cache_header_valid(&h, size)); h.num_db = 2;
    h.files_offset = -5; assert(!cache_header_valid(&h, size)); h.files_offset = 1000;
    h.files_count = 1 << 30; assert(!cache_header_valid(&h, size)); h.files_count = 3;
    h.mtimes_offset = INTS - 1; assert(!cache_header_valid(&h, size)); h.mtimes_offset = 2000;
    h.str_tbl_offset = INT_MIN; assert(!cache_header_valid(&h, size)); h.str_tbl_offset = 3000;
    h.name_list_offset = INTS + 10; assert(!cache_header_valid(&h, size)); h.name_list_offset = 3500;
    assert(cache_header_valid(&h, size));
    puts("Dts cache header: offsets and counts passed");
    return 0;
}
'''

for name, program in (("cache name", cache_name), ("cache header", cache_header)):
    with tempfile.TemporaryDirectory(prefix="dts-mm-", dir=os.environ.get("TMPDIR")) as work:
        work = Path(work)
        (work / "test.c").write_text(program)
        build = subprocess.run(cc + ["-std=gnu99", "-D_DEFAULT_SOURCE", "-UNDEBUG", "-g", "-O1", "-fno-omit-frame-pointer",
                                     "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                                     str(work / "test.c"), "-o", str(work / "test")], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
        if build.returncode:
            sys.exit("Dts %s: cannot build the test (older source?)\n%s" % (name, build.stdout[-600:]))
        run = subprocess.run([str(work / "test")], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if run.returncode:
            keep = [l for l in run.stdout.splitlines() if "ERROR:" in l or "SUMMARY:" in l or "Assertion" in l]
            sys.exit("Dts %s failed (exit %d)\n%s" % (name, run.returncode, "\n".join(keep[:6])))
        print(run.stdout.strip())
