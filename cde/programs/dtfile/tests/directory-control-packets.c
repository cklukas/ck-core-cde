#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef int Boolean;
typedef short Position;
#define True 1
#define False 0
#define NILL '\0'
#define MAX_PATH 1024
#define PIPEMSG_ERROR 1
#define PIPEMSG_DONE 3
#define PIPEMSG_PATH_LOGICAL_TYPES 4
#define DIRECTORY_PATH_LIMIT (MAX_PATH + 1)
#define DIRECTORY_POSITION_LIMIT (256 * 1024)

typedef struct {
    char *name;
    Position x;
    Position y;
    int stacking_order;
} PositionInfo;

static void *test_calloc(size_t n,size_t s){void *p=calloc(n,s);if(!p)abort();return p;}
static void *test_malloc(size_t s){void *p=malloc(s);if(!p)abort();return p;}
static char *test_new_string(const char *s){size_t n=strlen(s)+1;char *p=malloc(n);if(!p)abort();memcpy(p,s,n);return p;}
#define XtCalloc test_calloc
#define XtMalloc test_malloc
#define XtFree free
#define XtNewString test_new_string

static int DtfileProcessWriteAll(int fd,const void *data,size_t size)
{
    const char *next=data;
    if(fd<0||(!data&&size)){errno=EINVAL;return -1;}
    while(size){ssize_t n=write(fd,next,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;next+=n;size-=(size_t)n;}
    return 0;
}

/* PRODUCTION_FUNCTIONS */

static int failed;
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);failed=1;} } while(0)

static void must_write(int fd,const void *data,size_t size)
{
    const char *next=data;
    while(size){ssize_t n=write(fd,next,size);if(n<=0)abort();next+=n;size-=(size_t)n;}
}

static void send_string(int fd,const char *value)
{
    short length=value?(short)strlen(value):0;
    must_write(fd,&length,sizeof(length));if(length)must_write(fd,value,(size_t)length);
}

static void free_positions(PositionInfo *values,int count)
{
    int i;for(i=0;i<count;i++)free(values[i].name);free(values);
}

static void array_case(const char *name)
{
    size_t bytes=99;
    if(!strcmp(name,"array-negative"))CHECK(DirectoryArrayBytes(-1,8,10,&bytes)==-1&&errno==EPROTO);
    else if(!strcmp(name,"array-over-limit"))CHECK(DirectoryArrayBytes(11,8,10,&bytes)==-1&&errno==EPROTO);
    else CHECK(DirectoryArrayBytes(3,8,10,&bytes)==0&&bytes==24);
}

static void path_case(const char *name)
{
    int p[2],count=99;char **types=(char **)1,*tt=(char *)1;int encoded;
    CHECK(pipe(p)==0);
    if(!strcmp(name,"path-negative"))encoded=-1;
    else if(!strcmp(name,"path-over-limit"))encoded=DIRECTORY_PATH_LIMIT+1;
    else encoded=2;
    must_write(p[1],&encoded,sizeof(encoded));
    if(encoded==2){send_string(p[1],"TYPE1");send_string(p[1],!strcmp(name,"path-null-type")?NULL:"TYPE2");
        if(strcmp(name,"path-truncated"))send_string(p[1],"tt:/path");}
    close(p[1]);
    int rc=DirectoryReadPathInfo(p[0],&count,&types,&tt);close(p[0]);
    if(!strcmp(name,"path-valid")){CHECK(rc==0&&count==2&&!strcmp(types[1],"TYPE2")&&!strcmp(tt,"tt:/path"));DirectoryFreeStrings(types,count);free(tt);}
    else CHECK(rc==-1&&count==0&&types==NULL&&tt==NULL);
}

static void position_case(const char *name)
{
    int p[2],count=99;PositionInfo *values=(PositionInfo *)1;int encoded;
    Position x=7,y=9;int order=11;
    CHECK(pipe(p)==0);
    if(!strcmp(name,"position-negative"))encoded=-1;
    else if(!strcmp(name,"position-over-limit"))encoded=DIRECTORY_POSITION_LIMIT+1;
    else encoded=1;
    must_write(p[1],&encoded,sizeof(encoded));
    if(encoded==1){send_string(p[1],"icon");must_write(p[1],&x,sizeof(x));
        if(strcmp(name,"position-truncated")){must_write(p[1],&y,sizeof(y));must_write(p[1],&order,sizeof(order));}}
    close(p[1]);
    int rc=DirectoryReadPositionInfo(p[0],&count,&values);close(p[0]);
    if(!strcmp(name,"position-valid")){CHECK(rc==0&&count==1&&!strcmp(values[0].name,"icon")&&values[0].x==7&&values[0].stacking_order==11);free_positions(values,count);}
    else CHECK(rc==-1&&count==0&&values==NULL);
}

static void position_file_case(const char *name)
{
    FILE *fp=tmpfile();PositionInfo *values=(PositionInfo *)1;int count,i;
    CHECK(fp!=NULL);
    if(!strcmp(name,"position-file-negative"))fputs("-1\n",fp);
    else if(!strcmp(name,"position-file-over-limit"))fprintf(fp,"%d\n",DIRECTORY_POSITION_LIMIT+1);
    else if(!strcmp(name,"position-file-long-name")){fputs("1\n",fp);for(i=0;i<MAX_PATH;i++)fputc('x',fp);fputs("\n1 2 3\n",fp);}
    else if(!strcmp(name,"position-file-coordinate-range"))fputs("1\nicon\n32768 2 3\n",fp);
    else if(!strcmp(name,"position-file-truncated"))fputs("2\none\n1 2 3\n",fp);
    else fputs("2\none\n1 2 3\ntwo\n4 5 6\n",fp);
    rewind(fp);count=DirectoryLoadPositionInfo(fp,&values);fclose(fp);
    if(!strcmp(name,"position-file-valid")){CHECK(count==2&&!strcmp(values[1].name,"two")&&values[1].y==5);free_positions(values,count);}
    else CHECK(count==-1&&values==NULL);
}

static void position_path_case(const char *name)
{
    char path[MAX_PATH];char directory[MAX_PATH+1];int rc;
    if(!strcmp(name,"file-path-long")){memset(directory,'a',MAX_PATH);directory[MAX_PATH]='\0';rc=DirectoryBuildChildPath(path,sizeof(path),directory,"positions");CHECK(rc==-1&&errno==ENAMETOOLONG&&path[0]=='\0');}
    else if(!strcmp(name,"file-path-copy")){rc=DirectoryBuildFilePath(path,sizeof(path),"relative",NULL);CHECK(rc==0&&!strcmp(path,"relative"));}
    else if(!strcmp(name,"file-path-copy-long")){memset(directory,'a',MAX_PATH);directory[MAX_PATH]='\0';rc=DirectoryBuildFilePath(path,sizeof(path),directory,NULL);CHECK(rc==-1&&errno==ENAMETOOLONG&&path[0]=='\0');}
    else if(!strcmp(name,"file-path-link-relative")){strcpy(path,"/one/two/link");rc=DirectoryResolveLinkPath(path,sizeof(path),"../target");CHECK(rc==0&&!strcmp(path,"/one/two/../target"));}
    else if(!strcmp(name,"file-path-link-no-parent")){strcpy(path,"link");rc=DirectoryResolveLinkPath(path,sizeof(path),"target");CHECK(rc==0&&!strcmp(path,"target"));}
    else if(!strcmp(name,"file-path-link-absolute")){strcpy(path,"/old/link");rc=DirectoryResolveLinkPath(path,sizeof(path),"/new/target");CHECK(rc==0&&!strcmp(path,"/new/target"));}
    else if(!strcmp(name,"file-path-link-long")){strcpy(path,"/base/link");memset(directory,'a',MAX_PATH);directory[MAX_PATH]='\0';rc=DirectoryResolveLinkPath(path,sizeof(path),directory);CHECK(rc==-1&&errno==ENAMETOOLONG&&path[0]=='\0');}
    else if(!strcmp(name,"file-path-root")){rc=DirectoryBuildChildPath(path,sizeof(path),"/","positions");CHECK(rc==0&&!strcmp(path,"/positions"));}
    else {rc=DirectoryBuildChildPath(path,sizeof(path),"/tmp","positions");CHECK(rc==0&&!strcmp(path,"/tmp/positions"));}
}

static void writer_case(const char *name)
{
    int p[2];short tag;long time_value=17,decoded_time=0;int error=23,decoded_error=0,count=0;char **types=NULL,*tt=NULL;
    CHECK(pipe(p)==0);
    if(!strcmp(name,"write-done"))CHECK(DirectoryWriteDone(p[1],time_value)==0);
    else if(!strcmp(name,"write-error"))CHECK(DirectoryWriteError(p[1],error,time_value)==0);
    else if(!strcmp(name,"write-position")){PositionInfo sent={"icon",3,4,5};CHECK(PipeWritePositionInfo(p[1],&sent)==0);}
    else {char *sent[2]={"A","B"};CHECK(DirectoryWritePathInfo(p[1],2,sent,"tt:/x")==0);}
    close(p[1]);
    if(!strcmp(name,"write-position")){PositionInfo got;CHECK(PipeReadPositionInfo(p[0],&got)==0&&!strcmp(got.name,"icon")&&got.y==4&&got.stacking_order==5);free(got.name);close(p[0]);return;}
    CHECK(PipeReadExact(p[0],&tag,sizeof(tag))==0);
    if(!strcmp(name,"write-done")){CHECK(tag==PIPEMSG_DONE&&PipeReadExact(p[0],&decoded_time,sizeof(decoded_time))==0&&decoded_time==17);}
    else if(!strcmp(name,"write-error")){CHECK(tag==PIPEMSG_ERROR&&PipeReadExact(p[0],&decoded_error,sizeof(decoded_error))==0&&PipeReadExact(p[0],&decoded_time,sizeof(decoded_time))==0&&decoded_error==23&&decoded_time==17);}
    else {CHECK(tag==PIPEMSG_PATH_LOGICAL_TYPES&&DirectoryReadPathInfo(p[0],&count,&types,&tt)==0&&count==2&&!strcmp(tt,"tt:/x"));DirectoryFreeStrings(types,count);free(tt);}
    close(p[0]);
}

static void time_case(void)
{
    char value[100];time_t stamp=0;
    DirectoryFormatFileTime(stamp,value,sizeof(value));
    CHECK(value[0]!='\0'&&value[0]!='?'&&strchr(value,'\n')==NULL);
}

static void timer_case(const char *name)
{
    int p[2],status=99,decoded_status=99;long stamp=17,decoded_stamp=99;Boolean changed=True,decoded_changed=True;
    CHECK(pipe(p)==0);
    if(!strcmp(name,"status-write")){CHECK(DirectoryWriteStatus(p[1],23)==0);close(p[1]);CHECK(PipeReadExact(p[0],&decoded_status,sizeof(decoded_status))==0&&decoded_status==23);close(p[0]);return;}
    if(!strcmp(name,"timer-write")){CHECK(DirectoryWriteTimerResult(p[1],23,stamp,True)==0);close(p[1]);CHECK(DirectoryReadTimerResult(p[0],&decoded_status,&decoded_stamp,&decoded_changed)==0&&decoded_status==23&&decoded_stamp==17&&decoded_changed==True);close(p[0]);return;}
    must_write(p[1],&status,sizeof(status));
    if(strcmp(name,"timer-truncated-time"))must_write(p[1],&stamp,sizeof(stamp));
    if(strcmp(name,"timer-truncated-time")&&strcmp(name,"timer-truncated-boolean")){if(!strcmp(name,"timer-invalid-boolean"))changed=2;must_write(p[1],&changed,sizeof(changed));}
    close(p[1]);
    int rc=DirectoryReadTimerResult(p[0],&decoded_status,&decoded_stamp,&decoded_changed);close(p[0]);
    if(!strcmp(name,"timer-valid"))CHECK(rc==0&&decoded_status==99&&decoded_stamp==17&&decoded_changed==True);
    else CHECK(rc==-1&&decoded_status==EPROTO&&decoded_stamp==0&&decoded_changed==False);
}

int main(int argc,char **argv)
{
    const char *name;if(argc!=2)return 2;name=argv[1];
    if(!strncmp(name,"array-",6))array_case(name);
    else if(!strncmp(name,"path-",5))path_case(name);
    else if(!strncmp(name,"position-file-",14))position_file_case(name);
    else if(!strncmp(name,"position-",9))position_case(name);
    else if(!strncmp(name,"file-path-",10))position_path_case(name);
    else if(!strcmp(name,"time-format-valid"))time_case();
    else if(!strncmp(name,"write-",6))writer_case(name);
    else if(!strncmp(name,"timer-",6)||!strcmp(name,"status-write"))timer_case(name);
    else return 2;
    if(!failed)printf("PASS %s\n",name);
    return failed;
}
