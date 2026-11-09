#!/usr/bin/env python3
"""Exercise complete production Trash restore functions with real files/pipes.

Compilation and execution are deliberately explicit; importing this module does
not run checks. UI/resolver adapters do not manufacture transfer results.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import runpy
import re
import shlex
import subprocess
import sys
import tempfile
import time

CASES = (
    "resolve-error", "resolve-null", "restore-parents", "restore-relative", "restore-host", "known-dnd",
    "known-dnd-trailing", "known-dnd-relative", "known-dnd-host", "known-dnd-occupied",
    "orphan-absolute", "orphan-relative", "orphan-occupied",
    "invalid-leaf", "long-destination", "object-oom", "parent-oom",
    "parent-file", "parent-root", "checked-restore", "dangling-conflict",
    "write-short", "write-eintr", "write-zero", "write-error", "write-closed",
    "callback-short-tag", "callback-unknown-tag", "callback-short-results",
    "callback-invalid-result", "callback-read-eintr", "callback-read-error",
    "callback-mixed", "callback-known-skip", "callback-orphan-skip",
    "callback-error-then-done",
    "replace-exact", "replace-missing-source", "replace-erase-error", "replace-ignored-erase",
    "replace-hardlink", "replace-ancestor", "replace-descendant", "replace-target-gone",
    "conflict-duplicate", "conflict-missing", "conflict-extra", "conflict-index",
    "conflict-oversize", "conflict-short", "conflict-nul", "conflict-relative",
    "dialog-cancel", "dialog-wm-close", "dialog-confirm", "dialog-admission-failure",
    "dialog-pipe-failure", "dialog-fork-failure", "dialog-stage-oom",
    "dialog-source-gone", "dialog-erase-error", "dialog-retry",
    "admission-count-zero", "admission-count-negative", "admission-count-overflow",
    "append-count-negative", "append-count-overflow", "append-allocation-failure", "append-empty-success",
    "message-missing-directory", "message-empty", "message-append-failure",
    "message-count-error", "message-file-error",
    "batch-message-files", "batch-message-failure", "batch-message-invalid",
)
BASELINE_CASES = ("baseline-resolve-array", "baseline-known-dnd", "baseline-short-done", "baseline-missing-parent")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def utc():
    return datetime.now(timezone.utc).isoformat()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--case", action="append", choices=CASES + BASELINE_CASES)
    parser.add_argument("--baseline-control", action="store_true",
                        help="execute only old worker/callback/object ABI with explicit narrow coverage")
    parser.add_argument("--sanitizers", default="address,undefined")
    args = parser.parse_args(argv)
    source = args.module_source.resolve()
    if args.work_dir is None:
        root = Path(os.environ["TMPDIR"]).resolve()
        if os.uname().sysname == "Darwin" and not any(
                volume.is_mount() and root.is_relative_to(volume / "tmp")
                for volume in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE"))):
            parser.error("TMPDIR must be below tmp on a mounted approved Mac volume")
        with tempfile.TemporaryDirectory(prefix="dtfile-restore-", dir=root) as scratch:
            return main([*(sys.argv[1:] if argv is None else argv),
                         "--work-dir", str(Path(scratch) / "run")])
    else:
        work = args.work_dir.resolve()
        if work.exists():
            parser.error("--work-dir must be new")
        if os.uname().sysname == "Darwin" and not any(
                volume.is_mount() and work.is_relative_to(volume / "tmp")
                for volume in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE"))):
            parser.error("work must be below tmp on a mounted approved Mac volume")
        work.mkdir(parents=True, mode=0o700)
    (work / "tmp").mkdir(mode=0o700)
    env = dict(os.environ, TMPDIR=str(work / "tmp"), LANG="C", LC_ALL="C")
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    tests = Path(__file__).resolve().parent
    support = tests / "check-transfer-completion.py"
    base = tests / "transfer-completion.c"
    harness = tests / "restore-workflow.c"
    # Reuse the established verbatim C-definition extractor and actual
    # transfer fixture prelude. Neither support module executes its main.
    extraction = runpy.run_path(str(support), run_name="restore_extraction")
    record = {"started_utc": utc(), "jobs": 1, "module_source": str(source),
              "work_dir": str(work), "TMPDIR": env["TMPDIR"], "cases": [], "passed": False,
              "sanitizers": args.sanitizers,
              "ASAN_OPTIONS": env["ASAN_OPTIONS"], "UBSAN_OPTIONS": env["UBSAN_OPTIONS"],
              "coverage_limits": "Real production restore, callback, parent creation, ledger and "
                  "regular-file FileManip and fsErase functions. UI widgets/cache/notifications and local resolver/permission "
                  "adapters only; ToolTalk fixtures use an ASCII tokenizer and canonical-path "
                  "DtEliminateDots adapter. Reported syscall faults are not native-device faults. No GUI, "
                  "directory move, overwrite-dialog execution or ownership/privacy qualification.",
              "test_inputs": {p.name: sha(p.read_bytes()) for p in (Path(__file__), support, base, harness)}}
    def save():
        (work / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    try:
        record["transfer_extraction"] = extraction["extract"](source, work, include_workers=False)
        raw = (source / "Trash.c").read_bytes()
        text = raw.decode("utf-8")
        record["trash_sha256"] = sha(raw)
        record["metadata_sources"] = {name: sha((source / name).read_bytes())
                                      for name in ("TrashMetadata.c", "TrashMetadata.h", "dtcopy/fsrtns.c", "dtcopy/fsrtns.h")}
        names = ("RestoreWriteAll", "RestoreWriteResults", "RestoreCountValid", "RestoreFreeConflict",
                 "RestoreReleaseInputs", "RestoreReleaseRequest", "RestoreWriteConflict", "RestoreLeaf", "RestoreDestination",
                 "RestoreProcess", "RestoreReadResults", "RestorePipeCB", "RestoreObject",
                 "RestoreReadConflict", "RestoreUpdateDestination", "RestoreDirectoryContains",
                 "EnsureParentDirs", "WriteEntry", "WriteTrashEntries", "RestoreFromTrash",
                 "RestoreConflictDestroyed", "CreateRestoreDialog", "RestoreVerifyOk", "RestoreVerifyCancel",
                 "TrashAppendMessageFile", "MessageToFileList", "MessagesToFileList")
        if args.baseline_control:
            names = ("RestoreProcess", "RestorePipeCB", "RestoreObject", "EnsureParentDirs", "WriteEntry", "WriteTrashEntries")
            record["baseline_scope"] = "Unchanged old production worker/callback/object/parent and ledger bodies. " \
                "The harness calls their old ABI directly; no substitute new helper or fake operation result. " \
                "Only the four focused baseline cases execute; typed protocol/dialog cases are candidate-only."
        bodies, manifest = [], []
        for name in names:
            body, line = extraction["function"](text, name)
            bodies.append(f"#line {line} {json.dumps(str(source / 'Trash.c'))}\n{body}\n")
            manifest.append({"function": name, "line": line, "sha256": sha(body.encode())})
        definitions = [extraction["structure"](text, name) for name in ("TrashEntry", "RestoreFromTrashCBData")]
        definitions += [extraction["constant"](text, "SKIP_FILE")]
        if not args.baseline_control:
            enum = re.search(r"typedef\s+enum\s*\{[^}]*\}\s*RestoreReplacePolicy\s*;", text)
            if enum is None:
                raise ValueError("typed replacement policy missing")
            definitions = [enum.group(), *(extraction["structure"](text, name) for name in
                            ("RestoreTargetIntent", "RestoreConflict")), *definitions]
            definitions += [extraction["constant"](text, name) for name in
                            ("RESTORE_PATH_LIMIT", "PIPEMSG_RESTORE_CONFLICT")]
        (work / "restore-definitions.inc").write_text("\n\n".join(definitions) + "\n")
        (work / "restore-production.inc").write_text("\n".join(bodies))
        record["restore_functions"] = manifest
        for original, name in ((base, "transfer-base.c"), (harness, harness.name)):
            (work / name).write_bytes(original.read_bytes())
        binary = work / "restore-workflow"
        command = [*shlex.split(env.get("CC", "cc")), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                   "-D_DEFAULT_SOURCE", "-D_XOPEN_SOURCE=700", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                   "-Wno-unused-but-set-variable",
                   "-Wno-parentheses", "-Wno-dangling-else", "-I", str(work), "-I", str(source),
                   str(work / harness.name), "-o", str(binary)]
        if args.baseline_control:
            command += ["-DRESTORE_BASELINE=1"]
        if args.sanitizers != "none":
            command += ["-fsanitize=" + args.sanitizers, "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]
        record.update(compile_command=command, build_start_utc=utc(),
                      duration_baseline="First dedicated restore qualification; no duration baseline assumed.")
        start = time.monotonic()
        with (work / "build.log").open("w") as log:
            compiled = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=60)
        record.update(build_end_utc=utc(), build_seconds=time.monotonic() - start, build_exit_code=compiled.returncode)
        save()
        if compiled.returncode:
            print((work / "build.log").read_text(), end="")
            return 1
        record.update(executable_sha256=sha(binary.read_bytes()), executable_path=str(binary),
                      executable_mtime_ns=binary.stat().st_mtime_ns)
        selected = args.case or (BASELINE_CASES if args.baseline_control else CASES)
        allowed = BASELINE_CASES if args.baseline_control else CASES
        if any(name not in allowed for name in selected):
            raise ValueError("selected cases do not match candidate/baseline ABI")
        for name in selected:
            directory = work / name
            directory.mkdir(mode=0o700)
            result = {"name": name, "started_utc": utc()}
            start = time.monotonic()
            try:
                with (directory / "run.log").open("w") as log:
                    ran = subprocess.run([str(binary), name, str(directory)], cwd=directory, env=env,
                                         stdout=log, stderr=subprocess.STDOUT, timeout=10)
                result["exit_code"] = ran.returncode
            except subprocess.TimeoutExpired:
                result.update(exit_code=124, timeout=True)
            result.update(ended_utc=utc(), seconds=time.monotonic() - start, passed=result["exit_code"] == 0,
                          log_sha256=sha((directory / "run.log").read_bytes()))
            record["cases"].append(result)
            save()
            print(f"{'PASS' if result['passed'] else 'FAIL'} {name}", flush=True)
        record["source_unchanged"] = sha((source / "Trash.c").read_bytes()) == record["trash_sha256"] and all(
            sha((source / name).read_bytes()) == digest for name, digest in record["metadata_sources"].items()) and all(
            sha((source / name).read_bytes()) == digest
            for name, digest in record["transfer_extraction"]["source_sha256"].items())
        record["passed"] = record["source_unchanged"] and all(case["passed"] for case in record["cases"])
        record["ended_utc"] = utc()
        save()
        print(f"Receipt: {work / 'result.json'}")
        return 0 if record["passed"] else 1
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        record.update(error=str(error), ended_utc=utc())
        save()
        print(f"FAIL: {error}; receipt: {work / 'result.json'}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
