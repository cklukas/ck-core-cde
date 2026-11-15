/* Harness for check-access-ids.py: CheckAccess() of dtfile.  The production
 * function is pasted between the markers below. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* every change of the identity of the process is counted */
static int id_changes;

static int stub_setuid(uid_t u) { (void) u; id_changes++; return 0; }
static int stub_setgid(gid_t g) { (void) g; id_changes++; return 0; }
static int stub_setreuid(uid_t r, uid_t e) { (void) r; (void) e; id_changes++; return 0; }
static int stub_setregid(gid_t r, gid_t e) { (void) r; (void) e; id_changes++; return 0; }
static int stub_seteuid(uid_t u) { (void) u; id_changes++; return 0; }
static int stub_setegid(gid_t g) { (void) g; id_changes++; return 0; }

#define setuid stub_setuid
#define setgid stub_setgid
#define setreuid stub_setreuid
#define setregid stub_setregid
#define seteuid stub_seteuid
#define setegid stub_setegid
/* @@BODY@@ */

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s (identity changes: %d)\n", what, id_changes);
    exit(1);
}

int main(int argc, char **argv)
{
    char file[1024], missing[1024];
    FILE *fp;

    if (argc != 2)
        return 2;
    snprintf(file, sizeof file, "%s/access-ids-file", argv[1]);
    snprintf(missing, sizeof missing, "%s/access-ids-missing", argv[1]);
    unlink(missing);
    fp = fopen(file, "w");
    if (fp == nullptr)
        return 2;
    fclose(fp);
    chmod(file, 0644);

    if (CheckAccess(file, R_OK) != 0)
        fail("a readable file is refused");
    if (CheckAccess(file, X_OK) == 0)
        fail("a file without execute permission is accepted");
    errno = 0;
    if (CheckAccess(missing, F_OK) != -1 || errno != ENOENT)
        fail("a missing file is found");
    /* The check is made with the effective IDs without touching the IDs of
     * the process: a change that fails halfway, or cannot be undone, would
     * leave a program with the wrong identity. */
    if (id_changes != 0)
        fail("CheckAccess changed the identity of the process");
    unlink(file);
    puts("ok");
    return 0;
}
