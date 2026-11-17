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
/* $XConsortium: SvcTT.c /main/3 1995/10/26 15:30:18 rswiston $ */
/*%% 									 */
/*%%  	RESTRICTED CONFIDENTIAL INFORMATION:                             */
/*%% 									 */
/*%% 	The information in this document is subject to special		 */
/*%% 	restrictions in a confidential disclosure agreement between	 */
/*%% 	HP, IBM, Sun, USL, SCO and Univel.  Do not distribute this	 */
/*%% 	document outside HP, IBM, Sun, USL, SCO, or Univel without	 */
/*%% 	Sun's specific written approval.  This document and all copies	 */
/*%% 	and derivative works thereof must be returned or destroyed at	 */
/*%% 	Sun's request.							 */
/*%% 									 */
/*  (c) Copyright 1993, 1994 Hewlett-Packard Company			 */
/*  (c) Copyright 1993, 1994 International Business Machines Corp. 	 */
/*  (c) Copyright 1993, 1994 Sun Microsystems, Inc. 			 */
/*  (c) Copyright 1993, 1994 Novell, Inc. 				 */
/*%% 									 */
#include <string.h>
#include <sys/time.h>
#include <X11/Intrinsic.h>
#include <Dt/SvcTT.h>


/*
 * Ensure that there is a valid ToolTalk connection open.
 * If none is open, creates one and adds it to the XtAppContext
 * of widget.  Returns
 *   TT_OK		Success.
 *   TT_ERR_NOMP	ToolTalk could not be initialized.
 *   TT_ERR_POINTER	widget was null.
 */
static XtInputId s_tt_input_id = 0;

static void
_DtSvcTtXtInputHandler(
	XtPointer  client_data,
	int       *source,
	XtInputId *id
)
{
	XtInputId saved = id ? *id : 0;
	char *p;
	tttk_Xt_input_handler( client_data, source, id );
	p = tt_default_procid();
	if (tt_ptr_error( p ) == TT_OK) {
		tt_free( p );
	} else if (saved != 0 && s_tt_input_id == saved) {
		/* tttk_Xt_input_handler already called XtRemoveInput(*id) on TT_ERR_NOMP */
		s_tt_input_id = 0;
	}
}

Tt_status
_DtSvcInitToolTalk(
	Widget widget
)
{
	char	       *procid;
	Tt_status	status;
	int		fd;
	XtAppContext	ctxt;

	procid = tt_default_procid();
	status = tt_ptr_error( procid );
	if (status == TT_OK) {
		tt_free( procid );
	}
	if (widget != 0) {
		Display *dpy = XtDisplayOfObject( widget );
		if (dpy != 0) {
			Window root = DefaultRootWindow( dpy );
			Atom a_sun = XInternAtom( dpy, "_SUN_TT_SESSION", True );
			Atom a_cde = XInternAtom( dpy, "TT_SESSION", True );
			Atom prop = (a_sun != None) ? a_sun : a_cde;
			Atom actual = None;
			int fmt = 0;
			unsigned long nitems = 0, rem = 0;
			unsigned char *raw = 0;
			if (prop != None &&
			    XGetWindowProperty( dpy, root, prop, 0, 128, False,
						AnyPropertyType, &actual, &fmt,
						&nitems, &rem, &raw ) == Success &&
			    raw != 0) {
				if (fmt == 8 && nitems > 0 && raw[0] != '\0') {
					const char *xsess = (const char *)raw;
					const char *xsess_cmp =
					    (xsess[0] == 'P' && xsess[1] == ' ') ? xsess + 2 : xsess;
					char *cursess = (status == TT_OK) ? tt_default_session() : 0;
					if (status == TT_OK &&
					    tt_ptr_error( cursess ) == TT_OK &&
					    cursess != 0) {
						const char *cur_cmp =
						    (cursess[0] == 'P' && cursess[1] == ' ')
							? cursess + 2
							: cursess;
						if (strcmp( xsess_cmp, cur_cmp ) != 0) {
							if (s_tt_input_id != 0) {
								XtRemoveInput( s_tt_input_id );
								s_tt_input_id = 0;
							}
							tt_close();
							status = TT_ERR_NOMP;
							(void)tt_default_session_set( xsess );
						}
						tt_free( cursess );
					} else {
						if (cursess != 0 && tt_ptr_error( cursess ) == TT_OK) {
							tt_free( cursess );
						}
						(void)tt_default_session_set( xsess );
					}
				}
				XFree( raw );
			}
		}
	}
	if ((status == TT_ERR_NOMP) || (status == TT_ERR_PROCID)) {
		if (widget == 0) {
			return TT_ERR_POINTER;
		}
		if (s_tt_input_id != 0) {
			XtRemoveInput( s_tt_input_id );
			s_tt_input_id = 0;
		}
		procid = tt_open(); /* do not tt_free(); Xt will pass it back */
		status = tt_ptr_error( procid );
		if (status != TT_OK) {
			return status;
		}
		fd = tt_fd();
		status = tt_int_error( fd );
		if (status != TT_OK) {
			return status;
		}
		ctxt = XtWidgetToApplicationContext( widget );
		s_tt_input_id = XtAppAddInput( ctxt, fd, (XtPointer)XtInputReadMask,
			       _DtSvcTtXtInputHandler, procid );
	}
	return TT_OK;
}
