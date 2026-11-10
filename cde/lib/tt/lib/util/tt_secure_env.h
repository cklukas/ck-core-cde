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
 * tt_secure_env.h
 *
 * The environment of a process that runs with other privileges than the
 * user who started it.
 *
 * A set-user-ID or set-group-ID program, or a program that has raised its
 * effective ids, acts for its owner, but its environment, current directory
 * and arguments belong to the user who started it.  libtt must not let that
 * user name a program to run, a file to open for writing, a library to load
 * or a directory to read configuration from.
 */

#if !defined(_TT_SECURE_ENV_H)
#define _TT_SECURE_ENV_H

/*
 * Returns 1 when the effective user or group id of the process differs from
 * its real one, 0 otherwise.
 */
int		_tt_privileged_process(void);

/*
 * getenv() for a setting that names a program, a library or a file that
 * libtt acts on.  Returns 0, as if the variable were not set, when the
 * process is privileged.
 */
char	       *_tt_secure_getenv(const char *name);

/*
 * The home directory of the user: $HOME, or, in a privileged process, the
 * home directory of the real user from the password database.  Returns 0
 * when there is none.
 */
const char     *_tt_secure_home(void);

#endif
