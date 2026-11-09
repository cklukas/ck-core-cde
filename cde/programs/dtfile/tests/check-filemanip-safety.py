#!/usr/bin/env python3
"""Focused actual FileManip helper/path safety checks. No CDE/Motif build.

Require a fresh coordinated slot for compilation/execution. Use --helper-only
--paths-only and --directories-only in separate new work directories, sequentially. --extract-only
stages complete production functions without compiling or executing tests.
The sibling transfer driver supplies only its lexical C extractor utilities.
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


HELPER_CASES = (
    "helper-exit-zero", "helper-exit-one", "helper-missing-exec",
    "helper-sigterm", "helper-real-eintr", "helper-sigchld-ignored",
    "helper-sigchld-nocldwait", "helper-mask-restore", "helper-unrelated-child",
    "helper-invalid-pid-zero", "helper-invalid-pid-negative",
)


PATH_CASES = (
    "path-join-dynamic", "path-parent-native", "path-occupied-file",
    "path-occupied-directory", "path-occupied-symlink", "path-symlink-text",
    "path-container-deep", "path-move-followlink-null",
    "path-move-rollback-null-handler", "path-move-fallback-command",
    "ancestry-deep", "ancestry-root",
    "ancestry-dev-identity", "ancestry-root-non2", "ancestry-fstat-error",
    "ancestry-openat-error", "ancestry-close-error",
)


DIRECTORY_CASES = (
    "directory-copy-success", "directory-copy-failure", "directory-copy-missing-exec",
    "directory-copy-signal", "directory-move-cross-success", "directory-move-cross-failure",
    "directory-copy-dynamic-target", "directory-move-dynamic-target",
)

def extract_directories(source, work):
    extract_paths(source, work)
    lexical = runpy.run_path(str(Path(__file__).with_name("check-transfer-completion.py")))
    path = source / "FileManip.c"
    text = path.read_text()
    bodies = [lexical["structure"](text, "FileHelperSignals")]
    functions = []
    for name in ("FileHelperBegin", "FileHelperRestore", "FileHelperChildSignals", "FileHelperWait",
                 "FilePathCopy", "FilePathJoin", "FileParentPath", "FilePathsFree", "FileReadLink",
                 "FileDirectoryHasAncestor", "Check", "MoveDir", "CopyDir", "FileManip"):
        body, line = lexical["function"](text, name)
        if name == "CopyDir":
            # Existing production signature retains unused view/type inputs.
            # Suppress only those parameters around this verbatim body.
            bodies += ['#pragma GCC diagnostic push', '#pragma GCC diagnostic ignored "-Wunused-parameter"']
        bodies.append(f'#line {line} {json.dumps(str(path))}\n{body}\n')
        if name == "CopyDir": bodies.append('#pragma GCC diagnostic pop')
        functions.append({"function": name,"line":line,"sha256":hashlib.sha256(body.encode()).hexdigest()})
    generated = work / "filemanip-directory-production.inc"
    generated.write_text("\n".join(bodies))
    manifest = json.loads((work/"extraction-manifest.json").read_text())
    manifest.update(functions=functions,generated_sha256=digest(generated),
                    scope="whole FileManip directory dispatch, CopyDir/MoveDir and real helper lifetime; private outcome fixture, not dtfile_copy engine qualification",
                    helper="real exec of this fixture through a private dtfile_copy symlink; real pipe startup/gate/completion and wait statuses",
                    device_seam="only target-parent stat st_dev differs; real path/fd/access checks; no native cross-device claim",
                    busy_ui_adapter="private fixture has no CDE views; DirectoryBusy returns false; UI only records errors",
                    warning_scope="CopyDir legacy unused parameters only; body retained verbatim")
    (work/"extraction-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")

def extract_paths(source, work):
    lexical = runpy.run_path(str(Path(__file__).with_name("check-transfer-completion.py")))
    names = ("FileManip.c", "Main.h", "FileMgr.h", "SharedMsgs.h")
    raw = {name: (source / name).read_bytes() for name in names}
    text = {name: data.decode() for name, data in raw.items()}
    definitions = [lexical["constant"](text["Main.h"], n) for n in
                   ("MAX_PATH", "BLOCK_SIZE", "MOVE_FILE", "COPY_FILE", "LINK_FILE", "MERGE_DIR")]
    definitions += [lexical["constant"](text["FileMgr.h"], n) for n in
                    ("NOT_DESKTOP", "DESKTOP", "TRASH_DIRECTORY")]
    (work / "filemanip-path-definitions.inc").write_text("\n".join(definitions)+"\n")
    (work / "SharedMsgs.h").write_bytes(raw["SharedMsgs.h"])
    bodies, records = [], []
    for name in ("FilePathCopy", "FilePathJoin", "FileParentPath", "FilePathsFree", "FileReadLink",
                 "FileDirectoryHasAncestor", "Check", "FileManip"):
        body, line = lexical["function"](text["FileManip.c"], name)
        bodies.append(f'#line {line} {json.dumps(str(source / "FileManip.c"))}\n{body}\n')
        records.append({"function": name, "line": line,
                        "sha256": hashlib.sha256(body.encode()).hexdigest()})
    generated = work / "filemanip-path-production.inc"
    generated.write_text("\n".join(bodies))
    (work / "extraction-manifest.json").write_text(json.dumps({
        "module_source": str(source), "source_sha256": {n:hashlib.sha256(b).hexdigest() for n,b in raw.items()},
        "functions": records, "generated_sha256": digest(generated),
        "scope": "whole FileManip regular-file/symlink branches plus path/ancestry helpers; directory-transfer adapters abort",
        "seams": ["ancestry syscall errors are reported fault returns, not native I/O failures",
                  "non-2 root uses consistent inode metadata mapping with real descriptor traversal",
                  "symlink native limit fallback is explicitly reported; no >1024 claim when unsupported",
                  "resolver: _DtFollowLink may return NULL; regular-file MOVE fallback transfer remains real",
                  "rollback: link reports EXDEV and source unlink reports EACCES; transfer/target cleanup remain real",
                  "fallback dispatch: readlink reports EIO; RunFileCommand observes command and reports failure; no native helper execution claim"]
    }, indent=2)+"\n")

def extract_helpers(source, work):
    lexical = runpy.run_path(str(Path(__file__).with_name("check-transfer-completion.py")))
    path = source / "FileManip.c"
    raw = path.read_bytes()
    text = raw.decode()
    definitions = [lexical["structure"](text, "FileHelperSignals")]
    records = []
    for name in ("FileHelperBegin", "FileHelperRestore", "FileHelperChildSignals", "FileHelperWait"):
        body, line = lexical["function"](text, name)
        definitions.append(f'#line {line} {json.dumps(str(path))}\n{body}\n')
        records.append({"function": name, "line": line,
                        "sha256": hashlib.sha256(body.encode()).hexdigest()})
    generated = work / "filemanip-helper-production.inc"
    generated.write_text("\n".join(definitions))
    (work / "extraction-manifest.json").write_text(json.dumps({
        "module_source": str(source), "source_sha256": {"FileManip.c": hashlib.sha256(raw).hexdigest()},
        "functions": records, "generated_sha256": digest(generated),
        "scope": "actual helper lifetime functions with real child/status/signals; not directory/FileManip caller qualification",
        "observation_seam": "waitpid calls the real syscall, records real EINTR; timer releases real child via pipe"
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
    parser.add_argument("--work-dir", type=Path, required=True, help="new caller-owned dedicated output directory")
    parser.add_argument("--extract-only", action="store_true")
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--helper-only", action="store_true")
    modes.add_argument("--paths-only", action="store_true")
    modes.add_argument("--directories-only", action="store_true")
    parser.add_argument("--case", action="append", choices=HELPER_CASES + PATH_CASES + DIRECTORY_CASES, dest="cases")
    parser.add_argument("--sanitizers", default="address,undefined", help="compiler list, or none")
    args = parser.parse_args()
    cases = DIRECTORY_CASES if args.directories_only else HELPER_CASES if args.helper_only else PATH_CASES
    if args.cases and any(c not in cases for c in args.cases):
        parser.error("--case does not belong to the selected mode")
    work = args.work_dir.resolve()
    if os.uname().sysname == "Darwin" and not any(
            root.is_mount() and work.is_relative_to(root / "tmp")
            for root in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE"))):
        parser.error("Mac work must use tmp on a mounted approved volume")
    if work.exists():
        parser.error("work directory must be new; prior evidence is never overwritten")
    work.mkdir(parents=True, mode=0o700)
    (work / "tmp").mkdir(mode=0o700)
    env = dict(os.environ, TMPDIR=str(work / "tmp"), LANG="C", LC_ALL="C")
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    os.environ["TMPDIR"] = env["TMPDIR"]
    source = args.module_source.resolve()
    fixture = Path(__file__).with_name("filemanip-safety.c")
    names = ("FileManip.c",) if args.helper_only else ("FileManip.c", "Main.h", "FileMgr.h", "SharedMsgs.h")
    inputs = [Path(__file__), fixture, Path(__file__).with_name("check-transfer-completion.py")]
    inputs += [source / n for n in names]
    report = {"start_utc": utc(), "module_source": str(source), "work_dir": str(work),
              "mode": "directories" if args.directories_only else "helpers" if args.helper_only else "paths", "extract_only": args.extract_only,
              "passed": False, "compiler_jobs": 1, "execution": "strictly sequential",
              "sanitizers": args.sanitizers, "cases": [],
              "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None,
              "environment": {key:env[key] for key in ("TMPDIR","LANG","LC_ALL","ASAN_OPTIONS","UBSAN_OPTIONS")},
              "duration_baseline": "First run of split driver; use retained prior helper/path receipts for comparison.",
              "scope": ("whole CopyDir/MoveDir caller wait and outcome; real private fake helper, not dtfile_copy engine" if args.directories_only else
                        "helper lifecycle" if args.helper_only else "whole FileManip regular-file/symlink and path/ancestry helpers; directory-transfer adapters abort")}
    start = time.monotonic()
    status = 1
    try:
        report["input_sha256"] = {str(f):digest(f) for f in inputs}
        (work / fixture.name).write_bytes(fixture.read_bytes())
        (extract_directories if args.directories_only else extract_helpers if args.helper_only else extract_paths)(source,work)
        if args.extract_only:
            report.update(passed=None,status="extracted only; not compiled/executed")
            status = 0
        else:
            compiler = shlex.split(env.get("CC","cc"))
            if not compiler: raise ValueError("CC contains no command")
            flags = ["-std=c11","-D_POSIX_C_SOURCE=200809L","-Wall","-Wextra","-Werror",
                     "-g","-O1","-fno-omit-frame-pointer",
                     "-DFILEMANIP_DIRECTORIES_ONLY" if args.directories_only else "-DFILEMANIP_HELPER_ONLY" if args.helper_only else "-DFILEMANIP_PATHS_ONLY"]
            if (args.paths_only or args.directories_only) and os.uname().sysname == "Darwin": flags += ["-DCSRG_BASED"]
            if args.sanitizers != "none": flags += ["-fsanitize="+args.sanitizers]
            binary = work / ("filemanip-directories" if args.directories_only else "filemanip-helpers" if args.helper_only else "filemanip-paths")
            command = compiler + flags + shlex.split(env.get("CPPFLAGS","")) + shlex.split(env.get("CFLAGS",""))
            command += ["-I",str(work),str(work/fixture.name),"-pthread","-o",str(binary)]
            command += shlex.split(env.get("LDFLAGS",""))
            report["build"] = command_record(command,work,env,"build.log",60)
            report["status"] = "compile failed"
            if report["build"].get("exit_code") == 0:
                report["output"] = {"path":str(binary),"sha256":digest(binary),"mtime_ns":binary.stat().st_mtime_ns}
                for name in args.cases or cases:
                    invocation = [str(binary),name]
                    if args.paths_only or args.directories_only:
                        directory = work/"fixtures"/name
                        directory.mkdir(parents=True,mode=0o700)
                        invocation.append(str(directory))
                    run = command_record(invocation,work,env,name+".log",10)
                    run.update(case=name,passed=run.get("exit_code")==0)
                    report["cases"].append(run)
                report.update(passed=all(c["passed"] for c in report["cases"]),status="complete")
                status = 0 if report["passed"] else 1
        report["input_sha256_after"] = {str(f):digest(f) for f in inputs}
        report["source_unchanged"] = report["input_sha256"] == report["input_sha256_after"]
        if not report["source_unchanged"]:
            report.update(passed=False,status="inputs changed during qualification")
            status = 1
    except (OSError,ValueError,KeyError) as error:
        report.update(passed=False,status="setup/extraction failed",error=str(error))
    finally:
        report.update(end_utc=utc(),seconds=time.monotonic()-start)
        (work/"result.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({"result":str(work/"result.json"),"passed":report["passed"],"status":report["status"]}))
    return status


if __name__ == "__main__":
    raise SystemExit(main())
