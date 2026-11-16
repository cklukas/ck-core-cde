#!/usr/bin/env python3
"""Sanitizer regression test for dtdocbook, dtudcexch, dtudcfonted, and dtexec hardening."""
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
            return candidate / "ck-core-cde-dtdocbook-udc-hardening"
    return repo_root / "tests-scratch" / "dtdocbook-udc-hardening"


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
    repo_root = here.parents[4]

    ap = argparse.ArgumentParser()
    ap.add_argument("--source-revision", default=None)
    ap.add_argument("--expect-fail", action="store_true")
    ap.add_argument("--scratch", type=Path, default=None)
    args = ap.parse_args()

    scratch = args.scratch or default_scratch_root(repo_root)
    scratch.mkdir(parents=True, exist_ok=True)

    importbdf_src = read_source(repo_root, "cde/programs/dtudcexch/importbdf.c", args.source_revision)
    selectxlfd_src = read_source(repo_root, "cde/programs/dtudcexch/selectxlfd.c", args.source_revision)
    dtexec_src = read_source(repo_root, "cde/programs/dtexec/Main.c", args.source_revision)

    extracted = "\n".join(
        [
            "int" + extract_function(importbdf_src, "\nimpMergePtn(\n"),
            extract_function(selectxlfd_src, "char i2c(int num)"),
            extract_function(selectxlfd_src, "char *i2s(int code)"),
            extract_function(dtexec_src, "static char *\nCommandListJoin(const CommandList *list)"),
        ]
    )

    template = (here / "check-dtdocbook-udc-hardening.c").read_text(encoding="utf-8")
    combined_c = template.replace("/* @@EXTRACTED_FUNCTIONS@@ */", extracted)

    tag = args.source_revision or "worktree"
    c_path = scratch / f"check-dtdocbook-udc-hardening-{tag}.c"
    bin_path = scratch / f"check-dtdocbook-udc-hardening-{tag}"
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
    comp = subprocess.run(compile_cmd, capture_output=True, text=True)
    if comp.returncode != 0:
        if args.expect_fail:
            print(f"expected compile/test failure observed for {tag}")
            return 0
        sys.stderr.write(comp.stderr)
        return 1

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
