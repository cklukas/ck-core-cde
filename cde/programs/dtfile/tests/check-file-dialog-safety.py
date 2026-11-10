#!/usr/bin/env python3
"""Focused bounded FileDialog label and destination-path checks."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import time


CASES = (
    "join-root", "join-directory", "join-exact", "join-long", "join-null",
    "target-relative", "target-absolute", "target-join-long",
    "target-resolved-long", "target-resolve-error",
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def utc():
    return datetime.now(timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-source", type=Path,
                        default=Path(__file__).resolve().parents[1])
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--case", action="append", choices=CASES, dest="cases")
    parser.add_argument("--sanitizers", default="address,undefined")
    args = parser.parse_args()
    source = args.module_source.resolve()
    work = args.work_dir.resolve()
    if os.uname().sysname == "Darwin" and not any(
            root.is_mount() and work.is_relative_to(root / "tmp")
            for root in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE"))):
        parser.error("Mac work must use tmp on a mounted approved volume")
    if work.exists():
        parser.error("work directory must be new")
    work.mkdir(parents=True, mode=0o700)
    (work / "tmp").mkdir(mode=0o700)
    env = dict(os.environ, TMPDIR=str(work / "tmp"), LANG="C", LC_ALL="C")
    leak_check = "0" if os.uname().sysname == "Darwin" else "1"
    env.setdefault("ASAN_OPTIONS", f"detect_leaks={leak_check}:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    source_file = source / "FileDialog.c"
    fixture = Path(__file__).with_name("file-dialog-safety.c")
    lexical_file = Path(__file__).with_name("check-transfer-completion.py")
    lexical = runpy.run_path(str(lexical_file))
    text = source_file.read_text()
    functions = []
    records = []
    for name in ("FileDialogBuildPath", "GetToPath"):
        body, line = lexical["function"](text, name)
        functions.append(f'#line {line} {json.dumps(str(source_file))}\n{body}\n')
        records.append({"function": name, "line": line,
                        "sha256": hashlib.sha256(body.encode()).hexdigest()})
    production = work / "file-dialog-production.inc"
    production.write_text("\n".join(functions))
    (work / fixture.name).write_bytes(fixture.read_bytes())
    rename = lexical["function"](text, "RenameOk")[0]
    contracts = {
        "selected_labels_structured": text.count("FileDialogSelectedObjectLabel(") == 5
            and "char name_string[18+MAX_PATH]" not in text,
        "rename_paths_bounded": rename.count("FileDialogBuildPath(new, sizeof(new)") == 2
            and "sprintf(new" not in rename,
        "preview_target_bounded": "GetToPath (whole_name, sizeof(whole_name)" in text,
        "resolved_target_bounded": "path_length >= to_size" in text
            and "memcpy(to, path, path_length + 1)" in text,
    }
    report = {
        "start_utc": utc(), "passed": False, "cases": [],
        "sanitizers": args.sanitizers, "compiler_jobs": 1,
        "runtime": dict(zip(("sysname", "nodename", "release", "version", "machine"),
                            os.uname())),
        "source_sha256": digest(source_file), "fixture_sha256": digest(fixture),
        "driver_sha256": digest(Path(__file__)), "functions": records,
        "production_sha256": digest(production), "contract": contracts,
    }
    started = time.monotonic()
    try:
        if not all(contracts.values()):
            raise RuntimeError(f"source contract failed: {contracts}")
        compiler = shlex.split(env.get("CC", "cc"))
        version = subprocess.run(compiler + ["--version"], env=env, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report["compiler"] = {"command": compiler, "version": version.stdout,
                              "exit_code": version.returncode}
        if version.returncode != 0:
            raise RuntimeError("compiler identity failed")
        command = compiler + ["-std=c11", "-D_POSIX_C_SOURCE=200809L",
                              "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                              "-fno-omit-frame-pointer"]
        if args.sanitizers != "none":
            command += [f"-fsanitize={args.sanitizers}"]
        command += [fixture.name, "-o", "file-dialog-safety"]
        build = subprocess.run(command, cwd=work, env=env, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report["build"] = {"command": command, "exit_code": build.returncode,
                           "output": build.stdout}
        if build.returncode != 0:
            raise RuntimeError("focused fixture compile failed")
        for case in args.cases or CASES:
            run = subprocess.run([str(work / "file-dialog-safety"), case],
                                 cwd=work, env=env, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            item = {"case": case, "returncode": run.returncode,
                    "stdout": run.stdout, "stderr": run.stderr}
            report["cases"].append(item)
            if run.returncode != 0:
                raise RuntimeError(f"case failed: {case}")
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    report["end_utc"] = utc()
    report["seconds"] = time.monotonic() - started
    (work / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["passed"]:
        raise SystemExit(1)
    print(f"FileDialog safety: {len(report['cases'])} cases passed")


if __name__ == "__main__":
    main()
