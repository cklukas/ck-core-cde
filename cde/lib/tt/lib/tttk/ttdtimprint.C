/*
 * ttdtimprint.C
 *
 * ttdt_sender_imprint_on(): make the calling process act like a child of
 * another tool.
 *
 * The calling process is changed in the ways the caller names and no other
 * (Ttdt_sender_imprints).  Only the requests that the named ways and the
 * values the caller asks to be returned need are sent, addressed to the
 * handler.  The replies are checked and written down; when the call has
 * stopped waiting for them, the values that passed are put into the process
 * or handed to the caller.  A reply that comes after that finds its request
 * gone and changes nothing.
 */
#include "tt_options.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "Tt/tt_c.h"
#include "Tt/tttk.h"
#include "util/tt_port.h"
#include "tttk/tttk2free.h"
#include "tttk/tttkmessage.h"
#include "tttk/tttkutils.h"
#include "tttk/ttdesktop.h"
#include "tttk/ttdtprocess.h"

static const Ttdt_sender_imprints _ttDtKnownImprints =
	TTDT_IMPRINT_LOCALE | TTDT_IMPRINT_SITUATION | TTDT_IMPRINT_DISPLAY;

void
_ttDtImprintReplied(
	ImprintInfo    *info,
	Tt_message	msg,
	Tt_status	status
)
{
	info->repliesOutStanding--;
	for (int i = 0; i < _TTDT_IMPRINT_REQUESTS; i++) {
		if (info->pending[ i ] == msg) {
			info->pending[ i ] = 0;
		}
	}
	if (status != TT_OK) {
		info->malformed = 1;
	} else if (tt_message_state( msg ) != TT_HANDLED) {
		status = (Tt_status)tt_message_status( msg );
		if (status == TT_OK) {
			status = TT_DESKTOP_EPROTO;
		}
	}
	if ((status != TT_OK) && (info->status == TT_OK)) {
		info->status = status;
	}
}

//
// Write down the geometry of a Get_Geometry reply.  The sizes and offsets of
// a window fit the 16 bits of the X protocol; -1 and INT_MAX stand for what
// the handler did not answer.
//
static Tt_message
_ttDtNoteGeom(
	Tt_message	msg,
	void	       *clientData,
	Tt_message	,
	int		width,
	int		height,
	int		xOffset,
	int		yOffset
)
{
	ImprintInfo *info = (ImprintInfo *)clientData;
	Tt_status status = TT_OK;
	if (tt_message_state( msg ) == TT_HANDLED) {
		int sizesOk = ((width == -1) || ((width >= 0) && (width <= 65535))) &&
			      ((height == -1) || ((height >= 0) && (height <= 65535)));
		int offsetsOk =
			((xOffset == INT_MAX) || ((xOffset >= -32768) && (xOffset <= 32767))) &&
			((yOffset == INT_MAX) || ((yOffset >= -32768) && (yOffset <= 32767)));
		if (sizesOk && offsetsOk) {
			info->width = width;
			info->height = height;
			info->xOffset = xOffset;
			info->yOffset = yOffset;
		} else {
			status = TT_DESKTOP_EPROTO;
		}
	}
	_ttDtImprintReplied( info, msg, status );
	tttk_message_destroy( msg );
	return 0;
}

//
// Write down the display of a Get_XInfo reply, if it has the shape of one.
//
static Tt_message
_ttDtNoteXInfo(
	Tt_message	msg,
	void	       *clientData,
	Tt_message	,
	char           *display,
	int,		//visual
	int		//depth
)
{
	ImprintInfo *info = (ImprintInfo *)clientData;
	Tt_status status = TT_OK;
	if ((display != 0) && (info->display == 0) &&
	    _ttdt_display_valid( display )) {
		info->display = display;
		display = 0;
	} else if (display != 0) {
		status = TT_DESKTOP_EPROTO;
	}
	tt_free( display );
	_ttDtImprintReplied( info, msg, status );
	tttk_message_destroy( msg );
	return 0;
}

//
// Write down the locales of a Get_Locale reply.  The strings are freed by the
// caller when this returns, so they are copied.
//
static Tt_message
_ttDtNoteLocale(
	Tt_message	msg,
	void	       *clientData,
	const char    **categories,
	const char    **locales
)
{
	if (! _tttk_message_in_final_state( msg )) {
		return msg;
	}
	ImprintInfo *info = (ImprintInfo *)clientData;
	Tt_status status = TT_OK;
	int n = 0;
	while (categories[ n ] != 0) {
		if ((_ttdt_category( categories[ n ] ) == -1) ||
		    ((locales[ n ] != 0) && ! _ttdt_locale_valid( locales[ n ] ))) {
			status = TT_DESKTOP_EPROTO;
		}
		n++;
	}
	char **cats = (char **)calloc( n + 1, sizeof(char *) );
	char **locs = (char **)calloc( n + 1, sizeof(char *) );
	if ((cats == 0) || (locs == 0)) {
		status = TT_ERR_NOMEM;
	}
	for (int i = 0; (status == TT_OK) && (i < n); i++) {
		cats[ i ] = strdup( categories[ i ] );
		if (cats[ i ] == 0) {
			status = TT_ERR_NOMEM;
		}
		if ((status == TT_OK) && (locales[ i ] != 0)) {
			locs[ i ] = strdup( locales[ i ] );
			if (locs[ i ] == 0) {
				status = TT_ERR_NOMEM;
			}
		}
	}
	if ((status == TT_OK) && (info->categories == 0)) {
		info->categories = cats;
		info->locales = locs;
	} else {
		for (int i = 0; (cats != 0) && (i < n); i++) {
			free( cats[ i ] );
			if (locs != 0) {
				free( locs[ i ] );
			}
		}
		free( cats );
		free( locs );
	}
	_ttDtImprintReplied( info, msg, status );
	tttk_message_destroy( msg );
	return 0;
}

//
// Write down the directory of a Get_Situation reply, if it has the shape of
// one.
//
static Tt_message
_ttDtNoteSituation(
	Tt_message	msg,
	void	       *clientData,
	char           *cwd
)
{
	ImprintInfo *info = (ImprintInfo *)clientData;
	Tt_status status = TT_OK;
	if (cwd != 0) {
		if ((info->cwd == 0) && _ttdt_directory_valid( cwd )) {
			info->cwd = strdup( cwd );
			if (info->cwd == 0) {
				status = TT_ERR_NOMEM;
			}
		} else {
			status = TT_DESKTOP_EPROTO;
		}
	}
	tt_free( cwd );
	_ttDtImprintReplied( info, msg, status );
	tttk_message_destroy( msg );
	return 0;
}

//
// Write down that a request was sent, or why it was not.
//
static void
_ttDtImprintSent(
	ImprintInfo    *info,
	Tt_message	msg
)
{
	Tt_status status = tt_ptr_error( msg );
	if (status != TT_OK) {
		if (info->status == TT_OK) {
			info->status = status;
		}
		return;
	}
	for (int i = 0; i < _TTDT_IMPRINT_REQUESTS; i++) {
		if (info->pending[ i ] == 0) {
			info->pending[ i ] = msg;
			break;
		}
	}
	info->repliesOutStanding++;
}

Tt_status
ttdt_sender_imprint_on(
	const char     *handler,
	Tt_message	commission,
	Ttdt_sender_imprints imprints,
	char	      **pDisplay,
	int	       *width,
	int	       *height,
	int	       *xOffset,
	int	       *yOffset,
	XtAppContext	app2run,
	int		msTimeOut
)
{
	if ((imprints & ~_ttDtKnownImprints) != 0) {
		return TT_ERR_NUM;
	}
	//
	// The requests go to one handler.  Nobody else may answer them: what
	// is in the replies is put into this process.
	//
	_TttkItem2Free sender;
	if (handler == 0) {
		if (commission == 0) {
			return TT_ERR_POINTER;
		}
		char *senderId = tt_message_sender( commission );
		Tt_status status = tt_ptr_error( senderId );
		if (status != TT_OK) {
			return status;
		}
		sender = senderId;
		handler = senderId;
	}
	ImprintInfo info;
	info.repliesOutStanding = 0;
	info.status = TT_OK;
	info.malformed = 0;
	for (int i = 0; i < _TTDT_IMPRINT_REQUESTS; i++) {
		info.pending[ i ] = 0;
	}
	info.width = -1;
	info.height = -1;
	info.xOffset = INT_MAX;
	info.yOffset = INT_MAX;
	info.display = 0;
	info.cwd = 0;
	info.categories = 0;
	info.locales = 0;

	if ((width != 0) || (height != 0) || (xOffset != 0) || (yOffset != 0)) {
		_ttDtImprintSent( &info, ttdt_Get_Geometry( handler, commission,
					_ttDtNoteGeom, &info, 1 ));
	}
	if ((pDisplay != 0) || ((imprints & TTDT_IMPRINT_DISPLAY) != 0)) {
		// XXX sending XInfo before Geometry causes segv in handler!? Saberize.
		_ttDtImprintSent( &info, ttdt_Get_XInfo( handler, commission,
					_ttDtNoteXInfo, &info, 1 ));
	}
	if ((imprints & TTDT_IMPRINT_LOCALE) != 0) {
		_ttDtImprintSent( &info, ttdt_Get_Locale( handler, commission,
					_ttDtNoteLocale, &info, 0, 1 ));
	}
	if ((imprints & TTDT_IMPRINT_SITUATION) != 0) {
		_ttDtImprintSent( &info, ttdt_Get_Situation( handler, commission,
					_ttDtNoteSituation, &info, 1 ));
	}
	Tt_status status = TT_OK;
	if (info.repliesOutStanding > 0) {
		status = tttk_block_while( app2run, &info.repliesOutStanding,
					   msTimeOut );
	}
	//
	// A request that has not been answered stays in the handle table, since
	// a request is not destroyed before its reply, but it no longer has a
	// callback: the reply, if it comes later, finds nothing to write into.
	//
	for (int i = 0; i < _TTDT_IMPRINT_REQUESTS; i++) {
		if (info.pending[ i ] != 0) {
			tt_message_user_set( info.pending[ i ], _TttkCBKey, 0 );
			info.pending[ i ] = 0;
		}
	}
	//
	// What came, and what the caller named, goes into the process.  A
	// handler that sent arguments that cannot be used is not believed in
	// anything: nothing is put into the process and nothing is returned.
	//
	if (info.malformed) {
		tt_free( info.display );
		info.display = 0;
		info.width = -1;
		info.height = -1;
		info.xOffset = INT_MAX;
		info.yOffset = INT_MAX;
		imprints = TTDT_IMPRINT_NONE;
	}
	Tt_status applied = TT_OK;
	if (((imprints & TTDT_IMPRINT_LOCALE) != 0) && (info.categories != 0)) {
		applied = _ttdt_adopt_locale( info.categories, info.locales );
	}
	if (((imprints & TTDT_IMPRINT_SITUATION) != 0) && (info.cwd != 0) &&
	    (applied == TT_OK)) {
		applied = _ttdt_adopt_directory( info.cwd );
	}
	if (((imprints & TTDT_IMPRINT_DISPLAY) != 0) && (info.display != 0) &&
	    (applied == TT_OK)) {
		applied = _ttdt_adopt_display( info.display );
	}
	for (int i = 0; (info.categories != 0) && (info.categories[ i ] != 0); i++) {
		free( info.categories[ i ] );
		free( info.locales[ i ] );
	}
	free( info.categories );
	free( info.locales );
	free( info.cwd );
	if (pDisplay != 0) {
		*pDisplay = info.display;
	} else {
		tt_free( info.display );
	}
	if (width != 0) {
		*width = info.width;
	}
	if (height != 0) {
		*height = info.height;
	}
	if (xOffset != 0) {
		*xOffset = info.xOffset;
	}
	if (yOffset != 0) {
		*yOffset = info.yOffset;
	}
	if (status != TT_OK) {
		return status;
	}
	if (info.status != TT_OK) {
		return info.status;
	}
	return applied;
}
