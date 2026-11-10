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

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <DtMail/SpoolProtocol.h>
#include "spool.h"

#ifndef O_NOFOLLOW
#error the helper must not follow links: this system has no O_NOFOLLOW
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0	/* the client sets FD_CLOEXEC on what it receives */
#endif

/*
 * The flags of every open here: never follow a link at the last component,
 * never become the controlling terminal of the helper, never block on a
 * FIFO or a device that is found under the name of a mailbox.
 */
#define OPEN_COMMON	(O_NOFOLLOW | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)

int
dtsp_valid_user_name(const char *name)
{
    size_t i, length = strlen(name);

    if (length == 0 || length > DTSP_USER_MAX || name[0] == '.')
	return 0;
    for (i = 0; i < length; i++) {
	unsigned char c = (unsigned char) name[i];

	if (c == '/' || c < 0x20 || c == 0x7f)
	    return 0;
    }
    return 1;
}

int
dtsp_spool_init(struct dtsp_spool *spool, int dirfd, uid_t uid, gid_t mail_gid,
		const char *name)
{
    memset(spool, 0, sizeof *spool);
    spool->dirfd = -1;
    if (!dtsp_valid_user_name(name)) {
	errno = EINVAL;
	return -1;
    }
    spool->dirfd = dirfd;
    spool->uid = uid;
    spool->mail_gid = mail_gid;
    memcpy(spool->name, name, strlen(name) + 1);
    spool->lock_free[0] = spool->lock_free[1] = 1;
    return 0;
}

int
dtsp_random_suffix(char suffix[DTSP_SUFFIX_LENGTH + 1])
{
    static const char alphabet[] =
	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char bytes[DTSP_SUFFIX_LENGTH];
    size_t have = 0, i;
    int fd = open("/dev/urandom", O_RDONLY | O_NOCTTY | O_CLOEXEC);

    if (fd < 0)
	return -1;
    while (have < sizeof bytes) {
	ssize_t n = read(fd, bytes + have, sizeof bytes - have);

	if (n < 0 && errno == EINTR)
	    continue;
	if (n <= 0) {
	    (void) close(fd);
	    return -1;
	}
	have += (size_t) n;
    }
    (void) close(fd);
    for (i = 0; i < sizeof bytes; i++)
	suffix[i] = alphabet[bytes[i] % (sizeof alphabet - 1)];
    suffix[DTSP_SUFFIX_LENGTH] = '\0';
    return 0;
}

/* The status of a failed call: the errno value, or REFUSED for a link. */
static uint32_t
failure(int error)
{
    if (error == ELOOP || error == EMLINK)
	return DTSP_REFUSED;
    return error > 0 ? (uint32_t) error : (uint32_t) EIO;
}

/*
 * The name of the lock file of a kind.  A status when there is none: a kind
 * that does not exist is a bad request; a name that is the login name of
 * another user is the mailbox of that user, not a lock file.
 */
static uint32_t
lock_name(const struct dtsp_spool *spool, uint32_t kind,
	  char name[DTSP_LOCKNAME_MAX])
{
    const char *suffix;

    if (kind == DTSP_LOCK_MAIL)
	suffix = ".lock";
    else if (kind == DTSP_LOCK_DTMAIL)
	suffix = ".dtmail";
    else
	return DTSP_BAD_REQUEST;
    if (!spool->lock_free[kind - 1])
	return DTSP_REFUSED;
    /* the name is at most DTSP_USER_MAX bytes: this always fits */
    (void) snprintf(name, DTSP_LOCKNAME_MAX, "%s%s", spool->name, suffix);
    return DTSP_OK;
}

/* The descriptor is a regular file of the user. */
static uint32_t
check_file(const struct dtsp_spool *spool, int fd)
{
    struct stat sb;

    if (fstat(fd, &sb) != 0)
	return failure(errno);
    if (S_ISDIR(sb.st_mode))
	return EISDIR;
    if (!S_ISREG(sb.st_mode) || sb.st_uid != spool->uid)
	return DTSP_REFUSED;
    return DTSP_OK;
}

static int
clear_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags < 0)
	return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

uint32_t
dtsp_open(struct dtsp_spool *spool, uint32_t flags, int *fd,
	  uint32_t *access, uint32_t *created)
{
    int f = -1, attempt;
    uint32_t status;

    *fd = -1;
    *access = DTSP_ACCESS_READ;
    *created = 0;

    /*
     * Open the mailbox; when it is missing and the caller asked for it, make
     * it.  A delivery agent can make it between the two steps, so a mailbox
     * that exists after all is opened.
     */
    for (attempt = 0; attempt < 3; attempt++) {
	if (flags & DTSP_OPEN_WRITE) {
	    f = openat(spool->dirfd, spool->name, O_RDWR | OPEN_COMMON);
	    if (f >= 0)
		*access = DTSP_ACCESS_WRITE;
	    else if (!(flags & DTSP_OPEN_CREATE)
		     && (errno == EACCES || errno == EROFS || errno == EPERM))
		f = openat(spool->dirfd, spool->name, O_RDONLY | OPEN_COMMON);
	} else
	    f = openat(spool->dirfd, spool->name, O_RDONLY | OPEN_COMMON);
	if (f >= 0 || errno != ENOENT || !(flags & DTSP_OPEN_CREATE))
	    break;

	f = openat(spool->dirfd, spool->name,
		   O_RDWR | O_CREAT | O_EXCL | OPEN_COMMON, DTSP_MAILBOX_MODE);
	if (f >= 0) {
	    *created = 1;
	    *access = DTSP_ACCESS_WRITE;
	    if (fchown(f, (uid_t) -1, spool->mail_gid) != 0
		|| fchmod(f, DTSP_MAILBOX_MODE) != 0) {
		int error = errno;

		(void) close(f);
		(void) unlinkat(spool->dirfd, spool->name, 0);
		return failure(error);
	    }
	    break;
	}
	if (errno != EEXIST)
	    break;
    }
    if (f < 0)
	return failure(errno);

    status = check_file(spool, f);
    if (status == DTSP_OK && clear_nonblock(f) != 0)
	status = failure(errno);
    if (status != DTSP_OK) {
	(void) close(f);
	if (*created)
	    (void) unlinkat(spool->dirfd, spool->name, 0);
	*created = 0;
	return status;
    }
    *fd = f;
    return DTSP_OK;
}

/* Write all of a buffer. */
static int
write_all(int fd, const unsigned char *data, uint32_t length)
{
    uint32_t done = 0;

    while (done < length) {
	ssize_t n = write(fd, data + done, length - done);

	if (n < 0 && errno == EINTR)
	    continue;
	if (n <= 0) {
	    if (n == 0)
		errno = EIO;
	    return -1;
	}
	done += (uint32_t) n;
    }
    return 0;
}

uint32_t
dtsp_lock_try(struct dtsp_spool *spool, uint32_t kind,
	      const unsigned char *content, uint32_t length,
	      const char suffix[DTSP_SUFFIX_LENGTH + 1], uint32_t *age)
{
    char lock[DTSP_LOCKNAME_MAX];
    char temp[DTSP_TEMP_MAX];
    struct stat sb;
    uint32_t status;
    int fd, error;
    time_t made;

    *age = 0;
    status = lock_name(spool, kind, lock);
    if (status != DTSP_OK)
	return status;
    snprintf(temp, sizeof temp, "%s.lk.%s", spool->name, suffix);

    /* The file is made under a name nobody knows beforehand, exclusively. */
    fd = openat(spool->dirfd, temp,
		O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC,
		0660);
    if (fd < 0)
	return failure(errno);
    if (fchmod(fd, 0660) != 0 || write_all(fd, content, length) != 0
	|| fsync(fd) != 0 || fstat(fd, &sb) != 0) {
	error = errno;
	(void) close(fd);
	(void) unlinkat(spool->dirfd, temp, 0);
	return failure(error);
    }
    made = sb.st_ctime;
    if (close(fd) != 0) {
	error = errno;
	(void) unlinkat(spool->dirfd, temp, 0);
	return failure(error);
    }

    if (linkat(spool->dirfd, temp, spool->dirfd, lock, 0) == 0) {
	(void) unlinkat(spool->dirfd, temp, 0);
	return DTSP_OK;
    }
    error = errno;
    /*
     * A file system that is not local can make the link and report a failure
     * (the answer was lost and the call repeated): the lock is ours when the
     * temporary file has two names now.
     */
    if (fstatat(spool->dirfd, temp, &sb, AT_SYMLINK_NOFOLLOW) == 0
	&& sb.st_nlink == 2) {
	(void) unlinkat(spool->dirfd, temp, 0);
	return DTSP_OK;
    }
    (void) unlinkat(spool->dirfd, temp, 0);
    if (error != EEXIST)
	return failure(error);

    /*
     * Somebody holds the lock.  How old is it, by the clock of the file
     * system that holds it (which may be another machine's)?
     */
    if (fstatat(spool->dirfd, lock, &sb, AT_SYMLINK_NOFOLLOW) != 0)
	return errno == ENOENT ? DTSP_VANISHED : failure(errno);
    if (made > sb.st_ctime && (uintmax_t) (made - sb.st_ctime) < UINT32_MAX)
	*age = (uint32_t) (made - sb.st_ctime);
    else if (made > sb.st_ctime)
	*age = UINT32_MAX;
    return DTSP_BUSY;
}

/*
 * Read the lock file and compare it with the text the mailer wrote.  On
 * success the descriptor stays open for the caller, so that the release
 * removes the file that was compared.
 */
static uint32_t
lock_compare(struct dtsp_spool *spool, const char *lock,
	     const unsigned char *content, uint32_t length, int *fd)
{
    unsigned char text[DTSP_MAX_CONTENT + 1];
    size_t have = 0;
    uint32_t status;
    int f = openat(spool->dirfd, lock, O_RDONLY | OPEN_COMMON);

    *fd = -1;
    if (f < 0)
	return failure(errno);
    status = check_file(spool, f);
    if (status == DTSP_REFUSED || status == EISDIR) {
	/* A lock file may belong to a mail agent that runs as someone else:
	 * it is not ours, and not an error of the request.  Neither is
	 * anything but a file. */
	status = DTSP_NOT_OURS;
    }
    if (status != DTSP_OK) {
	(void) close(f);
	return status;
    }
    while (have < sizeof text) {
	ssize_t n = read(f, text + have, sizeof text - have);

	if (n < 0 && errno == EINTR)
	    continue;
	if (n < 0) {
	    int error = errno;

	    (void) close(f);
	    return failure(error);
	}
	if (n == 0)
	    break;
	have += (size_t) n;
    }
    if (have != length || memcmp(text, content, length) != 0) {
	(void) close(f);
	return DTSP_NOT_OURS;
    }
    *fd = f;
    return DTSP_OK;
}

uint32_t
dtsp_lock_verify(struct dtsp_spool *spool, uint32_t kind,
		 const unsigned char *content, uint32_t length)
{
    char lock[DTSP_LOCKNAME_MAX];
    uint32_t status;
    int fd;

    status = lock_name(spool, kind, lock);
    if (status != DTSP_OK)
	return status;
    status = lock_compare(spool, lock, content, length, &fd);
    if (fd >= 0)
	(void) close(fd);
    return status;
}

uint32_t
dtsp_lock_release(struct dtsp_spool *spool, uint32_t kind,
		  const unsigned char *content, uint32_t length)
{
    char lock[DTSP_LOCKNAME_MAX];
    struct stat opened, named;
    uint32_t status;
    int fd;

    status = lock_name(spool, kind, lock);
    if (status != DTSP_OK)
	return status;
    status = lock_compare(spool, lock, content, length, &fd);
    if (status != DTSP_OK)
	return status;

    /* Remove the file that was compared, not one that took its name. */
    if (fstat(fd, &opened) != 0
	|| fstatat(spool->dirfd, lock, &named, AT_SYMLINK_NOFOLLOW) != 0) {
	status = failure(errno);
    } else if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) {
	status = DTSP_NOT_OURS;
    } else if (unlinkat(spool->dirfd, lock, 0) != 0) {
	status = failure(errno);
    }
    (void) close(fd);
    return status;
}

uint32_t
dtsp_lock_break(struct dtsp_spool *spool, uint32_t kind)
{
    char lock[DTSP_LOCKNAME_MAX];
    uint32_t status;

    status = lock_name(spool, kind, lock);
    if (status != DTSP_OK)
	return status;
    if (unlinkat(spool->dirfd, lock, 0) != 0 && errno != ENOENT)
	return failure(errno);
    return DTSP_OK;
}

uint32_t
dtsp_temp_create(struct dtsp_spool *spool,
		 const char suffix[DTSP_SUFFIX_LENGTH + 1], int *fd)
{
    struct stat sb;
    mode_t mode = DTSP_MAILBOX_MODE;
    gid_t gid = spool->mail_gid;
    int f, error;

    *fd = -1;
    if (spool->have_temp)
	return DTSP_TEMP_PENDING;

    /* The new file gets the mode and the group of the one it replaces. */
    if (fstatat(spool->dirfd, spool->name, &sb, AT_SYMLINK_NOFOLLOW) == 0) {
	if (!S_ISREG(sb.st_mode) || sb.st_uid != spool->uid)
	    return DTSP_REFUSED;
	mode = (sb.st_mode & 0777) | 0600;
	gid = sb.st_gid;
    } else if (errno != ENOENT)
	return failure(errno);

    snprintf(spool->temp, sizeof spool->temp, "%s.tmp.%s", spool->name, suffix);
    f = openat(spool->dirfd, spool->temp,
	       O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC,
	       0600);
    if (f < 0)
	return failure(errno);

    /* The group of the old file when the helper may give it, else ours. */
    if (fchown(f, (uid_t) -1, gid) != 0
	&& (gid == spool->mail_gid || fchown(f, (uid_t) -1, spool->mail_gid) != 0)) {
	error = errno;
	goto fail;
    }
    if (fchmod(f, mode) != 0) {
	error = errno;
	goto fail;
    }
    spool->have_temp = 1;
    *fd = f;
    return DTSP_OK;

fail:
    (void) close(f);
    (void) unlinkat(spool->dirfd, spool->temp, 0);
    return failure(error);
}

uint32_t
dtsp_temp_commit(struct dtsp_spool *spool)
{
    struct stat sb;

    if (!spool->have_temp)
	return DTSP_NO_TEMP;
    if (fstatat(spool->dirfd, spool->name, &sb, AT_SYMLINK_NOFOLLOW) == 0) {
	if (!S_ISREG(sb.st_mode) || sb.st_uid != spool->uid)
	    return DTSP_REFUSED;
    } else if (errno != ENOENT)
	return failure(errno);
    if (renameat(spool->dirfd, spool->temp, spool->dirfd, spool->name) != 0)
	return failure(errno);
    spool->have_temp = 0;
    return DTSP_OK;
}

uint32_t
dtsp_temp_discard(struct dtsp_spool *spool)
{
    int result = 0;

    if (spool->have_temp) {
	if (unlinkat(spool->dirfd, spool->temp, 0) != 0 && errno != ENOENT)
	    result = errno;
	spool->have_temp = 0;
    }
    return result ? failure(result) : DTSP_OK;
}
