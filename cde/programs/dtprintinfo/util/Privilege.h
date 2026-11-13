/*
 * CDE - Common Desktop Environment
 *
 * These libraries and programs are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * These libraries and programs are distributed without any warranty; without
 * even the implied warranty of merchantability or fitness for a particular
 * purpose. See the GNU Lesser General Public License for more details.
 */

/*
 * Changes of the user identity of dtprintinfo.  The program may be installed
 * with the set-user-ID bit; it works as the calling user and takes root only
 * for the operations that need it.
 *
 * The header is plain C so that the functions can be tested on their own.
 */

#ifndef _Privilege_h
#define _Privilege_h

#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Take root as the effective user for an operation that needs it.  Nothing is
 * lost when this fails: the operation then fails by itself.  Returns 0 on
 * success.
 */
static inline int
RaiseToRoot(void)
{
   return seteuid(0);
}

/*
 * Return to working as the calling user.  If the identity cannot be given up,
 * everything that follows would run as root, so the process ends.
 */
static inline void
RestoreUserEuid(void)
{
   if (seteuid(getuid()) != 0 || geteuid() != getuid())
    {
      static const char message[] =
	 "dtprintinfo: cannot give up the effective user ID\n";
      ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);

      (void) written;
      _exit(EXIT_FAILURE);
    }
}

/*
 * Make the real, effective and saved user ID of the process UID.  Returns 0 on
 * success and -1 when the identity could not be changed; the caller must not
 * go on in that case.
 */
static inline int
BecomeUser(uid_t uid)
{
   if (setuid(uid) != 0 || getuid() != uid || geteuid() != uid)
      return -1;
   return 0;
}

#endif /* _Privilege_h */
