#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../calctool.h"
#include <Dt/MsgLog.h>

struct calcVars CalcVars;
Vars v = &CalcVars;
char *mpstrs[128];
char *vstrs[64];

static int msglog_calls = 0;
static DtMsgLogType last_msglog_type = DtMsgLogInformation;

char *
XtMalloc(unsigned int size)
{
    return malloc(size ? size : 1);
}

void
XtFree(char *ptr)
{
    free(ptr);
}

#undef strlen
size_t
Xestrlen(const char *s)
{
    return strlen(s);
}

void
DtMsgLogMessage(const char *program_name, DtMsgLogType msg_type, const char *format, ...)
{
    (void)program_name;
    (void)format;
    msglog_calls++;
    last_msglog_type = msg_type;
}

void
doerr(char *errmes)
{
    (void)errmes;
    v->error = 1;
}

#include "../mp.c"

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int i = 0; i < 128; i++)
        mpstrs[i] = "mp-diag\n";
    for (int i = 0; i < 64; i++)
        vstrs[i] = "ERROR";

    memset(&CalcVars, 0, sizeof(CalcVars));
    v->appname = "dtcalc";
    v->MPdebug = FALSE;
    v->MPerrors = FALSE;

    int acc = MAX_DIGITS + 12;
    int size = MP_SIZE;
    mpset(&acc, &size, &size);

    /* Under -O2, mpset() must NOT overflow signed int into MP.m == 0. */
    assert(MP.b > 1);
    assert(MP.t > 1);
    assert(MP.m > MP.t);
    assert(MP.m >= 1000000);
    assert(msglog_calls == 0);

    /* Verify normal MP arithmetic triggers zero errorlog traffic and zero errors. */
    int a[MP_SIZE], b[MP_SIZE], q[MP_SIZE];
    int n621 = 621, n1000 = 1000;
    mpcim(&n621, a);
    mpcim(&n1000, b);
    mpdiv(a, b, q);
    double d = 0.0;
    mpcmd(q, &d);
    assert(v->error == 0);
    assert(msglog_calls == 0);
    assert(fabs(d - 0.621) < 1e-9);

    /* Division by zero sets v->error without spamming errorlog when -D is off. */
    int zero_i = 0;
    int z[MP_SIZE];
    mpcim(&zero_i, z);
    mpdiv(a, z, q);
    assert(v->error == 1);
    assert(msglog_calls == 0);

    /* With -D enabled, MP diagnostics use DtMsgLogDebug instead of DtWarning. */
    v->error = 0;
    v->MPdebug = TRUE;
    v->MPerrors = TRUE;
    mpdiv(a, z, q);
    assert(v->error == 1);
    assert(msglog_calls == 1);
    assert(last_msglog_type == DtMsgLogDebug);

    printf("PASS check-mp-errorlog (MP.b=%d MP.t=%d MP.m=%d)\n", MP.b, MP.t, MP.m);
    return 0;
}
