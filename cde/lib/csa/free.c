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
/* $XConsortium: free.c /main/1 1996/04/21 19:23:16 drk $ */
/*
 *  (c) Copyright 1993, 1994 Hewlett-Packard Company
 *  (c) Copyright 1993, 1994 International Business Machines Corp.
 *  (c) Copyright 1993, 1994 Novell, Inc.
 *  (c) Copyright 1993, 1994 Sun Microsystems, Inc.
 */

#include <EUSCompat.h>
#include <stdckdint.h>
#include <stdlib.h>
#include "csa.h"
#include "free.h"

#define _DtCM_OUR_MEMORY	0x55555555

typedef void (*_DtCmFreeProc)(uint num_elem, void *ptr);

typedef struct {
	int		mark;
	uint		num_elem;
	_DtCmFreeProc	fproc;
} _DtCmMemHeader;

/******************************************************************************
 * forward declaration of static functions used within the file
 ******************************************************************************/
static void _DtCm_free_calendar_users(uint num_elem, void *vptr);

/*****************************************************************************
 * extern functions used in the library
 *****************************************************************************/

/*
 * Allocate a zeroed block of num_elem elements of elem_size bytes each,
 * preceded by a memory header.  Returns a pointer to the first element.
 */
static void *
alloc_counted(uint num_elem, size_t elem_size, _DtCmFreeProc fproc)
{
	size_t		size;
	char		*ptr;
	_DtCmMemHeader	*hptr;

	if (ckd_mul(&size, (size_t)num_elem, elem_size) ||
	    ckd_add(&size, size, sizeof(_DtCmMemHeader)) ||
	    (ptr = calloc(1, size)) == nullptr)
		return (nullptr);

	hptr = (_DtCmMemHeader *)ptr;
	hptr->mark = _DtCM_OUR_MEMORY;
	hptr->num_elem = num_elem;
	hptr->fproc = fproc;
	return (ptr + sizeof(_DtCmMemHeader));
}

extern CSA_calendar_user *
_DtCm_alloc_calendar_users(uint num_elem)
{
	return (alloc_counted(num_elem, sizeof(CSA_calendar_user),
	    _DtCm_free_calendar_users));
}

extern char **
_DtCm_alloc_character_pointers(uint num_elem)
{
	return (alloc_counted(num_elem, sizeof(char *),
	    _DtCm_free_character_pointers));
}

extern CSA_attribute *
_DtCm_alloc_attributes(uint num_elem)
{
	extern void _DtCm_free_attributes(uint size, void *attrs);

	return (alloc_counted(num_elem, sizeof(CSA_attribute),
	    _DtCm_free_attributes));
}

extern CSA_entry_handle *
_DtCm_alloc_entry_handles(uint num_elem)
{
	extern void _DtCm_free_entry_handles(uint num_entries, void *entries);

	return (alloc_counted(num_elem, sizeof(CSA_entry_handle),
	    _DtCm_free_entry_handles));
}

extern void *
_DtCm_alloc_entry(uint size)
{
	extern void _DtCm_free_entry_content(uint dummy, void *entry);

	return (alloc_counted(1, size, _DtCm_free_entry_content));
}

extern CSA_reminder_reference *
_DtCm_alloc_reminder_references(uint num_elem)
{
	extern void _DtCm_free_reminder_references(uint num_rems, void *rems);

	return (alloc_counted(num_elem, sizeof(CSA_reminder_reference),
	    _DtCm_free_reminder_references));
}

extern CSA_return_code
_DtCm_free(void *ptr)
{
	_DtCmMemHeader *hptr;

	if (ptr == nullptr)
		return (CSA_E_INVALID_MEMORY);

	hptr = (_DtCmMemHeader *)(((char *)ptr) - sizeof(_DtCmMemHeader));

	if (hptr->mark != _DtCM_OUR_MEMORY)
		return (CSA_E_INVALID_MEMORY);
	else {
		hptr->mark = 0;
		hptr->fproc(hptr->num_elem, ptr);
		free(hptr);
		return (CSA_SUCCESS);
	}
}

extern void
_DtCm_free_character_pointers(uint num_elem, void *vptr)
{
	char **ptr = (char **)vptr;
	int	i;

	for (i = 0; i < num_elem; i++) {
		if (ptr[i])
			free(ptr[i]);
		else
			break;
	}
}

/******************************************************************************
 * static functions used within in the file
 ******************************************************************************/

static void
_DtCm_free_calendar_users(uint num_elem, void *vptr)
{
	CSA_calendar_user *ptr = (CSA_calendar_user *)vptr;
	int	i;

	for (i = 0; i < num_elem; i++) {
		if (ptr[i].user_name)
			free(ptr[i].user_name);

		if (ptr[i].calendar_address)
			free(ptr[i].calendar_address);
	}
}

