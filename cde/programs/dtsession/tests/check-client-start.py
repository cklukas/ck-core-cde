#!/usr/bin/env python3
"""Check how dtsession starts the clients of a session.

A restored client is started in a child process that adds the environment saved
with it, changes to its directory and execs.  The child used to be made with
vfork(), which shares the memory of dtsession with it until the exec: what it
did before the exec (putenv of the saved variables, malloc, a log message)
changed the environment of dtsession itself and of every client started after
it, and a failed exec ended in SM_EXIT, which returns while the session is
being saved, so the child went on as a second dtsession.  The production
StartLocalClient and ForkWM of SmRestore.c are cut out of the source and run
for real, under AddressSanitizer and UndefinedBehaviorSanitizer, with stand-ins
for the toolkit.  Every scenario runs in its own process.

DTSESSION_SOURCE names another copy of the sources to test, for example an
older revision.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
text = (source / "SmRestore.c").read_text()


def function(pattern):
    match = re.search(pattern, text, re.M)
    assert match, pattern
    return text[match.start():text.index("\n}", match.start()) + 3] + "\n"


code = function(r"^static Boolean *\nStartLocalClient \(") + function(r"^static void *\nForkWM\( void \)")

harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int Boolean;
#define True 1
#define False 0
#define DtError 1
#define DT_ENV_RESTORE_PRE_DT 1
#define GETMESSAGE(set, n, s) (s)
#define CDE_INSTALLATION_TOP "/nonexistent"

static struct { Boolean ExitComplete; } smGD;
static struct { char *cantForkClientString; } smNLS = { "fork" };
static char **ignoreEnvPtr;
static char tmpExecWmFile[MAXPATHLEN + 1];
static Boolean localWmLaunched;
static int logged;
static void PrintErrnoError(int type, const char *m) { (void) type; (void) m; logged++; }
static void SetSIGPIPEToDefault(void) { }
static void _DtEnvControl(int what) { (void) what; }
static void MarkFileDescriptors(int start, int cmd, int data) { int fd; for (fd = start; fd < 64; fd++) fcntl(fd, cmd, data); }
static void LogCWDMessage(char *bad, char *app, char *good) { (void) bad; (void) app; (void) good; logged++; }
static char **RemoveEnvironmentVars(char **envp) { return envp; }
/* SmExit() leaves while the session is being saved only when it is allowed to */
static void SmExit(int status) { if (smGD.ExitComplete) _exit(status); }
#define SM_EXIT(status) SmExit(status)

''' + code + r'''
static char *work;
static pid_t test_pid;
#define CHECK_STARTED(s) do { if (!(s)) _exit(3); } while (0)

static int run_client(char **argv, char **envp)
{
    int status;
    pid_t child;
    char *program = argv[0];
    int started = StartLocalClient(program, argv, NULL, envp, False, True);
    if (getpid() != test_pid) { /* a child that came back from a failed exec */
        char marker[PATH_MAX];
        snprintf(marker, sizeof marker, "%s/second-dtsession", work);
        close(open(marker, O_CREAT | O_WRONLY, 0600));
        _exit(0);
    }
    CHECK_STARTED(started);
    child = wait(&status);
    return child > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int saved_environment_stays_in_the_client(void)
{
    char out[PATH_MAX], content[64] = "";
    char *envp[] = { "SAVED_SESSION_VAR=for-the-client", NULL };
    char script[PATH_MAX * 2];
    char *argv[] = { "sh", "-c", script, NULL };
    FILE *f;
    snprintf(out, sizeof out, "%s/seen", work);
    snprintf(script, sizeof script, "printf %%s \"$SAVED_SESSION_VAR\" > '%s'", out);
    unsetenv("SAVED_SESSION_VAR");
    CHECK(run_client(argv, envp) == 0);
    f = fopen(out, "r"); CHECK(f); CHECK(fgets(content, sizeof content, f)); fclose(f);
    CHECK(!strcmp(content, "for-the-client"));                    /* the client got it */
    CHECK(getenv("SAVED_SESSION_VAR") == NULL);                   /* dtsession did not */
    return 0;
}
static int next_client_does_not_inherit_it(void)
{
    char *first_env[] = { "SAVED_SESSION_VAR=first", "ANOTHER_SAVED_VAR=first", NULL };
    char *first[] = { "true", NULL };
    char *second[] = { "sh", "-c", "test -z \"$SAVED_SESSION_VAR\" -a -z \"$ANOTHER_SAVED_VAR\"", NULL };
    unsetenv("SAVED_SESSION_VAR"); unsetenv("ANOTHER_SAVED_VAR");
    CHECK(run_client(first, first_env) == 0);
    CHECK(run_client(second, NULL) == 0);                         /* a client without saved environment sees none */
    return 0;
}
static int failed_exec_ends_the_child(void)
{
    char *argv[] = { "/nonexistent/program", NULL };
    char marker[PATH_MAX];
    smGD.ExitComplete = False;                                    /* the session is being saved */
    run_client(argv, NULL);
    snprintf(marker, sizeof marker, "%s/second-dtsession", work);
    CHECK(access(marker, F_OK) != 0);                             /* the child did not return as a second dtsession */
    return 0;
}
static int failed_exec_of_the_window_manager_ends_the_child(void)
{
    int status;
    char marker[PATH_MAX];
    smGD.ExitComplete = False;
    localWmLaunched = True;                                       /* the child used to fall through in this case */
    ForkWM();
    if (getpid() != test_pid) {
        snprintf(marker, sizeof marker, "%s/second-dtsession", work);
        close(open(marker, O_CREAT | O_WRONLY, 0600));
        _exit(0);
    }
    wait(&status);
    snprintf(marker, sizeof marker, "%s/second-dtsession", work);
    CHECK(access(marker, F_OK) != 0);
    return 0;
}
static int log_message_for_a_long_program_name(void)
{
    static char name[9000];
    char *argv[] = { name, NULL };
    memset(name, 'n', sizeof name - 1);
    smGD.ExitComplete = True;
    CHECK(run_client(argv, NULL) != 0);                           /* exec fails, the message fits its buffer */
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"saved_environment_stays_in_the_client", saved_environment_stays_in_the_client},
    {"next_client_does_not_inherit_it", next_client_does_not_inherit_it},
    {"failed_exec_ends_the_child", failed_exec_ends_the_child},
    {"failed_exec_of_the_window_manager_ends_the_child", failed_exec_of_the_window_manager_ends_the_child},
    {"log_message_for_a_long_program_name", log_message_for_a_long_program_name},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    if (argc < 3) { for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    work = argv[2];
    test_pid = getpid();
    smGD.ExitComplete = True;
    alarm(60);
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
with tempfile.TemporaryDirectory(prefix="dtsession-client-", dir=os.environ.get("TMPDIR")) as scratch:
    scratch = Path(scratch)
    c = scratch / "client.c"
    c.write_text(harness)
    binary = scratch / "client"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-w", str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr[:6000])
        sys.exit("cannot compile the client start harness")
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True, env=env).stdout.split()
    for name in names:
        work = scratch / ("work-" + name)
        work.mkdir()
        run = subprocess.run([str(binary), name, str(work)], capture_output=True, text=True, timeout=120, env=env)
        if run.returncode:
            failed.append(name)
            lines = [l for l in run.stderr.strip().splitlines() if l.strip()]
            sys.stderr.write("%s failed: %s\n" % (name, lines[0] if lines else "exit %d (signal %d)" % (run.returncode, -run.returncode)))
if failed:
    sys.exit("%d of %d client start scenarios failed: %s" % (len(failed), len(names), ", ".join(failed)))
print("Client start: %d scenarios passed" % len(names))
