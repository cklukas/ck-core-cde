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
 * (c) Copyright 1989, 1990, 1991, 1992 OPEN SOFTWARE FOUNDATION, INC. 
 * ALL RIGHTS RESERVED 
*/ 
/* 
 * Motif Release 1.2
*/ 
/*
 * (c) Copyright 1987, 1988, 1989, 1990 HEWLETT-PACKARD COMPANY */

/*
 * Included Files:
 */

#include "WmGlobal.h" /* This should be the first include */
#include <errno.h>
#include <signal.h>
#include <unistd.h>


/*
 * include extern functions
 */

#include "WmFeedback.h"
#include "WmFunction.h"


/*
 * Function Declarations:
 */

#include "WmSignal.h" 


/*
 * Global Variables:
 */



/*************************************<->*************************************
 *
 *  Signal handling in the window manager
 *
 *  The signals fall into two groups.
 *
 *  Termination signals (INT, HUP, QUIT, TERM): the process is healthy, but
 *  the shutdown (the confirmation dialog, Xlib requests, saving the
 *  resources, closing the display) is not async-signal-safe and may
 *  interrupt Xlib or the toolkit half way.  QuitWmSignalHandler() therefore
 *  calls only XtNoticeSignal(); QuitWmSignalProc() runs from the event loop.
 *  A second signal while the first is still pending leaves at once with
 *  _exit().  Until SetupWmSignalDeferral() has registered the procedure, which
 *  happens right after the application context is created and before the
 *  display is opened, the handler has nothing to clean up and calls _exit().
 *
 *  Fatal signals (ILL, FPE, BUS, SEGV, SYS): the process state is already
 *  damaged and an event loop may never run again, so the work cannot be
 *  deferred.  AbortWmSignalHandler() keeps the best-effort reset of the
 *  keyboard focus, which is not async-signal-safe, so that the user is not
 *  left without a focus window.  To limit the risk, it is installed with
 *  SA_RESETHAND (the disposition is the default on entry, so a fault in the
 *  handler ends the process with the default action instead of recursing) and
 *  SA_NODEFER (the signal is not blocked, so that such a fault is delivered
 *  instead of being held back), and a second entry leaves with _exit().
 *
 *************************************<->***********************************/

static XtSignalId quitSignalId = 0;
static volatile sig_atomic_t quitRequested = 0;
static volatile sig_atomic_t abortEntered = 0;


/*************************************<->*************************************
 *
 *  AbortWmSignalHandler ()
 *
 *
 *  Description:
 *  -----------
 *  This function is called on receipt of a fatal signal. We reset
 *  the keyboard focus to pointer root before aborting.  See the comment
 *  above for why this best effort stays in the handler.
 *
 *************************************<->***********************************/
static void
AbortWmSignalHandler (int sig)
{
    struct sigaction sa;

    if (abortEntered)
    {
	_exit (1);
    }
    abortEntered = 1;

    /*
     * Set input focus back to pointer root
     */
    if (DISPLAY)
    {
	XSetInputFocus(DISPLAY, PointerRoot, RevertToPointerRoot, CurrentTime);
	XSync (DISPLAY, False);
	XCloseDisplay (DISPLAY);
    }

    /*
     * Invoke the default handler
     */
    (void) sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;
    (void) sigaction (sig, &sa, (struct sigaction *) 0);

    kill (getpid(), sig);

} /* END OF FUNCTION AbortSignalHandler */


/*************************************<->*************************************
 *
 *  RestoreDefaultSignalHandlers ()
 *
 *
 *  Description:
 *  -----------
 *  This function sets up the signal handlers for the window manager.
 *
 *************************************<->***********************************/

void
RestoreDefaultSignalHandlers (void)
{
    struct sigaction sa;
    struct sigaction osa;

    /*
     * Restore default action for signals we're interested in.
     */

    (void) sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;

    if ((sigaction (SIGINT, (struct sigaction *) 0, &osa) != 0) ||
	(osa.sa_handler != SIG_IGN))
    {
	(void) sigaction (SIGINT, &sa, (struct sigaction *) 0);
    }

    if ((sigaction (SIGHUP, (struct sigaction *) 0, &osa) != 0) ||
	(osa.sa_handler != SIG_IGN))
    {
	(void) sigaction (SIGHUP, &sa, (struct sigaction *) 0);
    }

    (void) sigaction (SIGQUIT, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGTERM, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGPIPE, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGCHLD, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGILL, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGFPE, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGBUS, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGSEGV, &sa, (struct sigaction *) 0);
#ifdef SIGSYS
    (void) sigaction (SIGSYS, &sa, (struct sigaction *) 0);
#endif
}


/*************************************<->*************************************
 *
 *  SetupWmSignalHandlers ()
 *
 *
 *  Description:
 *  -----------
 *  This function sets up the signal handlers for the window manager.
 *
 *************************************<->***********************************/

void SetupWmSignalHandlers (int dummy)
{
    struct sigaction 	sa;
    struct sigaction 	osa;

    /*
     * Catch software signals that we ask the user about
     * before quitting.
     */
    (void) sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = QuitWmSignalHandler;

    if ((sigaction (SIGINT, (struct sigaction *) 0, &osa) != 0) ||
	(osa.sa_handler != SIG_IGN))
    {
	(void) sigaction (SIGINT, &sa, (struct sigaction *) 0);
    }

    if ((sigaction (SIGHUP, (struct sigaction *) 0, &osa) != 0) ||
	(osa.sa_handler != SIG_IGN))
    {
	(void) sigaction (SIGHUP, &sa, (struct sigaction *) 0);
    }

    (void) sigaction (SIGQUIT, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGTERM, &sa, (struct sigaction *) 0);

    /*
     * Ignore child death
     */

#ifdef SA_NOCLDWAIT
    sa.sa_flags = SA_NOCLDWAIT;  /* Don't create zombies */
#else
    sa.sa_flags = 0;
#endif
    sa.sa_handler = SIG_IGN;
    (void) sigaction (SIGCHLD, &sa, (struct sigaction *) 0);
    sa.sa_flags = 0;

    /* 
     * Catch other fatal signals so we can reset the 
     * keyboard focus to pointer root before aborting
     * (best effort; see the comment at the top of the signal handling)
     */
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    sa.sa_handler = AbortWmSignalHandler;

    (void) sigaction (SIGILL, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGFPE, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGBUS, &sa, (struct sigaction *) 0);
    (void) sigaction (SIGSEGV, &sa, (struct sigaction *) 0);
#ifdef SIGSYS
    (void) sigaction (SIGSYS, &sa, (struct sigaction *) 0);
#endif

} /* END OF FUNCTION SetupWmSignalHandlers */


/*************************************<->*************************************
 *
 *  QuitWmSignalProc ()
 *
 *
 *  Description:
 *  -----------
 *  Runs from the event loop after QuitWmSignalHandler() called
 *  XtNoticeSignal(): asks for confirmation or quits the window manager.
 *
 *************************************<->***********************************/

static void
QuitWmSignalProc (XtPointer clientData, XtSignalId *id)
{
    (void) clientData;
    (void) id;

    /* a signal that arrives from now on is a new request */
    quitRequested = 0;

    if (wmGD.showFeedback & WM_SHOW_FB_KILL)
    {
	ConfirmAction (ACTIVE_PSD, QUIT_MWM_ACTION);
	XFlush(DISPLAY);
    }
    else
    {
	Do_Quit_Mwm(False);
    }

} /* END OF FUNCTION QuitWmSignalProc */


/*************************************<->*************************************
 *
 *  SetupWmSignalDeferral ()
 *
 *
 *  Description:
 *  -----------
 *  Registers the procedure that runs the quit request with the application
 *  context; from now on QuitWmSignalHandler() defers to the event loop.
 *
 *************************************<->***********************************/

void SetupWmSignalDeferral (XtAppContext appContext)
{
    quitSignalId = XtAppAddSignal (appContext, QuitWmSignalProc, nullptr);

} /* END OF FUNCTION SetupWmSignalDeferral */


/*************************************<->*************************************
 *
 *  QuitWmSignalHandler ()
 *
 *
 *  Description:
 *  -----------
 *  This function is called on receipt of a signal that is to terminate the
 *  window manager.  It calls only async-signal-safe functions.
 *
 *************************************<->***********************************/

void QuitWmSignalHandler (int dummy)
{
    int savedErrno = errno;

    (void) dummy;
    if (quitSignalId == 0)
    {
	/* before the application context exists nothing needs cleaning up */
	_exit (0);
    }
    if (quitRequested)
    {
	_exit (0);
    }
    quitRequested = 1;
    XtNoticeSignal (quitSignalId);
    errno = savedErrno;

} /* END OF FUNCTION QuitWmSignalHandler */
