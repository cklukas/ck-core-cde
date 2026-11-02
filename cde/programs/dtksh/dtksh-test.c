#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef DTKSH_TEST_BUILDDIR
#error DTKSH_TEST_BUILDDIR must name the dtksh build directory
#endif

int
main(int argc, char **argv)
{
    char executable[sizeof(DTKSH_TEST_BUILDDIR) + sizeof("/.libs/dtksh")];
    char libraries[sizeof(DTKSH_TEST_BUILDDIR) * 6 + 512];
    const char *inherited;

    (void)argc;
    snprintf(executable, sizeof(executable), "%s/.libs/dtksh",
             DTKSH_TEST_BUILDDIR);
    inherited = getenv("LD_LIBRARY_PATH");
    snprintf(libraries, sizeof(libraries),
             "%s/../../lib/DtPrint/.libs:%s/../../lib/DtHelp/.libs:"
             "%s/../../lib/DtWidget/.libs:%s/../../lib/DtSvc/.libs:"
             "%s/../../lib/tt/lib/.libs:%s/../../lib/DtXinerama/.libs%s%s",
             DTKSH_TEST_BUILDDIR, DTKSH_TEST_BUILDDIR,
             DTKSH_TEST_BUILDDIR, DTKSH_TEST_BUILDDIR,
             DTKSH_TEST_BUILDDIR, DTKSH_TEST_BUILDDIR,
             inherited ? ":" : "", inherited ? inherited : "");
    if (setenv("LD_LIBRARY_PATH", libraries, 1) < 0) {
        perror("setenv LD_LIBRARY_PATH");
        return 127;
    }
    execv(executable, argv);
    perror("exec dtksh");
    return 127;
}
