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
/* $XConsortium: xmwidgets.c /main/6 1996/04/01 17:54:38 rswiston $ */

/*	Copyright (c) 1991, 1992 UNIX System Laboratories, Inc. */
/*	All Rights Reserved     */

/*	THIS IS UNPUBLISHED PROPRIETARY SOURCE CODE OF          */
/*	UNIX System Laboratories, Inc.			        */
/*	The copyright notice above does not evidence any        */
/*	actual or intended publication of such source code.     */

#include	"name.h" 
#include	"shell.h" 
#undef access
#include <X11/Xauth.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/StringDefs.h>

#include <Xm/ArrowB.h>
#include <Xm/ArrowBG.h>
#include <Xm/BulletinB.h>
#include <Xm/CascadeB.h>
#include <Xm/CascadeBG.h>
#include <Xm/ComboBox.h>
#include <Xm/Command.h>
#include <Xm/DialogS.h>
#include <Xm/DrawingA.h>
#include <Xm/DrawnB.h>
#include <Xm/FileSB.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Label.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/MainW.h>
#include <Xm/MenuShell.h>
#include <Xm/MessageB.h>
#include <Xm/PanedW.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/SashP.h>
#include <Xm/Scale.h>
#include <Xm/ScrollBar.h>
#include <Xm/ScrolledW.h>
#include <Xm/SelectioB.h>
#include <Xm/Separator.h>
#include <Xm/SeparatoG.h>
#include <Xm/SSpinB.h>
#include <Xm/TearOffBP.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <Xm/ToggleBG.h>

#include <Dt/HelpDialog.h>
#include <Dt/HelpQuickD.h>
#include <Dt/Print.h>

#include "dtksh.h"

#include "xmksh.h"
#include "dtkcmds.h"
#include "widget.h"
#include "xmwidgets.h"
#include "msgs.h"




static discInfo * setArrowDisciplines( void ) ;
static discInfo * setComboDisciplines( void ) ;
static discInfo * setCmdDisciplines( void ) ;
static discInfo * setDAreaDisciplines( void ) ;
static discInfo * setDBtnDisciplines( void ) ;
static discInfo * setDtPrintSetupProcDisciplines( void ) ;
static discInfo * setFSelDisciplines( void ) ;
static discInfo * setListDisciplines( void ) ;
static discInfo * setPBtnDisciplines( void ) ;
static discInfo * setRCDisciplines( void ) ;
static discInfo * setScaleDisciplines( void ) ;
static discInfo * setSBarDisciplines( void ) ;
static discInfo * setSWinDisciplines( void ) ;
static discInfo * setSBoxDisciplines( void ) ;
static discInfo * setTextDisciplines( void ) ;
static discInfo * setTBtnDisciplines( void ) ;
static discInfo * setHelpDisciplines( void ) ;




static Namdisc_t arrowDisc = { .createf = arrowCreateDisc };
static Namdisc_t comboDisc = { .createf = comboCreateDisc };
static Namdisc_t cmdDisc = { .createf = cmdCreateDisc };
static Namdisc_t dAreaDisc = { .createf = dAreaCreateDisc };
static Namdisc_t dbtnDisc = { .createf = dbtnCreateDisc };
static Namdisc_t dtPrintDisc = { .createf = dtPrintSetupProcDisc };
static Namdisc_t fselDisc = { .createf = fselCreateDisc };
static Namdisc_t listDisc = { .createf = listCreateDisc };
static Namdisc_t pbtnDisc = { .createf = pbtnCreateDisc };
static Namdisc_t rcDisc = { .createf = rcCreateDisc };
static Namdisc_t scaleDisc = { .createf = scaleCreateDisc };
static Namdisc_t sbarDisc = { .createf = sbarCreateDisc };
static Namdisc_t swinDisc = { .createf = swinCreateDisc };
static Namdisc_t sboxDisc = { .createf = sboxCreateDisc };
static Namdisc_t textDisc = { .createf = textCreateDisc };
static Namdisc_t textDisc2 = { .createf = textCreateDisc2 };
static Namdisc_t tbtnDisc = { .createf = tbtnCreateDisc };
static Namdisc_t dftDisc = { .createf = dftCreateDisc };
static Namdisc_t helpDisc = { .createf = helpCreateDisc };


/*
 * The main window fixups are required because of a Motif bug; the
 * resources were typed as XmRWindows, instead of XmRWidgets.
 */
resfixup_t mainwindow_fixups[] = {
        { "menuBar", XmCMenuBar, XtRWidget, sizeof(Widget) },
        { "commandWindow", XmCCommandWindow, XtRWidget, sizeof(Widget) },
        { "messageWindow", XmCMessageWindow, XtRWidget, sizeof(Widget) },
        { nullptr }
};


/*
 * The help fixups are required to overcome name overlaps between other
 * widgets.
 */
resfixup_t helpDialogs_fixups[] = {
        { "helpType", DtCHelpType, "HelpType", sizeof(unsigned char) },
        { "scrollBarDisplayPolicy", XmCScrollBarDisplayPolicy,
               "HelpScrollBarDisplayPolicy", 
               sizeof(char) },
        { nullptr }
};


/*
 * The following fixups are required to force the resource type to be one
 * we have defined, so that our special converter will work.  Typically,
 * there is no default StringTable to String converter.
 */
resfixup_t list_fixups[] = {
        { "items", XmCItems, "ListItems", sizeof(XmStringTable) },
        { "selectedItems", XmCSelectedItems, "SelectedListItems", 
               sizeof(XmStringTable) },
        { nullptr }
};

resfixup_t command_fixups[] = {
        { "historyItems", XmCItems, "SelBoxItems", sizeof(XmStringTable) },
        { nullptr }
};

resfixup_t selbox_fixups[] = {
        { "listItems", XmCItems, "SelBoxItems", sizeof(XmStringTable) },
        { nullptr }
};

resfixup_t fileselbox_fixups[] = {
        { "fileListItems", XmCItems, "FSFileItems", sizeof(XmStringTable) },
        { "dirListItems", XmCItems, "FSDirItems", sizeof(XmStringTable) },
        { nullptr }
};


/*
 * The following shell resources are 'fixed' to allow them to be referred
 * to in a shell script by their symbolic names, as opposed to their
 * integer values.  These also need to be force onto any subclasses.
 */
resfixup_t wmshell_fixups[] = {
        { "winGravity", XtCWinGravity, "XWinGravity", sizeof(int) },
        { nullptr }
};

resfixup_t vendor_fixups[] = {
        { "winGravity", XtCWinGravity, "XWinGravity", sizeof(int) },
        { "mwmDecorations", XmCMwmDecorations, "MWMDecoration", sizeof(long) },
        { "mwmFunctions", XmCMwmFunctions, "MWMFunctions", sizeof(long) },
        { "mwmInputMode", XmCMwmInputMode, "MWMInputMode", sizeof(long) },
        { nullptr }
};

/*
 * The following resource needs to be 'fixed' because it not only accepts
 * an integer value, but it also supports one specially defined value.
 */
resfixup_t panedwin_con_fixups[] = {
        {"positionIndex", XmCPositionIndex, "PanedWinPosIndex", sizeof(short)},
        { nullptr }
};



classtab_t C[] = {
	/*
	 * NOTE: Keep these in alphabetical order
	 * because the initialize code below depends on
	 * the order.
	 */
	{ "XmArrowButton",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmArrowButtonGadget",  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmBulletinBoard",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmCascadeButton",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmCascadeButtonGadget", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmComboBox",	          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmCommand",	          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmDialogShell",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmDrawingArea",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmDrawnButton",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmFileSelectionBox",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmForm",	          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmFrame",	          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmLabel",	          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmLabelGadget",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmList",	          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmMainWindow",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmMenuShell",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmMessageBox",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmPanedWindow",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmPushButton",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmPushButtonGadget",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmRowColumn",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmScale",	          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmScrollBar",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmScrolledWindow",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmSelectionBox",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmSeparator",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmSeparatorGadget",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmSimpleSpinBox",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmTearOffButton",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmText",	          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmTextField",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmToggleButton",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "XmToggleButtonGadget", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "ApplicationShell",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "OverrideShell",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "Shell",	   	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "TopLevelShell",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "TransientShell",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "VendorShell",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "WMShell",		  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "DtHelpDialog",         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "DtHelpQuickDialog",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ "DtPrintSetupBox",	  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{ nullptr }
};

void
toolkit_init_widgets( void )
{
	int i, n = 0;
	struct namnod *nam;

	if (C[0].class != nullptr)
		return;
	/*
	 * NOTE: keep these in alphabetical order because
	 * the widget table above is in the same order.
	 */
	C[n].disciplines = setArrowDisciplines();
	C[n++].class = xmArrowButtonWidgetClass;;
	C[n].disciplines = setArrowDisciplines();
	C[n++].class = xmArrowButtonGadgetClass;;
	C[n++].class = xmBulletinBoardWidgetClass;
	C[n++].class = xmCascadeButtonWidgetClass;
	C[n++].class = xmCascadeButtonGadgetClass;
	C[n].disciplines = setComboDisciplines();
	C[n++].class = xmComboBoxWidgetClass;
	C[n].disciplines = setCmdDisciplines();
	C[n].resfix = &command_fixups[0];
	C[n++].class = xmCommandWidgetClass;
	C[n].resfix = &vendor_fixups[0];
	C[n++].class = xmDialogShellWidgetClass;
	C[n].disciplines = setDAreaDisciplines();
	C[n++].class = xmDrawingAreaWidgetClass;
	C[n].disciplines = setDBtnDisciplines();
	C[n++].class = xmDrawnButtonWidgetClass;
	C[n].resfix = &fileselbox_fixups[0];
	C[n].disciplines = setFSelDisciplines();
	C[n++].class = xmFileSelectionBoxWidgetClass;
	C[n++].class = xmFormWidgetClass;
	C[n++].class = xmFrameWidgetClass;
	C[n++].class = xmLabelWidgetClass;
	C[n++].class = xmLabelGadgetClass;
	C[n].resfix = &list_fixups[0];
	C[n].disciplines = setListDisciplines();
	C[n++].class = xmListWidgetClass;
	C[n].resfix = &mainwindow_fixups[0];
	C[n++].class = xmMainWindowWidgetClass;
	C[n++].class = xmMenuShellWidgetClass;
	C[n++].class = xmMessageBoxWidgetClass;
	C[n].confix = &panedwin_con_fixups[0];
	C[n++].class = xmPanedWindowWidgetClass;
	C[n].disciplines = setPBtnDisciplines();
	C[n++].class = xmPushButtonWidgetClass;
	C[n].disciplines = setPBtnDisciplines();
	C[n++].class = xmPushButtonGadgetClass;
	C[n].disciplines = setRCDisciplines();
	C[n++].class = xmRowColumnWidgetClass;
	C[n].disciplines = setScaleDisciplines();
	C[n++].class = xmScaleWidgetClass;
	C[n].disciplines = setSBarDisciplines();
	C[n++].class = xmScrollBarWidgetClass;
	C[n].disciplines = setSWinDisciplines();
	C[n++].class = xmScrolledWindowWidgetClass;
	C[n].disciplines = setSBoxDisciplines();
	C[n].resfix = &selbox_fixups[0];
	C[n++].class = xmSelectionBoxWidgetClass;
	C[n++].class = xmSeparatorWidgetClass;
	C[n++].class = xmSeparatorGadgetClass;
	C[n++].class = xmSimpleSpinBoxWidgetClass;
	C[n++].class = xmTearOffButtonWidgetClass;
	C[n].disciplines = setTextDisciplines();
	C[n++].class = xmTextWidgetClass;
	C[n].disciplines = setTextDisciplines();
	C[n++].class = xmTextFieldWidgetClass;
	C[n].disciplines = setTBtnDisciplines();
	C[n++].class = xmToggleButtonWidgetClass;
	C[n].disciplines = setTBtnDisciplines();
	C[n++].class = xmToggleButtonGadgetClass;

	C[n].resfix = &vendor_fixups[0];
	C[n++].class = applicationShellWidgetClass;
	C[n++].class = overrideShellWidgetClass;
	C[n++].class = shellWidgetClass;
	C[n].resfix = &vendor_fixups[0];
	C[n++].class = topLevelShellWidgetClass;
	C[n].resfix = &vendor_fixups[0];
	C[n++].class = transientShellWidgetClass;
	C[n].resfix = &vendor_fixups[0];
	C[n++].class = vendorShellWidgetClass;
	C[n].resfix = &wmshell_fixups[0];
	C[n++].class = wmShellWidgetClass;

	C[n].resfix = &helpDialogs_fixups[0];
	C[n].disciplines = setHelpDisciplines();
	C[n++].class = dtHelpDialogWidgetClass;
	C[n].resfix = &helpDialogs_fixups[0];
	C[n].disciplines = setHelpDisciplines();
	C[n++].class = dtHelpQuickDialogWidgetClass;
	C[n].disciplines = setDtPrintSetupProcDisciplines();
	C[n++].class = dtPrintSetupBoxWidgetClass;
}


/*****************************************************************************
 *****************************************************************************
 *
 * At initialization time, these functions create the structures which
 * map a callback name to its associated discipline table.
 *
 *****************************************************************************
 *****************************************************************************/

static discInfo *
setArrowDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(4, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNactivateCallback);
   info[0].discipline = (void *)&arrowDisc;
   info[1].callbackName = DtkStrDup(XmNarmCallback);
   info[1].discipline = (void *)&arrowDisc;
   info[2].callbackName = DtkStrDup(XmNdisarmCallback);
   info[2].discipline = (void *)&arrowDisc;
   info[3].callbackName = nullptr;
   info[3].discipline = nullptr;
   return(info);
}

static discInfo *
setComboDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(2, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNselectionCallback);
   info[0].discipline = (void *)&comboDisc;
   info[1].callbackName = nullptr;
   info[1].discipline = nullptr;
   return(info);
}

static discInfo *
setCmdDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(3, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNcommandChangedCallback);
   info[0].discipline = (void *)&cmdDisc;
   info[1].callbackName = DtkStrDup(XmNcommandEnteredCallback);
   info[1].discipline = (void *)&cmdDisc;
   info[2].callbackName = nullptr;
   info[2].discipline = nullptr;
   return(info);
}

static discInfo *
setDAreaDisciplines( void )

{
   discInfo * info = (discInfo *)XtCalloc(4, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNexposeCallback);
   info[0].discipline = (void *)&dAreaDisc;
   info[1].callbackName = DtkStrDup(XmNinputCallback);
   info[1].discipline = (void *)&dAreaDisc;
   info[2].callbackName = DtkStrDup(XmNresizeCallback);
   info[2].discipline = (void *)&dAreaDisc;
   info[3].callbackName = nullptr;
   info[3].discipline = nullptr;
   return(info);
}

static discInfo *
setDBtnDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(6, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNactivateCallback);
   info[0].discipline = (void *)&dbtnDisc;
   info[1].callbackName = DtkStrDup(XmNarmCallback);
   info[1].discipline = (void *)&dbtnDisc;
   info[2].callbackName = DtkStrDup(XmNdisarmCallback);
   info[2].discipline = (void *)&dbtnDisc;
   info[3].callbackName = DtkStrDup(XmNexposeCallback);
   info[3].discipline = (void *)&dbtnDisc;
   info[4].callbackName = DtkStrDup(XmNresizeCallback);
   info[4].discipline = (void *)&dbtnDisc;
   info[5].callbackName = nullptr;
   info[5].discipline = nullptr;
   return(info);
}

static discInfo *
setDtPrintSetupProcDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(2, sizeof(discInfo));

	/* Since the same discipline is used for all of DtPrintSetupBox's 
	 * DtPrintSetupProcs, we can use DtRPrintSetupProc for all of them
	 * to avoid defining unnecessary info entries.
	 */
   info[0].callbackName = DtkStrDup(DtRPrintSetupProc);
   info[0].discipline = (void *)&dtPrintDisc;
   info[1].callbackName = nullptr;
   info[1].discipline = nullptr;
   return(info);
}

static discInfo *
setFSelDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(5, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNapplyCallback);
   info[0].discipline = (void *)&fselDisc;
   info[1].callbackName = DtkStrDup(XmNcancelCallback);
   info[1].discipline = (void *)&fselDisc;
   info[2].callbackName = DtkStrDup(XmNnoMatchCallback);
   info[2].discipline = (void *)&fselDisc;
   info[3].callbackName = DtkStrDup(XmNokCallback);
   info[3].discipline = (void *)&fselDisc;
   info[4].callbackName = nullptr;
   info[4].discipline = nullptr;
   return(info);
}

static discInfo *
setListDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(6, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNbrowseSelectionCallback);
   info[0].discipline = (void *)&listDisc;
   info[1].callbackName = DtkStrDup(XmNdefaultActionCallback);
   info[1].discipline = (void *)&listDisc;
   info[2].callbackName = DtkStrDup(XmNextendedSelectionCallback);
   info[2].discipline = (void *)&listDisc;
   info[3].callbackName = DtkStrDup(XmNmultipleSelectionCallback);
   info[3].discipline = (void *)&listDisc;
   info[4].callbackName = DtkStrDup(XmNsingleSelectionCallback);
   info[4].discipline = (void *)&listDisc;
   info[5].callbackName = nullptr;
   info[5].discipline = nullptr;
   return(info);
}

static discInfo *
setPBtnDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(4, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNactivateCallback);
   info[0].discipline = (void *)&pbtnDisc;
   info[1].callbackName = DtkStrDup(XmNarmCallback);
   info[1].discipline = (void *)&pbtnDisc;
   info[2].callbackName = DtkStrDup(XmNdisarmCallback);
   info[2].discipline = (void *)&pbtnDisc;
   info[3].callbackName = nullptr;
   info[3].discipline = nullptr;
   return(info);
}

static discInfo *
setRCDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(6, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNentryCallback);
   info[0].discipline = (void *)&rcDisc;
   info[1].callbackName = DtkStrDup(XmNmapCallback);
   info[1].discipline = (void *)&rcDisc;
   info[2].callbackName = DtkStrDup(XmNtearOffMenuActivateCallback);
   info[2].discipline = (void *)&rcDisc;
   info[3].callbackName = DtkStrDup(XmNtearOffMenuDeactivateCallback);
   info[3].discipline = (void *)&rcDisc;
   info[4].callbackName = DtkStrDup(XmNunmapCallback);
   info[4].discipline = (void *)&rcDisc;
   info[5].callbackName = nullptr;
   info[5].discipline = nullptr;
   return(info);
}

static discInfo *
setScaleDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(3, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNdragCallback);
   info[0].discipline = (void *)&scaleDisc;
   info[1].callbackName = DtkStrDup(XmNvalueChangedCallback);
   info[1].discipline = (void *)&scaleDisc;
   info[2].callbackName = nullptr;
   info[2].discipline = nullptr;
   return(info);
}

static discInfo *
setSBarDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(9, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNdecrementCallback);
   info[0].discipline = (void *)&sbarDisc;
   info[1].callbackName = DtkStrDup(XmNdragCallback);
   info[1].discipline = (void *)&sbarDisc;
   info[2].callbackName = DtkStrDup(XmNincrementCallback);
   info[2].discipline = (void *)&sbarDisc;
   info[3].callbackName = DtkStrDup(XmNpageDecrementCallback);
   info[3].discipline = (void *)&sbarDisc;
   info[4].callbackName = DtkStrDup(XmNpageIncrementCallback);
   info[4].discipline = (void *)&sbarDisc;
   info[5].callbackName = DtkStrDup(XmNtoBottomCallback);
   info[5].discipline = (void *)&sbarDisc;
   info[6].callbackName = DtkStrDup(XmNtoTopCallback);
   info[6].discipline = (void *)&sbarDisc;
   info[7].callbackName = DtkStrDup(XmNvalueChangedCallback);
   info[7].discipline = (void *)&sbarDisc;
   info[8].callbackName = nullptr;
   info[8].discipline = nullptr;
   return(info);
}

static discInfo *
setSWinDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(2, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNtraverseObscuredCallback);
   info[0].discipline = (void *)&swinDisc;
   info[1].callbackName = nullptr;
   info[1].discipline = nullptr;
   return(info);
}

static discInfo *
setSBoxDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(5, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNapplyCallback);
   info[0].discipline = (void *)&sboxDisc;
   info[1].callbackName = DtkStrDup(XmNcancelCallback);
   info[1].discipline = (void *)&sboxDisc;
   info[2].callbackName = DtkStrDup(XmNnoMatchCallback);
   info[2].discipline = (void *)&sboxDisc;
   info[3].callbackName = DtkStrDup(XmNokCallback);
   info[3].discipline = (void *)&sboxDisc;
   info[4].callbackName = nullptr;
   info[4].discipline = nullptr;
   return(info);
}

static discInfo *
setTextDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(10, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNactivateCallback);
   info[0].discipline = (void *)&dftDisc;
   info[1].callbackName = DtkStrDup(XmNfocusCallback);
   info[1].discipline = (void *)&dftDisc;
   info[2].callbackName = DtkStrDup(XmNgainPrimaryCallback);
   info[2].discipline = (void *)&dftDisc;
   info[3].callbackName = DtkStrDup(XmNlosePrimaryCallback);
   info[3].discipline = (void *)&dftDisc;
   info[4].callbackName = DtkStrDup(XmNlosingFocusCallback);
   info[4].discipline = (void *)&textDisc;
   info[5].callbackName = DtkStrDup(XmNmodifyVerifyCallback);
   info[5].discipline = (void *)&textDisc;
   info[6].callbackName = DtkStrDup(XmNmodifyVerifyCallbackWcs);
   info[6].discipline = (void *)&textDisc2;
   info[7].callbackName = DtkStrDup(XmNmotionVerifyCallback);
   info[7].discipline = (void *)&textDisc;
   info[8].callbackName = DtkStrDup(XmNvalueChangedCallback);
   info[8].discipline = (void *)&dftDisc;
   info[9].callbackName = nullptr;
   info[9].discipline = nullptr;
   return(info);
}

static discInfo *
setTBtnDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(4, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(XmNarmCallback);
   info[0].discipline = (void *)&tbtnDisc;
   info[1].callbackName = DtkStrDup(XmNdisarmCallback);
   info[1].discipline = (void *)&tbtnDisc;
   info[2].callbackName = DtkStrDup(XmNvalueChangedCallback);
   info[2].discipline = (void *)&tbtnDisc;
   info[3].callbackName = nullptr;
   info[3].discipline = nullptr;
   return(info);
}

static discInfo *
setHelpDisciplines( void )
{
   discInfo * info = (discInfo *)XtCalloc(3, sizeof(discInfo));

   info[0].callbackName = DtkStrDup(DtNhyperLinkCallback);
   info[0].discipline = (void *)&helpDisc;
   info[1].callbackName = DtkStrDup(DtNcloseCallback);
   info[1].discipline = (void *)&helpDisc;
   info[2].callbackName = nullptr;
   info[2].discipline = nullptr;
   return(info);
}
