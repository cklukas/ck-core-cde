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

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "SmPriv.h"

/*
 * A program that cannot get rid of an identity it does not need must not go
 * on: everything it did from there on, and every program it started for the
 * user, would carry that identity.
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

void
SmPrivDropInstalledIdentity(void)
{
    uid_t user = getuid(), installedUser = geteuid();
    gid_t group = getgid(), installedGroup = getegid();

    /*
     * Root has nothing to give up: it can take any identity at will, and a
     * guard that took one back to prove the opposite would only fail.
     */
    if (user == 0)
    {
        return;
    }

    /*
     * The group first: the installed user may be the one that is allowed to
     * change it.  The call sets the saved id as well, which is what closes the
     * way back.
     */
    if (installedGroup != group)
    {
        if (setregid(group, group) != 0 || getegid() != group)
        {
            Fatal("cannot give up the group of the installed program");
        }
    }

    if (installedUser != user)
    {
        if (setreuid(user, user) != 0 || geteuid() != user)
        {
            Fatal("cannot give up the user of the installed program");
        }
    }

    /*
     * Nothing may lead back.  A saved id that was not changed, or a kernel
     * that reports success without doing anything, would let the installed
     * identity be taken again.
     */
    if (installedGroup != group)
    {
        if (setegid(installedGroup) == 0 || setgid(installedGroup) == 0 ||
            getegid() != group)
        {
            Fatal("the group of the installed program was not given up");
        }
    }

    if (installedUser != user)
    {
        if (seteuid(installedUser) == 0 || setuid(installedUser) == 0 ||
            geteuid() != user)
        {
            Fatal("the user of the installed program was not given up");
        }
    }
}
