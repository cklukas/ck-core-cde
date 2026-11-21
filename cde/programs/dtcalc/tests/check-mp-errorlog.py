#!/usr/bin/env python3
"""Verify dtcalc mp.c mpset() under -O2/UBSan and MP debug logging levels."""
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
    dtcalc_dir = here.parent
    cde_dir = dtcalc_dir.parent.parent
    cc = shlex.split(os.environ.get("CC", "cc"))
    san = _ck_sanitize_list(cc)

    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "Dt").mkdir()
        (tmp_path / "Dt" / "Dt.h").write_text("/* stub for generated Dt/Dt.h */\n", encoding="utf-8")
        binary = tmp_path / "check-mp-errorlog"
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
                f"-I{dtcalc_dir}",
                f"-I{cde_dir / 'include'}",
                f"-I{tmp_path}",
                str(here / "check-mp-errorlog.c"),
                "-lm",
                "-o",
                str(binary),
            ]
        )
        subprocess.run(cmd, check=True)
        env = dict(
            os.environ,
            ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=0",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
        )
        res = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
        sys.stdout.write(res.stdout)
        sys.stderr.write(res.stderr)
        return res.returncode


if __name__ == "__main__":
    raise SystemExit(main())
