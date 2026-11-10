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

#ifndef _DTMAIL_SPOOLPROTOCOL_H
#define _DTMAIL_SPOOLPROTOCOL_H

/*
 * The protocol between a mailer (libDtMail) and dtmailspool, the program that
 * is installed set-group-id for the mail group and does what needs that group
 * in the mail spool directory, for the user who started it and for nobody
 * else.
 *
 * The two sides talk over a connected AF_UNIX stream socket, which the mailer
 * hands to the helper as descriptor DTSP_PROTOCOL_FD.  A message is a frame:
 *
 *     uint32  DTSP_MAGIC
 *     uint32  operation (a request) or status (a reply)
 *     uint32  length of the payload, at most DTSP_MAX_PAYLOAD
 *     bytes   payload
 *
 * with the integers in the byte order of the host: both sides run on one
 * machine.  A reply that hands over a file carries the descriptor in the same
 * sendmsg() as its first bytes; a request never carries one.  The receiver
 * reads exactly the bytes of the frame it is reading and no more, so the
 * descriptor always arrives with the first byte of the frame it belongs to,
 * and refuses a frame that is longer or shorter than its operation allows, a
 * frame with another magic number and a descriptor where none belongs.
 *
 * The functions are static inline so that the helper (plain C, no library but
 * the C library) and libDtMail use the same code without a shared library.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

#define DTSP_MAGIC		0x44545350u	/* "DTSP" */
#define DTSP_HEADER_SIZE	12u
#define DTSP_MAX_PAYLOAD	256u
#define DTSP_PROTOCOL_FD	3
#define DTSP_MAX_CONTENT	128u		/* the text of a lock file */
#define DTSP_USER_MAX		100		/* the longest login name that is
						   served: the names made from it
						   fit NAME_MAX */
#define DTSP_STALE_AGE		300u		/* seconds after which a lock
						   file may be broken */

/* Operations. */
#define DTSP_OP_OPEN		1u	/* flags -> access, created + descriptor */
#define DTSP_OP_LOCK_TRY	2u	/* kind, content -> OK, BUSY(age), VANISHED */
#define DTSP_OP_LOCK_VERIFY	3u	/* kind, content -> OK, NOT_OURS */
#define DTSP_OP_LOCK_RELEASE	4u	/* kind, content -> OK, NOT_OURS */
#define DTSP_OP_LOCK_BREAK	5u	/* kind */
#define DTSP_OP_TEMP_CREATE	6u	/* -> descriptor of the new file */
#define DTSP_OP_TEMP_COMMIT	7u	/* rename it over the mailbox */
#define DTSP_OP_TEMP_DISCARD	8u	/* remove it */

/* Flags of DTSP_OP_OPEN. */
#define DTSP_OPEN_WRITE		1u	/* read and write when the file allows */
#define DTSP_OPEN_CREATE	2u	/* make the mailbox when it is missing */

/* Access in the reply of DTSP_OP_OPEN. */
#define DTSP_ACCESS_READ	1u
#define DTSP_ACCESS_WRITE	2u

/* Kinds of lock file, named after the mailbox: <user>.lock, <user>.dtmail. */
#define DTSP_LOCK_MAIL		1u
#define DTSP_LOCK_DTMAIL	2u

/*
 * Status of a reply: zero, an errno value, or one of these.  Every status
 * below DTSP_BUSY is the errno value the failed call left.
 */
#define DTSP_OK			0u
#define DTSP_BUSY		1000u	/* the lock file exists (payload: age) */
#define DTSP_VANISHED		1001u	/* ... and was gone when looked at */
#define DTSP_NOT_OURS		1002u	/* the lock file has other contents */
#define DTSP_REFUSED		1003u	/* not a mailbox file of the user */
#define DTSP_BAD_REQUEST	1004u	/* sent before the helper stops */
#define DTSP_TEMP_PENDING	1005u	/* a temporary file exists already */
#define DTSP_NO_TEMP		1006u	/* there is no temporary file */
#define DTSP_UNAVAILABLE	1007u	/* the client could not ask: the helper
					   did not start, died or answered
					   against the protocol; never sent */

/* Results of dtsp_receive(). */
#define DTSP_RECV_OK		0
#define DTSP_RECV_EOF		1	/* the peer closed between two frames */
#define DTSP_RECV_TRUNCATED	2	/* ... in the middle of a frame */
#define DTSP_RECV_BAD_MAGIC	3
#define DTSP_RECV_OVERSIZE	4	/* the length is above the limit */
#define DTSP_RECV_BAD_FD	5	/* a descriptor where none is allowed, or two */
#define DTSP_RECV_IO		6	/* errno says which */

struct dtsp_frame {
    uint32_t		code;			/* operation or status */
    uint32_t		length;			/* of payload */
    unsigned char	payload[DTSP_MAX_PAYLOAD];
    int			fd;			/* received descriptor or -1 */
};

static inline uint32_t
dtsp_get32(const unsigned char *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static inline void
dtsp_put32(unsigned char *p, uint32_t v)
{
    memcpy(p, &v, sizeof v);
}

#ifdef MSG_NOSIGNAL
#define DTSP_SEND_FLAGS		MSG_NOSIGNAL
#else
#define DTSP_SEND_FLAGS		0
#endif

#ifdef MSG_CMSG_CLOEXEC
#define DTSP_RECV_FLAGS		MSG_CMSG_CLOEXEC
#else
#define DTSP_RECV_FLAGS		0
#endif

/*
 * Receive up to `want` bytes of the frame being read.  A descriptor that comes
 * with them is stored in *fd when the frame may carry one and none has arrived
 * yet; every other descriptor is closed and reported.  Returns the number of
 * bytes (0 at end of file), or -1 with errno set, or -2 for a refused
 * descriptor.
 */
static inline ssize_t
dtsp_recv_part(int sock, void *buf, size_t want, int *fd, int allow_fd)
{
    struct iovec iov;
    struct msghdr msg;
    union {
	struct cmsghdr	align;
	unsigned char	space[CMSG_SPACE(sizeof(int) * 4)];
    } control;
    struct cmsghdr *c;
    ssize_t n;
    int refused = 0;

    iov.iov_base = buf;
    iov.iov_len = want;
    memset(&msg, 0, sizeof msg);
    memset(&control, 0, sizeof control);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.space;
    msg.msg_controllen = sizeof control.space;

    /* A program that must stop on a signal defines DTSP_BEFORE_READ as a
     * function that waits until the descriptor can be read (with the signal
     * unblocked while it waits) and returns non-zero when it was told to
     * stop; DTSP_INTERRUPTED as the test of its flag, so that a call that a
     * signal interrupted fails. */
#ifdef DTSP_BEFORE_READ
    if (DTSP_BEFORE_READ(sock) != 0) {
	errno = EINTR;
	return -1;
    }
#endif
    do {
	n = recvmsg(sock, &msg, DTSP_RECV_FLAGS);
    } while (n < 0 && errno == EINTR
#ifdef DTSP_INTERRUPTED
	     && !DTSP_INTERRUPTED
#endif
	     );
    if (n < 0)
	return -1;

    for (c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
	size_t count, i;

	if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
	    continue;
	count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
	for (i = 0; i < count; i++) {
	    int received;

	    memcpy(&received, CMSG_DATA(c) + i * sizeof(int), sizeof received);
#ifndef MSG_CMSG_CLOEXEC
	    (void) fcntl(received, F_SETFD, FD_CLOEXEC);
#endif
	    if (allow_fd && *fd < 0 && i == 0)
		*fd = received;
	    else {
		(void) close(received);
		refused = 1;
	    }
	}
    }
    if (msg.msg_flags & MSG_CTRUNC)
	refused = 1;
    return refused ? -2 : n;
}

/*
 * Read one frame.  allow_fd is true for the reply to a request that returns a
 * file.  On every result but DTSP_RECV_OK no descriptor is left open.
 */
static inline int
dtsp_receive(int sock, struct dtsp_frame *frame, int allow_fd)
{
    unsigned char header[DTSP_HEADER_SIZE];
    size_t have = 0;
    int result;

    frame->fd = -1;
    frame->code = 0;
    frame->length = 0;

    while (have < sizeof header) {
	ssize_t n = dtsp_recv_part(sock, header + have, sizeof header - have,
				   &frame->fd, allow_fd);
	if (n == -2) {
	    result = DTSP_RECV_BAD_FD;
	    goto fail;
	}
	if (n < 0) {
	    result = DTSP_RECV_IO;
	    goto fail;
	}
	if (n == 0) {
	    result = have == 0 ? DTSP_RECV_EOF : DTSP_RECV_TRUNCATED;
	    goto fail;
	}
	have += (size_t) n;
    }
    if (dtsp_get32(header) != DTSP_MAGIC) {
	result = DTSP_RECV_BAD_MAGIC;
	goto fail;
    }
    frame->code = dtsp_get32(header + 4);
    frame->length = dtsp_get32(header + 8);
    if (frame->length > DTSP_MAX_PAYLOAD) {
	result = DTSP_RECV_OVERSIZE;
	goto fail;
    }

    have = 0;
    while (have < frame->length) {
	ssize_t n = dtsp_recv_part(sock, frame->payload + have,
				   frame->length - have, &frame->fd, allow_fd);
	if (n == -2) {
	    result = DTSP_RECV_BAD_FD;
	    goto fail;
	}
	if (n < 0) {
	    result = DTSP_RECV_IO;
	    goto fail;
	}
	if (n == 0) {
	    result = DTSP_RECV_TRUNCATED;
	    goto fail;
	}
	have += (size_t) n;
    }
    return DTSP_RECV_OK;

fail:
    {
	int saved = errno;

	if (frame->fd >= 0) {
	    (void) close(frame->fd);
	    frame->fd = -1;
	}
	errno = saved;
    }
    return result;
}

/*
 * Write one frame, with the descriptor `fd` (when it is not negative) attached
 * to its first bytes.  Returns 0, or -1 with errno set.
 */
static inline int
dtsp_send(int sock, uint32_t code, const void *payload, uint32_t length, int fd)
{
    unsigned char buffer[DTSP_HEADER_SIZE + DTSP_MAX_PAYLOAD];
    size_t total, sent = 0;

    if (length > DTSP_MAX_PAYLOAD) {
	errno = EMSGSIZE;
	return -1;
    }
    dtsp_put32(buffer, DTSP_MAGIC);
    dtsp_put32(buffer + 4, code);
    dtsp_put32(buffer + 8, length);
    if (length)
	memcpy(buffer + DTSP_HEADER_SIZE, payload, length);
    total = DTSP_HEADER_SIZE + (size_t) length;

    while (sent < total) {
	struct iovec iov;
	struct msghdr msg;
	union {
	    struct cmsghdr	align;
	    unsigned char	space[CMSG_SPACE(sizeof(int))];
	} control;
	ssize_t n;

	iov.iov_base = buffer + sent;
	iov.iov_len = total - sent;
	memset(&msg, 0, sizeof msg);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	if (sent == 0 && fd >= 0) {
	    struct cmsghdr *c;

	    memset(&control, 0, sizeof control);
	    msg.msg_control = control.space;
	    msg.msg_controllen = sizeof control.space;
	    c = CMSG_FIRSTHDR(&msg);
	    c->cmsg_level = SOL_SOCKET;
	    c->cmsg_type = SCM_RIGHTS;
	    c->cmsg_len = CMSG_LEN(sizeof(int));
	    memcpy(CMSG_DATA(c), &fd, sizeof fd);
	}
	do {
	    n = sendmsg(sock, &msg, DTSP_SEND_FLAGS);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
	    return -1;
	if (n == 0) {
	    errno = EIO;
	    return -1;
	}
	sent += (size_t) n;
    }
    return 0;
}

#endif
