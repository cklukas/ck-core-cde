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
//%%  $TOG: tttk.C /main/5 1999/09/14 13:00:44 mgreess $ 			 				
#if HAVE_CONFIG_H
#include <cde_config.h>
#endif
#if HAVE_POLL_H
# include <poll.h>
#elif HAVE_SYS_POLL_H
# include <sys/poll.h>
#else
# include <poll.h>
#endif
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include "Tt/tt_c.h"
#include "util/tt_gettext.h"
#include "util/tt_Xlib.h"
#include "util/tt_port.h"
#include "Tt/tttk.h"
#include "tttk/tttk2free.h"
#include "tttk/tttkutils.h"
#include "tttk/tttkpattern.h"
#include "tttk/ttdtprocid.h"
#include "tttk/ttdesktop.h"

extern Tt_status	_tt_errno_status( int err_no );

const char  *Tttk_integer		= "integer";
const char  *Tttk_string		= "string";
const char  *Tttk_boolean		= "boolean";
const char  *Tttk_file			= "File";
const char  *Tttk_message_id		= "messageID";
const char  *Tttk_title			= "title";
const char  *Tttk_width			= "width";
const char  *Tttk_height		= "height";
const char  *Tttk_xoffset		= "xOffset";
const char  *Tttk_yoffset		= "yOffset";

_TtDtProcid *_ttdtme			= 0;

// API calls below...

char *
ttdt_open(
	int	       *ttFD,
	const char     *toolName,
	const char     *vendor,
	const char     *version,
	int		sendStarted
)
{
	_ttdtme = new _TtDtProcid( toolName, vendor, version );
	if (_ttdtme == 0) {
		return (char *)tt_error_pointer( TT_ERR_NOMEM );
	}
	char *procID = tt_open();
	Tt_status status = tt_ptr_error( procID );
	if (status != TT_OK) {
		return procID;
	}
	*ttFD = tt_fd();
	status = tt_int_error( *ttFD );
	if (status != TT_OK) {
		tt_free( procID );
		tt_close();
		return (char *)tt_error_pointer( status );
	}
	if (sendStarted) {
		ttdt_Started( 0, toolName, vendor, version, 1 );
	}
	return procID;
}

Tt_status
ttdt_close(
	const char     *procid,
	const char     *newProcid,
	int		sendStopped
)
{
	Tt_status status;
	if (procid != 0) {
		status = tt_default_procid_set( procid );
		if (status != TT_OK) {
			return status;
		}
	}
	if (sendStopped) {
		ttdt_Stopped( 0, _ttdtme->toolname(), _ttdtme->vendor(),
			      _ttdtme->version(), 1 );
	}
	status = tt_close();
	if (status != TT_OK) {
		return status;
	}
	if (newProcid != 0) {
		status = tt_default_procid_set( newProcid );
		if (status != TT_OK) {
			return status;
		}
	}
	return TT_OK;
}

Tt_pattern *
ttdt_session_join(
	const char	       *sessid,
	Ttdt_contract_cb	cb,
	Widget			shell,
	void		       *clientdata,
	int			join,
	Ttdt_session_requests	requests
)
{
	Tt_status	status;
	_TttkItem2Free	item2free;
	if (sessid == 0) {
		sessid = tt_default_session();
		status = tt_ptr_error( sessid );
		if (status != TT_OK) {
			return (Tt_pattern *)tt_error_pointer( status );
		}
		item2free = (char *)sessid;
	}
	Tt_pattern *pats = _ttdtme->pats_create( 0, cb, shell, clientdata,
						 requests );
	status = tt_ptr_error( pats );
	if (status != TT_OK) {
		return pats;
	}
	if (join) {
		status = tt_session_join( sessid );
		if (status != TT_OK) {
			_tttk_patterns_destroy( pats );
			return (Tt_pattern *)tt_error_pointer( status );
		}
	}
	return pats;
}

Tt_status
ttdt_session_quit(
	const char     *sessid,
	Tt_pattern     *pats,
	int		quit
)
{
	Tt_status	status;
	_TttkItem2Free	item2free;
	if (sessid == 0) {
		sessid = tt_default_session();
		status = tt_ptr_error( sessid );
		if (status != TT_OK) {
			return status;
		}
		item2free = (char *)sessid;
	}
	status = _tttk_patterns_destroy( pats );
	if (status != TT_OK) {
		return status;
	}
	if (quit) {
		status = tt_session_quit( sessid );
		if (status != TT_OK) {
			return status;
		}
	}
	return status;
}

Tt_pattern *
ttdt_message_accept(
	Tt_message	contract,
	Ttdt_contract_cb cb,
	Widget		shell,
	void	       *clientData,
	int		accept,
	int		sendStatus,
	Ttdt_contract_requests requests
)
{
	Tt_status status;
	// nothing is accepted for a request that cannot be answered
	if ((requests & ~(Ttdt_contract_requests)TTDT_CONTRACT_SET_XINFO) != 0) {
		return (Tt_pattern *)tt_error_pointer( TT_ERR_NUM );
	}
	if (((requests & TTDT_CONTRACT_SET_XINFO) != 0) && (shell == 0) &&
	    (cb == 0)) {
		return (Tt_pattern *)tt_error_pointer( TT_ERR_POINTER );
	}
	if (accept) {
		status = tt_message_accept( contract );
		if (status != TT_OK) {
			// Not fatal
		}
	}
	Tt_pattern *pats = _ttdtme->pats_create( contract, cb, shell,
			clientData, ((requests & TTDT_CONTRACT_SET_XINFO) != 0) ?
				TTDT_SESSION_SET_XINFO :
				TTDT_SESSION_NO_REQUESTS );
	status = tt_ptr_error( pats );
	if (status == TT_OK) {
		tt_message_user_set( contract, _TttkContractKey, pats );
	}
	if (sendStatus) {
		ttdt_Status( contract, contract,
			     catgets( _ttcatd, 1, 24, "Accepting request" ),
			     _ttdtme->toolname(), _ttdtme->vendor(),
			     _ttdtme->version(), 1 );
	}
	return pats;
}

Tt_pattern *
ttdt_subcontract_manage(
	Tt_message		subcontract,
	Ttdt_contract_cb	cb,
	Widget			shell,
	void		       *clientData
)
{
	const int numPats = 4;
	Tt_pattern *pats = (Tt_pattern *)malloc(numPats * sizeof(Tt_pattern));
	if (pats == 0) {
		return (Tt_pattern *)tt_error_pointer( TT_ERR_NOMEM );
	}
	pats[ 0 ] = 0;
	if (shell != 0) {
		pats[0] = ttdt_Get_Geometry_pat( TT_HANDLE, subcontract,
				_ttdt_do_GSet_Geometry, shell, 1 );
		pats[1] = ttdt_Get_XInfo_pat( TT_HANDLE, subcontract,
				_ttdt_do_Get_XInfo, shell, 1 );
	} else {
		pats[0] = ttdt_Get_Geometry_pat( TT_HANDLE, subcontract,
				(Ttdt_Geometry_out_cb)cb, clientData, 1 );
		pats[1] = ttdt_Get_XInfo_pat(	TT_HANDLE, subcontract,
				(Ttdt_XInfo_out_cb)cb, clientData, 1 );
	}
	pats[2] = _ttdt_pat(	TTDT_STATUS, _ttdt_contract_cb, TT_OBSERVE,
				subcontract, (void *)cb, clientData, 1 );
	pats[ numPats - 1 ] = 0;
	for (int i = 0; i < numPats; i++) {
		Tt_status status = tt_ptr_error( pats[ i ] );
		if (status != TT_OK) {
			_tttk_patterns_destroy( pats );
			free( pats );
			return (Tt_pattern *)tt_error_pointer( status );
		}
	}
	tt_message_user_set( subcontract, _TttkSubContractKey, pats );
	return pats;
}

void
tttk_Xt_input_handler(
	XtPointer  procid,
	int       *,
	XtInputId *id
)
{
	static const char *here = "ttdt_Xt_input_handler()";

	Tt_message msg = _tttk_message_receive( (char *)procid );
	Tt_status status = tt_ptr_error( msg );
	if (status != TT_OK) {
		_ttDtPrintStatus( here, "tttk_message_receive()", status);
		if (status == TT_ERR_NOMP) {
			ttdt_close( (const char *)procid, 0, 0 );
			if (! _tt_load_xt()) {
				return;
			}
			if (id != 0) {
				CALLXT(XtRemoveInput)( *id );
			}
		}
		return;
	}
	if (msg == 0) {
		return;
	}
	status = tttk_message_abandon( msg );
	if (status != TT_OK) {
		_ttDtPrintStatus( here, "tttk_message_abandon()", status );
	}
}

Tt_status
_tttk_block_procid_while(
	const int     *blocked,
	int            msTimeOut
)
{
	int fd = tt_fd();
	Tt_status status = tt_int_error( fd );
	if (status != TT_OK) {
		return status;
	}
	while ((blocked == 0) || (*blocked > 0)) {
		struct pollfd fds[ 1 ];
		fds[0].fd = fd;
		fds[0].events = POLLIN;
		int activeFDs = poll( fds, 1, msTimeOut );
		if (activeFDs == 0) {
			return TT_DESKTOP_ETIMEDOUT;
		} else if (activeFDs < 0) {
              if ((blocked != 0) && (*blocked)) {
				_ttDtPError( "_tttk_block_procid_while()",
					     "poll()" );
				return _tt_errno_status( errno );
			}
		}
		if (fds[ 0 ].revents != 0) {
			//
			// AIX has fd in struct pollfd as a long, not
			// an int, and complains (justifiably) when
			// long* is passed when int* is needed. So use
			// call-by-value-return instead of
			// call-by-reference.  Hey,
			// tttk_Xt_input_handler doesn\'t use the
			// value anyway...
			//
			int fd_temp;
			fd_temp = fds[0].fd;
			tttk_Xt_input_handler( 0, &fd_temp, 0 );
			fds[0].fd = fd_temp;
		}
		if (msTimeOut == 0) {
			return TT_OK;
		}
         if (msTimeOut == 0) {
            return TT_OK;
         }
     }
	return TT_OK;
}

void
_tttk_timed_out(
	XtPointer	p_timed_out,
	XtIntervalId *
)
{
	*(int *)p_timed_out = 1;
}

Tt_status
_tttk_block_app_while(
	XtAppContext	app2run,
	const int     *blocked,
	int            msTimeOut
)
{
	XtIntervalId alarm = 0;
     if ((blocked != 0) && (*blocked <= 0)) return TT_OK;
	int timed_out = (msTimeOut == 0);
	if ((! _tt_load_xt()) || (! _tt_load_xlib())) {
		return TT_ERR_ACCESS; // i.e. ELIBACC
	}
	if ((timed_out) && (! CALLXT(XtAppPending)( app2run ))) {
		//
		// We are non-blocking and no input is available,
		// so return.
		//
		return TT_DESKTOP_ETIMEDOUT;
	}
	if (msTimeOut > 0) {
		alarm = CALLXT(XtAppAddTimeOut)( app2run,
					(unsigned long)msTimeOut,
					_tttk_timed_out,
					&timed_out );
	}
	do {
		CALLXT(XtAppProcessEvent)( app2run, XtIMAll );
	} while ((! timed_out) && ((blocked == 0) || (*blocked > 0)));
	if (alarm != 0) {
		CALLXT(XtRemoveTimeOut)( alarm );
	}
	if (timed_out && (msTimeOut > 0)) {
		return TT_DESKTOP_ETIMEDOUT;
	}
	return TT_OK;
}

Tt_status
tttk_block_while(
	XtAppContext	app2run,
	const int     *blocked,
	int            msTimeOut
)
{
	if (app2run == 0) {
		return _tttk_block_procid_while( blocked, msTimeOut );
	} else {
		return _tttk_block_app_while( app2run, blocked, msTimeOut );
	}
}
