/* Actual production FileManip/helper/path extraction fixture.
 * UI and unreachable directory-transfer adapters are explicit. Fault seams
 * report syscall/command-boundary failures; no native device failure claim.
 * Run helper and path modes separately, sequentially on the reserved CPU.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(FILEMANIP_HELPER_ONLY)
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>

/* The observation seam calls real waitpid. It records actual EINTR and never
 * supplies a pid, status or operation result. Only the EINTR case arms a real
 * timer while the production wait owns a child blocked on a private pipe. */
static volatile sig_atomic_t helper_alarm_seen, helper_release_fd=-1;
static int helper_interrupt_mode, helper_timer_armed, helper_real_eintr, helper_wait_calls, helper_invalid_wait_calls;
static void helper_alarm(int sig)
{
    int saved=errno;(void)sig;++helper_alarm_seen;
    if(helper_release_fd>=0) { char byte=1;ssize_t ignored=write((int)helper_release_fd,&byte,1);(void)ignored; }
    errno=saved;
}
static pid_t helper_observe_waitpid(pid_t pid,int *status,int flags)
{
    ++helper_wait_calls;
    if(pid<=0) { ++helper_invalid_wait_calls;errno=EINVAL;return -1; }
    if(helper_interrupt_mode && !helper_timer_armed) {
        struct itimerval timer={{0,0},{0,50000}};
        if(setitimer(ITIMER_REAL,&timer,NULL)!=0)return -1;
        helper_timer_armed=1;
    }
    pid_t result=waitpid(pid,status,flags);
    if(result<0 && errno==EINTR)++helper_real_eintr;
    return result;
}
#define waitpid helper_observe_waitpid
#include "filemanip-helper-production.inc"
#undef waitpid

#define HVERIFY(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d %s errno=%d\n",name,__LINE__,#x,errno); return 1; } } while(0)
static int helper_mask_equal(const sigset_t *a,const sigset_t *b)
{
    /* Compare membership rather than padding/opaque sigset_t representation. */
    for(int sig=1;sig<NSIG;++sig)if(sigismember(a,sig)!=sigismember(b,sig))return 0;
    return 1;
}
static int helper_case(const char *name)
{
    if(!strcmp(name,"helper-invalid-pid-zero") || !strcmp(name,"helper-invalid-pid-negative")) {
        FileHelperSignals signals;sigset_t before,after;
        HVERIFY(sigprocmask(SIG_SETMASK,NULL,&before)==0);
        HVERIFY(FileHelperBegin(&signals)==0);
        HVERIFY(FileHelperWait(strstr(name,"zero")?0:-1,&signals)<0&&errno==EINVAL);
        HVERIFY(helper_wait_calls==0 && helper_invalid_wait_calls==0);
        HVERIFY(sigprocmask(SIG_SETMASK,NULL,&after)==0&&helper_mask_equal(&before,&after));
        printf("PASS %s waitpid_not_called\n",name);return 0;
    }
    int interrupt=!strcmp(name,"helper-real-eintr");
    int ignored=!strcmp(name,"helper-sigchld-ignored");
    int no_wait=!strcmp(name,"helper-sigchld-nocldwait");
    int mask_test=!strcmp(name,"helper-mask-restore");
    int term=!strcmp(name,"helper-sigterm");
    int unrelated=!strcmp(name,"helper-unrelated-child");
    int exit_one=!strcmp(name,"helper-exit-one");
    int missing=!strcmp(name,"helper-missing-exec");
    HVERIFY(interrupt||ignored||no_wait||mask_test||term||unrelated||exit_one||missing||!strcmp(name,"helper-exit-zero"));
    struct sigaction initial,chosen={0},expected,after,old_alarm,old_term;
    sigset_t initial_mask,expected_mask,after_mask,block;
    HVERIFY(sigaction(SIGCHLD,NULL,&initial)==0);
    HVERIFY(sigprocmask(SIG_SETMASK,NULL,&initial_mask)==0);
    chosen.sa_handler=ignored?SIG_IGN:SIG_DFL;sigemptyset(&chosen.sa_mask);
    if(no_wait)chosen.sa_flags=SA_NOCLDWAIT;
    HVERIFY(sigaction(SIGCHLD,&chosen,NULL)==0);
    sigemptyset(&block);
    if(mask_test) {sigaddset(&block,SIGCHLD);sigaddset(&block,SIGUSR1);HVERIFY(sigprocmask(SIG_BLOCK,&block,NULL)==0);}
    if(interrupt) {
        struct sigaction action={0};action.sa_handler=helper_alarm;sigemptyset(&action.sa_mask);
        HVERIFY(sigaction(SIGALRM,&action,&old_alarm)==0);
        sigemptyset(&block);sigaddset(&block,SIGALRM);HVERIFY(sigprocmask(SIG_UNBLOCK,&block,NULL)==0);
        helper_interrupt_mode=1;
    }
    if(term) {
        struct sigaction action={0};action.sa_handler=SIG_IGN;sigemptyset(&action.sa_mask);
        HVERIFY(sigaction(SIGTERM,&action,&old_term)==0);
        sigemptyset(&block);sigaddset(&block,SIGTERM);HVERIFY(sigprocmask(SIG_BLOCK,&block,NULL)==0);
    }
    HVERIFY(sigaction(SIGCHLD,NULL,&expected)==0);
    HVERIFY(sigprocmask(SIG_SETMASK,NULL,&expected_mask)==0);
    FileHelperSignals signals;
    HVERIFY(FileHelperBegin(&signals)==0);
    int ready[2],release[2];HVERIFY(pipe(ready)==0 && pipe(release)==0);
    pid_t other=-1;
    if(unrelated) {
        other=fork();HVERIFY(other>=0);
        if(other==0)_exit(23);
        siginfo_t info={0};HVERIFY(waitid(P_PID,(id_t)other,&info,WEXITED|WNOWAIT)==0);
        HVERIFY(info.si_pid==other); /* known unrelated zombie before helper */
    }
    pid_t child=fork();HVERIFY(child>=0);
    if(child==0) {
        close(ready[0]);close(release[1]);FileHelperChildSignals();
        struct sigaction action;sigset_t mask;
        if(sigaction(SIGTERM,NULL,&action)!=0 || action.sa_handler!=SIG_DFL ||
           sigaction(SIGCHLD,NULL,&action)!=0 || action.sa_handler!=SIG_DFL ||
           sigprocmask(SIG_SETMASK,NULL,&mask)!=0 || sigismember(&mask,SIGTERM) ||
           sigismember(&mask,SIGCHLD))_exit(91);
        char byte=1;if(write(ready[1],&byte,1)!=1)_exit(92);close(ready[1]);
        if(interrupt||term) {if(read(release[0],&byte,1)!=1)_exit(93);}
        close(release[0]);
        if(missing) {execl("./no-such-filemanip-helper","no-such-filemanip-helper",(char *)NULL);_exit(127);}
        _exit(exit_one?1:0);
    }
    close(ready[1]);close(release[0]);char byte;
    HVERIFY(read(ready[0],&byte,1)==1);close(ready[0]);
    if(interrupt)helper_release_fd=release[1];
    else if(term)HVERIFY(kill(child,SIGTERM)==0);
    else close(release[1]);
    errno=0;int result=FileHelperWait(child,&signals);int wait_error=errno;
    if(interrupt) {
        struct itimerval zero={{0,0},{0,0}};HVERIFY(setitimer(ITIMER_REAL,&zero,NULL)==0);
        close(release[1]);helper_release_fd=-1;
        HVERIFY(helper_alarm_seen>0 && helper_real_eintr>0);
    }
    if(term)close(release[1]);
    if(exit_one)HVERIFY(result<0 && wait_error==ECANCELED);
    else if(missing)HVERIFY(result<0 && wait_error==ENOEXEC);
    else if(term)HVERIFY(result<0 && wait_error==EIO);
    else HVERIFY(result==0);
    HVERIFY(sigaction(SIGCHLD,NULL,&after)==0);
    HVERIFY(after.sa_handler==expected.sa_handler && after.sa_flags==expected.sa_flags);
    HVERIFY(helper_mask_equal(&after.sa_mask,&expected.sa_mask));
    HVERIFY(sigprocmask(SIG_SETMASK,NULL,&after_mask)==0);
    HVERIFY(helper_mask_equal(&after_mask,&expected_mask));
    int status=0;HVERIFY(waitpid(child,&status,WNOHANG)<0 && errno==ECHILD);
    if(unrelated) {
        HVERIFY(waitpid(other,&status,0)==other);
        HVERIFY(WIFEXITED(status) && WEXITSTATUS(status)==23);
    }
    if(interrupt)HVERIFY(sigaction(SIGALRM,&old_alarm,NULL)==0);
    if(term)HVERIFY(sigaction(SIGTERM,&old_term,NULL)==0);
    HVERIFY(sigaction(SIGCHLD,&initial,NULL)==0);
    HVERIFY(sigprocmask(SIG_SETMASK,&initial_mask,NULL)==0);
    printf("PASS %s actual_waitpid_eintr=%d\n",name,helper_real_eintr);return 0;
}
int main(int argc,char **argv)
{ return argc==2?helper_case(argv[1]):2; }
#elif defined(FILEMANIP_PATHS_ONLY) || defined(FILEMANIP_DIRECTORIES_ONLY)
#include <stdarg.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>

typedef int Boolean;
typedef void *Widget;
typedef void (*FileOperationErrorProc)(Widget,char *,char *);
#define True 1
#define False 0
#define DPRINTF(args) ((void)0)
#define GETMESSAGE(set,id,text) (text)
#define XtNewString strdup
#define XtMalloc malloc
#define XtFree free
#include "filemanip-path-definitions.inc"
#include "SharedMsgs.h"
static char *home_host_name="local", *root_title="root";
static char *MOVE_CMD="/bin/mv", *LINK_CMD="/bin/ln";
static int path_errors, ancestry_fault, ancestry_hits, ancestry_non2;
static int path_fallback_mode, path_move_calls, path_link_calls, path_readlink_hits;
static int path_rollback_mode, path_unlink_hits, path_link_hits;
static int path_follow_null, path_follow_calls;
static int path_check_access(const char *p,int flags) {return access(p,flags);}
#define CheckAccess path_check_access
#if defined(FILEMANIP_DIRECTORIES_ONLY)
#include <sys/wait.h>
static char *DTCOPY;
static const char *directory_device_path;
static int directory_device_hits;
#define DBGFORK(args) do { if(0) printf args; } while(0)
static void DtLastChar(char *s,char **last,int *length)
{size_t n=strlen(s);*last=s+(n?n-1:0);*length=n?1:0;}
/* UI adapter: private fixture has no open CDE views. */
static int DirectoryBusy(char *path) {(void)path;return 0;}
#else
static int DirectoryBusy(char *path) {(void)path;abort();}
/* These adapters MUST remain unreachable. This mode qualifies whole
 * FileManip for regular files/symlinks only, never directory transfer. */
static Boolean CopyDir(Widget w,int mode,char *from,char *to,Boolean container,struct stat *st,
                       FileOperationErrorProc error,Boolean busy,int type)
{(void)w;(void)mode;(void)from;(void)to;(void)container;(void)st;(void)error;(void)busy;(void)type;abort();}
static Boolean MoveDir(Widget w,char *from,char *to,Boolean container,struct stat *st,
                       FileOperationErrorProc error,int type)
{(void)w;(void)from;(void)to;(void)container;(void)st;(void)error;(void)type;abort();}
#endif
/* Command boundary seam: deliberately reports failure while recording actual
 * production dispatch. This case does NOT qualify external helper execution. */
static int RunFileCommand(char *cmd,char *a,char *b,char *c)
{
    (void)a;(void)b;(void)c;if(!path_fallback_mode)abort();
    if(!strcmp(cmd,MOVE_CMD))++path_move_calls;
    else if(!strcmp(cmd,LINK_CMD))++path_link_calls;
    else abort();
    errno=EIO;return -1;
}
/* Resolver seam only; FileManip still owns the actual fallback transfer. */
static char *_DtFollowLink(char *p)
{ ++path_follow_calls;return path_follow_null?NULL:p; }
static char *DName(char *p) {char *s=strrchr(p,'/');return s?s+1:p;}
char *GetSharedMessage(int id) {(void)id;return "File operation failed: %s";}
static void path_error(Widget w,char *a,char *b) {(void)w;(void)a;(void)b;++path_errors;}
static int path_fstat(int fd,struct stat *st)
{
    if(ancestry_fault==1) {++ancestry_hits;errno=EIO;return -1;}
    int rc=fstat(fd,st);
    if(rc==0 && ancestry_non2)st->st_ino+=(ino_t)1000;
    return rc;
}
static int path_openat(int fd,const char *p,int flags,...)
{
    if(ancestry_fault==2) {++ancestry_hits;errno=EACCES;return -1;}
    return openat(fd,p,flags);
}
static int path_close(int fd)
{
    int rc=close(fd); /* Consume real descriptor even when reporting a fault. */
    if(ancestry_fault==3) {++ancestry_hits;errno=EIO;return -1;}
    return rc;
}
static ssize_t path_readlink(const char *path,char *value,size_t size)
{
    if(path_fallback_mode) {++path_readlink_hits;errno=EIO;return -1;}
    return readlink(path,value,size);
}
static int path_link(const char *from,const char *to)
{
    if(path_rollback_mode) {++path_link_hits;errno=EXDEV;return -1;}
    return link(from,to);
}
static int path_unlink(const char *path)
{
    if(path_rollback_mode && !strcmp(path,"source")) {++path_unlink_hits;errno=EACCES;return -1;}
    return unlink(path);
}
#if defined(FILEMANIP_DIRECTORIES_ONLY)
static int directory_stat(const char *path,struct stat *st)
{
    int rc=stat(path,st);
    if(rc==0&&directory_device_path&&!strcmp(path,directory_device_path)) {
        st->st_dev^=(dev_t)1;++directory_device_hits;
    }
    return rc;
}
#define stat(path,st) directory_stat(path,st)
#endif
#define fstat path_fstat
#define openat path_openat
#define close path_close
#define readlink path_readlink
#define link path_link
#define unlink path_unlink
#if defined(FILEMANIP_DIRECTORIES_ONLY)
#include "filemanip-directory-production.inc"
#else
#include "filemanip-path-production.inc"
#endif
#undef stat
#undef fstat
#undef openat
#undef close
#undef readlink
#undef link
#undef unlink
#define PVERIFY(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d %s errno=%d\n",name,__LINE__,#x,errno);return 1;}} while(0)
static int path_put(const char *p,const char *text)
{
    int fd=open(p,O_CREAT|O_EXCL|O_WRONLY,0600);if(fd<0)return -1;
    ssize_t n=write(fd,text,strlen(text));int rc=close(fd);
    return n==(ssize_t)strlen(text)&&rc==0?0:-1;
}
static int path_exact(const char *p,const char *text)
{
    char data[128];int fd=open(p,O_RDONLY);if(fd<0)return 0;
    ssize_t n=read(fd,data,sizeof(data));close(fd);
    return n==(ssize_t)strlen(text)&&!memcmp(data,text,(size_t)n);
}
static int path_case(const char *name)
{
    if(!strcmp(name,"path-join-dynamic")) {
        char raw[]="/native\n\\copy";char *copy=FilePathCopy(raw);PVERIFY(copy&&!strcmp(copy,raw));
        raw[1]='X';PVERIFY(!strcmp(copy,"/native\n\\copy"));free(copy);
        PVERIFY(!FilePathCopy(NULL)&&errno==EINVAL);
        size_t n=16000;char *directory=malloc(n+1);PVERIFY(directory);
        memset(directory,'d',n);directory[0]='/';directory[n]=0;
        char *p=FilePathJoin(directory,"native\n\\leaf");PVERIFY(p);
        PVERIFY(strlen(p)==n+1+strlen("native\n\\leaf") && !memcmp(p,directory,n));
        PVERIFY(!strcmp(p+n,"/native\n\\leaf"));
        char *root=FilePathJoin("/","leaf"), *trail=FilePathJoin("/dir/","leaf");
        PVERIFY(root && trail && !strcmp(root,"/leaf") && !strcmp(trail,"/dir/leaf"));
        FilePathsFree(p,root,trail,directory);
        PVERIFY(!FilePathJoin(NULL,"leaf") && errno==EINVAL);
        PVERIFY(!FilePathJoin("/dir","") && errno==EINVAL);
    } else if(!strcmp(name,"path-parent-native")) {
        const char raw[]="/dir\n\\native/leaf///";
        char *a=FileParentPath(raw),*b=FileParentPath("/"),*c=FileParentPath("leaf");
        PVERIFY(a&&b&&c && !strcmp(a,"/dir\n\\native") && !strcmp(b,"/") && !strcmp(c,"."));
        PVERIFY(!strcmp(raw,"/dir\n\\native/leaf///"));FilePathsFree(a,b,c,NULL);
    } else if(!strncmp(name,"path-occupied-",14)) {
        PVERIFY(path_put("source","input-source")==0);
        if(!strcmp(name+14,"file"))PVERIFY(path_put("target","old-target")==0);
        else if(!strcmp(name+14,"directory"))PVERIFY(mkdir("target",0700)==0);
        else {PVERIFY(!strcmp(name+14,"symlink"));PVERIFY(path_put("referent","old-target")==0);PVERIFY(symlink("referent","target")==0);}
        PVERIFY(!FileManip(NULL,COPY_FILE,"source","target",False,path_error,False,NOT_DESKTOP));
        PVERIFY(path_exact("source","input-source") && path_errors>0);
        if(!strcmp(name+14,"file"))PVERIFY(path_exact("target","old-target"));
        else if(!strcmp(name+14,"directory")) {struct stat st;PVERIFY(stat("target",&st)==0&&S_ISDIR(st.st_mode));PVERIFY(lstat("target/source",&st)<0&&errno==ENOENT);}
        else {char text[20];PVERIFY(readlink("target",text,sizeof(text))==8&&!memcmp(text,"referent",8));PVERIFY(path_exact("referent","old-target"));}
    } else if(!strcmp(name,"path-move-followlink-null")) {
        PVERIFY(path_put("source","input-source")==0);path_follow_null=1;
        PVERIFY(FileManip(NULL,MOVE_FILE,"source","target",False,path_error,False,NOT_DESKTOP));
        PVERIFY(path_follow_calls==1 && path_errors==0);
        PVERIFY(path_exact("target","input-source"));
        struct stat st;PVERIFY(lstat("source",&st)<0&&errno==ENOENT);
        puts("_DtFollowLink NULL resolver seam; actual regular-file MOVE copy fallback and source removal.");
    } else if(!strcmp(name,"path-move-rollback-null-handler")) {
        PVERIFY(path_put("source","input-source")==0);path_rollback_mode=1;
        PVERIFY(!FileManip(NULL,MOVE_FILE,"source","target",False,NULL,False,NOT_DESKTOP));
        PVERIFY(path_link_hits==1 && path_unlink_hits==1);
        PVERIFY(path_exact("source","input-source"));
        struct stat st;PVERIFY(lstat("target",&st)<0&&errno==ENOENT);
        puts("Reported EXDEV/source-unlink EACCES seams; actual copy and target rollback with NULL errorHandler.");
    } else if(!strcmp(name,"path-move-fallback-command")) {
        PVERIFY(symlink("missing-native-target","source-link")==0);path_fallback_mode=1;
        PVERIFY(!FileManip(NULL,MOVE_FILE,"source-link","target-link",False,NULL,False,NOT_DESKTOP));
        PVERIFY(path_readlink_hits==1 && path_move_calls==1 && path_link_calls==0);
        char value[64];PVERIFY(readlink("source-link",value,sizeof(value))==21&&!memcmp(value,"missing-native-target",21));
        struct stat st;PVERIFY(lstat("target-link",&st)<0&&errno==ENOENT);
        puts("Reported readlink/RunFileCommand failure seams; MOVE dispatch observed, LINK forbidden; no native exec claim.");
    } else if(!strcmp(name,"path-symlink-text")) {
        size_t n=2048;char *text=malloc(n+1);PVERIFY(text);memset(text,'x',n);text[n]=0;
        if(symlink(text,"source-link")!=0) {
            PVERIFY(errno==ENAMETOOLONG);n=700;text[n]=0;
            PVERIFY(symlink(text,"source-link")==0);
            puts("Native symlink limit: 700-byte growth fixture; no >1024 native claim.");
        }
        char *readback=FileReadLink("source-link");PVERIFY(readback&&strlen(readback)==n&&!memcmp(readback,text,n));free(readback);
        PVERIFY(FileManip(NULL,MOVE_FILE,"source-link","target-link",False,path_error,False,NOT_DESKTOP));
        readback=FileReadLink("target-link");PVERIFY(readback&&strlen(readback)==n&&!memcmp(readback,text,n));
        struct stat st;PVERIFY(lstat("source-link",&st)<0&&errno==ENOENT);FilePathsFree(text,readback,NULL,NULL);
    } else if(!strcmp(name,"path-container-deep") || !strncmp(name,"ancestry-",9)) {
        PVERIFY(mkdir("tree",0700)==0 && mkdir("sibling",0700)==0);
        struct stat ancestor,root,sibling;PVERIFY(stat("tree",&ancestor)==0&&stat("/",&root)==0&&stat("sibling",&sibling)==0);
        char *cwd=getcwd(NULL,0);PVERIFY(cwd);char *deep=FilePathJoin(cwd,"tree");PVERIFY(deep);
        char component[81];memset(component,'d',80);component[80]=0;
        long max=pathconf(".",_PC_PATH_MAX);PVERIFY(max>0);
        size_t wanted=max>2000?1500:600;
        while(strlen(deep)<wanted) {
            char *next=FilePathJoin(deep,component);PVERIFY(next);free(deep);deep=next;PVERIFY(mkdir(deep,0700)==0);
        }
        if(!strcmp(name,"path-container-deep")) {
            PVERIFY(path_put("source","input-source")==0);
            PVERIFY(FileManip(NULL,COPY_FILE,"source",deep,True,path_error,False,NOT_DESKTOP));
            char *target=FilePathJoin(deep,"source");PVERIFY(target&&path_exact(target,"input-source"));free(target);
            printf("Actual native container length=%zu native_path_limit=%ld\n",strlen(deep),max);
        } else if(!strcmp(name,"ancestry-deep")) {
            PVERIFY(FileDirectoryHasAncestor(deep,&ancestor)==1);
            PVERIFY(FileDirectoryHasAncestor(deep,&sibling)==0);
        } else if(!strcmp(name,"ancestry-root")) {
            PVERIFY(FileDirectoryHasAncestor(deep,&root)==1);
            PVERIFY(FileDirectoryHasAncestor("/",&sibling)==0);
        } else if(!strcmp(name,"ancestry-dev-identity")) {
            ancestor.st_dev^=(dev_t)1;
            PVERIFY(FileDirectoryHasAncestor(deep,&ancestor)==0); /* same inode alone is insufficient */
        } else if(!strcmp(name,"ancestry-root-non2")) {
            ancestry_non2=1;root.st_ino+=(ino_t)1000;PVERIFY(root.st_ino!=2);
            PVERIFY(FileDirectoryHasAncestor(deep,&root)==1);
            sibling.st_ino+=(ino_t)1000;PVERIFY(FileDirectoryHasAncestor("/",&sibling)==0);
            puts("Root non-2: metadata seam, real descriptor traversal; no native inode assignment claim.");
        } else {
            if(!strcmp(name,"ancestry-fstat-error"))ancestry_fault=1;
            else if(!strcmp(name,"ancestry-openat-error"))ancestry_fault=2;
            else {PVERIFY(!strcmp(name,"ancestry-close-error"));ancestry_fault=3;}
            PVERIFY(Check(NULL,deep,&sibling,COPY_FILE,path_error)!=0);
            PVERIFY(path_errors>0&&ancestry_hits>0);
            PVERIFY(errno==(ancestry_fault==2?EACCES:EIO));
        }
        FilePathsFree(deep,cwd,NULL,NULL);
    } else {fprintf(stderr,"Unknown path case %s\n",name);return 2;}
    printf("PASS %s\n",name);return 0;
}
#if defined(FILEMANIP_DIRECTORIES_ONLY)
#include <sys/wait.h>

/* A deliberately small external helper fixture, reached by real execlp.
 * It records argv, waits on a real gate, and produces controlled exit/signal
 * outcomes. It does not emulate dtfile_copy's filesystem transfer engine. */
static int directory_io(int fd,void *data,size_t length,int writing)
{
    unsigned char *p=data;
    while(length) {
        ssize_t n=writing?write(fd,p,length):read(fd,p,length);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        p+=(size_t)n;length-=(size_t)n;
    }
    return 0;
}
static int directory_helper(int argc,char **argv)
{
    const char *start=getenv("FILEMANIP_START_FD"),*gate=getenv("FILEMANIP_GATE_FD");
    const char *result=getenv("FILEMANIP_RESULT_FD"),*behavior=getenv("FILEMANIP_HELPER_BEHAVIOR");
    if(!start||!gate||!result||!behavior)return 110;
    close(atoi(result));
    int fd=open("helper.argv",O_CREAT|O_EXCL|O_WRONLY,0600);if(fd<0)return 111;
    pid_t identity[2]={getpid(),getppid()};
    if(directory_io(fd,identity,sizeof(identity),1)!=0)return 112;
    for(int i=0;i<argc;++i) {
        size_t length=strlen(argv[i]);
        if(directory_io(fd,&length,sizeof(length),1)!=0||directory_io(fd,argv[i],length,1)!=0)return 113;
    }
    if(close(fd)!=0)return 114;
    char byte='S';
    if(directory_io(atoi(start),&byte,1,1)!=0)return 115;
    if(directory_io(atoi(gate),&byte,1,0)!=0)return 116;
    close(atoi(start));close(atoi(gate));
    if(!strcmp(behavior,"signal")) {raise(SIGTERM);return 117;}
    if(path_put("helper.completed","helper finished")!=0)return 118;
    return !strcmp(behavior,"failure")?1:0;
}
static int directory_envfd(const char *key,int fd)
{char value[40];snprintf(value,sizeof(value),"%d",fd);return setenv(key,value,1);}
static int directory_case(const char *name,const char *binary)
{
    int move=strstr(name,"move-")!=NULL;
    int failure=strstr(name,"failure")!=NULL;
    int missing=strstr(name,"missing-exec")!=NULL;
    int signaled=strstr(name,"signal")!=NULL;
    int deep=strstr(name,"dynamic-target")!=NULL;
    PVERIFY(mkdir("source-home",0700)==0&&mkdir("target-home",0700)==0&&mkdir("helper-bin",0700)==0);
    char *cwd=getcwd(NULL,0);PVERIFY(cwd);
    char *source=FilePathJoin(cwd,"source-home/source-dir");
    char *container=FilePathJoin(cwd,"target-home");
    char *executable=FilePathJoin(cwd,"helper-bin/dtfile_copy");PVERIFY(source&&container&&executable);
    PVERIFY(mkdir(source,0700)==0);
    char *payload=FilePathJoin(source,"payload");PVERIFY(payload&&path_put(payload,"source preserved")==0);
    if(deep) {
        long maximum=pathconf(".",_PC_PATH_MAX);PVERIFY(maximum>0);
        size_t goal=maximum>2000?1500:600;
        while(strlen(container)<goal) {
            char *next=FilePathJoin(container,"native\\folder-long-component-0123456789");
            PVERIFY(next&&mkdir(next,0700)==0);free(container);container=next;
        }
    }
    char *target=FilePathJoin(container,"source-dir");PVERIFY(target);
    if(!missing)PVERIFY(symlink(binary,executable)==0);
    DTCOPY=executable;
    int started[2],gate[2],result_pipe[2];
    PVERIFY(pipe(started)==0&&pipe(gate)==0&&pipe(result_pipe)==0);
    PVERIFY(directory_envfd("FILEMANIP_START_FD",started[1])==0);
    PVERIFY(directory_envfd("FILEMANIP_GATE_FD",gate[0])==0);
    PVERIFY(directory_envfd("FILEMANIP_RESULT_FD",result_pipe[1])==0);
    PVERIFY(setenv("FILEMANIP_HELPER_BEHAVIOR",signaled?"signal":failure?"failure":"success",1)==0);
    struct CallResult {int returned,error,no_child;pid_t caller;int device_hits;} result={0};
    pid_t caller=fork();PVERIFY(caller>=0);
    if(caller==0) {
        close(started[0]);close(gate[1]);close(result_pipe[0]);
        directory_device_path=move?container:NULL;
        result.returned=FileManip(NULL,move?MOVE_FILE:COPY_FILE,source,container,True,path_error,False,NOT_DESKTOP);
        result.error=errno;result.caller=getpid();result.device_hits=directory_device_hits;
        int status=0;errno=0;
        result.no_child=waitpid(-1,&status,WNOHANG)<0&&errno==ECHILD;
        close(started[1]);close(gate[0]);
        int rc=directory_io(result_pipe[1],&result,sizeof(result),1);close(result_pipe[1]);
        _exit(rc==0?0:119);
    }
    close(started[1]);close(gate[0]);close(result_pipe[1]);
    if(!missing) {
        struct pollfd ready={started[0],POLLIN,0};PVERIFY(poll(&ready,1,3000)>0);
        char byte;PVERIFY(directory_io(started[0],&byte,1,0)==0&&byte=='S');
        struct pollfd early={result_pipe[0],POLLIN,0};
        PVERIFY(poll(&early,1,0)==0); /* caller must still wait while helper is gated */
        byte='G';PVERIFY(directory_io(gate[1],&byte,1,1)==0);
    }
    close(started[0]);close(gate[1]);
    struct pollfd done={result_pipe[0],POLLIN,0};PVERIFY(poll(&done,1,3000)>0);
    PVERIFY(directory_io(result_pipe[0],&result,sizeof(result),0)==0);close(result_pipe[0]);
    int status;PVERIFY(waitpid(caller,&status,0)==caller&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    PVERIFY(result.caller==caller&&result.no_child);
    PVERIFY(!!result.returned==!(failure||missing||signaled));
    if(missing)PVERIFY(result.error==ENOEXEC);
    if(signaled)PVERIFY(result.error==EIO);
    if(move)PVERIFY(result.device_hits>0);
    PVERIFY(path_exact(payload,"source preserved")); /* helper is an outcome fixture, not transfer engine */
    if(!missing) {
        int fd=open("helper.argv",O_RDONLY);PVERIFY(fd>=0);
        pid_t identity[2];PVERIFY(directory_io(fd,identity,sizeof(identity),0)==0);
        PVERIFY(identity[0]>0&&identity[0]!=caller&&identity[1]==caller);
        char *last[2]={NULL,NULL};int seen_move=0;
        for(;;) {
            size_t length;ssize_t n=read(fd,&length,sizeof(length));if(n==0)break;
            PVERIFY(n==(ssize_t)sizeof(length)&&length<65536);
            char *arg=malloc(length+1);PVERIFY(arg);
            PVERIFY(directory_io(fd,arg,length,0)==0);arg[length]=0;
            if(!strcmp(arg,"-move"))seen_move=1;
            free(last[0]);last[0]=last[1];last[1]=arg;
        }
        close(fd);PVERIFY(last[0]&&last[1]&&!strcmp(last[0],source)&&!strcmp(last[1],target));
        PVERIFY(seen_move==move);FilePathsFree(last[0],last[1],NULL,NULL);
        if(!signaled)PVERIFY(path_exact("helper.completed","helper finished"));
        printf("Actual helper pid=%ld caller=%ld resolved_target=%s length=%zu\n",(long)identity[0],(long)caller,target,strlen(target));
    }
    if(move)puts("Cross-device branch forced by target-parent st_dev metadata ONLY; not native EXDEV qualification.");
    FilePathsFree(source,container,executable,payload);FilePathsFree(target,cwd,NULL,NULL);
    printf("PASS %s real production directory caller/helper wait/outcome; helper transfer engine excluded\n",name);return 0;
}
#endif

int main(int argc,char **argv)
{
#if defined(FILEMANIP_DIRECTORIES_ONLY)
    if(argc>3&&!strcmp(argv[0],"dtfile_copy"))return directory_helper(argc,argv);
    if(argc!=3||chdir(argv[2])!=0)return 2;
    if(!strncmp(argv[1],"directory-",10))return directory_case(argv[1],argv[0]);
#endif
    return argc==3&&chdir(argv[2])==0?path_case(argv[1]):2;
}


#else
#error Select FILEMANIP_HELPER_ONLY, FILEMANIP_PATHS_ONLY or FILEMANIP_DIRECTORIES_ONLY
#endif
