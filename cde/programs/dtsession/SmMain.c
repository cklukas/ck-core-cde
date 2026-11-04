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
/* $TOG: SmMain.c /main/18 1998/04/20 12:59:26 mgreess $ */
/*                                                                      *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company                     *
 * (c) Copyright 1993, 1994 International Business Machines Corp.       *
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.                      *
 * (c) Copyright 1993, 1994 Novell, Inc.                                *
 */
/*************************************<+>*************************************
 *****************************************************************************
 **
 **  File:        SmMain.c
 **
 **  Project:     HP DT Session Manager (dtsession)
 **
 **  Description:
 **  -----------
 **  This is the controlling program for the session manager.  It
 **  calls routines to start the BMS, initialize globals and handlers,
 **  and restore the correct session.
 **
 **
 *******************************************************************
 **  (c) Copyright Hewlett-Packard Company, 1990.  All rights are
 **  reserved.  Copying or other reproduction of this program
 **  except for archival purposes is prohibited without prior
 **  written consent of Hewlett-Packard Company.
 ********************************************************************
 **
 **
 **
 *****************************************************************************
 *************************************<+>*************************************/

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>

#if defined (USE_X11SSEXT)
#include <X11/extensions/scrnsaver.h>
#endif /* USE_X11SSEXT */

#include <Xm/Xm.h>
#include <Dt/UserMsg.h>
#include <Dt/EnvControlP.h>
#include <Dt/DtP.h>
#include <Dt/Lock.h>
#ifdef USE_XINERAMA
#include <Dt/DtXinerama.h>		/* JET - Xinerama support */
#endif
#include "Sm.h"
#include "SmError.h"
#include "SmGlobals.h"
#include "SmCommun.h"
#include "SmRestore.h"
#include "SmUI.h"
#include "SrvPalette.h"
#include "SmProtocol.h"
#include "SmXSMP.h"
#include "SmPriv.h"

/*
 * Internal Functions
 */
static void StopAll(int i);
static void StopRequested(XtPointer client_data, int *source, XtInputId *id);
static void CatchStopSignal(void);
static void WatchStopSignal(void);
static void StopPipeFailed(void);
static int RegisterX11ScreenSaver(Display *display, int *ssEventType);
static int UsesProtectedPasswordFile(void);

/*
 * The pipe a SIGTERM handler leaves its request in
 */
static int stopPipe[2] = { -1, -1 };

/*
 * Internal Defines
 */
#ifdef _AIX
#define SECURE_SYS_PATH "/etc/security/passwd"
#endif
#if defined(SVR4) || defined(__linux__)
#define SECURE_SYS_PATH "/etc/shadow"
#endif
#ifdef CSRG_BASED
#define SECURE_SYS_PATH "/etc/master.passwd"
#endif

/*
 * Systems that keep the passwords in a file only the super user can read need
 * the installed identity of dtsession (setuid root) to check a password when
 * the display is unlocked.
 */
static int
UsesProtectedPasswordFile(void)
{
#ifdef SECURE_SYS_PATH
    struct stat buf;

    return stat(SECURE_SYS_PATH, &buf) != -1;
#else
    return 0;
#endif
}



/*************************************<->*************************************
 *
 *  main (argc, argv)
 *
 *
 *  Description:
 *  -----------
 *  Controls the startup and event dispatching of the session manager.
 *
 *
 *  Inputs:
 *  ------
 *  argc = command line options
 *  argv = number of command line options
 *
 *
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 *
 *************************************<->***********************************/
int
main (int argc, char **argv)
{
    int                         n, tmp;
    Arg                         args[10];
    XEvent                      next;
    String                      tmpString;
    XWindowAttributes           windAtt;
    XPropertyEvent              *pEvent = (XPropertyEvent *) &next;
    Display                     *srvDisplay;
    struct sigaction            stopvec;
    char		 	*lang;

    /*
     * Until this has happened the program runs with the identity it is
     * installed with, so nothing that reads the environment (the locale, the
     * toolkit) comes before it.  Systems that keep their passwords in a
     * protected file need the installed identity again to unlock the display.
     */
    SmPrivDropInstalledIdentity(UsesProtectedPasswordFile());

    setlocale( LC_ALL, "" );
    XtSetLanguageProc( NULL, NULL, NULL );

    smGD.smState = IN_PROCESS;
    smGD.programName = strdup (argv[0]);

    /* JET - By default, we always want to leave when SmExit() is called. */
    smGD.ExitComplete = True;

    /*
     * We report some errors before we call the DtInitialize procedure.
     * The Dt initialize procedure sets this name.  If we report
     * an error via DtSimple error before this is set we see the message
     * prepended with <unknown program name> in the error log.
     */
    DtProgName = SM_RESOURCE_NAME ;

    /*
     * Initialize LANG if it isn't defined.
     */
    if ((lang = getenv ("LANG")) == NULL)
    {
	lang = XtMalloc (7);
	(void) strcpy (lang, "LANG=C");
	(void) putenv (lang);
    }

   /*
    * Set up POSIX sigaction structs
    */

    /*
     * Must set SIGPIPE to SIG_IGN so the process does not exit
     * if a child terminates unexpectedly during an I/O operation
     * that raises SIGPIPE.  Note that before this process exec's
     * any subprocesses, SIGPIPE must be set to SIG_DFL.
     */
    sigemptyset(&stopvec.sa_mask);
    stopvec.sa_flags = 0;
    stopvec.sa_handler = SIG_IGN;
    (void) sigaction(SIGPIPE, &stopvec, (struct sigaction *) NULL);

    smGD.childvec.sa_handler = WaitChildDeath;
    sigemptyset(&smGD.childvec.sa_mask);
    smGD.childvec.sa_flags = 0;

    smGD.defvec.sa_handler = SIG_DFL;
    sigemptyset(&smGD.defvec.sa_mask);
    smGD.defvec.sa_flags = 0;

    smGD.appCon = 0;
    smGD.display = 0;

    /*
     * This must be done before XtToolkitInitialize
     * to set up the local environment
     */
    _DtEnvControl(DT_ENV_SET);
    _DtEnvControl(DT_ENV_SET_BIN);

    /*
     * Set up NLS error messages first
     */
    InitNlsStrings();

    /*
     * Set the paths of where resources are to be restored from
     */
    SetRestorePath(argc, argv);

    /*
     * The first thing that must happen is that resources must be restored
     * so that my resources will be correct
     */
    if(smGD.compatMode == False)
    {
     /*
      * Load session resources.
      */
      RestoreResources(False,
                       "-load",
                       "-system",
                       "-xdefaults",
                       smGD.resourcePath[0] != '\0' ? "-file" : NULL,
                       smGD.resourcePath,
                       NULL);
    }

    /*
     * Set up to catch SIGTERM
     */
    CatchStopSignal();

    /*
     * Now set up a communication with the toolkit
     *
     * Create one display connection for dtsession, and one for
     * the color server. We cannot share a display connection since
     * motif creates a display object for the color server's display during
     * color server initialization. Since the color server is not yet
     * operational, any dialogs (ie the dtsession logout confirmation
     * dialogs) created on that display do not get the color server colors.
     * The dtsession display object is created after color server
     * initialization is complete.
     */
    if (smGD.appCon == 0) {
	/*
	 * The following code will have been initialized if the
	 * session creation code posted a dialog.
	 */
        XtToolkitInitialize();
        smGD.appCon = XtCreateApplicationContext();
        smGD.display = XtOpenDisplay(smGD.appCon, NULL, argv[0],
				SM_RESOURCE_CLASS,
				NULL, 0, &argc, argv);
    }

    /*
     * A shutdown requested by SIGTERM is carried out from the event loop
     */
    WatchStopSignal();

    srvDisplay = XtOpenDisplay(smGD.appCon, NULL, argv[0], SM_RESOURCE_CLASS,
                                 NULL, 0, &argc, argv);

    /*
     * Initialize XSMP
     */
    if (!InitXSMP())
        SM_EXIT(-1);

    /* Added the following check to exit in case of error RK 09.08.93 */
    if(smGD.display == NULL)
    {
        PrintError(DtError, GETMESSAGE(4, 1, "Invalid display name - exiting."));
        SM_EXIT(-1);
    }

    /*
     * Lock out other session managers from running - if there is one
     * running already - exit
     */
    if(!_DtGetLock(smGD.display, SM_RUNNING_LOCK))
    {
        PrintError(DtError, GETMESSAGE(2, 2, "Another dtsession is currently running - exiting."));
        SM_EXIT(-1);
    }

    /* JET - initialize for Xinerama, if present 4/12/2001 */
#ifdef USE_XINERAMA
    smGD.DtXineramaInfo = _DtXineramaInit(smGD.display);

# ifdef DEBUG
    if (smGD.DtXineramaInfo == NULL)
      {                         /* No xinerama, how... sad. */
        fprintf(stderr, "### JET SmMain: Xinerama NOT available.\n");
      }
    else
      {
        fprintf(stderr, "### JET SmMain: Xinerama available, scrns = %d\n",
                dpyinfo.DtXineramaInfo->numscreens);
      }
# endif

#endif


   /*
    * Restore preferences
    */
    if (smGD.resourcePath[0] != '\0')
    {
      RestorePreferences(smGD.resourcePath);
    }

    /*
     * Start up the color server
     */
    InitializeDtcolor(srvDisplay, smGD.sessionType);

    /*
     * AFTER the colors are set up - create the top level widget
     * Set up a NULL WM_COMMAND property
     */
    n = 0;
    XtSetArg(args[n], XmNbackground,
             XBlackPixel(smGD.display, XDefaultScreen(smGD.display))); n++;
    XtSetArg(args[n], XmNmappedWhenManaged, False); n++;
    XtSetArg (args[n], XmNwidth, 1); n++;
    XtSetArg (args[n], XmNheight, 1); n++;
    smGD.topLevelWid = XtAppCreateShell (SM_RESOURCE_NAME, SM_RESOURCE_CLASS,
                                         applicationShellWidgetClass,
                                         smGD.display, args, n);
    XtRealizeWidget(smGD.topLevelWid);

    /*
     * Select to get the lock on timeout if the user requests it
     * and if the server supports it. This must be done before
     * InitSMGlobals() so smGD.lockOnTimeoutStatus is known.
     */
#if defined (USE_HPSSEXT)
#ifdef USE_HP_SPECIFIC_XLIB
    tmp = XHPSSChangeNotify(smGD.display, &XaSmScreenSaveRet, 3);
#else /* USE_HP_SPECIFIC_XLIB */
    tmp = -1;
#endif /* USE_HP_SPECIFIC_XLIB */
#elif defined (USE_X11SSEXT)
    tmp = RegisterX11ScreenSaver(smGD.display, &smGD.ssEventType);
#else
    tmp = -1;
#endif

    if(tmp != 0)
    {
	smGD.lockOnTimeoutStatus = False;
	XaSmScreenSaveRet = None;
#if defined (USE_X11SSEXT)
	smGD.ssEventType = None;
#endif
    }
    else
    {
	smGD.lockOnTimeoutStatus = True;
    }

    InitSMGlobals();

    /*
     * Put the program into a wait state
     */
    EnterWaitState();

    InitErrorHandler();

    /*
     * Restore resources for lang/resolution independence
     */
    if((smGD.resourcePath[0] != 0) || (smGD.compatMode == False))
    {
	RestoreIndependentResources();
    }

    /*
     * Now restore the rest of the clients and the settings
     */
    if((smGD.clientPath[0] != 0) && (smGD.compatMode == False))
    {
	if(RestoreState() == -1)
	{
	    StartWM();
	}
    }
    else
    {
	if(smGD.compatMode == True)
	{
	    /*
	     * dtstyle needs the DT_SESSION_STATE  even in
	     * compatibility mode
	     */
	    SetCompatState();
	}
	StartWM();
    }

    /*
     * Run the user's startup script if there is one
     */

    if(smGD.compatMode == False)
    {
	StartEtc(False); /* run sessionetc */
    }

    InitProtocol ();

    /*
     * If we are in compatibility mode - kill the parent and leave the
     * child running so that the script exits
     */
    if(smGD.compatMode == True)
    {
	KillParent();
    }

    /*
     * Select for property notify on the top level window - so that
     * when the style manager changes something - it is known
     */
    XGetWindowAttributes(smGD.display, smGD.topLevelWindow, &windAtt);
    XSelectInput(smGD.display, smGD.topLevelWindow,
		 windAtt.your_event_mask | PropertyChangeMask);

    LeaveWaitState();

    /*
     * Register for events
     * - PropertyChange (maskable)
     * - ClientMessage (non-maskable)
     */
    XtAddEventHandler(smGD.topLevelWid,
                      PropertyChangeMask,
                      True, ProcessEvent, NULL);

    smGD.smState = READY;

    while(1)
    {
      XtAppNextEvent(smGD.appCon, &next);
      if (next.type != 0)
      {
#if defined (USE_X11SSEXT)
        if (next.type == smGD.ssEventType)
        {
         /*
          * We should simply be calling XtDispatchEvent() but the toolkit
          * doesn't seem to know how to dispatch a run-time generated
          * event type.
          */
          ProcessEvent(smGD.topLevelWid, NULL, &next, NULL);
        }
        else
        {
          XtDispatchEvent(&next);
        }
#else
        XtDispatchEvent(&next);
#endif
      }
    }
}




/*************************************<->*************************************
 *
 *  StopAll
 *
 *
 *  Description:
 *  -----------
 *  Signal handler for SIGTERM. Causes dtsession to do a normal shutdown
 *  procedure without saving any state.
 *
 *
 *  Inputs:
 *  ------
 *  shutDown (state flag)
 *
 *
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 *
 *  We want to be careful to ignore the TERM signal if we are already
 *  shutting down when it occurs.
 *
 *************************************<->***********************************/

static void
StopAll(int i)
{
    int savedErrno = errno;
    ssize_t n;

    /*
     * Only async-signal-safe calls are made here.  The shutdown itself talks
     * to ToolTalk and X, allocates memory and runs a nested event loop, none
     * of which may run while the interrupted code is inside malloc or Xlib:
     * the handler leaves a byte in a pipe that the event loop reads.
     */
    if (stopPipe[1] != -1)
    {
        n = write(stopPipe[1], "x", 1);
        (void) n;
    }
    errno = savedErrno;
}

/*************************************<->*************************************
 *
 *  StopRequested
 *
 *
 *  Description:
 *  -----------
 *  Input procedure of the pipe StopAll writes to: carries out the shutdown
 *  a SIGTERM asked for.  ImmediateExit() drops a request that comes in while
 *  a shutdown is under way and gives the signal back when the shutdown is
 *  cancelled.
 *
 *************************************<->***********************************/

static void
StopRequested(XtPointer client_data, int *source, XtInputId *id)
{
    char buffer[16];
    ssize_t n;

    do
    {
        n = read(stopPipe[0], buffer, sizeof(buffer));
    } while (n == (ssize_t) sizeof(buffer));

    ImmediateExit(-1, 0, True);
}

/*************************************<->*************************************
 *
 *  CatchStopSignal
 *
 *
 *  Description:
 *  -----------
 *  Create the pipe StopAll writes to and catch SIGTERM.  If that cannot be
 *  done the signal gets its default action, which ends the process at once,
 *  whatever disposition the program was started with.
 *
 *************************************<->***********************************/

static void
StopPipeFailed(void)
{
    PrintErrnoError(DtError, GETMESSAGE(4, 12, "Unable to set up the termination signal.  The process ends at once, without saving the session, when it is terminated."));
}

static void
CatchStopSignal(void)
{
    struct sigaction action;
    int i;

    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (pipe(stopPipe) != 0)
    {
        StopPipeFailed();
        stopPipe[0] = stopPipe[1] = -1;
    }
    else
    {
        /* the handler must never block, and no program started later gets them */
        for (i = 0; i < 2; i++)
        {
            if (fcntl(stopPipe[i], F_SETFL, O_NONBLOCK) != 0 ||
                fcntl(stopPipe[i], F_SETFD, FD_CLOEXEC) != 0)
            {
                StopPipeFailed();
                close(stopPipe[0]);
                close(stopPipe[1]);
                stopPipe[0] = stopPipe[1] = -1;
                break;
            }
        }
    }

    action.sa_handler = (stopPipe[1] != -1) ? StopAll : SIG_DFL;
    if (sigaction(SIGTERM, &action, (struct sigaction *) NULL) != 0 &&
        stopPipe[1] != -1)
    {
        StopPipeFailed();
        close(stopPipe[0]);
        close(stopPipe[1]);
        stopPipe[0] = stopPipe[1] = -1;
    }
}

/*************************************<->*************************************
 *
 *  WatchStopSignal
 *
 *
 *  Description:
 *  -----------
 *  Have the event loop of the application read the pipe of CatchStopSignal.
 *
 *************************************<->***********************************/

static void
WatchStopSignal(void)
{
    if (stopPipe[0] != -1)
    {
        XtAppAddInput(smGD.appCon, stopPipe[0], (XtPointer) XtInputReadMask,
                      StopRequested, NULL);
    }
}

/*************************************<->*************************************
 *
 *  RegisterX11ScreenSaver
 *
 *
 *  Description:
 *  -----------
 *  Register with X11 screen saver server extension for screen saver events.
 *
 *  Inputs:
 *  ------
 *  display - display from XtOpenDisplay()
 *  pssEventType - pointer to buffer in which to return ss event type
 *
 *  Outputs:
 *  -------
 *  pssEventType - (rc=0) screen saver event type, (rc!=0) undefined
 *
 *  Returns:
 *  -------
 *  0 - successfully registered for screen saver events
 *  other - failed to register for screen saver events
 *
 *  Comments:
 *  --------
 *
 *************************************<->***********************************/

#if defined (USE_X11SSEXT)
static int
RegisterX11ScreenSaver(
  Display *display,
  int *pssEventType)
{
 /*
  * Register with X11 screen saver server extension.
  */
  int result = -1;
  int ssErrorBase;
  int majorVersion;
  int minorVersion;
  int screen;
  Window root;
  XID xid;
  Atom type;

  if (XScreenSaverQueryExtension(display, pssEventType, &ssErrorBase) &&
      XScreenSaverQueryVersion(display, &majorVersion, &minorVersion) &&
      majorVersion == 1)
  {
   /*
    * Server supports requested version of X11 screen saver extension.
    */
    screen = DefaultScreen(display);
    root = DefaultRootWindow(display);

    XGrabServer(display);
    if (!XScreenSaverGetRegistered(display, screen, &xid, &type))
    {
     /*
      * No other clients registered with this server so register this one.
      */
      XScreenSaverRegister(display, screen, XtWindow(smGD.topLevelWid), XA_WINDOW);
      result = 0;
    }
    XUngrabServer(display);

    if (result == 0)
    {
      XSetWindowAttributes attr;

     /*
      * Registration successful.
      */
      XScreenSaverSelectInput(display, root,
                              ScreenSaverNotifyMask|ScreenSaverCycleMask);

      /* Even though OverrideRedirect is the default attribute in this
       *  check, lets make sure it's set, and make sure only the
       *  CWOverrideRedirect attr is being looked at.  The rest is
       *  just random garbage anyway, since it's allocated from the stack.
       */
      attr.override_redirect = True;
      XScreenSaverSetAttributes(display, root, 0, 0, 1, 1, 0, CopyFromParent,
                          CopyFromParent, CopyFromParent, CWOverrideRedirect, &attr);
    }
  }
  return(result);
}
#endif /* USE_X11SSEXT */
