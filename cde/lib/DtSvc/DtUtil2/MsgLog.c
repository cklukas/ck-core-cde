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
 * (c) Copyright 1995 Digital Equipment Corporation.
 * (c) Copyright 1995 Hewlett-Packard Company.
 * (c) Copyright 1995 International Business Machines Corp.
 * (c) Copyright 1995 Sun Microsystems, Inc.
 * (c) Copyright 1995 Novell, Inc. 
 * (c) Copyright 1995 FUJITSU LIMITED.
 * (c) Copyright 1995 Hitachi.
 *
 * MsgLog.c - public interfaces for the Message Logging Service
 *
 * NOTE: the cpp define MSGLOG_CLIENT_ONLY is not defined when this
 *       file is compiled for the DtSvc library.  MSGLOG_CLIENT_ONLY
 *       is only defined when an application intends to use these
 *       routines directly because the application does not to build
 *       in a dependecy to the DtSvc library (e.g. dtexec).
 *
 * $TOG: MsgLog.c /main/21 1998/10/26 17:23:21 mgreess $
 *
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define X_INCLUDE_PWD_H
#define X_INCLUDE_TIME_H
#define XOS_USE_XT_LOCKING
#include <X11/Xos_r.h>
#include <sys/param.h>
#include <limits.h>

#include <Dt/DtPStrings.h>
#ifndef MSGLOG_CLIENT_ONLY
# include <Dt/UserMsg.h>
# include <DtSvcLock.h>
#endif /* MSGLOG_CLIENT_ONLY */
#include <Dt/DtGetMessageP.h>
#include <Dt/MsgLog.h>
#include <Dt/MsgLogI.h>

#define MAX_DATE_TIME_STRING 		256
#define DT_ERRORLOG_DEFAULT_MAX_BYTES	(2U * 1024U * 1024U)
#define DT_ERRORLOG_MIN_MAX_BYTES	4096U
#define DT_ERRORLOG_CONF_FILE		"errorlog.conf"
#define DT_ERRORLOG_DEDUP_WINDOW_SEC	5
#define DT_ERRORLOG_DEDUP_BURST_LIMIT	5
#define DT_ERRORLOG_DEDUP_BUF_SIZE	512

/* 
 * Static variables
 */
static const char 	* information_string = nullptr;
static const char 	* stderr_string = nullptr;
static const char 	* debug_string = nullptr;
static const char 	* warning_string = nullptr;
static const char 	* error_string = nullptr;
static const char 	* unknown_string = nullptr;
#ifndef MSGLOG_CLIENT_ONLY
static DtMsgLogHandler 	saved_msglog_handler 	= nullptr;
#endif /* MSGLOG_CLIENT_ONLY */


/*
 * Static constants
 */
static const char	* LOGFILE_NAME		= DtERRORLOG_FILE;
static const char	* TMP_DIR		= "/tmp";
static const char	* OPEN_FLAG		= "a+";
static const int 	SET_NUM 		= 50;

#ifdef CDE_LOGFILES_TOP
static const char	* CDE_VAR_TMP_DIR	= CDE_LOGFILES_TOP ;
#else
static const char	* CDE_VAR_TMP_DIR	= "/var/dt/tmp";
#endif


/*
 * Static function forward declarations
 */
static char * get_file_name (
	const char	* type,
	FILE		** fp,
	const char	* format,
	... );
static char * check_possible_files (
	const char	* type,
	FILE		** fp );
static void initialize_message_strings (void);
static void DtMsgLogDefaultHandler (
	const char *program_name,
	DtMsgLogType msg_type,
	const char *format,
	va_list args);


/*
 * copy_message_string - returns a private copy of the message, or
 *    the untranslated fallback text when memory is exhausted
 */
static const char * copy_message_string (
	const char	* message,
	const char	* fallback)
{
	char		* copy = strdup (message);

	return copy ? copy : fallback;
}

/*
 * initialize_message_string - 
 *
 * Modified: initializes the static message string variables
 *   
 */
static void initialize_message_strings (void)
{
      stderr_string      = copy_message_string (Dt11GETMESSAGE (SET_NUM, 2, "STDERR"), "STDERR");
      debug_string       = copy_message_string (Dt11GETMESSAGE (SET_NUM, 3, "DEBUG"), "DEBUG");
      warning_string     = copy_message_string (Dt11GETMESSAGE (SET_NUM, 4, "WARNING"), "WARNING");
      error_string       = copy_message_string (Dt11GETMESSAGE (SET_NUM, 5, "ERROR"), "ERROR");
      unknown_string     = copy_message_string (Dt11GETMESSAGE (SET_NUM, 6, "UNKNOWN"), "UNKNOWN");
      information_string = copy_message_string (Dt11GETMESSAGE (SET_NUM, 1, "INFORMATION"), "INFORMATION");
}


/*
 * Errorlog policy configuration:
 *   - Automatic trimming is ENABLED by default at 2 MiB
 *     (DT_ERRORLOG_DEFAULT_MAX_BYTES), keeping the newest ~half aligned to a
 *     record boundary so ~/.dt/errorlog cannot grow without bound.
 *   - Disable trimming with DT_ERRORLOG_TRIM=0 (or false/off/no/disable) or
 *     DT_ERRORLOG_MAX_BYTES=0 (or unlimited), or in $HOME/.dt/errorlog.conf
 *     via `trim=false` / `max_bytes=0`.
 *   - Minimum log level defaults to `info` (DtMsgLogDebug is suppressed unless
 *     DT_ERRORLOG_LEVEL=debug or CDE_DEBUG is enabled). Set DT_ERRORLOG_LEVEL
 *     to debug, info, warning, error, or off (or `level=...` in errorlog.conf).
 *   - Burst deduplication suppresses tight-loop repeats of the exact same
 *     message within DT_ERRORLOG_DEDUP_WINDOW_SEC seconds unless
 *     DT_ERRORLOG_DEDUP=0.
 */
typedef enum {
	MSGLOG_FILTER_DEBUG = 0,
	MSGLOG_FILTER_INFO = 1,
	MSGLOG_FILTER_WARNING = 2,
	MSGLOG_FILTER_ERROR = 3,
	MSGLOG_FILTER_OFF = 4
} MsgLogFilterLevel;

typedef struct {
	int			loaded;
	char			home_snapshot[MAXPATHLEN + 1];
	int			has_trim;
	int			trim_enabled;
	int			has_max_bytes;
	size_t			max_bytes;
	int			has_level;
	MsgLogFilterLevel	level;
	int			has_dedup;
	int			dedup_enabled;
} MsgLogFileConf;

static MsgLogFileConf	cached_file_conf = { 0 };

static char		last_dedup_prog[128] = { 0 };
static DtMsgLogType	last_dedup_type = DtMsgLogInformation;
static char		last_dedup_body[DT_ERRORLOG_DEDUP_BUF_SIZE] = { 0 };
static int		last_dedup_valid = 0;
static time_t		last_dedup_time = 0;
static time_t		last_dedup_flush_time = 0;
static unsigned long	last_dedup_repeats = 0;
static unsigned long	last_dedup_suppressed = 0;

static int parse_bool_token (const char *s, int *out)
{
	if (!s) return 0;
	while (*s && isspace((unsigned char)*s)) s++;
	if (*s == '\0') return 0;
	if (strcasecmp(s, "1") == 0 || strcasecmp(s, "true") == 0 ||
	    strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0 ||
	    strcasecmp(s, "enable") == 0 || strcasecmp(s, "enabled") == 0) {
		*out = 1;
		return 1;
	}
	if (strcasecmp(s, "0") == 0 || strcasecmp(s, "false") == 0 ||
	    strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0 ||
	    strcasecmp(s, "disable") == 0 || strcasecmp(s, "disabled") == 0 ||
	    strcasecmp(s, "none") == 0) {
		*out = 0;
		return 1;
	}
	return 0;
}

static int parse_size_token (const char *s, size_t *out)
{
	char		*end = nullptr;
	unsigned long long val;
	unsigned long long mult = 1ULL;

	if (!s) return 0;
	while (*s && isspace((unsigned char)*s)) s++;
	if (*s == '\0') return 0;

	if (strcasecmp(s, "0") == 0 || strcasecmp(s, "off") == 0 ||
	    strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0 ||
	    strcasecmp(s, "none") == 0 || strcasecmp(s, "unlimited") == 0 ||
	    strcasecmp(s, "disable") == 0 || strcasecmp(s, "disabled") == 0) {
		*out = 0;
		return 1;
	}
	if (*s == '-') return 0;

	errno = 0;
	val = strtoull(s, &end, 10);
	if (errno != 0 || end == s) return 0;

	while (*end && isspace((unsigned char)*end)) end++;
	if (*end != '\0') {
		if (strcasecmp(end, "k") == 0 || strcasecmp(end, "kb") == 0 ||
		    strcasecmp(end, "kib") == 0) {
			mult = 1024ULL;
		} else if (strcasecmp(end, "m") == 0 || strcasecmp(end, "mb") == 0 ||
			   strcasecmp(end, "mib") == 0) {
			mult = 1024ULL * 1024ULL;
		} else if (strcasecmp(end, "g") == 0 || strcasecmp(end, "gb") == 0 ||
			   strcasecmp(end, "gib") == 0) {
			mult = 1024ULL * 1024ULL * 1024ULL;
		} else if (strcasecmp(end, "b") == 0) {
			mult = 1ULL;
		} else {
			return 0;
		}
	}

	if (val == 0ULL) {
		*out = 0;
		return 1;
	}
	if (val > (unsigned long long)(SIZE_MAX / mult))
		*out = SIZE_MAX;
	else
		*out = (size_t)(val * mult);

	if (*out > 0 && *out < DT_ERRORLOG_MIN_MAX_BYTES)
		*out = DT_ERRORLOG_MIN_MAX_BYTES;
	return 1;
}

static int parse_level_token (const char *s, MsgLogFilterLevel *out)
{
	if (!s) return 0;
	while (*s && isspace((unsigned char)*s)) s++;
	if (*s == '\0') return 0;

	if (strcasecmp(s, "debug") == 0 || strcasecmp(s, "trace") == 0 ||
	    strcasecmp(s, "all") == 0 || strcasecmp(s, "verbose") == 0) {
		*out = MSGLOG_FILTER_DEBUG;
		return 1;
	}
	if (strcasecmp(s, "info") == 0 || strcasecmp(s, "information") == 0 ||
	    strcasecmp(s, "default") == 0) {
		*out = MSGLOG_FILTER_INFO;
		return 1;
	}
	if (strcasecmp(s, "warn") == 0 || strcasecmp(s, "warning") == 0 ||
	    strcasecmp(s, "stderr") == 0) {
		*out = MSGLOG_FILTER_WARNING;
		return 1;
	}
	if (strcasecmp(s, "err") == 0 || strcasecmp(s, "error") == 0 ||
	    strcasecmp(s, "errors") == 0) {
		*out = MSGLOG_FILTER_ERROR;
		return 1;
	}
	if (strcasecmp(s, "off") == 0 || strcasecmp(s, "none") == 0 ||
	    strcasecmp(s, "silent") == 0 || strcasecmp(s, "quiet") == 0) {
		*out = MSGLOG_FILTER_OFF;
		return 1;
	}
	return 0;
}

static void load_file_conf_locked (void)
{
	const char	*home = getenv("HOME");
	char		conf_path[MAXPATHLEN + 1];
	char		line[256];
	FILE		*cf;
	int		n;

	if (!home || !*home)
		return;
	if (cached_file_conf.loaded &&
	    strcmp(cached_file_conf.home_snapshot, home) == 0)
		return;

	memset(&cached_file_conf, 0, sizeof(cached_file_conf));
	cached_file_conf.loaded = 1;
	snprintf(cached_file_conf.home_snapshot,
		 sizeof(cached_file_conf.home_snapshot), "%s", home);

	n = snprintf(conf_path, sizeof(conf_path), "%s/%s/%s",
		     home, DtPERSONAL_CONFIG_DIRECTORY, DT_ERRORLOG_CONF_FILE);
	if (n < 0 || (size_t)n >= sizeof(conf_path))
		return;

	cf = fopen(conf_path, "r");
	if (!cf)
		return;

	while (fgets(line, sizeof(line), cf) != nullptr) {
		char *p = line;
		char *eq, *key, *val, *endp;
		while (*p && isspace((unsigned char)*p)) p++;
		if (*p == '\0' || *p == '#' || *p == ';') continue;
		eq = strchr(p, '=');
		if (!eq) continue;
		*eq = '\0';
		key = p;
		endp = eq - 1;
		while (endp >= key && isspace((unsigned char)*endp)) *endp-- = '\0';
		val = eq + 1;
		while (*val && isspace((unsigned char)*val)) val++;
		endp = val + strlen(val);
		while (endp > val && isspace((unsigned char)endp[-1])) *--endp = '\0';

		if (strcasecmp(key, "trim") == 0 ||
		    strcasecmp(key, "errorlogTrim") == 0) {
			int b = 1;
			if (parse_bool_token(val, &b)) {
				cached_file_conf.has_trim = 1;
				cached_file_conf.trim_enabled = b;
			}
		} else if (strcasecmp(key, "max_bytes") == 0 ||
			   strcasecmp(key, "max_size") == 0 ||
			   strcasecmp(key, "errorlogMaxBytes") == 0) {
			size_t sz = 0;
			if (parse_size_token(val, &sz)) {
				cached_file_conf.has_max_bytes = 1;
				cached_file_conf.max_bytes = sz;
			}
		} else if (strcasecmp(key, "level") == 0 ||
			   strcasecmp(key, "errorlogLevel") == 0) {
			MsgLogFilterLevel lv = MSGLOG_FILTER_INFO;
			if (parse_level_token(val, &lv)) {
				cached_file_conf.has_level = 1;
				cached_file_conf.level = lv;
			}
		} else if (strcasecmp(key, "dedup") == 0 ||
			   strcasecmp(key, "errorlogDedup") == 0) {
			int b = 1;
			if (parse_bool_token(val, &b)) {
				cached_file_conf.has_dedup = 1;
				cached_file_conf.dedup_enabled = b;
			}
		}
	}
	fclose(cf);
}

static void get_effective_msglog_policy (
	size_t			*max_bytes_out,
	MsgLogFilterLevel	*level_out,
	int			*dedup_out)
{
	int			trim_on = 1;
	size_t			max_bytes = DT_ERRORLOG_DEFAULT_MAX_BYTES;
	MsgLogFilterLevel	level = MSGLOG_FILTER_INFO;
	int			dedup_on = 1;
	const char		*env;

#ifndef MSGLOG_CLIENT_ONLY
	_DtSvcProcessLock();
#endif
	load_file_conf_locked();
	if (cached_file_conf.has_trim)
		trim_on = cached_file_conf.trim_enabled;
	if (cached_file_conf.has_max_bytes)
		max_bytes = cached_file_conf.max_bytes;
	if (cached_file_conf.has_level)
		level = cached_file_conf.level;
	if (cached_file_conf.has_dedup)
		dedup_on = cached_file_conf.dedup_enabled;
#ifndef MSGLOG_CLIENT_ONLY
	_DtSvcProcessUnlock();
#endif

	if ((env = getenv("CDE_DEBUG")) != nullptr && *env != '\0' &&
	    strcmp(env, "0") != 0)
		level = MSGLOG_FILTER_DEBUG;

	if ((env = getenv("DT_ERRORLOG_LEVEL")) != nullptr && *env != '\0')
		(void) parse_level_token(env, &level);

	if ((env = getenv("DT_ERRORLOG_TRIM")) != nullptr && *env != '\0')
		(void) parse_bool_token(env, &trim_on);

	if ((env = getenv("DT_ERRORLOG_MAX_BYTES")) != nullptr && *env != '\0')
		(void) parse_size_token(env, &max_bytes);
	else if ((env = getenv("DT_ERRORLOG_MAX_SIZE")) != nullptr && *env != '\0')
		(void) parse_size_token(env, &max_bytes);

	if ((env = getenv("DT_ERRORLOG_DEDUP")) != nullptr && *env != '\0')
		(void) parse_bool_token(env, &dedup_on);

	if (!trim_on)
		max_bytes = 0;

	if (max_bytes_out) *max_bytes_out = max_bytes;
	if (level_out) *level_out = level;
	if (dedup_out) *dedup_out = dedup_on;
}

static int msglog_type_allowed (
	DtMsgLogType		msg_type,
	MsgLogFilterLevel	min_level)
{
	switch (min_level) {
		case MSGLOG_FILTER_DEBUG:
			return 1;
		case MSGLOG_FILTER_INFO:
			return (msg_type != DtMsgLogDebug);
		case MSGLOG_FILTER_WARNING:
			return (msg_type == DtMsgLogStderr ||
				msg_type == DtMsgLogWarning ||
				msg_type == DtMsgLogError);
		case MSGLOG_FILTER_ERROR:
			return (msg_type == DtMsgLogError);
		case MSGLOG_FILTER_OFF:
		default:
			return 0;
	}
}

static int write_all_fd (int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t nw = write(fd, buf + off, len - off);
		if (nw < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		off += (size_t)nw;
	}
	return 0;
}

/*
 * msglog_trim_path_if_needed - trims an oversized errorlog in place while
 *    preserving its inode and keeping the newest ~half aligned to the next
 *    record boundary. Opens a separate non-O_APPEND descriptor so writes at
 *    offset 0 work reliably on Linux and all POSIX systems.
 */
static void msglog_trim_path_if_needed (
	const char	*path,
	size_t		max_bytes)
{
	struct stat	st;
	struct flock	fl;
	int		fd;
	size_t		reserve_hdr = 256U;
	size_t		keep_target;
	off_t		tail_off;
	size_t		to_read, got, start_idx, kept_len;
	char		*tail_buf;
	char		hdr[256];
	int		hdr_len;

	if (!path || max_bytes == 0)
		return;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
	    (uintmax_t)st.st_size <= (uintmax_t)max_bytes)
		return;

	fd = open(path, O_RDWR);
	if (fd < 0)
		return;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	while (fcntl(fd, F_SETLKW, &fl) == -1 && errno == EINTR) {
		/* retry lock acquisition on signal */
	}

	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
	    (uintmax_t)st.st_size <= (uintmax_t)max_bytes) {
		fl.l_type = F_UNLCK;
		(void) fcntl(fd, F_SETLK, &fl);
		close(fd);
		return;
	}

	keep_target = max_bytes / 2U;
	if (keep_target + reserve_hdr > max_bytes)
		keep_target = (max_bytes > reserve_hdr) ? (max_bytes - reserve_hdr) : (max_bytes / 2U);
	if (keep_target == 0)
		keep_target = 1;

	tail_off = (st.st_size > (off_t)keep_target) ? (st.st_size - (off_t)keep_target) : 0;
	to_read = (size_t)(st.st_size - tail_off);
	tail_buf = malloc(to_read + 1U);
	if (!tail_buf) {
		/* Fallback under extreme memory pressure: truncate directly */
		(void) ftruncate(fd, 0);
		fl.l_type = F_UNLCK;
		(void) fcntl(fd, F_SETLK, &fl);
		close(fd);
		return;
	}

	got = 0;
	if (lseek(fd, tail_off, SEEK_SET) != (off_t)-1) {
		while (got < to_read) {
			ssize_t nr = read(fd, tail_buf + got, to_read - got);
			if (nr < 0) {
				if (errno == EINTR) continue;
				break;
			}
			if (nr == 0) break;
			got += (size_t)nr;
		}
	}
	tail_buf[got] = '\0';

	start_idx = 0;
	if (tail_off > 0 && got > 0) {
		size_t search_limit = got / 2U;
		char *rec = strstr(tail_buf, "\n*** ");
		if (rec && (size_t)(rec - tail_buf) <= search_limit) {
			start_idx = (size_t)(rec - tail_buf) + 1U;
		} else {
			char *nl = memchr(tail_buf, '\n', search_limit);
			if (nl)
				start_idx = (size_t)(nl - tail_buf) + 1U;
		}
	}
	kept_len = (got > start_idx) ? (got - start_idx) : 0;

	hdr_len = snprintf(hdr, sizeof(hdr),
			   "*** INFORMATION(0): libDtSvc: PID %ld: "
			   "Trimmed errorlog from %lld to %zu bytes "
			   "(max %zu bytes; set DT_ERRORLOG_TRIM=0 to disable)\n\n",
			   (long)getpid(), (long long)st.st_size,
			   kept_len, max_bytes);
	if (hdr_len < 0)
		hdr_len = 0;
	else if ((size_t)hdr_len >= sizeof(hdr))
		hdr_len = (int)(sizeof(hdr) - 1U);

	if (lseek(fd, 0, SEEK_SET) != (off_t)-1 &&
	    write_all_fd(fd, hdr, (size_t)hdr_len) == 0 &&
	    write_all_fd(fd, tail_buf + start_idx, kept_len) == 0) {
		(void) ftruncate(fd, (off_t)((size_t)hdr_len + kept_len));
	}

	free(tail_buf);
	fl.l_type = F_UNLCK;
	(void) fcntl(fd, F_SETLK, &fl);
	close(fd);
}


/*
 * get_file_name - given a sprintf-like format and a variable
 *    list of args, create a filename and open the file.  
 *
 * Modified:
 *     
 *      fp		- set to the opened file or NULL if the open 
 *			  fails
 *
 * Returns: a filename or NULL if the filename cannot be opened
 *    with mode 'type'.
 */
static char * get_file_name (
	const char	* type,
	FILE		** fp,		/* MODIFIED */
	const char	* format,
	... )
{
	char		*file;
	va_list		args;
	int		n;
	size_t		max_bytes = 0;

	file = malloc(MAXPATHLEN+1);
	if (! file) return nullptr;

	Va_start (args, format);

	n = vsnprintf (file, MAXPATHLEN+1, format, args);
	va_end (args);

	if (n < 0 || n > MAXPATHLEN)
	{
	    /* the name is not a usable path */
	    free(file);
	    return (nullptr);
	}

	get_effective_msglog_policy(&max_bytes, nullptr, nullptr);
	if (max_bytes > 0)
		msglog_trim_path_if_needed(file, max_bytes);

	if ((*fp = fopen (file, type)) == nullptr)
        {
            free(file);
            return (nullptr);
        }

	return file;
}


/*
 * check_possible_files - generates possible filenames to use for
 *    the message logging.
 *
 *    The first one of the following files that is append'able is
 *    returned:
 *
 *       o $HOME/DtPERSONAL_CONFIG_DIRECTORY/LOGFILE_NAME
 *
 *       o CDE_VAR_TMP_DIR/$DTUSERSESSION/LOGFILE_NAME
 *
 *       o TMP_DIR/<login_name_from_passwd_file>/LOGFILE_NAME
 *
 * Note: #2 is only checked if $DTUSERSESSION is defined
 *
 * Modified:
 *
 *	fp		- set to the opened file or NULL if the open 
 *			  fails
 *
 * Returns: a filename if one if found that is append'able or NULL
 *    if such a file cannot be determined.
 */
static char * check_possible_files (
	const char	* type,
	FILE		** fp )		/* MODIFIED */
{
	char		* file;
	char		* env;
	_Xgetpwparams	pwd_buf;
	struct passwd	* pwd_ret;

	if ((env = getenv("HOME")) != nullptr && *env != '\0') {
		if ((file = get_file_name (type,
					   fp,
					   "%s/%s/%s",
					   env,
					   DtPERSONAL_CONFIG_DIRECTORY,
					   LOGFILE_NAME)) != nullptr)
			return (file);
	}

	if ((env = getenv ("DTUSERSESSION")) != nullptr) {
		if ((file = get_file_name (type,
					   fp,
					   "%s/%s/%s",
					   CDE_VAR_TMP_DIR,
					   env,
					   LOGFILE_NAME)) != nullptr)
			return (file);
	}

	if ((env = getenv ("LOGNAME")) != nullptr) {
		if ((file = get_file_name (type,
					   fp,
					   "%s/%s.%s",
					   TMP_DIR,
					   env,
					   LOGFILE_NAME)) != nullptr)
			return (file);
	}

	if ((env = getenv ("USER")) != nullptr) {
		if ((file = get_file_name (type,
					   fp,
					   "%s/%s.%s",
					   TMP_DIR,
					   env,
					   LOGFILE_NAME)) != nullptr)
			return (file);
	}

	if ((pwd_ret = _XGetpwuid (getuid(), pwd_buf)) != nullptr) {
		if ((file = get_file_name (type,
					   fp,
					   "%s/%s.%s",
					   TMP_DIR,
					   pwd_ret->pw_name,
					   LOGFILE_NAME)) != nullptr)
			return (file);
	}

	return (nullptr);
}

/*
 * DtMsgLogDefaultHandler -
 *
 * Returns: 0 if the message is successfully logged or 1 if an
 *    error occurs and the message is not logged.
 */
static void DtMsgLogDefaultHandler (
	const char 		* program_name,
	DtMsgLogType		msg_type,
	const char 		* format,
	va_list			args )
{
	FILE 			* fp = nullptr;
	char			* file = nullptr;
	time_t 			now;
	const char		* msg_string;  /* temp msg type string */
	const char		* eff_prog;
	const char		* eff_format;
	int			num_bytes;
	char 			buf[MAX_DATE_TIME_STRING];
#ifdef NLS16
	char			* tmp_format = nullptr;
	_Xltimeparams		localtime_buf;
	struct tm		* current_time;
#else
	_Xctimeparams		ctime_buf;
	char			* result;
#endif
	size_t			max_bytes = DT_ERRORLOG_DEFAULT_MAX_BYTES;
	MsgLogFilterLevel	min_level = MSGLOG_FILTER_INFO;
	int			dedup_on = 1;
	char			body_sample[DT_ERRORLOG_DEDUP_BUF_SIZE];
	int			body_len = -1;
	va_list			args_copy;
	unsigned long		flush_suppressed = 0;
	char			flush_prog[128] = { 0 };
	int			suppress_current = 0;

	if (!format)
		return;

	get_effective_msglog_policy(&max_bytes, &min_level, &dedup_on);
	if (!msglog_type_allowed(msg_type, min_level))
		return;

	if (!information_string) {
#ifndef MSGLOG_CLIENT_ONLY
		_DtSvcProcessLock();
#endif /* MSGLOG_CLIENT_ONLY */
		if (!information_string)
			initialize_message_strings ();
#ifndef MSGLOG_CLIENT_ONLY
		_DtSvcProcessUnlock();
#endif /* MSGLOG_CLIENT_ONLY */
	}

	/*
	 * Need to get a copy of the string in case another 
	 * thread calls catgets and puts a different
	 * string in catgets's static buffer before 
	 * msg_string is output
	 */
	switch (msg_type) {
		case	DtMsgLogInformation:
			msg_string = information_string;
			break;
		case	DtMsgLogStderr:
			msg_string = stderr_string;
			break;
		case	DtMsgLogDebug:
			msg_string = debug_string;
			break;
		case	DtMsgLogWarning:
			msg_string = warning_string;
			break;
		case	DtMsgLogError:
			msg_string = error_string;
			break;
		default:
			msg_string = unknown_string;
			break;
	}

#ifndef MSGLOG_CLIENT_ONLY
	eff_prog = program_name ? program_name : DtProgName;
#else
	eff_prog = program_name ? program_name : "";
#endif
	if (!eff_prog)
		eff_prog = "";

	now = time (nullptr);

#ifdef NLS16
	/*
	 * Need to save format because the next call to catgets
	 * may overwrite it on some platforms (if format itself 
	 * is the result of a call to catgets).
	 */
	tmp_format = strdup ((char *) format);
	eff_format = tmp_format ? tmp_format : format;
#else
	eff_format = format;
#endif

	va_copy(args_copy, args);
	body_len = vsnprintf(body_sample, sizeof(body_sample), eff_format, args_copy);
	va_end(args_copy);

	if (dedup_on && body_len >= 0 && (size_t)body_len < sizeof(body_sample)) {
#ifndef MSGLOG_CLIENT_ONLY
		_DtSvcProcessLock();
#endif
		if (last_dedup_valid &&
		    last_dedup_type == msg_type &&
		    strcmp(last_dedup_prog, eff_prog) == 0 &&
		    strcmp(last_dedup_body, body_sample) == 0 &&
		    now >= last_dedup_time &&
		    (now - last_dedup_time) <= DT_ERRORLOG_DEDUP_WINDOW_SEC) {
			last_dedup_repeats++;
			last_dedup_time = now;
			if (last_dedup_repeats >= DT_ERRORLOG_DEDUP_BURST_LIMIT) {
				last_dedup_suppressed++;
				suppress_current = 1;
				if ((now - last_dedup_flush_time) >= DT_ERRORLOG_DEDUP_WINDOW_SEC) {
					flush_suppressed = last_dedup_suppressed;
					snprintf(flush_prog, sizeof(flush_prog), "%s", last_dedup_prog);
					last_dedup_suppressed = 0;
					last_dedup_flush_time = now;
				}
			}
		} else {
			if (last_dedup_suppressed > 0) {
				flush_suppressed = last_dedup_suppressed;
				snprintf(flush_prog, sizeof(flush_prog), "%s", last_dedup_prog);
			}
			snprintf(last_dedup_prog, sizeof(last_dedup_prog), "%s", eff_prog);
			snprintf(last_dedup_body, sizeof(last_dedup_body), "%s", body_sample);
			last_dedup_type = msg_type;
			last_dedup_valid = 1;
			last_dedup_time = now;
			last_dedup_flush_time = now;
			last_dedup_repeats = 0;
			last_dedup_suppressed = 0;
		}
#ifndef MSGLOG_CLIENT_ONLY
		_DtSvcProcessUnlock();
#endif
	}

	if (suppress_current && flush_suppressed == 0) {
#ifdef NLS16
		free (tmp_format);
#endif
		return;
	}

	/*
	 * Write to stderr if a log file cannot be determined
	 * or if it isn't writeable.
	 */
	if ((fp = DtMsgLogOpenFile (OPEN_FLAG, &file)) == nullptr)
		fp = stderr;

	if (flush_suppressed > 0) {
		fprintf (fp,
			 "*** %s(0): %s: PID %ld: [suppressed %lu identical messages]\n\n",
			 information_string ? information_string : "INFORMATION",
			 flush_prog, (long)getpid(), flush_suppressed);
	}

	if (!suppress_current) {
#ifdef NLS16
		current_time = _XLocaltime(&now, localtime_buf);
		(void) strftime (buf, 
				 MAX_DATE_TIME_STRING, 
				 Dt11GETMESSAGE (48, 1, "%a %b %d %H:%M:%S %Y\n"),
				 current_time);

		num_bytes = fprintf (fp, "*** %s(%d): %s: PID %d: %s", 
				msg_string, msg_type, eff_prog, getpid(), buf);
#else
		result = _XCtime(&now, ctime_buf);
		num_bytes = fprintf (fp, "*** %s(%d): %s: PID %ld: %s", 
				msg_string, msg_type, eff_prog, (long)getpid(), result);
#endif

		if (body_len >= 0 && (size_t)body_len < sizeof(body_sample)) {
			fputs (body_sample, fp);
			num_bytes += body_len;
		} else {
			num_bytes += vfprintf (fp, eff_format, args);
		}
		// JET - extraneous noise, just output the count
		// fprintf (fp, "\n*** [%d]\n\n", num_bytes);
		fprintf (fp, "[%d bytes]\n\n", num_bytes);
	}

#ifdef NLS16
	free (tmp_format);
#endif

	if (fp != stderr) {
		(void) fflush (fp);
		(void) fclose(fp);
		if (file && max_bytes > 0)
			msglog_trim_path_if_needed(file, max_bytes);
	}

  	if (file)
		free (file);
}

void DtMsgLogMessage (
	const char 		* program_name,
	DtMsgLogType		msg_type,
	const char 		* format,
	... )
{
	va_list args;

	Va_start (args, format);
#ifndef MSGLOG_CLIENT_ONLY
	_DtSvcProcessLock();
	if (saved_msglog_handler != nullptr) {
		(*saved_msglog_handler) (program_name ? program_name : DtProgName,
					 msg_type, format, args);
		_DtSvcProcessUnlock();
	} else {
		_DtSvcProcessUnlock();
		DtMsgLogDefaultHandler(program_name, msg_type, format, args);
	}
#else
	DtMsgLogDefaultHandler(program_name, msg_type, format, args);
#endif /* MSGLOG_CLIENT_ONLY */
	va_end (args);
}


#ifndef MSGLOG_CLIENT_ONLY
/*
 * DtMsgLogSetHandler - caches an alternate message logging
 *    handler
 *
 * Modified:
 *
 *    saved_msglog_handler - set to the given handler
 *
 * Returns: if handler is NULL, the default handler is restored;
 *    returns a pointer to the previous handler
 *
 */
DtMsgLogHandler DtMsgLogSetHandler (
	DtMsgLogHandler		handler )
{
	DtMsgLogHandler 	previous_handler;

	_DtSvcProcessLock();
	if (handler == nullptr) {
		if (saved_msglog_handler) {
			previous_handler = saved_msglog_handler;
			saved_msglog_handler = nullptr;
			_DtSvcProcessUnlock();
			return (previous_handler);
		}
		else {
			saved_msglog_handler = nullptr;
			_DtSvcProcessUnlock();
			return DtMsgLogDefaultHandler;
		}
	}

	if (saved_msglog_handler)
		previous_handler = saved_msglog_handler;
	else
		previous_handler = DtMsgLogDefaultHandler;

	saved_msglog_handler = handler;
	_DtSvcProcessUnlock();
	return (previous_handler);
}
#endif /* MSGLOG_CLIENT_ONLY */


/*
 * DtMsgLogOpenFile - opens the logfile 
 *
 * Returns: returns a pointer to the opened logfile; if a logfile
 *    cannot be opened, stderr is returned
 *
 * Modified:  
 *
 *    fp - is set to the opened file
 *
 *    filename_return - will be set to the filename if it
 *       if it is not NULL and a file is opened.  If filename_return
 *       is not NULL and and a file is opened, the calling function
 *       should free the space allocated for the filename.
 */
FILE * DtMsgLogOpenFile (
	const char		* type,
	char			** filename_return)	/* MODIFIED */
{
	FILE		* fp = nullptr;
	char		* pch;

	pch = check_possible_files (type, &fp);

	if (filename_return)
		*filename_return = pch;
	else if (pch)
		free (pch);

	if (!fp)
		fp = stderr;

	return (fp);	
}
