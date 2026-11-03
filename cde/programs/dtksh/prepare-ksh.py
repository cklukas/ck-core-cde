"""Stage the verified ksh source and the ordered CDE patches in the build tree."""

import fcntl
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def prepare(work, revision, digest, url, supplied, configuration, patches):
    archive_name = f"ksh-{revision}.tar.gz"
    cached = work / archive_name
    bundled = Path(__file__).resolve().parent / archive_name
    work.mkdir(exist_ok=True)
    patches = [Path(patch) for patch in patches]
    receipt = json.dumps({
        "revision": revision, "sha256": digest, "configuration": configuration,
        "patches": [(p.name, hashlib.sha256(p.read_bytes()).hexdigest()) for p in patches],
    }, indent=2) + "\n"
    stamp = work / ".prepared"
    destination = work / "source"
    with (work / ".lock").open("a") as lock, tempfile.TemporaryDirectory(dir=work) as stage:
        fcntl.flock(lock, fcntl.LOCK_EX)
        stage = Path(stage)
        archive = stage / archive_name
        if supplied:
            candidate = Path(supplied)
        elif bundled.is_file():
            candidate = bundled
        elif cached.is_file():
            candidate = cached
        else:
            subprocess.run(["curl", "--fail", "--location", "--silent", "--show-error",
                            "--retry", "3", "--connect-timeout", "15", "--max-time", "180",
                            "--output", archive, url], check=True)
            candidate = archive
        # Verify the exact bytes we will extract, including on offline builds.
        if candidate != archive:
            shutil.copyfile(candidate, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            sys.exit(f"ksh93 archive SHA-256 mismatch (expected {digest})")
        if candidate != cached:
            archive.replace(cached)
            archive = cached
        if destination.is_dir() and stamp.is_file() and stamp.read_text() == receipt:
            return
        subprocess.run(["tar", "-xzf", archive, "-C", stage], check=True)
        source = stage / f"ksh-{revision}"
        for patch in patches:
            with open(patch, "rb") as changes:
                subprocess.run(["patch", "-p1", "-F0", "--batch", "--forward"],
                               cwd=source, stdin=changes, check=True)
        (stage / "receipt").write_text(receipt)
        # An interrupted replacement must never leave an old receipt
        # certifying the new tree. Only discard the old tree after the swap.
        if stamp.exists():
            stamp.unlink()
        if destination.exists():
            destination.rename(stage / "previous-source")
        source.rename(destination)
        (stage / "receipt").replace(stamp)
        for old_archive in work.glob("ksh-*.tar.gz"):
            if old_archive != cached:
                old_archive.unlink()


if __name__ == "__main__":
    try:
        prepare(Path(sys.argv[1]), *sys.argv[2:7], sys.argv[7:])
    except (OSError, subprocess.CalledProcessError) as error:
        sys.exit(f"Cannot prepare pinned ksh93: {error}")
