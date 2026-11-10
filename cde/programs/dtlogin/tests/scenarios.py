"""Helpers shared by the dtlogin scenario tests.

A scenario test compiles production code of dtlogin together with stand-ins
for what lies outside of it, under AddressSanitizer and UndefinedBehaviorSanitizer,
and runs every scenario in a process of its own: a crash or a sanitizer report in
one scenario fails that scenario only.  Set DTLOGIN_SOURCE to test another copy of
the sources.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTLOGIN_SOURCE") or Path(__file__).resolve().parents[1])


def read(name):
    """The text of a source file of dtlogin."""
    return (source / name).read_text()


def block(text, start, end="\n}\n"):
    """The text from START to the first END after it, END included."""
    first = text.index(start)
    return text[first:text.index(end, first) + len(end)]


def until(text, start, stop):
    """The text from START up to, not including, the first STOP after it."""
    first = text.index(start)
    return text[first:text.index(stop, first)]


def run_all(prefix, files, units, libs, scenarios, defines=()):
    """Compile the UNITS built from FILES and run every scenario; returns the number of failures.

    FILES maps file names to text, UNITS lists (name, extra flags) of the .c files among them.
    Names of the form "dir/file.c" would need the directory, so files stay flat; sources of
    dtlogin itself are found through -I and may be included by the units.
    """
    with tempfile.TemporaryDirectory(prefix=prefix, dir=os.environ.get("TMPDIR")) as work:
        work = Path(work)
        for name, text in files.items():
            (work / name).write_text(text)
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-std=c23", "-D_DEFAULT_SOURCE", "-DUNIXCONN", "-DTCPCONN", "-DXDMCP", "-DHASXDMAUTH",
                 "-DANSICPP", "-I", str(work), "-I", str(source), "-I", str(source.parents[1] / "include"),
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                 "-g", "-O1"] + list(defines)
        objects = []
        for unit, extra in units:
            obj = work / (unit + ".o")
            subprocess.run(cc + flags + extra + ["-c", str(work / (unit + ".c")), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = work / "scenarios"
        subprocess.run(cc + flags + objects + ["-o", str(binary)] + list(libs), check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0",
                   UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
        failed = 0
        for name in scenarios:
            scratch = work / name
            scratch.mkdir()
            try:
                done = subprocess.run([str(binary), name], cwd=scratch, env=env, capture_output=True,
                                      text=True, timeout=60)
            except subprocess.TimeoutExpired:
                failed += 1
                print("FAIL %s: did not finish within 60 seconds" % name)
                continue
            lines = (done.stdout + done.stderr).splitlines()
            reason = next((l for l in lines if l.startswith("FAIL") or "ERROR:" in l or "runtime error" in l),
                          lines[0] if lines else "exit status %d" % done.returncode)
            frame = next((l.strip() for l in lines
                          if re.match(r"\s*#[0-4] .* in \w+ .*(_unit|stubs|production)\.c", l)), "")
            if done.returncode:
                failed += 1
                print("FAIL %s: %s %s" % (name, reason.strip(), frame))
            else:
                print("ok   %s" % name)
        return failed


def finish(failed, what, summary):
    """Exit with a message when scenarios failed, else print the summary."""
    if failed:
        sys.exit("%s: %d scenario(s) failed" % (what, failed))
    print("%s: %s passed" % (what, summary))
