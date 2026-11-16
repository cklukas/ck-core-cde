#!/usr/bin/env python3
"""Regression test for dtksh converter, widget lookup, and bounds hardening."""

import argparse
import os
import subprocess
import sys
from pathlib import Path


def git_show(repo_root: Path, rev: str, rel_path: str) -> str:
    res = subprocess.run(
        ["git", "-C", str(repo_root), "show", f"{rev}:{rel_path}"],
        check=True,
        capture_output=True,
        text=True,
    )
    return res.stdout


def extract_brace_block(text: str, start_idx: int) -> str:
    brace_start = text.index("{", start_idx)
    depth = 0
    in_str = False
    in_chr = False
    in_line_comment = False
    in_block_comment = False
    escape = False
    for idx in range(brace_start, len(text)):
        ch = text[idx]
        nxt = text[idx + 1] if idx + 1 < len(text) else ""
        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
            continue
        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
            continue
        if escape:
            escape = False
            continue
        if ch == "\\":
            escape = True
            continue
        if not in_chr and ch == '"':
            in_str = not in_str
            continue
        if not in_str and ch == "'":
            in_chr = not in_chr
            continue
        if in_str or in_chr:
            continue
        if ch == "/" and nxt == "/":
            in_line_comment = True
            continue
        if ch == "/" and nxt == "*":
            in_block_comment = True
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[start_idx : idx + 1]
    raise RuntimeError(f"Unclosed brace block at {start_idx}")


def extract_function(text: str, signature: str) -> str:
    idx = text.rindex(signature)
    line_start = text.rfind("\n", 0, idx)
    prev_line_start = text.rfind("\n", 0, line_start) if line_start > 0 else 0
    prev_line = text[prev_line_start:line_start].strip()
    if prev_line and not prev_line.endswith(";") and not prev_line.endswith("}") and not prev_line.endswith("*/"):
        start = prev_line_start + 1
    else:
        start = line_start + 1 if line_start >= 0 else idx
    return extract_brace_block(text, start)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-revision", default=None)
    parser.add_argument("--expect-fail", action="store_true")
    parser.add_argument("--scratch", type=Path, default=None)
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    dtksh_dir = script_dir.parent
    repo_root = dtksh_dir.parents[2]

    if args.scratch is not None:
        scratch_dir = args.scratch.resolve()
    else:
        s8t_tmp = Path("/Volumes/S8T/tmp")
        if s8t_tmp.is_dir() and os.access(s8t_tmp, os.W_OK):
            scratch_dir = s8t_tmp / "cde-hardening-d952b1d5" / "dtksh-hardening"
        else:
            tmp_base = Path(os.environ.get("TMPDIR", str(dtksh_dir)))
            scratch_dir = tmp_base / "dtksh-hardening"
    scratch_dir.mkdir(parents=True, exist_ok=True)

    if args.source_revision:
        xmcvt_src = git_show(repo_root, args.source_revision, "cde/programs/dtksh/xmcvt.c")
        dtkcvt_src = git_show(repo_root, args.source_revision, "cde/programs/dtksh/dtkcvt.c")
        widget_src = git_show(repo_root, args.source_revision, "cde/programs/dtksh/widget.c")
    else:
        xmcvt_src = (dtksh_dir / "xmcvt.c").read_text(encoding="utf-8")
        dtkcvt_src = (dtksh_dir / "dtkcvt.c").read_text(encoding="utf-8")
        widget_src = (dtksh_dir / "widget.c").read_text(encoding="utf-8")

    xmcvt_src = xmcvt_src.replace("nullptr", "NULL")
    dtkcvt_src = dtkcvt_src.replace("nullptr", "NULL")
    widget_src = widget_src.replace("nullptr", "NULL")

    parts = [
        extract_function(xmcvt_src, "char *\n_CvtXmStringTableToString("),
        extract_function(xmcvt_src, "void\nDtkshCvtStringToNamedValue("),
        extract_function(xmcvt_src, "void\nDtkshCvtStringToTtScope("),
        extract_function(dtkcvt_src, "void\nDtkshCvtWindowToString("),
        extract_function(dtkcvt_src, "void\nDtkshCvtScreenToString("),
        extract_function(dtkcvt_src, "void\nDtkshCvtHexIntToString("),
        extract_function(dtkcvt_src, "void\nDtkshCvtIntToString("),
        extract_function(dtkcvt_src, "void\nDtkshCvtCallbackToString("),
        extract_function(widget_src, "Widget\nDtkshNameToWidget("),
        extract_function(widget_src, "static char *\ngetname("),
    ]

    inc_path = scratch_dir / "dtksh_extracted.inc"
    inc_path.write_text("\n\n".join(parts) + "\n", encoding="utf-8")

    bin_path = scratch_dir / "check-dtksh-hardening"
    cc = os.environ.get("CC", "cc")
    cmd = [
        cc,
        "-std=gnu11",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Wno-unused-function",
        "-Wno-unused-parameter",
        "-Wno-deprecated-declarations",
        "-fsanitize=address,undefined",
        f"-I{scratch_dir}",
        str(script_dir / "check-dtksh-hardening.c"),
        "-o",
        str(bin_path),
    ]
    comp = subprocess.run(cmd, capture_output=True, text=True)
    if comp.returncode != 0:
        if args.expect_fail:
            print(f"negative control ({args.source_revision}): compile failed as expected")
            return 0
        sys.stderr.write(comp.stderr)
        return comp.returncode

    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    run = subprocess.run([str(bin_path)], env=env, capture_output=True, text=True)
    if args.expect_fail:
        if run.returncode != 0:
            print(f"negative control ({args.source_revision}): fails as expected (rc={run.returncode})")
            return 0
        sys.stderr.write("ERROR: expected failure on unpatched source revision, but test passed\n")
        return 1

    if run.returncode != 0:
        sys.stderr.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode

    print(run.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
