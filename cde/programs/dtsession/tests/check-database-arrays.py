#!/usr/bin/env python3
"""Use real Xrm resources to check production session-array index handling."""
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
    if text[start:].startswith(("Bool\n", "Boolean\n", "void\n")):
        start = text.rfind("\n", 0, start - 1) + 1
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

header = r'''
#include <X11/Intrinsic.h>
#include <X11/Xresource.h>
#include <X11/SM/SM.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static XrmQuark anyQuark;
typedef void *ClientDB;
'''
records = text[text.index("typedef struct _ClientDBRec"):text.index("static Boolean xrmInitialized")]
types = (source / "SmXSMP.h").read_text()
types = types[types.index("typedef struct _XSMPClientDBRec"):types.index("typedef struct {", types.index("typedef struct _ProxyClientDBRec"))]
strings = "\n".join(line for line in re.findall(r'^static char \*\w+ = .*;$', text, re.M)
                    if not line.startswith(("static char *clientStr ", "static char *proxyStr ")))
start = text.index("typedef struct {\n    char **strings;")
end = text.index("} StringArrayContext;", start) + len("} StringArrayContext;")
body = text[start:end] + "\n" + "\n".join(function(name) for name in
        ("_resourceName", "_countProc", "_fillClientIDProc", "_fillStringArrayProc",
         "_getStringResource", "_getIntResource", "_getStringArrayResource", "_freeStringArray"))
body += "\n" + "\n".join(function(name) for name in
                         ("FreeXSMPClientDBRec", "FreeProxyClientDBRec",
                          "GetXSMPClientDBRec", "GetProxyClientDBRec"))
main = r'''
static void Check(const char *resources, Boolean valid, int count)
{
    XrmDatabase db=XrmGetStringDatabase(resources); assert(db);
    char **values=NULL;
    assert(_getStringArrayResource(db,"%s.RestartCommand","C0",&values,NULL)==valid);
    if (valid && count) {
        assert(values);
        for (int i=0; i<count; ++i) assert(values[i]);
        assert(values[count]==NULL);
        assert(strcmp(values[0],"first")==0);
    } else assert(values==NULL);
    _freeStringArray(values);
    XrmDestroyDatabase(db);
}
int main(void)
{
    XrmInitialize(); anyQuark=XrmStringToQuark("?");
    Check("C0.RestartCommand.0: first\n",True,1);
    Check("C0.RestartCommand.1: second\nC0.RestartCommand.0: first\n",True,2);
    Check("C0.RestartCommand.-1: bad\n",False,0);
    Check("C0.RestartCommand.1: sparse\n",False,0);
    Check("C0.RestartCommand.999999999999999999999999999: huge\n",False,0);
    Check("C0.RestartCommand.0x: bad\n",False,0);
    Check("C0.RestartCommand.0: first\nC0.RestartCommand.00: alias\n",False,0);
    Check("C0.RestartCommand.0: first\nC0.RestartCommand.2: hole\n",False,0);
    Check("C0.Unrelated: absent\n",True,0);
    int count=(int)(UINT_MAX / sizeof(char *) - 1);
    assert(_countProc(NULL,NULL,NULL,NULL,NULL,(XPointer)&count));
    assert(count==-1);
    char longID[8192]; memset(longID,'a',sizeof(longID)-1); longID[sizeof(longID)-1]='\0';
    char *name=_resourceName("%s.RestartCommand",longID);
    assert(name && strlen(name)==strlen(longID)+strlen(".RestartCommand"));
    XtFree(name);
    const char *invalid[]={"", "C0.evil.more.parts", "C0*evil", "C0\\evil", "C0 evil", NULL};
    for (int i=0; invalid[i]; ++i) assert(!_resourceName("%s.RestartCommand",invalid[i]));
    const char *numbers[]={"42", "-1", " +2 ", "2147483648", "-2147483649", "99999999999999999999999999", "1garbage", "", "x"};
    for (size_t i=0; i<sizeof(numbers)/sizeof(numbers[0]); ++i) {
        XrmDatabase db=NULL;
        XrmPutStringResource(&db,"C0.ScreenNum",numbers[i]);
        int value=123;
        assert(_getIntResource(db,"%s.ScreenNum","C0",&value,7)==(i<3));
        if (i==0) assert(value==42);
        if (i==1) assert(value==-1);
        if (i==2) assert(value==2);
        if (i>=3) assert(value==123);
        XrmDestroyDatabase(db);
    }
    char *id=NULL, **cursor=&id;
    char invalidID[]="C0.evil";
    XrmValue idValue={sizeof(invalidID),invalidID};
    assert(_fillClientIDProc(NULL,NULL,NULL,NULL,&idValue,(XPointer)&cursor));
    assert(!id && cursor==&id);
    char validID[]="C0"; idValue.addr=validID; idValue.size=sizeof(validID);
    assert(!_fillClientIDProc(NULL,NULL,NULL,NULL,&idValue,(XPointer)&cursor));
    assert(id==validID);
    char hiddenID[]="C0\0.other"; id=NULL; cursor=&id;
    idValue.addr=hiddenID; idValue.size=sizeof(hiddenID);
    assert(_fillClientIDProc(NULL,NULL,NULL,NULL,&idValue,(XPointer)&cursor));
    assert(!id);
    XrmDatabase typed=NULL;
    char hiddenNumber[]="1\0trailing";
    XrmValue hiddenValue={sizeof(hiddenNumber),hiddenNumber};
    XrmPutResource(&typed,"C0.ScreenNum","String",&hiddenValue);
    int unchanged=7;
    assert(!_getIntResource(typed,"%s.ScreenNum","C0",&unchanged,0) && unchanged==7);
    XrmPutResource(&typed,"C0.RestartCommand.0","String",&hiddenValue);
    char **invalidArray=NULL;
    assert(!_getStringArrayResource(typed,"%s.RestartCommand","C0",&invalidArray,NULL));
    assert(!invalidArray); XrmDestroyDatabase(typed);
    const char *records[]={"C0.ScreenNum: 0\nC0.RestartHint: 3\n",
                           "C0.ScreenNum: -1\n", "C0.RestartHint: 4\n",
                           "C0.RestartHint: -1\n", "C0.ScreenNum: 2147483648\n"};
    for (size_t i=0; i<sizeof(records)/sizeof(records[0]); ++i) {
        char *ids[]={"C0"};
        ClientDBRec db={0}; db.xrmDB=XrmGetStringDatabase(records[i]);
        db.openForInput=True; db.XSMPClients=ids; db.nXSMPClients=1;
        db.proxyClients=ids; db.nProxyClients=1;
        XSMPClientDBRecPtr xsmp=GetXSMPClientDBRec(&db);
        assert((xsmp!=NULL)==(i==0)); FreeXSMPClientDBRec(xsmp);
        ProxyClientDBRecPtr proxy=GetProxyClientDBRec(&db);
        assert((proxy!=NULL)==(i==0 || i==2 || i==3)); FreeProxyClientDBRec(proxy);
        XrmDestroyDatabase(db.xrmDB);
    }
    puts("Session arrays: real Xrm valid lists, invalid indices, holes and aliases passed");
}
'''
with tempfile.TemporaryDirectory(prefix="session-arrays-", dir=os.environ["TMPDIR"]) as work:
    c, binary = Path(work) / "arrays.c", Path(work) / "arrays"
    c.write_text(header + records + types + strings + body + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=address,undefined", "-g",
                    str(c), "-lXt", "-lX11", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
