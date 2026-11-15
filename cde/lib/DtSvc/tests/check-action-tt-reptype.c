/* Harness for check-action-tt-reptype.py: the representation guess for a
 * ToolTalk argument must be defined for every status the library returns. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
typedef int Tt_status;
typedef void *Tt_message;
#define TT_OK 0
#define TT_ERR_NUM 1035
#define TT_ERR_ARG 1004
#define DtACT_TT_REP_INT 1
#define DtACT_TT_REP_BUFFER 2
static Tt_status stub_status;
static Tt_status tt_message_arg_ival(Tt_message m, int arg, int *v)
{ (void) m; (void) arg; *v = 0; return stub_status; }
/* @@BODY@@ */
int main(void)
{
    stub_status = TT_OK;
    assert(_DtAct_tt_message_arg_reptype(nullptr, 0) == DtACT_TT_REP_INT);
    stub_status = TT_ERR_NUM;
    assert(_DtAct_tt_message_arg_reptype(nullptr, 0) == DtACT_TT_REP_BUFFER);
    stub_status = TT_ERR_ARG;                 /* any other failure */
    assert(_DtAct_tt_message_arg_reptype(nullptr, 0) == DtACT_TT_REP_BUFFER);
    puts("tt reptype ok");
    return 0;
}
