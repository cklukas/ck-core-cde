#!/usr/bin/env python3
"""Reject Clang function-cast warnings outside documented framework ABIs."""

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
XDRPROC_ADAPTER = re.compile(
    r"warning: cast from 'bool_t \(\*\)\(XDR \*, .*\)' .*"
    r"to 'xdrproc_t' .*\[-Wcast-function-type-strict\]$"
)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} BUILD-LOG", file=sys.stderr)
        return 2

    log_path = Path(sys.argv[1])
    rejected: list[str] = []
    accepted_inherit = 0
    accepted_xdr = 0

    for diagnostic in log_path.read_text(errors="replace").splitlines():
        match = WARNING.match(diagnostic)
        if match is None:
            continue

        # rpcgen deliberately emits typed serializers and passes them through
        # libtirpc's variadic xdrproc_t.  Accept only that precise bool_t/XDR
        # conversion, not arbitrary casts merely written as xdrproc_t.
        if XDRPROC_ADAPTER.search(diagnostic):
            accepted_xdr += 1
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
            accepted_inherit += 1
        else:
            rejected.append(diagnostic)

    if rejected:
        print("unapproved Clang function-cast diagnostics:", file=sys.stderr)
        print("\n".join(rejected), file=sys.stderr)
        return 1

    print(
        f"approved {accepted_inherit} Xt/Motif inherit-sentinel and "
        f"{accepted_xdr} rpcgen/libtirpc XDR diagnostics"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
