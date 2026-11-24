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
/* $TOG: SmSave.c /main/27 1998/07/23 18:07:58 mgreess $ */
/*************************************<+>*************************************
 *****************************************************************************
 **
 **  File:        SmSave.c
 **
 **  Project:     HP DT Session Manager (dtsession)
 **
 **  Description:
 **  -----------
 **  This file contains all routines that save off the session at any point
 **  in time.  The session includes settings, resources, and clients.  How
 **  the session manager behaves during a save depends on how the user has
 **  configured it to behave.
 **
 **
 *****************************************************************************
 *************************************<+>*************************************/
/*
 * (c) Copyright 1997 The Open Group
 * (c) Copyright 1996 Digital Equipment Corporation.
 * (c) Copyright 1990, 1993, 1994, 1996 Hewlett-Packard Company.
 * (c) Copyright 1993, 1994, 1996 International Business Machines Corp.
 * (c) Copyright 1993, 1994, 1996 Sun Microsystems, Inc.
 * (c) Copyright 1993, 1994, 1996 Novell, Inc. 
 * (c) Copyright 1996 FUJITSU LIMITED.
 * (c) Copyright 1996 Hitachi.
 */


#include <stdio.h>
#include <string.h>
#include <stdckdint.h>
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#ifdef _SUN_OS
#include <arpa/inet.h>
#include <string.h>
#endif /* _SUN_OS */

#include <X11/Intrinsic.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/StringDefs.h>
#include <X11/keysymdef.h>
#include <Dt/DtP.h>
#include <Dt/Connect.h>
#include <Dt/UserMsg.h>
#include <Dt/SessionM.h>
#include <Dt/MsgLog.h>
#include "Sm.h"
#include "SmSave.h"
#include "SmResource.h"
#include "SmError.h"
#include "SmGlobals.h"
#include "SmUI.h"
#include "SmWindow.h"
#include "SmProtocol.h"
#include "SmRestore.h"
#include "SmXSMP.h"
#include "SmDB.h"
#include "SmProperty.h"
#include "SmProp.h"
#include "SmFiles.h"
#include "SmWayland.h"
#include "Srv.h"
#include "SrvFile_io.h"

#ifdef   MULTIBYTE
#include <stdlib.h>
#include <limits.h>
#endif

#include <X11/Xlibint.h>

typedef struct _ProxyClientCache {
	Window		window;
	int		screen;
} ProxyClientCache, *ProxyClientCachePtr;

static ProxyClientCachePtr proxyList;
static int numProxyClients = 0;
static int numProxyAllocated = 0;
static int proxyCacheAllocIncrement = 10;

/*
 * Local Function Declarations
 */
static void CacheProxyClient (
	Boolean			firstClient, 
	int			screenNum, 
	Window			window);

static void PruneSessionDirectory (void);

static Boolean WriteClientDatabase (void);

static void OutputXSMPClients (
	ClientDB		outputDB);

static void OutputProxyClients (
	ClientDB		outputDB);

static Boolean CheckRequiredProperties (
	ClientRecPtr		pClientRec);

static void PrintStartupInfo( 
			ClientDB outputDB,
                        int screen,
                        Window window);
static int QueryServerSettings( void ) ;
static int SaveCustomizeSettings( void ) ;
static int OutputResource( void ) ;
static void PrintPointerControl( 
                        XrmDatabase *smBase) ;
static void PrintSessionInfo( 
                        XrmDatabase *smBase) ;
static void PrintScreenSaver( 
                        XrmDatabase *smBase) ;
static int PrintFontPaths( 
                        XrmDatabase *smBase,
                        char **fontPath,
                        int numPaths) ;
static int PrintKeyboardControl( 
                        XrmDatabase *smBase);
static int PrintPointerMapping( 
                        XrmDatabase *smBase,
                        char *buttonRet,
                        int numButton) ;
static void PrintWorkHintString( 
                        FILE *hintFile,
                        char *hintString) ;
static void PrintCmdHintString( 
                        FILE *hintFile,
                        char *hintString) ;
static void PrintCmdString( 
                        FILE *cmdFile,
                        char *cmdString) ;
static void PrintRemoteCmdString( 
                        FILE *cmdFile,
                        char *cmdString) ;


/*
 * Global variables that are exposed
 */
SmScreenInfo		screenSaverVals;
SmAudioInfo		audioVals;
SmKeyboardInfo		keyboardVals;

/*
 * Variables global to this module only
 */

/*
 * used as a buffer to write resources into
 * before they are written out
 */
static size_t	resSize = 10000;
static char	*resSpec;

/*
 * Appends text to the resource line in resSpec and enlarges the buffer when
 * the line does not fit.  Reports the error and returns False if it cannot.
 */
static Boolean
AppendResSpec(const char *text)
{
    size_t	used = strlen(resSpec);
    size_t	add = strlen(text);
    size_t	needed, newSize;
    char	*grown;

    if (ckd_add(&needed, used, add) || ckd_add(&needed, needed, (size_t) 1))
    {
	PrintError(DtError, smNLS.cantMallocErrorString);
	return(False);
    }
    if (needed > resSize)
    {
	if (ckd_add(&newSize, needed, (size_t) 5000))
	{
	    PrintError(DtError, smNLS.cantMallocErrorString);
	    return(False);
	}
	grown = (char *) SM_REALLOC(resSpec, newSize * sizeof(char));
	if (grown == nullptr)
	{
	    PrintErrnoError(DtError, smNLS.cantMallocErrorString);
	    return(False);
	}
	resSpec = grown;
	resSize = newSize;
    }
    memcpy(resSpec + used, text, add + 1);
    return(True);
}

static Boolean
AppendResSpecNumber(long value)
{
    char	digits[32];

    snprintf(digits, sizeof digits, "%ld", value);
    return(AppendResSpec(digits));
}

/*
 * Starts the resource line "<class>*<name>:" in resSpec.
 */
static Boolean
StartResSpec(const char *name)
{
    *resSpec = 0;
    return(AppendResSpec(SM_RESOURCE_CLASS) && AppendResSpec("*") &&
	   AppendResSpec(name) && AppendResSpec(":"));
}

/*
 * Puts the resource "<class>*<name>:<value>" into the database.
 */
static void
PutNumberResource(XrmDatabase *smBase, const char *name, long value)
{
    if (StartResSpec(name) && AppendResSpecNumber(value))
    {
	XrmPutLineResource(smBase, resSpec);
    }
}

/*
 * Vars used to maintain state that is sent to the SaveState
 * function.
 */
static Boolean	saveHome;
int		saveMode;




/*************************************<->*************************************
 *
 *  SetupSaveState
 *
 *  Description:
 *  -----------
 *  Do all of the preliminary work that must be done before the state
 *  of a checkpoint can be saved.
 *
 *  Inputs:
 *  ------
 *  saveToHome:  This session is to be saved to the home session or current
 *  mode = whether the session is to be reset or restarted
 *
 *************************************<->***********************************/
void
SetupSaveState(
        Boolean 	saveToHome,
        int 		mode)
{
  unsigned char		*propData;

  /*
   * Cache saveToHome and mode - they will be needed after all of
   * the XSMP clients are saved and the Proxy clients are saved.
   */
  saveHome = saveToHome;
  saveMode = mode;

  /*
   * First set up the save paths so we know if we have to do anything here
   */
  if ((SetSavePath(saveToHome, mode)) == -1)
      return;

  /*
   * Take care of the save mode property on the root window so that the
   * clients will know how to save this file (if they save files)
   */
  if(saveToHome == False)
  {
      propData = (unsigned char *) SM_CURRENT_DIRECTORY;
  }
  else
  {
      propData = (unsigned char *) SM_HOME_DIRECTORY;
  }

  /*
   * Change the save mode to give the subdirectory saved to
   *
   */
  XChangeProperty(smGD.display, RootWindow(smGD.display, 0),
		  XaSmSaveMode, XA_STRING, 8, PropModeReplace,
		   propData, strlen((char *)propData));
  XFlush(smGD.display);
}

/*************************************<->*************************************
 *
 *  SaveState
 *
 *  Description:
 *  -----------
 *  Tells the SM to notify the XSMP clients to save their state.
 *  The function CompleteSave fill be invoked after the XSMP clients
 *  have completed their save.
 *
 *  Inputs:
 *  ------
 *  saveToHome:  This session is to be saved to the home session or current
 *  mode = whether the session is to be reset or restarted
 *
 *  Comments:
 *  --------
 *  As a result of this routine files will be saved in the users .dt directory
 *  that will allow the session manager to restore the session at a later
 *  time.
 *
 *  NOTE - this function is called as the result of the following
 *	events:
 *
 *	1. User initiates a Save Home Session via the Style Manager
 *	2. The Session Manager receives a SM_XSESSION_EXIT message
 *	3. The Session Manager receives a SM_SAVE_SESSION message
 * 
 *************************************<->***********************************/
void
SaveState(
        Boolean 		saveToHome,
        int 			mode,
	int                     saveType,
	Bool                    shutdown,
	int                     interactStyle,
	Bool                    fast,
	Bool                    global)
{
  /*
   * Notify XSMP clients of the save
   */
  SaveYourselfReqProc (nullptr, nullptr, saveType, shutdown, interactStyle, 
			fast, global);
}


/*************************************<->*************************************
 *
 *  CompleteSave 
 *
 *  Description:
 *  -----------
 *  Called after the XSMP clients have saved their state.  In charge of 
 *  calling all routines that save session information for XSMP client
 *  and Proxy clients as well as settings, resources, etc.
 *
 *  Outputs:
 *  -------
 *
 *************************************<->***********************************/
int 
CompleteSave (void)
{

  FILE 			*convertFP;
  unsigned char		*propData;
  char			*tmpPath, tmpVersion[20];

  Boolean 		saveToHome = saveHome;
  int			mode = saveMode;

  /*
   * If this is a Home session and the user is logging out, return
   * now - the XSMP clients have all responded to the save and the
   * ICCC clients don't save their state when a Home session is
   * exited.
   */
  if (smGD.sessionType == HOME_SESSION && 
      smXSMP.saveState.shutdown == True && 
      smSettings.startState == DtSM_HOME_STATE)
      return (0);

  /*
   * Start a wait state - don't want anything to happen while
   * ICCC clients are being saved
   */
  EnterWaitState();
  
  resSpec = (char *) SM_MALLOC(resSize * sizeof(char));
  if (resSpec==nullptr)
  {
      PrintErrnoError(DtError, smNLS.cantMallocErrorString);
      return(-1);
  }

  if (!WriteClientDatabase ()) {
      LeaveWaitState();
      SM_FREE(resSpec);
      return (-1);
  }

  if (SmWaylandActive()) {
      char wlSaveDir[MAXPATHLEN + 1];
      if ((size_t)snprintf(wlSaveDir, sizeof(wlSaveDir), "%s/%s",
                           smGD.savePath, smGD.restoreSession) < sizeof(wlSaveDir)) {
          SmWaylandSaveClients(wlSaveDir);
      }
  }

  /*
   * WARNING - THIS VARIABLE MUST BE SET BEFORE THE SETTING INFORMATION IS
   * CALLED - SETTINGS MUST BE SAVED BEFORE RESOURCES BECAUSE SETTINGS
   * GETS INFORMATION SAVED IN RESOURCES (smToSet.dClickBuf)
   */
  smToSet.dClickBuf[0] = 0;

  /*
   * Do the actual output of the X settings information.  Output will
   * go to the resource database at this point and be written to the
   * resource file with the rest of the resources (below).
   */
  if(smRes.querySettings == True)
  {
      QueryServerSettings();
  }
  else
  {
      SaveCustomizeSettings();
  }

  /*
   * Do the output of resource information.
   */
  if(OutputResource())
  {
      LeaveWaitState();
      SM_FREE(resSpec);
      return(-1);
  }

  XDeleteProperty(smGD.display, DefaultRootWindow(smGD.display), XaSmSaveMode);
 
  /* 
   * Go save the default palette at this time
   */
  if(saveToHome == True)
     SaveDefaultPalette(smGD.display, smGD.savePath, DtSM_HOME_STATE);
  else
     SaveDefaultPalette(smGD.display, smGD.savePath, mode);
 
  PruneSessionDirectory ();

  LeaveWaitState();

  return(0);
}


/*************************************<->*************************************
 *
 *  PruneSessionDirectory -
 *
 *  Description: Removes the oldest session direcotry if the number
 *	of directories exceeds the smRes.numSessionsBackedup.
 *
 *  Inputs: void
 * 
 *************************************<->***********************************/
static void
PruneSessionDirectory (void)
{
	DIR			* dirp;
	struct dirent 		* dp;
	int			numDirs = 0;
	struct			stat buf;
	time_t			oldestTime;
	char			*clientDB;
	char			*dirPrefix;
	char			*oldDir;	/* the .old dir */
	char			*oldestDir;
	char			*saveDir;
	char			*tmpDir;

	if ((dirp = opendir (smGD.savePath)) == nullptr)
		return;

	clientDB = (char*) XtMalloc((2*MAXPATHLEN) + 1);
	oldestDir = (char*) XtMalloc(MAXPATHLEN + 1);
	saveDir = (char*) XtMalloc(MAXPATHLEN + 1);
	tmpDir = (char*) XtMalloc(MAXPATHLEN + 1);
	
	dirPrefix = XtMalloc (strlen (smGD.restoreSession) + 2);
	snprintf (dirPrefix, strlen (smGD.restoreSession) + 2, "%s.",
		  smGD.restoreSession);
	if (strlen (dirPrefix) > 5) {
		dirPrefix[4] = '.';
		dirPrefix[5] = '\000';
	}

	oldDir = XtMalloc (strlen (smGD.restoreSession) + 
			   strlen (SM_OLD_EXTENSION) + 2);
	snprintf (oldDir, strlen (smGD.restoreSession) +
		  strlen (SM_OLD_EXTENSION) + 2, "%s.%s",
		  smGD.restoreSession, SM_OLD_EXTENSION);

	(void) time (&oldestTime);

	while ((dp = readdir (dirp)) != nullptr) {
		/*
		 * Do some sanity checks to limit the number of * stat() 
		 * calls.  Note that for backward compatibility with CDE 
		 * 1.0 if a ".old" directory exists, keep it.
		 */
		if ((!strcmp (dp->d_name, ".")) || 
		    (!strcmp (dp->d_name, "..")) ||
		    (!strcmp (dp->d_name, oldDir)) ||
		    (strncmp  (dp->d_name, dirPrefix, strlen (dirPrefix))))
			continue;

		if ((size_t) snprintf (tmpDir, MAXPATHLEN + 1, "%s/%s",
				       smGD.savePath, dp->d_name) >=
		    MAXPATHLEN + 1)
			continue;

                if (((stat (tmpDir, &buf)) == 0) && S_ISDIR (buf.st_mode)) {
			if (buf.st_mtime < oldestTime) {
				/*
				 * Keep track of the oldest dir in case
				 * it needs to be removed
				 */
				oldestTime = buf.st_mtime;
				memcpy (oldestDir, tmpDir, strlen (tmpDir) + 1);
			}
			numDirs++;
		}
        }
	closedir(dirp);

	if (numDirs > smRes.numSessionsBackedup) {
		/*
		 * Need to remove the oldest directory.  The general
		 * process (using "home" as an example) is:
		 *
		 *   mv home home.tmp
		 *   mv oldestDir home
		 *   Execute the DiscardCommand props in the home directory
		 *   rm -rf home
		 *   mv home.tmp home
		 *
		 * The reason this is done is because when the 
		 * DiscardCommand were saved, the directory at the time of
		 * the save contained "home".  For example, a DiscardCommand
		 * may look like:
		 *
		 *    rm -f /u/somebody/.dt/sessions/home/app.state
		 *
		 * yet because of the session archiving, the app.state
		 * file may actually be in a directory like:
		 *
		 *    /u/somebody/.dt/sessions/home.<tempnam>
		 */ 

		char		* tmpName;
		Boolean		haveSession;

		tmpName = SmMakeTempDirectory (smGD.savePath,
					       smGD.restoreSession);
		if (tmpName == nullptr) {
			PrintErrnoError(DtError, smNLS.cantCreateDirsString);
		} else if ((size_t) snprintf (saveDir, MAXPATHLEN + 1, "%s/%s",
					      smGD.savePath,
					      smGD.restoreSession) >=
			   MAXPATHLEN + 1 ||
			   (size_t) snprintf (clientDB, 2 * MAXPATHLEN + 1,
					      "%s/%s/%s", smGD.savePath,
					      smGD.restoreSession,
					      SM_CLIENT_FILE2) >=
			   2 * MAXPATHLEN + 1) {
			/* the session path does not fit: prune nothing */
			PrintErrnoError(DtError, smNLS.cantCreateDirsString);
			rmdir (tmpName);
			free (tmpName);
		} else {
			haveSession = (stat (saveDir, &buf) == 0);

			if (haveSession &&
			    MoveDirectory (saveDir, tmpName, False) != 0) {
				/* the session stays where it is */
				rmdir (tmpName);
			} else if (MoveDirectory (oldestDir, saveDir, False) != 0) {
				/* nothing was pruned: put the session back */
				if (haveSession)
					MoveDirectory (tmpName, saveDir, False);
				else
					rmdir (tmpName);
			} else {
				ExecuteDiscardCommands (clientDB);

				if (haveSession) {
					MoveDirectory (tmpName, saveDir, True);
				} else {
					if (SmRemoveTree (saveDir) != 0)
						PrintErrnoError(DtError, GETMESSAGE(4, 4, "Unable to remove session directory.  Make sure write permissions exist on $HOME/.dt directory.  Invalid session files will not be removed."));
					rmdir (tmpName);
				}
			}
			free (tmpName);
		}
	}

	/*
	 * If exiting and the current session is a Current session
	 * but smSettings.startState has been set to to a 
	 * Home session (this would be true if a user started a 
	 * Current session but used the Style Manager to set the session
	 * upon next login to be a Home session), then if a home
	 * directory exists, mv current to current.old
	 */

	if (smXSMP.saveState.shutdown && smGD.sessionType == CURRENT_SESSION &&
	    smSettings.startState == DtSM_HOME_STATE) {

		if ((size_t) snprintf (tmpDir, MAXPATHLEN + 1, "%s/%s",
				       smGD.savePath, SM_HOME_DIRECTORY) <
		    MAXPATHLEN + 1 &&
		    ((stat (tmpDir, &buf)) == 0) && S_ISDIR (buf.st_mode) &&
		    (size_t) snprintf (saveDir, MAXPATHLEN + 1, "%s/%s",
				       smGD.savePath, oldDir) <
		    MAXPATHLEN + 1) {

                	if (((stat (saveDir, &buf)) == 0) && 
					S_ISDIR (buf.st_mode) &&
			    (size_t) snprintf (oldestDir, MAXPATHLEN + 1,
					       "%s/%s", smGD.savePath,
					       SM_CURRENT_DIRECTORY) <
			    MAXPATHLEN + 1) {

				if (SmRemoveTree (saveDir) != 0)
					PrintErrnoError(DtError, GETMESSAGE(4, 4, "Unable to remove session directory.  Make sure write permissions exist on $HOME/.dt directory.  Invalid session files will not be removed."));

				MoveDirectory (oldestDir, saveDir, False);
			}
		}
	}

	XtFree(clientDB);
	XtFree(dirPrefix);
	XtFree(oldDir);
	XtFree(oldestDir);
	XtFree(saveDir);
	XtFree(tmpDir);
}


/*************************************<->*************************************
 *
 *  WriteClientDatabase -
 *
 *  Description: Writes the client database (for XSMP and Proxy clients)
 *
 *  Inputs: None
 * 
 *  Returns True if the database is wrtten OK; False otherwise
 *
 *************************************<->***********************************/
static Boolean
WriteClientDatabase (void)
{
	char			*db;
	ClientDB		outputDB;

	db = (char*) XtMalloc(MAXPATHLEN + 1);
	if ((size_t) snprintf (db, MAXPATHLEN + 1, "%s/%s/%s", smGD.savePath,
			       smGD.restoreSession, SM_CLIENT_FILE2) >=
	    MAXPATHLEN + 1) {
		PrintErrnoError(DtError, smNLS.cantCreateDirsString);
		XtFree(db);
		return (False);
	}

	if ((outputDB = OpenOutputClientDB (db, smXSMP.dbVersion,
					    smXSMP.dbSessionId)) == nullptr) {
		char		* str;

		str = strdup ((char *) GETMESSAGE (40, 18,
			"The following database file cannot be opened for writing:\n\n   %s\n\nThe session will not be saved."));

		if (str) {
			DtMsgLogMessage (smGD.programName, DtMsgLogError,
					 str, db);
			free (str);
		}
			
		XtFree(db);
		return (False);
	}
	XtFree(db);

	OutputProxyClients (outputDB);
	
	OutputXSMPClients (outputDB);

	return CloseClientDB (outputDB, True);
}


/*************************************<->*************************************
 *
 *  OutputProxyClients -
 *
 *  Description: Outputs the Proxy clients
 *
 *  Inputs: database pointer
 * 
 *  Returns: void
 *
 *************************************<->***********************************/
static void
OutputProxyClients (
	ClientDB		outputDB)
{
	int		i;
	for (i = 0; i < numProxyClients; i++)
		PrintStartupInfo (outputDB, 
				  proxyList[i].screen, 
				  proxyList[i].window);
}


/*************************************<->*************************************
 *
 *  OutputXSMPClients -
 *
 *  Description: Outputs the XSMP clients
 *
 *  Inputs: database pointer
 * 
 *  Returns: void
 *
 *************************************<->***********************************/
static void
OutputXSMPClients (
	ClientDB		outputDB)
{
	XSMPClientDBRec		dbRec;
	ClientRecPtr            pClientRec;

	for (pClientRec = connectedList; pClientRec != nullptr;
		pClientRec = pClientRec->next) {

		dbRec.restartHint = pClientRec->restartHint;
		(void) GetCardPropertyValue (pClientRec, SmRestartStyleHint, 
					     &dbRec.restartHint);

		if (dbRec.restartHint == SmRestartNever)
			continue;

		if (!pClientRec->active)
			continue;

		if (!CheckRequiredProperties (pClientRec))
			continue;

		dbRec.screenNum = 0;

		dbRec.clientId = pClientRec->clientId;
		dbRec.clientHost = pClientRec->clientHost;

		dbRec.program = GetArrayPropertyValue (pClientRec, 
					SmProgram);
		dbRec.cwd = GetArrayPropertyValue (pClientRec, 
					SmCurrentDirectory);

		dbRec.restartCommand = GetListOfArrayPropertyValue (pClientRec,
					SmRestartCommand);
		dbRec.cloneCommand = GetListOfArrayPropertyValue (pClientRec,
					SmCloneCommand);
		dbRec.discardCommand = GetListOfArrayPropertyValue (pClientRec,
					SmDiscardCommand);
		dbRec.environment = GetListOfArrayPropertyValue (pClientRec,
					SmEnvironment);

		if (!PutXSMPClientDBRec (outputDB, &dbRec)) {
			
			char		*str;

			str = strdup ((char *) GETMESSAGE (40, 19,
				"Application '%s' cannot be saved."));

			if (str) {
				DtMsgLogMessage (smGD.programName, 
						 DtMsgLogError, str, 
					 	 dbRec.program);
				free (str);
			}
		}

		if (dbRec.restartCommand)
			XtFree ((char *) dbRec.restartCommand);
		if (dbRec.cloneCommand)
			XtFree ((char *) dbRec.cloneCommand);
		if (dbRec.discardCommand)
			XtFree ((char *) dbRec.discardCommand);
		if (dbRec.environment)
			XtFree ((char *) dbRec.environment);
	}
}


/*************************************<->*************************************
 *
 *  CheckRequiredProperties -
 *
 *  Description: Check a client's required properties
 *
 *  Inputs: ClientRecPtr
 * 
 *  Returns: True if the client has all of the required properties;
 *	otherwise returns False
 *
 *************************************<->***********************************/
static Boolean
CheckRequiredProperties (
	ClientRecPtr		pClientRec)
{
	PropertyRecPtr		pPropRec;
	char			* str;
	char			* program = nullptr;

	if ((program = GetArrayPropertyValue (pClientRec, SmProgram)) == nullptr){

		str = strdup ((char *) GETMESSAGE (40, 23,
			"The following application did not set the required\nproperty '%s':\n\n   %s\n\nThis application will not be saved."));

		if (str) {
			DtMsgLogMessage (smGD.programName, DtMsgLogError, str, 
					 SmProgram, pClientRec->clientId);
			free (str);
		}
		return (False);
	}

	if ((pPropRec = GetPropertyRec (pClientRec, SmRestartCommand)) == nullptr){

		str = strdup ((char *) GETMESSAGE (40, 24,
			"24 Application '%s' will not be saved because it did not set\nthe required property '%s'."));

		if (str) {
			DtMsgLogMessage (smGD.programName, DtMsgLogError,
					 str, program, SmRestartCommand);
			free (str);
		}
		return (False);
	}

	if ((pPropRec = GetPropertyRec (pClientRec, SmCloneCommand)) == nullptr) {
		/*
		 * This is a warning - still want to save the client
		 */
		str = strdup ((char *) GETMESSAGE (40, 25,
			"Application '%s' did not set the required property '%s'."));

		if (str) {
			DtMsgLogMessage (smGD.programName, DtMsgLogWarning, str,
					 program, SmCloneCommand);
			free (str);
		}
	}

	return (True);
}


/*************************************<->*************************************
 *
 *  NotifyProxyClients -
 *
 *
 *  Description:
 *  -----------
 *  Notify Proxy clients - calls routines to find top level windows, and 
 *  notifies the Proxy clients about an impending save.
 *
 *  Inputs:
 *  ------
 *
 *  Outputs:
 *  -------
 *
 *************************************<->***********************************/
void 
NotifyProxyClients (void)
{
  unsigned int 		topListLength, containedListLength;
  WindowInfo 		* topList;	/* list of top-level windows */
  WindowInfo 		topLevelWindowInfo;
  Atom 			actualType;
  int 			actualFormat;
  unsigned long 	nitems;
  unsigned long 	leftover;
  unsigned char 	* data = nullptr;
  Boolean		containedClient;
  int 			i, j;
  int			numClients = 0;

  /*
   * Save off the clients currently running.  This will be done
   * one screen at a time
   */
  for(j = 0;j < smGD.numSavedScreens;j++)
  {
      /*
       * Get a list of children of the root window. If there are none
       * return with an error condition.
       */
      if(GetTopLevelWindows(j,&topList,&topListLength, &containedListLength))
	  return;
  
      /*
       * Figure out how to restart the client associate with each window.
       */
      for (i=0 ; i<(topListLength + containedListLength); ++i)
      {
	  topLevelWindowInfo=topList[i];

	  if(i >= topListLength)
	  {
	      containedClient = True;
	  }
	  else
	  {
	      containedClient = False;
	  }
      
	  /*
	   * If this window is my window - skip it I don't want to
	   * save myself because I will be started automatically
	   */
	  if (XGetWindowProperty(smGD.display, topLevelWindowInfo.wid,
				 XA_WM_CLASS,0L,
				 (long)BUFSIZ,False,XA_STRING,&actualType,
				 &actualFormat,&nitems,&leftover,
				 &data)==Success)
	  {
	      if (actualType==XA_STRING &&
		  !strcmp((char *) data, SM_RESOURCE_NAME))
	      {
		  SM_FREE((char *)data);
		  continue;
	      }
	      else
	      {
		  SM_FREE((char *)data);
	      }
	  }
      
	 /*
	  * If the client participates in the WM_SAVE_YOURSELF protocol, 
	  * send the appropriate messages and wait for the proper 
	  * responses.
	  */
	 (void) SaveYourself(topLevelWindowInfo);

	 CacheProxyClient ((numClients == 0), j, topLevelWindowInfo.wid);

	 numClients++;
      }

      /*
       * Free space malloc'd by GetTopLevelWindowInfo
       */
      if(topListLength > 0)
      {
	  SM_FREE((char *) topList);
      }
  }
}


/*************************************<->*************************************
 *
 *  CacheProxyClient -
 *
 *  Description:
 *  -----------
 *  Add a Proxy client to the proxy client cache.
 *
 *  Inputs:
 *  ------
 *  firstClient - if True, this is the first client in the cache
 *  screenNum - the client's screen number
 *  window - the client's window number
 *
 *  Outputs:
 *  -------
 *
 *************************************<->***********************************/
static void CacheProxyClient (
	Boolean			firstClient, 
	int			screenNum, 
	Window			window)
{
	if (firstClient)
		numProxyClients = 0;

	if (numProxyClients >= numProxyAllocated) {
		numProxyAllocated += proxyCacheAllocIncrement;
		proxyList = (ProxyClientCachePtr) XtRealloc ((char *) proxyList,
			numProxyAllocated * sizeof (ProxyClientCache));
	}

	proxyList[numProxyClients].screen = screenNum;
	proxyList[numProxyClients].window = window;

	numProxyClients++;
}



/*************************************<->*************************************
 *
 *  PrintStartupInfo -
 *
 *  Description:
 *  -----------
 *  Output startup information for one proxy client 
 *
 *  Inputs:
 *  ------
 *  outputDB = a pointer to the client database
 *  screen = which screen the client is running on
 *  window = the top level window of the lient
 * 
 *  Outputs:
 *  -------
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
static void
PrintStartupInfo(
	ClientDB outputDB,
        int screen,
        Window window)
{
  Boolean		writeStatus;
  int 			wargc;
  char 			** wargv;
  char 			*clientMachine;
  XWindowAttributes 	windowAttr;
  Boolean		remoteHost;
  Boolean		xsmpClient;
  ProxyClientDBRec 	dbRec;
  char			** argv;
  int			i;
  Status		status;

#ifdef _SUN_OS /* pull out quotes from OpenWindow tools commands */
  char 			* ptr;
#endif

  /*
   * Get the attributes and properties on the window
   */
  if ((XGetWindowAttributes(smGD.display, window, &windowAttr)) == 0)
    return;

  if (GetStandardProperties(window, screen, &wargc, &wargv, 
			    &clientMachine, &xsmpClient)==0)
    return;
  
  /*
   * If the client doesn't have a WM_COMMAND property, we can't restart it
   */
  if (!wargv||!wargv[0]) 
  {
    if (wargv) XFreeStringList(wargv);
    if (clientMachine) XFree(clientMachine);
    return;
  }

  /*
   * If the client is an XSMP client, don't save it as a Proxy client.
   */
  if (xsmpClient)
  {
     XFreeStringList(wargv);
     if (clientMachine) XFree(clientMachine);
     return;
  }

#ifdef _SUN_OS /* pull out quotes from OpenWindow tools commands */
  if (wargc == 1) {
     ptr = wargv[0];
     /* check for foreground */
     if ((ptr = strstr (ptr, "fg \"")) != nullptr) {
        if (strlen (ptr) > 3)
           ptr[3] = ' ';
        if ((ptr = strchr (ptr, '"')) != nullptr)
           ptr[0] = ' ';
     }

     /* check for background */
     if ((ptr = strstr (wargv[0], "bg \"")) != nullptr) {
        if (strlen (ptr) > 3)
           ptr[3] = ' ';
        if ((ptr = strchr (ptr, '"')) != nullptr)
           ptr[0] = ' ';
     }
  }
#endif
  
  /*
   * Find out if the client is running locally
   */
  dbRec.clientHost = nullptr;
  if(clientMachine != nullptr)
  {
      if(DtIsLocalHostP(clientMachine) == FALSE)
      {
	  dbRec.clientHost = clientMachine;
      }
  }

  /*
   * Must NULL-terminate the WM_COMMAND before writing it
   */
  if (wargc < 0 || (size_t)wargc >= UINT_MAX / sizeof(char *))
  {
      XFreeStringList(wargv);
      if (clientMachine) XFree(clientMachine);
      return;
  }
  argv = (char **) XtMalloc (((size_t)wargc + 1) * sizeof (char *));
  if (!argv)
  {
      XFreeStringList(wargv);
      if (clientMachine) XFree(clientMachine);
      return;
  }

  for (i = 0; i < wargc; i++)
      argv[i] = wargv[i];
  argv[wargc] = nullptr;

  dbRec.command = argv;
  dbRec.screenNum = screen;

  writeStatus = PutProxyClientDBRec (outputDB, &dbRec);

  XtFree ((char *) argv);
  if (clientMachine)
      XFree(clientMachine);

  if (writeStatus == False)
  {
    /*
     * There should be an error written here - however
     * since this means that there already is a write error to
     * the disk - no error can be written
     */
    (void) CloseClientDB (outputDB, False);

    /*
     * On a write error - move the old session back to
     * be used and exit with an error condition.  The directory
     * that was written and the one it was archived to are the
     * ones SetSavePath() chose, whatever kind of session this is.
     */
    XFreeStringList(wargv);

    UndoSetSavePath();
    SM_EXIT(-1);
  }

  /*
   * Free up the argv property
   */
  XFreeStringList(wargv);
}


/*************************************<->*************************************
 *
 *  QueryServerSettings ()
 *
 *
 *  Description:
 *  -----------
 *  Query the server for all it's current settings information, and then
 *  write that information to the session managers settings resource file.
 *
 *  Inputs:
 *  ------
 *  smGD.settingPath = (global) pointer to file where
 *			setting info should be saved.
 * 
 *  Outputs:
 *  -------
 *
 *  Comments:
 *  --------
 *  This routine slows down the session manager's shutdown process
 *  considerably, and is therefore only called when the user specifies
 *  that all settings should be queried.  Otherwise, only settings set in
 *  the customizer should be saved.
 * 
 *************************************<->***********************************/
static int 
QueryServerSettings( void )
{
    int			i, numLoops;
    char		**fontPath;
    KeySym		*tmpSyms, tmpMod;
    XModifierKeymap	*modMap;
    char		*buttonRet;
    int			numButton, numPaths;
    int			buttonSize = 128;
    XrmDatabase 	smBase = nullptr;
    Boolean		ok;


    buttonRet = (char *) SM_MALLOC(buttonSize * sizeof(char));
    if (buttonRet==nullptr)
    {
	PrintErrnoError(DtError, smNLS.cantMallocErrorString);
	return(-1);
    }

    /*
     * First set up all pointer control parameters
     * These are simple integer values that get saved
     * here, then read into a structure and set at restore
     * time
     */
    XGetPointerControl(smGD.display, &smSettings.accelNum,
		       &smSettings.accelDenom,
		       &smSettings.threshold);
    PrintPointerControl(&smBase);

    /*
     * Get all the screen saver values.  These are all simple integer
     * values that are read back into a structure and set at restore
     * time.
     */
    XGetScreenSaver(smGD.display, &screenSaverVals.smTimeout,
		    &screenSaverVals.smInterval,
		    &screenSaverVals.smPreferBlank,
		    &screenSaverVals.smAllowExp);
    PrintScreenSaver(&smBase);

    /*
     *  Write out all font path information. 
     *  Write out all the paths one by one concatenated by commas.
     */
    fontPath = XGetFontPath(smGD.display, &numPaths);
    if(fontPath != nullptr)
    {
	if(PrintFontPaths(&smBase, fontPath, numPaths) != 0)
	{
	    XFreeFontPath(fontPath);
	    SM_FREE(buttonRet);
	    return(-1);
	}
    }

    /*
     * Write out all the keyboard control information.
     */
    XGetKeyboardControl(smGD.display, &smSettings.kbdState);
    if(PrintKeyboardControl(&smBase) != 0)
    {
	if (fontPath)
	    XFreeFontPath(fontPath);
	return(-1);
    }
    
    /*
     * Put all the button modifier information into the resource database.
     * Find out the number of buttons and then write out the character
     * string for each button.
     */
    numButton = XGetPointerMapping(smGD.display, (unsigned char *)buttonRet, 128);
    if(numButton > 128)
    {
	buttonRet = (char *) SM_REALLOC(buttonRet, numButton * sizeof(char));
	if (buttonRet==nullptr)
	{
	    if (fontPath)
		XFreeFontPath(fontPath);
	    PrintErrnoError(DtError, smNLS.cantMallocErrorString);
	    return(-1);
	}
	numButton = XGetPointerMapping(smGD.display, (unsigned char *)buttonRet, numButton);
    }
    if(PrintPointerMapping(&smBase, buttonRet,
			   numButton)  != 0)
    {
	if (fontPath)
	    XFreeFontPath(fontPath);
	SM_FREE(buttonRet);
	return(-1);
    }

    /*
     * Put all keyboard mapping information in the resource database
     * Don't bother putting this in a separate print function because
     * the customizer doesn't do keymap savings this way.
     */
    smSettings.numKeyCode = smGD.display->max_keycode -
	smGD.display->min_keycode;
    tmpSyms = XGetKeyboardMapping(smGD.display,
				  (KeyCode) smGD.display->min_keycode,
				  smSettings.numKeyCode,
				  &smSettings.keySymPerCode);
    if(tmpSyms == nullptr)
    {
	if (fontPath)
	    XFreeFontPath(fontPath);
	SM_FREE(buttonRet);
	PrintErrnoError(DtError, smNLS.cantMallocErrorString);
	return(-1);
    }
    
    PutNumberResource(&smBase, SmCnumKeyCode, (long) smSettings.numKeyCode);
    
    PutNumberResource(&smBase, SmCkeySymsPerKey, (long) smSettings.keySymPerCode);

    numLoops = smSettings.numKeyCode * smSettings.keySymPerCode;
    ok = StartResSpec(SmCkeySyms) && AppendResSpecNumber((long) tmpSyms[0]);
    for(i = 1;ok && i < numLoops;i++)
    {
	ok = AppendResSpec(",") && AppendResSpecNumber((long) tmpSyms[i]);
    }
    if (!ok)
    {
	SM_FREE((char *)tmpSyms);
	if (fontPath)
	    XFreeFontPath(fontPath);
	SM_FREE(buttonRet);
	XrmDestroyDatabase(smBase);
	return(-1);
    }
    XrmPutLineResource( &smBase, resSpec);
	

    /*
     * Write out all keyboard modifier info.  This will be a pretty slow
     * process considering that every keycode will have to be turned into
     * a keysym before writing.  Don't bother putting print stuff in a
     * separate function because it will not be handled in the customize
     * version.
     */
    modMap = XGetModifierMapping(smGD.display);
    if(modMap == nullptr)
    {
	SM_FREE((char *)tmpSyms);
	if (fontPath)
	    XFreeFontPath(fontPath);
	SM_FREE(buttonRet);
	PrintErrnoError(DtError, smNLS.cantMallocErrorString);
	return(-1);
    }

    PutNumberResource(&smBase, SmCmaxKeyPerMod, (long) modMap->max_keypermod);

    numLoops = (8 * modMap->max_keypermod) - 1;
    ok = StartResSpec(SmCmodMap);
    for(i = 0;ok && i <= numLoops;i++)
    {
	tmpMod = XKeycodeToKeysym(smGD.display, modMap->modifiermap[i], 0);
	ok = AppendResSpecNumber((long) tmpMod) &&
	     (i == numLoops || AppendResSpec(","));
    }
    if (!ok)
    {
	SM_FREE((char *)tmpSyms);
	XFreeModifiermap(modMap);
	if (fontPath)
	    XFreeFontPath(fontPath);
	SM_FREE(buttonRet);
	XrmDestroyDatabase(smBase);
	return(-1);
    }
    XrmPutLineResource( &smBase, resSpec);
    

    /*
     * Print session manager settings
     */
    PrintSessionInfo(&smBase);
    
    /*
     * Write settings resources out to disk
     */
    XrmPutFileDatabase(smBase, smGD.settingPath);

    /*
     * Free all allocated resources
     */
    if (fontPath)
	XFreeFontPath(fontPath);
    SM_FREE((char *)tmpSyms);
    XFreeModifiermap(modMap);
    SM_FREE(buttonRet);
    XrmDestroyDatabase(smBase);
    return(0);
}


/*************************************<->*************************************
 *
 *  SaveCustomizeSettings ()
 *
 *
 *  Description:
 *  ------------
 *  Saves those settings that have been set in the customizer during the
 *  session to whatever values they were set to during the session.
 *
 *
 *  Inputs:
 *  ------
 *  smGD.resourcePath = (global) a pointer
 *			to where the settings should be stored.
 *
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 *  This routine is the default saved for saving settings.  If the user
 *  wants all settings saved, they have to set a resource saying that that
 *  will be the case.
 * 
 *************************************<->***********************************/
static int 
SaveCustomizeSettings( void )
{
    int			i,numPaths;
    char		**fontPath;
    XrmDatabase 	smBase = nullptr;
    char		*ptrRet = nullptr, *tmpKey;
    Status		newStat;
	char		tmpMap[128];


    if(smCust.pointerChange)
    {
	newStat = _DtGetSmPointer(smGD.display, smGD.topLevelWindow,
				 XaDtSmPointerInfo, &ptrRet);
	if(newStat != Success)
	{
	    smToSet.pointerChange = False;
	    smToSet.pointerMapChange = False;
	}
	else
	{
	    sscanf(ptrRet, "%d %127s %d %d %48s",
		   &smToSet.numButton,
		   tmpMap,
		   &smSettings.accelNum,
		   &smSettings.threshold,
		   smToSet.dClickBuf);
	    smSettings.accelDenom = 1;

	    tmpKey = strtok(tmpMap, "_");
	    i = 0;
	    while(tmpKey != nullptr)
	    {
		smToSet.pointerMap[i] = (char) atoi(tmpKey);
		i++;
		tmpKey = strtok(nullptr, "_");
	    }


	    /*
	     * append a \n to dClickBuf because it was scanned out
	     */
	    {
		size_t used = strlen(smToSet.dClickBuf);

		if (used + 2 <= sizeof smToSet.dClickBuf)
		{
		    smToSet.dClickBuf[used] = '\n';
		    smToSet.dClickBuf[used + 1] = 0;
		}
	    }
	}
        if (ptrRet) XFree(ptrRet);
    }
				     
    if(smToSet.pointerChange == True)
    {
	PrintPointerControl(&smBase);
    }

    if(smToSet.pointerMapChange == True)
    {
	if(PrintPointerMapping(&smBase, smToSet.pointerMap,
			    smToSet.numButton) != 0)
	{
	    return(-1);
	}
    }

    if(smToSet.screenSavChange == True)
    {
	PrintScreenSaver(&smBase);
    }

    if(smCust.audioChange == True)
    {
      newStat = _DtGetSmAudio(smGD.display, smGD.topLevelWindow,
				   XaDtSmAudioInfo, &audioVals);
     
      if(newStat == Success)
      {
        smSettings.kbdState.bell_percent = audioVals.smBellPercent;
        smSettings.kbdState.bell_pitch = audioVals.smBellPitch;
        smSettings.kbdState.bell_duration = audioVals.smBellDuration;
      }  
      else
      {
        smToSet.audioChange = False;
      }
    }
    
    if (smToSet.audioChange)
    {
      PutNumberResource(&smBase, SmCbellPercent, (long) smSettings.kbdState.bell_percent);

      PutNumberResource(&smBase, SmCbellPitch, (long) smSettings.kbdState.bell_pitch);

      PutNumberResource(&smBase, SmCbellDuration, (long) smSettings.kbdState.bell_duration);
    }

    if(smCust.keyboardChange == True)
    {
      newStat = _DtGetSmKeyboard(smGD.display, smGD.topLevelWindow,
                                      XaDtSmKeyboardInfo, &keyboardVals);
      if(newStat == Success)
      {
        smSettings.kbdState.key_click_percent = keyboardVals.smKeyClickPercent;
        smSettings.kbdState.global_auto_repeat = keyboardVals.smGlobalAutoRepeat;
      }
      else
      {
        smToSet.keyboardChange = False;
      }
    }

    if(smToSet.keyboardChange == True)
    {
      PutNumberResource(&smBase, SmCkeyClick, (long) smSettings.kbdState.key_click_percent);

      PutNumberResource(&smBase, SmCglobalRepeats, (long) smSettings.kbdState.global_auto_repeat);
    }


    /*
     *  Write out all font path information. 
     *  Write out all the paths one by one concatenated by commas.
     */

    fontPath = XGetFontPath(smGD.display, &numPaths);
    if(fontPath != nullptr) {
	if(PrintFontPaths(&smBase,  fontPath, numPaths) != 0) {
	    XFreeFontPath(fontPath);
	    return(-1);
	}
    }

    /*
     * Save session manager settings
     */
    PrintSessionInfo(&smBase);
    
    if (fontPath)
	XFreeFontPath(fontPath);

    /*
     * Write settings resources out to disk
     */
    XrmPutFileDatabase(smBase, smGD.settingPath);
    XrmDestroyDatabase(smBase);

    return(0);
}



/*************************************<->*************************************
 *
 *  OutputResource ()
 *
 *
 *  Description:
 *  -----------
 *  Save session resources by getting the current RESOURCE_MANAGER property
 *  on the root window.
 *
 *  Inputs:
 *  ------
 *  smGD.resourcePath = (global) file name
 *			where property contents should be saved.
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 *  This method is not a 100% accurate representation of currently active
 *  resources in the system.  It is however, the best representation (the
 *  values with the highest priority) without becoming overly invasive
 *  and slow.
 * 
 *************************************<->***********************************/
static int 
OutputResource( void )
{
    Atom 		actualType;
    int	 		actualFormat;
    unsigned long	nitems, leftover; 
    char	 	*data = nullptr;
    XrmDatabase		db;
    Status		newStat;
    char		*fontBuf = nullptr, *langPtr, *sessionRes;
    Boolean		ok;
    size_t		used;
    int			written;
    float		fltYRes;
    int			intYRes;
    char                *preeditBuf = nullptr;

    /*
     * Add anything to the Resource Manager property that needs to be added
     */

    /*
     * Write out the LANG variable and the screen's Y resolution
     */
    *resSpec = 0;
    langPtr = getenv("LANG");
    ok = AppendResSpec(SM_RESOURCE_NAME) && AppendResSpec("*") &&
	 AppendResSpec(SmNsessionLang) && AppendResSpec(":");
    if (ok)
    {
	if((langPtr != nullptr) && (*langPtr != 0))
	{
	    ok = AppendResSpec(" ") && AppendResSpec(langPtr) &&
		 AppendResSpec("\n");
	}
	else
	{
	    ok = AppendResSpec("   \n");
	}
    }

    fltYRes = ((float) DisplayHeight(smGD.display, 0) /
	(float) DisplayHeightMM(smGD.display, 0)) * 1000;
    intYRes = fltYRes;

    ok = ok && AppendResSpec(SM_RESOURCE_NAME) && AppendResSpec("*") &&
	 AppendResSpec(SmNdisplayResolution) && AppendResSpec(":") &&
	 AppendResSpecNumber((long) intYRes) && AppendResSpec("\n");
    if (!ok)
    {
	return(-1);
    }
    _DtAddToResource(smGD.display, resSpec);
    
    if(smCust.fontChange == True)
    {
	newStat = _DtGetSmFont(smGD.display, smGD.topLevelWindow,
			      XaDtSmFontInfo, &fontBuf);
	if(newStat == Success)
	{
	    /*
	     * Create the auxillary resource file
	     */
	    if(SetFontSavePath(langPtr) != -1)
	    {
		db = XrmGetStringDatabase(fontBuf);
	    
		if(intYRes < MED_RES_Y_RES)
		{
		    sessionRes = SM_LOW_RES_EXT;
		}
		else
		{
		    if(intYRes >= HIGH_RES_Y_RES)
		    {
			sessionRes = SM_HIGH_RES_EXT;
		    }
		    else
		    {
			sessionRes = SM_MED_RES_EXT;
		    }
		}

		used = strlen(smGD.fontPath);
		written = snprintf(smGD.fontPath + used,
				   sizeof smGD.fontPath - used, "/%s.%s",
				   SM_FONT_FILE, sessionRes);
		if (written < 0 ||
		    (size_t) written >= sizeof smGD.fontPath - used)
		{
		    smGD.fontPath[used] = 0;
		    PrintError(DtError, smNLS.cantCreateDirsString);
		}
		else
		{
		    XrmPutFileDatabase(db, smGD.fontPath);
		}
		XrmDestroyDatabase(db);
	    }
	    
	    /*
	     * Now add this to the resource manager property to be saved
	     */
	    _DtAddToResource(smGD.display, fontBuf);
	}
        if (fontBuf) XFree(fontBuf);
    }

    if(smCust.preeditChange == True)
    {
	newStat = _DtGetSmPreedit(smGD.display, smGD.topLevelWindow,
				  XaDtSmPreeditInfo, &preeditBuf);
	if(newStat == Success)
	{
	    _DtAddToResource(smGD.display, preeditBuf);
	}
	if (preeditBuf) XFree(preeditBuf);
    }

    if((smCust.dClickChange == True) && (smToSet.dClickBuf[0] != 0))
    {
	_DtAddToResource(smGD.display, (char *)smToSet.dClickBuf);
    }

    /*
     * Get the contents of the _DT_SM_PREFERENCES property
     */
    data = _DtGetResString(smGD.display, _DT_ATR_PREFS);

   /* 
    * Get _DT_SM_PREFERENCES database 'db'.
    */
    db  = XrmGetStringDatabase((char *)data);

    XrmPutFileDatabase(db, smGD.resourcePath);

    /*
     * Don't forget to free your data
     */
    SM_FREE((char *)data);
    XrmDestroyDatabase(db);
    
    return(0);
}



/*************************************<->*************************************
 *
 *  PrintPointerControl (smBase)
 *
 *
 *  Description:
 *  -----------
 *  A convenience function that is separated out instead of being included
 *  in both QueryServerSettings, and SaveCustomizeSettings.
 *  Saves pointer control information to the named resource file.
 *
 *  Inputs:
 *  ------
 *  smBase = pointer to newly opened resource db used to store setting info
 *  resSpec = a buffer to hold string resource information until it is
 *            printed.
 *  smSettings = place where settings are stored.
 * 
 *  Outputs:
 *  -------
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
static void 
PrintPointerControl(
        XrmDatabase *smBase)
{
    PutNumberResource(smBase, SmCaccelNum, (long) smSettings.accelNum);
    
    PutNumberResource(smBase, SmCaccelDenom, (long) smSettings.accelDenom);
    
    PutNumberResource(smBase, SmCthreshold, (long) smSettings.threshold);

    return;
}


/*************************************<->*************************************
 *
 *  PrintSessionInfo (smBase)
 *
 *
 *  Description:
 *  -----------
 *  A convenience function that is separated out instead of being included
 *  in both QueryServerSettings, and SaveCustomizeSettings.
 *  Saves session manager settings to the named resource file.
 *
 *  Inputs:
 *  ------
 *  smBase = pointer to newly opened resource db used to store setting info
 *  resSpec = a buffer to hold string resource information until it is
 *            printed.
 *  smSettings = place where settings are stored.
 * 
 *  Outputs:
 *  -------
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
static void 
PrintSessionInfo(
        XrmDatabase *smBase)
{
    /*
     * Write out the settings for logout confirmation and
     * the correct session to return to
     */
    PutNumberResource(smBase, SmCshutDownState, (long) smSettings.confirmMode);
    
    PutNumberResource(smBase, SmCshutDownMode, (long) smSettings.startState);

    return;
}


/*************************************<->*************************************
 *
 *  PrintScreenSaver (smBase)
 *
 *
 *  Description:
 *  -----------
 *  A convenience function that is separated out instead of being included
 *  in both QueryServerSettings, and SaveCustomizeSettings.
 *  Saves screen saver information to the named resource file.
 *
 *  Inputs:
 *  ------
 *  smBase = pointer to newly opened resource db used to store setting info
 *  resSpec = a buffer to hold string resource information until it is
 *            printed.
 * 
 *  Outputs:
 *  -------
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
static void 
PrintScreenSaver(
        XrmDatabase *smBase)
{
    Status		newScreenStat;
    if(smCust.screenSavChange == True)
    {
	newScreenStat = _DtGetSmScreen(smGD.display, smGD.topLevelWindow,
				      XaDtSmScreenInfo, &screenSaverVals);
	/*
	 *  If the property has been deleted, it means that we return to
	 *  the default
	 */
	if(newScreenStat != Success)
	{
	    return;
	}
    }

    PutNumberResource(smBase, SmCtimeout, (long) screenSaverVals.smTimeout);
    
    PutNumberResource(smBase, SmCinterval, (long) screenSaverVals.smInterval);
    
    PutNumberResource(smBase, SmCpreferBlank, (long) screenSaverVals.smPreferBlank);
    
    PutNumberResource(smBase, SmCallowExp, (long) screenSaverVals.smAllowExp);

    return;
}


/*************************************<->*************************************
 *
 *  PrintFontPaths (smBase, fontPaths, numPaths)
 *
 *
 *  Description:
 *  -----------
 *  A convenience function that is separated out instead of being included
 *  in both QueryServerSettings, and SaveCustomizeSettings.
 *  Saves pointer control information to the named resource file.
 *
 *  Inputs:
 *  ------
 *  smBase = pointer to newly opened resource db used to store setting info
 *  resSpec = a buffer to hold string resource information until it is
 *            printed
 *  fontPaths = font paths to be saved
 *  numPaths = number of font paths to be saved
 * 
 *  Outputs:
 *  -------
 *  resSize = made bigger if current buffer is too small to hold all font info
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
static int 
PrintFontPaths(
        XrmDatabase *smBase,
        char **fontPath,
        int numPaths)
{
    int	i;
    
    /* If no fp save req'd, just pretend we did it */
    if (smRes.saveFontPath == False) return 0;

    if (!StartResSpec(SmCfontPath) || !AppendResSpec(*fontPath))
    {
	return(-1);
    }
    for(i = 1;i < numPaths;i++)
    {
	if (!AppendResSpec(",") || !AppendResSpec(fontPath[i]))
	{
	    return(-1);
	}
    }
    XrmPutLineResource(smBase, resSpec);

    return(0);
}



/*************************************<->*************************************
 *
 *  PrintKeyboardControl (smBase)
 *
 *
 *  Description:
 *  -----------
 *  A convenience function that is separated out instead of being included
 *  in both QueryServerSettings, and SaveCustomizeSettings.
 *  Saves pointer control information to the named resource file.
 *
 *  Inputs:
 *  ------
 *  smBase = pointer to newly opened resource db used to store setting info
 *  resSpec = a buffer to hold string resource information until it is
 *            printed
 *  resSize = size of current resource buffer
 * 
 *  Outputs:
 *  -------
 *  resSize = buffer is enlarged if more room is need to hold this info
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
static int 
PrintKeyboardControl(
        XrmDatabase *smBase)
{
    int		i,j;
    char	bitTest;
    Bool	firstRepeat = True;
    PutNumberResource(smBase, SmCkeyClick, (long) smSettings.kbdState.key_click_percent);

    PutNumberResource(smBase, SmCbellPercent, (long) smSettings.kbdState.bell_percent);

    PutNumberResource(smBase, SmCbellPitch, (long) smSettings.kbdState.bell_pitch);

    PutNumberResource(smBase, SmCbellDuration, (long) smSettings.kbdState.bell_duration);
    
    PutNumberResource(smBase, SmCledMask, (long) smSettings.kbdState.led_mask);

    PutNumberResource(smBase, SmCglobalRepeats, (long) smSettings.kbdState.global_auto_repeat);

    if (!StartResSpec(SmCautoRepeats))
    {
	return(-1);
    }

    /*
     * Now write out which keys need to be auto repeated one at a time
     */
    if(smSettings.kbdState.global_auto_repeat != AutoRepeatModeOn)
    {
	for(i = 0;i < 32;i++)
	{
	    bitTest = 0x01;
	    for(j = 0;j < 8;j++)
	    {
		if((bitTest & smSettings.kbdState.auto_repeats[i]) != 0)
		{
		    if(firstRepeat == True)
		    {
			firstRepeat = False;
		    }
		    else if (!AppendResSpec(","))
		    {
			return(-1);
		    }

		    if (!AppendResSpecNumber((long) ((8 * i) + j)))
		    {
			return(-1);
		    }
		}
		bitTest <<= 1;
	    }
	}
    }
    XrmPutLineResource(smBase, resSpec);

    return(0);
}



/*************************************<->*************************************
 *
 *  PrintPointerMapping (smBase, buttonRet,  numButton)
 *
 *
 *  Description:
 *  -----------
 *  A convenience function that is separated out instead of being included
 *  in both QueryServerSettings, and SaveCustomizeSettings.
 *  Saves pointer mapping information to the named resource file.
 *
 *  Inputs:
 *  ------
 *  smBase = pointer to newly opened resource db used to store setting info
 *  resSpec = a buffer to hold string resource information until it is
 *            printed
 *  resSize = size of resource buffer
 *  buttonRet = button mapping to be saved
 *  numButton = number of buttons in button mapping
 * 
 *  Outputs:
 *  -------
 *  resSize = size of buffer is change if it needs to be enlarged during
 *            routine
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
static int 
PrintPointerMapping(
        XrmDatabase *smBase,
        char *buttonRet,
        int numButton )
{
    int		i, numLoops;

    if (!StartResSpec(SmCbuttonMap))
    {
	return(-1);
    }

    numLoops = numButton - 1;
    for(i = 0;i <= numLoops;i++)
    {
	if (!AppendResSpecNumber((long) buttonRet[i]) ||
	    (i != numLoops && !AppendResSpec(",")))
	{
	    return(-1);
	}
	XrmPutLineResource(smBase, resSpec);
    }

    return(0);
}
