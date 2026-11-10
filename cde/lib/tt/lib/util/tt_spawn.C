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
 * tt_spawn.C
 *
 * See tt_spawn.h.
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "util/tt_spawn.h"
#include "util/tt_secure_env.h"

static int
is_blank(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

_Tt_argv::
_Tt_argv() : _vector(0), _count(0), _room(0)
{
}

_Tt_argv::
~_Tt_argv()
{
	for (int i = 0; i < _count; i++) {
		free(_vector[i]);
	}
	free(_vector);
}

bool_t _Tt_argv::
add(const char *arg)
{
	if (arg == 0) {
		arg = "";
	}
	if (_count + 2 > _room) {
		int	room = (_room == 0) ? 8 : 2 * _room;
		char  **vector = (char **)realloc(_vector,
						  room * sizeof(char *));
		if (vector == 0) {
			return FALSE;
		}
		_vector = vector;
		_room = room;
	}
	char *copy = strdup(arg);
	if (copy == 0) {
		return FALSE;
	}
	_vector[_count++] = copy;
	_vector[_count] = 0;
	return TRUE;
}

bool_t _Tt_argv::
add_words(const char *words)
{
	const char *p = words;

	while (p != 0 && *p != '\0') {
		while (is_blank(*p)) {
			p++;
		}
		const char *start = p;
		while (*p != '\0' && ! is_blank(*p)) {
			p++;
		}
		if (p == start) {
			break;
		}
		char *word = (char *)malloc((size_t)(p - start) + 1);
		if (word == 0) {
			return FALSE;
		}
		memcpy(word, start, (size_t)(p - start));
		word[p - start] = '\0';
		bool_t added = add(word);
		free(word);
		if (! added) {
			return FALSE;
		}
	}
	return TRUE;
}

_Tt_string _Tt_argv::
text() const
{
	_Tt_string	joined;

	for (int i = 0; i < _count; i++) {
		if (i > 0) {
			joined = joined.cat(" ");
		}
		joined = joined.cat(_vector[i]);
	}
	return joined;
}

// Gives up the privileges of a set-user-ID or set-group-ID program in the
// calling process for good.  Returns 0 on success.
static int
drop_privileges(void)
{
	gid_t	rgid = getgid();
	uid_t	ruid = getuid();

	// the group first: it cannot be changed once the user is given up
	if (setregid(rgid, rgid) != 0 || setreuid(ruid, ruid) != 0) {
		return -1;
	}
	if (getegid() != rgid || geteuid() != ruid) {
		return -1;
	}
	// the privileges must not be recoverable
	if (ruid != 0 && setuid(0) == 0) {
		return -1;
	}
	return 0;
}

// The code of a child that is to execute ARGV: never returns.
static void
exec_child(int privileged, const char *dir, const _Tt_argv &argv,
	   int in_fd, int out_fd)
{
	if (privileged && drop_privileges() != 0) {
		_exit(127);
	}
	if (in_fd >= 0 && dup2(in_fd, 0) < 0) {
		_exit(127);
	}
	if (out_fd >= 0 && dup2(out_fd, 1) < 0) {
		_exit(127);
	}
	if (dir != 0 && chdir(dir) != 0) {
		_exit(127);
	}
	if (argv.count() > 0) {
		execvp(argv.vector()[0], argv.vector());
	}
	_exit(127);
}

// system()'s handling of the signals while the children are waited for.
struct signal_state {
	struct sigaction	intr, quit, chld;
};

static void
block_signals(signal_state &old)
{
	struct sigaction ignore, dfl;

	memset(&ignore, 0, sizeof(ignore));
	ignore.sa_handler = SIG_IGN;
	sigemptyset(&ignore.sa_mask);
	dfl = ignore;
	dfl.sa_handler = SIG_DFL;
	sigaction(SIGINT, &ignore, &old.intr);
	sigaction(SIGQUIT, &ignore, &old.quit);
	sigaction(SIGCHLD, &dfl, &old.chld);
}

static void
restore_signals(const signal_state &old)
{
	sigaction(SIGINT, &old.intr, 0);
	sigaction(SIGQUIT, &old.quit, 0);
	sigaction(SIGCHLD, &old.chld, 0);
}

static pid_t
wait_for(pid_t pid, int *status)
{
	pid_t	w;

	do {
		w = waitpid(pid, status, 0);
	} while (w == -1 && errno == EINTR);
	return w;
}

int
_tt_run(const _Tt_argv &argv, const char *dir)
{
	if (argv.count() == 0) {
		return -1;
	}
	int		privileged = _tt_privileged_process();
	signal_state	old;

	block_signals(old);
	int	status = 0;
	pid_t	pid = fork();
	if (pid == 0) {
		restore_signals(old);
		exec_child(privileged, dir, argv, -1, -1);
	}
	pid_t	w = (pid > 0) ? wait_for(pid, &status) : -1;
	restore_signals(old);
	return (w == -1) ? -1 : status;
}

int
_tt_run_pipeline(const char *dir1, const _Tt_argv &first,
		 const char *dir2, const _Tt_argv &second)
{
	if (first.count() == 0 || second.count() == 0) {
		return -1;
	}
	int		privileged = _tt_privileged_process();
	int		fds[2];

	if (pipe(fds) != 0) {
		return -1;
	}
	signal_state	old;
	block_signals(old);

	pid_t	pid1 = fork();
	if (pid1 == 0) {
		restore_signals(old);
		close(fds[0]);
		exec_child(privileged, dir1, first, -1, fds[1]);
	}
	pid_t	pid2 = (pid1 > 0) ? fork() : -1;
	if (pid2 == 0) {
		restore_signals(old);
		close(fds[1]);
		exec_child(privileged, dir2, second, fds[0], -1);
	}
	close(fds[0]);
	close(fds[1]);

	int	status1 = 0, status2 = 0;
	pid_t	w1 = (pid1 > 0) ? wait_for(pid1, &status1) : -1;
	pid_t	w2 = (pid2 > 0) ? wait_for(pid2, &status2) : -1;
	restore_signals(old);

	if (w1 == -1 || w2 == -1) {
		return -1;
	}
	return (status2 != 0 || status1 == 0) ? status2 : status1;
}

FILE *
_tt_popen_read(const _Tt_argv &argv, int *pid)
{
	int	fds[2];

	if (argv.count() == 0 || pipe(fds) != 0) {
		return 0;
	}
	int	privileged = _tt_privileged_process();
	pid_t	child = fork();
	if (child == 0) {
		close(fds[0]);
		exec_child(privileged, 0, argv, -1, fds[1]);
	}
	close(fds[1]);
	FILE   *stream = (child > 0) ? fdopen(fds[0], "r") : 0;
	if (stream == 0) {
		close(fds[0]);
		if (child > 0) {
			int	status;
			kill(child, SIGTERM);
			wait_for(child, &status);
		}
		return 0;
	}
	*pid = (int)child;
	return stream;
}

int
_tt_pclose_read(FILE *stream, int pid)
{
	int	status = 0;

	fclose(stream);
	return (wait_for((pid_t)pid, &status) == -1) ? -1 : status;
}
