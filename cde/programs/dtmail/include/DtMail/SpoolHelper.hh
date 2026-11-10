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

#ifndef _SPOOLHELPER_HH
#define _SPOOLHELPER_HH

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <DtMail/SpoolProtocol.h>

// The client of dtmailspool, the program that holds the mail group so that
// dtmail and dtmailpr do not.
//
// The mailers run with the group of the user.  What the mail group is needed
// for, in a spool directory that only that group can write to, is done by
// the helper for the user's own mailbox file and for nothing else: it opens
// or creates the file and gives the mailer the descriptor, makes the
// replacement file of a save, and creates and removes the lock files.  The
// helper is started when it is first needed and stops when the connection is
// closed.  See DtMail/SpoolProtocol.h for the protocol.
//
// Every function returns a status of the protocol: DTSP_OK, an errno value
// (what the failed call of the helper left) or one of the other DTSP_ codes.
// statusErrno() turns a status into an errno value for the messages.  A
// helper that cannot be started, that dies or that answers in a way the
// protocol does not allow fails the call with DTSP_UNAVAILABLE and is not
// used again; the next call starts a new one.  Calls are serialised with the mutex of DtMail/Threads.hh,
// which is a lock where the mailer is built with threads.
class SpoolHelper {
  public:
    // The helper `program` and the spool directory it works in (the helper
    // has its own compile-time copy of the directory; this one is for
    // recognising the user's mailbox) for the user named `user`.
    SpoolHelper(const char *program, const char *spool_directory,
		const char *user);
    ~SpoolHelper();

    // The helper of this installation, for the user who runs the program.
    static SpoolHelper &shared(void);

    // Is `path` the mailbox of the user in the spool directory: a file of the
    // name of the user in the directory?  The directory is compared by
    // identity, so a path through a link to it is recognised.
    bool isMailbox(const char *path) const;

    // The path of the mailbox of the user: the name of the user in the spool
    // directory.  Allocated with malloc().
    char *mailboxPath(void) const;

    // Open the mailbox.  write asks for read and write when the file allows
    // it; writable says what was granted.  create makes the file when it does
    // not exist.
    int open(bool write, bool create, int &fd, bool &writable, bool &created);

    // The lock file `kind` (DTSP_LOCK_MAIL, DTSP_LOCK_DTMAIL) with the text
    // `content`.  lockTry() returns DTSP_OK, DTSP_BUSY (the file exists, and
    // `age` says how many seconds older than the one just made it is, by the
    // clock of the file system) or DTSP_VANISHED (it was gone when it was
    // looked at).  lockVerify() and lockRelease() return DTSP_NOT_OURS when
    // the file has other contents; the release removes the file only when
    // the contents are the text.  lockBreak() removes the file whoever made
    // it.
    int lockTry(uint32_t kind, const void *content, size_t length,
		uint32_t &age);
    int lockVerify(uint32_t kind, const void *content, size_t length);
    int lockRelease(uint32_t kind, const void *content, size_t length);
    int lockBreak(uint32_t kind);

    // The file that replaces the mailbox in a save: created with the mode and
    // group of the mailbox, renamed over it or removed.  One at a time.
    int createTemporary(int &fd);
    int commitTemporary(void);
    int discardTemporary(void);

    // Close the connection and wait for the helper to end; the helper removes
    // a temporary file that is left.  The exit status of the last helper, or
    // -1 when it did not end normally or none ran.
    void stop(void);
    int  lastExitStatus(void) const { return _exit_status; }

    // The errno value that tells of a status.
    static int statusErrno(int status);

  private:
    SpoolHelper(const SpoolHelper &);
    SpoolHelper &operator=(const SpoolHelper &);

    int start(void);
    bool reapEarlier(void);
    void disconnect(void);
    int call(uint32_t operation, const void *payload, uint32_t length,
	     bool returns_fd, struct dtsp_frame &reply);
    int lockCall(uint32_t operation, uint32_t kind, const void *content,
		 size_t length, uint32_t *age);

    char	*_program;
    char	*_spool_directory;
    char	*_user;
    void	*_mutex;
    int		 _socket;
    pid_t	 _pid;
    int		 _exit_status;
};

#endif
