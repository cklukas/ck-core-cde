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

#include <EUSCompat.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <rpc/rpc.h>
#include "cmsloop.h"

/* Set by the handlers, cleared by the loop. */
static volatile sig_atomic_t garbage_requested;
static volatile sig_atomic_t debug_requested;
static volatile sig_atomic_t hangup_requested;

/* What the loop waits with: the mask of the thread before the signals were
 * blocked.  A signal that arrives while the loop is busy is held back and
 * delivered by the wait. */
static sigset_t wait_mask;

static void
garbage_handler(int signal_number)
{
	(void)signal_number;
	garbage_requested = 1;
}

static void
debug_handler(int signal_number)
{
	(void)signal_number;
	debug_requested = 1;
}

static void
hangup_handler(int signal_number)
{
	(void)signal_number;
	hangup_requested = 1;
}

static void
set_handler(int signal_number, void (*handler)(int))
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	sigaction(signal_number, &action, NULL);
}

extern void
_DtCmsInstallSignalHandlers(void)
{
	sigset_t blocked;

	set_handler(SIGALRM, garbage_handler);
	set_handler(SIGUSR1, garbage_handler);
	set_handler(SIGUSR2, debug_handler);
	set_handler(SIGHUP, hangup_handler);

	/* a client that disconnects before it gets its reply is not an error
	 * of the daemon */
	signal(SIGPIPE, SIG_IGN);

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGALRM);
	sigaddset(&blocked, SIGUSR1);
	sigaddset(&blocked, SIGUSR2);
	sigaddset(&blocked, SIGHUP);
	sigprocmask(SIG_BLOCK, &blocked, &wait_mask);
	/* the wait lets everything through that the thread let through before */
	sigdelset(&wait_mask, SIGALRM);
	sigdelset(&wait_mask, SIGUSR1);
	sigdelset(&wait_mask, SIGUSR2);
	sigdelset(&wait_mask, SIGHUP);
}

extern int
_DtCmsRunServiceLoop(const _DtCmsLoopActions *actions)
{
	for (;;) {
		fd_set	readfds;
		int	nfds, ready;

		/* The signals are blocked here, so the flags change only
		 * while the loop waits. */
		if (hangup_requested)
			return (0);
		if (debug_requested) {
			debug_requested = 0;
			if (actions != NULL && actions->switch_debug != NULL)
				actions->switch_debug();
		}
		if (garbage_requested) {
			garbage_requested = 0;
			if (actions != NULL && actions->collect_garbage != NULL)
				actions->collect_garbage();
		}

		readfds = svc_fdset;
		nfds = svc_maxfd + 1;
		if (nfds > FD_SETSIZE)
			nfds = FD_SETSIZE;

		ready = pselect(nfds, &readfds, NULL, NULL, NULL, &wait_mask);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		if (ready > 0)
			svc_getreqset(&readfds);
	}
}
