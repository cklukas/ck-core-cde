/*
 * CDE - Common Desktop Environment
 *
 * Calendar Manager (dtcm) Server & Network Status dialog.
 * Reports live rpc.cmsd / rpcbind reachability, local vs. remote calendar
 * target, listening scope (loopback-only vs. LAN), systemd service state,
 * and calendar spool file status.
 */

#include <EUSCompat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>

#include <Xm/Xm.h>
#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/SeparatoG.h>
#include <Xm/Text.h>
#include <Xm/ToggleBG.h>

#include "server_status.h"

#define CMSD_RPC_PROG   100068UL
#define CMSD_RPC_VERS_5 5UL
#define CMSD_RPC_VERS_4 4UL
#define CMSD_UNIX_SOCK  "/run/rpc.cmsd/cmsd.sock"

static Widget g_status_dialog = nullptr;
static Widget g_status_text = nullptr;
static Widget g_ssh_toggle = nullptr;

static int
run_cmd_capture(char *const argv[], char *out, size_t out_sz)
{
    int pipefd[2];
    pid_t pid;
    size_t total = 0;
    int status = 0;

    if (!out || out_sz == 0)
        return -1;
    out[0] = '\0';

    if (pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);
    while (total + 1 < out_sz) {
        ssize_t n = read(pipefd[0], out + total, out_sz - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    out[total] = '\0';
    close(pipefd[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (errno == ECHILD) {
            while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r'))
                out[--total] = '\0';
            return (total > 0) ? 0 : -1;
        }
        return -1;
    }
    while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r'))
        out[--total] = '\0';

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void
split_calname(const char *calname, char *user_out, size_t user_sz,
              char *host_out, size_t host_sz)
{
    const char *at;

    user_out[0] = '\0';
    host_out[0] = '\0';

    if (!calname || !calname[0])
        return;

    at = strchr(calname, '@');
    if (at) {
        size_t ulen = (size_t)(at - calname);
        if (ulen >= user_sz)
            ulen = user_sz - 1;
        memcpy(user_out, calname, ulen);
        user_out[ulen] = '\0';
        snprintf(host_out, host_sz, "%s", at + 1);
    } else {
        snprintf(user_out, user_sz, "%s", calname);
    }
}

static int
is_local_host(const char *host)
{
    char local_host[256];

    if (!host || !host[0])
        return 1;
    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0 ||
        strcmp(host, "::1") == 0)
        return 1;

    memset(local_host, 0, sizeof(local_host));
    if (gethostname(local_host, sizeof(local_host) - 1) == 0) {
        if (strcmp(host, local_host) == 0)
            return 1;
        /* Match short hostname before first dot */
        char *dot = strchr(local_host, '.');
        if (dot) {
            *dot = '\0';
            if (strcmp(host, local_host) == 0)
                return 1;
        }
    }
    return 0;
}

static int
resolve_ipv4(const char *host, struct sockaddr_in *sin_out, char *ip_str, size_t ip_sz)
{
    struct addrinfo hints;
    struct addrinfo *res = nullptr;
    struct addrinfo *p;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res)
        return 0;

    for (p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            memcpy(sin_out, p->ai_addr, sizeof(*sin_out));
            if (ip_str && ip_sz > 0)
                inet_ntop(AF_INET, &sin_out->sin_addr, ip_str, (socklen_t)ip_sz);
            freeaddrinfo(res);
            return 1;
        }
    }
    freeaddrinfo(res);
    return 0;
}

static int
probe_cmsd_unix_nullproc(const char *sock_path, unsigned long *vers_out)
{
#if defined(OPT_TIRPC) && !defined(SunOS)
    static const unsigned long versions[] = { CMSD_RPC_VERS_5, CMSD_RPC_VERS_4 };
    struct sockaddr_un sun;
    struct timeval tv;

    if (!sock_path || !sock_path[0] || strlen(sock_path) >= sizeof(sun.sun_path))
        return 0;

    tv.tv_sec = 2;
    tv.tv_usec = 0;

    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        int sock = RPC_ANYSOCK;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);
        CLIENT *cl = clntunix_create(&sun, CMSD_RPC_PROG, versions[i], &sock, 0, 0);
        if (!cl)
            continue;
        enum clnt_stat st = clnt_call(cl, NULLPROC, (xdrproc_t)xdr_void, nullptr,
                                      (xdrproc_t)xdr_void, nullptr, tv);
        clnt_destroy(cl);
        if (st == RPC_SUCCESS) {
            if (vers_out)
                *vers_out = versions[i];
            return 1;
        }
    }
#else
    (void)sock_path;
    (void)vers_out;
#endif
    return 0;
}

static int
probe_cmsd_nullproc(const char *host, const char *proto, unsigned long *vers_out)
{
    static const unsigned long versions[] = { CMSD_RPC_VERS_5, CMSD_RPC_VERS_4 };
    struct timeval tv;

    tv.tv_sec = 2;
    tv.tv_usec = 0;

    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        CLIENT *cl = clnt_create(host, CMSD_RPC_PROG, versions[i], proto);
        if (!cl)
            continue;
        enum clnt_stat st = clnt_call(cl, NULLPROC, (xdrproc_t)xdr_void, nullptr,
                                      (xdrproc_t)xdr_void, nullptr, tv);
        clnt_destroy(cl);
        if (st == RPC_SUCCESS) {
            if (vers_out)
                *vers_out = versions[i];
            return 1;
        }
    }
    return 0;
}

#if defined(__linux__)
/*
 * Inspect /proc/net/tcp or /proc/net/udp for a specific local port.
 * Returns 1 if found and fills addr_out (e.g. "127.0.0.1" or "0.0.0.0"),
 * setting *is_loopback_ip to 1 if in 127.0.0.0/8.
 */
static int
lookup_proc_net_bind(const char *proc_path, unsigned short port,
                     char *addr_out, size_t addr_sz, int *is_loopback_ip)
{
    FILE *fp;
    char line[512];

    if (port == 0)
        return 0;

    fp = fopen(proc_path, "r");
    if (!fp)
        return 0;

    /* skip header */
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        unsigned int local_ip = 0;
        unsigned int local_port = 0;
        if (sscanf(line, " %*d: %8X:%4X ", &local_ip, &local_port) == 2) {
            if (local_port == port) {
                struct in_addr in;
                in.s_addr = local_ip;
                if (addr_out && addr_sz > 0)
                    inet_ntop(AF_INET, &in, addr_out, (socklen_t)addr_sz);
                if (is_loopback_ip)
                    *is_loopback_ip = ((ntohl(local_ip) & 0xff000000U) == 0x7f000000U);
                fclose(fp);
                return 1;
            }
        }
    }
    fclose(fp);
    return 0;
}
#endif

static void
build_status_report(Calendar *c, char *buf, size_t bufsz)
{
    char cal_user[128];
    char cal_host[256];
    char local_host[256];
    char ip_str[64];
    struct sockaddr_in sin;
    int local = 1;
    int resolved = 0;
    unsigned short udp_port = 0;
    unsigned short tcp_port = 0;
    unsigned long tcp_vers = 0;
    unsigned long udp_vers = 0;
    unsigned long unix_vers = 0;
    int tcp_ping_ok = 0;
    int udp_ping_ok = 0;
    int unix_ping_ok = 0;
    const char *unix_sock = getenv("DTCM_UNIX_SOCK");
    size_t off = 0;

    if (!unix_sock || !unix_sock[0])
        unix_sock = CMSD_UNIX_SOCK;

    memset(local_host, 0, sizeof(local_host));
    if (gethostname(local_host, sizeof(local_host) - 1) != 0)
        snprintf(local_host, sizeof(local_host), "localhost");

    split_calname(c ? c->calname : nullptr, cal_user, sizeof(cal_user),
                  cal_host, sizeof(cal_host));
    if (!cal_host[0])
        snprintf(cal_host, sizeof(cal_host), "%s", local_host);
    if (!cal_user[0]) {
        const char *env_u = getenv("USER");
        snprintf(cal_user, sizeof(cal_user), "%s", env_u ? env_u : "(unknown)");
    }

    local = is_local_host(cal_host);
    snprintf(ip_str, sizeof(ip_str), "(unresolved)");
    const char *probe_host = local ? "127.0.0.1" : cal_host;
    resolved = resolve_ipv4(probe_host, &sin, ip_str, sizeof(ip_str));

    if (local)
        unix_ping_ok = probe_cmsd_unix_nullproc(unix_sock, &unix_vers);

    if (resolved) {
        udp_port = pmap_getport(&sin, CMSD_RPC_PROG, CMSD_RPC_VERS_5, IPPROTO_UDP);
        if (udp_port == 0)
            udp_port = pmap_getport(&sin, CMSD_RPC_PROG, CMSD_RPC_VERS_4, IPPROTO_UDP);
        tcp_port = pmap_getport(&sin, CMSD_RPC_PROG, CMSD_RPC_VERS_5, IPPROTO_TCP);
        if (tcp_port == 0)
            tcp_port = pmap_getport(&sin, CMSD_RPC_PROG, CMSD_RPC_VERS_4, IPPROTO_TCP);

        if (tcp_port != 0)
            tcp_ping_ok = probe_cmsd_nullproc(probe_host, "tcp", &tcp_vers);
        if (udp_port != 0)
            udp_ping_ok = probe_cmsd_nullproc(probe_host, "udp", &udp_vers);
    }

    off += (size_t)snprintf(buf + off, bufsz - off,
        "Calendar & Connection\n"
        "------------------------------------------------------------\n"
        "  Active calendar : %s\n"
        "  Target host     : %s (%s)\n"
        "  CSA session     : %s\n"
        "\n"
        "RPC Service (rpc.cmsd)\n"
        "------------------------------------------------------------\n",
        (c && c->calname) ? c->calname : "(none)",
        cal_host,
        local ? (unix_ping_ok ? "local host via AF_UNIX" : "local host") : ip_str,
        (c && c->cal_handle != 0) ? "Connected (active CSA handle)" :
                                    "Not connected (offline / in-memory fallback)");

    if (local) {
        off += (size_t)snprintf(buf + off, bufsz - off,
            "  Unix socket     : %s (%s%s%s)\n",
            unix_sock,
            unix_ping_ok ? "responding, SO_PEERCRED enforced" : "not listening",
            unix_ping_ok ? ", v" : "",
            unix_ping_ok ? (unix_vers == 5 ? "5" : "4") : "");
    }

    int net_ports_active = (tcp_ping_ok || udp_ping_ok);
    if (net_ports_active || (!unix_ping_ok && (udp_port != 0 || tcp_port != 0))) {
        char tcp_state[64];
        char udp_state[64];

        if (tcp_port == 0)
            snprintf(tcp_state, sizeof(tcp_state), "not registered");
        else if (tcp_ping_ok)
            snprintf(tcp_state, sizeof(tcp_state), "port %u (responding, v%lu)", tcp_port, tcp_vers);
        else
            snprintf(tcp_state, sizeof(tcp_state), "port %u (not responding)", tcp_port);

        if (udp_port == 0)
            snprintf(udp_state, sizeof(udp_state), "not registered");
        else if (udp_ping_ok)
            snprintf(udp_state, sizeof(udp_state), "port %u (responding, v%lu)", udp_port, udp_vers);
        else
            snprintf(udp_state, sizeof(udp_state), "port %u (not responding)", udp_port);

        off += (size_t)snprintf(buf + off, bufsz - off,
            "  rpcbind (111)   : Registered (RPC program %lu)\n"
            "  TCP endpoint    : %s\n"
            "  UDP endpoint    : %s\n",
            CMSD_RPC_PROG, tcp_state, udp_state);
    } else if (unix_ping_ok) {
        off += (size_t)snprintf(buf + off, bufsz - off,
            "  TCP / UDP ports : None (Unix-socket-only mode; rpcbind not required)\n");
    } else {
        off += (size_t)snprintf(buf + off, bufsz - off,
            "  Status          : rpc.cmsd is NOT running\n"
            "  Action needed   : sudo systemctl enable --now rpc-cmsd.service\n");
    }

    if (local) {
        char svc_state[128];
        char show_out[512];
        char tcp_bind[64] = "none";
        char udp_bind[64] = "none";
        int tcp_lo_ip = 0;
        int udp_lo_ip = 0;
        int has_ip_deny = 0;
        int has_ip_allow_lo = 0;
        int has_priv_net = 0;
        int has_unix_only_af = 0;
        int has_net_flag = 0;
        char *argv_active[] = { "systemctl", "is-active", "rpc-cmsd.service", nullptr };
        char *argv_show[] = {
            "systemctl", "show",
            "-p", "PrivateNetwork",
            "-p", "RestrictAddressFamilies",
            "-p", "IPAddressDeny",
            "-p", "IPAddressAllow",
            "-p", "ExecStart",
            "rpc-cmsd.service", nullptr
        };

        if (run_cmd_capture(argv_active, svc_state, sizeof(svc_state)) < 0 || !svc_state[0])
            snprintf(svc_state, sizeof(svc_state), "(systemd unavailable)");

        if (run_cmd_capture(argv_show, show_out, sizeof(show_out)) == 0) {
            if (strstr(show_out, "PrivateNetwork=yes"))
                has_priv_net = 1;
            if (strstr(show_out, "RestrictAddressFamilies=AF_UNIX"))
                has_unix_only_af = 1;
            if (strstr(show_out, "IPAddressDeny=any") ||
                strstr(show_out, "IPAddressDeny=0.0.0.0/0"))
                has_ip_deny = 1;
            if (strstr(show_out, "IPAddressAllow=localhost") ||
                strstr(show_out, "IPAddressAllow=127.0.0.0/8"))
                has_ip_allow_lo = 1;
            if (strstr(show_out, " -n"))
                has_net_flag = 1;
        }

#if defined(__linux__)
        lookup_proc_net_bind("/proc/net/tcp", tcp_port, tcp_bind, sizeof(tcp_bind), &tcp_lo_ip);
        lookup_proc_net_bind("/proc/net/udp", udp_port, udp_bind, sizeof(udp_bind), &udp_lo_ip);
        if (!has_net_flag && strcmp(tcp_bind, "0.0.0.0") == 0)
            snprintf(tcp_bind, sizeof(tcp_bind), "0.0.0.0%%lo (loopback)");
        if (!has_net_flag && strcmp(udp_bind, "0.0.0.0") == 0)
            snprintf(udp_bind, sizeof(udp_bind), "0.0.0.0%%lo (loopback)");
#endif

        off += (size_t)snprintf(buf + off, bufsz - off,
            "\nNetwork Exposure & Security (Local Host)\n"
            "------------------------------------------------------------\n"
            "  systemd unit    : rpc-cmsd.service (%s)\n"
            "  Socket bind     : %s\n"
            "  Isolation       : %s\n",
            svc_state,
            (!net_ports_active && unix_ping_ok)
                ? "AF_UNIX only (zero TCP/UDP ports open)"
                : (!net_ports_active)
                    ? "none"
                    : tcp_bind,
            (has_priv_net && has_unix_only_af)
                ? "PrivateNetwork=yes + RestrictAddressFamilies=AF_UNIX + SO_PEERCRED"
                : (has_ip_deny && has_ip_allow_lo)
                    ? "systemd cgroup BPF (IPAddressDeny=any, Allow=localhost)"
                    : "standard");

        if (!net_ports_active && unix_ping_ok) {
            off += (size_t)snprintf(buf + off, bufsz - off,
                "  Reachability    : Local AF_UNIX only (SO_PEERCRED) -- zero LAN/multi-user exposure\n");
        } else if (!net_ports_active) {
            off += (size_t)snprintf(buf + off, bufsz - off,
                "  Reachability    : Stopped (no calendar service listening)\n");
        } else if (!has_net_flag && ((has_ip_deny && has_ip_allow_lo) || (tcp_lo_ip && udp_lo_ip))) {
            off += (size_t)snprintf(buf + off, bufsz - off,
                "  Reachability    : Localhost only (127.0.0.0/8 / lo) -- protected from LAN\n");
        } else if (!has_net_flag) {
            off += (size_t)snprintf(buf + off, bufsz - off,
                "  Reachability    : Loopback-enforced by rpc.cmsd (-l)\n");
        } else {
            off += (size_t)snprintf(buf + off, bufsz - off,
                "  Reachability    : WARNING -- Network mode (-n) enabled on LAN!\n"
                "                    ONC RPC AUTH_UNIX is unauthenticated; restrict\n"
                "                    with a host firewall or switch to -u / CalDAV.\n");
        }

        struct stat st_dir, st_cal;
        char cal_file[512];
        snprintf(cal_file, sizeof(cal_file), "/var/spool/calendar/callog.%s", cal_user);
        off += (size_t)snprintf(buf + off, bufsz - off,
            "\nLocal Calendar Storage\n"
            "------------------------------------------------------------\n"
            "  Spool directory : /var/spool/calendar (%s)\n"
            "  Calendar file   : %s (%s)\n",
            (stat("/var/spool/calendar", &st_dir) == 0 && S_ISDIR(st_dir.st_mode))
                ? "present" : "missing",
            cal_file,
            (stat(cal_file, &st_cal) == 0) ? "present" : "not created yet");
        off += (size_t)snprintf(buf + off, bufsz - off,
            "\nRemote Calendar Sharing Options\n"
            "------------------------------------------------------------\n"
            "  SSH tunnel      : %s\n"
            "  CalDAV (HTTPS)  : Available via File -> CalDAV Sync...\n",
            (getenv("DTCM_SSH_TUNNEL") && strcmp(getenv("DTCM_SSH_TUNNEL"), "1") == 0)
                ? "Enabled (remote Browse uses SSH StreamLocalForward -> remote cmsd.sock)"
                : "Disabled (check box below or browse user@ssh://remotehost)");
    } else {
        int ssh_on = (strncmp(cal_host, "ssh://", 6) == 0) ||
                     (getenv("DTCM_SSH_TUNNEL") && strcmp(getenv("DTCM_SSH_TUNNEL"), "1") == 0);
        off += (size_t)snprintf(buf + off, bufsz - off,
            "\nNetwork Note (Remote Calendar)\n"
            "------------------------------------------------------------\n"
            "  Transport       : %s\n",
            ssh_on
                ? "Encrypted SSH StreamLocalForward -> remote /run/rpc.cmsd/cmsd.sock (SO_PEERCRED)"
                : "Direct ONC RPC (AUTH_UNIX, unencrypted) -- enable SSH tunnel or use CalDAV");
    }
}

static void
refresh_status_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Calendar *c = (Calendar *)client_data;
    char report[4096];

    if (!g_status_text)
        return;

    build_status_report(c, report, sizeof(report));
    XmTextSetString(g_status_text, report);
}

static void
ssh_toggle_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    Calendar *c = (Calendar *)client_data;
    if (XmToggleButtonGadgetGetState(w))
        setenv("DTCM_SSH_TUNNEL", "1", 1);
    else
        unsetenv("DTCM_SSH_TUNNEL");
    refresh_status_cb(w, (XtPointer)c, nullptr);
}

static void
close_status_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (g_status_dialog)
        XtUnmanageChild(g_status_dialog);
}

static void
status_menu_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    dtcm_server_status_show((Calendar *)client_data);
}

void
dtcm_server_status_show(Calendar *c)
{
    char report[4096];

    if (!c || !c->frame)
        return;

    if (!g_status_dialog) {
        Arg al[16];
        int ac = 0;
        XmString title = XmStringCreateLocalized("Calendar : Server & Network Status");
        XmString refresh_lbl = XmStringCreateLocalized("Refresh");
        XmString close_lbl = XmStringCreateLocalized("Close");
        XmString ssh_lbl = XmStringCreateLocalized("Use SSH socket tunnel for remote calendars");

        XtSetArg(al[ac], XmNdialogTitle, title); ac++;
        XtSetArg(al[ac], XmNautoUnmanage, False); ac++;
        XtSetArg(al[ac], XmNmarginWidth, 10); ac++;
        XtSetArg(al[ac], XmNmarginHeight, 10); ac++;
        g_status_dialog = XmCreateFormDialog(c->frame, "serverStatusDialog", al, ac);

        ac = 0;
        XtSetArg(al[ac], XmNlabelString, close_lbl); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNwidth, 96); ac++;
        Widget close_btn = XmCreatePushButton(g_status_dialog, "closeButton", al, ac);
        XtAddCallback(close_btn, XmNactivateCallback, close_status_cb, nullptr);
        XtManageChild(close_btn);

        ac = 0;
        XtSetArg(al[ac], XmNlabelString, refresh_lbl); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNrightWidget, close_btn); ac++;
        XtSetArg(al[ac], XmNrightOffset, 8); ac++;
        XtSetArg(al[ac], XmNwidth, 96); ac++;
        Widget refresh_btn = XmCreatePushButton(g_status_dialog, "refreshButton", al, ac);
        XtAddCallback(refresh_btn, XmNactivateCallback, refresh_status_cb, (XtPointer)c);
        XtManageChild(refresh_btn);

        ac = 0;
        XtSetArg(al[ac], XmNlabelString, ssh_lbl); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        g_ssh_toggle = XmCreateToggleButtonGadget(g_status_dialog, "sshTunnelToggle", al, ac);
        XtAddCallback(g_ssh_toggle, XmNvalueChangedCallback, ssh_toggle_cb, (XtPointer)c);
        XtManageChild(g_ssh_toggle);

        ac = 0;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNbottomWidget, close_btn); ac++;
        XtSetArg(al[ac], XmNbottomOffset, 8); ac++;
        Widget sep = XmCreateSeparatorGadget(g_status_dialog, "statusSep", al, ac);
        XtManageChild(sep);

        ac = 0;
        XtSetArg(al[ac], XmNeditMode, XmMULTI_LINE_EDIT); ac++;
        XtSetArg(al[ac], XmNeditable, False); ac++;
        XtSetArg(al[ac], XmNcursorPositionVisible, False); ac++;
        XtSetArg(al[ac], XmNrows, 26); ac++;
        XtSetArg(al[ac], XmNcolumns, 82); ac++;
        XtSetArg(al[ac], XmNscrollHorizontal, True); ac++;
        XtSetArg(al[ac], XmNscrollVertical, True); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNbottomWidget, sep); ac++;
        XtSetArg(al[ac], XmNbottomOffset, 8); ac++;
        g_status_text = XmCreateScrolledText(g_status_dialog, "statusText", al, ac);
        XtManageChild(g_status_text);

        XtVaSetValues(g_status_dialog, XmNcancelButton, close_btn,
                      XmNdefaultButton, close_btn, nullptr);

        XmStringFree(title);
        XmStringFree(refresh_lbl);
        XmStringFree(close_lbl);
        XmStringFree(ssh_lbl);
    }

    if (g_ssh_toggle) {
        int on = (getenv("DTCM_SSH_TUNNEL") && strcmp(getenv("DTCM_SSH_TUNNEL"), "1") == 0);
        XmToggleButtonGadgetSetState(g_ssh_toggle, on ? True : False, False);
    }

    build_status_report(c, report, sizeof(report));
    XmTextSetString(g_status_text, report);
    XtManageChild(g_status_dialog);
}

void
dtcm_server_status_add_menu_item(Calendar *c, Widget view_menu)
{
    Arg al[4];
    int ac = 0;
    XmString label;
    Widget btn;

    if (!c || !view_menu)
        return;

    XmCreateSeparatorGadget(view_menu, "serverStatusSeparator", al, ac);

    label = XmStringCreateLocalized("Server & Network Status...");
    ac = 0;
    XtSetArg(al[ac], XmNlabelString, label); ac++;
    btn = XmCreatePushButtonGadget(view_menu, "serverStatus", al, ac);
    XtAddCallback(btn, XmNactivateCallback, status_menu_cb, (XtPointer)c);
    XmStringFree(label);
}
