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

#ifndef _CMSLOOP_H
#define _CMSLOOP_H

/*
 * The service loop of rpc.cmsd.
 *
 * rpc.cmsd works on calendars, registrations and reminder queues that no
 * code run by a signal can touch safely.  The signal handlers therefore only
 * record that a signal arrived.  The signals are blocked everywhere but in
 * the wait for the next request, and the work they ask for is done by the
 * loop between two requests.
 */

typedef struct {
	/* SIGALRM, SIGUSR1: look after registrations, reminders and logs */
	void (*collect_garbage)(void);
	/* SIGUSR2: switch the debug output */
	void (*switch_debug)(void);
} _DtCmsLoopActions;

/*
 * Install the handlers for SIGALRM, SIGUSR1, SIGUSR2 and SIGHUP, block the
 * four signals in the calling thread and ignore SIGPIPE.  The loop below is
 * the only place where they are delivered.
 */
extern void _DtCmsInstallSignalHandlers(void);

/*
 * Serve the requests of the transports registered with the RPC library until
 * SIGHUP arrives or the wait for requests fails.  The actions run between
 * requests, never while a request is processed.  A request that was started
 * when SIGHUP arrived is finished first.  Returns 0 for SIGHUP and -1 when
 * the wait failed.
 */
extern int _DtCmsRunServiceLoop(const _DtCmsLoopActions *actions);

#endif /* _CMSLOOP_H */
