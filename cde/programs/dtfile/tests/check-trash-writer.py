#!/usr/bin/env python3
"""Exercise the production Trash writer with real files and injected failures."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "Trash.c"
text = source.read_text()

def function(signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

header = r'''
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int Boolean;
typedef char *String;
#define True 1
#define False 0
static char *TrashInfoFileName=".trashinfo";
static char *NewTrashInfoFileName=".old-stage";
static int numTrashItems=1;
static struct {
    char *external, *filename, *deletionDate;
    unsigned long long recSize;
    Boolean recSizeValid;
} trashCan[]={{"/a", "one", NULL, 0, False}};
static int fault;
'''
wrappers = r'''
static int make_temp(char *p)
{ if (fault==1) { errno=EACCES; return -1; } return mkstemp(p); }
static FILE *open_stream(int fd, const char *mode)
{ if (fault==2) { errno=ENOMEM; return NULL; } return fdopen(fd,mode); }
static int set_mode(int fd, mode_t mode)
{ if (fault==3) { errno=EPERM; return -1; } return fchmod(fd,mode); }
static int set_owner(int fd, uid_t u, gid_t g)
{ if (fault==4) { errno=EPERM; return -1; } return fchown(fd,u,g); }
static int write_entry(FILE *f, String e, String i, String d,
                       unsigned long long size, Boolean valid)
{ if (fault==5) return -1; return WriteEntry(f,e,i,d,size,valid); }
static int flush_stream(FILE *f)
{ if (fault==6) { errno=ENOSPC; return EOF; } return fflush(f); }
static int sync_file(int fd)
{ if (fault==7) { errno=EIO; return -1; } return fsync(fd); }
static int close_stream(FILE *f)
{ int rc=fclose(f); if (fault==8) { errno=ENOSPC; return EOF; } return rc; }
static int replace_file(const char *s, const char *d)
{ if (fault==9) { errno=EACCES; return -1; } return rename(s,d); }
#define mkstemp make_temp
#define fdopen open_stream
#define fchmod set_mode
#define fchown set_owner
#define WriteEntry write_entry
#define fflush flush_stream
#define fsync sync_file
#define fclose close_stream
#define rename replace_file
'''
main = r'''
#undef mkstemp
#undef fdopen
#undef fchmod
#undef fchown
#undef WriteEntry
#undef fflush
#undef fsync
#undef fclose
#undef rename
int main(int argc, char **argv)
{
    assert(argc==2); assert(chdir(argv[1])==0);
    (void)NewTrashInfoFileName;
    for (fault=0; fault<=9; ++fault) {
        FILE *f=fopen(TrashInfoFileName,"w"); assert(f);
        assert(fputs("old\n",f)>=0); assert(fclose(f)==0);
        assert(WriteTrashEntries()==(fault==0));
        f=fopen(TrashInfoFileName,"r"); assert(f);
        char data[128]; assert(fgets(data,sizeof(data),f)); fclose(f);
        assert(strcmp(data,fault==0 ? "2 3 /a one\n" : "old\n")==0);
        if (!fault) {
            struct stat s; assert(stat(TrashInfoFileName,&s)==0);
            assert((s.st_mode&0777)==0600);
        }
        DIR *dir=opendir("."); assert(dir);
        struct dirent *entry;
        while ((entry=readdir(dir)))
            assert(strcmp(entry->d_name,".")==0 ||
                   strcmp(entry->d_name,"..")==0 ||
                   strcmp(entry->d_name,TrashInfoFileName)==0);
        closedir(dir);
    }
    assert(unlink(TrashInfoFileName)==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="trash-writer-", dir=os.environ["TMPDIR"]) as work:
    root = Path(work)
    fixture = root / "files"
    fixture.mkdir()
    c, binary = root / "writer.c", root / "writer"
    c.write_text(header + function("static int\nWriteEntry(") + wrappers +
                 function("static Boolean\nWriteTrashEntries(") + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function",
                    "-fsanitize=address,undefined", "-g", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(fixture)], check=True)
print("Trash writer: success and nine injected failure paths passed (ASan/UBSan)")
