#!/usr/bin/env python3
"""Run the reader of the database files (DtUtil1/DbReader.c) under the sanitizers.

The functions that read, join and expand the lines of a database file and
the definition of a record are cut out of DbReader.c and compiled with
tests/dbreader-test.c, which replaces the Xt and Xrm functions.  Every
scenario runs in its own process.  DBREADER_SOURCE names another copy of
DbReader.c to test, e.g. an older revision.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
source = here.parent
path = Path(os.environ.get("DBREADER_SOURCE", source / "DtUtil1" / "DbReader.c"))
text = path.read_text()
include = source.parents[1] / "include" / "Dt"
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

scenarios = ("messages", "lines", "many_variables", "cycles", "variables", "multibyte")
cc = shlex.split(os.environ.get("CC", "cc"))
failed = []
with tempfile.TemporaryDirectory(prefix="dbreader-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    (work / "dbreader-types.h").write_text(types)
    (work / "dbreader-functions.h").write_text(functions)
    binary = work / "dbreader-test"
    build = subprocess.run(cc + ["-std=gnu99", "-D_DEFAULT_SOURCE", "-UNDEBUG", "-g", "-O1", "-fno-omit-frame-pointer",
                                 "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                                 "-w", "-I", str(work),
                                 str(here / "dbreader-test.c"), "-o", str(binary)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if build.returncode:
        errors = [l for l in build.stdout.splitlines() if "error" in l]
        sys.exit("cannot build the database reader test:\n" + "\n".join(errors[:10]))
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:" + os.environ.get("ASAN_OPTIONS", ""))
    for scenario in scenarios:
        try:
            run = subprocess.run([str(binary), scenario], env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, timeout=120)
            code, output = run.returncode, run.stdout
        except subprocess.TimeoutExpired:
            code, output = -1, "timed out"
        if code == 0:
            print("ok   " + scenario)
            continue
        failed.append(scenario)
        report = output.strip().splitlines()
        keep = [l for l in report if "ERROR:" in l or "SUMMARY:" in l or "runtime error" in l or "Assertion" in l]
        print("FAIL " + scenario + " (exit " + str(code) + ")")
        for l in (report if os.environ.get("DBREADER_VERBOSE") else (keep or report[-4:])[:5]):
            print("     " + l)
if failed:
    sys.exit("database reader checks failed: " + ", ".join(failed))
print("database reader: " + str(len(scenarios)) + " scenarios passed")
