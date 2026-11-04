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
 **  dtsession is installed setuid root on systems without PAM, so that it can
 **  read the password database when the screen is unlocked.  Everything it
 **  does besides that runs as the user who started it.  Every change of
 **  identity is checked: a step that cannot be taken or that did not have the
 **  expected effect stops the process (when it gives something up) or is
 **  reported to the caller (when it asks for something).
 **
 *************************************<+>*************************************/

#ifndef _SmPriv_h
#define _SmPriv_h

/*
 * Return to the identity of the user who started the program.  Must be the
 * first thing main() does: until it has returned, the program runs with the
 * identity it is installed with.  If keepPasswordAccess is non-zero and the
 * program is installed setuid, the installed user id stays available as the
 * saved set-user-ID, for SmPrivRaise(); otherwise it is given up for good.
 * Does not return if the identity cannot be given up.
 */
extern void SmPrivDropInstalledIdentity(int keepPasswordAccess);

/*
 * Take the installed identity back for a short operation (reading the
 * password database).  Returns 0 if that identity is not available or could
 * not be taken; the program then still has the user's identity.  Every
 * successful SmPrivRaise() is followed by SmPrivLower().
 */
extern int SmPrivRaise(void);

/*
 * End SmPrivRaise().  Does not return if the user's identity cannot be
 * restored.
 */
extern void SmPrivLower(void);

/*
 * Give up the installed group, and the saved user id if one was kept, in a
 * process that is about to start a program.  Only calls that are safe in a
 * vfork() child are made.  Does not return if the identity cannot be given up.
 */
extern void SmPrivDropInChild(void);

/*
 * Switch the effective group between the user's (non-zero) and the group the
 * program is installed with (zero).  Asking for the installed group returns 0
 * if that failed; going back to the user's group does not return on failure.
 */
extern int SmPrivUseUserGroup(int userGroup);

#endif /* _SmPriv_h */
