#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *case_name;
static int pipe_fault, pipe_eintr, get_fault, get_eintr, set_fault, set_eintr;
static int close_fault, close_calls;
static int write_eintr, write_short, write_zero;

#define VERIFY(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", case_name, __LINE__, #value); return 1; \
} } while (0)

static int
TestPipe(int descriptors[2])
{
    if (pipe_fault) { errno = EMFILE; return -1; }
    if (pipe_eintr) { pipe_eintr = 0; errno = EINTR; return -1; }
    return pipe(descriptors);
}

static int
TestFcntl(int fd, int command, ...)
{
    int argument, result;
    va_list arguments;
    if (command == F_GETFD)
    {
        if (get_fault) { errno = EIO; return -1; }
        if (get_eintr) { get_eintr = 0; errno = EINTR; return -1; }
        return fcntl(fd, command);
    }
    VERIFY(command == F_SETFD);
    va_start(arguments, command);
    argument = va_arg(arguments, int);
    va_end(arguments);
    if (set_eintr) { set_eintr = 0; errno = EINTR; return -1; }
    if (set_fault && --set_fault == 0) { errno = EACCES; return -1; }
    result = fcntl(fd, command, argument);
    return result;
}

static int
TestClose(int fd)
{
    int result = close(fd);
    ++close_calls;
    if (close_fault) { errno = EINTR; return -1; }
    return result;
}

static ssize_t
TestWrite(int fd, const void *bytes, size_t length)
{
    if (write_eintr) { write_eintr = 0; errno = EINTR; return -1; }
    if (write_zero) return 0;
    if (write_short && length > 2) length = 2;
    return write(fd, bytes, length);
}

#define pipe TestPipe
#define fcntl TestFcntl
#define close TestClose
#define write TestWrite
#include "FileProcess.c"
#undef pipe
#undef fcntl
#undef close
#undef write

int
main(int argc, char **argv)
{
    int descriptors[2] = {91, 92}, saved;
    VERIFY(argc == 2);
    case_name = argv[1];
    if (strncmp(case_name, "write-", 6) == 0)
    {
        char data[6] = {0};
        if (strcmp(case_name, "write-null") == 0)
        {
            errno = 0;
            VERIFY(DtfileProcessWriteAll(-1, data, sizeof(data)) == -1 && errno == EINVAL);
            errno = 0;
            VERIFY(DtfileProcessWriteAll(1, NULL, 1) == -1 && errno == EINVAL);
        }
        else
        {
            VERIFY(pipe(descriptors) == 0);
            if (strcmp(case_name, "write-epipe") == 0)
            {
                VERIFY(close(descriptors[0]) == 0);
                descriptors[0] = -1;
                errno = 0;
                VERIFY(DtfileProcessWriteAll(descriptors[1], "abcdef", 6) == -1 && errno == EPIPE);
            }
            else
            {
                write_eintr = strcmp(case_name, "write-eintr") == 0;
                write_short = strcmp(case_name, "write-short") == 0;
                write_zero = strcmp(case_name, "write-zero") == 0;
                errno = 0;
                if (write_zero)
                    VERIFY(DtfileProcessWriteAll(descriptors[1], "abcdef", 6) == -1 && errno == EIO);
                else
                {
                    VERIFY(DtfileProcessWriteAll(descriptors[1], "abcdef", 6) == 0);
                    VERIFY(read(descriptors[0], data, sizeof(data)) == sizeof(data));
                    VERIFY(memcmp(data, "abcdef", sizeof(data)) == 0);
                }
            }
            if (descriptors[0] >= 0) VERIFY(close(descriptors[0]) == 0);
            VERIFY(close(descriptors[1]) == 0);
        }
    }
    else if (strcmp(case_name, "null") == 0)
    {
        errno = 0; VERIFY(DtfileProcessPipe(NULL) == -1 && errno == EINVAL);
    }
    else
    {
        pipe_fault = strcmp(case_name, "pipe-fault") == 0;
        pipe_eintr = strcmp(case_name, "pipe-eintr") == 0;
        get_fault = strcmp(case_name, "getfd-fault") == 0;
        get_eintr = strcmp(case_name, "getfd-eintr") == 0;
        set_fault = strcmp(case_name, "setfd-fault") == 0 ? 2 : 0;
        set_eintr = strcmp(case_name, "setfd-eintr") == 0;
        errno = 0;
        if (pipe_fault || get_fault || set_fault)
        {
            saved = pipe_fault ? EMFILE : get_fault ? EIO : EACCES;
            VERIFY(DtfileProcessPipe(descriptors) == -1);
            VERIFY(errno == saved && descriptors[0] == -1 && descriptors[1] == -1);
            VERIFY(close_calls == (pipe_fault ? 0 : 2));
        }
        else
        {
            VERIFY(DtfileProcessPipe(descriptors) == 0);
            VERIFY((fcntl(descriptors[0], F_GETFD) & FD_CLOEXEC) != 0);
            VERIFY((fcntl(descriptors[1], F_GETFD) & FD_CLOEXEC) != 0);
            close_fault = strcmp(case_name, "close-eintr") == 0;
            DtfileProcessClosePipe(descriptors);
            VERIFY(descriptors[0] == -1 && descriptors[1] == -1 && close_calls == 2);
            DtfileProcessClosePipe(descriptors);
            VERIFY(close_calls == 2);
        }
    }
    printf("PASS %s\n", case_name);
    return 0;
}
