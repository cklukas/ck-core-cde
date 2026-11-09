#!/usr/bin/env python3
"""Run the actual standalone XDG Trash reader and extracted Xt wrapper.

With no --work-dir, a disposable directory is created only beneath explicit
TMPDIR, matching the other native checks. To compare the old wrapper, keep
--module-source pointed at the new real reader and use --wrapper-source on
the baseline Trash.c; the receipt distinguishes those sources.
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
import tempfile
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def utc():
    return datetime.now(timezone.utc).isoformat()


def balanced(text, start, left, right):
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
        if text[i] in "\"'":
            quote = text[i]
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
                raise ValueError("unterminated C literal")
            continue
        if text[i] == left:
            depth += 1
        elif text[i] == right:
            depth -= 1
            if not depth:
                return i + 1
        i += 1
    raise ValueError("unbalanced C delimiters")


def extract_function(text, name):
    pattern = re.compile(r"(?m)^static\s+(?:Boolean|char\s*\*)\s+" + re.escape(name) + r"\s*\(")
    found = []
    for match in pattern.finditer(text):
        end_args = balanced(text, match.end() - 1, "(", ")")
        after = re.match(r"\s*", text[end_args:]).end() + end_args
        if after < len(text) and text[after] == "{":
            end = balanced(text, after, "{", "}")
            found.append((text[match.start():end], text[match.start():end_args],
                          text.count("\n", 0, match.start()) + 1))
    if len(found) != 1:
        raise ValueError(f"expected one production {name} definition, found {len(found)}")
    return found[0]


def mac_path(path):
    return any(root.is_mount() and path.is_relative_to(root / "tmp")
               for root in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE")))


def qualify(args):
    source = args.module_source.resolve()
    wrapper = (args.wrapper_source or source / "Trash.c").resolve()
    if wrapper.is_dir():
        wrapper /= "Trash.c"
    work = args.work_dir.resolve()
    if work.exists():
        raise ValueError("--work-dir must be new; prior evidence is preserved")
    if os.uname().sysname == "Darwin" and not mac_path(work):
        raise ValueError("Mac work must use tmp on a mounted approved volume")
    work.mkdir(mode=0o700, parents=True)
    (work / "tmp").mkdir(mode=0o700)
    fixture = Path(__file__).with_name("trash-info.c").resolve()
    (work / fixture.name).write_bytes(fixture.read_bytes())
    names = re.findall(r'(?m)^\s*\{"([^"]+)",', fixture.read_text())
    core = names + [
        "absolute-no-base", "relative-no-base", "file-nul", "unknown-fragment-spoof",
        "line-limit", "overlong-line", "overlong-unterminated", "file-limit",
        "overlong-file", "resolved-limit", "resolved-too-large", "read-error",
        "no-memory-1", "no-memory-2", "no-memory-3", "no-memory-4",
        "null-stream", "null-result", "free-null"]
    wrappers = [
        "absolute", "relative-to-original-base", "literal-percent-one-pass",
        "key-format-spaces", "encoded-leading-space", "normalized-first-duplicates-win",
        "first-fields-win", "later-invalid-duplicates-ignored", "complete-final-eof",
        "missing-header", "missing-date", "encoded-nul", "file-nul",
        "unknown-fragment-spoof", "read-error", "close-error", "missing-file",
        "no-memory-1", "no-memory-2", "fifo-input", "symlink-input",
        "fstat-error", "fdopen-error", "overlong-file"]
    cases = args.cases or [*("core-" + name for name in core),
                           *("wrapper-" + name for name in wrappers)]
    allowed = {"core-" + name for name in core} | {"wrapper-" + name for name in wrappers}
    if any(name not in allowed for name in cases):
        raise ValueError("unknown --case; use a core-/wrapper- fixture name")
    env = dict(os.environ, TMPDIR=str(work / "tmp"), LANG="C", LC_ALL="C")
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    inputs = [source / "XdgTrashInfo.c", source / "XdgTrashInfo.h", wrapper]
    record = {"started_utc": utc(), "jobs": 1, "module_source": str(source),
              "wrapper_source": str(wrapper), "source_hashes": {str(p): sha(p) for p in inputs},
              "fixture_sha256": sha(fixture), "driver_sha256": sha(Path(__file__).resolve()),
              "TMPDIR": env["TMPDIR"], "ASAN_OPTIONS": env["ASAN_OPTIONS"],
              "UBSAN_OPTIONS": env["UBSAN_OPTIONS"], "sanitizers": args.sanitizers,
              "coverage_limits": "Actual standalone module, test-only allocator seams; "
              "actual production wrapper with reported close/Xt-copy faults. "
              "Read EIO is a real stdio callback error after file data. No import/restore mutations or GUI.",
              "cases": [], "passed": False}
    result = work / "result.json"

    def save():
        result.write_text(json.dumps(record, indent=2) + "\n")

    try:
        text = wrapper.read_text()
        body, signature, line = extract_function(text, "ParseXdgTrashInfo")
        arity = signature[signature.index("(") + 1:-1].count(",") + 1
        if arity not in (3, 4):
            raise ValueError(f"unsupported production wrapper arity {arity}")
        pieces = []
        if arity == 3:
            decoder, _, decoder_line = extract_function(text, "XdgPercentDecode")
            pieces += [f"#line {decoder_line} {json.dumps(str(wrapper))}\n", decoder + "\n"]
        pieces += ["#define ParseXdgTrashInfo ProductionParseXdgTrashInfo\n",
                   f"#line {line} {json.dumps(str(wrapper))}\n", body + "\n",
                   "#undef ParseXdgTrashInfo\n",
                   "static Boolean TestParseTrashInfo(const char *name,const char *base,char **path,char **date)\n{\n"]
        pieces += ["    return ProductionParseXdgTrashInfo(name,base,path,date);\n" if arity == 4 else
                   "    (void)base; return ProductionParseXdgTrashInfo(name,path,date);\n", "}\n"]
        include = work / "trash-info-wrapper.inc"
        include.write_text("".join(pieces))
        record.update(wrapper_api_arity=arity, wrapper_body_sha256=hashlib.sha256(body.encode()).hexdigest(),
                      wrapper_include_sha256=sha(include))
        # Include the entire actual translation unit, not copied reader logic.
        # Standard headers precede allocator macros, avoiding macro-rewritten
        # libc declarations. Only negative allocation behavior is substituted.
        bridge = work / "trash-info-core.c"
        bridge.write_text(
            '#include <stdlib.h>\n#include <string.h>\n#include <stdint.h>\n'
            '#include "XdgTrashInfo.h"\n'
            'extern void *TrashInfoTestMalloc(size_t);\n'
            'extern char *TrashInfoTestStrdup(const char *);\n'
            '#define malloc TrashInfoTestMalloc\n#define strdup TrashInfoTestStrdup\n'
            f'#include {json.dumps(str(source / "XdgTrashInfo.c"))}\n')
        cc = shlex.split(env.get("CC", "cc"))
        flags = ["-std=c11", "-D_GNU_SOURCE", "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE",
                 "-D_POSIX_C_SOURCE=200809L", "-O0", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-I", str(source), "-I", str(work)]
        if args.sanitizers != "none":
            flags += ["-fsanitize=" + args.sanitizers, "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]
        binary = work / "trash-info"
        command = [*cc, *flags, str(work / fixture.name), str(bridge), "-o", str(binary)]
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
        for name in cases:
            directory = work / name
            directory.mkdir(mode=0o700)
            case = {"name": name, "started_utc": utc()}
            started = time.monotonic()
            try:
                with (directory / "run.log").open("w") as log:
                    ran = subprocess.run([str(binary), name, str(directory)], env=env, cwd=directory,
                                         stdout=log, stderr=subprocess.STDOUT, timeout=10, check=False)
                case["exit_code"] = ran.returncode
            except subprocess.TimeoutExpired as error:
                case.update(exit_code=124, timeout=str(error))
            case.update(ended_utc=utc(), seconds=time.monotonic() - started,
                        passed=case["exit_code"] == 0,
                        input_sha256=sha(directory / "input.trashinfo") if (directory / "input.trashinfo").exists() else None)
            record["cases"].append(case)
            save()
            print(f"{'PASS' if case['passed'] else 'FAIL'} {name} ({case['seconds']:.3f}s)", flush=True)
        record["source_unchanged"] = all(sha(Path(p)) == digest for p, digest in record["source_hashes"].items())
        record["passed"] = record["source_unchanged"] and all(case["passed"] for case in record["cases"])
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
    parser.add_argument("--wrapper-source", type=Path, help="optional old/new Trash.c; core still comes from --module-source")
    parser.add_argument("--work-dir", type=Path, help="new directory; default uses explicit TMPDIR and cleans afterward")
    parser.add_argument("--case", action="append", dest="cases")
    parser.add_argument("--sanitizers", default="address,undefined")
    args = parser.parse_args()
    try:
        if args.work_dir is not None:
            return qualify(args)
        if not os.environ.get("TMPDIR"):
            parser.error("TMPDIR must be explicit when --work-dir is omitted")
        temporary_root = Path(os.environ["TMPDIR"]).resolve()
        if os.uname().sysname == "Darwin" and not mac_path(temporary_root):
            parser.error("TMPDIR must use a mounted approved Mac temporary volume")
        with tempfile.TemporaryDirectory(prefix="dtfile-trash-info-", dir=temporary_root) as scratch:
            args.work_dir = Path(scratch) / "run"
            return qualify(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
