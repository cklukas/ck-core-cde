#!/usr/bin/env python3
"""Run production DtTerm code with realloc() failing.

The code under test is taken verbatim from the working tree (or from another
revision with --source-revision REV):

  linetodata   util/lineToData.c, the whole program, run on a font description
               with every realloc() failing in turn;
  enumname     enumToName() from TermPrim/TermPrimDebug.c;
  getpty       the pty tables and GetPty() from TermPrim/TermPrimGetPty-bsd.c,
               with open(), chown(), chmod() and ttyname() replaced by stubs.

Each is compiled with realloc-inject.h under AddressSanitizer and
UndefinedBehaviorSanitizer with halt on error.  Every extraction fails loudly
unless its anchors occur exactly once.

Default: every case must pass.
With --source-revision REV: negative control; every case must FAIL with the
sources of REV (exit status 0 means the failures were observed).
"""
import argparse
import atexit
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[3]
lib = "cde/lib/DtTerm/"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
ap.add_argument("--case", action="append", choices=["linetodata", "enumname", "getpty", "getpty_ttyname"])
args = ap.parse_args()

if args.scratch:
    scratch = args.scratch
    scratch.mkdir(parents=True, exist_ok=True)
else:
    scratch = Path(tempfile.mkdtemp(prefix="realloc-failures-", dir=os.environ.get("TMPDIR")))
    atexit.register(shutil.rmtree, scratch, True)
label = args.source_revision or "working tree"
tag = "control" if args.source_revision else "fixed"
srcdir = args.worktree / lib


def source(name):
    if args.source_revision:
        return subprocess.run(["git", "-C", str(args.worktree), "show",
                               f"{args.source_revision}:{lib}{name}"],
                              check=True, capture_output=True, text=True).stdout
    return (srcdir / name).read_text()


def extract(text, label, start_anchor, end_anchor="\n}\n", keep_start=0):
    if text.count(start_anchor) != 1:
        sys.exit(f"anchor {start_anchor.strip()!r} found {text.count(start_anchor)} times in {label}, expected 1")
    start = text.index(start_anchor) + keep_start
    end = text.index(end_anchor, start) + len(end_anchor)
    return text[start:end]


cc = shlex.split(os.environ.get("CC", "cc"))
cflags = ["-std=gnu23", "-w", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
          "-fno-omit-frame-pointer", "-g", f"-I{here}"]
env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
body = (here / "check-realloc-failures.c").read_text()
failed = []


def compile_case(name, text, defines=()):
    c, binary = scratch / f"realloc-{name}-{tag}.c", scratch / f"realloc-{name}-{tag}"
    c.write_text(text)
    subprocess.run(cc + list(defines) + cflags + [str(c), "-o", str(binary)], check=True)
    return binary


def verdict(name, ok, detail):
    """A case passes on the working tree and must fail on a control revision."""
    if args.source_revision:
        if ok:
            print(f"NEGATIVE CONTROL FAILED ({name}): {label} passed the injection test", file=sys.stderr)
            failed.append(name)
        else:
            print(f"negative control {name} ({label}): fails as expected\n{detail}")
    elif not ok:
        print(f"FAIL {name} ({label})\n{detail}", file=sys.stderr)
        failed.append(name)


def run_unit(name, prologue, pieces, defines):
    binary = compile_case(name, '#include "realloc-inject.h"\n' + prologue + pieces + "\n" + body,
                          [f"-DCASE_{name.upper()}"] + defines)
    run = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    detail = "\n".join(run.stderr.strip().splitlines()[:3])
    verdict(name, run.returncode == 0, f"exit {run.returncode}\n{detail}")


wanted = args.case or ["linetodata", "enumname", "getpty", "getpty_ttyname"]

if "enumname" in wanted:
    text = source("TermPrim/TermPrimDebug.c")
    pieces = (extract(text, "TermPrimDebug.c", "typedef struct {\n    char\t\t *string;\n", "} enumName;\n") +
              extract(text, "TermPrimDebug.c", "\nstatic char *\nenumToName\n(", keep_start=1))
    run_unit("enumname", "", pieces, [])

if "getpty" in wanted or "getpty_ttyname" in wanted:
    text = source("TermPrim/TermPrimGetPty-bsd.c")
    prologue = r'''
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#define True 1
#define False 0
#define CK_TTYNAME "/dev/ttys-name-from-ttyname-longer-than-the-opened-one"
static int ck_open_succeeds;
static int ck_open(const char *path, int flags, int mode)
{
    if (!ck_open_succeeds) {
        errno = ENOENT;
        return -1;
    }
    return open("/dev/null", O_RDWR);
}
static int ck_nothing(const char *path, int a, int b) { return 0; }
#define open ck_open
#define chown ck_nothing
#define chmod(path, mode) ck_nothing(path, 0, 0)
#define isDebugFSet(c, n) (0)
#define isDebugSet(c) (0)
#define AddPtyInfo(name) (0)
#define _XTtyname(fd) (CK_TTYNAME)
typedef int _Xttynameparams;
'''
    pieces = (extract(text, "TermPrimGetPty-bsd.c", "\nstatic char PTY_dev[]", "\n};\n", keep_start=1) +
              extract(text, "TermPrimGetPty-bsd.c",
                      "\nstatic int\nGetPty(char **ptySlave, char **ptyMaster)\n{", keep_start=1))
    for name in ("getpty", "getpty_ttyname"):
        if name in wanted:
            run_unit(name, prologue, pieces, [])

if "linetodata" in wanted:
    data = scratch / "linetodata.data"
    data.write_text("char 'a','b','c','d','e','f','g','h','i','j'\n"
                    "rect 0 0 50 50\n"
                    "line 0 0 100 0 1\n"
                    "stipple 0 0 100 0 1\n")
    c = scratch / f"realloc-linetodata-{tag}.c"
    binary = scratch / f"realloc-linetodata-{tag}"
    c.write_text('#include "realloc-inject.h"\n' + source("util/lineToData.c"))
    subprocess.run(cc + ["-DCK_FROM_ENVIRONMENT", f"-I{srcdir / 'TermPrim'}"] + cflags +
                   [str(c), "-o", str(binary)], check=True)
    count_file = scratch / "linetodata.count"

    def run(fail_at):
        count_file.unlink(missing_ok=True)
        e = dict(env, CK_REALLOC_FAIL_AT=str(fail_at), CK_REALLOC_COUNT_FILE=str(count_file))
        return subprocess.run([str(binary), "-f", str(data)], env=e, capture_output=True, text=True)

    ok, detail = True, ""
    first = run(0)
    total = int(count_file.read_text()) if count_file.exists() else 0
    if first.returncode != 0 or total != 5:
        ok, detail = False, f"control run: exit {first.returncode}, {total} realloc calls, expected 5\n" + \
                            "\n".join(first.stderr.strip().splitlines()[:3])
    for call in range(1, 6) if ok else []:
        r = run(call)
        if r.returncode != 1 or "out of memory" not in r.stderr:
            ok = False
            detail = f"realloc {call} failing: exit {r.returncode}\n" + "\n".join(r.stderr.strip().splitlines()[:3])
            break
    if ok:
        print("lineToData: every failed table growth ends the program with a message")
    verdict("linetodata", ok, detail)

if failed:
    sys.exit(1)
if not args.source_revision:
    print(f"PASS ({label})")
