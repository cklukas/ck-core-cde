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
/*%%  $XConsortium: isrename.c /main/3 1995/10/23 11:43:48 rswiston $ 			 				 */
/*
 * Copyright (c) 1988 by Sun Microsystems, Inc.
 */

/*
 * isrename.c
 *
 * Description:
 *	Rename an ISAM file. 
 */


#include "isam_impl.h"
#include <unistd.h>
#include <sys/time.h>

static void _removelast2(char *path);
static Bool _newfname(char *newbase, size_t size, const char *isfname,
		      const char *newname);
static void _rename_file(const char *isfname, const char *newbase,
			 void (*makesuffix)(char *, size_t));
static int _amrename(Bytearray *isfhandle, char *newname,
                     struct errcode *errcode);

/*
 * isfd = isrename(oldname, newname)
 *
 *
 * Errors:
 *	EBADFILE ISAM file is corrupted or it is not an NetISAM file
 *	EFLOCKED The file is exclusively locked by other process.
 *	EFNAME	Invalid ISAM file name 
 *	EFNAME	ISAM file does not exist
 *	ETOOMANY Too many ISAM file descriptors are in use (128 is the limit)
 *
 * The following error code is "borrowed" from UNIX:
 *	EACCES	UNIX file system protection denies access to the file:
 *	         - mode is INOUT or OUTPUT and ISAM file is on 
 *	           a Read-Only mounted file system
 *		 - UNIX file permissions don't allow access to the file
 */

int 
isrename(char *oldname, char *newname)
{
    Isfd		isfd, isfd2;
    Fab			*fab;
    char	  	olddir [MAXPATHLEN];
    char	  	newdir [MAXPATHLEN];
    char		datfname[MAXPATHLEN];

    if (!_isfname_valid(oldname) || !_isfname_valid(newname)) {
	_setiserrno2(EFNAME, 9, 0);
	return (ISERROR);
    }

    /*
     * Check that the old and new filename are in the same directory.
     */
    snprintf(olddir, sizeof(olddir), "%s", oldname);
    _removelast2(olddir);
    snprintf(newdir, sizeof(newdir), "%s", newname);
    _removelast2(newdir);

    if (strcmp(newdir, olddir) != 0) {
	_setiserrno2(EFNAME, 9, 0);
	return (ISERROR);
    }

    /*
     * Open the file
     */
    if ((isfd = isopen(oldname, ISINOUT)) == -1)
	return (ISERROR);		     /* iserrno is set */

    /*
     * Reject rename if 'newfile' exists.
     */
    if ((isfd2 = isopen(newname, ISINOUT)) != -1 || iserrno != ENOENT) {

	_setiserrno2(EEXIST, 9, 0);
	if (isfd2 >= 0) (void)isclose(isfd2);
	return (ISERROR);		     /* iserrno is set */
    }

    /*
     * Get File Access Block.
     */
    if ((fab = _isfd_find(isfd)) == nullptr) {
	_isfatal_error("isrename() cannot find FAB");
        _setiserrno2(EFATAL,'9','0');
        return (ISERROR);
    }

    if (_amrename(&fab->isfhandle, _lastelement(newname), &fab->errcode)) {
	_seterr_errcode(&fab->errcode);
	(void)isclose(isfd);
	return (ISERROR);
    }

    _fab_destroy(fab);			     /* Deallocate Fab object */
    _isfd_delete(isfd);

    /*
     * We must unlink() the .rec file, or isbuild() with the same 
     * following immediatly would fail because for NFS files the client
     * still thinks that the file exists for a few seconds.
     */

    snprintf(datfname, sizeof(datfname), "%s", oldname);
    _makedat_isfname(datfname, sizeof(datfname));
    (void)unlink(datfname);

    return (ISOK);			     /* Successful isrename() */
}

/*
 * _removelast2(path)
 *
 * Remove last element of path. E.g. /usr/db/part yields /usr/db.
 * Unlike _removelast() that path does not have to start with '/'.
 */

Static void
_removelast2(char *path)
{
    size_t	len = strlen(path);

    while (len > 0 && path[len - 1] != '/')
	path[--len] = '\0';
}

/*
 * _amrename(isfhandle, newname)
 *
 * _amrename() renames ISAM file
 *
 * Input params:
 *	isfhandle	Handle of ISAM file
 *
 * Output params:
 *	errcode		Error code
 *
 */


static int
_amrename(Bytearray *isfhandle, char *newname, struct errcode *errcode)
{
    Fcb			*fcb;
    char		*isfname = _getisfname(isfhandle);
    char		newbase[MAXPATHLEN];

    _isam_entryhook();

    if (!_newfname(newbase, sizeof(newbase), isfname, newname)) {
	_amseterrcode(errcode, EFNAME);
	goto ERROR;
    }

    /*
     * Get FCB corresponding to the isfhandle handle.
     */
    if ((fcb = _openfcb(isfhandle, errcode)) == nullptr) {
	goto ERROR;
    }

    /*
     * Delete FCB and remove it from FCB cache.
     */
    (void) _watchfd_decr(_isfcb_nfds(fcb));
    _isfcb_close(fcb);
    _mngfcb_delete(isfhandle);

    /*
     * Rename all UNIX files.
     */
    _rename_file(isfname, newbase, _makedat_isfname);
    _rename_file(isfname, newbase, _makeind_isfname);
    _rename_file(isfname, newbase, _makevar_isfname);

    _isam_exithook();
    return (ISOK);

 ERROR:

    _isam_exithook();
    return (ISERROR);
}


/*
 * _newfname(newbase, size, isfname, newname)
 *
 * Build the name of the renamed ISAM file: the directory of isfname
 * followed by newname, which carries no directory.  Return FALSE if the
 * result is not a valid ISAM file name.
 */
Static Bool
_newfname(char *newbase, size_t size, const char *isfname, const char *newname)
{
    char	dir[MAXPATHLEN];
    int		len;

    snprintf(dir, sizeof(dir), "%s", isfname);
    _removelast2(dir);
    len = snprintf(newbase, size, "%s%s", dir, newname);

    return (len >= 0 && (size_t)len < size && _isfname_valid(newbase));
}

/* Rename the file that the suffix function derives from the ISAM file name. */
Static void
_rename_file(const char *isfname, const char *newbase,
	     void (*makesuffix)(char *, size_t))
{
    char	namebuf[MAXPATHLEN];
    char	newbuf[MAXPATHLEN];

    snprintf(namebuf, sizeof(namebuf), "%s", isfname);
    snprintf(newbuf, sizeof(newbuf), "%s", newbase);
    makesuffix(namebuf, sizeof(namebuf));
    makesuffix(newbuf, sizeof(newbuf));

    /* A missing .ind or .var file is normal, so the result is not used. */
    (void)rename(namebuf, newbuf);
}
