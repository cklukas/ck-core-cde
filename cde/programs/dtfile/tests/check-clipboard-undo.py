#!/usr/bin/env python3
"""C23 + ASan/UBSan unit verification for dtfile file-clipboard URI & MIME codecs (WP-17)."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def detect_sanitize(cc_cmd: list[str]) -> list[str]:
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as td:
        probe_c = Path(td) / "probe.c"
        probe_bin = Path(td) / "probe"
        probe_c.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        for s in ("address,undefined", "undefined"):
            res = subprocess.run(cc_cmd + [f"-fsanitize={s}", str(probe_c), "-o", str(probe_bin)],
                                 capture_output=True)
            if res.returncode == 0 and subprocess.run([str(probe_bin)], capture_output=True).returncode == 0:
                return [f"-fsanitize={s}", "-fno-omit-frame-pointer"]
    return []


HARNESS_C = r"""
#include "ClipboardUndo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* 1. Round-trip RFC-3986 file:// URI encoding & decoding */
    const char *cases[] = {
        "/tmp/simple.txt",
        "/home/user/My Documents/Report #1 (Final) 100%.pdf",
        "/var/tmp/utf8-äöü-日本語-🚀/file?query=1&b=2.dat",
        "/"
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        char *uri = DtfilePathToFileUri(cases[i]);
        assert(uri != nullptr);
        assert(strncmp(uri, "file:///", 8) == 0);
        char *decoded = DtfileFileUriToPath(uri);
        assert(decoded != nullptr);
        assert(strcmp(decoded, cases[i]) == 0);
        XtFree(uri);
        XtFree(decoded);
    }

    /* 2. file://localhost/ authority acceptance & remote-host / malformed rejection */
    char *lh = DtfileFileUriToPath("file://localhost/etc/hosts\r\n");
    assert(lh != nullptr && strcmp(lh, "/etc/hosts") == 0);
    XtFree(lh);

    assert(DtfilePathToFileUri("relative/path") == nullptr);
    assert(DtfileFileUriToPath("http://example.com/a") == nullptr);
    assert(DtfileFileUriToPath("file://remotehost/share/a") == nullptr);
    assert(DtfileFileUriToPath("file:///bad%2Gescape") == nullptr);
    assert(DtfileFileUriToPath("file:///embedded%00nul") == nullptr);

    /* 3. x-special/gnome-copied-files (cut & copy) */
    const char *gnome_cut =
        "cut\n"
        "file:///tmp/dir%20one/alpha%231.txt\n"
        "file:///tmp/dir%20one/beta.txt";
    Boolean is_cut = False;
    char **paths = nullptr;
    int count = 0;
    assert(DtfileParseClipboardPayload("x-special/gnome-copied-files",
                                       gnome_cut, strlen(gnome_cut),
                                       &is_cut, &paths, &count));
    assert(is_cut == True);
    assert(count == 2);
    assert(strcmp(paths[0], "/tmp/dir one/alpha#1.txt") == 0);
    assert(strcmp(paths[1], "/tmp/dir one/beta.txt") == 0);
    DtfileFreeStringArray(paths, count);

    /* 4. text/uri-list with comments and CRLF */
    const char *uri_list =
        "# Exported from Wayland peer\r\n"
        "file:///var/tmp/one.txt\r\n"
        "file://localhost/var/tmp/two%202.txt\r\n";
    is_cut = True;
    paths = nullptr;
    count = 0;
    assert(DtfileParseClipboardPayload("text/uri-list",
                                       uri_list, strlen(uri_list),
                                       &is_cut, &paths, &count));
    assert(is_cut == False);
    assert(count == 2);
    assert(strcmp(paths[0], "/var/tmp/one.txt") == 0);
    assert(strcmp(paths[1], "/var/tmp/two 2.txt") == 0);
    DtfileFreeStringArray(paths, count);

    /* 5. UTF8_STRING plain absolute paths */
    const char *plain_paths = "/tmp/p1\n/tmp/p2\nrelative_ignored\n";
    assert(DtfileParseClipboardPayload("UTF8_STRING",
                                       plain_paths, strlen(plain_paths),
                                       &is_cut, &paths, &count));
    assert(is_cut == False);
    assert(count == 2);
    assert(strcmp(paths[0], "/tmp/p1") == 0);
    assert(strcmp(paths[1], "/tmp/p2") == 0);
    DtfileFreeStringArray(paths, count);

    printf("check-clipboard-undo: ALL ASSERTIONS PASSED\n");
    return 0;
}
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--module-source", default=str(Path(__file__).resolve().parent.parent))
    args = ap.parse_args()

    src_dir = Path(args.module_source).resolve()
    cc_cmd = shlex.split(os.environ.get("CC", "cc"))
    pkg_cfg = os.environ.get("PKG_CONFIG", "pkg-config")

    xt_cflags = shlex.split(
        subprocess.run([pkg_cfg, "--cflags", "xt", "x11"], capture_output=True, text=True).stdout
    )
    xt_libs = shlex.split(
        subprocess.run([pkg_cfg, "--libs", "xt", "x11"], capture_output=True, text=True).stdout
    ) or ["-lXt", "-lX11"]

    san_flags = detect_sanitize(cc_cmd)

    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as td:
        td_path = Path(td)
        harness_path = td_path / "harness.c"
        exe_path = td_path / "check_clipboard_undo"
        harness_path.write_text(HARNESS_C, encoding="utf-8")

        cmd = (
            cc_cmd
            + ["-std=gnu23", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-DDTFILE_CLIPBOARD_UNIT_TEST", f"-I{src_dir}"]
            + xt_cflags
            + san_flags
            + [str(src_dir / "ClipboardUndo.c"), str(harness_path), "-o", str(exe_path)]
            + xt_libs
        )
        comp = subprocess.run(cmd, capture_output=True, text=True)
        if comp.returncode != 0:
            sys.stderr.write(f"Compile failed:\n{' '.join(cmd)}\n{comp.stderr}\n")
            return 1

        env = os.environ.copy()
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        run = subprocess.run([str(exe_path)], env=env, capture_output=True, text=True)
        if run.returncode != 0:
            sys.stderr.write(f"Run failed:\n{run.stdout}\n{run.stderr}\n")
            return 1
        sys.stdout.write(run.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
