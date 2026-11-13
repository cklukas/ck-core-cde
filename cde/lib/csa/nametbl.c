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
/* $XConsortium: nametbl.c /main/1 1996/04/21 19:24:03 drk $ */
/*
 *  (c) Copyright 1993, 1994 Hewlett-Packard Company
 *  (c) Copyright 1993, 1994 International Business Machines Corp.
 *  (c) Copyright 1993, 1994 Novell, Inc.
 *  (c) Copyright 1993, 1994 Sun Microsystems, Inc.
 */

#include <EUSCompat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nametbl.h"
#include "hash.h"

#define	MAX_CAL_TBL_SIZE	23
#define MAX_ENTRY_TBL_SIZE	37

extern _DtCmNameTable *
_DtCm_make_name_table(int size, char **names)
{
	_DtCmNameTable *tbl;
	int i;

	if ((tbl = (_DtCmNameTable *)calloc(1, sizeof(_DtCmNameTable))) == nullptr)
		return (nullptr);

	if ((tbl->tbl = _DtCmMakeHash(MAX_CAL_TBL_SIZE)) == nullptr) {
		free(tbl);
		return (nullptr);
	}

	if ((tbl->names = (char **)calloc((size+1), sizeof(char *))) == nullptr) {
		_DtCm_free_name_table(tbl);
		return (nullptr);
	}

	for (i = 1; i <= size; i++) {
		if ((tbl->names[i] = strdup(names[i])) == nullptr) {
			tbl->size = i;
			_DtCm_free_name_table(tbl);
			return (nullptr);
		}

		*(int *)_DtCmGetHash(tbl->tbl, (unsigned char *)names[i]) = i;
	}

	tbl->size = i - 1;

	return (tbl);
}

extern void
_DtCm_free_name_table(_DtCmNameTable *tbl)
{
	int	i;

	if (tbl == nullptr) return;

	if (tbl->tbl)
		_DtCmDestroyHash(tbl->tbl, nullptr, nullptr);

	for (i = 1; i <= tbl->size; i++)
		free(tbl->names[i]);

	if (tbl->names)
		free(tbl->names);

	free(tbl);
}

/*
 * if index == 0, then add one more element to the table
 * otherwise use the index to add the new name and extend the
 * table by (index - size)
 */
extern CSA_return_code
_DtCm_add_name_to_table(_DtCmNameTable *tbl, int index, char *newname)
{
	int	*ptr;
	char	**newptr;

	if (index > 0 && index <= tbl->size && tbl->names[index])
		return (CSA_E_INVALID_PARAMETER);

	if (index == 0)
		index = tbl->size + 1;

	/* add new name to table */
	ptr = (int *)_DtCmGetHash(tbl->tbl, (unsigned char *)newname);
	if (ptr)
		*ptr = index;

	if (index > tbl->size) {
		if ((newptr = (char **)reallocarray(tbl->names, (index + 1), sizeof(char *))) == nullptr) {
			*ptr = -1;
			return (CSA_E_INSUFFICIENT_MEMORY);
		} else {
			tbl->names = newptr;
			memset((void *)&tbl->names[tbl->size+1], 0,
				sizeof(char *)*(index - tbl->size));
		}
	}

	if ((tbl->names[index] = strdup(newname)) == nullptr) {

		*ptr = -1;
		return (CSA_E_INSUFFICIENT_MEMORY);

	} else {
		if (index > tbl->size)
			tbl->size = index;
	}

	return (CSA_SUCCESS);
}

extern int
_DtCm_get_index_from_table(_DtCmNameTable *tbl, char *name)
{
	int *ptr;

	ptr = (int *)_DtCmFindHash(tbl->tbl, (unsigned char *)name);

	if (ptr)
		return (*ptr);
	else
		return (-1);
}

/*
 * if index == 0, then add one more element to the table
 * other use the index to add the new name and extend the
 * table by (index - size)
 */
extern CSA_return_code
_DtCmExtendNameTable(
	char		*name,
	int		index,
	int		type,
	_DtCmNameTable	*base,
	int		basesize,
	char		**basenames,
	_DtCmNameTable	**tbl,
	int		**types)
{
	_DtCmNameTable	*ntbl;
	int		*newarray;
	int		newindex;

	if (index > 0 && index <= (*tbl)->size)
		return (CSA_E_INVALID_PARAMETER);

	if (*tbl == base) {
		if ((ntbl = _DtCm_make_name_table(basesize, basenames)) == nullptr)
		{

			return (CSA_E_INSUFFICIENT_MEMORY);
		} else
			*tbl = ntbl;
	}

	if (types) {
		newindex = (index == 0) ? (*tbl)->size+1 : index;
		if ((newarray = (int *)reallocarray(*types, (newindex+1), sizeof(int))) == nullptr) {
			return (CSA_E_INSUFFICIENT_MEMORY);
		} else {
			*types = newarray;
			(*types)[newindex] = type;
		}
	}

	return (_DtCm_add_name_to_table(*tbl, index, name));
}

