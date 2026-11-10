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
/*                                                                      *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company                     *
 * (c) Copyright 1993, 1994 International Business Machines Corp.       *
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.                      *
 * (c) Copyright 1993, 1994 Novell, Inc.                                *
 */
/*
 * xdm - display manager daemon
 *
 * $XConsortium: reset.c /main/4 1995/10/27 16:14:40 rswiston $
 *
 * Copyright 1988 Massachusetts Institute of Technology
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of M.I.T. not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  M.I.T. makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * Author:  Keith Packard, MIT X Consortium
 */

/*
 * pseudoReset -- pretend to reset the server by killing all clients
 * with windows.  It will reset the server most of the time, unless
 * a client remains connected with no windows.
 */

# include	<setjmp.h>
# include	<sys/types.h>
# include	<sys/signal.h>
# include	"dm.h"
# include	"vgmsg.h"


/***************************************************************************
 *
 *  Local procedure declarations
 *
 ***************************************************************************/

static int ignoreErrors( Display *dpy, XErrorEvent *event) ;
static void killWindows( Display *dpy, Window window) ;



/***************************************************************************
 *
 *  
 *
 ***************************************************************************/

/*ARGSUSED*/
static int 
ignoreErrors( Display *dpy, XErrorEvent *event )
{
	Debug ("Ignoring error...\n");
	return 1;
}

/*
 * this is mostly bogus -- but quite useful.  I wish the protocol
 * had some way of enumerating and identifying clients, that way
 * this code wouldn't have to be this kludgy.
 */

static void 
killWindows( Display *dpy, Window window )
{
	Window	root, parent, *children;
	int	child;
	unsigned int nchildren = 0;
	
	while (XQueryTree (dpy, window, &root, &parent, &children, &nchildren)
	       && nchildren > 0)
	{
		for (child = 0; child < nchildren; child++) {
			Debug ("Calling XKillClient() for window 0x%x\n",
				children[child]);
			XKillClient (dpy, children[child]);
		}
		XFree ((char *)children);
	}
}

/* seconds the server gets to answer the requests of the reset */
#ifndef RESET_TIMEOUT
#define RESET_TIMEOUT	30
#endif

static void *
resetJob( void *display )
{
	Display	*dpy = display;
	Window	root;
	int	screen;

	XSetErrorHandler (ignoreErrors);
	for (screen = 0; screen < ScreenCount (dpy); screen++) {
		Debug ("Pseudo reset screen %d\n", screen);
		root = RootWindow (dpy, screen);
		killWindows (dpy, root);
	}
	Debug ("Before XSync\n");
	XSync (dpy, False);
	return NULL;
}

/*
 * this display connection better not have any windows...
 *
 * The requests go to a server that may not answer; they are made by a thread
 * that is given RESET_TIMEOUT seconds (see BoundedCall).  When it does not
 * finish the connection is not used again.
 */
 
void 
pseudoReset( Display *dpy )
{
	void	*unused;

	if (BoundedCall (RESET_TIMEOUT, resetJob, dpy, &unused) != 1) {
		LogError(
		  ReadCatalog(MC_LOG_SET,MC_LOG_PSEUDO,MC_DEF_LOG_PSEUDO));
	}
	XSetErrorHandler (NULL);
	Debug ("pseudoReset() done\n");
}
