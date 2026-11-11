/*
 * The Desktop requests that act on a whole process, and the process that
 * answers them or does not.
 *
 *   session-requests-test handle WORKDIR [REQUEST ...]
 *   session-requests-test send PROCID OP ...
 *   session-requests-test arguments
 *
 * All three join the session named by TT_SESSION.
 *
 * handle opens a connection, joins the session with ttdt_session_join() asking
 * for the REQUESTs named (get_locale, set_locale, get_environment,
 * set_environment, get_situation, set_situation, signal, get_sysinfo,
 * do_command, set_xinfo; none at all if there are no names) and a callback
 * that answers Do_Command by recording it, and prints
 * "ready PROCID locale=yes|no".  The word shell opens the display named by
 * $DISPLAY and joins with a realized application shell, the word contract
 * waits for a request Accept_Test instead and accepts it as a contract with
 * ttdt_message_accept() (set_xinfo is then the one request it can ask for).  The
 * working directory is WORKDIR, the variable SESSION_REQUESTS_PROBE is
 * "original" and SIGUSR1 is counted.  It serves its connection until its
 * standard input ends, then prints
 *   state env=VALUE cwd=DIRECTORY signals=N locale=NAME docommands=N probe2=VALUE probe3=VALUE display=VALUE connection=NAME
 * which says what happened to the process: $DISPLAY, and the display the
 * connection of the shell is to (none without a shell).
 *
 * send sends each request OP (the operation names get_locale ... do_command
 * as above), first with no handler named and then, if PROCID is not "-",
 * addressed to that handler, and prints for each
 *   result OP unaddressed|addressed STATE STATUS ARGUMENTS
 * STATE is handled, failed or rejected, STATUS ok, nomatch (no handler for the
 * request), enotsup (the handler failed it) or the number, and ARGUMENTS the
 * values the reply carries for the arguments after the first (all of them
 * for Get_Situation, whose one argument is the answer), if it was handled.
 *
 * contract PROCID sends the request Accept_Test, waits for the Status notice
 * of the process that accepted it, and sends Set_XInfo requests that carry the
 * message id of the contract, a valid one and one that is not; it prints
 * "contract NAME STATE STATUS" for each.
 *
 * xinfo PROCID sends the process values of Set_XInfo that it must not act on
 * (a display that is shell syntax or has no colon, a null byte inside it or a
 * thousand characters, a visual that is none, a depth of 0 or 64), then a
 * valid one, and prints "xinfo NAME STATE STATUS" for each.
 *
 * hostile PROCID sends the process, which asked for all the requests, values
 * that no message may make it act on (names of variables that are not names
 * or that no message may set, locales that name a path, directories that are
 * not absolute, signals that are none, strings with a null byte inside or of
 * absurd length) and prints "hostile NAME STATE STATUS" for each, then the
 * valid requests that remove a variable and set another.  The state of the
 * process afterwards says that the hostile ones changed nothing.
 *
 * arguments calls ttdt_session_join() with requests it must refuse (and one it
 * must not) and prints "join NAME STATUS" for each.
 *
 * TT_SESSION_REQUESTS_OLD_JOIN, defined when this is compiled, calls the
 * five argument ttdt_session_join() of a libtt from before the requests were
 * asked for; it is how the test is shown to fail on that library.
 * TT_SESSION_REQUESTS_OLD_XINFO does the same for a libtt from before Set_XInfo
 * was asked for: it never asks for it, and calls the six argument
 * ttdt_message_accept().
 */
#include <Tt/tttk.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define PROBE "SESSION_REQUESTS_PROBE"
#define PROBE2 "SESSION_REQUESTS_PROBE2"
#define PROBE3 "SESSION_REQUESTS_PROBE3"
#define XINFO_DISPLAY "xinfo.example.com:7.0"

#ifdef TT_SESSION_REQUESTS_OLD_JOIN
/* the header of that library does not have the requests: name them here */
typedef unsigned int Ttdt_session_requests;
enum {
    TTDT_SESSION_NO_REQUESTS	= 0,
    TTDT_SESSION_GET_LOCALE	= 1 << 0,
    TTDT_SESSION_SET_LOCALE	= 1 << 1,
    TTDT_SESSION_GET_ENVIRONMENT = 1 << 2,
    TTDT_SESSION_SET_ENVIRONMENT = 1 << 3,
    TTDT_SESSION_GET_SITUATION	= 1 << 4,
    TTDT_SESSION_SET_SITUATION	= 1 << 5,
    TTDT_SESSION_SIGNAL		= 1 << 6,
    TTDT_SESSION_GET_SYSINFO	= 1 << 7,
    TTDT_SESSION_DO_COMMAND	= 1 << 8,
    TTDT_SESSION_SET_XINFO	= 1 << 9
};
#endif

#ifdef TT_SESSION_REQUESTS_OLD_XINFO
/* the header of that library does not have Set_XInfo to ask for: name it here */
#define TTDT_SESSION_SET_XINFO	(1u << 9)
typedef unsigned int Ttdt_contract_requests;
enum {
    TTDT_CONTRACT_NO_REQUESTS	= 0,
    TTDT_CONTRACT_SET_XINFO	= 1 << 0
};
#endif

static const struct {
    const char *name;
    Ttdt_session_requests request;
    const char *op;
} requests[] = {
    { "get_locale",		TTDT_SESSION_GET_LOCALE,	"Get_Locale" },
    { "set_locale",		TTDT_SESSION_SET_LOCALE,	"Set_Locale" },
    { "get_environment",	TTDT_SESSION_GET_ENVIRONMENT,	"Get_Environment" },
    { "set_environment",	TTDT_SESSION_SET_ENVIRONMENT,	"Set_Environment" },
    { "get_situation",		TTDT_SESSION_GET_SITUATION,	"Get_Situation" },
    { "set_situation",		TTDT_SESSION_SET_SITUATION,	"Set_Situation" },
    { "signal",			TTDT_SESSION_SIGNAL,		"Signal" },
    { "get_sysinfo",		TTDT_SESSION_GET_SYSINFO,	"Get_Sysinfo" },
    { "do_command",		TTDT_SESSION_DO_COMMAND,	"Do_Command" },
    { "set_xinfo",		TTDT_SESSION_SET_XINFO,		"Set_XInfo" }
};
#define NREQUESTS ((int)(sizeof(requests) / sizeof(requests[0])))

static volatile sig_atomic_t signals;
static int do_commands;

static void
count_signal(int sig)
{
    (void)sig;
    signals++;
}

static Tt_pattern *
join(Ttdt_contract_cb cb, Ttdt_session_requests wanted, Widget shell)
{
#ifdef TT_SESSION_REQUESTS_OLD_JOIN
    (void)wanted;
    return ttdt_session_join(NULL, cb, shell, NULL, 1);
#else
#ifdef TT_SESSION_REQUESTS_OLD_XINFO
    wanted &= ~(Ttdt_session_requests)TTDT_SESSION_SET_XINFO;
#endif
    return ttdt_session_join(NULL, cb, shell, NULL, 1, wanted);
#endif
}

/* ttdt_message_accept() of this library or of the one from before Set_XInfo */
static int bad_contract_request;	/* ask for a request that is none */

static Tt_pattern *
accept_contract(Tt_message msg, Ttdt_contract_cb cb, Widget shell,
		Ttdt_session_requests wanted)
{
#ifdef TT_SESSION_REQUESTS_OLD_XINFO
    (void)wanted;
    return ttdt_message_accept(msg, cb, shell, NULL, 1, 1);
#else
    return ttdt_message_accept(msg, cb, shell, NULL, 1, 1,
			       bad_contract_request ? 1u << 20 :
			       (wanted & TTDT_SESSION_SET_XINFO) != 0 ?
			       TTDT_CONTRACT_SET_XINFO : TTDT_CONTRACT_NO_REQUESTS);
#endif
}

/*
 * The callback of the process.  It answers Do_Command by writing down that it
 * was asked; it never runs anything.
 */
static Tt_message
contract_cb(Tt_message msg, void *clientdata, Tt_message contract)
{
    char *op = tt_message_op(msg);
    Tttk_op which = tttk_string_op(op);

    (void)clientdata;
    (void)contract;
    tt_free(op);
    if (which != TTDT_DO_COMMAND)
	return msg;
    do_commands++;
    tt_message_arg_val_set(msg, 1, "recorded by the callback");
    tt_message_reply(msg);
    tttk_message_destroy(msg);
    return 0;
}

static int
by_name(const char *name)
{
    int i;

    for (i = 0; i < NREQUESTS; i++)
	if (strcmp(requests[i].name, name) == 0)
	    return i;
    return -1;
}

static const char *status_name(Tt_status s, char *buf, size_t size);

/* The display of the process and a realized shell on it, as a program has them. */
static Widget
open_shell(const char **connection)
{
    XtAppContext app;
    Display *dpy;
    Widget shell;
    int argc = 1;
    char *argv[] = { "session-requests-test", NULL };

    XtToolkitInitialize();
    app = XtCreateApplicationContext();
    dpy = XtOpenDisplay(app, NULL, "session-requests-test", "SessionRequestsTest",
			NULL, 0, &argc, argv);
    if (dpy == NULL)
	return (Widget)0;
    shell = XtAppCreateShell("session-requests-test", "SessionRequestsTest",
			     applicationShellWidgetClass, dpy, NULL, 0);
    XtRealizeWidget(shell);
    *connection = DisplayString(dpy);
    return shell;
}

/*
 * Serve the connection of a process that accepts a contract: the request
 * Accept_Test is accepted with ttdt_message_accept(), and what is addressed to
 * the process in its name is left to the patterns that makes.
 */
static void
serve_contract(Widget shell, Ttdt_session_requests wanted, int fd)
{
    for (;;) {
	struct pollfd pfds[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
	Tt_message msg;

	if (poll(pfds, 2, 250) < 0 && errno != EINTR)
	    break;
	if (pfds[1].revents & (POLLIN | POLLHUP))
	    break;
	while ((msg = tt_message_receive()) != NULL && tt_ptr_error(msg) == TT_OK) {
	    char *op = tt_message_op(msg);

	    if (op != NULL && strcmp(op, "Accept_Test") == 0 &&
		tt_message_state(msg) == TT_SENT) {
		Tt_pattern *pats = accept_contract(msg, contract_cb, shell, wanted);
		char buf[16];

		printf("accepted %s\n", status_name(tt_ptr_error(pats), buf, sizeof buf));
		fflush(stdout);
	    } else {
		tttk_message_abandon(msg);
	    }
	    tt_free(op);
	}
    }
}

static int
handle(int argc, char **argv)
{
    Ttdt_session_requests wanted = 0;
    struct sigaction act;
    char *procid;
    Tt_pattern *pats;
    char cwd[4096];
    const char *locale, *connection = "(none)";
    Widget shell = (Widget)0;
    int fd, i, can_set_locale, with_shell = 0, as_contract = 0;

    if (argc < 3) {
	fprintf(stderr, "usage: handle WORKDIR [shell] [contract] [REQUEST ...]\n");
	return 2;
    }
    for (i = 3; i < argc; i++) {
	int r;

	if (strcmp(argv[i], "shell") == 0) {
	    with_shell = 1;
	    continue;
	}
	if (strcmp(argv[i], "contract") == 0) {
	    as_contract = 1;
	    continue;
	}
	if (strcmp(argv[i], "bad-contract-request") == 0) {
	    bad_contract_request = 1;
	    continue;
	}
	r = by_name(argv[i]);
	if (r < 0) {
	    fprintf(stderr, "unknown request %s\n", argv[i]);
	    return 2;
	}
	wanted |= requests[r].request;
    }
    if (chdir(argv[2]) != 0) {
	perror(argv[2]);
	return 2;
    }
    setenv(PROBE, "original", 1);
    memset(&act, 0, sizeof act);
    act.sa_handler = count_signal;
    sigemptyset(&act.sa_mask);
    sigaction(SIGUSR1, &act, NULL);
    setlocale(LC_ALL, "C");
    can_set_locale = setlocale(LC_CTYPE, "C.UTF-8") != NULL;
    setlocale(LC_ALL, "C");

    procid = ttdt_open(&fd, "session-requests-test", "CDE", "1.0", 0);
    if (tt_ptr_error(procid) != TT_OK) {
	fprintf(stderr, "ttdt_open: %s\n", tt_status_message(tt_ptr_error(procid)));
	return 2;
    }
    if (with_shell) {
	shell = open_shell(&connection);
	if (shell == (Widget)0) {
	    fprintf(stderr, "cannot open the display\n");
	    return 2;
	}
    }
    if (as_contract) {
	/* the contract is joined to the session by hand: nothing else is asked */
	pats = NULL;
	if (tt_session_join(tt_default_session()) != TT_OK) {
	    fprintf(stderr, "cannot join the session\n");
	    return 2;
	}
    } else {
	pats = join(contract_cb, wanted, shell);
	if (tt_ptr_error(pats) != TT_OK) {
	    fprintf(stderr, "ttdt_session_join: %s\n",
		    tt_status_message(tt_ptr_error(pats)));
	    return 2;
	}
    }
    printf("ready %s locale=%s\n", procid, can_set_locale ? "yes" : "no");
    fflush(stdout);

    if (as_contract) {
	serve_contract(shell, wanted, fd);
    } else {
	for (;;) {
	    struct pollfd pfds[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };

	    if (poll(pfds, 2, 250) < 0 && errno != EINTR)
		break;
	    if (pfds[1].revents & (POLLIN | POLLHUP))
		break;
	    /* what a client does with its connection */
	    tttk_Xt_input_handler(NULL, &fd, NULL);
	}
    }

    locale = setlocale(LC_CTYPE, NULL);
    printf("state env=%s cwd=%s signals=%d locale=%s docommands=%d probe2=%s probe3=%s display=%s connection=%s\n",
	   getenv(PROBE) ? getenv(PROBE) : "(unset)",
	   getcwd(cwd, sizeof cwd) ? cwd : "(none)",
	   (int)signals, locale ? locale : "(none)", do_commands,
	   getenv(PROBE2) ? getenv(PROBE2) : "(unset)",
	   getenv(PROBE3) ? getenv(PROBE3) : "(unset)",
	   getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)", connection);
    fflush(stdout);
    if (pats != NULL)
	ttdt_session_quit(NULL, pats, 1);
    ttdt_close(NULL, NULL, 0);
    return 0;
}

static const char *
state_name(Tt_state s)
{
    switch (s) {
    case TT_HANDLED:	return "handled";
    case TT_FAILED:	return "failed";
    case TT_REJECTED:	return "rejected";
    default:		return "other";
    }
}

static const char *
status_name(Tt_status s, char *buf, size_t size)
{
    switch (s) {
    case TT_OK:			return "ok";
    case TT_ERR_NO_MATCH:	return "nomatch";
    case TT_DESKTOP_ENOTSUP:	return "enotsup";
    case TT_DESKTOP_EACCES:	return "eacces";
    case TT_DESKTOP_EINVAL:	return "einval";
    case TT_DESKTOP_EPROTO:	return "eproto";
    case TT_DESKTOP_ENOENT:	return "enoent";
    case TT_ERR_NUM:		return "num";
    case TT_ERR_POINTER:	return "pointer";
    default:
	snprintf(buf, size, "%d", (int)s);
	return buf;
    }
}

/* Make the request OP with the arguments the Desktop interface gives it. */
static Tt_message
make_request(int r, const char *handler, const char *workdir)
{
    Tt_message msg = tt_message_create();
    const char *op = requests[r].op;
    int i;

    tt_message_class_set(msg, TT_REQUEST);
    tt_message_scope_set(msg, TT_SESSION);
    if (handler != NULL) {
	tt_message_address_set(msg, TT_HANDLER);
	tt_message_handler_set(msg, handler);
    } else {
	tt_message_address_set(msg, TT_PROCEDURE);
    }
    tt_message_op_set(msg, op);
    switch (requests[r].request) {
    case TTDT_SESSION_GET_LOCALE:
	tt_message_arg_add(msg, TT_IN, Tttk_string, "LC_CTYPE");
	tt_message_arg_add(msg, TT_OUT, Tttk_string, NULL);
	break;
    case TTDT_SESSION_SET_LOCALE:
	tt_message_arg_add(msg, TT_IN, Tttk_string, "LC_CTYPE");
	tt_message_arg_add(msg, TT_IN, Tttk_string, "C.UTF-8");
	break;
    case TTDT_SESSION_GET_ENVIRONMENT:
	tt_message_arg_add(msg, TT_IN, Tttk_string, PROBE);
	tt_message_arg_add(msg, TT_OUT, Tttk_string, NULL);
	break;
    case TTDT_SESSION_SET_ENVIRONMENT:
	tt_message_arg_add(msg, TT_IN, Tttk_string, PROBE);
	tt_message_arg_add(msg, TT_IN, Tttk_string, "changed");
	break;
    case TTDT_SESSION_GET_SITUATION:
	tt_message_arg_add(msg, TT_OUT, Tttk_string, NULL);
	break;
    case TTDT_SESSION_SET_SITUATION:
	tt_message_arg_add(msg, TT_IN, Tttk_string, workdir);
	break;
    case TTDT_SESSION_SIGNAL:
	tt_message_iarg_add(msg, TT_IN, Tttk_integer, SIGUSR1);
	break;
    case TTDT_SESSION_GET_SYSINFO:
	for (i = 0; i < 8; i++)
	    tt_message_arg_add(msg, TT_OUT, Tttk_string, NULL);
	break;
    case TTDT_SESSION_DO_COMMAND:
	tt_message_arg_add(msg, TT_IN, Tttk_string, "not a command to run");
	tt_message_arg_add(msg, TT_OUT, Tttk_string, NULL);
	break;
    case TTDT_SESSION_SET_XINFO:
	/* the arguments of ttdt_Set_XInfo(): display, visual, depth */
	tt_message_arg_add(msg, TT_IN, Tttk_string, XINFO_DISPLAY);
	tt_message_arg_add(msg, TT_IN, Tttk_string, "TrueColor");
	tt_message_iarg_add(msg, TT_IN, Tttk_integer, 24);
	break;
    }
    return msg;
}

/* Wait for the reply to, or the failure of, a request that was sent. */
static Tt_message
exchange(Tt_message sent, int fd)
{
    int rounds;

    for (rounds = 0; rounds < 40; rounds++) {
	struct pollfd pfd = { fd, POLLIN, 0 };
	Tt_message got;

	if (poll(&pfd, 1, 250) <= 0)
	    continue;
	while ((got = tt_message_receive()) != NULL &&
	       tt_ptr_error(got) == TT_OK) {
	    Tt_state state;

	    if (got != sent) {
		tt_message_destroy(got);
		continue;
	    }
	    state = tt_message_state(got);
	    if (state == TT_HANDLED || state == TT_FAILED ||
		state == TT_REJECTED)
		return got;
	}
    }
    return NULL;
}

static void
send_one(int r, const char *handler, const char *workdir, int fd)
{
    Tt_message sent = make_request(r, handler, workdir);
    Tt_status status = tt_message_send(sent);
    Tt_message got;
    Tt_state state;
    char buf[16], values[1024] = "";
    int n, i;

    if (status != TT_OK) {
	printf("result %s %s failed send:%d\n", requests[r].op,
	       handler ? "addressed" : "unaddressed", (int)status);
	return;
    }
    got = exchange(sent, fd);
    if (got == NULL) {
	printf("result %s %s timeout -\n", requests[r].op,
	       handler ? "addressed" : "unaddressed");
	return;
    }
    state = tt_message_state(got);
    /* the answers: after the question, and all of Get_Situation's */
    n = state == TT_HANDLED ? tt_message_args_count(got) : 0;
    for (i = requests[r].request == TTDT_SESSION_GET_SITUATION ? 0 : 1;
	 i < n; i++) {
	char *v = tt_message_arg_val(got, i);

	if (tt_ptr_error(v) == TT_OK && v != NULL) {
	    size_t used = strlen(values);

	    snprintf(values + used, sizeof values - used, " %s", v);
	    tt_free(v);
	}
    }
    printf("result %s %s %s %s%s\n", requests[r].op,
	   handler ? "addressed" : "unaddressed", state_name(state),
	   status_name(tt_message_status(got), buf, sizeof buf), values);
    fflush(stdout);
    tt_message_destroy(got);
}

static int
send_requests(int argc, char **argv)
{
    char workdir[4096];
    int fd, i;

    if (argc < 4) {
	fprintf(stderr, "usage: send PROCID OP ...\n");
	return 2;
    }
    if (tt_ptr_error(tt_open()) != TT_OK) {
	fprintf(stderr, "tt_open failed\n");
	return 2;
    }
    fd = tt_fd();
    if (tt_session_join(tt_default_session()) != TT_OK) {
	fprintf(stderr, "cannot join the session\n");
	return 2;
    }
    /* Set_Situation goes to the directory the sender is in */
    if (getcwd(workdir, sizeof workdir) == NULL) {
	perror("getcwd");
	return 2;
    }
    for (i = 3; i < argc; i++) {
	int r = by_name(argv[i]);

	if (r < 0) {
	    fprintf(stderr, "unknown request %s\n", argv[i]);
	    return 2;
	}
	send_one(r, NULL, workdir, fd);
	if (strcmp(argv[2], "-") != 0)
	    send_one(r, argv[2], workdir, fd);
    }
    tt_close();
    return 0;
}

/*
 * Requests with values that no message may make a process act on.  Each
 * name stands for one request and the values it carries.
 */
static void
str(Tt_message m, const char *s)
{
    tt_message_arg_add(m, TT_IN, Tttk_string, s);
}

static void
bytes(Tt_message m, const char *s, int len)
{
    tt_message_barg_add(m, TT_IN, Tttk_string, (const unsigned char *)s, len);
}

/* a string of n copies of c */
static void
repeated(Tt_message m, char c, int n)
{
    char *s = malloc(n + 1);

    memset(s, c, n);
    s[n] = '\0';
    str(m, s);
    free(s);
}

static const struct {
    const char *name;
    const char *op;
} hostile_cases[] = {
    { "env-ld-preload",		"Set_Environment" },
    { "env-path",		"Set_Environment" },
    { "env-tt-prefix",		"Set_Environment" },
    { "env-name-digit",		"Set_Environment" },
    { "env-name-equals",	"Set_Environment" },
    { "env-name-empty",		"Set_Environment" },
    { "env-name-blank",		"Set_Environment" },
    { "env-name-long",		"Set_Environment" },
    { "env-name-nul",		"Set_Environment" },
    { "env-value-long",		"Set_Environment" },
    { "env-value-nul",		"Set_Environment" },
    { "env-mixed",		"Set_Environment" },
    { "env-many",		"Set_Environment" },
    { "env-get-name-blank",	"Get_Environment" },
    { "locale-path",		"Set_Locale" },
    { "locale-dots",		"Set_Locale" },
    { "locale-empty",		"Set_Locale" },
    { "locale-category",	"Set_Locale" },
    { "locale-long",		"Set_Locale" },
    { "locale-nul",		"Set_Locale" },
    { "locale-many",		"Set_Locale" },
    { "situation-relative",	"Set_Situation" },
    { "situation-control",	"Set_Situation" },
    { "situation-long",		"Set_Situation" },
    { "situation-nul",		"Set_Situation" },
    { "situation-missing",	"Set_Situation" },
    { "signal-zero",		"Signal" },
    { "signal-huge",		"Signal" },
    { "signal-negative",	"Signal" },
    /* valid, and the last: they take the variable away and put another in */
    { "env-remove",		"Set_Environment" },
    { "env-valid",		"Set_Environment" }
};
#define NHOSTILE ((int)(sizeof(hostile_cases) / sizeof(hostile_cases[0])))

static void
fill_hostile(const char *name, Tt_message m)
{
    int i;
    char var[32];

    if (strcmp(name, "env-ld-preload") == 0) {
	str(m, "LD_PRELOAD"); str(m, "/tmp/evil.so");
    } else if (strcmp(name, "env-path") == 0) {
	str(m, "PATH"); str(m, "/tmp");
    } else if (strcmp(name, "env-tt-prefix") == 0) {
	str(m, "TT_SESSION"); str(m, "x");
    } else if (strcmp(name, "env-name-digit") == 0) {
	str(m, "1BAD"); str(m, "v");
    } else if (strcmp(name, "env-name-equals") == 0) {
	str(m, "A=B"); str(m, "v");
    } else if (strcmp(name, "env-name-empty") == 0) {
	str(m, ""); str(m, "v");
    } else if (strcmp(name, "env-name-blank") == 0) {
	str(m, "A B"); str(m, "v");
    } else if (strcmp(name, "env-name-long") == 0) {
	repeated(m, 'A', 300); str(m, "v");
    } else if (strcmp(name, "env-name-nul") == 0) {
	bytes(m, "FOO\0BAR", 7); str(m, "v");
    } else if (strcmp(name, "env-value-long") == 0) {
	str(m, PROBE2); repeated(m, 'v', 40000);
    } else if (strcmp(name, "env-value-nul") == 0) {
	str(m, PROBE2); bytes(m, "ab\0cd", 5);
    } else if (strcmp(name, "env-mixed") == 0) {
	/* the first is fine, the second no message may set: nothing is set */
	str(m, PROBE2); str(m, "set");
	str(m, "LD_LIBRARY_PATH"); str(m, "/tmp");
    } else if (strcmp(name, "env-many") == 0) {
	for (i = 0; i < 200; i++) {
	    snprintf(var, sizeof var, "SESSION_REQUESTS_MANY%d", i);
	    str(m, var); str(m, "v");
	}
    } else if (strcmp(name, "env-get-name-blank") == 0) {
	str(m, "A B"); tt_message_arg_add(m, TT_OUT, Tttk_string, NULL);
    } else if (strcmp(name, "locale-path") == 0) {
	str(m, "LC_CTYPE"); str(m, "/tmp/evil");
    } else if (strcmp(name, "locale-dots") == 0) {
	str(m, "LC_CTYPE"); str(m, "..");
    } else if (strcmp(name, "locale-empty") == 0) {
	str(m, "LC_CTYPE"); str(m, "");
    } else if (strcmp(name, "locale-category") == 0) {
	str(m, "LC_FOO"); str(m, "C");
    } else if (strcmp(name, "locale-long") == 0) {
	str(m, "LC_CTYPE"); repeated(m, 'a', 2000);
    } else if (strcmp(name, "locale-nul") == 0) {
	str(m, "LC_CTYPE"); bytes(m, "C\0x", 3);
    } else if (strcmp(name, "locale-many") == 0) {
	for (i = 0; i < 20; i++) {
	    str(m, "LC_CTYPE"); str(m, "C");
	}
    } else if (strcmp(name, "situation-relative") == 0) {
	str(m, "relative/dir");
    } else if (strcmp(name, "situation-control") == 0) {
	str(m, "/tmp/a\nb");
    } else if (strcmp(name, "situation-long") == 0) {
	repeated(m, 'd', 5000);
    } else if (strcmp(name, "situation-nul") == 0) {
	bytes(m, "/tmp\0/x", 7);
    } else if (strcmp(name, "situation-missing") == 0) {
	str(m, "/nonexistent-directory-of-the-session-requests-test");
    } else if (strcmp(name, "signal-zero") == 0) {
	tt_message_iarg_add(m, TT_IN, Tttk_integer, 0);
    } else if (strcmp(name, "signal-huge") == 0) {
	tt_message_iarg_add(m, TT_IN, Tttk_integer, 99999);
    } else if (strcmp(name, "signal-negative") == 0) {
	tt_message_iarg_add(m, TT_IN, Tttk_integer, -5);
    } else if (strcmp(name, "env-remove") == 0) {
	/* a variable without a value is removed */
	str(m, PROBE); tt_message_arg_add(m, TT_IN, Tttk_string, NULL);
    } else if (strcmp(name, "env-valid") == 0) {
	str(m, PROBE3); str(m, "valid");
    }
}

static int
hostile(int argc, char **argv)
{
    int fd, c;

    if (argc < 3) {
	fprintf(stderr, "usage: hostile PROCID\n");
	return 2;
    }
    if (tt_ptr_error(tt_open()) != TT_OK) {
	fprintf(stderr, "tt_open failed\n");
	return 2;
    }
    fd = tt_fd();
    if (tt_session_join(tt_default_session()) != TT_OK) {
	fprintf(stderr, "cannot join the session\n");
	return 2;
    }
    for (c = 0; c < NHOSTILE; c++) {
	Tt_message sent = tt_message_create(), got;
	Tt_status status;
	char buf[16];

	tt_message_class_set(sent, TT_REQUEST);
	tt_message_scope_set(sent, TT_SESSION);
	tt_message_address_set(sent, TT_HANDLER);
	tt_message_handler_set(sent, argv[2]);
	tt_message_op_set(sent, hostile_cases[c].op);
	fill_hostile(hostile_cases[c].name, sent);
	status = tt_message_send(sent);
	if (status != TT_OK) {
	    printf("hostile %s failed send:%d\n", hostile_cases[c].name,
		   (int)status);
	    continue;
	}
	got = exchange(sent, fd);
	if (got == NULL) {
	    printf("hostile %s timeout -\n", hostile_cases[c].name);
	    continue;
	}
	printf("hostile %s %s %s\n", hostile_cases[c].name,
	       state_name(tt_message_state(got)),
	       status_name(tt_message_status(got), buf, sizeof buf));
	fflush(stdout);
	tt_message_destroy(got);
    }
    tt_close();
    return 0;
}

/*
 * Set_XInfo with values: each name stands for one set of them.  The first is
 * the display, the second the visual and the third the depth; a null display
 * or visual is an argument without a value, a depth of -1 is none.
 */
static void
fill_xinfo(const char *name, Tt_message m)
{
    const char *display = XINFO_DISPLAY, *visual = "TrueColor";
    int depth = 24;

    if (strcmp(name, "garbage") == 0)
	display = "x:0; touch /tmp/evil";
    else if (strcmp(name, "nocolon") == 0)
	display = "hostname";
    else if (strcmp(name, "unset") == 0)
	display = NULL;
    else if (strcmp(name, "visual") == 0)
	visual = "Fancy";
    else if (strcmp(name, "depth-zero") == 0)
	depth = 0;
    else if (strcmp(name, "depth-big") == 0)
	depth = 64;
    if (strcmp(name, "nul") == 0) {
	tt_message_barg_add(m, TT_IN, Tttk_string, (const unsigned char *)"x:0\0y", 5);
    } else if (strcmp(name, "long") == 0) {
	char *d = malloc(1001);

	memset(d, 'h', 1000);
	d[998] = ':';
	d[1000] = '\0';
	tt_message_arg_add(m, TT_IN, Tttk_string, d);
	free(d);
    } else {
	tt_message_arg_add(m, TT_IN, Tttk_string, display);
    }
    if (strcmp(name, "visual-long") == 0) {
	char *v = malloc(101);

	memset(v, 'v', 100);
	v[100] = '\0';
	tt_message_arg_add(m, TT_IN, Tttk_string, v);
	free(v);
    } else {
	tt_message_arg_add(m, TT_IN, Tttk_string, visual);
    }
    tt_message_iarg_add(m, TT_IN, Tttk_integer, depth);
}

static const char *xinfo_cases[] = {
    "garbage", "nocolon", "nul", "long", "unset", "visual", "visual-long",
    "depth-zero", "depth-big", "valid", NULL
};

/* Send a request and print what became of it: "KEY NAME STATE STATUS". */
static void
send_and_print(const char *key, const char *name, Tt_message sent, int fd)
{
    Tt_status status = tt_message_send(sent);
    Tt_message got;
    char buf[16];

    if (status != TT_OK) {
	printf("%s %s failed send:%d\n", key, name, (int)status);
	return;
    }
    got = exchange(sent, fd);
    if (got == NULL) {
	printf("%s %s timeout -\n", key, name);
	return;
    }
    printf("%s %s %s %s\n", key, name, state_name(tt_message_state(got)),
	   status_name(tt_message_status(got), buf, sizeof buf));
    fflush(stdout);
    tt_message_destroy(got);
}

static Tt_message
xinfo_request(const char *name, const char *handler, const char *contract_id)
{
    Tt_message m = tt_message_create();

    tt_message_class_set(m, TT_REQUEST);
    tt_message_scope_set(m, TT_SESSION);
    tt_message_address_set(m, TT_HANDLER);
    tt_message_handler_set(m, handler);
    tt_message_op_set(m, "Set_XInfo");
    fill_xinfo(name, m);
    if (contract_id != NULL)
	tt_message_arg_add(m, TT_IN, Tttk_message_id, contract_id);
    return m;
}

static int
connect_sender(int *fd)
{
    if (tt_ptr_error(tt_open()) != TT_OK) {
	fprintf(stderr, "tt_open failed\n");
	return -1;
    }
    *fd = tt_fd();
    if (tt_session_join(tt_default_session()) != TT_OK) {
	fprintf(stderr, "cannot join the session\n");
	return -1;
    }
    return 0;
}

static int
xinfo(int argc, char **argv)
{
    int fd, c;

    if (argc < 3) {
	fprintf(stderr, "usage: xinfo PROCID\n");
	return 2;
    }
    if (connect_sender(&fd) != 0)
	return 2;
    for (c = 0; xinfo_cases[c] != NULL; c++)
	send_and_print("xinfo", xinfo_cases[c], xinfo_request(xinfo_cases[c], argv[2], NULL), fd);
    tt_close();
    return 0;
}

/*
 * A contract: Accept_Test is accepted by the process, which sends a Status
 * notice to say so, and the Set_XInfo that follow carry the id of the contract.
 */
static int
contract(int argc, char **argv)
{
    int fd, rounds;
    Tt_message sent, got;
    char *id;
    int accepted = 0;

    if (argc < 3) {
	fprintf(stderr, "usage: contract PROCID\n");
	return 2;
    }
    if (connect_sender(&fd) != 0)
	return 2;
    sent = tt_message_create();
    tt_message_class_set(sent, TT_REQUEST);
    tt_message_scope_set(sent, TT_SESSION);
    tt_message_address_set(sent, TT_HANDLER);
    tt_message_handler_set(sent, argv[2]);
    tt_message_op_set(sent, "Accept_Test");
    if (tt_message_send(sent) != TT_OK) {
	fprintf(stderr, "cannot send the contract\n");
	return 2;
    }
    id = tt_message_id(sent);
    for (rounds = 0; rounds < 40 && !accepted; rounds++) {
	struct pollfd pfd = { fd, POLLIN, 0 };

	if (poll(&pfd, 1, 250) <= 0)
	    continue;
	while ((got = tt_message_receive()) != NULL && tt_ptr_error(got) == TT_OK) {
	    char *op = tt_message_op(got);

	    if (op != NULL && strcmp(op, "Status") == 0)
		accepted = 1;
	    tt_free(op);
	    if (got != sent)
		tt_message_destroy(got);
	}
    }
    if (!accepted)
	printf("contract accept timeout -\n");
    send_and_print("contract", "valid", xinfo_request("valid", argv[2], id), fd);
    send_and_print("contract", "garbage", xinfo_request("garbage", argv[2], id), fd);
    tt_free(id);
    tt_close();
    return 0;
}

/* Arguments ttdt_session_join() must refuse, and the statuses it refuses with. */
static int
refuse(void)
{
#ifdef TT_SESSION_REQUESTS_OLD_JOIN
    printf("skipped old library\n");
    return 0;
#else
    int fd;
    char *procid = ttdt_open(&fd, "session-requests-test", "CDE", "1.0", 0);
    Tt_pattern *pats;
    char buf[16];

    if (tt_ptr_error(procid) != TT_OK) {
	fprintf(stderr, "ttdt_open: %s\n", tt_status_message(tt_ptr_error(procid)));
	return 2;
    }
    pats = join(NULL, TTDT_SESSION_DO_COMMAND, (Widget)0);
    printf("join do_command-without-callback %s\n",
	   status_name(tt_ptr_error(pats), buf, sizeof buf));
    pats = join(contract_cb, 1u << 20, (Widget)0);
    printf("join unknown-request %s\n",
	   status_name(tt_ptr_error(pats), buf, sizeof buf));
    pats = join(NULL, TTDT_SESSION_SET_XINFO, (Widget)0);
    printf("join set_xinfo-without-callback-or-shell %s\n",
	   status_name(tt_ptr_error(pats), buf, sizeof buf));
    pats = join(contract_cb, TTDT_SESSION_SET_XINFO, (Widget)0);
    printf("join set_xinfo-with-callback %s\n",
	   status_name(tt_ptr_error(pats), buf, sizeof buf));
    if (tt_ptr_error(pats) == TT_OK)
	ttdt_session_quit(NULL, pats, 1);
    pats = join(NULL, TTDT_SESSION_NO_REQUESTS, (Widget)0);
    printf("join no-requests-without-callback %s\n",
	   status_name(tt_ptr_error(pats), buf, sizeof buf));
    if (tt_ptr_error(pats) == TT_OK)
	ttdt_session_quit(NULL, pats, 1);
    ttdt_close(NULL, NULL, 0);
    return 0;
#endif
}

int
main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "handle") == 0)
	return handle(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "send") == 0)
	return send_requests(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "hostile") == 0)
	return hostile(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "xinfo") == 0)
	return xinfo(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "contract") == 0)
	return contract(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "arguments") == 0)
	return refuse();
    fprintf(stderr, "usage: %s handle|send|hostile|xinfo|contract|arguments ...\n", argv[0]);
    return 2;
}
