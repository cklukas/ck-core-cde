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
/*%%  (c) Copyright 1993, 1994 Hewlett-Packard Company			 */
/*%%  (c) Copyright 1993, 1994 International Business Machines Corp.	 */
/*%%  (c) Copyright 1993, 1994 Sun Microsystems, Inc.			 */
/*%%  (c) Copyright 1993, 1994 Novell, Inc. 				 */
/*%%  $XConsortium: isfname.c /main/3 1995/10/23 11:40:04 rswiston $ 			 				 */
/*
 * Copyright (c) 1988 by Sun Microsystems, Inc.
 */

/*
 * isfname.c
 *
 * Description:
 *	Functions to translate ISAM file name to .rec, .ind, and .var file
 *	names. Also, several auxiliary functions that deal with path names.
 *
 */

#include "isam_impl.h"

/*
 * _isfname_valid(isfname)
 *
 * Return TRUE if the ISAM file name is short enough for all file names
 * derived from it to fit into a MAXPATHLEN buffer.
 */

Bool
_isfname_valid(const char *isfname)
{
    return (strnlen(isfname, ISMAXFNAMELEN + 1) <= ISMAXFNAMELEN) ? TRUE : FALSE;
}

/*
 * _append_suffix(path, size, suffix)
 *
 * Append suffix to the path held in a buffer of size bytes.  If the result
 * would not fit, the path is emptied so that no file with a truncated
 * name can be reached.
 */

static void
_append_suffix(char *path, size_t size, const char *suffix)
{
    size_t	len = strlen(path);
    size_t	suffixlen = strlen(suffix);

    if (len + suffixlen >= size) {
	path[0] = '\0';
	return;
    }
    memcpy(path + len, suffix, suffixlen + 1);
}

/*
 * _makedat_isfname(isfname, size)
 *
 * Return path to .rec file corresponding to the ISAM file isfname.
 * Conversion is done in place.
 */

void
_makedat_isfname(char *isfname, size_t size)
{
    /* Append .rec */
    _append_suffix(isfname, size, DAT_SUFFIX);
}

/*
 * _makeind_isfname(isfname)
 *
 * Return path to .ind file corresponding to the ISAM file isfname.
 * Conversion is done in place.
 */

void
_makeind_isfname(char *isfname, size_t size)
{
    /* Append .ind */
    _append_suffix(isfname, size, IND_SUFFIX);
}

/*
 * _makevar_isfname(isfname)
 *
 * Return path to .var file corresponding to the ISAM file isfname.
 * Conversion is done in place.
 */

void
_makevar_isfname(char *isfname, size_t size)
{
    /* Append .var */
    _append_suffix(isfname, size, VAR_SUFFIX);
}


/*
 * _removelast(path)
 *
 * Remove last element of path. E.g. /usr/db/part yields /usr/db.
 */

void
_removelast(char *path)
{
    size_t	len = strlen(path);

    while (len > 0 && path[len - 1] != '/')
	path[--len] = '\0';
}

/*
 * _lastelement(path)
 *
 * Return pointer to the last element in the path. 
 * E.g.: _lastelement("/usr/temp") returns "temp".
 */

char *
_lastelement(char *path)
{
    char	*p;
    
    p = path + strlen(path);

    while (*--p != '/' && p > path)
	{ ; }

    return ((*p == '/') ? (p + 1) : p);
}
