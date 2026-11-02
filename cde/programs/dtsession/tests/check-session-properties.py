#!/usr/bin/env python3
"""Execute session property readers with controlled Xlib data and ownership."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]

def function(file, signature):
    text = (source / file).read_text()
    start = text.index(signature)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

harness = r'''
#include <X11/Intrinsic.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SmWindow.h"
#define _XA_WM_STATE "WM_STATE"
#define SM_FREE(p) abort()
static struct { Display *display; } smGD;
static Atom XaSmClientId=10, XaWmState=20;
typedef struct ClientRec {
    char *clientId;
    int screenNum;
    struct ClientRec *next;
} ClientRec, *ClientRecPtr;
static ClientRec client={"id",0,NULL};
static ClientRecPtr connectedList=&client;
static int scenario, live, lists, tree;
static void *Reply(size_t size)
{ void *p=calloc(size+1,1); assert(p); ++live; return p; }
int XFree(void *p) { assert(p); --live; free(p); return 0; }
void XFreeStringList(char **list)
{ assert(list); --lists; free(list[0]); free(list); }
Status XGetCommand(Display *d, Window w, char ***argv, int *argc)
{
    (void)d; (void)w;
    *argv=calloc(2,sizeof(char *)); assert(*argv);
    (*argv)[0]=strdup("app"); assert((*argv)[0]); ++lists;
    *argc=scenario==9 ? 0 : 1;
    return scenario!=10;
}
Status XGetWMClientMachine(Display *d, Window w, XTextProperty *p)
{
    (void)d; (void)w;
    p->value=Reply(4); memcpy(p->value,"host",4);
    p->encoding=scenario==13 ? XA_ATOM : XA_STRING;
    p->format=scenario==11 ? 16 : 8;
    p->nitems=scenario==12 ? 0 : 4;
    if (scenario==15) p->value[0]='\0';
    return True;
}
int XGetWindowProperty(Display *d, Window w, Atom p, long o, long l,
    Bool del, Atom req, Atom *t, int *f, unsigned long *n,
    unsigned long *left, unsigned char **data)
{
    (void)d; (void)o; (void)l; (void)del; (void)req;
    *left=0;
    if (p==XaSmClientId) {
        *t=(scenario==0 || scenario==14 || scenario<9) ? XA_STRING : None;
        *f=scenario==1 ? 16 : 8;
        *n=scenario==3 ? 0 : scenario==8 || scenario==14 ? 3 : 2;
        *data=scenario==4 ? NULL : Reply(*n);
        if (*data) memcpy(*data,"id",*n<2 ? *n : 2);
        if (scenario==2) *left=1;
        if (scenario==5) *t=XA_ATOM;
        if (scenario==7 && *data) (*data)[0]='\0';
        if (scenario==8) { (*data)[1]='\0'; (*data)[2]='d'; }
        return scenario==6 ? BadWindow : Success;
    }
    int s=tree && w==2 ? 0 : scenario;
    *t=s==5 ? XA_STRING : XaWmState;
    *f=s==1 ? 8 : 32;
    *n=s==2 ? 1 : s==3 ? 0 : s==11 ? 3 : 2;
    *left=s==4 ? 4 : 0;
    *data=s==7 ? NULL : Reply(*n*sizeof(unsigned long));
    if (*data && *n) ((unsigned long *)*data)[0]=
        s==8 ? ULONG_MAX : s==9 ? IconicState : s==10 ? WithdrawnState : NormalState;
    return s==6 ? BadWindow : Success;
}
Status XGetWindowAttributes(Display *d, Window w, XWindowAttributes *a)
{ (void)d; (void)w; (void)a; return scenario!=12; }
Atom XInternAtom(Display *d, const char *name, Bool only)
{ (void)d; (void)name; (void)only; return 20; }
Status XQueryTree(Display *d, Window w, Window *root, Window *parent,
                  Window **children, unsigned int *count)
{
    (void)d; *root=1; *parent=0;
    *count=tree && w==1 ? 1 : 0;
    *children=Reply(sizeof(Window));
    (*children)[0]=2;
    return True;
}
'''
harness += function("SmProperty.c", "Status GetStandardProperties(")
harness += function("SmWindow.c", "static WindowInfo \nGetTopLevelWindowInfo(")
harness += r'''
int main(void)
{
    assert(sizeof(((WM_STATE *)0)->state)==sizeof(unsigned long));
    for (scenario=0; scenario<16; ++scenario) {
        int argc=77; char **argv=(char **)1, *machine=(char *)1;
        Boolean xsmp=False;
        int result=GetStandardProperties(1,3,&argc,&argv,&machine,&xsmp);
        if (scenario==0 || scenario==14) {
            assert(!result && xsmp && client.screenNum==3);
            assert(!argv && !machine && argc==0);
        } else if (scenario==9 || scenario==10) {
            assert(!result && !xsmp && !argv && !machine && argc==0);
        } else {
            assert(result && !xsmp && argc==1 && argv);
            assert((machine!=NULL)==!(scenario==11 || scenario==12 || scenario==13 || scenario==15));
            XFreeStringList(argv);
            if (machine) XFree(machine);
        }
        assert(live==0 && lists==0);
    }
    for (scenario=0; scenario<13; ++scenario) {
        WindowInfo result=GetTopLevelWindowInfo(1);
        int valid=scenario==0 || scenario==9 || scenario==10;
        assert((result.wid!=0)==valid);
        if (valid) assert(result.termState==(scenario==9 ? IconicState : scenario==10 ? WithdrawnState : NormalState));
        assert(live==0);
    }
    tree=1; scenario=5;
    WindowInfo nested=GetTopLevelWindowInfo(1);
    assert(nested.wid==2 && nested.termState==NormalState && live==0);
    puts("Session properties: 30 reply/ownership and recursive-window cases passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="session-properties-", dir=os.environ["TMPDIR"]) as work:
    c, binary = Path(work) / "session.c", Path(work) / "session"
    c.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", str(source),
                    str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
