#!/usr/bin/env python3
"""Sanitizer regression test for dtcreate, dtpad, dticon, dtcalc, and dtimsstart."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

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



def default_scratch_root(repo_root: Path) -> Path:
    for candidate in (Path("/Volumes/S8T/tmp"), Path("/Volumes/PRO-BLADE/tmp")):
        if candidate.is_dir() and os.access(candidate, os.W_OK):
            return candidate / "ck-core-cde-desktop-apps-hardening"
    return repo_root / "tests-scratch" / "desktop-apps-hardening"


def read_source(repo_root: Path, relpath: str, revision: str | None) -> str:
    if revision:
        return subprocess.run(
            ["git", "-C", str(repo_root), "show", f"{revision}:{relpath}"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    return (repo_root / relpath).read_text(encoding="utf-8")


def extract_function(source: str, start_marker: str) -> str:
    start = source.find(start_marker)
    if start < 0:
        raise RuntimeError(f"start marker not found: {start_marker!r}")
    brace_open = source.find("{", start)
    if brace_open < 0:
        raise RuntimeError(f"opening brace not found for: {start_marker!r}")
    depth = 0
    for idx in range(brace_open, len(source)):
        ch = source[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start : idx + 1] + "\n"
    raise RuntimeError(f"closing brace not found for: {start_marker!r}")


def main() -> int:
    here = Path(__file__).resolve().parent
    repo_root = here.parents[3]

    ap = argparse.ArgumentParser()
    ap.add_argument("--source-revision", default=None)
    ap.add_argument("--expect-fail", action="store_true")
    ap.add_argument("--scratch", type=Path, default=None)
    args = ap.parse_args()

    scratch = args.scratch or default_scratch_root(repo_root)
    scratch.mkdir(parents=True, exist_ok=True)

    fc_src = read_source(repo_root, "cde/programs/dtcreate/FileCharacteristics.c", args.source_revision)
    parser_src = read_source(repo_root, "cde/programs/dtcreate/parser.c", args.source_revision)
    fileio_src = read_source(repo_root, "cde/programs/dtpad/fileIo.c", args.source_revision)
    filedlg_src = read_source(repo_root, "cde/programs/dtpad/fileDlg.c", args.source_revision)
    dticon_src = read_source(repo_root, "cde/programs/dticon/main.c", args.source_revision)
    dtcalc_src = read_source(repo_root, "cde/programs/dtcalc/calctool.c", args.source_revision)
    dtims_src = read_source(repo_root, "cde/programs/dtimsstart/env.c", args.source_revision)

    extracted = "\n".join(
        [
            extract_function(fc_src, "void ParseAndUpdateID(FiletypeData *pFtD)"),
            extract_function(parser_src, "int\nProcessContents(FiletypeData *pFtD)"),
            extract_function(fileio_src, "static char * \nAddPound("),
            extract_function(filedlg_src, "void\nExtractAndStoreDir("),
            extract_function(dticon_src, "void\nsend_tt_saved(void)"),
            extract_function(dtcalc_src, "void\nprocess_item(int n)"),
            extract_function(dtims_src, "int\texpand_string(char *in_str, char *out_str, int out_len, ImsConf *ims)"),
        ]
    )

    template = (here / "check-desktop-apps-hardening.c").read_text(encoding="utf-8")
    combined_c = template.replace("/* @@EXTRACTED_FUNCTIONS@@ */", extracted)

    tag = args.source_revision or "worktree"
    c_path = scratch / f"check-desktop-apps-hardening-{tag}.c"
    bin_path = scratch / f"check-desktop-apps-hardening-{tag}"
    c_path.write_text(combined_c, encoding="utf-8")

    cc = shlex.split(os.environ.get("CC", "cc"))
    compile_cmd = cc + [
        "-std=gnu23",
        "-O1",
        "-g",
        "-fno-omit-frame-pointer",
        f"-fsanitize={_CK_SANITIZE_LIST}",
        "-fno-sanitize-recover=all",
        str(c_path),
        "-o",
        str(bin_path),
    ]
    subprocess.run(compile_cmd, check=True)

    env = dict(
        os.environ,
        ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0",
        UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1",
    )
    res = subprocess.run([str(bin_path)], env=env, capture_output=True, text=True)
    if args.expect_fail:
        if res.returncode == 0:
            print(f"UNEXPECTED PASS for {tag}", file=sys.stderr)
            return 1
        print(f"expected failure observed for {tag} (rc={res.returncode})")
        return 0

    if res.returncode != 0:
        sys.stderr.write(res.stderr)
        print(f"FAIL ({tag}): rc={res.returncode}", file=sys.stderr)
        return 1

    sys.stdout.write(res.stdout)
    print(f"PASS ({tag})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
