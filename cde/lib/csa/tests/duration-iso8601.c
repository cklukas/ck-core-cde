/* Harness for check-duration-iso8601.py: _csa_duration_to_iso8601() of libcsa.
 * The production function is pasted between the markers below. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int allocation_fails;

/* asprintf() whose allocation can be made to fail; like the C library after a
 * failure it leaves *strp undefined, here a string that must not be used */

static int stub_asprintf(char **strp, const char *format, ...)
{
    va_list ap;
    int n;

    if (allocation_fails) {
        *strp = strdup("undefined");
        return -1;
    }
    va_start(ap, format);
    n = vasprintf(strp, format, ap);
    va_end(ap);
    return n;
}
#define asprintf stub_asprintf
/* @@BODY@@ */

static void expect(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        exit(1);
    }
}

int main(void)
{
    char buf[64];

    memset(buf, 'x', sizeof buf);
    expect("a positive duration", _csa_duration_to_iso8601(5, buf) == 0 &&
           strcmp(buf, "+PT5S") == 0);
    expect("a negative duration", _csa_duration_to_iso8601(-90, buf) == 0 &&
           strcmp(buf, "-PT90S") == 0);

    allocation_fails = 1;
    memset(buf, 'x', sizeof buf);
    expect("a failed allocation is reported", _csa_duration_to_iso8601(5, buf) == -1);
    expect("a failed allocation leaves an empty string", buf[0] == '\0');

    puts("ok");
    return 0;
}
