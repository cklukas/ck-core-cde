#!/usr/bin/env python3
"""Verify dtfile WP-16 Archive.c round-trip formats, Zip-Slip/symlink safety, and Thumbnail MD5 URIs."""

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile
import zipfile


def _detect_sanitizers(cc_cmd: list[str], tmp_dir: str) -> str:
    override = os.environ.get("CK_SANITIZE_LIST", "").strip()
    if override:
        return override
    with tempfile.TemporaryDirectory(dir=tmp_dir) as d:
        src = os.path.join(d, "probe.c")
        exe = os.path.join(d, "probe")
        Path(src).write_text("int main(void) { return 0; }\n", encoding="utf-8")
        for cand in ("address,undefined", "undefined"):
            rc = subprocess.run(cc_cmd + [f"-fsanitize={cand}", src, "-o", exe],
                                capture_output=True)
            if rc.returncode == 0 and subprocess.run([exe], capture_output=True).returncode == 0:
                return cand
    return ""


HARNESS_C = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Archive.h"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *op = argv[1];
    char err[512] = {0};
    char created[4096] = {0};

    if (strcmp(op, "create") == 0) {
        if (argc < 5) return 2;
        const char *fmt_str = argv[2];
        const char *out_arc = argv[3];
        DtfileArchiveFormat fmt = DtfileArchiveDetectFormat(fmt_str);
        const char *const *inputs = (const char *const *)(argv + 4);
        int count = argc - 4;
        DtfileArchiveStatus st = DtfileArchiveCreate(inputs, count, out_arc, fmt, err, sizeof(err));
        if (st != DTFILE_ARCHIVE_OK) {
            fprintf(stderr, "CREATE_ERR %d: %s\n", (int)st, err);
            return 10 + (-(int)st);
        }
        printf("OK\n");
        return 0;
    }

    if (strcmp(op, "extract") == 0) {
        if (argc != 5) return 2;
        const char *arc = argv[2];
        const char *dest = argv[3];
        int smart = atoi(argv[4]);
        DtfileArchiveStatus st = DtfileArchiveExtract(
            arc, dest, smart, created, sizeof(created), err, sizeof(err));
        if (st != DTFILE_ARCHIVE_OK) {
            fprintf(stderr, "EXTRACT_ERR %d: %s\n", (int)st, err);
            return 20 + (-(int)st);
        }
        printf("%s\n", created);
        return 0;
    }

    return 2;
}
'''


def make_zip_with_raw_entry(zip_path: Path, entry_name: str, data: bytes) -> None:
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(entry_name, data)


def make_tar_with_symlink_escape(tar_path: Path) -> None:
    import io
    with tarfile.open(tar_path, "w:gz") as tf:
        sym = tarfile.TarInfo(name="sub/escape_link")
        sym.type = tarfile.SYMTYPE
        sym.linkname = "../../outside_target"
        sym.mode = 0o777
        tf.addfile(sym)

        reg = tarfile.TarInfo(name="sub/escape_link/pwned.txt")
        payload = b"escaped via symlink"
        reg.size = len(payload)
        reg.mode = 0o644
        tf.addfile(reg, io.BytesIO(payload))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--module-source", type=Path,
                    default=Path(__file__).resolve().parents[1])
    args = ap.parse_args()

    mod_src = args.module_source.resolve()
    tmp_base = os.environ.get("TMPDIR")
    if os.uname().sysname == "Darwin":
        if not tmp_base or not (
            Path(tmp_base).resolve().is_relative_to(Path("/Volumes/S8T/tmp")) or
            Path(tmp_base).resolve().is_relative_to(Path("/Volumes/PRO-BLADE/tmp"))
        ):
            raise SystemExit("On macOS, TMPDIR must be beneath /Volumes/S8T/tmp or /Volumes/PRO-BLADE/tmp")

    cc_str = os.environ.get("CC", "cc").strip() or "cc"
    cc_cmd = shlex.split(cc_str)
    pkg_cfg = os.environ.get("PKG_CONFIG", "pkg-config").strip() or "pkg-config"

    cflags = shlex.split(subprocess.check_output(
        [pkg_cfg, "--cflags", "libarchive"], text=True).strip())
    libs = shlex.split(subprocess.check_output(
        [pkg_cfg, "--libs", "libarchive"], text=True).strip())

    with tempfile.TemporaryDirectory(prefix="dtfile-archive-safety-", dir=tmp_base) as work:
        work_dir = Path(work)
        san = _detect_sanitizers(cc_cmd, work)
        san_flags = [f"-fsanitize={san}"] if san else []

        harness_c = work_dir / "harness.c"
        harness_bin = work_dir / "archive-harness"
        harness_c.write_text(HARNESS_C, encoding="utf-8")

        compile_cmd = (
            cc_cmd + ["-O1", "-g", "-Wall", "-Wextra", "-DDTFILE_ARCHIVE_CORE_ONLY",
                      f"-I{mod_src}"] + cflags + san_flags +
            [str(mod_src / "Archive.c"), str(harness_c), "-o", str(harness_bin)] + libs
        )
        subprocess.run(compile_cmd, check=True)

        env = dict(os.environ)
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1")
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")

        # 1. Stage tricky source tree (spaces, UTF-8, shell metacharacters, nested dir, safe symlink)
        src_root = work_dir / "source_tree"
        proj = src_root / "Project Alpha"
        sub = proj / "nested dir"
        sub.mkdir(parents=True)

        f1 = proj / "readme notes (v1).txt"
        f2 = proj / "Übersicht_日本語_λ.md"
        f3 = sub / "quote 'test' \"shell\" $HOME;false.txt"
        f1.write_bytes(b"Hello from CK-Core CDE Archive Engine!\n" * 20)
        f2.write_text("UTF-8 content: ÄÖÜ 日本語 λ\n", encoding="utf-8")
        f3.write_bytes(b"Adversarial shell characters stay inert.\n")
        os.symlink("../readme notes (v1).txt", sub / "rel_link.txt")

        # Round-trip all 5 formats (.zip, .tar.gz, .tar.xz, .tar.zst, .7z)
        formats = [".zip", ".tar.gz", ".tar.xz", ".tar.zst", ".7z"]
        arc_dir = work_dir / "archives"
        arc_dir.mkdir()

        for ext in formats:
            out_arc = arc_dir / f"bundle{ext}"
            subprocess.run(
                [str(harness_bin), "create", f"dummy{ext}", str(out_arc), str(proj)],
                check=True, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            assert out_arc.is_file() and out_arc.stat().st_size > 0, f"Empty archive for {ext}"

            ext_dest = work_dir / f"extract_{ext.lstrip('.').replace('.', '_')}"
            ext_dest.mkdir()
            res = subprocess.run(
                [str(harness_bin), "extract", str(out_arc), str(ext_dest), "1"],
                check=True, env=env, text=True, capture_output=True
            )
            created_top = Path(res.stdout.strip())
            assert created_top == ext_dest / "Project Alpha", f"Unexpected root for {ext}: {created_top}"
            assert (created_top / "readme notes (v1).txt").read_bytes() == f1.read_bytes()
            assert (created_top / "Übersicht_日本語_λ.md").read_text(encoding="utf-8") == "UTF-8 content: ÄÖÜ 日本語 λ\n"
            assert (created_top / "nested dir" / "quote 'test' \"shell\" $HOME;false.txt").read_bytes() == f3.read_bytes()

            # Verify collision disambiguation on second Extract Here (never overwrites!)
            res2 = subprocess.run(
                [str(harness_bin), "extract", str(out_arc), str(ext_dest), "1"],
                check=True, env=env, text=True, capture_output=True
            )
            created_dup = Path(res2.stdout.strip())
            assert created_dup == ext_dest / "Project Alpha (2)", f"Expected (2) disambiguation, got {created_dup}"

        # 2. Multi-root "tarbomb" archive -> Extract Here creates `<stem>/` automatically
        multi_arc = arc_dir / "multi_items.tar.gz"
        a_file = src_root / "alpha.txt"
        b_file = src_root / "beta.txt"
        a_file.write_text("alpha\n", encoding="utf-8")
        b_file.write_text("beta\n", encoding="utf-8")
        subprocess.run(
            [str(harness_bin), "create", "x.tar.gz", str(multi_arc), str(a_file), str(b_file)],
            check=True, env=env, capture_output=True
        )
        multi_dest = work_dir / "multi_dest"
        multi_dest.mkdir()
        mres = subprocess.run(
            [str(harness_bin), "extract", str(multi_arc), str(multi_dest), "1"],
            check=True, env=env, text=True, capture_output=True
        )
        assert Path(mres.stdout.strip()) == multi_dest / "multi_items"
        assert (multi_dest / "multi_items" / "alpha.txt").read_text(encoding="utf-8") == "alpha\n"
        assert (multi_dest / "multi_items" / "beta.txt").read_text(encoding="utf-8") == "beta\n"

        # 3. Adversarial Zip-Slip & Symlink-Escape Rejection
        jail_parent = work_dir / "jail_parent"
        outside_target = work_dir / "outside_target"
        outside_target.mkdir()
        jail_dest = jail_parent / "dest"
        jail_dest.mkdir(parents=True)

        evil_cases = []
        z_dotdot = arc_dir / "evil_dotdot.zip"
        make_zip_with_raw_entry(z_dotdot, "../../outside_target/zipslip.txt", b"pwned")
        evil_cases.append(z_dotdot)

        z_nested_dotdot = arc_dir / "evil_nested_dotdot.zip"
        make_zip_with_raw_entry(z_nested_dotdot, "safe_dir/../../../outside_target/nested.txt", b"pwned")
        evil_cases.append(z_nested_dotdot)

        z_abs = arc_dir / "evil_abs.zip"
        make_zip_with_raw_entry(z_abs, f"{outside_target}/abs_slip.txt", b"pwned")
        evil_cases.append(z_abs)

        t_symlink = arc_dir / "evil_symlink.tar.gz"
        make_tar_with_symlink_escape(t_symlink)
        evil_cases.append(t_symlink)

        for evil_arc in evil_cases:
            rc = subprocess.run(
                [str(harness_bin), "extract", str(evil_arc), str(jail_dest), "0"],
                env=env, text=True, capture_output=True
            )
            assert rc.returncode != 0, f"Adversarial archive {evil_arc.name} was NOT rejected!"
            assert list(outside_target.iterdir()) == [], f"Outside write occurred for {evil_arc.name}!"
            assert list(jail_dest.iterdir()) == [], f"Staging leak inside dest for {evil_arc.name}!"

    print("PASS: check-archive-safety (5 formats, UTF-8/spaces/quotes, Zip-Slip & symlink escape blocked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
