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
    static const char executable[] = DTKSH_TEST_BUILDDIR "/.libs/dtksh";
    static const char paths[] =
        DTKSH_TEST_BUILDDIR "/ksh-work/lib:"
        DTKSH_TEST_BUILDDIR "/../../lib/DtPrint/.libs:"
        DTKSH_TEST_BUILDDIR "/../../lib/DtHelp/.libs:"
        DTKSH_TEST_BUILDDIR "/../../lib/DtWidget/.libs:"
        DTKSH_TEST_BUILDDIR "/../../lib/DtSvc/.libs:"
        DTKSH_TEST_BUILDDIR "/../../lib/tt/lib/.libs:"
        DTKSH_TEST_BUILDDIR "/../../lib/DtXinerama/.libs";
    const char *inherited = getenv("LD_LIBRARY_PATH");
    const size_t length = sizeof(paths) - 1;

    (void)argc;
    if (!inherited || strncmp(inherited, paths, length) != 0 ||
        (inherited[length] && inherited[length] != ':')) {
        size_t capacity = sizeof(paths) + (inherited ? strlen(inherited) + 1 : 0);
        char *libraries = malloc(capacity);
        if (!libraries) {
            perror("allocate dtksh library path");
            return 127;
        }
        snprintf(libraries, capacity, "%s%s%s", paths,
                 inherited && *inherited ? ":" : "", inherited ? inherited : "");
        if (setenv("LD_LIBRARY_PATH", libraries, 1) < 0) {
            perror("setenv LD_LIBRARY_PATH");
            return 127;
        }
        free(libraries);
    }
    execv(executable, argv);
    perror("exec dtksh");
    return 127;
}
