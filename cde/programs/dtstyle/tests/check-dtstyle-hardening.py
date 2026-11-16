#!/usr/bin/env python3
"""Check dtstyle palette, resource, and I18n input-path hardening.

CheckForDeletedFile(), ReadInInfo(), and RemovePaletteFromList() from
ColorFile.c, GetDefaultPal() from ColorMain.c, GetImsList(), ReadPipe(), and
ProcessBuf() from I18nEnv.c, and _DtI18nErrorDialog() from I18nMain.c are
extracted verbatim from the working tree (or --source-revision REV) and
compiled with check-dtstyle-hardening.c under AddressSanitizer and
UndefinedBehaviorSanitizer with halt on error.

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
TEST = "check-dtstyle-hardening.c"
TAG = "dtstyle-hardening"

ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--expect-fail", action="store_true",
                help="succeed only if the tested source revision fails the test")
ap.add_argument("--worktree", type=Path, default=root)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()

expect_fail = args.expect_fail or bool(args.source_revision)
label = args.source_revision or "working tree"


def source(relative):
    """Text of a source file: the working tree, or the revision of the control."""
    if args.source_revision:
        return subprocess.run(["git", "-C", str(args.worktree), "show",
                               f"{args.source_revision}:{relative}"],
                              check=True, capture_output=True, text=True).stdout
    return (args.worktree / relative).read_text()


def extract(text, relative, start_anchor, end_anchor):
    """Return the text from start_anchor through end_anchor; start must be unique."""
    n = text.count(start_anchor)
    if n != 1:
        sys.exit(f"anchor {start_anchor.strip()!r} found {n} times in {relative}, expected 1")
    start = text.index(start_anchor)
    end = text.index(end_anchor, start + len(start_anchor)) + len(end_anchor)
    return text[start:end] + "\n"


def build():
    color_file = source("cde/programs/dtstyle/ColorFile.c")
    color_main = source("cde/programs/dtstyle/ColorMain.c")
    i18n_env = source("cde/programs/dtstyle/I18nEnv.c")
    i18n_main = source("cde/programs/dtstyle/I18nMain.c")

    pieces = [
        "static palette *RemovePaletteFromList( palette *tmp_palette );\n",
        extract(color_file, "ColorFile.c", "void\nCheckForDeletedFile(", "\n}\n"),
        extract(color_file, "ColorFile.c", "static void\nReadInInfo(", "\n} \n"),
        extract(color_file, "ColorFile.c", "static palette * \nRemovePaletteFromList(", "\n}\n"),
        extract(color_main, "ColorMain.c", "void \nGetDefaultPal(", "\n}\n"),
        extract(i18n_env, "I18nEnv.c", "static void\nReadPipe (", "\n}\n"),
        extract(i18n_env, "I18nEnv.c", "static int\nProcessBuf(", "\n}\n"),
        extract(i18n_env, "I18nEnv.c", "static void TimeOutProc (", "\n}\n"),
        extract(i18n_env, "I18nEnv.c", "static int \nGetImsList(", "\n}\n"),
        extract(i18n_main, "I18nMain.c", "void\n_DtI18nErrorDialog(", "\n}\n"),
    ]
    return "".join(pieces)


def main(scratch):
    parts = (here / TEST).read_text()
    template = parts.split("/* @@PROLOGUE@@ */")[1]
    template = template.replace("/* @@EXTRACTED@@ */", build())
    tag = TAG + ("-control" if expect_fail else "-fixed")
    c, binary = scratch / f"{tag}.c", scratch / tag
    c.write_text(template)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu23", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                    "-Wno-deprecated-declarations", "-Wno-unused-value",
                    "-Wno-unused-but-set-variable", "-Wno-format-security",
                    "-Wno-unused-parameter", "-Wno-parentheses",
                    f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-g", str(c), "-o", str(binary)], check=True)
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    return subprocess.run([str(binary)], env=env, capture_output=True, text=True)


default_tmp = "/Volumes/S8T/tmp" if os.path.isdir("/Volumes/S8T/tmp") and os.access("/Volumes/S8T/tmp", os.W_OK) else None
if args.scratch:
    args.scratch.mkdir(parents=True, exist_ok=True)
    run = main(args.scratch.resolve())
else:
    with tempfile.TemporaryDirectory(prefix=TAG + "-", dir=default_tmp) as tmp:
        run = main(Path(tmp))
sys.stdout.write(run.stdout)
if expect_fail:
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
