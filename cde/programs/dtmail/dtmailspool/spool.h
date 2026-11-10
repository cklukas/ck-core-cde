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

#ifndef _DTMAILSPOOL_SPOOL_H
#define _DTMAILSPOOL_SPOOL_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>

#include <DtMail/SpoolProtocol.h>

/*
 * The operations on the mail spool directory.  They work on the files of one
 * user, named by the spool, in one directory, and every name they make is
 * made here from the name of the user: nothing a client sends is ever used as
 * a path.  They do not change the group; the caller holds the group of the
 * spool directory for exactly the time of one call (main.c).
 */

/* DTSP_USER_MAX leaves room for ".tmp.XXXXXX" and ".dtmail" in NAME_MAX. */
#define DTSP_SUFFIX_LENGTH	6
#define DTSP_TEMP_MAX		(DTSP_USER_MAX + sizeof(".tmp.") + DTSP_SUFFIX_LENGTH)
#define DTSP_LOCKNAME_MAX	(DTSP_USER_MAX + sizeof(".dtmail"))

/* The mode of a mailbox file that is made here. */
#define DTSP_MAILBOX_MODE	0660

struct dtsp_spool {
    int		dirfd;			/* the spool directory */
    uid_t	uid;			/* the real user, owner of every file */
    gid_t	mail_gid;		/* the group the spool files get */
    char	name[DTSP_USER_MAX + 1];/* login name of uid */
    char	temp[DTSP_TEMP_MAX];	/* the temporary file, when have_temp */
    int		have_temp;
    int		lock_free[2];		/* <name>.lock, <name>.dtmail are no
					   login name of another user */
};

/*
 * A user name that is a single, plain file name component.  False for the
 * empty name, a name that is too long, one with a '/', a control character or
 * a leading '.', which also excludes "." and "..".
 */
int dtsp_valid_user_name(const char *name);

/*
 * The spool of the user `name` with the real user id `uid` in the directory
 * `dirfd`, whose files get the group `mail_gid`.  Every file name the other
 * functions use is made from the name, so a name that is not a plain file
 * name is refused here: 0 on success, -1 (EINVAL) otherwise.
 */
int dtsp_spool_init(struct dtsp_spool *spool, int dirfd, uid_t uid,
		    gid_t mail_gid, const char *name);

/* Six random letters and digits for the name of a temporary file. */
int dtsp_random_suffix(char suffix[DTSP_SUFFIX_LENGTH + 1]);

/* Every function returns a DTSP_ status: 0, an errno value or a code. */

uint32_t dtsp_open(struct dtsp_spool *spool, uint32_t flags, int *fd,
		   uint32_t *access, uint32_t *created);

uint32_t dtsp_lock_try(struct dtsp_spool *spool, uint32_t kind,
		       const unsigned char *content, uint32_t length,
		       const char suffix[DTSP_SUFFIX_LENGTH + 1], uint32_t *age);
uint32_t dtsp_lock_verify(struct dtsp_spool *spool, uint32_t kind,
			  const unsigned char *content, uint32_t length);
uint32_t dtsp_lock_release(struct dtsp_spool *spool, uint32_t kind,
			   const unsigned char *content, uint32_t length);
uint32_t dtsp_lock_break(struct dtsp_spool *spool, uint32_t kind);

uint32_t dtsp_temp_create(struct dtsp_spool *spool,
			  const char suffix[DTSP_SUFFIX_LENGTH + 1], int *fd);
uint32_t dtsp_temp_commit(struct dtsp_spool *spool);
uint32_t dtsp_temp_discard(struct dtsp_spool *spool);

#endif
