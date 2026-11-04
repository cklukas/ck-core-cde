#!/usr/bin/env python3
"""Check what the screen lock does with a typed password and when it refuses to lock.

The production CheckString(), WipePassword() and LockIsAvailable() of SmLock.c and
the start of LockDisplay() are cut out of the source and compiled, under AddressSanitizer
and UndefinedBehaviorSanitizer, with stand-ins for the password check
(SmPamCheckPassword, SmPamLockStatus), the dialogs and ToolTalk.  Every
scenario runs in its own process.

The display is unlocked by the password check of SmPam.c and by nothing else:
the source guard at the end fails when SmLock.c (or the rest of the program)
names a password database, crypt(), the root account or a list of keyholders,
when the PAM service of the lock lets anyone but the user in, or when the
program is still installed setuid.  The check itself is tested in
check-pam-lock.py.

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
text = (source / "SmLock.c").read_text(errors="replace")

# --- source guard: the only way to unlock is the password of the session's user, through PAM ---
problems = []
sources = sorted(source.glob("*.c")) + sorted(source.glob("*.h"))
code = {p.name: re.sub(r"/\*.*?\*/", "", p.read_text(errors="replace"), flags=re.S) for p in sources}

# 1. no other way to check a password and no other account: not in the lock, not anywhere in the program
forbidden = [r"\bcrypt\s*\(", r"<crypt\.h>", r"<shadow\.h>", r"\bgetspnam\b", r"\bgetpwnam\w*\s*\(",
             r"\bkeyholders\b", r"\bSmNkeys\b", r"\bPasswordMatchesHash\b", r"\bCanReAuthenticate\b",
             r"\bCheckPassword\b", r"\bAuthenticate\s*\(", r"\blocalAuthenticate\b", r"_DtSvcPamAuthenticate",
             r"\bSmPamAvailable\b", r"\btrustedSystemErrorString\b"]
for name, content in code.items():
    for pattern in forbidden:
        if re.search(pattern, content):
            problems.append("%s still has %s" % (name, pattern))

# 2. the user of the PAM conversation comes from the real user id and the service is fixed
pam_c = code.get("SmPam.c", "")
if '#define SM_PAM_SERVICE "dtsession"' not in pam_c:
    problems.append('SmPam.c does not define the fixed PAM service "dtsession"')
if "getpwuid(getuid())" not in pam_c:
    problems.append("SmPam.c does not take the user from getuid()")
for pattern in (r"\bgetenv\b", r"\bgetlogin\b", r"\bgeteuid\b", r"\bprogramName\b", r"\bargv\b"):
    if re.search(pattern, pam_c):
        problems.append("SmPam.c uses %s" % pattern)

# the dialog names the account whose password unlocks the display, not LOGNAME
ui = code.get("SmUI.c", "")
if re.search(r"getenv\s*\(\s*\"(LOGNAME|USER)\"", ui):
    problems.append("SmUI.c names the user in the lock dialog from the environment")
if "SmPamUserName()" not in ui:
    problems.append("SmUI.c does not take the name in the lock dialog from SmPamUserName()")

# 3. the resource that named other users who may unlock is gone from the documentation too
for relative in ("dtsession.man", "../../doc/en_US.UTF-8/guides/man/man1_dt/session.sgm"):
    page = source / relative
    if page.exists() and re.search(r"keyholder|\bDtsession\*keys\b", page.read_text()):
        problems.append("%s still documents keyholders" % relative)

# 4. the PAM service lets in the user it is given and nobody else
stack = []
skipping = 0
in_comment = False
for line in (source / "config" / "dtsession.src").read_text().splitlines():
    line = line.strip()
    if in_comment:
        in_comment = "*/" not in line
        continue
    if line.startswith("/*"):
        in_comment = "*/" not in line
        continue
    if line.startswith("#if"):
        skipping += 1
        continue
    if line.startswith("#endif"):
        skipping -= 1
        continue
    if skipping or not line:
        continue
    stack.append(tuple(line.split()))
expected = [("auth", "required", "pam_unix.so"), ("account", "required", "pam_unix.so"),
            ("session", "required", "pam_deny.so"), ("password", "required", "pam_deny.so")]
if stack != expected:
    problems.append("the PAM service in config/dtsession.src is %r, not %r" % (stack, expected))

# the service file the program looks for is the one the build installs: pamdir of config/Makefile.am
config_text = (source / "config" / "Makefile.am").read_text()
installed = set(re.findall(r"^pamdir\s*=\s*(\S+)", config_text, re.M))
looked_for = set(re.findall(r'SM_PAM_SERVICE_FILE=\\"([^"\\]+)\\"', (source / "Makefile.am").read_text()))
if not installed or {d + "/dtsession" for d in installed} != looked_for:
    problems.append("dtsession looks for %s, config/Makefile.am installs the service in %s" % (sorted(looked_for), sorted(installed)))
main_code = code.get("SmMain.c", "")
if "LockIsAvailable()" not in main_code:
    problems.append("SmMain.c does not check at startup whether the display can be locked")

# 5. dtsession is installed with the identity of the user who runs it, and the service as pam.d/dtsession
makefile = (source / "Makefile.am").read_text()
if re.search(r"install-exec-hook|chmod\s+[0-7]*[4-7][0-7]{3}|chown", makefile):
    problems.append("Makefile.am still installs dtsession setuid")
pam_config = (source / "config" / "Makefile.am").read_text()
if "dist_pam_DATA = $(CPP_TARGET)" not in pam_config or "CPP_TARGET = dtsession" not in pam_config:
    problems.append("config/Makefile.am does not install the service as pam.d/dtsession")

if problems:
    sys.exit("lock source guard failed:\n  " + "\n  ".join(problems))
print("Lock source guard: only the PAM check of the session's user unlocks the display")



def whole(signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 3] + "\n"


def lock_start():
    """LockDisplay() up to the point where it begins to build the dialogs."""
    start = text.index("void \nLockDisplay(")
    stop = text.index("    if(((smDD.lockCoverDialog == NULL)", start)
    body = text[start:stop]
    body = body.replace("void \nLockDisplay(", "static int\nLockDisplayStart(", 1)
    body = re.sub(r"\breturn;", "return 0;", body)
    return body + "    return 1;\n}\n"


harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

typedef unsigned char Boolean;
#define True 1
#define False 0
#define DtError 1

/* the password check: what it was asked and what it answers */
static char seen_password[256];
static int checks, correct_password, unlocked, bells;
static Boolean SmPamCheckPassword(const char *typed)
{
    snprintf(seen_password, sizeof seen_password, "%s", typed);
    checks++;
    return correct_password;
}
typedef enum { SM_PAM_READY, SM_PAM_NOT_BUILT, SM_PAM_NO_SERVICE, SM_PAM_NO_ACCOUNT } SmPamStatus;
static SmPamStatus pam_status = SM_PAM_READY;
static SmPamStatus SmPamLockStatus(void) { return pam_status; }
static const char *service_file = "/etc/pam.d/dtsession";
static const char *SmPamServiceFile(void) { return service_file; }

/* what the typed characters go through before they are compared */
#define SM_REALLOC(p, n) tracked_realloc(p, n)
static char *password_buffer; static size_t password_buffer_size;
static void *tracked_realloc(void *p, size_t n) { password_buffer = realloc(p, n); password_buffer_size = n; return password_buffer; }
static void UpdatePasswdField(int n) { (void) n; }
static void UnlockDisplay(Boolean a, Boolean b) { (void) a; (void) b; unlocked++; }
static void PrintErrnoError(int type, const char *m) { (void) type; (void) m; }
#define XBell(d, n) (bells++)

/* the start of LockDisplay() */
typedef struct tt_message *Tt_message;
typedef void *Widget;
typedef struct { unsigned long pixel; } XColor;
#define TT_NOTICE 1
#define TT_SESSION 1
static struct { int saverTimeout, lockTimeout; } smSaverRes = { 600, 0 };
static struct { int coverScreen; void *display; } smGD;
static struct { char *noPamErrorString, *pamServiceErrorString, *noAccountErrorString, *cantMallocErrorString; } smNLS;
typedef int XtIntervalId;
static XtIntervalId timerId, lockTimeId, lockDelayId, cycleId, flash_id;
static int errors_printed, notices_sent, system_ready;
static char printed[256];
static void PrintError(int type, const char *message) { (void) type; errors_printed++; snprintf(printed, sizeof printed, "%s", message); }
static Tt_message tttk_message_create(int a, int b, int c, int d, const char *op, int e) { (void) a; (void) b; (void) c; (void) d; (void) op; (void) e; return NULL; }
static void tt_message_send(Tt_message m) { (void) m; notices_sent++; }
static void tt_message_destroy(Tt_message m) { (void) m; }
static void SetSystemReady(void) { system_ready++; }

''' + whole("static void\nWipePassword(") + "\n" + whole("Boolean\nLockIsAvailable(") + "\n" + lock_start() + "\n" + whole("static void \nCheckString(") + r'''

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s\n", #condition); return 1; } } while (0)

static int in_memory(const char *needle)
{
    size_t i, n = strlen(needle);
    for (i = 0; password_buffer && i + n <= password_buffer_size; i++)
        if (!memcmp(password_buffer + i, needle, n)) return 1;
    return 0;
}
static void type(const char *characters)
{
    char *copy = strdup(characters);
    CheckString(copy, (int) strlen(characters));
    free(copy);
}
static int password_goes_to_the_check_alone(void)
{
    correct_password = 0;
    type("s3cr3t-pass\n");
    CHECK(checks == 1 && !strcmp(seen_password, "s3cr3t-pass") && bells == 1 && unlocked == 0);
    correct_password = 1;
    type("s3cr3t-pass\r");
    CHECK(checks == 2 && unlocked == 1);
    return 0;
}
static int password_wiped_after_a_wrong_one(void)
{
    correct_password = 0;
    type("s3cr3t-pass\n");
    CHECK(checks == 1 && !strcmp(seen_password, "s3cr3t-pass") && bells == 1 && unlocked == 0);
    CHECK(!in_memory("s3cr3t"));
    return 0;
}
static int password_wiped_after_the_right_one(void)
{
    correct_password = 1;
    type("s3cr3t-pass\r");
    CHECK(checks == 1 && unlocked == 1);
    CHECK(!in_memory("s3cr3t"));
    return 0;
}
static int password_wiped_when_the_line_is_killed(void)
{
    type("abcdefgh");
    CHECK(in_memory("abcdefgh"));
    type("\033");
    CHECK(!in_memory("abcdefgh") && !in_memory("abcd"));
    return 0;
}
static int password_wiped_by_backspace(void)
{
    type("abcdefgh\010\010\010");
    CHECK(in_memory("abcde") && !in_memory("abcdef") && !in_memory("fgh"));
    return 0;
}
static int password_wiped_when_the_dialog_times_out(void)
{
    type("abcdefgh");
    CheckString(NULL, 0);
    CHECK(!in_memory("abcd"));
    return 0;
}
static int long_password(void)
{
    char typed[400];
    memset(typed, 'x', 300); memcpy(typed + 300, "end\n", 5);
    correct_password = 0;
    type(typed);
    CHECK(strlen(seen_password) == 255 || strlen(seen_password) >= 200);
    CHECK(!in_memory("xxxxxxxx"));
    return 0;
}
static void messages(void)
{
    smNLS.noPamErrorString = "Unable to lock the display: dtsession was built without PAM, which checks the password.";
    smNLS.pamServiceErrorString = "Unable to lock the display: the PAM service file %s is missing or cannot be read.";
    smNLS.noAccountErrorString = "Unable to lock the display: the account of the user who started the session was not found.";
}
static int refuses_to_lock_without_pam(void)
{
    messages();
    pam_status = SM_PAM_NOT_BUILT;
    CHECK(LockDisplayStart(True) == 0);
    CHECK(errors_printed == 1 && !strcmp(printed, smNLS.noPamErrorString));       /* its own message, not a generic one */
    CHECK(bells == 1 && notices_sent == 1 && system_ready == 1);
    CHECK(LockDisplayStart(False) == 0);              /* also for the lock after a time-out */
    CHECK(errors_printed == 2 && notices_sent == 2 && system_ready == 2);
    return 0;
}
static int refuses_to_lock_without_the_service_file(void)
{
    messages();
    pam_status = SM_PAM_NO_SERVICE;
    CHECK(LockDisplayStart(True) == 0);
    CHECK(errors_printed == 1);
    CHECK(strstr(printed, "/etc/pam.d/dtsession") && !strstr(printed, "%s"));      /* it names the file */
    CHECK(!strcmp(printed, "Unable to lock the display: the PAM service file /etc/pam.d/dtsession is missing or cannot be read."));
    CHECK(bells == 1 && notices_sent == 1 && system_ready == 1);
    return 0;
}
static int service_file_message_from_a_catalog(void)
{
    messages();
    pam_status = SM_PAM_NO_SERVICE;
    service_file = "/usr/local/etc/pam.d/dtsession";
    smNLS.pamServiceErrorString = "%s fehlt %d %s";                   /* a translation can say anything: only the first %s is replaced */
    CHECK(LockIsAvailable() == False);
    CHECK(!strcmp(printed, "/usr/local/etc/pam.d/dtsession fehlt %d %s"));
    smNLS.pamServiceErrorString = "no insert at all";
    CHECK(LockIsAvailable() == False && !strcmp(printed, "no insert at all"));
    smNLS.pamServiceErrorString = "ends with it: %s";
    CHECK(LockIsAvailable() == False && !strcmp(printed, "ends with it: /usr/local/etc/pam.d/dtsession"));
    return 0;
}
static int refuses_to_lock_without_an_account(void)
{
    messages();
    pam_status = SM_PAM_NO_ACCOUNT;
    CHECK(LockDisplayStart(True) == 0);
    CHECK(errors_printed == 1 && !strcmp(printed, smNLS.noAccountErrorString));
    CHECK(bells == 1 && notices_sent == 1 && system_ready == 1);
    return 0;
}
static int startup_check_says_why(void)
{
    messages();
    pam_status = SM_PAM_NO_SERVICE;
    CHECK(LockIsAvailable() == False && errors_printed == 1);          /* the check main() makes: one message, no dialog, no notice */
    CHECK(bells == 0 && notices_sent == 0 && system_ready == 0);
    pam_status = SM_PAM_READY;
    CHECK(LockIsAvailable() == True && errors_printed == 1);           /* nothing to say when it is fine */
    return 0;
}
static int locks_when_the_check_is_there(void)
{
    messages();
    pam_status = SM_PAM_READY;
    CHECK(LockDisplayStart(True) == 1);
    CHECK(errors_printed == 0 && bells == 0 && notices_sent == 0 && system_ready == 0);
    CHECK(smGD.coverScreen == 1);
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"password_goes_to_the_check_alone", password_goes_to_the_check_alone},
    {"password_wiped_after_a_wrong_one", password_wiped_after_a_wrong_one},
    {"password_wiped_after_the_right_one", password_wiped_after_the_right_one},
    {"password_wiped_when_the_line_is_killed", password_wiped_when_the_line_is_killed},
    {"password_wiped_by_backspace", password_wiped_by_backspace},
    {"password_wiped_when_the_dialog_times_out", password_wiped_when_the_dialog_times_out},
    {"long_password", long_password},
    {"refuses_to_lock_without_pam", refuses_to_lock_without_pam},
    {"refuses_to_lock_without_the_service_file", refuses_to_lock_without_the_service_file},
    {"service_file_message_from_a_catalog", service_file_message_from_a_catalog},
    {"refuses_to_lock_without_an_account", refuses_to_lock_without_an_account},
    {"startup_check_says_why", startup_check_says_why},
    {"locks_when_the_check_is_there", locks_when_the_check_is_there},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    if (argc < 2) {
        for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name);
        return 0;
    }
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
count = 0
with tempfile.TemporaryDirectory(prefix="dtsession-lock-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    c = work / "lock.c"
    c.write_text(harness)
    binary = work / "lock"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-Wno-unused-function", "-Wno-unused-variable",
                  "-Wno-unused-but-set-variable", str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr)
        sys.exit("cannot compile the lock harness")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
    for scenario in names:
        count += 1
        run = subprocess.run([str(binary), scenario], capture_output=True, text=True)
        if run.returncode:
            failed.append(scenario)
            sys.stderr.write("%s failed:\n%s\n" % (scenario, run.stderr.strip().splitlines()[0] if run.stderr.strip() else "exit %d" % run.returncode))
if failed:
    sys.exit("%d of %d lock scenarios failed: %s" % (len(failed), count, ", ".join(failed)))
print("Lock: %d scenarios passed" % count)

