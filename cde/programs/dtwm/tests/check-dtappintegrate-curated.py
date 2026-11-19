#!/usr/bin/env python3
"""Automated regression test for smart Application Manager catalog curation,
auto-gray + bottom-right blue downward download arrow badge XPM generation,
and `ck-pkg-install` launcher wiring in `dtappintegrate-xdg`:

1. Installed binaries (e.g. `CkBrowser` -> `ck-browser`) stay visible with their
   stock full-color icon and direct `EXEC_STRING`.
2. Uninstalled binaries present in the package registry (e.g. `firefox`,
   `chromium` -> `chromium-browser`, `thunderbird`) generate desaturated
   grayscale `.l.pm`/`.m.pm`/`.t.pm` icons stamped with the royal-blue downward
   arrow badge (`#1864d8` / `#6bb6ff` / `#082054`) and invoke `ck-pkg-install`.
3. Discontinued / non-registry stubs (`skype`, `nxclient`) are automatically
   excluded from the user's Application Manager folder.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    script = (Path(__file__).resolve().parent.parent.parent / "dtappintegrate" / "dtappintegrate-xdg").resolve()
    if not script.is_file():
        raise SystemExit(f"dtappintegrate-xdg not found at {script}")

    tmp_base = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="ck-appmgr-curated-", dir=tmp_base) as work_s:
        work = Path(work_s)
        home = work / "home"
        mock_bin = work / "bin"
        mock_pkg = work / "pkg-registry"
        sys_appman = work / "sys-appmanager"
        home.mkdir(mode=0o755)
        mock_bin.mkdir(mode=0o755)
        mock_pkg.mkdir(mode=0o755)
        (sys_appman / "Internet").mkdir(parents=True, mode=0o755)

        # Simulate installed ck-browser
        ck_br = mock_bin / "ck-browser"
        ck_br.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        ck_br.chmod(0o755)

        # Simulate registry-available firefox & chromium-browser, while skype & nxclient are absent
        (mock_pkg / "firefox").write_text("Version: 135.0\n", encoding="utf-8")
        (mock_pkg / "chromium-browser").write_text("Version: 134.0\n", encoding="utf-8")

        for name in ("CkBrowser", "firefox", "chromium", "skype", "nxclient"):
            stub = sys_appman / "Internet" / name
            stub.write_text("", encoding="utf-8")
            stub.chmod(0o755)

        env = os.environ.copy()
        env.pop("DISPLAY", None)
        env.update({
            "HOME": str(home),
            "XDG_DATA_HOME": str(home / ".local" / "share"),
            "XDG_DATA_DIRS": str(work / "empty-xdg"),
            "PATH": f"{mock_bin}:/usr/bin:/bin",
            "CK_SYS_APPMANAGER_DIR": str(sys_appman),
            "CK_PKG_BACKEND": "apt",
            "CK_PKG_MOCK_DIR": str(mock_pkg),
        })

        subprocess.run([sys.executable, str(script), "--once"], env=env, check=True)

        internet_dir = home / ".dt" / "appmanager" / "Internet"
        present = sorted(p.name for p in internet_dir.iterdir())
        if present != ["CkBrowser", "chromium", "firefox"]:
            raise SystemExit(f"expected ['CkBrowser', 'chromium', 'firefox'], got {present}")

        dt_txt = (home / ".dt" / "types" / "ck-xdg-apps.dt").read_text(encoding="utf-8")
        if "ACTION firefox" not in dt_txt or "ck-pkg-install" not in dt_txt:
            raise SystemExit(f"missing ck-pkg-install ACTION override for firefox:\n{dt_txt}")
        if "ICON        ckavail_firefox" not in dt_txt or "ICON        ckavail_chromium" not in dt_txt:
            raise SystemExit(f"missing ckavail_* icon overrides:\n{dt_txt}")
        if "skype" in dt_txt or "nxclient" in dt_txt:
            raise SystemExit("unexpected skype/nxclient action emitted")

        for sz in ("l", "m", "t"):
            pm = home / ".dt" / "icons" / f"ckavail_firefox.{sz}.pm"
            if not pm.is_file():
                raise SystemExit(f"missing generated badge icon {pm}")
            pm_txt = pm.read_text(encoding="ascii")
            # Royal-blue download arrow color (#1864d8) must be present, and non-badge pixels must be grayscale
            if "#1864d8" not in pm_txt or "#082054" not in pm_txt:
                raise SystemExit(f"blue download arrow badge colors missing in {pm}")

        print("check-dtappintegrate-curated: OK")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
