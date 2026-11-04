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
//%%  $XConsortium: tt_db_partition_global_map_ref.C /main/3 1995/10/20 16:42:23 rswiston $ 			 				
/*
 * tt_db_partition_global_map_ref.cc - Define the TT DB server partition
 *                                     global map ref class.
 *
 * Copyright (c) 1992 by Sun Microsystems, Inc.
 *
 */

#include <errno.h>
#include <sys/stat.h>
#include "util/tt_port.h"
#include "util/tt_gettext.h"
#include "util/tt_file_system.h"
#include "util/tt_file_system_entry.h"
#include "tt_isam_file_utils.h"
#include "tt_isam_record_utils.h"
#include "tt_db_partition_redirection_map.h"
#include "tt_db_partition_global_map_ref.h"
#include "tt_db_server_consts.h"

extern _Tt_db_partition_redirection_map *db_pr_map;
extern FILE *errstr;

_Tt_db_server_db_table_ptr*
_Tt_db_partition_global_map_ref::dbPartitionMap = (_Tt_db_server_db_table_ptr *)
						  NULL;

_Tt_db_partition_global_map_ref::_Tt_db_partition_global_map_ref ()
{
  if (!dbPartitionMap) {
    dbPartitionMap = new _Tt_db_server_db_table_ptr;
    *dbPartitionMap = new _Tt_db_server_db_table ((_Tt_object_table_keyfn)
						  &dbPartitionMapKey);
  }
}

_Tt_db_partition_global_map_ref::~_Tt_db_partition_global_map_ref ()
{
  // The static table is freed up when the application dies.
}

void _Tt_db_partition_global_map_ref::addDB (_Tt_db_server_db_ptr &db)
{
  (*dbPartitionMap)->insert(db);
}

void _Tt_db_partition_global_map_ref::removeDB (const _Tt_string &partition)
{
  (*dbPartitionMap)->remove(partition);
}

_Tt_db_server_db_ptr
_Tt_db_partition_global_map_ref::getDB (const _Tt_string &partition)
{
  _Tt_string real_partition = db_pr_map->findEntry(partition);
  if (real_partition.is_null() || !real_partition.len()) {
    real_partition = partition;
  }

  _Tt_db_server_db_ptr db_ptr = (*dbPartitionMap)->lookup(real_partition);
  if (db_ptr.is_null()) {
    db_ptr = new _Tt_db_server_db(real_partition);

    _Tt_db_results results = db_ptr->getDBResults();
    if (results != TT_DB_OK) {
      _tt_syslog(errstr, LOG_ERR,
		 "_Tt_db_server_db(\"%s\"): %d (%s)",
		 (char *)real_partition, results,
		 (char *)db_ptr->getLastFileAccessed());
      db_ptr = (_Tt_db_server_db *)NULL;
    }
    else {
      addDB(db_ptr);
    }
  }

  return db_ptr;
}

/*
 * The partitions are the file systems of the host, the mount points of its
 * mount table (that is what _tt_get_file_partition() answers for a file), and
 * those the administrator names in the partition map.  A client says which
 * partition an object is in, in its object id, and a TT_DB directory with the
 * tables of a database is made in the partition if it is not there yet: no
 * other name may make one.  A partition is named as the mount table names it,
 * with no link, ".." or trailing "/" to get to it.
 */
bool_t
_Tt_db_partition_global_map_ref::isKnownPartition (const _Tt_string &partition)
{
  if (!partition.len() || ((char *)partition)[0] != '/' ||
      memchr((char *)partition, '\0', partition.len()) != NULL) {
    return FALSE;
  }

  _Tt_string redirection = db_pr_map->findEntry(partition);
  if (!redirection.is_null() && redirection.len()) {
    return TRUE;
  }

  _Tt_file_system           fs;
  _Tt_file_system_entry_ptr entry = fs.bestMatchToPath(partition);

  return (!entry.is_null() && entry->getMountPoint() == partition) ? TRUE : FALSE;
}

_Tt_string
_Tt_db_partition_global_map_ref::dbPartitionMapKey (_Tt_object_ptr &db)
{
  return (((_Tt_db_server_db *)db.c_pointer())->getPartition());
}
