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
//%%  $XConsortium: tt_db_access_utils.C /main/3 1995/10/23 10:00:48 rswiston $ 			 				
/* @(#)tt_db_access_utils.C	1.5 @(#)
 * tt_db_access_utils.cc - Defines the _Tt_db_access utilities.
 *
 * Copyright (c) 1992 by Sun Microsystems, Inc.
 */

#include <sys/stat.h>

#include "db/tt_db_access.h"
#include "db/tt_db_access_utils.h"

implement_ptr_to(_Tt_db_access)

// Normally, we don't put the constructors in the _utils.cc, but since
// tt_db_access.cc doesn\'t exist, it seems a waste to create it just
// to hold some null constructors.

_Tt_db_access::
_Tt_db_access()
{
      user = (uid_t)-1;
      group = (gid_t)-1;
      mode = (mode_t)-1; // Default: Everyone can read and write it
}

_Tt_db_access::
~_Tt_db_access()
{     
}     

bool _Tt_db_access::
permits (const _Tt_db_access &caller, bool write, bool user_only) const
{
      if (caller.user == 0) {
	    return true;
      }

      if (user == (uid_t)-1 || user == caller.user) {
	    return true;
      }

      if (user_only) {
	    return false;
      }

      if (mode == (mode_t)-1) {
	    return true;
      }

      // One class decides: a member of the group has no more than the group
      // bits give, even where the bits for the others give more.
      bool in_group = (group == (gid_t)-1 || group == caller.group);
      mode_t needed = in_group ? (write ? S_IWGRP : S_IRGRP)
			       : (write ? S_IWOTH : S_IROTH);

      return (mode & needed) != 0;
}
