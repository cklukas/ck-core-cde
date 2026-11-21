/*
 * CDE - Common Desktop Environment
 *
 * Optional SSH Unix-socket forwarding (StreamLocalForward) for remote
 * CDE Calendar Manager (rpc.cmsd) connections.
 *
 * Connects to a remote host's /run/rpc.cmsd/cmsd.sock over SSH without
 * exposing any TCP/UDP calendar ports on the LAN.
 */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ssh_tunnel.h"

#define MAX_SSH_TUNNELS 8
#define DEFAULT_REMOTE_CMSD_SOCK "/run/rpc.cmsd/cmsd.sock"

typedef struct {
    char host[256];
    char dir[256];
    char sock[320];
    pid_t pid;
} SshTunnelEntry;

static SshTunnelEntry g_tunnels[MAX_SSH_TUNNELS];
static int g_atexit_registered = 0;

int
_DtCm_ssh_tunnel_validate_host(const char *host, char *clean_out, size_t clean_sz)
{
    const char *p;

    if (host == NULL || host[0] == '\0')
        return -1;

    p = host;
    if (strncmp(p, "ssh://", 6) == 0)
        p += 6;

    if (p[0] == '\0' || p[0] == '-')
        return -1;

    size_t len = strlen(p);
    if (len >= 255)
        return -1;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || c == '@'))
            return -1;
    }

    if (clean_out && clean_sz > 0)
        snprintf(clean_out, clean_sz, "%s", p);
    return 0;
}

int
_DtCm_ssh_tunnel_requested(const char *host)
{
    if (host != NULL && strncmp(host, "ssh://", 6) == 0)
        return 1;
    const char *env = getenv("DTCM_SSH_TUNNEL");
    if (env != NULL && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0 ||
                        strcmp(env, "yes") == 0))
        return 1;
    return 0;
}

static void
cleanup_entry(SshTunnelEntry *e)
{
    if (e->pid > 0) {
        (void)kill(e->pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 20; i++) {
            pid_t r = waitpid(e->pid, &status, WNOHANG);
            if (r == e->pid || r < 0)
                break;
            usleep(25000);
        }
        if (kill(e->pid, 0) == 0) {
            (void)kill(e->pid, SIGKILL);
            (void)waitpid(e->pid, &status, WNOHANG);
        }
        e->pid = 0;
    }
    if (e->sock[0]) {
        (void)unlink(e->sock);
        e->sock[0] = '\0';
    }
    if (e->dir[0]) {
        (void)rmdir(e->dir);
        e->dir[0] = '\0';
    }
    e->host[0] = '\0';
}

void
_DtCm_ssh_tunnel_close_all(void)
{
    for (int i = 0; i < MAX_SSH_TUNNELS; i++) {
        if (g_tunnels[i].pid > 0 || g_tunnels[i].dir[0])
            cleanup_entry(&g_tunnels[i]);
    }
}

int
_DtCm_ssh_tunnel_open(const char *host, char *local_sock_out, size_t sock_sz)
{
    char clean_host[256];
    struct stat st;

    if (!local_sock_out || sock_sz == 0)
        return -1;
    local_sock_out[0] = '\0';

    if (_DtCm_ssh_tunnel_validate_host(host, clean_host, sizeof(clean_host)) != 0)
        return -1;

    /* Reuse an active tunnel for this host if already open */
    for (int i = 0; i < MAX_SSH_TUNNELS; i++) {
        if (g_tunnels[i].pid > 0 && strcmp(g_tunnels[i].host, clean_host) == 0) {
            if (kill(g_tunnels[i].pid, 0) == 0 &&
                stat(g_tunnels[i].sock, &st) == 0 && S_ISSOCK(st.st_mode)) {
                snprintf(local_sock_out, sock_sz, "%s", g_tunnels[i].sock);
                return 0;
            }
            cleanup_entry(&g_tunnels[i]);
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_SSH_TUNNELS; i++) {
        if (g_tunnels[i].pid == 0 && g_tunnels[i].dir[0] == '\0') {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Evict oldest slot */
        slot = 0;
        cleanup_entry(&g_tunnels[slot]);
    }

    const char *base_dir = getenv("XDG_RUNTIME_DIR");
    if (!base_dir || !base_dir[0] || stat(base_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        base_dir = getenv("TMPDIR");
        if (!base_dir || !base_dir[0])
            base_dir = "/tmp";
    }

    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "%s/dtcm-ssh-XXXXXX", base_dir);
    if (mkdtemp(tmpl) == NULL)
        return -1;
    (void)chmod(tmpl, 0700);

    char sock_path[320];
    snprintf(sock_path, sizeof(sock_path), "%s/cmsd.sock", tmpl);

    const char *remote_sock = getenv("DTCM_REMOTE_UNIX_SOCK");
    if (!remote_sock || !remote_sock[0])
        remote_sock = DEFAULT_REMOTE_CMSD_SOCK;

    char forward_arg[700];
    snprintf(forward_arg, sizeof(forward_arg), "%s:%s", sock_path, remote_sock);

    const char *ssh_bin = getenv("DTCM_SSH_BIN");
    if (!ssh_bin || !ssh_bin[0])
        ssh_bin = "ssh";

    pid_t pid = fork();
    if (pid < 0) {
        (void)rmdir(tmpl);
        return -1;
    }
    if (pid == 0) {
        char *argv[] = {
            (char *)ssh_bin,
            "-N",
            "-T",
            "-o", "BatchMode=yes",
            "-o", "ExitOnForwardFailure=yes",
            "-o", "StreamLocalBindMask=0177",
            "-o", "StreamLocalBindUnlink=yes",
            "-o", "ConnectTimeout=10",
            "-L", forward_arg,
            "--",
            clean_host,
            NULL
        };
        execvp(ssh_bin, argv);
        _exit(127);
    }

    if (!g_atexit_registered) {
        atexit(_DtCm_ssh_tunnel_close_all);
        g_atexit_registered = 1;
    }

    snprintf(g_tunnels[slot].host, sizeof(g_tunnels[slot].host), "%s", clean_host);
    snprintf(g_tunnels[slot].dir, sizeof(g_tunnels[slot].dir), "%s", tmpl);
    snprintf(g_tunnels[slot].sock, sizeof(g_tunnels[slot].sock), "%s", sock_path);
    g_tunnels[slot].pid = pid;

    /* Wait up to 5 seconds for ssh to create the forwarded local socket */
    for (int i = 0; i < 100; i++) {
        if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            snprintf(local_sock_out, sock_sz, "%s", sock_path);
            return 0;
        }
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            g_tunnels[slot].pid = 0;
            cleanup_entry(&g_tunnels[slot]);
            return -1;
        }
        usleep(50000);
    }

    cleanup_entry(&g_tunnels[slot]);
    return -1;
}
