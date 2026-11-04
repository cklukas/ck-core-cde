#!/usr/bin/env python3
"""Guard against debug-only code in dtsession that writes to a fixed name in /tmp or never returns.

The Xrm subtraction code had a DEBUG trace that appended the resource database
of the user to /tmp/dtsession.xrm: a fixed name, opened in append mode without
O_NOFOLLOW, in a directory every user can write to.  main() had a DEBUG busy loop for
attaching a debugger that spun for ever when it was started with more than five
arguments.  Neither belongs in a session manager; this test fails if a
dtsession source names a file in /tmp or spins in main(), and checks that the
file that held the trace still compiles with DEBUG defined.

DTSESSION_SOURCE names another copy of the sources to test, for example an
older revision.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
problems = []
for path in sorted(source.glob("*.c")) + sorted(source.glob("*.h")):
    text = path.read_text(errors="replace")
    stripped = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    for number, line in enumerate(stripped.split("\n"), 1):
        if re.search(r'"/tmp[/"]|"/var/tmp', line):
            problems.append("%s names a file in /tmp: %s" % (path.name, line.strip()))
        if re.search(r"while\s*\(\s*junk\s*\)", line):
            problems.append("%s spins in a debug busy loop" % path.name)
if problems:
    sys.exit("\n".join(problems))

cc = shlex.split(os.environ.get("CC", "gcc"))
with tempfile.TemporaryDirectory(prefix="dtsession-debug-", dir=os.environ.get("TMPDIR")) as work:
    for define in ([], ["-DDEBUG"]):
        built = subprocess.run(cc + ["-std=gnu23", "-fsyntax-only", "-w"] + define + [str(source / "SmXrm.c")],
                               capture_output=True, text=True)
        if built.returncode:
            sys.stderr.write(built.stderr[:3000])
            sys.exit("SmXrm.c does not compile%s" % (" with DEBUG" if define else ""))
print("Debug code: no fixed names in /tmp, no busy loops; SmXrm.c compiles with and without DEBUG")
