/* Harness for check-pretty-dir.py: format_dir_name_for_user() must never
 * write past the end of the caller's buffer, also for a directory that is
 * relative to HOME and fills the buffer. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#define nullptr ((void *)0)
typedef char *STRING;
#define MAXPATHLEN 4096
#define util_streq(s1,s2) (strcmp((s1),(s2)) == 0)
#define util_dprintf(...) ((void)0)
static STRING	home_env_var_name = "HOME";
static const char *relative_result;
static int util_cvt_path_to_relative(const char *path, const char *rel_to,
				     char *out, int size)
{ (void) path; (void) rel_to; snprintf(out, size, "%s", relative_result); return 0; }
/* @@BODY@@ */
int main(void)
{
    enum { SIZE = 16 };
    char *pretty = malloc(SIZE);          /* exact size: ASan sees a 1 byte overrun */
    assert(pretty != NULL);

    setenv("HOME", "/home/u", 1);
    relative_result = "abcdefghijklmnopqrstuvwxyz";     /* longer than the buffer */
    format_dir_name_for_user("/home/u/abc", pretty, SIZE);
    assert(strlen(pretty) == SIZE - 1);
    assert(strncmp(pretty, "~/abcdefghijkl", 14) == 0);

    relative_result = "src/dir";
    format_dir_name_for_user("/home/u/src/dir", pretty, SIZE);
    assert(strcmp(pretty, "~/src/dir") == 0);

    relative_result = ".";
    format_dir_name_for_user("/home/u", pretty, SIZE);
    assert(strcmp(pretty, "~") == 0);
    free(pretty);
    puts("pretty dir ok");
    return 0;
}
