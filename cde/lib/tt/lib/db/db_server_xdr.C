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
//%%  $XConsortium: db_server_xdr.C /main/4 1996/01/30 16:08:48 barstow $ 			 				
/*
 * @(#)db_server_xdr.C	1.19 95/01/06
 *
 * Tool Talk Database Server
 *
 * Copyright (c) 1992 by Sun Microsystems, Inc.
 *
 * This file contains xdr functions for the db server's rpc procedures.
 */

#include <stdlib.h>
#include <isam.h>

#include "db/db_server.h"

#if ISMAXRECLEN != 32767
#error "TT_DB_LIMIT_RECORD of db/db_server.h is the ISMAXRECLEN of mini_isam"
#endif

/*
 * A string of the protocol, of at most MAXLEN characters.  The storage of a
 * string that is decoded is as long as the string says it is, so the limit is
 * what makes it safe to read a length from a request.  A string that is not
 * there, a null pointer in a reply or an argument, is sent as the empty string:
 * the receiver gets "" for it, which everything that reads a string of this
 * protocol takes for no string.
 */
static bool_t
xdr_tt_cstring(XDR *xdrs, char **sp, u_int maxlen)
{
	if (xdrs->x_op == XDR_ENCODE && *sp == NULL) {
		char empty[1] = { '\0' };
		char *none = empty;

		return xdr_string(xdrs, &none, maxlen);
	}
	return xdr_string(xdrs, sp, maxlen);
}

bool_t
xdr_tt_file_name(XDR *xdrs, char **sp)
{
	return xdr_tt_cstring(xdrs, sp, TT_DB_LIMIT_FILE);
}

bool_t
xdr_tt_objid_name(XDR *xdrs, char **sp)
{
	return xdr_tt_cstring(xdrs, sp, TT_DB_LIMIT_OBJID);
}


bool_t xdr_tt_file_netfile_args(XDR * xdrs, _tt_file_netfile_args * argp)
{
        if (!xdr_tt_cstring(xdrs, &argp->file_or_netfile, TT_DB_LIMIT_NETFILE)) {
                return (FALSE);
        }
	return (TRUE);
}

bool_t
xdr_tt_file_netfile_results(XDR * xdrs, _tt_file_netfile_results * argp)
{
        if (!xdr_tt_cstring(xdrs, &argp->result_string, TT_DB_LIMIT_NETFILE)) {
                return (FALSE);
        }
        if (!xdr_tt_db_results(xdrs, &argp->results)) {
                return (FALSE);
        }
	if (!xdr_int(xdrs, &argp->tt_status)) {
		return (FALSE);
	}
	return (TRUE);
}


/* The record without the rest of the list. */

/*
 * A list of records goes over the wire as each record behind a flag that
 * says it is there (what xdr_pointer() does for the pointer to a record that
 * has a list in it) and a last flag that says it is not.  The length of the
 * list is up to the sender, so it is walked here, not recursed into: a
 * request can hold more records than there is stack.
 */

bool_t
xdr_tt_db_results(XDR *xdrs, _tt_db_results *objp)
{
	if (!xdr_enum(xdrs, (enum_t *)objp)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_ptype(XDR *xdrs, _tt_string *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->value, TT_DB_LIMIT_TYPE)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_objid(XDR *xdrs, _tt_string *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->value, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_session(XDR *xdrs, _tt_string *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->value, TT_DB_LIMIT_SESSION)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_property_value(XDR *xdrs, _tt_property_value *objp)
{
	if (!xdr_bytes(xdrs, (char **)&objp->value.value_val, (u_int *)&objp->value.value_len, TT_DB_LIMIT_PROPERTY_VALUE)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_property(XDR *xdrs, _tt_property *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->name, TT_DB_LIMIT_PROPERTY_NAME)) {
		return (FALSE);
	}
	if (!xdr_array(xdrs, (char **)&objp->values.values_val, (u_int *)&objp->values.values_len, TT_DB_LIMIT_PROPERTY_VALUES, sizeof(_tt_property_value), (xdrproc_t)xdr_tt_property_value)) {
		return (FALSE);
	}
	return (TRUE);
}

/*
 * A user, a group or a mode is 32 bits on the wire, whatever the size of its
 * type is (mode_t is 16 bits on some hosts), and -1, the wild card of an
 * access, is -1 on all of them.  The field is moved through a 32 bit value
 * of its own: a pointer to a uid_t cast to a long * made the routine read and
 * write 8 bytes of a 4 byte field on a 64 bit host, and the last field of an
 * access wrote 4 bytes beyond the end of it.
 */
template <class T>
static bool_t
xdr_tt_id32(XDR *xdrs, T *idp)
{
	u_int wire = 0;

	if (xdrs->x_op == XDR_ENCODE) {
		wire = (*idp == (T)-1) ? 0xffffffffU : (u_int)*idp;
	}
	if (!xdr_u_int(xdrs, &wire)) {
		return (FALSE);
	}
	if (xdrs->x_op == XDR_DECODE) {
		*idp = (wire == 0xffffffffU) ? (T)-1 : (T)wire;
	}
	return (TRUE);
}

bool_t
xdr_tt_access(XDR *xdrs, _tt_access *objp)
{
	if (!xdr_tt_id32(xdrs, &objp->user)) {
		return (FALSE);
	}
	if (!xdr_tt_id32(xdrs, &objp->group)) {
		return (FALSE);
	}
	if (!xdr_tt_id32(xdrs, &objp->mode)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_message(XDR *xdrs, _tt_message *objp)
{
	if (!xdr_bytes(xdrs, (char **)&objp->body.body_val, (u_int *)&objp->body.body_len, TT_DB_LIMIT_MESSAGE)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_create_file_args(XDR *xdrs, _tt_create_file_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_array(xdrs, (char **)&objp->properties.properties_val, (u_int *)&objp->properties.properties_len, TT_DB_LIMIT_PROPERTIES, sizeof(_tt_property), (xdrproc_t)xdr_tt_property)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_create_obj_args(XDR *xdrs, _tt_create_obj_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_cstring(xdrs, &objp->otype, TT_DB_LIMIT_TYPE)) {
		return (FALSE);
	}
	if (!xdr_array(xdrs, (char **)&objp->properties.properties_val, (u_int *)&objp->properties.properties_len, TT_DB_LIMIT_PROPERTIES, sizeof(_tt_property), (xdrproc_t)xdr_tt_property)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t   
xdr_tt_remove_file_args(XDR *xdrs, _tt_remove_file_args *objp)
{
        if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
                return (FALSE);
        }
        if (!xdr_tt_access(xdrs, &objp->access)) {
                return (FALSE);
        }
        return (TRUE);
}
 
bool_t
xdr_tt_remove_obj_args(XDR *xdrs, _tt_remove_obj_args *objp)
{
        if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
                return (FALSE);
        }
        if (!xdr_tt_cstring(xdrs, &objp->forward_pointer, TT_DB_LIMIT_OBJID)) {
                return (FALSE);
        }
        if (!xdr_tt_access(xdrs, &objp->access)) {
                return (FALSE);
        }
        return (TRUE);
}

bool_t
xdr_tt_move_file_args(XDR *xdrs, _tt_move_file_args *objp)
{
        if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
                return (FALSE);
        }
        if (!xdr_tt_cstring(xdrs, &objp->new_file, TT_DB_LIMIT_FILE)) {
                return (FALSE);
        }
        if (!xdr_tt_access(xdrs, &objp->access)) {
                return (FALSE);
        }
        return (TRUE);
}

bool_t
xdr_tt_set_file_prop_args(XDR *xdrs, _tt_set_file_prop_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_property(xdrs, &objp->property)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_set_file_props_args(XDR *xdrs, _tt_set_file_props_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_array(xdrs, (char **)&objp->properties.properties_val, (u_int *)&objp->properties.properties_len, TT_DB_LIMIT_PROPERTIES, sizeof(_tt_property), (xdrproc_t)xdr_tt_property)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_add_file_prop_args(XDR *xdrs, _tt_add_file_prop_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_property(xdrs, &objp->property)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->unique)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_del_file_prop_args(XDR *xdrs, _tt_del_file_prop_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_property(xdrs, &objp->property)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_get_file_prop_args(XDR *xdrs, _tt_get_file_prop_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_cstring(xdrs, &objp->name, TT_DB_LIMIT_PROPERTY_NAME)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_get_file_props_args(XDR *xdrs, _tt_get_file_props_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_get_file_objs_args(XDR *xdrs, _tt_get_file_objs_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_set_file_access_args(XDR *xdrs, _tt_set_file_access_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->new_access)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_get_file_access_args(XDR *xdrs, _tt_get_file_access_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_set_obj_prop_args(XDR *xdrs, _tt_set_obj_prop_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_property(xdrs, &objp->property)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_set_obj_props_args(XDR *xdrs, _tt_set_obj_props_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_array(xdrs, (char **)&objp->properties.properties_val, (u_int *)&objp->properties.properties_len, TT_DB_LIMIT_PROPERTIES, sizeof(_tt_property), (xdrproc_t)xdr_tt_property)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_add_obj_prop_args(XDR *xdrs, _tt_add_obj_prop_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_property(xdrs, &objp->property)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->unique)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_del_obj_prop_args(XDR *xdrs, _tt_del_obj_prop_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_property(xdrs, &objp->property)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_get_obj_prop_args(XDR *xdrs, _tt_get_obj_prop_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_cstring(xdrs, &objp->name, TT_DB_LIMIT_PROPERTY_NAME)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_get_obj_props_args(XDR *xdrs, _tt_get_obj_props_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_set_obj_type_args(XDR *xdrs, _tt_set_obj_type_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_cstring(xdrs, &objp->otype, TT_DB_LIMIT_TYPE)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_get_obj_type_args(XDR *xdrs, _tt_get_obj_type_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_set_obj_file_args(XDR *xdrs, _tt_set_obj_file_args *objp)
{
        if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
                return (FALSE);
        }
        if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
                return (FALSE);
        }
        if (!xdr_tt_access(xdrs, &objp->access)) {
                return (FALSE);
        }
        return (TRUE);
}

bool_t
xdr_tt_get_obj_file_args(XDR *xdrs, _tt_get_obj_file_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_set_obj_access_args(XDR *xdrs, _tt_set_obj_access_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->new_access)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_get_obj_access_args(XDR *xdrs, _tt_get_obj_access_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_is_file_in_db_args(XDR *xdrs, _tt_is_file_in_db_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_is_obj_in_db_args(XDR *xdrs, _tt_is_obj_in_db_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->objid, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_queue_msg_args(XDR *xdrs, _tt_queue_msg_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_array(xdrs, (char **)&objp->ptypes.values_val, (u_int *)&objp->ptypes.values_len, TT_DB_LIMIT_PTYPES, sizeof(_tt_string), (xdrproc_t)xdr_tt_ptype)) {
		return (FALSE);
	}
	if (!xdr_tt_message(xdrs, &objp->message)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_dequeue_msgs_args(XDR *xdrs, _tt_dequeue_msgs_args *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_array(xdrs, (char **)&objp->ptypes.values_val, (u_int *)&objp->ptypes.values_len, TT_DB_LIMIT_PTYPES, sizeof(_tt_string), (xdrproc_t)xdr_tt_ptype)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_db_cache_results(XDR *xdrs, _tt_db_cache_results *objp)
{
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_auth_level_results(XDR *xdrs, _tt_auth_level_results *objp)
{
	if (!xdr_int(xdrs, &objp->auth_level)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_file_partition_results(XDR *xdrs, _tt_file_partition_results *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->partition, TT_DB_LIMIT_PATH)) {
		return (FALSE);
	}
	if (!xdr_tt_cstring(xdrs, &objp->network_path, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_file_prop_results(XDR *xdrs, _tt_file_prop_results *objp)
{
	if (!xdr_tt_property(xdrs, &objp->property)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_file_props_results(XDR *xdrs, _tt_file_props_results *objp)
{
	if (!xdr_array(xdrs, (char **)&objp->properties.properties_val, (u_int *)&objp->properties.properties_len, TT_DB_LIMIT_PROPERTIES, sizeof(_tt_property), (xdrproc_t)xdr_tt_property)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_file_objs_results(XDR *xdrs, _tt_file_objs_results *objp)
{
	if (!xdr_array(xdrs, (char **)&objp->objids.values_val, (u_int *)&objp->objids.values_len, TT_DB_LIMIT_OBJECTS, sizeof(_tt_string), (xdrproc_t)xdr_tt_objid)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_file_access_results(XDR *xdrs, _tt_file_access_results *objp)
{
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_obj_prop_results(XDR *xdrs, _tt_obj_prop_results *objp)
{
	if (!xdr_tt_property(xdrs, &objp->property)) {
		return (FALSE);
	}
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_obj_props_results(XDR *xdrs, _tt_obj_props_results *objp)
{
	if (!xdr_array(xdrs, (char **)&objp->properties.properties_val, (u_int *)&objp->properties.properties_len, TT_DB_LIMIT_PROPERTIES, sizeof(_tt_property), (xdrproc_t)xdr_tt_property)) {
		return (FALSE);
	}
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_int(xdrs, &objp->cache_level)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_obj_type_results(XDR *xdrs, _tt_obj_type_results *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->otype, TT_DB_LIMIT_TYPE)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_obj_file_results(XDR *xdrs, _tt_obj_file_results *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->file, TT_DB_LIMIT_FILE)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_obj_access_results(XDR *xdrs, _tt_obj_access_results *objp)
{
	if (!xdr_tt_access(xdrs, &objp->access)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_is_file_in_db_results(XDR *xdrs, _tt_is_file_in_db_results *objp)
{
	if (!xdr_bool(xdrs, &objp->directory_flag)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_is_obj_in_db_results(XDR *xdrs, _tt_is_obj_in_db_results *objp)
{
	if (!xdr_tt_cstring(xdrs, &objp->forward_pointer, TT_DB_LIMIT_OBJID)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_dequeue_msgs_results(XDR *xdrs, _tt_dequeue_msgs_results *objp)
{
	if (!xdr_array(xdrs, (char **)&objp->messages.messages_val, (u_int *)&objp->messages.messages_len, TT_DB_LIMIT_MESSAGES, sizeof(_tt_message), (xdrproc_t)xdr_tt_message)) {
		return (FALSE);
	}
	if (!xdr_tt_db_results(xdrs, &objp->results)) {
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_tt_get_all_sessions_args(XDR *xdrs, _tt_get_all_sessions_args *objp)
{
	return(xdr_array(xdrs, (char **)&objp->oidkey.oidkey_val, (u_int *)&objp->oidkey.oidkey_len, OID_KEY_LENGTH, sizeof(char), (xdrproc_t)xdr_char));
}

bool_t
xdr_tt_get_all_sessions_results(XDR *xdrs, _tt_get_all_sessions_results *objp)
{
	if (!xdr_array(xdrs,
		       (char **)&objp->oidkey.oidkey_val,
		       (u_int *)&objp->oidkey.oidkey_len,
		       MAXKEYSIZE,
		       sizeof(char),
		       (xdrproc_t)xdr_char)) {
		return(FALSE);
	}

	if (!xdr_array(xdrs,
		       (char **)&objp->session_list.values_val,
		       (u_int *)&objp->session_list.values_len,
		       OPT_MAX_GET_SESSIONS,
		       sizeof(char *),
		       (xdrproc_t)xdr_tt_session)) {
		return (FALSE);
	}
	return(TRUE);
}

bool_t
xdr_tt_garbage_collect_results(XDR *xdrs, _tt_garbage_collect_results *objp)
{
	return(xdr_int(xdrs, &objp->tt_status));
}

bool_t
xdr_tt_delete_session_results(XDR *xdrs, _tt_delete_session_results *objp)
{
	return(xdr_int(xdrs, &objp->tt_status));
}


bool_t
xdr_tt_delete_session_args(XDR *xdrs, _tt_delete_session_args *objp)
{
	return(xdr_tt_cstring(xdrs, &objp->session.value, TT_DB_LIMIT_SESSION));
}


