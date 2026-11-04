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
/* $TOG: genauth.c /main/6 1997/03/25 12:33:13 barstow $ */
/* (c) Copyright 1997, The Open Group */
/*                                                                      *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company                     *
 * (c) Copyright 1993, 1994 International Business Machines Corp.       *
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.                      *
 * (c) Copyright 1993, 1994 Novell, Inc.                                *
 */

/*
 * @DEC_COPYRIGHT@
 */
/*
 * HISTORY
 * $Log$
 * Revision 1.1.2.2  1995/04/21  13:05:23  Peter_Derr
 * 	dtlogin auth key fixes from deltacde
 * 	[1995/04/14  18:03:41  Peter_Derr]
 *
 * 	R6 xdm code used in dtlogin.
 * 	[1995/04/10  16:52:31  Peter_Derr]
 *
 * Revision 1.1.3.3  1995/02/20  21:03:19  Peter_Derr
 * 	merge XC fix-11
 * 	[1995/02/20  20:13:02  Peter_Derr]
 * 
 * Revision 1.1.3.2  1994/07/13  19:26:25  Peter_Derr
 * 	Include Wrap.h to get definitions for XDM-AUTHENTICATION-1
 * 	authorization mechanism.
 * 	[1994/07/13  12:15:59  Peter_Derr]
 * 
 * $EndLog$
 */
#ifndef lint
static char *rcsid = "@(#)$RCSfile: genauth.c $ $Revision: /main/6 $ (DEC) $Date: 1997/03/25 12:33:13 $";
#endif
/*

Copyright (c) 1988  X Consortium

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE X CONSORTIUM BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of the X Consortium shall
not be used in advertising or otherwise to promote the sale, use or
other dealings in this Software without prior written authorization
from the X Consortium.

*/

/*
 * xdm - display manager daemon
 * Author:  Keith Packard, MIT X Consortium
 */

# include   <X11/Xauth.h>
# include   <X11/Xos.h>
# include   "dm.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * Authorization data is a secret shared with the X server, so it has to be
 * unpredictable: take it from the system's random number generator.
 * Returns 0, with nothing usable left in auth, if the generator cannot
 * supply len bytes.
 */
int
GenerateAuthData (char *auth, int len)
{
    static const char	randomFile[] = "/dev/urandom";
    int			fd;
    int			used = 0;
    ssize_t		got;

    if (len < 0)
	return 0;
    if ((fd = open (randomFile, O_RDONLY)) < 0)
    {
	LogError((unsigned char *) "Cannot open \"%s\", errno = %d\n",
		 randomFile, errno);
	return 0;
    }
    while (used < len)
    {
	got = read (fd, auth + used, len - used);
	if (got < 0 && errno == EINTR)
	    continue;
	if (got <= 0)
	    break;
	used += got;
    }
    close (fd);
    if (used != len)
    {
	LogError((unsigned char *) "Could not read %d bytes from \"%s\"\n",
		 len, randomFile);
	bzero (auth, len);
	return 0;
    }
    return 1;
}
