#!/usr/bin/env python3
"""Check the hash table and the name table of libcsa with every allocation failing.

The structures and the functions that make the hash table, look up or add an
entry and destroy it are extracted verbatim from hash.c, and those that make,
extend and free a name table from nametbl.c (the working tree, or another
revision with --source-revision REV).  They are compiled together with
check-name-table-alloc.c under AddressSanitizer and UndefinedBehaviorSanitizer
with halt on error.  The harness makes each allocation fail in turn and
asserts the error result, the absence of a write through a null pointer and
of a leak.  Every extraction fails loudly unless its anchors occur exactly
once.

Default: the test must pass.
With --source-revision REV: negative control; REV must FAIL the same test
(exit status 0 means the failure was observed).
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
directory = "cde/lib/csa/"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

if args.scratch:
    scratch = args.scratch
    scratch.mkdir(parents=True, exist_ok=True)
else:
    scratch = Path(tempfile.mkdtemp(prefix="name-table-alloc-", dir=os.environ.get("TMPDIR")))
    atexit.register(shutil.rmtree, scratch, True)
label = args.source_revision or "working tree"


def source(name):
    relative = directory + name
    if args.source_revision:
        return subprocess.run(["git", "-C", str(args.worktree), "show",
                               f"{args.source_revision}:{relative}"],
                              check=True, capture_output=True, text=True).stdout
    return (args.worktree / relative).read_text()


def extract(text, name, start_anchor, end_anchor):
    """Return the text from start_anchor through end_anchor; both must be unique."""
    n = text.count(start_anchor)
    if n != 1:
        sys.exit(f"anchor {start_anchor.strip()!r} found {n} times in {name}, expected 1")
    start = text.index(start_anchor)
    end = text.index(end_anchor, start) + len(end_anchor)
    return text[start:end] + "\n"


# Each function ends at its closing brace in column 0.
hash_text, name_text = source("hash.c"), source("nametbl.c")
hash_string_anchor = (
    "static int hash_string(const unsigned char *s, int modulo)\n{"
    if "static int hash_string(const unsigned char *s, int modulo)\n{" in hash_text
    else "static int hash_string(const char *s, int modulo)\n{"
)
hash_string_decl = hash_string_anchor.splitlines()[0] + ";\n"
production = (
    extract(hash_text, "hash.c", "typedef struct hash_entry {", "} hash;") +
    hash_string_decl +
    extract(hash_text, "hash.c", "\nvoid * _DtCmMakeHash(int size)\n{", "\n}\n") +
    extract(hash_text, "hash.c", "\nvoid ** _DtCmGetHash(void * t,", "\n}\n") +
    extract(hash_text, "hash.c", "\nvoid ** _DtCmFindHash(void * t,", "\n}\n") +
    extract(hash_text, "hash.c", "\nvoid _DtCmDestroyHash(\n", "\n}\n") +
    extract(hash_text, "hash.c", hash_string_anchor, "\n}\n") +
    extract(name_text, "nametbl.c", "extern _DtCmNameTable *\n_DtCm_make_name_table(", "\n}\n") +
    extract(name_text, "nametbl.c", "extern void\n_DtCm_free_name_table(", "\n}\n") +
    extract(name_text, "nametbl.c", "extern CSA_return_code\n_DtCm_add_name_to_table(", "\n}\n") +
    extract(name_text, "nametbl.c", "extern int\n_DtCm_get_index_from_table(", "\n}\n"))

parts = (here / "check-name-table-alloc.c").read_text()
prologue = parts.split("/* @@PROLOGUE@@ */")[1].split("/* @@BODY@@ */")[0]
body = parts.split("/* @@BODY@@ */")[1]

tag = "control" if args.source_revision else "fixed"
c, binary = scratch / f"name-table-alloc-{tag}.c", scratch / f"name-table-alloc-{tag}"
c.write_text(prologue + production + "\n" + body)
subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
               ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-deprecated-declarations", "-Wno-unused-value", "-Wno-unused-parameter",
                "-Wno-unused-but-set-variable", "-Wno-pointer-sign", "-Wno-absolute-value",
                f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)

env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0:symbolize=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
run = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
sys.stdout.write(run.stdout)
if args.source_revision:
    tail = "\n".join(run.stderr.strip().splitlines()[:6])
    if run.returncode == 0:
        print(f"NEGATIVE CONTROL FAILED: {label} passed the test", file=sys.stderr)
        sys.exit(1)
    print(f"negative control ({label}): fails as expected, exit {run.returncode}\n{tail}")
else:
    if run.returncode:
        sys.stderr.write(run.stderr)
        print(f"FAIL ({label}): exit {run.returncode}", file=sys.stderr)
        sys.exit(1)
    print(f"PASS ({label})")
