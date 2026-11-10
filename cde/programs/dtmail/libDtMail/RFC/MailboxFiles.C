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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <DtMail/DtMailTypes.h>
#include <DtMail/IO.hh>
#include <DtMail/SpoolHelper.hh>
#include "MailboxFiles.hh"

// ---------------------------------------------------------------------------
// A mailbox in a directory of the user: plain system calls, no group.

namespace {

// <base> followed by six random letters and digits, taken from the system's
// random source so that another user cannot predict the name.  False, with
// errno set, when the name does not fit or no random bytes are available.
bool
randomName(char *name, size_t size, const char *base, const char *middle)
{
    static const char alphabet[] =
	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char random_bytes[6];
    size_t have = 0;
    int fd;

    fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0)
	return false;
    while (have < sizeof(random_bytes)) {
	ssize_t got = ::read(fd, random_bytes + have,
			     sizeof(random_bytes) - have);
	if (got < 0 && errno == EINTR)
	    continue;
	if (got <= 0) {
	    ::close(fd);
	    if (got == 0)
		errno = EIO;
	    return false;
	}
	have += (size_t) got;
    }
    ::close(fd);

    char suffix[sizeof(random_bytes) + 1];
    for (size_t i = 0; i < sizeof(random_bytes); i++)
	suffix[i] = alphabet[random_bytes[i] % (sizeof(alphabet) - 1)];
    suffix[sizeof(random_bytes)] = '\0';

    int length = snprintf(name, size, "%s%s%s", base, middle, suffix);
    if (length < 0 || (size_t) length >= size) {
	errno = ENAMETOOLONG;
	return false;
    }
    return true;
}

// The text of a lock file is compared whole: it is short, and the one who
// made the file knows exactly what it wrote.
const size_t lockTextMax = DTSP_MAX_CONTENT;

class DirectMailboxFiles : public MailboxFiles {
  public:
    explicit DirectMailboxFiles(const char *path) : _path(strdup(path))
    {
	_temp[0] = '\0';
    }
    virtual ~DirectMailboxFiles() { free(_path); }

    virtual int open(int open_mode, mode_t create_mode, int *granted);
    virtual int createTemporary(int mailbox_fd, TemporaryStep *step);
    virtual const char *temporaryName(void) const { return _temp; }
    virtual int commitTemporary(void);
    virtual void discardTemporary(void);
    virtual int lockTry(uint32_t kind, const void *content, size_t length,
			uint32_t *age);
    virtual int lockVerify(uint32_t kind, const void *content, size_t length);
    virtual int lockRelease(uint32_t kind, const void *content, size_t length);
    virtual int lockBreak(uint32_t kind);

  private:
    bool lockName(uint32_t kind, char *name, size_t size) const;
    int  lockCompare(const char *lock, const void *content, size_t length,
		     int *fd);

    char *_path;
    char  _temp[MAXPATHLEN + 1];
};

int
DirectMailboxFiles::open(int open_mode, mode_t create_mode, int *granted)
{
    // the descriptor of a mailbox is not for the programs the mailer starts
#ifdef O_CLOEXEC
    open_mode |= O_CLOEXEC;
#endif
    mode_t old_umask = umask(DTMAIL_DEFAULT_CREATE_UMASK);
    int fd = SafeOpen(_path, open_mode, create_mode);
    int error = errno;

    *granted = open_mode & O_ACCMODE;
    // A mailbox that cannot be written is read, unless it is being made.
    if (fd < 0 && (open_mode & (O_ACCMODE | O_CREAT)) == O_RDWR
	&& (error == EACCES || error == EROFS || error == EPERM)) {
	fd = SafeOpen(_path, (open_mode & ~(O_ACCMODE | O_CREAT)) | O_RDONLY, create_mode);
	error = errno;
	*granted = O_RDONLY;
    }
    (void) umask(old_umask);
    errno = error;
    return fd;
}

int
DirectMailboxFiles::createTemporary(int mailbox_fd, TemporaryStep *step)
{
    struct stat info;
    int fd;

    if (SafeFStat(mailbox_fd, &info) < 0) {
	*step = TEMP_INFORMATION;
	return -1;
    }
    if (snprintf(_temp, sizeof(_temp), "%s.tmp.XXXXXX", _path)
	>= (int) sizeof(_temp)) {
	_temp[0] = '\0';
	errno = ENAMETOOLONG;
	*step = TEMP_CREATE;
	return -1;
    }

    // mkstemp() makes the file exclusively under a name that is not known
    // beforehand, and never opens a file or link that exists.
    fd = mkstemp(_temp);
    if (fd < 0) {
	*step = TEMP_CREATE;
	return -1;
    }
    if (SafeFChmod(fd, info.st_mode & 07777) == -1) {
	int error = errno;

	(void) SafeClose(fd);
	(void) SafeUnlink(_temp);
	errno = error;
	*step = TEMP_MODE;
	return -1;
    }

    // The owner and the group of the mailbox, as far as the user may give
    // them: a user can always give a file the group of a mailbox that the
    // user could open, and this fails harmlessly where it cannot be done
    // (bug 1216914: the save must work for a mailbox the user may write).
    (void) SafeFchown(fd, info.st_uid, (gid_t) -1);
    (void) SafeFchown(fd, (uid_t) -1, info.st_gid);
    return fd;
}

int
DirectMailboxFiles::commitTemporary(void)
{
    return SafeRename(_temp, _path);
}

void
DirectMailboxFiles::discardTemporary(void)
{
    (void) SafeUnlink(_temp);
}

bool
DirectMailboxFiles::lockName(uint32_t kind, char *name, size_t size) const
{
    const char *suffix;

    if (kind == DTSP_LOCK_MAIL)
	suffix = ".lock";
    else if (kind == DTSP_LOCK_DTMAIL)
	suffix = ".dtmail";
    else {
	errno = EINVAL;
	return false;
    }
    int length = snprintf(name, size, "%s%s", _path, suffix);
    if (length < 0 || (size_t) length >= size) {
	errno = ENAMETOOLONG;
	return false;
    }
    return true;
}

int
DirectMailboxFiles::lockTry(uint32_t kind, const void *content, size_t length,
			    uint32_t *age)
{
    char lock[MAXPATHLEN + 1];
    char temp[MAXPATHLEN + 1];
    struct stat made, existing;
    int fd;

#ifndef O_RSYNC
    int flags = O_RDWR | O_CREAT | O_EXCL | O_SYNC;
#else
    int flags = O_RDWR | O_CREAT | O_EXCL | O_SYNC | O_RSYNC;
#endif

    *age = 0;
    if (length < 1 || length > lockTextMax)
	return EINVAL;
    if (!lockName(kind, lock, sizeof(lock)))
	return errno;
    if (!randomName(temp, sizeof(temp), _path, ".lk."))
	return errno;

    // Create a temporary file with the text; the link to the name of the lock
    // is made in one step, so that a lock file never exists without its text.
    fd = SafeOpen(temp, flags, 0600);
    if (fd < 0)
	return errno;
    ssize_t written = SafeWrite(fd, content, length);
    int failure = 0;

    if (written != (ssize_t) length)
	failure = written < 0 ? errno : EIO;
    else if (fsync(fd) == -1 || SafeFStat(fd, &made) == -1)
	failure = errno;
    if (failure != 0) {
	(void) SafeClose(fd);
	(void) SafeRemove(temp);
	return failure;
    }
    if (SafeClose(fd) == -1) {
	int error = errno;

	(void) SafeRemove(temp);
	return error;
    }

    if (SafeLink(temp, lock) == 0) {
	(void) SafeRemove(temp);
	return DTSP_OK;
    }
    int error = errno;
    // A file system that is not local can make the link and report a failure
    // (the answer was lost and the call repeated): the lock is ours when the
    // temporary file has two names now.
    if (SafeLStat(temp, &existing) == 0 && existing.st_nlink == 2) {
	(void) SafeRemove(temp);
	return DTSP_OK;
    }
    (void) SafeRemove(temp);
    if (error != EEXIST)
	return error;

    // The lock is held.  Its age is told by the clock of the file system that
    // holds it, which may be another machine's.
    if (SafeLStat(lock, &existing) == -1)
	return errno == ENOENT ? (int) DTSP_VANISHED : errno;
    if (made.st_ctime > existing.st_ctime)
	*age = (uint32_t)(made.st_ctime - existing.st_ctime);
    return DTSP_BUSY;
}

// Read the lock file and compare it with the text that was written to it.  On
// success the descriptor stays open, so that the release removes the file that
// was compared.
int
DirectMailboxFiles::lockCompare(const char *lock, const void *content,
				size_t length, int *fd)
{
    unsigned char text[DTSP_MAX_CONTENT + 1];
    size_t have = 0;
    struct stat info;
    int f;

#ifndef O_RSYNC
    int flags = O_RDONLY | O_SYNC;
#else
    int flags = O_RDONLY | O_RSYNC | O_SYNC;
#endif

    *fd = -1;
    if (length < 1 || length > lockTextMax)
	return EINVAL;
    f = SafeOpen(lock, flags, 0);
    if (f < 0)
	return errno;
    if (SafeFStat(f, &info) == -1) {
	int error = errno;

	(void) SafeClose(f);
	return error;
    }
    if (!S_ISREG(info.st_mode)) {
	(void) SafeClose(f);
	return DTSP_NOT_OURS;
    }
    while (have < sizeof(text)) {
	ssize_t got = SafeRead(f, text + have, sizeof(text) - have);

	if (got < 0) {
	    int error = errno;

	    (void) SafeClose(f);
	    return error;
	}
	if (got == 0)
	    break;
	have += (size_t) got;
    }
    if (have != length || memcmp(text, content, length) != 0) {
	(void) SafeClose(f);
	return DTSP_NOT_OURS;
    }
    *fd = f;
    return DTSP_OK;
}

int
DirectMailboxFiles::lockVerify(uint32_t kind, const void *content,
			       size_t length)
{
    char lock[MAXPATHLEN + 1];
    int fd, status;

    if (!lockName(kind, lock, sizeof(lock)))
	return errno;
    status = lockCompare(lock, content, length, &fd);
    if (fd >= 0)
	(void) SafeClose(fd);
    return status;
}

int
DirectMailboxFiles::lockRelease(uint32_t kind, const void *content,
				size_t length)
{
    char lock[MAXPATHLEN + 1];
    struct stat opened, named;
    int fd, status;

    if (!lockName(kind, lock, sizeof(lock)))
	return errno;
    status = lockCompare(lock, content, length, &fd);
    if (status != DTSP_OK)
	return status;

    // Remove the file that was compared, not one that took its name.
    if (SafeFStat(fd, &opened) == -1 || SafeLStat(lock, &named) == -1)
	status = errno;
    else if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
	status = DTSP_NOT_OURS;
    else if (SafeUnlink(lock) == -1)
	status = errno;
    (void) SafeClose(fd);
    return status;
}

int
DirectMailboxFiles::lockBreak(uint32_t kind)
{
    char lock[MAXPATHLEN + 1];

    if (!lockName(kind, lock, sizeof(lock)))
	return errno;
    if (SafeRemove(lock) == -1 && errno != ENOENT)
	return errno;
    return DTSP_OK;
}

// ---------------------------------------------------------------------------
// The mailbox of the user in the mail spool directory: dtmailspool does it.

class SpoolMailboxFiles : public MailboxFiles {
  public:
    SpoolMailboxFiles(SpoolHelper &helper, const char *path)
	: _helper(helper)
    {
	// What the messages call the file; the helper chooses the name.
	snprintf(_temp, sizeof(_temp), "%s.tmp.XXXXXX", path);
    }

    virtual int open(int open_mode, mode_t create_mode, int *granted);
    virtual int createTemporary(int mailbox_fd, TemporaryStep *step);
    virtual const char *temporaryName(void) const { return _temp; }
    virtual int commitTemporary(void);
    virtual void discardTemporary(void);
    virtual int lockTry(uint32_t kind, const void *content, size_t length,
			uint32_t *age);
    virtual int lockVerify(uint32_t kind, const void *content, size_t length);
    virtual int lockRelease(uint32_t kind, const void *content, size_t length);
    virtual int lockBreak(uint32_t kind);

  private:
    int result(int status);

    SpoolHelper &_helper;
    char _temp[MAXPATHLEN + 1];
};

// The result of a call that returns a status: a failure sets errno.
int
SpoolMailboxFiles::result(int status)
{
    if (status != DTSP_OK)
	errno = MailboxFiles::statusErrno(status);
    return status;
}

int
SpoolMailboxFiles::open(int open_mode, mode_t, int *granted)
{
    int fd;
    bool writable, created;
    bool write = (open_mode & O_ACCMODE) == O_RDWR;
    bool create = (open_mode & O_CREAT) != 0;
    int status = _helper.open(write, create, fd, writable, created);

    if (result(status) != DTSP_OK)
	return -1;
    *granted = writable ? O_RDWR : O_RDONLY;
    return fd;
}

int
SpoolMailboxFiles::createTemporary(int, TemporaryStep *step)
{
    int fd;
    int status = _helper.createTemporary(fd);

    if (result(status) != DTSP_OK) {
	*step = TEMP_CREATE;
	return -1;
    }
    return fd;
}

int
SpoolMailboxFiles::commitTemporary(void)
{
    return result(_helper.commitTemporary()) == DTSP_OK ? 0 : -1;
}

void
SpoolMailboxFiles::discardTemporary(void)
{
    (void) _helper.discardTemporary();
}

int
SpoolMailboxFiles::lockTry(uint32_t kind, const void *content, size_t length,
			   uint32_t *age)
{
    return result(_helper.lockTry(kind, content, length, *age));
}

int
SpoolMailboxFiles::lockVerify(uint32_t kind, const void *content, size_t length)
{
    return result(_helper.lockVerify(kind, content, length));
}

int
SpoolMailboxFiles::lockRelease(uint32_t kind, const void *content,
			       size_t length)
{
    return result(_helper.lockRelease(kind, content, length));
}

int
SpoolMailboxFiles::lockBreak(uint32_t kind)
{
    return result(_helper.lockBreak(kind));
}

}  // namespace

int
MailboxFiles::statusErrno(int status)
{
    return SpoolHelper::statusErrno(status);
}

MailboxFiles *
MailboxFiles::forMailbox(const char *path)
{
    SpoolHelper &helper = SpoolHelper::shared();

    if (helper.isMailbox(path))
	return new SpoolMailboxFiles(helper, path);
    return new DirectMailboxFiles(path);
}
