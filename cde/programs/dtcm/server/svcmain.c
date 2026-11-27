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
/* $TOG: svcmain.c /main/10 1998/04/06 13:13:49 mgreess $ */
/*
 *  (c) Copyright 1993, 1994 Hewlett-Packard Company
 *  (c) Copyright 1993, 1994 International Business Machines Corp.
 *  (c) Copyright 1993, 1994 Novell, Inc.
 *  (c) Copyright 1993, 1994 Sun Microsystems, Inc.
 */

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <EUSCompat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#ifdef SVR4
#ifndef _NETINET_IN_H
#include <netinet/in.h>
#endif /* _NETINET_IN_H */
#endif

#if defined(SunOS)
#include <netconfig.h>
#include <netdir.h>
#include <sys/stropts.h>
#include <tiuser.h>
#elif defined(OPT_TIRPC)
#include <netconfig.h>
#include <rpc/rpcb_clnt.h>
#endif /* SunOS */
#include <sys/param.h>
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <sys/ucred.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <rpc/rpc.h>
#include <sys/file.h>
#include <sys/signal.h>
#include <pwd.h>
#include <grp.h>
#include "rpcextras.h"
#include "log.h"
#include "cmscalendar.h"
#include "repeat.h"
#include "lutil.h"
#include "cmsdata.h"
#include "connection.h"
#include "cmsloop.h"

#ifndef	S_IRWXU
#define	S_IRWXU		(S_IRUSR|S_IWUSR|S_IXUSR)
#endif
#ifndef	S_IRWXG
#define	S_IRWXG		(S_IRGRP|S_IWGRP|S_IXGRP)
#endif
#ifndef	S_IRWXO
#define	S_IRWXO		(S_IROTH|S_IWOTH|S_IXOTH)
#endif

#define S_MASK  (S_INPUT|S_HIPRI|S_ERROR|S_HANGUP)

#define _DtCMS_DEFAULT_UNIX_SOCK "/run/rpc.cmsd/cmsd.sock"

int debug;
static int standalone;			/* default is 0 */
static int network_bind;		/* default 0: loopback-only; 1 (-n): all interfaces */
static int unix_only;			/* 1 (-u): AF_UNIX only, no TCP/UDP/rpcbind */
static const char *unix_sock_path = _DtCMS_DEFAULT_UNIX_SOCK;
static int garbage_collection_time = 240; /* in min; default time is 4:00am */

char	*pgname;

uid_t daemon_uid;
gid_t daemon_gid;

/*
 * get garbage collection time
 * the given string should be in the format hhmm
 * where hh is 0 - 23 and mm is 00 - 59
 */
static int
_GetGtime(char *timestr)
{
	int	hour, minute, len, i;

	if (timestr == nullptr)
		goto error;

	if ((len = strlen(timestr)) > 4)
		goto error;

	for (i = 0; i < len; i++) {
		if (timestr[i] < '0' || timestr[i] > '9')
			goto error;
	}

	minute = atoi(&timestr[len - 2]);
	timestr[len - 2] = '\0';
	hour = atoi(timestr);

	if (hour > 23 || minute > 59)
		goto error;

	garbage_collection_time = hour * 60 + minute;

	return (0);

error:
	fprintf(stderr, "The time specified is invalid.\n");
	return (-1);
}

static void
parse_args(int argc, char **argv)
{
	int	opt;

	if (pgname = strrchr (argv[0], '/'))
		pgname++;
	else
		pgname = argv[0];

	while ((opt = getopt (argc, argv, "dsg:lnuU:")) != -1)
	{
		switch (opt)
		{
		case 'd':
			debug = 1;
			break;
		case 's':
			standalone = 1;
			break;
		case 'l':
			network_bind = 0;
			unix_only = 0;
			break;
		case 'n':
			network_bind = 1;
			unix_only = 0;
			break;
		case 'u':
			unix_only = 1;
			network_bind = 0;
			break;
		case 'U':
			unix_sock_path = optarg;
			break;
		case 'g':
			if (_GetGtime(optarg))
				goto error;
			break;
		case '?':
			goto error;
		}
	}

	if (optind == argc)
		return;

#if defined(_aix)
	/*
	 * rpc.cmsd gets started by the inetd.
	 * On AIX inetd requires that two arguments be supplied to the RPC
	 * programs as follows (from the inetd.conf man page):
	 *
	 * ServerArgs      Specifies the command line arguments that the
	 * inetd daemon should use to execute the server. The maximum number
	 * of arguments is five. The first argument specifies the name of the
	 * server used.  If the SocketType parameter is sunrpc_tcp or
	 * sunrpc_udp, * the second argument specifies the program name and
	 * the third argument specifies the version of the program. For
	 * services that the inetd daemon provides internally, this field
	 * should be empty.
	 */

	else if (optind == 1 && argc >= 3)
	{
		int i,j;
		char **argv_r;
		
		if (argc == 3)
		  return;
		  
		argv_r = (char **) malloc(argc * sizeof(char *));
		if (argv_r == nullptr) {
			fprintf (stderr, "%s: out of memory\n", pgname);
			exit (-1);
		}

		argv_r[0] = argv[0];
		for (i=optind+2, j=1; i<argc; i++,j++)
		  argv_r[j] = argv[i];
		parse_args(argc-2, argv_r);

		free((void *) argv_r);
		return;
	}
#endif
		
error:
	fprintf (stderr, "Usage: %s [-d] [-s] [-u | -l | -n] [-U unix_sock] [-g hhmm]\n", pgname);
	exit (-1);
}

static void
init_dir(void)
{
	char *dir = _DtCMS_DEFAULT_DIR;
	char msgbuf[BUFSIZ];
	int create_dir;
	struct stat info;
	mode_t mode;

	if (geteuid() != 0)
	{
		fprintf (stderr,
			"%s: must be run in super-user mode!  Exited.\n",
			pgname);
		exit (-1);
	}

	create_dir = 0;
	if (stat(dir, &info))
	{
		/* if directory does not exist, create the directory */
		if ((errno != ENOENT) || mkdir(dir, S_IRWXU|S_IRWXG|S_IRWXO))
		{
			if (errno == ENOENT)
				snprintf(msgbuf, sizeof msgbuf, "%s: cannot create %s.\n%s: %s",
					pgname, dir, pgname, "System error");
			else
				snprintf(msgbuf, sizeof msgbuf, "%s: cannot access %s.\n%s: %s",
					pgname, dir, pgname, "System error");
			perror (msgbuf);
			exit (-1);
		}
		create_dir = 1;
	}

	/* if dir is just created, we need to do chmod and chown.
	 * Otherwise, only do chmod and/or chown if permssion and/or
	 * ownership is wrong.
	 */
	mode = S_ISGID|S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO;

	if (create_dir || info.st_mode != (mode | S_IFDIR)) {

		/* set directory permission to be "rwxrwsrwt" */
		if (chmod(dir, mode)) {
			snprintf(msgbuf, sizeof msgbuf, "%s: Permission on %s%s\n%s%s\n%s%s",
				pgname, dir,
				" is wrong but cannot be corrected.", pgname,
				": This might happen if you are mounting the directory.",
				pgname, ": System error");
			perror(msgbuf);
			if (create_dir)
				rmdir(dir);
			exit(-1);
		}
	}

	if (create_dir || info.st_uid!=daemon_uid || info.st_gid!=daemon_gid) {
		/* set directory ownership to: owner = 1, group = 1 */
		if (chown(dir, daemon_uid, daemon_gid)) {
			snprintf(msgbuf, sizeof msgbuf, "%s: Ownership on %s%s\n%s%s\n%s%s",
				pgname, dir,
				" is wrong but cannot be corrected.", pgname,
				": This might happen if you are mounting the directory.",
				pgname, ": System error");
			perror(msgbuf);
			if (create_dir)
				rmdir(dir);
			exit(-1);
		}
	}

	/* Change current directory, so core file can be dumped. */
	if(-1 == chdir (dir)) {
	        perror(strerror(errno));
	}
}

/*
 * send a SIGHUP signal to the rpc.cmsd that is already running
 */
static void
send_hup(void)
{
	FILE	*fp = nullptr;
	char	buf[BUFSIZ];
	pid_t	pid, mypid = getpid();
	extern FILE *popen(const char *, const char *);
	extern int pclose(FILE *);

	snprintf(buf, sizeof buf, "ps -e|grep rpc.cmsd|grep -v grep");

	if ((fp = popen(buf, "r")) == nullptr) {
		if (debug)
			fprintf(stderr, "rpc.cmsd: popen failed\n");
	} else {
		while (fgets(buf, sizeof(buf), fp) != nullptr) {
			if ((pid = atol(buf)) != mypid) {
				if (kill(pid, SIGHUP))
				    perror("rpc.cmsd: failed to send SIGHUP");
				if (debug)
				    fprintf(stderr, "rpc.cmsd: %s %ld\n",
						"sent SIGHUP to", (long)pid);
			}
		}
		pclose(fp);
	}
}

/*
 * We only allow one rpc.cmsd to run on each machine.
 */
static int
lock_it(void)
{
	char *dir = _DtCMS_DEFAULT_DIR;
	char	buff [MAXPATHLEN];
	int	error;
	int	fd;
#ifdef SVR4
	struct flock locker;
	locker.l_type = F_WRLCK;
	locker.l_whence = 0;
	locker.l_start = 0;
	locker.l_len = 0;
#endif /* SVR4 */

	/* 
	 * /var/spool might be mounted.  Use .lock.hostname to
	 * prevent more than one cms running in each host.
	 */
	snprintf(buff, sizeof(buff), "%s/.lock.%s", dir, _DtCmGetLocalHost());

	fd = open(buff, O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (fd < 0)
	{
		perror (buff);
		exit (-1);
	}

	/*
	 * Note, we have to use flock() instead of lockf() because cms process
	 * is run in each host.
	 */
#ifdef SVR4
	if (fcntl (fd, F_SETLK, &locker) != 0)
#else
	if (flock (fd, LOCK_EX|LOCK_NB) != 0)
#endif /* SVR4 */
	{
		error = errno;

		close(fd);

		if (error != EWOULDBLOCK && error != EACCES) {

			perror ("rpc.cmsd: failed to lock lockfile");
			fprintf(stderr, "error = %d\n", error);
			exit (-1);

		} else {
			if (debug)
				fprintf(stderr, "rpc.cmsd: %s\n",
				    "lock_it failed due to another process");
			
			/* cms has been running.... */
			return(error);
		}
	}

	return (0);
}

static int
create_loopback_socket(int type)
{
	int fd;
	struct sockaddr_in addr;
	int bound_to_lo = 0;

	fd = socket(AF_INET, type, 0);
	if (fd < 0)
		return (-1);

#if defined(__linux__) && defined(SO_BINDTODEVICE)
	/*
	 * Bind to the loopback interface ("lo") so both 127.0.0.1 (localhost)
	 * and 127.0.1.1 (<hostname> on Debian/Ubuntu) remain reachable locally
	 * while external network interfaces are rejected by the kernel.
	 */
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3) == 0)
		bound_to_lo = 1;
#endif

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = bound_to_lo ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return (-1);
	}

	if (type == SOCK_STREAM && listen(fd, SOMAXCONN) < 0) {
		close(fd);
		return (-1);
	}

	return (fd);
}

#if defined(OPT_TIRPC) && !defined(SunOS)
static int
register_loopback_rpcb(SVCXPRT *transp, unsigned long prog, unsigned long vers,
                       const char *netid)
{
	struct netconfig *nconf;
	struct sockaddr_in sin;
	struct netbuf addr;
	bool_t ok;

	nconf = getnetconfigent(netid);
	if (nconf == nullptr)
		return (0);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(transp->xp_port);

	addr.buf = &sin;
	addr.len = sizeof(sin);
	addr.maxlen = sizeof(sin);

	ok = rpcb_set((rpcprog_t)prog, (rpcvers_t)vers, nconf, &addr);
	freenetconfigent(nconf);
	return (ok ? 1 : 0);
}
#endif

static int
caller_is_loopback(SVCXPRT *transp)
{
	const struct sockaddr *sa = (const struct sockaddr *)svc_getcaller(transp);

	if (sa == nullptr)
		return (0);
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
		return ((ntohl(sin->sin_addr.s_addr) & 0xff000000U) == 0x7f000000U);
	}
#ifdef AF_INET6
	if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
		if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr))
			return (1);
		if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
			uint32_t v4;
			memcpy(&v4, &sin6->sin6_addr.s6_addr[12], sizeof(v4));
			return ((ntohl(v4) & 0xff000000U) == 0x7f000000U);
		}
	}
#endif
	return (0);
}

static int
create_unix_socket(const char *path)
{
	int fd;
	struct sockaddr_un sun;
	char dirbuf[MAXPATHLEN];
	char *slash;
	mode_t old_umask;

	if (path == nullptr || path[0] == '\0' || strlen(path) >= sizeof(sun.sun_path))
		return (-1);

	/* Ensure parent directory exists (e.g. /run/rpc.cmsd) */
	snprintf(dirbuf, sizeof(dirbuf), "%s", path);
	slash = strrchr(dirbuf, '/');
	if (slash != nullptr && slash != dirbuf) {
		*slash = '\0';
		if (mkdir(dirbuf, 0755) < 0 && errno != EEXIST)
			return (-1);
		(void)chmod(dirbuf, 0755);
	}

	(void)unlink(path);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return (-1);

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", path);

	old_umask = umask(0);
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		umask(old_umask);
		close(fd);
		return (-1);
	}
	umask(old_umask);

	/* All local users may connect; SO_PEERCRED enforces true UID per call */
	if (chmod(path, 0666) < 0) {
		close(fd);
		(void)unlink(path);
		return (-1);
	}

	if (listen(fd, SOMAXCONN) < 0) {
		close(fd);
		(void)unlink(path);
		return (-1);
	}

	return (fd);
}

/*
 * Check whether transp is an AF_UNIX socket.
 * Returns:
 *   1 if AF_UNIX and peer credentials match (or NULLPROC ping),
 *  -1 if AF_UNIX and peer credentials are forged / unreadable (caller rejected),
 *   0 if not AF_UNIX (caller is TCP/UDP).
 */
static int
verify_unix_peercred(struct svc_req *rqstp, SVCXPRT *transp)
{
	struct sockaddr_storage ss;
	socklen_t sslen = sizeof(ss);

	if (transp == nullptr || transp->xp_fd < 0)
		return (0);

	memset(&ss, 0, sizeof(ss));
	if (getsockname(transp->xp_fd, (struct sockaddr *)&ss, &sslen) != 0)
		return (0);
	if (ss.ss_family != AF_UNIX)
		return (0);

#if defined(__linux__) && defined(SO_PEERCRED)
	{
		struct ucred cred;
		socklen_t credlen = sizeof(cred);

		if (getsockopt(transp->xp_fd, SOL_SOCKET, SO_PEERCRED,
		               &cred, &credlen) != 0)
			return (-1);

		/* Allow NULLPROC (RPC ping) without AUTH_UNIX */
		if (rqstp->rq_proc == NULLPROC)
			return (1);

		if (rqstp->rq_cred.oa_flavor == AUTH_UNIX) {
			struct authunix_parms *aup =
				(struct authunix_parms *)rqstp->rq_clntcred;
			if (aup == nullptr || (uid_t)aup->aup_uid != cred.uid) {
				if (debug)
					fprintf(stderr,
						"rpc.cmsd: rejected spoofed AUTH_UNIX uid=%u (SO_PEERCRED uid=%u pid=%d)\n",
						aup ? (unsigned)aup->aup_uid : 0U,
						(unsigned)cred.uid, (int)cred.pid);
				return (-1);
			}
		}
	}
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
	{
		uid_t peer_uid = (uid_t)-1;
		gid_t peer_gid = (gid_t)-1;
		int peer_pid = -1;
		int have_cred = 0;

#if defined(LOCAL_PEERCRED) && (defined(__FreeBSD__) || defined(__APPLE__))
		struct xucred xcred;
		socklen_t xlen = sizeof(xcred);
#ifndef SOL_LOCAL
#define SOL_LOCAL 0
#endif
		if (getsockopt(transp->xp_fd, SOL_LOCAL, LOCAL_PEERCRED,
		               &xcred, &xlen) == 0 &&
		    xcred.cr_version == XUCRED_VERSION) {
			peer_uid = xcred.cr_uid;
#if defined(__FreeBSD__) && __FreeBSD_version >= 1300000
			peer_pid = (int)xcred.cr_pid;
#endif
			have_cred = 1;
		}
#endif
		if (!have_cred && getpeereid(transp->xp_fd, &peer_uid, &peer_gid) == 0)
			have_cred = 1;
		if (!have_cred)
			return (-1);

		if (rqstp->rq_proc == NULLPROC)
			return (1);

		if (rqstp->rq_cred.oa_flavor == AUTH_UNIX) {
			struct authunix_parms *aup =
				(struct authunix_parms *)rqstp->rq_clntcred;
			if (aup == nullptr || (uid_t)aup->aup_uid != peer_uid) {
				if (debug)
					fprintf(stderr,
						"rpc.cmsd: rejected spoofed AUTH_UNIX uid=%u (LOCAL_PEERCRED uid=%u pid=%d)\n",
						aup ? (unsigned)aup->aup_uid : 0U,
						(unsigned)peer_uid, peer_pid);
				return (-1);
			}
		}
	}
#endif
	return (1);
}

static void
program(struct svc_req *rqstp, SVCXPRT *transp)
{
        char *result;
        char *argument = nullptr;
        program_handle ph = getph();
        struct rpcgen_table *proc;
        int unix_check = verify_unix_peercred(rqstp, transp);

        if (unix_check < 0) {
                svcerr_weakauth(transp);
                return;
        }
        if (unix_check == 0 && !network_bind && !caller_is_loopback(transp)) {
                svcerr_weakauth(transp);
                return;
        }

        /* first do some bounds checking: */
        if (rqstp->rq_vers >= ph->nvers) {
                svcerr_noproc(transp);
                return;
        }
        if (ph->prog[rqstp->rq_vers].nproc == 0) {
                svcerr_noproc(transp);
                return;
        }
        if (rqstp->rq_proc >= ph->prog[rqstp->rq_vers].nproc) {
                svcerr_noproc(transp);
                return;
        }

	if (rqstp->rq_proc == NULLPROC) {
		if (debug) fprintf(stderr, "rpc.cmsd: ping\n");
		(void)svc_sendreply(transp, _DtCm_xdr_void, (caddr_t)nullptr);
		return;
	}

        /* assert - the program number, version and proc numbers are valid */
        proc = &(ph->prog[rqstp->rq_vers].vers[rqstp->rq_proc]);
        argument = (char*)calloc(proc->len_arg, sizeof(char));
        if (argument == nullptr && proc->len_arg != 0) {
                svcerr_systemerr(transp);
                return;
        }
        if (!svc_getargs(transp, proc->xdr_arg, argument)) {
                svcerr_decode(transp);
                /* free what was decoded before the request was rejected */
                (void)svc_freeargs(transp, proc->xdr_arg, argument);
                free(argument);
                return;
        }

        result = (*proc->proc)(argument, rqstp);
        if (result != nullptr && !svc_sendreply(transp, proc->xdr_res, result)) {
                svcerr_systemerr(transp);
        }

        if (!svc_freeargs(transp, proc->xdr_arg, argument)) {
                (void)fprintf(stderr, "unable to free arguments");
                exit(1);
        }
        free(argument);
}

/*
 * garbage_collection_time (in min) is the time to do garbage collection
 * each day
 * This routine returns the difference between the first garbage collection
 * time and now so that the calling routine can set the alarm.
 */
static int
_GetFirstGarbageCollectionTime(void)
{
	int n=0, midnight=0, gtime=0;

	n = time(0);

	/* try today first */
	midnight = next_ndays(n, 0);
	gtime = next_nmins(midnight, garbage_collection_time);

	if (gtime < n) {
		/* the first garbage collection will be done tomorrow */
		midnight = next_ndays(n, 1);
		gtime = next_nmins(midnight, garbage_collection_time);
	}

	return (gtime - n);
}

static void
init_alarm(void)
{
	int next;

	_DtCmsInstallSignalHandlers();

	next = _GetFirstGarbageCollectionTime();
	alarm((unsigned) next);
}

int
main(int argc, char **argv)
{
        u_long version;
        program_handle ph = newph();
	struct passwd *pw;
	struct group *gr;
	struct rlimit rl;
	struct sockaddr_in saddr;
	socklen_t asize = sizeof (saddr);
	SVCXPRT *tcp_transp = (SVCXPRT *)-1;
	SVCXPRT *udp_transp = (SVCXPRT *)-1;
	SVCXPRT *unix_transp = (SVCXPRT *)-1;
	int	fd, error;

#if defined(SunOS)
	struct netconfig *nconf_udp;
	struct netconfig *nconf_tcp;
	struct t_info info;
	char mname[FMNAMESZ+1];
#endif /* SunOS */

	pw = (struct passwd *)getpwnam("daemon");
	gr = (struct group *)getgrnam("daemon");
	if (pw != nullptr) 
		daemon_uid = pw->pw_uid;
	else
		daemon_uid = 1;
	if (gr != nullptr)
		daemon_gid = gr->gr_gid;
	else 
		daemon_gid = 1;

	parse_args(argc, argv);

	/* check to see if we are started by inetd */
	if (getsockname(0, (struct sockaddr *)&saddr, &asize) == 0) {

		standalone = 0;

#if defined(SunOS)
		/* we need a TLI endpoint rather than a socket */
		if (ioctl(0, I_LOOK, mname) != 0) {
			perror("rpc.cmsd: ioctl failed to get module name");
			exit(1);
		}
		if (strcmp(mname, "sockmod") == 0) {
			/* Change socket fd to TLI fd */
			if (ioctl(0, I_POP, 0) || ioctl(0, I_PUSH, "timod")) {
				perror("rpc.cmsd: ioctl I_POP/I_PUSH failed");
				exit(1);
			}
		} else if (strcmp(mname, "timod") != 0) {
			fprintf(stderr, "rpc.cmsd: fd 0 is not timod\n");
			exit(1);
		}

	} else if (t_getinfo(0, &info) == 0) {
		standalone = 0;

#endif /* SunOS */

	} else
		standalone = 1;

	/*
	 * if it is started by inetd, make stderr to be
	 * output to console.
	 */
	if (!standalone) {
		if ((fd = open ("/dev/console", O_WRONLY)) >= 0) {
			if (fd != 2) {
				dup2(fd, 2);
				close (fd);
			}
		}
	}

	/* Set up private directory and switch euid/egid to daemon. */
	umask (S_IWOTH);
	init_dir();

	/* Don't allow multiple cms processes running in the same host. */
	if ((error = lock_it()) != 0 && !standalone) {
		/* we are invoked by inetd but another rpc.cmsd
		 * is alreay running, so send SIGHUP to it
		 */

		send_hup();

		/* try to lock it again */
		if (lock_it() != 0) {
			if (debug)
				fprintf(stderr, "cm: rpc.cmsd is still running\n");
			exit(0);
		}

	} else if (error != 0) {
		fprintf(stderr, "rpc.cmsd: rpc.cmsd is already running.\n");
		exit(0);
	}

#if defined(SunOS)
	/* raise the soft limit of number of file descriptor */
	/* this is to prevent the backend from running out of open file des */
	getrlimit(RLIMIT_NOFILE, &rl);
	rl.rlim_cur = (rl.rlim_max <= 256) ? rl.rlim_max : 256;
	setrlimit(RLIMIT_NOFILE, &rl);
#endif

#if defined(SunOS)
	nconf_udp = getnetconfigent("udp");
	nconf_tcp = getnetconfigent("tcp");

	for (version = 0; version < ph->nvers; version++) {
		/* don't register unsupported versions: */
		if (ph->prog[version].nproc == 0) continue;

		if (standalone) {
			rpcb_unset(ph->program_num, version, nullptr);
			if (debug)
				fprintf(stderr,
					"rpc.cmsd: rpcb_unset for version %ld\n",
					version);
		}

		/* brought up by inetd, use fd 0 which must be a TLI fd */
		if (udp_transp == (SVCXPRT *)-1) {
			udp_transp = svc_tli_create(standalone ? RPC_ANYFD : 0,
				nconf_udp, (struct t_bind*) nullptr, 0, 0); 

			if (udp_transp == nullptr) {
				t_error("rtable_main.c: svc_tli_create(udp)");
				exit(2);
			}
		}

		if (svc_reg(udp_transp, ph->program_num, version, program,
				standalone ? nconf_udp : nullptr) == 0) {
			t_error("rtable_main.c: svc_reg");
			exit(3);
		}

		/* Set up tcp for calls that potentially return */
		/* large amount of data.  This transport is not */
		/* registered with inetd so need to register it */
		/* with rpcbind ourselves.			*/

		rpcb_unset(ph->program_num, version, nconf_tcp);

		if (tcp_transp == (SVCXPRT *)-1) {
			tcp_transp = svc_tli_create(RPC_ANYFD, nconf_tcp,
					(struct t_bind *)nullptr, 0, 0);

			if (tcp_transp == nullptr) {
				t_error("rtable_main.c: svc_til_create(tcp)");
				exit(2);
			}
		}

		if (svc_reg(tcp_transp, ph->program_num, version, program,
				nconf_tcp) == 0) {
			t_error("rtable_main.c: svc_reg(tcp)");
			exit(3);
		}
	}/*for*/

	if (nconf_udp)
		freenetconfigent(nconf_udp);
	if (nconf_tcp)
		freenetconfigent(nconf_tcp);

#else

	for (version = 0; version < ph->nvers; version++) {
		/* don't register unsupported versions: */
		if (ph->prog[version].nproc == 0) continue;

		/* Always provide AF_UNIX listener in standalone mode for
		 * SO_PEERCRED-authenticated local access (no open ports). */
		if (standalone && unix_sock_path != nullptr && unix_sock_path[0] != '\0') {
			if (unix_transp == (SVCXPRT *)-1) {
				int unix_sock = create_unix_socket(unix_sock_path);
				if (unix_sock < 0) {
					perror("rpc.cmsd: cannot bind unix socket");
					if (unix_only)
						exit(1);
					unix_transp = nullptr;
				} else {
#if defined(OPT_TIRPC)
					unix_transp = svc_vc_create(unix_sock, 0, 0);
#else
					unix_transp = svctcp_create(unix_sock, 0, 0);
#endif
					if (unix_transp == nullptr) {
						(void)fprintf(stderr,
							"rtable_main.c: cannot create unix service.\n");
						close(unix_sock);
						if (unix_only)
							exit(1);
					}
				}
			}
			if (unix_transp != nullptr &&
			    !svc_register(unix_transp, ph->program_num, version, program, 0)) {
				(void)fprintf(stderr, "rtable_main.c: unable to register(unix)\n");
				if (unix_only)
					exit(1);
			}
		}

		if (unix_only)
			continue;

		if (standalone)
			(void) pmap_unset(ph->program_num, version);

		if (udp_transp == (SVCXPRT *)-1) {
			int udp_sock = standalone ? RPC_ANYSOCK : 0;
			if (standalone && !network_bind) {
				udp_sock = create_loopback_socket(SOCK_DGRAM);
				if (udp_sock < 0) {
					perror("rpc.cmsd: cannot bind loopback udp socket");
					exit(1);
				}
			}
			udp_transp = svcudp_create(udp_sock
#if defined(_AIX) || defined(__linux__) || defined(CSRG_BASED)
					);
#else
					,0,0);
#endif
			if (udp_transp == nullptr) {
				(void)fprintf(stderr,
				"rtable_main.c: cannot create udp service.\n");
				exit(1);
                	}
		}

#if defined(OPT_TIRPC)
		if (standalone && !network_bind) {
			if (!svc_register(udp_transp, ph->program_num, version, program, 0) ||
			    !register_loopback_rpcb(udp_transp, ph->program_num, version, "udp")) {
				(void)fprintf(stderr, "rtable_main.c: unable to register(udp)\n");
				exit(1);
			}
		} else
#endif
		if (!svc_register(udp_transp, ph->program_num, version, program,
				standalone ? IPPROTO_UDP : 0)) {
			(void)fprintf(stderr, "rtable_main.c: unable to register");
			exit(1);
		}

		/* Set up tcp for calls that potentially return */
		/* large amount of data.  This transport is not */
		/* registered with inetd so need to register it */
		/* with rpcbind ourselves.			*/

		if (tcp_transp == (SVCXPRT *)-1) {
			int tcp_sock = RPC_ANYSOCK;
			if (!network_bind) {
				tcp_sock = create_loopback_socket(SOCK_STREAM);
				if (tcp_sock < 0) {
					perror("rpc.cmsd: cannot bind loopback tcp socket");
					exit(1);
				}
			}
			tcp_transp = svctcp_create(tcp_sock, 0, 0);
			if (tcp_transp == nullptr) {
				(void)fprintf(stderr,
				"rtable_main.c: cannot create tcp service.\n");
				exit(1);
			}
		}

#if defined(OPT_TIRPC)
		if (!network_bind) {
			if (!svc_register(tcp_transp, ph->program_num, version, program, 0) ||
			    !register_loopback_rpcb(tcp_transp, ph->program_num, version, "tcp")) {
				(void)fprintf(stderr, "rtable_main.c: unable to register(tcp)\n");
				exit(1);
			}
		} else
#endif
		if (!svc_register(tcp_transp, ph->program_num, version, program,
				IPPROTO_TCP)) {
			(void)fprintf(stderr, "rtable_main.c: unable to register(tcp)");
			exit(1);
		}
	}

#endif /* SunOS */

#ifndef AIX
	/* never serve requests with the privileges of root */
	if(-1 == setegid (daemon_gid)) {
	        perror(strerror(errno));
	        exit(1);
	}
	if(-1 == seteuid (daemon_uid)) {
	        perror(strerror(errno));
	        exit(1);
	}
#endif /* AIX */

	init_time();
	init_alarm();
	_DtCm_init_hash();

	{
		static const _DtCmsLoopActions actions = {
			_DtCmsCollectGarbage, _DtCmsSwitchDebug
		};

		if (_DtCmsRunServiceLoop(&actions) == 0) {
			/* SIGHUP: another rpc.cmsd takes over */
			if (debug)
				fprintf(stderr,
					"rpc.cmsd: received SIGHUP, exiting\n");
			if (standalone && unix_transp != (SVCXPRT *)-1 &&
			    unix_transp != nullptr && unix_sock_path != nullptr)
				(void)unlink(unix_sock_path);
			exit(0);
		}
	}

	(void)fprintf(stderr, "rpc.cmsd: the wait for requests failed\n");
	return(1);
}

