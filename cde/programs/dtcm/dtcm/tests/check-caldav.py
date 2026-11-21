#!/usr/bin/env python3
"""Standalone ASan/UBSan end-to-end test for dtcm's CalDAV client (caldav_client.c + ics_io.c).

Spins up an in-process Python CalDAV server supporting Basic auth, PROPFIND,
GET, and PUT, then compiles and runs a C harness exercising:
  1. URL validation (rejects file://, ftp://, control chars, empty).
  2. Auth rejection (HTTP 401 on wrong password).
  3. PROPFIND discovery + GET + RFC 5545 parsing via ics_io.c.
  4. PUT serialization + hostile UID filename sanitization + round-trip GET.
  5. Profile save/load (mode 0600, plaintext password never stored).
"""

import base64
import http.server
import os
import socketserver
import subprocess
import sys
import tempfile
import threading

INITIAL_ICS = (
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "PRODID:-//CK Core CDE Test//EN\r\n"
    "BEGIN:VEVENT\r\n"
    "UID:seed-event-1@example.test\r\n"
    "DTSTART:20261121T100000Z\r\n"
    "DTEND:20261121T110000Z\r\n"
    "SUMMARY:Team Standup\r\n"
    "LOCATION:Room 42\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n"
)

C_HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "caldav_client.h"

int main(int argc, char **argv)
{
    assert(argc >= 2);
    const char *base_url = argv[1];
    char err[256] = "";

    /* 1. URL validation */
    assert(dtcm_caldav_validate_url("file:///etc/passwd", err, sizeof(err)) != 0);
    assert(dtcm_caldav_validate_url("ftp://example.com/cal", err, sizeof(err)) != 0);
    assert(dtcm_caldav_validate_url("https://example.com/cal\r\nEvil: 1", err, sizeof(err)) != 0);
    assert(dtcm_caldav_validate_url("", err, sizeof(err)) != 0);
    assert(dtcm_caldav_validate_url("https://example.com/dav/cal/", err, sizeof(err)) == 0);
    printf("ok    url-validation\n");

    if (!dtcm_caldav_is_supported()) {
        printf("SKIP  libcurl not enabled\n");
        return 0;
    }

    /* 2. Auth rejection */
    DtcmCaldavConfig bad = { 0 };
    snprintf(bad.url, sizeof(bad.url), "%s", base_url);
    snprintf(bad.username, sizeof(bad.username), "alice");
    snprintf(bad.password, sizeof(bad.password), "wrongpass");
    bad.verify_tls = 1;

    DtcmCaldavItem *items = NULL;
    int count = 0;
    assert(dtcm_caldav_propfind(&bad, &items, &count, err, sizeof(err)) != 0);
    assert(strstr(err, "401") != NULL);
    printf("ok    auth-401-rejected\n");

    /* 3. PROPFIND + GET */
    DtcmCaldavConfig cfg = { 0 };
    snprintf(cfg.url, sizeof(cfg.url), "%s", base_url);
    snprintf(cfg.username, sizeof(cfg.username), "alice");
    snprintf(cfg.password, sizeof(cfg.password), "s3cr3t");
    cfg.verify_tls = 1;

    assert(dtcm_caldav_propfind(&cfg, &items, &count, err, sizeof(err)) == 0);
    assert(count == 1);
    assert(strstr(items[0].href, "seed.ics") != NULL);

    DtcmIcsEvent *evs = NULL;
    int ev_count = 0;
    assert(dtcm_caldav_get_events(&cfg, items[0].href, &evs, &ev_count, err, sizeof(err)) == 0);
    assert(ev_count == 1);
    assert(strcmp(evs[0].summary, "Team Standup") == 0);
    assert(strcmp(evs[0].location, "Room 42") == 0);
    dtcm_ics_free_events(evs);
    dtcm_caldav_free_items(items);
    printf("ok    propfind-and-get\n");

    /* 4. PUT with hostile UID path traversal sanitized + round-trip */
    DtcmIcsEvent pushed = { 0 };
    snprintf(pushed.uid, sizeof(pushed.uid), "../../escape/uid-2@test");
    snprintf(pushed.summary, sizeof(pushed.summary), "CalDAV Synced Meeting");
    snprintf(pushed.location, sizeof(pushed.location), "Encrypted HTTPS");
    pushed.start_utc = 1795262400;
    pushed.end_utc = 1795266000;
    pushed.interval = 1;

    assert(dtcm_caldav_put_event(&cfg, &pushed, err, sizeof(err)) == 0);

    items = NULL;
    count = 0;
    assert(dtcm_caldav_propfind(&cfg, &items, &count, err, sizeof(err)) == 0);
    assert(count == 2);
    int found_pushed = 0;
    for (int i = 0; i < count; i++) {
        assert(strstr(items[i].href, "..") == NULL);
        if (strstr(items[i].href, "uid-2@test.ics")) {
            evs = NULL;
            ev_count = 0;
            assert(dtcm_caldav_get_events(&cfg, items[i].href, &evs, &ev_count, err, sizeof(err)) == 0);
            assert(ev_count == 1);
            assert(strcmp(evs[0].summary, "CalDAV Synced Meeting") == 0);
            dtcm_ics_free_events(evs);
            found_pushed = 1;
        }
    }
    assert(found_pushed == 1);
    dtcm_caldav_free_items(items);
    printf("ok    put-and-roundtrip\n");

    /* 5. Profile save/load: mode 0600, auto_sync_mins, no password persisted */
    cfg.auto_sync_mins = 15;
    assert(dtcm_caldav_save_profile(&cfg) == 0);
    char prof_path[512];
    snprintf(prof_path, sizeof(prof_path), "%s/.dt/dtcm_caldav.conf", getenv("HOME"));
    struct stat st;
    assert(stat(prof_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    FILE *fp = fopen(prof_path, "r");
    assert(fp != NULL);
    char contents[2048] = "";
    size_t n = fread(contents, 1, sizeof(contents) - 1, fp);
    contents[n] = '\0';
    fclose(fp);
    assert(strstr(contents, "s3cr3t") == NULL);

    DtcmCaldavConfig loaded = { 0 };
    assert(dtcm_caldav_load_profile(&loaded) == 0);
    assert(strcmp(loaded.url, cfg.url) == 0);
    assert(strcmp(loaded.username, "alice") == 0);
    assert(loaded.password[0] == '\0');
    assert(loaded.verify_tls == 1);
    assert(loaded.auto_sync_mins == 15);
    printf("ok    profile-0600-no-password-autosync\n");

    /* 6. Persistent (UID, ETag) sync state + PUT ETag capture */
    char out_href[1024] = "";
    char out_etag[256] = "";
    assert(dtcm_caldav_put_event_ex(&cfg, &pushed, out_href, sizeof(out_href),
                                    out_etag, sizeof(out_etag), err, sizeof(err)) == 0);
    assert(strstr(out_etag, "etag-") != NULL);

    DtcmCaldavSyncState ss = { 0 };
    assert(dtcm_caldav_state_load(&ss) == 0);
    assert(dtcm_caldav_state_upsert(&ss, cfg.url, out_href, pushed.uid, out_etag) == 0);
    assert(dtcm_caldav_state_save(&ss) == 0);
    dtcm_caldav_state_free(&ss);

    char st_path[512];
    snprintf(st_path, sizeof(st_path), "%s/.dt/dtcm_caldav_state", getenv("HOME"));
    assert(stat(st_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    assert(dtcm_caldav_state_load(&ss) == 0);
    const DtcmCaldavStateEntry *e1 = dtcm_caldav_state_find_href(&ss, cfg.url, out_href);
    assert(e1 != NULL && strcmp(e1->etag, out_etag) == 0);
    const DtcmCaldavStateEntry *e2 = dtcm_caldav_state_find_uid(&ss, cfg.url, pushed.uid);
    assert(e2 != NULL && strcmp(e2->href, out_href) == 0);
    dtcm_caldav_state_free(&ss);
    printf("ok    sync-state-etag-uid\n");

    /* 7. Desktop keyring store / lookup / clear via mock secret-tool */
    assert(dtcm_caldav_keyring_store(cfg.url, "alice", "s3cr3t") == 0);
    char kr_pass[256] = "";
    assert(dtcm_caldav_keyring_lookup(cfg.url, "alice", kr_pass, sizeof(kr_pass)) == 0);
    assert(strcmp(kr_pass, "s3cr3t") == 0);
    assert(dtcm_caldav_keyring_clear(cfg.url, "alice") == 0);
    assert(dtcm_caldav_keyring_lookup(cfg.url, "alice", kr_pass, sizeof(kr_pass)) != 0);
    printf("ok    keyring-store-lookup-clear\n");

    return 0;
}
"""


def make_handler(store):
    expected_auth = "Basic " + base64.b64encode(b"alice:s3cr3t").decode("ascii")

    class CaldavHandler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def _check_auth(self):
            if self.headers.get("Authorization") != expected_auth:
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="CalDAV"')
                self.end_headers()
                return False
            return True

        def do_PROPFIND(self):
            if not self._check_auth():
                return
            parts = ['<?xml version="1.0" encoding="utf-8"?><D:multistatus xmlns:D="DAV:">']
            for name in sorted(store.keys()):
                parts.append(
                    f"<D:response><D:href>/cal/{name}</D:href>"
                    f"<D:propstat><D:prop><D:getetag>\"etag-{name}\"</D:getetag>"
                    f"<D:getcontenttype>text/calendar</D:getcontenttype>"
                    f"</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
                )
            parts.append("</D:multistatus>")
            body = "".join(parts).encode("utf-8")
            self.send_response(207)
            self.send_header("Content-Type", "application/xml; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if not self._check_auth():
                return
            name = self.path.rsplit("/", 1)[-1]
            if name not in store:
                self.send_response(404)
                self.end_headers()
                return
            body = store[name].encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/calendar; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_PUT(self):
            if not self._check_auth():
                return
            name = self.path.rsplit("/", 1)[-1]
            length = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(length).decode("utf-8")
            store[name] = data
            self.send_response(201)
            self.send_header("ETag", f'"etag-{name}"')
            self.end_headers()

    return CaldavHandler


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    dtcm_dir = os.path.dirname(test_dir)
    cc = os.environ.get("CC", "clang")

    # Check if libcurl is available via pkg-config
    pc = subprocess.run(["pkg-config", "--cflags", "--libs", "libcurl"],
                        capture_output=True, text=True)
    have_curl = (pc.returncode == 0)
    curl_flags = pc.stdout.split() if have_curl else []

    store = {"seed.ics": INITIAL_ICS}
    server = socketserver.TCPServer(("127.0.0.1", 0), make_handler(store))
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    try:
        with tempfile.TemporaryDirectory() as td:
            harness_c = os.path.join(td, "caldav-test.c")
            exe = os.path.join(td, "caldav-test")
            with open(harness_c, "w", encoding="utf-8") as f:
                f.write(C_HARNESS)

            cmd = [
                cc, "-O1", "-g", "-fsanitize=address,undefined",
                f"-I{dtcm_dir}",
                f"-DHAVE_LIBCURL={1 if have_curl else 0}",
                harness_c,
                os.path.join(dtcm_dir, "caldav_client.c"),
                os.path.join(dtcm_dir, "ics_io.c"),
                "-o", exe,
                *curl_flags,
            ]
            subprocess.run(cmd, check=True)

            mock_st = os.path.join(td, "mock-secret-tool")
            with open(mock_st, "w", encoding="utf-8") as f:
                f.write(
                    "#!/usr/bin/env python3\n"
                    "import json, os, sys\n"
                    "db_path = os.environ['MOCK_KEYRING_DB']\n"
                    "db = json.load(open(db_path)) if os.path.exists(db_path) else {}\n"
                    "cmd = sys.argv[1]\n"
                    "# Ensure password is NEVER passed on argv\n"
                    "assert 's3cr3t' not in sys.argv\n"
                    "args = [a for a in sys.argv[2:] if not a.startswith('--')]\n"
                    "key = '|'.join(args)\n"
                    "if cmd == 'store':\n"
                    "    db[key] = sys.stdin.read()\n"
                    "    json.dump(db, open(db_path, 'w'))\n"
                    "    sys.exit(0)\n"
                    "elif cmd == 'lookup':\n"
                    "    if key in db:\n"
                    "        sys.stdout.write(db[key])\n"
                    "        sys.exit(0)\n"
                    "    sys.exit(1)\n"
                    "elif cmd == 'clear':\n"
                    "    db.pop(key, None)\n"
                    "    json.dump(db, open(db_path, 'w'))\n"
                    "    sys.exit(0)\n"
                    "sys.exit(2)\n"
                )
            os.chmod(mock_st, 0o755)

            env = dict(os.environ)
            env["HOME"] = td
            env["TMPDIR"] = td
            env["DTCM_SECRET_TOOL_BIN"] = mock_st
            env["MOCK_KEYRING_DB"] = os.path.join(td, "keyring.json")
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([exe, f"http://127.0.0.1:{port}/cal/"], env=env, check=True)
    finally:
        server.shutdown()
        server.server_close()

    print("check-caldav: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
