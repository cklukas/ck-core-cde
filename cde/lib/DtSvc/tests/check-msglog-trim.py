#!/usr/bin/env python3
"""Verify MsgLog.c automatic errorlog trimming, level filtering, and dedup."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def _ck_sanitize_list(cc: list[str]) -> str:
    override = os.environ.get("CK_SANITIZE_LIST", "").strip()
    if override:
        return override
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
        src = os.path.join(d, "p.c")
        exe = os.path.join(d, "p")
        Path(src).write_text("int main(void) { return 0; }\n", encoding="utf-8")
        for s in ("address,undefined", "undefined"):
            r = subprocess.run(cc + [f"-fsanitize={s}", src, "-o", exe], capture_output=True)
            if r.returncode == 0 and subprocess.run([exe], capture_output=True).returncode == 0:
                return s
    return "undefined"


def main() -> int:
    here = Path(__file__).resolve().parent
    dtsvc_dir = here.parent
    cde_dir = dtsvc_dir.parent.parent
    cc = shlex.split(os.environ.get("CC", "cc"))
    san = _ck_sanitize_list(cc)

    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as tmp:
        tmp_path = Path(tmp)
        binary = tmp_path / "check-msglog-trim"
        cmd = (
            cc
            + [
                "-std=gnu23",
                "-O2",
                "-g",
                "-Wall",
                "-Wno-unused-function",
                f"-fsanitize={san}",
                "-fno-sanitize-recover=all",
                "-DMULTIBYTE",
                "-DNLS16",
                f"-I{dtsvc_dir / 'include'}",
                f"-I{cde_dir / 'include'}",
                str(here / "check-msglog-trim.c"),
                "-o",
                str(binary),
            ]
        )
        subprocess.run(cmd, check=True)
        run_dir = tmp_path / "run"
        run_dir.mkdir()
        env = dict(
            os.environ,
            ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
        )
        res = subprocess.run([str(binary), str(run_dir)], env=env, capture_output=True, text=True)
        sys.stdout.write(res.stdout)
        sys.stderr.write(res.stderr)
        return res.returncode


if __name__ == "__main__":
    raise SystemExit(main())
