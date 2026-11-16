#!/usr/bin/env python3
"""Run the production request reader with independent per-screen Xlib replies."""
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
text = (source / "WmProperty.c").read_text()
start = text.index("void \nGetDtWmRequest (")
opening = text.index("{", start)
depth, end = 1, opening + 1
while depth:
    depth += (text[end] == "{") - (text[end] == "}")
    end += 1
header = r'''
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int Boolean;
typedef struct {
    Window wmWorkspaceWin;
    unsigned char *dtWmRequestData;
    unsigned long dtWmRequestLength, dtWmRequestOffset;
} WmScreenData;
static struct { Atom xa_DT_WM_REQUEST; } wmGD={42};
#define DISPLAY ((Display *)0)
#define GETMESSAGE(a,b,c) (c)
typedef struct {
    const char *bytes;
    size_t size;
    Atom type;
    int format, status;
    unsigned long count, left;
} Reply;
static Reply replies[3];
static int live, fail, fetches[3], deletes;
static void Warning(char *message) { (void)message; }
static char *XtMalloc(size_t size) { return fail ? NULL : malloc(size); }
int XFree(void *data) { assert(data && live>0); --live; free(data); return 0; }
int XDeleteProperty(Display *display, Window window, Atom atom)
{ (void)display; (void)window; assert(atom==42); ++deletes; return 0; }
int XGetWindowProperty(Display *display, Window window, Atom atom, long offset,
    long length, Bool remove, Atom requested, Atom *type, int *format,
    unsigned long *count, unsigned long *left, unsigned char **bytes)
{
    (void)display; assert(window>0 && window<3 && atom==42 && offset==0);
    assert(length==1000000 && remove && requested==AnyPropertyType);
    Reply reply=replies[window]; ++fetches[window];
    *type=reply.type; *format=reply.format; *count=reply.count; *left=reply.left;
    *bytes=NULL;
    if (reply.bytes) {
        *bytes=malloc(reply.size ? reply.size : 1); assert(*bytes); ++live;
        memcpy(*bytes,reply.bytes,reply.size);
    }
    memset(&replies[window],0,sizeof(Reply));
    return reply.status;
}
'''
main = r'''
static void Queue(int screen, const char *bytes, size_t size)
{ replies[screen]=(Reply){bytes,size,XA_STRING,8,Success,size,0}; }
static void Next(WmScreenData *screen, const char *expected, Boolean expectedMore)
{
    char *request=(char *)1; Boolean more=True;
    GetDtWmRequest(screen,&request,&more);
    assert(more==expectedMore);
    if (expected) { assert(request && strcmp(request,expected)==0); free(request); }
    else assert(!request);
}
int main(void)
{
    WmScreenData a={.wmWorkspaceWin=1}, b={.wmWorkspaceWin=2};
    Queue(1,"a1\0a2",6); Queue(2,"b1\0b2",6);
    Next(&a,"a1",True); Next(&b,"b1",True);
    Next(&a,"a2",False); Next(&b,"b2",False);
    assert(live==0 && fetches[1]==1 && fetches[2]==1);
    for (int which=0; which<8; ++which) {
        Queue(1,"ok",3);
        if (which==0) replies[1].type=XA_ATOM;
        if (which==1) replies[1].format=32;
        if (which==2) replies[1].status=BadWindow;
        if (which==3) replies[1].left=4;
        if (which==4) replies[1].bytes=NULL;
        if (which==5) replies[1].count=0;
        if (which==6) replies[1].count=4000001;
        if (which==7) replies[1].size=replies[1].count=2;
        Next(&a,NULL,False);
        assert(!live && !a.dtWmRequestData && !a.dtWmRequestLength && !a.dtWmRequestOffset);
    }
    assert(deletes==1);
    Queue(1,"a1\0a2",6); Queue(2,"b1",3);
    Next(&a,"a1",True);
    fail=1; Next(&a,NULL,False); fail=0;
    Next(&b,"b1",False); assert(!live);
    Queue(1,"recovered",10); Next(&a,"recovered",False);
    Queue(1,"",1); Next(&a,"",False);
    Next(NULL,NULL,False); assert(!live);
    puts("Desktop requests: per-screen interleaving, malformed batches and failed allocation passed");
}
'''
with tempfile.TemporaryDirectory(prefix="desktop-requests-", dir=os.environ["TMPDIR"]) as work:
    c, binary = Path(work) / "requests.c", Path(work) / "requests"
    c.write_text(header + text[start:end] + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu23", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    f"-fsanitize={_CK_SANITIZE_LIST}", "-g", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
