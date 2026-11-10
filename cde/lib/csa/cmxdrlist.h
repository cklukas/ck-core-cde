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

#ifndef _CMXDRLIST_H
#define _CMXDRLIST_H

#include <stddef.h>
#include <rpc/rpc.h>
#include <EUSCompat.h>
#include "cmlimits.h"

/*
 * Bounded strings, arrays and linked lists for the XDR routines of the
 * calendar protocols.
 */

/*
 * A string of at most MAXLEN characters.  A string that is not there
 * (a null pointer) is sent as the empty string.
 */
extern bool_t _DtCm_xdr_limited_string(XDR *xdrs, char **sp, u_int maxlen);

/*
 * An array of at most MAXCOUNT elements; like xdr_array, but the array may
 * not be missing when it has elements, and the count is bounded on encoding
 * as well as on decoding.
 */
extern bool_t _DtCm_xdr_limited_array(XDR *xdrs, char **arrayp, u_int *countp,
		u_int maxcount, u_int elsize, xdrproc_t proc);

/*
 * Singly linked lists.  The XDR form of a list is that of a pointer to a
 * node whose `next' member is a pointer to the next node: the fields of a
 * node that precede `next' come first, then whether another node follows,
 * then the next node, and last the fields of the node that follow `next'.
 * The routines of the nodes would nest as deep as the list is long, so the
 * list is coded in a loop instead, with the same bytes on the wire.
 */
typedef bool_t (*_DtCm_xdr_node_part)(XDR *xdrs, void *node);

typedef struct {
	size_t			size;		/* of a node */
	size_t			next_offset;	/* of the `next' member */
	_DtCm_xdr_node_part	before;		/* members before `next', or NULL */
	_DtCm_xdr_node_part	after;		/* members after `next', or NULL */
	u_int			limit;		/* nodes in the longest list */
} _DtCm_xdr_list_type;

/*
 * Codes the list that starts with HEAD, a node that exists already (the
 * routine of a pointer to a node allocates it).  When decoding it allocates
 * the other nodes, and when freeing it frees them and the members of all
 * nodes, but not HEAD itself.
 */
extern bool_t _DtCm_xdr_list(XDR *xdrs, void *head,
		const _DtCm_xdr_list_type *type);

#endif /* _CMXDRLIST_H */
