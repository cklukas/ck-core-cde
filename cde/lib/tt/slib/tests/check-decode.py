#!/usr/bin/env python3
"""Run the ttsession decoding and request tests under the sanitizers.

check-decode.py SRCDIR TT_SRCDIR

SRCDIR is lib/tt/slib of the source tree, TT_SRCDIR lib/tt.  Environment, as
`make check` supplies it (see lib/tt/slib/Makefile.am):
  TT_COMPILE  the compile command of libstt.a, flags included
  TT_LINK     the libtool link command of the build
  TT_LIBS     the libraries ttsession links, libstt.a and libtt.la first
  TT_CHECK_SOURCES  optional: a directory holding copies of lib/ and slib/
              of lib/tt; the tests then exercise those sources instead of
              the ones of the tree (this is how a test is shown to fail on
              the code it was written against)
  TT_CHECK_KEEP  optional: keep the test program, to run a case by hand
The production sources the tests exercise are compiled next to
decode-test.C and linked in front of the built libraries, so the code that
ships is the code that is tested (the definitions in the program take the
place of the same ones in libtt and libstt.a).  Each case runs in its own
process; any case that fails or crashes fails the run.  The sources are
compiled with the sanitizers when the build does not already have them.
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

# the sources under test: lib/ and slib/ of lib/tt, or a copy of them
override = os.environ.get("TT_CHECK_SOURCES")
root = Path(override).resolve() if override else tt_src
lib_src = root / "lib"
slib_src = root / "slib"

# The flags carry include and library paths relative to the build directory.
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="tt-decode-", dir=os.environ.get("TMPDIR")))

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


# The sources whose decoding, authentication and request handling the tests
# exercise.  Their objects stand in front of the ones in libtt and libstt.a.
sources = [
    lib_src / "util" / "tt_string.C",
    lib_src / "mp" / "mp_session.C",
    lib_src / "mp" / "mp_auth.C",
    lib_src / "mp" / "mp_auth_functions.C",
    lib_src / "mp" / "mp_xdr_functions.C",
    lib_src / "mp" / "mp_message.C",
    lib_src / "mp" / "mp_procid.C",
    lib_src / "mp" / "mp_msg_context.C",
    lib_src / "api" / "c" / "api_xdr.C",
    slib_src / "mp_rpc_implement.C",
    slib_src / "mp_s_xdr_functions.C",
    slib_src / "mp_otype.C",
]
if override:
    for s in sources:
        if not s.exists():
            fail("missing in TT_CHECK_SOURCES: %s" % s)
    print("sources under test: %s" % root)
    for s in sources:
        print("  %s %s" % (hashlib.sha256(s.read_bytes()).hexdigest()[:12], s.relative_to(root)))

# Headers of the tree under test come before the ones the build names.
includes = ["-I" + str(slib_src), "-I" + str(lib_src)]
cxx = compile_cmd[:1] + includes + compile_cmd[1:]

built = []
for source in [tests / "decode-test.C"] + sources:
    obj = work / (source.parent.name + "-" + source.stem + ".o")
    r = run(cxx + sanitize + ["-c", str(source), "-o", str(obj)])
    if r.returncode:
        fail("compile failed for %s:\n%s" % (source, r.stderr))
    built.append(str(obj))

binary = work / "decode-test"
# the tests stand in for /dev/urandom (see __wrap_open in decode-test.C)
r = run(link_cmd + sanitize + ["-Wl,--wrap=open", "-o", str(binary)] + built + libs)
if r.returncode:
    fail("link failed:\n" + r.stderr)

env = dict(os.environ)
# a failed allocation is an answer for the code, not the end of the program
env["ASAN_OPTIONS"] = ("detect_leaks=0:abort_on_error=0:"
                       "allocator_may_return_null=1:max_allocation_size_mb=512")
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
env["TT_DECODE_TEST_DIR"] = str(work)
cases = run([str(binary), "--list"], env=env).stdout.split()
if not cases:
    fail("the test program lists no cases")
failed = []
for case in cases:
    try:
        r = run([str(binary), case], env=env, timeout=30)
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
