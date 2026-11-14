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
 * bounded.c
 *
 * Calls of the X library with a time limit, shared by the login manager and
 * its greeter and chooser programs.
 */

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>

#include "bounded.h"

/***************************************************************************
 *
 *  BoundedCall
 *
 *  Call JOB (ARGUMENT) and wait at most SECONDS (0: as long as it takes) for
 *  it to return, for the calls of the X library that have no time limit of their
 *  own (opening a display, requests to a server that does not answer).  The
 *  call is made by a thread of its own, with every signal blocked, and the
 *  caller does not look at the signals the way the call would: nothing is
 *  jumped over.  Returns 1 and the result of the job in *RESULT when it
 *  returned in time, 0 when it did not and -1 when it could not be started.
 *  A job that did not return in time is left running: it owns ARGUMENT from
 *  then on, and what it returns is dropped.  The caller must not use what the
 *  job uses until the process ends.
 *
 ***************************************************************************/

struct bounded_call {
    pthread_mutex_t	lock;
    pthread_cond_t	finished;
    int			done;
    int			abandoned;
    void		*(*job)(void *);
    void		*argument;
    void		*result;
};

static void *
BoundedCallThread( void *data )
{
    struct bounded_call	*call = data;
    void		*result = call->job (call->argument);
    int			abandoned;

    pthread_mutex_lock (&call->lock);
    call->result = result;
    call->done = 1;
    abandoned = call->abandoned;
    if (!abandoned)
	pthread_cond_signal (&call->finished);
    pthread_mutex_unlock (&call->lock);
    if (abandoned)
	free (call);
    return nullptr;
}

int
BoundedCall( unsigned seconds, void *(*job)(void *), void *argument,
	     void **result )
{
    struct bounded_call	*call;
    pthread_attr_t	attributes;
    pthread_condattr_t	condition;
    pthread_t		thread;
    sigset_t		all, previous;
    struct timespec	deadline;
    int			started, status = 0;

    call = calloc (1, sizeof (*call));
    if (call == nullptr)
	return -1;
    call->job = job;
    call->argument = argument;
    pthread_mutex_init (&call->lock, nullptr);
    pthread_condattr_init (&condition);
    pthread_condattr_setclock (&condition, CLOCK_MONOTONIC);
    pthread_cond_init (&call->finished, &condition);
    pthread_condattr_destroy (&condition);
    pthread_attr_init (&attributes);
    pthread_attr_setdetachstate (&attributes, PTHREAD_CREATE_DETACHED);

    /* the signals are for the caller: the thread starts with all of them blocked */
    sigfillset (&all);
    pthread_sigmask (SIG_SETMASK, &all, &previous);
    started = pthread_create (&thread, &attributes, BoundedCallThread, call);
    pthread_sigmask (SIG_SETMASK, &previous, nullptr);
    pthread_attr_destroy (&attributes);
    if (started != 0)
    {
	free (call);
	return -1;
    }

    clock_gettime (CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += seconds;
    pthread_mutex_lock (&call->lock);
    while (!call->done && status == 0)
    {
	if (seconds)
	    status = pthread_cond_timedwait (&call->finished, &call->lock,
					     &deadline);
	else
	    pthread_cond_wait (&call->finished, &call->lock);
    }
    if (call->done)
    {
	*result = call->result;
	pthread_mutex_unlock (&call->lock);
	free (call);
	return 1;
    }
    call->abandoned = 1;
    pthread_mutex_unlock (&call->lock);
    return 0;
}
