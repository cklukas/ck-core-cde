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
/**---------------------------------------------------------------------
***	
***	file:		PanelS.c
***
***	project:	MotifPlus Widgets
***
***	description:	Source code for DtPanelShell class.
***	
***
***			(c) Copyright 1992 by Hewlett-Packard Company.
***
***
***-------------------------------------------------------------------*/


/*-------------------------------------------------------------
**	Include Files
*/

#include <Xm/XmP.h>

#include <X11/Xatom.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include <X11/ShellP.h>
#include <Dt/DtStrDefs.h>

#include "PanelSP.h"


/********    Public Function Declarations    ********/

/********    End Public Function Declarations    ********/

/********    Static Function Declarations    ********/


/********    End Static Function Declarations    ********/



/*-------------------------------------------------------------
**	Resource List
*/
#define R_Offset(field) \
	XtOffset (DtPanelShellWidget, panel.field)

static XtResource panelResources[]=
    {
	{
	XmNresolution,
	XmCDimension, XmRDimension, sizeof (Dimension),
	R_Offset (resolution), XmRImmediate, (XtPointer) 0
	},
	{
	XmNhighResFontList,
	XmCHighResFontList, XmRFontList, sizeof (XmFontList),
	R_Offset (high_res_font_list), XmRString, (XtPointer) "fixed"
	},
	{
	XmNmediumResFontList,
	XmCMediumResFontList, XmRFontList, sizeof (XmFontList),
	R_Offset (medium_res_font_list), XmRString, (XtPointer) "fixed"
	},
	{
	XmNlowResFontList,
	XmCLowResFontList, XmRFontList, sizeof (XmFontList),
	R_Offset (low_res_font_list), XmRString, (XtPointer) "fixed"
	}
    };



/*-------------------------------------------------------------
**	Class Record
*/

static CompositeClassExtensionRec compositeClassExtension =
    {
    /* next_extension	*/	nullptr,
    /* record_type	*/	NULLQUARK,
    /* version		*/	XtCompositeExtensionVersion,
    /* record_size	*/	sizeof(CompositeClassExtensionRec),
    /* accepts_objects	*/	TRUE
    };

DtPanelShellClassRec dtPanelShellClassRec =
  {
/*	Core Part
 */
    {
    /* superclass         */    (WidgetClass) &topLevelShellClassRec,
    /* class_name         */    "FrontPanel",
    /* size               */    sizeof(DtPanelShellRec),
    /* Class Initializer  */	nullptr,
    /* class_part_initialize*/	nullptr,
    /* Class init'ed ?    */	FALSE,
    /* initialize         */    nullptr,
    /* initialize_notify  */	nullptr,		
    /* realize            */    XtInheritRealize,
    /* actions            */    nullptr,
    /* num_actions        */    0,
    /* resources          */    panelResources,
    /* resource_count     */	XtNumber(panelResources),
    /* xrm_class          */    NULLQUARK,
    /* compress_motion    */    FALSE,
    /* compress_exposure  */    XtExposeCompressMaximal,
    /* compress_enterleave*/    FALSE,
    /* visible_interest   */    FALSE,
    /* destroy            */    nullptr,
    /* resize             */    XtInheritResize,
    /* expose             */    nullptr,
    /* set_values         */    nullptr,
    /* set_values_hook    */	nullptr,			
    /* set_values_almost  */	XtInheritSetValuesAlmost,
    /* get_values_hook    */	nullptr,			
    /* accept_focus       */    nullptr,
    /* intrinsics version */	XtVersion,
    /* callback offsets   */    nullptr,
    /* tm_table		  */	XtInheritTranslations,
    /* query_geometry	  */	nullptr,
    /* display_accelerator*/	nullptr,
    /* extension	  */	nullptr
    },
/*	Composite Part
 */
    {
    /* geometry_manager   */    XtInheritGeometryManager,
    /* change_managed     */    XtInheritChangeManaged,
    /* insert_child	  */	XtInheritInsertChild,
    /* delete_child	  */	XtInheritDeleteChild,
    /* extension	  */	(XtPointer)&compositeClassExtension
    },
/*	Constraint Part
 */
    {
    /* extension	  */	nullptr
    },
    {
    /* extension	  */	nullptr
    },
    {
    /* extension	  */	nullptr
    },
    {
    /* extension	  */	nullptr
    }
  };

WidgetClass dtPanelShellWidgetClass = (WidgetClass) &dtPanelShellClassRec;



/*-------------------------------------------------------------
**	Private Procs
**-------------------------------------------------------------
*/


/*-------------------------------------------------------------
**	Core Procs
**-------------------------------------------------------------
*/

