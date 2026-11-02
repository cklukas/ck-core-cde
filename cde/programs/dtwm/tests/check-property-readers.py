#!/usr/bin/env python3
"""Execute production property readers with controlled Xlib replies.

Reader functions are extracted to avoid the window manager's unrelated link
dependencies. Normal-hint records use the production WmICCC.h definitions;
the Motif WM info record is opaque because its reader never accesses members.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "WmProperty.c"
text = source.read_text()
functions = []
for signature in ("PropWMState *\nGetWMState(",
                  "PropMwmInfo *GetMwmInfo (", "char *GetUtf8String (",
                  "PropMwmHints *\nGetMwmHints(",
                  "SizeHints * \nGetNormalHints(",
                  "void ProcessWmColormapWindows ("):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    functions.append(text[start:end])

harness = r'''
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
typedef struct { int x, y; } AspectRatio;
#include "WmICCC.h"
typedef struct PropMwmInfo PropMwmInfo;
typedef struct PropMwmHints PropMwmHints;
#define PROP_MWM_INFO_ELEMENTS 2
#define PROP_MWM_HINTS_ELEMENTS 5
#define MAX_COLORMAP_WINDOWS_COUNT 40
#define DISPLAY ((Display *)0)
static struct {
    Atom xa_WM_STATE, xa_MWM_INFO, xa_UTF8_STRING, xa_WM_COLORMAP_WINDOWS;
    XContext windowContextType;
    Atom xa_MWM_HINTS;
} wmGD = {11,12,13,14,0,15};
typedef struct {
    Window client, *cmapWindows;
    Colormap clientColormap, *clientCmapList;
    int clientCmapCount, clientCmapIndex, *clientCmapFlags;
} ClientData;
static SizeHints sizeHints;
static int result, format, releases, allocate;
static Atom reply_type;
static unsigned long count, remaining;
static int xt_calls, xt_live, xt_fail, no_colormaps;
#define GETMESSAGE(a,b,c) (c)
static void Warning(char *s) { (void)s; }
static int HasProperty(ClientData *c, Atom a) { (void)c; (void)a; return 1; }
static void *XtMalloc(size_t n)
{
    if (++xt_calls == xt_fail) return NULL;
    void *p=malloc(n); assert(p); ++xt_live; return p;
}
static void *XtCalloc(size_t n, size_t s)
{
    if (++xt_calls == xt_fail) return NULL;
    void *p=calloc(n,s); assert(p); ++xt_live; return p;
}
static void XtFree(char *p) { assert(p); --xt_live; free(p); }
static Colormap FindColormap(ClientData *c, Window w)
{ (void)c; return no_colormaps ? None : w+100; }
static void ResetColormapData(ClientData *c, Window *w, int n)
{ (void)c; if (n) assert(w[0]==1); }
int XFindContext(Display *d, XID w, XContext c, XPointer *p)
{ (void)d; (void)w; (void)c; (void)p; return 0; }
int XChangeWindowAttributes(Display *d, Window w, unsigned long m, XSetWindowAttributes *a)
{ (void)d; (void)w; (void)m; (void)a; return 0; }
Status XGetWindowAttributes(Display *d, Window w, XWindowAttributes *a)
{ (void)d; (void)w; (void)a; return 0; }
int XGetWindowProperty(Display *d, Window w, Atom p, long o, long l,
    Bool del, Atom req, Atom *t, int *f, unsigned long *n,
    unsigned long *left, unsigned char **data)
{
    (void)d; (void)w; (void)p; (void)o; (void)l; (void)del; (void)req;
    *t=reply_type; *f=format; *n=count; *left=remaining;
    *data=allocate ? calloc(count+1,sizeof(long)) : NULL;
    if (*data) for (unsigned long i=0; i<count; ++i)
        ((unsigned long *)*data)[i]=i+2;
    return result;
}
int XFree(void *p) { assert(p); ++releases; free(p); return 0; }
'''
harness += "\n".join(functions)
harness += r'''
static void *read_property(int reader)
{
    if (reader == 0) return GetWMState(1);
    if (reader == 1) return GetMwmInfo(1);
    if (reader == 2) return GetUtf8String(NULL,1,99);
    ClientData client={0};
    return GetMwmHints(&client);
}
int main(void)
{
    for (int reader=0; reader<4; ++reader) {
        for (int scenario=0; scenario<9; ++scenario) {
            result=Success; reply_type=reader==3 ? 15 : 11+reader;
            format=reader==2 ? 8 : 32;
            count=reader==3 ? 5 : 2; remaining=0; releases=0; allocate=1;
            switch(scenario) {
            case 1: result=BadWindow; break;
            case 2: reply_type=99; break;
            case 3: format=16; break;
            case 4: count=0; break;
            case 5: remaining=4; break;
            case 6: allocate=0; break;
            case 7: count=1; break;
            case 8: count=3; break;
            }
            int valid=scenario==0 || (reader==2 && scenario>=7) ||
                      (reader==3 && scenario==5);
            void *p=read_property(reader);
            assert((p!=NULL)==valid);
            assert(releases==(allocate && !valid));
            if (p) XFree(p);
        }
    }
    assert(GetMwmHints(NULL)==NULL);
    for (unsigned long fields=0; fields<=18; ++fields) {
        ClientData client={0};
        result=Success; reply_type=XA_WM_SIZE_HINTS; format=32;
        count=fields; remaining=0; allocate=1;
        SizeHints *h=GetNormalHints(&client);
        assert((h->flags!=0)==(fields==15 || fields==18));
        if (fields==15) assert(h->icccVersion==ICCC_R2);
        if (fields==18) assert(h->icccVersion==ICCC_CURRENT);
    }
    for (int scenario=0; scenario<11; ++scenario) {
        ClientData client={0}; client.client=1; client.clientColormap=101;
        result=Success; reply_type=XA_WINDOW; format=32;
        count=2; remaining=0; allocate=1;
        xt_calls=0; xt_fail=0; xt_live=0; no_colormaps=0;
        if (scenario>=1 && scenario<=3) xt_fail=scenario;
        if (scenario==4) format=16;
        if (scenario==5) remaining=4;
        if (scenario==6) allocate=0;
        if (scenario==7) reply_type=XA_ATOM;
        if (scenario==8) { no_colormaps=1; client.client=2; }
        if (scenario==9) count=41;
        if (scenario==10) count=0;
        ProcessWmColormapWindows(&client);
        if (scenario==0) {
            assert(client.clientCmapCount==3);
            assert(client.cmapWindows[2]==3);
            assert(client.clientCmapList[2]==103);
            XtFree((char *)client.cmapWindows);
            XtFree((char *)client.clientCmapList);
            XtFree((char *)client.clientCmapFlags);
        } else {
            assert(client.clientCmapCount==0);
            assert(client.clientColormap==101);
        }
        assert(xt_live==0);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="dtwm-properties-", dir=os.environ["TMPDIR"]) as work:
    c = Path(work) / "readers.c"
    binary = Path(work) / "readers"
    c.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   shlex.split(os.environ.get("CPPFLAGS", "")) +
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-I", str(source.parent),
                    "-Wall", "-Wextra", "-Wno-sign-compare",
                    "-Werror", "-fsanitize=address,undefined", "-g",
                    str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("dtwm properties: 36 reply, 19 normal-hint and 11 colormap cases passed (ASan/UBSan)")
