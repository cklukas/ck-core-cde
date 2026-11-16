#!/usr/bin/env python3
"""Regression test for dthelpprint and dthelp parser hardening."""

import argparse
import os
import subprocess
import sys
from pathlib import Path

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
    dthelpprint_dir = script_dir.parent
    repo_root = dthelpprint_dir.parents[3]

    if args.scratch is not None:
        scratch_dir = args.scratch.resolve()
    else:
        s8t_tmp = Path("/Volumes/S8T/tmp")
        if s8t_tmp.is_dir() and os.access(s8t_tmp, os.W_OK):
            scratch_dir = s8t_tmp / "cde-hardening-d952b1d5" / "dthelpprint-hardening"
        else:
            tmp_base = Path(os.environ.get("TMPDIR", str(dthelpprint_dir)))
            scratch_dir = tmp_base / "dthelpprint-hardening"
    scratch_dir.mkdir(parents=True, exist_ok=True)

    if args.source_revision:
        init_src = git_show(repo_root, args.source_revision, "cde/programs/dthelp/dthelpprint/Initialize.c")
        topics_src = git_show(repo_root, args.source_revision, "cde/programs/dthelp/dthelpprint/PrintTopics.c")
        util_src = git_show(repo_root, args.source_revision, "cde/programs/dthelp/dthelpprint/PrintUtil.c")
        xref_src = git_show(repo_root, args.source_revision, "cde/programs/dthelp/parser/canon1/helptag/xref.c")
    else:
        init_src = (dthelpprint_dir / "Initialize.c").read_text(encoding="utf-8")
        topics_src = (dthelpprint_dir / "PrintTopics.c").read_text(encoding="utf-8")
        util_src = (dthelpprint_dir / "PrintUtil.c").read_text(encoding="utf-8")
        xref_src = (repo_root / "cde/programs/dthelp/parser/canon1/helptag/xref.c").read_text(encoding="utf-8")

    init_src = init_src.replace("nullptr", "NULL")
    topics_src = topics_src.replace("nullptr", "NULL")
    util_src = util_src.replace("nullptr", "NULL")

    parts = [
        extract_function(init_src, "char * _DtHPrCreateTmpFile("),
        extract_function(topics_src, "char * SectNumStr("),
        extract_function(util_src, "void PutOpt("),
    ]

    inc_path = scratch_dir / "dthelpprint_extracted.inc"
    inc_path.write_text("\n\n".join(parts) + "\n", encoding="utf-8")

    bin_path = scratch_dir / "check-dthelpprint-hardening"
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
        f"-fsanitize={_CK_SANITIZE_LIST}",
        f"-I{scratch_dir}",
        str(script_dir / "check-dthelpprint-hardening.c"),
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
    run = subprocess.run([str(bin_path), str(scratch_dir)], env=env, capture_output=True, text=True)
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

    # Verify canon1/helptag/xref.c does not close(tex) before defxref(tex, ...)
    dumpxref_body = extract_function(xref_src, "void dumpxref(void)")
    if "fputs(\"% Generated Cross-Reference Macros (for a particular document)\\n\", tex);\nfclose(tex);" in dumpxref_body:
        sys.stderr.write("ERROR: canon1/helptag/xref.c still closes tex before defxref\n")
        return 1

    print(run.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
