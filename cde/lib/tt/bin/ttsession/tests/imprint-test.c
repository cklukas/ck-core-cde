/*
 * ttdt_sender_imprint_on(): what it asks for, what it believes and what it
 * changes in the calling process.
 *
 *   imprint-test serve SCENARIO
 *   imprint-test imprint PROCID WORD ...
 *
 * Both join the session named by TT_SESSION.
 *
 * serve is the handler.  It opens a connection, prints "ready PROCID", and
 * answers the requests addressed to it until its standard input ends, writing
 * "asked OPERATION" for each.  Its answers are those of SCENARIO:
 *   good		a locale (C.UTF-8 for every category), the directory in
 *			$IMPRINT_TEST_DIR, the display other.example.com:5.0 and
 *			the geometry 400x300 at 20,30
 *   refuse		fails every request with TT_DESKTOP_ENOTSUP
 *   silent		never answers
 *   late		answers as good does, after 1500 ms
 *   no-situation	good, but fails Get_Situation
 *   and answers as good does except for one that cannot be used:
 *   locale-path	"/tmp/evil" as the locale of LC_CTYPE
 *   locale-dots	".."
 *   locale-nul		a locale with a null byte inside
 *   locale-long	a locale of 2000 characters
 *   locale-blank	a locale with a blank in it
 *   cwd-relative	"tmp/evil" as the directory
 *   cwd-nul		a directory with a null byte inside
 *   cwd-control	a directory with a newline in it
 *   cwd-long		a directory of 5000 characters
 *   display-garbage	"x:0; touch /tmp/evil" as the display
 *   display-nocolon	"hostname"
 *   display-nul	a display with a null byte inside
 *   display-long	a display of 1000 characters
 *   geometry-wild	a width of 1 << 30
 *
 * imprint calls ttdt_sender_imprint_on() with the handler PROCID ("-": none,
 * and no contract) and prints
 *   imprint STATUS display=NAME|(none) geometry=WIDTH,HEIGHT,X,Y
 *   state cwd=DIRECTORY locale=NAME display=NAME
 * where the state is that of the calling process afterwards.  The process
 * begins in the directory $IMPRINT_TEST_START, in the locale "C" and with
 * $DISPLAY original.example.com:0.  The WORDs say what is passed:
 *   locale situation display	the imprints of that name
 *   unknown-imprint		a value that is none of them
 *   want-display		a place to return the display in
 *   want-geometry		places to return the geometry in
 *   timeout=MS			the time to wait for the replies (default 5000)
 *   linger=MS			go on serving the connection for MS more
 *				milliseconds after the call has returned (a late
 *				reply arrives then)
 * The first line also says utf8=yes|no, whether the system has the locale
 * C.UTF-8 that the handler of the scenario good answers with.
 *
 * TT_IMPRINT_TEST_OLD_CALL, defined when this is compiled, calls the
 * ttdt_sender_imprint_on() of a libtt from before it was given the imprints;
 * it is how the test is shown to fail on that library.
 */
#include <Tt/tttk.h>

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef TT_IMPRINT_TEST_OLD_CALL
/* the header of a libtt from before the imprints does not have them: name them here */
typedef unsigned int Ttdt_sender_imprints;
enum {
    TTDT_IMPRINT_NONE		= 0,
    TTDT_IMPRINT_LOCALE		= 1 << 0,
    TTDT_IMPRINT_SITUATION	= 1 << 1,
    TTDT_IMPRINT_DISPLAY	= 1 << 2
};
#endif

#define START_DIR "IMPRINT_TEST_START"
#define TARGET_DIR "IMPRINT_TEST_DIR"

static const char *scenario;

static int
is(const char *name)
{
    return strcmp(scenario, name) == 0;
}

/* a string of n copies of c, ended by a null byte */
static char *
filled(char c, int n)
{
    char *s = malloc(n + 1);

    memset(s, c, n);
    s[n] = '\0';
    return s;
}

/* Set argument n of msg to the first len bytes of value. */
static void
set_bytes(Tt_message msg, int n, const char *value, int len)
{
    tt_message_arg_bval_set(msg, n, (const unsigned char *)value, len);
}

static void
answer_locale(Tt_message msg)
{
    int n = tt_message_args_count(msg), i;

    for (i = 0; i + 1 < n; i += 2) {
	const char *locale = "C.UTF-8";

	/* the first category is LC_CTYPE */
	if (i == 0) {
	    if (is("locale-path"))
		locale = "/tmp/evil";
	    else if (is("locale-dots"))
		locale = "..";
	    else if (is("locale-blank"))
		locale = "C UTF-8";
	    else if (is("locale-nul")) {
		set_bytes(msg, i + 1, "C\0UTF-8", 7);
		continue;
	    } else if (is("locale-long")) {
		char *l = filled('a', 2000);

		tt_message_arg_val_set(msg, i + 1, l);
		free(l);
		continue;
	    }
	}
	tt_message_arg_val_set(msg, i + 1, locale);
    }
}

static void
answer_situation(Tt_message msg)
{
    const char *dir = getenv(TARGET_DIR);

    if (is("cwd-relative"))
	dir = "tmp/evil";
    else if (is("cwd-control"))
	dir = "/tmp/a\nb";
    else if (is("cwd-nul")) {
	set_bytes(msg, 0, "/tmp\0/evil", 10);
	return;
    } else if (is("cwd-long")) {
	char *d = filled('d', 5000);

	d[0] = '/';
	tt_message_arg_val_set(msg, 0, d);
	free(d);
	return;
    }
    tt_message_arg_val_set(msg, 0, dir);
}

static void
answer_xinfo(Tt_message msg)
{
    const char *display = "other.example.com:5.0";

    if (is("display-garbage"))
	display = "x:0; touch /tmp/evil";
    else if (is("display-nocolon"))
	display = "hostname";
    else if (is("display-nul")) {
	set_bytes(msg, 0, "x:0\0y", 5);
	tt_message_arg_val_set(msg, 1, "TrueColor");
	tt_message_arg_ival_set(msg, 2, 24);
	return;
    } else if (is("display-long")) {
	char *d = filled('h', 1000);

	d[998] = ':';
	tt_message_arg_val_set(msg, 0, d);
	free(d);
	tt_message_arg_val_set(msg, 1, "TrueColor");
	tt_message_arg_ival_set(msg, 2, 24);
	return;
    }
    tt_message_arg_val_set(msg, 0, display);
    tt_message_arg_val_set(msg, 1, "TrueColor");
    tt_message_arg_ival_set(msg, 2, 24);
}

static void
answer_geometry(Tt_message msg)
{
    tt_message_arg_ival_set(msg, 0, is("geometry-wild") ? 1 << 30 : 400);
    tt_message_arg_ival_set(msg, 1, 300);
    tt_message_arg_ival_set(msg, 2, 20);
    tt_message_arg_ival_set(msg, 3, 30);
}

static void
answer(Tt_message msg)
{
    char *op = tt_message_op(msg);
    int is_locale, is_situation, is_xinfo, is_geometry;

    if (op == NULL || tt_message_class(msg) != TT_REQUEST) {
	tt_message_destroy(msg);
	tt_free(op);
	return;
    }
    is_locale = strcmp(op, "Get_Locale") == 0;
    is_situation = strcmp(op, "Get_Situation") == 0;
    is_xinfo = strcmp(op, "Get_XInfo") == 0;
    is_geometry = strcmp(op, "Get_Geometry") == 0;
    if (!is_locale && !is_situation && !is_xinfo && !is_geometry) {
	printf("other %s\n", op);
	fflush(stdout);
	tt_free(op);
	tt_message_destroy(msg);
	return;
    }
    printf("asked %s\n", op);
    fflush(stdout);
    tt_free(op);
    if (is("silent")) {
	/* keeps the request, so that it stays unanswered */
	return;
    }
    if (is("late"))
	usleep(1500000);
    if (is("refuse") || (is("no-situation") && is_situation)) {
	tt_message_status_set(msg, TT_DESKTOP_ENOTSUP);
	tt_message_fail(msg);
	tt_message_destroy(msg);
	return;
    }
    if (is_locale)
	answer_locale(msg);
    else if (is_situation)
	answer_situation(msg);
    else if (is_xinfo)
	answer_xinfo(msg);
    else
	answer_geometry(msg);
    tt_message_reply(msg);
    tt_message_destroy(msg);
}

/* Serve the connection as a handler does until stop_fd, a descriptor that is readable at the end */
static void
serve_requests(int fd, int stop_fd)
{
    for (;;) {
	struct pollfd pfds[2] = { { fd, POLLIN, 0 }, { stop_fd, POLLIN, 0 } };
	Tt_message msg;

	if (poll(pfds, 2, 100) < 0 && errno != EINTR)
	    break;
	if (pfds[1].revents & (POLLIN | POLLHUP))
	    break;
	while ((msg = tt_message_receive()) != NULL &&
	       tt_ptr_error(msg) == TT_OK) {
	    if (tt_message_state(msg) == TT_SENT)
		answer(msg);
	    else
		tt_message_destroy(msg);
	}
    }
}

/* Serve the connection as a client does for some time: what no one waits for is dropped */
static void
serve_as_client(int fd, int milliseconds)
{
    int waited;

    for (waited = 0; waited < milliseconds; waited += 100) {
	struct pollfd pfd = { fd, POLLIN, 0 };

	if (poll(&pfd, 1, 100) > 0)
	    tttk_Xt_input_handler(NULL, &fd, NULL);
    }
}

static int
serve(int argc, char **argv)
{
    char *procid;
    Tt_pattern pat;
    int fd;
    static const char *ops[] = { "Get_Locale", "Get_Situation", "Get_XInfo",
				 "Get_Geometry" };
    unsigned int i;

    if (argc < 3) {
	fprintf(stderr, "usage: serve SCENARIO\n");
	return 2;
    }
    scenario = argv[2];
    procid = tt_open();
    if (tt_ptr_error(procid) != TT_OK) {
	fprintf(stderr, "tt_open failed\n");
	return 2;
    }
    fd = tt_fd();
    pat = tt_pattern_create();
    tt_pattern_category_set(pat, TT_HANDLE);
    tt_pattern_scope_add(pat, TT_SESSION);
    tt_pattern_class_add(pat, TT_REQUEST);
    for (i = 0; i < sizeof ops / sizeof ops[0]; i++)
	tt_pattern_op_add(pat, ops[i]);
    tt_pattern_session_add(pat, tt_default_session());
    tt_pattern_register(pat);
    if (tt_session_join(tt_default_session()) != TT_OK) {
	fprintf(stderr, "cannot join the session\n");
	return 2;
    }
    printf("ready %s\n", procid);
    fflush(stdout);
    serve_requests(fd, 0);
    tt_close();
    return 0;
}

static const char *
status_name(Tt_status s, char *buf, size_t size)
{
    switch (s) {
    case TT_OK:			return "ok";
    case TT_ERR_NUM:		return "num";
    case TT_ERR_POINTER:	return "pointer";
    case TT_ERR_NO_MATCH:	return "nomatch";
    case TT_DESKTOP_ENOTSUP:	return "enotsup";
    case TT_DESKTOP_EPROTO:	return "eproto";
    case TT_DESKTOP_EINVAL:	return "einval";
    case TT_DESKTOP_ETIMEDOUT:	return "timedout";
    default:
	snprintf(buf, size, "%d", (int)s);
	return buf;
    }
}

static int
imprint(int argc, char **argv)
{
    Ttdt_sender_imprints imprints = TTDT_IMPRINT_NONE;
    int want_display = 0, want_geometry = 0, timeout = 5000, linger = 0;
    const char *handler;
    char *display = NULL, cwd[4096], buf[16];
    int w = 7, h = 7, x = 7, y = 7, i, fd, utf8;
    const char *locale, *shown_display;
    Tt_status status;

    if (argc < 3) {
	fprintf(stderr, "usage: imprint PROCID WORD ...\n");
	return 2;
    }
    handler = strcmp(argv[2], "-") == 0 ? NULL : argv[2];
    for (i = 3; i < argc; i++) {
	if (strcmp(argv[i], "locale") == 0)
	    imprints |= TTDT_IMPRINT_LOCALE;
	else if (strcmp(argv[i], "situation") == 0)
	    imprints |= TTDT_IMPRINT_SITUATION;
	else if (strcmp(argv[i], "display") == 0)
	    imprints |= TTDT_IMPRINT_DISPLAY;
	else if (strcmp(argv[i], "unknown-imprint") == 0)
	    imprints |= 1u << 20;
	else if (strcmp(argv[i], "want-display") == 0)
	    want_display = 1;
	else if (strcmp(argv[i], "want-geometry") == 0)
	    want_geometry = 1;
	else if (strncmp(argv[i], "timeout=", 8) == 0)
	    timeout = atoi(argv[i] + 8);
	else if (strncmp(argv[i], "linger=", 7) == 0)
	    linger = atoi(argv[i] + 7);
	else {
	    fprintf(stderr, "unknown word %s\n", argv[i]);
	    return 2;
	}
    }
    if (chdir(getenv(START_DIR)) != 0) {
	perror(START_DIR);
	return 2;
    }
    utf8 = setlocale(LC_CTYPE, "C.UTF-8") != NULL;
    setlocale(LC_ALL, "C");
    setenv("DISPLAY", "original.example.com:0", 1);
    if (tt_ptr_error(tt_open()) != TT_OK) {
	fprintf(stderr, "tt_open failed\n");
	return 2;
    }
    fd = tt_fd();
    if (tt_session_join(tt_default_session()) != TT_OK) {
	fprintf(stderr, "cannot join the session\n");
	return 2;
    }

#ifdef TT_IMPRINT_TEST_OLD_CALL
    /* the call of that library has no imprints: it adopts all it is told */
    status = ttdt_sender_imprint_on(handler, NULL,
				    want_display ? &display : NULL,
				    want_geometry ? &w : NULL,
				    want_geometry ? &h : NULL,
				    want_geometry ? &x : NULL,
				    want_geometry ? &y : NULL,
				    (XtAppContext)0, timeout);
#else
    status = ttdt_sender_imprint_on(handler, NULL, imprints,
				    want_display ? &display : NULL,
				    want_geometry ? &w : NULL,
				    want_geometry ? &h : NULL,
				    want_geometry ? &x : NULL,
				    want_geometry ? &y : NULL,
				    (XtAppContext)0, timeout);
#endif
    shown_display = display ? display : "(none)";
    printf("imprint %s display=%s geometry=%d,%d,%d,%d utf8=%s\n",
	   status_name(status, buf, sizeof buf), shown_display, w, h, x, y,
	   utf8 ? "yes" : "no");
    fflush(stdout);
    /* a reply that comes late meets whatever the call left behind */
    if (linger > 0)
	serve_as_client(fd, linger);
    locale = setlocale(LC_CTYPE, NULL);
    printf("state cwd=%s locale=%s display=%s\n",
	   getcwd(cwd, sizeof cwd) ? cwd : "(none)", locale ? locale : "(none)",
	   getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
    fflush(stdout);
    tt_free(display);
    tt_close();
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "serve") == 0)
	return serve(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "imprint") == 0)
	return imprint(argc, argv);
    fprintf(stderr, "usage: %s serve|imprint ...\n", argv[0]);
    return 2;
}
