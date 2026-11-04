#!/usr/bin/env python3
"""Run _DtDtsMMExpandValue of DtsMM.c under the sanitizers with long values.

The function is cut out of DtUtil1/DtsMM.c and compiled with a stub for the
variable expansion, so only the buffer it returns is tested.  DTSMM_SOURCE
names another copy of DtsMM.c to test, e.g. an older revision.
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
