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
/*%%  $XConsortium: db_server.h /main/3 1995/10/23 09:58:23 rswiston $ 			 				 */
/*-*-C++-*-
 * Tool Talk Database Server - db_server.h
 *
 * Copyright (c) 1992 by Sun Microsystems, Inc.
 *
 * This file contains the declaration of structures for rpc calls to the
 * database server.
 */

#ifndef  _DB_SERVER_H
#define  _DB_SERVER_H

#include <rpc/rpc.h>
#include <sys/param.h>

#include "util/tt_string.h"
#include "db/tt_db_results.h"
#include "tt_const.h"

/*
 * The limits of the protocol.
 *
 * The sizes in a request are the sender's.  The XDR routines allocate what a
 * length tells them to before they read what it is the length of, so a few
 * bytes of request can demand the whole of a 32-bit length.  Every string,
 * byte array and list of the protocol is therefore bounded, and the XDR
 * routines refuse a longer one, in a request and in a reply.
 *
 * Names are bounded by what the database stores of them: a file is named
 * "host:path", a path is at most MAXPATHLEN, the name of a property is the
 * MAX_PROP_LENGTH bytes of its field in a record.  The object id of an object
 * is its key (below 100 characters in base 64), the file system type, the host
 * and the partition.  Type names are those of ptypes and otypes, which are
 * identifiers.  A netfile holds the host and two paths.
 *
 * A value is bounded by the record that holds it: a record of the property
 * table is at most TT_DB_LIMIT_RECORD bytes (ISMAXRECLEN of mini_isam/isam.h,
 * which db_server_xdr.C checks) and starts with the key of the file or object
 * and the name of the property, and what is left is the most that a property
 * value, a session id or a part of a queued message can be.  A message is kept
 * in as many such parts as it takes, so it is bounded by TT_DB_LIMIT_MESSAGE
 * only (16 MiB is 515 records), which leaves room for the arguments of a
 * message to add up to 16 MiB.
 *
 * Lists are bounded by what a reply could carry of what the tables hold: one
 * value per queued message and one per session in a property, one record per
 * object.  The server refuses to queue the message that would make the list
 * of the queued messages of a file longer than a reply takes.  The sizes of the
 * lists in a request are in the same proportion as those of the lists in a
 * reply, as one routine decodes both.
 */
const u_int TT_DB_LIMIT_RECORD			= 32767;
const u_int TT_DB_LIMIT_PATH			= MAXPATHLEN;
const u_int TT_DB_LIMIT_FILE			= HOST_LENGTH + 1 + MAXPATHLEN;
/* a netfile is "TTN0HOST=a-b,RPATH=c-d,LPATH=e-f:" and the host, the path on
   the host and the path the client knows the file by (api_filemap.C) */
const u_int TT_DB_LIMIT_NETFILE			= 128 + HOST_LENGTH + 2 * MAXPATHLEN;
const u_int TT_DB_LIMIT_OBJID			= 128 + HOST_LENGTH + MAXPATHLEN;
const u_int TT_DB_LIMIT_TYPE			= 1024;
const u_int TT_DB_LIMIT_PROPERTY_NAME		= MAX_PROP_LENGTH;
/* what a record of the property table holds of a value: the rest of the
   record after the key and the name.  A property value, a session id and a
   part of a queued message each are one record. */
const u_int TT_DB_LIMIT_PROPERTY_VALUE		= TT_DB_LIMIT_RECORD -
						  OID_KEY_LENGTH - MAX_PROP_LENGTH;
const u_int TT_DB_LIMIT_SESSION			= TT_DB_LIMIT_PROPERTY_VALUE;
/* the marshalled message: the arguments of up to 16 MiB, and the rest of it */
const u_int TT_DB_LIMIT_MESSAGE			= 16 * 1024 * 1024 + 64 * 1024;

const u_int TT_DB_LIMIT_PROPERTIES		= 1 << 16;
const u_int TT_DB_LIMIT_PROPERTY_VALUES		= 1 << 16;
const u_int TT_DB_LIMIT_PTYPES			= 1 << 10;
const u_int TT_DB_LIMIT_OBJECTS			= 1 << 16;
const u_int TT_DB_LIMIT_MESSAGES		= 1 << 16;

typedef enum _Tt_db_results _tt_db_results;
bool_t xdr_tt_db_results(XDR *, _tt_db_results *);

/* strings of a request and of a reply, each bounded as its kind is above; a
   string that is not there (a null pointer) is sent as the empty string */
bool_t xdr_tt_file_name(XDR *, char **);
bool_t xdr_tt_objid_name(XDR *, char **);

struct _tt_string {
	char *value;
};
typedef struct _tt_string _tt_string;
bool_t xdr_tt_ptype(XDR *, _tt_string *);
bool_t xdr_tt_objid(XDR *, _tt_string *);
bool_t xdr_tt_session(XDR *, _tt_string *);

struct _tt_string_list {
       u_int values_len;
      _tt_string *values_val;
};
typedef _tt_string_list _tt_string_list;

struct _tt_property_value {
	struct {
		u_int value_len;
		char *value_val;
	} value;
};
typedef struct _tt_property_value _tt_property_value;
bool_t xdr_tt_property_value(XDR *, _tt_property_value *);

struct _tt_property {
	char *name;
	struct {
		u_int values_len;
		_tt_property_value *values_val;
	} values;
};
typedef struct _tt_property _tt_property;
bool_t xdr_tt_property(XDR *, _tt_property *);

struct _tt_property_list {
	u_int properties_len;
	_tt_property *properties_val;
};
typedef struct _tt_property_list _tt_property_list;

struct _tt_access {
	uid_t user;
	gid_t group;
	mode_t mode;
};
typedef struct _tt_access _tt_access;
bool_t xdr_tt_access(XDR *, _tt_access *);

struct _tt_message {
	struct {
		u_int body_len;
		char *body_val;
	} body;
};
typedef struct _tt_message _tt_message;
bool_t xdr_tt_message(XDR *, _tt_message *);

struct _tt_message_list {
       u_int messages_len;
       _tt_message *messages_val;
};
typedef _tt_message_list _tt_message_list;

struct _tt_create_file_args {
	char *file;
	_tt_property_list properties;
	_tt_access access;
};
typedef struct _tt_create_file_args _tt_create_file_args;
bool_t xdr_tt_create_file_args(XDR *, _tt_create_file_args *);

struct _tt_create_obj_args {
	char *file;
	char *objid;
	char *otype;
	_tt_property_list properties;
	_tt_access access;
};
typedef struct _tt_create_obj_args _tt_create_obj_args;
bool_t xdr_tt_create_obj_args(XDR *, _tt_create_obj_args *);

struct _tt_remove_file_args {
        char *file;
        _tt_access access;
};
typedef struct _tt_remove_file_args _tt_remove_file_args;
bool_t xdr_tt_remove_file_args(XDR *, _tt_remove_file_args *);

struct _tt_remove_obj_args {
        char *objid;
	char *forward_pointer;
        _tt_access access;
};
typedef struct _tt_remove_obj_args _tt_remove_obj_args;
bool_t xdr_tt_remove_obj_args(XDR *, _tt_remove_obj_args *);
 
struct _tt_move_file_args {
        char *file;
	char *new_file;
        _tt_access access;
};
typedef struct _tt_move_file_args _tt_move_file_args;
bool_t xdr_tt_move_file_args(XDR *, _tt_move_file_args *);

struct _tt_set_file_prop_args {
	char *file;
	_tt_property property;
	_tt_access access;
};
typedef struct _tt_set_file_prop_args _tt_set_file_prop_args;
bool_t xdr_tt_set_file_prop_args(XDR *, _tt_set_file_prop_args *);

struct _tt_set_file_props_args {
	char *file;
	_tt_property_list properties;
	_tt_access access;
};
typedef struct _tt_set_file_props_args _tt_set_file_props_args;
bool_t xdr_tt_set_file_props_args(XDR *, _tt_set_file_props_args *);

struct _tt_add_file_prop_args {
	char *file;
	_tt_property property;
	int unique;
	_tt_access access;
};
typedef struct _tt_add_file_prop_args _tt_add_file_prop_args;
bool_t xdr_tt_add_file_prop_args(XDR *, _tt_add_file_prop_args *);

struct _tt_del_file_prop_args {
	char *file;
	_tt_property property;
	_tt_access access;
};
typedef struct _tt_del_file_prop_args _tt_del_file_prop_args;
bool_t xdr_tt_del_file_prop_args(XDR *, _tt_del_file_prop_args *);

struct _tt_get_file_prop_args {
	char *file;
	char *name;
	_tt_access access;
	int cache_level;
};
typedef struct _tt_get_file_prop_args _tt_get_file_prop_args;
bool_t xdr_tt_get_file_prop_args(XDR *, _tt_get_file_prop_args *);

struct _tt_get_file_props_args {
	char *file;
	_tt_access access;
	int cache_level;
};
typedef struct _tt_get_file_props_args _tt_get_file_props_args;
bool_t xdr_tt_get_file_props_args(XDR *, _tt_get_file_props_args *);

struct _tt_get_file_objs_args {
        char *file;
	_tt_access access;
        int cache_level;
};
typedef struct _tt_get_file_objs_args _tt_get_file_objs_args;
bool_t xdr_tt_get_file_objs_args(XDR *, _tt_get_file_objs_args *);
 
struct _tt_set_file_access_args {
	char *file;
	_tt_access new_access;
	_tt_access access;
};
typedef struct _tt_set_file_access_args _tt_set_file_access_args;
bool_t xdr_tt_set_file_access_args(XDR *, _tt_set_file_access_args *);

struct _tt_get_file_access_args {
        char *file;
	_tt_access access;
};
typedef struct _tt_get_file_access_args _tt_get_file_access_args;
bool_t xdr_tt_get_file_access_args(XDR *, _tt_get_file_access_args *);
 
struct _tt_set_obj_prop_args {
	char *objid;
	_tt_property property;
	_tt_access access;
	int cache_level;
};
typedef struct _tt_set_obj_prop_args _tt_set_obj_prop_args;
bool_t xdr_tt_set_obj_prop_args(XDR *, _tt_set_obj_prop_args *);

struct _tt_set_obj_props_args {
	char *objid;
	_tt_property_list properties;
	_tt_access access;
	int cache_level;
};
typedef struct _tt_set_obj_props_args _tt_set_obj_props_args;
bool_t xdr_tt_set_obj_props_args(XDR *, _tt_set_obj_props_args *);

struct _tt_add_obj_prop_args {
	char *objid;
	_tt_property property;
	int unique;
	_tt_access access;
	int cache_level;
};
typedef struct _tt_add_obj_prop_args _tt_add_obj_prop_args;
bool_t xdr_tt_add_obj_prop_args(XDR *, _tt_add_obj_prop_args *);

struct _tt_del_obj_prop_args {
	char *objid;
	_tt_property property;
	_tt_access access;
	int cache_level;
};
typedef struct _tt_del_obj_prop_args _tt_del_obj_prop_args;
bool_t xdr_tt_del_obj_prop_args(XDR *, _tt_del_obj_prop_args *);

struct _tt_get_obj_prop_args {
	char *objid;
	char *name;
	_tt_access access;
	int cache_level;
};
typedef struct _tt_get_obj_prop_args _tt_get_obj_prop_args;
bool_t xdr_tt_get_obj_prop_args(XDR *, _tt_get_obj_prop_args *);

struct _tt_get_obj_props_args {
	char *objid;
	_tt_access access;
	int cache_level;
};
typedef struct _tt_get_obj_props_args _tt_get_obj_props_args;
bool_t xdr_tt_get_obj_props_args(XDR *, _tt_get_obj_props_args *);

struct _tt_set_obj_type_args {
	char *objid;
	char *otype;
	_tt_access access;
};
typedef struct _tt_set_obj_type_args _tt_set_obj_type_args;
bool_t xdr_tt_set_obj_type_args(XDR *, _tt_set_obj_type_args *);

struct _tt_get_obj_type_args {
        char *objid;
	_tt_access access;
};
typedef struct _tt_get_obj_type_args _tt_get_obj_type_args;
bool_t xdr_tt_get_obj_type_args(XDR *, _tt_get_obj_type_args *);
 
struct _tt_set_obj_file_args {
        char *objid;
        char *file;
        _tt_access access;
};
typedef struct _tt_set_obj_file_args _tt_set_obj_file_args;
bool_t xdr_tt_set_obj_file_args(XDR *, _tt_set_obj_file_args *);

struct _tt_get_obj_file_args {
        char *objid;
	_tt_access access;
};
typedef struct _tt_get_obj_file_args _tt_get_obj_file_args;
bool_t xdr_tt_get_obj_file_args(XDR *, _tt_get_obj_file_args *);
 
struct _tt_set_obj_access_args {
	char *objid;
	_tt_access new_access;
	_tt_access access;
};
typedef struct _tt_set_obj_access_args _tt_set_obj_access_args;
bool_t xdr_tt_set_obj_access_args(XDR *, _tt_set_obj_access_args *);

struct _tt_get_obj_access_args {
        char *objid;
	_tt_access access;
};
typedef struct _tt_get_obj_access_args _tt_get_obj_access_args;
bool_t xdr_tt_get_obj_access_args(XDR *, _tt_get_obj_access_args *);
 
struct _tt_is_file_in_db_args {
        char *file;
	_tt_access access;
};
typedef struct _tt_is_file_in_db_args _tt_is_file_in_db_args;
bool_t xdr_tt_is_file_in_db_args(XDR *, _tt_is_file_in_db_args *);
 
struct _tt_is_obj_in_db_args {
        char *objid;
	_tt_access access;
};
typedef struct _tt_is_obj_in_db_args _tt_is_obj_in_db_args;
bool_t xdr_tt_is_obj_in_db_args(XDR *, _tt_is_obj_in_db_args *);
 
struct _tt_queue_msg_args {
	char *file;
	_tt_string_list ptypes;
	_tt_message message;
};
typedef struct _tt_queue_msg_args _tt_queue_msg_args;
bool_t xdr_tt_queue_msg_args(XDR *, _tt_queue_msg_args *);

struct _tt_dequeue_msgs_args {
	char *file;
	_tt_string_list ptypes;
};
typedef struct _tt_dequeue_msgs_args _tt_dequeue_msgs_args;
bool_t xdr_tt_dequeue_msgs_args(XDR *, _tt_dequeue_msgs_args *);

struct _tt_file_netfile_args {
	char *file_or_netfile;
};
typedef struct _tt_file_netfile_args _tt_file_netfile_args;
bool_t xdr_tt_file_netfile_args(XDR *, _tt_file_netfile_args *);

struct _tt_get_all_sessions_args {
	struct {
		u_int oidkey_len;
		const char *oidkey_val;
	} oidkey;
};
typedef struct _tt_get_all_sessions_args _tt_get_all_sessions_args;
bool_t	xdr_tt_get_all_sessions_args(XDR *, _tt_get_all_sessions_args *);

struct _tt_delete_session_args {
    _tt_string		session;
};
typedef struct _tt_delete_session_args _tt_delete_session_args;
bool_t	xdr_tt_delete_session_args(XDR *, _tt_delete_session_args *);

struct _tt_db_cache_results {
	int cache_level;
	_tt_db_results results;
};
typedef struct _tt_db_cache_results _tt_db_cache_results;
bool_t xdr_tt_db_cache_results(XDR *, _tt_db_cache_results *);

struct _tt_auth_level_results {
	int auth_level;
	_tt_db_results results;
};
typedef struct _tt_auth_level_results _tt_auth_level_results;
bool_t xdr_tt_auth_level_results(XDR *, _tt_auth_level_results *);

struct _tt_file_partition_results {
	char *partition;
	char *network_path;
	_tt_db_results results;
};
typedef struct _tt_file_partition_results _tt_file_partition_results;
bool_t xdr_tt_file_partition_results(XDR *, _tt_file_partition_results *);

struct _tt_file_prop_results {
	_tt_property property;
	int cache_level;
	_tt_db_results results;
};
typedef struct _tt_file_prop_results _tt_file_prop_results;
bool_t xdr_tt_file_prop_results(XDR *, _tt_file_prop_results *);

struct _tt_file_props_results {
	_tt_property_list properties;
	int cache_level;
	_tt_db_results results;
};
typedef struct _tt_file_props_results _tt_file_props_results;
bool_t xdr_tt_file_props_results(XDR *, _tt_file_props_results *);

struct _tt_file_objs_results {
	_tt_string_list objids;
	int cache_level;
	_tt_db_results results;
};
typedef struct _tt_file_objs_results _tt_file_objs_results;
bool_t xdr_tt_file_objs_results(XDR *, _tt_file_objs_results *);

struct _tt_file_access_results {
	_tt_access access;
	_tt_db_results results;
};
typedef struct _tt_file_access_results _tt_file_access_results;
bool_t xdr_tt_file_access_results(XDR *, _tt_file_access_results *);

struct _tt_obj_prop_results {
	_tt_property property;
	char *file;
	int cache_level;
	_tt_db_results results;
};
typedef struct _tt_obj_prop_results _tt_obj_prop_results;
bool_t xdr_tt_obj_prop_results(XDR *, _tt_obj_prop_results *);

struct _tt_obj_props_results {
	_tt_property_list properties;
	char *file;
	int cache_level;
	_tt_db_results results;
};
typedef struct _tt_obj_props_results _tt_obj_props_results;
bool_t xdr_tt_obj_props_results(XDR *, _tt_obj_props_results *);

struct _tt_obj_type_results {
	char *otype;
	_tt_db_results results;
};
typedef struct _tt_obj_type_results _tt_obj_type_results;
bool_t xdr_tt_obj_type_results(XDR *, _tt_obj_type_results *);

struct _tt_obj_file_results {
	char *file;
	_tt_db_results results;
};
typedef struct _tt_obj_file_results _tt_obj_file_results;
bool_t xdr_tt_obj_file_results(XDR *, _tt_obj_file_results *);

struct _tt_obj_access_results {
	_tt_access access;
	_tt_db_results results;
};
typedef struct _tt_obj_access_results _tt_obj_access_results;
bool_t xdr_tt_obj_access_results(XDR *, _tt_obj_access_results *);

struct _tt_is_file_in_db_results {
	bool_t directory_flag;
	_tt_db_results results;
};
typedef struct _tt_is_file_in_db_results _tt_is_file_in_db_results;
bool_t xdr_tt_is_file_in_db_results(XDR *, _tt_is_file_in_db_results *);

struct _tt_is_obj_in_db_results {
	char *forward_pointer;
	_tt_db_results results;
};
typedef struct _tt_is_obj_in_db_results _tt_is_obj_in_db_results;
bool_t xdr_tt_is_obj_in_db_results(XDR *, _tt_is_obj_in_db_results *);

struct _tt_dequeue_msgs_results {
        _tt_message_list messages;
	_tt_db_results results;
};
typedef struct _tt_dequeue_msgs_results _tt_dequeue_msgs_results;
bool_t xdr_tt_dequeue_msgs_results(XDR *, _tt_dequeue_msgs_results *);

struct _tt_file_netfile_results {
	char *result_string;
	_tt_db_results results;
	int tt_status;
};
typedef struct _tt_file_netfile_results _tt_file_netfile_results;
bool_t xdr_tt_file_netfile_results(XDR *, _tt_file_netfile_results *);

struct _tt_get_all_sessions_results {
	struct {
		u_int oidkey_len;
		const char *oidkey_val;
	} oidkey;
	_tt_string_list		session_list;
};
typedef struct _tt_get_all_sessions_results _tt_get_all_sessions_results;
bool_t	xdr_tt_get_all_sessions_results(XDR *,_tt_get_all_sessions_results *);

struct _tt_delete_session_results {
	int	tt_status;
};
typedef struct _tt_delete_session_results _tt_delete_session_results;
bool_t	xdr_tt_delete_session_results(XDR *,_tt_delete_session_results *);

struct _tt_garbage_collect_results {
	int	tt_status;
};
bool_t	xdr_tt_garbage_collect_results(XDR *, _tt_garbage_collect_results *);

#define TT_DBSERVER_PROG 	((u_long)100083)
#define TT_DBSERVER_VERS 	((u_long)1)

/*
 * The procedures 1 to 24 were those of the servers that preceded this one: the
 * NetISAM procedures (open, read, write, ... a table by its path), which let a
 * client work on the tables of the server directly, and a few for the access
 * and the sessions of an oid.  They are not served any more: a request for one
 * of them is answered as a procedure that does not exist.
 */

#define TT_GET_MIN_AUTH_LEVEL   ((u_long)101)
#define TT_GET_FILE_PARTITION   ((u_long)102)
#define TT_CREATE_FILE          ((u_long)103)
#define TT_CREATE_OBJ           ((u_long)104)
#define TT_REMOVE_FILE          ((u_long)105)
#define TT_REMOVE_OBJ           ((u_long)106)
#define TT_MOVE_FILE            ((u_long)107)
#define TT_SET_FILE_PROPS       ((u_long)108)
#define TT_SET_FILE_PROP        ((u_long)109)
#define TT_ADD_FILE_PROP        ((u_long)110)
#define TT_DELETE_FILE_PROP     ((u_long)111)
#define TT_GET_FILE_PROP        ((u_long)112)
#define TT_GET_FILE_PROPS       ((u_long)113)
#define TT_GET_FILE_OBJS        ((u_long)114)
#define TT_SET_FILE_ACCESS      ((u_long)115)
#define TT_GET_FILE_ACCESS      ((u_long)116)
#define TT_SET_OBJ_PROPS        ((u_long)117)
#define TT_SET_OBJ_PROP         ((u_long)118)
#define TT_ADD_OBJ_PROP         ((u_long)119)
#define TT_DELETE_OBJ_PROP      ((u_long)120)
#define TT_GET_OBJ_PROP         ((u_long)121)
#define TT_GET_OBJ_PROPS        ((u_long)122)
#define TT_SET_OBJ_TYPE         ((u_long)123)
#define TT_GET_OBJ_TYPE         ((u_long)124)
#define TT_SET_OBJ_FILE         ((u_long)125)
#define TT_GET_OBJ_FILE         ((u_long)126)
#define TT_SET_OBJ_ACCESS       ((u_long)127)
#define TT_GET_OBJ_ACCESS       ((u_long)128)
#define TT_IS_FILE_IN_DB        ((u_long)129)
#define TT_IS_OBJ_IN_DB         ((u_long)130)
#define TT_QUEUE_MESSAGE        ((u_long)131)
#define TT_DEQUEUE_MESSAGES     ((u_long)132)
#define TTDB_FILE_NETFILE	((u_long)133)
#define TTDB_NETFILE_FILE	((u_long)134)

#define TT_GET_ALL_SESSIONS	((u_long)135)
#define TT_GARBAGE_COLLECT	((u_long)136)	/* Perform garbage cleanup */
#define TT_DELETE_SESSION	((u_long)137)	/* Delete named session */

#ifdef _TT_DBCLIENT_SIDE

#include "db/tt_db_client_utils.h"

extern clnt_stat _tt_get_rpc_result(void);
extern _tt_auth_level_results *_tt_get_min_auth_level_1(void *, CLIENT *);
extern _tt_file_partition_results *_tt_get_file_partition_1(char **, CLIENT *);
extern _tt_db_cache_results *_tt_create_file_1(_tt_create_file_args *, CLIENT *);
extern _tt_db_cache_results *_tt_create_obj_1(_tt_create_obj_args *, CLIENT *);
extern _tt_db_results *_tt_remove_file_1(_tt_remove_file_args *, CLIENT *);
extern _tt_db_results *_tt_remove_obj_1(_tt_remove_obj_args *, CLIENT *);
extern _tt_db_results *_tt_move_file_1(_tt_move_file_args *, CLIENT *);
extern _tt_db_cache_results *_tt_set_file_props_1(_tt_set_file_props_args *,
						  CLIENT *);
extern _tt_db_cache_results *_tt_set_file_prop_1(_tt_set_file_prop_args *,
						 CLIENT *);
extern _tt_db_cache_results *_tt_add_file_prop_1(_tt_add_file_prop_args *,
						 CLIENT *);
extern _tt_db_cache_results *_tt_delete_file_prop_1(_tt_del_file_prop_args *,
						    CLIENT *);
extern _tt_file_prop_results *_tt_get_file_prop_1(_tt_get_file_prop_args *,
						  CLIENT *);
extern _tt_file_props_results *_tt_get_file_props_1(_tt_get_file_props_args *,
						    CLIENT *);
extern _tt_file_objs_results *_tt_get_file_objs_1(_tt_get_file_objs_args *,
						  CLIENT *);
extern _tt_db_results *_tt_set_file_access_1(_tt_set_file_access_args *,
					     CLIENT *);
extern _tt_file_access_results *_tt_get_file_access_1(_tt_get_file_access_args *,
						      CLIENT *);
extern _tt_obj_props_results *_tt_set_obj_props_1(_tt_set_obj_props_args *,
						  CLIENT *);
extern _tt_obj_props_results *_tt_set_obj_prop_1(_tt_set_obj_prop_args *,
						 CLIENT *);
extern _tt_obj_props_results *_tt_add_obj_prop_1(_tt_add_obj_prop_args *,
						 CLIENT *);
extern _tt_obj_props_results *_tt_delete_obj_prop_1(_tt_del_obj_prop_args *,
						    CLIENT *);
extern _tt_obj_prop_results *_tt_get_obj_prop_1(_tt_get_obj_prop_args *,
						CLIENT *);
extern _tt_obj_props_results *_tt_get_obj_props_1(_tt_get_obj_props_args *,
						  CLIENT *);
extern _tt_db_results *_tt_set_obj_type_1(_tt_set_obj_type_args *,
					  CLIENT *);
extern _tt_obj_type_results *_tt_get_obj_type_1(_tt_get_obj_type_args *,
						CLIENT *);
extern _tt_db_results *_tt_set_obj_file_1(_tt_set_obj_file_args *,
					  CLIENT *);
extern _tt_obj_file_results *_tt_get_obj_file_1(_tt_get_obj_file_args *,
						CLIENT *);
extern _tt_db_results *_tt_set_obj_access_1(_tt_set_obj_access_args *,
					    CLIENT *);
extern _tt_obj_access_results *_tt_get_obj_access_1(_tt_get_obj_access_args *,
						    CLIENT *);
extern _tt_is_file_in_db_results *_tt_is_file_in_db_1(_tt_is_file_in_db_args *,
						      CLIENT *);
extern _tt_is_obj_in_db_results *_tt_is_obj_in_db_1(_tt_is_obj_in_db_args *,
						    CLIENT *);
extern _tt_db_results *_tt_queue_message_1(_tt_queue_msg_args *, CLIENT *);
extern _tt_dequeue_msgs_results *_tt_dequeue_messages_1(_tt_dequeue_msgs_args *,
							CLIENT *);
extern _tt_file_netfile_results * _tt_file_netfile_1(_tt_file_netfile_args *, CLIENT *);
extern _tt_file_netfile_results * _tt_netfile_file_1(_tt_file_netfile_args *, CLIENT *);

extern _tt_get_all_sessions_results *_tt_get_all_sessions_1(_tt_get_all_sessions_args *, CLIENT *);
extern _tt_garbage_collect_results *_tt_garbage_collect_1(void *, CLIENT *);
extern _tt_delete_session_results * _tt_delete_session_1(_tt_delete_session_args *, CLIENT *);
#else

extern _tt_auth_level_results *_tt_get_min_auth_level_1(void *,
							SVCXPRT*);
extern _tt_file_partition_results *_tt_get_file_partition_1(char **,
							    SVCXPRT*);
extern _tt_db_cache_results *_tt_create_file_1(_tt_create_file_args *,
					       SVCXPRT*);
extern _tt_db_cache_results *_tt_create_obj_1(_tt_create_obj_args *,
					      SVCXPRT*);
extern _tt_db_results *_tt_remove_file_1(_tt_remove_file_args *,
					 SVCXPRT*);
extern _tt_db_results *_tt_remove_obj_1(_tt_remove_obj_args *,
					SVCXPRT*);
extern _tt_db_results *_tt_move_file_1(_tt_move_file_args *,
				       SVCXPRT*);
extern _tt_db_cache_results *_tt_set_file_props_1(_tt_set_file_props_args *,
						  SVCXPRT*);
extern _tt_db_cache_results *_tt_set_file_prop_1(_tt_set_file_prop_args *,
						 SVCXPRT*);
extern _tt_db_cache_results *_tt_add_file_prop_1(_tt_add_file_prop_args *,
						 SVCXPRT*);
extern _tt_db_cache_results *_tt_delete_file_prop_1(_tt_del_file_prop_args *,
						    SVCXPRT*);
extern _tt_file_prop_results *_tt_get_file_prop_1(_tt_get_file_prop_args *,
						  SVCXPRT*);
extern _tt_file_props_results *_tt_get_file_props_1(_tt_get_file_props_args *,
						    SVCXPRT*);
extern _tt_file_objs_results *_tt_get_file_objs_1(_tt_get_file_objs_args *,
						  SVCXPRT*);
extern _tt_db_results *_tt_set_file_access_1(_tt_set_file_access_args *,
					     SVCXPRT*);
extern _tt_file_access_results *
       _tt_get_file_access_1(_tt_get_file_access_args *, SVCXPRT*);
extern _tt_obj_props_results *_tt_set_obj_props_1(_tt_set_obj_props_args *,
						  SVCXPRT*);
extern _tt_obj_props_results *_tt_set_obj_prop_1(_tt_set_obj_prop_args *,
						 SVCXPRT*);
extern _tt_obj_props_results *_tt_add_obj_prop_1(_tt_add_obj_prop_args *,
						 SVCXPRT*);
extern _tt_obj_props_results *_tt_delete_obj_prop_1(_tt_del_obj_prop_args *,
						    SVCXPRT*);
extern _tt_obj_prop_results *_tt_get_obj_prop_1(_tt_get_obj_prop_args *,
						SVCXPRT*);
extern _tt_obj_props_results *_tt_get_obj_props_1(_tt_get_obj_props_args *,
						  SVCXPRT*);
extern _tt_db_results *_tt_set_obj_type_1(_tt_set_obj_type_args *,
					  SVCXPRT*);
extern _tt_obj_type_results *_tt_get_obj_type_1(_tt_get_obj_type_args *,
						SVCXPRT*);
extern _tt_db_results *_tt_set_obj_file_1(_tt_set_obj_file_args *,
					  SVCXPRT *);
extern _tt_obj_file_results *_tt_get_obj_file_1(_tt_get_obj_file_args *,
						SVCXPRT*);
extern _tt_db_results *_tt_set_obj_access_1(_tt_set_obj_access_args *,
					    SVCXPRT*);
extern _tt_obj_access_results *_tt_get_obj_access_1(_tt_get_obj_access_args *,
						    SVCXPRT*);
extern _tt_is_file_in_db_results *_tt_is_file_in_db_1(_tt_is_file_in_db_args *,
						      SVCXPRT *);
extern _tt_is_obj_in_db_results *_tt_is_obj_in_db_1(_tt_is_obj_in_db_args *,
						    SVCXPRT*);
extern _tt_db_results *_tt_queue_message_1(_tt_queue_msg_args *,
					   SVCXPRT*);
extern _tt_dequeue_msgs_results *
       _tt_dequeue_messages_1(_tt_dequeue_msgs_args *, SVCXPRT*);

extern _tt_file_netfile_results * _tt_file_netfile_1(_tt_file_netfile_args *,
						SVCXPRT*);
extern _tt_file_netfile_results * _tt_netfile_file_1(_tt_file_netfile_args *,
						SVCXPRT*);


extern _tt_get_all_sessions_results *
_tt_get_all_sessions_1(_tt_get_all_sessions_args *    /*NOTUSED*/,
		       SVCXPRT 			 * /*NOTUSED*/);

extern _tt_delete_session_results *
_tt_delete_session_1(_tt_delete_session_args *    /*NOTUSED*/,
		     SVCXPRT 			 * /*NOTUSED*/);

extern _tt_garbage_collect_results *
_tt_garbage_collect_1(void    * /*NOTUSED*/,
		      SVCXPRT * /*NOTUSED*/);

#endif /* _TT_DBCLIENT_SIDE */
#endif /* _DB_SERVER_H */
