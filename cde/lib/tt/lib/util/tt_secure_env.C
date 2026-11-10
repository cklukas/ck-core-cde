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
 * tt_secure_env.C
 *
 * See tt_secure_env.h.
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <string.h>
#include "util/tt_secure_env.h"

int
_tt_privileged_process(void)
{
	return (geteuid() != getuid()) || (getegid() != getgid());
}

char *
_tt_secure_getenv(const char *name)
{
	if (name == 0 || _tt_privileged_process()) {
		return 0;
	}
	return getenv(name);
}

const char *
_tt_secure_home(void)
{
	if (! _tt_privileged_process()) {
		return getenv("HOME");
	}
	// the home directory of the user who started the program, not the
	// one the environment names; the entry lives in static storage
	struct passwd *pw = getpwuid(getuid());
	if (pw == 0 || pw->pw_dir == 0 || pw->pw_dir[0] != '/') {
		return 0;
	}
	return pw->pw_dir;
}
