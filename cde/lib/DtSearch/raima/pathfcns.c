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
/* $XConsortium: pathfcns.c /main/2 1996/05/09 04:13:28 drk $ */
/*
 *   COMPONENT_NAME: austext
 *
 *   FUNCTIONS: con_dbd
 *		con_dbf
 *		d_ctbpath
 *		get_element
 *		isabs
 *		remdrv
 *		remfile
 *
 *   ORIGINS: 157
 *
 *   OBJECT CODE ONLY SOURCE MATERIALS
 */
/*-----------------------------------------------------------------------
   pathfcns.c - Dictionary and file path functions

   This file contains the functions con_dbd and con_dbf which construct
   the full path for the dictionary and data files.  It provides a uniform
   method of dealing with how to construct the full paths given the schema
   path, d_open path, and the paths provided in DBDPATH and DBFPATH.  This
   function does not depend upon the runtime and can be included with
   utilities that do not call the runtime.

   This file also contains the function get_element which extracts a single
   element out of the possibly multiple-element DBDPATH or DBFPATH.  Each
   element is separated by a semacolon.

   AUTHOR:      R.S. Carlson
   DATE:        08-Jun-88
   PROJECT:     db_VISTA 3.10 enhancements

   Copyright (C) 1988 by Raima Corporation
-----------------------------------------------------------------------*/

/* ********************** EDIT HISTORY *******************************

 SCR    DATE    INI                   DESCRIPTION
----- --------- --- -----------------------------------------------------
      16-JUN-88 RSC Cleaned up prototyping (proto.h) and made functions static
      23-Jun-88 RSC Make const_dbd and const_dbf unique to 7 chars
*/


/* ********************** INCLUDE FILES ****************************** */

#include <stdio.h>
#include "vista.h"
#include "dbtype.h"

#ifdef dbdpath
#undef dbdpath
#endif
#ifdef dbfpath
#undef dbfpath
#endif

/* ********************** FUNCTION PROTOTYPES ************************ */

static int isabs(char *);
					/* TRUE if path is absolute */
static char * remdrv(char *);
					/* Removes drive spec from path */
static char * remfile(char *);
					/* Removes file from path */

/* ======================================================================
   Construct the full path to the dictionary.
*/
int con_dbd(
char *path_str,	/* (output) string to receive the full path to the
			   dictionary */
char *dbname,	/* contains the filename of the dictionary preceded
			   optionally by a path preceded optionally (DOS only)
			   by a drive spec. */
char *dbdpath	/* contains one element of the environment variable
			   DBDPATH.  NULL means no DBDPATH defined. */
)
{
/*
   RETURNS: db_status, S_OKAY means no errors
   ASSUMES: That the string dbdpath contains enough room
		  to add a DIRCHAR if it isn't in there.
*/

   int len;				/* Length of the constructed path */

   /* We stop constructing the string when we find an absolute path.
      Otherwise dbdpath, if defined, is put in front of the name; it
      must end with a DIRCHAR or ':' */
   if (isabs(dbname) || dbdpath == nullptr || dbdpath[0] == '\0')
      len = snprintf(path_str, FILENMLEN, "%s", dbname);
   else {
      size_t n = strlen(dbdpath);
      const char *sep = (dbdpath[n-1] != DIRCHAR && dbdpath[n-1] != ':')
			? DIRSTR : "";
      len = snprintf(path_str, FILENMLEN, "%s%s%s", dbdpath, sep,
		     remdrv(dbname));
   }

   /* Leave room for the .dbd extension */
   if (len < 0 || len >= FILENMLEN - 4) return (db_status = S_NAMELEN);

   /* Now add the .dbd extension */
   memcpy(path_str + len, ".dbd", sizeof(".dbd"));
   return (db_status = S_OKAY);

}
/* ======================================================================
   Construct full path to data/key files
*/
int con_dbf(
char *path_str,	/* (output) receives full path to dictionary */
char *dbfile,	/* path to database file (defn in schema) */
char *dbname,	/* d_open dbname argument - path to dictionary */
char *dbfpath	/* one element from DBFPATH, or NULL for no path */
)
{
/*
   RETURNS: db_status, S_OKAY means no error.
   ASSUMES: None.
*/
   char filespec[FILENMLEN];		/* Scratch work space */
   int len;

   /* Stop construction when we get to an absolute path.  If we prepend
      then remove the previous drive specifier */
   if (snprintf(path_str, FILENMLEN, "%s", dbfile) >= FILENMLEN)
      return (db_status = S_NAMELEN);
   if (isabs(dbfile)) return (db_status = S_OKAY);

   /* Add only the drive specification and directory component of the
      path supplied in dbname */
   if ((dbname = remfile(dbname)) != nullptr) {
      size_t db_len = strlen(dbname);
      const char *rem = remdrv(path_str);
      size_t rem_len = strlen(rem);
      if (db_len + rem_len >= FILENMLEN)
	 return(db_status = S_NAMELEN);
      /* Construct dbname + schema and place it in the o/p string */
      memcpy(filespec, dbname, db_len);
      memcpy(filespec + db_len, rem, rem_len + 1);
      memcpy(path_str, filespec, db_len + rem_len + 1);
      if (isabs(dbname)) return (db_status = S_OKAY);
   }

   /* Now add the path specification from DBFPATH, if defined */
   if (dbfpath == nullptr) return (db_status = S_OKAY);
   len = strlen(dbfpath);		/* Make sure it ends with DIRCHAR */
   if (len > 0 && dbfpath[len-1] != DIRCHAR && dbfpath[len-1] != ';')
      len = snprintf(filespec, sizeof(filespec), "%s%s%s", dbfpath, DIRSTR,
		     remdrv(path_str));
   else
      len = snprintf(filespec, sizeof(filespec), "%s%s", dbfpath,
		     remdrv(path_str));
   if (len < 0 || len >= (int)sizeof(filespec))
      return (db_status = S_NAMELEN);
   memcpy(path_str, filespec, len + 1);
   return (db_status = S_OKAY);

}

/* ======================================================================
   extracts a single element from DBxPATH type variables
*/
char * get_element(
char *dbxpath,	/* DBxPATH, elements separated by semicolons
			   NULL causes get_element to return NULL.
			   A NULL string also causes a NULL return */
int num			/* Element to extract (0 = first) */
)
{
/*
   RETURNS: Pointer to string with element.  NULL if no such element
	    exists.  If there is only one element in the string, then
	    that element is returned regardless of the value of num.
   ASSUMES: dbxpath cannot exceed FILENMLEN chars.
*/

   static char element[FILENMLEN+2];	/* return value */
   int i;
   char *save;

   if (dbxpath == nullptr) return (nullptr);
   if (dbxpath[0] == '\0') return (nullptr);	/* NULL string? */
   snprintf(element, FILENMLEN + 1, "%s", dbxpath);

   /* If there is only one element, always return that */
   if (strchr(element,';') == nullptr) {
      i = strlen(element);
      if (element[i-1] != DIRCHAR && element[i-1] != ':') {
	 element[i++] = DIRCHAR;
	 element[i] = '\0';
      }
      return (element);
   }

   /* There are multiple elements - return the one requested */
   dbxpath = element - 1;
   for (i=0; i<num; i++)
      if ((dbxpath = strchr(dbxpath+1,';')) == nullptr) return (nullptr);
   dbxpath++;
   if ((save = strchr(dbxpath+1,';')) != nullptr) *save = '\0';
   i = (dbxpath - (char *)element) + strlen((const char *)dbxpath);
   if (i > 0 && element[i-1] != DIRCHAR && element[i-1] != ':') {
      element[i++] = DIRCHAR;
      element[i] = '\0';
   }
   return (dbxpath);
}

/* ======================================================================
   Returns TRUE of path is absolute
*/
static int isabs(
char *path_str	/* path to test, NULL causes iabs to return FALSE */
)
{
/*
   RETURNS: TRUE if path is absolute
   ASSUMES: None.
*/
   char *path;			/* Points to path w/o drive spec */

   if (path_str == nullptr) return (FALSE);
   path = remdrv(path_str);
   if (path[0] != DIRCHAR) return (FALSE);
   return (TRUE);
}

/* ======================================================================
   Removes drive specifier from path
*/
static char * remdrv(
char *path_str		/* path to remove drive from */
)
{
/*
   RETURNS:       Pointer to string w/o drive specification.  Note that
		  this simply points further into path_str.
   ASSUMES: None.
*/
   char *path;			/* Return value */

   if ((path = strrchr(path_str,':')) == nullptr) return (path_str);
   return (path+1);
}

/* ======================================================================
   Removes file from a path.
*/
static char * remfile(
char *path_str		/* Path to remove filename from */
)
{
/*
   RETURNS: Pointer to the path with the filename removed (a static
	    string declared within remfile).  If there is no filename
	    (only a directory or drive spec), remfile will return
	    NULL.  The returned string will end with a DIRCHAR
   ASSUMES: path_str cannot exceed FILENMLEN characters.
*/
   static char dirpath[FILENMLEN];		/* Return value */

   /* There is only a filename if string does not contain a DIRCHAR or
      ':' which separates drive spec from path. */
   if (path_str == nullptr) return (nullptr);
   snprintf(dirpath, sizeof(dirpath), "%s", path_str); /* Copy into output string */
   if ((path_str = strrchr(dirpath,DIRCHAR)) == nullptr)
      if ((path_str = strrchr(dirpath,':')) == nullptr) return (nullptr);
   path_str[1] = '\0';			/* Truncate string */
   return (dirpath);
}

/* Set Country Table path
*/
int d_ctbpath(const char *ctb)
{
   int i;

   DB_ENTER(NO_DB_ID TASK_ID LOCK_SET(LOCK_NONE));

   if ( dbopen )
      dberr(S_DBOPEN);
   else {
      i = strlen(ctb);
      if ( i >= FILENMLEN - 1 ) RETURN( dberr(S_NAMELEN) );
      memcpy(db_global.ctbpath, ctb, i + 1);
      if ( i > 0 && ctb[i-1] != DIRCHAR && ctb[i-1] != ':' ) {
	 db_global.ctbpath[i++] = DIRCHAR;
	 db_global.ctbpath[i] = '\0';
      }

      db_status = S_OKAY;
   }

   RETURN( db_status );
}
/* vpp -nOS2 -dUNIX -nBSD -nVANILLA_BSD -nVMS -nMEMLOCK -nWINDOWS -nFAR_ALLOC -f/usr/users/master/config/nonwin pathfcns.c */
