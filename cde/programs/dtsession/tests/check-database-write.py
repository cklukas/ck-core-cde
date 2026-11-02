#!/usr/bin/env python3
"""Exercise production database open/commit with real files and injected errors."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
text = (source / "SmDB.c").read_text()

def function(name):
    start = text.index("\n" + name + "(")
    start = text.rfind("\n", 0, start) + 1
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

record = text[text.index("typedef struct _ClientDBRec"):text.index("static Boolean xrmInitialized")]
header = r'''
#include <assert.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int Boolean;
typedef void *XrmDatabase;
typedef void *ClientDB;
#define True 1
#define False 0
enum { NONE, CREATE, STREAM, HEADER, ERROR_FLAG, FLUSH, SYNC, CLOSE, RENAME, ALLOCATION };
static int fault;
static char *versionStr=".version", *dtsessionIDStr=".dtsessionID";
static void _initXrm(void) {}
static void XrmDestroyDatabase(XrmDatabase db) { (void)db; }
static int Failed(void) { errno=ENOSPC; return -1; }
static int Stage(char *path) { return fault==CREATE ? Failed() : mkstemp(path); }
static FILE *Stream(int fd, const char *mode)
{ if (fault==STREAM) { Failed(); return NULL; } return fdopen(fd,mode); }
static int Print(FILE *stream, const char *format, ...)
{
    va_list args; int result;
    if (fault==HEADER) return Failed();
    va_start(args,format); result=vfprintf(stream,format,args); va_end(args);
    return result;
}
static int Error(FILE *stream) { return fault==ERROR_FLAG ? 1 : ferror(stream); }
static int Flush(FILE *stream) { return fault==FLUSH ? Failed() : fflush(stream); }
static int Sync(int fd) { return fault==SYNC ? Failed() : fsync(fd); }
static int Close(FILE *stream)
{ int result=fclose(stream); return fault==CLOSE ? Failed() : result; }
static int Rename(const char *from, const char *to)
{ return fault==RENAME ? Failed() : rename(from,to); }
static void *Allocate(size_t size)
{ if (fault==ALLOCATION) { Failed(); return NULL; } return malloc(size); }
#define XtMalloc Allocate
#define XtCalloc calloc
#define XtNewString strdup
#define XtFree free
#define mkstemp Stage
#define fdopen Stream
#define fprintf Print
#define ferror Error
#define fflush Flush
#define fsync Sync
#define fclose Close
#define rename Rename
Boolean CloseClientDB(ClientDB, Boolean);
#define CLIENT_ID_SZ 20
'''
types = (source / "SmXSMP.h").read_text()
types = types[types.index("typedef struct _XSMPClientDBRec"):types.index("typedef struct {", types.index("typedef struct _ProxyClientDBRec"))]
strings = "\n".join(re.findall(r'^static char \*\w+ = .*;$', text, re.M))
writers = "\n".join(function(name) for name in
                    ("_resourceName", "_putStringResource", "_putIntResource",
                     "_putStringArrayResource", "PutXSMPClientDBRec", "PutProxyClientDBRec"))
main = r'''
#undef mkstemp
#undef fdopen
#undef fprintf
#undef ferror
#undef fflush
#undef fsync
#undef fclose
#undef rename
static void Original(void)
{
    FILE *file=fopen("session.db","w"); assert(file);
    assert(fputs("previous session\n",file)>=0 && fclose(file)==0);
}
static void CheckOriginal(void)
{
    char text[64]; FILE *file=fopen("session.db","r"); assert(file);
    assert(fgets(text,sizeof(text),file) && strcmp(text,"previous session\n")==0);
    assert(fclose(file)==0);
}
static void NoStage(void)
{
    glob_t files;
    assert(glob("session.db.*",0,NULL,&files)==GLOB_NOMATCH);
    globfree(&files);
}
int main(void)
{
    assert(!CloseClientDB(NULL,True));
    assert(!OpenOutputClientDB(NULL,"1","id"));
    for (int which=CREATE; which<=RENAME; ++which) {
        Original();
        fault=which<=HEADER ? which : NONE;
        ClientDB db=OpenOutputClientDB("session.db","1","id");
        if (which<=HEADER) assert(!db);
        else {
            assert(db); CheckOriginal();
            struct stat info;
            assert(stat(((ClientDBRec *)db)->tmpDBFileName,&info)==0);
            assert((info.st_mode & 0777)==0600);
            fault=which;
            assert(!CloseClientDB(db,True));
        }
        fault=NONE; CheckOriginal(); NoStage();
    }
    ClientDB db=OpenOutputClientDB("session.db","1","id"); assert(db);
    assert(CloseClientDB(db,False)); CheckOriginal(); NoStage();
    assert(unlink("session.db")==0);
    db=OpenOutputClientDB("session.db","1","id"); assert(db);
    assert(CloseClientDB(db,False));
    assert(access("session.db",F_OK)!=0); NoStage();
    Original();
    for (int which=0; which<6; ++which) {
        db=OpenOutputClientDB("session.db","1","id"); assert(db);
        XSMPClientDBRec xsmp={0}; xsmp.clientId="id";
        ProxyClientDBRec proxy={0};
        if (which==0 || which==1) fault=which==0 ? HEADER : ALLOCATION;
        if (which==2) ((ClientDBRec *)db)->nextXSMPClientIndex=INT_MAX;
        if (which==3) ((ClientDBRec *)db)->nextProxyClientIndex=INT_MAX;
        if (which<3) assert(!PutXSMPClientDBRec(db,&xsmp));
        else if (which==3) assert(!PutProxyClientDBRec(db,&proxy));
        else if (which==4) assert(!PutXSMPClientDBRec(db,NULL));
        else assert(!PutProxyClientDBRec(db,NULL));
        fault=NONE;
        assert(!PutProxyClientDBRec(db,&proxy));
        assert(!CloseClientDB(db,True)); CheckOriginal(); NoStage();
    }
    db=OpenOutputClientDB("session.db","1","id"); assert(db);
    char *command[]={"application","argument",NULL};
    XSMPClientDBRec xsmp={0}; xsmp.clientId="id"; xsmp.restartCommand=command;
    ProxyClientDBRec proxy={0}; proxy.command=command;
    assert(PutXSMPClientDBRec(db,&xsmp) && PutProxyClientDBRec(db,&proxy));
    assert(CloseClientDB(db,True)); NoStage();
    char text[256]; FILE *file=fopen("session.db","r"); assert(file);
    size_t bytes=fread(text,1,sizeof(text)-1,file); text[bytes]='\0';
    assert(strstr(text,".version: 1\n") && strstr(text,".dtsessionID: id\n"));
    assert(fclose(file)==0);
    puts("Session database: private staging, abort, commit and eight fault paths passed");
}
'''
with tempfile.TemporaryDirectory(prefix="session-database-", dir=os.environ["TMPDIR"]) as work:
    c, binary = Path(work) / "database.c", Path(work) / "database"
    c.write_text(header + record + types + strings + writers + function("OpenOutputClientDB") +
                 function("CloseClientDB") + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], cwd=work, check=True)
