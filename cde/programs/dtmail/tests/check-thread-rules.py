#!/usr/bin/env python3
"""ASan/UBSan unit tests for dtmail MailThreadRules (threading, Quick Filter, Rules 0600, notifications)."""

import os
import subprocess
import sys
import tempfile

C_TEST = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "MailThreadRules.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *dir = argv[1];

    /* 1. Subject normalization + conversation threading */
    char ns[128];
    dtmail_normalize_subject("Re: Fwd:   Q4 Plan  ", ns, sizeof(ns));
    assert(strcmp(ns, "q4 plan") == 0);

    DtMailThreadItem items[3];
    memset(items, 0, sizeof(items));
    items[0].orig_index = 0;
    snprintf(items[0].msg_id, sizeof(items[0].msg_id), "<m1@ex>");
    snprintf(items[0].subject, sizeof(items[0].subject), "Q4 Plan");
    items[0].timestamp = 100;

    items[1].orig_index = 1;
    snprintf(items[1].msg_id, sizeof(items[1].msg_id), "<m2@ex>");
    snprintf(items[1].subject, sizeof(items[1].subject), "Lunch?");
    items[1].timestamp = 150;

    items[2].orig_index = 2;
    snprintf(items[2].msg_id, sizeof(items[2].msg_id), "<m3@ex>");
    snprintf(items[2].in_reply_to, sizeof(items[2].in_reply_to), "<m1@ex>");
    snprintf(items[2].subject, sizeof(items[2].subject), "Re: Q4 Plan");
    items[2].timestamp = 200;

    int nthreads = dtmail_thread_group(items, 3);
    assert(nthreads == 2);
    /* Thread 0 has orig 0 (depth 0) then orig 2 (depth 1); Thread 1 has orig 1 */
    assert(items[0].orig_index == 0 && items[0].depth == 0);
    assert(items[1].orig_index == 2 && items[1].depth == 1);
    assert(items[2].orig_index == 1 && items[2].depth == 0);
    printf("ok    conversation-threading\n");

    /* 2. Quick filter matching */
    assert(dtmail_quick_filter_matches("Alice", "Q4 Plan", "budget", 1, 0, 1, "q4", DTMAIL_FILTER_UNREAD) == 1);
    assert(dtmail_quick_filter_matches("Alice", "Q4 Plan", "budget", 0, 0, 1, "q4", DTMAIL_FILTER_UNREAD) == 0);
    assert(dtmail_quick_filter_matches("Alice", "Q4 Plan", "budget", 1, 1, 0, "", DTMAIL_FILTER_STARRED) == 1);
    assert(dtmail_quick_filter_matches("Alice", "Q4 Plan", "budget", 1, 0, 1, "", DTMAIL_FILTER_HAS_ATTACH) == 1);
    printf("ok    quick-filter-matching\n");

    /* 3. Rules 0600 persistence + evaluation */
    dtmail_rules_shutdown();
    DtMailRule r1;
    memset(&r1, 0, sizeof(r1));
    snprintf(r1.name, sizeof(r1.name), "Spam Filter");
    r1.field = DTMAIL_RULE_FIELD_SUBJECT;
    snprintf(r1.pattern, sizeof(r1.pattern), "FREE PRIZE");
    r1.action = DTMAIL_RULE_ACT_JUNK;
    snprintf(r1.target_folder, sizeof(r1.target_folder), "Junk");
    r1.enabled = 1;
    assert(dtmail_rules_add(&r1) == 0);

    char rpath[1024];
    snprintf(rpath, sizeof(rpath), "%s/dtmail_rules", dir);
    assert(dtmail_rules_save_file(rpath) == 0);
    struct stat st;
    assert(stat(rpath, &st) == 0 && (st.st_mode & 0777) == 0600);

    dtmail_rules_shutdown();
    assert(dtmail_rules_load_file(rpath) == 0);
    assert(dtmail_rules_count() == 1);
    assert(dtmail_rules_evaluate("spammer@bad", "me@ok", "Claim your free prize now") == 0);
    assert(dtmail_rules_evaluate("alice@ok", "me@ok", "Architecture Review") == -1);
    printf("ok    rules-0600-and-eval\n");
    return 0;
}
"""


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    dtmail_root = os.path.dirname(test_dir)
    dtmail_src = os.path.join(dtmail_root, "dtmail")
    cxx = os.environ.get("CXX", "clang++")

    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "thread-rules-test.cc")
        exe = os.path.join(td, "thread-rules-test")
        with open(src, "w", encoding="utf-8") as f:
            f.write(C_TEST)
        subprocess.run(
            [cxx, "-O1", "-g", "-fsanitize=address,undefined",
             f"-I{dtmail_src}", src,
             os.path.join(dtmail_src, "MailThreadRules.C"),
             "-o", exe],
            check=True,
        )
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        subprocess.run([exe, td], env=env, check=True)
    print("check-thread-rules: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
