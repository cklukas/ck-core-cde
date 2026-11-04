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
/*************************************<+>*************************************
 **
 **  File:        SmPriv.h
 **
 **  Project:     DT Session Manager (dtsession)
 **
 **  Description:
 **  -----------
 **  The identity (user and group ids) dtsession runs with.
 **
 **  dtsession is installed with the identity of the user who runs it: not
 **  setuid and not setgid.  Nothing it does needs another identity, and every
 **  program it starts for the user would inherit one.  The one function
 **  here is the guard for an installation that does not follow that rule.
 **
 *************************************<+>*************************************/

#ifndef _SmPriv_h
#define _SmPriv_h

/*
 * Return to the identity of the user who ran the program if it runs with the
 * identity of a setuid or setgid file (the effective user or group id differs
 * from the real one): the real, effective and saved ids all become the user's,
 * and the installed ones are tried once more to make sure that they cannot be
 * taken back.  Must be the first thing main() does: until it has returned the
 * program runs with the identity it is installed with.  Does not return if
 * the identity cannot be given up, or if the way back to it stays open.
 * Does nothing, and calls nothing, when the ids are the same, and when the
 * real user is root, which can take any identity anyway.
 */
extern void SmPrivDropInstalledIdentity(void);

#endif /* _SmPriv_h */
