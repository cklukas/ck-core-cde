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
 * $TOG: choose.c /main/5 1997/08/13 11:42:50 kaleb $
 *
 * Copyright 1990 Massachusetts Institute of Technology
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of M.I.T. not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  M.I.T. makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * M.I.T. DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL M.I.T.
 * BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN 
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Author:  Keith Packard, MIT X Consortium
 */

/*
 * choose.c
 *
 * xdm interface to chooser program
 */

# include   "dm.h"
# include   "vg.h"

# include	<X11/X.h>
# include	<sys/types.h>
# include	<sys/socket.h>
# include	<netinet/in.h>
# include	<sys/un.h>
# include	<ctype.h>
# include	<errno.h>

static char *Print8Address (ARRAY8Ptr   Address);

int LoadXloginResources(struct display *d); // session.c
void GetSysParms(char **tzpp, char **fhpp, char **ftpp); // fontpath.c
int GetChooserAddr(char *addr, int *lenp); // socket.c
int NetaddrFamily(XdmcpNetaddr netaddrp); // netaddr.c


static int
FormatBytes (
    unsigned char *data,
    int     length,
    char    *buf,
    int     buflen)
{
    int	    i;
    static char	    HexChars[] = "0123456789abcdef";

    if (buflen < length * 2 + 1)
	return 0;
    for (i = 0; i < length; i++)
    {
	*buf++ = HexChars[(data[i] >> 4) & 0xf];
	*buf++ = HexChars[(data[i]) & 0xf];
    }
    *buf++ = '\0';
    return 1;
}

static int
FormatARRAY8 (
    ARRAY8Ptr   a,
    char        *buf,
    int         buflen)
{
    return FormatBytes (a->data, a->length, buf, buflen);
}

/*
 * The clients that sent an indirect query and have not been managed yet.  The
 * query is a datagram that anybody can send with any source address, so the
 * list is bounded and an entry that nobody follows up is dropped.
 */
#define MAX_INDIRECT_USERS	256
#define INDIRECT_USER_LIFETIME	(10 * 60)	/* seconds */

typedef struct _IndirectUsers {
    struct _IndirectUsers   *next;
    ARRAY8	client;
    CARD16	connectionType;
    time_t	time;
} IndirectUsersRec, *IndirectUsersPtr;

static IndirectUsersPtr	indirectUsers;

/* drop the clients that have been waiting for too long; the count is returned */
static int
ExpireIndirectClients (time_t now)
{
    IndirectUsersPtr	i, next, *prev;
    int			count = 0;

    prev = &indirectUsers;
    for (i = indirectUsers; i; i = next)
    {
	next = i->next;
	if (now - i->time > INDIRECT_USER_LIFETIME)
	{
	    *prev = next;
	    XdmcpDisposeARRAY8 (&i->client);
	    free ((char *) i);
	}
	else
	{
	    prev = &i->next;
	    ++count;
	}
    }
    return count;
}

int
RememberIndirectClient (
    ARRAY8Ptr   clientAddress,
    CARD16      connectionType)
{
    IndirectUsersPtr	i;
    time_t		now = time (0);

    for (i = indirectUsers; i; i = i->next)
	if (XdmcpARRAY8Equal (clientAddress, &i->client) &&
	    connectionType == i->connectionType)
	{
	    i->time = now;
	    return 1;
	}
    if (ExpireIndirectClients (now) >= MAX_INDIRECT_USERS)
	return 0;
    i = (IndirectUsersPtr) malloc (sizeof (IndirectUsersRec));
    if (!i)
	return 0;
    if (!XdmcpCopyARRAY8 (clientAddress, &i->client))
    {
	free ((char *) i);
	return 0;
    }
    i->connectionType = connectionType;
    i->time = now;
    i->next = indirectUsers;
    indirectUsers = i;
    return 1;
}

void
ForgetIndirectClient (
    ARRAY8Ptr   clientAddress,
    CARD16      connectionType)
{
    IndirectUsersPtr	i, prev;

    prev = 0;
    for (i = indirectUsers; i; i = i->next)
    {
	if (XdmcpARRAY8Equal (clientAddress, &i->client) &&
	    connectionType == i->connectionType)
	{
	    if (prev)
		prev->next = i->next;
	    else
		indirectUsers = i->next;
	    XdmcpDisposeARRAY8 (&i->client);
	    free ((char *) i);
	    break;
	}
	prev = i;
    }
}

int
IsIndirectClient (
    ARRAY8Ptr   clientAddress,
    CARD16      connectionType)
{
    IndirectUsersPtr	i;

    for (i = indirectUsers; i; i = i->next)
	if (XdmcpARRAY8Equal (clientAddress, &i->client) &&
	    connectionType == i->connectionType)
	    return 1;
    return 0;
}

static int
FormatChooserArgument (
    char    *buf,
    int     len)
{
    unsigned char   addr_buf[1024];
    int		    addr_len = sizeof (addr_buf);
    unsigned char   result_buf[1024];
    int		    result_len = 0;
    int		    netfamily;

    if (GetChooserAddr (addr_buf, &addr_len) == -1)
    {
	LogError ((unsigned char *)"Cannot get return address for chooser socket\n");
	Debug ("Cannot get chooser socket address\n");
	return 0;
    }
    netfamily = NetaddrFamily((XdmcpNetaddr)addr_buf);
    switch (netfamily) {
    case AF_UNIX:
	{
	    /* the path of the socket follows the family */
	    struct sockaddr_un *un = (struct sockaddr_un *) addr_buf;
	    size_t pathlen = strnlen (un->sun_path, sizeof (un->sun_path));

	    if (pathlen == 0 || pathlen >= sizeof (un->sun_path) ||
		2 + pathlen > sizeof (result_buf))
		return 0;
	    result_buf[0] = netfamily >> 8;
	    result_buf[1] = netfamily & 0xFF;
	    memcpy ((char *)result_buf+2, un->sun_path, pathlen);
	    result_len = 2 + pathlen;
	}
	break;
    default:
	Debug ("Chooser family %d isn't known\n", netfamily);
	return 0;
    }

    return FormatBytes (result_buf, result_len, buf, len);
}

/*
 * What the choosers report.  An entry is created when a display is started
 * with a chooser (AwaitIndirectChoice) and holds no choice until the chooser
 * of that display reports one; the choice is kept CHOICE_LIFETIME seconds for
 * the query that the display sends next.  A report for a client that has no
 * entry is not a report from a chooser this manager started and is dropped.
 */
#define CHOICE_LIFETIME		15		/* seconds a choice is kept */
#define CHOOSER_LIFETIME	(10 * 60)	/* seconds a chooser may run */
#define MAX_CHOICES		256

typedef struct _Choices {
    struct _Choices *next;
    ARRAY8	    client;
    CARD16	    connectionType;
    ARRAY8	    choice;
    int		    chosen;
    long	    time;
} ChoiceRec, *ChoicePtr;

static ChoicePtr   choices;

/* drop the entries that are out of date; the number of entries left is returned */
static int
ExpireChoices (long now)
{
    ChoicePtr	c, next, *prev;
    int		count = 0;

    prev = &choices;
    for (c = choices; c; c = next)
    {
	next = c->next;
	if (now - c->time > (c->chosen ? CHOICE_LIFETIME : CHOOSER_LIFETIME))
	{
	    Debug ("Timeout choice\n");
	    *prev = next;
	    XdmcpDisposeARRAY8 (&c->client);
	    XdmcpDisposeARRAY8 (&c->choice);
	    free ((char *) c);
	}
	else
	{
	    prev = &c->next;
	    ++count;
	}
    }
    return count;
}

ARRAY8Ptr
IndirectChoice (
    ARRAY8Ptr   clientAddress,
    CARD16      connectionType)
{
    ChoicePtr	c;

    ExpireChoices (time (0));
    for (c = choices; c; c = c->next)
	if (c->chosen &&
	    XdmcpARRAY8Equal (clientAddress, &c->client) &&
	    connectionType == c->connectionType)
	    return &c->choice;
    return 0;
}

/*
 * A chooser is about to run for this client: its choice will be accepted.
 */
int
AwaitIndirectChoice (
    ARRAY8Ptr   clientAddress,
    CARD16      connectionType)
{
    ChoicePtr	c;
    long	now = time (0);

    if (ExpireChoices (now) >= MAX_CHOICES)
	return 0;
    for (c = choices; c; c = c->next)
	if (XdmcpARRAY8Equal (clientAddress, &c->client) &&
	    connectionType == c->connectionType)
	{
	    XdmcpDisposeARRAY8 (&c->choice);
	    c->chosen = 0;
	    c->time = now;
	    return 1;
	}
    c = (ChoicePtr) malloc (sizeof (ChoiceRec));
    if (!c)
	return 0;
    if (!XdmcpCopyARRAY8 (clientAddress, &c->client))
    {
	free ((char *) c);
	return 0;
    }
    c->connectionType = connectionType;
    c->choice.length = 0;
    c->choice.data = 0;
    c->chosen = 0;
    c->time = now;
    c->next = choices;
    choices = c;
    return 1;
}

static int
RegisterIndirectChoice (
    ARRAY8Ptr   clientAddress,
    CARD16 connectionType,
    ARRAY8Ptr   choice)
{
    ChoicePtr	c;
    ARRAY8	copy;

    Debug ("Got indirect choice back (%s)\n", Print8Address(clientAddress));
    ExpireChoices (time (0));
    for (c = choices; c; c = c->next)
	if (XdmcpARRAY8Equal (clientAddress, &c->client) &&
	    connectionType == c->connectionType)
	    break;
    if (!c)
	return 0;

    /* the old choice stays when the new one cannot be kept */
    if (!choice->length || !XdmcpCopyARRAY8 (choice, &copy))
	return 0;
    XdmcpDisposeARRAY8 (&c->choice);
    c->choice = copy;
    c->chosen = 1;
    c->time = time (0);

    Debug("choice=(%s)\n", Print8Address(choice)); 

    return 1;
}

/*ARGSUSED*/
static void
AddChooserHost (
    CARD16      connectionType,
    ARRAY8Ptr   addr,
    char        *closure)
{
    char	***argp;
    char	hostbuf[1024];

    argp = (char ***) closure;
    if (!*argp)
	return; /* Earlier callback failed; do not restart from a host argument. */
    if (addr->length == strlen ("BROADCAST") &&
	!strncmp ((char *)addr->data, "BROADCAST", addr->length))
    {
	if (!parseArgs (argp, "BROADCAST")) {
	    freeEnv(*argp);
	    *argp = NULL;
	}
    }
    else if (FormatARRAY8 (addr, hostbuf, sizeof (hostbuf)))
    {
	if (!parseArgs (argp, hostbuf)) {
	    freeEnv(*argp);
	    *argp = NULL;
	}
    }
}

/* seconds an accepted chooser connection may stay silent */
#ifndef CHOOSER_REPLY_TIMEOUT
#define CHOOSER_REPLY_TIMEOUT 5
#endif

int
ProcessChooserSocket (
    int fd)
{
    int client_fd;
    char	buf[1024];
    int		len;
    struct sockaddr_storage peer;
    socklen_t	peerlen = sizeof (peer);
    XdmcpBuffer	buffer;
    ARRAY8	clientAddress;
    CARD16	connectionType;
    ARRAY8	choice;
    struct timeval timeout;

    Debug ("Process chooser socket\n");
    client_fd = accept (fd, (struct sockaddr *)&peer, &peerlen);
    if (client_fd == -1)
    {
	LogError ((unsigned char *)"Cannot accept chooser connection\n");
	return 0;
    }
    Debug ("Accepted %d\n", client_fd);
    /* the peer is not trusted: a silent connection must not stall the manager */
    timeout.tv_sec = CHOOSER_REPLY_TIMEOUT;
    timeout.tv_usec = 0;
    setsockopt (client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout));
    
    len = read (client_fd, buf, sizeof (buf));
    Debug ("Read returns %d\n", len);
    if (len > 0)
    {
    	buffer.data = (BYTE *) buf;
    	buffer.size = sizeof (buf);
    	buffer.count = len;
    	buffer.pointer = 0;
	clientAddress.data = 0;
	clientAddress.length = 0;
	choice.data = 0;
	choice.length = 0;
	if (XdmcpReadARRAY8 (&buffer, &clientAddress) &&
	    XdmcpReadCARD16 (&buffer, &connectionType) &&
	    XdmcpReadARRAY8 (&buffer, &choice))
	{
	    Debug ("Read from chooser successfully\n");
	    if (!RegisterIndirectChoice (&clientAddress, connectionType, &choice))
		Debug ("Invalid chooser reply\n");
	}
	XdmcpDisposeARRAY8 (&clientAddress);
	XdmcpDisposeARRAY8 (&choice);
    }
    else
    {
	LogError ((unsigned char *)"Choice response read error %s\n", strerror(errno));
    }

    close (client_fd);
    return 1;
}

void
RunChooser (
    struct display  *d)
{
    char    **args = NULL;
    char    buf[1024];
    char    **env;

    Debug ("RunChooser %s\n", d->name);
    SetTitle (d->name, "chooser");
    LoadXloginResources (d);

    if (!parseArgs (&args, d->chooser) || !args[0])
	goto argument_failure;
    strcpy (buf, "-xdmaddress ");
    if (FormatChooserArgument (buf + strlen (buf), sizeof (buf) - strlen (buf)) &&
	!parseArgs (&args, buf))
	goto argument_failure;
    strcpy (buf, "-clientaddress ");
    if (FormatARRAY8 (&d->clientAddr, buf + strlen (buf), sizeof (buf) - strlen (buf)) &&
	!parseArgs (&args, buf))
	goto argument_failure;
    sprintf (buf, "-connectionType %d", d->connectionType);
    if (!parseArgs (&args, buf))
	goto argument_failure;
    ForEachChooserHost (&d->clientAddr,
			d->connectionType,
			AddChooserHost,
			(char *) &args);
    if (!args)
	goto argument_failure;
    env = systemEnv (d, (char *) 0, (char *) 0);
    if (d->authFile)
            env = setEnv (env, "XAUTHORITY", d->authFile);
    if (d->pmSearchPath)
            env = setEnv(env, "XMICONSEARCHPATH", d->pmSearchPath);
    if (d->bmSearchPath)
            env = setEnv(env, "XMICONBMSEARCHPATH", d->bmSearchPath);
    if ( d->language && strlen(d->language) > 0 )
	    env = setEnv(env, "LANG", d->language);
    if ( d->langList && strlen(d->langList) > 0 )
            env = setEnv(env, LANGLIST,  d->langList);
#if !defined (ENABLE_DYNAMIC_LANGLIST)
    else
            if ( strlen(languageList) > 0 )
                 env = setEnv(env, LANGLIST, languageList);
#endif /* ENABLE_DYNAMIC_LANGLIST */
    if ( d->setup)
            env = setEnv(env, "XSETUP", d->setup);
    if(d->displayType.location == Local)
            env = setEnv (env, LOCATION, "local");
    else
            env = setEnv (env, LOCATION, "remote");


    Debug ("Running %s\n", args[0]);
    execute (args, env);
    Debug ("Couldn't run %s\n", args[0]);
    LogError ((unsigned char *)"Cannot execute %s\n", args[0]);
    exit (REMANAGE_DISPLAY);

argument_failure:
    freeEnv(args);
    exit(REMANAGE_DISPLAY);
}

static char *
Print8Address (
    ARRAY8Ptr   Address)
{
  static char buf[200];
  size_t used = 0;
  int i;

  /* an address is up to 255 bytes: show as many as fit */
  buf[0]='\0';
  for (i = 0; i < (int)Address->length; i++) {
      int n = snprintf(buf + used, sizeof(buf) - used, " %d", Address->data[i]);

      if (n < 0 || (size_t)n >= sizeof(buf) - used)
	  break;
      used += n;
  }
  return(buf);
}
