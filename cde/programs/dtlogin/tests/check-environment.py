#!/usr/bin/env python3
"""Run login environment helpers under allocation and putenv failure injection."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
text = (source / "util.c").read_text()

def function(name):
    pattern = r"(?:static\s+)?(?:unsigned\s+char|char|void|int)\s*(?:\*\s*)*\s*" + name + r"\s*\("
    for match in re.finditer(pattern, text):
        opening = text.index("{", match.end())
        if text.find(";", match.end(), opening) >= 0:
            continue
        depth, end = 1, opening + 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        return text[match.start():end]
    raise AssertionError(name)

header = r'''
#include <assert.h>
#include <errno.h>
#include <nl_types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int Bool;
#define False 0
#define True 1
#define CDE_INSTALLATION_TOP "/usr/dt"
#define MC_LOG_SET 0
#define MC_LOG_MAKEENV 0
#define MC_DEF_LOG_MAKEENV "makeEnv"
#define MC_LOG_SETENV 0
#define MC_DEF_LOG_SETENV "setEnv"
static nl_catd nl_fd;
static int allocations, fail_at, live, reject_environment;
static char *installed;
static void *Allocate(size_t size)
{
    if (++allocations==fail_at) { errno=ENOMEM; return NULL; }
    void *p=malloc(size); if (p) ++live; return p;
}
static void *Grow(void *old, size_t size)
{
    if (++allocations==fail_at) { errno=ENOMEM; return NULL; }
    void *p=realloc(old,size); if (p && !old) ++live; return p;
}
static void Release(void *p) { if (p) { --live; free(p); } }
static char *Duplicate(const char *s)
{ size_t n=strlen(s)+1; char *p=Allocate(n); if (p) memcpy(p,s,n); return p; }
static char *CurrentEnvironment(const char *name)
{ assert(strcmp(name,"NLSPATH")==0); return "/existing/%N.cat"; }
static int InstallEnvironment(char *value)
{
    assert(value && strncmp(value,"NLSPATH=",8)==0);
    if (reject_environment) return -1;
    installed=value; return 0;
}
static nl_catd OpenCatalog(const char *name, int flags)
{ (void)name; (void)flags; return (nl_catd)1; }
static char *CatalogText(nl_catd c, int set, int msg, const char *fallback)
{ (void)c; (void)set; (void)msg; return (char *)fallback; }
static void LogOutOfMem(unsigned char *message) { (void)message; }
#define CATOPEN OpenCatalog
#define CATGETS CatalogText
#define malloc Allocate
#define realloc Grow
#define free Release
#define strdup Duplicate
#define getenv CurrentEnvironment
#define putenv InstallEnvironment
'''
body = "\n".join(function(name) for name in
                 ("ReadCatalog", "EnvAllocationSize", "makeEnv", "getEnv", "setEnv", "freeEnv", "parseEnv"))
main = r'''
#undef malloc
#undef realloc
#undef free
#undef strdup
#undef getenv
#undef putenv
int main(int argc, char **argv)
{
    assert(argc==2);
    int mode=atoi(argv[1]);
    fail_at=mode==1 ? 1 : 0; reject_environment=mode==2;
    assert(strcmp((char *)ReadCatalog(0,0,"fallback"),"fallback")==0);
    assert((installed!=NULL)==(mode==0));
    if (installed) assert(strstr(installed,"/existing/%N.cat:"));
    fail_at=0; reject_environment=0;
    size_t size;
    assert(EnvAllocationSize(1,2,&size) && size==5);
    assert(!EnvAllocationSize(SIZE_MAX,1,&size));
    assert(!EnvAllocationSize(SIZE_MAX-2,1,&size));
    assert(getEnv(NULL,"MISSING")==NULL);
    char **environment=setEnv(NULL,"ONE","first"); assert(environment);
    environment=setEnv(environment,"ONE","second");
    assert(strcmp(getEnv(environment,"ONE"),"second")==0);
    environment=setEnv(environment,"MARKER","");
    assert(strcmp(environment[1],"MARKER")==0);
    for (int fail=1; fail<=2; ++fail) {
        int before=live; char **old=environment;
        allocations=0; fail_at=fail;
        environment=setEnv(environment,"NEW","value");
        assert(environment==old && getEnv(environment,"NEW")==NULL && live==before);
    }
    fail_at=0;
    char outer[]="a b";
    assert(strcmp(strtok(outer," "),"a")==0);
    environment=parseEnv(environment,"TWO=two THREE=three");
    assert(strcmp(getEnv(environment,"TWO"),"two")==0);
    assert(strcmp(strtok(NULL," "),"b")==0);
    int before=live;
    allocations=0; fail_at=1;
    char **old=environment;
    environment=parseEnv(environment,"FOUR=four");
    assert(environment==old && live==before && getEnv(environment,"FOUR")==NULL);
    fail_at=0;
    freeEnv(environment);
    Release(installed);
    assert(live==0);
    puts("Login environment: checked sizes, updates, allocation failures and tokenizer ownership passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="login-environment-", dir=os.environ["TMPDIR"]) as work:
    c, binary = Path(work) / "environment.c", Path(work) / "environment"
    c.write_text(header + body + main)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(c), "-o", str(binary)], check=True)
    for mode in range(3):
        subprocess.run([str(binary), str(mode)], check=True)
