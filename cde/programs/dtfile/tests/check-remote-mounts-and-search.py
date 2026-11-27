#!/usr/bin/env python3
"""Verify dtfile Find ck_fs_search integration and webdavs/sftp/smb user-space mount helpers."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

C_HARNESS = r"""
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ck_fs_search.h>

static void run_search_and_collect(const char *root,
                                   const char *filter,
                                   const char *content,
                                   char hits[][512],
                                   int max_hits,
                                   int *out_count)
{
    ckfes_options_v1 opts;
    ckfes_options_init_v1(&opts);
    opts.root_path.data = root;
    opts.root_path.units = strlen(root);
    opts.root_path.encoding = CKFES_POSIX_BYTES;

    if (filter && *filter && strcmp(filter, "*") != 0) {
        opts.name_query.data = filter;
        opts.name_query.units = strlen(filter);
        opts.name_query.encoding = CKFES_POSIX_BYTES;
        if (strchr(filter, '*') != NULL || strchr(filter, '?') != NULL)
            opts.name_mode = CKFES_NAME_GLOB;
        else
            opts.name_mode = CKFES_NAME_SUBSTRING;
    } else {
        opts.name_mode = CKFES_NAME_NONE;
    }

    if (content && *content) {
        opts.content_query.data = content;
        opts.content_query.units = strlen(content);
        opts.content_query.encoding = CKFES_POSIX_BYTES;
    }

    opts.case_sensitive = 0;
    opts.include_hidden = 0;
    opts.skip_binary_content = 1;
    opts.max_depth = CKFES_HARD_MAX_DEPTH;
    opts.max_results = 50000;

    ckfes_job *job = NULL;
    ckfe_error err;
    memset(&err, 0, sizeof(err));
    assert(ckfes_start_v1(&opts, &job, &err) == CKFE_API_OK && job != NULL);

    ckfes_snapshot_v1 snap;
    for (;;) {
        memset(&snap, 0, sizeof(snap));
        snap.version = CKFES_VERSION;
        snap.struct_size = sizeof(snap);
        assert(ckfes_poll_v1(job, &snap) == CKFE_API_OK);
        if (snap.state != CKFES_RUNNING)
            break;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    assert(snap.state == CKFES_COMPLETE);

    int count = 0;
    uint64_t offset = 0;
    ckfes_hit_v1 page[32];
    while (offset < snap.matched_entries && count < max_hits) {
        uint32_t got = 0;
        if (ckfes_page_v1_get(job, offset, page, 32, &got) != CKFE_API_OK || got == 0)
            break;
        for (uint32_t i = 0; i < got && count < max_hits; i++) {
            uint64_t req = 0;
            uint32_t enc = 0;
            assert(page[i].relative_units < 500);
            assert(ckfes_path_copy_v1(job, page[i].hit_index, hits[count],
                                      page[i].relative_units, &req, &enc) == CKFE_API_OK);
            hits[count][page[i].relative_units] = '\0';
            count++;
        }
        offset += got;
    }

    uint32_t drained = 0;
    while (ckfes_worker_drained_v1(job, &drained) == CKFE_API_OK && drained == 0) {
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    ckfes_release(job);
    *out_count = count;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *root = argv[1];

    /* 1. Glob search (*.md) */
    char hits[16][512];
    int n = 0;
    run_search_and_collect(root, "*.md", "", hits, 16, &n);
    assert(n == 1);
    assert(strcmp(hits[0], "docs/notes.md") == 0);
    puts("ok    find-ckfes-glob");

    /* 2. Case-insensitive content search with shell-adversarial filenames & binary skip */
    run_search_and_collect(root, "", "NEEDLE_TOKEN_42", hits, 16, &n);
    assert(n == 2);
    int saw_notes = 0, saw_adv = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(hits[i], "docs/notes.md") == 0) saw_notes = 1;
        if (strstr(hits[i], "quote\"$(echo_bad)`tick`.txt") != NULL) saw_adv = 1;
    }
    assert(saw_notes && saw_adv);
    puts("ok    find-ckfes-content-and-adversarial-names");

    /* 3. Cancel + drain lifecycle */
    ckfes_options_v1 opts;
    ckfes_options_init_v1(&opts);
    opts.root_path.data = root;
    opts.root_path.units = strlen(root);
    opts.root_path.encoding = CKFES_POSIX_BYTES;
    opts.name_mode = CKFES_NAME_NONE;
    ckfes_job *job = NULL;
    ckfe_error err;
    memset(&err, 0, sizeof(err));
    assert(ckfes_start_v1(&opts, &job, &err) == CKFE_API_OK && job != NULL);
    ckfes_request_cancel(job);
    uint32_t drained = 0;
    while (ckfes_worker_drained_v1(job, &drained) == CKFE_API_OK && drained == 0) {
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    ckfes_release(job);
    puts("ok    find-ckfes-cancel-drain");
    return 0;
}
"""


def main() -> int:
    root = Path(__file__).resolve().parents[1]

    # Source contract checks on Find.c and Devices.c
    find_src = (root / "Find.c").read_text(encoding="utf-8")
    devices_src = (root / "Devices.c").read_text(encoding="utf-8")
    assert "ckfes_start_v1" in find_src and "FindFsPollTimerCb" in find_src
    assert 'strncasecmp(uri, "webdavs://", 10)' in devices_src
    assert 'snprintf(out, out_sz, "davs://%s", raw + 10)' in devices_src
    assert "DTFILE_SSHFS_BIN" in devices_src and "get_user_mounts_root_dir" in devices_src
    print("ok    dtfile-source-contracts")

    pkg_cfg = os.environ.get("PKG_CONFIG", "pkg-config")
    pkg_env = dict(os.environ)
    extra_pc = [
        p for p in (
            "/usr/dt/lib/pkgconfig",
            "/usr/dt/libdata/pkgconfig",
            "/var/tmp/ck-wayland-build/wp00/fs-prefix/lib/pkgconfig",
        )
        if Path(p).is_dir()
    ]
    if extra_pc:
        cur_pc = pkg_env.get("PKG_CONFIG_PATH", "")
        pkg_env["PKG_CONFIG_PATH"] = ":".join(extra_pc + ([cur_pc] if cur_pc else []))

    pc_proc = subprocess.run(
        [pkg_cfg, "--cflags", "--libs", "ck-core-fs-lib"],
        capture_output=True,
        text=True,
        env=pkg_env,
    )
    if pc_proc.returncode != 0:
        print("SKIP: ck-core-fs-lib not found in pkg-config; set PKG_CONFIG_PATH")
        return 77
    flags = shlex.split(pc_proc.stdout)

    tmp_base = os.environ.get("TMPDIR") or None
    with tempfile.TemporaryDirectory(prefix="dtfile-find-remote-", dir=tmp_base) as td:
        td_path = Path(td)
        c_src = td_path / "harness.c"
        exe = td_path / "harness"
        c_src.write_text(C_HARNESS, encoding="utf-8")

        cc = shlex.split(os.environ.get("CC", "cc"))
        lib_dirs = [f[2:] for f in flags if f.startswith("-L")]
        rpath_flags = [f"-Wl,-rpath,{d}" for d in lib_dirs]
        subprocess.run(
            cc + ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                  str(c_src), "-o", str(exe), *flags, *rpath_flags],
            check=True,
        )

        fx = td_path / "fixture"
        (fx / "docs").mkdir(parents=True)
        (fx / "docs" / "notes.md").write_text("Alpha needle_token_42 omega\n", encoding="utf-8")
        (fx / 'quote"$(echo_bad)`tick`.txt').write_text("has NEEDLE_TOKEN_42 safely\n", encoding="utf-8")
        # Binary file containing NUL + token must be skipped when skip_binary_content=1
        (fx / "binary.dat").write_bytes(b"\x00\x01\x02NEEDLE_TOKEN_42\x00")
        (fx / ".hidden_secret.md").write_text("NEEDLE_TOKEN_42\n", encoding="utf-8")

        subprocess.run([str(exe), str(fx)], check=True, timeout=60)

    print("check-remote-mounts-and-search: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
