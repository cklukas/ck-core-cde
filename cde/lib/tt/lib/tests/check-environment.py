#!/usr/bin/env python3
"""Run the libtt environment and program-start tests under the sanitizers.

check-environment.py SRCDIR TT_SRCDIR

SRCDIR is lib/tt/lib of the source tree, TT_SRCDIR lib/tt.  Environment, as
`make check` supplies it (see lib/tt/lib/Makefile.am):
  TT_COMPILE  the compile command of libtt, flags included
  TT_LINK     the libtool link command of the build
  TT_LIBS     the libraries the test links: libtt.la first
  TT_CHECK_SOURCES  optional: a directory holding a copy of lib/ of lib/tt;
              the tests then exercise those sources instead of the ones of
              the tree (this is how a test is shown to fail on the code it
              was written against)
  TT_CHECK_KEEP  optional: keep the test program, to run a case by hand
  TT_ENV_TEST_REAL_UID, TT_ENV_TEST_REAL_GID  optional, with the superuser:
              the ids the set-user-ID case continues with as its real ids
The production sources the tests exercise are compiled next to
environment-test.C and linked in front of the built libtt, so the code that
ships is the code that is tested.  Each case runs in its own process; any
case that fails or crashes fails the run.  The sources are compiled with the
sanitizers when the build does not already have them.
"""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

srcdir = Path(sys.argv[1]).resolve()
tt_src = Path(sys.argv[2]).resolve()
tests = srcdir / "tests"
compile_cmd = shlex.split(os.environ["TT_COMPILE"])
link_cmd = shlex.split(os.environ["TT_LINK"])
libs = shlex.split(os.environ.get("TT_LIBS", ""))

override = os.environ.get("TT_CHECK_SOURCES")
root = Path(override).resolve() if override else tt_src
lib_src = root / "lib"

# The flags carry include and library paths relative to the build directory.
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="tt-environment-", dir=os.environ.get("TMPDIR")))

sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
if any(a.startswith("-fsanitize") for a in compile_cmd):
    sanitize = []                    # the build already carries them
else:
    probe = work / "probe.cc"
    probe.write_text("int main() { return 0; }\n")
    r = subprocess.run(compile_cmd + sanitize + ["-o", str(work / "probe"), str(probe)],
                       cwd=builddir, text=True, capture_output=True)
    if r.returncode:
        print("the compiler has no sanitizers: checking results only")
        sanitize = []


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=builddir, text=True, capture_output=True, **kw)


def fail(message):
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(message)


# The sources whose use of the environment and of other programs the tests
# exercise.  Their objects stand in front of the ones in libtt.
sources = [
    lib_src / "util" / "tt_secure_env.C",
    lib_src / "util" / "tt_spawn.C",
    lib_src / "util" / "tt_trace.C",
    lib_src / "util" / "tt_path.C",
    lib_src / "util" / "tt_ldpath.C",
    lib_src / "mp" / "mp_auth_functions.C",
    lib_src / "mp" / "mp_c_session.C",
]
if override:
    for s in sources:
        if not s.exists():
            fail("missing in TT_CHECK_SOURCES: %s" % s)
    print("sources under test: %s" % root)
    for s in sources:
        print("  %s %s" % (hashlib.sha256(s.read_bytes()).hexdigest()[:12], s.relative_to(root)))

# Headers of the tree under test come before the ones the build names.
cxx = compile_cmd[:1] + ["-I" + str(lib_src)] + compile_cmd[1:]

built = []
for source in [tests / "environment-test.C"] + sources:
    obj = work / (source.parent.name + "-" + source.stem + ".o")
    r = run(cxx + sanitize + ["-c", str(source), "-o", str(obj)])
    if r.returncode:
        fail("compile failed for %s:\n%s" % (source, r.stderr))
    built.append(str(obj))

binary = work / "environment-test"
# a process with other effective than real ids is simulated by the test
r = run(link_cmd + sanitize + ["-Wl,--wrap=geteuid", "-Wl,--wrap=getegid",
                               "-o", str(binary)] + built + libs)
if r.returncode:
    fail("link failed:\n" + r.stderr)

env = dict(os.environ)
env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0"
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
env["TT_ENV_TEST_DIR"] = str(work)
cases = run([str(binary), "--list"], env=env).stdout.split()
if not cases:
    fail("the test program lists no cases")
failed = []
for case in cases:
    try:
        r = run([str(binary), case], env=env, timeout=60)
        code, err = r.returncode, r.stderr or ""
    except subprocess.TimeoutExpired:
        code, err = -1, "timed out"
    status = "ok" if code == 0 else "FAIL"
    print(f"{status:5} {case}")
    if code:
        failed.append(case)
        print("\n".join("      " + line for line in err.splitlines()[:12]))
print(f"{len(cases) - len(failed)}/{len(cases)} cases passed")
if os.environ.get("TT_CHECK_KEEP"):
    print("test program kept in %s" % work)
else:
    shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed else 0)
