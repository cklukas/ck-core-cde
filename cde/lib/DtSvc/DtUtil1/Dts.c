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
 * (c) Copyright 1993, 1994 Hewlett-Packard Company                     *
 * (c) Copyright 1993, 1994 International Business Machines Corp.       *
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.                      *
 * (c) Copyright 1993, 1994 Novell, Inc.                                *
 */
/*
 *+SNOTICE
 *
 *	$TOG: Dts.c /main/18 1999/10/15 12:18:39 mgreess $
 *
 *	RESTRICTED CONFIDENTIAL INFORMATION:
 *	
 *	The information in this document is subject to special
 *	restrictions in a confidential disclosure agreement bertween
 *	HP, IBM, Sun, USL, SCO and Univel.  Do not distribute this
 *	document outside HP, IBM, Sun, USL, SCO, or Univel wihtout
 *	Sun's specific written approval.  This documment and all copies
 *	and derivative works thereof must be returned or destroyed at
 *	Sun's request.
 *
 *	Copyright 1993 Sun Microsystems, Inc.  All rights reserved.
 *
 *+ENOTICE
 */
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>
#include <sys/mman.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(CSRG_BASED)
#define MAXINT INT_MAX
#else
#include <values.h>
#endif
#include <sys/param.h>
#include <sys/errno.h>
#ifdef	_SUN_OS
#include <libgen.h>
#endif
#include <sys/utsname.h>
#define X_INCLUDE_STRING_H
#define XOS_USE_XT_LOCKING
#include <X11/Xos_r.h>
#include <Dt/DtNlUtils.h>
#include "Dt/DtsMM.h"
#include "Dt/Dts.h"
#include "DtSvcLock.h"
#include <Dt/UserMsg.h>

#include <X11/Xosdefs.h>
#include <errno.h>
#ifdef X_NOT_STDC_ENV
extern int errno;
#endif

struct	list
{
	int boson;
	DtDtsMMRecord *rec;
};

typedef	struct	type_info
{
	char			*file_path;
	char			*name;
	const struct stat	*file_stat;
	const struct stat	*file_lstat;
	int			file_fd;
	const char		*buffer;
	long			buff_size;
	long			mmap_size_to_free;
	long			size_to_free;
	const char		*opt_name;
	int			error;
	const char		*link_path;
	const char		*link_name;
	int			set_datatype;
	char			*ot;
	char			*mb;
	size_t			mb_size;
	int			name_type;
	int			*name_prev;
	int			name_count;
	char			*orig_attr;
} type_info_t;

static	DtShmBoson	dtdts_path_pattern = 0;
static	DtShmBoson	dtdts_name_pattern = 0;
static	DtShmBoson	dtdts_mode = 0;
static	DtShmBoson	dtdts_link_name = 0;
static	DtShmBoson	dtdts_link_path = 0;
static	DtShmBoson	dtdts_content = 0;
static	DtShmBoson	dtdts_data_attributes_name = 0;
static	DtShmBoson	dtdts_da_is_action = 0;
static	DtShmBoson	dtdts_da_icon = 0;
static	DtShmBoson	dtdts_da_description = 0;
static	DtShmBoson	dtdts_da_label = 0;

#define	MB_SIZE	100
#define _MBLEN(p) (mblen(p, MB_CUR_MAX) > 1 ? mblen(p, MB_CUR_MAX) : 1)
#define _MBADV(p) ((p) += _MBLEN(p))

/* external functions */
extern	char	*strdup(const char *);
extern	FILE	*popen(const char *, const char *);
extern	char	*DtActionIcon(const char *);
extern	char	*DtActionDescription(const char *);
extern	char	*DtActionLabel(const char *);
extern  XtAppContext _DtAppContext;

/* local functions */
static	DtDtsMMRecord *	name_list(type_info_t *linfo,
		DtDtsMMDatabase	*db,
		DtDtsMMRecord *rec_ptr);
static	const struct stat *get_stat(type_info_t *info);
static	char *	cleanup(char *ot, type_info_t *info);

static int      csh_match(const char *, const char *);
static int      csh_match_star(const char *, const char *);

/* filetype comparison function for sorting */
extern	int	cde_dc_compare(const void *entry1, const void *entry2);
extern	int	cde_da_compare(DtDtsMMRecord **entry1, DtDtsMMRecord **entry2);
extern	int	cde_dc_field_compare(const void *entry1, const void *entry2);


void
_DtDtsClear(void)
{
        _DtSvcProcessLock();
	dtdts_path_pattern = 0;
	dtdts_name_pattern = 0;
	dtdts_mode = 0;
	dtdts_link_name = 0;
	dtdts_link_path = 0;
	dtdts_content = 0;
	dtdts_data_attributes_name = 0;
	dtdts_da_is_action = 0;
	dtdts_da_icon = 0;
	dtdts_da_description = 0;
	dtdts_da_label = 0;
	_DtSvcProcessUnlock();
}

/*
 * The character at the start of s.  A byte that does not start a valid
 * multibyte character is the character of its own value, so that the
 * comparison never reads a character that was not set.
 */
static wchar_t
mb_char(const char *s)
{
	wchar_t	wc = L'\0';

	if(mbtowc(&wc, s, MB_CUR_MAX) < 0)
	{
		/* leave the conversion state as it was before the bad byte */
		(void)mbtowc(NULL, NULL, 0);
		return((wchar_t)(unsigned char)*s);
	}
	return(wc);
}

static int
gmatch(const char *string, const char *pattern)
{
	wchar_t         string_ch;
	int             k;
	wchar_t         pattern_ch;
	wchar_t         lower_bound;
	char            *p;

top:
	for (; 1; _MBADV(pattern), _MBADV(string))
	{
		lower_bound = (wchar_t)MAXINT;
		string_ch = mb_char(string);
		pattern_ch = mb_char(pattern);
		switch (pattern_ch)
		{
		case L'[':
			k = 0;
			for(pattern_ch = mb_char(_MBADV(pattern));
                            pattern_ch != L'\0';
                            pattern_ch = mb_char(_MBADV(pattern)))
			{
				switch (pattern_ch)
				{
				case L']':
					if (!k)
					{
						return 0;
					}
					_MBADV(string);
					_MBADV(pattern);
					goto top;
				case L'-':
					if(lower_bound <= string_ch) {
					    p = (char *)pattern;
					    pattern_ch = mb_char(_MBADV(pattern));
					    k |= (string_ch <= pattern_ch);
					    pattern = p;
					}
					else
					    k |= 0;
					/* Fall through... */
				default:
					if (string_ch ==
						 (lower_bound = pattern_ch))
					{
						k++;
					}
				}
			}
			return 0;
		case L'*':
			return csh_match_star(string, _MBADV(pattern));
		case L'\0':
			return ((string_ch != L'\0') ? 0 : 1);
		case L'?':
			if (string_ch == L'\0')
			{
				return 0;
			}
			break;
		default:
			if (pattern_ch != string_ch)
			{
				return 0;
			}
			break;
		}
	}
	/* NOTREACHED */
}

/*
 * csh_match_star(string, pattern) This matches the '*' portion of a pattern.
 */
static int
csh_match_star(const char *string, const char *pattern)
{
	wchar_t         pattern_ch;
	wchar_t         string_ch;

	pattern_ch = mb_char(pattern);
	switch (pattern_ch)
	{
	case L'\0':
		return 1;
	case L'[':
	case L'?':
	case L'*':
		while (*string)
		{
			if(gmatch(string, pattern)) {
			    _MBADV(string);
			    return 1;
			}
			else
			    _MBADV(string);
		}
		break;
	default:
		pattern_ch = mb_char(pattern);
		_MBADV(pattern);
		while (*string)
		{
			string_ch = mb_char(string);
			_MBADV(string);
			if(string_ch == pattern_ch && gmatch(string, pattern))
			{
				return 1;
			}
		}
		break;
	}
	return 0;
}

/*
 * Return the scratch buffer of info, which is at least size bytes long.
 * The buffer is created on first use (MB_SIZE bytes, or size if that is
 * larger) and enlarged when size exceeds its recorded size; its contents
 * are kept and the added bytes are zeroed.  A size of 0 asks for the
 * current buffer.  NULL is returned when the request cannot be met (more
 * than an object can hold, or no memory); the buffer and its recorded size
 * are then left as they were.
 */
static char *
max_buf(size_t size, type_info_t *info)
{
	char	*mb;

	if(size > (size_t)PTRDIFF_MAX)
	{
		return(NULL);
	}
	if(!info->mb)
	{
		size_t	new_size = (size > MB_SIZE) ? size : MB_SIZE;

		mb = (char *)calloc(new_size, 1);
		if(!mb)
		{
			return(NULL);
		}
		info->mb = mb;
		info->mb_size = new_size;
	}
	else if(size > info->mb_size)
	{
		mb = (char *)realloc(info->mb, size);
		if(!mb)
		{
			return(NULL);
		}
		memset(mb + info->mb_size, 0, size - info->mb_size);
		info->mb = mb;
		info->mb_size = size;
	}
	return(info->mb);
}

static type_info_t *
set_vals(const char		*fn,
	const char		*buf,
	const int		bs,
	const struct stat	*fs,
	const char		*ln,
	const struct stat	*ls,
	const char		*on)
{
	char		*tmp;
	type_info_t	*linfo = (type_info_t *)calloc(1, sizeof(type_info_t));

	if(!linfo)
	{
		return(0);
	}
	linfo->buff_size = -1;
	linfo->file_fd = -1;
	linfo->error = 0;
	linfo->mmap_size_to_free = -1;
	linfo->size_to_free = -1;
	linfo->set_datatype = 0;
	linfo->ot = 0;
	linfo->mb = 0;
	linfo->mb_size = 0;
	linfo->orig_attr = 0;

	if(fn)
	{
		linfo->file_path = strdup(fn);
		if(!linfo->file_path)
		{
			goto no_memory;
		}
		linfo->name = strrchr(linfo->file_path, '/');
		if(linfo->name)
		{
			linfo->name++;
		}
		else
		{
			char	cwd[MAXPATHLEN];

			if(getcwd(cwd, sizeof(cwd)))
			{
				size_t	cwd_len = strlen(cwd);
				char	*path = (char *)malloc(cwd_len + 1 +
						strlen(linfo->file_path) + 1);

				if(!path)
				{
					goto no_memory;
				}
				memcpy(path, cwd, cwd_len);
				path[cwd_len] = '/';
				strcpy(path + cwd_len + 1, linfo->file_path);

				free(linfo->file_path);
				linfo->file_path = path;
				linfo->name = path + cwd_len + 1;
			}
			else
			{
				/* no working directory: keep the relative name */
				linfo->name = linfo->file_path;
			}
		}
	}
	else if(buf)
	{
		if(!fs)
		{
			struct	stat	*buf;
			buf = malloc(sizeof(struct stat));
			if(!buf)
			{
				goto no_memory;
			}
			memset(buf, '\0', sizeof(struct stat));
			buf->st_mode = 	S_IFREG | S_IROTH | S_IRGRP | 
					S_IRUSR | S_IWOTH | S_IWGRP | 
					S_IWUSR;
			linfo->file_stat = (const struct stat *)buf;
		}
	}
			
	/*
	 * A buffer is as long as the caller says; a buffer of no length or
	 * of a negative one has nothing to look at.
	 */
	if(buf && bs > 0)
	{
		linfo->buffer = buf;
		linfo->buff_size = bs;
	}
	if(fs)
	{
		linfo->file_stat = (struct stat *)malloc(sizeof(struct stat));
		if(!linfo->file_stat)
		{
			goto no_memory;
		}
		memcpy((void *)linfo->file_stat,
			(struct stat *)fs,
			sizeof(struct stat));
	}
	if(ln)
	{
		if(*ln == '/')
		{
			linfo->link_path = (char *)strdup(ln);
			if(!linfo->link_path)
			{
				goto no_memory;
			}
			linfo->link_name = strrchr(ln, '/');
			if(linfo->link_name)
			{
				linfo->link_name++;
			}
		}
		else
		{
			linfo->link_name = (char *)ln;
		}
	}
	if(ls)
	{
		linfo->file_lstat = (struct stat *)malloc(sizeof(struct stat));
		if(!linfo->file_lstat)
		{
			goto no_memory;
		}
		memcpy((void*)linfo->file_lstat,
			(struct stat *)ls,
			sizeof(struct stat));
	}
	if(on)
	{
		linfo->opt_name = strdup(on);
		if(!linfo->opt_name)
		{
			goto no_memory;
		}
	}

	return(linfo);

no_memory:
	/* release whatever was set up so far */
	free(cleanup(0, linfo));
	return(0);
}

static char *
cleanup(char *ot, type_info_t *info)
{
	if(ot)
	{
		ot = strdup(ot);
	}
	else if(info->ot)
	{
		ot = strdup(info->ot);
	}
	else
	{
		ot = strdup(DtDTS_DT_UNKNOWN);
	}
	if(info->ot)
	{
		free(info->ot);
	}

	if(info->file_stat)
	{
		free((void *)info->file_stat);
	}
	if(info->file_lstat)
	{
		free((void *)info->file_lstat);
	}

	info->file_stat = 0;
	info->file_lstat = 0;
	if(info->buffer)
	{
		if(info->mmap_size_to_free != -1)
		{
			if(munmap((caddr_t)info->buffer, 
				info->mmap_size_to_free) != 0)
			{
			    _DtSimpleError(
					DtProgName, DtError, NULL,
					"munmap", NULL);
			}
			info->buffer = 0;
			info->mmap_size_to_free = -1;
		}
		else if(info->size_to_free != -1)
		{
			free((void *)info->buffer);
			info->buffer = 0;
			info->size_to_free = -1;
		}
	}
	if(info->file_fd != -1)
	{
		close(info->file_fd);
		info->file_fd = -1;
	}
	info->buffer = 0;
	if(info->file_path)
	{
		free((void *)info->file_path);
		info->file_path = 0;
		info->name = 0;
	}
	if(info->name)
	{
		free((void *)info->name);
		info->name = 0;
	}
	info->file_path = 0;
	info->name = 0;
	info->file_fd = -1;
	info->buff_size = -1;
	info->error = 0;
	if(info->link_path != (char *)0 && info->link_path != (char *)-1)
	{
		free((void *)info->link_path);
	}

	if(info->opt_name)
	{
		free((void *)info->opt_name);
	}
	if(info->mb)
	{
		free(info->mb);
	}
	if(info->orig_attr)
	{
		_DtDtsMMSafeFree(info->orig_attr);
	}

	free(info);
	info = 0;
	return(ot);
}

static	const char*
get_opt_name(type_info_t *info)
{
	return(info->opt_name);
}

static	const char*
get_name(type_info_t *info)
{
	return(info->name);
}

static	const char*
get_file_path(type_info_t *info)
{
	if(info->file_path)
	{
		return(info->file_path);
	}
	else
	{
		return(0);
	}
}

static int
get_fd(type_info_t *info)
{
	if(!info->error && info->file_fd == -1)
	{
		if(info->file_path == 0)
		{
			return(-1);
		}
		if((info->file_fd = open(info->file_path, O_RDONLY|O_NOCTTY, 0)) == -1)
		{
			info->error = errno;
			return(-1);
		}
	}
	return(info->file_fd);
}

static const struct stat *
get_lstat(type_info_t *info)
{
	struct	stat	buf;

	if(!info->file_lstat)
	{
		if(NULL == info->file_path)
		{
			info->error = ENOENT;
			info->file_lstat = 0;
			return(0);
		}
		else if(lstat(info->file_path, &buf) == -1)
		{
			info->error = errno;
			info->file_lstat = 0;
			return(0);
		}
		else
		{
			info->file_lstat = (struct stat *)malloc(sizeof(struct stat));
			if(!info->file_lstat)
			{
				info->error = ENOMEM;
				return(0);
			}
			memcpy((char *)info->file_lstat, &buf, sizeof(buf));
		}
	}
	return(info->file_lstat);
}

static const struct stat *
get_stat(type_info_t *info)
{
	struct	stat	buf;
	int		fd;

	if (!info->file_stat)
	{
		if(NULL == get_file_path(info))
		{
			info->error = ENOENT;
			info->file_stat = 0;
		}
		else if(stat(get_file_path(info), &buf) == -1)
		{
			if(errno == ENOENT)
			{
				if(get_lstat(info))
				{
					info->ot = strdup(DtDTS_DT_BROKEN_LINK);
				}
			}
			info->error = errno;
			info->file_stat = 0;
		}
		else
		{
			info->file_stat = (struct stat *)malloc(sizeof(struct stat));
			if(!info->file_stat)
			{
				info->error = ENOMEM;
				return(0);
			}
			memcpy((char *)info->file_stat, &buf, sizeof(buf));
		}
	}

	return(info->file_stat);
}

static int
islink(type_info_t *info)
{
	if(info->file_lstat || info->link_path || info->link_name)
	{
		return(1);
	}
	return(0);
}

/*
 * Follow the symbolic links from the file to what they lead to: link_path
 * is the last name and link_name its last component.  A relative target is
 * relative to the directory of the link.  A chain of more than
 * DTS_MAX_LINKS links, or one that leads back to itself, is an error.
 */
#define DTS_MAX_LINKS 40

void
get_link_info(type_info_t *info)
{
	char		buff[MAXPATHLEN + 1];
	char		*name = 0;
	ssize_t		n;
	int		links = 0;

	if(info->link_path == 0)
	{
		if(get_file_path(info) == 0)
		{
			info->link_path = (char *)-1;
			info->link_name = (char *)-1;
			return;
		}
		name = strdup(get_file_path(info));
		if(name == 0)
		{
			info->error = ENOMEM;
			info->link_path = (char *)-1;
			info->link_name = (char *)-1;
			return;
		}
		for(;;)
		{
			char	*next;

			n = readlink(name, buff, MAXPATHLEN);
			if(n < 0)
			{
				break;
			}
			if(n == 0 || n >= MAXPATHLEN)
			{
				/* an empty target, or one that was cut off */
				errno = (n == 0) ? ENOENT : ENAMETOOLONG;
				n = -1;
				break;
			}
			if(++links > DTS_MAX_LINKS)
			{
				errno = ELOOP;
				n = -1;
				break;
			}
			buff[n] = '\0';
			if(buff[0] == '/')
			{
				next = strdup(buff);
			}
			else
			{
				/* relative to the directory of the link */
				char	*slash = strrchr(name, '/');
				size_t	dir_len = slash ? (size_t)(slash - name) + 1 : 0;

				next = (char *)malloc(dir_len + (size_t)n + 1);
				if(next)
				{
					memcpy(next, name, dir_len);
					memcpy(next + dir_len, buff, (size_t)n + 1);
				}
			}
			if(next == 0)
			{
				errno = ENOMEM;
				n = -1;
				break;
			}
			free(name);
			name = next;
		}
		if(errno == EINVAL || errno == ENOENT)
		{
			info->link_path = name;
			info->link_name = strrchr(info->link_path, '/');
			if(info->link_name == 0)
			{
				info->link_name = info->link_path;
			}
			else
			{
				info->link_name++;
			}
		}
		else
		{
			free(name);
			info->error = errno;
			info->link_path = (char *)-1;
			info->link_name = (char *)-1;
		}
	}
	return;
}

static const char *
get_link_path(type_info_t *info)
{
	get_link_info(info);
	return(info->link_path);
}

static const char *
get_link_name(type_info_t *info)
{
	get_link_info(info);
	return(info->link_name);
}

/*
 * Read from fd until size bytes are in buffer or the file ends; returns the
 * number of bytes, or -1 with errno set.
 */
static ssize_t
read_file(int fd, char *buffer, size_t size)
{
	size_t	have = 0;

	while(have < size)
	{
		ssize_t	got = read(fd, buffer + have, size - have);

		if(got < 0)
		{
			if(errno == EINTR)
			{
				continue;
			}
			return(-1);
		}
		if(got == 0)
		{
			break;
		}
		have += (size_t)got;
	}
	return((ssize_t)have);
}

/*
 * The contents of the file, mapped or read; buff_size is their length,
 * which is what could be read, not what the file had when it was looked at.
 */
static const char *
get_buff(type_info_t *info)
{
	const struct	stat	*buf;

	if(!info->buffer && info->buffer != (char *)-1)
	{
		buf = get_stat(info);
		if(buf && buf->st_size > 0)
		{
			char	*mem;
			ssize_t	got;
			long	size;

			if(buf->st_size >= LONG_MAX)
			{
				/* more than a buffer can describe */
				info->error = EFBIG;
				return((char *)0);
			}
			size = (long)buf->st_size;
			if ((info->file_fd == -1) && (get_fd(info) == -1))
			   return 0;
			
			mem = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE,
				info->file_fd, 0);
			if(mem != (char *)MAP_FAILED)
			{
				info->buffer = mem;
				info->mmap_size_to_free = size;
				info->buff_size = size;
				return(info->buffer);
			}

			/* what is read is kept in memory that starts as zeros */
			mem = (char *)calloc((size_t)size + 1, 1);
			if(mem == 0)
			{
				info->error = ENOMEM;
				return(0);
			}
			got = read_file(info->file_fd, mem, (size_t)size);
			if(got < 0)
			{
				info->error = errno;
				free(mem);
				return(0);
			}
			info->buffer = mem;
			info->size_to_free = size + 1;
			info->buff_size = (long)got;
		}
		else
		{
			return((char *)0);
		}

	}
	return(info->buffer);
}

long
get_buff_size(type_info_t *info)
{
	return(info->buff_size);
}
static DtDtsMMDatabase *
get_dc_db(void)
{
	DtDtsMMDatabase *dc_db = 0;

	if(!dc_db)
	{
#ifdef	DEBUG
fprintf(stderr, "Load DataCriteria\n");
#endif
		dc_db = _DtDtsMMGet(DtDTS_DC_NAME);
		if(!dc_db)
		{
			_DtSimpleError(
				DtProgName, DtError, NULL,
				"No DataBase loaded\n", NULL);
			return(NULL);
		}
	}
#ifdef NO_MMAP
	if(dc_db->compare != cde_dc_compare)
	{
		int	i;
		for(i = 0; i < dc_db->recordCount; i++)
		{
			if(dc_db->recordList[i]->compare !=cde_dc_field_compare)
			{
				_DtDtsMMFieldSort(dc_db->recordList[i], 
					cde_dc_field_compare);
			}
		}
		_DtDtsMMRecordSort(dc_db, cde_dc_compare);
#ifdef DEBUG
_DtDtsMMPrint(stdout);
#endif
	}
#endif
	return(dc_db);
}

static DtDtsMMDatabase *
get_da_db(void)
{
	DtDtsMMDatabase *da_db = 0;
	if(!da_db)
	{
#ifdef	DEBUG
fprintf(stderr, "Load DataAttributes\n");
#endif
		da_db = _DtDtsMMGet(DtDTS_DA_NAME);
		if(!da_db)
		{
			_DtSimpleError(
				DtProgName, DtError, NULL,
				"No DataBase loaded\n", NULL);
			return(NULL);
		}
	}

#ifdef NO_MMAP
	if(da_db->compare != _DtDtsMMCompareRecordNames)
	{
		int	i;
		for(i = 0; i < da_db->recordCount; i++)
		{
			if(da_db->recordList[i]->compare !=
					_DtDtsMMCompareFieldNames)
			{
				_DtDtsMMFieldSort(da_db->recordList[i], 
					_DtDtsMMCompareFieldNames);
			}
		}

		_DtDtsMMRecordSort(da_db, _DtDtsMMCompareRecordNames);
	}
#endif
	return(da_db);
}

#define	DTSATTRVAL(attr_name) if(_DtDtsMMStringToBoson(attr_name) == fld_ptr->fieldName)

static int
type_content(char *attr, type_info_t *info)
{
	int	match = 0;
	char	*c;
	int	end = 0;
	size_t	s, avail;
	long	offset;
	const	char	*buff = 0;
	const	struct	stat	*buf;
	char		*p;
	_Xstrtokparams	strtok_buf;

	if(buf = get_stat(info))
	{
		if((buf->st_mode&S_IFMT) == S_IFDIR && 
			(c = strstr(attr, "filename")))
		{
			char *file;
			const char *path;

			c = _XStrtok(c, " \t\n", strtok_buf);
			c = _XStrtok(NULL, " \t\n", strtok_buf);
			path = get_file_path(info);
			if(!c || !path)
			{
				return(0);
			}
			file = calloc(1, strlen(path)+strlen(c)+2);
			if(!file)
			{
				return(0);
			}
			sprintf(file, "%s/%s", path, c);
			if(access(file, F_OK))
			{
				free(file);
				return(0);
			}
			else
			{
				free(file);
				return(1);
			}
		}
		else if((buf->st_mode&S_IFMT) != S_IFREG)
		{
			return(0);
		}
	}
	buff = get_buff(info);
	if((long)buff == 0 || (long)buff == -1 || get_buff_size(info) <= 0)
	{
		return(0);
	}
	avail = (size_t)get_buff_size(info);

	attr = strdup(attr);
	if(!attr)
	{
		return(0);
	}
	c = _XStrtok(attr, " \t\n", strtok_buf);
	if(!c)
	{
		/* no start offset at all */
		free(attr);
		return(0);
	}
	if(!isdigit((unsigned char)*c) && *c != '-')
	{
		/* a start given as a string is not supported */
		match = 0;
	}
	else
	{
		if(*c == '-')
		{
			end = 1;
			c++;
		}

		offset = strtol(c, NULL, 10);
		if(offset < 0)
		{
			/* not an offset a buffer could be indexed with */
			free(attr);
			return(0);
		}
		s = (size_t)offset;
		if(end) 
		{
			/* counted from the end of what was read */
			if (s > avail)
			{
				free(attr);
				return(match);
			}
			s = avail - s;
		}
		if(s > avail)
		{
			/* starts after the end of the contents */
			free(attr);
			return(0);
		}
		c = _XStrtok(NULL, " \t\n", strtok_buf);
		if(!c)
		{
			/* an offset without a type */
			free(attr);
			return(0);
		}
		switch(c[1])
		{
		case	't': /* string */
		{
			int	cl;

			c = _XStrtok(NULL, "\n", strtok_buf);
			if(!c)
			{
				/* no string to compare with */
				match = 0;
				break;
			}
			cl = strlen(c);
			if((avail - s) >= (size_t)cl && memcmp(&buff[s], c, cl) == 0)
			{
				match = 1;
			}
			else
			{
				match = 0;
			}
			break;
		}
		case	'y': /* byte */
		{
			unsigned char	lv;
			unsigned char	num;
			int	i = 0;

			match = 0;
			while(c = _XStrtok(NULL, "\t \n", strtok_buf))
			{
				if(s+i*sizeof(lv)+sizeof(lv) > avail)
				{
					match = 0;
					break;
				}
				memcpy(&lv, &buff[s+i*sizeof(lv)], sizeof(lv));
				num = (unsigned char)strtol(c, &p, 0);
				if (num != lv || c == p)
				{
					match = 0;
					break;
				}
				else
				{
					match = 1;
				}
				i++;
			}
			break;
		}
		case	'h': /* short */
		{
			unsigned short	lv;
			unsigned short	num;
			int		i = 0;
			const unsigned char *bufPtr;

			match = 0;
			for(i = 0;(c = _XStrtok(NULL, "\t \n", strtok_buf)); i++)
			{
				if(s+i*sizeof(lv)+sizeof(lv) > avail)
				{
					match = 0;
					break;
				}
				bufPtr = (unsigned char *)
				  &buff[s + (i * sizeof(lv))];
				lv = (((unsigned short)*bufPtr) << 8) |
				     ((unsigned short)*(bufPtr + 1));
				num = (unsigned short)strtol(c, &p, 0);
				if (num != lv || c == p)
				{
					match = 0;
					break;
				}
				else
				{
					match = 1;
				}
			}
			break;
		}
		case	'o': /* long */
		{
			/* Not true long - really 4 bytes. */
			unsigned int	lv;
			unsigned int	num;
			int		i = 0;
			const unsigned char *bufPtr;

			match = 0;
			for(i = 0;(c = _XStrtok(NULL, "\t \n", strtok_buf)); i++)
			{
				if(s+i*sizeof(lv)+sizeof(lv) > avail)
				{
					match = 0;
					break;
				}
				bufPtr = (unsigned char *)
				  &buff[s + (i * sizeof(lv))];
				lv = (((unsigned int)*bufPtr) << 24) |
				     (((unsigned int)*(bufPtr + 1)) << 16) |
				     (((unsigned int)*(bufPtr + 2)) << 8) |
				     ((unsigned int)*(bufPtr + 3));
				num = (unsigned int)strtol(c, &p, 0);
				if (num != lv || c == p)
				{
					match = 0;
					break;
				}
				else
				{
					match = 1;
				}
			}
			break;
		}
		default:
			match = 0;
		}
		
	}
	free(attr);
	return(match);
}

/*
 * Find the data type name in the NUL terminated text of a directory's
 * DtDTS_DT_DIR file: the word that follows DtDTS_DATA_ATTRIBUTES_NAME.
 * Returns 1 and the extent [*start, *end) of that word, or 0 when the
 * keyword or the word after it is missing.
 */
static int
dir_datatype_extent(const u_char *buff, int *start, int *end)
{
	const u_char	*p = (const u_char *)strstr((const char *)buff,
					DtDTS_DATA_ATTRIBUTES_NAME);

	if(p == NULL)
	{
		return(0);
	}
	while(*p && !isspace(*p)) p++;
	while(isspace(*p)) p++;
	if(!*p)
	{
		return(0);
	}
	*start = p-buff;
	while(*p && !isspace(*p)) p++;
	*end = p-buff;
	return(1);
}

static char *
_DtDtsGetDataType(const char *file)
{
	int		fd;
	char		*name;
	size_t		name_size;
	struct stat	file_stat;
	u_char		*buff = 0;
	int		start;
	char		*dt = 0;
	int		end;

	name_size = strlen(file)+strlen(DtDTS_DT_DIR)+2;
	name = calloc(1, name_size);
	if(!name)
	{
		return(0);
	}
	snprintf(name, name_size, "%s/%s", file, DtDTS_DT_DIR);
	if((fd = open(name, O_RDONLY, 0644)) != -1)
	{
		if(fstat(fd, &file_stat) == 0)
		{
			buff = (u_char *)calloc((size_t)1, file_stat.st_size+1);
			if(buff)
			{
				read(fd, buff, file_stat.st_size);
			}
		}
		if(buff && dir_datatype_extent(buff, &start, &end))
		{
			buff[end] = '\0';
			dt = strdup((char *)&buff[start]);
		}
		close(fd);
	}
	free(buff);
	free(name);
	return(dt);
}

static int
type_mode(char *attr, type_info_t *info)
{
	int		match = 0;
	const	struct	stat	*buf = get_stat(info);
	const	struct	stat	*lbuf;

	if (!buf || !*attr)
	{
		return(0);
	}

	do
	{
		match = 0;
		switch(*attr)
		{
		case	'f':
			match = (buf->st_mode&S_IFMT) == S_IFREG;
			break;
		case	'c':
			match = (buf->st_mode&S_IFMT) == S_IFCHR;
			break;
		case	'b':
			match = (buf->st_mode&S_IFMT) == S_IFBLK;
			break;
		case	'd':
			if((buf->st_mode&S_IFMT) == S_IFDIR)
			{
				int	n;
				int	fd;

				match = 1;
				if(!info->set_datatype)
				{
					info->set_datatype = 1;
					info->ot = 
					      _DtDtsGetDataType(get_file_path(info));
					if(info->ot) return(match);
				}
			}
			else
			{
				match = 0;
			}
	
			break;
		case	'l':
			if(islink(info))
			{
				match = 1;
			}
			else if(lbuf = get_lstat(info))
			{
				match = (lbuf->st_mode&S_IFMT) == S_IFLNK;
			}
			else
			{
				match = 0;
			}
			break;
		case	'r':
			if(buf->st_mode&S_IROTH || \
			   buf->st_mode&S_IRGRP || \
			   buf->st_mode&S_IRUSR )
			{
				match = 1;
			}
			else
			{
				match = 0;
			}
			break;
		case	'w':
			if(buf->st_mode&S_IWOTH || \
			   buf->st_mode&S_IWGRP || \
			   buf->st_mode&S_IWUSR )
			{
				match = 1;
			}
			else
			{
				match = 0;
			}
			break;
		case	'x':
			if(buf->st_mode&S_IXOTH || \
			   buf->st_mode&S_IXGRP || \
			   buf->st_mode&S_IXUSR ) 
			{
				match = 1;
			}
			else
			{
				match = 0;
			}
			break;
		case	'g':
			match = buf->st_mode&S_ISGID;
			break;
		case	'u':
			match = buf->st_mode&S_ISUID;
			break;
		}
		attr++;
	} while(*attr && match);
	return(match);
}

static int
type_name(const char *name, char *attr)
{
	int	match = 0;

	if(name && name != (char *)-1)
	{
#ifdef USE_FNMATCH
		match = !fnmatch(attr, name, 0);
#else
		match =  gmatch(name, attr);
#endif
	}
	return(match);
}

static int
type_path(const char *path, char *attr)
{
	char	*c;
	int	match = 0;

	if(path && (intptr_t)path != -1)
	{
#ifdef USE_FNMATCH
		match = !fnmatch(attr, path, 0);
#else
		match = gmatch(path, attr);
#endif
	}
	return(match);
}

static char *
next_sep(char *str, char *sep)
{
	char	*c;
	char	*prev;

	c = str;
	while(*c)
	{
		c = _dt_strpbrk(c, sep);
		if(!c)
		{
			return(c);
		}
		prev = c-1;
		if((c == str) || (_is_previous_single(str, c) && (*prev != '\\')))
		{
			return(c);
		}
		_MBADV(c);
	}
	return((char *)0);
}

char	*
DtDtsDataToDataType(const char *fp,
	const void		*buf,
	const int		bs,
	const struct stat	*fs,
	const char		*ln,
	const struct stat	*ls,
	const char		*on)
{
	DtDtsMMDatabase	*db;
	DtDtsMMRecord	*rec_ptr = 0;
	DtDtsMMRecord	*rec_ptr_list;
	DtDtsMMField	*fld_ptr;
	DtDtsMMField	*fld_ptr_list;
	type_info_t	*info = 0;
	int	i;
	int	j;
	int	rec_m = 0;
	int	fld_m = 0;
	int	atr_m = 0;
	int	p_atr_m = 1;
	int	c_atr_m = 1;
	char	*ot = NULL;
	char	*file;
	char	*attr;
	char	op;

	_DtSvcAppLockDefault();
	_DtSvcProcessLock();
	db = get_dc_db();
	if(!db)
	{
		_DtSvcProcessUnlock();
		_DtSvcAppUnlockDefault();
		return(strdup("UNKNOWN"));
	}

	info = set_vals(fp, buf, bs, fs, ln, ls, on);
	if(!info)
	{
		_DtSvcProcessUnlock();
		_DtSvcAppUnlockDefault();
		return(strdup(DtDTS_DT_UNKNOWN));
	}
	if(!dtdts_path_pattern)
	{
		dtdts_path_pattern = _DtDtsMMStringToBoson(DtDTS_PATH_PATTERN);
		dtdts_name_pattern = _DtDtsMMStringToBoson(DtDTS_NAME_PATTERN);
		dtdts_mode = _DtDtsMMStringToBoson(DtDTS_MODE);
		dtdts_link_name = _DtDtsMMStringToBoson(DtDTS_LINK_NAME);
		dtdts_link_path = _DtDtsMMStringToBoson(DtDTS_LINK_PATH);
		dtdts_content = _DtDtsMMStringToBoson(DtDTS_CONTENT);
		dtdts_data_attributes_name = _DtDtsMMStringToBoson(DtDTS_DATA_ATTRIBUTES_NAME);

		dtdts_da_is_action = _DtDtsMMStringToBoson(DtDTS_DA_IS_ACTION);
		dtdts_da_icon = _DtDtsMMStringToBoson(DtDTS_DA_ICON);
		dtdts_da_description = _DtDtsMMStringToBoson(DtDTS_DA_DESCRIPTION);
		dtdts_da_label = _DtDtsMMStringToBoson(DtDTS_DA_LABEL);
	}

	for(i = 0; !rec_m && (rec_ptr = name_list(info, db, rec_ptr)); i++)
	{
		fld_ptr_list = _DtDtsMMGetPtr(rec_ptr->fieldList);
		fld_m = 1;
		for(j=0; fld_m && j < rec_ptr->fieldCount; j++)
		{
			char	sep = '&';
			DtDtsMMField	*fld_ptr = &fld_ptr_list[j];

			p_atr_m = 1;
			c_atr_m = 1;
			info->orig_attr = _DtDtsMMExpandValue(
					_DtDtsMMBosonToString(fld_ptr->fieldValue));
			/* a field without a value is an empty attribute */
			attr = info->orig_attr ? info->orig_attr : "";
			do
			{
				int	neg = 0;
				char	*new_sep;
				size_t	n;
				char	*c;

				atr_m = 1;
				while(*attr == '!')
				{
					neg = !neg;
					attr++;
				}
				new_sep = next_sep(attr, "&|\0");
				if(new_sep == 0)
				{
					new_sep = attr + strlen(attr);
				}
				n = (size_t)(new_sep-attr)+1;
				c = max_buf(n, info);
				if(!c)
				{
					/* no room to examine this attribute */
					_DtSvcProcessUnlock();
					_DtSvcAppUnlockDefault();
					return(cleanup(0, info));
				}

				strncpy(c, attr, new_sep-attr);
				c[new_sep-attr] = '\0';
				attr = new_sep;
				new_sep = c;
/*
				while(new_sep = (char *)_dt_strpbrk(new_sep, "&|"))
				{
					new_sep--;
					strcpy(new_sep, &new_sep[1]);
					new_sep++;
				}
*/

				if(fld_ptr->fieldName == dtdts_path_pattern)
				{
					atr_m = type_path(
							get_file_path(info),
							max_buf((size_t)0, info));
				}
				else if(fld_ptr->fieldName == dtdts_name_pattern)
				{
					if(get_file_path(info))
					{
						atr_m = type_name(
							get_name(info),
							max_buf((size_t)0, info));
					}
					else
					{
						atr_m = type_name(
							get_opt_name(info),
							max_buf((size_t)0, info));
					}
				}
				else if(fld_ptr->fieldName == dtdts_mode)
				{
					if(get_file_path(info))
					{
						atr_m = type_mode(
							max_buf((size_t)0, info), info);
					}
					else
					{
						atr_m = !neg;
					}
				}
				else if(fld_ptr->fieldName == dtdts_link_name)
				{
					atr_m = type_name(
							get_link_name(info),
							max_buf((size_t)0, info));

				}
				else if(fld_ptr->fieldName == dtdts_link_path)
				{
					atr_m = type_path(
							get_link_path(info),
							max_buf((size_t)0, info));
				}
				else if(fld_ptr->fieldName == dtdts_content)
				{
					atr_m = type_content(
							max_buf((size_t)0, info), info);
				}
				else if(fld_ptr->fieldName == dtdts_data_attributes_name)
				{
					info->ot = strdup(max_buf((size_t)0, info));
					if(!info->ot)
					{
						_DtSvcProcessUnlock();
						_DtSvcAppUnlockDefault();
						return(cleanup(0, info));
					}
				}

				if(info->error == ELOOP)
				{
				        _DtSvcProcessUnlock();
					_DtSvcAppUnlockDefault();
					return(
					   cleanup(DtDTS_DT_RECURSIVE_LINK, info)
						);
				}
				if(info->ot)
				{
				        _DtSvcProcessUnlock();
					_DtSvcAppUnlockDefault();
					return(cleanup(0, info));
				}

				atr_m=(neg)?!atr_m:atr_m;

				switch(sep)
				{
				case	'&':
					p_atr_m = atr_m && p_atr_m;
					break;
				case	'|':
					p_atr_m = atr_m || p_atr_m;
					break;
				}
				if(attr && *attr)
				{
					sep = *attr;
					attr++;
					switch(sep)
					{
					case	'&':
						c_atr_m = p_atr_m?1:0;
						break;
					case	'|':
						c_atr_m = p_atr_m?0:1;
						break;
					}
				}
				else
				{
					c_atr_m = 0;
				}
			} while ( c_atr_m );
			if(info->orig_attr)
			{
				_DtDtsMMSafeFree(info->orig_attr);
				info->orig_attr = 0;
			}

			if(!p_atr_m)
			{
				fld_m = 0;
			}
		}
		if(fld_m)
		{
			rec_m = 1;
		}
	}
	_DtSvcProcessUnlock();
	_DtSvcAppUnlockDefault();
	return(cleanup(0, info));
}

char	*
DtDtsFileToDataType(const char *filepath)
{
	char *dt = DtDtsDataToDataType(filepath, 0, -1, 0, 0, 0, 0);
	return(dt);
}

char	*
DtDtsBufferToDataType(const void *buffer, const int size, const char *name)
{
	char *dt = DtDtsDataToDataType(0, buffer, size, 0, 0, 0, name);
	return(dt);
}

/*
 * Add the first n bytes of value to the string in *buf, which holds *used
 * bytes and has room for *size.  With quote the bytes are quoted for the
 * shell as one word.  Returns 0 when memory runs out.
 */
static int
add_text(char **buf, size_t *used, size_t *size,
	const char *value, size_t n, int quote)
{
	size_t	need = *used + n + 1;
	size_t	i;

	if(quote)
	{
		need = *used + 2 + 4 * n + 1;
	}
	if(need > *size)
	{
		size_t	new_size = *size ? *size : 64;
		char	*grown;

		while(new_size < need)
		{
			new_size *= 2;
		}
		grown = (char *)realloc(*buf, new_size);
		if(!grown)
		{
			return(0);
		}
		*buf = grown;
		*size = new_size;
	}
	if(quote)
	{
		(*buf)[(*used)++] = '\'';
		for(i = 0; i < n; i++)
		{
			if(value[i] == '\'')
			{
				memcpy(*buf + *used, "'\\''", 4);
				*used += 4;
			}
			else
			{
				(*buf)[(*used)++] = value[i];
			}
		}
		(*buf)[(*used)++] = '\'';
	}
	else
	{
		memcpy(*buf + *used, value, n);
		*used += n;
	}
	(*buf)[*used] = '\0';
	return(1);
}

/*
 * Replace the keywords %file%, %dir%, %name%, %suffix% and %base% of attr
 * with the parts of the path.  The name is the part after the last slash,
 * the suffix what follows the last dot of the name, and the base the name
 * without that dot and the suffix.  With quote each part is quoted for the
 * shell, so that a file name cannot become more than one word of a command.
 */
static char *
expand_keywords(const char *attr, const char *in_pathname, int quote)
{
	char		*buf = 0;
	size_t		used = 0, size = 0;
	const char	*c;
	const char	*name, *dot;
	size_t		dir_len;

	if(!attr)
	{
		return(0);
	}
	if(!in_pathname)
	{
		return(strdup(attr));
	}
	name = strrchr(in_pathname, '/');
	dir_len = name ? (size_t)(name - in_pathname) : 0;
	name = name ? name + 1 : in_pathname;
	dot = strrchr(name, '.');

	if(!add_text(&buf, &used, &size, "", 0, 0))
	{
		return(0);
	}
	for(c = attr; *c; )
	{
		int	ok = 1;

		if(*c != '%')
		{
			ok = add_text(&buf, &used, &size, c, 1, 0);
			c++;
		}
		else if(!strncmp(c, "%file%", 6))
		{
			ok = add_text(&buf, &used, &size, in_pathname,
					strlen(in_pathname), quote);
			c += 6;
		}
		else if(!strncmp(c, "%dir%", 5))
		{
			/* a name without a directory part lives in "." and a
			 * name in the root directory has "/" for its directory */
			if(!name || name == in_pathname)
			{
				ok = add_text(&buf, &used, &size, ".", 1, quote);
			}
			else if(dir_len == 0)
			{
				ok = add_text(&buf, &used, &size, "/", 1, quote);
			}
			else
			{
				ok = add_text(&buf, &used, &size, in_pathname,
						dir_len, quote);
			}
			c += 5;
		}
		else if(!strncmp(c, "%name%", 6))
		{
			ok = add_text(&buf, &used, &size, name, strlen(name),
					quote);
			c += 6;
		}
		else if(!strncmp(c, "%suffix%", 8))
		{
			/* a name without a suffix has the empty one */
			ok = dot ? add_text(&buf, &used, &size, dot + 1,
					strlen(dot + 1), quote)
				 : add_text(&buf, &used, &size, "", 0, quote);
			c += 8;
		}
		else if(!strncmp(c, "%base%", 6))
		{
			size_t	n = dot ? (size_t)(dot - name) : strlen(name);

			ok = add_text(&buf, &used, &size, name, n, quote);
			c += 6;
		}
		else
		{
			/* no match -- just copy the character */
			ok = add_text(&buf, &used, &size, c, 1, 0);
			c++;
		}
		if(!ok)
		{
			free(buf);
			return(0);
		}
	}
	return(buf);
}

static char *
expand_keyword(const char *attr_in, const char *in_pathname)
{
	return(expand_keywords(attr_in, in_pathname, 0));
}

/*
 * Append add to old, which is consumed: it is freed if it cannot be
 * enlarged.  Returns the new string, or NULL when memory ran out.
 */
char *
append(char *old, char *add)
{
	char	*new;

	if(old)
	{
		new = realloc(old, strlen(old)+strlen(add)+1);
		if(!new)
		{
			free(old);
			return(NULL);
		}
	}
	else
	{
		new = malloc(strlen(add)+1);
		if(!new)
		{
			return(NULL);
		}
		*new = '\0';
	}

	strcat(new, add);

	return(new);
}

/* The output of a command that is put into a value is cut at this length. */
#define DTS_MAX_COMMAND_OUTPUT	65536

/*
 * Replace the commands in backquotes of attr with their output.  The text
 * between the commands is kept as it is.  The keywords of the commands are
 * replaced first, each as one word for the shell, when a path is given;
 * the text that is put in for them is never searched for commands.  A
 * backquote that has no partner stays in the text.
 */
char *
expand_shell(const char *attr, const char *in_pathname)
{
	char	*srch;
	char	*nattr;
	char	*start, *end;
	char	*kept;			/* the first byte not yet in results */
	size_t	size;
	FILE	*fd;
	char	buff[200];
	char	*results = 0;

	if(!attr)
	{
		return(NULL);
	}
	nattr = strdup(attr);
	if(!nattr)
	{
		return(NULL);
	}
	srch = kept = nattr;
	while((start = DtStrchr(srch, '`')) != NULL)
	{
		char	*command;

		if((srch != start) && _is_previous_single(srch, start) && 
		   (*(start-1) == '`'))
		{
			srch = start + 1;
			continue;
		}
		if((end = DtStrchr(&start[1], '`')) == NULL)
		{
			/* no partner: the rest of the text stays as it is */
			break;
		}
		*start = '\0';
		*end = '\0';
		results = append(results, kept);
		if(!results)
		{
			free(nattr);
			return(NULL);
		}

		command = expand_keywords(&start[1], in_pathname, 1);
		if(!command)
		{
			free(nattr);
			free(results);
			return(NULL);
		}
		if((fd = popen(command, "r")) == NULL)
		{
			_DtSimpleError(
				DtProgName, DtError, NULL,
				"%s", command);
			free(command);
			free(nattr);
			free(results);
			return(NULL);
		}
		free(command);

		memset(buff, '\0', sizeof(buff));
		size = 0;
		while(size < DTS_MAX_COMMAND_OUTPUT &&
			fread(buff, 1, sizeof(buff)-1, fd))
		{
			buff[sizeof(buff)-1] = '\0';
			size += strlen(buff);
			results = append(results, buff);
			if(!results)
			{
				pclose(fd);
				free(nattr);
				return(NULL);
			}
			memset(buff, '\0', sizeof(buff));
		}
		pclose(fd);
		srch = kept = end+1;
	}
	results = append(results, kept);
	free(nattr);
	return(results);
}

static char *
expand_value(DtShmBoson attr, char *in_pathname)
{
	char *tmp;
	char *exp;
	char *shell_exp;

	if(attr && attr != -1)
	{
		/*
		 * The commands in the text of the database run with the
		 * keywords quoted; the keywords of the rest of the text and
		 * of what the commands print are replaced afterwards.
		 */
		tmp = _DtDtsMMExpandValue(_DtDtsMMBosonToString(attr));
		shell_exp = expand_shell(tmp, in_pathname);
		_DtDtsMMSafeFree(tmp);
		exp = expand_keyword(shell_exp, in_pathname);
		free(shell_exp);
	}
	else
	{
		exp = NULL;
	}

	return(exp);
}

char	*
DtDtsDataTypeToAttributeValue(const char *obj_type, const char *attr, const char *filename)
{
	DtDtsMMDatabase	*db;
	DtDtsMMRecord	*entry;
	char		*value = 0;

	_DtSvcAppLockDefault();
	_DtSvcProcessLock();
	db = get_da_db();
	if(db && obj_type)
	{
		entry = _DtDtsMMGetRecordByName(db, (char *)obj_type);

		if(entry)
		{
			char	*name = 0;
			DtDtsMMField *fld = _DtDtsMMGetField(entry,(char *)attr);


			if ( fld )
			{
				value = expand_value(fld->fieldValue,
					(char *)filename);
			}
		}
	}
	_DtSvcProcessUnlock();
	_DtSvcAppUnlockDefault();
	return(value);
}

DtDtsAttribute	**
DtDtsDataTypeToAttributeList(const char *obj_type, const char *filename)
{
	DtDtsMMDatabase	*db;
	DtDtsMMRecord	*entry;
	DtDtsMMField	*fld_ptr_list;
	DtDtsMMField	*fld_ptr;
	DtDtsAttribute	**list = NULL;
	int		i;
	int		action_flag = 0;
	int		sort_flag = 0;
	int		found_flag = 0;
	DtDtsMMField	*fld;

	_DtSvcAppLockDefault();
	_DtSvcProcessLock();    
	db = get_da_db();
	entry = (db && obj_type) ?
		_DtDtsMMGetRecordByName(db, (char *)obj_type) : NULL;

	if(!entry)
	{
		_DtSvcProcessUnlock();
		_DtSvcAppUnlockDefault();
		return (list);
	}

	list = (DtDtsAttribute	**)calloc(entry->fieldCount+1, 
						sizeof(DtDtsAttribute *));
	if(!list)
	{
		_DtSvcProcessUnlock();
		_DtSvcAppUnlockDefault();
		return (list);
	}
	fld_ptr_list = _DtDtsMMGetPtr(entry->fieldList);
	for(i = 0; i < entry->fieldCount; i++)
	{
		char	*tmp;
		fld_ptr = &fld_ptr_list[i];

		list[i] = (DtDtsAttribute *)malloc(sizeof(DtDtsAttribute));
		if(!list[i])
		{
			/* free the entries made so far */
			DtDtsFreeAttributeList(list);
			list = NULL;
			break;
		}
		list[i]->name = 
			expand_value(fld_ptr->fieldName,
					 (char *)filename);

		list[i]->value = 
			expand_value(fld_ptr->fieldValue, (char *)filename);
	}
	if(list)
	{
		list[i] = 0;
	}
	_DtSvcProcessUnlock();
	_DtSvcAppUnlockDefault();
	return(list);
}

void
DtDtsFreeAttributeList(DtDtsAttribute **list)
{
	DtDtsAttribute **item = list;
	int i = 0;

	if(list)
	{
		while(list[i])
		{
			free(list[i]->name);
			free(list[i]->value);
			free(list[i]);
			i++;
		}
		free(list);
	}
}

DtDtsAttribute	**
DtDtsFileToAttributeList(const char *filepath)
{
	char	*ot = DtDtsFileToDataType(filepath);
	DtDtsAttribute **al = DtDtsDataTypeToAttributeList(ot, filepath);

	DtDtsFreeDataType(ot);
	return(al);
}

DtDtsAttribute	**
DtDtsBufferToAttributeList(const void *buffer, const int size, const char *name)
{
	char	*ot = DtDtsBufferToDataType(buffer, size, name);
	DtDtsAttribute **al = DtDtsDataTypeToAttributeList(ot, NULL);

	DtDtsFreeDataType(ot);
	return(al);
}

char	*
DtDtsFileToAttributeValue(const char *filepath, const char *attr)
{
	char	*ot = DtDtsFileToDataType(filepath);
	char	*value = DtDtsDataTypeToAttributeValue(ot, attr, filepath);

	DtDtsFreeDataType(ot);
	return(value);
}

char	*
DtDtsBufferToAttributeValue(const void *buffer, const int size, const char *attr, const char *name)
{
	char	*ot = DtDtsBufferToDataType(buffer, size, name);
	char	*value = DtDtsDataTypeToAttributeValue(ot, attr, name);

	DtDtsFreeDataType(ot);
	return(value);
}

void
DtDtsFreeAttributeValue(char *value)
{
	free(value);
}

void
DtDtsFreeDataType(char *datatype)
{
	free(datatype);
}

int
DtDtsDataTypeIsAction(const char *datatype)
{
	char	*val;
	if(val = DtDtsDataTypeToAttributeValue(datatype, "IS_ACTION", NULL))
	{
		DtDtsFreeAttributeValue(val);
		return(1);
	}
	else
	{
		return(0);
	}
}

char **
DtDtsDataTypeNames(void)
{
	DtDtsMMDatabase	*db;
	DtDtsMMRecord	*rec_list;
	DtDtsMMRecord	*rec_ptr;
	int		i = 0, j=0;
	char		**names;

	_DtSvcAppLockDefault();
	_DtSvcProcessLock();    
	db = get_da_db();
	if(!db)
	{
		_DtSvcProcessUnlock();
		_DtSvcAppUnlockDefault();
		return(NULL);
	}

	names = (char **)calloc(db->recordCount+1, sizeof(char *));
	if(!names)
	{
		_DtSvcProcessUnlock();
		_DtSvcAppUnlockDefault();
		return(NULL);
	}

	rec_list = _DtDtsMMGetPtr(db->recordList);
	for(i = 0; i < db->recordCount; i++)
	{
		rec_ptr = &rec_list[i];
		names[j] = strdup((char *)_DtDtsMMBosonToString(rec_ptr->recordName));
		if(!names[j])
		{
			DtDtsFreeDataTypeNames(names);
			names = NULL;
			break;
		}
		j++;
	}
	if(names)
	{
		names[j] = 0;
	}
	_DtSvcProcessUnlock();
	_DtSvcAppUnlockDefault();
	return(names);
}

void
DtDtsRelease(void)
{
}

char **
DtDtsFindAttribute(const char *name, const char *value)
{
	DtDtsMMDatabase	*ot;
	int		i;
	int		j = 0;
	char		**list;
	char		*v;
	DtDtsMMRecord	*rec_ptr;
	DtDtsMMRecord	*rec_ptr_list;

	_DtSvcAppLockDefault();
	_DtSvcProcessLock();    /* To avoid deadlock with DtDtsMMDatabase mutex
				   lock */
	ot = get_da_db();
	if(!ot || ot->recordCount == 0)
	{
		_DtSvcProcessUnlock(); 
		_DtSvcAppUnlockDefault();
		return(NULL);
	}

	/* room for every record and for the terminating null */
	list = (char **)calloc(ot->recordCount+1, sizeof(char *));
	if(!list)
	{
		_DtSvcProcessUnlock();
		_DtSvcAppUnlockDefault();
		return(NULL);
	}
	rec_ptr_list = _DtDtsMMGetPtr(ot->recordList);
	for(i = 0; i < ot->recordCount; i++)
	{
		rec_ptr = &rec_ptr_list[i];
		v = _DtDtsMMExpandValue(_DtDtsMMGetFieldByName(rec_ptr, (char *)name));
		if(v && !strcmp(value, v))
		{
			list[j] = strdup(_DtDtsMMBosonToString(rec_ptr->recordName));
			if(!list[j])
			{
				_DtDtsMMSafeFree(v);
				DtDtsFreeDataTypeNames(list);
				list = NULL;
				break;
			}
			j++;
		}
		_DtDtsMMSafeFree(v);
	}
	if(list)
	{
		list[j] = 0;
	}
	_DtSvcProcessUnlock();
	_DtSvcAppUnlockDefault();
	return(list);
}

void
DtDtsFreeDataTypeNames(char **list)
{
	int	i = 0;

	while(list[i])
	{
		free(list[i]);
		list[i++] = 0;
	}
	free(list);
}

#define	DIR_INFO	"DIR_INFO\n{\n\t%s\t%s\n}\n"
char *
DtDtsSetDataType(const char *filename, const char *datatype_in, const int overide)
{
	int		fsize;
	char		*file;
	int		fd;
	int		size;
	u_char		*buff = NULL;
	struct stat	file_stat;
	char		*datatype = 0;

	fsize = strlen(filename)+strlen(DtDTS_DT_DIR)+2;
	file = (char *)calloc(1,fsize);
	if(!file)
	{
		return(0);
	}
	sprintf(file, "%s/%s", filename, DtDTS_DT_DIR);

	if((fd = open(file, O_EXCL|O_CREAT|O_RDWR, 0644)) != -1)
	{
		int	write_size;

		size = strlen(DIR_INFO);
		size += strlen(DtDTS_DATA_ATTRIBUTES_NAME);
		size += strlen(datatype_in);
		size-=3;
		buff = (u_char *)calloc((size_t)1, size);
		if(buff)
		{
			sprintf((char *)buff, DIR_INFO, DtDTS_DATA_ATTRIBUTES_NAME, datatype_in);

			write_size = write(fd, buff, size);
			if(write_size != size)
			{
			    _DtSimpleError(
					DtProgName, DtError, NULL,
					"%s", (char*) buff);
			}
			datatype = strdup(datatype_in);
		}
		free(buff);
		close(fd);
	}
	else if(overide && (fd = open(file, O_RDWR, 0644)) != -1)
	{
		int	start;
		int	end;
		int	dtsize = strlen(datatype_in);
		int	write_size;

		if(fstat(fd, &file_stat) == 0)
		{
			buff = (u_char *)calloc((size_t)1, file_stat.st_size+1);
			if(buff)
			{
				read(fd, buff, file_stat.st_size);
			}
		}
		if(!buff)
		{
			/* the old contents cannot be examined: leave them alone */
		}
		else if(!dir_datatype_extent(buff, &start, &end))
		{
			free(buff);
			size = strlen(DIR_INFO);
			size += strlen(DtDTS_DATA_ATTRIBUTES_NAME);
			size += dtsize;
			size-=3;
			buff = (u_char *)calloc((size_t)1, size);
			if(buff)
			{
				sprintf((char *)buff, DIR_INFO,
						DtDTS_DATA_ATTRIBUTES_NAME,
						datatype_in);
				write_size = write(fd, buff, size);
				if(write_size != size)
				{
				    _DtSimpleError(
						DtProgName, DtError, NULL,
						"%s", (char*) buff);
				}
				datatype = strdup(datatype_in);
			}
		}
		else
		{
			int	off, total;

			lseek(fd, start, SEEK_SET);
			if(write(fd, datatype_in, dtsize) != dtsize)
			{
			    _DtSimpleError(
					DtProgName, DtError, NULL,
					"%s", (char*) file);
			}
			off = write(fd, &buff[end], strlen((char *)&buff[end])+1);
			if(off != strlen((char *)&buff[end])+1)
			{
			    _DtSimpleError(
					DtProgName, DtError, NULL,
					"%s", (char*) file);
			}
			else
			{
				total = file_stat.st_size-(end-start)+dtsize;
				ftruncate(fd, total);
				datatype = strdup(datatype_in);
			}
		}
		free(buff);
		close(fd);
	}
	else if (access(file, R_OK) == 0)
	{
		datatype = _DtDtsGetDataType(filename);
	}
	else
	{
		datatype = 0;
	}
	free(file);
	return(datatype);
}

static int
srch(const void *a, const void *b)
{
	int results = ((struct list *)a)->boson - ((struct list *)b)->boson;

	if(results == 0)
	{
		results = ((struct list *)a)->rec - ((struct list *)b)->rec;
	}
	return(results);
}

int *
get_name_list(char *name, int *count)
{
	int	boson = name?_DtDtsMMStringToBoson((const char *)name):-1;
	int	*results;
	int	*list;
	int	size;

	*count = 0;
	if(boson == -1)
	{
		return((int *)-1);
	}
	results = (int *)_DtShmFindIntTabEntry(_DtDtsMMGetDCNameIndex(&size), boson);
	if (!results)
	{
		return((int *)-1);
	}
	if(*results < 0)
	{
		list = (int *)_DtDtsMMGetPtr(-(*results));
		*count = _DtDtsMMGetPtrSize(-(*results));
	}
	else
	{
		list = results;
		*count = 1;
	}
	return(list);
}

static DtDtsMMRecord *
name_list(type_info_t *linfo, DtDtsMMDatabase	*db, DtDtsMMRecord *rec_ptr)
{
	int			i;
	char			*src_str;
	char			*name;
	int			size;
	int			isnew;
	DtDtsMMRecord		*record_list;
	char			*suffix = 0;

	if(rec_ptr == 0)
	{
		/* if this is the first time */
		if(linfo->name)
		{
			/* get the name */
			name = linfo->name;
		}
		else
		{
			/* if not name use opt name */
			name = (char *)linfo->opt_name;
		}

		/* initialize name count */
		linfo->name_count = 0;
		if(!name)
		{
			/* a name could not be found so this must be a buffer */
			linfo->name_prev = (int *)
				_DtDtsMMGetBufferIndex(&linfo->name_count);
			linfo->name_type = 3;
		}
		else
		{
			if(name && *name)
			{
				/* now find suffix if any */
				suffix = strrchr(name, '.');
			}
		}

		if(!linfo->name_count && name)
		{
			/* find if name exist in our list */
			linfo->name_prev = get_name_list(name, 
					&linfo->name_count);
			linfo->name_type = 1;
		}
		if(!linfo->name_count && suffix)
		{
			/* find if suffix exist in our list */
			linfo->name_prev = get_name_list(suffix, 
					&linfo->name_count);
			linfo->name_type = 2;
		}
		if(!linfo->name_count)
		{
			/* neither exist so check ambugious ones */
			linfo->name_prev = (int *)_DtDtsMMGetNoNameIndex(&linfo->name_count);
			linfo->name_type = 3;
		}
	}
	else
	{
		/* we found one befor so . . . */
		if(linfo->name_count > 0)
		{
			/* if there are more on the list get the next one */
			linfo->name_prev++;
		}
		else
		{
			/* go to the next list */
			switch(linfo->name_type)
			{
			case	1:
				/* if we were in a name portion then we need to
					check for a suffix; a buffer has only
					its optional name */
				suffix = strrchr(linfo->name ? linfo->name :
						linfo->opt_name, '.');
				if(suffix)
				{
					/* if found us it */
					linfo->name_prev = get_name_list(suffix, 
						&linfo->name_count);
					linfo->name_type = 2;
					break;
				}
				/* other wise */
			case	2:
				/* use the ambigous list */
				linfo->name_prev = 
					_DtDtsMMGetNoNameIndex(&linfo->name_count);
				linfo->name_type = 3;
				break;
			case	3:
				/* start now with the buffer list */
				linfo->name_prev = (int *)
					_DtDtsMMGetBufferIndex(&linfo->name_count);
				linfo->name_type = 3;

			default:
				return(0);
			}
		}
	}
	record_list = _DtDtsMMGetPtr(db->recordList);
	if(linfo->name_count == 0)
	{
		return(0);
	}
	else
	{
		linfo->name_count--;
		return(&record_list[*linfo->name_prev]);
	}
}
		
#ifdef NEED_STRCASECMP
/*
 * In case strcasecmp is not provided by the system here is one
 * which does the trick.
 */
static int
strcasecmp(const char *s1,
	   const char *s2)
{
    int c1, c2;

    while (*s1 && *s2) {
	c1 = isupper(*s1) ? tolower(*s1) : *s1;
	c2 = isupper(*s2) ? tolower(*s2) : *s2;
	if (c1 != c2)
	    return (c1 - c2);
	s1++;
	s2++;
    }
    return (int) (*s1 - *s2);
}
#endif

Boolean
DtDtsIsTrue(const char *str)
{
	if (str && ((strcasecmp(str, "true") == 0)
	 || (strcasecmp(str, "yes") == 0)
	 || (strcasecmp(str, "on") == 0)
	 || (strcasecmp(str, "1") == 0)))
	{
		return(TRUE);
	}
	else
	{
		return(FALSE);
	}
}

