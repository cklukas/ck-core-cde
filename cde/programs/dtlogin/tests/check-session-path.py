#!/usr/bin/env python3
"""Exercise the complete saved-session path reader, including stream errors."""
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
text = (source / "verify.c").read_text()
start = text.index("static int\nReadSessionPath(")
end = text.index("\nint \nVerify(", start)
header = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static int fault;
static int Read(FILE *file) { if (fault) { errno=EIO; return EOF; } return fgetc(file); }
static int Error(FILE *file) { return fault || ferror(file); }
#define fgetc Read
#define ferror Error
'''
main = r'''
#undef fgetc
#undef ferror
static void Check(const char *input, size_t length, int expected)
{
    FILE *file=tmpfile(); assert(file);
    assert(fwrite(input,1,length,file)==length && fseek(file,0,SEEK_SET)==0);
    char path[32]; memset(path,'x',sizeof(path));
    assert(ReadSessionPath(file,path,sizeof(path))==expected);
    if (expected) assert(strcmp(path,"/path with space/session")==0);
    else assert(path[0]=='\0');
    assert(fclose(file)==0);
}
int main(void)
{
    const char *valid="/path with space/session";
    Check(valid,strlen(valid),1);
    Check("/path with space/session\n",strlen(valid)+1,1);
    Check("/path with space/session\r\n",strlen(valid)+2,1);
    Check("",0,0);
    Check("\n",1,0);
    Check("/path\0hidden",12,0);
    Check("/path\nsecond",12,0);
    Check("/path\rsecond",12,0);
    char oversized[128]; memset(oversized,'a',sizeof(oversized));
    Check(oversized,sizeof(oversized),0);
    fault=1; Check(valid,strlen(valid),0);
    puts("Session paths: complete reads, whitespace, terminators, NUL and read errors passed");
}
'''
with tempfile.TemporaryDirectory(prefix="login-path-", dir=os.environ["TMPDIR"]) as work:
    c, binary = Path(work) / "path.c", Path(work) / "path"
    c.write_text(header + text[start:end] + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu23", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    f"-fsanitize={_CK_SANITIZE_LIST}", "-g", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
