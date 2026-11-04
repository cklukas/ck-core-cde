#!/usr/bin/env python3
"""Drive dtsession's XSMP callbacks the way session management clients do.

dtsession is the session manager of the X Session Management Protocol: clients
connect over ICE sockets (only holders of the user's ICE authority cookie get
in), register, set and delete properties and take part in saves.  The
production RegisterClientProc, ProcessInteract and GetWsmClientsHandler of
SmXSMP.c and the property callbacks of SmProp.c are compiled, under
AddressSanitizer and UndefinedBehaviorSanitizer, with stand-ins for SMlib and
ToolTalk, and called with what clients and the window manager can send: a
client that restarts with the id it had before the session was saved, a
connection that registers while others have not, a host name without a
transport, replies with lists shorter than their count says, and a client that
deletes its first property.  Every scenario runs in its own process.

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
text = (source / "SmXSMP.c").read_text()


def function(signature):
    start = text.rindex(signature)   # the definition comes after any prototype
    return text[start:text.index("\n}", start) + 3] + "\n"


code = function("static\nvoid FreeProps (")
if "RemoveClient" in text:
    code += function("static void\nRemoveClient (")
code += function("void CloseDownClient (") + function("Status \nRegisterClientProc (") + function("static\nvoid ProcessInteract (")
if "WsmClientListIsValid" in text:
    code += function("static Boolean\nWsmClientListIsValid (")
code += function("Tt_callback_action GetWsmClientsHandler(")

harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <X11/Intrinsic.h>
#include <X11/SM/SMlib.h>
#include "SmXSMP.h"
#include "SmProp.h"

typedef void *Tt_message;
typedef void *Tt_pattern;
typedef int Tt_callback_action;
#define TT_CALLBACK_PROCESSED 1

XSMPSettings smXSMP;
ClientRecPtr connectedList;
static int interacts, generated, replies, saves;
static char *replied_id, *host_name_result;
static int workspace = 1;
static struct { void *display; } smGD;
#define SM_RUNNING_LOCK "lock"
static int exits, restart_commands, interact_null;
static SmsConn interacted[8];
static void ExecuteCommandProperty(const char *what, ClientRecPtr c) { (void) what; (void) c; restart_commands++; }
static void _DtReleaseLock(void *d, const char *l) { (void) d; (void) l; }
#define SM_EXIT(status) (exits++)
#define SmsCleanUp(c) ((void) (c))
#define IceSetShutdownNegotiation(c, b) ((void) 0)
#define IceCloseConnection(c) 0

#define SmsGenerateClientID fake_SmsGenerateClientID
#define SmsClientHostName fake_SmsClientHostName
#define SmsRegisterClientReply fake_SmsRegisterClientReply
#define SmsSaveYourself fake_SmsSaveYourself
#define SmsInteract fake_SmsInteract
static char *SmsGenerateClientID(SmsConn c) { (void) c; generated++; return strdup("1new-id"); }
static char *SmsClientHostName(SmsConn c) { (void) c; return host_name_result; }
static Status SmsRegisterClientReply(SmsConn c, char *id) { (void) c; replies++; replied_id = strdup(id); return 1; }
static void SmsSaveYourself(SmsConn c, int a, Bool b, int d, Bool e) { (void) c; (void) a; (void) b; (void) d; (void) e; saves++; }
static void SmsInteract(SmsConn c) { if (!c) interact_null++; if (interacts < 8) interacted[interacts] = c; interacts++; }
static int GetCurrentWorkspaceNumber(void) { return workspace; }

/* the reply to the GetWsmClients request */
static int tt_args = 3, tt_count;
static char *tt_ids; static int tt_ids_length;
static int *tt_nums; static int tt_nums_length;
static int tt_args_count(Tt_message m) { (void) m; return tt_args; }
static void tt_ival(Tt_message m, int n, int *v) { (void) m; (void) n; *v = tt_count; }
static void tt_bval(Tt_message m, int n, unsigned char **v, int *length)
{
    (void) m;
    if (n == 1) { *v = (unsigned char *) tt_ids; *length = tt_ids_length; }
    else { *v = (unsigned char *) tt_nums; *length = tt_nums_length; }
}
static void tt_reply(Tt_message m) { (void) m; }
static void tt_destroy(Tt_message m) { (void) m; }
#define tt_message_args_count tt_args_count
#define tt_message_arg_ival tt_ival
#define tt_message_arg_bval tt_bval
#define tt_message_reply tt_reply
#define tt_message_destroy tt_destroy

''' + code + r'''
#include "SmProp.c"

/* --- helpers --- */
static ClientRecPtr new_client(const char *id, int active)
{
    ClientRecPtr c = calloc(1, sizeof *c), tail;
    c->clientId = id ? strdup(id) : NULL;
    c->active = active;
    if (!connectedList) connectedList = c;
    else { for (tail = connectedList; tail->next; tail = tail->next); tail->next = c; }
    return c;
}
static void database(const char *id)
{
    XSMPClientDBRecPtr d = calloc(1, sizeof *d);
    d->clientId = strdup(id);
    d->next = smXSMP.xsmpDbList;
    smXSMP.xsmpDbList = d;
}
static char *dup_exact(const void *data, size_t n) { char *p = malloc(n ? n : 1); memcpy(p, data, n); return p; }
static void reset(void)
{
    connectedList = NULL; smXSMP.xsmpDbList = NULL; replies = generated = interacts = saves = 0;
    exits = restart_commands = interact_null = 0; host_name_result = NULL;
    memset(&smXSMP.saveState, 0, sizeof smXSMP.saveState);
}
static int connections;
static void connect_client(ClientRecPtr c) { c->smConn = (SmsConn) (uintptr_t) (0x1000 + ++connections); c->iceConn = (IceConn) (uintptr_t) (0x2000 + connections); c->active = 1; }

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* --- registration --- */
static int restart_with_a_saved_id(void)
{
    ClientRecPtr other, me;
    reset();
    other = new_client("1other", 1);
    me = new_client(NULL, 0);                   /* the connection that is registering has no id yet */
    database("1saved");
    host_name_result = strdup("local/box:/tmp/.ICE-unix/42");
    CHECK(RegisterClientProc(NULL, me, strdup("1saved")) == 1);
    CHECK(me->clientId && !strcmp(me->clientId, "1saved") && me->active);
    CHECK(replies == 1 && !strcmp(replied_id, "1saved") && saves == 0);
    (void) other;
    return 0;
}
static int connection_without_id_in_the_list(void)
{
    ClientRecPtr a, b, me;
    reset();
    a = new_client(NULL, 0);                     /* another connection that has not registered */
    me = new_client("1mine", 1);
    b = new_client(NULL, 0);
    database("1saved");
    (void) a; (void) b; (void) me;
    CHECK(RegisterClientProc(NULL, b, strdup("1saved")) == 1);
    CHECK(RegisterClientProc(NULL, a, strdup("1mine")) == 0);    /* in use by an active client */
    return 0;
}
static int unknown_and_reused_ids(void)
{
    ClientRecPtr gone, me;
    reset();
    gone = new_client("1gone", 0);               /* terminated: its id may be taken again */
    me = new_client(NULL, 0);
    CHECK(RegisterClientProc(NULL, me, strdup("1nobody")) == 0);
    CHECK(RegisterClientProc(NULL, me, strdup("1gone")) == 1);
    (void) gone;
    return 0;
}
static int new_client_gets_an_id(void)
{
    ClientRecPtr me;
    reset();
    me = new_client(NULL, 0);
    host_name_result = strdup("tcp/far:6000");
    CHECK(RegisterClientProc(NULL, me, NULL) == 1);
    CHECK(generated == 1 && saves == 1 && !strcmp(me->clientId, "1new-id"));
    CHECK(!strcmp(me->clientHost, "far:6000"));
    return 0;
}
static int host_name_without_transport(void)
{
    ClientRecPtr me;
    reset();
    me = new_client(NULL, 0);
    host_name_result = strdup("plainhost");
    CHECK(RegisterClientProc(NULL, me, NULL) == 1);
    CHECK(me->clientHost && !strcmp(me->clientHost, "plainhost"));
    return 0;
}

/* --- the window manager's list of clients --- */
static void reply(int args, int count, const char *ids, int ids_length, const int *nums, int nums_length)
{
    tt_args = args; tt_count = count;
    tt_ids = ids ? dup_exact(ids, ids_length) : NULL; tt_ids_length = ids_length;
    tt_nums = nums ? (int *) dup_exact(nums, nums_length) : NULL; tt_nums_length = nums_length;
    smXSMP.saveState.clientIds = NULL; smXSMP.saveState.workspaceNums = NULL;
}
static int wsm_reply_valid(void)
{
    int nums[2] = {1, 2};
    ClientRecPtr c;
    reset();
    c = new_client("1a", 1); connect_client(c); c->interactRequested = 1;
    smXSMP.saveState.interactClient = c;
    reply(3, 2, "1a\0" "1b\0", 6, nums, sizeof nums);
    CHECK(GetWsmClientsHandler(NULL, NULL) == TT_CALLBACK_PROCESSED);
    CHECK(smXSMP.saveState.numClientIds == 2 && interacts == 1);
    return 0;
}
static int wsm_reply_shorter_than_its_count(void)
{
    int nums[2] = {1, 2};
    ClientRecPtr c;
    reset();
    c = new_client("1a", 1); connect_client(c); c->interactRequested = 1;
    smXSMP.saveState.interactClient = c;
    reply(3, 1000, "1a\0" "1b\0", 6, nums, sizeof nums);          /* a thousand clients, two strings */
    CHECK(GetWsmClientsHandler(NULL, NULL) == TT_CALLBACK_PROCESSED);
    CHECK(smXSMP.saveState.numClientIds == 0 && interacts == 1);   /* the client may interact anyway */
    reply(3, 2, "1a1b", 4, nums, sizeof nums);                    /* strings without terminators */
    GetWsmClientsHandler(NULL, NULL);
    CHECK(smXSMP.saveState.numClientIds == 0);
    reply(3, 2, "1a\0" "1b\0", 6, nums, 4);                       /* workspace numbers for one client only */
    GetWsmClientsHandler(NULL, NULL);
    CHECK(smXSMP.saveState.numClientIds == 0);
    reply(3, -5, "1a\0", 3, nums, sizeof nums);
    GetWsmClientsHandler(NULL, NULL);
    CHECK(smXSMP.saveState.numClientIds == 0);
    return 0;
}
static int wsm_reply_after_the_save_ended(void)
{
    int nums[1] = {1};
    reset();
    smXSMP.saveState.interactClient = NULL;                       /* the save was cancelled meanwhile */
    reply(3, 1, "1a\0", 3, nums, sizeof nums);
    CHECK(GetWsmClientsHandler(NULL, NULL) == TT_CALLBACK_PROCESSED);
    reply(2, 0, NULL, 0, NULL, 0);
    CHECK(GetWsmClientsHandler(NULL, NULL) == TT_CALLBACK_PROCESSED);
    return 0;
}
static int interact_before_registration(void)
{
    int nums[1] = {1};
    ClientRecPtr c, early;
    reset();
    c = new_client("1a", 1); connect_client(c); c->interactRequested = 1;
    early = new_client(NULL, 0); early->interactRequested = 1;     /* asked to interact before it registered */
    smXSMP.saveState.interactClient = early;
    reply(3, 1, "1a\0", 3, nums, sizeof nums);
    CHECK(GetWsmClientsHandler(NULL, NULL) == TT_CALLBACK_PROCESSED);
    return 0;
}

/* --- properties --- */
static SmProp *make_property(const char *name, const char *value)
{
    SmProp *p = malloc(sizeof *p);
    p->name = strdup(name); p->type = strdup(SmARRAY8); p->num_vals = 1;
    p->vals = malloc(sizeof(SmPropValue));
    p->vals[0].length = strlen(value); p->vals[0].value = strdup(value);
    return p;
}
static int delete_properties(void)
{
    ClientRecPtr c;
    SmProp **props = malloc(3 * sizeof(SmProp *));        /* the callback frees the array */
    char **names;
    reset();
    c = new_client("1a", 1);
    props[0] = make_property("Program", "xterm");
    props[1] = make_property("UserID", "me");
    props[2] = make_property("CurrentDirectory", "/home");
    SetPropertiesProc(NULL, c, 3, props);
    /* the first property of the list */
    names = malloc(sizeof(char *)); names[0] = strdup("Program");
    DeletePropertiesProc(NULL, c, 1, names);
    CHECK(GetPropertyRec(c, "Program") == NULL && GetPropertyRec(c, "UserID") != NULL);
    CHECK(c->props != NULL && !strcmp(c->props->prop.name, "UserID"));
    /* the last, then the one left, then one that is not there */
    names = malloc(2 * sizeof(char *)); names[0] = strdup("CurrentDirectory"); names[1] = strdup("Nothing");
    DeletePropertiesProc(NULL, c, 2, names);
    CHECK(GetPropertyRec(c, "CurrentDirectory") == NULL && c->props && !c->props->next);
    names = malloc(sizeof(char *)); names[0] = strdup("UserID");
    DeletePropertiesProc(NULL, c, 1, names);
    CHECK(c->props == NULL);
    names = malloc(sizeof(char *)); names[0] = strdup("UserID");
    DeletePropertiesProc(NULL, c, 1, names);                       /* nothing left to delete */
    CHECK(GetArrayPropertyValue(c, "Program") == NULL);
    return 0;
}

/* --- the life of a client record --- */
static void set_restart_hint(ClientRecPtr c, int hint)
{
    SmProp **props = malloc(sizeof(SmProp *));
    SmProp *p = malloc(sizeof *p);
    p->name = strdup(SmRestartStyleHint); p->type = strdup(SmCARD8); p->num_vals = 1;
    p->vals = malloc(sizeof(SmPropValue));
    p->vals[0].length = 1; p->vals[0].value = malloc(1); *(char *) p->vals[0].value = (char) hint;
    props[0] = p;
    SetPropertiesProc(NULL, c, 1, props);
}
static int leak_register_restart(void)
{
    ClientRecPtr me;
    reset();
    me = new_client(NULL, 0);
    database("1saved");
    host_name_result = strdup("local/box:/tmp/.ICE-unix/42");
    CHECK(RegisterClientProc(NULL, me, strdup("1saved")) == 1);
    CHECK(!strcmp(me->clientHost, "box:/tmp/.ICE-unix/42"));
    free(replied_id); free(me->clientId); free(me->clientHost); free(me);
    free(smXSMP.xsmpDbList->clientId); free(smXSMP.xsmpDbList);
    return 0;
}
static int leak_register_new_client(void)
{
    ClientRecPtr me;
    reset();
    me = new_client(NULL, 0);
    host_name_result = strdup("tcp/far:6000");
    CHECK(RegisterClientProc(NULL, me, NULL) == 1);
    free(replied_id); free(me->clientId); free(me->clientHost); free(me);
    return 0;
}
static int leak_close_removes_a_client_that_wants_no_restart(void)
{
    ClientRecPtr a, b, c;
    SmProp **props;
    reset();
    a = new_client("1a", 1); connect_client(a); a->clientHost = strdup("box");
    b = new_client("1b", 1); connect_client(b);
    c = new_client("1c", 1); connect_client(c);
    set_restart_hint(b, SmRestartNever);
    props = malloc(sizeof(SmProp *)); props[0] = make_property("Program", "xclock");
    SetPropertiesProc(NULL, b, 1, props);
    smXSMP.saveState.interactClient = b;
    CloseDownClient(b);
    CHECK(connectedList == a && a->next == c && c->next == NULL);   /* b is out of the list... */
    CHECK(smXSMP.saveState.interactClient == NULL);                  /* ...nothing points to it... */
    CHECK(restart_commands == 0);
    CloseDownClient(a);                                              /* a restart hint of "if running": the record stays */
    CHECK(connectedList == a && !a->active && a->smConn == NULL);
    free(a->clientId); free(a->clientHost); free(a); free(c->clientId); free(c);
    return 0;
}
static int first_in_the_list_is_removed(void)
{
    ClientRecPtr a, b;
    reset();
    a = new_client("1a", 1); connect_client(a);
    b = new_client("1b", 1); connect_client(b);
    set_restart_hint(a, SmRestartNever);
    CloseDownClient(a);
    CHECK(connectedList == b && b->next == NULL);
    return 0;
}
static int client_that_asked_to_interact_disconnects(void)
{
    int nums[1] = {1};
    ClientRecPtr a, b;
    int hint;
    for (hint = 0; hint < 2; hint++) {                              /* a record that stays, and one that is removed */
        reset();
        a = new_client("1a", 1); connect_client(a); a->interactRequested = 1;
        b = new_client("1b", 1); connect_client(b); b->interactRequested = 1;
        if (hint) set_restart_hint(a, SmRestartNever);
        smXSMP.saveState.interactClient = a;                        /* the request that is waiting for the window manager */
        CloseDownClient(a);
        CHECK(smXSMP.saveState.interactClient == NULL);
        reply(3, 1, "1b\0", 3, nums, sizeof nums);
        CHECK(GetWsmClientsHandler(NULL, NULL) == TT_CALLBACK_PROCESSED);
        CHECK(interact_null == 0);                                    /* nobody is told to interact on a closed connection */
        CHECK(interacts == 1 && interacted[0] == b->smConn);          /* the client that is still there is */
        connectedList = NULL;
    }
    return 0;
}
static int closed_client_before_the_interaction_without_workspaces(void)
{
    ClientRecPtr a;
    reset();
    a = new_client("1a", 1); connect_client(a); a->interactRequested = 1;
    smXSMP.saveState.interactClient = a;
    CloseDownClient(a);
    reply(2, 0, NULL, 0, NULL, 0);                                    /* the window manager did not answer properly */
    CHECK(GetWsmClientsHandler(NULL, NULL) == TT_CALLBACK_PROCESSED);
    CHECK(interact_null == 0 && interacts == 0);
    return 0;
}
static int shutdown_ends_with_the_last_client(void)
{
    ClientRecPtr a, b;
    reset();
    smXSMP.saveState.shutdown = 1;
    a = new_client("1a", 1); connect_client(a);
    b = new_client("1b", 1); connect_client(b);
    CloseDownClient(a);
    CHECK(exits == 0);
    CloseDownClient(b);
    CHECK(exits == 1);
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"leak_register_restart", leak_register_restart},
    {"leak_register_new_client", leak_register_new_client},
    {"leak_close_removes_a_client_that_wants_no_restart", leak_close_removes_a_client_that_wants_no_restart},
    {"first_in_the_list_is_removed", first_in_the_list_is_removed},
    {"client_that_asked_to_interact_disconnects", client_that_asked_to_interact_disconnects},
    {"closed_client_before_the_interaction_without_workspaces", closed_client_before_the_interaction_without_workspaces},
    {"shutdown_ends_with_the_last_client", shutdown_ends_with_the_last_client},
    {"restart_with_a_saved_id", restart_with_a_saved_id},
    {"connection_without_id_in_the_list", connection_without_id_in_the_list},
    {"unknown_and_reused_ids", unknown_and_reused_ids},
    {"new_client_gets_an_id", new_client_gets_an_id},
    {"host_name_without_transport", host_name_without_transport},
    {"wsm_reply_valid", wsm_reply_valid},
    {"wsm_reply_shorter_than_its_count", wsm_reply_shorter_than_its_count},
    {"wsm_reply_after_the_save_ended", wsm_reply_after_the_save_ended},
    {"interact_before_registration", interact_before_registration},
    {"delete_properties", delete_properties},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    if (argc < 2) { for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
with tempfile.TemporaryDirectory(prefix="dtsession-xsmp-", dir=os.environ.get("TMPDIR")) as scratch:
    scratch = Path(scratch)
    c = scratch / "xsmp.c"
    c.write_text(harness)
    binary = scratch / "xsmp"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-w", "-I", str(source), str(c), "-o", str(binary), "-lXt", "-lSM", "-lICE", "-lX11"]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr[:6000])
        sys.exit("cannot compile the XSMP harness")
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True, env=env).stdout.split()
    for name in names:
        # scenarios named leak_* free everything they allocate: what is left is the code's
        run_env = dict(os.environ, ASAN_OPTIONS="detect_leaks=%d" % name.startswith("leak_"))
        run = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=60, env=run_env)
        if run.returncode:
            failed.append(name)
            lines = [l for l in run.stderr.strip().splitlines() if l.strip()]
            sys.stderr.write("%s failed: %s\n" % (name, lines[0] if lines else "exit %d (signal %d)" % (run.returncode, -run.returncode)))
            if os.environ.get("DTSESSION_VERBOSE"):
                sys.stderr.write("\n".join(lines[:30]) + "\n")
if failed:
    sys.exit("%d of %d XSMP scenarios failed: %s" % (len(failed), len(names), ", ".join(failed)))
print("XSMP: %d scenarios passed" % len(names))
