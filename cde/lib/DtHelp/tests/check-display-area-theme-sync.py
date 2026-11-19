#!/usr/bin/env python3
"""Verify DtHelp display-area live color/decor sync and child-resize handling."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def detect_sanitizers(cc_cmd: list[str], tmp_dir: str | None) -> str:
    override = os.environ.get("CK_SANITIZE_LIST", "").strip()
    if override:
        return override
    with tempfile.TemporaryDirectory(dir=tmp_dir) as d:
        src = Path(d) / "probe.c"
        exe = Path(d) / "probe"
        src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        for san in ("address,undefined", "undefined"):
            r = subprocess.run(
                cc_cmd + [f"-fsanitize={san}", str(src), "-o", str(exe)],
                capture_output=True,
            )
            if r.returncode == 0 and subprocess.run([str(exe)], capture_output=True).returncode == 0:
                return san
    return "undefined"


def extract_c_function(text: str, start_anchor: str) -> str:
    count = text.count(start_anchor)
    if count != 1:
        sys.exit(f"expected anchor {start_anchor!r} once, found {count}")
    start = text.index(start_anchor)
    brace = text.index("{", start)
    depth = 1
    idx = brace + 1
    while idx < len(text) and depth > 0:
        if text[idx] == "{":
            depth += 1
        elif text[idx] == "}":
            depth -= 1
        idx += 1
    return text[start:idx] + "\n\n"


def main() -> int:
    here = Path(__file__).resolve().parent
    callbacks_src = (here.parent / "Callbacks.c").read_text(encoding="utf-8")

    extracted = "".join(
        [
            extract_c_function(callbacks_src, "static Boolean\nSyncDisplayAreaVisuals("),
            extract_c_function(callbacks_src, "static void\nDrawWholeCanvas("),
            extract_c_function(callbacks_src, "void\n_DtHelpCleanAndDrawWholeCanvas("),
            extract_c_function(callbacks_src, "void\n_DtHelpExposeCB("),
            extract_c_function(callbacks_src, "void\n_DtHelpResizeCB("),
        ]
    )

    template = (here / "check-display-area-theme-sync.c").read_text(encoding="utf-8")
    body = template.split("/* @@PROLOGUE@@ */", 1)[1].replace("/* @@EXTRACTED@@ */", extracted)

    cc_cmd = shlex.split(os.environ.get("CC", "cc"))
    tmp_base = os.environ.get("TMPDIR")
    san = detect_sanitizers(cc_cmd, tmp_base)

    with tempfile.TemporaryDirectory(prefix="dthelp-theme-sync-", dir=tmp_base) as work_s:
        work = Path(work_s)
        c_file = work / "check_theme_sync.c"
        exe_file = work / "check_theme_sync"
        c_file.write_text(body, encoding="utf-8")
        subprocess.run(
            cc_cmd
            + [
                "-std=gnu23",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-fsanitize={san}",
                "-fno-sanitize-recover=all",
                "-g",
                "-O1",
                str(c_file),
                "-o",
                str(exe_file),
            ],
            check=True,
        )
        env = dict(os.environ)
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1")
        env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1")
        subprocess.run([str(exe_file)], check=True, env=env)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
