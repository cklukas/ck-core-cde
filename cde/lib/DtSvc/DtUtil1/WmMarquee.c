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
/************************************<+>*************************************
 ****************************************************************************
 **
 **   File:     WmMarquee.c
 **
 **   RCS:	$XConsortium: WmMarquee.c /main/4 1995/10/26 15:12:59 rswiston $
 **
 **   Project:  DT Workspace Manager
 **
 **   Description: Get Marquee Selection Data
 **
 ** (c) Copyright 1993, 1994 Hewlett-Packard Company
 ** (c) Copyright 1993, 1994 International Business Machines Corp.
 ** (c) Copyright 1993, 1994 Sun Microsystems, Inc.
 ** (c) Copyright 1993, 1994 Novell, Inc.
 **
 ****************************************************************************
 ************************************<+>*************************************/
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <Tt/tttk.h>
#include <Dt/Wsm.h> 
#include <Dt/WsmM.h>
#include <Dt/SvcTT.h>
#include "WsmP.h" 
#include <Dt/Service.h>
#include <Xm/Xm.h>

/*************************************<->*************************************
 *
 *  Tt_callback_action _WsSelectionCB (Tt_message m, Tt_pattern p)
 *
 *
 *  Description:
 *  -----------
 *  Internal function called for a marquee selection
 *
 *
 *  Inputs:
 *  ------
 *  m		- ToolTalk message
 *  p		- ToolTalk pattern
 *
 *  Outputs:
 *  --------
 *  Return	- ToolTalk callback status
 *
 *  Comments:
 *  ---------
 * 
 *************************************<->***********************************/

static Tt_callback_action
_WsSelectionCB (Tt_message m, Tt_pattern p)
{
    struct _DtWsmCBContext	*pCbCtx;
    int				type, val;
    Position			x, y;
    Dimension			width, height;

    pCbCtx = (struct _DtWsmCBContext *)tt_pattern_user(p, 0);

    /*
     * 0th arg: screen number, string, not used
     */

    tt_message_arg_ival(m, 1, &type);	/* type */
    tt_message_arg_ival(m, 2, &val);	/* x */
    x = val;
    tt_message_arg_ival(m, 3, &val);	/* y */
    y = val;
    tt_message_arg_ival(m, 4, &val);	/* width */
    width = val;
    tt_message_arg_ival(m, 5, &val);	/* height */
    height = val;

    /*
     * Call registered callback function.
     */
    (*pCbCtx->callback.marquee_selection)(pCbCtx->widget, type, x, y,
                                          width, height,
                                          pCbCtx->client_data);
    return TT_CALLBACK_PROCESSED;
} /* END OF FUNCTION _DtWsmWsModifyHandler */

/*************************************<->*************************************
 *
 *  DtWsmCBContext * DtWsmAddMarqueeSelectionCallback (widget, 
 *						 	marquee_select, 
 *							client_data)
 *
 *
 *  Description:
 *  -----------
 *  Register a function to be called when a marquee selection is made
 *
 *
 *  Inputs:
 *  ------
 *  widget		- widget for this client
 *  marquee_select	- function to call for marquee select
 *  client_data		- additional data to pass back to client when called.
 *
 *  Outputs:
 *  --------
 *  Return	- ptr to callback context data (opaque) 
 *
 *  Comments:
 *  ---------
 *  The callback context data ptr should be saved if you intend to
 *  removed this callback at some point in the future. 
 * 
 *************************************<->***********************************/
DtWsmCBContext 
_DtWsmAddMarqueeSelectionCallback (
	Widget				widget,
	DtWsmMarqueeSelectionProc	marquee_selection,
	XtPointer			client_data)
{
    struct _DtWsmCBContext *pCbCtx;
    int		screen;
    String	sName;
    char	sNum[32];

    Tt_status	status;
    Tt_pattern	pattern;
    char *	sessId;

    /*
     * This function register a ToolTalk pattern for every
     * callback added.
     */
    _DtSvcInitToolTalk(widget);

    pattern = tt_pattern_create();
    status = tt_ptr_error(pattern);
    if (status != TT_OK) {
	return NULL;
    }

    if (tt_pattern_category_set(pattern, TT_OBSERVE) != TT_OK) {
	return NULL;
    }
    if (tt_pattern_scope_add(pattern, TT_SESSION) != TT_OK) {
	return NULL;
    }
    sessId = tt_default_session();
    if (tt_pattern_session_add(pattern, sessId) != TT_OK) {
	return NULL;
    }
    tt_free(sessId);

    screen = XScreenNumberOfScreen(XtScreen(widget));
    sprintf(sNum, "%d", screen);
    sName = _DtWsmSelectionNameForScreen (screen);

    pCbCtx = (struct _DtWsmCBContext *)
		XtMalloc(sizeof(struct _DtWsmCBContext));
    pCbCtx->pattern = pattern;
    pCbCtx->widget = widget;
    pCbCtx->callback.marquee_selection = marquee_selection;
    pCbCtx->client_data = client_data;
    pCbCtx->nested_context = NULL;

    /*
     * Only receive DtMarquee_Selection notice from the screen
     * we registered with.
     */
    status = tt_pattern_arg_add(pattern, TT_IN, Tttk_string, sNum);
    if (status != TT_OK) {
	XtFree((char *)pCbCtx);
	return NULL;
    }

    if (tt_pattern_op_add(pattern, "DtMarquee_Selection") != TT_OK) {
	XtFree((char *)pCbCtx);
	return NULL;
    }

    if (tt_pattern_state_add(pattern, TT_SENT) != TT_OK) {
	XtFree((char *)pCbCtx);
	return NULL;
    }

    /*
     * Store information needed by the callback in the user data
     * fields of the pattern.
     */
    status = tt_pattern_user_set(pattern, 0, (void *)pCbCtx);
    if (status != TT_OK) {
	XtFree((char *)pCbCtx);
	return NULL;
    }

    /*
     * _WsSelectionCB is the ToolTalk callback which will call
     * the user callback.
     */
    if (tt_pattern_callback_add(pattern, _WsSelectionCB) != TT_OK) {
	XtFree((char *)pCbCtx);
	return NULL;
    }

    if (tt_pattern_register(pattern) != TT_OK) {
	XtFree((char *)pCbCtx);
	return NULL;
    }

    XtFree (sName);

    return (pCbCtx);

} /* END OF FUNCTION DtWsmAddMarqueeSelectionCallback */
