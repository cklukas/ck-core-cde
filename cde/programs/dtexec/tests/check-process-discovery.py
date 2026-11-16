#!/usr/bin/env python3
"""Execute production child discovery against a task-owned procfs fixture."""
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
text = (source / "Main.c").read_text()
start = text.index("typedef struct {\n    char **items;")
end = text.index("static void\nDescribeChildListForLog", start)
header = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
typedef int Boolean;
#define True 1
#define False 0
static const char *fixture;
static int read_mode, read_calls, fail_realloc;
static void MapPath(const char *path, char *mapped, size_t capacity)
{
    assert(strncmp(path,"/proc/",6)==0);
    int length=snprintf(mapped,capacity,"%s%s",fixture,path);
    assert(length>0 && (size_t)length<capacity);
}
static int ProcOpen(const char *path, int flags)
{ char mapped[4096]; MapPath(path,mapped,sizeof(mapped)); return open(mapped,flags); }
static FILE *ProcFopen(const char *path, const char *mode)
{ char mapped[4096]; MapPath(path,mapped,sizeof(mapped)); return fopen(mapped,mode); }
static ssize_t ProcRead(int fd, void *data, size_t length)
{
    if (read_mode && read_calls++==0) { errno=EINTR; return -1; }
    if (read_mode && length>3) length=3;
    return read(fd,data,length);
}
static void *Grow(void *data, size_t length)
{ if (fail_realloc) { errno=ENOMEM; return NULL; } return realloc(data,length); }
#define open ProcOpen
#define fopen ProcFopen
#define read ProcRead
#define realloc Grow
'''
main = r'''
#undef open
#undef fopen
#undef read
#undef realloc
int main(int argc, char **argv)
{
    assert(argc==2); fixture=argv[1];
    assert(NextListCapacity(0,sizeof(void *))==8);
    assert(NextListCapacity(INT_MAX,sizeof(void *))==0);
    assert(NextListCapacity(INT_MAX/2+1,1)==INT_MAX);
    assert(NextListCapacity(0,SIZE_MAX)==0);
    CommandList list; CommandListInit(&list);
    fail_realloc=1; assert(!CommandListAdd(&list,"one"));
    assert(!list.items && list.count==0 && list.capacity==0);
    fail_realloc=0;
    assert(CommandListAdd(&list,"one")); assert(!CommandListAdd(&list,"one"));
    assert(CommandListAdd(&list,"two"));
    char *joined=CommandListJoin(&list);
    assert(joined && strcmp(joined,"one\ntwo")==0);
    free(joined); CommandListFree(&list);
    for (read_mode=0; read_mode<=1; ++read_mode) {
        read_calls=0;
        int count=0;
        joined=CollectChildCommandList(1,&count);
        assert(joined && count==6);
        const char *expected[]={"/bin/root","/bin/two","/bin/three",
                                "/bin/late","/bin/five","/bin/six"};
        for (size_t i=0; i<sizeof(expected)/sizeof(*expected); ++i)
            assert(strstr(joined,expected[i]));
        free(joined);
    }
    char small[4];
    assert(!ReadProcCmdline(1,small,sizeof(small)));
    char command[64];
    assert(!ReadProcCmdline(7,command,sizeof(command)));
    assert(command[0]=='\0');
    puts("Process discovery: nested siblings, cycles, long PID lists, short reads and growth bounds passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="dtexec-processes-", dir=os.environ["TMPDIR"]) as work:
    root = Path(work)
    processes = {1: (b"/bin/root\0arg\0", "2 3 " + "0 " * 2100 + "12345 -1 999999999999999999999 2x\n"),
                 2: (b"/bin/two\0", "5\n"), 3: (b"/bin/three\0", "6\n"),
                 5: (b"/bin/five\0", "1 2\n"), 6: (b"/bin/six\0", ""),
                 12345: (b"/bin/late\0", ""), 7: (b"unterminated", "")}
    for pid, (command, children) in processes.items():
        directory = root / "proc" / str(pid)
        (directory / "task" / str(pid)).mkdir(parents=True)
        (directory / "cmdline").write_bytes(command)
        (directory / "task" / str(pid) / "children").write_text(children)
    c, binary = root / "discovery.c", root / "discovery"
    c.write_text(header + text[start:end] + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu23", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    f"-fsanitize={_CK_SANITIZE_LIST}", "-g", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(root)], check=True)
