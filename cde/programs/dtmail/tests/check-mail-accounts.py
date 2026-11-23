#!/usr/bin/env python3
"""ASan/UBSan unit tests for dtmail MailAccountStore (presets, 0600 TSV, fallback inbox, keyring)."""

import os
import stat
import subprocess
import sys
import tempfile

C_TEST = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "MailAccountStore.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *dir = argv[1];

    /* 1. Presets */
    DtMailAccount gm;
    memset(&gm, 0, sizeof(gm));
    snprintf(gm.id, sizeof(gm.id), "work-gmail");
    snprintf(gm.name, sizeof(gm.name), "Work Gmail");
    snprintf(gm.email, sizeof(gm.email), "alice@gmail.com");
    dtmail_account_apply_preset(&gm, "gmail");
    assert(strcmp(gm.imap_host, "imap.gmail.com") == 0);
    assert(gm.imap_port == 993 && gm.imap_tls == DTMAIL_TLS_IMPLICIT);
    assert(strcmp(gm.smtp_host, "smtp.gmail.com") == 0);
    assert(gm.auth_mode == DTMAIL_AUTH_OAUTH2 && gm.oauth_provider == DTMAIL_OAUTH_GMAIL);

    DtMailAccount ic;
    memset(&ic, 0, sizeof(ic));
    snprintf(ic.id, sizeof(ic.id), "personal-icloud");
    snprintf(ic.name, sizeof(ic.name), "Personal iCloud");
    snprintf(ic.email, sizeof(ic.email), "alice@icloud.com");
    dtmail_account_apply_preset(&ic, "icloud");
    assert(strcmp(ic.imap_host, "imap.mail.me.com") == 0);
    assert(ic.smtp_port == 587 && ic.smtp_tls == DTMAIL_TLS_STARTTLS);
    printf("ok    provider-presets\n");

    /* 2. Upsert + 0600 save/load round-trip */
    dtmail_accounts_shutdown();
    assert(dtmail_accounts_upsert(&gm) == 0);
    assert(dtmail_accounts_upsert(&ic) == 1);
    assert(dtmail_accounts_count() == 2);

    char store_path[1024];
    snprintf(store_path, sizeof(store_path), "%s/dtmail_accounts", dir);
    assert(dtmail_accounts_save_file(store_path) == 0);
    struct stat st;
    assert(stat(store_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    dtmail_accounts_shutdown();
    assert(dtmail_accounts_count() == 0);
    assert(dtmail_accounts_load_file(store_path) == 0);
    assert(dtmail_accounts_count() == 2);
    assert(strcmp(dtmail_accounts_get(0)->imap_host, "imap.gmail.com") == 0);
    assert(strcmp(dtmail_accounts_get(1)->email, "alice@icloud.com") == 0);
    printf("ok    accounts-0600-roundtrip\n");

    /* 3. Startup inbox resolution (remote account -> fallback when no spool) */
    setenv("HOME", dir, 1);
    char inbox[1024];
    int used_fb = 0;
    assert(dtmail_resolve_startup_inbox("/nonexistent/var/mail/alice", inbox, sizeof(inbox), &used_fb) == 0);
    assert(used_fb == 0);
    assert(strstr(inbox, "/.dt/mail/work-gmail/INBOX") != NULL);

    dtmail_accounts_shutdown();
    assert(dtmail_resolve_startup_inbox("/nonexistent/var/mail/alice", inbox, sizeof(inbox), &used_fb) == 0);
    assert(used_fb == 1);
    assert(strstr(inbox, "/.dt/mail/Inbox") != NULL);
    assert(stat(inbox, &st) == 0 && S_ISREG(st.st_mode));
    printf("ok    startup-inbox-fallback\n");

    /* 4. Keyring store/lookup/clear via mock secret-tool */
    assert(dtmail_keyring_store("work-gmail", "password", "s3cr3t-pass") == 0);
    char got[128];
    assert(dtmail_keyring_lookup("work-gmail", "password", got, sizeof(got)) == 0);
    assert(strcmp(got, "s3cr3t-pass") == 0);
    assert(dtmail_keyring_clear("work-gmail", "password") == 0);
    assert(dtmail_keyring_lookup("work-gmail", "password", got, sizeof(got)) != 0);
    printf("ok    keyring-store-lookup-clear\n");

    /* The helper may close stdin without consuming any bytes. */
    char *large_secret = (char *)malloc(1024 * 1024);
    assert(large_secret);
    memset(large_secret, 'x', 1024 * 1024 - 1);
    large_secret[1024 * 1024 - 1] = '\0';
    assert(dtmail_keyring_store("work-gmail", "closed", large_secret) == -1);
    free(large_secret);
    printf("ok    closed-keyring-helper-is-an-error\n");

    return 0;
}
"""

MOCK_SECRET_TOOL = """#!/usr/bin/env python3
import os, sys, json
db_path = os.environ["MOCK_KEYRING_DB"]
db = {}
if os.path.exists(db_path):
    with open(db_path, "r") as f:
        db = json.load(f)
cmd = sys.argv[1]
args = sys.argv[2:]
kv = {}
i = 0
while i < len(args):
    if args[i] == "--label":
        i += 2
    elif i + 1 < len(args):
        kv[args[i]] = args[i+1]
        i += 2
    else:
        i += 1
key = f"{kv.get('service')}:{kv.get('account')}:{kv.get('kind')}"
if cmd == "store":
    if kv.get("kind") == "closed":
        sys.exit(1)
    db[key] = sys.stdin.read()
    with open(db_path, "w") as f:
        json.dump(db, f)
    sys.exit(0)
elif cmd == "lookup":
    if key in db:
        sys.stdout.write(db[key])
        sys.exit(0)
    sys.exit(1)
elif cmd == "clear":
    db.pop(key, None)
    with open(db_path, "w") as f:
        json.dump(db, f)
    sys.exit(0)
sys.exit(1)
"""


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    dtmail_root = os.path.dirname(test_dir)
    dtmail_src = os.path.join(dtmail_root, "dtmail")
    cxx = os.environ.get("CXX", "clang++")

    with tempfile.TemporaryDirectory() as td:
        mock = os.path.join(td, "mock-secret-tool")
        with open(mock, "w", encoding="utf-8") as f:
            f.write(MOCK_SECRET_TOOL)
        os.chmod(mock, stat.S_IRWXU)

        src = os.path.join(td, "accounts-test.cc")
        exe = os.path.join(td, "accounts-test")
        with open(src, "w", encoding="utf-8") as f:
            f.write(C_TEST)

        subprocess.run(
            [cxx, "-O1", "-g", "-fsanitize=address,undefined",
             f"-I{dtmail_src}", src,
             os.path.join(dtmail_src, "MailAccountStore.C"),
             "-o", exe],
            check=True,
        )
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        env["DTMAIL_KEYRING_CMD"] = mock
        env["MOCK_KEYRING_DB"] = os.path.join(td, "keyring.json")
        subprocess.run([exe, td], env=env, check=True)
    print("check-mail-accounts: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
