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
/* $XConsortium: fileIo.c /main/4 1996/10/04 17:24:28 drk $ */
/**********************************<+>*************************************
***************************************************************************
**
**  File:        fileIo.c
**
**  Project:     DT dtpad, a memo maker type editor based on the Dt Editor
**               widget.
**
**  Description:
**  -----------
**
*******************************************************************
**  (c) Copyright Hewlett-Packard Company, 1991, 1992.  All rights are
**  reserved.  Copying or other reproduction of this program
**  except for archival purposes is prohibited without prior
**  written consent of Hewlett-Packard Company.
********************************************************************
**
********************************************************************
**  (c) Copyright 1993, 1994 Hewlett-Packard Company
**  (c) Copyright 1993, 1994 International Business Machines Corp.
**  (c) Copyright 1993, 1994 Sun Microsystems, Inc.
**  (c) Copyright 1993, 1994 Novell, Inc.
********************************************************************
**
**  ToDo:	roll in fixes for 10.0 ivepad
**
**
**************************************************************************
**********************************<+>*************************************/
#include "dtpad.h"
#include <Xm/TextP.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <Xm/TextF.h>
#include <Xm/LabelG.h>
#include <Dt/HourGlass.h>


/************************************************************************
 * LoadFile - Opens the specified file and loads its contents to the
 *	DtEditor widget.
 *
 *	The filename is specified either by pPad->fileStuff.fileName or
 *	the 'include' argument.  If the filename is specified via include,
 *	the file is inserted at the current cursor location.  In both
 *	instances, the filename is assumed to be relative to the local host
 *	where dtpad is running.
 *	
 ************************************************************************/
/* [SuG 5/18/95] Done
   The NOT_SAMPLE warnings/errors need to be added to the message
   catalog. Removed ifdefs.*/

void 
LoadFile(
	Editor *pPad,
        char *include)
{
    char *		localName;
    DtEditorErrorCode	errorCode;
    DtEditorContentRec	contentRec;
    Boolean		fileLoaded = False;
    Boolean		fileLoadError = False;


    if (include != (char *)nullptr) {
	localName = include; 
    } else { 
        localName = pPad->fileStuff.fileName; 
	pPad->fileStuff.readOnly = False;
    } 

    if (localName && *localName) {
	/* try to load the file to the Dt Editor widget */ 
        _DtTurnOnHourGlass(pPad->app_shell);
        XSync(XtDisplay(pPad->editor), False);
	if (include != (char *) nullptr) {
	    errorCode = DtEditorInsertFromFile(
				pPad->editor,
				localName);
	} else {
	    pPad->fileStuff.fileExists = True;
	    errorCode = DtEditorSetContentsFromFile(
				pPad->editor,
				localName);
	}
        _DtTurnOffHourGlass(pPad->app_shell);
	switch (errorCode) {
	    case DtEDITOR_NO_ERRORS:	/* file is read/writeable */
		fileLoaded = True;
		break;
	    case DtEDITOR_READ_ONLY_FILE:
		fileLoaded = True;
		if (include == (char *) nullptr) {
		    pPad->fileStuff.readOnly = True;
		    if (pPad->xrdb.readOnlyWarning &&
		      ! pPad->xrdb.statusLine &&
		      ! pPad->xrdb.viewOnly) { 
			Warning(pPad, (char *) GETMESSAGE(6, 1,
			  "This file has been opened for reading only."),
			  XmDIALOG_WARNING);
		    }
		}
		break;
	    case DtEDITOR_NONEXISTENT_FILE:
		if (include != (char *) nullptr ) {
		    Warning(pPad, (char *) GETMESSAGE(6, 2,
			    "This file does not exist."),
			XmDIALOG_ERROR);
		    fileLoadError = True;
		} else if (pPad->xrdb.viewOnly) {
		    Warning(pPad, (char *) GETMESSAGE(6, 2,
			    "This file does not exist."),
			XmDIALOG_ERROR);
		} else if (pPad->xrdb.missingFileWarning) {
		    Warning(pPad, (char *) GETMESSAGE(6, 2,
			    "This file does not exist."),
			XmDIALOG_WARNING);
		}
		break;
	    case DtEDITOR_DIRECTORY:
		Warning(pPad, (char *) GETMESSAGE(6, 3,
			    "Unable to edit a Directory."),
			XmDIALOG_ERROR);
		fileLoadError = True;
		break;
	    case DtEDITOR_CHAR_SPECIAL_FILE:
		Warning(pPad, (char *) GETMESSAGE(6, 4,
			    "Unable to edit a character special device file."),
			XmDIALOG_ERROR);
		fileLoadError = True;
		break;
	    case DtEDITOR_BLOCK_MODE_FILE:
		Warning(pPad, (char *) GETMESSAGE(6, 5,
			    "Unable to edit a block mode device file."),
			XmDIALOG_ERROR);
		fileLoadError = True;
		break;
	    case DtEDITOR_NULLS_REMOVED:
		Warning(pPad, (char *) GETMESSAGE(6, 6,
			    "Embedded Nulls stripped from file."),
			XmDIALOG_WARNING);
		fileLoaded = True;
		break;
	    case DtEDITOR_INSUFFICIENT_MEMORY:
		Warning(pPad, (char*) GETMESSAGE(6, 7,
			    "Unable to load file (insufficient memory)."),
			XmDIALOG_ERROR);
		fileLoadError = True;
		break;
	    case DtEDITOR_NO_FILE_ACCESS:
		Warning(pPad,  (char *) GETMESSAGE(6, 8,
			    "Unable to access file (can't determine its status)."),
			XmDIALOG_ERROR);
		fileLoadError = True;
		break;
	    case DtEDITOR_UNREADABLE_FILE:
		Warning(pPad, (char *) GETMESSAGE(6, 9,
			    "Unable to read from the file (probably no read permission)."),
			XmDIALOG_ERROR);
		fileLoadError = True;
		break;
	    default:
		Warning(pPad, (char *) GETMESSAGE(6, 12,
			    "Unable to read from the file (unknown reason)."),
			XmDIALOG_ERROR);
		fileLoadError = True;
		break;
	} /* switch (errorCode) */
    } /* file name specified */
    
    if (include != (char *)nullptr)
	return; 

    /*
     * We only get this far if the current file was to be changed
     * (i.e. the file was not included)
     */ 
    SetStatusLineMsg(pPad);	/* set readOnly/viewOnly in status line msg */
 
    if (fileLoaded) {
	pPad->fileStuff.fileExists = True;
    } else {
	pPad->fileStuff.fileExists = False;

	/* null out Editor widget */
	contentRec.type = DtEDITOR_TEXT;
	contentRec.value.string = "";
	errorCode = DtEditorSetContents(pPad->editor, &contentRec);
	if (errorCode != DtEDITOR_NO_ERRORS)
	    Warning(pPad,"Internal Error: Unable to NULL Editor Widget.",XmDIALOG_ERROR);

	/* clear stale file stuff */
	if (fileLoadError) {
	    if (pPad->fileStuff.fileName && *pPad->fileStuff.fileName)
		XtFree(pPad->fileStuff.fileName);
	    pPad->fileStuff.fileName = nullptr;
	    ChangeMainWindowTitle(pPad);
	    return;
	}

	/* set the pathDir and fileName */
	if (pPad->fileStuff.fileName && *pPad->fileStuff.fileName) {
	    char *lastSlash;

	    snprintf(pPad->fileStuff.pathDir, sizeof(pPad->fileStuff.pathDir),
		     "%s", pPad->fileStuff.fileName);
	    if ((lastSlash = MbStrrchr(pPad->fileStuff.pathDir, '/')) != 
		                                             (char *)nullptr) {
	       *(lastSlash + 1) = (char)'\0';
	    }

	    lastSlash = MbStrrchr(pPad->fileStuff.fileName, '/');
	    if (lastSlash != (char *)nullptr) {
	        lastSlash++;
	    } else {
	        lastSlash = pPad->fileStuff.fileName;
	    }
	    if (*lastSlash == (char)'-' || *lastSlash == (char)'*') {
	        XtFree(pPad->fileStuff.fileName);
	        pPad->fileStuff.fileName = (char *)nullptr;
	    }
	}
    } /* ! fileLoaded */
}


/************************************************************************
 * GetTempFile - creates an empty temporary file and returns its name in
 *	XtMalloc'ed storage, or NULL if the file cannot be created.  The
 *	file is made with mkstemp(3) (mode 0600, owned by the caller), so no
 *	other user can have planted a file or a symbolic link at the name;
 *	the caller writes to it by name and removes it when done.
 ************************************************************************/
char *
GetTempFile(void)
{
    static const char nameTemplate[] = "/dtpXXXXXX";
    char *tempname = (char *)XtMalloc(strlen(P_tmpdir) + sizeof(nameTemplate));
    int fd;

    strcpy(tempname, P_tmpdir);
    strcat(tempname, nameTemplate);
    fd = mkstemp(tempname);
    if (fd == -1)
    {
        XtFree(tempname);
        return (char *)nullptr;
    }
    close(fd);
    return tempname;
}


extern Editor *pPadList;    /* declared/set in main.c */


/************************************************************************
 * AddPound - add a "#" character to the beginning of the base name of
 *	the file name passed in the nameBuf parameter.	A pointer to the
 *	new name is returned.  The name is constructed in a static buffer.
 *	If nameBuf is empty, it returns the file name "#UNTITLED#".
 ************************************************************************/
static char * 
AddPound(
        char *nameBuf)
{
    char tempBuf[512];
    static char returnBuf[512];

    if (nameBuf != (char *)nullptr && nameBuf[0] != (char) '\0') {
        char *baseName;
        size_t rlen;
	snprintf(tempBuf, sizeof(tempBuf), "%s", nameBuf);
        baseName = MbStrrchr(tempBuf, '/');
	if (baseName != (char *)nullptr) {
		size_t dirLen = (size_t)(baseName - tempBuf) + 1;
		if (dirLen >= sizeof(returnBuf))
		    dirLen = sizeof(returnBuf) - 1;
		memcpy(returnBuf, tempBuf, dirLen);
		returnBuf[dirLen] = (char)'\0';
		baseName++;
		if (dirLen + 1 < sizeof(returnBuf))
		    snprintf(returnBuf + dirLen, sizeof(returnBuf) - dirLen,
			     "#%s", baseName);
	} else {
	    snprintf(returnBuf, sizeof(returnBuf), "#%.*s",
		     (int)(sizeof(returnBuf) - 2), tempBuf);
	}
	rlen = strlen(returnBuf);
	if (rlen > 0 && returnBuf[rlen - 1] != (char)'#' &&
	    rlen + 1 < sizeof(returnBuf)) {
	    returnBuf[rlen] = (char)'#';
	    returnBuf[rlen + 1] = (char)'\0';
	}
    } else {
	snprintf(returnBuf, sizeof(returnBuf), "#%s#",
		 GETMESSAGE(5, 21, "UNTITLED"));
    }
    return returnBuf;
}


/************************************************************************
 * PanicSaveEditSessions - immediately saves the text for each edit session
 *	handled by the text editor to #[#]<filename>, and returns.
 *
 *	This is a best-effort emergency save and is not async-signal-safe;
 *	the only signal handler that may call it is the one for the fatal
 *	signals (see panicFatalHandler() in main.c), where the process cannot
 *	continue anyway.  A second call while the first is running, which is a
 *	fault inside the save, leaves at once with _exit().
 ************************************************************************/
static volatile sig_atomic_t panicSaving = 0;

void 
PanicSaveEditSessions(void)
{
    Editor *pPad;
    struct stat statbuf;        /* Information on a file. */
    Boolean addNewLines;
    DtEditorErrorCode errorCode;

    if (panicSaving)
	_exit(1);
    panicSaving = 1;

    for (pPad = pPadList; pPad != (Editor *)nullptr; pPad = pPad->pNextPad) {
	char *fileName = pPad->fileStuff.fileName;
	if (pPad->inUse != True || pPad->editor == nullptr || 
		(! DtEditorCheckForUnsavedChanges(pPad->editor)))
	    continue;
	/* -----> Try to cook up a name for a file that doesn't already exist */
	fileName = AddPound(pPad->fileStuff.fileName);
	if (stat(fileName, &statbuf) == 0) {
	    fileName = AddPound(fileName);
	    if (stat(fileName, &statbuf) == 0) {
	        fileName = AddPound(fileName);
	        if (stat(fileName, &statbuf) == 0) {
		    /* Give up. We've tried enough names.  The user loses. */
	            continue;
		}
	    }
	}
	addNewLines = pPad->xrdb.wordWrap == True &&
			  pPad->fileStuff.saveWithNewlines == True;
	errorCode = DtEditorSaveContentsToFile(
			pPad->editor,
			fileName,
			False,		/* don't overwrite existing file */
			addNewLines,	/* replace soft line feeds? */
			False);		/* don't mark contents as saved */

	/* don't really care about errorCode since we're in a hurry and
	 * can't do anything about it anyway */
    }
}


/************************************************************************
 * PanicSave - saves the edit sessions and exits.  For callers in normal
 *	context (the X error handlers, the termination signal procedure).
 ************************************************************************/
void 
PanicSave(void)
{
    PanicSaveEditSessions();
    exit(1);
}


/************************************************************************
 * SetStatusLineMsg - sets the message (XmTextField) portion of the
 *	DtEditor widget's status line based on whether the pad is in
 *	viewOnly mode or the file being edited/displayed is readOnly.
 ************************************************************************/
/* ARGSUSED */
void
SetStatusLineMsg(
	Editor *pPad)
{
    char *message="";

    if (pPad->xrdb.viewOnly) {
	message = (char *) GETMESSAGE(6, 10, "View Only");
    } else if (pPad->fileStuff.readOnly) {
	message = (char *) GETMESSAGE(6, 11, "Read Only");
    }
    
    XmTextFieldSetString(pPad->statusLineMsg, message);
}
