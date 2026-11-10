#!/usr/bin/env python3
"""Qualify actual FileManip/FileOp transfer completion with syscall seams.

No CDE/Motif build or GUI is needed. The C fixture is always this driver's
sibling; --module-source (or DTFILE_TRANSFER_MODULE_SOURCE) selects baseline
or candidate production code without copying or changing fixture logic.
Run sequentially on a reserved CPU, e.g. taskset -c 0 python3 <driver> ...
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time


CASES = (
    "copy-success", "move-success", "copy-short", "move-short",
    "copy-read-eintr", "copy-write-eintr", "move-write-eintr",
    "copy-write-zero", "move-write-zero", "move-read-error",
    "copy-write-error", "move-write-error", "copy-null-handler-write-error",
    "copy-close-target", "move-close-target", "copy-close-source",
    "move-close-source", "copy-first-error", "move-first-error", "copy-collision",
    "buffer-success", "buffer-short", "buffer-write-eintr", "buffer-write-zero",
    "buffer-write-error", "buffer-close-error", "buffer-first-error",
    "buffer-cleanup-error", "buffer-empty", "buffer-negative",
    "buffer-null-positive", "buffer-occupied", "buffer-symlink",
    "worker-normal-success", "worker-desktop-success", "worker-normal-same-copy",
    "worker-normal-first-fails", "worker-desktop-first-fails",
    "worker-normal-last-fails", "worker-desktop-last-fails",
    "worker-normal-resolve", "worker-desktop-resolve",
    "worker-normal-target-resolve", "worker-desktop-target-resolve",
    "worker-normal-collision-keep-both", "worker-normal-collision-clean-cancel",
    "pipe-read-invalid", "pipe-string-negative", "pipe-string-truncated",
    "pipe-string-valid", "pipe-string-field-empty", "pipe-string-field-truncated",
    "pipe-write-oversize", "pipe-field-truncated",
    "pipe-index-negative", "pipe-index-equal", "pipe-index-valid",
    "trash-index-negative", "trash-index-equal", "trash-index-valid",
    "trash-results-negative", "trash-results-null", "trash-results-oversize",
    "trash-results-truncated", "trash-results-valid",
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def utc():
    return datetime.now(timezone.utc).isoformat()


def balanced_end(text, start, opening, closing):
    """Return the position after a matched delimiter, ignoring C trivia."""
    if text[start] != opening:
        raise ValueError("delimiter scan did not start at its opening")
    depth, i = 0, start
    while i < len(text):
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise ValueError("unterminated C comment")
            i = end + 2
            continue
        if text.startswith("//", i):
            end = text.find("\n", i + 2)
            i = len(text) if end < 0 else end + 1
            continue
        char = text[i]
        if char in "\"'":
            quote = char
            i += 1
            while i < len(text):
                if text[i] == "\\":
                    i += 2
                elif text[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
            else:
                raise ValueError("unterminated C string/character literal")
            continue
        if char == opening:
            depth += 1
        elif char == closing:
            depth -= 1
            if not depth:
                return i + 1
        i += 1
    raise ValueError("unbalanced production C delimiters")


def skip_trivia(text, position):
    while position < len(text):
        if text[position].isspace():
            position += 1
        elif text.startswith("/*", position):
            end = text.find("*/", position + 2)
            if end < 0:
                raise ValueError("unterminated C comment")
            position = end + 2
        elif text.startswith("//", position):
            end = text.find("\n", position + 2)
            position = len(text) if end < 0 else end + 1
        else:
            break
    return position


def function(text, name):
    # Anchor at a return type, not a call or a comment containing the name.
    pattern = re.compile(
        r"(?m)^(?:static[ \t\r\n]+)?"
        r"(?:Boolean|DtfileConflictOperation|int|void|String|char[ \t]*\*)[ \t\r\n]+"
        + re.escape(name) + r"[ \t\r\n]*\("
    )
    definitions = []
    for match in pattern.finditer(text):
        after_args = balanced_end(text, match.end() - 1, "(", ")")
        body = skip_trivia(text, after_args)
        if body < len(text) and text[body] == "{":
            end = balanced_end(text, body, "{", "}")
            definitions.append((text[match.start():end], text.count("\n", 0, match.start()) + 1))
    if len(definitions) != 1:
        raise ValueError(f"expected one definition of {name}, found {len(definitions)}")
    return definitions[0]


def structure(text, name):
    for match in re.finditer(r"\btypedef\s+struct(?:\s+\w+)?\s*\{", text):
        end = balanced_end(text, match.end() - 1, "{", "}")
        suffix = re.match(r"\s*" + re.escape(name) + r"\s*;", text[end:])
        if suffix:
            return text[match.start():end + suffix.end()]
    raise ValueError(f"production typedef {name} not found")


def constant(text, name):
    matches = re.findall(r"(?m)^#\s*define[ \t]+" + re.escape(name) + r"[ \t]+[^\r\n]+", text)
    if len(matches) != 1:
        raise ValueError(f"expected one definition of {name}, found {len(matches)}")
    return matches[0]


def extract(source, work, include_workers=True):
    """Copy complete production definitions verbatim, preserving source lines."""
    filenames = ("FileManip.c", "FileOp.c", "FileProcess.c", "FileProcess.h",
                 "FileOpProtocol.h",
                 "FileConflict.c", "FileConflict.h",
                 "FileConflictWire.c", "FileConflictWire.h",
                 "Main.h", "FileMgr.h", "Common.h", "SharedMsgs.h", "Trash.c",
                 "ModAttrP.c")
    raw = {name: (source / name).read_bytes() for name in filenames}
    text = {name: data.decode("utf-8") for name, data in raw.items()}
    definitions = [
        constant(text["Main.h"], name)
        for name in ("MAX_PATH", "BLOCK_SIZE", "MOVE_FILE", "COPY_FILE",
                     "LINK_FILE", "REMOVE_FILE", "TRASH_FILE", "RENAME_FILE",
                     "MERGE_DIR", "MAKE_BUFFER")
    ]
    definitions += [
        constant(text["FileMgr.h"], name)
        for name in ("NOT_DESKTOP", "DESKTOP", "NOT_DESKTOP_DIR", "TRASH_DIRECTORY")
    ]
    definitions += re.findall(r"(?m)^#define[ \t]+PIPEMSG_\w+[ \t]+[^\r\n]+", text["FileOp.c"])
    definitions += [structure(text["Common.h"], "BufferInfo"),
                    structure(text["FileOp.c"], "DirUpdate")]
    (work / "transfer-definitions.inc").write_text("\n\n".join(definitions) + "\n")

    functions = [("FileManip.c", name) for name in
                 ("FilePathCopy", "FilePathJoin", "FileParentPath",
                  "FilePathsFree", "FileReadLink", "FileManip")] + [
        ("FileOp.c", name) for name in (
            "PipeRead", "PipeReadExact", "PipeWriteString", "PipeReadStringField",
            "PipeReadString", "PipeWriteErrmsg", "FileOpReadIndex",
            "FileOpError", "CreateFileFromBuffer", "SendModifyMsg",
            "appendErrorMessage", "DisplayErrorMessage")
    ]
    functions += [("Trash.c", "MoveToTrashResultIndex"),
                  ("Trash.c", "TrashReadResults")]
    if include_workers:
        functions += [("FileOp.c", name) for name in
                      ("FileOpPathJoin", "FileOpParentPath", "FileOpDecisionPath",
                       "FileOpUniquePath", "FileOpObservationMatches",
                       "FileOpConflictOperation", "FileOpHandleConflicts",
                       "FileMoveCopyProcess", "FileMoveCopyProcessDesktop")]
    output, manifest = [], []
    for filename, name in functions:
        body, line = function(text[filename], name)
        output.append(f"#line {line} {json.dumps(str(source / filename))}\n")
        output.append(body + "\n")
        manifest.append({"file": filename, "function": name, "line": line,
                         "verbatim_sha256": sha(body.encode()), "bytes": len(body.encode())})
    (work / "transfer-production.inc").write_text("\n".join(output))
    result = {"module_source": str(source),
              "source_sha256": {name: sha(data) for name, data in raw.items()},
              "functions": manifest,
              "generated_sha256": {name: sha((work / name).read_bytes()) for name in
                                   ("transfer-definitions.inc", "transfer-production.inc")}}
    callback_bodies = {name: function(text["FileOp.c"], name)[0] for name in
                       ("FileOpPipeCB", "ChangeIconPipeCB", "MakeFilePipeCB")}
    protocol_contract = {
        "fileop_index_reads": callback_bodies["FileOpPipeCB"].count("FileOpReadIndex("),
        "fileop_checked_fields": callback_bodies["FileOpPipeCB"].count("PipeReadExact("),
        "rename_checked_fields": callback_bodies["ChangeIconPipeCB"].count("PipeReadExact("),
        "create_checked_fields": callback_bodies["MakeFilePipeCB"].count("PipeReadExact("),
        "callbacks_check_tag_size": sum("n != sizeof(pipe_msg)" in body
                                        for body in callback_bodies.values()),
        "optional_error_arguments": sum(
            "PipeReadStringField(*fd, &err_arg)" in body and
            "err_arg == NULL" not in body
            for body in (callback_bodies["FileOpPipeCB"],
                         callback_bodies["ChangeIconPipeCB"])),
    }
    if protocol_contract != {"fileop_index_reads": 2, "fileop_checked_fields": 4,
                             "rename_checked_fields": 3, "create_checked_fields": 2,
                             "callbacks_check_tag_size": 3,
                             "optional_error_arguments": 2}:
        raise ValueError(f"callback protocol contract mismatch: {protocol_contract}")
    result["protocol_contract"] = protocol_contract
    consumer_bodies = {
        name: function(text[filename], name)[0]
        for filename, name in (
            ("Trash.c", "TrashRecSizePipeCB"),
            ("Trash.c", "EmptyTrashPipeCB"),
            ("ModAttrP.c", "AttrChangePipeCB"))
    }
    move_start = text["Trash.c"].index("MoveToTrashPipeCB(")
    move_end = text["Trash.c"].index("\nstatic void\nMoveToTrash(", move_start)
    consumer_bodies["MoveToTrashPipeCB"] = text["Trash.c"][move_start:move_end]
    move_request_end = text["Trash.c"].index("\n/*====================================================================", move_end)
    move_request_body = text["Trash.c"][move_end:move_request_end]
    empty_request_start = text["Trash.c"].index("\nstatic void\nEmptyTrash(")
    empty_request_end = text["Trash.c"].index("\n/* Never truncate", empty_request_start)
    empty_request_body = text["Trash.c"][empty_request_start:empty_request_end]
    producer_bodies = [
        function(text[filename], name)[0]
        for filename, name in (
            ("Trash.c", "TrashMaybeStartRecSizeScan"),
            ("Trash.c", "MoveToTrashProcess"),
            ("Trash.c", "EmptyTrashProcess"),
            ("ModAttrP.c", "AttrChangeProcess"))
    ]
    consumer_contract = {
        "callbacks_with_protocol_feedback": sum(
            "FileOpProtocolError(" in body for body in consumer_bodies.values()),
        "recsize_exact_fields": consumer_bodies["TrashRecSizePipeCB"].count("PipeReadExact("),
        "move_exact_fields": consumer_bodies["MoveToTrashPipeCB"].count("PipeReadExact("),
        "move_checked_indices": consumer_bodies["MoveToTrashPipeCB"].count("MoveToTrashResultIndex("),
        "empty_exact_fields": consumer_bodies["EmptyTrashPipeCB"].count("PipeReadExact("),
        "empty_checked_results": consumer_bodies["EmptyTrashPipeCB"].count("TrashReadResults("),
        "empty_suppresses_duplicate_feedback":
            consumer_bodies["EmptyTrashPipeCB"].count("!protocol_error") >= 2,
        "attribute_exact_fields": consumer_bodies["AttrChangePipeCB"].count("PipeReadExact("),
        "raw_producer_writes": sum(len(re.findall(r"(?<!\w)write\s*\(", body))
                                   for body in producer_bodies),
        "callbacks_check_tag_size": sum("sizeof(pipe_msg)" in body
                                         for body in consumer_bodies.values()),
        "move_request_guard":
            "!RestoreCountValid(file_count) || file_list == NULL" in move_request_body,
        "empty_request_guard":
            "!RestoreCountValid(del_count) || del_list == NULL" in empty_request_body,
    }
    expected_consumer_contract = {
        "callbacks_with_protocol_feedback": 4,
        "recsize_exact_fields": 1,
        "move_exact_fields": 2,
        "move_checked_indices": 3,
        "empty_exact_fields": 1,
        "empty_checked_results": 1,
        "empty_suppresses_duplicate_feedback": True,
        "attribute_exact_fields": 2,
        "raw_producer_writes": 0,
        "callbacks_check_tag_size": 4,
        "move_request_guard": True,
        "empty_request_guard": True,
    }
    if consumer_contract != expected_consumer_contract:
        raise ValueError(f"worker consumer contract mismatch: {consumer_contract}")
    result["consumer_contract"] = consumer_contract
    (work / "extraction-manifest.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def inventory(directory):
    result = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            result[str(path.relative_to(directory))] = {"symlink": os.readlink(path)}
        elif path.is_file():
            result[str(path.relative_to(directory))] = {
                "bytes": path.stat().st_size, "sha256": sha(path.read_bytes())}
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-source", type=Path,
                        default=Path(os.environ.get("DTFILE_TRANSFER_MODULE_SOURCE",
                                                    Path(__file__).resolve().parents[1])))
    parser.add_argument("--work-dir", type=Path, help="new explicit task-owned directory; never /tmp on Mac")
    parser.add_argument("--case", action="append", choices=CASES, dest="cases")
    parser.add_argument("--list-cases", action="store_true")
    parser.add_argument("--sanitizers", default="address,undefined",
                        help="compiler sanitizer list, or 'none'; recorded in result.json")
    args = parser.parse_args(argv)
    if args.list_cases:
        print("\n".join(CASES))
        return 0
    if args.work_dir is None:
        temporary_root = Path(os.environ["TMPDIR"]).resolve()
        if os.uname().sysname == "Darwin" and not any(
                root.is_mount() and temporary_root.is_relative_to(root / "tmp")
                for root in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE"))):
            parser.error("TMPDIR must use a mounted approved Mac temporary volume")
        with tempfile.TemporaryDirectory(prefix="dtfile-transfer-", dir=temporary_root) as scratch:
            return main([*(sys.argv[1:] if argv is None else argv),
                         "--work-dir", str(Path(scratch) / "run")])
    work = args.work_dir.resolve()
    if work.exists():
        parser.error("--work-dir must be new; prior evidence/builds are never overwritten")
    if os.uname().sysname == "Darwin" and not any(
            root.is_mount() and work.is_relative_to(root / "tmp")
            for root in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE"))):
        parser.error("Mac work must be beneath tmp on a mounted approved volume")
    source = args.module_source.resolve()
    work.mkdir(parents=True, mode=0o700)
    (work / "tmp").mkdir(mode=0o700)
    env = dict(os.environ, TMPDIR=str(work / "tmp"), LANG="C", LC_ALL="C")
    leak_check = "0" if os.uname().sysname == "Darwin" else "1"
    env.setdefault("ASAN_OPTIONS", f"detect_leaks={leak_check}:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    harness = Path(__file__).resolve().with_name("transfer-completion.c")
    frozen_harness = work / harness.name
    frozen_harness.write_bytes(harness.read_bytes())
    binary = work / "transfer-completion"
    record = {"started_utc": utc(), "jobs": 1, "module_source": str(source),
              "harness_source": str(harness), "harness_sha256": sha(frozen_harness.read_bytes()),
              "driver_sha256": sha(Path(__file__).read_bytes()), "work_dir": str(work),
              "sanitizers": args.sanitizers, "TMPDIR": env["TMPDIR"],
              "ASAN_OPTIONS": env["ASAN_OPTIONS"], "UBSAN_OPTIONS": env["UBSAN_OPTIONS"],
              "duration_baseline": "First dedicated qualification; no measured baseline assumed.",
              "coverage_limits": "Regular-file transfers and actual buffer/worker functions. "
              "Trash result bounds and shared decoders execute as extracted production code; "
              "the four Xt callback integrations are source-contract checks. "
              "No GUI, directory transfers, remote resolver or native-device-fault claim. "
              "UI/notification adapters and reported syscall faults are test-only.",
              "cases": [], "passed": False}
    result_file = work / "result.json"

    def save():
        result_file.write_text(json.dumps(record, indent=2) + "\n")

    try:
        workers = any(name.startswith("worker-") for name in (args.cases or CASES))
        record["extraction"] = extract(source, work, include_workers=workers)
        command = [*shlex.split(env.get("CC", "cc")), "-std=c11",
                   "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", "-D_XOPEN_SOURCE=700",
                   "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                   # Unchanged legacy whole-function declarations/branches:
                   "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                   "-Wno-parentheses", "-Wno-dangling-else",
                   "-I", str(work), "-I", str(source), str(frozen_harness)]
        if workers:
            command += ["-DTRANSFER_INCLUDE_WORKERS=1", str(source / "FileConflict.c"),
                        str(source / "FileConflictWire.c")]
        command += [str(source / "FileProcess.c")]
        command += ["-pthread", "-o", str(binary)]
        if args.sanitizers != "none":
            command += ["-fsanitize=" + args.sanitizers,
                        "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]
        record.update(build_start_utc=utc(), compile_command=command)
        started = time.monotonic()
        try:
            with (work / "build.log").open("w") as log:
                built = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       timeout=60, check=False)
            record["build_exit_code"] = built.returncode
        finally:
            record.update(build_end_utc=utc(), build_seconds=time.monotonic() - started)
            save()
        if built.returncode:
            print((work / "build.log").read_text(), end="")
            return 1
        record.update(executable_path=str(binary), executable_sha256=sha(binary.read_bytes()),
                      executable_mtime_ns=binary.stat().st_mtime_ns)
        for name in args.cases or CASES:
            directory = work / name
            directory.mkdir(mode=0o700)
            case = {"name": name, "started_utc": utc(), "command": [str(binary), name, str(directory)]}
            started = time.monotonic()
            try:
                with (directory / "run.log").open("w") as log:
                    ran = subprocess.run(case["command"], env=env, cwd=directory,
                                         stdout=log, stderr=subprocess.STDOUT, timeout=10, check=False)
                case["exit_code"] = ran.returncode
            except subprocess.TimeoutExpired as error:
                case.update(exit_code=124, timeout=str(error))
            case.update(ended_utc=utc(), seconds=time.monotonic() - started,
                        passed=case["exit_code"] == 0, output_files=inventory(directory))
            record["cases"].append(case)
            save()
            print(f"{'PASS' if case['passed'] else 'FAIL'} {name} ({case['seconds']:.3f}s)", flush=True)
        record["source_unchanged"] = all(
            sha((source / name).read_bytes()) == digest
            for name, digest in record["extraction"]["source_sha256"].items())
        record["passed"] = record["source_unchanged"] and all(case["passed"] for case in record["cases"])
        record["ended_utc"] = utc()
        save()
        print(f"Receipt: {result_file}", flush=True)
        return 0 if record["passed"] else 1
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        record.update(error=str(error), ended_utc=utc(), passed=False)
        save()
        print(f"FAIL: {error}; receipt: {result_file}", flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
