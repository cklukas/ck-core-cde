#!/usr/bin/env python3
"""ASan/UBSan unit test for dtcm Undo ring buffer (undo_stack.c)."""
import os
import subprocess
import sys
import tempfile

C_TEST = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "undo_stack.h"

int main(void)
{
    dtcm_undo_clear();
    assert(!dtcm_undo_can_undo());
    assert(dtcm_undo_count() == 0);

    dtcm_undo_push_delete(1000, 4600, 1, "Meeting A\nLocation: Room 1\nCategory: Work");
    dtcm_undo_push_reschedule(2000, 5600, 3800, 7400, 1, "Meeting B");
    assert(dtcm_undo_can_undo());
    assert(dtcm_undo_count() == 2);

    DtcmUndoEntry e;
    assert(dtcm_undo_pop(&e) == 1);
    assert(e.kind == DTCM_UNDO_RESCHEDULE);
    assert(e.old_start == 2000 && e.old_end == 5600);
    assert(e.new_start == 3800 && e.new_end == 7400);
    assert(strcmp(e.summary, "Meeting B") == 0);

    assert(dtcm_undo_pop(&e) == 1);
    assert(e.kind == DTCM_UNDO_DELETE);
    assert(e.old_start == 1000 && e.old_end == 4600);
    assert(strstr(e.summary, "Meeting A") != NULL);
    assert(!dtcm_undo_can_undo());
    assert(dtcm_undo_pop(&e) == 0);
    printf("ok    undo-push-pop\n");

    /* Ring wraparound (> DTCM_UNDO_MAX entries) */
    for (int i = 0; i < DTCM_UNDO_MAX + 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Item %d", i);
        dtcm_undo_push_delete(100 + i, 200 + i, 1, buf);
    }
    assert(dtcm_undo_count() == DTCM_UNDO_MAX);
    assert(dtcm_undo_pop(&e) == 1);
    assert(strcmp(e.summary, "Item 14") == 0);
    for (int i = 0; i < DTCM_UNDO_MAX - 1; i++)
        assert(dtcm_undo_pop(&e) == 1);
    assert(strcmp(e.summary, "Item 5") == 0);
    assert(dtcm_undo_count() == 0);
    printf("ok    undo-ring-wraparound\n");

    return 0;
}
"""


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    dtcm_dir = os.path.dirname(test_dir)
    cc = os.environ.get("CC", "clang")

    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "undo-test.c")
        exe = os.path.join(td, "undo-test")
        with open(src, "w", encoding="utf-8") as f:
            f.write(C_TEST)
        subprocess.run(
            [cc, "-O1", "-g", "-fsanitize=address,undefined",
             f"-I{dtcm_dir}", src,
             os.path.join(dtcm_dir, "undo_stack.c"),
             "-o", exe],
            check=True,
        )
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        subprocess.run([exe], env=env, check=True)
    print("check-undo-stack: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
