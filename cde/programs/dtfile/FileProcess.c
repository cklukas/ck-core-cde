/*
 * CDE - Common Desktop Environment
 *
 * These libraries and programs are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * These libraries and programs are distributed without any warranty; without
 * even the implied warranty of merchantability or fitness for a particular
 * purpose. See the GNU Lesser General Public License for more details.
 */

#include "FileProcess.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

typedef struct {
    sigset_t previous;
    sigset_t pipe_signal;
    int was_pending;
} ProcessWriteScope;

static int
ProcessWriteBegin(ProcessWriteScope *scope)
{
    sigset_t pending;
    int error;
    sigemptyset(&scope->pipe_signal);
    sigaddset(&scope->pipe_signal, SIGPIPE);
    error = pthread_sigmask(SIG_BLOCK, &scope->pipe_signal, &scope->previous);
    if (error != 0) { errno = error; return -1; }
    if (sigpending(&pending) != 0)
    {
        int saved = errno;
        (void)pthread_sigmask(SIG_SETMASK, &scope->previous, NULL);
        errno = saved;
        return -1;
    }
    scope->was_pending = sigismember(&pending, SIGPIPE);
    return 0;
}

static int
ProcessWriteEnd(ProcessWriteScope *scope, int result)
{
    int saved = errno, error;
    if (result != 0 && saved == EPIPE && !scope->was_pending)
    {
        sigset_t pending;
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE))
        {
            int signal_number;
            do { error = sigwait(&scope->pipe_signal, &signal_number); }
            while (error == EINTR);
        }
    }
    error = pthread_sigmask(SIG_SETMASK, &scope->previous, NULL);
    if (error != 0 && result == 0) { errno = error; return -1; }
    errno = saved;
    return result;
}

int
DtfileProcessWriteAll(int descriptor, const void *bytes, size_t length)
{
    ProcessWriteScope scope;
    const unsigned char *next = bytes;
    int result = 0;
    if (descriptor < 0 || (bytes == NULL && length != 0))
    { errno = EINVAL; return -1; }
    if (ProcessWriteBegin(&scope) != 0) return -1;
    while (length != 0)
    {
        ssize_t written = write(descriptor, next, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || (size_t)written > length)
        {
            if (written >= 0) errno = EIO;
            result = -1;
            break;
        }
        next += (size_t)written;
        length -= (size_t)written;
    }
    return ProcessWriteEnd(&scope, result);
}

void
DtfileProcessClose(int *descriptor)
{
    int fd;
    if (descriptor == NULL || *descriptor < 0) return;
    fd = *descriptor;
    *descriptor = -1;
    (void)close(fd);
}

void
DtfileProcessClosePipe(int descriptors[2])
{
    if (descriptors == NULL) return;
    DtfileProcessClose(&descriptors[0]);
    DtfileProcessClose(&descriptors[1]);
}

static int
ProcessCloseOnExec(int fd)
{
    int flags, result;
    do { flags = fcntl(fd, F_GETFD); } while (flags < 0 && errno == EINTR);
    if (flags < 0) return -1;
    do { result = fcntl(fd, F_SETFD, flags | FD_CLOEXEC); }
    while (result < 0 && errno == EINTR);
    return result;
}

int
DtfileProcessPipe(int descriptors[2])
{
    int saved;
    if (descriptors == NULL) { errno = EINVAL; return -1; }
    descriptors[0] = descriptors[1] = -1;
    while (pipe(descriptors) != 0)
        if (errno != EINTR) return -1;
    if (ProcessCloseOnExec(descriptors[0]) == 0 &&
        ProcessCloseOnExec(descriptors[1]) == 0)
        return 0;
    saved = errno;
    DtfileProcessClosePipe(descriptors);
    errno = saved;
    return -1;
}
