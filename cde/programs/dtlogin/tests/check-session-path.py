#!/usr/bin/env python3
"""Exercise the complete saved-session path reader, including stream errors."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

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
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
