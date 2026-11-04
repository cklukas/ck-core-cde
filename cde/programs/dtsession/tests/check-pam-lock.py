#!/usr/bin/env python3
"""Check how the screen lock asks PAM whether a typed password is the right one.

SmPam.c is compiled as it is, under AddressSanitizer and UndefinedBehaviorSanitizer,
against stand-ins for the PAM library (pam_start, pam_authenticate,
pam_acct_mgmt and pam_end, with a PAM header of its own), for the password
database and for the identity of the process.  The stand-in for
pam_authenticate calls the conversation function of the program the way a
module does and takes the answer back, so the real conversation function runs.
Functions that would let the program name another account (getpwnam, getenv,
getlogin, geteuid) stop the test when they are called.  A second build
without PAM must not refer to the PAM library at all.

Every scenario runs in its own process.  The service-name scenario also runs
with other program names (argv[0]) and hostile environments.

DTSESSION_SOURCE names another copy of the sources to test, for example one
with a deliberate defect.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
if not (source / "SmPam.c").exists():
    sys.exit("%s has no SmPam.c: the screen lock does not check passwords through PAM" % source)

PAM_HEADER = r"""
#ifndef FAKE_PAM_APPL_H
#define FAKE_PAM_APPL_H
typedef struct pam_handle pam_handle_t;
struct pam_message { int msg_style; const char *msg; };
struct pam_response { char *resp; int resp_retcode; };
struct pam_conv {
    int (*conv)(int, const struct pam_message **, struct pam_response **, void *);
    void *appdata_ptr;
};
#define PAM_SUCCESS 0
#define PAM_OPEN_ERR 1
#define PAM_SYSTEM_ERR 4
#define PAM_BUF_ERR 5
#define PAM_PERM_DENIED 6
#define PAM_AUTH_ERR 7
#define PAM_AUTHINFO_UNAVAIL 9
#define PAM_USER_UNKNOWN 10
#define PAM_MAXTRIES 11
#define PAM_NEW_AUTHTOK_REQD 12
#define PAM_ACCT_EXPIRED 13
#define PAM_CONV_ERR 19
#define PAM_PROMPT_ECHO_OFF 1
#define PAM_PROMPT_ECHO_ON 2
#define PAM_ERROR_MSG 3
#define PAM_TEXT_INFO 4
#define PAM_MAX_NUM_MSG 32
#define PAM_SILENT 0x8000U
#define PAM_DISALLOW_NULL_AUTHTOK 0x0001U
int pam_start(const char *, const char *, const struct pam_conv *, pam_handle_t **);
int pam_authenticate(pam_handle_t *, int);
int pam_acct_mgmt(pam_handle_t *, int);
int pam_end(pam_handle_t *, int);
#endif
"""

TOOLKIT_HEADER = r"""
#ifndef FAKE_INTRINSIC_H
#define FAKE_INTRINSIC_H
typedef unsigned char Boolean;
#define True 1
#define False 0
#endif
"""

COMMON = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <pwd.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); return 1; } } while (0)

/* the program name an older version of the lock took the service from */
static struct { char *programName; } smGD;

/* the identity of the process and the password database */
static uid_t real_uid = 1234;
static struct passwd accounts[3] = {
    { .pw_name = "root", .pw_uid = 0 },
    { .pw_name = "alice", .pw_uid = 1000 },
    { .pw_name = "bob", .pw_uid = 1234 },
};
static struct passwd nameless;
static int entry_without_name;
static struct passwd *fake_getpwuid(uid_t uid)
{
    int i;
    if (entry_without_name) { nameless.pw_name = (char *) ""; nameless.pw_uid = uid; return &nameless; }
    for (i = 0; i < 3; i++) if (accounts[i].pw_uid == uid) return &accounts[i];
    return NULL;
}
static uid_t fake_getuid(void) { return real_uid; }
static void fake_endpwent(void) { }
static void forbidden(const char *what) { fprintf(stderr, "failed: %s was called\n", what); abort(); }
static struct passwd *forbidden_getpwnam(const char *n) { (void) n; forbidden("getpwnam"); return NULL; }
static char *forbidden_getenv(const char *n) { (void) n; forbidden("getenv"); return NULL; }
static char *forbidden_getlogin(void) { forbidden("getlogin"); return NULL; }
static uid_t forbidden_geteuid(void) { forbidden("geteuid"); return 0; }

/* allocation failures: the fail_at-th allocation of the code under test fails */
static int allocations, fail_at;
static void *counted_calloc(size_t n, size_t size)
{
    if (fail_at && ++allocations == fail_at) return NULL;
    return calloc(n, size);
}
static char *counted_strdup(const char *s)
{
    if (fail_at && ++allocations == fail_at) return NULL;
    return strdup(s);
}
/* the file of the PAM service: whether it can be read, and what the code asked */
static int service_file_errno;                 /* 0: readable */
static char accessed[128];
static int access_calls, accessed_mode;
static int fake_access(const char *path, int mode)
{
    access_calls++; accessed_mode = mode;
    snprintf(accessed, sizeof accessed, "%s", path);
    if (service_file_errno) { errno = service_file_errno; return -1; }
    return 0;
}
#define access fake_access
#define getuid fake_getuid
#define getpwuid fake_getpwuid
#define endpwent fake_endpwent
#define getpwnam forbidden_getpwnam
#define getenv forbidden_getenv
#define getlogin forbidden_getlogin
#define geteuid forbidden_geteuid
#define calloc(n, size) counted_calloc(n, size)
#define strdup(s) counted_strdup(s)
"""

PAM_STANDIN = r"""
#include <security/pam_appl.h>

/* what the stand-in PAM library saw and what it is told to do */
struct pam_handle { int unused; };
static pam_handle_t the_handle;
static struct {
    int start_calls, auth_calls, acct_calls, end_calls;
    char order[16];
    char service[64], user[64];
    unsigned auth_flags, acct_flags;
    int end_status;
    pam_handle_t *started, *ended;
    const struct pam_conv *conv;
    int start_status, auth_status, acct_status;     /* returned when the password is right */
    int styles[8], nstyles;                         /* what the modules ask while authenticating */
    int conv_calls, conv_result;
    char answer[8][64]; int answered[8];
    const char *expected;                           /* the password the stack accepts */
    int (*hook)(const struct pam_conv *);           /* replaces the usual questions */
} pam;

static void reset(void)
{
    memset(&pam, 0, sizeof(pam));
    pam.styles[0] = PAM_PROMPT_ECHO_OFF; pam.nstyles = 1;
    pam.expected = "secret";
    real_uid = 1234; smGD.programName = NULL; entry_without_name = 0;
    allocations = 0; fail_at = 0;
    service_file_errno = 0; accessed[0] = 0; access_calls = 0; accessed_mode = 0;
}
static void note(char call) { size_t n = strlen(pam.order); if (n + 1 < sizeof(pam.order)) pam.order[n] = call; }

int pam_start(const char *service, const char *user, const struct pam_conv *conv, pam_handle_t **handle)
{
    pam.start_calls++; note('s');
    snprintf(pam.service, sizeof(pam.service), "%s", service ? service : "(null)");
    snprintf(pam.user, sizeof(pam.user), "%s", user ? user : "(null)");
    pam.conv = conv;
    if (pam.start_status != PAM_SUCCESS) { *handle = NULL; return pam.start_status; }
    *handle = pam.started = &the_handle;
    return PAM_SUCCESS;
}

int pam_authenticate(pam_handle_t *handle, int flags)
{
    struct pam_message messages[8];
    const struct pam_message *pointers[8];
    struct pam_response *responses = NULL;
    int i;

    pam.auth_calls++; note('a');
    pam.auth_flags = (unsigned) flags;
    if (handle != &the_handle) { fprintf(stderr, "failed: authenticate on a handle that was not started\n"); abort(); }
    if (pam.hook) return pam.hook(pam.conv);
    if (pam.nstyles > 0) {
        for (i = 0; i < pam.nstyles; i++) { messages[i].msg_style = pam.styles[i]; messages[i].msg = "Password: "; pointers[i] = &messages[i]; }
        pam.conv_calls++;
        pam.conv_result = pam.conv->conv(pam.nstyles, pointers, &responses, pam.conv->appdata_ptr);
        if (pam.conv_result != PAM_SUCCESS) return PAM_CONV_ERR;
        /* like the library: take the answers, wipe and free them */
        for (i = 0; i < pam.nstyles; i++) {
            if (responses[i].resp) {
                snprintf(pam.answer[i], sizeof(pam.answer[i]), "%s", responses[i].resp);
                pam.answered[i] = 1;
                memset(responses[i].resp, 0, strlen(responses[i].resp));
                free(responses[i].resp);
            }
        }
        free(responses);
        for (i = 0; i < pam.nstyles; i++)
            if (pam.styles[i] == PAM_PROMPT_ECHO_OFF && (!pam.answered[i] || strcmp(pam.answer[i], pam.expected)))
                return PAM_AUTH_ERR;
    }
    return pam.auth_status;
}

int pam_acct_mgmt(pam_handle_t *handle, int flags)
{
    (void) handle;
    pam.acct_calls++; note('m');
    pam.acct_flags = (unsigned) flags;
    return pam.acct_status;
}

int pam_end(pam_handle_t *handle, int status)
{
    pam.end_calls++; note('e');
    pam.ended = handle;
    pam.end_status = status;
    return PAM_SUCCESS;
}
"""

PAM_SCENARIOS = r"""
/* ask the lock about a password the way CheckString does: from a buffer of its own */
static int ask(const char *typed)
{
    int saved = fail_at, result;
    char *copy;

    fail_at = 0;
    copy = (strdup)(typed);
    fail_at = saved;
    result = SmPamCheckPassword(copy);
    free(copy);
    return result;
}

static int success(void)
{
    reset();
    CHECK(ask("secret") == True);
    CHECK(!strcmp(pam.service, "dtsession") && !strcmp(pam.user, "bob"));
    CHECK(!strcmp(pam.order, "same"));
    CHECK(pam.auth_flags == PAM_DISALLOW_NULL_AUTHTOK && pam.acct_flags == PAM_DISALLOW_NULL_AUTHTOK);
    CHECK(pam.ended == pam.started && pam.end_status == PAM_SUCCESS && pam.end_calls == 1);
    CHECK(pam.conv_calls == 1 && pam.conv_result == PAM_SUCCESS && pam.answered[0]);
    return 0;
}
static int wrong_password(void)
{
    reset();
    CHECK(ask("wrong") == False);
    CHECK(!strcmp(pam.order, "sae"));
    CHECK(pam.end_calls == 1 && pam.ended == pam.started && pam.end_status == PAM_AUTH_ERR);
    CHECK(ask("") == False);
    CHECK(ask("secre") == False);
    CHECK(ask("secrets") == False);
    return 0;
}
static int start_failure(void)
{
    static const int failures[] = { PAM_SYSTEM_ERR, PAM_BUF_ERR, PAM_OPEN_ERR };
    size_t i;
    for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        reset();
        pam.start_status = failures[i];
        CHECK(ask("secret") == False);
        CHECK(!strcmp(pam.order, "s"));          /* nothing after a failed start, no handle to end */
    }
    return 0;
}
static int authentication_error(void)
{
    static const int errors[] = { PAM_AUTH_ERR, PAM_AUTHINFO_UNAVAIL, PAM_USER_UNKNOWN, PAM_MAXTRIES, PAM_PERM_DENIED, PAM_SYSTEM_ERR };
    size_t i;
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        reset();
        pam.auth_status = errors[i];             /* the stack refuses although the password was right */
        CHECK(ask("secret") == False);
        CHECK(!strcmp(pam.order, "sae") && pam.end_status == errors[i]);
    }
    return 0;
}
static int account_expired(void)
{
    static const int errors[] = { PAM_ACCT_EXPIRED, PAM_PERM_DENIED, PAM_AUTHINFO_UNAVAIL, PAM_USER_UNKNOWN, PAM_SYSTEM_ERR };
    size_t i;
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        reset();
        pam.acct_status = errors[i];
        CHECK(ask("secret") == False);
        CHECK(!strcmp(pam.order, "same") && pam.end_status == errors[i]);
    }
    return 0;
}
static int password_expired_during_the_session_unlocks(void)
{
    reset();
    pam.acct_status = PAM_NEW_AUTHTOK_REQD;      /* the password expired while the user was logged in */
    CHECK(ask("secret") == True);
    CHECK(!strcmp(pam.order, "same") && pam.end_status == PAM_NEW_AUTHTOK_REQD && pam.end_calls == 1);
    return 0;
}
static int expired_password_does_not_excuse_a_wrong_one(void)
{
    reset();
    pam.acct_status = PAM_NEW_AUTHTOK_REQD;
    CHECK(ask("wrong") == False);                /* the authentication fails first; the account is not looked at */
    CHECK(!strcmp(pam.order, "sae"));
    return 0;
}
static int new_authtok_from_the_authentication_is_refused(void)
{
    reset();
    pam.auth_status = PAM_NEW_AUTHTOK_REQD;      /* only the account check may say so */
    CHECK(ask("secret") == False);
    CHECK(!strcmp(pam.order, "sae"));
    return 0;
}
static int conversation_refuses_visible_questions(void)
{
    static const int styles[] = { PAM_PROMPT_ECHO_ON, 7, 0, -1, 99 };
    size_t i;
    for (i = 0; i < sizeof(styles) / sizeof(styles[0]); i++) {
        reset();
        pam.styles[0] = styles[i];
        CHECK(ask("secret") == False);
        CHECK(pam.conv_result == PAM_CONV_ERR && !pam.answered[0]);
        CHECK(!strcmp(pam.order, "sae"));
    }
    reset();                                     /* a password prompt together with a visible question */
    pam.styles[1] = PAM_PROMPT_ECHO_ON; pam.nstyles = 2;
    CHECK(ask("secret") == False);
    CHECK(pam.conv_result == PAM_CONV_ERR && !pam.answered[0] && !pam.answered[1]);
    reset();
    pam.styles[0] = PAM_PROMPT_ECHO_ON; pam.styles[1] = PAM_PROMPT_ECHO_OFF; pam.nstyles = 2;
    CHECK(ask("secret") == False);
    CHECK(pam.conv_result == PAM_CONV_ERR);
    return 0;
}
static struct pam_response *answers_seen;
static int bad_requests(const struct pam_conv *conv)
{
    struct pam_message message = { PAM_PROMPT_ECHO_OFF, "Password: " };
    const struct pam_message *one[1] = { &message };
    const struct pam_message *many[PAM_MAX_NUM_MSG + 1];
    struct pam_response *responses;
    int i;

    for (i = 0; i <= PAM_MAX_NUM_MSG; i++) many[i] = &message;
    responses = NULL;
    if (conv->conv(0, one, &responses, conv->appdata_ptr) == PAM_SUCCESS || responses) return 1;
    if (conv->conv(-1, one, &responses, conv->appdata_ptr) == PAM_SUCCESS || responses) return 2;
    if (conv->conv(PAM_MAX_NUM_MSG + 1, many, &responses, conv->appdata_ptr) == PAM_SUCCESS || responses) return 3;
    if (conv->conv(1, NULL, &responses, conv->appdata_ptr) == PAM_SUCCESS || responses) return 4;
    if (conv->conv(1, one, NULL, conv->appdata_ptr) == PAM_SUCCESS) return 5;
    if (conv->conv(1, one, &responses, NULL) == PAM_SUCCESS || responses) return 6;
    /* the largest request is answered, and only the password prompts get an answer */
    message.msg_style = PAM_TEXT_INFO;
    if (conv->conv(PAM_MAX_NUM_MSG, many, &responses, conv->appdata_ptr) != PAM_SUCCESS || !responses) return 7;
    for (i = 0; i < PAM_MAX_NUM_MSG; i++) if (responses[i].resp) return 8;
    free(responses);
    return PAM_AUTH_ERR;
}
static int conversation_bad_requests(void)
{
    reset();
    pam.hook = bad_requests;
    CHECK(ask("secret") == False);
    CHECK(pam.end_status == PAM_AUTH_ERR);       /* any other value is the number of the request that was not refused */
    (void) answers_seen;
    return 0;
}
static int conversation_answers_every_password_prompt(void)
{
    reset();
    pam.styles[0] = PAM_TEXT_INFO; pam.styles[1] = PAM_PROMPT_ECHO_OFF;
    pam.styles[2] = PAM_ERROR_MSG; pam.styles[3] = PAM_PROMPT_ECHO_OFF; pam.nstyles = 4;
    CHECK(ask("secret") == True);
    CHECK(!pam.answered[0] && pam.answered[1] && !pam.answered[2] && pam.answered[3]);
    CHECK(!strcmp(pam.answer[1], "secret") && !strcmp(pam.answer[3], "secret"));
    return 0;
}
static int stack_without_a_question(void)
{
    reset();
    pam.nstyles = 0;                             /* a stack that never asks for a password */
    CHECK(ask("secret") == True);
    CHECK(pam.conv_calls == 0);
    return 0;
}
static int allocation_failures(void)
{
    int n;
    for (n = 1; n <= 5; n++) {
        reset();
        pam.styles[1] = PAM_PROMPT_ECHO_OFF; pam.nstyles = 2;
        fail_at = n;
        if (ask("secret") != False && allocations >= n) { fprintf(stderr, "failed: allocation %d failed and the password was accepted\n", n); return 1; }
        if (allocations >= n) CHECK(pam.acct_calls == 0);
        if (allocations >= n && n >= 2) CHECK(pam.conv_result == PAM_CONV_ERR);   /* the conversation reports the failure itself */
        CHECK(pam.start_calls == pam.end_calls);  /* a handle that was started is ended; the leak check does the rest */
    }
    return 0;
}
static int service_name_is_fixed(void)
{
    static char *names[] = { "dtsession", "/usr/dt/bin/dtsession", "/usr/bin/login", "sshd", "", "su", "../../etc/pam.d/other", NULL };
    int i;
    for (i = 0; names[i]; i++) {
        reset();
        smGD.programName = names[i];
        CHECK(ask("secret") == True);
        CHECK(!strcmp(pam.service, "dtsession"));
    }
    return 0;
}
static int user_is_the_one_of_the_real_uid(void)
{
    static const struct { uid_t uid; const char *name; } cases[] = { { 1234, "bob" }, { 1000, "alice" }, { 0, "root" } };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset();
        real_uid = cases[i].uid;
        CHECK(ask("secret") == True);
        CHECK(!strcmp(pam.user, cases[i].name) && pam.start_calls == 1);
    }
    return 0;
}
static int lock_status(void)
{
    reset();
    CHECK(SmPamLockStatus() == SM_PAM_READY);
    CHECK(access_calls == 1 && !strcmp(accessed, "/etc/pam.d/dtsession") && accessed_mode == R_OK);
    CHECK(pam.start_calls == 0);                  /* looking is not authenticating */
    reset(); service_file_errno = ENOENT;        /* the file is not there */
    CHECK(SmPamLockStatus() == SM_PAM_NO_SERVICE);
    reset(); service_file_errno = EACCES;        /* it cannot be read */
    CHECK(SmPamLockStatus() == SM_PAM_NO_SERVICE);
    reset(); service_file_errno = ENOTDIR;
    CHECK(SmPamLockStatus() == SM_PAM_NO_SERVICE);
    reset(); real_uid = 4321;                     /* no account for the user */
    CHECK(SmPamLockStatus() == SM_PAM_NO_ACCOUNT);
    reset(); entry_without_name = 1;
    CHECK(SmPamLockStatus() == SM_PAM_NO_ACCOUNT);
    reset(); real_uid = 4321; service_file_errno = ENOENT;   /* the file is looked at first */
    CHECK(SmPamLockStatus() == SM_PAM_NO_SERVICE);
    return 0;
}
static int service_file_name(void)
{
    reset();
    CHECK(!strcmp(SmPamServiceFile(), "/etc/pam.d/dtsession"));
    CHECK(access_calls == 0);
    return 0;
}
static int check_password_does_not_depend_on_the_file(void)
{
    reset(); service_file_errno = ENOENT;         /* the check of the password is only reached when the lock exists */
    CHECK(ask("secret") == True);
    return 0;
}
static int no_password(void)
{
    reset();
    CHECK(SmPamCheckPassword(NULL) == False);
    CHECK(pam.start_calls == 0);
    return 0;
}
static int user_name_for_the_dialog(void)
{
    char *name;
    reset();
    name = SmPamUserName();
    CHECK(name && !strcmp(name, "bob"));
    free(name);
    real_uid = 0;
    name = SmPamUserName();
    CHECK(name && !strcmp(name, "root"));
    free(name);
    real_uid = 4321;
    CHECK(SmPamUserName() == NULL);
    reset();
    entry_without_name = 1;
    CHECK(SmPamUserName() == NULL);
    reset();
    fail_at = 1;
    CHECK(SmPamUserName() == NULL);
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"success", success}, {"wrong_password", wrong_password}, {"start_failure", start_failure},
    {"authentication_error", authentication_error}, {"account_expired", account_expired},
    {"password_expired_during_the_session_unlocks", password_expired_during_the_session_unlocks},
    {"expired_password_does_not_excuse_a_wrong_one", expired_password_does_not_excuse_a_wrong_one},
    {"new_authtok_from_the_authentication_is_refused", new_authtok_from_the_authentication_is_refused},
    {"conversation_refuses_visible_questions", conversation_refuses_visible_questions},
    {"conversation_bad_requests", conversation_bad_requests},
    {"conversation_answers_every_password_prompt", conversation_answers_every_password_prompt},
    {"stack_without_a_question", stack_without_a_question},
    {"allocation_failures", allocation_failures},
    {"service_name_is_fixed", service_name_is_fixed},
    {"user_is_the_one_of_the_real_uid", user_is_the_one_of_the_real_uid},
    {"lock_status", lock_status},
    {"service_file_name", service_file_name},
    {"check_password_does_not_depend_on_the_file", check_password_does_not_depend_on_the_file},
    {"user_name_for_the_dialog", user_name_for_the_dialog},
    {"no_password", no_password},
    {NULL, NULL}
};
"""

NOPAM_SCENARIOS = r"""
static int no_pam(void)
{
    char *name = SmPamUserName();
    CHECK(name && !strcmp(name, "bob"));         /* the name of the user does not depend on PAM */
    free(name);
    CHECK(SmPamLockStatus() == SM_PAM_NOT_BUILT);  /* nothing of PAM, the service file included, is looked at */
    CHECK(SmPamServiceFile() == NULL);
    CHECK(access_calls == 0);
    CHECK(SmPamCheckPassword("secret") == False);
    CHECK(SmPamCheckPassword("") == False);
    CHECK(SmPamCheckPassword(NULL) == False);
    return 0;
}
static struct { const char *name; int (*run)(void); } scenarios[] = { {"no_pam", no_pam}, {NULL, NULL} };
"""

MAIN = r"""
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
"""

cc = shlex.split(os.environ.get("CC", "gcc"))
common_flags = ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
                "-Wno-unused-parameter"]
failed = []
count = 0
hostile_environment = {"USER": "root", "LOGNAME": "root", "LNAME": "root", "PAM_SERVICE": "sshd",
                       "PAM_USER": "root", "HOME": "/root"}

with tempfile.TemporaryDirectory(prefix="dtsession-pam-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    (work / "include" / "security").mkdir(parents=True)
    (work / "include" / "X11").mkdir()
    (work / "include" / "security" / "pam_appl.h").write_text(PAM_HEADER)
    (work / "include" / "X11" / "Intrinsic.h").write_text(TOOLKIT_HEADER)

    variants = (("pam", ["-DHAS_PAM_LIBRARY", '-DSM_PAM_SERVICE_FILE="/etc/pam.d/dtsession"'], PAM_STANDIN + '#include "%s"\n' % (source / "SmPam.c") + PAM_SCENARIOS),
                ("nopam", [], '#include "%s"\n' % (source / "SmPam.c") + NOPAM_SCENARIOS))
    for name, define, body in variants:
        # SmPam.c itself is compiled with warnings as errors, apart from the harness
        strict = cc + ["-std=gnu23", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", "-I", str(work / "include")] + define + [str(source / "SmPam.c")]
        checked = subprocess.run(strict, capture_output=True, text=True)
        if checked.returncode:
            sys.stderr.write(checked.stderr)
            sys.exit("SmPam.c does not compile cleanly in the %s variant" % name)
        c = work / ("pam-%s.c" % name)
        c.write_text(COMMON + body + MAIN)
        binary = work / ("pam-%s" % name)
        build = cc + common_flags + ["-I", str(work / "include"), "-I", str(source)] + define + [str(c), "-o", str(binary)]
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
                sys.stderr.write("%s/%s failed:\n%s\n" % (name, scenario, "\n".join(run.stderr.strip().splitlines()[:6]) or "exit %d" % run.returncode))
        if name == "pam":
            # the program name and the environment do not matter either
            for argv0 in ("dtsession", "/usr/dt/bin/dtsession", "login", "sshd", "../../su"):
                for scenario in ("service_name_is_fixed", "user_is_the_one_of_the_real_uid"):
                    count += 1
                    env = dict(os.environ, **hostile_environment)
                    run = subprocess.run([argv0, scenario], executable=str(binary), env=env, capture_output=True, text=True)
                    if run.returncode:
                        failed.append("pam/%s as '%s'" % (scenario, argv0))
                        sys.stderr.write("pam/%s as '%s' failed:\n%s\n" % (scenario, argv0, "\n".join(run.stderr.strip().splitlines()[:6]) or "exit %d" % run.returncode))

if failed:
    sys.exit("%d of %d PAM lock scenarios failed: %s" % (len(failed), count, ", ".join(failed)))
print("PAM lock: %d scenarios passed" % count)
