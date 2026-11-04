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
 **  File:        SmPam.h
 **
 **  Project:     DT Session Manager (dtsession)
 **
 **  Description:
 **  -----------
 **  The password check of the screen lock.
 **
 **  The display is unlocked with the password of the user who started the
 **  session and with no other: not the password of another account, and not
 **  the one of an account named in a dialog, a resource or the environment.
 **  The account is the one the real user ID of the process belongs to.  The
 **  password is checked by PAM (service "dtsession", see config/dtsession.src);
 **  there is no other way, so a dtsession built without PAM cannot lock the
 **  display.
 **
 *************************************<+>*************************************/

#ifndef _SmPam_h
#define _SmPam_h

#include <X11/Intrinsic.h>

/*
 * The name of the account of the user who started the session, in memory that
 * the caller frees; null if there is no such account or no memory.  It is the
 * account whose password unlocks the display, and the lock dialog names it.
 */
extern char *SmPamUserName(void);

/*
 * Why the display can or cannot be locked.  The lock needs a way to unlock it
 * again, and that way is the PAM service "dtsession" for the user who started
 * the session.
 */
typedef enum
{
    SM_PAM_READY,       /* it can be locked */
    SM_PAM_NOT_BUILT,   /* the program was built without PAM */
    SM_PAM_NO_SERVICE,  /* the file of the PAM service is missing or unreadable */
    SM_PAM_NO_ACCOUNT   /* the user who started the session has no account */
} SmPamStatus;

/*
 * Check, without authenticating anyone, that the display can be locked: the
 * program was built with PAM, the file of the PAM service can be read and the
 * user who started it has an entry in the password database.  The checks are
 * made in that order.
 */
extern SmPamStatus SmPamLockStatus(void);

/*
 * The file of the PAM service (config/dtsession.src, installed as
 * pam.d/dtsession), or null in a program built without PAM.
 */
extern const char *SmPamServiceFile(void);

/*
 * Whether the given password is the one of the user who started the session
 * and that user's account may be used.  False for every failure: the
 * account cannot be found, PAM cannot be started, the password is wrong, the
 * account has expired, is locked or is refused by the stack, or the
 * conversation with a module fails.  A password that expired while the user
 * was logged in does not keep the user out; its change is for the next login.
 */
extern Boolean SmPamCheckPassword(const char *password);

#endif /* _SmPam_h */
