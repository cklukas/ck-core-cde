#!/usr/bin/env python3
"""Count compiler warnings per component and class, and compare to a baseline.

Record a baseline from a verbose `make` log of a `--enable-hardening` build:

    warning-baseline.py record LOG --depth 2 --strip SRCDIR --strip BUILDDIR \
        --label NAME > baseline.json

Fail when a later build reports more warnings than the baseline in any
component and class (the baseline is a ratchet; it may only fall):

    warning-baseline.py compare LOG BASELINE --depth 2 --strip SRCDIR ...

List the individual warnings of chosen classes with resolved paths, for
triage:

    warning-baseline.py list LOG --class -Wuse-after-free --class -Warray-bounds \
        --strip SRCDIR ...

Paths are resolved against the `Entering directory` lines of the make log and
the first matching --strip prefix is removed.  The leading --depth path
components name the component.  A warning is unique by (file, line, column,
class), so a header included by many translation units counts once.
"""
import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict

# Classes that point at memory-safety, lifetime or error-handling defects,
# as opposed to style or signedness noise.
DEFECT_CLASSES = [
    "-Wnull-dereference", "-Wuninitialized", "-Wmaybe-uninitialized",
    "-Wformat-security", "-Wformat-overflow", "-Wformat-truncation",
    "-Wstringop-overflow", "-Wstringop-truncation", "-Wstringop-overread",
    "-Warray-bounds", "-Wuse-after-free", "-Wdangling-pointer", "-Wrestrict",
    "-Wunused-result", "-Wreturn-type", "-Wimplicit-function-declaration",
    "-Wincompatible-pointer-types", "-Wint-conversion", "-Wpointer-to-int-cast",
    "-Wint-to-pointer-cast", "-Wsizeof-pointer-memaccess", "-Wfree-nonheap-object",
    "-Wmismatched-dealloc", "-Walloc-size-larger-than", "-Wmisleading-indentation",
    "-Wimplicit-fallthrough", "-Wcast-function-type", "-Waddress",
]

ENTER = re.compile(r"make(?:\[\d+\])?: (Entering|Leaving) directory '([^']+)'")
WARN = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): warning: "
                  r"(?P<msg>.*?)(?: \[(?P<cls>-W[^\]=]+=?[^\]]*)\])?$")


def count(log, depth, strips):
    stack, seen, details = [], set(), []
    by_comp, total = defaultdict(Counter), Counter()
    for line in open(log, errors="replace"):
        m = ENTER.search(line)
        if m:
            if m.group(1) == "Entering":
                stack.append(m.group(2))
            elif stack:
                stack.pop()
            continue
        w = WARN.match(line.rstrip("\n"))
        if not w:
            continue
        path = os.path.normpath(os.path.join(stack[-1] if stack else "", w["file"]))
        for p in strips:
            p = os.path.normpath(p)
            if path.startswith(p + os.sep):
                path = os.path.relpath(path, p)
                break
        # GCC prints parameterized classes such as -Wimplicit-fallthrough=.
        cls = (w["cls"] or "(unclassified)").rstrip("=")
        key = (path, w["line"], w["col"], cls)
        if key in seen:
            continue
        seen.add(key)
        comp = "/".join(path.split("/")[:depth])
        by_comp[comp][cls] += 1
        total[cls] += 1
        details.append((path, int(w["line"]), cls, w["msg"]))
    return by_comp, total, details


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["record", "compare", "list"])
    ap.add_argument("log")
    ap.add_argument("baseline", nargs="?")
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--strip", action="append", default=[])
    ap.add_argument("--label", default="")
    ap.add_argument("--class", dest="classes", action="append", default=[],
                    help="warning class for list mode, written --class=-Wnull-dereference")
    args = ap.parse_args()
    by_comp, total, details = count(args.log, args.depth, args.strip)

    if args.mode == "list":
        wanted = set(args.classes) or set(DEFECT_CLASSES)
        for path, line, cls, msg in sorted(details):
            if cls in wanted:
                print(f"{path}:{line}: [{cls}] {msg}")
        return 0

    if args.mode == "record":
        defect = {c: total[c] for c in DEFECT_CLASSES if total[c]}
        json.dump({
            "label": args.label,
            "unique_warnings": sum(total.values()),
            "defect_class_warnings": sum(defect.values()),
            "defect_classes": dict(sorted(defect.items(), key=lambda kv: -kv[1])),
            "totals": dict(total.most_common()),
            "components": {c: dict(v.most_common())
                           for c, v in sorted(by_comp.items(), key=lambda kv: -sum(kv[1].values()))},
        }, sys.stdout, indent=1)
        print()
        return 0

    base = json.load(open(args.baseline))["components"]
    worse = []
    for comp, classes in by_comp.items():
        for cls, n in classes.items():
            old = base.get(comp, {}).get(cls, 0)
            if n > old:
                worse.append((comp, cls, old, n))
    for comp, cls, old, n in sorted(worse):
        print(f"regression: {comp} {cls}: {old} -> {n}")
    print(f"{len(worse)} regression(s)")
    return 1 if worse else 0


if __name__ == "__main__":
    sys.exit(main())
