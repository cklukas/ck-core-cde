#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef int Boolean;
#define True 1
#define False 0
#define NILL '\0'
#define FILEDATABUF 50

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

static void FreeFileData(FileData *data, Boolean all)
{
    if(!data)return;
    free(data->file_name);free(data->action_name);free(data->logical_type);
    free(data->link);free(data->final_link);
    if(all){freed_records++;free(data);}
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

static void free_list(FileData *head)
{
    while(head){FileData *next=head->next;FreeFileData(head,True);head=next;}
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
    if(!strcmp(name,"pack-valid")) {
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
