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

#ifdef HAVE_CONFIG_H
#include <cde_config.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <EUSCompat.h>
#include "cmxdrlist.h"

extern bool_t
_DtCm_xdr_limited_string(XDR *xdrs, char **sp, u_int maxlen)
{
	if (xdrs->x_op == XDR_ENCODE && *sp == NULL) {
		char empty[1] = { '\0' };
		char *none = empty;

		return (xdr_string(xdrs, &none, maxlen));
	}

	return (xdr_string(xdrs, sp, maxlen));
}

extern bool_t
_DtCm_xdr_limited_array(XDR *xdrs, char **arrayp, u_int *countp,
	u_int maxcount, u_int elsize, xdrproc_t proc)
{
	if (xdrs->x_op == XDR_ENCODE &&
	    (*countp > maxcount || (*countp > 0 && *arrayp == NULL)))
		return (FALSE);

	return (xdr_array(xdrs, arrayp, countp, maxcount, elsize, proc));
}

static void *
get_next(const void *node, size_t offset)
{
	void *next;

	memcpy(&next, (const char *)node + offset, sizeof(next));
	return (next);
}

static void
set_next(void *node, size_t offset, void *next)
{
	memcpy((char *)node + offset, &next, sizeof(next));
}

/* Reverse the list in place; returns the new first node. */
static void *
reverse(void *head, size_t offset)
{
	void *previous = NULL;

	while (head != NULL) {
		void *next = get_next(head, offset);

		set_next(head, offset, previous);
		previous = head;
		head = next;
	}
	return (previous);
}

/*
 * The members after `next' come in the order opposite to the list: the
 * routine is called for the last node first.  The list is turned around
 * while it is done and always turned back.
 */
static bool_t
code_after_parts(XDR *xdrs, void *head, const _DtCm_xdr_list_type *type)
{
	void *last = reverse(head, type->next_offset);
	void *node;
	bool_t ok = TRUE;

	for (node = last; node != NULL && ok;
	    node = get_next(node, type->next_offset))
		ok = type->after(xdrs, node);

	(void)reverse(last, type->next_offset);
	return (ok);
}

static bool_t
free_list(XDR *xdrs, void *head, const _DtCm_xdr_list_type *type)
{
	void *node = head;

	while (node != NULL) {
		void *next = get_next(node, type->next_offset);

		if (type->before != NULL)
			(void)type->before(xdrs, node);
		if (type->after != NULL)
			(void)type->after(xdrs, node);
		if (node != head)
			free(node);
		node = next;
	}
	return (TRUE);
}

extern bool_t
_DtCm_xdr_list(XDR *xdrs, void *head, const _DtCm_xdr_list_type *type)
{
	void *node, *added;
	u_int count = 0;
	bool_t more;

	switch (xdrs->x_op) {
	case XDR_FREE:
		return (free_list(xdrs, head, type));

	case XDR_ENCODE:
		for (node = head; node != NULL;
		    node = get_next(node, type->next_offset)) {
			if (++count > type->limit)
				return (FALSE);
			if (type->before != NULL &&
			    !type->before(xdrs, node))
				return (FALSE);
			more = get_next(node, type->next_offset) != NULL;
			if (!xdr_bool(xdrs, &more))
				return (FALSE);
		}
		if (type->after != NULL)
			return (code_after_parts(xdrs, head, type));
		return (TRUE);

	case XDR_DECODE:
		node = head;
		set_next(node, type->next_offset, NULL);
		for (;;) {
			count++;
			if (type->before != NULL &&
			    !type->before(xdrs, node))
				return (FALSE);
			if (!xdr_bool(xdrs, &more))
				return (FALSE);
			if (!more)
				break;
			if (count >= type->limit)
				return (FALSE);
			if ((added = calloc(1, type->size)) == NULL)
				return (FALSE);
			/* linked in before it is read, so that it is freed
			 * with the others when decoding fails */
			set_next(node, type->next_offset, added);
			node = added;
		}
		if (type->after != NULL)
			return (code_after_parts(xdrs, head, type));
		return (TRUE);
	}

	return (FALSE);
}
