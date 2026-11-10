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

#ifndef _MAILBOXFILES_HH
#define _MAILBOXFILES_HH

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// The operations of RFCMailBox on the files around a mailbox that are not
// reads and writes of its descriptor: opening and creating it, the file that
// replaces it in a save, and the lock files beside it.
//
// There are two implementations.  A mailbox in a directory of the user is
// handled with plain system calls.  The mailbox of the user in the mail spool
// directory, which only the mail group can write to, is handled by
// dtmailspool through SpoolHelper; the mailer itself never holds the group.
//
// A status is DTSP_OK, an errno value or one of the DTSP_ codes of
// DtMail/SpoolProtocol.h, and errno is set to what the failure was where a
// function returns -1.
class MailboxFiles {
  public:
    // Which step of making the replacement file failed.
    enum TemporaryStep { TEMP_INFORMATION, TEMP_CREATE, TEMP_MODE };

    virtual ~MailboxFiles() { }

    // The implementation for the mailbox file `path` (a real path, links
    // resolved).
    static MailboxFiles *forMailbox(const char *path);

    // errno for a status, for the messages.
    static int statusErrno(int status);

    // Open the mailbox: open_mode is O_RDONLY, O_RDWR or O_RDWR | O_CREAT.
    // O_RDWR without O_CREAT opens the file read-only when it does not allow
    // writing, and `granted` says which it is.  Returns a descriptor, or -1.
    virtual int open(int open_mode, mode_t create_mode, int *granted) = 0;

    // Make the file that will replace the mailbox, with the mode and the
    // group of the mailbox, whose descriptor is mailbox_fd.  Returns its
    // descriptor, or -1 and says which step failed.
    virtual int createTemporary(int mailbox_fd, TemporaryStep *step) = 0;
    virtual const char *temporaryName(void) const = 0;
    // Rename it over the mailbox: 0, or -1.
    virtual int commitTemporary(void) = 0;
    // Remove it.
    virtual void discardTemporary(void) = 0;

    // The lock files <mailbox>.lock and <mailbox>.dtmail.  See SpoolHelper.hh
    // for the meaning of the results.
    virtual int lockTry(uint32_t kind, const void *content, size_t length,
			uint32_t *age) = 0;
    virtual int lockVerify(uint32_t kind, const void *content, size_t length) = 0;
    virtual int lockRelease(uint32_t kind, const void *content, size_t length) = 0;
    virtual int lockBreak(uint32_t kind) = 0;
};

#endif
