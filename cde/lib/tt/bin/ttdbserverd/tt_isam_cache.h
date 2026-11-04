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
 * tt_isam_cache.h - The NetISAM files rpc.ttdbserverd has open.
 *
 * The server keeps the files it opened open, in a table of descriptors, and
 * hands an open one out again when it is asked for the same file in the same
 * mode.  When NetISAM has no descriptor left, the one used least recently is
 * closed.
 */

#ifndef _TT_ISAM_CACHE_H
#define _TT_ISAM_CACHE_H

#include <sys/types.h>
#include "util/tt_string.h"

#define _TT_MAX_ISFD  128

/* structure containing info open ISAM files: full path and opener's uids */
struct _Tt_db_info {
	_Tt_db_info() : opener_uid((uid_t)-1), open_mode(0),
			client_has_open(0), server_has_open(0), reftime(0) {}

	_Tt_string	db_path;
	uid_t		opener_uid;
	int		open_mode;	// mode file was opened with
	int		client_has_open; // 1 iff we think somebodys using it
	int		server_has_open; // 1 iff we opened it
	int		reftime;	 // "time" of last reference for LRU
};

// The table of the files, indexed by the descriptor NetISAM gave.
extern _Tt_db_info _tt_db_table[_TT_MAX_ISFD];

// A "pseudo clock" that is counted up every time a descriptor is referred
// to; the value goes to the reftime of its entry, so that the least recently
// used descriptor can be found when one has to be given back.
extern int _tt_refclock;

// Like isopen and isclose, but the file stays open when it is closed, for
// the next one to ask for it.  They return -1 when they fail.
int  cached_isopen(const char *filepath, int mode);
int  cached_isclose(int isfd);

// Close every file of the table and compress it (delete the empty records).
void isgarbage_collect();

#endif /* _TT_ISAM_CACHE_H */
