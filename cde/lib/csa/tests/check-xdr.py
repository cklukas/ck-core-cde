#!/usr/bin/env python3
"""Run the XDR regression tests of the calendar protocols under the sanitizers.

check-xdr.py SRCDIR

Environment, as `make check` supplies it (see lib/csa/Makefile.am):
  CSA_COMPILE  the compile command of libcsa, flags included
  CSA_LINK     the libtool link command of the build
  CSA_LIBS     the libraries the test needs (the RPC library)
  CSA_XDR_DIR  where cmxdr.c, cmcbxdr.c, rtable2_xdr.c, rtable3_xdr.c,
               rtable4_xdr.c and cmxdrlist.c are taken from (default SRCDIR)
The XDR sources are compiled next to the test with the sanitizers (when the
build does not already have them) and the test program is run once for each
case with leak detection on; any case that fails or crashes fails the run.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

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


srcdir = Path(sys.argv[1]).resolve()
xdrdir = Path(os.environ.get("CSA_XDR_DIR", srcdir)).resolve()
tests = srcdir / "tests"
compile_cmd = shlex.split(os.environ["CSA_COMPILE"])
link_cmd = shlex.split(os.environ["CSA_LINK"])
libs = shlex.split(os.environ.get("CSA_LIBS", ""))
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="csa-xdr-", dir=os.environ.get("TMPDIR")))

sanitize = [f"-fsanitize={_CK_SANITIZE_LIST}", "-fno-omit-frame-pointer"]
if any(a.startswith("-fsanitize") for a in compile_cmd):
    sanitize = []
else:
    probe = work / "probe.c"
    probe.write_text("int main(void) { return 0; }\n")
    r = subprocess.run(compile_cmd + sanitize + ["-o", str(work / "probe"), str(probe)],
                       cwd=builddir, text=True, capture_output=True)
    if r.returncode or subprocess.run([str(work / "probe")], cwd=builddir, capture_output=True).returncode:
        print("the compiler has no sanitizers: checking results only")
        sanitize = []
    elif subprocess.run(compile_cmd + sanitize + ["-fno-sanitize=function", "-o", str(work / "probe"), str(probe)],
                        cwd=builddir, text=True, capture_output=True).returncode == 0:
        sanitize.append("-fno-sanitize=function")


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=builddir, text=True, capture_output=True, **kw)


def fail(message):
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(message)


names = ["cmxdr", "cmcbxdr", "rtable2_xdr", "rtable3_xdr", "rtable4_xdr",
         "cmxdrlist"]
sources = [tests / "xdr-test.c"]
for name in names:
    path = xdrdir / (name + ".c")
    if path.exists():
        sources.append(path)
    elif name != "cmxdrlist":
        fail("missing %s" % path)

objects = []
for source in sources:
    obj = work / (source.stem + ".o")
    # the headers are those of the library under test
    r = run(compile_cmd + sanitize + ["-I" + str(srcdir), "-I" + str(xdrdir),
                                      "-c", str(source), "-o", str(obj)])
    if r.returncode:
        fail("compile failed for %s:\n%s" % (source, r.stderr))
    objects.append(str(obj))

binary = work / "xdr-test"
r = run(link_cmd + sanitize + ["-o", str(binary)] + objects + libs)
if r.returncode:
    fail("link failed:\n" + r.stderr)

env = dict(os.environ)
# a length from a message must not make the decoder ask for gigabytes
env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=0:max_allocation_size_mb=512"
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
cases = run([str(binary), "--list"], env=env).stdout.split()
failed = []
for case in cases:
    r = run([str(binary), case], env=env, timeout=300)
    status = "ok" if r.returncode == 0 else "FAIL"
    print(f"{status:5} {case}")
    if r.returncode:
        failed.append(case)
        print("\n".join("      " + line for line in (r.stderr or "").splitlines()[:10]))
print(f"{len(cases) - len(failed)}/{len(cases)} cases passed")
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed else 0)
