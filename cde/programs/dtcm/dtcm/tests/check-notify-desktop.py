#!/usr/bin/env python3
"""Standalone test for dtcm desktop alarm notifications (notify_desktop.c)."""

import json
import os
import subprocess
import sys
import tempfile
import time

MOCK_NOTIFY = r"""#!/usr/bin/env python3
import json, os, sys
out = os.environ["MOCK_NOTIFY_OUT"]
with open(out, "w", encoding="utf-8") as f:
    json.dump(sys.argv[1:], f)
"""

C_HARNESS = r"""
#include <assert.h>
#include <stdlib.h>
#include "notify_desktop.h"

int main(int argc, char **argv)
{
    assert(argc >= 2);
    if (argv[1][0] == '1') {
        assert(dtcm_notify_reminder("-urgent budget review\n", "14:00 to 15:00") == 0);
    } else {
        setenv("DTCM_DESKTOP_NOTIFY", "0", 1);
        assert(dtcm_notify_reminder("Should not fire", "10:00") == 0);
    }
    return 0;
}
"""


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    dtcm_dir = os.path.dirname(test_dir)
    cc = os.environ.get("CC", "clang")

    with tempfile.TemporaryDirectory() as td:
        mock = os.path.join(td, "mock-notify-send")
        with open(mock, "w", encoding="utf-8") as f:
            f.write(MOCK_NOTIFY)
        os.chmod(mock, 0o755)

        src = os.path.join(td, "notify-test.c")
        exe = os.path.join(td, "notify-test")
        with open(src, "w", encoding="utf-8") as f:
            f.write(C_HARNESS)

        subprocess.run(
            [cc, "-O1", "-g", "-fsanitize=address,undefined",
             f"-I{dtcm_dir}", src, os.path.join(dtcm_dir, "notify_desktop.c"),
             "-o", exe],
            check=True,
        )

        out_json = os.path.join(td, "out.json")
        env = dict(os.environ)
        env["DTCM_NOTIFY_BIN"] = mock
        env["MOCK_NOTIFY_OUT"] = out_json
        env["ASAN_OPTIONS"] = "detect_leaks=0"

        subprocess.run([exe, "1"], env=env, check=True)
        for _ in range(40):
            if os.path.exists(out_json) and os.path.getsize(out_json) > 0:
                break
            time.sleep(0.05)
        assert os.path.exists(out_json), "mock notify-send did not run"
        args = json.load(open(out_json, encoding="utf-8"))
        assert "--app-name=Calendar" in args
        assert "--icon=office-calendar" in args
        assert "--" in args
        sep_idx = args.index("--")
        assert args[sep_idx + 1] == "Calendar Reminder (14:00 to 15:00)"
        assert args[sep_idx + 2] == "-urgent budget review"
        print("ok    notify-send-invoked-with-flag-guard")

        os.unlink(out_json)
        subprocess.run([exe, "0"], env=env, check=True)
        time.sleep(0.2)
        assert not os.path.exists(out_json), "notification fired despite DTCM_DESKTOP_NOTIFY=0"
        print("ok    opt-out-respected")

    print("check-notify-desktop: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
