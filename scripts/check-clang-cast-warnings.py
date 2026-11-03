#!/usr/bin/env python3
"""Reject Clang function-cast warnings except Xt/Motif inherit sentinels."""

from pathlib import Path
import re
import sys


WARNING = re.compile(
    r"^(?P<path>.*?):(?P<line>[0-9]+):[0-9]+: warning: .*"
    r"\[-Wcast-function-type-strict\]$"
)
INHERIT_SENTINEL = re.compile(
    r"\b(?:(?:Xt|Xm|Dt)Inherit[A-Za-z_]+|_XtInherit)\b"
)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} BUILD-LOG", file=sys.stderr)
        return 2

    log_path = Path(sys.argv[1])
    rejected: list[str] = []
    accepted = 0

    for diagnostic in log_path.read_text(errors="replace").splitlines():
        match = WARNING.match(diagnostic)
        if match is None:
            continue

        source_path = Path(match.group("path"))
        try:
            # Compiler line numbers count newline characters, while
            # str.splitlines() also treats form feeds in old C sources as
            # separators.
            source_line = source_path.read_text(errors="replace").split("\n")[
                int(match.group("line")) - 1
            ]
        except (OSError, IndexError):
            rejected.append(diagnostic)
            continue

        if INHERIT_SENTINEL.search(source_line):
            accepted += 1
        else:
            rejected.append(diagnostic)

    if rejected:
        print("unapproved Clang function-cast diagnostics:", file=sys.stderr)
        print("\n".join(rejected), file=sys.stderr)
        return 1

    print(f"approved {accepted} Xt/Motif inherit-sentinel diagnostics")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
