#!/usr/bin/env python3
"""Run the production SDL attribute value readers on malformed attributes.

UtilSDL.c is taken verbatim from the working tree (or from another revision
with --source-revision REV), preceded by realloc-inject.h and followed by
check-sdl-attributes.c, and compiled with CvString.c under AddressSanitizer
and UndefinedBehaviorSanitizer with halt on error.  Three scenarios run in
a process each: "number" (a number that is cut off by the end of the file or
has no closing quote must not free the caller's stack buffer), "id" (an empty
id must not be dereferenced) and "valid" (well-formed values parse as
before).

Default: every scenario must pass.
With --source-revision REV: negative control; the scenarios "number" and "id"
must FAIL with the sources of REV and "valid" must pass (exit status 0 means
that this was observed).

The sources call DtHelp functions that are not part of the test, so on Linux
the link needs the built libraries: pass --library-dir as for
check-realloc-failures.py.
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

def _ck_sanitize_list() -> str:
    import os, shlex, subprocess, tempfile
    override = os.environ.get("CK_SANITIZE_LIST", "").strip()
    if override:
        return override
    for var in ("CC", "CXX", "CLANG", "TT_COMPILE", "CSA_COMPILE", "CMSD_COMPILE",
                "SPC_COMPILE", "DTSVC_COMPILE", "DTSR_COMPILE", "DTMAIL_CC",
                "DTMAIL_COMPILE", "DIRSELECT_CC"):
        val = os.environ.get(var, "").strip()
        if val:
            cc = shlex.split(val)[:1]
            break
    else:
        cc = ["cc"]
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
        src, exe = os.path.join(d, "p.c"), os.path.join(d, "p")
        with open(src, "w", encoding="utf-8") as f:
            f.write("int main(void) { return 0; }\n")
        for s in ("address,undefined", "undefined"):
            r = subprocess.run(cc + [f"-fsanitize={s}", src, "-o", exe], capture_output=True)
            if r.returncode == 0 and subprocess.run([exe], capture_output=True).returncode == 0:
                return s
    return "undefined"


_CK_SANITIZE_LIST = _ck_sanitize_list()


here = Path(__file__).resolve().parent
root = here.parents[3]
lib = "cde/lib/DtHelp/"

SCENARIOS = ["number", "id", "valid"]
CONTROL_FAILS = {"number", "id"}

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of UtilSDL.c to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
ap.add_argument("--library-dir", action="append", type=Path, metavar="DIR",
                default=[Path(d) for d in os.environ.get("DTHELP_LIBDIR", "").split(os.pathsep) if d],
                help="library directory for the link on Linux, repeatable (default: $DTHELP_LIBDIR, "
                     "colon separated); see check-realloc-failures.py")
args = ap.parse_args()

if args.scratch:
    scratch = args.scratch
    scratch.mkdir(parents=True, exist_ok=True)
else:
    scratch = Path(tempfile.mkdtemp(prefix="sdl-attributes-", dir=os.environ.get("TMPDIR")))
    atexit.register(shutil.rmtree, scratch, True)
label = args.source_revision or "working tree"
srcdir = args.worktree / lib

if args.source_revision:
    source = subprocess.run(
        ["git", "-C", str(args.worktree), "show", f"{args.source_revision}:{lib}UtilSDL.c"],
        check=True, capture_output=True, text=True).stdout
else:
    source = (srcdir / "UtilSDL.c").read_text()

if sys.platform == "darwin":
    lazy = ["-Wl,-undefined,dynamic_lookup"]
else:
    libdirs = [str(d.resolve()) for d in args.library_dir]
    if not any((Path(d) / "libDtHelp.so").exists() for d in libdirs):
        sys.exit("on this platform the built libDtHelp.so is needed: pass --library-dir DIR "
                 "(or set DTHELP_LIBDIR) with the directory that contains it")
    lazy = [f"-L{d}" for d in libdirs] + [f"-Wl,-rpath,{d}" for d in libdirs] + \
           ["-lDtHelp", "-lDtSearch", "-lDtSvc", "-ltt", "-lDtXinerama", "-lXm", "-lXpm", "-lXt", "-lX11", "-lm"] + \
           shlex.split(os.environ.get("LDFLAGS", ""))
includes = [f"-I{here}", f"-I{srcdir}", f"-I{args.worktree}/cde/include/Dt",
            f"-I{args.worktree}/cde/include/DtI", f"-I{args.worktree}/cde/include",
            "-I/opt/X11/include", "-I/opt/homebrew/include", "-I/usr/local/include"]
env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0:symbolize=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"sdl-attributes-{tag}.c", scratch / f"sdl-attributes-{tag}"
c.write_text('#include "realloc-inject.h"\n' + source + "\n" + (here / "check-sdl-attributes.c").read_text())
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-w", "-DDTLIB", "-D_DEFAULT_SOURCE",
                f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] + includes + [str(c), str(srcdir / "CvString.c")] +
               lazy + ["-o", str(binary)], check=True)

failed = []
for name in SCENARIOS:
    run = subprocess.run([str(binary), name], env=env, capture_output=True, text=True, timeout=60)
    sys.stdout.write(run.stdout)
    expect_fail = bool(args.source_revision) and name in CONTROL_FAILS
    if expect_fail:
        if run.returncode == 0:
            print(f"NEGATIVE CONTROL FAILED ({name}): {label} passed", file=sys.stderr)
            failed.append(name)
        else:
            tail = "\n".join(run.stderr.strip().splitlines()[:3])
            print(f"negative control {name} ({label}): fails as expected, exit {run.returncode}\n{tail}")
    elif run.returncode:
        sys.stderr.write(run.stderr)
        print(f"FAIL {name} ({label}): exit {run.returncode}", file=sys.stderr)
        failed.append(name)

if failed:
    sys.exit(1)
if not args.source_revision:
    print(f"PASS ({label})")
else:
    print(f"negative control ({label}): fails as expected")
