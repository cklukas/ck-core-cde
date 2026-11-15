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
 * $XConsortium: connect.c /main/3 1995/11/06 18:03:27 rswiston $
 * 
 * @(#)connect.c	3.25 13 Feb 1994	cde_app_builder/src/abmf
 * 
 * RESTRICTED CONFIDENTIAL INFORMATION:
 * 
 * The information in this document is subject to special restrictions in a
 * confidential disclosure agreement between HP, IBM, Sun, USL, SCO and
 * Univel.  Do not distribute this document outside HP, IBM, Sun, USL, SCO,
 * or Univel without Sun's specific written approval.  This document and all
 * copies and derivative works thereof must be returned or destroyed at Sun's
 * request.
 * 
 * Copyright 1993 Sun Microsystems, Inc.  All rights reserved.
 * 
 */

/*
 * connect.c - Handles connections stuff
 */

#include <stdio.h>
#include <string.h>
#include <ab_private/trav.h>
#include "ui_header_fileP.h"	/* abmfP_comp_get_widget_specific_includes */
#include "obj_namesP.h"
#include "connectP.h"

#define	APP_PREFIX_LEN	4
#define APP_PREFIX	"_app"

static int	abmfP_set_action_names_for_obj(ABObj rootObj);


STRING
abmfP_get_action_name(ABObj action)
{
    static char         name[256];
    STRING		parent_name = (STRING) nullptr;
    ABObj               from = nullptr, parent = nullptr;
    STRING		tmpName = nullptr;
    *name = 0;

    from = obj_get_from(action);
    if (action->info.action.auto_named && (from != nullptr))
    {
        /* If the source object is the project,
         * then this is an "Application" connection.
	 * This probably won't happen because right
	 * now "Application" connections can only be
	 * of type AB_FUNC_USER_DEF (which means auto-
	 * named is FALSE).
         */
        if (obj_is_project(from))
	{
	    parent = obj_get_project(action);
	    parent_name = (STRING) util_malloc(
				strlen(obj_get_name(parent)) 
				+ APP_PREFIX_LEN + 1);
	    strcpy(parent_name, obj_get_name(parent));
	    strcat(parent_name, APP_PREFIX);
	}
	else
	{
            parent = obj_get_module(from);
            parent_name = obj_get_name(parent);
	}

        if (!util_strempty(parent_name) &&
            !util_strempty(obj_get_name(from)))
	{
	    sprintf(name, "%s_%s%s",
		    parent_name,
		    obj_get_name(from),
		    obj_get_func_name_suffix(action));
	}

        if (obj_is_project(from))
        {
	    util_free(parent_name);
	}
    }
    else
    {
	if ((tmpName = obj_get_func_name(action)) != nullptr)
	{
	    sprintf(name, "%s", tmpName);
	}
    }

    return (name[0] == 0 ? nullptr : name);
}


/*
 *
 */
int
abmfP_tree_set_action_names(ABObj rootObj)
{
    int			return_value = 0;
    AB_TRAVERSAL	moduleTrav;
    ABObj		module = nullptr;

    if (!obj_is_project(rootObj))
    {
	return_value = abmfP_set_action_names_for_obj(rootObj);
    }
    else
    {
	/* for efficiency, we'll only visit those modules that are 
	 * going to be written
	 */
	abmfP_set_action_names_for_obj(rootObj);	/* project 1st */

	for (trav_open(&moduleTrav, rootObj, AB_TRAV_MODULES);
		(module = trav_next(&moduleTrav)) != nullptr; )
	{
	    if (obj_get_write_me(module))
	    {
	        abmfP_set_action_names_for_obj(module);	/* each module */
	    }
	}
	trav_close(&moduleTrav);
    }

    return return_value;
}


static int
abmfP_set_action_names_for_obj(ABObj rootObj)
{
    int			return_value = 0;
    int          	actionNum = -1;
    AB_TRAVERSAL        actionTrav;
    ABObj               action = nullptr;
    ABObj		fromObj = nullptr;
    char                buf[256];

    if (mfobj_data(rootObj) == nullptr)
    {
	return -1;
    }

    for (trav_open(&actionTrav, rootObj, AB_TRAV_ACTIONS_FOR_OBJ);
	 	(action = trav_next(&actionTrav)) != nullptr;)
    {
	if (   (obj_get_func_type(action) != AB_FUNC_USER_DEF)
	    && ((fromObj = obj_get_from(action)) != nullptr) )
	{
	        action->info.action.auto_named = TRUE;
		actionNum = mfobj_data(fromObj)->num_auto_callbacks + 1;
		mfobj_data(fromObj)->num_auto_callbacks = actionNum;
	        sprintf(buf, "_CB%d", actionNum);
	        obj_set_func_name_suffix(action, buf);
	}
    }
    trav_close(&actionTrav);

    return return_value;
}


int
abmfP_get_connect_includes(StringList includeFiles, ABObj projOrModule)
{
    AB_TRAVERSAL	trav;
    BOOL		actionIsCrossModule = FALSE;
    ABObj		project = obj_get_project(projOrModule);
    ABObj		action = nullptr;
    ABObj		toObj = nullptr;
    ABObj		compRoot = nullptr;
    ABObj		toModule = nullptr;
    ABObj		win_parent = nullptr;
    ABObj		popup_win = nullptr;
    char		headerFilename[MAXPATHLEN+1];
    *headerFilename = 0;

    strlist_set_is_unique(includeFiles, TRUE);
    assert(project != nullptr);
    if (project == nullptr)
    {
	return 0;
    }

    for (trav_open(&trav, projOrModule, AB_TRAV_ACTIONS_FOR_OBJ);
	(action = trav_next(&trav)) != nullptr; )
    {
	actionIsCrossModule = obj_is_cross_module(action);
	toObj = obj_get_to(action);
	if (toObj == nullptr)
	{
	    continue;
	}

	toModule = obj_get_module(toObj);

	if (actionIsCrossModule)
	{
	    sprintf(headerFilename, "\"%s\"", 
	        abmfP_get_ui_header_file_name(toModule));
            strlist_add_str(includeFiles,  headerFilename, nullptr);
  	}

	/*
	 * Check to see if the popup's window parent is in another module
	 */
	compRoot = obj_get_root(toObj);
	popup_win = nullptr;
	if (obj_is_popup_win(compRoot))
	{
	    popup_win = compRoot;
	}
	if (popup_win == nullptr) 
	{
	    popup_win = obj_get_parent_of_type(compRoot, AB_TYPE_DIALOG);
	}
	if (popup_win == nullptr)
	{
	    popup_win = obj_get_parent_of_type(compRoot, AB_TYPE_BASE_WINDOW);
	}

	if (popup_win != nullptr)
	{
	    win_parent= obj_get_win_parent(popup_win);
	    if (win_parent != nullptr)
	    {
	       toModule = obj_get_module(win_parent);
	       if (toModule != nullptr)
	       {
		   sprintf(headerFilename, "\"%s\"", 
			abmfP_get_ui_header_file_name(toModule));
	           strlist_add_str(includeFiles, headerFilename, nullptr);
	       }
 	    }
        }

	/*
	 * For some actions, we need widget-specific convenience
	 * functions and resource strings that exist in the header files.
	 */
	if (obj_get_func_type(action) == AB_FUNC_BUILTIN)
	{
	    switch (obj_get_func_builtin(action))
	    {
		case AB_STDACT_SET_LABEL:
		case AB_STDACT_SET_TEXT:
		case AB_STDACT_SET_VALUE:
	            abmfP_comp_get_widget_specific_includes(
			includeFiles, obj_get_root(toObj));
		break;
	    }
	}
    }
    trav_close(&trav);

    return 0;
}

