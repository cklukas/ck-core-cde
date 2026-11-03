#!/usr/bin/env python3
"""Compile the production controller against only the selected installed C ABI."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
flags = shlex.split(subprocess.check_output(
    [os.environ.get("PKG_CONFIG", "pkg-config"), "--cflags", "--libs", "ck-core-fs-lib"], text=True))
with tempfile.TemporaryDirectory(prefix="dtfile-sync-consumer-", dir=os.environ["TMPDIR"]) as work:
    work = Path(work)
    binary = work / "controller"
    fault_flags = ["-DDTFILE_SYNC_WRAP_NAMES=1", "-Wl,--wrap=ckfet_native_copy_v1",
                   "-Wl,--wrap=ckfet_rule_counts_get_v1"] if sys.platform.startswith("linux") else []
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-pthread", "-I", str(root),
        str(root / "tests/sync-controller.c"), str(root / "dtsync/SyncJob.c"),
        "-o", str(binary), *flags, *fault_flags], check=True)
    private_root = os.environ.get("DTFILE_PRIVATE_TEST_ROOT", os.environ["TMPDIR"])
    with tempfile.TemporaryDirectory(prefix="dtfile-sync-home-", dir=private_root) as private:
        fixture = Path(private) / "home"
        fixture.mkdir(mode=0o700)
        subprocess.run([str(binary), str(fixture)], check=True, timeout=45)
    # Exercise the production preferences parser and atomic persistence worker.
    # On filesystems without owner enforcement, select a private fixture volume
    # explicitly; refusing that storage is itself covered by the native tests.
    profiles = work / "profiles"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-pthread",
        "-DSYNC_PROFILES_TEST", str(root / "dtsync/SyncProfiles.c"),
        "-o", str(profiles), *flags], check=True)
    with tempfile.TemporaryDirectory(prefix="dtfile-sync-profiles-",
            dir=private_root) as private:
        subprocess.run([str(profiles), private], check=True, timeout=60)
