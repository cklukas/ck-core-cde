#!/usr/bin/env python3
"""Check dtappgather privilege handling, DTUSERSESSION traversal, TTFile and CString bounds.

The production sources of libCliSrv/ (cstring.C, DirIterator.C, Environ.C,
UnixEnv.C, TTFile.C) and dtappg/ (Options.C, dtappgather.C) are taken from the
working tree (or from another git revision with --source-revision REV),
retargeted from /var/dt to the test's scratch directory, and compiled together
with check-dtappgather.C under AddressSanitizer and UndefinedBehaviorSanitizer
with halt on error.

Default: the test must pass.
With --source-revision REV: negative control; REV must FAIL the same test
(exit status 0 means the failure was observed).
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[3]
base = "cde/programs/dtsearchpath/"

SOURCES = [
    "libCliSrv/cstring.C",
    "libCliSrv/DirIterator.C",
    "libCliSrv/Environ.C",
    "libCliSrv/UnixEnv.C",
    "libCliSrv/TTFile.C",
    "dtappg/Options.C",
    "dtappg/dtappgather.C",
]

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

label = args.source_revision or "working tree"


def default_tmp_root():
    env_tmp = os.environ.get("TMPDIR")
    if sys.platform == "darwin":
        for vol in (Path("/Volumes/S8T/tmp"), Path("/Volumes/PRO-BLADE/tmp")):
            if vol.parent.is_mount():
                vol.mkdir(parents=True, exist_ok=True)
                return str(vol)
    return env_tmp


def read_source(rel):
    full = base + rel
    if args.source_revision:
        return subprocess.run(
            ["git", "-C", str(args.worktree), "show", f"{args.source_revision}:{full}"],
            check=True, capture_output=True, text=True
        ).stdout
    return (args.worktree / full).read_text()


def run_in(scratch: Path):
    pieces = []
    var_root = str(scratch / "var")
    seen_dir_iter = False
    for rel in SOURCES:
        text = read_source(rel)
        if "#include \"DirIterator.h\"" in text:
            if seen_dir_iter:
                text = text.replace("#include \"DirIterator.h\"", "/* DirIterator.h already included */")
            seen_dir_iter = True
        if rel == "libCliSrv/UnixEnv.C" and "#else\n\tmanpath =" not in text:
            text = text.replace("#elif defined(__NetBSD__)", "#elif defined(__NetBSD__) || defined(__APPLE__)")
        if rel == "dtappg/dtappgather.C":
            if text.count("int main (") != 1:
                sys.exit(f"anchor 'int main (' not unique in {rel} ({label})")
            text = text.replace('"/var"', f'"{var_root}"')
            text = text.replace('"/var/dt', f'"{var_root}/dt')
            text = text.replace("int main (", "static int dtappgather_main (")
        pieces.append(f"/* === {rel} === */\n" + text)

    harness = (here / "check-dtappgather.C").read_text()
    if harness.count("/* @@PRODUCTION@@ */") != 1:
        sys.exit("missing /* @@PRODUCTION@@ */ marker in check-dtappgather.C")
    combined = harness.replace("/* @@PRODUCTION@@ */", "\n".join(pieces))

    tag = "control" if args.source_revision else "fixed"
    cpp = scratch / f"dtappgather-{tag}.C"
    binary = scratch / f"dtappgather-{tag}"
    cpp.write_text(combined)

    cxx = shlex.split(os.environ.get("CXX", "c++"))
    inc_root = args.worktree / base
    includes = [
        f"-I{inc_root / 'libCliSrv'}",
        f"-I{inc_root / 'dtappg'}",
        f"-I{args.worktree / 'cde/lib'}",
        f"-I{args.worktree / 'cde/include'}",
    ]
    if Path("/usr/include/tirpc").is_dir():
        includes.append("-I/usr/include/tirpc")
    subprocess.run(
        cxx + includes + [
            "-O1", "-g", "-w",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer",
            str(cpp), "-o", str(binary),
        ],
        check=True,
    )

    env = dict(
        os.environ,
        ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
    )
    if not args.source_revision:
        run = subprocess.run([str(binary), str(scratch)], env=env, capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        if run.returncode:
            sys.stderr.write(run.stderr)
            print(f"FAIL ({label}): exit {run.returncode}", file=sys.stderr)
            sys.exit(1)
        print(f"PASS ({label})")
        return

    parts = ["tokenizer", "dir-iterator", "ttfile", "dtappgather-security"]
    for part in parts:
        run = subprocess.run([str(binary), str(scratch), part], env=env, capture_output=True, text=True)
        if run.returncode == 0:
            print(f"NEGATIVE CONTROL FAILED ({part}): {label} passed", file=sys.stderr)
            sys.exit(1)
        tail = "\n".join(run.stderr.strip().splitlines()[:4])
        print(f"negative control {part} ({label}): fails as expected, exit {run.returncode}\n{tail}")


if args.scratch:
    args.scratch.mkdir(parents=True, exist_ok=True)
    run_in(args.scratch.resolve())
else:
    with tempfile.TemporaryDirectory(prefix="dtappgather-test-", dir=default_tmp_root()) as tmp:
        run_in(Path(tmp))
