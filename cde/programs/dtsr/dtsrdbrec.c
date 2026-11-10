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
 *   COMPONENT_NAME: austext
 *
 *   FUNCTIONS: main
 *		print_dbrec
 *
 *   ORIGINS: 27
 *
 *
 *   (C) COPYRIGHT International Business Machines Corp. 1994,1995
 *   All Rights Reserved
 *   Licensed Materials - Property of IBM
 *   US Government Users Restricted Rights - Use, duplication or
 *   disclosure restricted by GSA ADP Schedule Contract with IBM Corp.
 */
/************************* DTSRDBREC.C **************************
 * $XConsortium: dtsrdbrec.c /main/6 1996/08/12 12:18:12 cde-ibm $
 * January 1994.
 * Prints out data in a dbrec system record in human readable form.
 *
 * $Log$
 * Revision 2.4  1996/02/01  18:18:18  miker
 * Added DtSrLaJPN2.  Deleted BETA definition.
 *
 * Revision 2.3  1995/10/25  19:49:12  miker
 * Renamed from dbrec.c.  Added prolog.c.
 *
 * Log: dbrec.c,v
 * Revision 2.2  1995/10/19  20:31:07  miker
 * No longer necessary to rename d00 file.
 * Database files may be read-only.
 *
 * Revision 2.1  1995/09/22  19:34:56  miker
 * Freeze DtSearch 0.1, AusText 2.1.8
 *
 * Revision 1.12  1995/09/19  21:51:43  miker
 * ifdef DTSEARCH, use DtSrVERSION instead of AUSAPI_VERSION in banner.
 *
 * Revision 1.11  1995/08/31  22:24:50  miker
 * Added report of or_language and other minor changes for DtSearch.
 * DtSearch executable (without semantic report) renamed dtsrdbrec.
 */
#include "SearchP.h"

#define PROGNAME	"DTSRDBREC"
#define MS_misc		1
#define MS_dbrec	23

/**#define MAIN_PROGRAM ***/

/* print_dbrec() is the library's (lib/DtSearch/dtsrdbrec.c): the tool and the
 * engine had a copy each, and the tool's own took the place of the library's
 * in the program. */

#ifdef MAIN_PROGRAM
#include <locale.h>
#include <fcntl.h>
#include "vista.h"
/************************************************/
/*                                              */
/*                    main			*/
/*                                              */
/************************************************/
int	main (int argc, char *argv[])
{
    struct or_dbrec dbrec;
    char            renamebuf[256];
    time_t          now;

    aa_argv0 = argv[0];
    setlocale (LC_ALL, "");
    dtsearch_catd = CATOPEN(FNAME_DTSRCAT, 0);
    austools_catd = CATOPEN(FNAME_AUSCAT, 0);

    time (&now);
    strftime (renamebuf, sizeof (renamebuf),
	CATGETS(dtsearch_catd, MS_misc, 22, "%A, %b %d %Y, %I:%M %p"),
	localtime (&now));
    printf (CATGETS(dtsearch_catd, MS_misc, 23,
	"%s: Version %s.  Run %s.\n"),
	aa_argv0,
	DtSrVERSION,
	renamebuf);

    if (argc < 2) {
	printf (CATGETS(dtsearch_catd, MS_dbrec, 310,
		"USAGE: %s <dbname>\n"), aa_argv0);
	return 2;
    }
    snprintf(renamebuf, sizeof(renamebuf), "%s.d00", argv[1]);
    db_oflag = O_RDONLY;	/* db files may be read-only */
    d_open (argv[1], "o");
    if (db_status != S_OKAY) {
	printf (CATGETS(dtsearch_catd, MS_dbrec, 330,
		"Could not open '%s' database.\n%s\n"),
		argv[1], vista_msg(PROGNAME"293"));
	return 3;
    }
    d_recfrst (OR_DBREC, 0);
    if (db_status != S_OKAY) {
	printf (CATGETS(dtsearch_catd, MS_dbrec, 340,
		"No dbrec record in database '%s'.\n"),
		argv[1]);
	return 4;
    }
    d_recread (&dbrec, 0);
    if (db_status != S_OKAY) {
	printf (CATGETS(dtsearch_catd, MS_dbrec, 350,
		"Can't read dbrec record in database '%s'.\n%s\n"),
		argv[1], vista_msg(PROGNAME"306"));
	return 5;
    }
    swab_dbrec (&dbrec, NTOH);
    print_dbrec (argv[1], &dbrec);
    return 0;
}  /* main() */

#endif

/************************* DTSRDBREC.C **************************/
