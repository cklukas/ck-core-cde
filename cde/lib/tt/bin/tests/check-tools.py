#!/usr/bin/env python3
"""Run the argument handling tests of the ToolTalk command line tools.

check-tools.py SHELL_DIR TYPE_COMP_DIR TTSESSION TTTAR

SHELL_DIR holds the built ttrm, ttmv and ttcp, TYPE_COMP_DIR the built
tt_type_comp (lib/tt/bin/shell and lib/tt/bin/tt_type_comp of the build tree),
TTSESSION and TTTAR the built ttsession and tttar.
The tools run the commands they stand for (rm, mv, cp and the C preprocessor)
themselves.  The tests give them file names and preprocessor options that hold
blanks and characters a shell treats specially, in a directory of their own, and
check that the tools act on exactly those names and that nothing else is run.
ttrm and ttmv are given -f and ttcp does the copy before it contacts ToolTalk,
so they need no session; tttar runs against a process-tree ttsession of its
own, started here, when the portmapper is there to register it with.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

shell_dir = Path(sys.argv[1]).resolve()
type_comp_dir = Path(sys.argv[2]).resolve()
ttsession = Path(sys.argv[3]).resolve()
tttar = Path(sys.argv[4]).resolve()
work = Path(tempfile.mkdtemp(prefix="tt-tools-", dir=os.environ.get("TMPDIR")))

env = dict(os.environ)
env.pop("DISPLAY", None)            # no session to start
env.pop("TT_SESSION", None)
env["HOME"] = str(work)

failures = []
count = 0


def case(name):
    global count
    count += 1
    d = work / ("case%d" % count)
    d.mkdir()
    return name, d


def tool(path, args, d):
    try:
        return subprocess.run([str(path)] + args, cwd=d, env=env, text=True,
                              stdin=subprocess.DEVNULL, capture_output=True,
                              timeout=30)
    except subprocess.TimeoutExpired:
        # a tool that waits for input it was never meant to read
        return subprocess.CompletedProcess(args, -1, "", "timed out")


def check(name, ok, detail=""):
    print("%-5s %s" % ("ok" if ok else "FAIL", name))
    if not ok:
        failures.append(name)
        for line in detail.splitlines()[:8]:
            print("      " + line)


def names(d):
    return sorted(p.name for p in d.iterdir())


# ---- ttrm ------------------------------------------------------------------
name, d = case("ttrm_names")
for n in ("a b", "x;touch MARK", "$(touch MARK)", "*", "keep1", "keep2"):
    (d / n).write_text("x\n")
r = tool(shell_dir / "ttrm", ["-f", "a b", "x;touch MARK", "$(touch MARK)", "*"], d)
check("ttrm removes the files it is given, whatever their names",
      names(d) == ["keep1", "keep2"], "left: %s\n%s" % (names(d), r.stderr))
check("ttrm runs nothing else", not (d / "MARK").exists())

# ---- ttmv ------------------------------------------------------------------
name, d = case("ttmv_names")
(d / "from one").write_text("1\n")
(d / "from;two").write_text("2\n")
r = tool(shell_dir / "ttmv", ["-f", "from one", "to one;touch MARK"], d)
tool(shell_dir / "ttmv", ["-f", "from;two", "$(touch MARK)"], d)
check("ttmv moves a file to the name it is given",
      names(d) == ["$(touch MARK)", "to one;touch MARK"],
      "found: %s\n%s" % (names(d), r.stderr))
check("ttmv runs nothing else", not (d / "MARK").exists())

# ---- ttcp ------------------------------------------------------------------
name, d = case("ttcp_names")
(d / "source file").write_text("copy me\n")
tool(shell_dir / "ttcp", ["source file", "target;touch MARK"], d)
tool(shell_dir / "ttcp", ["source file", "$(touch MARK)"], d)
check("ttcp copies a file to the name it is given",
      names(d) == ["$(touch MARK)", "source file", "target;touch MARK"]
      and (d / "target;touch MARK").read_text() == "copy me\n",
      "found: %s" % names(d))
check("ttcp runs nothing else", not (d / "MARK").exists())

# ---- tt_type_comp ----------------------------------------------------------
TYPES = """#ifdef FOO
ptype Foo_Ptype {
    start "foo";
    handle:
    session Mail( in ALL contents ) => start opnum = 1 ;
};
#endif
"""
type_comp = type_comp_dir / "tt_type_comp"

name, d = case("type_comp_file_name")
src = d / "t;>MARK;.types"
src.write_text(TYPES)
r = tool(type_comp, ["-s", "-x", "-o", "out.xdr", "-DFOO", src.name], d)
check("tt_type_comp compiles a source file whose name has shell characters",
      r.returncode == 0 and (d / "out.xdr").exists(), r.stderr)
check("tt_type_comp runs nothing else for a file name", not (d / "MARK").exists())
r = tool(type_comp, ["-s", "-P", "out.xdr"], d)
check("the compiled file holds the type of the conditional part",
      "Foo_Ptype" in r.stdout, r.stdout + r.stderr)

name, d = case("type_comp_options")
(d / "types.types").write_text(TYPES)
tool(type_comp, ["-s", "-x", "-o", "out.xdr", "-DFOO;touch MARK", "types.types"], d)
tool(type_comp, ["-s", "-x", "-o", "out2.xdr", "-DFOO", "-I/x;touch MARK", "types.types"], d)
check("tt_type_comp runs nothing else for a preprocessor option",
      not (d / "MARK").exists(), "found: %s" % names(d))

name, d = case("type_comp_long_arguments")
(d / "types.types").write_text(TYPES)
options = ["-DFOO"] + ["-DLONG_OPTION_%d=%s" % (i, "v" * 100) for i in range(60)]
r = tool(type_comp, ["-s", "-x", "-o", "out.xdr"] + options + ["types.types"], d)
check("tt_type_comp passes more options than a fixed command line holds",
      r.returncode == 0 and (d / "out.xdr").exists(), r.stderr)

# ---- tttar -----------------------------------------------------------------
def session_started():
    """A process-tree ttsession for tttar; returns (process, session id) or None."""
    if subprocess.run(["rpcinfo", "-p"], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode != 0:
        return None                 # nothing to register the session with
    out = work / "ttsession.out"
    proc = subprocess.Popen([str(ttsession), "-S", "-p"], stdout=out.open("w"),
                            stderr=subprocess.STDOUT, env=env, cwd=work)
    for _ in range(50):
        sid = out.read_text().splitlines()[:1]
        if sid and len(sid[0].split()) >= 8:
            return proc, sid[0]
        if proc.poll() is not None:
            break
        subprocess.run(["sleep", "0.1"])
    proc.kill()
    return None


started = session_started()
if started is None:
    print("skip  tttar: no ttsession could be started")
else:
    proc, sid = started
    name, d = case("tttar_names")
    tenv = dict(env, TT_SESSION=sid)
    (d / "a b").write_text("1\n")
    (d / "x;touch MARK").write_text("2\n")
    (d / "$(touch MARK)").write_text("3\n")
    members = ["a b", "x;touch MARK", "$(touch MARK)"]
    subprocess.run([str(tttar), "cf", "out.tar"] + members, cwd=d, env=tenv,
                   stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
    r = subprocess.run([str(tttar), "tf", "out.tar"], cwd=d, env=tenv,
                       stdin=subprocess.DEVNULL, text=True, capture_output=True,
                       timeout=30)
    check("tttar archives the files it is given, whatever their names",
          all(m in r.stdout.splitlines() for m in members), r.stdout + r.stderr)
    (d / "ex").mkdir()
    subprocess.run([str(tttar), "xf", "../out.tar"], cwd=d / "ex", env=tenv,
                   stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
    check("tttar extracts them", names(d / "ex") == sorted(members) + ["tttarfile"]
          or names(d / "ex") == sorted(members), "found: %s" % names(d / "ex"))
    check("tttar runs nothing else", not (d / "MARK").exists())
    # the session leaves its registration behind when it is stopped
    proc.terminate()
    proc.wait()
    prog = sid.split()[2]
    for vers in ("1", "4"):
        subprocess.run(["rpcinfo", "-d", prog, vers], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)

shutil.rmtree(work, ignore_errors=True)
print("%d checks failed" % len(failures) if failures else "all checks passed")
sys.exit(1 if failures else 0)
