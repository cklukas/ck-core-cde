#!/usr/bin/env python3
"""Exercise the mail transport wakeup and process ownership with real children."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def definition(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


SOURCE = r"""
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

using SubProcessFinishedProc = void(int, int, void *);
struct waitentry_t {
    waitentry_t *next;
    int pid, status;
    void *data;
    SubProcessFinishedProc *proc;
};
static waitentry_t *_waitlist;
static int _transport_read_fd = -1;
static volatile sig_atomic_t _transport_write_fd = -1;
struct RFCTransport {
    static void childHandler(int);
    void signalRegister();
    void processSendmailReturn();
};

#include "production.h"

static int completions;
static void finished(int pid, int status, void *data)
{
    int *record = static_cast<int *>(data);
    assert(pid == record[0] && status == record[1]);
    completions++;
}

static pid_t child(int status)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid)
        _exit(status);
    return pid;
}

int main()
{
    int fds[2];
    assert(makeTransportWakePipe(fds) == 0);
    _transport_read_fd = fds[0];
    _transport_write_fd = fds[1];
    assert(fcntl(fds[1], F_GETFL) & O_NONBLOCK);
    assert(fcntl(fds[0], F_GETFD) & FD_CLOEXEC);
    assert(fcntl(fds[1], F_GETFD) & FD_CLOEXEC);
    RFCTransport transport;
    transport.signalRegister();

    char notifications[128] = {};
    while (write(fds[1], notifications, sizeof(notifications)) > 0) {}
    assert(errno == EAGAIN);
    RFCTransport::childHandler(SIGCHLD);
    assert(errno == EAGAIN);

    int messages[2][2] = {{-1, 15}, {-1, 26}};
    for (auto &message : messages) {
        auto *entry = static_cast<waitentry_t *>(malloc(sizeof(waitentry_t)));
        assert(entry);
        message[0] = child(message[1]);
        *entry = {_waitlist, message[0], -1, message, finished};
        _waitlist = entry;
    }
    pid_t foreign = child(37);
    int status;
    pid_t waited;
    do {
        waited = waitpid(foreign, &status, 0);
    } while (waited < 0 && errno == EINTR);
    assert(waited == foreign && WIFEXITED(status) && WEXITSTATUS(status) == 37);

    while (completions != 2) {
        struct pollfd ready = {fds[0], POLLIN, 0};
        int rc = poll(&ready, 1, 2000);
        if (rc < 0 && errno == EINTR)
            continue;
        assert(rc == 1 && (ready.revents & POLLIN));
        transport.processSendmailReturn();
    }
    assert(!_waitlist);
    close(fds[0]);
    close(fds[1]);
}
"""


def main():
    app = Path(__file__).resolve().parents[1]
    transport = (app / "libDtMail/RFC/RFCTransport.C").read_text()
    compose = (app / "dtmail/SendMsgDialog.C").read_text()
    definitions = [definition(compose, "static int makeTransportWakePipe")]
    definitions.extend(definition(transport, "void\nRFCTransport::" + name)
                       for name in ("processSendmailReturn", "childHandler", "signalRegister"))
    with tempfile.TemporaryDirectory(prefix="dtmail-children-") as path:
        work = Path(path)
        (work / "production.h").write_text("\n".join(definitions) + "\n")
        src, exe = work / "children.cc", work / "children"
        src.write_text(SOURCE)
        subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                        "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        subprocess.run([exe], check=True, timeout=10)
    print("PASS transport wakeup is nonblocking and only collects its own children")


if __name__ == "__main__":
    main()
