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
/* (c) Copyright 1997 The Open Group */
/*                                                                      *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company                     *
 * (c) Copyright 1993, 1994 International Business Machines Corp.       *
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.                      *
 * (c) Copyright 1993, 1994 Novell, Inc.                                *
 */
/*
 * xdm - display manager daemon
 *
 * $TOG: policy.c /main/6 1997/03/14 13:45:03 barstow $
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
 * policy.c.  Implement site-dependent policy for XDMCP connections
 */

# include "dm.h"

static ARRAY8 noAuthentication = { (CARD16) 0, (CARD8Ptr) 0 };
static ARRAY8 loopbackAddress  = { (CARD16) 0, (CARD8Ptr) 0 };

typedef struct _XdmAuth {
    ARRAY8  authentication;
    ARRAY8  authorization;
} XdmAuthRec, *XdmAuthPtr;

XdmAuthRec auth[] = {
#ifdef HASXDMAUTH
{ {(CARD16) 20, (CARD8 *) "XDM-AUTHENTICATION-1"},
  {(CARD16) 19, (CARD8 *) "XDM-AUTHORIZATION-1"},
},
#endif
{ {(CARD16) 0, (CARD8 *) 0},
  {(CARD16) 0, (CARD8 *) 0},
}
};

#define NumAuth	(sizeof auth / sizeof auth[0])



/***************************************************************************
 *
 *  Local procedure declarations
 *
 ***************************************************************************/

static char * WillingMsg( void ) ;


/********    End Local Function Declarations    ********/





ARRAY8Ptr 
ChooseAuthentication( ARRAYofARRAY8Ptr authenticationNames )
{
    int	i, j;

    for (i = 0; i < authenticationNames->length; i++)
	for (j = 0; j < NumAuth; j++)
	    if (XdmcpARRAY8Equal (&authenticationNames->data[i],
				  &auth[j].authentication))
		return &authenticationNames->data[i];
    return &noAuthentication;
}

int 
CheckAuthentication( struct protoDisplay *pdpy, ARRAY8Ptr displayID,
        	     ARRAY8Ptr name, ARRAY8Ptr data )
{
#ifdef HASXDMAUTH
    if (name->length == 20 && !memcmp (name->data, "XDM-AUTHENTICATION-1", 20))
	return XdmCheckAuthentication (pdpy, displayID, name, data);
#endif
    return TRUE;
}

int 
SelectAuthorizationTypeIndex( ARRAY8Ptr authenticationName,
			      ARRAYofARRAY8Ptr authorizationNames )
{
    int	i, j;

    for (j = 0; j < NumAuth; j++)
	if (XdmcpARRAY8Equal (authenticationName,
			      &auth[j].authentication))
	    break;
    if (j < NumAuth)
    {
    	for (i = 0; i < authorizationNames->length; i++)
	    if (XdmcpARRAY8Equal (&authorizationNames->data[i],
				  &auth[j].authorization))
	    	return i;
    }
    for (i = 0; i < authorizationNames->length; i++)
	if (ValidAuthorization (authorizationNames->data[i].length,
				(char *) authorizationNames->data[i].data))
	    return i;
    return -1;
}

#if 0
int 
Willing( struct sockaddr *addr, int addrlen, ARRAY8Ptr authenticationName,
	 ARRAY8Ptr status )
#endif

int 
Willing(
        ARRAY8Ptr addr,
#if NeedWidePrototypes
        int connectionType,
#else
        CARD16 connectionType,
#endif /* NeedWidePrototypes */
        ARRAY8Ptr authenticationName,
        ARRAY8Ptr status,
        xdmOpCode type )
{
    static char	statusBuf[256];
    int		ret;
#if 0
    extern char	*localHostname (void);
#endif
    ret = AcceptableDisplayAddress (addr, connectionType, type);
    if (!ret)
	snprintf (statusBuf, sizeof(statusBuf), "Display not authorized to connect");
    else
	snprintf (statusBuf, sizeof(statusBuf), "%s", WillingMsg());
#if 0
    	snprintf (statusBuf, sizeof(statusBuf), "host %s", localHostname());
#endif
    /* enforce termination */
    statusBuf[255] = '\0';

    status->length = strlen(statusBuf);
    status->data = (CARD8Ptr) malloc (status->length);
    if (!status->data)
	status->length = 0;
    else
	bcopy (statusBuf, (char *)status->data, status->length);
    return ret;
}

ARRAY8Ptr 
Accept( struct sockaddr *from, int fromlen,
#if NeedWidePrototypes
        int displayNumber )
#else
        CARD16 displayNumber )
#endif /* NeedWidePrototypes */
{
    return 0;
}

int 
SelectConnectionTypeIndex( ARRAY16Ptr connectionTypes,
			   ARRAYofARRAY8Ptr connectionAddresses,
			   ARRAY8Ptr peerAddress )
{

    int i;
    
    /*
     *  the display is reached at the address it names, and the name comes from
     *  the request: a request names only the address it comes from.  A request
     *  from this host itself (the loopback address) may name any address of the
     *  host; the first one that is not the loopback address is used.
     */
     
    /*
     *  initialize loopback address array if not already done so...
     *
     */
    if (loopbackAddress.length == 0 &&
	XdmcpAllocARRAY8 (&loopbackAddress, 4) ) {

	loopbackAddress.data[0] = 127;
	loopbackAddress.data[1] = 0;
	loopbackAddress.data[2] = 0;
	loopbackAddress.data[3] = 1;
    }
    
    /*
     *  only IPv4 addresses can be turned into a display name.  Return -1
     *  when the display offers no usable address.
     */
    for (i = 0; i < connectionAddresses->length &&
		i < connectionTypes->length; i++) {
	if (connectionTypes->data[i] == FamilyInternet &&
	    connectionAddresses->data[i].length == 4 &&
	    !XdmcpARRAY8Equal (&connectionAddresses->data[i],
				&loopbackAddress) &&
	    peerAddress && peerAddress->length == 4 &&
	    (XdmcpARRAY8Equal (&connectionAddresses->data[i], peerAddress) ||
	     XdmcpARRAY8Equal (peerAddress, &loopbackAddress)))
	    return i;
    }

    return -1;
}




/***************************************************************************
 *
 *  WillingMsg
 *
 *  Generate a message for the "Willing" status field.
 *  
 ***************************************************************************/

# define LINEBUFSIZE 32

static char *
WillingMsg( void )
{
    static char retbuf[LINEBUFSIZE];
    char	linebuf[LINEBUFSIZE];
    char	*cp;
    FILE	*f;


    /* Return selected part from an 'uptime' to Server 	*/
    /* for use in hosts status field when XDMCP broadcast is used */
    /* (useful for selecting host to be managed by) 	*/

    strcpy(retbuf, "Willing to Manage");

    /*
     *  read the command output through a pipe; a temporary file named in
     *  a shared directory could be replaced by a link before it is read.
     */
    f = popen ("uptime | awk '{printf(\"%s %-.5s  load: %.3s, %.3s, %.3s\","
	       "$(NF-6),$(NF-5),$(NF-2),$(NF-1),$NF)}'", "r");
    if (f != (FILE *) NULL) {
	if (fgets(linebuf,sizeof(linebuf),f) != NULL) {
	    if ( (cp = strchr(linebuf,'\n')) != NULL)
		*cp = '\0';

	    if (strlen(linebuf) > 10) 	/* seems reasonable? */
		snprintf(retbuf, sizeof(retbuf), "%s", linebuf);
	}

	pclose(f);
    }

    return (retbuf);
}
