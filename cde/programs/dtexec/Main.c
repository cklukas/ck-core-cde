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
 * (c) Copyright 1995 Digital Equipment Corporation.
 * (c) Copyright 1993, 1994, 1995 Hewlett-Packard Company
 * (c) Copyright 1993, 1994, 1995 International Business Machines Corp.
 * (c) Copyright 1993, 1994, 1995 Sun Microsystems, Inc.
 * (c) Copyright 1993, 1994, 1995 Novell, Inc. 
 * (c) Copyright 1995 FUJITSU LIMITED.
 * (c) Copyright 1995 Hitachi.
 */
/******************************************************************************
 *
 * File:         Main.c
 * RCS:          $TOG: Main.c /main/9 1999/09/20 15:36:11 mgreess $
 * Package:      dtexec for CDE 1.0
 *
 *****************************************************************************/

#define SPINBLOCK	if (getenv("_DTEXEC_DEBUG")) {int i; i=1; while(i) i=1;}

/*
 * _DTEXEC_NLS16 controls whether I18N code should be used.   As of 7/94,
 * libDtSvc should be the only one using dtexec, and the command line
 * and parsing code are I18N insensitive, and thats o.k.
 *
 * If turned on, some routines from DtSvc/DtUtil2/DtNlUtils.c will be
 * needed.
 */
#undef _DTEXEC_NLS16

#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <sys/wait.h>

#include "osdep.h"	/* select(2) mask width and bit manipulation macros */
#include <Tt/tt_c.h>
#include <locale.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <Tt/tttk.h>

#define ACTION_ICON_CACHE_UPDATE_OP "DtActionIconCache_Update"
#define ACTION_ICON_CACHE_DELAY_SECONDS 5
#define DTEXEC_ACTION_ICON_LOG_ENV "DTEXEC_ACTION_ICON_CACHE_LOG"

#include <Dt/MsgLog.h>

/*
 * From Dt/ActionP.h - never change since these are in effect protocol
 * codes!   See Dt/ActionP.h for more details.
 */
#ifndef _ActionP_h
#define _DtActCHILD_UNKNOWN             (1<<0)  /* 1 - child status unknown */
#define _DtActCHILD_PENDING_START       (1<<1)  /* 2 - child start pending */
#define _DtActCHILD_ALIVE_UNKNOWN       (1<<2)  /* 4 - child alive but unknown*/
#define _DtActCHILD_ALIVE               (1<<3)  /* 8 - child alive and well */
#define _DtActCHILD_DONE                (1<<4)  /* 16 - child done */
#define _DtActCHILD_FAILED              (1<<5)  /* 32 - child failed */
#define _DtActCHILD_CANCELED            (1<<6)  /* 64 - child canceled */
#endif /* _ActionP_h */

#define MAX_EXEC_ARGS		1000    /* Maximum number of arguments for */
					/* execvp call. */
#define True		1
#define False		0
#define PERM_TERM	1
#define TRANSIENT	2
#define SHORT		3

/*
 * Triggered by a SIGCLD, the shutdown sequence.
 */
#define SDP_DONE_STARTING	1
#define SDP_DONE_REPLY_WAIT	2
#define SDP_DONE_REPLIED	3
#define SDP_DONE_PANIC_CLEANUP	4
#define SDP_FINAL_LINGER	5

/*
 * Timeout period in milliseconds for select() when we are rediscovering
 * signals and engage in a shutdown process.   More often than not, we
 * will services signals out of select() immediately.   The only interesting
 * moment is waiting for a Done(Reply).
 */
#define SHORT_SELECT_TIMEOUT	20

/*
 * External system calls:
 */
extern pid_t 	fork ();
extern int 	execvp ();
extern pid_t 	wait ();
extern int 	atoi ();
extern void 	_exit ();
extern unsigned int 	sleep ();
extern void 	exit ();
extern char     *getenv();

/*
 * Local func protos
 */
void DoneRequest(int doneCode);
static void DtexecLog(const char *fmt, ...);
static void MaybeSendActionIconCacheUpdate(void);
static char *CollectChildCommandList(pid_t pid, int *count);
static Boolean ReadProcCmdline(pid_t pid, char *buffer, size_t size);

/*
 * Global variables.
 */
struct timeval startTimeG;
struct timezone zoneG;
int    requestTypeG;
pid_t  childPidG;		/* PID of child we fork/exec */
long   waitTimeG;		/* -open setting */
char  *dtSvcProcIdG;		/* TT Proc ID of our caller */
int    dtSvcInvIdG;		/* Invocation ID that our caller manages us by*/
int    dtSvcChildIdG;		/* Child ID that our caller manages us by */
int    tmpFileCntG;		/* tmp file count */
char **tmpFilesG;		/* tmp files we might need to unlink */
fd_set allactivefdsG;		/* all possible fildes of interest. */
				/*    - mskcnt generated from osdep.h */
int    rediscoverSigCldG;	/* if a SIGCLD goes off */
int    rediscoverUrgentSigG;	/* if a SIGTERM, SIGHUP or SIGQUIT goes off */
int    shutdownPhaseG;		/* shutdown progress state variable */
int    ttfdG;			/* tooltalk fildes */
int    errorpipeG[2];		/* dtexec <--< child   stderr pipe */
static char *actionCommandG = NULL;
static Boolean actionIconCacheNotified = False;
static struct timeval actionIconCacheDeadlineG;
static Boolean actionIconCacheDeadlineSet = False;


/******************************************************************************
 *
 * Minaturized versions of tttk_*() routines.   Used so we don't have to
 * pull in tttk which in turn would pull in Xt.
 *
 *****************************************************************************/

char  *dtexec_Tttk_integer    = "integer";
char  *dtexec_Tttk_message_id = "messageID";

Tt_message
dtexec_tttk_message_create(
	Tt_message		context,
	Tt_class		theClass,
	Tt_scope		theScope,
	const char	       *handler,
	const char	       *op,
	Tt_message_callback	callback
)
{
	Tt_message msg;
	Tt_address address;
	Tt_status status;


	msg = tt_message_create();
	status = tt_ptr_error( msg );
	if (status != TT_OK) {
		return msg;
	}

	status = tt_message_class_set( msg, theClass );
	if (status != TT_OK) {
		return (Tt_message)tt_error_pointer( status );
	}

	status = tt_message_scope_set( msg, theScope );
	if (status != TT_OK) {
		return (Tt_message)tt_error_pointer( status );
	}

	address = TT_PROCEDURE;
	if (handler != 0) {
		status = tt_message_handler_set( msg, handler );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		address = TT_HANDLER;
	}

	status = tt_message_address_set( msg, address );
	if (status != TT_OK) {
		return (Tt_message)tt_error_pointer( status );
	}

	if (op != 0) {
		status = tt_message_op_set( msg, op );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	}

	if (callback != 0) {
		status = tt_message_callback_add( msg, callback );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	}

	return msg;
}

Tt_status
dtexec_tttk_message_destroy(
	Tt_message msg
)
{
	Tt_status status;

	status = tt_message_destroy(msg);
	return status;
}

int
dtexec_tttk_message_am_handling(
        Tt_message msg
)
{
	char *handler;
	int am_handling;

        if (tt_message_class( msg ) != TT_REQUEST) {
                return 0;
        }
        if (tt_message_state( msg ) != TT_SENT) {
                return 0;
        }
        handler = tt_message_handler( msg );
        am_handling = 0;
        if ((tt_ptr_error( handler ) == TT_OK) && (handler != 0)) {
                am_handling = 1;
        }
        tt_free( handler );
        return am_handling;
}

Tt_status
dtexec_tttk_message_abandon(
        Tt_message msg
)
{
	int fail;
	Tt_status status;

        if (dtexec_tttk_message_am_handling( msg )) {
                fail = 0;
                if (tt_message_address( msg ) == TT_HANDLER) {
                        fail = 1;
                } else if (tt_message_status( msg ) == TT_WRN_START_MESSAGE) {
                        fail = 1;
                }

                if (fail) {
			if (tt_message_class( msg ) == TT_REQUEST) {
				tt_message_status_set(msg, TT_DESKTOP_ENOTSUP);
				status = tt_message_fail( msg );
				dtexec_tttk_message_destroy( msg );
			}
			else {
				status = dtexec_tttk_message_destroy( msg );
			}
                } else {
			tt_message_status_set( msg, TT_DESKTOP_ENOTSUP );
			status = tt_message_reject( msg );
			dtexec_tttk_message_destroy( msg );
                }

        } else {
                status = dtexec_tttk_message_destroy( msg );
        }

	return status;
}


/******************************************************************************
 *
 * Help - print the usage and exit.
 *
 *****************************************************************************/

static void
Help(
        char *argv[] )
{
   (void) fprintf (stderr, "Usage:\n");
   (void) fprintf (stderr, "\t%s [-options ...] cmd [cmd arg ...]\n", argv[0]);
   (void) fprintf (stderr, "\n");
   (void) fprintf (stderr, "where options include:\n");
   (void) fprintf (stderr, "\t-open open-option\n");
   (void) fprintf (stderr, "\t\t-1 (default) continue to execute after cmd terminates,\n");
   (void) fprintf (stderr, "\t\t   thus keeping the terminal window open.\n");
   (void) fprintf (stderr, "\t\t 0 exit as soon as cmd terminates, thus allowing\n");
   (void) fprintf (stderr, "\t\t   the terminal window to close.\n");
   (void) fprintf (stderr, "\t\t n continue to execute if cmd terminates within n\n");
   (void) fprintf (stderr, "\t\t   seconds of starting.\n");
   (void) fprintf (stderr, "\t-ttprocid procid\n");
   (void) fprintf (stderr, "\t-tmp tmpfile [-tmp tmpfile ...]\n");
   (void) fprintf (stderr, "\n");
   _exit(0);
}


/******************************************************************************
 *
 * PanicSignal - see InitializeSignalHandling()
 *
 *****************************************************************************/

static void
#if defined(__aix) || defined(CSRG_BASED) || defined(__linux__)
PanicSignal(int s)
#else
PanicSignal(void)
#endif /* __aix */
{
    int i;

    /*
     * Crude, but let libDtSvc know we've been forced down.
     * Atleast libDtSvc will get a hint and know to cleanup.
     */
    if (dtSvcProcIdG)
	DoneRequest(_DtActCHILD_FAILED);

    if (!dtSvcProcIdG) {
	/*
	 * We cannot talk with caller, so do cleanup
	 * of tmp files.
	 */
	for (i = 0; i < tmpFileCntG; i++ ) {
	    chmod( tmpFilesG[i], (S_IRUSR|S_IWUSR) );
	    unlink( tmpFilesG[i] );
	}
    }

    _exit(0);
}


/******************************************************************************
 *
 * IgnoreSignal - see InitializeSignalHandling()
 *
 *****************************************************************************/

static void
#if defined(__aix) || defined(CSRG_BASED) || defined(__linux__)
IgnoreSignal(int i)
#else
IgnoreSignal(void)
#endif /* __aix */
{
    /*
     * If the child is still in the same process group, it should be
     * getting the same signal too.
     */
    if (rediscoverSigCldG) {
	if (shutdownPhaseG == SDP_FINAL_LINGER) {
	    /*
	     * We were shutdown long ago and lingering, so go ahead
	     * and exit now.
	     */
	    _exit(0);
	}
	else {
	    /*
	     * Still shutting down, so flip requestTypeG so that dtexec
	     * will not linger when the shutdown process completes.
	     */
	    requestTypeG = TRANSIENT;
	}
    }
    else {
	/*
	 * If and when the child does repond to the signal we are
	 * ignoring for now, don't allow dtexec to linger.
	 */
	requestTypeG = TRANSIENT;
    }
}

/******************************************************************************
 *
 * UrgentSignal - see InitializeSignalHandling()
 *
 *****************************************************************************/

static void
#if defined(__aix) || defined(CSRG_BASED) || defined(__linux__)
UrgentSignal(int i)
#else
UrgentSignal(void)
#endif /* __aix */
{
    /*
     * Set global so the central control point ( select() ) will
     * rediscover the urgent signal.
     */
    rediscoverUrgentSigG = 1;

    /*
     * If the child is still in the same process group, it should be
     * getting the same signal too.
     */
    if (rediscoverSigCldG) {
	if (shutdownPhaseG == SDP_FINAL_LINGER) {
	    /*
	     * We were shutdown long ago and lingering, so go ahead
	     * and exit now.
	     */
	    _exit(0);
	}
	else {
	    /*
	     * Still shutting down, so flip requestTypeG so that dtexec
	     * will not linger when the shutdown process completes.
	     */
	    requestTypeG = TRANSIENT;
	}
    }
    else {
	/*
	 * If and when the child does repond to the signal we are
	 * ignoring for now, don't allow dtexec to linger.
	 *
	 * This is mildly different than the IgnoreSignal case because
	 * there is a timeout associated with UrgentSignals.
	 */
	requestTypeG = TRANSIENT;
    }
}


/******************************************************************************
 *
 * SigCld - see InitializeSignalHandling()
 *
 *****************************************************************************/
static void
#if defined(__aix) || defined(CSRG_BASED) || defined(__linux__)
SigCld(int i)
#else
SigCld(void)
#endif /* __aix */
{
    int exitStatus;
    pid_t pid;

    /*
     * Query why the SIGCLD happened - a true termination or just a stopage.
     * We only care about terminations.
     */
    pid = wait(&exitStatus);

    if (pid == -1) {
	if (errno == ECHILD) {
	    /*
	     * No child found with wait(), so sure, act like we did
	     * see a SIGCLD.
	     */
	    rediscoverSigCldG = 1;
	}
    }
    else if (!WIFSTOPPED(exitStatus)) {
	/*
	 * The SIGCLD was *not* the result of being stopped, so the child
	 * has indeed terminated. 
	 */
	rediscoverSigCldG = 1;
    }
}


/******************************************************************************
 *
 * InitializeSignalHandling - Set up the signal catchers.
 *
 * Keep this code in sync with fork/exec code so as to NOT pass
 * these catchers onto our child.
 *
 * "Ignore"
 *    1. if already SIGCLD
 *          A. if SDP_FINAL_LINGER, _exit(0)
 *          B. else set requestTypeG to TRANSIENT
 *    2. else ignore signal
 *    3. select-loop-spin
 *
 *    comment: if fact we don't ignore the signal, but instead let the
 *             child control the pace of the response to the signal.
 *
 * "Urgent"
 *    1. if already SIGCLD
 *          A. if SDP_FINAL_LINGER, _exit(0)
 *          B. else set requestTypeG to TRANSIENT
 *    2. wait 5 seconds hoping to goto "SIGCLD" path from select-loop-spin
 *    3. else do "Panic" path after 5 seconds
 *
 * "SIGCLD"
 *    1. send Done(Request)
 *    2. wait for Done(Reply) in select-loop-spin
 *    3. send Quit(Reply)
 *    4. cleanup
 *    5. FinalLinger()
 *
 * "Panic"
 *    1. send Done(Request)
 *    2. send Quit(Reply)
 *    3. cleanup
 *    4. _exit(0)
 *
 * "Default"
 *    1. default signal action
 *
 *****************************************************************************/
static void
InitializeSignalHandling( void )
{
   long                    oldMask;
   struct sigaction        svec;

   /*
    * "Graceful Signal" handlers
    *    - SIGCLD for normal child termination - best case.
    */
   sigemptyset(&svec.sa_mask);
   svec.sa_flags   = 0;
   svec.sa_handler = SigCld;
   (void) sigaction(SIGCHLD, &svec, (struct sigaction *) NULL);

   /*
    * "Urgent Signal" handlers
    *    - SIGTERM for standard kill(1)
    *
    * We treat these signals in a special way.   When the signal comes
    * in, we hope to see the child respond with a SIGCLD within 5
    * seconds, else we act like the SIGTERM was for us and go down.
    */
   sigemptyset(&svec.sa_mask);
   svec.sa_flags   = 0;
   svec.sa_handler = UrgentSignal;
   (void) sigaction(SIGTERM, &svec, (struct sigaction *) NULL);

   /*
    * "Panic Signal" handlers.
    *    - SIGINT for BBA coverage.
    */
   sigemptyset(&svec.sa_mask);
   svec.sa_flags   = 0;
   svec.sa_handler = PanicSignal;
   (void) sigaction(SIGINT, &svec, (struct sigaction *) NULL);

   /*
    * "Ignore Signal" handlers.
    *    - SIGUSR1 - let child decide how to respond
    *    - SIGUSR2 - let child decide how to respond
    *    - SIGHUP  - let child decide how to respond
    */
   sigemptyset(&svec.sa_mask);
   svec.sa_flags   = 0;
   svec.sa_handler = IgnoreSignal;
   (void) sigaction(SIGUSR1, &svec, (struct sigaction *) NULL);
   (void) sigaction(SIGUSR2, &svec, (struct sigaction *) NULL);
   (void) sigaction(SIGHUP, &svec, (struct sigaction *) NULL);

   /*
    * "Default Signal" handlers.
    *    - SIGQUIT - let core dump happen as expected.   If done,
    *                libDtSvc will never get a done notice.
    */
}


/******************************************************************************
 *
 * After all is said and done, enter FinalLinger which
 * will decide if and how long the associated terminal
 * emulator will stay up.
 *
 *****************************************************************************/
void FinalLinger(void)
{
    int exitStatus;
    struct timeval finalTime;

    /*
     * Make sure to reap child.   The SIGCLD handler may have done
     * this already.
     */
    (void) wait(&exitStatus);

    if (requestTypeG == PERM_TERM)
	(void) sleep (99999999);
    else if (requestTypeG == TRANSIENT)
	(void) _exit (0);
    else {
	/*
	 * Check the time stamps and either sleep forever or exit.
	 */
	if ((gettimeofday (&finalTime, &zoneG)) == -1)
	    (void) _exit (1);

	if ((finalTime.tv_sec - startTimeG.tv_sec) < waitTimeG)
	    (void) sleep (99999999);
	else
	    (void) _exit (0);
    }
}


/******************************************************************************
 *
 * ExecuteCommand -
 *
 *****************************************************************************/

static int
ExecuteCommand (
	char **commandArray)
{
   int i, index1;
   int exitStatus;
   struct sigaction svec;

   for (index1 = 0; (index1 < 10) && ((childPidG = fork()) < 0); index1++) {
      /* Out of resources ? */
      if (errno != EAGAIN)
	 break;
      /* If not out of resources, sleep and try again */
      (void) sleep ((unsigned long) 2);
   }

   if (childPidG < 0) {
      return (False);
   }

   if (childPidG == 0) {
      /*
       * Child Process.
       */

      /*
       * Hook stderr to error pipe back to parent.
       */
      if ((errorpipeG[0] != -1) && (errorpipeG[1] != -1)) {
	 dup2(errorpipeG[1], 2);
	 close(errorpipeG[0]);
      }

      /*
       * The child should have default behavior for all signals.
       */
      sigemptyset(&svec.sa_mask);
      svec.sa_flags   = 0;
      svec.sa_handler = SIG_DFL;

      /* Normal */
      (void) sigaction(SIGCHLD, &svec, (struct sigaction *) NULL);

      /* Urgent */
      (void) sigaction(SIGTERM, &svec, (struct sigaction *) NULL);

      /* Panic */
      (void) sigaction(SIGINT, &svec, (struct sigaction *) NULL);

      /* Ignore */
      (void) sigaction(SIGUSR1, &svec, (struct sigaction *) NULL);
      (void) sigaction(SIGUSR2, &svec, (struct sigaction *) NULL);
      (void) sigaction(SIGHUP, &svec, (struct sigaction *) NULL);

      for (i=3; i < FOPEN_MAX; i++) {
         if ( i != errorpipeG[1] )
	     (void) fcntl (i, F_SETFD, FD_CLOEXEC);
      }

      (void) execvp(commandArray[0], commandArray);

      (void) fprintf (stderr, "Cannot execute \"%s\".\n", commandArray[0]);

      (void) _exit (1);
   }

   if (errorpipeG[1] != -1) {
      close(errorpipeG[1]);
      errorpipeG[1] = -1;
   }

   return (True);
}

/******************************************************************************
 *
 * ParseCommandLine
 *
 * Peel off options to dtexec.   As soon as an argv[n] is not a valid
 * dtexec option, assume it is the start of the cmd line and return.
 *
 *****************************************************************************/

static char **
ParseCommandLine(
        int argc,
        char **argv )
{
    char **argv2;
    char *tick1, *tick2;
#ifdef _DTEXEC_NLS16
    int  tmpi;
#endif /* _DTEXEC_NLS16 */

    argv2 = (char **) argv;

    tmpFileCntG = 0;
    tmpFilesG = (char **) NULL;

    for (argc--, argv++; argc > 0; argc--, argv++) {
	if ( ! strcmp(argv[0] , "-open") ) {
	    argc--; argv++;

	    if ( argc == 0 ) Help( argv2 );
	    waitTimeG = atoi (argv[0]);

	    if (waitTimeG < 0)
		requestTypeG = PERM_TERM;
	    else if (waitTimeG == 0)
		requestTypeG = TRANSIENT;
	    else {
		requestTypeG = SHORT;
	    }
	}
	else if ( ! strcmp(argv[0] , "-ttprocid") ) {
	    argc--; argv++;
	    if ( argc == 0 ) Help( argv2 );

	    /*
	     * Pull the -ttprocid argument apart for it's 3 components.
	     *    <libDtSvc's ProcId>_<Invocation Id>_<Child Id>
	     */
#ifdef _DTEXEC_NLS16
	    tmpi = mblen(argv[0]);
	    dtSvcProcIdG = (char *) malloc( tmpi + 1 );

	    memcpy( dtSvcProcIdG, argv[0], tmpi );
	    dtSvcProcIdG[tmpi] = NULL;
#else
	    dtSvcProcIdG = (char *) malloc( strlen(argv[0]) + 1 );
	    strcpy( dtSvcProcIdG, argv[0] );
#endif /* _DTEXEC_NLS16 */

	    /*
	     * Search from the end for underscore separators.
	     */
#ifdef _DTEXEC_NLS16
	    tick2 = (char *) Dt_strrchr( dtSvcProcIdG, '_' );
#else
	    tick2 = (char *) strrchr( dtSvcProcIdG, '_' );
#endif /* _DTEXEC_NLS16 */

            if (tick2)
		*tick2 = 0;

#ifdef _DTEXEC_NLS16
	    tick1 = (char *) Dt_strrchr( dtSvcProcIdG, '_' );
#else
	    tick1 = (char *) strrchr( dtSvcProcIdG, '_' );
#endif /* _DTEXEC_NLS16 */

	    if ( tick1 && tick2 ) {
		*tick1 = 0;
		*tick2 = 0;

		dtSvcInvIdG = atoi((char *) (tick1 + 1));
		dtSvcChildIdG = atoi((char *) (tick2 + 1));

		if ( !(dtSvcInvIdG && dtSvcChildIdG) ) {
		    /*
		     * Don't have two non-zero values, so we cannot use the
		     * -ttprocid provided.
		     */
		    free(dtSvcProcIdG);
		    dtSvcProcIdG = (char *) NULL;
		}
	    }
	    else {
		/*
		 * Unable to find _ (underscore) separators.
		 */
		free(dtSvcProcIdG);
		dtSvcProcIdG = (char *) NULL;
	    }
	}
	else if ( ! strcmp(argv[0] , "-tmp") ) {
	    argc--; argv++;
	    if ( argc == 0 ) Help( argv2 );
	    tmpFileCntG++;
	    tmpFilesG = (char **) realloc( (char *) tmpFilesG,
						tmpFileCntG * sizeof(char *) );
	    tmpFilesG[tmpFileCntG-1] = argv[0];
	}
	else if ( ! strncmp(argv[0], "-h", 2) ) {
	    Help( argv2 );
	    /*
	     * Technically we should see if a -ttprocid was given and
	     * possibly send back a Done(Request).
	     */
	    (void) _exit (1);
	}
	else {
	    return( argv );
	}
    }
    /*
     * No arguments to dtexec, so nothing to fork/exec.
     */
    return( (char **) NULL );
}

/******************************************************************************
 *
 * Shutdown Tooltalk connection.
 *
 *****************************************************************************/
void DetachFromTooltalk(
    unsigned long *nocare1)     /* if Xt - XtInputId *id; */
{
    char *sessid;

    if (dtSvcProcIdG) {
	/*
	 * NULL the global to indicate that we no longer want to
	 * chit-chat with Tooltalk.
	 */
	dtSvcProcIdG = (char *) NULL;

	sessid = tt_default_session();
	tt_session_quit(sessid);
	tt_free(sessid);
	tt_close();
    }

    /*
     * Unregister the Tooltalk fildes from the select mask.
     */
    if (ttfdG != -1) {
	BITCLEAR(allactivefdsG, ttfdG);
	ttfdG = -1;
    }
}

/******************************************************************************
 *
 * Alternate input handler to tttk_Xt_input_handler
 *
 * If we end up pulling Xt in, toss this routine and use
 * tttk_Xt_input_handler instead.
 *
 *****************************************************************************/
void
input_handler(
    char          *nocare1,	/* if Xt - XtPointer w */
    int           *nocare2,	/* if Xt - int *source */
    unsigned long *nocare3)	/* if Xt - XtInputId *id; */
{
    Tt_message msg;
    Tt_status status;


    msg = tt_message_receive();
    status = tt_ptr_error( msg );

    if (status != TT_OK) {
	/*
	 * Problem; think about bailing.
	 */
	if (status == TT_ERR_NOMP) {
	    /*
	     * Big time lost.
	     */
	    DetachFromTooltalk(NULL);
	}
	return;
    }

    if (msg == 0) {
	/*
	 * A pattern callback ate the message for us.
	 */
	return;
    }

    /*
     * All messages should have been consumed by a callback.
     * Pick between failing and rejecting the msg.
     */
    status = dtexec_tttk_message_abandon( msg );
    if (status != TT_OK) {
	/* don't care */
    }
}


/******************************************************************************
 *
 * Initiallize Tooltalk world.
 *
 *****************************************************************************/
static void
ToolTalkError(char *errfmt, Tt_status status)
{
    char	*statmsg;

    if (! tt_is_err(status)) return;

    statmsg = tt_status_message(status);
    DtMsgLogMessage( "Dtexec", DtMsgLogStderr, errfmt, statmsg );
}

int InitializeTooltalk(void)
{
    char * procid;
    Tt_status status;
    int fd;

    procid = tt_default_procid();
    status = tt_ptr_error(procid);
    if ((status == TT_ERR_NOMP) || (status == TT_ERR_PROCID)) {
	/*
	 * We need to try to establish a connection
	 */
	procid = tt_open();
	status = tt_ptr_error(procid);
	if (status != TT_OK) {
	    ToolTalkError("Could not connect to ToolTalk:\n%s\n", status);
	    return (False);
	}
	tt_free(procid);

	/*
	 * Determine the Tooltalk fildes.
	 */
	fd = tt_fd();
	status = tt_int_error(fd);
	if (status != TT_OK) {
	    ToolTalkError("Could not connect to ToolTalk:\n%s\n", status);
	    tt_close();
	    ttfdG = -1;
	    return(False);
	}
	else {
	    ttfdG = fd;
	}

#ifdef DtActUseXtOverSelect
	/*
	 * Add the ToolTalk file descriptor to the set monitored by Xt
	 */
	XtAddInput(fd, (XtPointer)XtInputReadMask, input_handler, 0);
#endif /* DtActUseXtOverSelect */
    }

    return (True);
}

/******************************************************************************
 *
 * Send some identification back to libDtSvc so it can talk back to
 * dtexec.  The request format is:
 *
 *          op(_DtActDtexecID)      - the pattern
 *        iarg(invID)               - matches libDtSvc's invocation ID
 *        iarg(childID)             - matches libDtSvc's child ID
 *         arg(dtexec's ProcID)     - dtexec's procid handle
 *
 * libDtSvc should be able to pluck the invID and childID to immediately
 * dereference into it's Child-Invocation-Record that is tracking this
 * dtexec invocation.   It just slips in "dtexec's ProcID" and then full
 * two-way communication is established.
 *
 *****************************************************************************/
/*************************************************
 *
 * Routine to catch identification reply.
 */
Tt_callback_action IdSelfToCallerReplyCB(
    Tt_message msg,
    Tt_pattern pattern)
{
    Tt_state state;
    Tt_status status;
    char *errorMsg;


    status = tt_message_status(msg);
    state = tt_message_state(msg);

    if (state == TT_FAILED) {
	/*
	 * tjg: At some point, may want to dump the following error
	 * message into a log file.  May have to wrap long messages.
	 */
	if (status < TT_ERR_LAST)
	    errorMsg = tt_status_message(status);
	else
	    errorMsg = tt_message_status_string(msg);

	dtexec_tttk_message_destroy(msg);
	DetachFromTooltalk(NULL);
    }
    else if (state == TT_HANDLED) {
	dtexec_tttk_message_destroy(msg);
	/*
	 * Nothing substantial to do with the request-reply in the current
	 * implementation.
	 */
    }
    else {
    }

    return( (Tt_callback_action) TT_CALLBACK_PROCESSED );
}

/*************************************************
 *
 * Routine to send identification request.
 */
void IdSelfToCallerRequest(void)
{
    Tt_message msg;
    Tt_status status;
    char *procid;

    procid = tt_default_procid();

    msg = dtexec_tttk_message_create( (Tt_message) NULL, TT_REQUEST, TT_SESSION,
				dtSvcProcIdG,
				"_DtActDtexecID",
				IdSelfToCallerReplyCB );
    tt_message_iarg_add( msg, TT_IN, dtexec_Tttk_integer, dtSvcInvIdG );
    tt_message_iarg_add( msg, TT_IN, dtexec_Tttk_integer, dtSvcChildIdG );
    tt_message_arg_add( msg, TT_IN, dtexec_Tttk_message_id, procid );

    status = tt_message_send( msg );

    tt_free(procid);

    if (status != TT_OK) {
	dtexec_tttk_message_destroy( msg );
	DetachFromTooltalk(NULL);
    }
}

/******************************************************************************
 *
 * Send a Done notice back to libDtSvc.
 *
 *     _DtActDtexecDone
 *        iarg(invID)               - matches libDtSvc's invocation ID
 *        iarg(childID)             - matches libDtSvc's child ID
 *        iarg(DtActionStatus)      - a DtActionStatus style code
 *
 *****************************************************************************/
/*************************************************
 *
 * Routine to catch identification reply.
 */
Tt_callback_action DoneRequestReplyCB(
    Tt_message msg,
    Tt_pattern pattern)
{
    Tt_state state;
    Tt_status replyStatus;


    state = tt_message_state(msg);

    if (state == TT_FAILED) {
	dtexec_tttk_message_destroy(msg);
	DetachFromTooltalk(NULL);

	shutdownPhaseG = SDP_DONE_PANIC_CLEANUP;
    }
    else if (state == TT_HANDLED) {
	dtexec_tttk_message_destroy(msg);

	shutdownPhaseG = SDP_DONE_REPLIED;
    }
    else {
    }

    return( (Tt_callback_action) TT_CALLBACK_PROCESSED );
}

/*************************************************
 *
 * Routine to send done request.
 */
void DoneRequest(int doneCode)

{
    static int beenhere = 0;
    Tt_message  msg;
    Tt_status status;
    char *procid;

    /*
     * Only allow one Done(Request) to be issued.
     */
    if (!beenhere) {
	beenhere = 1;

	procid = tt_default_procid();

	msg = dtexec_tttk_message_create( (Tt_message) NULL,
				TT_REQUEST, TT_SESSION,
				dtSvcProcIdG,
				"_DtActDtexecDone",
				DoneRequestReplyCB );
	tt_message_iarg_add( msg, TT_IN, dtexec_Tttk_integer, dtSvcInvIdG );
	tt_message_iarg_add( msg, TT_IN, dtexec_Tttk_integer, dtSvcChildIdG );
	tt_message_iarg_add( msg, TT_IN, dtexec_Tttk_integer, doneCode );

	status = tt_message_send( msg );

	tt_free(procid);

	if (status != TT_OK) {
	    dtexec_tttk_message_destroy( msg );
	    DetachFromTooltalk(NULL);
	}
    }
}

static void
DtexecLog(const char *fmt, ...)
{
    if (!fmt)
        return;

    const char *logPath = getenv(DTEXEC_ACTION_ICON_LOG_ENV);
    if (!logPath || logPath[0] == '\0')
        return;

    FILE *fp = fopen(logPath, "a");
    if (!fp)
        return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(fp, "%s ", timestr);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fclose(fp);
}

typedef struct {
    char **items;
    int count;
    int capacity;
} CommandList;

typedef struct {
    pid_t *items;
    int count;
    int capacity;
} PidSet;

static void
CommandListInit(CommandList *list)
{
    if (!list)
        return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void
CommandListFree(CommandList *list)
{
    if (!list)
        return;

    for (int i = 0; i < list->count; ++i)
        free(list->items[i]);

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static Boolean
CommandListAdd(CommandList *list, const char *value)
{
    if (!list || !value || *value == '\0')
        return False;

    for (int i = 0; i < list->count; ++i)
    {
        if (strcmp(list->items[i], value) == 0)
            return False;
    }

    if (list->count >= list->capacity)
    {
        int newCap = list->capacity ? list->capacity * 2 : 8;
        char **newItems = (char **)realloc(list->items,
                                           newCap * sizeof(char *));
        if (!newItems)
            return False;
        list->items = newItems;
        list->capacity = newCap;
    }

    list->items[list->count] = strdup(value);
    if (!list->items[list->count])
        return False;

    list->count++;
    return True;
}

static char *
CommandListJoin(const CommandList *list)
{
    if (!list || list->count == 0)
        return NULL;

    size_t total = 0;
    for (int i = 0; i < list->count; ++i)
        total += strlen(list->items[i]) + 1;

    char *result = (char *)malloc(total);
    if (!result)
        return NULL;

    char *ptr = result;
    for (int i = 0; i < list->count; ++i)
    {
        size_t len = strlen(list->items[i]);
        memcpy(ptr, list->items[i], len);
        ptr += len;
        if (i + 1 < list->count)
            *ptr++ = '\n';
    }

    *ptr = '\0';
    return result;
}

static void
PidSetInit(PidSet *set)
{
    if (!set)
        return;
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static void
PidSetFree(PidSet *set)
{
    if (!set)
        return;
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static Boolean
PidSetAdd(PidSet *set, pid_t pid)
{
    if (!set || pid <= 0)
        return False;

    for (int i = 0; i < set->count; ++i)
    {
        if (set->items[i] == pid)
            return False;
    }

    if (set->count >= set->capacity)
    {
        int newCap = set->capacity ? set->capacity * 2 : 8;
        pid_t *newItems = (pid_t *)realloc(set->items,
                                           newCap * sizeof(pid_t));
        if (!newItems)
            return False;
        set->items = newItems;
        set->capacity = newCap;
    }

    set->items[set->count++] = pid;
    return True;
}

static Boolean
ReadProcCmdline(pid_t pid, char *buffer, size_t size)
{
    if (!buffer || size == 0)
        return False;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return False;

    ssize_t len = read(fd, buffer, (ssize_t)(size - 1));
    close(fd);

    if (len <= 0)
        return False;

    buffer[len] = '\0';
    return True;
}

static void
GatherChildExecutables(pid_t pid, CommandList *commands, PidSet *visited)
{
    if (!commands || !visited || pid <= 0 || !PidSetAdd(visited, pid))
        return;

    char cmdline[PATH_MAX];
    if (ReadProcCmdline(pid, cmdline, sizeof(cmdline)) && cmdline[0])
        CommandListAdd(commands, cmdline);

    char path[128];
    snprintf(path, sizeof(path), "/proc/%d/task/%d/children", pid, pid);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), fp))
    {
        char *token = strtok(buffer, " \t\n");
        while (token)
        {
            pid_t child = (pid_t)strtol(token, NULL, 10);
            if (child > 0)
                GatherChildExecutables(child, commands, visited);
            token = strtok(NULL, " \t\n");
        }
    }

    fclose(fp);
}

static char *
CollectChildCommandList(pid_t pid, int *count)
{
    CommandList commands;
    PidSet visited;

    CommandListInit(&commands);
    PidSetInit(&visited);

    GatherChildExecutables(pid, &commands, &visited);

    char *result = CommandListJoin(&commands);

    if (count)
        *count = commands.count;

    CommandListFree(&commands);
    PidSetFree(&visited);

    return result;
}

static void
DescribeChildListForLog(const char *childList, char *snippet, size_t snippetSize,
                        int *count)
{
    if (count)
        *count = 0;

    if (!snippet || snippetSize == 0)
        return;

    snippet[0] = '\0';

    if (!childList || childList[0] == '\0')
        return;

    const char *cur = childList;
    int idx = 0;

    while (*cur)
    {
        const char *end = cur;
        while (*end && *end != '\n')
            ++end;

        if (idx == 0)
        {
            size_t len = (size_t)(end - cur);
            size_t copy = len < snippetSize - 1 ? len : snippetSize - 1;
            memcpy(snippet, cur, copy);
            snippet[copy] = '\0';
        }

        ++idx;

        if (*end == '\0')
            break;

        cur = end + 1;
    }

    if (count)
        *count = idx;
}

static void
SendActionIconCacheUpdateNotice(const char *childList)
{
    if (!actionCommandG || !childList)
        return;

    char snippet[128];
    int childCount = 0;
    DescribeChildListForLog(childList, snippet, sizeof(snippet), &childCount);

    DtexecLog("Preparing notice action=%s children=%d first=%s childListLen=%zu",
              actionCommandG ? actionCommandG : "(null)",
              childCount,
              snippet[0] ? snippet : "(none)",
              strlen(childList));
    DtexecLog("Child list for action=%s:\n%s",
              actionCommandG ? actionCommandG : "(null)",
              childList);

    Tt_message msg = dtexec_tttk_message_create((Tt_message)NULL,
                                                TT_NOTICE,
                                                TT_SESSION,
                                                (const char *)NULL,
                                                ACTION_ICON_CACHE_UPDATE_OP,
                                                (Tt_message_callback)NULL);
    if (tt_ptr_error(msg) != TT_OK)
        return;

    if (tt_message_arg_add(msg, TT_IN, Tttk_string, actionCommandG) != TT_OK ||
        tt_message_arg_add(msg, TT_IN, Tttk_string, childList) != TT_OK)
    {
        DtexecLog("Notice argument add failed for action=%s", actionCommandG);
        tt_message_destroy(msg);
        return;
    }

    if (tt_message_send(msg) != TT_OK)
    {
        DtexecLog("Notice send failed for action=%s", actionCommandG);
    }
    else
    {
        DtexecLog("Notice sent for action=%s", actionCommandG);
    }
    tt_message_destroy(msg);
}

static void
MaybeSendActionIconCacheUpdate(void)
{
    if (actionIconCacheNotified || childPidG <= 0 || !actionCommandG ||
        !actionIconCacheDeadlineSet)
        return;

    struct timeval now;
    (void) gettimeofday(&now, NULL);

    if (now.tv_sec < actionIconCacheDeadlineG.tv_sec ||
        (now.tv_sec == actionIconCacheDeadlineG.tv_sec &&
         now.tv_usec < actionIconCacheDeadlineG.tv_usec))
    {
        return;
    }

    DtexecLog("Cache update triggered action=%s pid=%d",
              actionCommandG ? actionCommandG : "(null)",
              childPidG);

    int childCount = 0;
    char *childList = CollectChildCommandList(childPidG, &childCount);
    if (childList)
    {
        char snippet[128];
        DescribeChildListForLog(childList, snippet, sizeof(snippet), NULL);
        DtexecLog("Collected %d child commands for pid=%d first=%s",
                  childCount, childPidG,
                  snippet[0] ? snippet : "(none)");
    }
    else
        DtexecLog("No child commands found for pid=%d", childPidG);
    if (childList)
    {
        SendActionIconCacheUpdateNotice(childList);
        free(childList);
    }

    actionIconCacheNotified = True;
}

/******************************************************************************
 *
 * main
 *
 *****************************************************************************/
int
main (
        int argc,
        char **argv )
{
    char **cmdLine;
    int    success;
    fd_set readfds, exceptfds;
    int    nfound;
    struct timeval timeoutShort, timeoutLong;
    int    junki,i;
    char  *tmpBuffer;
    int    errorBytes;
    int    firstPass, tmpi;
    char  *tmpProgName = NULL;


    setlocale( LC_ALL, "" );

#ifdef _DTEXEC_NLS16
    Dt_nlInit();
#endif /* _DTEXEC_NLS16 */
 
    /*
     * For debugging purposes, a way to pause the process and allow
     * time for a xdb -P debugger attach. If no args, (e.g. libDtSvc is
     * test running the executable), cruise on.
     */
    if (getenv("_DTEXEC_DEBUG") && (argc > 1)) {
	/*
	 * Don't block in a system call, or on libDtSvc's attempts to
	 * just test exec us.
	 */
	SPINBLOCK
    }

    /*
     * Note: dtSvcProcIdG is used like a boolean to control whether
     * we are communicating with libDtSvc using Tooltalk.
     */
    dtSvcProcIdG = (char *) NULL;	/* assume not communicating with TT */
    ttfdG = -1;

    cmdLine = ParseCommandLine (argc, argv);

    if (cmdLine && cmdLine[0])
        actionCommandG = strdup(cmdLine[0]);

    /*
     * If a signal goes off *outside* the upcoming select, we'll need to
     * rediscover the signal by letting select() timeout.
     *
     * We might also set a rediscover flag to fake a signal response.
     */
    rediscoverSigCldG = 0;		/* boolean and counter */
    rediscoverUrgentSigG = 0;		/* boolean and counter */

    InitializeSignalHandling ();

    /*
     * Create a pipe for logging of errors for actions without
     * windows.
     */
    errorpipeG[0] = -1;                 /* by default, no stderr redirection */
    errorpipeG[1] = -1;
    if ( requestTypeG == TRANSIENT ) {    /* should be WINDOW_TYPE NO_STDIO  */
	if ( pipe(errorpipeG) == -1 ) {
	    errorpipeG[0] = -1;
	    errorpipeG[1] = -1;
	}
    }

    if (cmdLine) {
	success = ExecuteCommand (cmdLine);
	if (!success) {
	    /*
	     * Act like we were killed - it will result in a
	     * DtACTION_FAILED.
	     */
	    childPidG = -1;
	    rediscoverUrgentSigG = 1;
	}
    }
    else {
	/*
	 * Act like we had a child and it went away - it will result
	 * in a DtACTION_DONE.
	 */
	childPidG = -1;
	rediscoverSigCldG = 1;
    }

   /*
    * Note when we started so we can compare times when we finish.
    */
   (void) gettimeofday (&startTimeG, &zoneG);
   actionIconCacheDeadlineG.tv_sec = startTimeG.tv_sec + ACTION_ICON_CACHE_DELAY_SECONDS;
   actionIconCacheDeadlineG.tv_usec = startTimeG.tv_usec;
   actionIconCacheDeadlineSet = True;

    if (dtSvcProcIdG) {
	if ( !InitializeTooltalk() ) {
	    /*
	     * We have no hope of talking to our caller via Tooltalk.
	     */
	    dtSvcProcIdG = (char *) NULL;
	}
    }

    /*
     * Tie in to the default session and start chatting.
     */
    if (dtSvcProcIdG) tt_session_join(tt_default_session());

    /*
     * Finally send caller our current proc id so they can talk back.
     */
    if (dtSvcProcIdG) IdSelfToCallerRequest();

    /*
     * Monitor file descriptors for activity.  If errors occur on a fds,
     * it will be removed from allactivefdsG after handling the error.
     */
    CLEARBITS(allactivefdsG);

    /*
     * Add Tooltalk
     */
    if ( ttfdG != -1 )
	BITSET(allactivefdsG, ttfdG);          /* add Tooltalk */

    /*
     * Add Error Log
     */
    if ( errorpipeG[0] != -1 )
	BITSET(allactivefdsG, errorpipeG[0]);  /* add read side of error pipe */

    /*
     * Set options for rediscovery and not-rediscovery modes of
     * operation. 
     */
    shutdownPhaseG = SDP_DONE_STARTING;	/* triggered with rediscoverSigCldG */
    timeoutShort.tv_sec  = 0;		/* in quick rediscovery mode */
    timeoutShort.tv_usec = SHORT_SELECT_TIMEOUT;
    timeoutLong.tv_sec  = 86400;	/* don't thrash on rediscovery */
    timeoutLong.tv_usec = 0;

    for (;;) {
	COPYBITS(allactivefdsG, readfds);
	COPYBITS(allactivefdsG, exceptfds);

#if defined(__linux__)
       /* JET 9/1/98 - linux select will actually modify the timeout struct -
        *  if a select exits early then the timeout struct will contain the
        *  amount remaining.  When this gets to 0,0, an infinite loop
        *  will occur.  So... setup the timeouts each iteration. 
        */

       timeoutShort.tv_sec  = 0;               /* in quick rediscovery mode */
       timeoutShort.tv_usec = SHORT_SELECT_TIMEOUT;
       timeoutLong.tv_sec  = 86400;    /* don't thrash on rediscovery */
       timeoutLong.tv_usec = 0;
#endif


	if (rediscoverSigCldG || rediscoverUrgentSigG) {
	    nfound =select(MAXSOCKS, FD_SET_CAST(&readfds), FD_SET_CAST(NULL),
				FD_SET_CAST(&exceptfds), &timeoutShort);
 	}
	else {
	    nfound =select(MAXSOCKS, FD_SET_CAST(&readfds), FD_SET_CAST(NULL),
				FD_SET_CAST(&exceptfds), &timeoutLong);
	}

	if (nfound == -1) {
	    /*
	     * Handle select() problem.
	     */
	    if (errno == EINTR) {
		/*
		 * A signal happened - let rediscover flags redirect flow
		 * via short select timeouts.
		 */
	    }
	    else if ((errno == EBADF) || (errno == EFAULT)) {
		/*
		 * A connection probably dropped.
		 */
		if (ttfdG != -1) {
		    if ( GETBIT(exceptfds, ttfdG) ) {
			/*
			 * Tooltalk connection has gone bad.
			 *
			 * Judgement call - when the Tooltalk connection goes
			 * bad, let dtexec continue rather than doing an exit.
			 */
			DetachFromTooltalk(NULL);
		    }
		}

		if (errorpipeG[0] != -1) {
		    if ( GETBIT(exceptfds, errorpipeG[0]) ) {
			/*
			 * Error pipe has gone bad.
			 */
			close(errorpipeG[0]);
			BITCLEAR(allactivefdsG, errorpipeG[0]);
			errorpipeG[0] = -1;
		    }
		}
	    }
	    else {
		/*
		 * We have bad paremeters to select()
		 */
	    }
	    /*
	     * So that select() errors cannot dominate, now behave as
	     * though only a timeout had occurred.
	     */
	    nfound = 0;
	}

	if (nfound > 0) {
	    /*
	     * Have some input to process.  Figure out who.
	     */
	    if (ttfdG != -1) {
		if ( GETBIT(readfds, ttfdG) ) {
		    /* Clear bit first, since calling input_handler() could */
		    /* have the side-effect of setting ttfdG to -1! */
		    BITCLEAR(readfds, ttfdG);

		    /*
		     * Tooltalk activity.
		     *
		     * Note that the input_handler parameters match
		     * an XtInputHandler() style callback in case Xt is
		     * ever used.
		     */
		    input_handler((char *) NULL, (int *) &junki,
					(unsigned long *) &junki);
		}
	    }

	    if (errorpipeG[0] != -1) {
		if ( GETBIT(readfds, errorpipeG[0]) ) {
		    /*
		     * Stderr activity.
		     *
		     * Read the errorpipe until no more seems available.
		     * Call that good enough and write a time-stamped
		     * block to the errorLog file.
		     */
		    errorBytes = 0;		/* what we have so far */
		    tmpBuffer  = NULL;
		    firstPass = 1;

		    while (1) {
			char buf;
			nfound =select(MAXSOCKS, FD_SET_CAST(&readfds), FD_SET_CAST(NULL),
			     FD_SET_CAST(NULL), &timeoutShort);
			if (nfound > 0) {
			    tmpi = read (errorpipeG[0], &buf, 1);
			} else {
			    tmpi = 0;
			}

			if ( tmpi > 0 ) {
			    /*
			     * Grow buffer to hold entire error stream.
			     */
			    firstPass = 0;
			    if (tmpBuffer == NULL)
				tmpBuffer = (char *) malloc(
						tmpi + 1);
			    else
				tmpBuffer = (char *) realloc( tmpBuffer,
						errorBytes + tmpi + 1);
			    /*
			     * Drain error pipe.
			     */
			    tmpBuffer[errorBytes] = buf;
			    errorBytes += tmpi;
			    tmpBuffer[errorBytes] = '\0';

			    if (errorBytes < 65535) {
				/*
				 * Pause a bit and wait for a continuation of
				 * the error stream if there is more.
				 */
				select(0, FD_SET_CAST(NULL),
					  FD_SET_CAST(NULL),
					  FD_SET_CAST(NULL), &timeoutShort);
			    }
			    else {
				/*
				 * We have enough to do a dump now.
				 */
				break;
			    }
			}
			else {
			    /*
			     * No more to read.
			     */
			    if (firstPass) {
				/*
				 * On the first pass after select(), if we have 0 bytes,
				 * it really means the pipe has gone down.
				 */
				close(errorpipeG[0]);
				BITCLEAR(allactivefdsG, errorpipeG[0]);
				BITCLEAR(readfds, errorpipeG[0]);
				errorpipeG[0] = -1;
			    }
			    break;
			}
		    }

		    if (tmpBuffer) {

			if (!tmpProgName) {
			    tmpProgName = (char *) malloc (strlen (argv[0]) + 
							   strlen (cmdLine[0]) +
							   5);
			    if (!tmpProgName)
				tmpProgName = argv[0];
			    else {
				/*
				 * To identify the process for this stderr,
				 * use both argv[0] and the name of the
				 * process that was execvp'd
				 */
				(void) strcpy (tmpProgName, "(");
			        (void) strcat (tmpProgName, argv[0]);
				(void) strcat (tmpProgName, ") ");
				(void) strcat (tmpProgName, cmdLine[0]);
			    }
			}
			DtMsgLogMessage( tmpProgName, DtMsgLogStderr, "%s", 
					 tmpBuffer );
			free( tmpBuffer );
		    }

		    if (errorpipeG[0] != -1)
			BITCLEAR(readfds, errorpipeG[0]);
		}
	    }
	    /*
	     * So that select() data cannot dominate, now behave as
	     * though only a timeout had occurred.
	     */
	    nfound = 0;
	}

	MaybeSendActionIconCacheUpdate();

	if (nfound == 0) {
	    /*
	     * Timeout.  We are probably rediscovering and have entered
	     * a shutdown phase.   The following rediscover handlers are
	     * in priority order.
	     *
	     * Note that by way of timeouts and events, we will make
	     * multiple passes through this block of code.
	     */

	    if (rediscoverUrgentSigG) {
		/*
		 * Handle urgent signal.
		 *
		 * Tact: wait awhile and see if a SIGCLD will happen.
		 * If it does, then a normal shutdown will suffice.
		 * If a SIGCLD does not happen, then do a raw exit(0).
		 * Exit is required for BBA anyway.
		 */

		if (rediscoverSigCldG)
		    /*
		     * Rather than act on the Urgent Signal, defer to the
		     * SIGCLD Signal shutdown process.
		     */
		    rediscoverUrgentSigG = 0;
		else
		    /*
		     * Still in a mode where we have an outstanding
		     * Urgent Signal but no SIGCLD.  Bump a counter
		     * which moves us closer to doing an exit().
		     */
		    rediscoverUrgentSigG++;

		/*
		 * After 5 seconds (add select timeout too) waiting for
		 * a SIGCLD, give up and exit.
		 */
		if (rediscoverUrgentSigG > ((1000/SHORT_SELECT_TIMEOUT)*5) ) {
#if defined(__aix) || defined(CSRG_BASED) || defined(__linux__)
		    PanicSignal(0);
#else
		    PanicSignal();
#endif
		}
	    }

	    if (rediscoverSigCldG) {
		/*
		 * Handle SIGCLD signal.
		 *
		 * Under SIGCLD, we will make multiple passes through the
		 * following, implementing a phased shutdown.
		 */
		if (shutdownPhaseG == SDP_DONE_STARTING) {
		    /*
		     * Send Done(Request) for starters.
		     */
		    if (dtSvcProcIdG) DoneRequest(_DtActCHILD_DONE);

		    if (dtSvcProcIdG) {
			/*
			 * Sit and wait for the Done Reply in select()
			 */
			shutdownPhaseG = SDP_DONE_REPLY_WAIT;
		    }
		    else {
			/*
			 * Unable to send Done Reply.  Assume we're on
			 * our own from now on.
			 */
			shutdownPhaseG = SDP_DONE_PANIC_CLEANUP;
		    }
		}

		if (shutdownPhaseG == SDP_DONE_REPLY_WAIT) {
		    /*
		     * After 5 minutes of passing through REPLY_WAIT,
		     * assume the Done(Reply) will never come in and
		     * move on.
		     */
		    rediscoverSigCldG++;
		    if (rediscoverSigCldG > ((1000/SHORT_SELECT_TIMEOUT)*300)) {

			if (dtSvcProcIdG) {
			    /*
			     * Try to detatch from Tooltalk anyway.
			     */
			    DetachFromTooltalk(NULL);
			}

			shutdownPhaseG = SDP_DONE_PANIC_CLEANUP;
		    }

		    /*
		     * See if the Tooltalk connection is still alive.   If
		     * not, then no reason to wait around.
		     */
		    else if (!dtSvcProcIdG) {
			shutdownPhaseG = SDP_DONE_PANIC_CLEANUP;
		    }

		}

		if (shutdownPhaseG == SDP_DONE_REPLIED) {
		    /*
		     * We have our Done(Reply), so proceed.
		     */

		    if (dtSvcProcIdG)
			shutdownPhaseG = SDP_FINAL_LINGER;
		    else
			shutdownPhaseG = SDP_DONE_PANIC_CLEANUP;
		}

		if (shutdownPhaseG == SDP_DONE_PANIC_CLEANUP) {
		    /*
		     * We cannot talk with caller, so do cleanup
		     * of tmp files.
		     */
		    for (i = 0; i < tmpFileCntG; i++ ) {
			chmod( tmpFilesG[i], (S_IRUSR|S_IWUSR) );
			unlink( tmpFilesG[i] );
		    }

		    shutdownPhaseG = SDP_FINAL_LINGER;
		}

		if (shutdownPhaseG == SDP_FINAL_LINGER) {
		    /*
		     * All done.
		     */
		    static int skipFirst = 1;

		    if (skipFirst) {
			/*
			 * Rather than a quick departure from the select()
			 * loop, make one more pass.  If the child has gone
			 * down quickly, the SIGCLD may have caused us to
			 * get here before any errorPipeG information has
			 * had a chance to reach us.
			 */
			skipFirst = 0;
		    }
		    else {
			FinalLinger();
		    }
		}
	    }
	}
    }
}
