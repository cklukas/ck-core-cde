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
//%%  $TOG: dm_server.C /main/4 1999/10/08 12:01:17 mgreess $
/*
 *  Tool Talk Database Server - dm_server.C
 *
 *  Copyright (c) 1989 Sun Microsystems, Inc.
 *
 *
 * This file contains the db server wrappers for the following NetISAM
 * functions:
 *
 * isaddindex, isbuild, isclose, iscntl, isdelrec, iserase, isopen, isread,
 * isrewrec, isstart, iswrite.
 *
 * Each wrapper is an rpc procedure that calls the actual NetISAM function
 * that it wraps on behalf of the client.
 *
 * The file also contains functions for implementing access control and
 * cacheing isopens.
 */

#include "tt_options.h"
#include "dm_access_cache.h"
#include "tt_db_server_consts.h"
#include "tt_db_msg_q_lock.h"
#include "tt_isam_file_utils.h"
#include "tt_isam_record_utils.h"
#include "tt_db_server_db.h"
#include "tt_db_partition_global_map_ref.h"
#include "db_server_globals.h"
#include "db/tt_db_key.h"
#include "db/db_server.h"
#include "db/tt_db_key.h"
#include "db/tt_db_key_utils.h"
#include "util/tt_string.h"
#include "util/tt_path.h"
#include "util/tt_log.h"
#include "util/tt_port.h"
#include "util/tt_gettext.h"
#include "util/tt_file_system.h"
#include "util/tt_file_system_entry.h"
#include "dm/dm_enums.h"
#include "dm/dm_recfmts.h"
#include <errno.h>
#include <limits.h>
#include <memory.h>
#include <string.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <isam.h>
#include <time.h>

extern "C" { int isaddindex(int, struct keydesc*); }
extern "C" { int isbuild(const char*, int, struct keydesc*, int); }
extern "C" { int isclose(int); }
extern "C" { int iscntl(int, int, ...); }
extern "C" { int isdelcurr(int); }
extern "C" { int isdelrec(int, long); }
extern "C" { int iserase(const char*); }
extern "C" { int isfsync(int); }
extern "C" { int isopen(const char*, int); }
extern "C" { int isread(int, char*, int); }
extern "C" { int isrename(char*, char*); }
extern "C" { int isrewcurr(int, char*); }
extern "C" { int isrewrec(int, long, char*); }
extern "C" { int isstart(int, struct keydesc*, int, char*, int); }
extern "C" { int iswrite(int, char*); }
extern "C" { int isunlock(int); }

extern int _tt_auth_level;
extern int _tt_gidlen;
extern gid_t _tt_gid, _tt_gidlist[NGROUPS];
extern uid_t _tt_uid;
extern keydesc _tt_oid_keydesc;
extern _Tt_oid_access_queue_ptr _tt_oa_cache;
extern _Tt_link_access_queue_ptr _tt_la_cache;
extern char _tt_log_file[];
extern _Tt_db_info _tt_db_table[_TT_MAX_ISFD];	/* table of NetISAM db opened */
extern int _tt_refclock;
extern time_t _tt_mtab_last_mtime;
extern int access_checking;
extern void _tt_process_transaction();
extern FILE *errstr;

int        find_endstring(const char *string, const char *end_string);
bool_t     msg_q_lock(int         isfd,
		      const char *record,
		      int         length,
		      SVCXPRT    *transp);
_Tt_string map_old_db_to_new_db(const char *old_db);
_Tt_string get_new_db(_Tt_string old_db,
		      int        index,
		      _Tt_string new_file);

// HACK??? Why do some of these have slashes and some not?

const char *_TT_DOCOID_PATH = "/docoid_path";
const char *_TT_LINK_ACCESS = "/link_access";
const char *_TT_LINK_PROP = "link_prop";
const char *_TT_LINK_ENDS = "link_ends";
const char *_TT_OID_ACCESS = "/oid_access";
const char *_TT_OID_CONTAINER = "/oid_container";
const char *_TT_OID_PROP = "/oid_prop";
const char *_TT_LOG_FILE = "/log_file";
// the tables of the layout that replaced those of the old servers; the
// old names are those of the tables a client of an old server asks for
const char *_TT_FILE_TABLE = "/" TT_DB_FILE_TABLE_FILE;
const char *_TT_FILE_OBJECT_MAP = "/" TT_DB_FILE_OBJECT_MAP_FILE;
const char *_TT_ACCESS_TABLE = "/" TT_DB_ACCESS_TABLE_FILE;
const int _TT_ROOT_UID = 0;

static _Tt_isam_results res;
static char _tt_record[ISMAXRECLEN];
static int _tt_record_len;	/* the length of the record _tt_get_record() read */

enum _Tt_dm_access_category { DM_USER, DM_GROUP, DM_OTHER};

#define DM_TEST_CRASH 0

/*
 *  _tt_get_record - read a record from the database with the given directory
 *  prefix and db_name.  The input record 'rec' contains the key for searching.
 *  The result of the read is returned in _tt_record.  If succeeds returns 1.
 *  If fails returns 0.
 */

int
_tt_get_record(int prefix_len, const char *prefix, const char *db_name, char *rec)
{
	static const char *here = "_tt_get_record()";
	if (!rec) {	/* rec must contain key, even when reading by recnum */
		return 0;
	}
	char *db = (char *)malloc(prefix_len + strlen(db_name) + 2);
	memcpy(db, prefix, prefix_len+1);
	strcpy(db + prefix_len +1, db_name);
	/*
	 * HACK:  Somehow, isopening non-existent ISAM data bases seems
	 * to burn isfds.  I can't see in the NetISAM code where this happens,
	 * but it sure is happening.  Bypass this by statting the .rec
	 * file before trying to do isopen.
	 */

	char *dblong = (char *)malloc(strlen(db)+1+4);
	int isfd;
	struct stat statbuf;
	strcpy(dblong,db);
	strcat(dblong,".rec");
	isfd = stat(dblong,&statbuf);
	if (-1==isfd) {
		_tt_syslog(errstr, LOG_ERR, "%s: %m", dblong);
		free(db);
		free(dblong);
		return 0;
	}
	free(dblong);

	isfd = cached_isopen(db, ISINOUT+ISFIXLEN+ISMANULOCK);
	free(db);
	if (isfd == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isopen(): %d", here, iserrno);
		return 0;
	}
	if (isstart(isfd, &_tt_oid_keydesc, OID_KEY_LENGTH, rec, ISEQUAL) == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isstart(): %d", here, iserrno);
		cached_isclose(isfd);
		return 0;
	}
	memcpy(_tt_record, rec, OID_KEY_LENGTH);
	if (isread(isfd, _tt_record, ISNEXT) == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isread(): %d", here, iserrno);
		cached_isclose(isfd);
		return 0;
	}
	_tt_record_len = isreclen;
	if (cached_isclose(isfd) == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isclose(): %d", here, iserrno);
		return 0;
	}
	return 1;
}

/*
 *  _tt_write_record - write a record to the database with the given directory
 *  prefix and db_name.  The input record 'rec' contains record for writing.
 *  If succeeds returns 1.  If fails returns 0.
 */

int
_tt_write_record(int prefix_len, const char *prefix, const char *db_name,
		 char *rec, int reclen)
{
	static const char *here = "_tt_write_record()";
	if (!rec) {
		return 0;
	}
	char *db = (char *)malloc(prefix_len + strlen(db_name) + 2);
	memcpy(db, prefix, prefix_len+1);
	strcpy(db + prefix_len +1, db_name);
	/*
	 * HACK:  Somehow, isopening non-existent ISAM data bases seems
	 * to burn isfds.  I can't see in the NetISAM code where this happens,
	 * but it sure is happening.  Bypass this by statting the .rec
	 * file before trying to do isopen.
	 */

	char *dblong = (char *)malloc(strlen(db)+1+4);
	int isfd;
	struct stat statbuf;
	strcpy(dblong,db);
	strcat(dblong,".rec");
	isfd = stat(dblong,&statbuf);
	if (-1==isfd) {
		_tt_syslog(errstr, LOG_ERR, "%s: %m", dblong);
		free(db);
		free(dblong);
		return 0;
	}
	free(dblong);

	isfd = cached_isopen(db, ISINOUT+ISFIXLEN+ISMANULOCK);
	free(db);
	if (isfd == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isopen(): %d", here, iserrno);
		return 0;
	}
	memcpy(_tt_record, rec, OID_KEY_LENGTH);
	isreclen = reclen;
	if (iswrite(isfd, rec) == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: iswrite(): %d", here, iserrno);
		cached_isclose(isfd);
		return 0;
	}
	if (cached_isclose(isfd) == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isclose(): %d", here, iserrno);
		return 0;
	}
	return 1;
}

/*
 *  _tt_decode_access - the owner, group and mode in the access record REC of
 *  RECLEN bytes, in the layout of the table it was read from:
 *
 *  - the tables that rpc.ttdbserverd makes itself have a record of a key of
 *    TT_DB_KEY_LENGTH bytes and a long for each number, which holds the
 *    number in network order;
 *  - the tables that a client of the old servers makes (the length of its
 *    key is that of OID_KEY_LENGTH) have 16 bits for each number, in network
 *    order, and 0xffff for "none".
 *
 *  "None" is -1 in both.  Returns 0 if the record is of neither layout.
 */

int
_tt_decode_access(const char *rec, int reclen, uid_t *uid, gid_t *group,
		  mode_t *mode)
{
	if (reclen == TT_DB_KEY_LENGTH + 3 * TT_DB_LONG_SIZE) {
		long n_user = 0, n_group = 0, n_mode = 0;

		memcpy(&n_user, rec + TT_DB_ACCESS_USER_OFFSET, TT_DB_LONG_SIZE);
		memcpy(&n_group, rec + TT_DB_ACCESS_GROUP_OFFSET, TT_DB_LONG_SIZE);
		memcpy(&n_mode, rec + TT_DB_ACCESS_MODE_OFFSET, TT_DB_LONG_SIZE);
		*uid = (uid_t)ntohl(n_user);
		*group = (gid_t)ntohl(n_group);
		*mode = (mode_t)ntohl(n_mode);
		return 1;
	}
	if (reclen == OID_KEY_LENGTH + 3 * 2) {
		unsigned short n_user, n_group, n_mode;

		memcpy(&n_user, rec + OID_KEY_LENGTH, sizeof(n_user));
		memcpy(&n_group, rec + OID_KEY_LENGTH + 2, sizeof(n_group));
		memcpy(&n_mode, rec + OID_KEY_LENGTH + 4, sizeof(n_mode));
		n_user = ntohs(n_user);
		n_group = ntohs(n_group);
		n_mode = ntohs(n_mode);
		*uid = (n_user == 0xffff) ? (uid_t)-1 : (uid_t)n_user;
		*group = (n_group == 0xffff) ? (gid_t)-1 : (gid_t)n_group;
		*mode = (n_mode == 0xffff) ? (mode_t)-1 : (mode_t)n_mode;
		return 1;
	}
	return 0;
}

/*
 *  _tt_get_oid_access - get the input oid's owner, group, and mode from the
 *  access table of the database of DB_PATH.  The table is read each time: the
 *  new interface of the server changes it as well, and a cache that keeps
 *  the record by the oid alone would hand the record of one database to
 *  the request for another.
 *  Returns 0 if there is such a record, 1 if there is none or it is none.
 */

int
_tt_get_oid_access(char *key, int prefix_len, char *db_path,
		    uid_t &uid, gid_t &group, mode_t &mode)
{
	if (!_tt_get_record(prefix_len, db_path, _TT_ACCESS_TABLE, key)) {
		// no oid record in access table
		return 1;
	}
	return _tt_decode_access(_tt_record, _tt_record_len, &uid, &group,
				 &mode) ? 0 : 1;
}

/*
 *  _tt_get_link_permissions - get the link's permissions, user, and groups.
 *  Returns 0 if the link has no access info, 1 if succeeds.
 */

int
_tt_get_link_permissions(char *key, int prefix_len, char *db_path,
			 uid_t &uid, gid_t &group, mode_t &mode)
{
	/* lookup in LRU cache */
	_Tt_link_access_ptr la = _tt_la_cache->lookup(key);
	if (la.is_null()) {
		/* if not in cache, get from LINK-ACCESS database */
		if (_tt_get_record(prefix_len, db_path,
				   _TT_LINK_ACCESS, key)) {
			la = new _Tt_link_access(_tt_record);
			if (!la.is_null()) {
				_tt_la_cache->enqueue(la);	/* put in LRU cache */
			}
		} else {
			return 1;
		}
	} else {
		_tt_la_cache->promote(la);	/* update the LRU cache */
	}
	uid = la->user();
	group = la->group();
	mode = la->mode();
	return 0;
}

/*
 *  Check for stale NetISAM file descriptor.  Returns 1 if client's uid matches
 *  database opener's uid.  Otherwise returns 0.  The descriptor is the
 *  client's own number: one that is not an entry of _tt_db_table is stale too.
 */

int
_tt_check_stale_isfd(int isfd)
{
	if (isfd < 0 || isfd >= _TT_MAX_ISFD) {
		return 0;
	}
	if (_tt_uid != _tt_db_table[isfd].opener_uid) {
		return 0;
	}
	return 1;
}

/*
 *  _tt_client_record - copy a record sent by a client to BUF, which holds
 *  ISMAXRECLEN bytes, and zero what follows it.  NetISAM takes the record
 *  length of the file it works on, or the key fields of an index, from the
 *  buffer it is given, whatever number of bytes the client sent.  Given the
 *  client's own, shorter buffer it would read past the end of it, and store
 *  what it found in the file.
 */

static void
_tt_client_record(char *buf, const char *rec_val, u_int rec_len)
{
	memset(buf, 0, ISMAXRECLEN);
	if (rec_val != NULL && rec_len > 0) {
		memcpy(buf, rec_val, rec_len < ISMAXRECLEN ? rec_len : ISMAXRECLEN);
	}
}

/*
 *  _tt_set_record_key - the session procedures and gettype look for a record
 *  of the property table by the object it belongs to and the name of the
 *  property, which is how that table is keyed.  Zeroes the request record
 *  and puts the key in it: the key of the object, and the NAME of NAMELEN
 *  bytes in the field of the name.  Returns 0, and leaves the record alone,
 *  if the key is not there or longer than a key, or the name is none or does
 *  not fit its field.
 */

static int
_tt_set_record_key(const char *oidkey, u_int oidkey_len, const char *name,
		   size_t namelen)
{
	if (oidkey == NULL || oidkey_len == 0 || oidkey_len > (u_int)OID_KEY_LENGTH ||
	    name == NULL || namelen == 0 || namelen > (size_t)MAX_PROP_LENGTH) {
		return 0;
	}
	memset(_tt_record, 0, sizeof(_tt_record));
	memcpy(_tt_record, oidkey, oidkey_len);
	memcpy(_tt_record + OID_KEY_LENGTH, name, namelen);
	return 1;
}

/*
 *  _tt_oid_accessp - this function determines whether the client with the
 *  extracted uid and gids have read or write permissions as specified by the
 *  input mode ('r', 'w' or 'r' + 'w') to the properties of the object whose
 *  key is the first OID_KEY_LENGTH bytes of the record REC of REC_LEN bytes
 *  in the property table.  The protection of the file the object is in
 *  decides, as rpc.ttdbserverd(1M) says of the option that turns this off:
 *
 *  - the owner of the file or of the object may do anything;
 *  - a member of the group of the file may do what the group bits of the
 *    mode of the file, or of the mode of the object, give to the group;
 *  - everybody else what the other bits give.
 *
 *  A request that reads and writes needs both.  The properties of a file
 *  itself (which has no file of its own to look at) are everybody's.  If no
 *  access record is found, an error has occurred and no permission is given.
 *  If the client has the access permission, returns 1.  Otherwise, returns 0.
 */

int
_tt_oid_accessp(int _isfd, const char *rec, u_int rec_len, int mode)
{
	static const char *here = "_tt_oid_accessp()";
	int prefix_len;
	_Tt_dm_access_category category = DM_OTHER;
	uid_t oid_uid, file_uid;
	gid_t oid_group, file_group;
	mode_t oid_mode, file_mode;

	if (! access_checking) {
		// access checking turned off
		return 1;
	}
	if (_tt_uid == _TT_ROOT_UID) {
		// The superuser always has access
		return 1;
	}
	if (!_tt_check_stale_isfd(_isfd)) {
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
		return 0;
	}
	char *db_path = _tt_db_table[_isfd].db_path;
	if (!db_path) {
		_tt_syslog(errstr, LOG_ERR, "%s: db_path == 0", here);
		return 0;
	}
	prefix_len = _Tt_dirname(db_path);
	if (prefix_len < 0) {
		_tt_syslog(errstr, LOG_ERR, "%s: _Tt_dirname(db_path) < 0",
			   here);
		return 0;
	}
	int basename_len = _Tt_basename(db_path);
	char *db_basename = db_path + basename_len;
	// The path is that of the table of the current layout (isopen and
	// isbuild map the name the client used), which is the name to look for.
	if (strcmp(db_basename, TT_DB_PROPERTY_TABLE_FILE) == 0) {
		if (rec == NULL || rec_len < (u_int)OID_KEY_LENGTH) {
			// no object to ask about
			_tt_syslog(errstr, LOG_ERR, "%s: no object key", here);
			return 0;
		}
		char key[OID_KEY_LENGTH];
		memcpy(key, rec, sizeof(key));

		bool want_read = (mode == 'r' || mode == 'r' + 'w');
		bool want_write = (mode == 'w' || mode == 'r' + 'w');
		if (!want_read && !want_write) {
			return 0;
		}
		/* get the OID-ACCESS info */
		if (_tt_get_oid_access(key, prefix_len, db_path,
				       oid_uid, oid_group, oid_mode)) {
			// no record, error
			_tt_syslog(errstr, LOG_ERR, "_tt_get_oid_access() != 0");
			return 0;
		}
		/* get the document oid */
		if (!_tt_get_record(prefix_len, db_path, _TT_FILE_OBJECT_MAP,
				    key)) {
			// only a doc oid has no mapping, allow access
			return 1;
		}
		/* get the document path */
		if (!_tt_get_record(prefix_len, db_path, _TT_FILE_TABLE,
				    _tt_record + OID_KEY_LENGTH)) {
			_tt_syslog(errstr, LOG_ERR,
				   "_tt_get_record(,,_TT_FILE_TABLE,) == 0");
			return 0;
		}
		if (_tt_record_len <= OID_KEY_LENGTH ||
		    _tt_record_len >= (int)sizeof(_tt_record)) {
			_tt_syslog(errstr, LOG_ERR, "%s: no path of the file", here);
			return 0;
		}
		_tt_record[_tt_record_len] = '\0';
		char *path = _tt_record + OID_KEY_LENGTH;
		char *p = strchr(path, ':');
		if (!p) {
			_tt_syslog(errstr, LOG_ERR, "strchr(path, ':') != 0");
			return 0;
		}
		/* get the document's permissions */
		struct stat buf;
		if (stat(p+1, &buf) != -1) {
			file_uid = buf.st_uid;
			file_group = buf.st_gid;
			file_mode = buf.st_mode;
		} else {
			file_uid = oid_uid;
			file_group = oid_group;
			file_mode = oid_mode;
		}
		if (_tt_uid == file_uid || _tt_uid == oid_uid) {
			category = DM_USER;
		} else {
			for (int i = 0; i < _tt_gidlen; i++) {
				if (_tt_gidlist[i] == file_group) {
					category = DM_GROUP;
					break;
				}
			}
		}
		if (file_mode == (mode_t)-1) {
			// no mode, assume OK
			return 1;
		}
		/* check access request against permissions */
		mode_t granted = file_mode;
		mode_t request_mask = 0;
		if (oid_mode != (mode_t)-1) {
			granted |= oid_mode;
		}
		switch (category) {
		      case DM_USER:
			// The owner is allowed all privileges
			return 1;
		      case DM_GROUP:
			request_mask = (want_read ? S_IRGRP : 0) |
				       (want_write ? S_IWGRP : 0);
			break;
		      case DM_OTHER:
			request_mask = (want_read ? S_IROTH : 0) |
				       (want_write ? S_IWOTH : 0);
			break;
		}
		return (granted & request_mask) == request_mask;
	} else if (strcmp(db_basename, _TT_LINK_PROP) == 0 ||
		   strcmp(db_basename, _TT_LINK_ENDS) == 0) {
		mode_t request_mask = 0;

		/* get the LINK access permissions */
		if (_tt_get_link_permissions((char *)rec, prefix_len, db_path,
					     oid_uid, oid_group, oid_mode)) {
			// no link permissions, error
			_tt_syslog(errstr, LOG_ERR,
				   "_tt_get_link_permissions() != 0");
			return 0;
		}
		if (_tt_uid == oid_uid) {
			category = DM_USER;
		} else {
			for (int i = 0; i < _tt_gidlen; i++) {
				if (_tt_gidlist[i] == oid_group) {
					category = DM_GROUP;
					break;
				}
			}
		}
		if (oid_mode == (mode_t)-1) {
			// no mode, assume OK
			return 1;
		}
		/* check LINK access request against permissions */
		switch (category) {
		      case DM_USER:
			switch (mode) {
			      case 'r':
				// everything fails if the owner can't read
				// the link, so just let him.
				return 1;
				// request_mask = S_IRUSR;
				// break;
			      case 'w':
				request_mask = S_IWUSR;
				break;
			      case ('r' + 'w'):
				request_mask = S_IRUSR + S_IWUSR;
				break;
			}
			break;
		      case DM_GROUP:
			switch (mode) {
			      case 'r':
				request_mask = S_IRGRP;
				break;
			      case 'w':
				request_mask = S_IWGRP;
				break;
			      case ('r' + 'w'):
				request_mask = S_IRGRP + S_IWGRP;
				break;
			}
			break;
		      case DM_OTHER:
			switch (mode) {
			      case 'r':
				request_mask = S_IROTH;
				break;
			      case 'w':
				request_mask = S_IWOTH;
				break;
			      case ('r' + 'w'):
				request_mask = S_IROTH + S_IWOTH;
				break;
			}
			break;
		}
		if ((request_mask & oid_mode) == 0) {
			return 0;
		}
	}
	return 1;
}

/*
 * Read the oid owner as a uid.
 */
_Tt_dm_status
_tt_read_oid_user(int isfd, const char *key, uid_t &uid,
		  gid_t &group, mode_t &mode)
{
	static const char *here = "_tt_read_oid_user()";
	memcpy(_tt_record, key, OID_KEY_LENGTH);
	if (isstart(isfd, &_tt_oid_keydesc, OID_KEY_LENGTH, _tt_record,
		    ISEQUAL) == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isstart(): %d", here, iserrno);
		if (iserrno == ENOREC) {
			return DM_NO_RECORD;
		} else {
			return DM_ERROR;
		}
	}
	if (isread(isfd, _tt_record, ISNEXT) == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isread(): %d", here, iserrno);
		return DM_ERROR;
	}
	_Tt_oid_access_ptr oa = new _Tt_oid_access(_tt_record);
	uid = oa->user();
	group = oa->group();
	mode = oa->mode();
	return DM_OK;
}

/*
 * Write out the oid access fields.
 *
 * The uid can only be written if there does not already exist one.
 * The group can only be set by the owner and must be a group of the user.
 * The mode can only be changed by the owner.
 *
 * The superuser can change anything.
 */
_Tt_dm_status
_tt_write_oid_access(int isfd, const char *key, uid_t uid,
		     gid_t group, mode_t mode)
{
	static const char *here = "_tt_write_oid_access()";
	_Tt_oid_access_ptr oa;

	memcpy(_tt_record, key, OID_KEY_LENGTH);
	if (isstart(isfd, &_tt_oid_keydesc, OID_KEY_LENGTH, _tt_record,
		    ISEQUAL) == -1) {
		if (iserrno != ENOREC) {
			_tt_syslog(errstr, LOG_ERR, "%s: isstart(): %d",
				   here, iserrno);
			return DM_ERROR;
		}
		// no record found so we are writing for the first time
		if (uid == (uid_t)-1) {
			// no record and we are not writing the uid
			return DM_ACCESS_DENIED;
		}
		if (_tt_uid == _TT_ROOT_UID) {
			// superuser can do anything
		} else if (_tt_uid != uid) {
			// Can only write yourself as owner
			return DM_ACCESS_DENIED;
		}
		oa = new _Tt_oid_access(key, uid, group, mode);
		isreclen = oa->reclen();
		if (iswrite(isfd, oa->rec()) == -1) {
			return DM_ERROR;
		}
	} else {
		if (isread(isfd, _tt_record, ISNEXT) == -1) {
			_tt_syslog(errstr, LOG_ERR, "%s: isread(): %d",
				   here, iserrno);
			return DM_ERROR;
		}
		oa = new _Tt_oid_access(_tt_record);
		uid_t owner = oa->user();
		int valid_group = 0;
		for (int i = 0; i < _tt_gidlen; i++) {
			if (group == _tt_gidlist[i]) {
				valid_group = 1;
				break;
			}
		}
		if (_tt_uid == _TT_ROOT_UID) {
			// superuser can do anything
		} else if (uid != (uid_t)-1) {
			// can't change uid
			return DM_ACCESS_DENIED;
		} else if (group != (gid_t)-1 &&
		           (_tt_uid != owner || !valid_group)) {
			// only owner can change group and group must be valid
			return DM_ACCESS_DENIED;
		} else if (mode != (mode_t)-1 && _tt_uid != owner) {
			// only owner can change mode
			return DM_ACCESS_DENIED;
		}
		if (uid != (uid_t)-1) {
			oa->set_user(uid);
		}
		if (group != (gid_t)-1) {
			oa->set_group(group);
		}
		if (mode != (mode_t)-1) {
			oa->set_mode(mode);
		}
		isreclen = oa->reclen();
		if (isrewrec(isfd, isrecnum, oa->rec()) == -1) {
			return DM_ERROR;
		}
		// Flush any cached access values
		_Tt_oid_access_ptr oa = _tt_oa_cache->lookup(key);
		if (! oa.is_null()) {
			_tt_oa_cache->remove(oa);
		}
		_Tt_link_access_ptr la = _tt_la_cache->lookup(key);
		if (! la.is_null()) {
			_tt_la_cache->remove(la);
		}
	}
	return DM_OK;
}

/*
 *  _tt_update_modtime - count up the modification date of the object OIDKEY,
 *  the value of its property _MODIFICATION_DATE, if it has one.  KEY and
 *  KEY_LEN are those of the request, the descriptor of the key and the length
 *  of the key.
 */

void
_tt_update_modtime(int			  isfd,
		   keydesc 		* key,
		   int			  key_len,
		   const char		* oidkey,
		   u_int		  oidkey_len,
		   _Tt_isam_results	& res)
{
	static const char *here = "_tt_update_modtime()";
	if (!_tt_set_record_key(oidkey, oidkey_len, _MP_NODE_MOD_PROP,
				strlen(_MP_NODE_MOD_PROP))) {
		res.result = -1;
		res.iserrno = ERPC;
		return;
	}
	res.result = isstart(isfd, key, key_len, _tt_record, ISEQUAL);
	if (res.result == -1) {
		if (iserrno != ENOREC && iserrno != EENDFILE) {
			_tt_syslog(errstr, LOG_ERR, "%s: isstart(): %d",
				   here, iserrno);
			res.iserrno = iserrno;
			return;
		}
		// no mod time?
		res.result = 0;
		res.iserrno = 0;
		return;
	}
	res.result = isread(isfd, _tt_record, ISNEXT);
	if (res.result == -1) {
		if (iserrno != ENOREC && iserrno != EENDFILE) {
			_tt_syslog(errstr, LOG_ERR, "%s: isread(): %d",
				   here, iserrno);
			res.iserrno = iserrno;
			return;
		}
		// no mod time?
		res.result = 0;
		res.iserrno = 0;
		return;
	}

	int value_len = isreclen - MAX_PROP_LENGTH - OID_KEY_LENGTH;
	if (value_len < 0) {
		value_len = 0;
	}
	_Tt_string last_mod_time_string((const unsigned char *)
					&_tt_record[OID_KEY_LENGTH +
						    MAX_PROP_LENGTH],
					value_len);
	long last_mod_time = atol((char *)last_mod_time_string);
	if (last_mod_time < 0 || last_mod_time == LONG_MAX) {
		last_mod_time = 0;
	}
	else {
		last_mod_time++;
	}

	char mod_string[32];
	snprintf(mod_string, sizeof(mod_string), "%ld", last_mod_time);
	memcpy(&_tt_record[OID_KEY_LENGTH + MAX_PROP_LENGTH], mod_string,
	       strlen(mod_string));
	isreclen = OID_KEY_LENGTH + MAX_PROP_LENGTH + strlen(mod_string);
	res.result = isrewcurr(isfd, _tt_record);
	res.iserrno = iserrno;
	return;
}

/*
 *  The clients of the old servers name a table of their database
 *  <partition>/TT_DB/<table>, where <table> is one of the names the old
 *  servers gave the tables of a partition (oid_prop, oid_access,
 *  oid_container, docoid_path).  The server maps those to the tables of the
 *  layout that replaced them, and builds, opens and erases nothing else:
 *  every other file is no ToolTalk database (bugid 1049947).
 *
 *  _tt_table_path_ok() holds the path to that.  It is an absolute path
 *  of components that are no "." and no "..", the last of which is the
 *  table and the one before it is TT_DB itself, not a name that has TT_DB
 *  in it.  The directory above TT_DB is a partition the server keeps
 *  databases for, and TT_DB is a directory of its own, not a link to one
 *  somewhere else.  PARTITION is the name of that partition.
 */

static int
_tt_table_path_ok(const char *path, _Tt_string &partition)
{
	if (path == NULL || path[0] != '/' || strlen(path) > (size_t)PATH_LENGTH) {
		return 0;
	}

	// the start (a slash) of the last two components
	const char *prev = NULL, *last = NULL;
	for (const char *p = path; *p != '\0'; ) {
		const char *name = p + 1;
		size_t n = strcspn(name, "/");

		if (n == 0 || (name[0] == '.' && (n == 1 ||
						  (n == 2 && name[1] == '.')))) {
			return 0;
		}
		prev = last;
		last = p;
		p = name + n;
	}
	if (prev == NULL ||
	    (size_t)(last - prev - 1) != strlen("TT_DB") ||
	    memcmp(prev + 1, "TT_DB", strlen("TT_DB")) != 0) {
		return 0;
	}

	partition = (prev == path) ? _Tt_string("/") :
		_Tt_string((const unsigned char *)path, (int)(prev - path));
	if (!_Tt_db_partition_global_map_ref::isKnownPartition(partition)) {
		return 0;
	}

	_Tt_string dir((const unsigned char *)path, (int)(last - path));
	struct stat st;
	if (lstat((char *)dir, &st) == 0 && !S_ISDIR(st.st_mode)) {
		return 0;
	}
	return 1;
}

/*
 *  The files of a table are TABLE.rec, TABLE.ind and, if the records are of
 *  varying length, TABLE.var: none of them is to be a link to a file
 *  somewhere else.  Returns 0 if one is.
 */

static int
_tt_table_files_ok(const char *table)
{
	static const char *suffixes[] = { ".rec", ".ind", ".var" };
	size_t len = strlen(table);

	for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
		char *file = (char *)malloc(len + strlen(suffixes[i]) + 1);
		struct stat st;
		int link;

		if (file == NULL) {
			return 0;
		}
		strcpy(file, table);
		strcat(file, suffixes[i]);
		link = (lstat(file, &st) == 0 && S_ISLNK(st.st_mode));
		free(file);
		if (link) {
			return 0;
		}
	}
	return 1;
}

/*
 *  Is the table PATH names one of an old server (the name is one of those,
 *  which map_old_db_to_new_db() knows)?
 */

static int
_tt_old_table_name(const char *path)
{
	return find_endstring(path, _TT_DOCOID_PATH) > 0 ||
		find_endstring(path, _TT_OID_CONTAINER) > 0 ||
		find_endstring(path, _TT_OID_PROP) > 0 ||
		find_endstring(path, _TT_OID_ACCESS) > 0;
}

static _Tt_isam_results *
_tt_refuse_table()
{
	// Might consider logging this one to the console, since it's
	// evidence of an attempted security violation.
	res.result = -1;
	res.iserrno = DM_ACCESS_DENIED;
	_tt_syslog(errstr, LOG_ERR,
		   catgets(_ttcatd, 5, 3, "Security violation: "
			   "RPC call wanted me to open a file that "
			   "is not a ToolTalk database"));
	return (&res);
}

/*
 *  _tt_min_auth_level_1 - returns the minimally required authentication level.
 */

int *
_tt_min_auth_level_1(char ** /* file */, SVCXPRT * /* transp */)
{
	return &_tt_auth_level;
}

/*
 *  _tt_isaddindex_1 - wrapper for NetISAM isaddindex
 */

_Tt_isam_results *
_tt_isaddindex_1(_Tt_isaddindex_args *args, SVCXPRT * /* transp */)
{
	/* check for stale NetISAM file descriptor */
	if (!_tt_check_stale_isfd(args->isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
	} else if (args->key == NULL) {
		res.result = -1;
		res.iserrno = EBADKEY;
	} else {
		res.result = isaddindex(args->isfd, args->key);
		res.iserrno = iserrno;
	}
	return (&res);
}

/*
 *  _tt_isbuild_1 - wrapper for NetISAM isbuild.  The input path is in the form
 *	<host>::<local path>
 */

_Tt_isam_results *
_tt_isbuild_1(_Tt_isbuild_args *args, SVCXPRT * /* transp */)
{
	static const char *here = "_tt_isbuild_1()";
        _Tt_string db_path = (char *)NULL;
	_Tt_string partition;
	mode_t prev_umask;
	isreclen = args->isreclen;
	if (!_tt_table_path_ok(args->path, partition) ||
	    !_tt_table_files_ok(args->path)) {
		return _tt_refuse_table();
	}
	db_path = map_old_db_to_new_db(args->path);
	if (db_path.is_null() || db_path.len() == 0 || args->key == NULL) {
		// not a path of a table, or no key descriptor
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: no table or no key", here);
		return (&res);
	}
	if (!_tt_table_files_ok(db_path)) {
		return _tt_refuse_table();
	}
	prev_umask = umask(~(S_IRUSR+S_IWUSR));	// Only I can read and write
	res.result = isbuild(db_path, args->reclen, args->key, args->mode);
	umask(prev_umask);
	if (res.result == -1) {
		char *root_dir = (char *)db_path;
		int dir_len;
		if (root_dir) {
			dir_len = _Tt_dirname(root_dir);
		} else {
			dir_len = 0;
		}
		if (dir_len <= 0 ||
		    ((iserrno != ENOENT) && (iserrno != EFNAME))) {
			// no root dir to build or strange error
			_tt_syslog(errstr, LOG_ERR, "%s: isbuild(): %d",
				   here, iserrno);
			res.iserrno = iserrno;
			return (&res);
		}
		// failed because dir doesn't exist - create it
		char *dir = (char *)malloc(dir_len+2);
		memcpy(dir, root_dir, dir_len+1);
		dir[dir_len+1] = '\0';
		/*
		 * Create the TT_DIR directory with read and search (x)
		 * permissions for everyone.  Can't think of any harm that
		 * could be done by allowing people to see the file names,
		 * and it would be irritating to somebody poking around
		 * looking for a problem not to be able to cd into the
		 * directory.  Setting the group search (x) permission is
		 * particularly important, else if set-gid is set for the
		 * directory (to get BSD semantics) ls shows the perms as
		 * rwx--S---, and the capital S looks like a bug to most
		 * people.
		 */
		if (mkdir(dir, S_IRWXU+S_IRGRP+S_IXGRP+S_IROTH+S_IXOTH)
		    == -1) {
			_tt_syslog(errstr, LOG_ERR, "dir: %m", dir);
			free(dir);
			res.iserrno = iserrno;
			return (&res);
		}
		free(dir);

		prev_umask = umask(~(S_IRUSR+S_IWUSR));
		res.result = isbuild((char *)db_path, args->reclen,
				     args->key, args->mode);
		umask(prev_umask);
		if (res.result == -1) {
			_tt_syslog(errstr, LOG_ERR, "%s: isbuild(): %d",
				   here, iserrno);
			res.iserrno = iserrno;
			return (&res);
		}
	}
	if (res.result >= _TT_MAX_ISFD) {
		// NetISAM has more descriptors than _tt_db_table has entries
		(void)isclose(res.result);
		res.result = -1;
		res.iserrno = ETOOMANY;
		return (&res);
	}
	_tt_db_table[res.result].db_path = db_path;
	_tt_db_table[res.result].opener_uid = _tt_uid;
	res.iserrno = iserrno;
	return (&res);
}

/*
 *  _tt_isclose_1 - wrapper for NetISAM isclose
 */

_Tt_isam_results *
_tt_isclose_1(int *isfd, SVCXPRT * /* transp */)
{
	static const char *here = "_tt_isclose_1()";
	if (!_tt_check_stale_isfd(*isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
	} else {
	  // JET - 06/12/2002
	  // VU#975403 - CERT TT vulnerability.  By passing an invalid isfd
	  // a local or remote attacker can zero out 4 bytes at any location,
	  // thereby allowing other exploits (items 2 & 3 - delete or
	  // overwrite any file on the system.)
	  // _tt_check_stale_isfd() makes sure: 0 >= isfd < _TT_MAX_ISFD

	  res.result = cached_isclose(*isfd);
	  if (res.result != -1) {
	    _tt_db_table[*isfd].db_path = 0;
	    _tt_db_table[*isfd].opener_uid = (uid_t)-1;
	  } else {
	    _tt_syslog(errstr, LOG_ERR, "%s: isclose(): %d",
		       here, iserrno);
	  }
	  res.iserrno = iserrno;
	}
	return (&res);
}

/*
 *  _tt_iscntl_1 - wrapper for NetISAM iscntl
 */

_Tt_iscntl_results *
_tt_iscntl_1(_Tt_iscntl_args *args, SVCXPRT * /* transp */)
{
	static const char *here = "_tt_iscntl_1()";
	static _Tt_iscntl_results res;
	static char magic[] = "   1";	/* xdr_char() stores what it sends */
	if (!_tt_check_stale_isfd(args->isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
	} else {
/*
		res.result = iscntl(args->isfd, args->func, args->arg.arg_val);
		res.iserrno = iserrno;
		res.arg.arg_val = args->arg.arg_val;
		res.arg.arg_len = args->arg.arg_len;
		if (res.result == -1) {
			_tt_syslog(errstr, LOG_ERR, "%s: iscntl(): %d",
				   here, iserrno);
		}
*/
		res.iserrno = 0;
		res.arg.arg_val = magic;
		res.arg.arg_len = 4;
		res.result = 0;
	}
	return (&res);
}

/*
 *  _tt_isdelrec_1 - wrapper for NetISAM isdelrec
 */

_Tt_isam_results *
_tt_isdelrec_1(_Tt_isdelrec_args *args, SVCXPRT *transp)
{
	static const char *here = "_tt_isdelrec_1()";
	if (!_tt_check_stale_isfd(args->isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
	} else {
		if (args->rec.rec_len > 0) {
			if (_tt_oid_accessp(args->isfd, args->rec.rec_val,
						   args->rec.rec_len, 'w')) {
			  if (msg_q_lock(args->isfd,
					 args->rec.rec_val,
					 args->rec.rec_len,
					 transp)) {
			    res.result = isdelrec(args->isfd, args->recnum);
			    res.iserrno = iserrno;
			  }
			  else {
			    res.result = -1;
			    res.iserrno = ERPC;
			  }
			} else {
				res.result = -1;
				res.iserrno = DM_ACCESS_DENIED;
			}
		} else {
			res.result = -1;
			res.iserrno = ERPC;
			_tt_syslog(errstr, LOG_ERR, "%s: rec_len <= 0", here);
		}
	}
	return (&res);
}

/*
 *  _tt_iserase_1 - wrapper for NetISAM iserase
 */

_Tt_isam_results *
_tt_iserase_1(char **path, SVCXPRT * /* transp */)
{
	_Tt_string partition;

	// the table of an old server by the name it had: the tables of the
	// layout that replaced them have other names, and are not erased
	if (path == NULL || !_tt_table_path_ok(*path, partition) ||
	    !_tt_old_table_name(*path) || !_tt_table_files_ok(*path)) {
		return _tt_refuse_table();
	}
	res.result = iserase(*path);
	res.iserrno = iserrno;
	if (res.result == -1) {
		_tt_syslog(errstr, LOG_ERR, "iserase(): %d", iserrno);
	}
	return (&res);
}

/*
 *  _tt_isopen_1 - wrapper for NetISAM isopen
 */

_Tt_isam_results *
_tt_isopen_1(_Tt_isopen_args *args, SVCXPRT * /* transp */)
{
	static const char *here = "_tt_isopen_1()";
	_Tt_string db_path = args->path;
	_Tt_string partition;
	if (db_path.is_null()) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: args->path.is_null()", here);
		return (&res);
	}
	// bugid 1049947: someone could use our dbserver to get at
	// other isam files on the same machine.  The path has to be that of
	// a table of the TT_DB directory of a partition (see
	// _tt_table_path_ok()), and neither it nor the files of the table
	// may be a link to somewhere else.  The "right way" to protect the
	// other files would be to write a distinctive signature into the
	// APPL_MAGIC bucket, and check for that signature on open.
	if (!_tt_table_path_ok(args->path, partition) ||
	    !_tt_table_files_ok(args->path)) {
		return _tt_refuse_table();
	}
	int prefix_len = strrchr((char *)db_path, '/') - (char *)db_path;

	memcpy(_tt_log_file, (char *)db_path, prefix_len);
	snprintf(_tt_log_file + prefix_len, MAXPATHLEN - prefix_len, "%s", _TT_LOG_FILE);
	if (access(_tt_log_file, F_OK) == 0) {
		_tt_process_transaction();
	}

	db_path = map_old_db_to_new_db(args->path);
	if (db_path.is_null() || db_path.len() == 0) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: map_old_db_to_new_db() == 0",
			   here);
		return (&res);
	}
	if (!_tt_table_files_ok(db_path)) {
		return _tt_refuse_table();
	}

	res.result = cached_isopen((char *)db_path, args->mode);
	if (res.result != -1) {	/* record database name and isfd */
	  _tt_db_table[res.result].db_path = db_path;
	  _tt_db_table[res.result].opener_uid = _tt_uid;
	} else {
	  //
	  // Try to create the file.
	  // The constructor create the files.
	  // Then call isopen again,
	  // the files will then exist. bug# 1179660
	  //
	  // The database is made in the partition the path is in, which is
	  // the one the client named.
	  _Tt_db_server_db	* db;

	  db = new _Tt_db_server_db(partition);
	  delete db;

	  res.result = cached_isopen((char *)db_path, args->mode);
	  if (res.result != -1) {	/* record database name and isfd */
	    _tt_db_table[res.result].db_path = db_path;
	    _tt_db_table[res.result].opener_uid = _tt_uid;
	  } else {
	    _tt_syslog(errstr, LOG_ERR, "%s: isopen(): %d", here, iserrno);
	  }
	}
	res.iserrno = iserrno;
	return (&res);
}

/*
 *  _tt_isread_1 - wrapper for NetISAM isread
 */

_Tt_isread_results *
_tt_isread_1(_Tt_isread_args *args, SVCXPRT *transp)
{
	static const char *here = "_tt_isread_1()";
	static _Tt_isread_results res;
	// a reply that fails must not carry the record of an earlier request
	res.rec.rec_len = 0;
	res.rec.rec_val = 0;
	if (!_tt_check_stale_isfd(args->isfd)) {
		res.isresult.result = -1;
		res.isresult.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
		return (&res);
	}
	if (_tt_oid_accessp(args->isfd, args->rec.rec_val,
				   args->rec.rec_len, 'r')) {

		isrecnum = args->isrecnum;
		_tt_client_record(_tt_record, args->rec.rec_val,
				  args->rec.rec_len);

		if (msg_q_lock(args->isfd,
			       args->rec.rec_val,
			       args->rec.rec_len,
			       transp)) {
			
			res.isresult.result = isread(args->isfd,
						     _tt_record,
						     args->mode);

#ifdef Foo
			//
			// Do not return the _MODIFICATIN_DATE information
			// to 4.x client systems (the only ones calling
			// this function). That is only in the file:
			//
			//	TT_DB_PROPERTY_TABLE_FILE (table_fn below)
			//
			// NOTE:
			//
			//_MODIFICATION_DATE==TT_DB_PROPS_CACHE_LEVEL_PROPERTY.
			//
			// The record format is:
			//
			//	------------------------------
			//	| OID-KEY | PROPERTY | VALUE |
			//	------------------------------
			//
			// and OID_KEY_LENGTH is the size of OID-KEY.
			//
			
			const char *table_fn = TT_DB_PROPERTY_TABLE_FILE;
			const char *mod_name=TT_DB_PROPS_CACHE_LEVEL_PROPERTY;

			//
			// Find the last slash in the full path name so
			// we can compare the file name (not dir names).
			//
			char *file = strrchr(_tt_db_table[args->isfd].db_path,
					     '/');
			if (file == NULL) {
				// Hack to avoid core dump if no '/' in name.
				file = _tt_db_table[args->isfd].db_path;
			} else {
				file++;	// Point past '/' to file name.
			}
			if ((strncmp(table_fn, file, strlen(table_fn)) == 0)
			    && (strncmp(mod_name,
					&_tt_record[OID_KEY_LENGTH],
					strlen(mod_name)) ==0)) {
				//
				// If you find a _MODIFICATION_DATE then,
				// Just return the next record or error.
				//
				return _tt_isread_1(args, transp);

			} else {
#endif // Foo
				res.isresult.iserrno = iserrno;
				res.rec.rec_len = isreclen;
				res.rec.rec_val = _tt_record;
#ifdef Foo
			}
#endif
		} else {
			res.isresult.result = -1;
			res.isresult.iserrno = ERPC;
		}
		
		if (res.isresult.result == -1 && iserrno != EENDFILE) {
			_tt_syslog(errstr, LOG_ERR, "%s: isread(): %d",
				   here, iserrno);
		}
	} else {
		res.isresult.result = -1;
		res.isresult.iserrno = DM_ACCESS_DENIED;
		res.rec.rec_len = 0;
		res.rec.rec_val = 0;
	}
	res.isreclen = isreclen;
	res.isrecnum = isrecnum;
	return (&res);
}

/*
 *  _tt_isrewrec_1 - wrapper for NetISAM isrewrec
 */

_Tt_isam_results *
_tt_isrewrec_1(_Tt_isrewrec_args *args, SVCXPRT *transp)
{
	static const char *here = "_tt_isrewrec_1()";
	if (!_tt_check_stale_isfd(args->isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
	} else {
		if (args->rec.rec_len > 0) {
			if (_tt_oid_accessp(args->isfd, args->rec.rec_val,
						   args->rec.rec_len, 'w')) {
			  if (msg_q_lock(args->isfd,
					 args->rec.rec_val,
					 args->rec.rec_len,
					 transp)) {
			    _tt_client_record(_tt_record, args->rec.rec_val,
				      args->rec.rec_len);
			    isreclen = args->rec.rec_len;
			    res.result = isrewrec(args->isfd, args->recnum,
						  _tt_record);
			    res.iserrno = iserrno;
			    if (res.result == -1) {
				    _tt_syslog(errstr, LOG_ERR, "%s: isrewrec"
					       "(): %d", here, iserrno);
			    }
			  }
			  else {
			    res.result = -1;
			    res.iserrno = ERPC;
			  }
			} else {
				res.result = -1;
				res.iserrno = DM_ACCESS_DENIED;
			}
		} else {
			res.result = -1;
			res.iserrno = ERPC;
			_tt_syslog(errstr, LOG_ERR, "%s: rec_len <= 0", here);
		}
	}
	return (&res);
}

/*
 *  _tt_isstart_1 - wrapper for NetISAM isstart
 */

_Tt_isam_results *
_tt_isstart_1(_Tt_isstart_args *args, SVCXPRT *transp)
{
	static const char *here = "_tt_isstart_1";
	if (!_tt_check_stale_isfd(args->isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
	} else if (args->key == NULL) {
		res.result = -1;
		res.iserrno = EBADKEY;
		_tt_syslog(errstr, LOG_ERR, "%s: key == 0", here);
	} else if (args->rec.rec_val==0 && args->mode!=ISFIRST) {
		res.result = -1;
		res.iserrno = EBADKEY;
		_tt_syslog(errstr, LOG_ERR, "rec_val == 0 && mode != ISFIRST");
	} else if (msg_q_lock(args->isfd,
			      args->rec.rec_val,
			      args->rec.rec_len,
			      transp)) {
		_tt_client_record(_tt_record, args->rec.rec_val,
				  args->rec.rec_len);
		res.result = isstart(args->isfd, args->key, args->key_len,
				     _tt_record, args->mode);
		res.iserrno = iserrno;
		if (res.result == -1) {
			// ENOREC happens a lot even in normal operation.
			if (iserrno!=ENOREC) {
				_tt_syslog(errstr, LOG_ERR, "%s: isstart(): %d",
					   here, iserrno);
			}
		}
	}
	else {
	        res.result = -1;
		res.iserrno = ERPC;
	}
	return (&res);
}

/*
 *  _tt_iswrite_1 - wrapper for NetISAM iswrite
 */

_Tt_isam_results *
_tt_iswrite_1(_Tt_iswrite_args *args, SVCXPRT *transp)
{
	static const char *here = "_tt_iswrite_1()";
	if (!_tt_check_stale_isfd(args->isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
	} else {
		if (args->rec.rec_len > 0) {
			if (_tt_oid_accessp(args->isfd,
					    args->rec.rec_val,
					    args->rec.rec_len, 'w')) {
			  if (msg_q_lock(args->isfd,
					 args->rec.rec_val,
					 args->rec.rec_len,
					 transp)) {
			    _tt_client_record(_tt_record, args->rec.rec_val,
				      args->rec.rec_len);
			    isreclen = args->rec.rec_len;
			    res.result = iswrite(args->isfd, _tt_record);
			    res.iserrno = iserrno;
			    if (res.result == -1) {
				    _tt_syslog(errstr, LOG_ERR, "%s: iswrite"
					       "(): %d", here, iserrno);
			    }
			  }
			  else {
			    res.result = -1;
			    res.iserrno = ERPC;
			  }
			} else {
				res.result = -1;
				res.iserrno = DM_ACCESS_DENIED;
			}
		} else {
			res.result = -1;
			res.iserrno = ERPC;
		}
	}
	return (&res);
}

/*
 *  _tt_test_and_set_1 - reads (test) the record with the given input
 *  (key + prop name).  If the record already existed, returns it and 0.
 *  Otherwise, writes (set) the given record out and returns 1.  If any error,
 *  returns -1.
 */

_Tt_test_and_set_results *
_tt_test_and_set_1(_Tt_test_and_set_args *args, SVCXPRT *transp)
{
	static const char *here = "_tt_test_and_set_1()";
	static _Tt_test_and_set_results res;
	// a reply that fails must not carry the record of an earlier request
	res.rec.rec_len = 0;
	res.rec.rec_val = 0;
	if (!_tt_check_stale_isfd(args->isfd)) {
		res.isresult.result = -1;
		res.isresult.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
		return (&res);
	}
	if (args->key == NULL) {
		res.isresult.result = -1;
		res.isresult.iserrno = EBADKEY;
		_tt_syslog(errstr, LOG_ERR, "%s: key == 0", here);
		return (&res);
	}
	if (!_tt_oid_accessp(args->isfd, args->rec.rec_val,
				   args->rec.rec_len, 'r' + 'w')) {
		res.isresult.result = -1;
		res.isresult.iserrno = DM_ACCESS_DENIED;
		res.rec.rec_len = 0;
		res.rec.rec_val = 0;
		return (&res);
	}
	if (!msg_q_lock(args->isfd,
			args->rec.rec_val,
			args->rec.rec_len,
			transp)) {
	        res.isresult.result = -1;
		res.isresult.iserrno = ERPC;
		res.rec.rec_len = 0;
		res.rec.rec_val = 0;
		return (&res);
	}
	_tt_client_record(_tt_record, args->rec.rec_val, args->rec.rec_len);
	// look for this record
	if (isstart(args->isfd, args->key, args->key_len,
		    _tt_record, ISEQUAL) == -1) {
		if (iserrno == ENOREC) {
			// not found, we can write ours
			isreclen = args->rec.rec_len;
			if (iswrite(args->isfd, _tt_record)
			    != -1) {
				res.isresult.result = 1;
				res.rec.rec_len = 0;
				res.rec.rec_val = 0;
				res.isrecnum = isrecnum;
				return (&res);
			}
			_tt_syslog(errstr, LOG_ERR, "%s: iswrite(): %d",
				   here, iserrno);
			res.isresult.iserrno = iserrno;
			res.isresult.result = -1;
			res.rec.rec_len = 0;
			res.rec.rec_val = 0;
			return (&res);
		} else {
			_tt_syslog(errstr, LOG_ERR, "%s: isstart(): %d",
				   here, iserrno);
			res.isresult.iserrno = iserrno;
			res.isresult.result = -1;
			res.rec.rec_len = 0;
			res.rec.rec_val = 0;
			return (&res);
		}
	}
	// we found a record, read it and return it
	if (isread(args->isfd, _tt_record, ISNEXT) != -1) {
		res.isresult.result = 0;
		res.isresult.iserrno = iserrno;
		res.rec.rec_len = isreclen;
		res.rec.rec_val = _tt_record;
		res.isrecnum = isrecnum;
		return (&res);
	}
	// EENDFILE happens a lot even in normal operation.
	if (iserrno && iserrno!=EENDFILE) {
		_tt_syslog(errstr, LOG_ERR, "%s: isread(): %d",
			   here, iserrno);
	}
	res.isresult.iserrno = iserrno;
	res.isresult.result = -1;
	res.rec.rec_len = 0;
	res.rec.rec_val = 0;
	return (&res);
}


/*
 *  _tt_transaction_1 - given a list of records, writes them out to the log file
 *  associated with the given database.  If the log file writes succeed returns
 *  0, otherwise returns -1.  The actual updating of database records is done
 *  by procedure _tt_process_transaction and is invoked by _tt_dbserver_prog_1
 *  after a reply is sent to the client.  In case where there's a crash, the
 *  server recovers by checking for the existents of log files every time it
 *  opens a database and invoking _tt_process_transaction.  The log file is in
 *  the same directory as that of the database.  The log file's name is
 *  "log_file".
 *
 *  The log file has the following format:
 *	<transaction begin/commit flag>
 *	<path of the target database of the transaction>
 *	<the set of records to be written/updated>,
 *		where each element of the set is
 *		<record number, record length, record data>
 */

_Tt_isam_results *
_tt_transaction_error(int fd)
{
	if (fd != -1) {
		close(fd);
		unlink(_tt_log_file);
		_tt_log_file[0] = '\0';
	}
	res.result = -1;
	return (&res);
}

/*
 *  A transaction writes all of its records, so the client needs the
 *  permission to write each of them, not that of the first alone.
 */

static int
_tt_transaction_accessp(int isfd, const _Tt_trans_record *trec)
{
	for (; trec != NULL; trec = trec->next) {
		if (!_tt_oid_accessp(isfd, trec->rec.rec_val, trec->rec.rec_len,
				     'w')) {
			return 0;
		}
	}
	return 1;
}

_Tt_isam_results *
_tt_transaction_1(_Tt_transaction_args* args, SVCXPRT * /* transp */)
{
        struct stat buf; // JET - VU#975403/VU#299816

	static const char *here = "_tt_transaction_1()";
	/* check for stale NetISAM file descriptor */
	if (!_tt_check_stale_isfd(args->isfd)) {
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
		// no log file of ours yet, and the isfd is not ours to close
		return _tt_transaction_error(-1);
	}
	_Tt_trans_record *trec = args->recs;
	if (!trec) {
		res.iserrno = ERPC;
		return _tt_transaction_error(-1);
	}
	int fd = -1;
	_tt_log_file[0] = '\0';
	/* Must have write permission to start transaction */
	if (_tt_transaction_accessp(args->isfd, trec)) {
		char *db_path = _tt_db_table[args->isfd].db_path;
		if (!db_path) {
			res.iserrno = ERPC;
			return _tt_transaction_error(fd);
		}
		int prefix_len = _Tt_dirname(db_path) + 1;
		if (prefix_len < 1) {
			res.iserrno = ERPC;
			return _tt_transaction_error(fd);
		}
		memcpy(_tt_log_file, db_path, prefix_len);
		snprintf(_tt_log_file + prefix_len, MAXPATHLEN - prefix_len, "%s", _TT_LOG_FILE);
		if (access(_tt_log_file, F_OK) == 0) {
			_tt_process_transaction();
		}

		// JET - 06/24/2002 VU#975403/VU#299816 - CERT TT
		// vulnerability.  check for the presence of a
		// symlink.  Abort (nicely) if there.

		if(lstat(_tt_log_file, &buf) != -1)
		  {		// present
		    if (S_ISLNK(buf.st_mode))
		      {		// it's a symlink.  Oops.
			_tt_syslog(errstr, LOG_ERR, 
				   "%s: _tt_log_file is a symlink.  Aborting.",
				   here );
			res.result = -1;
			res.iserrno = DM_ACCESS_DENIED;
			return(&res);
		      }
		  }

		if ((fd = open(_tt_log_file,
			       O_RDWR | O_CREAT | O_NOFOLLOW,
			       S_IREAD + S_IWRITE)) == -1) {
			res.iserrno = DM_WRITE_FAILED;
			return _tt_transaction_error(fd);
		}
		/* Turn on close-on-exec */
		if(fcntl(fd, F_SETFD, 1) == -1) {
			res.iserrno = DM_WRITE_FAILED;
			return _tt_transaction_error(fd);
		}

		/* reset to beginning of file */
		off_t offset;
		if ((offset = lseek(fd, 0, SEEK_SET)) == -1) {
			res.iserrno = DM_WRITE_FAILED;
			return _tt_transaction_error(fd);
		}
		/* set the transaction flag to signify begin of transaction */
		int flag = 1;
		if (write(fd, (char *)&flag, sizeof(int)) == -1) {
			res.iserrno = DM_WRITE_FAILED;
			return _tt_transaction_error(fd);
		}
		/* write the target database path out to the log file */
		if (write(fd, db_path, strlen(db_path)+1) == -1) {
			res.iserrno = DM_WRITE_FAILED;
			return _tt_transaction_error(fd);
		}
		/* write the records out to the log file */
		while (trec) {
			/* write the record's new flag out to the log file */
			if (write(fd, (char *)&trec->newp, sizeof(int)) == -1) {
				res.iserrno = DM_WRITE_FAILED;
				return _tt_transaction_error(fd);
			}
			/* write the record number out to the log file */
			if (write(fd, (char *)&trec->recnum, sizeof(long))
			    == -1) {
				res.iserrno = DM_WRITE_FAILED;
				return _tt_transaction_error(fd);
			}
			/* write the record's length out to the log file */
			if (write(fd, (char *)&trec->rec.rec_len, sizeof(u_int))
			    == -1) {
				res.iserrno = DM_WRITE_FAILED;
				return _tt_transaction_error(fd);
			}
			/* write the record's content out to the log file */
			if (write(fd, (char *)trec->rec.rec_val,
				  trec->rec.rec_len) == -1) {
				res.iserrno = DM_WRITE_FAILED;
				return _tt_transaction_error(fd);
			}
			trec = trec->next;
		}
		if (DM_TEST_CRASH) {
			/* Test crash recovery */
			fprintf(stderr,"_tt_transaction_1: simulating server crash to test crash recovery . . . exiting\n");
			exit(1);
		}
		/* clear the transaction flag to signify transaction commit */
		if (lseek(fd, 0, SEEK_SET) == -1) {
			res.iserrno = DM_WRITE_FAILED;
			return _tt_transaction_error(fd);
		}
		flag = 0;
		if (write(fd, (char *)&flag, sizeof(int)) == -1) {
			res.iserrno = DM_WRITE_FAILED;
			return _tt_transaction_error(fd);
		}
		/* close the log file */
		if (close(fd) == -1) {
			res.iserrno = DM_WRITE_FAILED;
			return _tt_transaction_error(fd);
		}
		res.result = 0;
	} else {
		res.result = -1;
		res.iserrno = DM_ACCESS_DENIED;
	}
	return (&res);
}

/*
 *  _tt_mfs_1 - find a mount entry whose mounted directory/file system is the
 *  longest match of the given directory/absolute path.  For example, the mount
 *  entry "/home3" matches "/home3/dynamo" better than the mount entry "/".
 *  If msfp == 1, then match using directory.  Otherwise, match using absolute
 *  path.
 */

char **
_tt_mfs_1(char **path, SVCXPRT * /* transp */)
{
	static char *res;
	res = (char *)"";
	if (*path && **path) {
		_Tt_file_system fs;
		_Tt_file_system_entry_ptr fsep = fs.bestMatchToPath(*path);
		if (!fsep.is_null()) {
			res = fsep->getMountPoint();
		}
	}
	return (&res);
}

/*
 *  The oid access procedures work on the oid key the client sent, which is
 *  read as a whole key, in the table of the isfd it sent: returns 1 if the
 *  client sent one, and is the one who opened the table.
 */
static int
_tt_oid_key_ok(const _Tt_oidaccess_args *args)
{
	if (!_tt_check_stale_isfd(args->isfd)) {
		_tt_syslog(errstr, LOG_ERR, "_tt_check_stale_isfd() == 0");
		return 0;
	}
	return args->key.key_val != NULL && args->key.key_len == OID_KEY_LENGTH;
}

/*
 *  _tt_getoidaccess_1 - get the uid/group/mode for an oid
 */
_Tt_oidaccess_results *
_tt_getoidaccess_1(_Tt_oidaccess_args *args, SVCXPRT * /* transp */)
{
	static _Tt_oidaccess_results res;

	if (!_tt_oid_key_ok(args)) {
		res.result = DM_ERROR;
		res.iserrno = ERPC;
		return (&res);
	}
	res.result = _tt_read_oid_user(args->isfd, args->key.key_val,
				       res.uid, res.group, res.mode);
	res.iserrno = iserrno;
	return (&res);
}

/*
 *  _tt_setoiduser_1 - set the user for an oid
 */
_Tt_isam_results *
_tt_setoiduser_1(_Tt_oidaccess_args *args, SVCXPRT * /* transp */)
{
	if (!_tt_oid_key_ok(args)) {
		res.result = DM_ERROR;
		res.iserrno = ERPC;
		return (&res);
	}
	res.result = _tt_write_oid_access(args->isfd, args->key.key_val,
					  (uid_t)args->value, (gid_t)-1,
					  (mode_t)-1);
	res.iserrno = iserrno;
	return (&res);
}

/*
 *  _tt_setoidgroup_1 - set the user for an oid
 */
_Tt_isam_results *
_tt_setoidgroup_1(_Tt_oidaccess_args *args, SVCXPRT * /* transp */)
{
	if (!_tt_oid_key_ok(args)) {
		res.result = DM_ERROR;
		res.iserrno = ERPC;
		return (&res);
	}
	res.result = _tt_write_oid_access(args->isfd, args->key.key_val,
					  (uid_t)-1, (gid_t)args->value,
					  (mode_t)-1);
	res.iserrno = iserrno;
	return (&res);
}

/*
 *  _tt_setoidmode_1 - set the user for an oid
 */
_Tt_isam_results *
_tt_setoidmode_1(_Tt_oidaccess_args *args, SVCXPRT * /* transp */)
{
	if (!_tt_oid_key_ok(args)) {
		res.result = DM_ERROR;
		res.iserrno = ERPC;
		return (&res);
	}
	res.result = _tt_write_oid_access(args->isfd, args->key.key_val,
					  (uid_t)-1, (gid_t)-1,
					  (mode_t)args->value);
	res.iserrno = iserrno;
	return (&res);
}

_Tt_spec_props *
_tt_readspec_1(_Tt_spec_props * /* argp */, SVCXPRT * /* transp */)
{
	static _Tt_spec_props res;

	res.iserrno = iserrno;
	return (&res);
}

_Tt_isam_results *
_tt_writespec_1(_Tt_spec_props * /* argp */, SVCXPRT * /* transp */)
{
	res.iserrno = iserrno;
	return (&res);
}

/*
 *  The session procedures work on the property _TT_SESSIONS of an object, one
 *  record for each session joined to it, with the session id as its value.
 *  The request must name the object (by its key, the first bytes of every
 *  record of it), carry the length of the key that isstart is to use, and
 *  the session id must fit in the value of a record.
 */
static int
_tt_session_args_ok(const _Tt_session_args *argp)
{
	return argp->key != NULL && argp->key_len > 0 &&
		argp->key_len <= OID_KEY_LENGTH + MAX_PROP_LENGTH &&
		argp->oidkey.oidkey_val != NULL && argp->oidkey.oidkey_len > 0 &&
		argp->oidkey.oidkey_len <= (u_int)OID_KEY_LENGTH &&
		argp->session.session_len <= sizeof(Table_oid_prop::propval);
}

/*
 *  Is the session id in the property record just read, which is isreclen
 *  bytes long, SESSIONID?  The record is compared as it is stored, up to
 *  its own length, never beyond what the client sent.
 */
static int
_tt_session_matches(const _Tt_string &sessionid, const Table_oid_prop *tp)
{
	int len = isreclen - MAX_PROP_LENGTH - OID_KEY_LENGTH;
	return len == sessionid.len() &&
		(len <= 0 || memcmp((char *) sessionid, tp->propval, len) == 0);
}

/*
 *  Is the record just read a session of the object the request names?
 *  Returns 0 once the records of the property of the object are past.
 */
static int
_tt_session_record(const _Tt_session_args *argp, const Table_oid_prop *tp)
{
	return strncmp(_TT_FILEJOIN_PROPNAME, tp->propname, MAX_PROP_LENGTH) == 0 &&
		memcmp(argp->oidkey.oidkey_val, tp->objkey,
		       argp->oidkey.oidkey_len) == 0;
}

/*
 *  The session procedures change the property of an object: its owner, or
 *  who the protection of its file lets, can.  The record the access is
 *  asked for is the object's own, which isstart is about to look for.
 */
static int
_tt_session_accessp(const _Tt_session_args *argp)
{
	char record[OID_KEY_LENGTH];

	memset(record, 0, sizeof(record));
	memcpy(record, argp->oidkey.oidkey_val, argp->oidkey.oidkey_len);
	return _tt_oid_accessp(argp->isfd, record, sizeof(record), 'w');
}

_Tt_isam_results *
_tt_addsession_1(_Tt_session_args *argp, SVCXPRT * /* transp */)
{
	static const char *here = "_tt_addsession_1()";

	Table_oid_prop* tp = (Table_oid_prop *) _tt_record;
	_Tt_string sessionid((unsigned char *)argp->session.session_val,
			     argp->session.session_len);
	int isfd = argp->isfd;
	_Tt_string filejoin_prop(_TT_FILEJOIN_PROPNAME);

	if (!_tt_check_stale_isfd(isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
		return (&res);
	}
	// Start reading session props
	if (!_tt_session_args_ok(argp)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: bad key or session", here);
		return (&res);
	}
	if (!_tt_session_accessp(argp)) {
		res.result = -1;
		res.iserrno = DM_ACCESS_DENIED;
		return (&res);
	}
	// the check has used the request record to read the other tables
	if (!_tt_set_record_key((char *)argp->oidkey.oidkey_val,
				argp->oidkey.oidkey_len,
				_TT_FILEJOIN_PROPNAME,
				strlen(_TT_FILEJOIN_PROPNAME))) {
		res.result = -1;
		res.iserrno = ERPC;
		return (&res);
	}
	res.result = isstart(isfd, argp->key, argp->key_len,
			     _tt_record, ISEQUAL);
	res.iserrno = iserrno;
	if (res.result == -1) {
		if (iserrno != ENOREC && iserrno != EENDFILE) {
			_tt_syslog(errstr, LOG_ERR, "%s: isstart(): %d",
				   here, iserrno);
			return (&res);
		}
		// no record found, no sessions on this doc node
	} else {
		// check to make sure this session is not already registered
		for (;;) {
			res.result = isread(isfd, _tt_record, ISNEXT);
			if (res.result == -1) {
				if (iserrno != ENOREC && iserrno != EENDFILE) {
					_tt_syslog(errstr, LOG_ERR, "%s: isread"
						   "(): %d", here, iserrno);
					res.iserrno = iserrno;
					return (&res);
				}
				break;
			}
			if (!_tt_session_record(argp, tp)) {
				// ran past the sessions of the object
				break;
			}
			if (_tt_session_matches(sessionid, tp)) {
				// already here so we're done
				res.result = 0;
				res.iserrno = 0;
				return (&res);
			}
		}
	}

	// need to add this session to the doc node
	
	// zero out _tt_record, which is where tp points
	memset(_tt_record, 0, sizeof(_tt_record));
	memcpy(tp->objkey, argp->oidkey.oidkey_val, argp->oidkey.oidkey_len);
	snprintf(tp->propname, sizeof(tp->propname), "%s", (char *) filejoin_prop);
	if (sessionid.len() > 0) {
		memcpy(tp->propval, (char *) sessionid, sessionid.len());
	}
	isreclen = OID_KEY_LENGTH+MAX_PROP_LENGTH+sessionid.len();
	res.result = iswrite(isfd, _tt_record);
	if (res.result == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: iswrite(): %d", here, iserrno);
		res.iserrno = iserrno;
		return (&res);
	}
	_tt_update_modtime(isfd, argp->key,
			   argp->key_len,
			   (char *)argp->oidkey.oidkey_val,
			   argp->oidkey.oidkey_len,
			   res);
	return (&res);
}

_Tt_isam_results *
_tt_delsession_1(_Tt_session_args *argp, SVCXPRT * /* transp */)
{
	static const char *here = "_tt_delsession_1()";
	Table_oid_prop* tp = (Table_oid_prop *) _tt_record;
	_Tt_string sessionid((unsigned char *)argp->session.session_val,
			     argp->session.session_len);
	int isfd = argp->isfd;

	if (!_tt_check_stale_isfd(isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
		return (&res);
	}
	// Start reading session props
	if (!_tt_session_args_ok(argp)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: bad key or session", here);
		return (&res);
	}
	if (!_tt_session_accessp(argp)) {
		res.result = -1;
		res.iserrno = DM_ACCESS_DENIED;
		return (&res);
	}
	if (!_tt_set_record_key((char *)argp->oidkey.oidkey_val,
				argp->oidkey.oidkey_len,
				_TT_FILEJOIN_PROPNAME,
				strlen(_TT_FILEJOIN_PROPNAME))) {
		res.result = -1;
		res.iserrno = ERPC;
		return (&res);
	}
	res.result = isstart(isfd, argp->key, argp->key_len,
			     _tt_record, ISEQUAL);
	res.iserrno = iserrno;
	if (res.result == -1) {
//
//     		
//		_tt_syslog(errstr, LOG_ERR, "%s: isstart(): %d", here, iserrno);
		return (&res);
	}
	// look for this session
	int not_found = ENOREC;
	for (;;) {
		res.result = isread(isfd, _tt_record, ISNEXT);
		if (res.result == -1) {
			if (iserrno != ENOREC && iserrno != EENDFILE) {
				_tt_syslog(errstr, LOG_ERR, "%s: isread(): %d",
					   here, iserrno);
				not_found = iserrno;
			}
			break;
		}
		if (!_tt_session_record(argp, tp)) {
			// ran past the sessions of the object
			res.result = -1;
			break;
		}
		if (_tt_session_matches(sessionid, tp)) {
			// we found it, now delete it
			res.result = isdelcurr(isfd);
			if (res.result == -1) {
				_tt_syslog(errstr, LOG_ERR, "%s: isdelcurr"
					   "(): %d", here, iserrno);
				res.iserrno = iserrno;
				return (&res);
			}
			_tt_update_modtime(isfd, argp->key,
					   argp->key_len,
					   (char *)argp->oidkey.oidkey_val,
					   argp->oidkey.oidkey_len,
					   res);
			return (&res);
		}
	}
	// the session is not one of the object's: there is no such record
	res.iserrno = not_found;
	return (&res);
}

_Tt_spec_props *
_tt_gettype_1(_Tt_spec_props *argp, SVCXPRT * /* transp */)
{
	static const char *here = "_tt_gettype_1()";
	static _Tt_spec_props res;
	static _Tt_prop props;
	int isfd = argp->isfd;

	// a reply that fails must not carry the record of an earlier request
	res.props.props_len = 0;
	res.props.props_val = 0;
	if (!_tt_check_stale_isfd(isfd)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: _tt_check_stale_isfd() == 0",
			   here );
		return (&res);
	}
	// the type of the object is the value of the property the request names
	if (argp->key == NULL || argp->key_len <= 0 ||
	    argp->key_len > OID_KEY_LENGTH + MAX_PROP_LENGTH ||
	    argp->props.props_len < 1 || argp->props.props_val == NULL ||
	    argp->props.props_val[0].propname.propname_val == NULL ||
	    argp->oidkey.oidkey_val == NULL ||
	    !_tt_set_record_key(argp->oidkey.oidkey_val, argp->oidkey.oidkey_len,
				argp->props.props_val[0].propname.propname_val,
				argp->props.props_val[0].propname.propname_len)) {
		res.result = -1;
		res.iserrno = ERPC;
		_tt_syslog(errstr, LOG_ERR, "%s: bad key", here);
		return (&res);
	}
	// The object's property is read: needs read access to it
	char oid[OID_KEY_LENGTH];
	memcpy(oid, _tt_record, sizeof(oid));
	if (!_tt_oid_accessp(isfd, oid, sizeof(oid), 'r')) {
		res.result = -1;
		res.iserrno = DM_ACCESS_DENIED;
		return (&res);
	}
	if (!_tt_set_record_key(argp->oidkey.oidkey_val, argp->oidkey.oidkey_len,
				argp->props.props_val[0].propname.propname_val,
				argp->props.props_val[0].propname.propname_len)) {
		res.result = -1;
		res.iserrno = ERPC;
		return (&res);
	}
	res.result = isstart(isfd, argp->key, argp->key_len,
			     _tt_record, ISEQUAL);

	res.iserrno = iserrno;
	if (res.result == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isstart(): %d", here, iserrno);
		return (&res);
	}
	res.result = isread(isfd, _tt_record, ISNEXT);
	if (res.result == -1) {
		_tt_syslog(errstr, LOG_ERR, "%s: isread(): %d", here, iserrno);
		return (&res);
	}
	res.props.props_len = 1;
	res.props.props_val = &props;
	props.value.value_val = &_tt_record[OID_KEY_LENGTH+MAX_PROP_LENGTH];
	props.value.value_len = isreclen-MAX_PROP_LENGTH-OID_KEY_LENGTH;
	props.recnum = 0;
	props.propname.propname_val = 0;
	props.propname.propname_len = 0;
	res.iserrno = iserrno;
	return (&res);
}

//
// Go through the _tt_db_table and check on each file.
// Then compresse the files (empty records deleted).
//
void
isgarbage_collect()
{
	int	offset;
	_Tt_string	dbName;

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

		  //
		  // Save name as cached_isclose() removes the
		  // name from the table.
		  //
		  dbName = _tt_db_table[offset].db_path;
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

int find_endstring(const char *string, const char *end_string)
{
        int sl = strlen(string);
        int esl = strlen(end_string);

	if (sl >= esl) {
		if (!strncmp(string+sl-esl, end_string, esl)) {
			return sl-esl;
		}
	}

	return -1;
}

// See if the message queue is locked or should be locked...
//
// Args: file descriptor, record buffer, record length, RPC client info
//
bool_t msg_q_lock(int isfd, const char *record, int length, SVCXPRT *transp)
{
  _Tt_string db_file = _tt_db_table [isfd].db_path;

  // If this is not the property table then we don't care about it
  if (!db_file.sh_match("*property_table")) {
    return TRUE;
  }

  // See if this is a _TT_QUEUED_MSGS property access...
  // (with a 16 byte key, the rec length must be at least 31)
  if ((length > 30) && !memcmp(record+16, "_TT_QUEUED_MSGS", 15)) {
    // Get the address of the client
#if defined(OPT_TLI) || defined(OPT_TIRPC)
    netbuf *client_address = svc_getrpccaller(transp);
    _Tt_string client_id(client_address->len);
    memcpy((char *)client_id, client_address->buf, client_address->len);
#else
    struct sockaddr_in *client_address = svc_getcaller(transp);

    _Tt_string client_id(sizeof(struct in_addr));
    memcpy( (char *)client_id, &client_address->sin_addr,
	    sizeof(struct in_addr) );
#endif

    // Get the file key
    _Tt_string file_key(16);
    (void)memcpy((char *)file_key, record, 16);

    // Set a lock for this client and file key and lock the message queue
    // for new DB clients
    _Tt_db_msg_q_lock lock;
    return lock.testAndSetLock(client_id, file_key);
  }
  // Else, see if this is a _TT_MSG_<ID#>_<PART#> property access
  // (with a 16 byte key, the rec length must be at least 27)
  else if ((length > 26) && !memcmp(record+16, "_TT_MSG_", 8)) {
    // Get the address of the client
#if defined(OPT_TLI) || defined(OPT_TIRPC)
    netbuf *client_address = svc_getrpccaller(transp);
    _Tt_string client_id(client_address->len);
    (void)memcpy((char *)client_id, client_address->buf, client_address->len);
#else
    struct sockaddr_in *client_address = svc_getcaller(transp);
    _Tt_string client_id(sizeof(struct in_addr));
    memcpy( (char *)client_id, &client_address->sin_addr,
	    sizeof(struct in_addr) );
#endif

    // Get the file key
    _Tt_string file_key(16);
    (void)memcpy((char *)file_key, record, 16);

    // Unlock the message queue for new DB clients
    _Tt_db_msg_q_lock lock;
    lock.unsetLock(client_id, file_key);
  }

  return TRUE;
}

_Tt_string map_old_db_to_new_db(const char *old_db)
{
	_Tt_string new_db(old_db);
	int	   index;

	if ((index = find_endstring(old_db, _TT_DOCOID_PATH)) > 0) {
		new_db = get_new_db(new_db, index, TT_DB_FILE_TABLE_FILE);
	}
	else if ((index = find_endstring(old_db, _TT_OID_CONTAINER)) > 0) {
		new_db = get_new_db(new_db, index, TT_DB_FILE_OBJECT_MAP_FILE);
	}
	else if ((index = find_endstring(old_db, _TT_OID_PROP)) > 0) {
		new_db = get_new_db(new_db, index, TT_DB_PROPERTY_TABLE_FILE);
	}
	else if ((index = find_endstring(old_db, _TT_OID_ACCESS)) > 0) {
		new_db = get_new_db(new_db, index, TT_DB_ACCESS_TABLE_FILE);
	}
	else {
		new_db = (char *)NULL;
	}

	return new_db;
}

// Takes the old DB path and table name, the index of where the table name
// starts in the path and the new table name to replace the old name
// with.  It returns the path with the new table name.  This function also
// handles the renaming of the old DB table to the new name and the change
// of format in the access table.
_Tt_string get_new_db(_Tt_string old_db, int index, _Tt_string new_file)
{
  static bool_t old_db_diagnostic = FALSE;

  _Tt_string new_db(old_db);
  new_db = new_db.left(index);
  if (new_db [new_db.len()-1] != '/') {
    new_db = new_db.cat("/");
  }
  new_db = new_db.cat(new_file);

  _Tt_string new_db_index(new_db);
  new_db_index = new_db_index.cat(".ind");
  
  _Tt_string old_db_index(old_db);
  old_db_index = old_db_index.cat(".ind");
  
  struct stat stat_buf;
  int         temp_errno = 0;

  // If there is no new DB and there is an old DB...
  if (stat((char *)new_db_index, &stat_buf) &&
      ((temp_errno = errno) == ENOENT) &&
      !stat((char *)old_db_index, &stat_buf)) {
    // If the file being accessed is the access_table...
    if (new_file == TT_DB_ACCESS_TABLE_FILE) {
      _Tt_isam_file_ptr old_db_table =
        new _Tt_isam_file(old_db, ISFIXLEN+ISINOUT+ISEXCLLOCK);
      _Tt_isam_file_ptr access_table;
      
      int results = old_db_table->getErrorStatus();
      
      if (!results) {
	_Tt_isam_key_descriptor_ptr access_key = new _Tt_isam_key_descriptor;
	access_key->addKeyPart(0, TT_DB_KEY_LENGTH, BINTYPE);
	
	access_table =
	  new _Tt_isam_file(new_db,
			    TT_DB_KEY_LENGTH+3*TT_DB_LONG_SIZE, 
			    TT_DB_KEY_LENGTH+3*TT_DB_LONG_SIZE,   
			    access_key,
			    ISFIXLEN+ISINOUT+ISEXCLLOCK);
	(void)access_table->writeMagicString(_Tt_string(TT_DB_VERSION));
	
	results = access_table->getErrorStatus();
      }
      
      if (!results) {
	_Tt_isam_record_ptr new_record = access_table->getEmptyRecord();
	_Tt_isam_record_ptr record;
	
	while (!results) {
	  record = old_db_table->readRecord(ISNEXT);
	  results = old_db_table->getErrorStatus();
	  
	  if (!results) {
	    memset((char *)new_record->getRecord(),
		   '\0',
		   new_record->getLength());

	    memcpy((char *)new_record->getRecord(),
		   (char *)record->getRecord(),
		   TT_DB_KEY_LENGTH);

            short n_user = *(short *)
                            ((char *)record->getRecord()+TT_DB_KEY_LENGTH);
            long user = (long)ntohs(n_user);
            u_long nl_user = htonl(user);
            memcpy((char *)new_record->getRecord()+TT_DB_ACCESS_USER_OFFSET,
                   (char *)&nl_user,
                   TT_DB_LONG_SIZE);

            short n_group = *(short *)
                             ((char *)record->getRecord()+
                                      TT_DB_KEY_LENGTH+TT_DB_SHORT_SIZE);
            long group = (long)ntohs(n_group);
            u_long nl_group = htonl(group);
            memcpy((char *)new_record->getRecord()+TT_DB_ACCESS_GROUP_OFFSET,
                   (char *)&nl_group,
                   TT_DB_LONG_SIZE);
         
            short n_mode = *(short *)
                            ((char *)record->getRecord()+
                                     TT_DB_KEY_LENGTH+2*TT_DB_SHORT_SIZE);
            long mode = (long)ntohs(n_mode);
            u_long nl_mode = htonl(mode);
            memcpy((char *)new_record->getRecord()+TT_DB_ACCESS_MODE_OFFSET,
                   (char *)&nl_mode,
                   TT_DB_LONG_SIZE);
	    
	    results = access_table->writeRecord(new_record);
	  }
	}
	
	if (results == EENDFILE) {
	  results = 0;
	}
      }
      
      if (!results) {
	(void)iserase((char *)old_db);
      }
      
      if (results) {
	new_db = (char *)NULL;
      }
    }
    // Else, everything other than the access table is just renamed...
    else {
      if (isrename((char *)old_db, (char *)new_db)) {
	new_db = (char *)NULL;
      }

      if (!new_db.is_null()) {
        _Tt_isam_file_ptr new_db_table =
	  new _Tt_isam_file(old_db, ISFIXLEN+ISINOUT+ISEXCLLOCK);
        int results = new_db_table->getErrorStatus();

	if (results) {
          new_db_table = new _Tt_isam_file(old_db, ISVARLEN+ISINOUT+ISEXCLLOCK);
          results = new_db_table->getErrorStatus();
	}

	if (!results) {
	  (void)new_db_table->writeMagicString(TT_DB_VERSION);
        }
      }
    }
  }
  // The stat on the new DB generated a nasty error...
  else if (temp_errno && (temp_errno != ENOENT)) {
    new_db = (char *)NULL;
  }
  // Old and new DB files exist...
  else if (!stat((char *)old_db_index, &stat_buf)) {
    if (old_db_diagnostic == FALSE) {
	    _tt_syslog(errstr, LOG_ERR,
		       catgets(_ttcatd, 5, 4,
			       "Any data written using an old (<= 1.0.2) "
			       "rpc.ttdbserverd after using a new (>= 1.1) "
			       "rpc.ttdbserverd will be ignored"));
	    old_db_diagnostic = TRUE;
    }
  }
  
  return new_db;
}
