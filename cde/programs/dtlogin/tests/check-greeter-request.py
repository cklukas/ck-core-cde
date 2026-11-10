#!/usr/bin/env python3
"""Read requests of the login manager the way the greeter does.

ReadRequest() of vgutil.c is compiled with the definitions of the request protocol and run on
pipes under ASan and UBSan: the length of a request is the sender's word, a pipe delivers a message in
as many pieces as it likes, and the strings in a request must end inside it.  Every scenario runs in
a process of its own.  Set DTLOGIN_SOURCE to test another copy of the sources.
"""
from scenarios import block, finish, read, run_all

utility = read("vgutil.c")
if "ReadRequest" in utility:
    reader = block(utility, "int\nReadRequest(")
else:
    # sources from before the reader existed: the greeter read the header and then the rest, once each
    reader = r'''
int ReadRequest(int fd, char *buf, size_t size)
{
  RequestHeader *phdr = (RequestHeader *)buf;
  int count = read(fd, buf, sizeof(*phdr));
  int remainder;
  (void)size;
  if (count != sizeof(*phdr)) return(0);
  remainder = phdr->length - sizeof(*phdr);
  if (remainder > 0) count = read(fd, buf + sizeof(*phdr), remainder);
  return(phdr->length);
}
'''

unit = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "vgproto.h"
''' + reader + r'''
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

static void Send(int fd, const void *data, size_t n) { CHECK(write(fd, data, n) == (ssize_t)n); }

static size_t Message(unsigned char *out, const char *text)
{
    RequestMessage r;
    size_t n = strlen(text) + 1;
    memset(&r, 0, sizeof r);
    r.hdr.opcode = REQUEST_OP_MESSAGE;
    r.hdr.length = sizeof r + n;
    r.offMessage = sizeof r;
    memcpy(out, &r, sizeof r);
    memcpy(out + sizeof r, text, n);
    return sizeof r + n;
}

static void Whole(void)
{
    int fds[2];
    unsigned char out[200];
    char buf[REQUEST_LIM_MAXLEN];
    size_t n = Message(out, "hello");

    CHECK(pipe(fds) == 0);
    Send(fds[1], out, n);
    CHECK(ReadRequest(fds[0], buf, sizeof buf) == (int)n);
    CHECK(strcmp(buf + ((RequestMessage *)buf)->offMessage, "hello") == 0);
}

static void InPieces(void)
{
    int fds[2];
    unsigned char out[200];
    char buf[REQUEST_LIM_MAXLEN];
    size_t n = Message(out, "a message that arrives a byte at a time");
    pid_t pid;

    CHECK(pipe(fds) == 0);
    pid = fork();
    if (pid == 0) {
        for (size_t i = 0; i < n; ++i) { Send(fds[1], out + i, 1); usleep(2000); }
        _exit(0);
    }
    /* the reader waits for all of it, however it is delivered */
    CHECK(ReadRequest(fds[0], buf, sizeof buf) == (int)n);
    CHECK(strcmp(buf + ((RequestMessage *)buf)->offMessage, "a message that arrives a byte at a time") == 0);
    waitpid(pid, NULL, 0);
}

static void TooLong(void)
{
    int fds[2];
    char buf[REQUEST_LIM_MAXLEN + 64];
    RequestHeader h = { REQUEST_OP_MESSAGE, 0, 60000 };

    memset(buf, 0x55, sizeof buf);
    CHECK(pipe(fds) == 0);
    Send(fds[1], &h, sizeof h);
    /* a request that cannot fit is refused, and nothing is written beyond the buffer */
    CHECK(ReadRequest(fds[0], buf, REQUEST_LIM_MAXLEN) == 0);
    for (size_t i = REQUEST_LIM_MAXLEN; i < sizeof buf; ++i) CHECK((unsigned char)buf[i] == 0x55);
}

static void ShorterThanItsHeader(void)
{
    int fds[2];
    char buf[REQUEST_LIM_MAXLEN];
    RequestHeader h = { REQUEST_OP_MESSAGE, 0, 2 };

    CHECK(pipe(fds) == 0);
    Send(fds[1], &h, sizeof h);
    CHECK(ReadRequest(fds[0], buf, sizeof buf) == 0);
}

static void CutShort(void)
{
    int fds[2];
    unsigned char out[200];
    char buf[REQUEST_LIM_MAXLEN];
    size_t n = Message(out, "a message that is cut short");

    CHECK(pipe(fds) == 0);
    Send(fds[1], out, n - 5);
    close(fds[1]);
    CHECK(ReadRequest(fds[0], buf, sizeof buf) == 0);
    CHECK(ReadRequest(fds[0], buf, sizeof buf) == 0);       /* end of file */
}

static void StringsEndInside(void)
{
    int fds[2];
    unsigned char out[200];
    char buf[REQUEST_LIM_MAXLEN];
    size_t n = Message(out, "unterminated");

    out[n - 1] = 'X';                                       /* the sender forgot the NUL */
    CHECK(pipe(fds) == 0);
    Send(fds[1], out, n);
    CHECK(ReadRequest(fds[0], buf, sizeof buf) == (int)n);
    CHECK(strlen(buf + ((RequestMessage *)buf)->offMessage) == strlen("unterminated"));
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"whole", Whole}, {"in-pieces", InPieces}, {"too-long", TooLong},
    {"shorter-than-its-header", ShorterThanItsHeader}, {"cut-short", CutShort},
    {"strings-end-inside", StringsEndInside},
};

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; ++i)
        if (strcmp(argv[1], scenarios[i].name) == 0) { scenarios[i].run(); puts("ok"); return 0; }
    fprintf(stderr, "FAIL unknown scenario %s\n", argv[1]);
    return 2;
}
'''

names = ["whole", "in-pieces", "too-long", "shorter-than-its-header", "cut-short", "strings-end-inside"]
failed = run_all("greeter-request-", {"request_unit.c": unit}, [("request_unit", ["-w"])], [], names)
finish(failed, "Greeter requests", "whole, split, oversized, short and unterminated requests")
