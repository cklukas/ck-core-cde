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
 **  File:        SmPriv.c
 **
 **  Project:     DT Session Manager (dtsession)
 **
 **  Description:
 **  -----------
 **  The identity dtsession runs with.  See SmPriv.h.
 **
 *************************************<+>*************************************/

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "SmPriv.h"

/*
 * The identity the program was started with.  The user is the one who ran it;
 * the installed identity is what the file system gives the program (setuid
 * root without PAM, setgid for the console-management group on some systems).
 */
static uid_t userUid, installedUid;
static gid_t userGid, installedGid;

/* Non-zero while the installed user id is kept as the saved set-user-ID. */
static int passwordAccess;

/*
 * A program that cannot get rid of an identity it does not need must not go
 * on: everything it did from there on, and every program it started for the
 * user, would carry that identity.  Only write() and _exit() are used, so the
 * function can be called in a vfork() child.
 */
static void
Fatal(const char *message)
{
    static const char prefix[] = "dtsession: ";
    ssize_t n;

    n = write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    n = write(STDERR_FILENO, message, strlen(message));
    n = write(STDERR_FILENO, "\n", 1);
    (void) n;
    _exit(EXIT_FAILURE);
}

/*
 * Make the real and the effective group the given ones.  The effective group
 * is what grants access, so it is always checked; the real group, which only
 * matters because it can be swapped back, is checked where the call sets it.
 */
static int
SetGroups(gid_t real, gid_t effective)
{
#ifndef SVR4
    if (setregid(real, effective) != 0)
        return 0;
    return getgid() == real && getegid() == effective;
#else
    if (setgid(real) != 0 || setegid(effective) != 0)
        return 0;
    return getegid() == effective;
#endif
}

static int
SetUserIds(uid_t user)
{
#ifndef SVR4
    return setreuid(user, user) == 0;
#else
    return setuid(user) == 0 && seteuid(user) == 0;
#endif
}

void
SmPrivDropInstalledIdentity(int keepPasswordAccess)
{
    userUid = getuid();
    installedUid = geteuid();
    userGid = getgid();
    installedGid = getegid();
    passwordAccess = 0;

    /* A program that was not installed setuid has nothing to give up. */
    if (installedUid != userUid && userUid != 0) {
        if (keepPasswordAccess) {
            /* real and effective id are the user's, the saved id stays */
            if (seteuid(userUid) != 0 || getuid() != userUid ||
                geteuid() != userUid)
                Fatal("cannot give up root privileges");
            passwordAccess = 1;
        } else {
            if (!SetUserIds(userUid) || getuid() != userUid ||
                geteuid() != userUid)
                Fatal("cannot give up root privileges");
            /* ...and there must be no way back */
            if (seteuid(installedUid) == 0 || setuid(installedUid) == 0 ||
                geteuid() != userUid)
                Fatal("root privileges were not given up");
        }
    }

    /*
     * The installed group is kept as the real group (so that it can be taken
     * back for console management) and the user's group becomes effective.
     */
    if (installedGid != userGid && !SetGroups(installedGid, userGid))
        Fatal("cannot give up the group of the installed program");
}

int
SmPrivRaise(void)
{
    if (!passwordAccess)
        return 0;
    if (seteuid(installedUid) != 0)
        return 0;
    if (geteuid() != installedUid) {
        /* the kernel reported success but nothing changed */
        SmPrivLower();
        return 0;
    }
    return 1;
}

void
SmPrivLower(void)
{
    if (!passwordAccess)
        return;
    if (seteuid(userUid) != 0 || geteuid() != userUid)
        Fatal("cannot give up root privileges");
}

void
SmPrivDropInChild(void)
{
    /* No static state is written here: a vfork() child shares it with its parent. */
    if (passwordAccess) {
        /*
         * The saved set-user-ID goes too: this process is about to exec.  The
         * installed identity is taken back first because only then does the
         * change reach the saved id on every kind of system.
         */
        if (seteuid(installedUid) != 0 || geteuid() != installedUid ||
            !SetUserIds(userUid) || getuid() != userUid ||
            geteuid() != userUid || seteuid(installedUid) == 0 ||
            geteuid() != userUid)
            Fatal("cannot give up root privileges");
    }
    if (!SetGroups(userGid, userGid))
        Fatal("cannot give up the group of the installed program");
}

int
SmPrivUseUserGroup(int userGroup)
{
    if (installedGid == userGid)
        return 1;
    if (userGroup) {
        if (!SetGroups(installedGid, userGid))
            Fatal("cannot give up the group of the installed program");
        return 1;
    }
    return SetGroups(userGid, installedGid);
}
