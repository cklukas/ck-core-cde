/* Real recursive file operations under long, task-owned fixture paths. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int fault, attempts;
static struct dirent *ReadDirectory(DIR *dir)
{
    if (fault==1 && attempts++==0) { errno=EINTR; return NULL; }
    if (fault==2) { errno=EIO; return NULL; }
    return readdir(dir);
}
static int CloseDirectory(DIR *dir)
{
    int result=closedir(dir);
    if (fault==3) { errno=EIO; return -1; }
    return result;
}
static int RenameForTest(const char *source, const char *target)
{
    if (fault==4 || fault==5) { errno=EXDEV; return -1; }
    return rename(source,target);
}
static ssize_t WriteForTest(int fd, const void *data, size_t length)
{
    if (fault==5) { errno=ENOSPC; return -1; }
    return write(fd,data,length);
}
static int IgnoreError(char *path, int error)
{ (void)path; assert(error==ENOSPC); return 0; }
#define readdir ReadDirectory
#define closedir CloseDirectory
#define rename RenameForTest
#define write WriteForTest
#include "dtcopy/fsrtns.c"
#undef readdir
#undef closedir
#undef rename
#undef write

static void WriteFixture(const char *path)
{
    FILE *stream=fopen(path,"w"); assert(stream);
    assert(fputs("payload",stream)>=0);
    assert(fclose(stream)==0);
}

int main(int argc, char **argv)
{
    assert(argc==2 && chdir(argv[1])==0);
    char *parent=malloc(5001), *joined;
    assert(parent); memset(parent,'p',5000); parent[5000]='\0';
    assert(ChildPath(parent,"child",&joined)==0);
    assert(strlen(joined)==5006 && strcmp(joined+5000,"/child")==0);
    free(parent); free(joined);
    assert(ChildPath("","child",&joined)==EINVAL && !joined);

    char path[4096]="tree", component[181], leaf[4096];
    memset(component,'d',180); component[180]='\0';
    assert(mkdir(path,0700)==0);
    for (int i=0; i<7; ++i) {
        size_t length=strlen(path);
        assert(snprintf(path+length,sizeof(path)-length,"/%s",component)>0);
        assert(mkdir(path,0700)==0);
    }
    assert(strlen(path)>1024);
    assert(snprintf(leaf,sizeof(leaf),"%s/leaf",path)>0);
    WriteFixture(leaf);
    int result;
    fsCopyLink(path,"copy",0,&result); assert(result==0);
    assert(access("copy/leaf",F_OK)==0);

    fault=2; fsEmpty(path,&result); assert(result==EIO);
    assert(access(leaf,F_OK)==0);
    fault=1; attempts=0; fsEmpty(path,&result); assert(result==0);
    assert(access(leaf,F_OK)!=0 && errno==ENOENT);
    WriteFixture(leaf);
    fault=3; fsEmpty(path,&result); assert(result==EIO);
    fault=0; WriteFixture(leaf);
    fault=4; fsMove(leaf,"moved",0,&result); assert(result==0);
    assert(access(leaf,F_OK)!=0 && errno==ENOENT);
    assert(access("moved",F_OK)==0);

    fault=5; errorCallback=IgnoreError;
    WriteFixture(leaf);
    fsMove(leaf,"failed-move",0,&result); assert(result==0);
    assert(access(leaf,F_OK)==0);
    fsMove("copy","skipped-tree",0,&result); assert(result==0);
    assert(access("copy/leaf",F_OK)==0);
    fault=0; errorCallback=NULL;
    assert(unlink("failed-move")==0);
    fsErase("skipped-tree",&result,1); assert(result==0);

    fault=0;
    char target[701], copied[701];
    memset(target,'x',700); target[700]='\0';
    assert(symlink(target,"link")==0);
    fsCopyLink("link","linkcopy",0,&result); assert(result==0);
    assert(readlink("linkcopy",copied,sizeof(copied))==700);
    assert(memcmp(target,copied,700)==0);
    assert(unlink("link")==0 && unlink("linkcopy")==0);
    fsErase("tree",&result,1); assert(result==0);
    fsErase("copy",&result,1); assert(result==0);
    assert(unlink("moved")==0);
    puts("File paths: long operations, skipped moves, link growth and directory errors passed");
    return 0;
}
