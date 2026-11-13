#!/usr/bin/env python3
"""A cache file of the type database with damaged tables is refused.

The cache is a header and a list of integers that hold the databases, the
records, the fields, two integer tables and the string table.  The header
check only looked at the indexes of the header; counts, offsets and string
offsets inside the tables were trusted, so a file with bad values was read
outside the mapping.  The test builds a valid cache with the routines that
build the real one, maps it with _DtDtsMMapDB(), reads everything in it,
and then damages it in one place at a time (some 80 changes: counts, indexes,
string offsets, chains, block sizes) and checks that every damaged file is
refused, and a file the fuzzer found.

_DtDtsMMapDB() and what it calls are cut out of DtUtil1/DtsMM.c, and strtab.c
and inttab.c are compiled whole, all under AddressSanitizer and
UndefinedBehaviorSanitizer.  The file is read through a copy of exactly its
size, so that every read outside of it is reported.

  --source-revision REV   negative control: take DtsMM.c, strtab.c and
                          inttab.c from the git revision REV; the test must
                          FAIL, and the exit status is 0 only when it does.
  --worktree DIR          the checkout to read (default: the one of this file)
  --scratch DIR           where to build (default: a directory below TMPDIR)
"""
import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
ap = argparse.ArgumentParser()
ap.add_argument("--source-revision", help="git revision of the sources to use as a negative control")
ap.add_argument("--worktree", type=Path, default=here.parents[3])
ap.add_argument("--scratch", type=Path, default=None)
args = ap.parse_args()
label = args.source_revision or "working tree"
relative = "cde/lib/DtSvc/DtUtil1/"


def source(name):
    if args.source_revision:
        return subprocess.run(["git", "-C", str(args.worktree), "show", args.source_revision + ":" + relative + name],
                              check=True, capture_output=True, text=True).stdout
    return (args.worktree / relative / name).read_text()


mm = source("DtsMM.c")
strtab = source("strtab.c")
inttab = source("inttab.c")
header = (args.worktree / "cde/include/Dt/DtsMM.h").read_text()


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
    """The definition of NAME in DtsMM.c with the lines of its type above it."""
    for match in re.finditer(r"^" + re.escape(name) + r"\s*\(", mm, re.M):
        i, depth = match.end(), 1
        while depth:
            if mm[i] in "\"'" or mm.startswith(("/*", "//"), i):
                i = skip_literal(mm, i)
                continue
            depth += (mm[i] == "(") - (mm[i] == ")")
            i += 1
        if not re.match(r"\s*\{", mm[i:]):
            continue
        start = match.start()
        while True:
            previous = mm.rfind("\n", 0, start - 1) + 1
            line = mm[previous:start - 1]
            if not line.strip() or line.lstrip().startswith(("*", "/*", "#", "}")) or line.rstrip().endswith((";", "*/", "}")):
                break
            start = previous
        i = mm.index("{", i) + 1
        depth = 1
        while depth:
            if mm[i] in "\"'" or mm.startswith(("/*", "//"), i):
                i = skip_literal(mm, i)
                continue
            depth += (mm[i] == "{") - (mm[i] == "}")
            i += 1
        return mm[start:i] + "\n"
    if required:
        raise AssertionError(name + " not found in DtsMM.c of " + label)
    return ""


types = "".join(re.findall(r"^typedef\s+int\s+\w+;.*\n", header, re.M))
for name in ("DtDtsMMField", "DtDtsMMRecord", "DtDtsMMDatabase", "DtDtsMMHeader"):
    types += re.search(r"typedef\s+struct\s*\{[^{}]*\}\s*" + name + r";", header).group(0) + "\n"
types += '#define DtDTS_DC_NAME "DATA_CRITERIA"\n'
view = re.search(r"struct cache_view\s*\{.*?\n\};\n", mm, re.S)
types += view.group(0) if view else ""

state = re.search(r"^static\tDtDtsMMDatabase\t\*db_list;.*?(?=^int _DtDtsMMUnLoad)", mm, re.M | re.S)
assert state, "the variables of the mapping not found in DtsMM.c of " + label
names = ["_DtDtsMMGetPtr", "_DtDtsMMGetPtrSize", "_DtDtsMMGetDCNameIndex", "_DtDtsMMGetDbName",
         "_DtDtsMMGetNoNameIndex", "_DtDtsMMGetBufferIndex", "_DtDtsMMGetFileList", "_DtDtsMMBosonToString",
         "_DtDtsMMStringToBoson", "_DtDtsMMGet", "_DtDtsMMGetField", "_DtDtsMMGetFieldByName",
         "_DtDtsMMGetRecordByName", "cache_size_fits", "index_inside", "cache_header_valid",
         "cache_block", "cache_ints", "cache_string", "cache_records_below", "cache_record_entry",
         "cache_name_entry", "cache_inttab", "cache_database_valid", "cache_tables_valid",
         "_DtDtsMMapDB", "_DtDtsMMUnLoad"]
optional = {"cache_block", "cache_ints", "cache_string", "cache_records_below", "cache_record_entry",
            "cache_name_entry", "cache_inttab", "cache_database_valid", "cache_tables_valid"}
functions = state.group(0) + "".join(function(n, n not in optional) for n in names)

cc = shlex.split(os.environ.get("CC", "cc"))
flags = ["-std=gnu23", "-D_GNU_SOURCE", "-UNDEBUG", "-g", "-O1", "-fno-omit-frame-pointer",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-w"]
includes = ["-I", str(args.worktree / "cde/include"), "-I", str(args.worktree / "cde/include/Dt"),
            "-I", str(args.worktree / relative.rstrip("/"))]
if args.source_revision:
    # the hash of the old strtab.c shifts by 32 and more for long names; that is
    # a defect of its own, and it must not hide what this test looks for
    flags.append("-fno-sanitize=shift")
if "_DtShmValidateStrtab" in strtab and "_DtShmValidateInttab" in inttab:
    flags.append("-DHAVE_TABLE_CHECKS")

scratch = nullcontext(str(args.scratch)) if args.scratch else tempfile.TemporaryDirectory(prefix="dtsmm-cache-", dir=os.environ.get("TMPDIR"))
if args.scratch:
    args.scratch.mkdir(parents=True, exist_ok=True)
with scratch as work:
    work = Path(work)
    (work / "dtsmm-types.h").write_text(types)
    (work / "dtsmm-functions.h").write_text(functions)
    (work / "strtab.c").write_text(strtab)
    (work / "inttab.c").write_text(inttab)
    objects = []
    for c in (work / "strtab.c", work / "inttab.c", args.worktree / relative / "intarray.c",
              args.worktree / relative / "DtHash.c", here / "check-dtsmm-cache-bounds.c"):
        obj = work / (c.stem + ".o")
        build = subprocess.run(cc + flags + includes + ["-I", str(work), "-c", str(c), "-o", str(obj)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if build.returncode:
            sys.exit("cannot build " + c.name + ":\n" + "\n".join([l for l in build.stdout.splitlines() if "error" in l][:10]))
        objects.append(str(obj))
    binary = work / "cache-bounds-test"
    link = subprocess.run(cc + flags + objects + ["-o", str(binary)], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    if link.returncode:
        sys.exit("cannot link the cache test:\n" + link.stdout[-800:])
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:" + os.environ.get("ASAN_OPTIONS", ""), TMPDIR=str(work))
    try:
        run = subprocess.run([str(binary)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
        code, output = run.returncode, run.stdout
    except subprocess.TimeoutExpired:
        code, output = -1, "timed out"

lines = output.strip().splitlines()
keep = [l for l in lines if "ACCEPTED" in l or "ERROR:" in l or "SUMMARY:" in l or "runtime error" in l
        or "Assertion" in l or "accepted" in l or "timed out" in l]
if args.source_revision:
    if code == 0:
        sys.exit("NEGATIVE CONTROL FAILED: " + label + " refused every damaged cache")
    print("negative control (" + label + "): fails as expected, exit " + str(code))
    for l in keep[:6]:
        print("     " + l)
else:
    if code:
        print("\n".join(lines if os.environ.get("DTSMM_VERBOSE") else (keep or lines[-6:])[:12]))
        sys.exit("cache bounds check failed (" + label + "), exit " + str(code))
    print("\n".join(lines))
    print("Dts cache bounds: passed (" + label + ")")
