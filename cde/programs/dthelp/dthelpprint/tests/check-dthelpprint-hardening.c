#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

typedef char Boolean;
#ifndef True
#define True 1
#endif
#ifndef False
#define False 0
#endif

#define EOS '\0'
#define DIR_SLASH '/'
#define TMP_DIR_STR "/.dt/tmp"
#define EMPTY_STR ""
#define INSET 5
#define MAXSECTS 64
#define MAX_COMMAND_LENGTH 5120

#define _DTGETMESSAGE(set, num, def) (def)

typedef struct _TocEntry {
    char *locationId;
    int sectNums[MAXSECTS];
    char *sectStr;
    int pageNumber;
    struct _TocEntry *nextEntry;
    struct _TocEntry *leftLoc;
    struct _TocEntry *rightLoc;
} TocEntry;

typedef struct _Toc {
    TocEntry *linearEntries;
    TocEntry *lastLinearEntry;
    TocEntry *sortedEntries;
} Toc;

static char g_test_home[MAXPATHLEN + 1];

static void
GetHomeDir(char *dest)
{
    snprintf(dest, MAXPATHLEN, "%s", g_test_home);
}

static int
_DtHelpCeStrCaseCmp(const char *s1, const char *s2)
{
    return strcasecmp(s1, s2);
}

static int
TocFindSortedEntry(Toc *toc, char *locationId, TocEntry **ret_parent, TocEntry **ret_entry)
{
    *ret_parent = NULL;
    *ret_entry = NULL;
    TocEntry *cur = toc->sortedEntries;
    while (cur != NULL)
    {
        int cmp = _DtHelpCeStrCaseCmp(locationId, cur->locationId);
        if (cmp == 0)
        {
            *ret_entry = cur;
            return 0;
        }
        *ret_parent = cur;
        cur = (cmp < 0) ? cur->leftLoc : cur->rightLoc;
    }
    return -1;
}

#include "dthelpprint_extracted.inc"

static void
test_create_tmp_file_symlink_safety(const char *scratch_dir)
{
    snprintf(g_test_home, sizeof(g_test_home), "%s/home_dthelpprint", scratch_dir);
    mkdir(g_test_home, 0700);

    char dt_dir[MAXPATHLEN + 1];
    char dt_tmp_dir[MAXPATHLEN + 1];
    snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", g_test_home);
    snprintf(dt_tmp_dir, sizeof(dt_tmp_dir), "%s/.dt/tmp", g_test_home);
    mkdir(dt_dir, 0700);
    mkdir(dt_tmp_dir, 0700);

    /* Plant a symlink at the first candidate filename phlp<pid>_0.txt */
    char victim[MAXPATHLEN + 1];
    char planted_link[MAXPATHLEN + 1];
    snprintf(victim, sizeof(victim), "%s/victim.txt", scratch_dir);
    snprintf(planted_link, sizeof(planted_link), "%s/phlp%d_0.txt", dt_tmp_dir, (int)getpid());
    unlink(victim);
    unlink(planted_link);
    assert(symlink(victim, planted_link) == 0);

    char *created = _DtHPrCreateTmpFile("phlp", ".txt");
    assert(created != NULL);
    /* Must NOT return the planted symlink path! */
    assert(strcmp(created, planted_link) != 0);

    struct stat st;
    assert(lstat(created, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 0777) == 0600);

    unlink(created);
    free(created);
    unlink(planted_link);
    rmdir(dt_tmp_dir);
    rmdir(dt_dir);
    rmdir(g_test_home);
}

static void
test_sect_num_str_bounds(void)
{
    int sectNums[MAXSECTS];
    memset(sectNums, 0, sizeof(sectNums));
    sectNums[0] = 1;
    /* Large section numbers (> 999) previously overflowed char partial[5] */
    for (int i = 1; i < 40; ++i)
        sectNums[i] = 100000 + i;

    char buf[MAXSECTS * 4 + 5];
    memset(buf, 'X', sizeof(buf));
    char *res = SectNumStr(sectNums, NULL, buf);
    assert(res == buf);
    assert(strlen(buf) <= MAXSECTS * 4 + 4);
}

static void
test_put_opt_bounds(void)
{
    char cmd[MAX_COMMAND_LENGTH];
    memset(cmd, 'A', MAX_COMMAND_LENGTH - 10);
    cmd[MAX_COMMAND_LENGTH - 10] = '\0';

    PutOpt(cmd, "-u", "very_long_user_filename_that_exceeds_remaining_space", True);
    assert(strlen(cmd) < MAX_COMMAND_LENGTH);
}

int
main(int argc, char **argv)
{
    const char *scratch_dir = (argc > 1) ? argv[1] : ".";
    test_create_tmp_file_symlink_safety(scratch_dir);
    test_sect_num_str_bounds();
    test_put_opt_bounds();
    printf("check-dthelpprint-hardening: all tests passed\n");
    return 0;
}
