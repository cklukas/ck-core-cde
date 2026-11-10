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
/*                                                                      *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company                     *
 * (c) Copyright 1993, 1994 International Business Machines Corp.       *
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.                      *
 * (c) Copyright 1993, 1994 Novell, Inc.                                *
 */
/*
 * xdm - display manager daemon
 *
 * $XConsortium: server.c /main/4 1995/10/27 16:14:56 rswiston $
 *
 * Copyright 1988 Massachusetts Institute of Technology
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of M.I.T. not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  M.I.T. makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * Author:  Keith Packard, MIT X Consortium
 */

# include	<sys/types.h>
# include	<sys/socket.h>
# include	<sys/signal.h>
# include	<setjmp.h>
# include	<poll.h>
# include	<sys/socket.h>
# include       <errno.h>
# include	<pwd.h>
# include	"dm.h"
# include	"vgmsg.h"

static volatile sig_atomic_t receivedUsr1;	/* SIGUSR1: the server is ready */
static volatile sig_atomic_t pauseTimedOut;	/* SIGALRM: the pause is over */



/***************************************************************************
 *
 *  Local procedure declarations
 *
 ***************************************************************************/

static SIGVAL CatchUsr1( int arg ) ;
static void   GetRemoteAddress( struct display *d, int fd) ;
static int    PingLost( Display *display ) ;
static int    serverPause( unsigned t, int serverPid) ;





/***************************************************************************
 *
 *  Global variables
 *
 ***************************************************************************/

static Display	*dpy;



/*
 * The handlers leave a note; serverPause looks at the notes.
 */
static SIGVAL
CatchUsr1( int arg )
{
    (void) arg;
    receivedUsr1 = 1;
}

static SIGVAL
PauseAlarm( int arg )
{
    (void) arg;
    pauseTimedOut = 1;
}

/* SIGCHLD only has to end the sleep of the pause */
static SIGVAL
PauseChild( int arg )
{
    (void) arg;
}

/* a handler that does not restart the system calls it interrupts */
static void
CatchSignal( int sig, void (*handler)(int), struct sigaction *previous )
{
    struct sigaction action;

    memset (&action, 0, sizeof (action));
    action.sa_handler = handler;
    sigemptyset (&action.sa_mask);
    sigaction (sig, &action, previous);
}

int 
StartServerOnce( struct display *d )
{
    char	**f;
    char	**argv;
    int		pid;
    char	**env;
    
    extern struct passwd   puser;	/* pseudo_user password entry	*/

    Debug ("Starting server for %s\n", d->name);
    receivedUsr1 = 0;
    CatchSignal (SIGUSR1, CatchUsr1, NULL);
    argv = d->argv;
    switch (pid = fork ()) {
    case 0:
	CleanUpChild ();
	if (d->authFile) {
	    if (!appendArg (&argv, "-auth") || !appendArg (&argv, d->authFile))
		exit(1);
	}
	if (!argv || !argv[0]) {
	    LogError(ReadCatalog(MC_LOG_SET,MC_LOG_NO_ARGS,MC_DEF_LOG_NO_ARGS));
	    exit(1);
	}
	Debug("Server invoked as ");
	for (f = argv; *f; f++)
	    Debug ("'%s' ", *f);
	Debug ("\n");

	/*
         *  set the permissions on  console devices to  pseudo-user.
         *  run the server as a pseudo-user, not root...
         */
#ifdef sun
        if (solaris_setdevperm(d->gettyLine, puser.pw_uid, puser.pw_gid) == 0)
             Debug ("Unable to set permissions on console devices ..\n");
        else {
#endif
             /* a server that keeps the privileges of the manager must not be started */
             if(-1 == setgid (puser.pw_gid)) {
                  Debug ("setgid() failed setting %d\n", puser.pw_gid);
                  LogError ((unsigned char *)"Cannot set group %d for the server of %s: errno = %d\n",
                            (int) puser.pw_gid, d->name, errno);
                  exit(1);
             }
             if(-1 == setuid (puser.pw_uid)) {
                  Debug ("setuid() failed setting %d\n", puser.pw_uid);
                  LogError ((unsigned char *)"Cannot set user %d for the server of %s: errno = %d\n",
                            (int) puser.pw_uid, d->name, errno);
                  exit(1);
             }
#ifdef sun
        }
#endif

	/*
	 *  build the server environment (if any)...
	 */
	env = 0;
	if (d->environStr && strlen(d->environStr) > 0)
	    env = parseEnv(env, d->environStr);
#ifdef sun
	if (getEnv (env, "OPENWINHOME") == NULL) 
	    env = setEnv(env, "OPENWINHOME", "/usr/openwin");
#endif
#ifdef _AIX
	if (getEnv (env, "ODMDIR") == NULL)
	    env = setEnv(env, "ODMDIR", "/etc/objrepos");
#ifdef _POWER
	env = setEnv(env, "XTOEXEC", "true");		/* flag for xserverrc */
#endif
#endif

	/*
	 * give the server SIGUSR1 ignored,
	 * it will notice that and send SIGUSR1
	 * when ready
	 */
	signal (SIGUSR1, SIG_IGN);
	(void) execve (argv[0], argv, env);
	LogError(ReadCatalog(
		MC_LOG_SET,MC_LOG_NO_EXESRV,MC_DEF_LOG_NO_EXESRV),argv[0]);
	exit(1);
    case -1:
	LogError(ReadCatalog(MC_LOG_SET,MC_LOG_FAIL_FORK,MC_DEF_LOG_FAIL_FORK));
	return 0;
    default:
	break;
    }
    Debug ("Server started. Process ID = %d\n", pid);
    d->serverPid = pid;
    if (serverPause ((unsigned) d->openDelay, pid))
	return FALSE;
    return TRUE;
}


int 
StartServer( struct display *d )
{
    int	i;
    int	ret = FALSE;

    i = 0;
    while (d->serverAttempts == 0 || i < d->serverAttempts)
    {
	if ((ret = StartServerOnce (d)) == TRUE)
	    break;
	sleep (d->openDelay);
	i++;
    }
    return ret;
}


/*
 * sleep for t seconds, return 1 if the server is dead when
 * the sleep finishes, 0 else.  The sleep ends early when the server says
 * it is ready (SIGUSR1) or ends (SIGCHLD).
 */

static int 
serverPause( unsigned t, int serverPid )
{
    sigset_t		block, previous;
    struct sigaction	oldAlarm, oldChild;
    int			pid;
    int			dead = 0;

    Debug ("Display Manager pausing until SIGUSR1 from server or timeout\n");

    /* the notes are looked at, and the sleep entered, with the signals blocked */
    sigemptyset (&block);
    sigaddset (&block, SIGUSR1);
    sigaddset (&block, SIGALRM);
    sigaddset (&block, SIGCHLD);
    sigprocmask (SIG_BLOCK, &block, &previous);
    CatchSignal (SIGALRM, PauseAlarm, &oldAlarm);
    CatchSignal (SIGCHLD, PauseChild, &oldChild);
    pauseTimedOut = 0;
    if (!receivedUsr1)
	alarm (t);
    else
	Debug ("ServerPause(): already received USR1\n");

    for (;;) {
	pid = waitpid ((pid_t) -1, NULL, WNOHANG);
	if (pid == serverPid || (pid == -1 && errno == ECHILD)) {
	    dead = 1;
	    break;
	}
	if (pid > 0)
	    continue;
	if (receivedUsr1 || pauseTimedOut) {
	    Debug ("Server alive and kicking\n");
	    break;
	}
	sigsuspend (&previous);
    }

    alarm ((unsigned) 0);
    sigaction (SIGALRM, &oldAlarm, NULL);
    sigaction (SIGCHLD, &oldChild, NULL);
    sigprocmask (SIG_SETMASK, &previous, NULL);
    if (dead) {
	Debug ("Server dead\n");
	Debug ("Server died\n");
	LogError(ReadCatalog(MC_LOG_SET,MC_LOG_SRV_DIED,MC_DEF_LOG_SRV_DIED));
    }
    return dead;
}


/*
 * this code is complicated by some TCP failings.  On
 * many systems, the connect will occasionally hang forever,
 * this trouble is avoided by making the connection in a thread
 * that is given openTimeout seconds (see BoundedCall) and giving up,
 * terminating the server, when it does not return.
 */

static void *
OpenDisplayJob( void *name )
{
    Display *display = XOpenDisplay ((char *) name);

    free (name);
    return display;
}

static void
GetRemoteAddress( struct display *d, int fd )
{
    char    buf[512];
    int	    len = sizeof (buf);

    if (d->peer)
	free ((char *) d->peer);
    getpeername (fd, (struct sockaddr *) buf, &len);
    d->peerlen = 0;
    if (len)
    {
	d->peer = malloc (len);
	if (d->peer)
	{
	    bcopy (buf, (char *) d->peer, len);
	    d->peerlen = len;
	}
    }
    Debug ("Got remote address %s %d\n", d->name, d->peerlen);
}



/****************************************************************************
 *
 *  LogOpenError()
 *
 *  If d->startAttempts is a large number and a connection cannot be made to
 *  the server, the error log can fill up rapidly with error messages. This
 *  could be common in X-terminals that do not support XDMCP and are turned
 *  off over a weekend. This routine attempts to reduce the number of error
 *  messages logged in this scenario.
 *  
 ****************************************************************************/

int 
LogOpenError( int count )
{

    if ( count <= 10			    ) return 1;
    if ( count <= 100 && (count %  10 == 0) ) return 1;
    if ( count <= 500 && (count %  50 == 0) ) return 1;
    if ( 		 (count % 100 == 0) ) return 1;
    
    return 0;
}


int 
WaitForServer( struct display *d )
{
    int	    i;

    for (i = 0; i < (d->openRepeat > 0 ? d->openRepeat : 1); i++) {
	void	*opened = NULL;
	char	*name = strdup (d->name);
	int	completed;

	Debug ("Before XOpenDisplay(%s)\n", d->name);
	completed = name ? BoundedCall ((unsigned) d->openTimeout,
					OpenDisplayJob, name, &opened) : -1;
	if (completed != 0) {
	    if (completed < 0)
		free (name);
	    dpy = (Display *) opened;
	    Debug ("After XOpenDisplay()\n");
	    if (dpy) {
	    	if (d->displayType.location == Foreign)
		    GetRemoteAddress (d, ConnectionNumber (dpy));
	    	RegisterCloseOnFork (ConnectionNumber (dpy));
                (void) fcntl (ConnectionNumber (dpy), F_SETFD, 0);
	    	return 1;
	    } else {
	    	Debug ("OpenDisplay failed\n");
	    }
	    Debug ("Waiting for server to start %d\n", i);
	    sleep ((unsigned) d->openDelay);
    	} else {
	    Debug ("Hung in open, aborting\n");
	    if (LogOpenError(d->startTries))
		LogError(ReadCatalog(
			MC_LOG_SET,MC_LOG_HUNG_DPY,MC_DEF_LOG_HUNG_DPY),
			   d->name, d->startTries);
	    break;
    	}
    }
    Debug ("Giving up on server\n");
    if (LogOpenError(d->startTries))
	LogError(ReadCatalog(
		MC_LOG_SET,MC_LOG_FAIL_SRVOPEN,MC_DEF_LOG_FAIL_SRVOPEN),
		d->startTries, d->name);
    return 0;
}

void
ResetServer( struct display *d )
{
    if (dpy && d->displayType.origin != FromXDMCP)
	pseudoReset (dpy);
}


/****************************************************************************
 *
 *  Server pinging routines...
 *
 *  These routines attempt to determine if the server is still alive.
 *  Periodically (d->pingInterval) an XSync is sent to the server. If an I/O
 *  error occurs, then the connection has been lost and the server needs to
 *  be reset. If the server is blocked for some reason (i.e. server grab) the
 *  XSync will block until a local timer expires. In this case, we just note
 *  the block and continue...
 *
 *  7/26/90 - prr
 *  The XSync was replaced by a socket-level ping. For some reason, an XSync
 *  to a grabbed server causes a subsequent pseudoReset (KillClients) to not 
 *  kill all clients. The socket ping is not affected by a grabbed server,
 *  but it cannot detect a server shutdown and restart within one ping
 *  interval. 
 *
 ****************************************************************************/

static sigjmp_buf	pingTime;
static int	serverDead = FALSE;

/*
 * Has the server closed the connection?  The descriptor is looked at without
 * waiting: a hang-up or error, or end of file where there should be nothing
 * or an answer, means the server has gone.
 */
static int
ConnectionLost( int fd )
{
    struct pollfd	pfd;
    char		byte;
    ssize_t		count;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll (&pfd, 1, 0) <= 0)
	return FALSE;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
	return TRUE;
    if (pfd.revents & POLLIN)
    {
	count = recv (fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
	if (count == 0)
	    return TRUE;
	if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
	    return TRUE;
    }
    return FALSE;
}

/* can something be written without waiting for the server? */
static int
CanWrite( int fd )
{
    struct pollfd	pfd;

    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    return poll (&pfd, 1, 0) > 0 && (pfd.revents & POLLOUT);
}

static int
PingLost( Display *display )
{
    serverDead = TRUE;
    siglongjmp (pingTime, 1);
}


int 
PingServer( struct display *d, Display *alternateDpy )
{
    XIOErrorHandler oldError;
    int	    fd;
    int	    blocked = FALSE;

    (void) d;
    if (!alternateDpy)
	alternateDpy = dpy;
    fd = ConnectionNumber (alternateDpy);
    serverDead = FALSE;
    oldError = XSetIOErrorHandler (PingLost);
    if (!sigsetjmp (pingTime, 1))
    {
	Debug ("Ping server\n");
	/*
	 * Nothing here waits for the server: a request is only written when the
	 * connection can take it, and the answer is not waited for.  A server that
	 * has gone shows as a connection that was closed or that cannot be written.
	 */
	if (ConnectionLost (fd))
	    serverDead = TRUE;
	else if (!CanWrite (fd))
	{
	    Debug ("Server blocked, continuing...\n");
	    blocked = TRUE;
	}
	else
	{
	    XNoOp (alternateDpy);
	    XFlush (alternateDpy);
	    if (ConnectionLost (fd))
		serverDead = TRUE;
	    else
		Debug ("Server alive\n");
	}

	/* what the server has sent is read and dropped (the connection has nothing to flush here) */
	while (!serverDead && !blocked && XPending (alternateDpy)) {
	    XEvent event;
	    XNextEvent(alternateDpy, &event);
	}
    }
    XSetIOErrorHandler (oldError);
    if (serverDead)
	Debug ("Server dead\n");
    return !serverDead;
}
