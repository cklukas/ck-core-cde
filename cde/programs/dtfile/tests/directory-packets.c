#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int Boolean;
#define True 1
#define False 0
#define NILL '\0'
#define FILEDATABUF 50
#define PIPEMSG_DONE 3
#define PIPEMSG_DESKTOP_REMOVED 8
#define PIPEMSG_DESKTOP_CHANGED 9

typedef struct _FileData {
    struct _FileData *next;
    char *file_name;
    char *action_name;
    unsigned char physical_type;
    char *logical_type;
    int errnum;
    struct stat stat;
    char *link;
    char *final_link;
    Boolean is_subdir;
    Boolean is_broken;
} FileData;

typedef struct _FileData2 {
    struct _FileData2 *next;
    short file_name;
    short action_name;
    unsigned char physical_type;
    short logical_type;
    int errnum;
    struct stat stat;
    short link;
    short final_link;
    Boolean is_subdir;
    Boolean is_broken;
    char text[BUFSIZ];
} FileData2;

static int freed_records;
static void *test_malloc(size_t size) { void *p=malloc(size); if(!p) abort(); return p; }
static void *test_calloc(size_t n,size_t size) { void *p=calloc(n,size); if(!p) abort(); return p; }
#define XtMalloc test_malloc
#define XtCalloc test_calloc
#define XtFree free

static void FreeFileData(FileData *data, Boolean all)
{
    if(!data)return;
    free(data->file_name);free(data->action_name);free(data->logical_type);
    free(data->link);free(data->final_link);
    if(all){freed_records++;free(data);}
}

static int exact_write(int fd,const void *data,size_t size)
{
    const char *next=data;while(size){ssize_t n=write(fd,next,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;next+=n;size-=(size_t)n;}return 0;
}
static int DtfileProcessWriteAll(int fd,const void *data,size_t size){return exact_write(fd,data,size);}
static int PipeReadExact(int fd,void *data,size_t size)
{
    char *next=data;while(size){ssize_t n=read(fd,next,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;next+=n;size-=(size_t)n;}return 0;
}
static int PipeWriteString(int fd,char *value)
{
    size_t length=value?strlen(value):0;short encoded;if(length>SHRT_MAX)return -1;encoded=(short)length;return exact_write(fd,&encoded,sizeof(encoded))!=0||(length&&exact_write(fd,value,length)!=0)?-1:(int)length;
}
static int PipeReadStringField(int fd,char **value)
{
    short length;char *text;*value=NULL;if(PipeReadExact(fd,&length,sizeof(length))!=0||length<0)return -1;if(!length)return 0;text=malloc((size_t)length+1);if(PipeReadExact(fd,text,(size_t)length)!=0){free(text);return -1;}text[length]=0;*value=text;return 0;
}

/* PRODUCTION_FUNCTIONS */

static int failed;
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);failed=1;} } while(0)

static int make_record(FileData2 *record,const char *name,const char *type)
{
    memset(record,0,sizeof(*record));record->physical_type=7;record->errnum=11;
    record->is_subdir=True;
    return FileData2PackText(record,name,"action",type,"link","final");
}

static void desktop_case(const char *name,FileData2 *record)
{
    int p[2],rc;short message;char *host=(char *)1,*directory=(char *)1,*file=(char *)1;FileData *decoded=(FileData *)1;
    CHECK(pipe(p)==0);
    if(!strcmp(name,"desktop-invalid-message")) {
        errno=0;CHECK(DirectoryWriteDesktopRecord(p[1],PIPEMSG_DONE,"host","/dir","file",record)==-1&&errno==EINVAL);close(p[0]);close(p[1]);return;
    } else if(!strcmp(name,"desktop-null-name")) {
        errno=0;CHECK(DirectoryWriteDesktopRecord(p[1],PIPEMSG_DESKTOP_REMOVED,"host","/dir",NULL,NULL)==-1&&errno==EINVAL);close(p[0]);close(p[1]);return;
    } else if(!strcmp(name,"desktop-wire-pointer")) {
        FileData2 packed;record->next=(FileData2 *)(uintptr_t)1;
        CHECK(DirectoryWriteDesktopRecord(p[1],PIPEMSG_DESKTOP_CHANGED,"host","/dir","file",record)==0);close(p[1]);
        CHECK(PipeReadExact(p[0],&message,sizeof(message))==0);CHECK(PipeReadStringField(p[0],&host)==0);CHECK(PipeReadStringField(p[0],&directory)==0);CHECK(PipeReadStringField(p[0],&file)==0);CHECK(PipeReadExact(p[0],&packed,sizeof(packed))==0&&packed.next==NULL);free(host);free(directory);free(file);close(p[0]);return;
    } else if(!strcmp(name,"desktop-write-changed")) {
        CHECK(DirectoryWriteDesktopRecord(p[1],PIPEMSG_DESKTOP_CHANGED,"host","/dir","alpha",record)==0);close(p[1]);
        CHECK(PipeReadExact(p[0],&message,sizeof(message))==0&&message==PIPEMSG_DESKTOP_CHANGED);
    } else if(!strcmp(name,"desktop-write-removed")) {
        CHECK(DirectoryWriteDesktopRecord(p[1],PIPEMSG_DESKTOP_REMOVED,"host","/dir","file",NULL)==0);close(p[1]);
        CHECK(PipeReadExact(p[0],&message,sizeof(message))==0&&message==PIPEMSG_DESKTOP_REMOVED);
    } else {
        message=PIPEMSG_DESKTOP_CHANGED;exact_write(p[1],&(short){4},sizeof(short));
        if(strcmp(name,"desktop-truncated-host")){exact_write(p[1],"host",4);PipeWriteString(p[1],"/dir");PipeWriteString(p[1],!strcmp(name,"desktop-mismatched-data")?"other":"alpha");if(strcmp(name,"desktop-truncated-data")){FileData2 malformed=*record;if(!strcmp(name,"desktop-invalid-data"))malformed.file_name=-1;exact_write(p[1],&malformed,sizeof(malformed));}}
        close(p[1]);
    }
    rc=DirectoryReadDesktopRecord(p[0],message,&host,&directory,&file,&decoded);close(p[0]);
    if(!strcmp(name,"desktop-write-changed")) {CHECK(rc==0&&!strcmp(host,"host")&&!strcmp(file,"alpha")&&!strcmp(decoded->file_name,"alpha"));FreeFileData(decoded,True);free(host);free(directory);free(file);}
    else if(!strcmp(name,"desktop-write-removed")) {CHECK(rc==0&&decoded==NULL&&!strcmp(file,"file"));free(host);free(directory);free(file);}
    else CHECK(rc==-1&&host==NULL&&directory==NULL&&file==NULL&&decoded==NULL&&errno==EPROTO);
}

static void free_list(FileData *head)
{
    while(head){FileData *next=head->next;FreeFileData(head,True);head=next;}
}

static void pack_filedata_case(const char *name)
{
    FileData source;FileData2 packed;FileData *decoded=NULL;size_t used=0;int rc;
    memset(&source,0,sizeof(source));source.file_name="alpha";source.action_name="action";source.logical_type="TYPE";source.link="link";source.final_link="final";source.physical_type=7;source.errnum=ENOENT;source.is_subdir=True;
    if(!strcmp(name,"pack-filedata-null")){errno=0;CHECK(FileData2PackFileData(&packed,NULL)==-1&&errno==EINVAL);return;}
    if(!strcmp(name,"pack-filedata-overflow")){char *large=malloc(BUFSIZ);memset(large,'x',BUFSIZ-1);large[BUFSIZ-1]=0;source.action_name=large;errno=0;CHECK(FileData2PackFileData(&packed,&source)==-1&&errno==EOVERFLOW);free(large);return;}
    rc=FileData2PackFileData(&packed,&source);CHECK(rc>0&&packed.next==NULL&&packed.errnum==ENOENT);CHECK(FileData2Decode(&packed,sizeof(packed),&decoded,&used)==0&&!strcmp(decoded->file_name,"alpha")&&!strcmp(decoded->final_link,"final")&&decoded->physical_type==7);FreeFileData(decoded,True);
}

int main(int argc,char **argv)
{
    FileData2 one,two;
    FileData *decoded=NULL;
    size_t used=99;
    int one_len,two_len;
    unsigned char packet[2*sizeof(FileData2)+1];
    const char *name;
    if(argc!=2)return 2;
    name=argv[1];
    one_len=make_record(&one,"alpha","TYPE");
    two_len=make_record(&two,"beta","TYPE2");
    CHECK(one_len>0&&two_len>0);
    if(!strncmp(name,"pack-filedata-",14)) {
        pack_filedata_case(name);
    } else if(!strncmp(name,"desktop-",8)) {
        desktop_case(name,&one);
    } else if(!strcmp(name,"pack-valid")) {
        CHECK(one.file_name==5&&one.action_name==6&&one.logical_type==4);
    } else if(!strcmp(name,"pack-overflow")) {
        FileData2 before=one;
        char *large=malloc(BUFSIZ);memset(large,'x',BUFSIZ-1);large[BUFSIZ-1]=0;
        errno=0;CHECK(FileData2PackText(&one,large,"a","b","c","d")==-1&&errno==EOVERFLOW);free(large);
        CHECK(!memcmp(&one,&before,sizeof(one)));
    } else if(!strcmp(name,"stat-failed")) {
        struct stat unavailable;memset(&unavailable,0xff,sizeof(unavailable));
        CHECK(FileDataStatIsLink(-1,&unavailable)==False);
    } else if(!strcmp(name,"stat-link")) {
        struct stat link_data;memset(&link_data,0,sizeof(link_data));link_data.st_mode=S_IFLNK;
        CHECK(FileDataStatIsLink(0,&link_data)==True);
    } else if(!strcmp(name,"decode-valid")) {
        CHECK(FileData2Decode(&one,(size_t)one_len,&decoded,&used)==0);
        CHECK(used==(size_t)one_len&&!strcmp(decoded->file_name,"alpha")&&!strcmp(decoded->logical_type,"TYPE"));
        CHECK(decoded->physical_type==7&&decoded->errnum==11&&decoded->is_subdir==True);free_list(decoded);
    } else if(!strcmp(name,"decode-negative")) {
        one.file_name=-1;errno=0;CHECK(FileData2Decode(&one,sizeof(one),&decoded,&used)==-1&&errno==EPROTO&&decoded==NULL);
    } else if(!strcmp(name,"decode-short-header")) {
        errno=0;CHECK(FileData2Decode(&one,offsetof(FileData2,text)-1,&decoded,&used)==-1&&errno==EPROTO);
    } else if(!strcmp(name,"decode-short-text")) {
        errno=0;CHECK(FileData2Decode(&one,(size_t)one_len-1,&decoded,&used)==-1&&errno==EPROTO);
    } else {
        memcpy(packet,&one,(size_t)one_len);memcpy(packet+one_len,&two,(size_t)two_len);
        if(!strcmp(name,"packet-valid")) {
            CHECK(FileData2DecodePacket(packet,(size_t)(one_len+two_len),2,&decoded)==0);
            CHECK(decoded&&decoded->next&&!strcmp(decoded->next->file_name,"beta"));free_list(decoded);
        } else if(!strcmp(name,"packet-zero-count")) {
            errno=0;CHECK(FileData2DecodePacket(packet,(size_t)one_len,0,&decoded)==-1&&errno==EPROTO);
        } else if(!strcmp(name,"packet-large-count")) {
            errno=0;CHECK(FileData2DecodePacket(packet,(size_t)one_len,FILEDATABUF+1,&decoded)==-1&&errno==EPROTO);
        } else if(!strcmp(name,"packet-extra-byte")) {
            packet[one_len]=0;errno=0;CHECK(FileData2DecodePacket(packet,(size_t)one_len+1,1,&decoded)==-1&&errno==EPROTO);
        } else if(!strcmp(name,"packet-short-second")) {
            freed_records=0;errno=0;CHECK(FileData2DecodePacket(packet,(size_t)(one_len+two_len-1),2,&decoded)==-1&&errno==EPROTO);
            CHECK(decoded==NULL&&freed_records==1);
        } else return 2;
    }
    if(!failed)printf("PASS %s\n",name);
    return failed;
}
