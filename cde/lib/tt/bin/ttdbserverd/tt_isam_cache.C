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
/*
 * tt_isam_cache.C - The NetISAM files rpc.ttdbserverd has open (see
 * tt_isam_cache.h).
 */

#include "tt_isam_cache.h"
#include "tt_isam.h"
#include "tt_options.h"
#include "db_server_globals.h"
#include <errno.h>
#include <isam.h>

_Tt_db_info	_tt_db_table[_TT_MAX_ISFD];
int		_tt_refclock = 0;

//
// Go through the _tt_db_table and check on each file.
// Then compresse the files (empty records deleted).
//
void
isgarbage_collect()
{
	int	offset;

	for (offset = 0; offset < _TT_MAX_ISFD; offset++) {

#ifdef OPT_GARBAGE_THREADS
		//
		// Allow an RPC process to run until it exits.
		//
		UNLOCK_RPC();
		thr_yield();
		LOCK_RPC();
#endif

		// Don't mess with open files!
		if (_tt_db_table[offset].db_path.len() > 0) {

		  cached_isclose(offset);
		  isgarbage(_tt_db_table[offset].db_path);
		}
	}

	return;
}

int
cached_isopen(const char *filepath, int mode)
{
	// First search for an available open isfd for this file,
	// reuse it if found.

	int i, isfd, lru_fd;
	_Tt_string fp(filepath);
	_Tt_db_info *p;

	for (i=0; i<_TT_MAX_ISFD; i++) {

		p = _tt_db_table+i;
		if (!p->client_has_open &&
		    p->db_path == fp &&
		    p->open_mode == mode) {
			p->client_has_open = 1;
			p->reftime = _tt_refclock++;
			return i;
		}
	}

	// No suitable reusable isfd was found.  Try opening a new one.
	// If we can't open one because they're all in use (ETOOMANY),
	// close the least-recently-used one and try opening again.

	for (;;) {
		isfd = isopen(filepath, mode);
		if (isfd >= _TT_MAX_ISFD) {
			// NetISAM has more descriptors than _tt_db_table has
			// entries (its limit used to be that of the table):
			// give this one back and go on as if NetISAM had none
			// left.
			(void)isclose(isfd);
			isfd = -1;
			iserrno = ETOOMANY;
		}
		if (isfd!=-1) {
			// Opened fine.
			p = _tt_db_table+isfd;
			p->db_path = fp;
			p->open_mode = mode;
			p->client_has_open = 1;
			p->server_has_open = 1;
			p->reftime = _tt_refclock++;
			return isfd;
		} else if (iserrno!=ETOOMANY) {
			// some REAL problem..
		  	//
		  	return isfd;
		}
		// find the least-recently-used isfd and close it.
		// Note that any fd a client has open is considered
		// more recently used than any fd a client doesn't have
		// open; this prevents us from giving away fd's a client
		// thinks it has open, unless we're really in desparate
		// straits.  It would be nice to completely decouple the
		// "fd" numbers we hand back from the isfd's, so that
		// stale fd's could be more reliably distinguished.

		lru_fd = -1;

		for (i = 0; i<_TT_MAX_ISFD; i++) {
			if (!_tt_db_table[i].server_has_open) {
				// we never opened this one, so we better
				// not close it.
				continue;
			}
			if (lru_fd < 0) {
				lru_fd = i;
			}
			if (_tt_db_table[i].reftime<
				   _tt_db_table[lru_fd].reftime) {
				// this entry is older, check open status
				if (_tt_db_table[lru_fd].client_has_open ||
				    !_tt_db_table[i].client_has_open) {
					lru_fd = i;
				}
			} else {
				// this entry is newer, but still check
				// open status
				if (_tt_db_table[lru_fd].client_has_open &&
				    !_tt_db_table[i].client_has_open) {
					lru_fd = i;
				}
			}
		}

		if (lru_fd < 0) {
			// None of the descriptors in use is one we opened and
			// can close (they come from isbuild, or from a
			// database file opened elsewhere): the table is full.
			return -1;
		}
		isclose(lru_fd);


		// mark cache entry invalid in case open fails.

		_tt_db_table[lru_fd].client_has_open = 0;
		_tt_db_table[lru_fd].server_has_open = 0;
		_tt_db_table[lru_fd].db_path = "";
	}
}

int
cached_isclose(int isfd)
{
	int isrc;

	if (-1 == isfd) return ISERROR;

	if (isfd < 0 || isfd >= _TT_MAX_ISFD) {
		// not one of the descriptors _tt_db_table describes (NetISAM
		// has more): there is nothing cached about it to keep
		return isclose(isfd);
	}

	if (!_tt_db_table[isfd].server_has_open) {
		// The open was not cached (perhaps because it was an
		// isbuild instead of an isopen.)  So do a real
		// close.
		isrc = isclose(isfd);

		// mark cache entry invalid (it probably was already.)

		_tt_db_table[isfd].client_has_open = 0;
		_tt_db_table[isfd].server_has_open = 0;
		_tt_db_table[isfd].db_path = "";

		return isrc;
	}


	// Ensure everything gets written out.

	isrc = isfsync(isfd);

	// Mark cache entry as reusable

	_tt_db_table[isfd].client_has_open = 0;
	return isrc;
}
