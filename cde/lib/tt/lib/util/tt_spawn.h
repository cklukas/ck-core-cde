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
 * tt_spawn.h
 *
 * Running programs without a shell.
 *
 * A command line that is assembled as a string and given to system() or
 * popen() is read by a shell, so a file name, a display name or a setting
 * from the environment that contains a blank or a shell metacharacter changes
 * what runs.  The routines here take the program and each of its arguments as
 * one unit and execute them directly.
 */

#if !defined(_TT_SPAWN_H)
#define _TT_SPAWN_H

#include "util/tt_string.h"

/*
 * A program and its arguments, as the null-terminated vector execvp() takes.
 */
class _Tt_argv {
    public:
	_Tt_argv();
	~_Tt_argv();

	// Appends a copy of ARG.  Returns FALSE, and leaves the vector as it
	// was, when memory is short.
	bool_t		add(const char *arg);

	// Appends the words of WORDS, which are separated by white space.
	// No quoting, escaping or expansion is done.  Returns FALSE when
	// memory is short; some of the words may have been added by then.
	bool_t		add_words(const char *words);

	int		count() const {return _count;}
	char * const   *vector() const {return _vector;}

	// The arguments joined by blanks, for messages.
	_Tt_string	text() const;

    private:
	_Tt_argv(const _Tt_argv &);
	_Tt_argv	&operator=(const _Tt_argv &);

	char	      **_vector;
	int		_count;
	int		_room;
};

/*
 * Runs ARGV (searching PATH for its first element) in a child process, after
 * changing to directory DIR if DIR is not null, and waits for it.  SIGINT and
 * SIGQUIT are ignored while it runs, as system() does.  Returns the wait
 * status, which is 127 << 8 when the program could not be executed, or -1
 * when the child could not be created or waited for.
 *
 * A child of a privileged process (see _tt_privileged_process()) gives up the
 * extra privileges for good before it changes directory or executes anything
 * and exits with status 127 when it cannot.
 */
int		_tt_run(const _Tt_argv &argv, const char *dir = 0);

/*
 * Runs FIRST and SECOND, each after changing to its directory when that is not
 * null, with the standard output of FIRST connected to the standard input of
 * SECOND, and waits for both.  Returns the wait status of SECOND, or, when
 * that succeeded, the wait status of FIRST if it did not; -1 when a child
 * could not be created or waited for.
 */
int		_tt_run_pipeline(const char *dir1, const _Tt_argv &first,
				 const char *dir2, const _Tt_argv &second);

/*
 * Runs ARGV with its standard output connected to the stream that is
 * returned, as popen(command, "r") does for a shell command.  PID receives
 * the process id for _tt_pclose_read().  Returns 0 when the program could not
 * be started.
 */
FILE	       *_tt_popen_read(const _Tt_argv &argv, int *pid);

/*
 * Closes a stream from _tt_popen_read() and waits for the program.  Returns
 * its wait status, or -1.
 */
int		_tt_pclose_read(FILE *stream, int pid);

#endif
