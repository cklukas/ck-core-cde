/* CDE desktop shell entry point. See COPYING for the LGPL license. */
#include "shell.h"
#include <X11/Intrinsic.h>
#include <Dt/DtNlUtils.h>
#include <Dt/EnvControlP.h>
#include "dtksh.h"
#include "dtkcmds.h"

static void
desktop_init(Shell_t *shell, int phase)
{
    (void)shell;
    if (phase == 0) {
        int *locked = LockKshFileDescriptors();
        XtSetLanguageProc(NULL, NULL, NULL);
        DtNlInitialize();
        _DtEnvControl(DT_ENV_SET);
        UnlockKshFileDescriptors(locked);
        dtksh_init();
    }
}

int
main(int argc, char **argv)
{
    sh_main(argc, argv, desktop_init);
}
