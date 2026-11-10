#!/usr/bin/env python3
"""Run the data-type attribute buffer code of Dts.c under AddressSanitizer.

The production functions are cut out of DtUtil1/Dts.c, wrapped with stubs for
the shared-memory database and with allocation failure injection, and compiled
with the tests/dts-buffers.c harness.  Every scenario runs in its own process
so that one sanitizer report cannot hide another.

DTS_SOURCE names another copy of Dts.c to test, e.g. an older revision.
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
dts_c = Path(os.environ.get("DTS_SOURCE", source / "DtUtil1" / "Dts.c"))
text = dts_c.read_text()
dts_h = (source.parents[1] / "include" / "Dt" / "Dts.h").read_text()
mm_h = (source.parents[1] / "include" / "Dt" / "DtsMM.h").read_text()


def skip_literal(text, i):
    """Return the index after the string, character literal or comment at i."""
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
    """Return the definition of NAME, with the type line above its name."""
    for match in re.finditer(r"^" + re.escape(name) + r"\s*\(", text, re.M):
        i, depth = match.end(), 1
        while depth:
            if text[i] in "\"'" or text.startswith(("/*", "//"), i):
                i = skip_literal(text, i)
                continue
            depth += (text[i] == "(") - (text[i] == ")")
            i += 1
        if not re.match(r"\s*\{", text[i:]):
            continue                      # a call or a prototype
        start = text.rfind("\n", 0, match.start() - 1) + 1
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
        raise AssertionError(name + " not found in " + str(dts_c))
    return ""


def line(pattern, where=text, required=True):
    match = re.search(pattern, where, re.M)
    assert match or not required, pattern
    return match.group(0) + "\n" if match else ""


types = ""
# Database field and record types, and the names the code matches against.
types += "".join(re.findall(r"^typedef\s+int\s+\w+;.*\n", mm_h, re.M))
for name in ("DtDtsMMField", "DtDtsMMRecord", "DtDtsMMDatabase"):
    types += re.search(r"typedef\s+struct\s*\{[^{}]*\}\s*" + name + r";", mm_h).group(0) + "\n"
types += re.search(r"typedef\s+struct\s+_DtDtsAttribute\s*\{[^{}]*\}\s*DtDtsAttribute;", dts_h).group(0) + "\n"
types += "".join(sorted(set(re.findall(r'^#define\s+DtDTS_\w+\s+"[^"\n]*"[ \t]*$\n', dts_h, re.M))))
# Local definitions of Dts.c.
for pattern in (r"^#define\s+MB_SIZE\s+\d+\s*$", r"^#define\s+DTS_MAX_LINKS\s+\d+\s*$",
                r"^#define\s+DTS_MAX_COMMAND_OUTPUT\s+\d+\s*$", r"^#define\s+_MBLEN\(.*$", r"^#define\s+_MBADV\(.*$",
                r"^#define\s+DIR_INFO\s+\".*\"\s*$"):
    # the limits of the links and of command output are not in older sources
    types += line(pattern, required="DTS_MAX" not in pattern)
types += re.search(r"typedef\s+struct\s+type_info\s*\{.*?\}\s*type_info_t;", text, re.S).group(0) + "\n"
types += "".join(re.findall(r"^static\s+DtShmBoson\s+dtdts_\w+\s*=\s*0;\s*$\n", text, re.M))

head = "static int csh_match_star(const char *, const char *);\n"
head += "void DtDtsFreeAttributeList(DtDtsAttribute **);\nvoid DtDtsFreeDataTypeNames(char **);\n"
# The probes of the file system are stubs in the first build and the code of Dts.c in the second.
real_probes = ("get_fd", "get_lstat", "get_stat", "read_file", "get_buff", "get_link_info",
               "get_link_path", "get_link_name")


# helpers that older sources do not have
OPTIONAL = ("mb_char", "read_file", "add_text", "expand_keywords")


def functions_of(real):
    out = head
    names = ["mb_char", "gmatch", "csh_match_star", "max_buf", "get_file_path", "get_name", "get_opt_name"]
    if real:
        names += real_probes
    names += ["get_buff_size", "cleanup", "set_vals", "type_content"]
    for name in names:
        out += function(name, required=name not in OPTIONAL)
    # The helper that finds the type word of a directory file; older sources read it inline.
    out += function("dir_datatype_extent", required=False)
    for name in ("_DtDtsGetDataType", "type_name", "type_path", "next_sep", "DtDtsDataToDataType",
                 "add_text", "expand_keywords", "expand_keyword", "append", "expand_shell", "expand_value",
                 "DtDtsDataTypeToAttributeList", "DtDtsFreeAttributeList", "DtDtsDataTypeNames",
                 "DtDtsFindAttribute", "DtDtsFreeDataTypeNames", "DtDtsSetDataType"):
        out += function(name, required=name not in OPTIONAL)
    return out


stubbed = ("max_buf", "data_to_data_type", "type_content", "directory_type",
           "expand_keyword", "expand_shell", "attribute_lists", "set_vals",
           "shell_text", "value_injection", "invalid_characters")
real = ("links", "contents", "buffer_sizes")
builds = (("dts-buffers", False, stubbed), ("dts-files", True, real))
cc = shlex.split(os.environ.get("CC", "cc"))
failed = []
scenarios = stubbed + real
with tempfile.TemporaryDirectory(prefix="dts-buffers-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    (work / "dts-types.h").write_text(types)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:" + os.environ.get("ASAN_OPTIONS", ""))
    arena = work / "arena"
    arena.mkdir()
    for build, is_real, names in builds:
        (work / "dts-functions.h").write_text(functions_of(is_real))
        binary = work / build
        subprocess.run(cc + ["-std=c99", "-D_DEFAULT_SOURCE", "-UNDEBUG", "-g", "-O1", "-fno-omit-frame-pointer",
                             "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
                       + (["-DREAL_PROBES"] if is_real else [])
                       + (["-DOLD_EXPAND_SHELL"] if "expand_keywords" not in text else []) +
                       ["-Wno-parentheses", "-Werror=implicit-function-declaration",
                        "-Werror=incompatible-pointer-types",
                        "-I", str(work), str(here / "dts-buffers.c"), "-o", str(binary)], check=True)
        for scenario in names:
            run = subprocess.run([str(binary), scenario, str(arena)], env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if run.returncode == 0:
                print("ok   " + scenario)
                continue
            failed.append(scenario)
            report = run.stdout.strip().splitlines()
            keep = [l for l in report if "ERROR:" in l or "SUMMARY:" in l or "runtime error" in l
                    or "Assertion" in l or "check failed" in l]
            print("FAIL " + scenario + " (exit " + str(run.returncode) + ")")
            for l in (keep or report[-6:])[:6]:
                print("     " + l)
if failed:
    sys.exit("Dts.c buffer checks failed: " + ", ".join(failed))
print("Dts.c data-type buffers: " + str(len(scenarios)) + " scenarios passed")
