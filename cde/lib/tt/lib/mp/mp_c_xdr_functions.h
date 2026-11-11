/*
 *
 * mp_c_xdr_functions.h
 *
 * Client-side only xdr functions.
 *
 * What the server sends is decoded into the classes of the client, so that
 * every message a client holds is a _Tt_c_message.  The xdr routines of the
 * shared classes construct the shared classes, which a client must not cast
 * to its own.  The wire format is that of mp_xdr_functions.h, which the
 * server encodes with.
 */
#ifndef MP_C_XDR_FUNCTIONS_H
#define MP_C_XDR_FUNCTIONS_H
#include <rpc/rpc.h>
#include "util/tt_string.h"
#include "mp/mp_c_message_utils.h"

struct _Tt_c_next_message_args: public _Tt_allocated {
	_Tt_c_next_message_args() : clear_signal(0) {}
	_Tt_c_message_list_ptr	msgs;
	int			clear_signal;
};

bool_t		tt_c_xdr_next_message_args(XDR *xdrs,
					   _Tt_c_next_message_args *args);
#endif				/* MP_C_XDR_FUNCTIONS_H */
