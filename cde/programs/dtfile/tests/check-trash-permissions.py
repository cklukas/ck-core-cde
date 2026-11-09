#!/usr/bin/env python3
"""Check whole production Trash permission functions using private fixtures.

No arguments is suitable for make check: a disposable work directory is made
under explicit TMPDIR. --work-dir retains logs, hashes, outputs and result.json.
No native NFS server, GUI, delete authority or ancestor-race protection is claimed.
--baseline-source selects exact old Trash.c assessment bodies/ABI and runs only
readdir-eio/closedir-eio by default; missing new helpers are not counted as proof.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import sys
import tempfile
import time

CASES = ("normal-tree", "deep-tree", "symlink-no-follow", "parent-permission",
         "child-permission", "readdir-eintr", "readdir-eio", "closedir-eio",
         "first-error", "child-oom", "helper-bytes", "probe-success", "probe-deep",
         "probe-close-error", "probe-remove-error", "probe-first-error")
BASELINE_CASES = ("readdir-eio", "closedir-eio")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def utc():
    return datetime.now(timezone.utc).isoformat()


def approved_mac(path):
    return any(root.is_mount() and path.is_relative_to(root / "tmp")
               for root in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE")))


def qualify(args):
    work = args.work_dir.resolve()
    if work.exists():
        raise ValueError("--work-dir must be new")
    if os.uname().sysname == "Darwin" and not approved_mac(work):
        raise ValueError("Mac work must use tmp on a mounted approved volume")
    work.mkdir(parents=True, mode=0o700)
    (work / "tmp").mkdir(mode=0o700)
    env = dict(os.environ, TMPDIR=str(work / "tmp"), LC_ALL="C", LANG="C")
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    baseline = args.baseline_source is not None
    source = args.baseline_source.resolve() if baseline else args.module_source.resolve() / "Trash.c"
    if source.is_dir():
        source /= "Trash.c"
    selected_cases = args.cases or (BASELINE_CASES if baseline else CASES)
    if baseline and any(name not in BASELINE_CASES for name in selected_cases):
        raise ValueError("--baseline-source is limited to readdir-eio and closedir-eio")
    sibling = Path(__file__).resolve().parent
    extractor = sibling / "check-transfer-completion.py"
    fixture = sibling / "trash-permissions.c"
    record = {"started_utc": utc(), "jobs": 1, "source": str(source),
              "source_sha256": sha(source), "fixture_sha256": sha(fixture),
              "extractor_sha256": sha(extractor), "driver_sha256": sha(Path(__file__).resolve()),
              "TMPDIR": env["TMPDIR"], "ASAN_OPTIONS": env["ASAN_OPTIONS"],
              "UBSAN_OPTIONS": env["UBSAN_OPTIONS"], "sanitizers": args.sanitizers,
              "coverage_limits": "Whole production permission functions; real private files, "
              "test-only syscall failures and root/NFS branch selectors. No native NFS or "
              "retained deletion authority claim. Return77 is an explicit unsupported case.",
              "baseline_source": str(source) if baseline else None,
              "selected_cases": list(selected_cases),
              "unrun_cases": [name for name in CASES if name not in selected_cases],
              "baseline_limits": ("Only old readdir/closedir false-success behavior is exercised; "
                                  "no helper, OOM, deep-tree or root/NFS baseline coverage claimed."
                                  if baseline else None),
              "cases": [], "passed": False}
    result = work / "result.json"

    def save():
        result.write_text(json.dumps(record, indent=2) + "\n")

    try:
        # Reuse the reviewed lexer without triggering that driver's main or
        # generating project __pycache__ files.
        sys.dont_write_bytecode = True
        helpers = runpy.run_path(str(extractor), run_name="trash_permissions_extractor")
        function, balanced_end = helpers["function"], helpers["balanced_end"]
        text = source.read_text()
        entry, entry_line = function(text, "CheckDeletePermission")
        recur, recur_line = function(text, "CheckDeletePermissionRecur")
        try:
            child, child_line = function(text, "DeletePermissionChild")
        except ValueError:
            child, child_line = None, 0
        args_end = balanced_end(recur, recur.index("("), "(", ")")
        chunks = [f"#define HAVE_DELETE_PERMISSION_CHILD {int(child is not None)}\n",
                  recur[:args_end] + ";\n"]
        manifest = []
        if baseline:
            chunks.append("#define CheckDeletePermission BaselineCheckDeletePermission\n")
        for name, body, line in (("DeletePermissionChild", child, child_line),
                                 ("CheckDeletePermission", entry, entry_line),
                                 ("CheckDeletePermissionRecur", recur, recur_line)):
            if body is not None:
                chunks += [f"#line {line} {json.dumps(str(source))}\n", body + "\n"]
                manifest.append({"name": name, "line": line,
                                 "sha256": hashlib.sha256(body.encode()).hexdigest()})
        if baseline:
            # The old signature/body and recursive signature are copied verbatim.
            # Only the entry symbol is renamed. This call-through reports its
            # actual return/errno and then restores errno for the unchanged C
            # assertions; it makes no assessment or aggregation decision.
            chunks.append(
                "#undef CheckDeletePermission\n"
                "static int CheckDeletePermission(char *parentdir,char *destinationPath)\n"
                "{\n"
                "  int result=BaselineCheckDeletePermission(parentdir,destinationPath);\n"
                "  int error=errno;\n"
                '  fprintf(stderr,"BASELINE ASSESSMENT: result=%d errno=%d\\n",result,error);\n'
                "  errno=error;return result;\n}\n")
        include = work / "trash-permissions-production.inc"
        include.write_text("\n".join(chunks))
        record.update(functions=manifest, extracted_sha256=sha(include),
                      child_helper_present=child is not None,
                      borrowed_candidate_helper=False,
                      assessment_entry_signature=entry[:balanced_end(entry, entry.index("("), "(", ")")],
                      recursive_signature=recur[:args_end])
        copied = work / fixture.name
        copied.write_bytes(fixture.read_bytes())
        binary = work / "trash-permissions"
        command = [*shlex.split(env.get("CC", "cc")), "-std=c11", "-O0", "-g",
                   "-D_GNU_SOURCE", "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE",
                   "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-I", str(work), str(copied), "-o", str(binary)]
        if baseline:
            command.append("-Wno-parentheses")  # Preserve old while(dp=readdir(...)) verbatim.
        if args.sanitizers != "none":
            command += ["-fsanitize=" + args.sanitizers, "-fno-omit-frame-pointer",
                        "-fno-sanitize-recover=all"]
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
        record.update(executable_path=str(binary), executable_sha256=sha(binary),
                      executable_mtime_ns=binary.stat().st_mtime_ns)
        for name in selected_cases:
            directory = work / name
            directory.mkdir(mode=0o700)
            case = {"name": name, "started_utc": utc()}
            started = time.monotonic()
            try:
                with (directory / "run.log").open("w") as log:
                    ran = subprocess.run([str(binary), name, str(directory)], cwd=directory,
                                         env=env, stdout=log, stderr=subprocess.STDOUT,
                                         timeout=10, check=False)
                case["exit_code"] = ran.returncode
            except subprocess.TimeoutExpired as error:
                case.update(exit_code=124, timeout=str(error))
            case.update(ended_utc=utc(), seconds=time.monotonic() - started,
                        unsupported=case["exit_code"] == 77, passed=case["exit_code"] == 0)
            record["cases"].append(case)
            save()
            label = "SKIP" if case["unsupported"] else "PASS" if case["passed"] else "FAIL"
            print(f"{label} {name} ({case['seconds']:.3f}s)", flush=True)
        record["source_unchanged"] = sha(source) == record["source_sha256"]
        record["unsupported_cases"] = [c["name"] for c in record["cases"] if c["unsupported"]]
        record["passed"] = record["source_unchanged"] and all(
            c["passed"] or c["unsupported"] for c in record["cases"])
        record["ended_utc"] = utc()
        save()
        print(f"Receipt: {result}", flush=True)
        return 0 if record["passed"] else 1
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        record.update(error=str(error), ended_utc=utc(), passed=False)
        save()
        print(f"FAIL: {error}; receipt: {result}", flush=True)
        return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--baseline-source", type=Path,
                        help="old Trash.c; focused readdir-eio/closedir-eio behavioral control only")
    parser.add_argument("--work-dir", type=Path, help="new retained work directory; default uses explicit TMPDIR")
    parser.add_argument("--case", action="append", dest="cases", choices=CASES)
    parser.add_argument("--sanitizers", default="address,undefined")
    args = parser.parse_args()
    try:
        if args.work_dir is not None:
            return qualify(args)
        if not os.environ.get("TMPDIR"):
            parser.error("TMPDIR must be explicit when --work-dir is omitted")
        temporary_root = Path(os.environ["TMPDIR"]).resolve()
        if os.uname().sysname == "Darwin" and not approved_mac(temporary_root):
            parser.error("TMPDIR must use a mounted approved Mac temporary volume")
        with tempfile.TemporaryDirectory(prefix="dtfile-trash-permissions-", dir=temporary_root) as scratch:
            args.work_dir = Path(scratch) / "run"
            return qualify(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
