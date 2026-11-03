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
/*%%  $XConsortium: ttdnd.h /main/3 1995/10/23 09:57:38 rswiston $ 			 				 */
/*
 *
 * ttdnd.h
 *
 * Copyright (c) 1990 by Sun Microsystems, Inc.
 */
#ifndef _TTDND_H
#define _TTDND_H

#ifdef __cplusplus
extern "C" {
#endif

#define TTDND_ATM_TARGET "_SUN_TOOLTALK"

typedef int (Ttdnd_send_data_func)(int chunknumber, int maxchunk,
				   const char *type,
				   void *buffer, int *chunksize,
				   int *is_last);
typedef int (Ttdnd_receive_data_func)(int chunknumber, const char *type,
				      void *buffer, int chunksize,
				      int is_last);
typedef void Ttdnd_input_func(void);

int ttdnd_init(int Xid, const char **validtypes,
	       Ttdnd_send_data_func *sendfunc,
	       Ttdnd_input_func **input_func, int *fd);

char *ttdnd_negotiate_types(const char **sourcetypes,
			    const char **targettypes);

void ttdnd_start_transfer(int sourceXid, const char *source_type,
			  Ttdnd_receive_data_func *receivefunc);

#ifdef __cplusplus
}
#endif
#endif /* _TTDND_H */
				
				 
