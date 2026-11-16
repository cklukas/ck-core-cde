#!/usr/bin/env python3
"""Variables of the database files (DtUtil1/DbReader.c) under the sanitizers.

Three things are checked, each in its own process with a time limit:

  - a "set DtDbVersion=..." line of any length: the version is expanded in a
    buffer that holds the expansion (it used to be a 13 byte copy that the
    expansion overran), and a version that is not 1.0 rejects the file;
  - an invalid multibyte sequence in the name of a "set" line or of a
    reference: the scan of the name goes on and ends (it used to step back
    and never end);
  - the same results as before for valid lines: set variables, the
    environment, valid multibyte names.

The functions are cut out of DbReader.c and compiled with
tests/check-dbreader-variables.c, which replaces the Xt and Xrm functions.

  --source-revision REV   negative control: take DbReader.c from the git
                          revision REV; the test must FAIL, and the exit
                          status is 0 only when it does.
  --worktree DIR          the checkout to read (default: the one of this file)
  --scratch DIR           where to build (default: a directory below TMPDIR)
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
from contextlib import nullcontext

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


here = Path(__file__).resolve().parent
source = here.parent
ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of DbReader.c to use as a negative control")
ap.add_argument("--worktree", type=Path, default=source.parents[1].parent)
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()
relative = "cde/lib/DtSvc/DtUtil1/DbReader.c"
if args.source_revision:
    text = subprocess.run(["git", "-C", str(args.worktree), "show", args.source_revision + ":" + relative],
                          check=True, capture_output=True, text=True).stdout
    label = args.source_revision
else:
    text = (args.worktree / relative).read_text()
    label = "working tree"
path = Path(relative)
include = args.worktree / "cde" / "include" / "Dt"
reader_h = (include / "DbReader.h").read_text()
dtsdb_h = (include / "DtsDb.h").read_text() if (include / "DtsDb.h").exists() else ""


def skip_literal(text, i):
    if text.startswith("/*", i):
        return text.index("*/", i + 2) + 2
    if text.startswith("//", i):
        return text.index("\n", i)
    quote = text[i]
    i += 1
    while text[i] != quote:
        i += 2 if text[i] == "\\" else 1
    return i + 1


def function(name, required=True):
    """The definition of NAME, with the lines of its type above it."""
    for match in re.finditer(r"^" + re.escape(name) + r"\s*\(", text, re.M):
        i, depth = match.end(), 1
        while depth:
            if text[i] in "\"'" or text.startswith(("/*", "//"), i):
                i = skip_literal(text, i)
                continue
            depth += (text[i] == "(") - (text[i] == ")")
            i += 1
        if not re.match(r"\s*\{", text[i:]):
            continue
        # the type lines above the name (blank lines and comments end them)
        start = match.start()
        while True:
            previous = text.rfind("\n", 0, start - 1) + 1
            line = text[previous:start - 1]
            if not line.strip() or line.lstrip().startswith(("*", "/*", "#", "}")) or line.rstrip().endswith((";", "*/", "}")):
                break
            start = previous
        i = text.index("{", i) + 1
        depth = 1
        while depth:
            if text[i] in "\"'" or text.startswith(("/*", "//"), i):
                i = skip_literal(text, i)
                continue
            depth += (text[i] == "{") - (text[i] == "}")
            i += 1
        return text[start:i] + "\n"
    if required:
        raise AssertionError(name + " not found in " + str(path))
    return ""


types = ""
for name in ("DtDtsDbField",):
    types += re.search(r"typedef\s+struct\s*\{[^{}]*\}\s*" + name + r";", reader_h + dtsdb_h).group(0) + "\n"
types += re.search(r"typedef\s+Boolean\s*\(\*DtDbConverter\)[^;]*;", reader_h).group(0) + "\n"
types += re.search(r"typedef\s+struct\s*\{[^{}]*DtDbConverter\s*\*\s*converters;[^{}]*\}\s*DtDbRecordDesc;", reader_h).group(0) + "\n"
for pattern in (r"^#define\s+MAX_LINE_LENGTH\s+\d+\s*$", r"^#define\s+NUMBER_FIELDS_DEFAULT\s+\d+\s*$",
                r"^#define\s+NUMBER_FIELDS_INCREMENT\s+\d+\s*$", r"^#define\s+DTUNLIMITEDFIELDS\s+\d+\s*$",
                r"^#define\s+AT_START\s+\d+", r"^#define\s+NO_START_SYMBOL\s+\d+", r"^#define\s+START_SYMBOL\s+\d+",
                r"^#define\s+VERSION_\w+\s+.*$", r"^#define\s+MAX_VARIABLE_REPLACEMENTS\s+\d+\s*$",
                r"^#define\s+MAX_EXPANDED_LINE_LENGTH\s+.*$"):
    types += "".join(m.group(0) + "\n" for m in re.finditer(pattern, text + reader_h, re.M))
types += re.search(r"typedef struct \{\s*char \* varName;\s*char \* value;\s*\} VariableEntry;", text).group(0) + "\n"
types += "static int variableCount;\nstatic VariableEntry * variableSet;\n"
if re.search(r"^static int variableCapacity;", text, re.M):
    types += "static int variableCapacity;\n"
# the declarations of the static functions of the file
declarations = re.search(r"/\*+\s+Static Function Declarations\s+\*+/(.*?)/\*+\s+End Static Function Declarations", text, re.S).group(1)

names = ["report_error", "read_line", "clean_line", "get_variable", "ResolveVariableReference",
         "_DtDbFillVariables", "DefineVariable", "ClearVariables", "SplitField", "MatchKeyword", "ReadNextEntry"]
functions = declarations + "".join(function(n, required=n != "report_error") for n in names)

scenarios = ("version_overflow", "version_valid", "invalid_multibyte", "valid_multibyte")
cc = shlex.split(os.environ.get("CC", "cc"))
failed = []
scratch = nullcontext(str(args.scratch)) if args.scratch else tempfile.TemporaryDirectory(prefix="dbreader-vars-", dir=os.environ.get("TMPDIR"))
if args.scratch:
    args.scratch.mkdir(parents=True, exist_ok=True)
with scratch as work:
    work = Path(work)
    (work / "dbreader-types.h").write_text(types)
    (work / "dbreader-functions.h").write_text(functions)
    binary = work / "dbreader-variables-test"
    build = subprocess.run(cc + ["-std=gnu23", "-D_GNU_SOURCE", "-UNDEBUG", "-g", "-O1", "-fno-omit-frame-pointer",
                                 f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-sanitize-recover=undefined",
                                 "-w", "-I", str(work),
                                 str(here / "check-dbreader-variables.c"), "-o", str(binary)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if build.returncode:
        errors = [l for l in build.stdout.splitlines() if "error" in l]
        sys.exit("cannot build the database variable test:\n" + "\n".join(errors[:10]))
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:" + os.environ.get("ASAN_OPTIONS", ""))
    for scenario in scenarios:
        try:
            run = subprocess.run([str(binary), scenario], env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, timeout=60)
            code, output = run.returncode, run.stdout
        except subprocess.TimeoutExpired:
            code, output = -1, "timed out"
        if code == 0:
            print("ok   " + scenario)
            continue
        failed.append(scenario)
        report = output.strip().splitlines()
        keep = [l for l in report if "ERROR:" in l or "SUMMARY:" in l or "runtime error" in l or "Assertion" in l
                or "Alarm" in l or "timed out" in l]
        print("FAIL " + scenario + " (exit " + str(code) + ")")
        for l in (report if os.environ.get("DBREADER_VERBOSE") else (keep or report[-4:])[:4]):
            print("     " + l)
if args.source_revision:
    if not failed:
        sys.exit("NEGATIVE CONTROL FAILED: " + label + " passed the variable test")
    print("negative control (" + label + "): fails as expected (" + ", ".join(failed) + ")")
elif failed:
    sys.exit("database variable checks failed: " + ", ".join(failed))
else:
    print("database variables: " + str(len(scenarios)) + " scenarios passed (" + label + ")")
