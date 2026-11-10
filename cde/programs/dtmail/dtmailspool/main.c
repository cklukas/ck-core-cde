/*
 * CDE - Common Desktop Environment
 *
 * Copyright (c) 1993-2012, The Open Group. All rights reserved.
 *
 * These libraries and programs are free software; you can
 * redistribute them and/or modify them under the terms of the GNU
 * Lesser General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * These libraries and programs are distributed in the hope that
 * they will be useful, but WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with these libraries and programs; if not, write
 * to the Free Software Foundation, Inc., 51 Franklin Street, Fifth
 * Floor, Boston, MA 02110-1301 USA
 */

/*
 * dtmailspool - the one part of the mailer that holds the mail group.
 *
 * It is installed set-group-id for the group of the mail spool directory and
 * is started by dtmail and dtmailpr with a connected socket as descriptor 3.
 * For the user who started it, and nobody else, it opens, creates, locks and
 * replaces that user's own mailbox file in the spool directory and gives the
 * descriptors of the files it opened to the mailer.  The protocol is
 * described in DtMail/SpoolProtocol.h.
 *
 * What it does about trust:
 *  - It reads no environment variable, no option and no argument.  The
 *    environment is emptied before anything else runs.
 *  - The spool directory is a constant of the build, the name of every file is
 *    made here from the login name of the real user, and a request cannot name
 *    a file.  Every open of a file in the directory follows no link, and the
 *    result must be a regular file that the user owns.
 *  - It holds the mail group only while one operation on the spool directory
 *    runs.  The request is read, checked and answered with the group of the
 *    user as effective group.  Every change of the effective group is
 *    verified; when it cannot be done or did not happen, the helper stops.
 *  - A request that is not well-formed ends the helper, which removes the
 *    temporary file it made, if any, as it does when the mailer closes the
 *    connection.
 *  - It lives and dies with the connection. The signals of a terminal (an
 *    interrupt, a quit, a hangup) reach the whole process group of the mailer
 *    and are ignored here, so that the mailer can still release its locks
 *    through the helper when it ends on one of them. A SIGTERM ends the helper
 *    between two requests; it is blocked while a request is served.
 *
 * Exit status: 0 when the mailer closed the connection between two requests
 * or the helper was told to stop, EXIT_PROTOCOL for a request that is
 * malformed, EXIT_IO for a connection that failed, EXIT_GROUP for an
 * effective group that could not be set, EXIT_SETUP for a start that is not
 * as the mailer starts it.  Nothing is written to a standard stream.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1	/* syscall() for close_range() */
#endif

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#ifndef DTMAIL_SPOOL_DIR
#error DTMAIL_SPOOL_DIR is the directory of the mail spool and is set by the build
#endif

#define EXIT_PROTOCOL	10
#define EXIT_IO		11
#define EXIT_GROUP	20
#define EXIT_SETUP	21

static volatile sig_atomic_t stop_requested;
static sigset_t waiting_mask;		/* what is blocked while waiting for a request */

static int wait_readable(int sock);
#define DTSP_BEFORE_READ(sock) wait_readable(sock)
#define DTSP_INTERRUPTED (stop_requested)
#include <DtMail/SpoolProtocol.h>
#include "spool.h"

extern char **environ;

static gid_t user_gid;		/* the real group of the user */
static gid_t mail_gid;		/* the group the program was installed with */

/*
 * Make `gid` the effective group and make sure it is.  There is no way to
 * go on in the state that this leaves when it fails: the program stops.
 */
static void
group_set(gid_t gid)
{
    if (getegid() == gid)
	return;
    if (setegid(gid) != 0 || getegid() != gid)
	_exit(EXIT_GROUP);
#ifdef __linux__
    /* a change of the ids sets the flag back to the system's default */
    (void) prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
}

/*
 * The group is held for the call of the operation and for nothing else;
 * errno is what the operation left.
 */
static uint32_t
with_group(uint32_t (*operation)(void *), void *argument)
{
    uint32_t status;

    group_set(mail_gid);
    status = operation(argument);
    group_set(user_gid);
    return status;
}

static void
note_signal(int signo)
{
    (void) signo;
    stop_requested = 1;
}

/* Descriptors 0, 1 and 2 are open, so that no file is ever opened as one. */
static int
settle_standard_descriptors(void)
{
    int fd;

    for (fd = 0; fd <= 2; fd++) {
	if (fcntl(fd, F_GETFD) < 0) {
	    int opened = open("/dev/null", O_RDWR);

	    if (opened != fd)
		return -1;
	}
    }
    return 0;
}

static void
close_other_descriptors(void)
{
    long limit;
    int fd;

#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, DTSP_PROTOCOL_FD + 1, ~0u, 0) == 0)
	return;
#endif
    limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 65536)
	limit = 65536;
    for (fd = DTSP_PROTOCOL_FD + 1; fd < limit; fd++)
	(void) close(fd);
}

static int
reset_signals(void)
{
    sigset_t none, terminate;
    struct sigaction action;
    int signo;

    sigemptyset(&none);
    if (sigprocmask(SIG_SETMASK, &none, NULL) != 0)
	return -1;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;
    for (signo = 1; signo < 64; signo++) {
	if (signo == SIGKILL || signo == SIGSTOP)
	    continue;
	if (sigaction(signo, &action, NULL) != 0 && errno != EINVAL)
	    return -1;
    }
    /* A write to a closed connection is an error, not a signal; a limit on
     * the size of a file or on the time is an error of the call, not the end
     * of the helper in the middle of a request; the signals of a terminal
     * are for the mailer, which has locks to release through the helper. */
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) != 0
	|| sigaction(SIGXFSZ, &action, NULL) != 0
	|| sigaction(SIGXCPU, &action, NULL) != 0
	|| sigaction(SIGINT, &action, NULL) != 0
	|| sigaction(SIGQUIT, &action, NULL) != 0
	|| sigaction(SIGHUP, &action, NULL) != 0)
	return -1;
    /* A SIGTERM ends the helper. It is blocked except while the helper waits
     * for a request, where it interrupts the wait (no SA_RESTART), so that it
     * can neither be lost between a test and a wait nor cut a request. */
    action.sa_handler = note_signal;
    if (sigaction(SIGTERM, &action, NULL) != 0)
	return -1;
    sigemptyset(&waiting_mask);
    sigemptyset(&terminate);
    sigaddset(&terminate, SIGTERM);
    return sigprocmask(SIG_BLOCK, &terminate, NULL);
}

/* The limits that the mailer set for itself are not the helper's: it has
 * to finish what it has begun. */
static void
raise_limits(void)
{
    static const int which[] = { RLIMIT_CPU, RLIMIT_FSIZE };
    size_t i;

    for (i = 0; i < sizeof which / sizeof which[0]; i++) {
	struct rlimit limit;

	if (getrlimit(which[i], &limit) == 0 && limit.rlim_cur != limit.rlim_max) {
	    limit.rlim_cur = limit.rlim_max;
	    (void) setrlimit(which[i], &limit);
	}
    }
}

/*
 * Wait until a request can be read, with SIGTERM deliverable only here.
 * Non-zero when the helper is to stop. An error of the wait is left to the
 * read, which reports it.
 */
static int
wait_readable(int sock)
{
    for (;;) {
	fd_set readable;
	int n;

	if (stop_requested)
	    return 1;
	FD_ZERO(&readable);
	FD_SET(sock, &readable);
	n = pselect(sock + 1, &readable, NULL, NULL, NULL, &waiting_mask);
	if (n >= 0 || errno != EINTR)
	    return 0;
    }
}

static int
name_of_user(uid_t uid, char *name, size_t size)
{
    struct passwd entry, *found = NULL;
    size_t length = 1024;
    char *buffer;
    int result;

    while (length <= 65536) {
	buffer = malloc(length);
	if (buffer == NULL)
	    return -1;
	result = getpwuid_r(uid, &entry, buffer, length, &found);
	if (result == ERANGE) {
	    free(buffer);
	    length *= 4;
	    continue;
	}
	if (result != 0 || found == NULL || found->pw_uid != uid
	    || found->pw_name == NULL || strlen(found->pw_name) >= size
	    || !dtsp_valid_user_name(found->pw_name)) {
	    free(buffer);
	    return -1;
	}
	memcpy(name, found->pw_name, strlen(found->pw_name) + 1);
	free(buffer);
	return 0;
    }
    return -1;
}

/*
 * Is `name` the login name of a user? A lock file of the user X is called
 * X.lock, which would be the mailbox of a user of that name. -1 when that is
 * not known: then the name is taken to be one.
 */
static int
login_exists(const char *name)
{
    struct passwd entry, *found = NULL;
    size_t length = 1024;

    while (length <= 65536) {
	char *buffer = malloc(length);
	int result;

	if (buffer == NULL)
	    return -1;
	result = getpwnam_r(name, &entry, buffer, length, &found);
	free(buffer);
	if (result == ERANGE) {
	    length *= 4;
	    continue;
	}
	if (result == 0)
	    return found != NULL;
	/* the values that POSIX allows for "there is no such user" */
	if (result == ENOENT || result == ESRCH || result == EBADF
	    || result == EPERM)
	    return 0;
	return -1;
    }
    return -1;
}

static int
lock_name_is_free(const char *name, const char *suffix)
{
    char lock[DTSP_USER_MAX + sizeof(".dtmail") + 1];

    (void) snprintf(lock, sizeof lock, "%s%s", name, suffix);
    return login_exists(lock) == 0;
}

/* The operations, each with the arguments it needs, run with the group. */

struct open_call {
    struct dtsp_spool	*spool;
    uint32_t		flags, access, created;
    int			fd;
};

static uint32_t
run_open(void *p)
{
    struct open_call *c = p;

    return dtsp_open(c->spool, c->flags, &c->fd, &c->access, &c->created);
}

struct lock_call {
    struct dtsp_spool	*spool;
    uint32_t		operation, kind, length, age;
    const unsigned char	*content;
    char		suffix[DTSP_SUFFIX_LENGTH + 1];
};

static uint32_t
run_lock(void *p)
{
    struct lock_call *c = p;

    switch (c->operation) {
    case DTSP_OP_LOCK_TRY:
	return dtsp_lock_try(c->spool, c->kind, c->content, c->length,
			     c->suffix, &c->age);
    case DTSP_OP_LOCK_VERIFY:
	return dtsp_lock_verify(c->spool, c->kind, c->content, c->length);
    case DTSP_OP_LOCK_RELEASE:
	return dtsp_lock_release(c->spool, c->kind, c->content, c->length);
    default:
	return dtsp_lock_break(c->spool, c->kind);
    }
}

struct temp_call {
    struct dtsp_spool	*spool;
    uint32_t		operation;
    char		suffix[DTSP_SUFFIX_LENGTH + 1];
    int			fd;
};

static uint32_t
run_temp(void *p)
{
    struct temp_call *c = p;

    switch (c->operation) {
    case DTSP_OP_TEMP_CREATE:
	return dtsp_temp_create(c->spool, c->suffix, &c->fd);
    case DTSP_OP_TEMP_COMMIT:
	return dtsp_temp_commit(c->spool);
    default:
	return dtsp_temp_discard(c->spool);
    }
}

/*
 * The reply is sent when the work is done; the request and what it asks are
 * checked before the group is taken.  Returns 0 to go on, or an exit status.
 */
static int
reply(int sock, uint32_t status, const unsigned char *payload, uint32_t length,
      int fd)
{
    return dtsp_send(sock, status, payload, length, fd) == 0 ? 0 : EXIT_IO;
}

static int
bad_request(int sock)
{
    (void) dtsp_send(sock, DTSP_BAD_REQUEST, NULL, 0, -1);
    return EXIT_PROTOCOL;
}

/* kind (u32), length (u32), content: false unless it is exactly that. */
static int
parse_lock(const struct dtsp_frame *request, struct lock_call *call)
{
    uint32_t length;

    if (request->length < 8)
	return 0;
    call->kind = dtsp_get32(request->payload);
    length = dtsp_get32(request->payload + 4);
    if (length < 1 || length > DTSP_MAX_CONTENT
	|| request->length != 8u + length)
	return 0;
    if (call->kind != DTSP_LOCK_MAIL && call->kind != DTSP_LOCK_DTMAIL)
	return 0;
    call->length = length;
    call->content = request->payload + 8;
    return 1;
}

static int
handle(struct dtsp_spool *spool, int sock, const struct dtsp_frame *request)
{
    unsigned char payload[8];

    switch (request->code) {
    case DTSP_OP_OPEN: {
	struct open_call call;
	uint32_t status;
	int result;

	if (request->length != 4)
	    return bad_request(sock);
	call.spool = spool;
	call.flags = dtsp_get32(request->payload);
	if (call.flags & ~(DTSP_OPEN_WRITE | DTSP_OPEN_CREATE))
	    return bad_request(sock);
	call.fd = -1;
	status = with_group(run_open, &call);
	dtsp_put32(payload, call.access);
	dtsp_put32(payload + 4, call.created);
	result = reply(sock, status, payload, status == DTSP_OK ? 8 : 0,
		       status == DTSP_OK ? call.fd : -1);
	if (call.fd >= 0)
	    (void) close(call.fd);
	return result;
    }

    case DTSP_OP_LOCK_TRY:
    case DTSP_OP_LOCK_VERIFY:
    case DTSP_OP_LOCK_RELEASE: {
	struct lock_call call;
	uint32_t status;

	memset(&call, 0, sizeof call);
	if (!parse_lock(request, &call))
	    return bad_request(sock);
	call.spool = spool;
	call.operation = request->code;
	if (request->code == DTSP_OP_LOCK_TRY
	    && dtsp_random_suffix(call.suffix) != 0)
	    return reply(sock, EIO, NULL, 0, -1);
	status = with_group(run_lock, &call);
	dtsp_put32(payload, call.age);
	return reply(sock, status, payload, status == DTSP_BUSY ? 4 : 0, -1);
    }

    case DTSP_OP_LOCK_BREAK: {
	struct lock_call call;

	memset(&call, 0, sizeof call);
	if (request->length != 4)
	    return bad_request(sock);
	call.kind = dtsp_get32(request->payload);
	if (call.kind != DTSP_LOCK_MAIL && call.kind != DTSP_LOCK_DTMAIL)
	    return bad_request(sock);
	call.spool = spool;
	call.operation = DTSP_OP_LOCK_BREAK;
	return reply(sock, with_group(run_lock, &call), NULL, 0, -1);
    }

    case DTSP_OP_TEMP_CREATE:
    case DTSP_OP_TEMP_COMMIT:
    case DTSP_OP_TEMP_DISCARD: {
	struct temp_call call;
	uint32_t status;
	int result;

	if (request->length != 0)
	    return bad_request(sock);
	memset(&call, 0, sizeof call);
	call.spool = spool;
	call.operation = request->code;
	call.fd = -1;
	if (request->code == DTSP_OP_TEMP_CREATE
	    && dtsp_random_suffix(call.suffix) != 0)
	    return reply(sock, EIO, NULL, 0, -1);
	status = with_group(run_temp, &call);
	result = reply(sock, status, NULL, 0, status == DTSP_OK ? call.fd : -1);
	if (call.fd >= 0)
	    (void) close(call.fd);
	return result;
    }

    default:
	return bad_request(sock);
    }
}

static int
serve(struct dtsp_spool *spool, int sock)
{
    for (;;) {
	struct dtsp_frame request;
	int result;

	if (stop_requested)
	    return 0;
	result = dtsp_receive(sock, &request, 0);
	if (result == DTSP_RECV_EOF)
	    return 0;
	if (result == DTSP_RECV_IO)
	    return stop_requested ? 0 : EXIT_IO;
	if (result != DTSP_RECV_OK)
	    return EXIT_PROTOCOL;
	result = handle(spool, sock, &request);
	if (result != 0)
	    return result;
    }
}

static uint32_t
run_discard(void *p)
{
    return dtsp_temp_discard(p);
}

int
main(void)
{
    static char *no_environment[] = { NULL };
    struct dtsp_spool spool;
    struct rlimit no_core = { 0, 0 };
    struct stat sb;
    char name[DTSP_USER_MAX + 1];
    int dirfd, status;

    /* Nothing that was inherited is trusted. */
    environ = no_environment;
    if (fstat(DTSP_PROTOCOL_FD, &sb) != 0 || !S_ISSOCK(sb.st_mode))
	return EXIT_SETUP;
    if (settle_standard_descriptors() != 0)
	return EXIT_SETUP;
    close_other_descriptors();
    if (reset_signals() != 0)
	return EXIT_SETUP;
    (void) umask(077);
    raise_limits();
    (void) setrlimit(RLIMIT_CORE, &no_core);
#ifdef __linux__
    (void) prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif

    /* The helper is installed set-group-id and never set-user-id. */
    if (geteuid() != getuid())
	return EXIT_SETUP;
    user_gid = getgid();
    mail_gid = getegid();
    group_set(user_gid);

    if (name_of_user(getuid(), name, sizeof name) != 0)
	return EXIT_SETUP;

    group_set(mail_gid);
    dirfd = open(DTMAIL_SPOOL_DIR,
		 O_RDONLY | O_DIRECTORY | O_NOCTTY | O_CLOEXEC);
    group_set(user_gid);
    if (dirfd < 0 || fstat(dirfd, &sb) != 0 || !S_ISDIR(sb.st_mode))
	return EXIT_SETUP;
    if (dtsp_spool_init(&spool, dirfd, getuid(), mail_gid, name) != 0)
	return EXIT_SETUP;
    spool.lock_free[DTSP_LOCK_MAIL - 1] = lock_name_is_free(name, ".lock");
    spool.lock_free[DTSP_LOCK_DTMAIL - 1] = lock_name_is_free(name, ".dtmail");

    status = serve(&spool, DTSP_PROTOCOL_FD);

    if (spool.have_temp)
	(void) with_group(run_discard, &spool);
    return status;
}
