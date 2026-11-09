/* Focused checks for the production conflict model, pipe codec and extracted
 * worker path/observation helpers. Whole-worker and live Motif UI behavior are
 * qualified by the component checkpoint, not simulated here.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(COLLISION_MODEL_ONLY)
#include "FileConflict.h"
#define VERIFY(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d %s\n",name,__LINE__,#x); return 1; } } while (0)

#include "FileConflictWire.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>

static int codec_batch(DtfileConflictBatch *b)
{
    const char s[]="/source/one\n\t\\\303\251", t[]="/target/one";
    if(DtfileConflictBatchInit(b,DTFILE_CONFLICT_COPY,4,2)!=0)return -1;
    if(DtfileConflictSetItem(b,0,3,0,s,sizeof(s)-1,t,sizeof(t)-1)!=0)return -1;
    return DtfileConflictSetItem(b,1,1,0,"/source/two",11,"/target/two",11);
}
static size_t codec_capture(int fd,unsigned char *bytes,size_t limit)
{
    size_t n=0;ssize_t k;
    while(n<limit && (k=read(fd,bytes+n,limit-n))>0)n+=(size_t)k;
    return n;
}
static void codec_word(unsigned char *bytes,size_t word,uint32_t value)
{ value=htonl(value);memcpy(bytes+word*4,&value,4); }
static int codec_case(const char *name)
{
    DtfileConflictBatch b={0},out={0};
    DtfileConflictDecision choices[2]={{3,DTFILE_CONFLICT_SKIP,NULL},
                                      {1,DTFILE_CONFLICT_RENAME_INCOMING,"new\n\\\303\251"}};
    DtfileConflictDecision *answer=NULL;
    unsigned char bytes[4096];int p[2];size_t n;
    VERIFY(codec_batch(&b)==0);
    if(!strcmp(name,"codec-request-budget") || !strcmp(name,"codec-tagged-request") ||
       !strcmp(name,"codec-partial-header")) {
        VERIFY(pipe(p)==0);
        if(!strcmp(name,"codec-tagged-request")) {
            const unsigned short expected=14;unsigned short tag=0;
            VERIFY(DtfileConflictWriteTaggedRequest(p[1],expected,&b)==0);
            VERIFY(read(p[0],&tag,sizeof(tag))==(ssize_t)sizeof(tag) && tag==expected);
        } else VERIFY(DtfileConflictWriteRequest(p[1],&b)==0);
        close(p[1]);
        n=codec_capture(p[0],bytes,sizeof(bytes));close(p[0]);VERIFY(n>40);
        VERIFY(pipe(p)==0);
        DtfileConflictReader *r=DtfileConflictReaderNew();VERIFY(r!=NULL);
        if(!strcmp(name,"codec-partial-header")) {
            VERIFY(write(p[1],bytes,3)==3);
            VERIFY(DtfileConflictReadAvailable(r,p[0],65536)==0); /* writer ALIVE */
            VERIFY(DtfileConflictReaderTake(r,&out)<0 && !out.items);
            VERIFY(write(p[1],bytes+3,n-3)==(ssize_t)(n-3));
        } else VERIFY(write(p[1],bytes,n)==(ssize_t)n);
        int result=0;size_t calls=0;
        while(result==0 && calls++<n+1) {
            int before=0,after=0;
            VERIFY(ioctl(p[0],FIONREAD,&before)==0);
            result=DtfileConflictReadAvailable(r,p[0],1);
            VERIFY(ioctl(p[0],FIONREAD,&after)==0);
            VERIFY(before-after>=0 && before-after<=1); /* actual budget, not timing */
        }
        VERIFY(result==1);
        DtfileConflictBatch occupied={0};
        VERIFY(DtfileConflictBatchInit(&occupied,DTFILE_CONFLICT_COPY,1,1)==0);
        VERIFY(DtfileConflictReaderTake(r,&occupied)<0 && occupied.items!=NULL);
        DtfileConflictBatchFree(&occupied);
        VERIFY(DtfileConflictReaderTake(r,&out)==0);
        VERIFY(out.count==2 && out.items[0].request_index==3 && out.items[1].request_index==1);
        VERIFY(!strcmp(out.items[0].source,b.items[0].source));
        VERIFY(!strcmp(out.items[1].target,b.items[1].target));
        VERIFY(DtfileConflictReaderTake(r,&b)<0); /* does not overwrite b */
        close(p[0]);close(p[1]);DtfileConflictReaderFree(r);DtfileConflictBatchFree(&out);
    } else if(!strncmp(name,"codec-request-bad-",18)) {
        VERIFY(pipe(p)==0);VERIFY(DtfileConflictWriteRequest(p[1],&b)==0);close(p[1]);
        n=codec_capture(p[0],bytes,sizeof(bytes));close(p[0]);VERIFY(n>40);
        const char *fault=name+18;
        if(!strcmp(fault,"magic"))codec_word(bytes,0,0);
        else if(!strcmp(fault,"count"))codec_word(bytes,3,DTFILE_CONFLICT_ITEM_LIMIT+1u);
        else if(!strcmp(fault,"duplicate")) {
            size_t second=36+strlen(b.items[0].source)+strlen(b.items[0].target);
            VERIFY(second+4<=n);/* byte spans need not be word aligned */
            uint32_t v=htonl(3);memcpy(bytes+second,&v,4);
        } else if(!strcmp(fault,"nul"))bytes[36]=0;
        else if(!strcmp(fault,"footer"))memset(bytes+n-4,0,4);
        else if(!strcmp(fault,"truncated"))--n;
        else { VERIFY(0); }
        VERIFY(pipe(p)==0);VERIFY(write(p[1],bytes,n)==(ssize_t)n);close(p[1]);
        DtfileConflictReader *r=DtfileConflictReaderNew();VERIFY(r!=NULL);
        int result=0;size_t calls=0;
        while(result==0 && calls++<n+2)result=DtfileConflictReadAvailable(r,p[0],7);
        VERIFY(result<0);int saved=errno;
        VERIFY(DtfileConflictReaderTake(r,&out)<0 && !out.items);
        VERIFY(DtfileConflictReadAvailable(r,p[0],7)<0 && errno==saved);
        DtfileConflictReaderFree(r);close(p[0]);
    } else if(!strcmp(name,"codec-decisions-roundtrip") || !strcmp(name,"codec-cancel")) {
        VERIFY(pipe(p)==0);
        int cancel=!strcmp(name,"codec-cancel");
        VERIFY((cancel?DtfileConflictWriteCancel(p[1]):DtfileConflictWriteDecisions(p[1],&b,choices))==0);
        close(p[1]);
        VERIFY(DtfileConflictReadDecisions(p[0],getppid(),&b,&answer)==(cancel?0:1));close(p[0]);
        if(cancel)VERIFY(answer==NULL);
        else {
            VERIFY(answer && answer[0].request_index==3 && answer[1].request_index==1);
            VERIFY(answer[0].action==DTFILE_CONFLICT_SKIP && !answer[0].name);
            VERIFY(answer[1].action==DTFILE_CONFLICT_RENAME_INCOMING && !strcmp(answer[1].name,choices[1].name));
            DtfileConflictDecisionsFree(answer,b.count);
        }
    } else if(!strcmp(name,"codec-invalid-decision-no-output")) {
        VERIFY(pipe(p)==0);choices[1].request_index=3; /* later decision invalid */
        VERIFY(DtfileConflictWriteDecisions(p[1],&b,choices)<0);
        VERIFY(fcntl(p[0],F_SETFL,O_NONBLOCK)==0);
        VERIFY(read(p[0],bytes,sizeof(bytes))<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
        close(p[0]);close(p[1]);
    } else if(!strncmp(name,"codec-reply-bad-",16)) {
        VERIFY(pipe(p)==0);VERIFY(DtfileConflictWriteDecisions(p[1],&b,choices)==0);close(p[1]);
        n=codec_capture(p[0],bytes,sizeof(bytes));close(p[0]);VERIFY(n>40);
        const char *fault=name+16;
        if(!strcmp(fault,"index"))codec_word(bytes,6,3); /* second decision index */
        else if(!strcmp(fault,"nul"))bytes[36]=0; /* second manual name */
        else if(!strcmp(fault,"footer"))memset(bytes+n-4,0,4);
        else if(!strcmp(fault,"truncated"))--n;
        else VERIFY(0);
        VERIFY(pipe(p)==0);VERIFY(write(p[1],bytes,n)==(ssize_t)n);close(p[1]);
        answer=(DtfileConflictDecision *)(uintptr_t)1;
        VERIFY(DtfileConflictReadDecisions(p[0],getppid(),&b,&answer)<0);
        VERIFY(answer==NULL);close(p[0]); /* complete invalid packet publishes NOTHING */
    } else if(!strcmp(name,"codec-owner-mismatch")) {
        VERIFY(pipe(p)==0); /* open writer, no first byte: owner check must abort */
        answer=(DtfileConflictDecision *)(uintptr_t)1;
        VERIFY(DtfileConflictReadDecisions(p[0],getppid()+1,&b,&answer)<0 && errno==ECANCELED);
        VERIFY(answer==NULL);close(p[0]);close(p[1]);
    } else if(!strcmp(name,"codec-closed-peer") || !strcmp(name,"codec-pending-sigpipe")) {
        struct sigaction original,action={0},after;
        action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
        VERIFY(sigaction(SIGPIPE,&action,&original)==0);
        sigset_t mask,oldmask,current,pending;sigemptyset(&mask);sigaddset(&mask,SIGPIPE);
        VERIFY(pthread_sigmask(SIG_SETMASK,NULL,&oldmask)==0);
        int was_pending=!strcmp(name,"codec-pending-sigpipe");
        if(was_pending) {
            VERIFY(pthread_sigmask(SIG_BLOCK,&mask,NULL)==0);
            VERIFY(pthread_kill(pthread_self(),SIGPIPE)==0);
            VERIFY(sigpending(&pending)==0 && sigismember(&pending,SIGPIPE)==1);
        } else {
            VERIFY(pthread_sigmask(SIG_UNBLOCK,&mask,NULL)==0);
        }
        for(int which=0;which<4;++which) {
            VERIFY(pipe(p)==0);close(p[0]);errno=0;
            int rc=which==0?DtfileConflictWriteCancel(p[1]):which==1?
                   DtfileConflictWriteRequest(p[1],&b):which==2?
                   DtfileConflictWriteTaggedRequest(p[1],14,&b):
                   DtfileConflictWriteDecisions(p[1],&b,choices);
            VERIFY(rc<0 && errno==EPIPE);close(p[1]);
            VERIFY(sigaction(SIGPIPE,NULL,&after)==0 && after.sa_handler==SIG_DFL);
            VERIFY(pthread_sigmask(SIG_SETMASK,NULL,&current)==0);
            VERIFY(sigismember(&current,SIGPIPE)==was_pending);
            VERIFY(sigpending(&pending)==0 && sigismember(&pending,SIGPIPE)==was_pending);
        }
        if(was_pending) { int sig=0;VERIFY(sigwait(&mask,&sig)==0 && sig==SIGPIPE); }
        VERIFY(pthread_sigmask(SIG_SETMASK,&oldmask,NULL)==0);
        VERIFY(sigaction(SIGPIPE,&original,NULL)==0);
    } else { fprintf(stderr,"Unknown codec case %s\n",name);return 2; }
    DtfileConflictBatchFree(&b);printf("PASS %s\n",name);return 0;
}

static int model_case(const char *name)
{
    DtfileConflictBatch b={0};
    DtfileConflictDecision d={0};
    if(!strcmp(name,"model-native-bytes")) {
        char source[]="/source/line\n\t\\\303\251";
        char target[]="/target/literal\\x0A";
        VERIFY(DtfileConflictBatchInit(&b,DTFILE_CONFLICT_COPY,3,1)==0);
        VERIFY(DtfileConflictSetItem(&b,0,2,0,source,sizeof(source)-1,target,sizeof(target)-1)==0);
        source[1]='X';target[1]='Y'; /* API owns independent native copies. */
        VERIFY(!strcmp(b.items[0].source,"/source/line\n\t\\\303\251"));
        VERIFY(!strcmp(b.items[0].target,"/target/literal\\x0A"));
        VERIFY(b.items[0].request_index==2 && DtfileConflictBatchValidate(&b)==0);
        char *display=DtfileConflictDisplayPath(b.items[0].source);
        VERIFY(display && !strcmp(display,"/source/line\\x0A\\x09\\x5C\\xC3\\xA9"));
        VERIFY(strcmp(display,b.items[0].source)!=0);free(display);
    } else if(!strcmp(name,"model-request-indices")) {
        VERIFY(DtfileConflictBatchInit(&b,DTFILE_CONFLICT_COPY,4,2)==0);
        VERIFY(DtfileConflictSetItem(&b,0,3,0,"/s/a",4,"/t/a",4)==0);
        VERIFY(DtfileConflictSetItem(&b,1,1,0,"/s/b",4,"/t/b",4)==0);
        VERIFY(DtfileConflictBatchValidate(&b)==0);
        d.request_index=1;d.action=DTFILE_CONFLICT_SKIP;
        VERIFY(DtfileConflictDecisionValidate(&b,0,&d)<0); /* same count is not authority */
        d.request_index=3;VERIFY(DtfileConflictDecisionValidate(&b,0,&d)==0);
        DtfileConflictBatchFree(&b);
        VERIFY(DtfileConflictBatchInit(&b,DTFILE_CONFLICT_COPY,4,2)==0);
        VERIFY(DtfileConflictSetItem(&b,0,3,0,"/s/a",4,"/t/a",4)==0);
        VERIFY(DtfileConflictSetItem(&b,1,3,0,"/s/b",4,"/t/b",4)==0);
        VERIFY(DtfileConflictBatchValidate(&b)<0); /* duplicate indices cannot execute */
    } else if(!strcmp(name,"model-atomic-span-rejection")) {
        const char nulpath[]={'/','x',0,'y'};
        VERIFY(DtfileConflictBatchInit(&b,DTFILE_CONFLICT_COPY,1,1)==0);
        VERIFY(DtfileConflictSetItem(&b,0,0,0,nulpath,sizeof(nulpath),"/t",2)<0);
        VERIFY(!b.items[0].source && !b.items[0].target && b.native_bytes==0);
        VERIFY(DtfileConflictSetItem(&b,0,0,0,"relative",8,"/t",2)<0);
        VERIFY(!b.items[0].source && !b.items[0].target && b.native_bytes==0);
        VERIFY(DtfileConflictSetItem(&b,0,0,0,"/s",2,"/t",2)==0);
        VERIFY(DtfileConflictSetItem(&b,0,0,0,"/other",6,"/next",5)<0);
        VERIFY(!strcmp(b.items[0].source,"/s") && !strcmp(b.items[0].target,"/t"));
        VERIFY(DtfileConflictBatchValidate(&b)==0);
    } else if(!strcmp(name,"model-move-self")) {
        VERIFY(DtfileConflictBatchInit(&b,DTFILE_CONFLICT_MOVE,2,2)==0);
        VERIFY(DtfileConflictSetItem(&b,0,0,DTFILE_CONFLICT_SAME_ENTRY,"/t/a",4,"/t/a",4)==0);
        VERIFY(DtfileConflictSetItem(&b,1,1,0,"/s/b",4,"/t/b",4)==0);
        VERIFY(DtfileConflictDefaultAction(&b,0)==DTFILE_CONFLICT_SKIP);
        VERIFY(DtfileConflictDefaultAction(&b,1)==DTFILE_CONFLICT_REPLACE);
        VERIFY(!DtfileConflictActionAllowed(&b,0,DTFILE_CONFLICT_REPLACE));
        VERIFY(!DtfileConflictActionAllowed(&b,0,DTFILE_CONFLICT_KEEP_BOTH));
        VERIFY(DtfileConflictActionAllowed(&b,0,DTFILE_CONFLICT_RENAME_INCOMING));
        VERIFY(!DtfileConflictChangesExisting(&b,0,DTFILE_CONFLICT_RENAME_INCOMING));
        VERIFY(DtfileConflictChangesExisting(&b,1,DTFILE_CONFLICT_REPLACE));
        VERIFY(!DtfileConflictChangesExisting(&b,1,DTFILE_CONFLICT_KEEP_BOTH));
        VERIFY(!DtfileConflictChangesExisting(&b,1,DTFILE_CONFLICT_SKIP));
        d.request_index=0;d.action=DTFILE_CONFLICT_REPLACE;
        VERIFY(DtfileConflictDecisionValidate(&b,0,&d)<0);
        d.action=DTFILE_CONFLICT_KEEP_BOTH;VERIFY(DtfileConflictDecisionValidate(&b,0,&d)<0);
        d.action=DTFILE_CONFLICT_RENAME_EXISTING;d.name="different";
        VERIFY(DtfileConflictDecisionValidate(&b,0,&d)<0);
        d.action=DTFILE_CONFLICT_RENAME_INCOMING;VERIFY(DtfileConflictDecisionValidate(&b,0,&d)==0);
    } else if(!strcmp(name,"model-copy-link-self")) {
        DtfileConflictOperation operations[]={DTFILE_CONFLICT_COPY,DTFILE_CONFLICT_LINK};
        for(size_t i=0;i<2;++i) {
            VERIFY(DtfileConflictBatchInit(&b,operations[i],1,1)==0);
            VERIFY(DtfileConflictSetItem(&b,0,0,DTFILE_CONFLICT_SAME_ENTRY,"/t/a",4,"/t/a",4)==0);
            VERIFY(DtfileConflictDefaultAction(&b,0)==DTFILE_CONFLICT_KEEP_BOTH);
            VERIFY(DtfileConflictActionAllowed(&b,0,DTFILE_CONFLICT_KEEP_BOTH));
            VERIFY(!DtfileConflictChangesExisting(&b,0,DTFILE_CONFLICT_KEEP_BOTH));
            d.request_index=0;d.action=DTFILE_CONFLICT_KEEP_BOTH;d.name=NULL;
            VERIFY(DtfileConflictDecisionValidate(&b,0,&d)==0);
            DtfileConflictBatchFree(&b);
        }
    } else if(!strcmp(name,"model-buffer-source")) {
        VERIFY(DtfileConflictBatchInit(&b,DTFILE_CONFLICT_BUFFER,1,1)==0);
        VERIFY(DtfileConflictSetItem(&b,0,0,0,"/s",2,"/t",2)<0);
        VERIFY(!b.items[0].target);
        VERIFY(DtfileConflictSetItem(&b,0,0,DTFILE_CONFLICT_SAME_ENTRY,NULL,0,"/t",2)<0);
        VERIFY(DtfileConflictSetItem(&b,0,0,0,NULL,0,"/t",2)==0);
        VERIFY(DtfileConflictBatchValidate(&b)==0 && !b.items[0].source);
    } else if(!strcmp(name,"model-manual-name-and-merge")) {
        VERIFY(DtfileConflictBatchInit(&b,DTFILE_CONFLICT_COPY,2,2)==0);
        VERIFY(DtfileConflictSetItem(&b,0,0,DTFILE_CONFLICT_CAN_MERGE,"/s/a",4,"/t/a",4)==0);
        VERIFY(DtfileConflictSetItem(&b,1,1,0,"/s/b",4,"/t/b",4)==0);
        d.request_index=0;d.action=DTFILE_CONFLICT_MERGE;
        VERIFY(DtfileConflictDecisionValidate(&b,0,&d)==0);
        d.request_index=1;VERIFY(DtfileConflictDecisionValidate(&b,1,&d)<0);
        d.action=DTFILE_CONFLICT_RENAME_INCOMING;
        VERIFY(DtfileConflictDecisionValidate(&b,1,&d)<0);
        d.name="";VERIFY(DtfileConflictDecisionValidate(&b,1,&d)<0);
        d.name="native\n\\\303\251";VERIFY(DtfileConflictDecisionValidate(&b,1,&d)==0);
        d.name="sub/name";VERIFY(DtfileConflictDecisionValidate(&b,1,&d)<0);
        d.name=".";VERIFY(DtfileConflictDecisionValidate(&b,1,&d)<0);
        d.name="..";VERIFY(DtfileConflictDecisionValidate(&b,1,&d)<0);
        d.action=DTFILE_CONFLICT_SKIP;VERIFY(DtfileConflictDecisionValidate(&b,1,&d)<0);
        d.name=NULL;VERIFY(DtfileConflictDecisionValidate(&b,1,&d)==0);
    } else if(!strcmp(name,"model-path-bound")) {
        char *path=malloc(DTFILE_CONFLICT_PATH_LIMIT+2u);VERIFY(path!=NULL);
        memset(path,'a',DTFILE_CONFLICT_PATH_LIMIT+1u);path[0]='/';path[DTFILE_CONFLICT_PATH_LIMIT+1u]=0;
        VERIFY(DtfileConflictBatchInit(&b,DTFILE_CONFLICT_COPY,1,1)==0);
        VERIFY(DtfileConflictSetItem(&b,0,0,0,path,DTFILE_CONFLICT_PATH_LIMIT+1u,"/t",2)<0);
        VERIFY(!b.items[0].source && !b.items[0].target);
        VERIFY(DtfileConflictSetItem(&b,0,0,0,path,DTFILE_CONFLICT_PATH_LIMIT,"/t",2)==0);
        VERIFY(strlen(b.items[0].source)==DTFILE_CONFLICT_PATH_LIMIT);
        VERIFY(DtfileConflictBatchValidate(&b)==0);free(path);
    } else { fprintf(stderr,"Unknown model case %s\n",name);return 2; }
    DtfileConflictBatchFree(&b);
    VERIFY(!b.items && !b.count && !b.native_bytes);
    DtfileConflictBatchFree(&b);DtfileConflictBatchFree(NULL);
    printf("PASS %s\n",name);return 0;
}
int main(int argc,char **argv)
{ return argc==2 ? (!strncmp(argv[1],"codec-",6) ? codec_case(argv[1]) : model_case(argv[1])) : 2; }
#elif defined(COLLISION_WORKER_HELPERS)

#define XtMalloc malloc
#define XtFree free
#define VERIFY(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d %s\n",name,__LINE__,#x); return 1; } } while (0)
#include "collision-helper-production.inc"

static int helper_write(const char *path,const char *text,int append)
{
    int fd=open(path,O_WRONLY|O_CREAT|(append?O_APPEND:O_TRUNC),0600);
    size_t length=strlen(text);ssize_t written;
    if(fd<0)return -1;
    written=write(fd,text,length);
    return close(fd)==0&&written==(ssize_t)length?0:-1;
}

static int helper_case(const char *name)
{
    if(!strcmp(name,"helper-paths")) {
        char *path=FileOpPathJoin("/","item");
        VERIFY(path && !strcmp(path,"/item"));XtFree(path);
        errno=0;VERIFY(FileOpPathJoin("/target","sub/item")==NULL && errno==EINVAL);
        path=FileOpDecisionPath("/target/item","renamed");
        VERIFY(path && !strcmp(path,"/target/renamed"));XtFree(path);
        VERIFY(symlink("missing","item_1")==0);
        path=FileOpUniquePath("item");
        VERIFY(path && !strcmp(path,"item_2"));XtFree(path);
        VERIFY(unlink("item_1")==0);
    } else if(!strcmp(name,"helper-observations")) {
        struct stat observed;
        VERIFY(helper_write("observed","one",0)==0);
        VERIFY(lstat("observed",&observed)==0);
        VERIFY(FileOpObservationMatches("observed",&observed));
        VERIFY(helper_write("observed","-changed",1)==0);
        VERIFY(!FileOpObservationMatches("observed",&observed));
        VERIFY(lstat("observed",&observed)==0 && rename("observed","old-object")==0);
        VERIFY(helper_write("observed","replacement",0)==0);
        VERIFY(!FileOpObservationMatches("observed",&observed));
        VERIFY(unlink("observed")==0 && unlink("old-object")==0);
    } else return 2;
    printf("PASS %s\n",name);return 0;
}

int main(int argc,char **argv)
{ return argc==2 ? helper_case(argv[1]) : 2; }
#endif /* COLLISION_MODEL_ONLY */
