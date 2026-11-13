/* Harness for check-privilege.py: util/Privilege.h of dtprintinfo against a
 * fake kernel.  The header is pasted between the markers below. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* a program installed set-user-ID root and run by the user 1000 */
static uid_t fake_ruid = 1000, fake_euid = 0;
static int refuse;	/* the kernel refuses the change with EPERM */
static int lie;		/* the kernel reports success without changing anything */

static uid_t stub_getuid(void) { return fake_ruid; }
static uid_t stub_geteuid(void) { return fake_euid; }
static int stub_seteuid(uid_t u)
{
    if (refuse) { errno = EPERM; return -1; }
    if (!lie) fake_euid = u;
    return 0;
}
static int stub_setuid(uid_t u)
{
    if (refuse) { errno = EPERM; return -1; }
    if (!lie) { fake_ruid = fake_euid = u; }
    return 0;
}

#define getuid stub_getuid
#define geteuid stub_geteuid
#define seteuid stub_seteuid
#define setuid stub_setuid
/* @@BODY@@ */

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    exit(1);
}

/* run RestoreUserEuid() in a child; returns its exit status, or -1 if it returned */
static int restore_in_child(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        RestoreUserEuid();
        _exit(77);		/* it returned */
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        fail("child did not exit");
    return WEXITSTATUS(status) == 77 ? -1 : WEXITSTATUS(status);
}

int main(void)
{
    /* taking root and giving it up again */
    if (RaiseToRoot() != 0 || fake_euid != 0)
        fail("RaiseToRoot");
    fake_euid = 0;
    if (restore_in_child() != -1)
        fail("RestoreUserEuid must return when the kernel allows it");
    fake_euid = 0;
    RestoreUserEuid();
    if (fake_euid != 1000)
        fail("RestoreUserEuid did not give up root");

    /* a refused or pretended change leaves root in place: the process must
     * end instead of running on with it */
    fake_euid = 0; refuse = 1; lie = 0;
    if (restore_in_child() != EXIT_FAILURE)
        fail("RestoreUserEuid went on after a refused change");
    fake_euid = 0; refuse = 0; lie = 1;
    if (restore_in_child() != EXIT_FAILURE)
        fail("RestoreUserEuid went on after a change that did not happen");
    refuse = lie = 0;

    /* the identity of a started program */
    fake_ruid = 1000; fake_euid = 0;
    if (BecomeUser(1234) != 0 || fake_ruid != 1234 || fake_euid != 1234)
        fail("BecomeUser");
    fake_ruid = 1000; fake_euid = 0; refuse = 1;
    if (BecomeUser(1234) != -1)
        fail("BecomeUser must report a refused change");
    refuse = 0; lie = 1;
    fake_ruid = 1000; fake_euid = 0;
    if (BecomeUser(1234) != -1)
        fail("BecomeUser must notice a change that did not happen");

    puts("ok");
    return 0;
}
