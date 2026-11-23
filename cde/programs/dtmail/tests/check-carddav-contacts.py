#!/usr/bin/env python3
"""Run the CardDAV response decoder against successful and rejected snapshots."""

import base64
import http.server
import os
from pathlib import Path
import shlex
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading


def check_https(exe, work):
    if not shutil.which("openssl"):
        print("SKIP CardDAV HTTPS exercise: openssl is required for the fixture")
        return
    pem, key = work / "cert.pem", work / "key.pem"
    subprocess.run([
        "openssl", "req", "-new", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1",
        "-keyout", key, "-out", pem,
    ], check=True, capture_output=True)
    secret = work / "secret-tool"
    secret.write_text("#!/bin/sh\nprintf '%s\\n' fixture-password\n", encoding="ascii")
    secret.chmod(0o700)
    credentials = "Basic " + base64.b64encode(b"fixture:fixture-password").decode("ascii")
    succeeded = threading.Event()
    addressbook = b"""<D:multistatus xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav">
<D:response><D:href>/book/remote.vcf</D:href><D:propstat><D:prop><C:address-data>BEGIN:VCARD
VERSION:4.0
FN:Remote Contact
EMAIL:remote@example.test
END:VCARD
</C:address-data></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>
</D:multistatus>"""

    class CardDavHandler(http.server.BaseHTTPRequestHandler):
        def do_REPORT(self):
            body = self.rfile.read(int(self.headers["Content-Length"]))
            if self.headers.get("Authorization") != credentials:
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="Fixture"')
                self.end_headers()
                return
            if (self.path != "/book/" or self.headers.get("Depth") != "1"
                    or b"<C:addressbook-query" not in body or b"<C:address-data" not in body):
                self.send_error(400)
                return
            self.send_response(207)
            self.send_header("Content-Length", str(len(addressbook)))
            self.end_headers()
            self.wfile.write(addressbook)
            succeeded.set()

        def log_message(self, *_):
            pass

    class FixtureServer(http.server.ThreadingHTTPServer):
        def handle_error(self, request, address):
            # During the untrusted-CA test, libcurl can reset a TLS connection
            # before sending an HTTP request.
            if not isinstance(sys.exc_info()[1], ConnectionResetError):
                super().handle_error(request, address)

    with FixtureServer(("127.0.0.1", 0), CardDavHandler) as server:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(pem, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            env = {**os.environ, "DTMAIL_KEYRING_CMD": str(secret), "NO_PROXY": "127.0.0.1"}
            endpoint = f"https://127.0.0.1:{server.server_port}/book/"
            subprocess.run([exe, endpoint, pem, "untrusted"], env=env, check=True)
            assert not succeeded.is_set()
            subprocess.run([exe, endpoint, pem, "trusted"], env=env, check=True)
            assert succeeded.is_set()
        finally:
            server.shutdown()
            worker.join()
    print("PASS CardDAV authenticated HTTPS REPORT, CA validation, and autocomplete result")


def main():
    src = Path(__file__).resolve().parent
    app = src.parent / "dtmail"
    pkg = ["pkg-config", "libcurl >= 7.85", "libxml-2.0"]
    if subprocess.run(pkg + ["--exists"], check=False).returncode:
        print("SKIP CardDAV test: libcurl >= 7.85 and libxml2 are required")
        return
    flags = shlex.split(subprocess.check_output(pkg + ["--cflags", "--libs"], text=True))
    with tempfile.TemporaryDirectory(prefix="dtmail-carddav-") as work:
        exe = Path(work) / "carddav"
        subprocess.run([
            *shlex.split(os.environ.get("CXX", "c++")),
            "-std=c++17", "-Wall", "-Wextra", "-DDTMAIL_CARDDAV_ENABLED=1",
            "-pthread", "-I", str(app), str(src / "check-carddav-contacts.cc"),
            str(app / "CardDavContacts.C"), str(app / "MailAccountStore.C"),
            "-o", str(exe), *flags,
        ], check=True)
        subprocess.run([exe], check=True)
        check_https(exe, Path(work))
    print("PASS CardDAV collection atomicity, vCard unfolding and recipient parsing")


if __name__ == "__main__":
    main()
