#!/usr/bin/env python3
"""Execute production terminal string and numeric parser operations."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
primitive = (source / "TermPrim/TermPrimParser.c").read_text()
terminal = (source / "Term/TermParse.c").read_text()

def function(text, name):
    match = re.search(r"\bvoid\s+" + re.escape(name) + r"\s*\(", text)
    assert match, name
    start = match.start()
    opening = text.index("{", match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

header = r'''
#include <X11/Intrinsic.h>
#include <assert.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "TermPrimParserP.h"
#define MAXINT INT_MAX
#define NPARAM 16
#define PCOUNT(c) ((c)->parms[0])
#define Debug(a,b) ((void)0)
static ParserContextRec context;
static ParserContext FixtureContext(Widget w) { (void)w; return &context; }
#define GetParserContext(w) FixtureContext(w)
static struct { Boolean userKeysLocked, needToLockUserKeys; } data;
typedef __typeof__(data) *DtTermData;
static struct { struct { DtTermData td; } vt; } terminalWidget;
typedef __typeof__(terminalWidget) *DtTermWidget;
static int stores;
static size_t stored_length;
static void _DtTermFunctionKeyStringStore(Widget w, int key, char *value)
{ (void)w; (void)key; ++stores; stored_length=strlen(value); }
'''
macro_start = terminal.index("#define STORELASTARG")
macro_end = terminal.index("\n\n", macro_start)
body = terminal[macro_start:macro_end] + "\n"
for name in ("_DtTermPrimParserClearParm", "_DtTermPrimParserClrStrParm",
             "_DtTermPrimParserEnterNum", "_DtTermPrimParserNumParmPush"):
    body += function(primitive, name) + "\n"
for name in ("_DtTermClearParam", "_DtTermEnterNum", "_DtTermParsePushNum",
             "_DtTermSaveChar", "_DtTermParseHexDigit", "_DtTermParseUserKeyLoad"):
    body += function(terminal, name) + "\n"
main = r'''
int main(void)
{
    assert(setlocale(LC_CTYPE,"C.UTF-8") || setlocale(LC_CTYPE,"en_US.UTF-8"));
    terminalWidget.vt.td=&data;
    Widget w=(Widget)&terminalWidget;
    _DtTermClearParam(w);
    context.inputChar[0]='A'; context.inputCharLen=1;
    for (int i=0; i<STR_SIZE+10; ++i) _DtTermSaveChar(w);
    assert(context.stringParms[0].length==STR_SIZE);
    assert(context.stringParms[0].str[STR_SIZE]=='\0');
    _DtTermClearParam(w);
    context.stringParms[0].length=STR_SIZE-1;
    context.inputChar[0]=0xc3; context.inputChar[1]=0xa9; context.inputCharLen=2;
    _DtTermSaveChar(w); assert(context.stringParms[0].length==STR_SIZE-1);
    context.stringParms[0].length=STR_SIZE-2;
    _DtTermSaveChar(w); assert(context.stringParms[0].length==STR_SIZE);
    _DtTermClearParam(w);
    context.inputChar[0]=0xff; context.inputCharLen=1;
    _DtTermSaveChar(w); assert(context.stringParms[0].length==0);
    context.inputCharLen=MB_LEN_MAX+1;
    _DtTermSaveChar(w); assert(context.stringParms[0].length==0);
    _DtTermClearParam(w);
    context.inputCharLen=1;
    for (int i=0; i<2*STR_SIZE; ++i) {
        context.inputChar[0]=i%2 ? '1' : '4'; _DtTermParseHexDigit(w);
    }
    assert(context.stringParms[0].length==2*STR_SIZE);
    context.inputChar[0]='F'; _DtTermParseHexDigit(w);
    assert(context.stringParms[0].length==2*STR_SIZE);
    _DtTermParseUserKeyLoad(w);
    assert(stores==1 && stored_length==STR_SIZE);
    context.inputCharLen=1; context.inputChar[0]='4'; _DtTermParseHexDigit(w);
    _DtTermParseUserKeyLoad(w); assert(stores==1);
    context.inputCharLen=1; context.inputChar[0]='G'; _DtTermParseHexDigit(w);
    assert(context.stringParms[0].length==0);
    _DtTermClearParam(w);
    for (int i=0; i<50000; ++i) {
        context.inputChar[0]='1'; _DtTermEnterNum(w); _DtTermParsePushNum(w);
    }
    STORELASTARG(&context);
    assert(PCOUNT(&context)==NPARAM && context.parameterOverflow);
    for (int i=1; i<=NPARAM; ++i) assert(context.parms[i]==1);
    for (int i=NPARAM+1; i<NUM_PARMS; ++i) assert(context.parms[i]==0);
    _DtTermClearParam(w); assert(!context.parameterOverflow);
    context.workingNum=42;
    _DtTermPrimParserNumParmPush(w,-1);
    assert(context.workingNum==42);
    puts("Terminal parser: string capacity, UTF-8, 512 hex digits and excess parameters passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="terminal-parser-", dir=os.environ["TMPDIR"]) as work:
    c, binary = Path(work) / "parser.c", Path(work) / "parser"
    c.write_text(header + body + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", str(source / "TermPrim"),
                    str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
