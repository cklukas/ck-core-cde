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

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <DtMail/IO.hh>
#include <DtMail/SpoolHelper.hh>
#include <DtMail/Threads.hh>

// The helper of this installation and the directory it works in are set by
// the build (the helper has the directory built in as well).
#ifndef DTMAIL_SPOOL_HELPER
#error DTMAIL_SPOOL_HELPER is the installed path of dtmailspool and is set by the build
#endif
#ifndef DTMAIL_SPOOL_DIR
#error DTMAIL_SPOOL_DIR is the mail spool directory and is set by the build
#endif

SpoolHelper::SpoolHelper(const char *program, const char *spool_directory,
			 const char *user)
    : _program(strdup(program)),
      _spool_directory(strdup(spool_directory)),
      _user(strdup(user)),
      _mutex(MutexInit()),
      _socket(-1),
      _pid(-1),
      _exit_status(-1)
{
}

SpoolHelper::~SpoolHelper()
{
    stop();
    free(_program);
    free(_spool_directory);
    free(_user);
    MutexDestroy(_mutex);
}

SpoolHelper &
SpoolHelper::shared(void)
{
    static SpoolHelper *helper = NULL;
    static void *guard = MutexInit();	// a local static: initialised once

    MutexLock lock(guard);
    if (helper == NULL) {
	passwd pw;

	GetPasswordEntry(pw);
	helper = new SpoolHelper(DTMAIL_SPOOL_HELPER, DTMAIL_SPOOL_DIR,
				 pw.pw_name);
    }
    return *helper;
}

int
SpoolHelper::statusErrno(int status)
{
    if (status == DTSP_OK)
	return 0;
    if (status > 0 && status < (int) DTSP_BUSY)
	return status;
    switch ((uint32_t) status) {
      case DTSP_BUSY:
	return EEXIST;
      case DTSP_VANISHED:
	return ENOENT;
      case DTSP_REFUSED:
      case DTSP_NOT_OURS:
	return EACCES;
      default:		// including DTSP_UNAVAILABLE: no helper to ask
	return EIO;
    }
}

bool
SpoolHelper::isMailbox(const char *path) const
{
    const char *slash = strrchr(path, '/');
    char directory[4096];
    struct stat in_path, in_spool;
    size_t length;

    // A name the helper would not serve (it stops at its start) is no mailbox
    // of the helper's.
    if (strlen(_user) > DTSP_USER_MAX)
	return false;
    if (slash == NULL || strcmp(slash + 1, _user) != 0)
	return false;
    length = slash == path ? 1 : (size_t)(slash - path);
    if (length >= sizeof directory)
	return false;
    memcpy(directory, path, length);
    directory[length] = '\0';

    if (stat(directory, &in_path) != 0 || stat(_spool_directory, &in_spool) != 0)
	return false;
    return in_path.st_dev == in_spool.st_dev && in_path.st_ino == in_spool.st_ino;
}

char *
SpoolHelper::mailboxPath(void) const
{
    size_t size = strlen(_spool_directory) + 1 + strlen(_user) + 1;
    char *path = (char *) malloc(size);

    if (path != NULL)
	snprintf(path, size, "%s/%s", _spool_directory, _user);
    return path;
}

// Start the helper with a socket as its descriptor DTSP_PROTOCOL_FD and with
// nothing else of ours: no environment, the default signals, and every other
// descriptor of ours closed on exec.  Called with the lock held.
int
SpoolHelper::start(void)
{
    int pair[2];
    int theirs;
    int rc;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    sigset_t none, all;
    char *argv[2];
    char *envp[1];
    pid_t pid;

    reapEarlier();
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
	return errno;

    // The helper's end goes to a descriptor above the one it is given, so
    // that the duplication cannot land on itself.
    theirs = fcntl(pair[1], F_DUPFD_CLOEXEC, 10);
    if (theirs < 0) {
	int error = errno;

	close(pair[0]);
	close(pair[1]);
	return error;
    }
    close(pair[1]);
    if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) != 0) {
	int error = errno;

	close(pair[0]);
	close(theirs);
	return error;
    }
#ifdef SO_NOSIGPIPE
    {
	int on = 1;

	setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    }
#endif

    sigemptyset(&none);
    sigfillset(&all);
    argv[0] = _program;
    argv[1] = NULL;
    envp[0] = NULL;

    rc = posix_spawn_file_actions_init(&actions);
    if (rc == 0) {
	rc = posix_spawnattr_init(&attributes);
	if (rc == 0) {
	    rc = posix_spawn_file_actions_adddup2(&actions, theirs,
						  DTSP_PROTOCOL_FD);
	    if (rc == 0)
		rc = posix_spawnattr_setsigmask(&attributes, &none);
	    if (rc == 0)
		rc = posix_spawnattr_setsigdefault(&attributes, &all);
	    if (rc == 0)
		rc = posix_spawnattr_setflags(&attributes,
					      POSIX_SPAWN_SETSIGMASK
					      | POSIX_SPAWN_SETSIGDEF);
	    if (rc == 0)
		rc = posix_spawn(&pid, _program, &actions, &attributes,
				 argv, envp);
	    posix_spawnattr_destroy(&attributes);
	}
	posix_spawn_file_actions_destroy(&actions);
    }
    close(theirs);
    if (rc != 0) {
	close(pair[0]);
	return rc;
    }

    _socket = pair[0];
    _pid = pid;
    _exit_status = -1;
    return 0;
}

// Collect the status of the helper if it has ended.  True when there is no
// helper left to wait for (it was collected, or somebody else did).
bool
SpoolHelper::reapEarlier(void)
{
    int status;
    pid_t done;

    if (_pid <= 0)
	return true;
    do {
	done = waitpid(_pid, &status, WNOHANG);
    } while (done < 0 && errno == EINTR);
    if (done == _pid) {
	_exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	_pid = -1;
	return true;
    }
    if (done < 0) {		// not our child any more
	_pid = -1;
	return true;
    }
    return false;
}

// Close the connection and reap the helper, which ends when it sees the end of
// the connection.  Called with the lock held.  A helper that has not ended
// after a second is waited for again at the next start and at the end.
void
SpoolHelper::disconnect(void)
{
    if (_socket >= 0) {
	close(_socket);
	_socket = -1;
    }
    for (int tries = 0; tries < 100 && !reapEarlier(); tries++) {
	struct timespec pause = { 0, 10 * 1000 * 1000 };
	nanosleep(&pause, NULL);
    }
}

void
SpoolHelper::stop(void)
{
    MutexLock lock(_mutex);
    disconnect();
}

// One request and its reply.  `reply` holds the reply and, when the request
// returns a file, the descriptor, which the caller owns.  The result is the
// status of the reply, or the reason the helper could not be asked.
int
SpoolHelper::call(uint32_t operation, const void *payload, uint32_t length,
		  bool returns_fd, struct dtsp_frame &reply)
{
    MutexLock lock(_mutex);
    int result;

    reply.fd = -1;
    if (_socket < 0 && start() != 0)
	return DTSP_UNAVAILABLE;

    if (dtsp_send(_socket, operation, payload, length, -1) != 0) {
	disconnect();
	return DTSP_UNAVAILABLE;
    }
    result = dtsp_receive(_socket, &reply, returns_fd);
    if (result != DTSP_RECV_OK) {
	disconnect();
	return DTSP_UNAVAILABLE;
    }

    // A reply is what the protocol allows for the request, or it is the end
    // of this helper.
    bool succeeded = reply.code == DTSP_OK;
    bool ok_shape = true;

    if (returns_fd)
	ok_shape = succeeded ? reply.fd >= 0 : reply.fd < 0;
    else
	ok_shape = reply.fd < 0;
    if (succeeded && operation == DTSP_OP_OPEN && reply.length != 2 * sizeof(uint32_t))
	ok_shape = false;
    if (!succeeded && reply.code != DTSP_BUSY && reply.length != 0)
	ok_shape = false;
    if (reply.code == DTSP_BUSY
	&& (operation != DTSP_OP_LOCK_TRY || reply.length != sizeof(uint32_t)))
	ok_shape = false;
    if (succeeded && operation != DTSP_OP_OPEN && reply.length != 0)
	ok_shape = false;
    if (reply.code == DTSP_BAD_REQUEST)
	ok_shape = false;

    if (!ok_shape) {
	if (reply.fd >= 0) {
	    close(reply.fd);
	    reply.fd = -1;
	}
	disconnect();
	return DTSP_UNAVAILABLE;
    }
    return (int) reply.code;
}

int
SpoolHelper::open(bool write, bool create, int &fd, bool &writable,
		  bool &created)
{
    unsigned char payload[4];
    struct dtsp_frame reply;
    int status;

    dtsp_put32(payload, (write ? DTSP_OPEN_WRITE : 0)
			| (create ? DTSP_OPEN_CREATE : 0));
    fd = -1;
    writable = false;
    created = false;
    status = call(DTSP_OP_OPEN, payload, sizeof payload, true, reply);
    if (status != DTSP_OK)
	return status;

    uint32_t granted = dtsp_get32(reply.payload);
    uint32_t made = dtsp_get32(reply.payload + 4);

    // The helper grants write access only to a caller who asked for it or who
    // made the file.
    if ((granted != DTSP_ACCESS_READ && granted != DTSP_ACCESS_WRITE)
	|| made > 1 || (made == 1 && !create)
	|| (granted == DTSP_ACCESS_WRITE && !write && made == 0)) {
	close(reply.fd);
	MutexLock lock(_mutex);
	disconnect();
	return DTSP_UNAVAILABLE;
    }
    fd = reply.fd;
    writable = granted == DTSP_ACCESS_WRITE;
    created = made == 1;
    return DTSP_OK;
}

int
SpoolHelper::lockCall(uint32_t operation, uint32_t kind, const void *content,
		      size_t length, uint32_t *age)
{
    unsigned char payload[8 + DTSP_MAX_CONTENT];
    struct dtsp_frame reply;
    int status;

    if (length < 1 || length > DTSP_MAX_CONTENT)
	return EINVAL;
    dtsp_put32(payload, kind);
    dtsp_put32(payload + 4, (uint32_t) length);
    memcpy(payload + 8, content, length);
    status = call(operation, payload, (uint32_t)(8 + length), false, reply);
    if (status == DTSP_BUSY && age != NULL)
	*age = dtsp_get32(reply.payload);
    return status;
}

int
SpoolHelper::lockTry(uint32_t kind, const void *content, size_t length,
		     uint32_t &age)
{
    age = 0;
    return lockCall(DTSP_OP_LOCK_TRY, kind, content, length, &age);
}

int
SpoolHelper::lockVerify(uint32_t kind, const void *content, size_t length)
{
    return lockCall(DTSP_OP_LOCK_VERIFY, kind, content, length, NULL);
}

int
SpoolHelper::lockRelease(uint32_t kind, const void *content, size_t length)
{
    return lockCall(DTSP_OP_LOCK_RELEASE, kind, content, length, NULL);
}

int
SpoolHelper::lockBreak(uint32_t kind)
{
    unsigned char payload[4];
    struct dtsp_frame reply;

    dtsp_put32(payload, kind);
    return call(DTSP_OP_LOCK_BREAK, payload, sizeof payload, false, reply);
}

int
SpoolHelper::createTemporary(int &fd)
{
    struct dtsp_frame reply;
    int status = call(DTSP_OP_TEMP_CREATE, NULL, 0, true, reply);

    fd = status == DTSP_OK ? reply.fd : -1;
    return status;
}

int
SpoolHelper::commitTemporary(void)
{
    struct dtsp_frame reply;

    return call(DTSP_OP_TEMP_COMMIT, NULL, 0, false, reply);
}

int
SpoolHelper::discardTemporary(void)
{
    struct dtsp_frame reply;

    return call(DTSP_OP_TEMP_DISCARD, NULL, 0, false, reply);
}
