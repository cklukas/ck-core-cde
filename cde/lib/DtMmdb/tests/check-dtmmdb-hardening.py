#!/usr/bin/env python3
"""Sanitizer regression test for DtMmdb and dtinfo hardening fixes."""
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
            return candidate / "ck-core-cde-dtmmdb-hardening"
    return repo_root / "tests-scratch" / "dtmmdb-hardening"


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

    mempool_src = read_source(repo_root, "cde/lib/DtMmdb/dstr/memory_pool.C", args.source_revision)
    buffer_src = read_source(repo_root, "cde/lib/DtMmdb/utility/buffer.C", args.source_revision)
    dtsr_src = read_source(
        repo_root,
        "cde/programs/dtinfo/dtinfo/src/UAS/DtSR/DtSR_SearchResultsEntry.C",
        args.source_revision,
    )

    extracted = "\n".join(
        [
            extract_function(mempool_src, "memory_pool::memory_pool(int x)"),
            extract_function(mempool_src, "memory_pool::~memory_pool()"),
            extract_function(mempool_src, "char* memory_pool::alloc(size_t sz)"),
            extract_function(mempool_src, "void memory_pool::free(char* str)"),
            extract_function(buffer_src, "void buffer::_alloc(const int sz)"),
            extract_function(buffer_src, "buffer::~buffer()"),
            extract_function(buffer_src, "int buffer::expand_chunk(const int newsz)"),
            extract_function(dtsr_src, "int\nDtSR_SearchResultsEntry::unset_proximity("),
            extract_function(dtsr_src, "int\nDtSR_SearchResultsEntry::set_proximity("),
            extract_function(dtsr_src, "int\nDtSR_SearchResultsEntry::overlay_proximity("),
            extract_function(dtsr_src, "int\nDtSR_SearchResultsEntry::get_proximity("),
        ]
    )

    template = (here / "check-dtmmdb-hardening.C").read_text(encoding="utf-8")
    combined_cxx = template.replace("/* @@EXTRACTED_FUNCTIONS@@ */", extracted)

    tag = args.source_revision or "worktree"
    cxx_path = scratch / f"check-dtmmdb-hardening-{tag}.cc"
    bin_path = scratch / f"check-dtmmdb-hardening-{tag}"
    cxx_path.write_text(combined_cxx, encoding="utf-8")

    cxx = shlex.split(os.environ.get("CXX", "c++"))
    compile_cmd = cxx + [
        "-std=gnu++17",
        "-O1",
        "-g",
        "-fno-omit-frame-pointer",
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all",
        str(cxx_path),
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
