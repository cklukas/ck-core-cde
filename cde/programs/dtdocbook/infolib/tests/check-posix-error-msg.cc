/* Harness for check-posix-error-msg.py: PosixError must keep a terminated
 * copy of a message that is longer than its buffer. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
class Exception { public: virtual ~Exception() {} };
#define DECLARE_EXCEPTION(a, b)
/* @@BODY@@ */
int main(void)
{
    char longmsg[400];
    memset(longmsg, 'x', sizeof longmsg - 1);
    longmsg[sizeof longmsg - 1] = '\0';

    PosixError *e = new PosixError(5, longmsg);
    assert(strlen(e->msg()) == (size_t) PosixError::MAXMSG);
    assert(e->error_no() == 5);
    delete e;

    e = new PosixError(5, "short");
    assert(strcmp(e->msg(), "short") == 0);
    delete e;
    puts("posix error message ok");
    return 0;
}
