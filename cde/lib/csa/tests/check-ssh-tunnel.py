#!/usr/bin/env python3
"""Standalone ASan/UBSan test for libcsa's SSH Unix-socket forwarding (ssh_tunnel.c).

Tests:
1. Hostname validation (rejects leading '-', shell metacharacters, whitespace,
   empty strings; accepts valid hostnames, user@host, and ssh://host).
2. Tunnel creation with a mock DTCM_SSH_BIN that creates the forwarded AF_UNIX
   socket: verifies 0700 directory permissions, tunnel reuse, end-to-end data
   over the forwarded socket, and clean SIGTERM + directory cleanup on close.
"""

import os
import stat
import subprocess
import sys
import tempfile

MOCK_SSH = r"""#!/usr/bin/env python3
import os, signal, socket, sys

# Parse -L <local_sock>:<remote_sock>
local_sock = None
for i, arg in enumerate(sys.argv):
    if arg == "-L" and i + 1 < len(sys.argv):
        local_sock = sys.argv[i + 1].split(":", 1)[0]
if not local_sock:
    sys.exit(2)

if os.path.exists(local_sock):
    os.unlink(local_sock)

srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(local_sock)
os.chmod(local_sock, 0o600)
srv.listen(4)

def on_term(signum, frame):
    srv.close()
    sys.exit(0)

signal.signal(signal.SIGTERM, on_term)

while True:
    try:
        conn, _ = srv.accept()
        data = conn.recv(64)
        conn.sendall(b"PONG:" + data)
        conn.close()
    except OSError:
        break
"""

C_HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "ssh_tunnel.h"

int main(void)
{
    char clean[256];

    /* 1. Hostname validation */
    assert(_DtCm_ssh_tunnel_validate_host("", clean, sizeof(clean)) != 0);
    assert(_DtCm_ssh_tunnel_validate_host("-oProxyCommand=id", clean, sizeof(clean)) != 0);
    assert(_DtCm_ssh_tunnel_validate_host("ssh://-evil", clean, sizeof(clean)) != 0);
    assert(_DtCm_ssh_tunnel_validate_host("host;id", clean, sizeof(clean)) != 0);
    assert(_DtCm_ssh_tunnel_validate_host("host name", clean, sizeof(clean)) != 0);
    assert(_DtCm_ssh_tunnel_validate_host("host/path", clean, sizeof(clean)) != 0);
    assert(_DtCm_ssh_tunnel_validate_host("workstation.example.com", clean, sizeof(clean)) == 0);
    assert(strcmp(clean, "workstation.example.com") == 0);
    assert(_DtCm_ssh_tunnel_validate_host("ssh://alice@cal-host-01.local", clean, sizeof(clean)) == 0);
    assert(strcmp(clean, "alice@cal-host-01.local") == 0);
    printf("ok    hostname-validation\n");

    /* 2. Request detection */
    unsetenv("DTCM_SSH_TUNNEL");
    assert(_DtCm_ssh_tunnel_requested("example.com") == 0);
    assert(_DtCm_ssh_tunnel_requested("ssh://example.com") == 1);
    setenv("DTCM_SSH_TUNNEL", "1", 1);
    assert(_DtCm_ssh_tunnel_requested("example.com") == 1);
    printf("ok    request-detection\n");

    /* 3. Open tunnel, check 0700 dir, verify reuse & round-trip */
    char sock1[320] = "";
    char sock2[320] = "";
    assert(_DtCm_ssh_tunnel_open("ssh://cal.example.test", sock1, sizeof(sock1)) == 0);
    assert(_DtCm_ssh_tunnel_open("cal.example.test", sock2, sizeof(sock2)) == 0);
    assert(strcmp(sock1, sock2) == 0);

    char dir[320];
    snprintf(dir, sizeof(dir), "%s", sock1);
    char *slash = strrchr(dir, '/');
    assert(slash != NULL);
    *slash = '\0';
    struct stat st;
    assert(stat(dir, &st) == 0);
    assert((st.st_mode & 0777) == 0700);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock1);
    assert(connect(fd, (struct sockaddr *)&sun, sizeof(sun)) == 0);
    assert(write(fd, "HELLO", 5) == 5);
    char reply[64] = "";
    ssize_t n = read(fd, reply, sizeof(reply) - 1);
    close(fd);
    assert(n == 10 && strcmp(reply, "PONG:HELLO") == 0);
    printf("ok    tunnel-open-reuse-and-io\n");

    /* 4. Cleanup */
    _DtCm_ssh_tunnel_close_all();
    assert(stat(sock1, &st) != 0);
    assert(stat(dir, &st) != 0);
    printf("ok    tunnel-cleanup\n");

    return 0;
}
"""


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    csa_dir = os.path.dirname(test_dir)
    cc = os.environ.get("CC", "clang")

    with tempfile.TemporaryDirectory() as td:
        mock_ssh = os.path.join(td, "mock-ssh")
        with open(mock_ssh, "w", encoding="utf-8") as f:
            f.write(MOCK_SSH)
        os.chmod(mock_ssh, 0o755)

        harness_c = os.path.join(td, "ssh-tunnel-test.c")
        exe = os.path.join(td, "ssh-tunnel-test")
        with open(harness_c, "w", encoding="utf-8") as f:
            f.write(C_HARNESS)

        subprocess.run(
            [
                cc, "-O1", "-g", "-fsanitize=address,undefined",
                f"-I{csa_dir}",
                harness_c,
                os.path.join(csa_dir, "ssh_tunnel.c"),
                "-o", exe,
            ],
            check=True,
        )

        env = dict(os.environ)
        env["DTCM_SSH_BIN"] = mock_ssh
        env["XDG_RUNTIME_DIR"] = td
        env["TMPDIR"] = td
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        subprocess.run([exe], env=env, check=True)

    print("check-ssh-tunnel: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
