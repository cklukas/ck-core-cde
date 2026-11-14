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

/* $XConsortium: lutil.c /main/4 1996/11/21 20:00:35 drk $ */
/*
 *  (c) Copyright 1993, 1994 Hewlett-Packard Company
 *  (c) Copyright 1993, 1994 International Business Machines Corp.
 *  (c) Copyright 1993, 1994 Novell, Inc.
 *  (c) Copyright 1993, 1994 Sun Microsystems, Inc.
 */

#include <sys/param.h>
#include <EUSCompat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rpc/rpc.h>
#include <unistd.h>
#if defined(sun)
#include <netdb.h>
#include <sys/systeminfo.h>
#endif
#define X_INCLUDE_PWD_H
#define XOS_USE_XT_LOCKING
#if defined(__linux__)
#undef SVR4
#endif
#include <X11/Xos_r.h>
#if defined(__linux__)
#define SVR4
#endif

#include "lutil.h"

#if !defined(__linux__)
extern char * strdup(const char *);
#endif

extern char *
_DtCmGetPrefix(char *str, char sep)
{
        size_t len;
        char *prefix;

        if (str == nullptr)
                return(nullptr);

        /* the string comes from the network, so it has no length limit */
        for (len = 0; str[len] != '\0' && str[len] != sep; len++)
                ;
        if (len == 0)
                return(nullptr);

        if ((prefix = malloc(len + 1)) == nullptr)
                return(nullptr);
        memcpy(prefix, str, len);
        prefix[len] = '\0';
        return(prefix);
}

extern char *
_DtCmGetLocalHost(void)
{
	static char host[MAXHOSTNAMELEN+1];
	static boolean_t known = B_FALSE;

	if (!known) {
		known = B_TRUE;
		host[0] = '\0';
#if defined(sun)
		(void)sysinfo(SI_HOSTNAME, host, MAXHOSTNAMELEN);
#else
		(void)gethostname(host, MAXHOSTNAMELEN);
#endif /* sun */
	}

	return (host);
}

extern char *
_DtCmGetLocalDomain(char *hostname)
{
	static char	*domain = nullptr;
	char		buf[BUFSIZ], *ptr;
	CLIENT		*cl;

	if (domain == nullptr) {
		domain = (char *)calloc(BUFSIZ, 1);
		if (domain == nullptr)
			return (nullptr);
#if defined(sun)
		sysinfo(SI_SRPC_DOMAIN, domain, BUFSIZ - 1);
#else
		if (getdomainname(domain, BUFSIZ - 1) != 0)
			domain[0] = '\0';	/* no domain name known */
#endif /* sun */

		/* check domain name */
		/* this is a hack to find out the domain name that
		 * is acceptable to the rpc interface, e.g.
		 * DGDO.Eng.Sun.COM is returned by sysinfo but
		 * this name is not acceptable to the rpc interface
		 * hence we need to stripe out the first component
		 */
		ptr = domain;
		if (hostname == nullptr) hostname = _DtCmGetLocalHost();
		while (1) {
			snprintf(buf, sizeof buf, "%s.%s", hostname, ptr);
			if ((cl = clnt_create(buf, 100068, 5, "udp")) == nullptr) {
				ptr = strchr(ptr, '.');
				if (ptr)
					ptr++;
				else
					break;
			} else {
				clnt_destroy(cl);
				break;
			}
		}
		if (ptr && ptr != domain)
			domain = ptr;
	}

	return (domain);
}

extern char *
_DtCmGetHostAtDomain(void)
{
	static char	hostname[BUFSIZ];
	static boolean_t known = B_FALSE;
	char		*host;

	if (!known) {
		known = B_TRUE;

		host = _DtCmGetLocalHost();
		if (strchr(host, '.') == nullptr)
			snprintf(hostname, BUFSIZ, "%s.%s", host,
				_DtCmGetLocalDomain(host));
		else
			snprintf(hostname, BUFSIZ, "%s", host);
	}

	return (hostname);
}

extern char *
_DtCmGetUserName(void)
{
        static char name[BUFSIZ];
	static boolean_t known = B_FALSE;
	_Xgetpwparams	pwd_buf;
	struct passwd *	pwd_ret;

        if (!known) {
	  known = B_TRUE;

	  if ((pwd_ret = _XGetpwuid(geteuid(), pwd_buf)) == nullptr)
	    snprintf(name, sizeof name, "%s", "nobody");
	  else
	    snprintf(name, sizeof name, "%s", pwd_ret->pw_name);
        }

	return name;
}

/*
 * this routine checks whether the given name is a valid user name
 */
extern boolean_t
_DtCmIsUserName(char *user)
{
	_Xgetpwparams	pwd_buf;
	struct passwd *	pwd_ret;

	pwd_ret = _XGetpwnam(user, pwd_buf);
	if (pwd_ret == nullptr)
		return (B_FALSE);
	else
		return (B_TRUE);
}

