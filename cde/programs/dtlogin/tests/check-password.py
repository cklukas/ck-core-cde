#!/usr/bin/env python3
"""Drive the greeter dialogue of the login manager through an expired password.

The production state machine of session.c (ManageGreeter) and, where it is there, the conversation
that carries the questions of the PAM stack to the greeter are cut out of the source and run with
scripted answers of the greeter, a scripted authentication and a stand-in for the password change of
libDtSvc that asks the questions a PAM stack asks.  What the manager sends to the greeter is recorded
and compared with what a user would see.  The scenarios run under ASan and UBSan, each in a process
of its own.  Set DTLOGIN_SOURCE to test another copy of the sources.
"""
from scenarios import block, finish, read, run_all, until

session = read("session.c")

states = until(session, "#define GREET_STATE_LOGIN", "char *globalDisplayName;")
if "* Strings of the greeter" in session:
    greeter_start = "/*****************************************************************************\n* Strings of the greeter"
else:
    greeter_start = "/*****************************************************************************\n* ManageGreeter"
greeter_code = until(session, greeter_start, "static void\nTellGreeter(\n  RequestHeader *phdr)")

harness = r'''
#define _GNU_SOURCE
#include "dm.h"
#include "vgmsg.h"
#include "vgproto.h"
#include "sysauth.h"
#include <security/pam_appl.h>

int _DtSvcPamChangePassword(const char *, const char *, const char *,
                            int (*)(int, const struct pam_message **, struct pam_response **, void *), void *);

#ifndef MC_PASSWD_CHANGED
#define MC_PASSWD_CHANGED 17
#define MC_PASSWD_NOT_CHANGED 18
#endif

/* the response to an expired-password message as dtgreet sends it */
typedef struct { ResponseHeader hdr; int change; } ExpassResponse;

enum { MAX_SCRIPT = 32, MAX_LOG = 64 };
struct step { int opcode; const char *text; int change; int offset; };     /* an answer of the greeter */
struct pamprompt { int style; const char *text; };                       /* a question of the stack */
struct logged { int opcode; int echo; int id; char text[600]; int length; };

extern struct step script[MAX_SCRIPT]; extern int nscript, next_step;
extern struct pamprompt prompts[8]; extern int nprompts;
extern int authenticate_codes[8], authenticate_calls, pam_status, pam_calls;
extern char pam_answers[8][64]; extern int pam_conv_result, pam_messages;
extern struct logged sent[MAX_LOG]; extern int nsent;
extern char *progName;
void reset(void);
extern int debugLevel;
''' + states + r'''
int ManageGreeterTest(struct display *d, struct greet_info *greet, struct greet_state *state);
void StartState(struct greet_state *state);
'''

unit = r'''
#include "harness.h"

static void TellGreeter(RequestHeader *phdr);
static int AskGreeter(RequestHeader *preqhdr, char *b, int blen);
''' + greeter_code + r'''
void StartState(struct greet_state *state) { memset(state, 0, sizeof *state); state->id = GREET_STATE_ENTER; }
int ManageGreeterTest(struct display *d, struct greet_info *greet, struct greet_state *state)
{ struct verify_info verify; return ManageGreeter(d, greet, &verify, state); }

static void TellGreeter(RequestHeader *phdr)
{
    struct logged *l = &sent[nsent < MAX_LOG ? nsent++ : MAX_LOG - 1];
    char *base = (char *)phdr;
    l->opcode = phdr->opcode; l->length = phdr->length; l->echo = -1; l->id = -1; l->text[0] = '\0';
    if (phdr->length > REQUEST_LIM_MAXLEN) { fprintf(stderr, "FAIL request of %d bytes\n", phdr->length); exit(1); }
    if (phdr->opcode == REQUEST_OP_CHALLENGE) {
        RequestChallenge *r = (RequestChallenge *)phdr;
        l->echo = r->bEcho; l->id = r->idMC;
        snprintf(l->text, sizeof l->text, "%s", base + r->offChallenge);
        if (r->offChallenge >= phdr->length || !memchr(base + r->offChallenge, 0, phdr->length - r->offChallenge))
        { fprintf(stderr, "FAIL challenge text is not inside the request\n"); exit(1); }
    } else if (phdr->opcode == REQUEST_OP_MESSAGE) {
        RequestMessage *r = (RequestMessage *)phdr;
        l->id = r->idMC;
        snprintf(l->text, sizeof l->text, "%s", base + r->offMessage);
        if (r->offMessage >= phdr->length || !memchr(base + r->offMessage, 0, phdr->length - r->offMessage))
        { fprintf(stderr, "FAIL message text is not inside the request\n"); exit(1); }
    }
}

/* the next scripted answer of the greeter; 0 when dtgreet is gone */
static int AskGreeter(RequestHeader *preqhdr, char *buf, int blen)
{
    struct step *s;
    ResponseHeader *h = (ResponseHeader *)buf;
    (void)preqhdr;
    if (next_step >= nscript) return 0;
    s = &script[next_step++];
    memset(buf, 0, blen);
    h->opcode = s->opcode;
    if (s->opcode == REQUEST_OP_CHALLENGE) {
        ResponseChallenge *r = (ResponseChallenge *)h;
        size_t n = strlen(s->text);
        r->offResponse = s->offset ? s->offset : sizeof *r;
        memcpy(buf + sizeof *r, s->text, n + 1);
        h->length = sizeof *r + n + 1;
    } else if (s->opcode == REQUEST_OP_EXPASSWORD) {
        ExpassResponse *r = (ExpassResponse *)h;
        r->change = s->change;
        h->length = sizeof *r;
    } else {
        h->length = sizeof *h;
    }
    return h->length;
}
'''

stubs = r'''
#define _GNU_SOURCE
#include "harness.h"
#include <stdarg.h>

struct step script[MAX_SCRIPT]; int nscript, next_step;
struct pamprompt prompts[8]; int nprompts;
int authenticate_codes[8], authenticate_calls, pam_status, pam_calls, pam_conv_result, pam_messages;
char pam_answers[8][64];
struct logged sent[MAX_LOG]; int nsent;
char *progName = "dtlogin";
int debugLevel;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

void Debug(char *fmt, ...) { (void)fmt; }
void LogError(unsigned char *fmt, ...) { (void)fmt; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }

void reset(void)
{
    memset(script, 0, sizeof script); nscript = next_step = 0;
    memset(prompts, 0, sizeof prompts); nprompts = 0;
    memset(authenticate_codes, 0, sizeof authenticate_codes); authenticate_calls = 0;
    pam_status = PAM_SUCCESS; pam_calls = 0; pam_conv_result = PAM_SUCCESS; pam_messages = 0;
    memset(pam_answers, 0, sizeof pam_answers);
    memset(sent, 0, sizeof sent); nsent = 0;
}

int Authenticate(struct display *d, char *name, char *passwd, char **msg)
{
    (void)d; (void)passwd; (void)msg;
    if (!name) return VF_INVALID;            /* the reset of the authentication at the start of the login screen */
    return authenticate_codes[authenticate_calls++];
}

/* the password change of libDtSvc: put the questions of the stack to the conversation of the caller */
int _DtSvcPamChangePassword(const char *service, const char *user, const char *display,
                            int (*conv)(int, const struct pam_message **, struct pam_response **, void *),
                            void *appdata)
{
    int i, result = pam_status;
    (void)service; (void)user; (void)display;
    pam_calls++;
    for (i = 0; i < nprompts; i++) {
        struct pam_message m = { prompts[i].style, prompts[i].text };
        const struct pam_message *p[1] = { &m };
        struct pam_response *r = NULL;
        int status = conv(1, p, &r, appdata);
        pam_messages++;
        pam_conv_result = status;
        if (status != PAM_SUCCESS) return PAM_AUTHTOK_ERR;
        if (r && r[0].resp) {
            snprintf(pam_answers[i], sizeof pam_answers[i], "%s", r[0].resp);
            memset(r[0].resp, 0, strlen(r[0].resp));
            free(r[0].resp);
        }
        free(r);
    }
    return result;
}

static struct display TheDisplay;
static struct greet_info Greet;
static struct greet_state State;

static void Say(int opcode, const char *text, int change)
{
    CHECK(nscript < MAX_SCRIPT);
    script[nscript].opcode = opcode; script[nscript].text = text; script[nscript].change = change;
    nscript++;
}
#define ANSWER(text) Say(REQUEST_OP_CHALLENGE, text, 0)

static void Prompt(int style, const char *text)
{
    CHECK(nprompts < 8);
    prompts[nprompts].style = style; prompts[nprompts].text = text; nprompts++;
}

/* run the dialogue until the script of the greeter runs out */
static int Run(void)
{
    int rounds = 0;
    TheDisplay.name = ":0";
    StartState(&State);
    while (ManageGreeterTest(&TheDisplay, &Greet, &State)) CHECK(++rounds < 200);
    return rounds;
}

/* the user logged in with an expired password: user name, password and the OK of the message */
static void AgedLogin(int change)
{
    authenticate_codes[0] = VF_CHALLENGE;
    authenticate_codes[1] = VF_PASSWD_AGED;
    ANSWER("alice");
    ANSWER("oldpw");
    Say(REQUEST_OP_EXPASSWORD, NULL, change);
}

static int Index(int opcode, int from)
{
    for (int i = from; i < nsent; i++) if (sent[i].opcode == opcode) return i;
    return -1;
}

static void ChangeSucceeds(void)
{
    int i;

    reset();
    AgedLogin(1);
    Prompt(PAM_PROMPT_ECHO_OFF, "Current password: ");
    Prompt(PAM_PROMPT_ECHO_OFF, "New password: ");
    Prompt(PAM_PROMPT_ECHO_OFF, "Retype new password: ");
    ANSWER("oldpw"); ANSWER("newpw1"); ANSWER("newpw1");
    Say(REQUEST_OP_MESSAGE, NULL, 0);
    Run();

    /* the expired-password message, then the questions of the stack as the user sees them */
    i = Index(REQUEST_OP_EXPASSWORD, 0);
    CHECK(i >= 0);
    CHECK(pam_calls == 1 && pam_messages == 3);
    CHECK(sent[i + 1].opcode == REQUEST_OP_CHALLENGE && sent[i + 1].echo == 0 && !strcmp(sent[i + 1].text, "Current password: "));
    CHECK(sent[i + 2].opcode == REQUEST_OP_CHALLENGE && sent[i + 2].echo == 0 && !strcmp(sent[i + 2].text, "New password: "));
    CHECK(sent[i + 3].opcode == REQUEST_OP_CHALLENGE && !strcmp(sent[i + 3].text, "Retype new password: "));
    CHECK(!strcmp(pam_answers[0], "oldpw") && !strcmp(pam_answers[1], "newpw1") && !strcmp(pam_answers[2], "newpw1"));
    /* the result is shown, and the login screen comes back with the user name */
    CHECK(sent[i + 4].opcode == REQUEST_OP_MESSAGE && sent[i + 4].id == MC_PASSWD_CHANGED);
    CHECK(sent[i + 5].opcode == REQUEST_OP_CHALLENGE && sent[i + 5].echo == 1);
    /* nothing of the login is kept: the old password is gone and the user is not logged in */
    CHECK(Greet.password == NULL && State.authenticated == 0);
}

static void ChangeDeclined(void)
{
    reset();
    AgedLogin(0);
    Run();
    CHECK(pam_calls == 0);
    CHECK(Index(REQUEST_OP_EXPASSWORD, 0) >= 0);
    CHECK(sent[nsent - 1].opcode == REQUEST_OP_CHALLENGE && sent[nsent - 1].echo == 1);
    CHECK(State.authenticated == 0);
}

static void ChangeRefused(void)
{
    int i;

    reset();
    AgedLogin(1);
    Prompt(PAM_PROMPT_ECHO_OFF, "New password: ");
    ANSWER("x");
    pam_status = PAM_AUTHTOK_ERR;
    Say(REQUEST_OP_MESSAGE, NULL, 0);
    Run();
    i = Index(REQUEST_OP_MESSAGE, 0);
    CHECK(i >= 0 && sent[i].id == MC_PASSWD_NOT_CHANGED);
    CHECK(State.authenticated == 0 && Greet.password == NULL);
}

static void UserGivesUp(void)
{
    int i;

    reset();
    AgedLogin(1);
    Prompt(PAM_PROMPT_ECHO_OFF, "New password: ");
    Prompt(PAM_PROMPT_ECHO_OFF, "Retype new password: ");
    Say(REQUEST_OP_CLEAR, NULL, 0);                 /* the user pressed Start Over at the first question */
    Say(REQUEST_OP_MESSAGE, NULL, 0);
    Run();
    CHECK(pam_calls == 1 && pam_messages == 1 && pam_conv_result == PAM_CONV_ERR);
    i = Index(REQUEST_OP_MESSAGE, 0);
    CHECK(i >= 0 && sent[i].id == MC_PASSWD_NOT_CHANGED);
}

static void GreeterEnds(void)
{
    reset();
    AgedLogin(1);
    Prompt(PAM_PROMPT_ECHO_OFF, "New password: ");
    Run();                                          /* dtgreet is gone when the question is asked */
    CHECK(pam_conv_result == PAM_CONV_ERR);
    CHECK(Index(REQUEST_OP_MESSAGE, 0) < 0);        /* nobody to show the result to */
}

static void ModuleMessages(void)
{
    int i;

    reset();
    AgedLogin(1);
    Prompt(PAM_PROMPT_ECHO_OFF, "New password: ");
    Prompt(PAM_ERROR_MSG, "BAD PASSWORD: it is too short");
    Prompt(PAM_PROMPT_ECHO_OFF, "New password: ");
    ANSWER("short");
    Say(REQUEST_OP_MESSAGE, NULL, 0);               /* the box with what the module said is closed */
    ANSWER("a-long-enough-password");
    Say(REQUEST_OP_MESSAGE, NULL, 0);
    Run();
    i = Index(REQUEST_OP_EXPASSWORD, 0);
    CHECK(sent[i + 2].opcode == REQUEST_OP_MESSAGE && sent[i + 2].id == 0 &&
          !strcmp(sent[i + 2].text, "BAD PASSWORD: it is too short"));
    CHECK(!strcmp(pam_answers[0], "short") && !strcmp(pam_answers[2], "a-long-enough-password"));
}

static void LongQuestions(void)
{
    static char long_text[2000];
    int i;

    reset();
    memset(long_text, 'q', sizeof long_text - 1);
    AgedLogin(1);
    Prompt(PAM_PROMPT_ECHO_ON, long_text);
    Prompt(PAM_TEXT_INFO, long_text);
    ANSWER("typed");
    Say(REQUEST_OP_MESSAGE, NULL, 0);
    Run();
    /* a question longer than the message to the greeter can hold is cut off, not written beyond it */
    i = Index(REQUEST_OP_EXPASSWORD, 0);
    CHECK(sent[i + 1].opcode == REQUEST_OP_CHALLENGE && sent[i + 1].echo == 1);
    CHECK(strlen(sent[i + 1].text) > 300 && strlen(sent[i + 1].text) < REQUEST_LIM_MAXLEN);
    CHECK(!strcmp(pam_answers[0], "typed"));
}

static void DamagedAnswers(void)
{
    int i;

    reset();
    AgedLogin(1);
    Prompt(PAM_PROMPT_ECHO_OFF, "New password: ");
    /* an answer whose string does not lie inside the message is no answer */
    CHECK(nscript < MAX_SCRIPT);
    script[nscript].opcode = REQUEST_OP_CHALLENGE; script[nscript].text = "x"; script[nscript].offset = 400; nscript++;
    Say(REQUEST_OP_MESSAGE, NULL, 0);
    Run();
    CHECK(pam_conv_result == PAM_CONV_ERR);
    i = Index(REQUEST_OP_MESSAGE, 0);
    CHECK(i >= 0 && sent[i].id == MC_PASSWD_NOT_CHANGED);
}

static void UnrelatedAnswersWait(void)
{
    int i;

    reset();
    AgedLogin(1);
    Prompt(PAM_PROMPT_ECHO_OFF, "New password: ");
    Say(REQUEST_OP_LANG, NULL, 0);                   /* a language change arrives while the question is open */
    ANSWER("newpw");
    Say(REQUEST_OP_MESSAGE, NULL, 0);
    Run();
    CHECK(!strcmp(pam_answers[0], "newpw"));
    i = Index(REQUEST_OP_MESSAGE, 0);
    CHECK(i >= 0 && sent[i].id == MC_PASSWD_CHANGED);
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"change-succeeds", ChangeSucceeds},
    {"change-declined", ChangeDeclined},
    {"change-refused", ChangeRefused},
    {"user-gives-up", UserGivesUp},
    {"greeter-ends", GreeterEnds},
    {"module-messages", ModuleMessages},
    {"long-questions", LongQuestions},
    {"damaged-answers", DamagedAnswers},
    {"unrelated-answers-wait", UnrelatedAnswersWait},
};

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; ++i)
        if (strcmp(argv[1], scenarios[i].name) == 0) { scenarios[i].run(); puts("ok"); return 0; }
    fprintf(stderr, "FAIL unknown scenario %s\n", argv[1]);
    return 2;
}
'''

names = ["change-succeeds", "change-declined", "change-refused", "user-gives-up", "greeter-ends",
         "module-messages", "long-questions", "damaged-answers", "unrelated-answers-wait"]
failed = run_all("login-password-",
                 {"harness.h": harness, "session_unit.c": unit, "stubs.c": stubs},
                 [("session_unit", ["-w"]), ("stubs", ["-Wall", "-Wextra", "-Wno-unused-parameter"])],
                 [], names, defines=["-DHAS_PAM_LIBRARY"])
finish(failed, "Password change", "the expired-password dialogue with the greeter")
