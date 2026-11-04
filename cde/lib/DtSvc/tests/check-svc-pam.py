#!/usr/bin/env python3
"""Check the PAM helper of libDtSvc (DtUtil2/SvcPam.c) that dtlogin authenticates with.

The production SvcPam.c is compiled as it is, under AddressSanitizer and
UndefinedBehaviorSanitizer, against a stand-in for the PAM library with a PAM
header of its own.  The stand-in for pam_authenticate calls the conversation
function of the helper the way a module does, with the messages as an array of
pointers to separately allocated messages (which is what Linux-PAM and OpenPAM
pass), and takes the answers back.  Every scenario runs in its own process.

DTSVC_SOURCE names a directory with another copy of SvcPam.c to test, for
example one with a deliberate defect.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSVC_SOURCE") or Path(__file__).resolve().parents[1] / "DtUtil2")

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
#define PAM_SYSTEM_ERR 4
#define PAM_BUF_ERR 5
#define PAM_PERM_DENIED 6
#define PAM_AUTH_ERR 7
#define PAM_NEW_AUTHTOK_REQD 12
#define PAM_ACCT_EXPIRED 13
#define PAM_CONV_ERR 19
#define PAM_SERVICE 1
#define PAM_USER 2
#define PAM_TTY 3
#define PAM_RHOST 4
#define PAM_PROMPT_ECHO_OFF 1
#define PAM_PROMPT_ECHO_ON 2
#define PAM_ERROR_MSG 3
#define PAM_TEXT_INFO 4
#define PAM_MAX_NUM_MSG 32
#define PAM_DISALLOW_NULL_AUTHTOK 0x0001U
#define PAM_ESTABLISH_CRED 0x0002U
int pam_start(const char *, const char *, const struct pam_conv *, pam_handle_t **);
int pam_end(pam_handle_t *, int);
int pam_set_item(pam_handle_t *, int, const void *);
int pam_authenticate(pam_handle_t *, int);
int pam_acct_mgmt(pam_handle_t *, int);
int pam_setcred(pam_handle_t *, int);
int pam_open_session(pam_handle_t *, int);
int pam_close_session(pam_handle_t *, int);
#endif
"""

harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <security/pam_appl.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); return 1; } } while (0)

/* allocation failures: the fail_at-th allocation of the helper fails */
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
#define calloc(n, size) counted_calloc(n, size)
#define strdup(s) counted_strdup(s)

/* the stand-in for the PAM library: handles that remember their items */
struct pam_handle { int live; char service[64], user[64], tty[64], rhost[64]; const struct pam_conv *conv; };
#define MAX_HANDLES 16
static struct pam_handle handles[MAX_HANDLES];
static int nhandles, starts, ends, live_handles;
static struct {
    int start_status, set_item_fail_item;          /* a failing start; an item whose pam_set_item fails */
    int auth_status, acct_status, session_status;
    int styles[8], nstyles;                        /* what the modules ask while authenticating */
    char answer[8][64]; int answered[8];
    const char *expected;                          /* the password the stack accepts */
    char user_authenticated[64];                   /* whose password pam_authenticate checked */
    int auth_calls, acct_calls, end_status;
    char order[32];
    int conv_result;
} pam;
static void note(char c) { size_t n = strlen(pam.order); if (n + 1 < sizeof pam.order) pam.order[n] = c; }
static void reset(void)
{
    memset(&pam, 0, sizeof pam);
    pam.expected = "secret";
    pam.styles[0] = PAM_PROMPT_ECHO_OFF; pam.nstyles = 1;
    memset(handles, 0, sizeof handles);
    nhandles = starts = ends = live_handles = 0;
    allocations = fail_at = 0;
}

int pam_start(const char *service, const char *user, const struct pam_conv *conv, pam_handle_t **handle)
{
    struct pam_handle *h;
    starts++; note('s');
    if (pam.start_status) { *handle = &handles[MAX_HANDLES - 1]; return pam.start_status; }   /* undefined after a failed start */
    h = &handles[nhandles++];
    h->live = 1; live_handles++;
    snprintf(h->service, sizeof h->service, "%s", service ? service : "");
    snprintf(h->user, sizeof h->user, "%s", user ? user : "");
    h->conv = conv;
    *handle = h;
    return PAM_SUCCESS;
}
int pam_end(pam_handle_t *h, int status)
{
    if (!h->live) { fprintf(stderr, "failed: pam_end on a handle that was ended\n"); abort(); }
    h->live = 0; live_handles--; ends++; pam.end_status = status; note('e');
    return PAM_SUCCESS;
}
int pam_set_item(pam_handle_t *h, int item, const void *value)
{
    if (!h->live) { fprintf(stderr, "failed: pam_set_item on a handle that was ended\n"); abort(); }
    if (item == pam.set_item_fail_item) return PAM_SYSTEM_ERR;
    switch (item) {
        case PAM_SERVICE: snprintf(h->service, sizeof h->service, "%s", (const char *) value); break;
        case PAM_USER: snprintf(h->user, sizeof h->user, "%s", (const char *) value); break;
        case PAM_TTY: snprintf(h->tty, sizeof h->tty, "%s", (const char *) value); break;
        case PAM_RHOST: snprintf(h->rhost, sizeof h->rhost, "%s", (const char *) value); break;
    }
    return PAM_SUCCESS;
}
int pam_authenticate(pam_handle_t *h, int flags)
{
    struct pam_message messages[8];
    const struct pam_message *pointers[8];
    struct pam_message *separate[8];
    struct pam_response *responses = NULL;
    int i;
    (void) flags;
    pam.auth_calls++; note('a');
    snprintf(pam.user_authenticated, sizeof pam.user_authenticated, "%s", h->user);
    for (i = 0; i < pam.nstyles; i++) {
        /* every message in memory of its own: the pointers are not an array of structs */
        separate[i] = malloc(sizeof **separate);
        separate[i]->msg_style = pam.styles[i]; separate[i]->msg = "Password: ";
        pointers[i] = separate[i];
        messages[i] = *separate[i];
    }
    if (pam.nstyles > 0) {
        pam.conv_result = h->conv->conv(pam.nstyles, pointers, &responses, h->conv->appdata_ptr);
        for (i = 0; i < pam.nstyles; i++) free(separate[i]);
        if (pam.conv_result != PAM_SUCCESS) return PAM_CONV_ERR;
        for (i = 0; i < pam.nstyles; i++) {
            if (responses[i].resp) {
                snprintf(pam.answer[i], sizeof pam.answer[i], "%s", responses[i].resp);
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
int pam_acct_mgmt(pam_handle_t *h, int flags) { (void) h; (void) flags; pam.acct_calls++; note('m'); return pam.acct_status; }
int pam_setcred(pam_handle_t *h, int flags) { (void) h; (void) flags; note('c'); return pam.session_status; }
int pam_open_session(pam_handle_t *h, int flags) { (void) h; (void) flags; note('o'); return pam.session_status; }
int pam_close_session(pam_handle_t *h, int flags) { (void) h; (void) flags; note('x'); return pam.session_status; }

#include "SvcPam.c"

/* a new start: the stand-in and the state of the helper (which a process keeps for its life) */
static void begin(void) { reset(); pamh = NULL; saved_user_passwd = NULL; }

/* the handle the helper holds now, or NULL */
#define HELD() (pamh)
static int authenticate(const char *user, const char *passwd)
{
    int saved = fail_at, result;
    char *copy;
    fail_at = 0; copy = (strdup)(passwd); fail_at = saved;
    result = _DtSvcPamAuthenticate("dtlogin", user, ":0", copy);
    free(copy);
    return result;
}

static int success(void)
{
    begin();
    CHECK(authenticate("alice", "secret") == PAM_SUCCESS);
    CHECK(!strcmp(pam.order, "sam"));                         /* start, authenticate, account check */
    CHECK(!strcmp(pam.user_authenticated, "alice"));
    CHECK(HELD() == &handles[0] && live_handles == 1);        /* kept for the session calls */
    CHECK(!strcmp(handles[0].service, "dtlogin") && !strcmp(handles[0].tty, ":0"));
    CHECK(saved_user_passwd == NULL);                         /* the password was on loan for the call only */
    return 0;
}
static int wrong_password_ends_the_handle(void)
{
    begin();
    CHECK(authenticate("alice", "wrong") == PAM_AUTH_ERR);
    CHECK(!strcmp(pam.order, "sae") && pam.acct_calls == 0);  /* no account check after a refusal */
    CHECK(HELD() == NULL && live_handles == 0 && pam.end_status == PAM_AUTH_ERR);
    CHECK(saved_user_passwd == NULL);
    return 0;
}
static int no_password(void)
{
    begin();
    CHECK(_DtSvcPamAuthenticate("dtlogin", "alice", ":0", NULL) == PAM_AUTH_ERR);
    CHECK(starts == 0 && HELD() == NULL);
    return 0;
}
static int account_that_may_not_log_in(void)
{
    static const int refused[] = { PAM_ACCT_EXPIRED, PAM_PERM_DENIED, PAM_SYSTEM_ERR };
    size_t i;
    for (i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        begin();
        pam.acct_status = refused[i];                         /* the password is right, the account is not usable */
        CHECK(authenticate("alice", "secret") == refused[i]);
        CHECK(!strcmp(pam.order, "same") && HELD() == NULL && live_handles == 0);
    }
    return 0;
}
static int expired_password_is_reported_and_the_handle_kept(void)
{
    begin();
    pam.acct_status = PAM_NEW_AUTHTOK_REQD;
    CHECK(authenticate("alice", "secret") == PAM_NEW_AUTHTOK_REQD);    /* dtlogin shows the expired-password dialog */
    CHECK(HELD() == &handles[0] && live_handles == 1);
    return 0;
}
static int next_attempt_checks_the_user_it_is_given(void)
{
    begin();
    CHECK(authenticate("alice", "wrong") == PAM_AUTH_ERR);
    CHECK(authenticate("bob", "secret") == PAM_SUCCESS);
    CHECK(!strcmp(pam.user_authenticated, "bob"));            /* not "alice" from the earlier attempt */
    begin();
    CHECK(authenticate("alice", "secret") == PAM_SUCCESS);
    CHECK(authenticate("bob", "secret") == PAM_SUCCESS);
    CHECK(!strcmp(pam.user_authenticated, "bob") && !strcmp(handles[1].user, "bob"));
    CHECK(live_handles == 1 && handles[0].live == 0 && HELD() == &handles[1]);   /* the earlier handle was ended */
    return 0;
}
static int session_calls_use_the_handle_of_the_login(void)
{
    begin();
    CHECK(authenticate("alice", "secret") == PAM_SUCCESS);
    CHECK(_DtSvcPamSetcred("dtlogin", "alice", ":0") == PAM_SUCCESS);
    CHECK(_DtSvcPamOpenSession("dtlogin", "alice", "remote.example:0") == PAM_SUCCESS);
    CHECK(_DtSvcPamCloseSession("dtlogin", "alice", "remote.example:0") == PAM_SUCCESS);
    CHECK(starts == 1 && !strcmp(pam.order, "samcox"));
    CHECK(!strcmp(handles[0].rhost, "remote.example") && !strcmp(handles[0].user, "alice"));
    return 0;
}
static int session_call_without_a_login_starts_a_handle(void)
{
    begin();
    CHECK(_DtSvcPamOpenSession("dtlogin", "alice", NULL) == PAM_SUCCESS);
    CHECK(starts == 1 && !strcmp(handles[0].user, "alice") && HELD() == &handles[0]);
    return 0;
}
static int existing_handle_takes_the_new_user_and_a_missing_service(void)
{
    begin();
    CHECK(authenticate("alice", "secret") == PAM_SUCCESS);
    CHECK(_DtSvcPamOpenSession(NULL, "bob", NULL) == PAM_SUCCESS);      /* no service given: nothing is read uninitialized */
    CHECK(!strcmp(handles[0].user, "bob") && !strcmp(handles[0].service, "dtlogin"));
    return 0;
}
static int failures_of_the_setup_end_the_handle(void)
{
    static const int items[] = { PAM_SERVICE, PAM_USER, PAM_TTY };
    size_t i;
    for (i = 0; i < sizeof items / sizeof items[0]; i++) {
        begin();
        CHECK(authenticate("alice", "secret") == PAM_SUCCESS);
        pam.set_item_fail_item = items[i];
        CHECK(_DtSvcPamOpenSession("dtlogin", "alice", ":0") == PAM_SYSTEM_ERR);
        CHECK(HELD() == NULL && live_handles == 0);
    }
    begin();
    pam.start_status = PAM_SYSTEM_ERR;
    CHECK(authenticate("alice", "secret") == PAM_SYSTEM_ERR);
    CHECK(HELD() == NULL && ends == 0);                       /* a start that failed leaves nothing to end */
    return 0;
}
static int conversation_answers_every_hidden_prompt(void)
{
    begin();
    pam.styles[0] = PAM_TEXT_INFO; pam.styles[1] = PAM_PROMPT_ECHO_OFF; pam.styles[2] = PAM_ERROR_MSG;
    pam.styles[3] = PAM_PROMPT_ECHO_OFF; pam.nstyles = 4;
    CHECK(authenticate("alice", "secret") == PAM_SUCCESS);
    CHECK(!pam.answered[0] && pam.answered[1] && !pam.answered[2] && pam.answered[3]);
    CHECK(!strcmp(pam.answer[1], "secret") && !strcmp(pam.answer[3], "secret"));
    return 0;
}
static int conversation_refuses_a_visible_question(void)
{
    static const int styles[] = { PAM_PROMPT_ECHO_ON, 7, 0, -1 };
    size_t i;
    for (i = 0; i < sizeof styles / sizeof styles[0]; i++) {
        begin();
        pam.styles[0] = PAM_PROMPT_ECHO_OFF; pam.styles[1] = styles[i]; pam.nstyles = 2;
        CHECK(authenticate("alice", "secret") != PAM_SUCCESS);
        CHECK(pam.conv_result == PAM_CONV_ERR && !pam.answered[0]);    /* nothing is answered, the first answer is freed */
    }
    return 0;
}
static int conversation_without_a_password_fails(void)
{
    struct pam_message m = { PAM_PROMPT_ECHO_OFF, "Password: " };
    const struct pam_message *p[1] = { &m };
    struct pam_response *r = NULL;
    begin();
    CHECK(login_conv(1, p, &r, NULL) == PAM_CONV_ERR && r == NULL);   /* outside an authentication there is no password */
    CHECK(login_conv(0, p, &r, NULL) == PAM_CONV_ERR);
    CHECK(login_conv(-1, p, &r, NULL) == PAM_CONV_ERR);
    CHECK(login_conv(PAM_MAX_NUM_MSG + 1, p, &r, NULL) == PAM_CONV_ERR);
    CHECK(login_conv(1, NULL, &r, NULL) == PAM_CONV_ERR);
    CHECK(login_conv(1, p, NULL, NULL) == PAM_CONV_ERR);
    return 0;
}
static int allocation_failures_leak_nothing(void)
{
    int n;
    for (n = 1; n <= 4; n++) {
        begin();
        pam.styles[1] = PAM_PROMPT_ECHO_OFF; pam.nstyles = 2;
        fail_at = n;
        if (authenticate("alice", "secret") == PAM_SUCCESS && allocations >= n) {
            fprintf(stderr, "failed: allocation %d failed and the password was accepted\n", n);
            return 1;
        }
        CHECK(saved_user_passwd == NULL);
        if (pamh) { pam_end(pamh, PAM_SUCCESS); pamh = NULL; }      /* what a login that is over does */
        CHECK(live_handles == 0);
    }
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"success", success},
    {"wrong_password_ends_the_handle", wrong_password_ends_the_handle},
    {"no_password", no_password},
    {"account_that_may_not_log_in", account_that_may_not_log_in},
    {"expired_password_is_reported_and_the_handle_kept", expired_password_is_reported_and_the_handle_kept},
    {"next_attempt_checks_the_user_it_is_given", next_attempt_checks_the_user_it_is_given},
    {"session_calls_use_the_handle_of_the_login", session_calls_use_the_handle_of_the_login},
    {"session_call_without_a_login_starts_a_handle", session_call_without_a_login_starts_a_handle},
    {"existing_handle_takes_the_new_user_and_a_missing_service", existing_handle_takes_the_new_user_and_a_missing_service},
    {"failures_of_the_setup_end_the_handle", failures_of_the_setup_end_the_handle},
    {"conversation_answers_every_hidden_prompt", conversation_answers_every_hidden_prompt},
    {"conversation_refuses_a_visible_question", conversation_refuses_a_visible_question},
    {"conversation_without_a_password_fails", conversation_without_a_password_fails},
    {"allocation_failures_leak_nothing", allocation_failures_leak_nothing},
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
with tempfile.TemporaryDirectory(prefix="dtsvc-pam-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    (work / "include" / "security").mkdir(parents=True)
    (work / "include" / "security" / "pam_appl.h").write_text(PAM_HEADER)
    c = work / "svcpam.c"
    c.write_text(harness)
    binary = work / "svcpam"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
                  "-Wno-unused-parameter", "-Wno-unused-but-set-variable",
                  "-I", str(work / "include"), "-I", str(source), str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr)
        sys.exit("cannot compile the PAM helper harness")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
    for scenario in names:
        count += 1
        run = subprocess.run([str(binary), scenario], capture_output=True, text=True)
        if run.returncode:
            failed.append(scenario)
            sys.stderr.write("%s failed:\n%s\n" % (scenario, "\n".join(run.stderr.strip().splitlines()[:6]) or "exit %d" % run.returncode))
if failed:
    sys.exit("%d of %d PAM helper scenarios failed: %s" % (len(failed), count, ", ".join(failed)))
print("DtSvc PAM helper: %d scenarios passed" % count)
