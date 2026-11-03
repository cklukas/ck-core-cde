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
/*
 * File:         sbstdinc.h $TOG: sbstdinc.h /main/7 1998/07/31 17:50:45 mgreess $
 * Language:     C
 *
 * (c) Copyright 1988, Hewlett-Packard Company, all rights reserved.
 *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company			*
 * (c) Copyright 1993, 1994 International Business Machines Corp.	*
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.			*
 * (c) Copyright 1993, 1994 Novell, Inc.				*
 */

#ifndef _sbstdinc_h
#define _sbstdinc_h

#include <sys/types.h>
#include <sys/param.h>

#ifdef SVR4
#include <netdb.h>		/* MAXHOSTNAMELEN */
#endif /* SVR4 */

#ifndef howmany
#define howmany(x, y)  (((x)+((y)-1))/(y))   /* From <sys/param.h>, but not an XPG3 file */
#endif

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>
#include <Dt/MsgCatP.h>

/************************************************************************/
/* Routines not defined in include files (yet).				*/
/************************************************************************/

/* BSD has bzero(), bcmp(), and bcopy() defined. */
#if !defined(__bsd) && !defined(CSRG_BASED)

#if !defined(__linux__) && !defined(_XFUNCS_H_) && !defined(sun)
extern void bcopy(const void *source, void *destination, size_t length);
extern int bcmp(const void *b1, const void *b2, size_t length);
extern void bzero(void *b, size_t length);
#endif

extern char *mktemp(char *tmplate);

#endif


#ifndef  SBSTDINC_H_NO_REDEFINE   /* sbstdinc.c turns this on */

/************************************************************************/
/* Routines from <string.h> 						*/
/*  --- These always get redefined so we can catch null ptr deref's     */
/************************************************************************/

extern XeString Xestrcat(XeString s1, ConstXeString s2);
#ifdef strcat
# undef strcat
#endif
#define strcat Xestrcat

extern XeString Xestrncat(XeString s1, ConstXeString s2, size_t n);
#ifdef strncat
# undef strncat
#endif
#define strncat Xestrncat

extern int Xestrcmp(ConstXeString s1, ConstXeString s2);
#ifdef strcmp
# undef strcmp
#endif
#define strcmp Xestrcmp

extern int Xestrncmp(ConstXeString s1, ConstXeString s2, size_t n);
#ifdef strncmp
# undef strncmp
#endif
#define strncmp Xestrncmp

extern XeString Xestrcpy(XeString s1, ConstXeString s2);
#ifdef strcpy
# undef strcpy
#endif
#define strcpy Xestrcpy

extern XeString Xestrncpy(XeString s1, ConstXeString s2, size_t n);
#ifdef strncpy
# undef strncpy
#endif
#define strncpy Xestrncpy

extern int Xestrcoll(ConstXeString s1, ConstXeString s2);
#ifdef strcoll
# undef strcoll
#endif
#define strcoll Xestrcoll

extern size_t Xestrxfrm(XeString s1, ConstXeString s2, size_t n);
#ifdef strxfrm
# undef strxfrm
#endif
#define strxfrm Xestrxfrm

extern XeString Xestrchr(ConstXeString s, int c);
#ifdef strchr
# undef strchr
#endif
#define strchr Xestrchr

extern XeString Xestrpbrk(ConstXeString s1, ConstXeString s2);
#ifdef strpbrk
# undef strpbrk
#endif
#define strpbrk Xestrpbrk

extern XeString Xestrrchr(ConstXeString s, int c);
#ifdef strrchr
# undef strrchr
#endif
#define strrchr Xestrrchr

extern XeString Xestrstr(ConstXeString s1, ConstXeString s2);
#ifdef strstr
# undef strstr
#endif
#define strstr Xestrstr

extern XeString Xestrtok(XeString s1, ConstXeString s2);
#ifdef strtok
# undef strtok
#endif
#define strtok Xestrtok

extern size_t Xestrlen(ConstXeString s);
#ifdef strlen
# undef strlen
#endif
#define strlen Xestrlen

extern XeString Xestrdup(ConstXeString s);
#ifdef strdup
# undef strdup
#endif
#define strdup Xestrdup

#endif /* ifndef  SBSTDINC_H_NO_REDEFINE */

#endif /*  _sbstdinc_h */ 
