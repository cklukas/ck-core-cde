#!/usr/bin/env python3
"""Compile and run the ttsnoop hardening regression test."""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[4],
        help="Path to the repository root",
    )
    parser.add_argument(
        "--source-revision",
        default=None,
        help="Optional git revision to extract cde/programs/ttsnoop from",
    )
    parser.add_argument(
        "--cxx",
        default=os.environ.get("CXX", "c++"),
        help="C++ compiler to use",
    )
    parser.add_argument(
        "--scratch",
        type=pathlib.Path,
        default=None,
        help="Directory for temporary build artifacts",
    )
    parser.add_argument(
        "--keep-scratch",
        action="store_true",
        help="Preserve the scratch directory",
    )
    parser.add_argument(
        "--expect-fail",
        action="store_true",
        help="Succeed only if at least one regression test part fails",
    )
    return parser.parse_args()


def extract_tree(repo_root: pathlib.Path, revision: str, dest: pathlib.Path) -> None:
    archive = subprocess.run(
        ["git", "-C", str(repo_root), "archive", revision, "cde/programs/ttsnoop"],
        check=True,
        stdout=subprocess.PIPE,
    )
    subprocess.run(
        ["tar", "-xf", "-", "-C", str(dest)],
        input=archive.stdout,
        check=True,
    )


def verify_source_contracts(ttsnoop_dir: pathlib.Path) -> list[str]:
    failures: list[str] = []

    ttsnoop_main = (ttsnoop_dir / "ttsnoop.C.src").read_text(errors="ignore")
    if 'getopt(argc, argv, "?F:<:v:m:nNe:TuS:Xw:l:o:O:")' not in ttsnoop_main:
        failures.append("ttsnoop.C.src getopt optstring is missing m:")

    arg_chooser = (ttsnoop_dir / "argChooser_stubs.C.src").read_text(errors="ignore")
    if not re.search(
        r"_DtTtMessageUpdate\(\s*msg,\s*choice,\s*nth,\s*mode,\s*vtype,\s*val,\s*ival\s*\);\s*break;",
        arg_chooser,
    ):
        failures.append("argChooser_stubs.C.src falls through from message update to pattern update")

    tt_chooser = (ttsnoop_dir / "ttChooser_stubs.C.src").read_text(errors="ignore")
    if "case _DtTtChooserDtFileDeleted:" not in tt_chooser[tt_chooser.find("choiceOkayed"):]:
        failures.append("ttChooser_stubs.C.src choiceOkayed is missing _DtTtChooserDtFileDeleted")
    if not re.search(
        r"DtTtSetLabel\(\s*label,\s*func,\s*status\s*\);\s*XtUnmanageChild\(\s*instance->chooser_shellform\s*\);\s*break;\s*case _DtTtChooserProcidSetDefault:",
        tt_chooser,
    ):
        failures.append("ttChooser_stubs.C.src falls through into _DtTtChooserProcidSetDefault")

    msg_props = (ttsnoop_dir / "messageProps_stubs.C.src").read_text(errors="ignore")
    reject_idx = msg_props.find("msgReject(")
    if reject_idx == -1 or "tt_message_reject(" not in msg_props[reject_idx:reject_idx + 600]:
        failures.append("messageProps_stubs.C.src msgReject does not call tt_message_reject")

    str_chooser = (ttsnoop_dir / "stringChooser_stubs.C.src").read_text(errors="ignore")
    putenv_idx = str_chooser.find("putenv( ")
    if putenv_idx == -1 or "strdup" not in str_chooser[max(0, putenv_idx - 200):putenv_idx + 200]:
        failures.append("stringChooser_stubs.C.src passes XtFree-managed buffer directly to putenv")

    return failures


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    test_src = pathlib.Path(__file__).resolve().with_name("check-ttsnoop.C")

    if args.scratch is not None:
        scratch_root = args.scratch.resolve()
        scratch_root.mkdir(parents=True, exist_ok=True)
    else:
        default_tmp = pathlib.Path("/Volumes/S8T/tmp")
        base_tmp = str(default_tmp) if default_tmp.is_dir() and os.access(default_tmp, os.W_OK) else None
        scratch_root = pathlib.Path(tempfile.mkdtemp(prefix="ttsnoop-test.", dir=base_tmp))

    try:
        if args.source_revision:
            extract_tree(repo_root, args.source_revision, scratch_root)
            ttsnoop_dir = scratch_root / "cde/programs/ttsnoop"
        else:
            ttsnoop_dir = repo_root / "cde/programs/ttsnoop"

        # Create stub *_ui.h and X11/Xm/Dt headers required by DtTt.h and DtTt.C
        inc_dir = scratch_root / "include"
        inc_dir.mkdir(parents=True, exist_ok=True)
        (inc_dir / "X11").mkdir(exist_ok=True)
        (inc_dir / "Xm").mkdir(exist_ok=True)
        (inc_dir / "Dt").mkdir(exist_ok=True)
        (inc_dir / "X11/Intrinsic.h").write_text(
            "#ifndef _XtIntrinsic_h\n"
            "#define _XtIntrinsic_h\n"
            "typedef struct _WidgetRec *Widget;\n"
            "typedef struct _XtAppContextStruct *XtAppContext;\n"
            "typedef void *XtPointer;\n"
            "typedef unsigned char Boolean;\n"
            "typedef char *String;\n"
            "typedef unsigned long XtInputMask;\n"
            "#endif\n"
        )
        for ui_hdr in (
            "ttChooser_ui.h",
            "stringChooser_ui.h",
            "messageProps_ui.h",
            "patternProps_ui.h",
            "Xm/TextF.h",
            "Dt/SpinBox.h",
            "Dt/HelpDialog.h",
        ):
            (inc_dir / ui_hdr).write_text("/* stub */\n")
        for src_file in ("DtTt.h", "DtTt.C"):
            shutil.copy2(ttsnoop_dir / src_file, inc_dir / src_file)

        bin_path = scratch_root / "check-ttsnoop"
        compile_cmd = [
            args.cxx,
            "-std=c++17",
            "-g",
            "-O1",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            f"-I{inc_dir}",
            f"-I{ttsnoop_dir}",
            f"-I{repo_root / 'cde/include'}",
            *(["-I/usr/include/tirpc"] if pathlib.Path("/usr/include/tirpc").is_dir() else []),
            str(test_src),
            "-o",
            str(bin_path),
        ]
        comp = subprocess.run(compile_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        contract_failures = verify_source_contracts(ttsnoop_dir)
        failed_parts: list[str] = list(contract_failures)

        if comp.returncode != 0:
            failed_parts.append(f"compile({comp.stderr.strip()})")
        else:
            for part in ("procid", "labels", "shell"):
                env = os.environ.copy()
                env["ASAN_OPTIONS"] = "detect_leaks=0"
                proc = subprocess.run(
                    [str(bin_path), part],
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                if proc.returncode != 0:
                    failed_parts.append(part)

        if args.expect_fail:
            if not failed_parts:
                print("FAIL: expected failure, but all parts passed", file=sys.stderr)
                return 1
            print(f"PASS (expected failure observed in: {', '.join(failed_parts)})")
            return 0

        if failed_parts:
            print(f"FAIL: parts failed: {', '.join(failed_parts)}", file=sys.stderr)
            return 1

        print(f"PASS ({args.source_revision or 'working tree'})")
        return 0
    finally:
        if not args.keep_scratch and args.scratch is None:
            shutil.rmtree(scratch_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
