/* Actual standalone XDG reader plus the extracted production Xt wrapper.
 * Stream read errors use real stdio callbacks; close/allocation faults are
 * test-only reported failures, not native-device-fault claims.
 * Paths below are expected native bytes, never display-escape decoding.
 * FIRST occurrence wins, including when later duplicates contain bad input.
 * No parser implementation is reproduced here. */
#include <stddef.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "XdgTrashInfo.h"

typedef struct {
    const char *name;
    const char *metadata;
    int valid;
    const char *absolute_path;     /* NULL for base-relative or invalid inputs. */
    const char *relative_to_base;  /* Exact suffix to append to confirmed base. */
    const char *date;
} TrashInfoFixture;

static const TrashInfoFixture trash_info_fixtures[] = {
    {"absolute",
     "[Trash Info]\nPath=/original/item\nDeletionDate=2024-02-29T23:59:59\n",
     1, "/original/item", NULL, "2024-02-29T23:59:59"},
    {"key-format-spaces",
     "[Trash Info]\nPath \t= \t/outside/item\nDeletionDate\t = \t2024-02-29T23:59:59\n",
     1, "/outside/item", NULL, "2024-02-29T23:59:59"},
    {"encoded-leading-space",
     "[Trash Info]\nPath= \t%20%09folder/item\nDeletionDate=2024-02-29T23:59:59\n",
     1, NULL, " \tfolder/item", "2024-02-29T23:59:59"},
    {"normalized-first-duplicates-win",
     "[Trash Info]\nPath \t= /first\nPath=/second\n"
     "DeletionDate \t= 2000-01-01T00:00:00\nDeletionDate=2024-02-29T23:59:59\n",
     1, "/first", NULL, "2000-01-01T00:00:00"},
    {"literal-percent-one-pass",
     "[Trash Info]\nPath=/original/%252F%25name\nDeletionDate=2000-01-01T00:00:00\n",
     1, "/original/%2F%name", NULL, "2000-01-01T00:00:00"},
    {"control-native-bytes",
     "[Trash Info]\nPath=/original/%01%09%0a%0D%5c%7F%C3%A9\nDeletionDate=2024-02-29T23:59:59\n",
     1, "/original/\001\t\n\r\\\177\303\251", NULL, "2024-02-29T23:59:59"},
    {"relative-to-original-base",
     "[Trash Info]\nPath=folder/item%20one\nDeletionDate=2024-02-29T23:59:59\n",
     1, NULL, "folder/item one", "2024-02-29T23:59:59"},
    {"relative-literal-encoded-dots",
     "[Trash Info]\nPath=%252e%252e/item\nDeletionDate=2024-02-29T23:59:59\n",
     1, NULL, "%2e%2e/item", "2024-02-29T23:59:59"},
    {"dot-lookalikes",
     "[Trash Info]\nPath=.../name..part\nDeletionDate=2024-02-29T23:59:59\n",
     1, NULL, ".../name..part", "2024-02-29T23:59:59"},
    {"first-fields-win",
     "[Trash Info]\nPath=/first\nDeletionDate=2000-01-01T00:00:00\n"
     "Path=/second\nDeletionDate=2024-02-29T23:59:59\n",
     1, "/first", NULL, "2000-01-01T00:00:00"},
    {"later-invalid-duplicates-ignored",
     "[Trash Info]\nPath=/first\nDeletionDate=2000-01-01T00:00:00\n"
     "Path=%00\nDeletionDate=\n",
     1, "/first", NULL, "2000-01-01T00:00:00"},
    {"unknown-lines-ignored",
     "[Trash Info]\nUnknown=value\nPath=/original/item\n"
     "Another=ignored\nDeletionDate=2024-02-29T23:59:59\n",
     1, "/original/item", NULL, "2024-02-29T23:59:59"},
    {"crlf", "[Trash Info]\r\nPath=/original/item\r\nDeletionDate=2024-02-29T23:59:59\r\n",
     1, "/original/item", NULL, "2024-02-29T23:59:59"},
    {"complete-final-eof", "[Trash Info]\nPath=/original/item\nDeletionDate=2024-02-29T23:59:59",
     1, "/original/item", NULL, "2024-02-29T23:59:59"},
    {"leap-second", "[Trash Info]\nPath=/original/item\nDeletionDate=2000-02-29T23:59:60\n",
     1, "/original/item", NULL, "2000-02-29T23:59:60"},
    {"invalid-calendar", "[Trash Info]\nPath=/original/item\nDeletionDate=1900-02-29T12:00:00\n",
     0, NULL, NULL, NULL},
    {"invalid-clock", "[Trash Info]\nPath=/original/item\nDeletionDate=2024-02-29T24:00:00\n",
     0, NULL, NULL, NULL},
    {"invalid-date-format", "[Trash Info]\nPath=/original/item\nDeletionDate=2024-02-29 12:00:00\n",
     0, NULL, NULL, NULL},
    {"missing-header",
     "Path=/original/item\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"header-not-first",
     "Unknown=value\n[Trash Info]\nPath=/original/item\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"missing-path",
     "[Trash Info]\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"missing-date", "[Trash Info]\nPath=/original/item\n",
     0, NULL, NULL, NULL},
    {"empty-path",
     "[Trash Info]\nPath=\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"empty-date", "[Trash Info]\nPath=/original/item\nDeletionDate=\n",
     0, NULL, NULL, NULL},
    {"encoded-nul",
     "[Trash Info]\nPath=/original/%00suffix\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"invalid-first-path-not-rescued",
     "[Trash Info]\nPath=/original/%00suffix\nPath=/later\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"invalid-first-date-not-rescued",
     "[Trash Info]\nPath=/original/item\nDeletionDate=\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"invalid-percent",
     "[Trash Info]\nPath=/original/%GG\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"trailing-percent",
     "[Trash Info]\nPath=/original/%\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"half-percent",
     "[Trash Info]\nPath=/original/%2\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"relative-parent-component",
     "[Trash Info]\nPath=folder/../escape\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL},
    {"relative-encoded-parent",
     "[Trash Info]\nPath=folder/%2E%2e/escape\nDeletionDate=2024-02-29T23:59:59\n",
     0, NULL, NULL, NULL}
};


typedef int Boolean;
#define True 1
#define False 0
static const char *case_name;
static int failed, core_fail_at, core_allocations, core_fault_hits;
static int wrapper_read_error, wrapper_close_error, wrapper_closes, cookie_closes;
static int wrapper_fstat_error, wrapper_fdopen_error, descriptor_fault_hits;
static int wrapper_raw_closes, wrapper_stream_closes;
static int xt_fail_at, xt_calls, xt_fault_hits, copy_before_close;
static void *xt_owned[32];

#define CHECK(test) do { if (!(test)) { fprintf(stderr,"FAIL %s:%d: %s\n",case_name,__LINE__,#test);failed=1; } } while (0)
static void fixture(int ok) { if (!ok) { perror("trash-info fixture");exit(2); } }
/* Allocation-only seams around the entire actual standalone module. */
void *TrashInfoTestMalloc(size_t size)
{
    if (++core_allocations==core_fail_at) { ++core_fault_hits;errno=ENOMEM;return NULL; }
    return malloc(size);
}
char *TrashInfoTestStrdup(const char *text)
{
    if (++core_allocations==core_fail_at) { ++core_fault_hits;errno=ENOMEM;return NULL; }
    return strdup(text);
}
static void *XtAllocate(size_t size)
{
    void *p=malloc(size);fixture(p!=NULL);
    for (size_t i=0;i<32;++i) if (!xt_owned[i]) { xt_owned[i]=p;return p; }
    fixture(0);return NULL;
}
static char *XtCopy(const char *text)
{
    ++xt_calls;
    if (!wrapper_closes) ++copy_before_close;
    if (xt_calls==xt_fail_at) { ++xt_fault_hits;errno=ENOMEM;return NULL; }
    char *copy=XtAllocate(strlen(text)+1);strcpy(copy,text);return copy;
}
static void XtRelease(void *p)
{
    if (!p) return;
    for (size_t i=0;i<32;++i) if (xt_owned[i]==p) { xt_owned[i]=NULL;free(p);return; }
    CHECK(0); /* The wrapper must never free unowned/static data. */
}
typedef struct { int fd; size_t remaining; } ErrorCookie;
static ssize_t cookie_read(void *data,char *out,size_t size)
{
    ErrorCookie *cookie=data;
    if (!cookie->remaining) { errno=EIO;return -1; }
    if (size>cookie->remaining) size=cookie->remaining;
    ssize_t n=read(cookie->fd,out,size);
    if (n>0) cookie->remaining-=(size_t)n;
    return n;
}
static int cookie_close(void *data)
{
    ErrorCookie *cookie=data;int rc=close(cookie->fd);free(cookie);++cookie_closes;return rc;
}
#ifdef __APPLE__
static int apple_cookie_read(void *data,char *out,int size)
{ return (int)cookie_read(data,out,(size_t)size); }
#endif
static FILE *error_stream_fd(int fd)
{
    struct stat st;fixture(fstat(fd,&st)==0 && st.st_size>=0);
    ErrorCookie *cookie=malloc(sizeof(*cookie));fixture(cookie!=NULL);
    cookie->fd=fd;cookie->remaining=(size_t)st.st_size;
#ifdef __APPLE__
    FILE *stream=funopen(cookie,apple_cookie_read,NULL,NULL,cookie_close);
#else
    cookie_io_functions_t io={0};io.read=cookie_read;io.close=cookie_close;
    FILE *stream=fopencookie(cookie,"r",io);
#endif
    fixture(stream!=NULL);return stream;
}
static FILE *error_stream(const char *name)
{
    int fd=open(name,O_RDONLY);fixture(fd>=0);return error_stream_fd(fd);
}
static FILE *OpenForWrapper(const char *path,const char *mode)
{ return wrapper_read_error?error_stream(path):fopen(path,mode); }
static int OpenDescriptor(const char *path,int flags)
{
    CHECK((flags&O_ACCMODE)==O_RDONLY && (flags&O_NONBLOCK) && (flags&O_NOFOLLOW) && (flags&O_CLOEXEC));
    return open(path,flags);
}
static int StatDescriptor(int fd,struct stat *info)
{
    if (wrapper_fstat_error) { ++descriptor_fault_hits;errno=EIO;return -1; }
    return fstat(fd,info);
}
static FILE *StreamDescriptor(int fd,const char *mode)
{
    if (wrapper_fdopen_error) { ++descriptor_fault_hits;errno=ENOMEM;return NULL; }
    return wrapper_read_error?error_stream_fd(fd):fdopen(fd,mode);
}
static int CloseDescriptor(int fd)
{ ++wrapper_closes;++wrapper_raw_closes;return close(fd); }
static int CloseForWrapper(FILE *stream)
{
    int rc=fclose(stream);++wrapper_closes;++wrapper_stream_closes;
    if (wrapper_close_error) { errno=EIO;return EOF; }
    return rc;
}
#define open OpenDescriptor
#define fstat StatDescriptor
#define fdopen StreamDescriptor
#define close CloseDescriptor
#define fopen OpenForWrapper
#define fclose CloseForWrapper
#define XtMalloc XtAllocate
#define XtNewString XtCopy
#define XtFree XtRelease
#include "trash-info-wrapper.inc"
#undef open
#undef fstat
#undef fdopen
#undef close
#undef fopen
#undef fclose
#undef XtMalloc
#undef XtNewString
#undef XtFree

static const char *valid_info="[Trash Info]\nPath=/original/item\nDeletionDate=2024-02-29T23:59:59\n";
static void put_bytes(FILE *fp,const char *text,size_t size)
{ fixture(fwrite(text,1,size,fp)==size); }
static void put_text(FILE *fp,const char *text) { put_bytes(fp,text,strlen(text)); }

static void run_record(const char *name,int wrapper)
{
    const TrashInfoFixture *record=NULL;
    for (size_t i=0;i<sizeof(trash_info_fixtures)/sizeof(trash_info_fixtures[0]);++i)
        if (!strcmp(trash_info_fixtures[i].name,name)) record=&trash_info_fixtures[i];
    const char *base="/original-base",*expected="/original/item",*date="2024-02-29T23:59:59";
    char *owned_expected=NULL,*owned_base=NULL;
    int status=DTFILE_XDG_INFO_OK;
    FILE *fp=fopen("input.trashinfo","wb");fixture(fp!=NULL);
    if (record) {
        put_text(fp,record->metadata);
        status=record->valid?DTFILE_XDG_INFO_OK:DTFILE_XDG_INFO_INVALID;
        expected=record->absolute_path;date=record->date;
        if (record->relative_to_base) {
            size_t n=strlen(base)+strlen(record->relative_to_base)+2;
            owned_expected=malloc(n);fixture(owned_expected!=NULL);
            snprintf(owned_expected,n,"%s/%s",base,record->relative_to_base);expected=owned_expected;
        }
    } else if (!strcmp(name,"line-limit") || !strcmp(name,"overlong-line") ||
               !strcmp(name,"overlong-unterminated")) {
        put_text(fp,"[Trash Info]\n");
        size_t length=DTFILE_XDG_INFO_LINE_LIMIT+(!strcmp(name,"line-limit")?0:1);
        for (size_t i=0;i<length;++i) fixture(fputc('x',fp)!=EOF);
        if (strcmp(name,"overlong-unterminated")) {
            put_text(fp,"\nPath=/original/item\nDeletionDate=2024-02-29T23:59:59\n");
        }
        status=!strcmp(name,"line-limit")?DTFILE_XDG_INFO_OK:DTFILE_XDG_INFO_TOO_LARGE;
    } else if (!strcmp(name,"file-limit") || !strcmp(name,"overlong-file")) {
        put_text(fp,valid_info);
        size_t total=DTFILE_XDG_INFO_FILE_LIMIT+(!strcmp(name,"overlong-file")?1:0);
        for (size_t i=strlen(valid_info);i<total;++i) fixture(fputc(i%1000==999?'\n':'x',fp)!=EOF);
        status=!strcmp(name,"file-limit")?DTFILE_XDG_INFO_OK:DTFILE_XDG_INFO_TOO_LARGE;
    } else if (!strcmp(name,"unknown-fragment-spoof")) {
        put_text(fp,"[Trash Info]\n");
        for (size_t i=0;i<4095;++i) fixture(fputc('x',fp)!=EOF);
        put_text(fp,"Path=/forged\nDeletionDate=2024-02-29T23:59:59\n");
        status=DTFILE_XDG_INFO_INVALID;
    } else if (!strcmp(name,"file-nul")) {
        static const char raw[]="[Trash Info]\nPath=/original/item\0hidden\nDeletionDate=2024-02-29T23:59:59\n";
        put_bytes(fp,raw,sizeof(raw)-1);status=DTFILE_XDG_INFO_INVALID;
    } else if (!strcmp(name,"resolved-limit") || !strcmp(name,"resolved-too-large")) {
        size_t length=DTFILE_XDG_INFO_LINE_LIMIT-(!strcmp(name,"resolved-limit")?2:1);
        owned_base=malloc(length+1);fixture(owned_base!=NULL);
        memset(owned_base,'x',length);owned_base[0]='/';owned_base[length]=0;base=owned_base;
        put_text(fp,"[Trash Info]\nPath=r\nDeletionDate=2024-02-29T23:59:59\n");
        status=!strcmp(name,"resolved-limit")?DTFILE_XDG_INFO_OK:DTFILE_XDG_INFO_TOO_LARGE;
        if (status==DTFILE_XDG_INFO_OK) {
            owned_expected=malloc(length+3);fixture(owned_expected!=NULL);
            memcpy(owned_expected,base,length);memcpy(owned_expected+length,"/r",3);expected=owned_expected;
        }
    } else if (!strcmp(name,"relative-no-base") || !strncmp(name,"no-memory-",10)) {
        put_text(fp,"[Trash Info]\nPath=folder/item\nDeletionDate=2024-02-29T23:59:59\n");
        if (!strcmp(name,"relative-no-base")) { base=NULL;status=DTFILE_XDG_INFO_INVALID; }
        else {
            int at=atoi(name+10);fixture(at>=1 && at<=4);
            if (wrapper) xt_fail_at=at;else core_fail_at=at;
            status=DTFILE_XDG_INFO_NO_MEMORY;
        }
    } else {
        fixture(!strcmp(name,"read-error") || !strcmp(name,"close-error") ||
                !strcmp(name,"missing-file") || !strcmp(name,"absolute-no-base") ||
                !strcmp(name,"fifo-input") || !strcmp(name,"symlink-input") ||
                !strcmp(name,"fstat-error") || !strcmp(name,"fdopen-error"));
        put_text(fp,valid_info);
        if (!strcmp(name,"read-error")) { wrapper_read_error=1;status=DTFILE_XDG_INFO_IO_ERROR; }
        if (!strcmp(name,"close-error")) { wrapper_close_error=1;status=DTFILE_XDG_INFO_IO_ERROR; }
        if (!strcmp(name,"missing-file")) status=DTFILE_XDG_INFO_IO_ERROR;
        if (!strcmp(name,"absolute-no-base")) base=NULL;
        if (!strcmp(name,"fifo-input") || !strcmp(name,"symlink-input")) status=DTFILE_XDG_INFO_INVALID;
        if (!strcmp(name,"fstat-error")) { wrapper_fstat_error=1;status=DTFILE_XDG_INFO_IO_ERROR; }
        if (!strcmp(name,"fdopen-error")) { wrapper_fdopen_error=1;status=DTFILE_XDG_INFO_NO_MEMORY; }
    }
    fixture(fclose(fp)==0);
    if (wrapper) {
        const char *filename=!strcmp(name,"missing-file")?"missing.trashinfo":"input.trashinfo";
        if (!strcmp(name,"fifo-input")) { fixture(mkfifo("fifo.trashinfo",0600)==0);filename="fifo.trashinfo"; }
        if (!strcmp(name,"symlink-input")) { fixture(symlink("input.trashinfo","link.trashinfo")==0);filename="link.trashinfo"; }
        char *path=NULL,*deletion=NULL;
        int ok=TestParseTrashInfo(filename,base,&path,&deletion);
        fprintf(stderr,"WRAPPER: ok=%d expected_status=%d closes=%d copies_before_close=%d Xt_calls=%d\n",
                ok,status,wrapper_closes,copy_before_close,xt_calls);
        CHECK(!!ok==(status==DTFILE_XDG_INFO_OK));
        if (status==DTFILE_XDG_INFO_OK) {
            CHECK(path && expected && !strcmp(path,expected));
            CHECK(deletion && date && !strcmp(deletion,date));
        } else CHECK(!path && !deletion);
        CHECK(wrapper_closes==(!strcmp(name,"missing-file") || !strcmp(name,"symlink-input")?0:1));
        if (wrapper_fstat_error || wrapper_fdopen_error) {
            CHECK(descriptor_fault_hits==1 && wrapper_raw_closes==1 && wrapper_stream_closes==0);
        }
        if (!strcmp(name,"fifo-input")) {
            struct stat info;CHECK(lstat(filename,&info)==0 && S_ISFIFO(info.st_mode));
            CHECK(wrapper_raw_closes==1 && wrapper_stream_closes==0);
        }
        if (!strcmp(name,"symlink-input")) {
            struct stat info;char link[64];ssize_t n=readlink(filename,link,sizeof(link));
            CHECK(lstat(filename,&info)==0 && S_ISLNK(info.st_mode));
            CHECK(n==(ssize_t)strlen("input.trashinfo") && !memcmp(link,"input.trashinfo",(size_t)n));
        }
        if (!strcmp(name,"fifo-input") || !strcmp(name,"symlink-input")) {
            char bytes[128];FILE *verify=fopen("input.trashinfo","rb");fixture(verify!=NULL);
            size_t n=fread(bytes,1,sizeof(bytes),verify);
            CHECK(n==strlen(valid_info) && !memcmp(bytes,valid_info,n));
            fixture(fclose(verify)==0);
        }
        if (xt_fail_at) CHECK(xt_fault_hits==1);
        XtRelease(path);XtRelease(deletion);
        for (size_t i=0;i<32;++i) {
            CHECK(xt_owned[i]==NULL);
            if (xt_owned[i]) { free(xt_owned[i]);xt_owned[i]=NULL; }
        }
    } else {
        fp=wrapper_read_error?error_stream("input.trashinfo"):fopen("input.trashinfo","rb");
        fixture(fp!=NULL);int fd=wrapper_read_error?-1:fileno(fp);
        DtfileXdgTrashInfo result={0};
        int actual=DtfileReadXdgTrashInfo(fp,base,&result);
        fprintf(stderr,"CORE: status=%d expected=%d allocations=%d injected=%d\n",
                actual,status,core_allocations,core_fault_hits);
        CHECK(actual==status);
        if (status==DTFILE_XDG_INFO_OK) {
            CHECK(result.path && expected && !strcmp(result.path,expected));
            CHECK(result.deletion_date && date && !strcmp(result.deletion_date,date));
        } else CHECK(!result.path && !result.deletion_date);
        if (core_fail_at) CHECK(core_fault_hits==1);
        CHECK(cookie_closes==0);
        if (fd>=0) CHECK(fcntl(fd,F_GETFD)>=0);
        if (fd<0 || fcntl(fd,F_GETFD)>=0) fixture(fclose(fp)==0);
        DtfileFreeXdgTrashInfo(&result);
        CHECK(!result.path && !result.deletion_date);
    }
    free(owned_expected);free(owned_base);
}
int main(int argc,char **argv)
{
    fixture(argc==3);case_name=argv[1];fixture(chdir(argv[2])==0);
    if (!strcmp(case_name,"core-null-stream")) {
        DtfileXdgTrashInfo result={0};
        CHECK(DtfileReadXdgTrashInfo(NULL,"/base",&result)==DTFILE_XDG_INFO_INVALID);
        CHECK(!result.path && !result.deletion_date);DtfileFreeXdgTrashInfo(&result);
    } else if (!strcmp(case_name,"core-null-result")) {
        FILE *fp=fopen("null-result.info","w+");fixture(fp!=NULL);put_text(fp,valid_info);rewind(fp);
        int fd=fileno(fp);
        CHECK(DtfileReadXdgTrashInfo(fp,"/base",NULL)==DTFILE_XDG_INFO_INVALID);
        CHECK(fcntl(fd,F_GETFD)>=0);fixture(fclose(fp)==0);
    } else if (!strcmp(case_name,"core-free-null")) DtfileFreeXdgTrashInfo(NULL);
    else if (!strncmp(case_name,"core-",5)) run_record(case_name+5,0);
    else { fixture(!strncmp(case_name,"wrapper-",8));run_record(case_name+8,1); }
    if (!failed) printf("PASS %s\n",case_name);
    return failed?1:0;
}
