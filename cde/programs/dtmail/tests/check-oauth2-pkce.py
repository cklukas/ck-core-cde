#!/usr/bin/env python3
"""ASan/UBSan tests for Phase 3: OAuth2 PKCE, loopback callback, token exchange,
iTIP .ics invitation parser, and AddressAutocomplete 0600 store."""

import os
import subprocess
import sys
import tempfile
import threading
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
DTMAIL_DIR = os.path.join(os.path.dirname(HERE), "dtmail")
CXX = os.environ.get("CXX", "clang++")


class MockTokenHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", errors="replace")
        if "grant_type=authorization_code" in body and "code=test_auth_code_123" in body:
            resp = b'{"access_token":"tok_access_1","refresh_token":"tok_refresh_1","expires_in":3600}'
            self.send_response(200)
        elif "grant_type=refresh_token" in body and "refresh_token=tok_refresh_1" in body:
            resp = b'{"access_token":"tok_access_2","expires_in":1800}'
            self.send_response(200)
        else:
            resp = b'{"error":"invalid_grant"}'
            self.send_response(400)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, fmt, *args):
        pass


HARNESS = r'''
#include "OAuth2Client.h"
#include "ItipBanner.h"
#include "AddressAutocomplete.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static void test_pkce_rfc7636_vector(void) {
    /* RFC 7636 Appendix B test vector */
    const char *verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    char challenge[128] = "";
    assert(dtmail_oauth2_compute_challenge(verifier, challenge, sizeof(challenge)) == 0);
    assert(strcmp(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") == 0);

    DtMailPkcePair pair;
    assert(dtmail_oauth2_generate_pkce(&pair) == 0);
    assert(strlen(pair.code_verifier) >= 43);
    assert(strlen(pair.code_challenge) == 43);
    assert(strlen(pair.state) >= 20);
    puts("ok    pkce-rfc7636-vector");
}

typedef struct {
    int port;
    char state[64];
} LoopbackClientArg;

static void *send_loopback_cb(void *arg) {
    LoopbackClientArg *a = (LoopbackClientArg *)arg;
    usleep(50000);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)a->port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        char req[512];
        snprintf(req, sizeof(req),
                 "GET /callback?code=test_auth_code_123&state=%s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                 a->state);
        (void)write(fd, req, strlen(req));
        char resp[512];
        (void)read(fd, resp, sizeof(resp));
    }
    close(fd);
    return NULL;
}

static void test_loopback_and_token_exchange(const char *token_url) {
    DtMailPkcePair pair;
    assert(dtmail_oauth2_generate_pkce(&pair) == 0);

    int port = 0;
    int lfd = dtmail_oauth2_loopback_listen(&port);
    assert(lfd >= 0 && port > 0);

    DtMailAccount acct;
    dtmail_account_apply_preset(&acct, "gmail");
    snprintf(acct.oauth_client_id, sizeof(acct.oauth_client_id), "test-client-id");
    snprintf(acct.oauth_token_url, sizeof(acct.oauth_token_url), "%s", token_url);

    char auth_url[1024] = "";
    assert(dtmail_oauth2_build_auth_url(&acct, &pair, port, auth_url, sizeof(auth_url)) == 0);
    assert(strstr(auth_url, "code_challenge_method=S256") != NULL);
    assert(strstr(auth_url, pair.code_challenge) != NULL);

    LoopbackClientArg carg;
    carg.port = port;
    snprintf(carg.state, sizeof(carg.state), "%s", pair.state);
    pthread_t th;
    assert(pthread_create(&th, NULL, send_loopback_cb, &carg) == 0);

    char code[256] = "";
    assert(dtmail_oauth2_loopback_wait_code(lfd, pair.state, 5, code, sizeof(code)) == 0);
    assert(strcmp(code, "test_auth_code_123") == 0);
    pthread_join(th, NULL);
    close(lfd);

    char redir[128];
    snprintf(redir, sizeof(redir), "http://127.0.0.1:%d/callback", port);
    DtMailOAuthTokens tok;
    char err[256] = "";
    assert(dtmail_oauth2_exchange_code(&acct, code, pair.code_verifier, redir, &tok, err, sizeof(err)) == 0);
    assert(strcmp(tok.access_token, "tok_access_1") == 0);
    assert(strcmp(tok.refresh_token, "tok_refresh_1") == 0);
    assert(tok.expires_in == 3600);

    DtMailOAuthTokens tok2;
    assert(dtmail_oauth2_refresh_token(&acct, tok.refresh_token, &tok2, err, sizeof(err)) == 0);
    assert(strcmp(tok2.access_token, "tok_access_2") == 0);
    assert(tok2.expires_in == 1800);
    puts("ok    loopback-and-token-exchange");
}

static void test_itip_parse_and_save(const char *tmp_home) {
    setenv("HOME", tmp_home, 1);
    setenv("DTMAIL_NO_LAUNCH_DTCM", "1", 1);

    const char *ics =
        "Content-Type: text/calendar; method=REQUEST\r\n\r\n"
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "METHOD:REQUEST\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:evt-42@example.com\r\n"
        "SUMMARY:Design Sync\r\n"
        "DTSTART:20261124T150000Z\r\n"
        "DTEND:20261124T160000Z\r\n"
        "LOCATION:Studio 2\r\n"
        "ORGANIZER:mailto:alice@example.com\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n";

    DtMailItipInvite inv;
    assert(dtmail_itip_parse(ics, strlen(ics), &inv) == 0);
    assert(inv.valid == 1);
    assert(strcmp(inv.method, "REQUEST") == 0);
    assert(strcmp(inv.summary, "Design Sync") == 0);
    assert(strcmp(inv.location, "Studio 2") == 0);
    assert(strcmp(inv.dtstart, "20261124T150000Z") == 0);

    char saved[1024] = "";
    assert(dtmail_itip_open_in_dtcm(ics, strlen(ics), saved, sizeof(saved)) == 0);
    struct stat st;
    assert(stat(saved, &st) == 0);
    assert((st.st_mode & 0777) == 0600);
    puts("ok    itip-parse-and-save");
}

static void test_contacts_autocomplete(const char *tmp_home) {
    char cpath[1024];
    snprintf(cpath, sizeof(cpath), "%s/.dt/dtmail_contacts", tmp_home);
    dtmail_contacts_shutdown();
    assert(dtmail_contacts_harvest_header("Alice Rivera <alice@example.com>, bob@example.com") == 2);
    assert(dtmail_contacts_harvest_header("alice@example.com") == 1); /* dedup */
    assert(dtmail_contacts_count() == 2);
    assert(dtmail_contacts_save_file(cpath) == 0);

    struct stat st;
    assert(stat(cpath, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    dtmail_contacts_shutdown();
    assert(dtmail_contacts_load_file(cpath) == 0);
    assert(dtmail_contacts_count() == 2);

    char matches[8][256];
    int n = dtmail_contacts_match("ali", matches, 8);
    assert(n == 1);
    assert(strcmp(matches[0], "Alice Rivera <alice@example.com>") == 0);
    dtmail_contacts_shutdown();
    puts("ok    contacts-autocomplete-0600");
}

int main(int argc, char **argv) {
    assert(argc >= 3);
    test_pkce_rfc7636_vector();
    test_loopback_and_token_exchange(argv[1]);
    test_itip_parse_and_save(argv[2]);
    test_contacts_autocomplete(argv[2]);
    puts("check-oauth2-pkce: all assertions passed");
    return 0;
}
'''


def main():
    srv = HTTPServer(("127.0.0.1", 0), MockTokenHandler)
    tport = srv.server_address[1]
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()

    with tempfile.TemporaryDirectory(prefix="dtmail-p3-") as td:
        harness_c = os.path.join(td, "harness.C")
        bin_path = os.path.join(td, "test_p3")
        with open(harness_c, "w") as f:
            f.write(HARNESS)

        curl_cflags = subprocess.check_output(["pkg-config", "--cflags", "libcurl"], text=True).split()
        curl_libs = subprocess.check_output(["pkg-config", "--libs", "libcurl"], text=True).split()

        cmd = [
            CXX, "-std=c++11", "-g", "-O1",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-DHAVE_LIBCURL=1",
            "-DDTMAIL_ITIP_NO_GUI=1",
            f"-I{DTMAIL_DIR}",
            *curl_cflags,
            harness_c,
            os.path.join(DTMAIL_DIR, "MailAccountStore.C"),
            os.path.join(DTMAIL_DIR, "OAuth2Client.C"),
            os.path.join(DTMAIL_DIR, "ItipBanner.C"),
            os.path.join(DTMAIL_DIR, "AddressAutocomplete.C"),
            *curl_libs,
            "-lpthread",
            "-o", bin_path,
        ]
        subprocess.run(cmd, check=True)

        home_dir = os.path.join(td, "home")
        os.makedirs(home_dir, exist_ok=True)
        subprocess.run([bin_path, f"http://127.0.0.1:{tport}/token", home_dir], check=True)

    srv.shutdown()


if __name__ == "__main__":
    main()
