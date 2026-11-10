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
#include <unistd.h>

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
