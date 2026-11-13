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
/* $TOG: main.C /main/7 1998/10/26 17:19:37 mgreess $ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include "dmx.hh"
#include <locale.h>
#include <sys/param.h>
// #include <Dt/DtNlUtils.h>
#ifdef __cplusplus
extern "C" {
#endif
    void Dt_nlInit( void );
#ifdef __cplusplus
}
#endif
#include <Dt/EnvControlP.h>
#include <Dt/DtPStrings.h>

/*
 * Create the empty file "<dir>/<name_template>" with mkstemp(3) and return its
 * open descriptor (mode 0600, owned by the caller), with the name in malloc'ed
 * storage in *name.  The file exists when this returns, so no other user can
 * plant a file or a symbolic link at the name before it is opened.  If dir is
 * NULL, empty or not writable, $TMPDIR or the system default directory is
 * used.  name_template ends in "XXXXXX".  Returns -1 if no file can be made.
 */
static int
create_temp_file (const char *dir, const char *name_template, char **name)
{
	const char	*dirs[3];
	char		path[MAXPATHLEN];
	int		i, len, fd;

	dirs[0] = dir;
	dirs[1] = getenv ("TMPDIR");
	dirs[2] = P_tmpdir;

	for (i = 0; i < 3; i++)
	{
		if (dirs[i] == NULL || *dirs[i] == '\0')
			continue;
		len = snprintf (path, sizeof (path), "%s/%s", dirs[i],
				name_template);
		if (len < 0 || (size_t) len >= sizeof (path))
			continue;
		fd = mkstemp (path);
		if (fd == -1)
			continue;
		*name = strdup (path);
		if (*name == NULL)
		{
			close (fd);
			unlink (path);
			return -1;
		}
		return fd;
	}
	return -1;
}

int
main (int argc, char **argv)
{
	// parse command-line options
	int	c;
	extern char *optarg;
	extern int optind;
	char	*ffile = NULL;
	int	aflag = 0, pgflag = 0;
	int	errflag = 0;

	/*
	 * To make DtDts*() function correctly, we have to call
	 * DtInitialize() or DtAppInitialize(). But they require Widget....
	 * Instead, just call Dt_nlInit() that is an internal function of
	 * libDtSvc.a. This is a temporary hack.....
	 */
	_DtEnvControl(DT_ENV_SET);
	setlocale( LC_ALL, "" );
	Dt_nlInit();

	while ((c = getopt(argc, argv, "f:aph?")) != EOF)
		switch (c)
		{
			case 'p':
				pgflag++;
				//printf ("Print each message on its own page\n");
				break;
			case 'a':
				aflag++;
				//printf ("Strip attachments\n");
				break;
			case 'f':
				ffile = optarg;
				//printf ("Input file is: %s\n", ffile);
				break;
			case '?':
			case 'h':
				errflag++;
				break;
			default:	
				errflag++;
		}
	if (errflag)
	{
		fprintf(stderr, "usage: %s [-p] [-a] [-f <filename>] \n", argv[0]);
		exit (1);
	}

	//for ( ; optind < argc; optind++)
		//(void)printf("%s\n", argv[optind]);
	

	// create DtMail session

	DtMailEnv		dmxenv;
	DtMail::Session		*session;

	DmxMailbox		mbox;

	session = new DtMail::Session (dmxenv, "dtmailpr");


	if (handleError (dmxenv, "new session") == B_TRUE)
		exit (1);

	if (session == NULL)
	{
		fprintf (stderr, "Error opening session...exiting.\n");
		exit (1);
	}

	// initialize typing system (will go away eventually)
	DtDtsLoadDataTypes ();

	// temporary hack, until I'm sure that buffer objects are working
	char buf [BUFSIZ];
	int n = 0;
	char 	*name;
	FILE	*msgFile=NULL;

	if (ffile == NULL)
	{
		char	tmpdir[MAXPATHLEN+1];
		char	*home = getenv("HOME");
		int	fd;

		if (home != NULL && *home != '\0')
			snprintf(
				tmpdir,
				sizeof (tmpdir),
				"%s/%s",
				home,
				DtPERSONAL_TMP_DIRECTORY);
		else
			tmpdir[0] = '\0';

		fd = create_temp_file (tmpdir, "dtmprXXXXXX", &name);
		if (fd == -1 || (msgFile = fdopen (fd, "w+")) == NULL)
		{
			perror ("tmpfile");
			exit (1);
		}

		while ( (n = read (fileno (stdin), buf, BUFSIZ)) > 0)
		{
			if (write (fileno (msgFile), buf, n) != n)
			{
				perror ("write");
				exit (1);
			}
		}

		if (n < 0)
		{
			perror ("read");
		}
	
	        fclose (msgFile);

	} else {
		name = ffile;
	}
	DtMail::MailBox		*mailbox = NULL;

	// try to construct mbox
	mailbox = session->mailBoxConstruct (
				dmxenv,
				DtMailFileObject,
				name,
				NULL,
				NULL,
				NULL);

	if (handleError (dmxenv, "new DtMail::MailBox") == B_TRUE)
		exit (1);

	mbox.name = new char [strlen (name) +1];
	strcpy (mbox.name, name);

	mbox.mbox = mailbox;
	mbox.loadMessages ();

	int m = 0;

	for (m = 1; m <= mbox.messageCount; m++)
	{
		mbox.msg[m].getFlags ();
		mbox.msg[m].display ();
		if (m < mbox.messageCount) {
			if (pgflag) {
				printf ("");
			} else {
				printf ("\n\n");
			}
		}
	}

	return 0;
}
