#!/usr/bin/env python3
"""Run the actual post-copy move gate with controlled operation outcomes."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
text = (source / "dtcopy/main_dtcopy.c").read_text()
start = text.index("  rc = SyncDirectory(&app_args.sync);")
end = text.index("  /* display the result */", start)
header = r'''
#include <assert.h>
#include <stdio.h>
#include "dtcopy/OperationOutcome.h"
#define True 1
#define GETMESSAGE(a,b,c) (c)
static struct { struct { char *source; } sync; } app_args={{"source"}};
static int G_move=1, G_do_copy, nerr, had_skipped_items, syncResult, eraseResult, eraseErrors, erasures, messages;
static int (*progressCallback)(char *), (*errorCallback)(char *, int);
static int moveDeleteCallback(char *path) { (void)path; return 0; }
static int moveErrorCallback(char *path, int error) { (void)path; (void)error; return 0; }
static int SyncDirectory(void *params) { (void)params; return syncResult; }
static void AppendErrorMsg(char *message) { (void)message; ++messages; }
static void fsErase(char *path, int *result, int force)
{ (void)path; assert(force); ++erasures; *result=eraseResult; nerr+=eraseErrors; }
static DtcopyOutcome Finish(void)
{
    int rc;
    DtcopyOutcome outcome;
'''
main = r'''
    return DtcopyGetOutcome(rc,!G_do_copy,nerr,had_skipped_items);
}
int main(void)
{
    /* Exercise precedence and every independent incomplete disposition. */
    for (int result=-1; result<=1; ++result)
      for (int cancel=0; cancel<=1; ++cancel)
        for (int errors=0; errors<=1; ++errors)
          for (int skipped=0; skipped<=1; ++skipped) {
            DtcopyOutcome expected = cancel ? DTCOPY_CANCELLED :
              result<0 ? DTCOPY_FAILED :
              result>0 || errors || skipped ? DTCOPY_PARTIAL : DTCOPY_COMPLETED;
            assert(DtcopyGetOutcome(result,cancel,errors,skipped)==expected);
          }
    /* Each incomplete case must retain the source, even with zero errors. */
    for (int which=0; which<5; ++which) {
        G_do_copy=which!=0; nerr=which==1; had_skipped_items=which==2;
        syncResult=which==3 ? -1 : which==4 ? 1 : 0;
        erasures=messages=0;
        assert(Finish()!=DTCOPY_COMPLETED && !erasures && messages==1);
    }
    G_do_copy=1; nerr=had_skipped_items=syncResult=0; erasures=messages=0;
    assert(Finish()==DTCOPY_COMPLETED && erasures==1 && !messages);
    /* Retirement failures must not turn a successful copy into success. */
    eraseResult=-1; erasures=0;
    assert(Finish()==DTCOPY_FAILED && erasures==1);
    eraseResult=0; eraseErrors=1; erasures=0;
    assert(Finish()==DTCOPY_PARTIAL && erasures==1);
    eraseErrors=nerr=0;
    G_move=0; erasures=0;
    assert(Finish()==DTCOPY_COMPLETED && !erasures);
    puts("Operation feedback: 24 outcome combinations; move retention, retirement failures, successful move and copy gates passed");
}
'''
with tempfile.TemporaryDirectory(prefix="dtcopy-outcome-", dir=os.environ["TMPDIR"]) as work:
    c, binary = Path(work) / "outcome.c", Path(work) / "outcome"
    c.write_text(header + text[start:end] + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c99", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-g", "-I", str(source), str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
