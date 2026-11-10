#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef int Boolean;
#define NILL '\0'
#define True 1
#define MAXPATHLEN 4096

typedef struct _FileData {
    struct _FileData *next;char *file_name;char *action_name;
    unsigned char physical_type;char *logical_type;int errnum;
    struct stat stat;char *link;char *final_link;Boolean is_subdir;Boolean is_broken;
} FileData;
typedef struct _FileData2 {
    struct _FileData2 *next;short file_name;short action_name;
    unsigned char physical_type;short logical_type;int errnum;
    struct stat stat;short link;short final_link;Boolean is_subdir;Boolean is_broken;
    char text[BUFSIZ];
} FileData2;
static void *test_calloc(size_t n,size_t s){void *p=calloc(n,s);if(!p)abort();return p;}
static char *test_new_string(const char *s){size_t n=strlen(s)+1;char *p=malloc(n);if(!p)abort();memcpy(p,s,n);return p;}
#define XtCalloc test_calloc
#define XtNewString test_new_string

/* PRODUCTION_FUNCTION */

int main(void)
{
    FileData2 record;
    int length=0;
    memset(&record,0,sizeof(record));
    record.file_name=-1;
    record.logical_type=1;
    record.text[0]='T';
    (void)FileData2toFileData(&record,&length);
    fprintf(stderr,"legacy decoder accepted a negative field length\n");
    return 0;
}
