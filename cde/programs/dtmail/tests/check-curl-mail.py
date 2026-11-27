#!/usr/bin/env python3
"""ASan/UBSan loopback mock IMAP + SMTP + RFC 8620/8621 JMAP tests for dtmail CurlMailEngine."""

from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading

C_TEST = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CurlMailEngine.h"

int main(int argc, char **argv) {
    assert(argc == 5);
    int imap_port = atoi(argv[1]);
    int smtp_port = atoi(argv[2]);
    int jmap_port = atoi(argv[3]);
    const char *dir = argv[4];

    DtMailAccount acct;
    memset(&acct, 0, sizeof(acct));
    snprintf(acct.id, sizeof(acct.id), "test-acct");
    snprintf(acct.email, sizeof(acct.email), "alice@example.test");
    snprintf(acct.username, sizeof(acct.username), "alice@example.test");
    snprintf(acct.imap_host, sizeof(acct.imap_host), "127.0.0.1");
    acct.imap_port = imap_port;
    acct.imap_tls = DTMAIL_TLS_NONE;
    snprintf(acct.smtp_host, sizeof(acct.smtp_host), "127.0.0.1");
    acct.smtp_port = smtp_port;
    acct.smtp_tls = DTMAIL_TLS_NONE;
    acct.auth_mode = DTMAIL_AUTH_PASSWORD;

    char err[256];
    assert(dtmail_imap_test_connection(&acct, "pass123", err, sizeof(err)) == 0);
    printf("ok    imap-noop-test\n");

    char folders[8][128];
    int nf = dtmail_imap_list_folders(&acct, "pass123", folders, 8, err, sizeof(err));
    assert(nf == 2);
    assert(strcmp(folders[0], "INBOX") == 0);
    assert(strcmp(folders[1], "Sent") == 0);
    printf("ok    imap-list-folders\n");

    char mbox[1024];
    snprintf(mbox, sizeof(mbox), "%s/INBOX", dir);
    int added = 0;
    assert(dtmail_imap_sync_folder(&acct, "pass123", "INBOX", mbox, &added, err, sizeof(err)) == 0);
    assert(added == 1);

    /* Second sync must be idempotent (UID already recorded) */
    int added2 = 0;
    assert(dtmail_imap_sync_folder(&acct, "pass123", "INBOX", mbox, &added2, err, sizeof(err)) == 0);
    assert(added2 == 0);

    /* Verify mbox has '>' escaped body "From " line */
    FILE *fp = fopen(mbox, "rb");
    assert(fp != NULL);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    assert(strstr(buf, "Subject: Hello IMAP") != NULL);
    assert(strstr(buf, "\n>From the team\n") != NULL);
    printf("ok    imap-uid-sync-and-mbox-escape\n");

    const char *rcpts[1] = { "bob@example.test" };
    const char *raw =
        "From: alice@example.test\r\n"
        "To: bob@example.test\r\n"
        "Subject: SMTP Test\r\n"
        "\r\n"
        "Hello over SMTP!\r\n";
    assert(dtmail_smtp_send(&acct, "pass123", "alice@example.test",
                            rcpts, 1, raw, strlen(raw), err, sizeof(err)) == 0);
    printf("ok    smtp-send-roundtrip\n");

    /* RFC 8620/8621 JMAP over HTTP(S) with OAuth2 Bearer token */
    setenv("HOME", dir, 1);
    DtMailAccount jacct;
    memset(&jacct, 0, sizeof(jacct));
    dtmail_account_apply_preset(&jacct, "fastmail_jmap");
    snprintf(jacct.id, sizeof(jacct.id), "jmap-acct");
    snprintf(jacct.email, sizeof(jacct.email), "alice@fastmail.test");
    snprintf(jacct.imap_host, sizeof(jacct.imap_host), "http://127.0.0.1:%d/jmap/session", jmap_port);
    snprintf(jacct.smtp_host, sizeof(jacct.smtp_host), "http://127.0.0.1:%d/jmap/session", jmap_port);
    assert(dtmail_account_is_jmap(&jacct) == 1);
    assert(dtmail_account_is_jmap(&acct) == 0);

    assert(dtmail_imap_test_connection(&jacct, "oauth_bearer_tok_99", err, sizeof(err)) == 0);
    printf("ok    jmap-session-discovery\n");

    char jfolders[8][128];
    int jnf = dtmail_imap_list_folders(&jacct, "oauth_bearer_tok_99", jfolders, 8, err, sizeof(err));
    assert(jnf == 2);
    assert(strcmp(jfolders[0], "INBOX") == 0);
    assert(strcmp(jfolders[1], "Sent") == 0);
    printf("ok    jmap-mailbox-get\n");

    char jmbox[1024];
    assert(dtmail_account_folder_path(jacct.id, "INBOX", jmbox, sizeof(jmbox)) == 0);
    int jadded = 0;
    assert(dtmail_imap_sync_folder(&jacct, "oauth_bearer_tok_99", "INBOX", jmbox, &jadded, err, sizeof(err)) == 0);
    assert(jadded == 1);
    int jadded2 = 0;
    assert(dtmail_imap_sync_folder(&jacct, "oauth_bearer_tok_99", "INBOX", jmbox, &jadded2, err, sizeof(err)) == 0);
    assert(jadded2 == 0);

    FILE *jfp = fopen(jmbox, "rb");
    assert(jfp != NULL);
    size_t jn = fread(buf, 1, sizeof(buf) - 1, jfp);
    buf[jn] = '\0';
    fclose(jfp);
    assert(strstr(buf, "Subject: Hello JMAP") != NULL);
    printf("ok    jmap-email-query-get-sync\n");

    /* Read mapped UID from jmbox.uids and verify Email/set flag update */
    char juid_path[1280];
    snprintf(juid_path, sizeof(juid_path), "%s.uids", jmbox);
    FILE *ufp = fopen(juid_path, "r");
    assert(ufp != NULL);
    unsigned long juid = 0;
    assert(fscanf(ufp, "%lu", &juid) == 1 && juid > 0);
    fclose(ufp);
    assert(dtmail_imap_set_flag(&jacct, "oauth_bearer_tok_99", "INBOX", juid, "\\Flagged", 1, err, sizeof(err)) == 0);
    printf("ok    jmap-email-set-flag\n");

    const char *jraw =
        "From: alice@fastmail.test\r\n"
        "To: bob@example.test\r\n"
        "Subject: JMAP Outgoing\r\n"
        "\r\n"
        "Hello over JMAP Submission!\r\n";
    assert(dtmail_smtp_send(&jacct, "oauth_bearer_tok_99", "alice@fastmail.test",
                            rcpts, 1, jraw, strlen(jraw), err, sizeof(err)) == 0);
    printf("ok    jmap-upload-import-submission\n");

    return 0;
}
"""


def run_mock_imap(srv: socket.socket, stop_evt: threading.Event) -> None:
    sample_msg = (
        b"From: Bob <bob@example.test>\r\n"
        b"To: Alice <alice@example.test>\r\n"
        b"Subject: Hello IMAP\r\n"
        b"\r\n"
        b"First line\r\n"
        b"From the team\r\n"
    )
    srv.settimeout(0.5)
    while not stop_evt.is_set():
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        with conn:
            conn.sendall(b"* OK [CAPABILITY IMAP4rev1] Mock IMAP ready\r\n")
            buf = b""
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                buf += data
                while b"\r\n" in buf:
                    line, buf = buf.split(b"\r\n", 1)
                    parts = line.decode("utf-8", "replace").split()
                    if len(parts) < 2:
                        continue
                    tag, cmd = parts[0], parts[1].upper()
                    rest = " ".join(parts[2:])
                    if cmd == "CAPABILITY":
                        conn.sendall(b"* CAPABILITY IMAP4rev1\r\n")
                        conn.sendall(f"{tag} OK CAPABILITY completed\r\n".encode())
                    elif cmd in ("LOGIN", "AUTHENTICATE"):
                        conn.sendall(f"{tag} OK Logged in\r\n".encode())
                    elif cmd == "NOOP":
                        conn.sendall(f"{tag} OK NOOP completed\r\n".encode())
                    elif cmd == "LIST":
                        conn.sendall(b'* LIST (\\HasNoChildren) "/" "INBOX"\r\n')
                        conn.sendall(b'* LIST (\\HasNoChildren \\Sent) "/" "Sent"\r\n')
                        conn.sendall(f"{tag} OK LIST completed\r\n".encode())
                    elif cmd in ("SELECT", "EXAMINE"):
                        conn.sendall(b"* 1 EXISTS\r\n")
                        conn.sendall(f"{tag} OK SELECT completed\r\n".encode())
                    elif cmd == "UID":
                        sub = rest.upper()
                        if sub.startswith("SEARCH"):
                            conn.sendall(b"* SEARCH 42\r\n")
                            conn.sendall(f"{tag} OK SEARCH completed\r\n".encode())
                        elif sub.startswith("FETCH"):
                            hdr = f"* 1 FETCH (UID 42 BODY[] {{{len(sample_msg)}}}\r\n".encode()
                            conn.sendall(hdr + sample_msg + b")\r\n")
                            conn.sendall(f"{tag} OK FETCH completed\r\n".encode())
                        else:
                            conn.sendall(f"{tag} OK UID completed\r\n".encode())
                    elif cmd == "LOGOUT":
                        conn.sendall(b"* BYE Logging out\r\n")
                        conn.sendall(f"{tag} OK LOGOUT completed\r\n".encode())
                        break
                    else:
                        conn.sendall(f"{tag} OK\r\n".encode())


def run_mock_smtp(srv: socket.socket, stop_evt: threading.Event, received: list) -> None:
    srv.settimeout(0.5)
    while not stop_evt.is_set():
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        with conn:
            conn.sendall(b"220 mock.smtp ESMTP\r\n")
            in_data = False
            msg_lines = []
            buf = b""
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\r\n" in buf:
                    line, buf = buf.split(b"\r\n", 1)
                    if in_data:
                        if line == b".":
                            in_data = False
                            received.append(b"\r\n".join(msg_lines))
                            conn.sendall(b"250 OK queued\r\n")
                        else:
                            msg_lines.append(line)
                        continue
                    up = line.decode("utf-8", "replace").upper()
                    if up.startswith("EHLO"):
                        conn.sendall(b"250-mock.smtp\r\n250 AUTH PLAIN\r\n")
                    elif up == "AUTH PLAIN":
                        conn.sendall(b"334 \r\n")
                    elif up.startswith("AUTH "):
                        conn.sendall(b"235 Authentication successful\r\n")
                    elif up.startswith("MAIL FROM:") or up.startswith("RCPT TO:"):
                        conn.sendall(b"250 OK\r\n")
                    elif up == "DATA":
                        in_data = True
                        msg_lines = []
                        conn.sendall(b"354 End data with <CR><LF>.<CR><LF>\r\n")
                    elif up == "QUIT":
                        conn.sendall(b"221 Bye\r\n")
                        break
                    else:
                        conn.sendall(b"235 OK\r\n")


def make_jmap_handler(state: dict):
    class MockJmapHandler(BaseHTTPRequestHandler):
        def _check_auth(self) -> bool:
            if self.headers.get("Authorization") != "Bearer oauth_bearer_tok_99":
                self.send_response(401)
                self.end_headers()
                return False
            return True

        def _send_json(self, obj: dict) -> None:
            raw = json.dumps(obj).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def do_GET(self):
            if not self._check_auth():
                return
            port = self.server.server_address[1]
            if self.path == "/jmap/session":
                self._send_json({
                    "apiUrl": f"http://127.0.0.1:{port}/jmap/api",
                    "downloadUrl": f"http://127.0.0.1:{port}/jmap/download/{{accountId}}/{{blobId}}/{{name}}?type={{type}}",
                    "uploadUrl": f"http://127.0.0.1:{port}/jmap/upload/{{accountId}}/",
                    "primaryAccounts": {"urn:ietf:params:jmap:mail": "u-acct-1"},
                    "accounts": {"u-acct-1": {"name": "alice@fastmail.test"}},
                })
            elif self.path.startswith("/jmap/download/u-acct-1/B-msg-1/"):
                eml = (
                    b"From: Carol <carol@example.test>\r\n"
                    b"To: Alice <alice@fastmail.test>\r\n"
                    b"Subject: Hello JMAP\r\n"
                    b"\r\n"
                    b"Body delivered over RFC 8621 JMAP.\r\n"
                )
                self.send_response(200)
                self.send_header("Content-Type", "message/rfc822")
                self.send_header("Content-Length", str(len(eml)))
                self.end_headers()
                self.wfile.write(eml)
            else:
                self.send_response(404)
                self.end_headers()

        def do_POST(self):
            if not self._check_auth():
                return
            ln = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(ln)
            if self.path.startswith("/jmap/upload/u-acct-1/"):
                state["uploaded"].append(body)
                self._send_json({"accountId": "u-acct-1", "blobId": "B-up-99", "size": len(body)})
                return
            req = json.loads(body.decode("utf-8"))
            responses = []
            for name, args, call_id in req.get("methodCalls", []):
                if name == "Mailbox/get":
                    responses.append(["Mailbox/get", {
                        "accountId": "u-acct-1",
                        "list": [
                            {"id": "mb-inbox", "name": "Inbox", "role": "inbox"},
                            {"id": "mb-sent", "name": "Sent", "role": "sent"},
                        ],
                    }, call_id])
                elif name == "Email/query":
                    responses.append(["Email/query", {"accountId": "u-acct-1", "ids": ["M-email-77"]}, call_id])
                elif name == "Email/get":
                    responses.append(["Email/get", {
                        "accountId": "u-acct-1",
                        "list": [{"id": "M-email-77", "blobId": "B-msg-1"}],
                    }, call_id])
                elif name == "Email/set":
                    state["flag_updates"].append(args.get("update", {}))
                    responses.append(["Email/set", {"accountId": "u-acct-1", "updated": {"M-email-77": None}}, call_id])
                elif name == "Identity/get":
                    responses.append(["Identity/get", {
                        "accountId": "u-acct-1",
                        "list": [{"id": "id-alice", "email": "alice@fastmail.test"}],
                    }, call_id])
                elif name == "Email/import":
                    responses.append(["Email/import", {
                        "accountId": "u-acct-1",
                        "created": {"m0": {"id": "M-sent-99", "blobId": "B-up-99"}},
                    }, call_id])
                elif name == "EmailSubmission/set":
                    state["submissions"].append(args.get("create", {}))
                    responses.append(["EmailSubmission/set", {
                        "accountId": "u-acct-1",
                        "created": {"sub0": {"id": "sub-1"}},
                    }, call_id])
            self._send_json({"methodResponses": responses})

        def log_message(self, fmt, *args):
            pass

    return MockJmapHandler


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    dtmail_root = os.path.dirname(test_dir)
    dtmail_src = os.path.join(dtmail_root, "dtmail")
    cxx = os.environ.get("CXX", "clang++")

    imap_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    imap_srv.bind(("127.0.0.1", 0))
    imap_srv.listen(4)
    imap_port = imap_srv.getsockname()[1]

    smtp_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    smtp_srv.bind(("127.0.0.1", 0))
    smtp_srv.listen(4)
    smtp_port = smtp_srv.getsockname()[1]

    jmap_state = {"uploaded": [], "flag_updates": [], "submissions": []}
    jmap_srv = HTTPServer(("127.0.0.1", 0), make_jmap_handler(jmap_state))
    jmap_port = jmap_srv.server_address[1]

    stop_evt = threading.Event()
    received = []
    t1 = threading.Thread(target=run_mock_imap, args=(imap_srv, stop_evt), daemon=True)
    t2 = threading.Thread(target=run_mock_smtp, args=(smtp_srv, stop_evt, received), daemon=True)
    t3 = threading.Thread(target=jmap_srv.serve_forever, daemon=True)
    t1.start()
    t2.start()
    t3.start()

    try:
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "curl-mail-test.cc")
            exe = os.path.join(td, "curl-mail-test")
            with open(src, "w", encoding="utf-8") as f:
                f.write(C_TEST)
            subprocess.run(
                [cxx, "-O1", "-g", "-DHAVE_LIBCURL=1", "-fsanitize=address,undefined",
                 f"-I{dtmail_src}", src,
                 os.path.join(dtmail_src, "CurlMailEngine.C"),
                 os.path.join(dtmail_src, "MailAccountStore.C"),
                 os.path.join(dtmail_src, "MailThreadRules.C"),
                 "-lcurl", "-o", exe],
                check=True,
            )
            env = dict(os.environ)
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([exe, str(imap_port), str(smtp_port), str(jmap_port), td], env=env, check=True)
        assert len(received) == 1 and b"Hello over SMTP!" in received[0]
        assert len(jmap_state["uploaded"]) == 1 and b"Hello over JMAP Submission!" in jmap_state["uploaded"][0]
        assert jmap_state["flag_updates"] == [{"M-email-77": {"keywords/$flagged": True}}]
        assert len(jmap_state["submissions"]) == 1
    finally:
        stop_evt.set()
        jmap_srv.shutdown()
        imap_srv.close()
        smtp_srv.close()

    print("check-curl-mail: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
