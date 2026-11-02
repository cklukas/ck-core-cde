#include "CvStringI.h"
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(setlocale(LC_CTYPE,"C.UTF-8") || setlocale(LC_CTYPE,"en_US.UTF-8"));
    const char text[]="A\xe2\x82\xacZ";
    assert(_DtCvChar(text,0,0)==L'A');
    for (int i=1; i<=3; ++i) assert(_DtCvChar(text,0,i)==0x20ac);
    assert(_DtCvChar(text,0,4)==L'Z');
    assert(_DtCvChar(text,0,5)==0);
    assert(_DtCvChar(text,0,6)==(wchar_t)-1);
    assert(_DtCvChar(text,0,-1)==(wchar_t)-1);
    assert(_DtCvChar(NULL,0,0)==(wchar_t)-1);
    assert(_DtCvChar("\xff" "A",0,0)==(wchar_t)-1);
    assert(_DtCvChar("\xff" "A",0,1)==L'A');
    assert(_DtCvChar("\xe2\x82",0,1)==(wchar_t)-1);
    assert(_DtCvChar(L"AB",1,1)==L'B');
    assert(_DtCvChar(L"AB",1,2)==0);
    assert(_DtCvChar(L"AB",1,3)==(wchar_t)-1);
    struct { char value[4]; char guard; } out={{0},'G'};
    assert(_DtCvCopyFirstChar("\xe2\x82\xacZ",out.value,sizeof(out.value))==3);
    assert(strcmp(out.value,"\xe2\x82\xac")==0 && out.guard=='G');
    assert(_DtCvCopyFirstChar("\xe2\x82\xac",out.value,3)==0);
    assert(out.value[0]=='\0' && out.guard=='G');
    assert(_DtCvCopyFirstChar("\xff",out.value,sizeof(out.value))==0);
    assert(_DtCvCopyFirstChar("\xe2\x82",out.value,sizeof(out.value))==0);
    assert(_DtCvCopyFirstChar(NULL,out.value,sizeof(out.value))==0);
    assert(_DtCvCopyFirstChar("A",NULL,0)==0);
    assert(_DtCvCopyFirstChar("",out.value,sizeof(out.value))==0);
    assert(_DtCvCopyFirstChar("AB",out.value,sizeof(out.value))==1);
    assert(strcmp(out.value,"A")==0);
    puts("Help strings: byte offsets, malformed UTF-8 and bounded alignment copies passed");
    return 0;
}
