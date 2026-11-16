#!/usr/bin/env python3
"""Exercise dtfile's checked pipe and descriptor ownership helpers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time
from datetime import datetime, timezone

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


CASES = ("success", "null", "pipe-fault", "pipe-eintr", "getfd-fault",
         "getfd-eintr", "setfd-fault", "setfd-eintr", "close-eintr",
         "write-success", "write-null", "write-eintr", "write-short",
         "write-zero", "write-epipe")

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def now():
    return datetime.now(timezone.utc).isoformat()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--module-source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--sanitizers", default=_CK_SANITIZE_LIST)
    args = parser.parse_args()
    source, work = args.module_source.resolve(), args.work_dir.resolve()
    if work.exists(): parser.error("--work-dir must be new")
    if os.uname().sysname == "Darwin" and not any(
            root.is_mount() and work.is_relative_to(root / "tmp")
            for root in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE"))):
        parser.error("Mac work must be below tmp on a mounted approved volume")
    work.mkdir(parents=True, mode=0o700)
    tmp = work / "tmp"; tmp.mkdir(mode=0o700)
    leak_check = "0" if os.uname().sysname == "Darwin" else "1"
    env = dict(os.environ, TMPDIR=str(tmp), LANG="C", LC_ALL="C",
               ASAN_OPTIONS=f"detect_leaks={leak_check}:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    harness = Path(__file__).with_name("process-lifecycle.c")
    frozen = work / harness.name; frozen.write_bytes(harness.read_bytes())
    for name in ("FileProcess.c", "FileProcess.h"):
        (work / name).write_bytes((source / name).read_bytes())
    binary = work / "process-lifecycle"
    file_op = source / "FileOp.c"
    file_op_text = file_op.read_text()
    source_contract = {
        "checked_pipe_sites": file_op_text.count("DtfileProcessPipe("),
        "raw_pipe_sites": len(re.findall(r"(?<!Process)\bpipe\s*\(", file_op_text)),
        "raw_write_sites": len(re.findall(r"(?<!Process)\bwrite\s*\(", file_op_text)),
        "child__exit_sites": file_op_text.count("_exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE)"),
        "shared_static_status": "static int status" in file_op_text,
        "per_operation_status": "cb_data->status" in file_op_text,
        "select_failure_handoff": bool(re.search(
            r"select failed in FileMoveCopy.*?XtAppAddInput", file_op_text, re.S)),
    }
    if source_contract != {"checked_pipe_sites": 4, "raw_pipe_sites": 0,
                           "raw_write_sites": 1,
                           "child__exit_sites": 3, "shared_static_status": False,
                           "per_operation_status": True, "select_failure_handoff": True}:
        raise SystemExit(f"FileOp startup contract mismatch: {source_contract}")
    command = [*shlex.split(env.get("CC", "cc")), "-std=gnu23", "-Wall", "-Wextra", "-Werror",
               "-O1", "-g", "-I", str(work), str(frozen), "-pthread", "-o", str(binary)]
    if args.sanitizers != "none": command += ["-fsanitize=" + args.sanitizers, "-fno-omit-frame-pointer"]
    record = {"started_utc": now(), "cases": [], "command": command, "passed": False,
              "jobs": 1, "sanitizers": args.sanitizers,
              "source_contract": source_contract,
              "source_sha256": {p.name: digest(p) for p in
                                (source / "FileProcess.c", source / "FileProcess.h", file_op, harness)}}
    started = time.monotonic()
    with (work / "build.log").open("w") as log:
        built = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    record.update(build_exit_code=built.returncode, build_seconds=time.monotonic() - started)
    if built.returncode:
        (work / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        print((work / "build.log").read_text(), end="")
        return 1
    for case in CASES:
        log_path = work / (case + ".log")
        started = time.monotonic()
        with log_path.open("w") as log:
            run = subprocess.run([str(binary), case], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=10)
        entry = {"name": case, "exit_code": run.returncode, "passed": run.returncode == 0,
                 "seconds": time.monotonic() - started, "log_sha256": digest(log_path)}
        record["cases"].append(entry)
        print(("PASS " if entry["passed"] else "FAIL ") + case)
    source_unchanged = all(digest(source / name) == value for name, value in
                           record["source_sha256"].items() if name != harness.name)
    source_unchanged = source_unchanged and digest(harness) == record["source_sha256"][harness.name]
    record.update(ended_utc=now(), source_unchanged=source_unchanged,
                  passed=source_unchanged and all(item["passed"] for item in record["cases"]),
                  executable_sha256=digest(binary))
    (work / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    return 0 if record["passed"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
