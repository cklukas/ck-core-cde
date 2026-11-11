/*
 *
 * mp_c_xdr_functions.C
 *
 * Client-side only xdr functions (see mp_c_xdr_functions.h).
 */
#include "mp/mp_c_xdr_functions.h"
#include "mp/mp_c_message.h"

bool_t
tt_c_xdr_next_message_args(XDR *xdrs, _Tt_c_next_message_args *args)
{
	return(args->msgs.xdr(xdrs) &&
	       xdr_int(xdrs, (int *)&args->clear_signal));
}
