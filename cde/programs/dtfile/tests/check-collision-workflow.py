#!/usr/bin/env python3
"""Focused production collision model, codec and worker-helper checks.

--model-only compiles the real model/codec modules and runs sequentially.
--helpers-only extracts and runs the real path/observation helper functions.
--extract-only stages the selected production sources without compiling them.
Invoke only with an execution loan, e.g. taskset -c 0 python3 <driver> ...
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shlex
import signal
import subprocess
import time


MODEL_CASES = (
    "model-native-bytes", "model-request-indices", "model-atomic-span-rejection",
    "model-move-self", "model-copy-link-self", "model-buffer-source",
    "model-manual-name-and-merge", "model-path-bound",
)

CODEC_CASES = (
    "codec-request-budget", "codec-tagged-request", "codec-partial-header", "codec-request-bad-magic",
    "codec-request-bad-count", "codec-request-bad-duplicate", "codec-request-bad-nul",
    "codec-request-bad-footer", "codec-request-bad-truncated", "codec-decisions-roundtrip",
    "codec-cancel", "codec-invalid-decision-no-output", "codec-reply-bad-index",
    "codec-reply-bad-nul", "codec-reply-bad-footer", "codec-reply-bad-truncated",
    "codec-owner-mismatch", "codec-closed-peer", "codec-pending-sigpipe",
)
FOCUSED_CASES = MODEL_CASES + CODEC_CASES
HELPER_CASES = ("helper-paths", "helper-observations")


def extract_model(source, work):
    names = ("FileConflict.c", "FileConflict.h", "FileConflictWire.c", "FileConflictWire.h")
    hashes = {}
    for name in names:
        data = (source / name).read_bytes()
        (work / name).write_bytes(data)
        hashes[name] = hashlib.sha256(data).hexdigest()
    (work / "extraction-manifest.json").write_text(json.dumps({
        "status": "model sources prepared for focused execution",
        "module_source": str(source), "source_sha256": hashes,
        "model_cases": MODEL_CASES, "codec_cases": CODEC_CASES,
        "future_compile_inputs": ["-DCOLLISION_MODEL_ONLY", "collision-workflow.c", "FileConflict.c"],
        "scope": "real model/codec APIs only; not worker/UI qualification"
    }, indent=2) + "\n")


def extract_helpers(source, work):
    lexical = runpy.run_path(str(Path(__file__).with_name("check-transfer-completion.py")))
    text = (source / "FileOp.c").read_text()
    records = []
    bodies = []
    for name in ("FileOpPathJoin", "FileOpParentPath", "FileOpDecisionPath",
                 "FileOpUniquePath", "FileOpObservationMatches"):
        body, line = lexical["function"](text, name)
        bodies.append(f'#line {line} {json.dumps(str(source / "FileOp.c"))}\n{body}\n')
        records.append({"function": name, "line": line,
                        "sha256": hashlib.sha256(body.encode()).hexdigest()})
    (work / "collision-helper-production.inc").write_text("\n".join(bodies))
    (work / "extraction-manifest.json").write_text(json.dumps({
        "status": "production helper sources extracted for focused execution",
        "module_source": str(source), "functions": records,
        "source_sha256": {"FileOp.c": hashlib.sha256(text.encode()).hexdigest()},
        "cases": HELPER_CASES,
        "scope": "actual path and lstat-observation helpers; not UI or operation execution"
    }, indent=2) + "\n")

def utc():
    return datetime.now(timezone.utc).isoformat()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command_record(command, work, env, log_name, timeout):
    record = {"command": command, "cwd": str(work), "start_utc": utc(),
              "timeout_seconds": timeout, "log": str(work / log_name)}
    started = time.monotonic()
    with (work / log_name).open("wb") as log:
        try:
            process = subprocess.Popen(command, cwd=work, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            record["pid"] = process.pid
            try:
                process.wait(timeout=timeout)
                record["exit_code"] = process.returncode
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                record.update(exit_code=None, timed_out=True)
        except OSError as error:
            record.update(exit_code=None, error=str(error))
    record.update(end_utc=utc(), seconds=time.monotonic() - started,
                  log_sha256=digest(work / log_name))
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--work-dir", type=Path, required=True, help="new dedicated output directory")
    parser.add_argument("--extract-only", action="store_true")
    parser.add_argument("--model-only", action="store_true")
    parser.add_argument("--helpers-only", action="store_true")
    parser.add_argument("--case", action="append", choices=FOCUSED_CASES + HELPER_CASES, dest="cases")
    parser.add_argument("--sanitizers", default="address,undefined", help="compiler list, or none")
    args = parser.parse_args()
    if args.model_only == args.helpers_only:
        parser.error("choose exactly one of --model-only or --helpers-only")
    allowed_cases = HELPER_CASES if args.helpers_only else FOCUSED_CASES
    if args.cases and any(c not in allowed_cases for c in args.cases):
        parser.error("--case must belong to the selected focused mode")
    work = args.work_dir.resolve()
    if os.uname().sysname == "Darwin" and not any(
            root.is_mount() and work.is_relative_to(root / "tmp")
            for root in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE"))):
        parser.error("Mac work must use tmp on a mounted approved volume")
    if work.exists():
        parser.error("work directory must be new; retain old evidence")
    work.mkdir(parents=True, mode=0o700)
    (work / "tmp").mkdir(mode=0o700)
    env = dict(os.environ, TMPDIR=str(work / "tmp"), LANG="C", LC_ALL="C")
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    os.environ["TMPDIR"] = env["TMPDIR"]
    source = args.module_source.resolve()
    fixture = Path(__file__).with_name("collision-workflow.c")
    source_names = (("FileOp.c",) if args.helpers_only else
                    ("FileConflict.c", "FileConflict.h", "FileConflictWire.c", "FileConflictWire.h"))
    inputs = [fixture, Path(__file__)] + [source / n for n in source_names]
    if not args.model_only:
        inputs.append(Path(__file__).with_name("check-transfer-completion.py"))
    report = {"start_utc": utc(), "module_source": str(source), "work_dir": str(work),
              "mode": "worker helpers" if args.helpers_only else
                      "model/codec",
              "extract_only": args.extract_only, "passed": False,
              "compiler_jobs": 1, "execution": "strictly sequential",
              "sanitizers": args.sanitizers, "cases": [],
              "environment": {key: env[key] for key in
                              ("TMPDIR", "LANG", "LC_ALL", "ASAN_OPTIONS", "UBSAN_OPTIONS")},
              "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
              "duration_baseline": "No measured baseline for this focused model driver yet.",
              "scope": ("actual path/lstat helpers only; no UI or operation execution" if args.helpers_only else
                        "model/pipe-codec APIs only; no whole worker, UI or baseline qualification")}
    started = time.monotonic()
    exit_code = 1
    try:
        report["input_sha256"] = {str(p): digest(p) for p in inputs}
        (work / "collision-workflow.c").write_bytes(fixture.read_bytes())
        stage = extract_helpers if args.helpers_only else extract_model
        stage(source, work)
        if args.extract_only:
            report.update(status="extracted only; not compiled/executed", passed=None)
            exit_code = 0
        else:
            compiler = shlex.split(env.get("CC", "cc"))
            if not compiler:
                raise ValueError("CC has no compiler command")
            flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                     "-Werror", "-g", "-O1", "-fno-omit-frame-pointer",
                     "-DCOLLISION_WORKER_HELPERS" if args.helpers_only else "-DCOLLISION_MODEL_ONLY"]
            if args.sanitizers != "none":
                flags += ["-fsanitize=" + args.sanitizers]
            command = compiler + flags + shlex.split(env.get("CPPFLAGS", "")) + shlex.split(env.get("CFLAGS", ""))
            binary = work / ("collision-helpers" if args.helpers_only else "collision-model")
            command += ["-I", str(work), str(work / "collision-workflow.c")]
            if args.model_only:
                command += [str(work / "FileConflict.c"), str(work / "FileConflictWire.c")]
            command += ["-pthread", "-o", str(binary)]
            command += shlex.split(env.get("LDFLAGS", ""))
            report["build"] = command_record(command, work, env, "build.log", 60)
            report["status"] = "compile failed"
            if report["build"].get("exit_code") == 0:
                report["output"] = {"path": str(binary), "sha256": digest(binary),
                                    "mtime_ns": binary.stat().st_mtime_ns}
                for name in args.cases or allowed_cases:
                    invocation = [str(binary), name]
                    run = command_record(invocation, work, env, name + ".log", 10)
                    run.update(case=name, passed=run.get("exit_code") == 0)
                    report["cases"].append(run)
                report["passed"] = all(c["passed"] for c in report["cases"])
                report["status"] = "complete"
                exit_code = 0 if report["passed"] else 1
        report["input_sha256_after"] = {str(p): digest(p) for p in inputs}
        report["source_unchanged"] = report["input_sha256"] == report["input_sha256_after"]
        if not report["source_unchanged"]:
            report.update(passed=False, status="inputs changed during qualification")
            exit_code = 1
    except (OSError, ValueError, KeyError) as error:
        report.update(error=str(error), passed=False, status="setup/extraction failed; not behavioral baseline evidence")
    finally:
        report.update(end_utc=utc(), seconds=time.monotonic() - started)
        (work / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"result": str(work / "result.json"), "passed": report["passed"],
                      "status": report["status"]}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
