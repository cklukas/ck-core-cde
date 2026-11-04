#!/usr/bin/env python3
"""Check how the screen lock decides that a typed password is right.

The production localAuthenticate() of SmLock.c is cut out of the source and
compiled, under AddressSanitizer and UndefinedBehaviorSanitizer, with stand-ins
for the password database and for crypt() (which returns a null pointer for a
hash it cannot handle, as the C library of some systems does, or a failure
token that equals the hash).  Both build variants are covered: without PAM
(setuid dtsession reading /etc/shadow) and with PAM (the service is named after
the program).  Every scenario runs in its own process.

DTSESSION_SOURCE names another copy of the sources to test, for example an
older revision.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
text = (source / "SmLock.c").read_text()


def between(start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


reauth = between("/* Test for re-auth ability", "#endif /* linux */")
chain_end = "#endif /* HAS_PAM_LIBRARY, __linux__ */" if "HAS_PAM_LIBRARY, __linux__" in text else "#endif /* SIA */"
authenticate = between("static Boolean \nlocalAuthenticate(\n", chain_end) + "#endif\n"
helper = ""
if "PasswordMatchesHash (passwd, hash)" in text:
    first = text.rindex("#if !defined(HAS_PAM_LIBRARY)\n/*****", 0, text.index("PasswordMatchesHash (passwd, hash)"))
    helper = text[first:text.index("#endif /* !HAS_PAM_LIBRARY */", first)] + "#endif\n"

def whole(signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 3] + "\n"


check_string = (whole("static void\nWipePassword(") if "WipePassword" in text else "") + whole("static void \nCheckString(")

harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>
#include <shadow.h>
#include <unistd.h>

typedef int Boolean;
#define True 1
#define False 0
#define PAM_SUCCESS 0

/* the program name and, in older sources, the identity of the lock */
static struct { char *programName; int secureSystem; unsigned unLockUID, runningUID; } smGD;
#define SM_SETEUID(x) (0)
int SmPrivRaise(void) { return 0; }
void SmPrivLower(void) { }

/* the password database */
static struct passwd account;
static struct spwd shadow_entry;
static int have_account, have_shadow;
static char service_seen[64];
static struct passwd *fake_getpwnam(const char *n) { (void) n; return have_account ? &account : NULL; }
static struct passwd *fake_getpwuid(uid_t u) { (void) u; return have_account ? &account : NULL; }
static struct spwd *fake_getspnam(const char *n) { (void) n; return have_shadow ? &shadow_entry : NULL; }
static void fake_endpwent(void) { }
static void fake_endspent(void) { }

/*
 * crypt(): "$6$salt$..." hashes are computed (the result is the setting up to
 * the last '$' followed by h(password)); "*0" echoes a failure token equal to
 * its setting; every other setting is invalid and gives a null pointer.
 */
static char crypt_result[256];
static char *fake_crypt(const char *phrase, const char *setting)
{
    if (!strcmp(setting, "*0")) return strcpy(crypt_result, "*0");
    if (!strncmp(setting, "$6$", 3)) {
        const char *end = strrchr(setting, '$');
        snprintf(crypt_result, sizeof(crypt_result), "%.*s$h(%s)", (int) (end - setting), setting, phrase);
        return crypt_result;
    }
    return NULL;
}
static int fake_pam(char *service, char *user, void *conv, char *passwd)
{
    (void) user; (void) conv;
    snprintf(service_seen, sizeof(service_seen), "%s", service);
    return strcmp(passwd, "secret") ? 7 : PAM_SUCCESS;
}
#define _DtSvcPamAuthenticate fake_pam
#define getpwnam fake_getpwnam
#define getpwuid fake_getpwuid
#define getspnam fake_getspnam
#define endpwent fake_endpwent
#define endspent fake_endspent
#define crypt fake_crypt

/* what the typed characters go through before they are compared */
#define DtError 1
#define SM_REALLOC(p, n) tracked_realloc(p, n)
#define UINT_MAX 4294967295u
static char *password_buffer; static size_t password_buffer_size;
static void *tracked_realloc(void *p, size_t n) { password_buffer = realloc(p, n); password_buffer_size = n; return password_buffer; }
static char seen_password[256]; static int checks, correct_password, unlocked, bells;
static Boolean CheckPassword(char *typed) { snprintf(seen_password, sizeof seen_password, "%s", typed); checks++; return correct_password; }
static void UpdatePasswdField(int n) { (void) n; }
static void UnlockDisplay(Boolean a, Boolean b) { (void) a; (void) b; unlocked++; }
static void PrintErrnoError(int type, const char *m) { (void) type; (void) m; }
#define XBell(d, n) (bells++)
static struct { char *cantMallocErrorString; } smNLS;

''' + helper + reauth + "\n" + authenticate + "\n" + check_string + r'''
#undef crypt

static char passwd_hash[128], shadow_hash[128];
static void database(const char *passwd_field, const char *shadow_field)
{
    memset(&account, 0, sizeof(account));
    memset(&shadow_entry, 0, sizeof(shadow_entry));
    snprintf(passwd_hash, sizeof(passwd_hash), "%s", passwd_field);
    account.pw_name = "user";
    account.pw_passwd = passwd_hash;
    have_account = 1;
    have_shadow = shadow_field != NULL;
    if (have_shadow) {
        snprintf(shadow_hash, sizeof(shadow_hash), "%s", shadow_field);
        shadow_entry.sp_namp = "user";
        shadow_entry.sp_pwdp = shadow_hash;
    }
}
static int unlock(const char *typed)
{
    char *copy = strdup(typed);
    int result = localAuthenticate(NULL, 1000, copy);
    free(copy);
    return result;
}

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s\n", #condition); return 1; } } while (0)

#ifndef HAS_PAM_LIBRARY
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
static int shadow_correct(void) { database("x", "$6$ab$h(secret)"); CHECK(unlock("secret")); return 0; }
static int shadow_wrong(void) { database("x", "$6$ab$h(secret)"); CHECK(!unlock("wrong")); CHECK(!unlock("")); return 0; }
static int locked_account(void)
{
    database("x", "!$6$ab$h(secret)"); CHECK(!unlock("secret"));
    database("x", "!"); CHECK(!unlock("secret"));
    database("x", "*"); CHECK(!unlock("secret"));
    return 0;
}
static int failure_token_is_the_hash(void) { database("x", "*0"); CHECK(!unlock("anything")); return 0; }
static int empty_hash(void) { database("x", ""); CHECK(!unlock("secret")); CHECK(!unlock("")); return 0; }
static int old_hash_in_password_file(void)
{
    database("$6$cc$h(old)", "$6$ab$h(secret)");
    CHECK(!unlock("old"));
    CHECK(unlock("secret"));
    return 0;
}
static int no_shadow(void)
{
    database("$6$ab$h(secret)", NULL);
    CHECK(unlock("secret")); CHECK(!unlock("wrong"));
    database("x", NULL); CHECK(!unlock("secret"));
    return 0;
}
static int check_only(void)
{
    database("x", "$6$ab$h(secret)");
    CHECK(localAuthenticate(NULL, 1000, NULL) == True);
    return 0;
}
#else
static int pam_directory_in_program_name(void)
{
    char *saved = smGD.programName;
    smGD.programName = "/usr/dt/bin/dtsession";
    database("x", NULL);
    CHECK(unlock("secret")); CHECK(!strcmp(service_seen, "dtsession")); CHECK(!unlock("wrong"));
    smGD.programName = saved;
    return 0;
}
static int pam_program_found_through_path(void)
{
    smGD.programName = "dtsession";
    database("x", NULL);
    service_seen[0] = 0;
    CHECK(unlock("secret"));
    CHECK(!strcmp(service_seen, "dtsession"));
    return 0;
}
#endif

static struct { const char *name; int (*run)(void); } scenarios[] = {
#ifndef HAS_PAM_LIBRARY
    {"password_wiped_after_a_wrong_one", password_wiped_after_a_wrong_one},
    {"password_wiped_after_the_right_one", password_wiped_after_the_right_one},
    {"password_wiped_when_the_line_is_killed", password_wiped_when_the_line_is_killed},
    {"password_wiped_by_backspace", password_wiped_by_backspace},
    {"password_wiped_when_the_dialog_times_out", password_wiped_when_the_dialog_times_out},
    {"long_password", long_password},
    {"shadow_correct", shadow_correct}, {"shadow_wrong", shadow_wrong},
    {"locked_account", locked_account}, {"failure_token_is_the_hash", failure_token_is_the_hash},
    {"empty_hash", empty_hash}, {"old_hash_in_password_file", old_hash_in_password_file},
    {"no_shadow", no_shadow}, {"check_only", check_only},
#else
    {"pam_directory_in_program_name", pam_directory_in_program_name},
    {"pam_program_found_through_path", pam_program_found_through_path},
#endif
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
    for name, define in (("password-file", []), ("pam", ["-DHAS_PAM_LIBRARY"])):
        c = work / ("lock-%s.c" % name)
        c.write_text(harness)
        binary = work / ("lock-%s" % name)
        build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                      "-fno-omit-frame-pointer", "-Wno-unused-function", "-Wno-unused-variable",
                      "-Wno-unused-but-set-variable"] + define + [str(c), "-o", str(binary)]
        compiled = subprocess.run(build, capture_output=True, text=True)
        if compiled.returncode:
            sys.stderr.write(compiled.stderr)
            sys.exit("cannot compile the %s variant" % name)
        names = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
        for scenario in names:
            count += 1
            run = subprocess.run([str(binary), scenario], capture_output=True, text=True)
            if run.returncode:
                failed.append("%s/%s" % (name, scenario))
                sys.stderr.write("%s/%s failed:\n%s\n" % (name, scenario, run.stderr.strip().splitlines()[0] if run.stderr.strip() else "exit %d" % run.returncode))
if failed:
    sys.exit("%d of %d lock authentication scenarios failed: %s" % (len(failed), count, ", ".join(failed)))
print("Lock authentication: %d scenarios passed" % count)
