#!/usr/bin/env python3
"""Run production DtHelp code with realloc() failing.

For each case one production source file is taken verbatim from the working
tree (or from another revision with --source-revision REV), preceded by
realloc-inject.h and followed by check-realloc-failures.c, and compiled with
a few plain sources it links against, under AddressSanitizer and
UndefinedBehaviorSanitizer with halt on error.  The test makes a chosen
realloc() fail and checks that the old block stays usable and that nothing
is leaked.

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
lib = "cde/lib/DtHelp/"

# macOS keeps iconv() in a separate library
libiconv = ["-liconv"] if sys.platform == "darwin" else []

# case name -> (instrumented source, plain sources linked with it, extra flags)
CASES = {
    "linkmgr":     ("LinkMgr.c",     ["CvString.c"], []),
    "utilsdl":     ("UtilSDL.c",     ["CvString.c"], []),
    "canvasos":    ("CanvasOs.c",    ["CvString.c", "bufio.c"], []),
    "stringfuncs": ("StringFuncs.c", ["CvString.c"], ["-DICONV_CONST="] + libiconv),
    "accesssdl":   ("AccessSDL.c",   ["CvString.c"], []),
    "formatsdl_string": ("FormatSDL.c", ["CvString.c"], []),
    "formatsdl_chunks": ("FormatSDL.c", ["CvString.c"], []),
}

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
ap.add_argument("--library-dir", action="append", type=Path, metavar="DIR",
                default=[Path(d) for d in os.environ.get("DTHELP_LIBDIR", "").split(os.pathsep) if d],
                help="library directory for the link on Linux, repeatable (default: $DTHELP_LIBDIR, "
                     "colon separated); the built libDtHelp.so, libDtSearch.so, libDtSvc.so, libtt.so and libDtXinerama.so, and libXm "
                     "if it is not in the system path, must be found in these directories")
ap.add_argument("--case", action="append", choices=sorted(CASES), help="run only this case")
args = ap.parse_args()

if args.scratch:
    scratch = args.scratch
    scratch.mkdir(parents=True, exist_ok=True)
else:
    scratch = Path(tempfile.mkdtemp(prefix="realloc-failures-", dir=os.environ.get("TMPDIR")))
    atexit.register(shutil.rmtree, scratch, True)
label = args.source_revision or "working tree"
srcdir = args.worktree / lib

if args.source_revision:
    text_of = lambda name: subprocess.run(
        ["git", "-C", str(args.worktree), "show", f"{args.source_revision}:{lib}{name}"],
        check=True, capture_output=True, text=True).stdout
else:
    text_of = lambda name: (srcdir / name).read_text()

body = (here / "check-realloc-failures.c").read_text()
# The instrumented file calls DtHelp functions that are not part of the test.
# Every reference must resolve: the instrumented file's own definitions come
# first, everything else is taken from the built library.  On Linux, $LDFLAGS
# (the build's -L and -rpath for libXm and the like) is part of the link.
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
env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

failed = []
for name in args.case or sorted(CASES):
    source, plain, flags = CASES[name]
    tag = "control" if args.source_revision else "fixed"
    c, binary = scratch / f"realloc-{name}-{tag}.c", scratch / f"realloc-{name}-{tag}"
    c.write_text('#include "realloc-inject.h"\n' + text_of(source) + "\n" + body)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + flags + [f"-DCASE_{name.upper()}"] +
                   ["-std=gnu99", "-w", "-DDTLIB", "-D_DEFAULT_SOURCE",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-g"] + includes + [str(c)] +
                   [str(srcdir / p) for p in plain] + lazy + ["-o", str(binary)], check=True)
    run = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    if args.source_revision:
        if run.returncode == 0:
            print(f"NEGATIVE CONTROL FAILED ({name}): {label} passed the injection test", file=sys.stderr)
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
