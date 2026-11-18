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
/*****************************************************************************
 **
 **   File:        Main.c
 **
 **   Project:     DT 3.0
 **
 **   Description: main Dtstyle program
 **
 **
 ****************************************************************************
 ************************************<+>*************************************/
/*$TOG: Main.c /main/16 1998/07/30 12:32:34 mgreess $*/
/* 
 * (c) Copyright 1997, The Open Group 
 */
/*
 * (c) Copyright 1996 Digital Equipment Corporation.
 * (c) Copyright 1990, 1993, 1996 Hewlett-Packard Company.
 * (c) Copyright 1996 International Business Machines Corp.
 * (c) Copyright 1996 Sun Microsystems, Inc.
 * (c) Copyright 1996 Novell, Inc. 
 * (c) Copyright 1996 FUJITSU LIMITED.
 * (c) Copyright 1996 Hitachi.
 */

/*+++++++++++++++++++++++++++++++++++++++*/
/* include files                         */
/*+++++++++++++++++++++++++++++++++++++++*/

#include <Dt/DtSigP.h>
#include "signal.h"
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#ifdef TIME
# include <time.h>
#endif
#ifndef NO_MESSAGE_CATALOG
# include <Dt/MsgCatP.h>
# define TRUE 1
# define FALSE 0
#endif
#include <locale.h>

/* #include <sys/dir.h> */
#include <X11/Xlib.h>
#include <Xm/MwmUtil.h>

#include <Xm/Xm.h>
#include <Xm/XmP.h>
#include <Xm/MessageB.h>

#include <Dt/Dt.h>
#include <Dt/GetDispRes.h>
#include <Dt/EnvControlP.h>
#include <Dt/Message.h>
#include <Dt/SessionM.h>
#include <Dt/UserMsg.h>
#include <Dt/Wsm.h>
#include <Dt/DtNlUtils.h>
#include <Dt/DtosP.h>
#include <Dt/Lock.h>

#include "MainWin.h"
#include "ColorMain.h"
#include "ColorFile.h"
#include "Resource.h"
#include "Protocol.h"
#include "SaveRestore.h"
#include "ColorPalette.h"
#include "Backdrop.h"
#include "Mouse.h"

void loadDatabase(void); // ColorMain.c
void ListenForWorkspaceChange(void); // Protocol.c


/*+++++++++++++++++++++++++++++++++++++++*/
/* include extern functions              */
/*+++++++++++++++++++++++++++++++++++++++*/
#include "Main.h"

/*+++++++++++++++++++++++++++++++++++++++*/
/* Local #defines                        */
/*+++++++++++++++++++++++++++++++++++++++*/
#define STYLE_LOCK "STYLE_LOCK"
#define SYS_FILE_SEARCH_PATH "DTPMSYSFILESEARCHPATH"
#define MAX_STR_LEN 128

/*+++++++++++++++++++++++++++++++++++++++*/
/* Internal Functions                    */
/*+++++++++++++++++++++++++++++++++++++++*/


static int ErrorHandler(
                        Display *disp,
                        XErrorEvent *event ) ;
static int IOErrorHandler(
                        Display *disp ) ;

static void ToolkitErrorHandler(
                        char *message) _X_NORETURN ;
static void errParentMap( 
                        Widget w,
                        XtPointer client_data,
                        XEvent *event,
                        Boolean *continue_to_dispatch) ;
static void UnmanageCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void DestroyCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static Boolean NewCreateD( XtPointer shell) ;
static Boolean NewAddTo( XtPointer shell) ;
static Boolean NewCreateTop1( XtPointer shell) ;
static Boolean NewCreateTop2( XtPointer shell) ;
static Boolean NewAddSysPath( XtPointer shell) ;
static Boolean NewAddDirectories( XtPointer shell) ;
static Boolean NewAddHomePath( XtPointer shell) ;
static Boolean NewReadPal( XtPointer shell) ;
static Boolean NewInitPal( XtPointer shell) ;
static Boolean NewAllocColor( XtPointer shell) ;
static Boolean NewBottomColor( XtPointer shell) ;
static Boolean NewCreateButtons( XtPointer shell) ;

static void ExitSignalHandler(int signal_number);
static void ExitSignalProc(XtPointer clientData, XtSignalId *id);
static XtSignalId exitSignalId = 0;
static volatile sig_atomic_t exitRequested = 0;
static void WaitChildDeath(int signal_number);

/*++++++++++++++++++++++++++++++++++++++*/
/* Internal Variables                   */
/*++++++++++++++++++++++++++++++++++++++*/
static XrmOptionDescRec option_list[] =
{
   {  "-session",  "session",    XrmoptionSepArg,  nullptr  },
};

/*++++++++++++++++++++++++++++++++++++++*/
/* Global Variables                     */
/*++++++++++++++++++++++++++++++++++++++*/
Style style;
char  *progName;

/*+++++++++++++++++++++++++++++++++++++++++++*/
/*Misc functions all the dialogs use         */
/*+++++++++++++++++++++++++++++++++++++++++++*/

/*+++++++++++++++++++++++++++++++++++++++++++*/
/* raise a dialog window above peer dialogs  */
/* Needs the SHELL of the dialog, not the    */
/* dialog box.                               */
/*+++++++++++++++++++++++++++++++++++++++++++*/
void 
raiseWindow(
        Window dialogWin )
{
  static int changeMask = CWStackMode;
  static XWindowChanges windowChanges = {0,0,0,0,0,0,Above};

    XReconfigureWMWindow(style.display, dialogWin,
      style.screenNum, changeMask, &windowChanges);
}


/************************************************************************
 * CenterMsgCB 
 *    - to be used with message dialogs (assumptions are being made that 
 *      parent is dialog shell, and child is bb, due to Xm hacks for them)
 *      (eg. it sets bb x,y to 0,0 and parents x,y to x,y set for bb)
 *    - parent for positioning only (may not be real parent)
 *    - use client_data for parent... if NULL, use style.errParent if ok,
 *    - or main style.shell (makes this routine more generally usefull)
 *
 ************************************************************************/
void 
CenterMsgCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    int n;
    Position newX, newY;
    Arg args[4];
    Widget   shell;

    /* figure out what to use as "visual" parent */
    shell = (Widget)client_data;
    if (shell == nullptr)
    {
        if (!style.errParent || !XtParent(style.errParent) || 
	    !XtIsManaged(style.errParent))
            shell = style.shell;
        else
            shell = XtParent(style.errParent);
    }
    else
	shell = XtParent(shell);

    /* calculate new x,y to be centered in visualParent */
    newX = XtX(shell) + XtWidth(shell)/2 - XtWidth(w)/2;
    newY = XtY(shell) + XtHeight(shell)/2 - XtHeight(w)/2;

    if (newX < 0) newX = 0;
    if (newY < 0) newY = 0;

    n = 0;
    XtSetArg(args[n], XmNx, newX); n++;
    XtSetArg(args[n], XmNy, newY); n++;
    XtSetValues(w, args, n);
}


/*************************************************************/
/* ErrDialog                                                 */
/* Put up an error dialog and block until the user clicks OK */
/* by default, there is no cancel or help button, but the    */
/* dialog is created with autoUnmanage false, and ok/cancel  */
/* callbacks to do the unmanage, so that a help button can   */
/* be used                                                   */
/*************************************************************/
void 
ErrDialog(
        char *errString,
        Widget visualParent )
{
    int           n;
    Arg           args[10];
    XmString      ok;

    /* create the compound string */
    style.tmpXmStr = CMPSTR(errString);

    style.errParent = visualParent;

    if (style.errDialog == nullptr)     /* create it */
    {
        ok = XmStringCreateLocalized((String) _DtOkString);

        n = 0;
        XtSetArg(args[n], XmNokLabelString, ok); n++;
        XtSetArg(args[n], XmNmessageString, style.tmpXmStr);                n++;
        XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC);                n++;
        XtSetArg (args[n], XmNautoUnmanage, False);                         n++;
        XtSetArg (args[n], XmNdefaultPosition, False);                      n++;
        style.errDialog = XmCreateErrorDialog(style.shell,"ErrorNotice",args,n);

        XtAddCallback (style.errDialog, XmNokCallback, UnmanageCB, nullptr);
        XtAddCallback (style.errDialog, XmNcancelCallback, UnmanageCB, nullptr);
        XtAddCallback (style.errDialog, XmNmapCallback, CenterMsgCB, nullptr);
        XtUnmanageChild ( XmMessageBoxGetChild (style.errDialog,
                                                XmDIALOG_CANCEL_BUTTON));
        XtUnmanageChild ( XmMessageBoxGetChild (style.errDialog,
                                                XmDIALOG_HELP_BUTTON));

        /* set the dialog shell parent title */
        n=0;
        XtSetArg (args[n], XmNmwmInputMode,
                        MWM_INPUT_PRIMARY_APPLICATION_MODAL); n++;
        XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
        XtSetArg (args[n], XmNtitle, ((char *)GETMESSAGE(2, 3, "Error")));   n++;
        XtSetValues (XtParent(style.errDialog), args, n);
    }
    else                 /* change the string */
    {
        n = 0;
        XtSetArg(args[n], XmNmessageString, style.tmpXmStr); n++;
        XtSetValues (style.errDialog, args, n);
    }

    /* free the compound string */
    XmStringFree (style.tmpXmStr);

    if (XtIsManaged(style.errParent) || XtParent(style.errParent) == nullptr)
    {
        XtManageChild(style.errDialog);
        /* ring the bell (PM behavior) */
        XBell(style.display, 0);
    }
    else
    {
        XtAddEventHandler(XtParent(style.errParent), StructureNotifyMask, 0,
                          errParentMap, nullptr);
    }

}



static const char *s_startupDialogName = nullptr;

static void
openStartupDialogByName(Widget shell, const char *name)
{
    if (!shell || !style.display)
        return;
    if (XtIsRealized(shell))
        XMapRaised(style.display, XtWindow(shell));
    if (!name || name[0] == '\0' || strncmp(name, "Main", 4) == 0)
        return;
    if (strncmp(name, "Display", 7) == 0) {
        extern void popup_displayBB(Widget);
        popup_displayBB(shell);
    } else if (strncmp(name, "Backdrop", 8) == 0) {
        BackdropDialog(shell);
    } else if (strncmp(name, "Mouse", 5) == 0 || strncmp(name, "Touchpad", 8) == 0) {
        popup_mouseBB(shell);
    } else if (strncmp(name, "Color", 5) == 0) {
        Customize(shell);
    }
}

static void
openDisplayDeferredCB(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    openStartupDialogByName((Widget)client_data, s_startupDialogName);
}

static void
openDialogPropEH(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)cont;
    if (!ev || ev->type != PropertyNotify || ev->xproperty.state != PropertyNewValue)
        return;
    Atom open_a = XInternAtom(style.display, "_DTSTYLE_OPEN_DIALOG", False);
    if (ev->xproperty.atom != open_a)
        return;
    Atom act_t = None;
    int act_f = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(style.display, XtWindow(style.shell), open_a, 0, 16, True,
                           XA_STRING, &act_t, &act_f, &nitems, &rem, &data) == Success && data) {
        openStartupDialogByName(style.shell, (const char *)data);
        XFree(data);
    }
}

static void 
errParentMap(
        Widget w,
        XtPointer client_data,
        XEvent *event,
        Boolean *continue_to_dispatch )
{
    if (event->type == MapNotify)
    {
        XtManageChild(style.errDialog);
        /* ring the bell (PM behavior) */
        XBell(style.display, 0);
        XtRemoveEventHandler(XtParent(style.errParent), StructureNotifyMask,
                             0, errParentMap, nullptr);
    }
}

/*********************************************************/
/* InfoDialog                                            */
/* Put up a modeless info dialog.                        */
/* There is no cancel or help button.                    */
/* Dialog is created with autoUnmanage true.             */
/* An ok callback is added which will destroy the dialog */
/* and optionally unmap the parent.                      */
/*********************************************************/
void
InfoDialog(
        char *infoString,
        Widget parent,
        Boolean unmapParent )
{
    int             n;
    Arg             args[10];
    static XmString ok = nullptr;
    Widget          w;

    /* create the compound string */
    style.tmpXmStr = CMPSTR(infoString);

    if (ok == nullptr)
	ok = CMPSTR((String) _DtOkString);

    /* create it */
    n = 0;
    XtSetArg(args[n], XmNokLabelString, ok);                            n++;
    XtSetArg(args[n], XmNmessageString, style.tmpXmStr);                n++;
    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_MODELESS);               n++;
    XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC);                n++;
    w = XmCreateInformationDialog(parent, "Notice", args, n);

    if (unmapParent)
        XtAddCallback (w, XmNokCallback, DestroyCB, parent);
    else
        XtAddCallback (w, XmNokCallback, DestroyCB, nullptr);
    XtUnmanageChild ( XmMessageBoxGetChild(w, XmDIALOG_CANCEL_BUTTON) );
    XtUnmanageChild ( XmMessageBoxGetChild(w, XmDIALOG_HELP_BUTTON) );

    /* set the dialog shell parent title */
    n=0;
    XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
    XtSetArg (args[n], XmNtitle, ((char *)GETMESSAGE(2, 2, "Notice")));   n++;
    XtSetValues (XtParent(w), args, n);

    /* free the compound string */
    XmStringFree (style.tmpXmStr);

    /* manage the info dialog */
    XtManageChild(w);

}


/*++++++++++++++++++++++++++++++++++++++*/
/* UnmanageCB                           */
/*++++++++++++++++++++++++++++++++++++++*/
static void 
UnmanageCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  XtUnmanageChild(w);
}


/*++++++++++++++++++++++++++++++++++++++*/
/* DestroyCB                            */
/*++++++++++++++++++++++++++++++++++++++*/
static void 
DestroyCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  XtDestroyWidget(XtParent(w));
  if (client_data != nullptr)
      XtUnmanageChild((Widget)client_data); 
}


/**********************************************************/
/* putDialog                                              */
/* move a dialog up so it isn't covering the main window  */
/* Or down if there is no room up.                        */
/* note: "parent" needs to have valid x,y information...  */
/*       ex: a child of dialog shell doesn't, its parent  */
/*       does, so the parent shell would be passed in     */
/**********************************************************/
void 
putDialog(
        Widget parent,
        Widget dialog )
{
  int n;
  Position newX, newY, pY, pX;
  Dimension pHeight, myHeight, pWidth, myWidth;
  Arg args[4];

    pX = XtX(parent);
    pY = XtY(parent);
    pHeight = XtHeight(parent);
    pWidth = XtWidth(parent);
    myHeight = XtHeight(dialog);
    myWidth = XtWidth(dialog);

    if ((newY = pY - myHeight +5) < 32) 
        newY = pY + pHeight;
    if (newY < 32)
        newY = 32;
    newX = pX + (Position)pWidth/2 - (Position)myWidth/2;
    if (newX < 12)
        newX = 12;

    n = 0;
    XtSetArg(args[n], XmNx, newX); n++;
    XtSetArg(args[n], XmNy, newY); n++;
    XtSetValues(dialog,args,n);

#ifdef PutDDEBUG
  printf("newX, newY, pY, pX;\n");
  printf("%d    %d    %d  %d\n",newX, newY, pY, pX);
  printf("pHeight, myHeight, pWidth, myWidth;\n");
  printf("%d       %d        %d      %d\n", pHeight, myHeight, pWidth, myWidth);
#endif
}


/*++++++++++++++++++++++++++++++++++++++*/
/* main                                 */
/*++++++++++++++++++++++++++++++++++++++*/
int 
main(
        int argc,
        char **argv )
{
    int             n;
    Arg             args[MAX_ARGS];
    XEvent          event;
    XPropertyEvent *pEvent=(XPropertyEvent *)&event;
    long            mwmFunc;
    Boolean         useMaskRtn, useIconFileCacheRtn;    
    char           *dirs = nullptr;
    char           *string;
	Visual         *visual;

#ifdef USERHELP
malloc_check(1);
malloc_trace(0);
#endif

    XtSetLanguageProc(nullptr, nullptr, nullptr);
    _DtEnvControl(DT_ENV_SET); 

    Boolean openDisplayDialogOnStart = False;
    Boolean applyTouchpadCli = False;
    int cliTp[5] = { 1, 0, 1, 1, 0 };
    {
        int out_argc = 1;
        for (int ai = 1; ai < argc; ++ai) {
            if (strcmp(argv[ai], "-display_dialog") == 0 ||
                strcmp(argv[ai], "--display-dialog") == 0 ||
                strcmp(argv[ai], "-monitors") == 0 ||
                strcmp(argv[ai], "--monitors") == 0) {
                openDisplayDialogOnStart = True;
                s_startupDialogName = "Display";
                continue;
            }
            if (strcmp(argv[ai], "-backdrop") == 0 ||
                strcmp(argv[ai], "--backdrop") == 0) {
                openDisplayDialogOnStart = True;
                s_startupDialogName = "Backdrop";
                continue;
            }
            if (strcmp(argv[ai], "-mouse") == 0 ||
                strcmp(argv[ai], "--mouse") == 0 ||
                strcmp(argv[ai], "-touchpad") == 0 ||
                strcmp(argv[ai], "--touchpad") == 0) {
                openDisplayDialogOnStart = True;
                s_startupDialogName = "Mouse";
                continue;
            }
            if (strcmp(argv[ai], "-color") == 0 ||
                strcmp(argv[ai], "--color") == 0) {
                openDisplayDialogOnStart = True;
                s_startupDialogName = "Color";
                continue;
            }
            if ((strcmp(argv[ai], "-apply-touchpad") == 0 ||
                 strcmp(argv[ai], "--apply-touchpad") == 0) &&
                ai + 5 < argc) {
                applyTouchpadCli = True;
                for (int ti = 0; ti < 5; ++ti)
                    cliTp[ti] = atoi(argv[ai + 1 + ti]) ? 1 : 0;
                ai += 5;
                continue;
            }
            argv[out_argc++] = argv[ai];
        }
        argv[out_argc] = nullptr;
        argc = out_argc;
    }

    /* Initialize the toolkit and open the display */
    style.shell = 
        XtInitialize(argv[0], XMCLASS, option_list, 1, (int *)&argc, argv);

    /* Registered before the INT and TERM handlers are installed, so the
     * handlers can always defer the exit to the event loop. */
    exitSignalId = XtAppAddSignal(XtWidgetToApplicationContext(style.shell),
                                  ExitSignalProc, nullptr);

    /* Allow all WS manipulation functions except resize and maximize */
    mwmFunc = MWM_FUNC_ALL ^ (MWM_FUNC_RESIZE | MWM_FUNC_MAXIMIZE); 

    n = 0;
    XtSetArg(args[n], XmNmwmFunctions, mwmFunc); n++;
    XtSetArg(args[n], XmNuseAsyncGeometry, True); n++;
    XtSetValues(style.shell, args, n);

    /* initialize global style data */

    style.display    = XtDisplay(style.shell);
    style.screen     = DefaultScreenOfDisplay(style.display);
    style.screenNum  = DefaultScreen(style.display);
    style.colormap   = DefaultColormap(style.display, style.screenNum);
    style.root       = DefaultRootWindow(style.display);
    style.execName   = argv[0];
    style.errDialog  = nullptr;
    style.tmpXmStr   = nullptr;
    {
        const char *home = getenv("HOME");

        if (home == nullptr)
            home = "";
        style.home = (char *) XtMalloc(strlen(home) + 1);
        strcpy(style.home, home);
    }
    style.colorDialog = nullptr;
    style.backdropDialog = nullptr;
    style.fontDialog = nullptr;
    style.kbdDialog = nullptr;
    style.mouseDialog = nullptr;
    style.audioDialog = nullptr;
    style.screenDialog = nullptr;
    style.displayDialog = nullptr;
    style.startupDialog = nullptr;
    style.dtwmDialog = nullptr;
    style.i18nDialog = nullptr;
	visual = XDefaultVisual(style.display,style.screenNum);
	style.visualClass = visual->class;
    
    if (progName = DtStrrchr(argv[0], '/')) progName++;
    else progName = argv[0];

    if (applyTouchpadCli)
    {
        InitDtstyleProtocol();
        ApplyAndPersistTouchpadCLI(cliTp[0], cliTp[1], cliTp[2], cliTp[3], cliTp[4]);
        XSync(style.display, False);
        return 0;
    }

    /* Get the lock established to ensure only one dtstyle process
     * is running per screen .. first malloc enough space*/

    if (_DtGetLock (style.display, STYLE_LOCK) == 0)
    {
        {
            Atom win_a = XInternAtom(style.display, "_DTSTYLE_SHELL_WIN", True);
            Atom open_a = XInternAtom(style.display, "_DTSTYLE_OPEN_DIALOG", False);
            Atom act_t = None;
            int act_f = 0;
            unsigned long nitems = 0, rem = 0;
            unsigned char *pdata = nullptr;
            if (win_a != None &&
                XGetWindowProperty(style.display, style.root, win_a, 0, 1, False,
                                   XA_WINDOW, &act_t, &act_f, &nitems, &rem, &pdata) == Success &&
                pdata && nitems >= 1) {
                Window target_w = *(Window *)pdata;
                const char *dlg_name = s_startupDialogName ? s_startupDialogName : "Main";
                XChangeProperty(style.display, target_w, open_a, XA_STRING, 8,
                                PropModeReplace, (const unsigned char *)dlg_name,
                                (int)strlen(dlg_name));
                XFree(pdata);
                XSync(style.display, False);
                exit(0);
            }
            if (pdata) XFree(pdata);
        }
        _DtSimpleError (progName, DtError, nullptr, "%s",
	     ((char *)GETMESSAGE(2, 5, "Style Manager is already running, second attempt aborted.")));
        exit(1);
    }

    InitDtstyleProtocol();
    SetWindowProperties();

    /* Register error handlers */
    XSetErrorHandler(ErrorHandler);
    XSetIOErrorHandler(IOErrorHandler);
    XtAppSetErrorHandler(XtWidgetToApplicationContext(style.shell),
                         ToolkitErrorHandler);
    XtAddEventHandler(style.shell, StructureNotifyMask, 0,
                         MwmReparentNotify, nullptr);

    /* set up resolution dependent layout variables */
    switch (_DtGetDisplayResolution(style.display, style.screenNum))
    {
        case LOW_RES_DISPLAY:
            style.horizontalSpacing = 
            style.verticalSpacing = 3;
            break;
            
        case MED_RES_DISPLAY:
            style.horizontalSpacing = 
            style.verticalSpacing = 5;
            break;
            
        case HIGH_RES_DISPLAY:
            style.horizontalSpacing = 
            style.verticalSpacing = 8;
            break;
    }
    
    GetApplicationResources();
    
    XmeGetIconControlInfo(style.screen, &useMaskRtn,
			  &style.useMultiColorIcons, &useIconFileCacheRtn);
    
    
    /* add the directory $HOME/.dt/backdrops */
    
    string = (char *)XtMalloc(strlen(style.home) + strlen("/.dt/backdrops:") + 1);
    sprintf(string, "%s/.dt/backdrops:", style.home);

    dirs = (char *)XtCalloc(1, strlen("/etc/dt/backdrops:/usr/dt/backdrops") + 
			    (style.xrdb.backdropDir == nullptr ? 2 :
			    strlen(style.xrdb.backdropDir)) + 
			    strlen(string) + 2);
  

    strcpy(dirs, string);
    if (style.xrdb.backdropDir) 
      {
	strcat(dirs, style.xrdb.backdropDir);
	strcat(dirs, ":"); 
      }
    strcat(dirs, "/etc/dt/backdrops:/usr/dt/backdrops");

    _DtWsmSetBackdropSearchPath(style.screen, dirs, style.useMultiColorIcons);

    XtFree((char *)string);
    
    XtFree((char *)dirs);
    
    

    style.count = 0;
    /* if this is started from save session we need to set up the BMS
       first, otherwise do it after making the window. (for user perception
       for how long it takes for the dtstyle to come up) */
    if(style.xrdb.session != nullptr) {
      DtInitialize (style.display, style.shell, progName, progName);
      /*Restore a session or build and display the main Window.*/
      if(!restoreSession(style.shell,style.xrdb.session))
	init_mainWindow(style.shell);
    }
    else {
      init_mainWindow(style.shell);
      DtInitialize (style.display, style.shell, progName, progName);
      InitializeAtoms();
      CheckMonitor(style.shell);
      GetDefaultPal(style.shell);
    }
    
    _DtSignal(SIGINT, ExitSignalHandler);
    _DtSignal(SIGTERM, ExitSignalHandler);

    /* to avoid defunct screen saver processes */    
    _DtSignal(SIGCHLD, WaitChildDeath);

    /* backdrop dialog  needs to know when the workspace changes to recolor 
       the bitmap displayed in the dialog */
    ListenForWorkspaceChange();

    if (XtIsRealized(style.shell)) {
        Window sw = XtWindow(style.shell);
        Atom win_a = XInternAtom(style.display, "_DTSTYLE_SHELL_WIN", False);
        XChangeProperty(style.display, style.root, win_a, XA_WINDOW, 32,
                        PropModeReplace, (const unsigned char *)&sw, 1);
        XtAddEventHandler(style.shell, PropertyChangeMask, False, openDialogPropEH, nullptr);
    }

    if (openDisplayDialogOnStart) {
        XtAppAddTimeOut(XtWidgetToApplicationContext(style.shell), 180UL,
                        openDisplayDeferredCB, (XtPointer)style.shell);
    }

    /* if using COLOR builtin, style.workProcs is True */

    if ((XmeUseColorObj() != FALSE) && style.workProcs)

        XtAppAddWorkProc(XtWidgetToApplicationContext(style.shell), 
                        NewCreateD, style.shell);

    XtAppMainLoop(XtWidgetToApplicationContext(style.shell));

    return 0;
}

/************************************************************************
 *
 *  ErrorHandler
 *
 ************************************************************************/
static int
ErrorHandler(
        Display *disp,
        XErrorEvent *event )

{
  #define _DTSTYLE_BUFSIZE 1024
  char errmsg[_DTSTYLE_BUFSIZE];

  _DtPrintDefaultErrorSafe(disp, event, errmsg, _DTSTYLE_BUFSIZE);
  _DtSimpleError(progName, DtWarning, nullptr, errmsg, nullptr);

   /* We do not want to exit here lets try to continue... */
  return 1;
}


/************************************************************************
 *
 *  IOErrorHandler
 *
 ************************************************************************/
static int
IOErrorHandler (Display *display)
{
#ifdef DEBUG
    Warning ("X IO error occurred during generic operation");
#endif /* DEBUG */

    exit (1);
    return 1;

} 


/************************************************************************
 *
 *  ToolkitErrorHandler
 * 
 *  All Xt memory allocation errors should fall through to this routine.
 *  There is no need to check for Xtmalloc errors where they are used.
 *
 ************************************************************************/
static void
ToolkitErrorHandler(
        char *message )

{
    if (message)
        _DtSimpleError(progName, DtError, nullptr, message, nullptr);
    _DtSimpleError (progName, DtError, nullptr, 
        GETMESSAGE(2, 6, "An X Toolkit error occurred... Exiting.\n"));
    exit (1);
}


static Boolean 
NewCreateD(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /*  Create the Dialog Box Dialog */
    CreateDialogBoxD((Widget)shell);

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("CreateDialogBoxD elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewAddTo, shell);

    return(True);
}

static Boolean 
NewAddTo(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /*  Create the Dialog Box Dialog */
    AddToDialogBox();

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("AddToDialogBox elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewCreateTop1, shell);

    return(True);
}

static Boolean 
NewCreateTop1(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* Create the top portion of the color dialog */
    CreateTopColor1();

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("CreateTopColor1 elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewCreateTop2, shell);
    return(True);
}

static Boolean 
NewCreateTop2(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* Create the top portion of the color dialog */
    CreateTopColor2();

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("CreateTopColor2 elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewAddSysPath, shell);

    return(True);
}

static Boolean 
NewAddSysPath(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* initialize the system directory */
    AddSystemPath();

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("AddSystemPath elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewAddDirectories, shell);

    return(True);
}

static Boolean 
NewAddDirectories(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* initialize the directorys list in the resource */
    if(style.xrdb.paletteDir != nullptr)
      AddDirectories(style.xrdb.paletteDir);
    else
      style.count++;

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("AddDirectories elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewAddHomePath, shell);

    return(True);
}

static Boolean 
NewAddHomePath(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* initialize the home directory */
    AddHomePath();

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("AddHomePath elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewReadPal, shell);

    return(True);
}

static Boolean 
NewReadPal(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* Read in the palettes one at a time */
    if (ReadPaletteLoop(True)) {

#ifdef TIME
       gettimeofday(&second, &tzp);
       if(first.tv_usec > second.tv_usec){
          second.tv_usec += 1000000;
          second.tv_sec--;
       }
       printf("ReadPaletteLoop DONE elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    /* Stop work procs if no palettes found - fatal color error.
     * When the color button is pressed, the regular processing
     * will pick up on NumOfPalettes == 0 and post an error
     * dialog */
     
       if (NumOfPalettes == 0)
            return(True);
       loadDatabase();
       XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewInitPal, shell);
       return(True);
    }

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("ReadPaletteLoop NOT DONE elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif
      
    return(False);

}

static Boolean 
NewInitPal(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* initialize the palette list */
    if(InitializePaletteList((Widget)shell, paletteList, True)) {

#ifdef TIME
       gettimeofday(&second, &tzp);
       if(first.tv_usec > second.tv_usec){
          second.tv_usec += 1000000;
          second.tv_sec--;
       }
       printf("InitializePaletteList DONE elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

       XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewAllocColor, shell);
       return(True);
     }

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("InitializePaletteList NOT DONE elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    return(False);
}

static Boolean 
NewAllocColor(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* initialize pixels to the right color */
    AllocatePaletteCells((Widget)shell);

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("AllocatePaletteCells elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewBottomColor, shell);

    return(True);
}

static Boolean 
NewBottomColor(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* Create the bottom portion of the color dialog */
    CreateBottomColor();

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("CreateBottomColor elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    XtAppAddWorkProc(XtWidgetToApplicationContext((Widget) shell), NewCreateButtons, shell);

    return(True);
}

static Boolean 
NewCreateButtons(
        XtPointer shell )
{
#ifdef TIME
    struct timeval first, second, lapsed;
    struct timezone tzp;

    gettimeofday(&first, &tzp);
#endif

    /* initialize pixels to the right color */
    CreatePaletteButtons(style.buttonsForm);

#ifdef TIME
    gettimeofday(&second, &tzp);
    if(first.tv_usec > second.tv_usec){
       second.tv_usec += 1000000;
       second.tv_sec--;
    }
    printf("CreatePaletteButtons elapsed time is %ld seconds, %ld microseconds\n", second.tv_sec - first.tv_sec, second.tv_usec - first.tv_usec);
#endif

    return(True);
}

/*************************************<->*************************************
 *
 *  WaitChildDeath()
 *
 *
 *  Description:
 *  -----------
 *  When a SIGCHLD signal comes in, wait for all child processes to die.
 *
 *
 *  Inputs:
 *  ------
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
/* Runs from the event loop after ExitSignalHandler() called
 * XtNoticeSignal(), so the Motif exit callback is not run in signal context. */
static void
ExitSignalProc(XtPointer clientData, XtSignalId *id)
{
  (void) clientData;
  (void) id;
  activateCB_exitBtn(nullptr, nullptr, nullptr);
}

/* The signal handler may call only async-signal-safe functions. */
static void
ExitSignalHandler(int signal_number)
{
  int savedErrno = errno;

  (void) signal_number;
  if (exitRequested)
    _exit(0);
  exitRequested = 1;
  XtNoticeSignal(exitSignalId);
  errno = savedErrno;
}

/* Only async-signal-safe functions are called here. */
static void
WaitChildDeath(int signal_number)
{
  int   stat_loc;
  int   savedErrno = errno;

  (void) signal_number;
  while (waitpid(-1, &stat_loc, WNOHANG) > 0)
    ;
  errno = savedErrno;
}
