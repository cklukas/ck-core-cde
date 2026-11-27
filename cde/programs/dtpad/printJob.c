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
/* $TOG: printJob.c /main/29 1997/08/01 14:32:04 samborn $ */
/**********************************<+>*************************************
***************************************************************************
**
**  File:        printJob.c
**
**  Project:     HP DT dtpad, a memo maker type editor based on the
**               Dt Editor widget.
**
**  Description:  Routines which manipulate the print setup dialog
**
**************************************************************************
**********************************<+>*************************************/
/*
 *                   Common Desktop Environment
 *
 *   (c) Copyright 1993, 1994, 1995 Hewlett-Packard Company
 *   (c) Copyright 1993, 1994, 1995 International Business Machines Corp.
 *   (c) Copyright 1993, 1994, 1995 Sun Microsystems, Inc.
 *   (c) Copyright 1993, 1994, 1995 Novell, Inc.
 *   (c) Copyright 1995 Digital Equipment Corp.
 *   (c) Copyright 1995 Fujitsu Limited
 *   (c) Copyright 1995 Hitachi, Ltd.
 *
 *
 *                     RESTRICTED RIGHTS LEGEND
 *
 *Use, duplication, or disclosure by the U.S. Government is subject to
 *restrictions as set forth in subparagraph (c)(1)(ii) of the Rights in
 *Technical Data and Computer Software clause in DFARS 252.227-7013.  Rights
 *for non-DOD U.S. Government Departments and Agencies are as set forth in
 *FAR 52.227-19(c)(1,2).

 *Hewlett-Packard Company, 3000 Hanover Street, Palo Alto, CA 94304 U.S.A.
 *International Business Machines Corp., Route 100, Somers, NY 10589 U.S.A.
 *Sun Microsystems, Inc., 2550 Garcia Avenue, Mountain View, CA 94043 U.S.A.
 *Novell, Inc., 190 River Road, Summit, NJ 07901 U.S.A.
 *Digital Equipment Corp., 111 Powdermill Road, Maynard, MA 01754, U.S.A.
 *Fujitsu Limited, 1015, Kamikodanaka Nakahara-Ku, Kawasaki 211, Japan
 *Hitachi, Ltd., 6, Kanda Surugadai 4-Chome, Chiyoda-ku, Tokyo 101, Japan
 */


#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <Xm/Xm.h>
#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#if 0 && defined(PRINTING_SUPPORTED)
#include <Xm/Print.h>
#endif /* PRINTING_SUPPORTED */
#include <Dt/Editor.h>
#include <Dt/Print.h>

#include "dtpad.h"

static void	_pjCreatePrintShell(PrintJob *pJob);
static void	_pjCreateOutputWidgets(PrintJob *pJob);
static void	_pjDoPrint(PrintJob*);
static void	_pjUpdatePageHeaders(
				PrintJob*,
				PrintStringTypeEnum,
				PrintStringTypeEnum,
				PrintStringTypeEnum,
				PrintStringTypeEnum);
#if 0 && defined(PRINTING_SUPPORTED)
static void	_pjFinishedPrintToFile(
				Display*,
				XPContext,
				XPGetDocStatus,
				XPointer);
#endif /* PRINTING_SUPPORTED */
static char *	_pjGetPageHeaderString(PrintJob*, PrintStringTypeEnum);

static void	_pjCancelCB (Widget, XtPointer client_data, XtPointer);
static void	_pjCloseDisplayCB (Widget, XtPointer client_data, XtPointer);
static void	_pjPdmSetupCB (Widget, XtPointer client_data, XtPointer);
static void	_pjPdmNotificationCB (Widget, XtPointer client_data, XtPointer);
static void	_pjPrintCB (Widget, XtPointer client_data, XtPointer);
static void	_pjPrintOnePageCB(Widget, XtPointer, XtPointer);

static void	_pjRegisterActivePrintDisplay(Display*);
static void	_pjUnregisterActivePrintDisplay(Display*);

static Display	*_pjErrorPrintDisplay = nullptr;
static Display	**_pjActivePrintDisplay = nullptr;
static int	_pjMaxActivePrintDisplay = 0;

/************************************************************************
 * PrintJobCreate
 *	Creates a new print job for the specified document file.
 ************************************************************************/
PrintJob *
PrintJobCreate(
	char *documentName,
	char *tempFileName,
	Boolean silent,
	Editor *pPad
	)
{
    PrintJob *pJob;

    pJob = (PrintJob *) XtMalloc( sizeof(PrintJob) );
    memset(pJob, 0, sizeof(PrintJob));

    pJob->pPad = pPad;
    pJob->parentShell = pPad->app_shell;
    pJob->documentName = XtNewString(documentName);
    pJob->tempFileName = XtNewString(tempFileName);
    pJob->silent = silent;

    pJob->pOutput = nullptr;
    pJob->pSetup = nullptr;
    pJob->pShell = nullptr;
    pJob->printData = (DtPrintSetupData*) XtMalloc(sizeof(DtPrintSetupData));
    memset(pJob->printData, 0, sizeof(DtPrintSetupData));

    pJob->npagesDone = 0;
    pJob->npagesTotal = 0;

    pJob->nextpageShell = nullptr;
    pJob->nextpageButton = nullptr;

    return pJob;
}

/************************************************************************
 * PrintJobDestroy
 *	Destroys the specified PrintJob.
 ************************************************************************/
void
PrintJobDestroy(PrintJob *pJob)
{
    if (pJob == nullptr)
      return;

    if (pJob->pPad)
    {
	pJob->pPad->numPendingTasks--;
        ClearStatusMessage(pJob->pPad);
        XtSetSensitive(pJob->pPad->fileStuff.fileWidgets.printBtn, True);
        /*
        XtSetSensitive(pJob->pPad->fileStuff.fileWidgets.silentPrintBtn, True);
        */
    }
    if (pJob->documentName != nullptr)
    {
        free((char*) pJob->documentName);
	pJob->documentName = nullptr;
    }
    if (pJob->tempFileName != nullptr)
    {
        free((char*) pJob->tempFileName);
	pJob->tempFileName = nullptr;
    }
    if (pJob->pOutput != nullptr)
    {
        PrintOutputDestroy(pJob->pOutput);
	pJob->pOutput = nullptr;
    }
    if (pJob->pShell != nullptr)
    {
        _pjRegisterActivePrintDisplay(XtDisplay(pJob->pShell));
	if (XtDisplay(pJob->pShell) == PrintJobGetErrorPrintDisplay())
	{
	    char *errMsg;

	    /*
	     * Need to display an error dialog;
	     */
	    errMsg =
	      GETMESSAGE(
		14, 24,
		"The X Print Server is temporarily out of resources.");

	    Warning(pJob->pPad, (char *) errMsg, XmDIALOG_ERROR);
	    PrintJobSetErrorPrintDisplay(nullptr);
	}

        XtDestroyWidget(pJob->pShell);
	pJob->pShell = nullptr;
    }
    if (pJob->pSetup != nullptr)
    {
        PrintSetupDestroy(pJob->pSetup);
	pJob->pSetup = nullptr;
    }
    if (pJob->printData)
    {
        DtPrintFreeSetupData(pJob->printData);
        XtFree((XtPointer) pJob->printData);
	pJob->printData = nullptr;
    }
    if (pJob->nextpageShell)
    {
        XtDestroyWidget(pJob->nextpageShell);
	pJob->nextpageShell = nullptr;
    }

    XtFree((char *) pJob);
}

/************************************************************************
 * PrintJobExecute
 *	Executes the specified PrintJob
 ************************************************************************/
void
PrintJobExecute(PrintJob *pJob)
{
    if (pJob == nullptr) return;

    pJob->pSetup = PrintSetupCreate(
				pJob->parentShell,
				pJob->documentName,
				pJob->pPad->xrdb.wordWrap,
				pJob->pPad,
				_pjCancelCB, pJob,
				_pjCloseDisplayCB, pJob,
				_pjPrintCB, pJob,
				_pjPdmSetupCB, pJob);
    if (pJob->silent)
    {
	/*
	 * The DtPrintSetupDialog will display itself automatically
	 * along with an error message in the event of an error.
	 */
        if (FALSE == PrintSetupGetDefaultPrintData(
						pJob->pSetup,
						pJob->printData))
	  return;

        _pjCreatePrintShell(pJob);
        _pjDoPrint(pJob);
    }
    else
      PrintSetupDisplay(pJob->pSetup);
}

/************************************************************************
 * PrintJobCancel
 *	Cancels and deletes the specified print job.
 ************************************************************************/
void
PrintJobCancel(PrintJob *pJob)
{
    PrintJobDestroy(pJob);
}


/************************************************************************
 * PrintJobGetErrorPrintDisplay
 *	Returns the last print display on which an error occurred.
 ************************************************************************/
Display *
PrintJobGetErrorPrintDisplay(void)
{
    return _pjErrorPrintDisplay;
}

/************************************************************************
 * PrintJobSetErrorPrintDisplay
 *	Save a pointer to the print display on which an error occurred.
 ************************************************************************/
void
PrintJobSetErrorPrintDisplay(Display *display)
{
    _pjErrorPrintDisplay = display;
}

/************************************************************************
 * PrintJobIsActivePrintDisplay
 *	Save a pointer to the print display on which an error occurred.
 ************************************************************************/
Boolean
PrintJobIsActivePrintDisplay(Display *display)
{
    int i;

    for (i = 0; i < _pjMaxActivePrintDisplay; i++)
      if (display == _pjActivePrintDisplay[i])
	return True;

    return False;
}

/************************************************************************
 * _pjRegisterActivePrintDisplay
 *	Save the Display pointer for an active print display connection
 ************************************************************************/
static void
_pjRegisterActivePrintDisplay(Display *display)
{
    size_t size;
    Display **grown;
    int i;

    if (0 == _pjMaxActivePrintDisplay)
    {
	_pjMaxActivePrintDisplay = 10;
	size = _pjMaxActivePrintDisplay * sizeof(Display*);
	_pjActivePrintDisplay = (Display**) malloc(size);
        memset((char*) _pjActivePrintDisplay, 0, size);
    }

    for (i = 0; i < _pjMaxActivePrintDisplay; i++)
    {
	if (nullptr == _pjActivePrintDisplay[i])
	{
	    _pjActivePrintDisplay[i] = display;
	    return;
	}
    }

    if (_pjMaxActivePrintDisplay > INT_MAX / 2)
	return;
    grown = (Display**) reallocarray((void*) _pjActivePrintDisplay,
				     2 * _pjMaxActivePrintDisplay,
				     sizeof(Display*));
    if (nullptr == grown)
	return;
    _pjActivePrintDisplay = grown;
    memset((char*) (_pjActivePrintDisplay + _pjMaxActivePrintDisplay), 0,
	   _pjMaxActivePrintDisplay * sizeof(Display*));
    _pjActivePrintDisplay[_pjMaxActivePrintDisplay] = display;
    _pjMaxActivePrintDisplay *= 2;
}

/************************************************************************
 * _pjUnregisterActivePrintDisplay
 *	Delete the Display pointer for an active print display connection
 ************************************************************************/
static void
_pjUnregisterActivePrintDisplay(Display *display)
{
    int i;

    for (i = 0; i < _pjMaxActivePrintDisplay; i++)
      if (display == _pjActivePrintDisplay[i])
	_pjActivePrintDisplay[i] = nullptr;
}


/************************************************************************
 * _pjCreatePrintShell
 *      Creates the print shell (XmPrintShell or XmDialogShell) to control
 *	printing.
 ************************************************************************/
static void
_pjCreatePrintShell(PrintJob *pJob)
{
    DtPrintSetupData    *psd = nullptr;

    if (pJob == nullptr ||
	pJob->pShell != nullptr ||
	pJob->parentShell == nullptr ||
	pJob->pSetup == nullptr) return;

#if 0 && defined(PRINTING_SUPPORTED)
    
    /*
     * Create the print shell and
     * the print output widgets
     */
#ifdef PRINT_TO_VIDEO
    /*
     * Create a dialog shell widget on the video display.
     */
    pJob->pShell = XmCreateDialogShell(
				pJob->parentShell,
				"PrintVideo",
				nullptr, 0);
    {
        XmString    xms;

        pJob->nextpageShell = XmCreateDialogShell(
					pJob->parentShell,
					"PrintNextPage",
					nullptr, 0);

        xms = XmStringCreateLocalized("Push for Next Page");
        pJob->nextpageButton = XtVaCreateManagedWidget(
                                	"NextPageButton",
                                	xmPushButtonWidgetClass,
                                	pJob->nextpageShell,
                                	XmNlabelString, xms,
                                	XmNwidth, 200,
                                	XmNheight, 200,
                                	nullptr);
        XmStringFree(xms);

        XtAddCallback(
                pJob->nextpageButton,
                XmNactivateCallback,
                _pjPrintOnePageCB,
                (XtPointer) pJob);

        XtManageChild(pJob->nextpageShell);
        XtRealizeWidget(pJob->nextpageShell);
    }
#else

    /*
     * Create an XmPrintShell widget on the print display.  
     */ 
    psd = pJob->printData;
    if (psd != nullptr)
      pJob->pShell = XmPrintSetup(
		pJob->parentShell,
		XpGetScreenOfContext(psd->print_display, psd->print_context),
		"Print",
		nullptr, 0);

    if (pJob->pShell == nullptr) return;

    XtAddCallback(
                pJob->pShell,
                XmNpageSetupCallback,
                _pjPrintOnePageCB,
                (XtPointer) pJob);
    XtAddCallback(
                pJob->pShell,
                XmNpdmNotificationCallback,
                _pjPdmNotificationCB,
                (XtPointer) pJob);
#endif

    if (pJob->pShell == nullptr) return;
#endif  /* PRINTING_SUPPORTED */

}



/************************************************************************
 * _pjCreateOutputWidgets
 *	Actually run the specified PrintJob.
 *      Creates the PrintOutput object to be used for printing.
 *      Initializes the mailbox to iterate through the messages.
 *      Calls spoolOne to send a print job to the Xp server.
 ************************************************************************/
static void
_pjCreateOutputWidgets(PrintJob *pJob)
{
    DtEditorErrorCode	errorCode;
    Boolean		parseError;
    DtPrintSetupData    *psd = nullptr;
    int                 save_data;

    /*
     * Notify the user that we're printing
     */
    /* TBD:  Is there a status bar on pPad? */

    if (pJob->pShell == nullptr)
    {
        {/* TBD error dialog */}
        PrintJobDestroy(pJob);
        return;
    }

    pJob->pOutput = PrintOutputCreate(pJob->pShell);
    PrintOutputSetWordWrap(pJob->pOutput, PrintSetupUseWordWrap(pJob->pSetup));
    PrintOutputSetPageMargins(
	pJob->pOutput,
        PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_TOP),
        PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_RIGHT),
        PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_BOTTOM),
        PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_LEFT),
        &parseError
        );
    if (parseError)
    {
	/*
	 * Display an error dialog.
        DtMailGenDialog *genDialog = new DtMailGenDialog(
                                                        "Dialog",
                                                        _parent->baseWidget());
	 */
        char    *i18nMsg;
        char	*errMsg;

        i18nMsg = GETMESSAGE(
                        14, 1,
                        "One of the following margin specifiers \n has incorrect syntax: \n %s \n %s \n %s \n %s \nContinue using default margins?"
                        );

        errMsg = (char *) XtMalloc(1024);
        sprintf(
	    errMsg,
	    i18nMsg,
	    PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_TOP),
	    PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_RIGHT),
	    PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_BOTTOM),
	    PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_LEFT)
	    );

	Warning(pJob->pPad, (char *) errMsg, XmDIALOG_WARNING);
        XtFree(errMsg);
    }

    /*
     * Load the file
     */
    errorCode = PrintOutputLoadFile(pJob->pOutput, pJob->tempFileName);
    switch (errorCode)
    {
	case DtEDITOR_NO_ERRORS:
	case DtEDITOR_READ_ONLY_FILE:
	    break;
	case DtEDITOR_NONEXISTENT_FILE:
	    Warning(
		pJob->pPad,
		(char *) GETMESSAGE(14, 2, "File does not exist."),
		XmDIALOG_WARNING);
	    break;
	case DtEDITOR_DIRECTORY:
	    Warning(
		pJob->pPad,
		(char *) GETMESSAGE(14, 3, "Specified file is a directory."),
		XmDIALOG_WARNING);
	    break;
	case DtEDITOR_CHAR_SPECIAL_FILE:
	case DtEDITOR_BLOCK_MODE_FILE:
	    Warning(
		pJob->pPad,
		(char *) GETMESSAGE(14, 4, "File type error."),
		XmDIALOG_WARNING);
	    break;
	case DtEDITOR_NULLS_REMOVED:
	    Warning(
		pJob->pPad,
		(char *) GETMESSAGE(14, 5, "File contains NULL characters."),
		XmDIALOG_WARNING);
	    break;
	case DtEDITOR_INSUFFICIENT_MEMORY:
	    Warning(
		pJob->pPad,
		(char*)
		GETMESSAGE(14, 6, "Unable to load file (insufficient memory)."),
		XmDIALOG_ERROR);
		break;
	case DtEDITOR_NO_FILE_ACCESS:
	case DtEDITOR_UNREADABLE_FILE:
	default:
	    Warning(
		pJob->pPad,
		(char *)
		GETMESSAGE(14, 7, "File does not have read permissions"),
		XmDIALOG_WARNING);
	    break;
    }

    XtRealizeWidget( pJob->pShell );

    PrintOutputFirstPage(pJob->pOutput);
    pJob->npagesTotal = PrintOutputGetNumLines(pJob->pOutput) +
    			PrintOutputGetLinesPerPage(pJob->pOutput) - 1;
    pJob->npagesTotal /= PrintOutputGetLinesPerPage(pJob->pOutput);
    pJob->npagesDone = 0;
}




/************************************************************************
 * _pjDoPrint
 *	Actually run the specified PrintJob.
 *      Creates the PrintOutput object to be used for printing.
 *      Initializes the mailbox to iterate through the messages.
 *      Calls spoolOne to send a print job to the Xp server.
 ************************************************************************/
static char *
_pjGetHdrTemplateString(PrintJob *pJob, PrintStringTypeEnum type)
{
    if (type == PRINT_PAGE_NUMBER_STRING_TYPE_ENUM) {
        return XtNewString(GETMESSAGE(14, 10, "Page %d of %d"));
    }
    return _pjGetPageHeaderString(pJob, type);
}

static void
_pjDoPrint(PrintJob *pJob)
{
    if (pJob == nullptr || pJob->printData == nullptr) {
        PrintJobDestroy(pJob);
        return;
    }

    DtPrintSetupData *psd = pJob->printData;
    const char *env_pdf = getenv("DTPAD_PRINT_PDF_OUT");
    DtPrintSetupData local_psd = *psd;
    if (env_pdf && env_pdf[0]) {
        local_psd.destination = DtPRINT_TO_FILE;
        local_psd.dest_info = (String)env_pdf;
    }

    FILE *fp = (pJob->tempFileName && pJob->tempFileName[0])
                   ? fopen(pJob->tempFileName, "rb")
                   : nullptr;
    char *doc_bytes = nullptr;
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) {
            long sz = ftell(fp);
            rewind(fp);
            if (sz >= 0 && sz <= 64L * 1024L * 1024L) {
                doc_bytes = (char *)calloc((size_t)sz + 1, 1);
                if (doc_bytes && sz > 0) {
                    (void)fread(doc_bytes, 1, (size_t)sz, fp);
                }
            }
        }
        fclose(fp);
    }
    if (!doc_bytes) {
        doc_bytes = strdup("");
    }

    DtPrintJobOptions jopts;
    DtPrintInitJobOptions(&jopts);
    if (pJob->pSetup && pJob->pSetup->dtprintSetup) {
        DtPrintReadWidgetJobOptions(pJob->pSetup->dtprintSetup, &jopts);
    }
    if (pJob->pSetup) {
        jopts.word_wrap = PrintSetupUseWordWrap(pJob->pSetup);
        jopts.margin_top_pt = DtPrintParseMarginPoints(
            PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_TOP), 54.0);
        jopts.margin_right_pt = DtPrintParseMarginPoints(
            PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_RIGHT), 54.0);
        jopts.margin_bottom_pt = DtPrintParseMarginPoints(
            PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_BOTTOM), 54.0);
        jopts.margin_left_pt = DtPrintParseMarginPoints(
            PrintSetupGetMarginSpec(pJob->pSetup, DTPRINT_OPTION_MARGIN_LEFT), 54.0);
    }
    jopts.job_title = (pJob->documentName && pJob->documentName[0])
                          ? pJob->documentName : "dtpad Document";

    char *hl = pJob->pSetup
                   ? _pjGetHdrTemplateString(
                         pJob, PrintSetupGetHdrFtrSpec(pJob->pSetup, DTPRINT_OPTION_HEADER_LEFT))
                   : nullptr;
    char *hr = pJob->pSetup
                   ? _pjGetHdrTemplateString(
                         pJob, PrintSetupGetHdrFtrSpec(pJob->pSetup, DTPRINT_OPTION_HEADER_RIGHT))
                   : nullptr;
    char *fl = pJob->pSetup
                   ? _pjGetHdrTemplateString(
                         pJob, PrintSetupGetHdrFtrSpec(pJob->pSetup, DTPRINT_OPTION_FOOTER_LEFT))
                   : nullptr;
    char *fr = pJob->pSetup
                   ? _pjGetHdrTemplateString(
                         pJob, PrintSetupGetHdrFtrSpec(pJob->pSetup, DTPRINT_OPTION_FOOTER_RIGHT))
                   : nullptr;
    jopts.header_left = hl;
    jopts.header_right = hr;
    jopts.footer_left = fl;
    jopts.footer_right = fr;

    const char *tmp_base = getenv("TMPDIR");
    if (!tmp_base || !tmp_base[0]) tmp_base = "/var/tmp";
    char tmp_pdf[PATH_MAX];
    snprintf(tmp_pdf, sizeof(tmp_pdf), "%s/dtpad-print-%d-%ld.pdf",
             tmp_base, (int)getpid(), (long)time(nullptr));

    char err_buf[256] = {0};
    int pages = 0;
    int job_id = 0;
    XtEnum rc = DtPrintRenderTextToPdf(doc_bytes, &jopts, tmp_pdf, &pages, err_buf, sizeof(err_buf));
    if (rc == DtPRINT_SUCCESS) {
        rc = DtPrintDeliverPdfJob(&local_psd, &jopts, tmp_pdf, &job_id, err_buf, sizeof(err_buf));
    }
    unlink(tmp_pdf);
    free(doc_bytes);
    if (hl) free(hl);
    if (hr) free(hr);
    if (fl) free(fl);
    if (fr) free(fr);

    if (rc != DtPRINT_SUCCESS && !pJob->silent && pJob->pPad) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s\n%s",
                 GETMESSAGE(14, 14, "'Print' was unsuccessful."),
                 err_buf[0] ? err_buf : "");
        Warning(pJob->pPad, msg, XmDIALOG_WARNING);
    } else if (rc == DtPRINT_SUCCESS && local_psd.destination == DtPRINT_TO_FILE &&
               !pJob->silent && pJob->pPad) {
        SetStatusMessage(
            pJob->pPad,
            (char *)GETMESSAGE(14, 15, "'Print to File' completed successfully.")
        );
    }

    if (pJob->pPad) {
        /* Balance PrintJobDestroy's numPendingTasks-- */
        pJob->pPad->numPendingTasks++;
    }
    PrintJobDestroy(pJob);
}

/************************************************************************
 * _pjUpdatePageHeaders
 *	Configures the header and footer string in the PrintOutput. 
 ************************************************************************/
static void
_pjUpdatePageHeaders(
		PrintJob		*pJob,
		PrintStringTypeEnum	hl_type,
		PrintStringTypeEnum	hr_type,
		PrintStringTypeEnum	fl_type,
		PrintStringTypeEnum	fr_type
		)
{
    char *hl_string,
         *hr_string,
         *fl_string,
         *fr_string;

    if (pJob == (PrintJob *) nullptr) return;

    hl_string = _pjGetPageHeaderString(pJob, hl_type);
    hr_string = _pjGetPageHeaderString(pJob, hr_type);
    fl_string = _pjGetPageHeaderString(pJob, fl_type);
    fr_string = _pjGetPageHeaderString(pJob, fr_type);

    if (PRINT_NONE_STRING_TYPE_ENUM == hl_type &&
	PRINT_NONE_STRING_TYPE_ENUM == hr_type)
      PrintOutputHideHeaders(pJob->pOutput);
    else
      PrintOutputShowHeaders(pJob->pOutput);

    if (PRINT_NONE_STRING_TYPE_ENUM == fl_type &&
	PRINT_NONE_STRING_TYPE_ENUM == fr_type)
      PrintOutputHideFooters(pJob->pOutput);
    else
      PrintOutputShowFooters(pJob->pOutput);


    PrintOutputSetHdrFtrStrings(
				pJob->pOutput,
                                hl_string,
                                hr_string,
                                fl_string,
                                fr_string
				);
    free(hl_string);
    free(hr_string);
    free(fl_string);
    free(fr_string);
}

/************************************************************************
 * _pjGetPageHeaderString
 *	Returns a header and footer string of the specified type.
 ************************************************************************/
static char *
_pjGetPageHeaderString(PrintJob *pJob, PrintStringTypeEnum type)
{
    char *format,
         *buf = (char *) nullptr;

    switch (type)
    {
        case PRINT_NONE_STRING_TYPE_ENUM:
            buf = XtNewString(" ");
            break;
        case PRINT_DATE_STRING_TYPE_ENUM:
            {
		time_t	clock;
                char    *date;

		clock = time((time_t*) nullptr);
		date = ctime(&clock);
		/* Strip off the trailing newline. */
		date[strlen(date)-1] = '\0';
                format = GETMESSAGE(14, 8, "Date:  %s");
                buf = (char *) malloc(strlen(format) + strlen(date) + 1);
                if (buf != (char *) nullptr)
                  sprintf(buf, format, date);
            }
            break;
        case PRINT_DOCNAME_STRING_TYPE_ENUM:
            {
                unsigned	buflen;

                format = GETMESSAGE(14, 9, "Document:  %s");
                buflen = strlen(format) + strlen(pJob->documentName) + 1;
                buf = (char *) malloc(buflen);
                if (buf != (char *) nullptr)
                  sprintf(buf, format, pJob->documentName);
            }
            break;
        case PRINT_PAGE_NUMBER_STRING_TYPE_ENUM:
	    /*
             * Allocate space for the format and the translated page number.
             */
            {
                format = GETMESSAGE(14, 10, "Page %d of %d");
                buf = (char *) malloc(strlen(format) + 16);
                if (buf != (char *) nullptr)
                  sprintf(buf, format, pJob->npagesDone, pJob->npagesTotal);
            }
            break;
        case PRINT_USER_NAME_STRING_TYPE_ENUM:
            /*
             * Allocate space for the format and the username.
             */
            {
		struct passwd	*pw;
		const char	*user_name;

                format = GETMESSAGE(14, 11, "Document For:  %s");
		pw = getpwuid(getuid());
		user_name = (pw != nullptr && pw->pw_name != nullptr) ?
		    pw->pw_name : "";
	        buf = (char *) malloc(strlen(format) + strlen(user_name) + 1);
	        if (buf != (char *) nullptr)
	          sprintf(buf, format, user_name);
            }
            break;
        default:
            buf = XtNewString("DEFAULT not impld");
            break;
    }
    return buf;
}


/*
 *
 * Name: _pjFinishedPrintToFile
 *
 * Description:
 *
 *     App-specific print data holder allocate function.
 *
 */
#if 0 && defined(PRINTING_SUPPORTED)
static void _pjFinishedPrintToFile(
			Display		*display,
			XPContext	context,
			XPGetDocStatus	status,
			XPointer	client_data)
{
    char	*message = nullptr;
    PrintJob	*pJob = (PrintJob *) client_data;
    Editor	*pPad = pJob->pPad;

    if (status != XPGetDocFinished)
    {

	message = (char *) GETMESSAGE(
				14, 14,
				"'Print to File' was unsuccessful.");
	Warning(pPad, message, XmDIALOG_WARNING);
    }
    else if (display != PrintJobGetErrorPrintDisplay())
    {

	message = (char *) GETMESSAGE(
				14, 15,
				"'Print to File' completed successfully.");
	Warning(pPad, message, XmDIALOG_INFORMATION);
    }
    PrintJobDestroy(pJob);
}
#endif /* PRINTING_SUPPORTED */


/*
 * Name: _pjCancelCB
 * Description:
 *      An XtCallbackProc which can be added to the callback list of
 *      a widget to cancel the print job passed back as client_data.
 */
static void
_pjCancelCB (Widget widget, XtPointer client_data, XtPointer call_data)
{
    PrintJob *pJob = (PrintJob *) client_data;

    PrintJobDestroy(pJob);
}

/*
 * Name: _pjCloseDisplayCB
 * Description:
 *      An XtCallbackProc which can be added to the callback list of
 *      a widget to cancel the print job passed back as client_data.
 */
static void
_pjCloseDisplayCB (Widget widget, XtPointer client_data, XtPointer call_data)
{
    PrintJob *pJob = (PrintJob *) client_data;
    DtPrintSetupCallbackStruct *pbs = (DtPrintSetupCallbackStruct *) call_data;

    if (pJob->pShell != nullptr)
    {
	XtDestroyWidget(pJob->pShell);
	pJob->pShell = nullptr;
    }
    DtPrintFreeSetupData(pJob->printData);
}

/*
 * Name: _pjPrintCB
 * Description:
 *      An XtCallbackProc which can be added to the callback list of
 *      a widget to execute the print job passed back as client_data.
 */
static void
_pjPrintCB (Widget widget, XtPointer client_data, XtPointer call_data)
{
    PrintJob *pJob = (PrintJob *) client_data;
    DtPrintSetupCallbackStruct *pbs = (DtPrintSetupCallbackStruct *) call_data;

    DtPrintCopySetupData(pJob->printData, pbs->print_data);
    _pjCreatePrintShell(pJob);
    _pjDoPrint(pJob);
}


/*
 * Name: _pjPdmSetupCB
 * Description:
 *      An XtCallbackProc which can be added to the callback list of
 *      a widget to popup the PDM for the print job.
 */
static void
_pjPdmSetupCB(Widget print_setup, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    DtPrintPopupJobSetupDialog(print_setup, print_setup);
}


/************************************************************************
 * _pjPdmNotificationCB
 *	XmNpdmNotificationCallback for the XmPrintShell
 ************************************************************************/
static void
_pjPdmNotificationCB (Widget widget, XtPointer client_data, XtPointer call_data)
{
#if 0 && defined(PRINTING_SUPPORTED)
    PrintJob			*pJob = (PrintJob*) client_data;
    XmPrintShellCallbackStruct	*pscbs = (XmPrintShellCallbackStruct*)call_data;
    char			*message = nullptr;

    switch (pscbs->reason)
    {
    
        case XmCR_PDM_NONE:
        case XmCR_PDM_START_ERROR:
        case XmCR_PDM_START_VXAUTH:
        case XmCR_PDM_START_PXAUTH:
	    message = (char *) GETMESSAGE(
			14, 25,
			"Print Dialog Manager error - setup failed.");
	    break;
	default:
	    message = nullptr;
	    break;
    }

    if (message != nullptr)
      Warning( pJob->pPad, message, XmDIALOG_WARNING);
#endif  /* PRINTING_SUPPORTED */
}



/************************************************************************
 * _pjPrintOnePageCB
 *	XmNpageSetupCallback for the XmPrintShell
 ************************************************************************/
static void
_pjPrintOnePageCB(
		Widget widget,
		XtPointer client_data,
		XtPointer call_data
		)
{
#if 0 && defined(PRINTING_SUPPORTED)
    PrintJob	*pJob = (PrintJob *) client_data;

    XmPrintShellCallbackStruct  *pscbs = (XmPrintShellCallbackStruct*)call_data;
    int                         top = 0;

    if (pJob->pOutput == nullptr)
    {
        XtArgVal	width0, height0;
        Dimension	width, height;

        width = 0; height=0;
        XtVaGetValues(
                pJob->pShell,
                XmNwidth, &width0,
                XmNheight, &height0,
                nullptr);
        width = (Dimension)width0;
        height = (Dimension)height0;

#if defined(PRINT_TO_VIDEO)
        printf("PrintShell in _pjPrintOnePageCB: <W %d - H %d>\n",width,height);
#endif

	if (width < 100 || height < 100)
	{
            width = 2550; height=3250;
            XtVaSetValues(
                pJob->pShell,
                XmNwidth, width,
                XmNheight, height,
                nullptr);
            width = 0; height=0;
            XtVaGetValues(
                pJob->pShell,
                XmNwidth, &width,
                XmNheight, &height,
                nullptr);
#if defined(PRINT_TO_VIDEO)
            printf(
	      "PrintShell in _pjPrintOnePageCB: <W %d - H %d>\n",width,height);
#endif
	}
        _pjCreateOutputWidgets(pJob);
    }

#ifdef PRINT_TO_VIDEO
    if (pJob->npagesDone > 0 && pJob->npagesDone == pJob->npagesTotal)
#else
    if (pscbs->last_page)
#endif
    {
        DtPrintSetupData    *psd = pJob->printData;

        /*
         * This spool job is done.  Clean up.
	 * If this is a print to file job,
	 * clean up in the job finished callback.
         */
        if (psd->destination != DtPRINT_TO_FILE)
          PrintJobDestroy(pJob);
        return;
    }

    if (pJob->npagesDone > 0 && PrintOutputPageDown(pJob->pOutput) == FALSE)
    {
#ifndef PRINT_TO_VIDEO
        pscbs->last_page = TRUE;
#endif
        return;
    }

    /*
     * Update header and footer strings.
     */
    pJob->npagesDone++;
    _pjUpdatePageHeaders(
	pJob,
	PrintSetupGetHdrFtrSpec(pJob->pSetup, DTPRINT_OPTION_HEADER_LEFT),
	PrintSetupGetHdrFtrSpec(pJob->pSetup, DTPRINT_OPTION_HEADER_RIGHT),
	PrintSetupGetHdrFtrSpec(pJob->pSetup, DTPRINT_OPTION_FOOTER_LEFT),
	PrintSetupGetHdrFtrSpec(pJob->pSetup, DTPRINT_OPTION_FOOTER_RIGHT)
	);

    /*
     * Notify the user that we're printing
     */
    if (pJob->pPad != nullptr)
    {
        static char	buf[1024];
        char 		*format;

        format = GETMESSAGE(14, 22, "Printing %s: page %d of %d ...");
        sprintf(buf, format,
		pJob->documentName,
		pJob->npagesDone,
		pJob->npagesTotal);
        SetStatusMessage(pJob->pPad, buf);
    }

#ifndef PRINT_TO_VIDEO
    if (pJob->npagesDone >= pJob->npagesTotal)
      pscbs->last_page = TRUE;
#endif

#endif /* PRINTING_SUPPORTED */
}
