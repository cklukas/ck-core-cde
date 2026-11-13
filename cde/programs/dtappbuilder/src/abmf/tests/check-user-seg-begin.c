/* Harness for check-user-seg-begin.py: find_user_seg_begin() must return
 * the offset after the user segment marker, and ERR_EOF when the file has
 * none, instead of ending without a return value. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
typedef FILE *File;
typedef char *STRING;
#define ERR_EOF (-305)
#define util_streq(s1,s2) (strcmp((s1),(s2)) == 0)
static STRING		userSegStartString = "DTB_USER_RES_START";
/* @@BODY@@ */
static long run(const char *content)
{
    FILE *f = tmpfile();
    long r;
    assert(f != NULL);
    fputs(content, f);
    rewind(f);
    r = find_user_seg_begin(f);
    fclose(f);
    return r;
}

int main(void)
{
    const char *marked = "line one\n! DTB_USER_RES_START\nuser text\n";
    assert(run(marked) == (long) (strlen("line one\n! DTB_USER_RES_START\n")));
    assert(run("nothing here\njust text\n") == ERR_EOF);
    assert(run("") == ERR_EOF);
    puts("user segment begin ok");
    return 0;
}
