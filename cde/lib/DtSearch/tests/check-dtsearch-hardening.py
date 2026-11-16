#!/usr/bin/env python3
"""Sanitizer regression test for DtSearch hardening fixes."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys


def default_scratch_root(repo_root: Path) -> Path:
    for candidate in (Path("/Volumes/S8T/tmp"), Path("/Volumes/PRO-BLADE/tmp")):
        if candidate.is_dir() and os.access(candidate, os.W_OK):
            return candidate / "ck-core-cde-dtsearch-hardening"
    return repo_root / "tests-scratch" / "dtsearch-hardening"


def read_source(repo_root: Path, relpath: str, revision: str | None) -> str:
    if revision:
        return subprocess.run(
            ["git", "-C", str(repo_root), "show", f"{revision}:{relpath}"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    return (repo_root / relpath).read_text(encoding="utf-8")


def extract_function(source: str, start_marker: str, end_marker: str) -> str:
    start = source.find(start_marker)
    if start < 0:
        raise RuntimeError(f"start marker not found: {start_marker!r}")
    end = source.find(end_marker, start)
    if end < 0:
        raise RuntimeError(f"end marker not found: {end_marker!r}")
    return source[start : end + len(end_marker)] + "\n"


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

    msgutil_src = read_source(repo_root, "cde/lib/DtSearch/msgutil.c", args.source_revision)
    dtsrjoint_src = read_source(repo_root, "cde/lib/DtSearch/dtsrjoint.c", args.source_revision)
    dtsrapi_src = read_source(repo_root, "cde/lib/DtSearch/dtsrapi.c", args.source_revision)
    fileman_src = read_source(repo_root, "cde/lib/DtSearch/fileman.c", args.source_revision)

    extracted = "\n".join(
        [
            extract_function(msgutil_src, "static LLIST   *split_llist (LLIST * lst)", "}  /* split_llist() */"),
            extract_function(msgutil_src, "static LLIST   *merge_llist (LLIST * l1, LLIST * l2)", "}  /* merge_llist() */"),
            extract_function(msgutil_src, "LLIST          *sort_llist (LLIST * lst)", "}  /* sort_llist() */"),
            extract_function(msgutil_src, "int             clean_wrap (char *string, int wraplen)", "}  /* clean_wrap() */"),
            extract_function(dtsrjoint_src, "static DtSrResult *ditto_pop (DtSrResult ** lst)", "}  /* ditto_pop() */"),
            extract_function(dtsrjoint_src, "static DtSrResult *ditto_split (DtSrResult * lst)", "}  /* ditto_split() */"),
            extract_function(dtsrjoint_src, "static DtSrResult *merge_by_prox (DtSrResult * l1, DtSrResult * l2)", "}  /* merge_by_prox() */"),
            extract_function(dtsrjoint_src, "static DtSrResult *merge_by_date (DtSrResult * l1, DtSrResult * l2)", "}  /* merge_by_date() */"),
            extract_function(dtsrjoint_src, "static DtSrResult *ditto_sort (DtSrResult * lst)", "}  /* ditto_sort() */"),
            extract_function(dtsrapi_src, "static int      both_valid_dates (char *date1, char *date2)", "}  /* both_valid_dates() */"),
            extract_function(fileman_src, "int\tfread_d99_header (FILE_HEADER *flh, FILE *fp)", "}  /* fread_d99_header() */"),
        ]
    )

    template = (here / "check-dtsearch-hardening.c").read_text(encoding="utf-8")
    combined_c = template.replace("/* @@EXTRACTED_FUNCTIONS@@ */", extracted)

    tag = args.source_revision or "worktree"
    c_path = scratch / f"check-dtsearch-hardening-{tag}.c"
    bin_path = scratch / f"check-dtsearch-hardening-{tag}"
    c_path.write_text(combined_c, encoding="utf-8")

    cc = shlex.split(os.environ.get("CC", "cc"))
    compile_cmd = cc + [
        "-std=gnu23",
        "-O1",
        "-g",
        "-fno-omit-frame-pointer",
        "-fsanitize=address,undefined",
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
