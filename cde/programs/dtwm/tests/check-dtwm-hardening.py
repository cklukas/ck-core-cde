#!/usr/bin/env python3
"""Compile and run standalone ASan/UBSan tests for dtwm input-path & bounds hardening."""

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



def load_source(repo_root: Path, rel_path: Path, revision):
    if revision:
        proc = subprocess.run(
            ["git", "-C", str(repo_root), "show", f"{revision}:{rel_path.as_posix()}"],
            check=True,
            capture_output=True,
            text=True,
        )
        return proc.stdout
    return (repo_root / rel_path).read_text(encoding="utf-8")


def extract_function(text: str, signature: str) -> str:
    start = text.rindex(signature)
    brace_open = text.index("{", start)
    depth = 1
    idx = brace_open + 1
    in_str = False
    in_chr = False
    in_line_comment = False
    in_block_comment = False
    while idx < len(text) and depth > 0:
        ch = text[idx]
        nxt = text[idx + 1] if idx + 1 < len(text) else ""
        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
        elif in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                idx += 1
        elif in_str:
            if ch == "\\":
                idx += 1
            elif ch == '"':
                in_str = False
        elif in_chr:
            if ch == "\\":
                idx += 1
            elif ch == "'":
                in_chr = False
        else:
            if ch == "/" and nxt == "/":
                in_line_comment = True
                idx += 1
            elif ch == "/" and nxt == "*":
                in_block_comment = True
                idx += 1
            elif ch == '"':
                in_str = True
            elif ch == "'":
                in_chr = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
        idx += 1
    return text[start:idx]


def main() -> int:
    here = Path(__file__).resolve().parent
    dtwm_dir = here.parent
    repo_root = here.parents[3]

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-revision",
        metavar="REV",
        help="extract dtwm functions from git revision REV instead of working tree",
    )
    parser.add_argument(
        "--expect-fail",
        action="store_true",
        help="succeed only if the compiled test binary fails (negative control)",
    )
    parser.add_argument(
        "--scratch",
        type=Path,
        default=None,
        help="scratch directory for temporary build artifacts",
    )
    args = parser.parse_args()

    rel_dtwm = dtwm_dir.relative_to(repo_root)
    wm_parse = load_source(repo_root, rel_dtwm / "WmParse.c", args.source_revision)
    wm_res_parse = load_source(repo_root, rel_dtwm / "WmResParse.c", args.source_revision)
    db_load = load_source(repo_root, rel_dtwm / "DataBaseLoad.c", args.source_revision)
    wm_wrkspace = load_source(repo_root, rel_dtwm / "WmWrkspace.c", args.source_revision)
    wm_resource = load_source(repo_root, rel_dtwm / "WmResource.c", args.source_revision)

    parts = [
        extract_function(wm_parse, "unsigned char *\n_DtWmParseFilenameExpand ("),
        extract_function(wm_res_parse, "void ParseSessionClientState (WmScreenData *pSD"),
        extract_function(wm_res_parse, "void ParseSessionGeometry (WmScreenData *pSD"),
        extract_function(wm_res_parse, "void ParseSessionWorkspaces (WmScreenData *pSD"),
        extract_function(wm_res_parse, "void ParseSessionCommand (WmScreenData *pSD"),
        extract_function(wm_res_parse, "void ParseSessionHost (WmScreenData *pSD"),
        extract_function(wm_res_parse, "static unsigned int StrToHex(unsigned char *str)\n{"),
        extract_function(wm_res_parse, "static unsigned int StrToOct(unsigned char *str)\n{"),
        extract_function(wm_res_parse, "static unsigned int StrToNum(unsigned char *str)\n{"),
        extract_function(wm_res_parse, "Boolean ParseBtnEvent (unsigned char  **linePP,"),
        extract_function(db_load, "static void\nEliminateEntries ("),
        extract_function(db_load, "static void\nResolveDuplicates ("),
        extract_function(wm_wrkspace, "WmWorkspaceData * \nCreateWorkspace("),
        extract_function(wm_wrkspace, "void \nRemoveClientFromWsList("),
        extract_function(wm_resource, "String\nResCat ("),
    ]

    scratch_base = args.scratch or (
        Path(os.environ["TMPDIR"]) if "TMPDIR" in os.environ else None
    )
    if scratch_base is not None:
        scratch_base.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="dtwm-hardening-", dir=scratch_base) as tmp:
        tmp_path = Path(tmp)
        extracted_h = tmp_path / "dtwm_extracted.h"
        extracted_h.write_text("\n\n".join(parts) + "\n", encoding="utf-8")
        binary = tmp_path / "check-dtwm-hardening"

        cc = shlex.split(os.environ.get("CC", "cc"))
        cflags = shlex.split(os.environ.get("CFLAGS", ""))
        cmd = (
            cc
            + cflags
            + [
                "-std=gnu23",
                "-g",
                "-O1",
                "-Wall",
                "-Wextra",
                "-Wno-unused-parameter",
                "-Wno-unused-variable",
                "-Wno-sign-compare",
                f"-fsanitize={_CK_SANITIZE_LIST}",
                "-fno-sanitize-recover=all",
                f'-DDTWM_EXTRACTED_HEADER="{extracted_h}"',
                str(here / "check-dtwm-hardening.c"),
                "-o",
                str(binary),
            ]
        )
        subprocess.run(cmd, check=True)
        proc = subprocess.run([str(binary)], capture_output=True, text=True)
        if args.expect_fail:
            if proc.returncode == 0:
                sys.stderr.write(
                    "UNEXPECTED PASS on negative control:\n"
                    + proc.stdout
                    + proc.stderr
                )
                return 1
            print(
                f"negative control ({args.source_revision or 'working-tree'}): "
                f"fails as expected (rc={proc.returncode})"
            )
            return 0

        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            return proc.returncode
        sys.stdout.write(proc.stdout)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
