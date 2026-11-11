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
//%%  (c) Copyright 1993, 1994 Hewlett-Packard Company			
//%%  (c) Copyright 1993, 1994 International Business Machines Corp.	
//%%  (c) Copyright 1993, 1994 Sun Microsystems, Inc.			
//%%  (c) Copyright 1993, 1994 Novell, Inc. 				
//%%  $TOG: ttdesktop.C /main/4 1999/10/14 19:06:10 mgreess $ 			 				
/*
 * Copyright 1993 Sun Microsystems, Inc.  All rights reserved.
 */
#include "tt_options.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <locale.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#if defined(OPT_SYSINFO)
#include <sys/systeminfo.h>
#endif
#include <netdb.h>	// MAXHOSTNAMELEN
#include <sys/param.h>  // MAXHOSTNAMELEN on AIX
// Because otherwise e.g. XtNiconic cashes out as XtShellStrings[nnn]
#define XTSTRINGDEFINES
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include "Tt/tt_c.h"
#include "api/c/api_api.h"
#include "util/tt_Xlib.h"
#include "util/tt_env_names.h"
#include "util/tt_port.h"
#include "Tt/tttk.h"
#include "tttk/tttk2free.h"
#include "tttk/tttkutils.h"
#include "tttk/tttkmessage.h"
#include "tttk/tttkpattern.h"
#include "tttk/ttdtprocid.h"
#include "tttk/ttdesktop.h"
#include "tttk/ttdtprocess.h"

extern _TtDtProcid *_ttdtme;

static Tt_message	_ttDtRefuse( Tt_message msg, void *clientData,
				     Tt_status status );

// These should be in a .h file...?

static const char *_Tt_categories[] = {
	"LC_CTYPE",
	"LC_NUMERIC",
	"LC_TIME",
	"LC_COLLATE",
	"LC_MESSAGES",
	"LC_MONETARY",
	"LC_ALL",
	0
};

//
// Create a message addressed to our commissioner
//
static Tt_message
_ttDesktopMessageCreate(
	Tt_message		commission,
	Tt_class		theClass,
	const char	       *handler,
	Tttk_op			op,
	_TtDtMessageCB		toolkitCB,
	void		       *clientCB,
	void		       *clientData
)
{
	const char *_handler = handler;
	_TttkItem2Free fuse;
	if ((handler == 0) && (commission != 0)) {
		_handler = tt_message_sender( commission );
		fuse = (caddr_t)_handler;
	}
	return _ttDtPMessageCreate( commission, theClass, TT_SESSION,
				    handler, op, toolkitCB,
				    clientCB, clientData );
}

//
// Add commission id and send, if appropriate
//
Tt_message
_ttDesktopMessageFinish(
	Tt_message		msg,
	Tt_message		commission,
	int			send
)
{
	_TttkItem2Free fuse = msg;
	Tt_status status;
	if (commission != 0) {
		char *id = _tttk_message_id( commission );
		status = tt_message_arg_add( msg, TT_IN, Tttk_message_id, id );
		tt_free( id );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	}
	if (send) {
		status = tt_message_send( msg );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	}
	fuse = (caddr_t)0;
	return msg;
}

//
// Add commission id and register in session, if appropriate
//
Tt_pattern
_ttDesktopPatternFinish(
	Tt_pattern	pat,
	Tt_message	commission,
	int		register_it
)
{
	_TttkItem2Free fuse = pat;
	Tt_status status;
	if (commission != 0) {
		char *id = _tttk_message_id( commission );
		status = tt_pattern_arg_add( pat, TT_IN, Tttk_message_id, id );
		tt_free( id );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		tt_pattern_user_set( pat, _TttkContractKey, commission );
	}
	if (register_it) {
		status = tt_pattern_register( pat );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
	}
	fuse = (caddr_t)0;
	return pat;
}

//
// Returns widget if it is a realized shell and mappedWhenManaged, else 0
//
Widget
_ttdt_realized_widget(
	void *widget,
	int   mappedWhenManaged_Shell
)
{
	Widget _widget = (Widget)widget;
	if (_widget == 0) return _widget;
	if ((! _tt_load_xt()) || (! _tt_load_xlib())) {
		return 0;
	}
	if (! CALLXT(XtIsRealized)( _widget )) {
		return 0;
	}
	if (! mappedWhenManaged_Shell) {
		return _widget;
	}
	Boolean mappedWhenManaged = False;
	CALLXT(XtVaGetValues)( _widget,
			       XtNmappedWhenManaged, &mappedWhenManaged, NULL );
	if (! mappedWhenManaged) {
		return 0;
	}
	return _widget;
	/*
	WidgetClass shellClass =
		(WidgetClass)XTSYM(applicationShellWidgetClass);
	if (CALLXT(XtIsSubclass)( _widget, shellClass )) { // XXX never true!?
		return _widget;
	}
	return 0;
	*/
}

//
// Parse a request and pass it to the user's contract callback.
//
Tt_message
_ttdt_contract_cb(
	Tt_message	msg,
	Tt_pattern	pat,
	void	       *clientCB,
	void	       *clientData
)
{
	Ttdt_contract_cb _cb = (Ttdt_contract_cb)clientCB;
	char *opString = tt_message_op( msg );
	Tttk_op op = tttk_string_op( opString );
	tt_free( opString );
	Boolean sensitive = True;
	int silent, force;
	static Atom wmProtocols = 0;
	static Atom wmDeleteWindow = 0;
	switch (op) {
		Widget widget;
		Display *dpy;
		Window win;
		XClientMessageEvent ev;
	    case TTDT_GET_STATUS:
		// Fill in the answers we know
		tt_message_arg_val_set( msg, 1, _ttdtme->vendor() );
		tt_message_arg_val_set( msg, 2, _ttdtme->toolname() );
		tt_message_arg_val_set( msg, 3, _ttdtme->version() );
		break;
	    case TTDT_QUIT:
	    case TTDT_PAUSE:
	    case TTDT_RESUME:
		if (_cb != 0) {
			break;
		}
		widget = _ttdt_realized_widget( clientData );
		if (widget == 0) {
			tttk_message_fail( msg, TT_DESKTOP_ENOTSUP, 0, 0 );
			return 0;
		}
		dpy = (Display *)CALLXT(XtDisplay)( widget );
		win = CALLXT(XtWindow)( widget );
		switch (op) {
		    case TTDT_QUIT:
			silent = _tttk_message_arg_ival( msg, 0, 0 );
			force  = _tttk_message_arg_ival( msg, 1, 0 );
			if (silent || force) {
				//
				// tttk cannot guarantee that the application
				// will neither block on user input nor
				// cancel the WM_DELETE_WINDOW
				//
				tttk_message_fail( msg, TT_DESKTOP_ENOTSUP,
						   0, 0 );
				return 0;
			}
			memset( &ev, 0, sizeof(ev) );
			ev.type = ClientMessage;
			ev.display = dpy;
			ev.window = win;
			if (wmProtocols == 0) {
				wmProtocols = CALLX11(XInternAtom)( dpy,
						"WM_PROTOCOLS", False );
				wmDeleteWindow = CALLX11(XInternAtom)( dpy,
						"WM_DELETE_WINDOW", False );
			}
			ev.message_type = wmProtocols;
			ev.format = 32;
			ev.data.l[0] = wmDeleteWindow;
			ev.data.l[1] = 0; //XtLastTimestampProcessed?
			CALLX11(XSendEvent)( dpy, win, False, 0L, (XEvent*)&ev );
			tt_message_reply( msg );
			tt_message_destroy( msg );
			return 0;
		    case TTDT_PAUSE:
			sensitive = False;
		    case TTDT_RESUME:
			if (CALLXT(XtIsSensitive)( widget ) == sensitive) {
				tt_message_status_set(msg,TT_DESKTOP_EALREADY);
			} else {
				CALLXT(XtSetSensitive)( widget, sensitive );
			}
			tt_message_reply( msg );
			tt_message_destroy( msg );
			return 0;
		}
		break;
	}
	if (_cb == 0) {
		return msg;
	}
	return (*_cb)( msg, clientData, _tttk_pattern_contract( pat ) );
}

//
// Create a pattern for a desktop message
//
Tt_pattern
_ttdt_pat(
	Tttk_op			op,
	_TtDtMessageCB		internalCB,
	Tt_category		category,
	Tt_message		commission,
	void		       *clientCB,
	void		       *clientdata,
	int			register_it
)
{
	Tt_pattern pat = _ttDtPatternCreate( category, TT_SESSION,
					     register_it, 0, op,
					     internalCB, clientCB,
					     clientdata, 0 );
	Tt_status status = tt_ptr_error( pat );
	if (status != TT_OK) {
		return pat;
	}
	_TttkItem2Free fuse = pat;
	Tt_mode mode = TT_OUT;
	switch (op) {
	    case TTDT_SET_XINFO:
		mode = TT_IN;
	    case TTDT_GET_XINFO:
		status = tt_pattern_arg_add( pat, mode, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, mode, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, mode, Tttk_integer, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		break;
	    case TTDT_SET_GEOMETRY:
		mode = TT_INOUT;
	    case TTDT_GET_GEOMETRY:
		status = tt_pattern_arg_add( pat, mode, Tttk_width, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, mode, Tttk_height, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, mode, Tttk_xoffset, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, mode, Tttk_yoffset, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		break;
	    case TTDT_SET_ICONIFIED:
		mode = TT_IN;
	    case TTDT_GET_ICONIFIED:
		status = tt_pattern_arg_add( pat, mode, Tttk_boolean, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		break;
	    case TTDT_QUIT:
		status = tt_pattern_arg_add( pat, TT_IN, Tttk_boolean, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, TT_IN, Tttk_boolean, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		break;
	    case TTDT_STATUS:
		mode = TT_IN;
	    case TTDT_GET_STATUS:
		status = tt_pattern_arg_add( pat, mode, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, mode, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, mode, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, mode, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		break;
	    case TTDT_DO_COMMAND:
		status = tt_pattern_arg_add( pat, TT_IN, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		status = tt_pattern_arg_add( pat, TT_OUT, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_pattern)tt_error_pointer( status );
		}
		break;
	    case TTDT_PAUSE:
	    case TTDT_RESUME:
		// No args
		break;
	}
	fuse = (caddr_t)0;
	return _ttDesktopPatternFinish( pat, commission, register_it );
}

static Tt_message
_ttDtStarted(
	Tttk_op		op,
	Tt_message	context,
	const char     *toolName,
	const char     *vendor,
	const char     *version,
	int		sendAndDestroy
)
{
	Tt_message msg = tttk_message_create( context, TT_NOTICE, TT_SESSION, 0,
					      _ttDtOp( op ), 0 );
	Tt_status status = tt_ptr_error( msg );
	if (status != TT_OK) {
		return msg;
	}
	tt_message_arg_add( msg, TT_IN, Tttk_string, toolName );
	tt_message_arg_add( msg, TT_IN, Tttk_string, vendor );
	tt_message_arg_add( msg, TT_IN, Tttk_string, version );
	if (! sendAndDestroy) {
		return msg;
	}
	status = tt_message_send( msg );
	if (status != TT_OK) {
		tttk_message_destroy( msg );
		return (Tt_message)tt_error_pointer( status );
	}
	tttk_message_destroy( msg );
	return 0;
}

Tt_message
ttdt_Started(
	Tt_message	context,
	const char     *toolName,
	const char     *vendor,
	const char     *version,
	int		sendAndDestroy
)
{
	return _ttDtStarted( TTDT_STARTED, context, toolName, vendor, version,
			     sendAndDestroy );
}

Tt_message
ttdt_Stopped(
	Tt_message	context,
	const char     *toolName,
	const char     *vendor,
	const char     *version,
	int		sendAndDestroy
)
{
	return _ttDtStarted( TTDT_STOPPED, context, toolName, vendor, version,
			     sendAndDestroy );
}

Tt_message
ttdt_Status(
	Tt_message	context,
	Tt_message	commission,
	const char     *statusString,
	const char     *toolName,
	const char     *vendor,
	const char     *version,
	int		sendAndDestroy
)
{
	char *handler = 0;
	if (commission != 0) {
		handler = tt_message_sender( commission );
	}
	Tt_message msg = _ttDtPMessageCreate( context, TT_NOTICE, TT_SESSION,
					handler, TTDT_STATUS, 0, 0, 0 );
	tt_free( handler );
	Tt_status status = tt_ptr_error( msg );
	if (status != TT_OK) {
		return msg;
	}
	tt_message_arg_add( msg, TT_IN, Tttk_string, statusString );
	tt_message_arg_add( msg, TT_IN, Tttk_string, toolName );
	tt_message_arg_add( msg, TT_IN, Tttk_string, vendor );
	tt_message_arg_add( msg, TT_IN, Tttk_string, version );
	msg = _ttDesktopMessageFinish( msg, commission, sendAndDestroy );
	status = tt_ptr_error( msg );
	if (sendAndDestroy && (tt_ptr_error( msg ) == TT_OK)) {
		tttk_message_destroy( msg );
		return 0;
	}
	return msg;
}

//
// Returns X11 geometry of a widget
//
static int
_ttXGetGeometry(
	Widget		widget,
	int	       *w,
	int	       *h,
	int	       *x,
	int	       *y
)
{
	if ((! _tt_load_xt()) || (! _tt_load_xlib())) {
		return 0;
	}
	XWindowAttributes	attrs;
	Window			junkwin;
	Display		       *dpy = (Display *)CALLXT(XtDisplay)( widget );
	Window			win = CALLXT(XtWindow)( widget );
	int			rx, ry;
	if (! CALLX11(XGetWindowAttributes)( dpy, win, &attrs )) {
		return 0;
	}
	// XXX x,y still seem to be a little off
	CALLX11(XTranslateCoordinates)( dpy, win, attrs.root, 
					-attrs.border_width,
					-attrs.border_width,
					&rx, &ry, &junkwin );
	*x = rx;
	*y = ry;
	*w = attrs.width;
	*h = attrs.height;
	return 1;
}

//
// Help out on Set_Geometry requests and edicts
//
static Tt_message
_ttdt_do_Set_Geometry(
	Tt_message	msg,
	void	       *clientData,
	int	       *width,
	int	       *height,
	int	       *xOffset,
	int	       *yOffset
)
{
	_TttkItem2Free fuse = msg;
	Widget widget = _ttdt_realized_widget( clientData );
	if (widget == 0) {
		tttk_message_fail( msg, TT_DESKTOP_ENOTSUP, 0, 0 );
		return 0;
	}
	int newX, newY, newW, newH;
	if (! _ttXGetGeometry( widget, &newW, &newH, &newX, &newY )) {
		tttk_message_fail( msg, TT_DESKTOP_ENOTSUP, 0, 0 );
		return 0;
	}
	if (*width > 0) {
		newW = *width;
	}
	if (*height > 0) {
		newH = *height;
	}
	if (*xOffset != INT_MAX) {
		newX = *xOffset;
	}
	if (*yOffset != INT_MAX) {
		newY = *yOffset;
	}
	CALLX11(XMoveResizeWindow)( CALLXT(XtDisplay)( widget),
				    CALLXT(XtWindow)( widget ),
				    newX, newY, newW, newH );
	fuse = (caddr_t)0;
	return msg;
}

//
// Pattern callback for {GS}et_Geometry requests and Set_Geometry edicts
//
Tt_message
_ttdt_do_GSet_Geometry(
	Tt_message	msg,
	void	       *clientData,
	Tt_message	,
	int	       *width,
	int	       *height,
	int	       *xOffset,
	int	       *yOffset
)
{
	_TttkItem2Free fuse = msg;
	Widget widget = _ttdt_realized_widget( clientData );
	if (widget == 0) {
		tttk_message_fail( msg, TT_DESKTOP_ENOTSUP, 0, 0 );
		return 0;
	}
	char *ops = tt_message_op( msg );
	Tttk_op op = tttk_string_op( ops );
	tt_free( ops );
	Tt_status status;
	if ( op == TTDT_SET_GEOMETRY) {
		msg = _ttdt_do_Set_Geometry( msg, clientData, width,
					     height, xOffset, yOffset );
		status = tt_ptr_error( msg );
		if ((status != TT_OK) || (msg == 0)) {
			return msg;
		}
	}
	int x, y, w, h;
	if (! _ttXGetGeometry( widget, &w, &h, &x, &y )) {
		tttk_message_fail( msg, TT_DESKTOP_ENOTSUP, 0, 0 );
		return 0;
	}
	*width = w;
	*height = h;
	*xOffset = x;
	*yOffset = y;
	fuse = (caddr_t)0;
	return msg;
}

//
// Parse Get_Geometry reply and pass it to user callback.
//
static Tt_message
_ttdt_Geometry_in_cb(
	Tt_message	msg,
	Tt_pattern	,
	void	       *clientCB,
	void	       *clientData
)
{
	if (! _tttk_message_in_final_state( msg )) {
		// Not in final state; our address space is probably handler
		return msg;
	}
	_TttkItem2Free fuse = msg;
	int width   = _tttk_message_arg_ival( msg, 0, -1 );
	int height  = _tttk_message_arg_ival( msg, 1, -1 );
	int xOffset = _tttk_message_arg_ival( msg, 2, INT_MAX );
	int yOffset = _tttk_message_arg_ival( msg, 3, INT_MAX );
	Ttdt_Geometry_in_cb _cb = (Ttdt_Geometry_in_cb)clientCB;
	fuse = (caddr_t)0; // prevent message destruction
	//
	// A Ttdt_Geometry_in_cb is never involved in a pattern,
	// so contract is always 0.
	//
	msg = (*_cb)( msg, clientData, 0, width, height, xOffset, yOffset );
	return msg;
}

//
// Create and optionally send a {GS}et_Geometry request or edict
//
static Tt_message
_ttdt_GSet_Geometry(
	Tttk_op			op,
	const char	       *handler,
	Tt_message		commission,
	Tt_class		theClass,
	Ttdt_Geometry_in_cb	callback,
	void		       *clientdata,
	int			w,
	int			h,
	int			x,
	int			y,
	int			send
)
{
	Tt_message msg = _ttDesktopMessageCreate( commission, theClass,
				handler, op, _ttdt_Geometry_in_cb,
				(void *)callback, clientdata );
	Tt_status status = tt_ptr_error( msg );
	if (status != TT_OK) {
		return msg;
	}
	_TttkItem2Free fuse = msg;
	if (op == TTDT_GET_GEOMETRY) {
		status = tt_message_arg_add( msg, TT_OUT, Tttk_width, 0 );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_arg_add( msg, TT_OUT, Tttk_height, 0 );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_arg_add( msg, TT_OUT, Tttk_xoffset, 0 );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_arg_add( msg, TT_OUT, Tttk_yoffset, 0 );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	} else {
		status = tt_message_iarg_add( msg, TT_INOUT, Tttk_width, w );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_iarg_add( msg, TT_INOUT, Tttk_height, h );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_iarg_add( msg, TT_INOUT, Tttk_xoffset, x );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_iarg_add( msg, TT_INOUT, Tttk_yoffset, y );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	}
	fuse = (caddr_t)0;
	return _ttDesktopMessageFinish( msg, commission, send );
}

Tt_message
ttdt_Get_Geometry(
	const char	       *handler,
	Tt_message		commission,
	Ttdt_Geometry_in_cb	callback,
	void		       *clientdata,
	int			send
)
{
	return _ttdt_GSet_Geometry( TTDT_GET_GEOMETRY, handler, commission,
				    TT_REQUEST, callback, clientdata,
				    0, 0, 0, 0, send );
}

Tt_message
ttdt_Set_Geometry(
	const char	       *handler,
	Tt_message		commission,
	Tt_class		theClass,
	Ttdt_Geometry_in_cb	callback,
	void		       *clientdata,
	int			width,
	int			height,
	int			xoffset,
	int			yoffset,
	int			send
)
{
	return _ttdt_GSet_Geometry( TTDT_SET_GEOMETRY, handler, commission,
				    theClass, callback, clientdata,
				    width, height, xoffset, yoffset, send );
}

//
// Parse {GS}et_Geometry request, pass it to user callback,
// and optionally fill in and send the reply.
//
static Tt_message
_ttdt_Geometry_out_cb(
	Tt_message	msg,
	Tt_pattern	pat,
	void	       *clientCB,
	void	       *clientData
)
{
	_TttkItem2Free fuse = msg;
	int width   = _tttk_message_arg_ival( msg, 0, -1 );
	int height  = _tttk_message_arg_ival( msg, 1, -1 );
	int xOffset = _tttk_message_arg_ival( msg, 2, INT_MAX );
	int yOffset = _tttk_message_arg_ival( msg, 3, INT_MAX );
	Ttdt_Geometry_out_cb _cb = (Ttdt_Geometry_out_cb)clientCB;
	Tt_message contract = _tttk_pattern_contract( pat );
	msg = (*_cb)( msg, clientData, contract, &width, &height, &xOffset,
		      &yOffset );
	Tt_status status = tt_ptr_error( msg );
	if ((status != TT_OK) || (msg == 0)) {
		// user already replied or failed, and destroyed
		fuse = (caddr_t)0;
		return msg;
	}
	if (width != -1) {
		tt_message_arg_ival_set( msg, 0, width );
	}
	if (height != -1) {
		tt_message_arg_ival_set( msg, 1, height );
	}
	if (xOffset != INT_MAX) {
		tt_message_arg_ival_set( msg, 2, xOffset );
	}
	if (yOffset != INT_MAX) {
		tt_message_arg_ival_set( msg, 3, yOffset );
	}
	status = _tttk_message_reply( msg );
	if (status != TT_OK) {
		return (Tt_message)tt_error_pointer( status );
	}
	return 0;
}

Tt_pattern
ttdt_Set_Geometry_pat(
	Tt_category		category,
	Tt_message		commission,
	Ttdt_Geometry_out_cb	callback,
	void		       *clientdata,
	int			register_it
)
{
	return _ttdt_pat( TTDT_SET_GEOMETRY, _ttdt_Geometry_out_cb,
			  category, commission,
			  (void *)callback, clientdata, register_it );
}

Tt_pattern
ttdt_Get_Geometry_pat(
	Tt_category		category,
	Tt_message		commission,
	Ttdt_Geometry_out_cb	callback,
	void		       *clientdata,
	int			register_it
)
{
	return _ttdt_pat( TTDT_GET_GEOMETRY, _ttdt_Geometry_out_cb,
			  category, commission,
			  (void *)callback, clientdata, register_it );
}

static int
_ttdt_is_iconic(
	Widget widget
)
{
	Boolean iconic;
	// XXX XtNiconic always returns false?!
	CALLXT(XtVaGetValues)( widget, XtNiconic, &iconic, NULL );
	return (iconic == TRUE);
}

//
// Pattern callback for {GS}et_Iconified, {GS}et_Mapped requests and edicts
//
Tt_message
_ttdt_do_wm_state(
	Tt_message	msg,
	void	       *clientData,
	Tt_message	,
	int	       *iconified_or_mapped
)
{
	Widget widget = _ttdt_realized_widget( clientData );
	if (widget == 0) {
		tttk_message_fail( msg, TT_DESKTOP_ENOTSUP, 0, 0 );
		return 0;
	}
	char *ops = tt_message_op( msg );
	Tttk_op op = tttk_string_op( ops );
	tt_free( ops );
	Display		       *dpy = (Display *)CALLXT(XtDisplay)( widget );
	Window			win = CALLXT(XtWindow)( widget );
	int			iconic;
	int			mapped;
	XWindowAttributes	attribs;
	switch (op) {
	    case TTDT_SET_ICONIFIED:
		CALLX11(XGetWindowAttributes)( dpy, win, &attribs );
		if (*iconified_or_mapped) {
			int screen = CALLX11(XScreenNumberOfScreen)(
					attribs.screen );
			CALLX11(XIconifyWindow)( dpy, win, screen );
		} else {
			CALLX11(XRaiseWindow)( dpy, win ); // or XMapRaised?
		}
		// Fall through, to see if it worked
	    case TTDT_GET_ICONIFIED:
		iconic = _ttdt_is_iconic( widget );
		if (   (op == TTDT_SET_ICONIFIED)
		    && (*iconified_or_mapped != iconic))
		{
			tttk_message_fail( msg, TT_ERR_ACCESS, 0, 0 );
			return 0;
		}
		*iconified_or_mapped = iconic;
		break;
	    case TTDT_SET_MAPPED:
		CALLX11(XGetWindowAttributes)( dpy, win, &attribs );
		if (*iconified_or_mapped) {
			CALLX11(XMapWindow)( dpy, win );
		} else {
			int screen = CALLX11(XScreenNumberOfScreen)(
					attribs.screen );
			CALLX11(XWithdrawWindow)( dpy, win, screen );
		}
		// Fall through, to see if it worked
	    case TTDT_GET_MAPPED:
		CALLX11(XGetWindowAttributes)( dpy, win, &attribs );
		mapped = attribs.map_state != IsUnmapped;
		if (   (op == TTDT_SET_ICONIFIED)
		    && (*iconified_or_mapped != mapped))
		{
			tttk_message_fail( msg, TT_ERR_ACCESS, 0, 0 );
			return 0;
		}
		*iconified_or_mapped = mapped;
		break;
	}
	return msg;
}

//
// Parse Get_Iconified reply and pass it to user callback.
//
static Tt_message
_ttdt_Iconified_in_cb(
	Tt_message	msg,
	Tt_pattern	pat,
	void	       *clientCB,
	void	       *clientData
)
{
	if (! _tttk_message_in_final_state( msg )) {
		// Not in final state; our address space is probably handler
		return msg;
	}
	_TttkItem2Free fuse = msg;
	int iconified = _tttk_message_arg_ival( msg, 0, 0 );
	Ttdt_Iconified_in_cb _cb = (Ttdt_Iconified_in_cb)clientCB;
	fuse = (caddr_t)0; // prevent message destruction
	msg = (*_cb)( msg, clientData, _tttk_pattern_contract( pat ),
		      iconified );
	return msg;
}

//
// Create and optionally send a {GS}et_Iconified request or edict
//
static Tt_message
_ttdt_GSet_Iconified(
	Tttk_op			op,
	const char	       *handler,
	Tt_message		commission,
	Tt_class		theClass,
	Ttdt_Iconified_in_cb	callback,
	void		       *clientdata,
	int			iconified,
	int			send
)
{
	Tt_message msg = _ttDesktopMessageCreate( commission, theClass,
				handler, op, _ttdt_Iconified_in_cb,
				(void *)callback, clientdata );
	Tt_status status = tt_ptr_error( msg );
	if (status != TT_OK) {
		return msg;
	}
	_TttkItem2Free fuse = msg;
	if (op == TTDT_GET_ICONIFIED) {
		status = tt_message_arg_add( msg, TT_OUT, Tttk_boolean, 0 );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	} else {
		status = tt_message_iarg_add( msg, TT_IN, Tttk_boolean,
					      iconified );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	}
	fuse = (caddr_t)0;
	return _ttDesktopMessageFinish( msg, commission, send );
}

Tt_message
ttdt_Get_Iconified(
	const char	       *handler,
	Tt_message		commission,
	Ttdt_Iconified_in_cb	callback,
	void		       *clientdata,
	int			send
)
{
	return _ttdt_GSet_Iconified( TTDT_GET_ICONIFIED, handler, commission,
				     TT_REQUEST, callback, clientdata,
				     0, send );
}

Tt_message
ttdt_Set_Iconified(
	const char	       *handler,
	Tt_message		commission,
	Tt_class		theClass,
	Ttdt_Iconified_in_cb	callback,
	void		       *clientdata,
	int			iconified,
	int			send
)
{
	return _ttdt_GSet_Iconified( TTDT_SET_ICONIFIED, handler, commission,
				     theClass, callback, clientdata,
				     iconified, send );
}

//
// Parse {GS}et_Iconified request, pass it to user callback,
// and optionally fill in and send the reply.
//
static Tt_message
_ttdt_Iconified_out_cb(
	Tt_message	msg,
	Tt_pattern	pat,
	void	       *clientCB,
	void	       *clientData
)
{
	_TttkItem2Free fuse = msg;
	int iconified = _tttk_message_arg_ival( msg, 0, 0 );
	Ttdt_Iconified_out_cb _cb = (Ttdt_Iconified_out_cb)clientCB;
	msg = (*_cb)( msg, clientData, _tttk_pattern_contract( pat ),
		      &iconified );
	Tt_status status = tt_ptr_error( msg );
	if ((status != TT_OK) || (msg == 0)) {
		// user already replied or failed, and destroyed
		fuse = (caddr_t)0;
		return msg;
	}
	tt_message_arg_ival_set( msg, 0, iconified );
	status = _tttk_message_reply( msg );
	if (status != TT_OK) {
		return (Tt_message)tt_error_pointer( status );
	}
	return 0;
}

Tt_pattern
ttdt_Set_Iconified_pat(
	Tt_category		category,
	Tt_message		commission,
	Ttdt_Iconified_out_cb	callback,
	void		       *clientdata,
	int			register_it
)
{

	return _ttdt_pat( TTDT_SET_ICONIFIED, _ttdt_Iconified_out_cb,
			  category, commission,
			  (void *)callback, clientdata, register_it );
}

Tt_pattern
ttdt_Get_Iconified_pat(
	Tt_category		category,
	Tt_message		commission,
	Ttdt_Iconified_out_cb	callback,
	void		       *clientdata,
	int			register_it
)
{
	return _ttdt_pat( TTDT_GET_ICONIFIED, _ttdt_Iconified_out_cb,
			  category, commission,
			  (void *)callback, clientdata, register_it );
}

//
// Pattern callback for Set_XInfo requests.  The display of a widget cannot
// be changed and neither can its visual or depth, so the request sets $DISPLAY,
// which is what the process starts and opens from now on use; the windows and
// the connection it has stay where they are.  The display must have the shape
// of one, the visual one of the names of X.h, the depth 1 to 32.
//
Tt_message
_ttdt_do_Set_XInfo(
	Tt_message	msg,
	void	       *,
	Tt_message	,
	char          **display,
	int	       *visual,
	int	       *depth
)
{
	Tt_status status = TT_OK;
	if (*display == 0) {
		status = TT_DESKTOP_EPROTO;
	} else if (_tttk_message_arg_is_set( msg, 1 ) && (*visual == -1)) {
		status = TT_DESKTOP_EINVAL;
	} else if (_tttk_message_arg_is_set( msg, 2 ) &&
		   ((*depth < 1) || (*depth > 32))) {
		status = TT_DESKTOP_EINVAL;
	}
	if (status == TT_OK) {
		status = _ttdt_adopt_display( *display );
	}
	if (status != TT_OK) {
		tttk_message_fail( msg, status, 0, 1 );
		return 0;
	}
	tt_message_reply( msg );
	tttk_message_destroy( msg );
	return 0;
}

//
// Map the name of an X11 visual to a visual #defined in X.h
//
static const char *_ttdt_visuals[] = {
	"StaticGray",
	"GrayScale",
	"StaticColor",
	"PseudoColor",
	"TrueColor",
	"DirectColor"
};

static int
_ttDtVisual(
	const char *visualName
)
{
	if (visualName == 0) {
		return -1;
	}
	for (int visual = StaticGray; visual <= DirectColor; visual++) {
		if (strcmp( visualName, _ttdt_visuals[visual] ) == 0) {
			return visual;
		}
	}
	return -1;
}

static const char *
_ttDtVisualString(
	int visual
)
{
	if ((visual < StaticGray) || (visual > DirectColor)) {
		return 0;
	}
	return _ttdt_visuals[ visual ];
}

//
// Pattern callback for Get_XInfo requests
//
Tt_message
_ttdt_do_Get_XInfo(
	Tt_message	msg,
	void	       *_widget,
	Tt_message	,
	char          **display,
	int	       *visual,
	int	       *depth
)
{
	_TttkItem2Free fuse = msg;
	Widget widget = _ttdt_realized_widget( _widget, 0 );
	if (widget == 0) {
		tttk_message_fail( msg, TT_DESKTOP_ENOTSUP, 0, 0 );
		return 0;
	}
	Display *dpy = (Display *)CALLXT(XtDisplay)( widget );
	*display = DisplayString( dpy );
	if (*display == 0) {
		tttk_message_fail( msg, TT_ERR_INTERNAL,
				   "DisplayString() == 0", 0 );
		return 0;
	}
	int is_local = 0;
	char *display_and_screen;
	if ((*display)[0] == ':') {
		display_and_screen = *display;
		is_local = 1;
	} else if (strncmp(*display, "unix:", 5) == 0) {
		display_and_screen = (*display)+4; // !5; we want the colon
		is_local = 1;
	}
	if (is_local) {
		_Tt_string portable_display = _tt_gethostname();
		portable_display = portable_display.cat(display_and_screen);
		*display = _tt_strdup((char *)portable_display);
	} else {
		*display = _tt_strdup( *display );
	}
	*visual = DefaultVisualOfScreen(
			DefaultScreenOfDisplay( dpy ))->c_class;
	*depth = DefaultDepthOfScreen( DefaultScreenOfDisplay( dpy ));
	fuse = (caddr_t)0;
	return msg;
}

//
// Parse Get_XInfo reply and pass it to user callback.
//
static Tt_message
_ttdt_XInfo_in_cb(
	Tt_message	msg,
	Tt_pattern	pat,
	void	       *clientCB,
	void	       *clientData
)
{
	if (! _tttk_message_in_final_state( msg )) {
		// Not in final state; our address space is probably handler
		return msg;
	}
	_TttkItem2Free fuse = msg;
	char *display = 0;
	if (_tttk_message_arg_is_set( msg, 0 )) {
		Tt_status status = _ttdt_arg_string( msg, 0, _TTDT_MAX_DISPLAY,
						     &display );
		if (status != TT_OK) {
			fuse = (caddr_t)0;
			return _ttDtRefuse( msg, clientData, status );
		}
	}
	char *visual = _tttk_message_arg_val( msg, 1, 0 );
	int _visual = _ttDtVisual( visual );
	tt_free( visual );
	int depth = _tttk_message_arg_ival( msg, 2, 0 );
	Ttdt_XInfo_in_cb _cb = (Ttdt_XInfo_in_cb)clientCB;
	fuse = (caddr_t)0; // prevent message destruction
	msg = (*_cb)( msg, clientData, _tttk_pattern_contract( pat ),
		      display, _visual, depth );
	return msg;
}

//
// Create and optionally send a {GS}et_XInfo request or edict
//
static Tt_message
_ttdt_GSet_XInfo(
	Tttk_op			op,
	const char	       *handler,
	Tt_message		commission,
	Tt_class		theClass,
	Ttdt_XInfo_in_cb	callback,
	void		       *clientdata,
	const char	       *display,
	int			visual,
	int			depth,
	int			send
)
{
	Tt_message msg = _ttDesktopMessageCreate( commission, theClass,
				handler, op, _ttdt_XInfo_in_cb,
				(void *)callback, clientdata );
	Tt_status status = tt_ptr_error( msg );
	if (status != TT_OK) {
		return msg;
	}
	_TttkItem2Free fuse = msg;
	if (op == TTDT_GET_XINFO) {
		status = tt_message_arg_add( msg, TT_OUT, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_arg_add( msg, TT_OUT, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_arg_add( msg, TT_OUT, Tttk_integer, 0 );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	} else {
		status = tt_message_arg_add( msg, TT_IN, Tttk_string, display);
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_arg_add( msg, TT_IN, Tttk_string,
					     _ttDtVisualString( visual ));
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		// as the pattern of a handler has it: all three are in arguments
		status = tt_message_iarg_add( msg, TT_IN, Tttk_integer,
					      depth );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	}
	fuse = (caddr_t)0;
	return _ttDesktopMessageFinish( msg, commission, send );
}

Tt_message
ttdt_Get_XInfo(
	const char	       *handler,
	Tt_message		commission,
	Ttdt_XInfo_in_cb	callback,
	void		       *clientdata,
	int			send
)
{
	return _ttdt_GSet_XInfo( TTDT_GET_XINFO, handler, commission,
				 TT_REQUEST, callback, clientdata,
				 0, 0, 0, send );
}

Tt_message
ttdt_Set_XInfo(
	const char	       *handler,
	Tt_message		commission,
	Tt_class		theClass,
	Ttdt_XInfo_in_cb	callback,
	void		       *clientdata,
	const char	       *display,
	int			visual,
	int			depth,
	int			send
)
{
	return _ttdt_GSet_XInfo( TTDT_SET_XINFO, handler, commission,
				 theClass, callback, clientdata,
				 display, visual, depth, send );
}

//
// Parse {GS}et_XInfo request, pass it to user callback,
// and optionally fill in and send the reply.
//
static Tt_message
_ttdt_XInfo_out_cb(
	Tt_message	msg,
	Tt_pattern	pat,
	void	       *clientCB,
	void	       *clientData
)
{
	_TttkItem2Free fuse = msg;
	//
	// The arguments of a request are the sender's: strings of bounded
	// size with no null byte inside.
	//
	char *display = 0;
	char *_visual = 0;
	Tt_status argStatus = TT_OK;
	if (_tttk_message_arg_is_set( msg, 0 )) {
		argStatus = _ttdt_arg_string( msg, 0, _TTDT_MAX_DISPLAY,
					      &display );
	}
	if ((argStatus == TT_OK) && _tttk_message_arg_is_set( msg, 1 )) {
		argStatus = _ttdt_arg_string( msg, 1, _TTDT_MAX_VISUAL_NAME,
					      &_visual );
	}
	if (argStatus != TT_OK) {
		tt_free( display );
		tt_free( _visual );
		fuse = (caddr_t)0;
		return _ttDtRefuse( msg, 0, argStatus );
	}
	int visual = _ttDtVisual( _visual );
	tt_free( _visual );
	int depth = _tttk_message_arg_ival( msg, 2, 1 );
	Ttdt_XInfo_out_cb _cb = (Ttdt_XInfo_out_cb)clientCB;
	msg = (*_cb)( msg, clientData, _tttk_pattern_contract( pat ),
		      &display, &visual, &depth );
	Tt_status status = tt_ptr_error( msg );
	if ((status != TT_OK) || (msg == 0)) {
		// user already replied or failed, and destroyed
		fuse = (caddr_t)0;
		return msg;
	}
	tt_message_arg_val_set( msg, 0, display );
	tt_message_arg_val_set( msg, 1, _ttDtVisualString( visual ));
	tt_message_arg_ival_set( msg, 2, depth );
	status = _tttk_message_reply( msg );
	if (status != TT_OK) {
		return (Tt_message)tt_error_pointer( status );
	}
	return 0;
}

Tt_pattern
ttdt_Set_XInfo_pat(
	Tt_category		category,
	Tt_message		commission,
	Ttdt_XInfo_out_cb	callback,
	void		       *clientdata,
	int			register_it
)
{
	return _ttdt_pat( TTDT_SET_XINFO, _ttdt_XInfo_out_cb,
			  category, commission,
			  (void *)callback, clientdata, register_it );
}

Tt_pattern
ttdt_Get_XInfo_pat(
	Tt_category		category,
	Tt_message		commission,
	Ttdt_XInfo_out_cb	callback,
	void		       *clientdata,
	int			register_it
)
{
	return _ttdt_pat( TTDT_GET_XINFO, _ttdt_XInfo_out_cb,
			  category, commission,
			  (void *)callback, clientdata, register_it );
}

//
// Arguments that cannot be used.  A request is failed; a reply to a request
// of ttdt_sender_imprint_on() (clientData is its ImprintInfo) is noted there.
// Either way msg is gone.
//
static Tt_message
_ttDtRefuse(
	Tt_message	msg,
	void	       *clientData,
	Tt_status	status
)
{
	if (_tttk_message_am_handling( msg )) {
		tttk_message_fail( msg, status, 0, 1 );
	} else {
		if (clientData != 0) {
			_ttDtImprintReplied( (ImprintInfo *)clientData, msg,
					     status );
		}
		tttk_message_destroy( msg );
	}
	return 0;
}

//
// Parse Get_Locale reply and pass it to user callback.
// Also used to parse Set_Locale request and pass it to _ttDtApplyLocale().
// The categories and the locales are strings of the sizes and shapes of
// names, and are freed when the callback returns.  A locale may be left out
// (null) by a handler that does not answer for its category.
//
Tt_message
_ttDtGetLocaleCB(
	Tt_message	msg,
	Tt_pattern	,
	void	       *clientCB,
	void	       *clientData
)
{
	_TttkItem2Free fuse = msg;
	int numArgs = tt_message_args_count( msg );
	Tt_status status = tt_int_error( numArgs );
	if (status != TT_OK) {
		return (Tt_message)tt_error_pointer( status );
	}
	int n = numArgs / 2;
	if (n > _TTDT_MAX_LOCALE_PAIRS) {
		fuse = (caddr_t)0;
		return _ttDtRefuse( msg, clientData, TT_DESKTOP_EINVAL );
	}
	_TttkList2Free fuses( 2 * n + 2 );
	char **categories = (char **)tt_malloc( (n + 1) * sizeof(char *) );
	char **locales    = (char **)tt_malloc( (n + 1) * sizeof(char *) );
	if ((categories == 0) || (locales == 0)) {
		tt_free( (caddr_t)categories );
		tt_free( (caddr_t)locales );
		fuse = (caddr_t)0;
		return _ttDtRefuse( msg, clientData, TT_ERR_NOMEM );
	}
	for (int i = 0; i <= n; i++) {
		categories[ i ] = 0;
		locales[    i ] = 0;
	}
	//
	// We only need these guys until after we call clientCB
	//
	fuses += (caddr_t)categories;
	fuses += (caddr_t)locales;
	for (int i = 0; i < n; i++) {
		status = _ttdt_arg_string( msg, 2 * i, _TTDT_MAX_CATEGORY_NAME,
					   &categories[i] );
		if (status == TT_OK) {
			fuses += categories[i];
			if (_tttk_message_arg_is_set( msg, 2 * i + 1 )) {
				status = _ttdt_arg_string( msg, 2 * i + 1,
						_TTDT_MAX_LOCALE_NAME,
						&locales[i] );
				if (status == TT_OK) {
					fuses += locales[i];
				}
			}
		}
		if (status != TT_OK) {
			fuse = (caddr_t)0;
			return _ttDtRefuse( msg, clientData, status );
		}
	}
	fuse = (caddr_t)0; // aborts message destruction
	Ttdt_Get_Locale_msg_cb _cb = (Ttdt_Get_Locale_msg_cb)clientCB;
	return (*_cb)( msg, clientData,
		       (const char **)categories,
		       (const char **)locales );
}

//
// A callback used internally that sets the locales of this address space
// from a Set_Locale request, and answers it.
//
static Tt_message
_ttDtApplyLocale(
	Tt_message	msg,
	void	       *,
	const char    **categories,
	const char    **locales
)
{
	Tt_status status = _ttdt_adopt_locale( categories, locales );
	if (status == TT_OK) {
		tt_message_reply( msg );
		tttk_message_destroy( msg );
	} else {
		tttk_message_fail( msg, status, 0, 1 );
	}
	return 0;
}

//
// The reply is passed to clientCB, which is required.
//
Tt_message
ttdt_Get_Locale(
	const char         *handler,
	Tt_message	    commission,
	Ttdt_Get_Locale_msg_cb  clientCB,
	void		   *clientData,
	const char        **categories,
	int                 send
)
{
	const char *_handler = handler;
	if ((handler == 0) && (commission != 0)) {
		_handler = tt_message_sender( commission );
	}
	if (clientCB == 0) {
		return (Tt_message)tt_error_pointer( TT_ERR_POINTER );
	}
	Tt_message msg = _ttDtPMessageCreate( commission, TT_REQUEST,
				TT_SESSION, handler,
				TTDT_GET_LOCALE, _ttDtGetLocaleCB, (void *)clientCB,
				clientData );
	Tt_status status = tt_ptr_error( msg );
	if (status != TT_OK) {
		return msg;
	}
	//
	// Guarantees that msg will be destroyed when this function returns
	//
	_TttkItem2Free fuse = msg;
	const char **_cats = categories;
	if (_cats == 0) {
		_cats = _Tt_categories;
	}
	while (*_cats != 0) {
		status = tt_message_arg_add( msg, TT_IN, Tttk_string, *_cats );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		status = tt_message_arg_add( msg, TT_OUT, Tttk_string, 0 );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
		_cats++;
	}
	if (send) {
		status = tt_message_send( msg );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	}
	fuse = (caddr_t)0;
	return msg;
}

//
// Parse Get_Situation reply and pass it to user callback.  The directory is
// a string of the size of a path, freed with tt_free() by the callback.
//
static Tt_message
_ttDtGetSituationCB(
	Tt_message	msg,
	Tt_pattern	,
	void	       *clientCB,
	void	       *clientData
)
{
	if (! _tttk_message_in_final_state( msg )) {
		// Not in final state; our address space is probably handler
		return msg;
	}
	_TttkItem2Free fuse = msg;
	char *cwd = 0;
	if (_tttk_message_arg_is_set( msg, 0 )) {
		Tt_status status = _ttdt_arg_string( msg, 0, PATH_MAX, &cwd );
		if (status != TT_OK) {
			fuse = (caddr_t)0;
			return _ttDtRefuse( msg, clientData, status );
		}
	}
	Ttdt_Get_Situation_msg_cb _cb = (Ttdt_Get_Situation_msg_cb)clientCB;
	msg = (*_cb)( msg, clientData, cwd );
	fuse = (caddr_t)0; // prevent message destruction
	return msg;
}

//
// The reply is passed to clientCB, which is required.
//
Tt_message
ttdt_Get_Situation(
	const char     *handler,
	Tt_message	commission,
	Ttdt_Get_Situation_msg_cb	clientCB,
	void	       *clientData,
	int		send
)
{
	const char *_handler = handler;
	if ((handler == 0) && (commission != 0)) {
		_handler = tt_message_sender( commission );
	}
	if (clientCB == 0) {
		return (Tt_message)tt_error_pointer( TT_ERR_POINTER );
	}
	Tt_message msg = _ttDtPMessageCreate( commission, TT_REQUEST,
				TT_SESSION, _handler,
				TTDT_GET_SITUATION, _ttDtGetSituationCB,
				(void *)clientCB, clientData );
	Tt_status status = tt_ptr_error( msg );
	if (status != TT_OK) {
		return msg;
	}
	//
	// Guarantees that msg will be destroyed when this function returns
	//
	_TttkItem2Free fuse = msg;
	status = tt_message_arg_add( msg, TT_OUT, Tttk_string, 0 );
	if (status != TT_OK) {
		return (Tt_message)tt_error_pointer( status );
	}
	if (send) {
		status = tt_message_send( msg );
		if (status != TT_OK) {
			return (Tt_message)tt_error_pointer( status );
		}
	}
	fuse = (caddr_t)0;
	return msg;
}

//
// Set or remove the variables of a Set_Environment request: pairs of a name
// and, if it is to be set, a value.  Everything is checked before the first
// variable is changed, so that a request that asks for one that no message
// may set (TT_DESKTOP_EACCES) or one that is not a variable at all
// (TT_DESKTOP_EINVAL) changes nothing.
//
static Tt_status
_ttDtSetEnvironment(
	Tt_message	msg,
	int		n
)
{
	if (n > _TTDT_MAX_VARIABLES) {
		return TT_DESKTOP_EINVAL;
	}
	char **names  = (char **)calloc( n + 1, sizeof(char *) );
	char **values = (char **)calloc( n + 1, sizeof(char *) );
	Tt_status status = ((names != 0) && (values != 0)) ? TT_OK : TT_ERR_NOMEM;
	for (int i = 0; (status == TT_OK) && (i < n); i++) {
		status = _ttdt_arg_string( msg, 2 * i, _TTDT_MAX_VARIABLE_NAME,
					   &names[ i ] );
		if (status == TT_OK) {
			if (! _ttdt_variable_name_valid( names[ i ] )) {
				status = TT_DESKTOP_EINVAL;
			} else if (_tt_env_name_unsafe( names[ i ] )) {
				_tt_syslog( 0, LOG_WARNING, "not setting $%s "
					    "for a Set_Environment request",
					    names[ i ] );
				status = TT_DESKTOP_EACCES;
			}
		}
		if ((status == TT_OK) && _tttk_message_arg_is_set( msg, 2*i+1 )) {
			status = _ttdt_arg_string( msg, 2 * i + 1,
						   _TTDT_MAX_VARIABLE_VALUE,
						   &values[ i ] );
		}
	}
	for (int i = 0; (status == TT_OK) && (i < n); i++) {
		status = _ttdt_adopt_variable( names[ i ], values[ i ] );
	}
	for (int i = 0; (names != 0) && (i < n); i++) {
		tt_free( names[ i ] );
	}
	for (int i = 0; (values != 0) && (i < n); i++) {
		tt_free( values[ i ] );
	}
	free( names );
	free( values );
	return status;
}

//
// Handle a POSIX-related Desktop request
//
Tt_message
_ttdt_posix_cb(
	Tt_message	msg,
	Tt_pattern	pat,
	void	       *,
	void	       *
)
{	char *opString = tt_message_op( msg );
	Tttk_op op = tttk_string_op( opString );
	tt_free( opString );
	int numArgs = tt_message_args_count( msg );
	Tt_status status = tt_int_error( numArgs );
	if (status != TT_OK) {
		return (Tt_message)tt_error_pointer( status );
	}
	switch (op) {
		char	       *categoryName, *variable, *value;
		int		category, i;
		struct utsname	names;
#if defined(OPT_SYSINFO)		
		char		buf[ SYS_NMLN ];
#endif
	    case TTDT_SET_LOCALE:
		return _ttDtGetLocaleCB( msg, pat, (void *)_ttDtApplyLocale, 0 );
	    case TTDT_GET_LOCALE:
		if (numArgs / 2 > _TTDT_MAX_LOCALE_PAIRS) {
			tttk_message_fail( msg, TT_DESKTOP_EINVAL, 0, 1 );
			return 0;
		}
		for (i = 0; i < numArgs/2; i++) {
			status = _ttdt_arg_string( msg, 2 * i,
					_TTDT_MAX_CATEGORY_NAME, &categoryName );
			if (status != TT_OK) {
				tttk_message_fail( msg, status, 0, 1 );
				return 0;
			}
			category = _ttdt_category( categoryName );
			if (category != -1) {
				tt_message_arg_val_set( msg, 2 * i + 1,
						setlocale( category, 0 ));
			}
			tt_free( categoryName );
		}
		tt_message_reply( msg );
		tttk_message_destroy( msg );
		return 0;
	    case TTDT_GET_ENVIRONMENT:
		if (numArgs / 2 > _TTDT_MAX_VARIABLES) {
			tttk_message_fail( msg, TT_DESKTOP_EINVAL, 0, 1 );
			return 0;
		}
		for (i = 0; i < numArgs/2; i++) {
			status = _ttdt_arg_string( msg, 2 * i,
					_TTDT_MAX_VARIABLE_NAME, &variable );
			if ((status == TT_OK) &&
			    ! _ttdt_variable_name_valid( variable )) {
				status = TT_DESKTOP_EINVAL;
			}
			if (status != TT_OK) {
				tt_free( variable );
				tttk_message_fail( msg, status, 0, 1 );
				return 0;
			}
			tt_message_arg_val_set( msg, 2 * i + 1, getenv( variable ));
			tt_free( variable );
		}
		tt_message_reply( msg );
		tttk_message_destroy( msg );
		return 0;
	    case TTDT_SET_ENVIRONMENT:
		status = _ttDtSetEnvironment( msg, numArgs / 2 );
		if (status != TT_OK) {
			tttk_message_fail( msg, status, 0, 1 );
			return 0;
		}
		tt_message_reply( msg );
		tttk_message_destroy( msg );
		return 0;
	    case TTDT_SET_SITUATION:
		value = 0;
		if ((numArgs > 0) && _tttk_message_arg_is_set( msg, 0 )) {
			status = _ttdt_arg_string( msg, 0, PATH_MAX, &value );
		} else {
			// the file of the message is the directory
			value = tt_message_file( msg );
			status = tt_ptr_error( value );
			if (status != TT_OK) {
				value = 0;
			} else if (value == 0) {
				status = TT_DESKTOP_EPROTO;
			}
		}
		if (status == TT_OK) {
			status = _ttdt_adopt_directory( value );
		}
		tt_free( value );
		if (status != TT_OK) {
			tttk_message_fail( msg, status, 0, 1 );
			return 0;
		}
		tt_message_reply( msg );
		tttk_message_destroy( msg );
		return 0;
	    case TTDT_GET_SITUATION:
		value = getcwd( 0, MAXPATHLEN );
		if (value == 0) {
			tttk_message_fail( msg, _tt_errno_status( errno ),
					   0, 1 );
			return 0;
		}
		status = tt_message_arg_val_set( msg, 0, value );
		free( value );
		if (status != TT_OK) {
			tttk_message_fail( msg, status, 0, 1 );
			return 0;
		}
		tt_message_reply( msg );
		tttk_message_destroy( msg );
		return 0;
	    case TTDT_SIGNAL:
		i = _tttk_message_arg_ival( msg, 0, 0 );
		if ((i <= 0) || (i >= NSIG)) {
			tttk_message_fail( msg, TT_DESKTOP_EINVAL, 0, 1 );
			return 0;
		}
		// Reply before actually signalling, in case we die
		tt_message_reply( msg );
		tttk_message_destroy( msg );
		kill( getpid(), i );
		return 0;
	    case TTDT_GET_SYSINFO:
		if (uname( &names ) < 0) {
			tttk_message_fail( msg, _tt_errno_status( errno ),
					   0, 1 );
			return 0;
		}
		// The first 5 values are from uname and seem pretty standard
		tt_message_arg_val_set( msg, 0, names.sysname );
		tt_message_arg_val_set( msg, 1, names.nodename );
		tt_message_arg_val_set( msg, 2, names.release );
		tt_message_arg_val_set( msg, 3, names.version );
		tt_message_arg_val_set( msg, 4, names.machine );
		// The last 3 are from sysinfo which seems to be SVR4 only.
		// For platforms without the sysinfo call, we just leave
		// the values unset for now
#if defined(OPT_SYSINFO)		
		if (sysinfo( SI_ARCHITECTURE, buf, SYS_NMLN ) >= 0) {
			tt_message_arg_val_set( msg, 5, buf );
		}
		if (sysinfo( SI_HW_PROVIDER, buf, SYS_NMLN ) >= 0) {
			tt_message_arg_val_set( msg, 6, buf );
		}
		if (sysinfo( SI_HW_SERIAL, buf, SYS_NMLN ) >= 0) {
			tt_message_arg_val_set( msg, 7, buf );
		}
#endif
		tt_message_reply( msg );
		tttk_message_destroy( msg );
		return 0;
	}
	return msg;
}

//
// Pattern callback for Raise, Lower requests
//
Tt_message
_ttdt_do_RaiseLower(
	Tt_message	msg,
	void	       *_widget,
	Tt_message
)
{
	Widget widget = _ttdt_realized_widget( _widget, 0 );
	if (widget == 0) {
		tttk_message_fail( msg, TT_DESKTOP_ENOTSUP, 0, 1 );
		return 0;
	}
	char *ops = tt_message_op( msg );
	Tttk_op op = tttk_string_op( ops );
	tt_free( ops );
	Display		       *dpy = (Display *)CALLXT(XtDisplay)( widget );
	Window			win = CALLXT(XtWindow)( widget );
	switch (op) {
	    case TTDT_RAISE:
		CALLX11(XRaiseWindow)( dpy, win );
		break;
	    case TTDT_LOWER:
		CALLX11(XLowerWindow)( dpy, win );
		break;
	    default:
		tttk_message_fail( msg, TT_DESKTOP_ENOTSUP, 0, 1 );
		return 0;
	}
	_tttk_message_reply( msg );
	tttk_message_destroy( msg );
	return 0;
}
