/*
 * Send a notice with the disposition TT_QUEUE to a session and print what an
 * observer of it receives.
 *
 *   notice-test observe OP SECONDS
 *   notice-test send OP SECONDS
 *
 * Both join the session named by TT_SESSION.  The observer registers a pattern
 * for the operation OP in the scope of the session and prints a line
 * "observed STATE" (the state of the message as a name) for every message the
 * pattern delivers, for SECONDS seconds.  The sender sends a notice with the
 * operation OP and the disposition TT_QUEUE, and prints "sender STATE" for every
 * state change that is reported to it for SECONDS seconds.
 */
#include <Tt/tt_c.h>

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *
state_name(Tt_state s)
{
    switch (s) {
    case TT_CREATED:	return "created";
    case TT_SENT:	return "sent";
    case TT_HANDLED:	return "handled";
    case TT_FAILED:	return "failed";
    case TT_QUEUED:	return "queued";
    case TT_STARTED:	return "started";
    case TT_REJECTED:	return "rejected";
    default:		return "other";
    }
}

int
main(int argc, char **argv)
{
    char *procid;
    Tt_pattern pattern;
    Tt_message sent = NULL, got;
    int fd, rounds, observe;

    if (argc != 4 || (strcmp(argv[1], "observe") != 0 &&
		      strcmp(argv[1], "send") != 0)) {
	fprintf(stderr, "usage: %s observe|send OP SECONDS\n", argv[0]);
	return 2;
    }
    observe = (argv[1][0] == 'o');
    procid = tt_open();
    if (tt_ptr_error(procid) != TT_OK) {
	fprintf(stderr, "tt_open: %s\n", tt_status_message(tt_ptr_error(procid)));
	return 2;
    }
    fd = tt_fd();
    if (tt_session_join(tt_default_session()) != TT_OK) {
	fprintf(stderr, "cannot join the session\n");
	return 2;
    }
    if (observe) {
	pattern = tt_pattern_create();
	tt_pattern_category_set(pattern, TT_OBSERVE);
	tt_pattern_scope_add(pattern, TT_SESSION);
	tt_pattern_op_add(pattern, argv[2]);
	tt_pattern_session_add(pattern, tt_default_session());
	if (tt_pattern_register(pattern) != TT_OK) {
	    fprintf(stderr, "cannot register the pattern\n");
	    return 2;
	}
	printf("ready\n");
	fflush(stdout);
    } else {
	sent = tt_message_create();
	tt_message_class_set(sent, TT_NOTICE);
	tt_message_scope_set(sent, TT_SESSION);
	tt_message_address_set(sent, TT_PROCEDURE);
	tt_message_disposition_set(sent, TT_QUEUE);
	tt_message_op_set(sent, argv[2]);
	if (tt_message_send(sent) != TT_OK) {
	    fprintf(stderr, "cannot send the notice\n");
	    return 2;
	}
    }

    for (rounds = 0; rounds < 4 * atoi(argv[3]); rounds++) {
	struct pollfd pfd = { fd, POLLIN, 0 };

	if (poll(&pfd, 1, 250) <= 0)
	    continue;
	while ((got = tt_message_receive()) != NULL &&
	       tt_ptr_error(got) == TT_OK) {
	    printf("%s %s\n", got == sent ? "sender" : "observed",
		   state_name(tt_message_state(got)));
	    fflush(stdout);
	    if (got != sent)
		tt_message_destroy(got);
	}
    }
    tt_close();
    return 0;
}
