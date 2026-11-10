/* Execute extracted, unchanged production transfer functions. No Xt session.
 * Directory/UI branches are unreachable adapters; regular files, buffers,
 * protocol records and all transfer/completion decisions remain production.
 * Syscall faults are test-only reported returns, not native-device failures. */
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef int Boolean;
typedef void *Widget;
typedef char *String;
typedef int Tt_status;
typedef struct DesktopRec DesktopRec;
typedef struct { int file_count; } FileOpCBData;
typedef struct { int file_count; int done_count; } MoveToTrashCBData;
typedef void (*FileOperationErrorProc)(Widget, char *, char *);
#define True 1
#define False 0
#define TRUE 1
#define FALSE 0
#define TT_OK 0
#define DPRINTF(args) ((void)0)
#define GETMESSAGE(set,id,text) (text)
#define XtMalloc(n) malloc(n)
#ifdef TRANSFER_INCLUDE_WORKERS
#define XtCalloc(n,s) calloc(n,s)
#endif
#define XtRealloc(p,n) realloc(p,n)
#define XtNewString(s) strdup(s)
#define XtFree(p) free(p)
#ifndef S_IFNWK
#define S_IFNWK 0
#endif
#include "transfer-definitions.inc"
#include "SharedMsgs.h"
#include "FileProcess.h"
#ifdef TRANSFER_INCLUDE_WORKERS
#include "FileConflict.h"
#include "FileConflictWire.h"
#endif

static char *home_host_name = "local", *root_title = "root";
static char *desktop_dir = "/unused-desktop", *MOVE_CMD = "/bin/mv", *LINK_CMD = "/bin/ln";
static const char *case_name;
static int failed, error_messages, source_fd=-1, target_fd=-1;
static int source_closes, target_closes, link_calls, read_calls, write_calls;
static int injected, write_error, close_error, cleanup_error, target_collision;
static int resolve_target_error, resolve_first_error, resolve_collision_error, resolve_first_calls;
static int worker_fault, worker_last_fault, seen_first, seen_second, executable_calls;
static int same_name_generations, same_path_splits;
static char message[4096];

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",case_name,__LINE__,#c); failed=1; } } while (0)
static void fixture(int ok) { if (!ok) { perror("transfer fixture"); exit(2); } }
static void put(const char *name,const void *bytes,size_t size)
{
    int fd=open(name,O_WRONLY|O_CREAT|O_EXCL,0600); fixture(fd>=0);
    fixture(write(fd,bytes,size)==(ssize_t)size); fixture(close(fd)==0);
}
static int exact(const char *name,const void *bytes,size_t size)
{
    unsigned char data[9000]; int fd=open(name,O_RDONLY);
    if (fd<0) return 0;
    ssize_t n=read(fd,data,sizeof(data)); int result=n==(ssize_t)size && !memcmp(data,bytes,size);
    fixture(close(fd)==0); return result;
}
static int absent(const char *name) { struct stat st; errno=0; return lstat(name,&st)<0 && errno==ENOENT; }
static void unreachable(void) { fputs("Unexpected directory/UI branch\n",stderr); abort(); }
static int CheckAccess(const char *path,int flags) { return access(path,flags); }
static int DirectoryBusy(char *path) { (void)path; unreachable(); return 0; }
static Boolean CopyDir(Widget w,int mode,char *from,char *to,Boolean container,struct stat *st,
                       FileOperationErrorProc handler,Boolean busy,int type)
{ (void)w;(void)mode;(void)from;(void)to;(void)container;(void)st;(void)handler;(void)busy;(void)type;unreachable();return False; }
static Boolean MoveDir(Widget w,char *from,char *to,Boolean container,struct stat *st,
                       FileOperationErrorProc handler,int type)
{ (void)w;(void)from;(void)to;(void)container;(void)st;(void)handler;(void)type;unreachable();return False; }
static int RunFileCommand(char *cmd,char *a,char *b,char *c)
{ (void)cmd;(void)a;(void)b;(void)c;unreachable();return -1; }
#ifdef TRANSFER_INCLUDE_WORKERS
static void fsErase(char *path,int *result,int force)
{ (void)force;*result=unlink(path)==0?0:errno; }
#endif
static void DtLastChar(char *text,char **last,int *length)
{ *last=text+strlen(text)-1;*length=1; }
static char *_DtFollowLink(char *text) { return text; } /* Regular-file fixtures only. */
static char *DName(char *text) { char *p=strrchr(text,'/'); return p?p+1:text; }
static char *_DtPName(char *text) { (void)text; unreachable();return NULL; }
static char *_DtResolveAppManPath(char *text,char *app) { (void)app;unreachable();return text; }
static void DtEliminateDots(char *path) { (void)path; } /* Adapter inputs are canonical private paths. */
/* Deterministic name-plumbing only; the production Same branch still owns
 * dispatch, transfer, completion and allocation lifetimes. */
static void generate_NewPath(char *a,char *b)
{
    fixture(!strcmp(case_name,"worker-normal-same-copy"));
    char chosen[PATH_MAX];fixture(strlen(b)+sizeof("-copy")<=sizeof(chosen));
    strcpy(chosen,b);strcat(chosen,"-copy");strcpy(a,chosen);++same_name_generations;
}
static void split_path(char *a,char *b,char *c)
{
    fixture(!strcmp(case_name,"worker-normal-same-copy"));
    char *slash=strrchr(a,'/');fixture(slash!=NULL);
    size_t length=(size_t)(slash-a);memcpy(b,a,length);b[length]=0;
    strcpy(c,slash+1);++same_path_splits;
}
char *GetSharedMessage(int id)
{
    (void)id;
    return "Transfer failed: %s"; /* UI text boundary, not transfer logic. */
}
static Boolean _DtIsBufferExecutable(void *data,int size)
{ ++executable_calls;CHECK(data && size>0);return False; }
static char *ResolveLocalPathName(char *host,char *directory,char *file,char *local,Tt_status *status)
{
    (void)host;(void)local;
    if ((!file && resolve_target_error) ||
        (file && resolve_first_error && !strcmp(file,"first")) ||
        (file && resolve_collision_error && !strcmp(file,"first") && ++resolve_first_calls==2)) {
        *status=1;return NULL;
    }
    *status=TT_OK;
    size_t n=strlen(directory)+(file?strlen(file)+1:0)+1;
    char *path=malloc(n);fixture(path!=NULL);
    if (file) snprintf(path,n,"%s/%s",directory,file); else strcpy(path,directory);
    return path;
}
static void Error(Widget w,char *text,char *path)
{ (void)w;(void)path;++error_messages;snprintf(message,sizeof(message),"%s",text?text:""); }
static int TestOpen(const char *path,int flags,...)
{
    mode_t mode=0;
    if (flags&O_CREAT) { va_list args;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args); }
    if ((flags&O_CREAT) && target_collision) {
        target_collision=0;put(path,"other owner",11);++injected;
    }
    if ((flags&O_CREAT) && ((worker_fault && strstr(path,"/first")) ||
                              (worker_last_fault && strstr(path,"/second")))) {
        if (strstr(path,"/first")) seen_first=1; else seen_second=1;
        ++injected;errno=EACCES;return -1;
    }
    int fd=open(path,flags,mode);
    if (fd>=0) {
        if ((flags&O_ACCMODE)==O_RDONLY) source_fd=fd;
        else {
            target_fd=fd;
            if (strstr(path,"/first")) seen_first=1;
            if (strstr(path,"/second")) seen_second=1;
        }
    }
    return fd;
}
static int TestCreat(const char *path,mode_t mode)
{ return TestOpen(path,O_CREAT|O_WRONLY|O_TRUNC,mode); }
static ssize_t TestRead(int fd,void *data,size_t size)
{
    if (fd!=source_fd) return read(fd,data,size);
    ++read_calls;
    if (!strcmp(case_name,"copy-read-eintr") && read_calls==1) { ++injected;errno=EINTR;return -1; }
    if (!strcmp(case_name,"move-read-error") && read_calls>1) { ++injected;errno=EIO;return -1; }
    if (!strcmp(case_name,"move-read-error") && size>7) size=7;
    return read(fd,data,size);
}
static ssize_t TestWrite(int fd,const void *data,size_t size)
{
    if (fd!=target_fd) return write(fd,data,size);
    ++write_calls;
    if (strstr(case_name,"write-eintr") && write_calls==1) { ++injected;errno=EINTR;return -1; }
    if (strstr(case_name,"write-zero")) { ++injected;return 0; }
    if (write_error && write_calls>1) { ++injected;errno=EIO;return -1; }
    if (write_error && size>5) size=5;
    if ((strstr(case_name,"short") || strstr(case_name,"write-eintr")) && size>3) size=3;
    return write(fd,data,size);
}
static int TestClose(int fd)
{
    int src=fd==source_fd,dst=fd==target_fd;
    if (src) ++source_closes;
    if (dst) ++target_closes;
    int rc=close(fd); /* Fault reports still consume the actual fd; never retry. */
    if ((close_error==1 && dst) || (close_error==2 && src) || (close_error==3 && (src||dst))) {
        ++injected;errno=strstr(case_name,"first-error")?ENOSPC:EINTR;return -1;
    }
    return rc;
}
static int TestLink(const char *from,const char *to)
{ (void)from;(void)to;++link_calls;errno=EXDEV;return -1; }
static int TestUnlink(const char *path)
{
    if (cleanup_error && strstr(path,"target")) { ++injected;errno=EACCES;return -1; }
    return unlink(path);
}
/* Only production code is compiled through these syscall seams. Harness
 * setup, verification and pipe decoding below use real unwrapped syscalls. */
#define open TestOpen
#define creat TestCreat
#define read TestRead
#define write TestWrite
#define close TestClose
#define link TestLink
#define unlink TestUnlink
#include "transfer-production.inc"
#undef open
#undef creat
#undef read
#undef write
#undef close
#undef link
#undef unlink

static unsigned char payload[8193];
static void copy_case(void)
{
    put("./source",payload,sizeof(payload));
    int move=!strncmp(case_name,"move-",5);
    write_error=strstr(case_name,"write-error") || strstr(case_name,"first-error");
    close_error=strstr(case_name,"close-target")?1:strstr(case_name,"close-source")?2:
        strstr(case_name,"first-error")?3:0;
    target_collision=strstr(case_name,"collision")!=NULL;
    Boolean result=FileManip(NULL,move?MOVE_FILE:COPY_FILE,"./source","./target",False,
                            strstr(case_name,"null-handler")?NULL:Error,False,NOT_DESKTOP);
    int success=!strcmp(case_name,"copy-success") || !strcmp(case_name,"move-success") ||
        strstr(case_name,"short") || strstr(case_name,"write-eintr") || strstr(case_name,"read-eintr");
    CHECK(!!result==!!success);
    if (success) {
        CHECK(exact("./target",payload,sizeof(payload)));
        CHECK(move?absent("./source"):exact("./source",payload,sizeof(payload)));
    } else {
        CHECK(exact("./source",payload,sizeof(payload)));
        if (strstr(case_name,"collision")) CHECK(exact("./target","other owner",11));
        if (strstr(case_name,"first-error")) CHECK(strstr(message,strerror(EIO))!=NULL);
    }
    CHECK(source_closes==1);
    CHECK(target_closes==(strstr(case_name,"collision")?0:1));
    if (move) CHECK(link_calls==1);
    if (!success || strstr(case_name,"eintr")) CHECK(injected>0);
}
static size_t drain(int fd,unsigned char *data,size_t capacity)
{
    size_t used=0;ssize_t n;
    while ((n=read(fd,data+used,capacity-used))>0) { used+=(size_t)n;fixture(used<capacity); }
    fixture(n==0);fixture(close(fd)==0);return used;
}
static int wire_take(const unsigned char *wire,size_t bytes,size_t *at,void *value,size_t size)
{
    if (*at>bytes || size>bytes-*at) return 0;
    memcpy(value,wire+*at,size);*at+=size;return 1;
}
static int wire_string(const unsigned char *wire,size_t bytes,size_t *at)
{
    short length;
    if (!wire_take(wire,bytes,at,&length,sizeof(length)) || length<0 || (size_t)length>bytes-*at) return 0;
    *at+=(size_t)length;return 1;
}
static int wire_valid(const unsigned char *wire,size_t bytes,int require_done,int returned)
{
    size_t at=0;
    while (at<bytes) {
        short tag;int value,count;long timestamp;
        if (!wire_take(wire,bytes,&at,&tag,sizeof(tag))) return 0;
        switch (tag) {
        case PIPEMSG_DONE:
            return wire_take(wire,bytes,&at,&value,sizeof(value)) && value==returned && at==bytes;
        case PIPEMSG_FILEOP_ERROR:
            if (!wire_take(wire,bytes,&at,&value,sizeof(value)) || !wire_string(wire,bytes,&at) ||
                !wire_string(wire,bytes,&at)) return 0;
            break;
        case PIPEMSG_TARGET_TIME:
            if (!wire_take(wire,bytes,&at,&timestamp,sizeof(timestamp))) return 0;
            break;
        case PIPEMSG_FILE_MODIFIED:
            if (!wire_take(wire,bytes,&at,&value,sizeof(value)) ||
                !wire_take(wire,bytes,&at,&timestamp,sizeof(timestamp)) || !wire_string(wire,bytes,&at)) return 0;
            break;
        case PIPEMSG_CONFLICT_BATCH: {
            uint32_t words[5];
            if (!wire_take(wire,bytes,&at,words,sizeof(words))) return 0;
            count=(int)ntohl(words[3]);
            if (ntohl(words[0])!=UINT32_C(0x44434631) || count<1 || count>2) return 0;
            for (int i=0;i<count;++i) {
                uint32_t item[4];
                if (!wire_take(wire,bytes,&at,item,sizeof(item))) return 0;
                size_t source_length=ntohl(item[2]),target_length=ntohl(item[3]);
                if (source_length>bytes-at) return 0;at+=source_length;
                if (target_length>bytes-at) return 0;at+=target_length;
            }
            uint32_t trailer;
            if (!wire_take(wire,bytes,&at,&trailer,sizeof(trailer)) ||
                ntohl(trailer)!=UINT32_C(0x454e4431)) return 0;
            break;
        }
        case PIPEMSG_MOVE_TO_SAME_DIR:
            if (!wire_take(wire,bytes,&at,&value,sizeof(value))) return 0;
            break;
        default:return 0;
        }
    }
    return !require_done;
}
static void buffer_case(void)
{
    int channel[2];fixture(pipe(channel)==0);
    int size=(int)sizeof(payload);void *buffer=payload;
    int occupied=strstr(case_name,"occupied")!=NULL,symlink_case=strstr(case_name,"symlink")!=NULL;
    if (occupied) put("./target","existing",8);
    if (symlink_case) { put("./victim","keep victim",11);fixture(symlink("victim","./target")==0); }
    if (strstr(case_name,"negative")) size=-1;
    if (strstr(case_name,"null-positive")) buffer=NULL;
    if (strstr(case_name,"empty")) { buffer=NULL;size=0; }
    write_error=strstr(case_name,"write-error") || strstr(case_name,"first-error");
    close_error=strstr(case_name,"close-error")?1:strstr(case_name,"first-error")?3:0;
    cleanup_error=strstr(case_name,"cleanup-error")!=NULL;
    if (cleanup_error) write_error=1;
    Boolean result=CreateFileFromBuffer(channel[1],".","./target",buffer,size);
    fixture(close(channel[1])==0);
    unsigned char wire[16384];size_t bytes=drain(channel[0],wire,sizeof(wire));
    CHECK(wire_valid(wire,bytes,0,0));
    int success=!strcmp(case_name,"buffer-success") || strstr(case_name,"short") ||
        strstr(case_name,"write-eintr") || strstr(case_name,"empty");
    CHECK(!!result==!!success);
    if (success) CHECK(exact("./target",payload,(size_t)size));
    else {
        CHECK(bytes>=sizeof(short)+sizeof(int));
        short tag=0;if (bytes>=sizeof(tag)) memcpy(&tag,wire,sizeof(tag));
        CHECK(tag==PIPEMSG_FILEOP_ERROR);
        if (occupied) CHECK(exact("./target","existing",8));
        else if (symlink_case) {
            struct stat st;CHECK(lstat("./target",&st)==0 && S_ISLNK(st.st_mode));
            CHECK(exact("./victim","keep victim",11));
        } else if (!cleanup_error) CHECK(absent("./target"));
    }
    if (strstr(case_name,"negative") || strstr(case_name,"null-positive") || strstr(case_name,"empty"))
        CHECK(executable_calls==0);
    CHECK(target_closes==(occupied||symlink_case||size<0||(size>0&&!buffer)?0:1));
    if (strstr(case_name,"first-error") || cleanup_error) {
        short length=0;
        size_t offset=sizeof(short)+sizeof(int);
        if (bytes>=offset+sizeof(length)) {
            memcpy(&length,wire+offset,sizeof(length));offset+=sizeof(length);
            CHECK(length>0 && offset+(size_t)length<=bytes);
            if (length>0 && offset+(size_t)length<=bytes) {
                char text[4096];size_t n=(size_t)length<sizeof(text)-1?(size_t)length:sizeof(text)-1;
                memcpy(text,wire+offset,n);text[n]=0;CHECK(strstr(text,strerror(EIO))!=NULL);
            }
        } else CHECK(0);
    }
}
static void pipe_case(void)
{
    int channel[2];
    if (!strcmp(case_name,"pipe-read-invalid")) {
        char byte;
        errno=0;CHECK(PipeRead(-1,&byte,-1)==-1 && errno==EINVAL);
        errno=0;CHECK(PipeRead(-1,NULL,1)==-1 && errno==EINVAL);
        return;
    }
    fixture(pipe(channel)==0);
    if (!strcmp(case_name,"pipe-field-truncated")) {
        int value=0;fixture(write(channel[1],"xy",2)==2);fixture(close(channel[1])==0);
        CHECK(PipeReadExact(channel[0],&value,sizeof(value))==-1);
    } else if (!strncmp(case_name,"pipe-index-",11)) {
        FileOpCBData data={.file_count=2};int value=!strcmp(case_name,"pipe-index-negative")?-1:
            !strcmp(case_name,"pipe-index-equal")?2:1,decoded=99;
        fixture(write(channel[1],&value,sizeof(value))==sizeof(value));fixture(close(channel[1])==0);
        int result=FileOpReadIndex(channel[0],&data,&decoded);
        CHECK(result==(!strcmp(case_name,"pipe-index-valid")?0:-1));CHECK(decoded==value);
    } else if (!strncmp(case_name,"trash-index-",12)) {
        MoveToTrashCBData data={.file_count=2,.done_count=
            !strcmp(case_name,"trash-index-negative")?-1:
            !strcmp(case_name,"trash-index-equal")?2:1};
        int decoded=99,result=MoveToTrashResultIndex(&data,&decoded);
        CHECK(result==(!strcmp(case_name,"trash-index-valid")?0:-1));
        if (!strcmp(case_name,"trash-index-valid")) CHECK(decoded==1);
    } else if (!strncmp(case_name,"trash-results-",14)) {
        int values[2]={7,11},decoded[2]={99,99};
        if (!strcmp(case_name,"trash-results-negative")) {
            errno=0;CHECK(TrashReadResults(channel[0],decoded,-1)==-1 && errno==EINVAL);
        } else if (!strcmp(case_name,"trash-results-null")) {
            errno=0;CHECK(TrashReadResults(channel[0],NULL,1)==-1 && errno==EINVAL);
        } else if (!strcmp(case_name,"trash-results-oversize")) {
            errno=0;CHECK(TrashReadResults(channel[0],decoded,INT_MAX)==-1 && errno==EINVAL);
        } else {
            size_t bytes=!strcmp(case_name,"trash-results-truncated")?sizeof(int):sizeof(values);
            fixture(write(channel[1],values,bytes)==(ssize_t)bytes);fixture(close(channel[1])==0);
            int result=TrashReadResults(channel[0],decoded,2);
            CHECK(result==(!strcmp(case_name,"trash-results-valid")?0:-1));
            if (!strcmp(case_name,"trash-results-valid"))
                CHECK(decoded[0]==7 && decoded[1]==11);
            fixture(close(channel[0])==0);return;
        }
    } else if (!strcmp(case_name,"pipe-string-field-empty")) {
        short length=0;char *value=(char *)1;
        fixture(write(channel[1],&length,sizeof(length))==sizeof(length));
        fixture(close(channel[1])==0);
        CHECK(PipeReadStringField(channel[0],&value)==0 && value==NULL);
    } else if (!strcmp(case_name,"pipe-string-field-truncated")) {
        short length=3;char *value=(char *)1;
        fixture(write(channel[1],&length,sizeof(length))==sizeof(length));
        fixture(write(channel[1],"x",1)==1);fixture(close(channel[1])==0);
        CHECK(PipeReadStringField(channel[0],&value)==-1 && value==NULL);
    } else if (!strcmp(case_name,"pipe-string-negative")) {
        short length=-1;fixture(write(channel[1],&length,sizeof(length))==sizeof(length));
        fixture(close(channel[1])==0);errno=0;
        CHECK(PipeReadString(channel[0])==NULL && errno==EPROTO);
    } else if (!strcmp(case_name,"pipe-string-truncated")) {
        short length=5;fixture(write(channel[1],&length,sizeof(length))==sizeof(length));
        fixture(write(channel[1],"xy",2)==2);fixture(close(channel[1])==0);
        CHECK(PipeReadString(channel[0])==NULL);
    } else if (!strcmp(case_name,"pipe-string-valid")) {
        short length=3;fixture(write(channel[1],&length,sizeof(length))==sizeof(length));
        fixture(write(channel[1],"abc",3)==3);fixture(close(channel[1])==0);
        char *value=PipeReadString(channel[0]);CHECK(value && !strcmp(value,"abc"));XtFree(value);
    } else {
        size_t length=(size_t)SHRT_MAX+1;char *value=malloc(length+1);fixture(value!=NULL);
        memset(value,'x',length);value[length]=0;errno=0;
        CHECK(PipeWriteString(channel[1],value)==-1 && errno==EOVERFLOW);
        free(value);fixture(close(channel[1])==0);
        char byte;CHECK(read(channel[0],&byte,1)==0);
    }
    fixture(close(channel[0])==0);
}
#ifdef TRANSFER_INCLUDE_WORKERS
static void worker_case(void)
{
    char cwd[PATH_MAX],source[PATH_MAX],target[PATH_MAX];
    fixture(getcwd(cwd,sizeof(cwd))!=NULL);
    fixture(mkdir("sources",0700)==0 && mkdir("targets",0700)==0);
    fixture(strlen(cwd)+9<sizeof(source));
    strcpy(source,cwd);strcat(source,"/sources");strcpy(target,cwd);strcat(target,"/targets");
    int desktop=strstr(case_name,"desktop")!=NULL;
    int same_copy=!strcmp(case_name,"worker-normal-same-copy");
    int collision=same_copy || strstr(case_name,"collision")!=NULL;
    int copy=collision || strstr(case_name,"resolve")!=NULL || strstr(case_name,"regular")!=NULL;
    if (same_copy) strcpy(target,source); /* Both actual source paths collide with themselves. */
    DirUpdate updates[2];memset(updates,0,sizeof(updates));
    for (int i=0;i<2;++i) {
        updates[i].host="local";updates[i].directory=source;updates[i].file=i?"second":"first";
        updates[i].bufferInfo.buf_ptr=payload;updates[i].bufferInfo.size=(int)sizeof(payload);
    }
    if (copy) {
        put("sources/first",payload,sizeof(payload));
        put("sources/second",payload+(same_copy?1:0),sizeof(payload)-(same_copy?1:0));
    }
    if (collision && !same_copy) {
        put("targets/first","occupied first",14);
        put("targets/second","occupied second",15);
    }
    resolve_target_error=strstr(case_name,"target-resolve")!=NULL;
    resolve_first_error=copy && !collision && !resolve_target_error;
    resolve_collision_error=0;
    worker_fault=strstr(case_name,"first-fails")!=NULL;
    worker_last_fault=strstr(case_name,"last-fails")!=NULL;
    int output[2],reply[2];fixture(pipe(output)==0 && pipe(reply)==0);
    if (collision) {
        DtfileConflictBatch batch={0};
        DtfileConflictDecision decisions[2]={{0}};
        fixture(DtfileConflictBatchInit(&batch,DTFILE_CONFLICT_COPY,2,2)==0);
        for (int i=0;i<2;++i) {
            char source_path[PATH_MAX],target_path[PATH_MAX];
            const char *leaf=i?"second":"first";
            fixture(strlen(source)+strlen(leaf)+2<=sizeof(source_path));
            fixture(strlen(target)+strlen(leaf)+2<=sizeof(target_path));
            strcpy(source_path,source);strcat(source_path,"/");strcat(source_path,leaf);
            strcpy(target_path,target);strcat(target_path,"/");strcat(target_path,leaf);
            fixture(DtfileConflictSetItem(&batch,(size_t)i,(uint32_t)i,
                    same_copy?DTFILE_CONFLICT_SAME_ENTRY:0,
                    source_path,strlen(source_path),target_path,strlen(target_path))==0);
            decisions[i].request_index=(uint32_t)i;
            decisions[i].action=DTFILE_CONFLICT_KEEP_BOTH;
        }
        if (strstr(case_name,"cancel"))
            fixture(DtfileConflictWriteCancel(reply[1])==0);
        else
            fixture(DtfileConflictWriteDecisions(reply[1],&batch,decisions)==0);
        DtfileConflictBatchFree(&batch);
    }
    fixture(close(reply[1])==0);
    errno=0;
    int rc=desktop?FileMoveCopyProcessDesktop(output[1],reply[0],target,"local",updates,2,copy?COPY_FILE:MAKE_BUFFER,NULL):
        FileMoveCopyProcess(output[1],reply[0],NULL,target,"local",updates,2,copy?COPY_FILE:MAKE_BUFFER,NULL);
    int operation_errno=errno;
    fixture(close(reply[0])==0 && close(output[1])==0);
    unsigned char wire[16384];size_t bytes=drain(output[0],wire,sizeof(wire));
    short tag=0;int reported=999;
    if (bytes>=sizeof(tag)+sizeof(reported)) {
        memcpy(&tag,wire+bytes-sizeof(tag)-sizeof(reported),sizeof(tag));
        memcpy(&reported,wire+bytes-sizeof(reported),sizeof(reported));
    }
    CHECK(wire_valid(wire,bytes,1,rc));
    int success=(collision && !strstr(case_name,"cancel")) || strstr(case_name,"success")!=NULL;
    CHECK(tag==PIPEMSG_DONE && reported==rc && rc==(success?0:-1));
    if (!collision && !resolve_target_error) {
        if (!worker_last_fault) CHECK(exact("targets/second",payload,sizeof(payload)));
        else CHECK(exact("targets/first",payload,sizeof(payload)) && absent("targets/second"));
        if (!copy) CHECK(seen_second);
    }
    if (collision && strstr(case_name,"cancel")) {
        CHECK(operation_errno==ECANCELED);
        CHECK(exact("targets/first","occupied first",14));
        CHECK(exact("targets/second","occupied second",15));
    }
    if (same_copy) {
        CHECK(exact("sources/first",payload,sizeof(payload)));
        CHECK(exact("sources/second",payload+1,sizeof(payload)-1));
        CHECK(exact("sources/first_1",payload,sizeof(payload)));
        CHECK(exact("sources/second_1",payload+1,sizeof(payload)-1));
    } else if (collision && !strstr(case_name,"cancel")) {
        CHECK(exact("targets/first","occupied first",14));
        CHECK(exact("targets/second","occupied second",15));
        CHECK(exact("targets/first_1",payload,sizeof(payload)));
        CHECK(exact("targets/second_1",payload,sizeof(payload)));
    } else if (copy) {
        CHECK(exact("sources/first",payload,sizeof(payload)));
        CHECK(exact("sources/second",payload,sizeof(payload)));
    }
}
#endif
int main(int argc,char **argv)
{
    fixture(argc==3);case_name=argv[1];fixture(chdir(argv[2])==0);
    for (size_t i=0;i<sizeof(payload);++i) payload[i]=(unsigned char)(i%251);
#ifdef TRANSFER_INCLUDE_WORKERS
    if (!strncmp(case_name,"worker-",7)) worker_case();
    else
#endif
    if (!strncmp(case_name,"pipe-",5) || !strncmp(case_name,"trash-",6)) pipe_case();
    else if (!strncmp(case_name,"buffer-",7)) buffer_case();
    else copy_case();
    if (!failed) printf("PASS %s\n",case_name);
    return failed?1:0;
}
