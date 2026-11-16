#!/usr/bin/env python3
"""Run the actual post-copy move gate with controlled operation outcomes."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

def _ck_sanitize_list() -> str:
    import os, shlex, subprocess, tempfile
    override = os.environ.get("CK_SANITIZE_LIST", "").strip()
    if override:
        return override
    for var in ("CC", "CXX", "CLANG", "TT_COMPILE", "CSA_COMPILE", "CMSD_COMPILE",
                "SPC_COMPILE", "DTSVC_COMPILE", "DTSR_COMPILE", "DTMAIL_CC",
                "DTMAIL_COMPILE", "DIRSELECT_CC"):
        val = os.environ.get(var, "").strip()
        if val:
            cc = shlex.split(val)[:1]
            break
    else:
        cc = ["cc"]
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
        src, exe = os.path.join(d, "p.c"), os.path.join(d, "p")
        with open(src, "w", encoding="utf-8") as f:
            f.write("int main(void) { return 0; }\n")
        for s in ("address,undefined", "undefined"):
            r = subprocess.run(cc + [f"-fsanitize={s}", src, "-o", exe], capture_output=True)
            if r.returncode == 0 and subprocess.run([exe], capture_output=True).returncode == 0:
                return s
    return "undefined"


_CK_SANITIZE_LIST = _ck_sanitize_list()


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
                   ["-std=gnu23", "-Wall", "-Wextra", "-Werror", f"-fsanitize={_CK_SANITIZE_LIST}",
                    "-g", "-I", str(source), str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
