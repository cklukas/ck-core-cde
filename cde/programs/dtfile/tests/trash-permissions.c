/* Actual Trash permission functions, real private trees, reported syscall
 * faults. Root/NFS selector overrides exercise classification only: these
 * fixtures make no claim about a native NFS server or retained delete rights. */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/vfs.h>
#include <linux/magic.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/mount.h>
#elif defined(__NetBSD__)
#include <sys/statvfs.h>
#endif
typedef int Boolean;
#define True 1
#define False 0
#define DPRINTF(args) ((void)0)
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif

static const char *case_name;
static int failed, injected, allocations, opened, closed, leaf_checks, followed;
static int root_probe, root_reads, read_fault, close_fault, allocation_fault;
static DIR *fault_directory;
static int probe_fd=-1, probe_closes, probe_removes;
static char parent[PATH_MAX], destination[PATH_MAX], probe_path[PATH_MAX], probe_template[PATH_MAX];
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s (errno=%d)\n",case_name,__LINE__,#c,errno);failed=1; } } while (0)
static void fixture(int ok) { if (!ok) { perror("trash-permission fixture");exit(2); } }
static void put(const char *name)
{
    int fd=open(name,O_CREAT|O_EXCL|O_WRONLY,0600);fixture(fd>=0);
    fixture(write(fd,"keep payload",12)==12);fixture(close(fd)==0);
}
static int payload_unchanged(const char *name)
{
    char bytes[32];int fd=open(name,O_RDONLY);if (fd<0) return 0;
    ssize_t length=read(fd,bytes,sizeof(bytes));fixture(close(fd)==0);
    return length==12 && !memcmp(bytes,"keep payload",12);
}
static int CheckAccess(const char *path,int mode)
{
    if ((!strcmp(case_name,"parent-permission") && !strcmp(path,parent)) ||
        (!strcmp(case_name,"child-permission") && strstr(path,"/tree/child"))) {
        ++injected;errno=EACCES;return -1;
    }
    return access(path,mode);
}
static uid_t TestUid(void)
{
    uid_t real=getuid();
    return root_probe?0:(real?real:1000);
}
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
static int TestStatfs(const char *path,struct statfs *info)
{
    int result=statfs(path,info);
    if (!result && root_probe) {
#if defined(__linux__)
        info->f_type=NFS_SUPER_MAGIC;
#else
        strcpy(info->f_fstypename,"nfs");
#endif
    }
    return result;
}
#elif defined(__NetBSD__)
static int TestStatvfs(const char *path,struct statvfs *info)
{ return statvfs(path,info); }
#endif
static int TestLstat(const char *path,struct stat *info)
{
    const char *name=strrchr(path,'/');name=name?name+1:path;
    if (!strcmp(name,"file")) ++leaf_checks;
    return lstat(path,info);
}
static DIR *TestOpendir(const char *path)
{
    if (strstr(path,"/outside") || strstr(path,"/tree/link")) ++followed;
    DIR *dir=opendir(path);
    if (dir) { if (!opened) fault_directory=dir;++opened; }
    return dir;
}
static struct dirent *TestReaddir(DIR *dir)
{
    if (dir==fault_directory) {
        ++root_reads;
        if (read_fault && !injected && (read_fault==EINTR || root_reads>=2)) {
            ++injected;errno=read_fault;return NULL;
        }
    }
    return readdir(dir);
}
static int TestClosedir(DIR *dir)
{
    int target=dir==fault_directory;
    int result=closedir(dir);++closed;
    if (target && close_fault) { errno=close_fault;return -1; }
    return result;
}
static void *TestMalloc(size_t size)
{
    if (++allocations==allocation_fault) { ++injected;errno=ENOMEM;return NULL; }
    return malloc(size);
}
static int TestMkstemp(char *path)
{
    CHECK(strlen(path)<sizeof(probe_template));strcpy(probe_template,path);
    int fd=mkstemp(path);
    if (fd>=0) { probe_fd=fd;CHECK(strlen(path)<sizeof(probe_path));strcpy(probe_path,path); }
    return fd;
}
static int TestClose(int fd)
{
    int result=close(fd);
    if (fd==probe_fd) {
        ++probe_closes;
        if (strstr(case_name,"close-error") || strstr(case_name,"first-error")) {
            ++injected;errno=EIO;return -1;
        }
    }
    return result;
}
static int TestRemove(const char *path)
{
    ++probe_removes;
    if (strstr(case_name,"remove-error") || strstr(case_name,"first-error")) {
        ++injected;errno=EACCES;return -1;
    }
    return remove(path);
}
#define getuid TestUid
#define statfs(...) TestStatfs(__VA_ARGS__)
#define statvfs(...) TestStatvfs(__VA_ARGS__)
#define lstat TestLstat
#define opendir TestOpendir
#define readdir TestReaddir
#define closedir TestClosedir
#define malloc TestMalloc
#define mkstemp TestMkstemp
#define close TestClose
#define remove TestRemove
#include "trash-permissions-production.inc"
#undef getuid
#undef statfs
#undef statvfs
#undef lstat
#undef opendir
#undef readdir
#undef closedir
#undef malloc
#undef mkstemp
#undef close
#undef remove

static int make_deep(char result[PATH_MAX])
{
    long maximum=pathconf(".",_PC_PATH_MAX);
    if (maximum>0 && maximum<=1250) {
        fprintf(stderr,"SKIP: native pathname limit %ld prevents a real >1024-byte path\n",maximum);
        return 0;
    }
    char path[PATH_MAX];fixture(getcwd(path,sizeof(path))!=NULL);
    while (strlen(path)<1120) {
        const char *part="/private-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
        fixture(strlen(path)+strlen(part)+16<sizeof(path));strcat(path,part);
        fixture(mkdir(path,0700)==0);
    }
    strcpy(result,path);return 1;
}
static int helper_case(void)
{
#if HAVE_DELETE_PERMISSION_CHILD
    const char *raw="/native/a\\\001-\303\251",*name="kid\n\t\\\002";
    char *joined=DeletePermissionChild(raw,name);
    CHECK(joined && !strcmp(joined,"/native/a\\\001-\303\251/kid\n\t\\\002"));free(joined);
    joined=DeletePermissionChild("/","leaf");CHECK(joined && !strcmp(joined,"/leaf"));free(joined);
    joined=DeletePermissionChild("/parent/","leaf");CHECK(joined && !strcmp(joined,"/parent/leaf"));free(joined);
    char *long_parent=malloc(1201);fixture(long_parent!=NULL);memset(long_parent,'x',1200);long_parent[0]='/';long_parent[1200]=0;
    joined=DeletePermissionChild(long_parent,"leaf");
    CHECK(joined && strlen(joined)==1205 && !memcmp(joined,long_parent,1200) && !strcmp(joined+1200,"/leaf"));
    free(joined);free(long_parent);
    allocation_fault=allocations+1;errno=0;joined=DeletePermissionChild("/parent","leaf");
    CHECK(!joined && errno==ENOMEM && injected==1);
    return failed?1:0;
#else
    fputs("SKIP: selected baseline has no DeletePermissionChild helper\n",stderr);return 77;
#endif
}
int main(int argc,char **argv)
{
    fixture(argc==3);case_name=argv[1];fixture(chdir(argv[2])==0);
    if (!strcmp(case_name,"helper-bytes")) return helper_case();
    fixture(getcwd(parent,sizeof(parent))!=NULL);
    fixture(mkdir("tree",0700)==0 && mkdir("tree/child",0700)==0);
    put("tree/child/file");
    fixture(strlen(parent)+6<sizeof(destination));strcpy(destination,parent);strcat(destination,"/tree");
    if (!strcmp(case_name,"deep-tree") || !strcmp(case_name,"probe-deep")) {
        char deep[PATH_MAX];if (!make_deep(deep)) return 77;
        if (!strcmp(case_name,"deep-tree")) {
            fixture(strlen(deep)+6<sizeof(destination));strcpy(destination,deep);strcat(destination,"/tree");
            fixture(mkdir(destination,0700)==0);
            char file[PATH_MAX];strcpy(file,destination);strcat(file,"/file");put(file);
        } else strcpy(parent,deep);
    }
    if (!strcmp(case_name,"symlink-no-follow")) {
        fixture(mkdir("outside",0700)==0);put("outside/file");
        fixture(symlink("../outside","tree/link")==0);
    }
    read_fault=!strcmp(case_name,"readdir-eintr")?EINTR:
        (!strcmp(case_name,"readdir-eio") || !strcmp(case_name,"first-error"))?EIO:0;
    close_fault=!strcmp(case_name,"closedir-eio")?EIO:!strcmp(case_name,"first-error")?ENOSPC:0;
    if (!strcmp(case_name,"child-oom")) allocation_fault=1;
    root_probe=!strncmp(case_name,"probe-",6);
    char *saved_parent=strdup(parent),*saved_destination=strdup(destination);
    fixture(saved_parent && saved_destination);
    errno=0;int result=CheckDeletePermission(parent,destination),error=errno;
    int expected=!strcmp(case_name,"parent-permission") || !strcmp(case_name,"child-permission")?EACCES:
        !strcmp(case_name,"child-oom")?ENOMEM:
        !strcmp(case_name,"readdir-eio") || !strcmp(case_name,"first-error") ||
        !strcmp(case_name,"closedir-eio") || !strcmp(case_name,"probe-close-error") ||
        !strcmp(case_name,"probe-first-error")?EIO:
        !strcmp(case_name,"probe-remove-error")?EACCES:0;
    CHECK(expected?(result==-1 && error==expected):result==0);
    CHECK(!strcmp(parent,saved_parent) && !strcmp(destination,saved_destination));
    CHECK(payload_unchanged("tree/child/file"));
    if (!strcmp(case_name,"deep-tree")) {
        char file[PATH_MAX];fixture(strlen(destination)+6<sizeof(file));
        strcpy(file,destination);strcat(file,"/file");CHECK(payload_unchanged(file));
    }
    CHECK(opened==closed);
    if (!root_probe && !expected) CHECK(leaf_checks>0);
    if (!strcmp(case_name,"readdir-eintr")) CHECK(injected==1 && root_reads>1 && leaf_checks>0);
    if (!strcmp(case_name,"parent-permission")) CHECK(opened==0 && injected==1);
    if (!strcmp(case_name,"child-permission") || !strcmp(case_name,"child-oom") || !strcmp(case_name,"readdir-eio"))
        CHECK(injected==1);
    if (!strcmp(case_name,"symlink-no-follow")) {
        struct stat info;CHECK(lstat("tree/link",&info)==0 && S_ISLNK(info.st_mode));
        CHECK(!followed && payload_unchanged("outside/file"));
    }
    if (root_probe) {
        fprintf(stderr,"SIMULATED ROOT/NFS branch: close=%d remove=%d result=%d errno=%d\n",probe_closes,probe_removes,result,error);
        CHECK(probe_closes==1 && probe_removes==1);
        size_t n=strlen(parent);
        CHECK(!memcmp(probe_template,parent,n) && !strcmp(probe_template+n,"/dtfile.XXXXXX"));
        struct stat info;
        if (strstr(case_name,"remove-error") || strstr(case_name,"first-error")) {
            CHECK(lstat(probe_path,&info)==0);
            fixture(remove(probe_path)==0); /* Test cleanup after reported removal refusal. */
        }
        errno=0;CHECK(lstat(probe_path,&info)<0 && errno==ENOENT);
    }
    free(saved_parent);free(saved_destination);
    if (!failed) printf("PASS %s\n",case_name);
    return failed?1:0;
}
