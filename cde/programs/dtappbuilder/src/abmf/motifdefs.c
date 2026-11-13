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
 *	$XConsortium: motifdefs.c /main/3 1995/11/06 18:09:19 rswiston $
 *
 *	@(#)motifdefs.c	1.13 02 May 1995	cde_app_builder/src/abmf
 *
 * 	RESTRICTED CONFIDENTIAL INFORMATION:
 *	
 *	The information in this document is subject to special
 *	restrictions in a confidential disclosure agreement between
 *	HP, IBM, Sun, USL, SCO and Univel.  Do not distribute this
 *	document outside HP, IBM, Sun, USL, SCO, or Univel without
 *	Sun's specific written approval.  This document and all copies
 *	and derivative works thereof must be returned or destroyed at
 *	Sun's request.
 *
 *	Copyright 1993 Sun Microsystems, Inc.  All rights reserved.
 *
 */


/*
 *  motifdefs.c
 */

#include "motifdefsP.h"
#include <ab_private/objxm.h>

/*************************************************************************
**                                                                      **
**       Constants (#define and const)					**
**                                                                      **
**************************************************************************/

/*************************************************************************
**                                                                      **
**       Private Functions (C declarations and macros)			**
**                                                                      **
**************************************************************************/

/*************************************************************************
**                                                                      **
**       Data     	                                                **
**                                                                      **
**************************************************************************/

/*************************************************************************
**                                                                      **
**       Function Definitions                                           **
**                                                                      **
**************************************************************************/


/*
 * Widget class names
 */
ISTRING	abmfP_abPopupMenu = nullptr;
ISTRING	abmfP_abPopupMenu2 = nullptr;
ISTRING	abmfP_abPulldownMenu = nullptr;
ISTRING	abmfP_abPulldownMenu2 = nullptr;
ISTRING	abmfP_dtComboBox = nullptr;
ISTRING	abmfP_dtEditor = nullptr;
ISTRING	abmfP_dtHelpDialog = nullptr;
ISTRING	abmfP_dtHelpQuickDialog = nullptr;
ISTRING	abmfP_dtMenuButton = nullptr;
ISTRING	abmfP_dtSpinBox = nullptr;
ISTRING	abmfP_dtTerm = nullptr;
ISTRING	abmfP_dtTermPrim = nullptr;
ISTRING	abmfP_xmArrowButton = nullptr;
ISTRING	abmfP_xmBulletinBoard = nullptr;
ISTRING	abmfP_xmCascadeButton = nullptr;
ISTRING	abmfP_xmCommand = nullptr;
ISTRING	abmfP_xmDialogShell = nullptr;
ISTRING abmfP_xmDragOverShell = nullptr;
ISTRING	abmfP_xmDrawingArea = nullptr;
ISTRING	abmfP_xmDrawnButton = nullptr;
ISTRING	abmfP_xmFileSelectionBox = nullptr;
ISTRING	abmfP_xmForm = nullptr;
ISTRING	abmfP_xmFrame = nullptr;
ISTRING	abmfP_xmLabel = nullptr;
ISTRING	abmfP_xmList = nullptr;
ISTRING	abmfP_xmMainWindow = nullptr;
ISTRING	abmfP_xmManager = nullptr;
ISTRING	abmfP_xmMenuShell = nullptr;
ISTRING	abmfP_xmMessageBox = nullptr;
ISTRING	abmfP_xmPanedWindow = nullptr;
ISTRING	abmfP_xmPrimitive = nullptr;
ISTRING	abmfP_xmPushButton = nullptr;
ISTRING	abmfP_xmRowColumn = nullptr;
ISTRING abmfP_xmSash = nullptr;
ISTRING	abmfP_xmScale = nullptr;
ISTRING	abmfP_xmScrollbar = nullptr;
ISTRING	abmfP_xmScrolledWindow = nullptr;
ISTRING	abmfP_xmSelectionBox = nullptr;
ISTRING	abmfP_xmSeparator = nullptr;
ISTRING abmfP_xmTearOffButton = nullptr;
ISTRING	abmfP_xmText = nullptr;
ISTRING	abmfP_xmTextField = nullptr;
ISTRING	abmfP_xmToggleButton = nullptr;
ISTRING	abmfP_xtApplicationShell = nullptr;
ISTRING	abmfP_xtComposite = nullptr;
ISTRING	abmfP_xtConstraint = nullptr;
ISTRING	abmfP_xtCore = nullptr;
ISTRING	abmfP_xtOverrideShell = nullptr;
ISTRING	abmfP_xtShell = nullptr;
ISTRING	abmfP_xtTopLevelShell = nullptr;
ISTRING	abmfP_xtTransientShell = nullptr;
ISTRING	abmfP_xtVendorShell = nullptr;
ISTRING	abmfP_xtWMShell = nullptr;

/* backwards compatibitility (inconsistent names) */
ISTRING	abmfP_applicationShell = nullptr;
ISTRING	abmfP_topLevelShell = nullptr;
ISTRING	abmfP_xmComboBox = nullptr;
ISTRING	abmfP_xmMenuButton = nullptr;
ISTRING	abmfP_xmSpinBox = nullptr;
ISTRING	abmfP_xmTerm = nullptr;
ISTRING	abmfP_xmTermPrim = nullptr;


/*
 * Resources
 */
ISTRING	abmfP_XmNheight = nullptr;
ISTRING	abmfP_XmNlabelString = nullptr;
ISTRING	abmfP_XmNset = nullptr;


int 
abmfP_motifdefs_init(void)
{
    static BOOL	inited= FALSE;
    if (inited)
    {
	return 0;
    }
    inited= TRUE;

    /*
     * Widget class names.  The _<widget> string variables are declared as part
     * of libABobjXm.
     *
     * abPulldownMenu, and abPulldownMenu2 are equivalent. It's a minor bug
     * that we have two.
     */
    abmfP_abPopupMenu        = istr_const("xmPopupMenuWidgetClass");
    abmfP_abPopupMenu2       = istr_const("xmPopupMenuPseudoClass");
    abmfP_abPulldownMenu     = istr_const("xmPulldownMenuWidgetClass");
    abmfP_abPulldownMenu2    = istr_const("xmPulldownMenuPseudoClass");
    abmfP_dtComboBox         = istr_const(_dtComboBox);
    abmfP_dtEditor           = istr_const("dtEditorWidgetClass");
    abmfP_dtHelpDialog       = istr_const("dtHelpDialogWidgetClass");
    abmfP_dtHelpQuickDialog  = istr_const("dtHelpQuickDialogWidgetClass");
    abmfP_dtMenuButton 	     = istr_const(_dtMenuButton);
    abmfP_dtSpinBox 	     = istr_const(_dtSpinBox);
    abmfP_dtTerm 	     = istr_const(_dtTerm);
    abmfP_dtTermPrim 	     = istr_const("dtTermPrimitiveWidgetClass");
    abmfP_xmArrowButton	     = istr_const(_xmArrowButton);
    abmfP_xmBulletinBoard    = istr_const(_xmBulletinBoard);
    abmfP_xmCascadeButton    = istr_const(_xmCascadeButton);
    abmfP_xmCommand 	     = istr_const(_xmCommand);
    abmfP_xmDialogShell      = istr_const(_xmDialogShell);
    abmfP_xmDragOverShell    = istr_const("xmDragOverShellWidgetClass");
    abmfP_xmDrawingArea      = istr_const(_xmDrawingArea);
    abmfP_xmDrawnButton      = istr_const(_xmDrawnButton);
    abmfP_xmFileSelectionBox = istr_const(_xmFileSelectionBox);
    abmfP_xmForm 	     = istr_const(_xmForm);
    abmfP_xmFrame 	     = istr_const(_xmFrame);
    abmfP_xmLabel 	     = istr_const(_xmLabel);
    abmfP_xmList 	     = istr_const(_xmList);
    abmfP_xmMainWindow 	     = istr_const(_xmMainWindow);
    abmfP_xmManager	     = istr_const(_xmManager);
    abmfP_xmMenuShell 	     = istr_const(_xmMenuShell);
    abmfP_xmMessageBox	     = istr_const(_xmMessageBox);
    abmfP_xmPanedWindow      = istr_const(_xmPanedWindow);
    abmfP_xmPrimitive 	     = istr_const(_xmPrimitive);
    abmfP_xmPushButton 	     = istr_const(_xmPushButton);
    abmfP_xmRowColumn 	     = istr_const(_xmRowColumn);
    abmfP_xmSash	     = istr_const("xmSashWidgetClass");
    abmfP_xmScale 	     = istr_const(_xmScale);
    abmfP_xmScrollbar	     = istr_const(_xmScrollBar);
    abmfP_xmScrolledWindow   = istr_const(_xmScrolledWindow);
    abmfP_xmSelectionBox     = istr_const(_xmSelectionBox);
    abmfP_xmSeparator 	     = istr_const(_xmSeparator);
    abmfP_xmTearOffButton    = istr_const("xmTearOffButtonWidgetClass");
    abmfP_xmText 	     = istr_const(_xmText);
    abmfP_xmTextField 	     = istr_const(_xmTextField);
    abmfP_xmToggleButton     = istr_const(_xmToggleButton);
    abmfP_xtApplicationShell = istr_const("applicationShellWidgetClass");
    abmfP_xtComposite	     = istr_const("compositeWidgetClass");
    abmfP_xtConstraint	     = istr_const("constraintWidgetClass");
    abmfP_xtCore	     = istr_const("coreWidgetClass");
    abmfP_xtOverrideShell    = istr_const("overrideShellWidgetClass");
    abmfP_xtShell	     = istr_const("shellWidgetClass");
    abmfP_xtTopLevelShell    = istr_const("topLevelShellWidgetClass");
    abmfP_xtTransientShell   = istr_const("transientShellWidgetClass");
    abmfP_xtVendorShell	     = istr_const("vendorShellWidgetClass");
    abmfP_xtWMShell	     = istr_const("wmShellWidgetClass");

    /* backwards compatibitility (inconsistent names) */
    abmfP_applicationShell   = istr_dup(abmfP_xtApplicationShell);
    abmfP_topLevelShell      = istr_dup(abmfP_xtTopLevelShell);
    abmfP_xmComboBox	     = istr_dup(abmfP_dtComboBox);
    abmfP_xmMenuButton       = istr_dup(abmfP_dtMenuButton);
    abmfP_xmSpinBox	     = istr_dup(abmfP_dtSpinBox);
    abmfP_xmTerm	     = istr_dup(abmfP_dtTerm);
    abmfP_xmTermPrim	     = istr_dup(abmfP_dtTermPrim);


    /*
     * Resources
     */
    abmfP_XmNheight = istr_const("XmNheight");
    abmfP_XmNlabelString = istr_const("XmNlabelString");
    abmfP_XmNset = istr_const("XmNset");

    return 0;
}

