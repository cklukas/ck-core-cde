#!/usr/bin/env python3
"""Check that dtterm lowers its effective uid before it touches the environment.

dtterm may be installed setuid root.  Whatever main() does before the first
terminal widget is created runs with the installed identity, and the toolkit
reads resource files and search paths named by the caller's environment, so the
first thing main() has to do is return to the user's identity.  This is a source
guard: the behaviour itself needs a setuid binary and a privileged tracer (the
procedure is in docs/improvements/terminal.md).
"""
from pathlib import Path
import re
import sys

source = (Path(__file__).resolve().parents[1] / "DtTermMain.c").read_text()
match = re.search(r"^main\(int argc, char \*\*argv\)\n\{", source, re.M)
assert match, "main() not found"
body = source[match.end():]
body = body[:body.index("\n}\n")]

drop = body.find("_DtTermPrimToggleSuidRoot(False)")
assert drop >= 0, "main() never lowers the effective uid"
statements = ["getenv(", "atexit(", "XtSetLanguageProc(", "DtTermInitialize(", "XtAppInitialize(",
              "XtOpenApplication(", "XtOpenDisplay(", "XOpenDisplay(", "_DtEnvControl("]
early = [name for name in statements if 0 <= body.find(name) < drop]
if early:
    sys.exit("main() calls %s before it lowers the effective uid" % ", ".join(early))
print("dtterm lowers its effective uid before any toolkit or environment access")
