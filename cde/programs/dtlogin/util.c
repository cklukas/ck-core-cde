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
/* (c) Copyright 1997 The Open Group */
/*                                                                      *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company                     *
 * (c) Copyright 1993, 1994 International Business Machines Corp.       *
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.                      *
 * (c) Copyright 1993, 1994 Novell, Inc.                                *
 */
/*
 * xdm - display manager daemon
 *
 * $TOG: util.c /main/15 1998/04/06 13:22:20 mgreess $
 *
 * Copyright 1988 Massachusetts Institute of Technology
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of M.I.T. not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  M.I.T. makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * Author:  Keith Packard, MIT X Consortium
 */

/*
 * util.c
 *
 * various utility routines
 */


#include    <sys/stat.h>
#include    <setjmp.h>
#include    <pthread.h>
#include    <time.h>
#include    <string.h>
#include    <stdint.h>
#include    <dirent.h>
#include    <Dt/MsgCatP.h>
#include    <X11/Xutil.h>
#include    <X11/Intrinsic.h>
#include    <X11/StringDefs.h>
#include    <X11/Xmu/SysUtil.h>
#include    <Dt/HourGlass.h>
#include   <signal.h>

# include	"dm.h"
# include	"vgmsg.h"
nl_catd		nl_fd = (nl_catd)-1;	/* message catalog file descriptor */

#if !defined(NL_CAT_LOCALE)
#define NL_CAT_LOCALE 0
#endif

#if !defined (ENABLE_DYNAMIC_LANGLIST)
#define LANGLISTSIZE    2048
char    languageList[LANGLISTSIZE];     /* global list of languages     */
#endif /* ENABLE_DYNAMIC_LANGLIST */

/***************************************************************************
 *
 *  Local procedure declarations
 *
 ***************************************************************************/

static char * makeEnv(
                        char *name,
                        char *value) ;

static SIGVAL MakeLangAbort(
			int arg	);

static int MatchesFileSuffix(const char *filename, const char *suffix);

static void   ScanNLSDir(
			char * dirname );

/********    End Local Function Declarations    ********/


/***************************************************************************
 *
 *  ReadCatalog
 *
 *  read a string from the message catalog
 ***************************************************************************/

unsigned char *
ReadCatalog( int set_num, int msg_num, char *def_str )
{
    static Bool alreadyopen = False;

    if (alreadyopen == False)
    {
      char *curNlsPath, *newNlsPath;
      size_t newNlsPathLen;

      alreadyopen = True;

     /*
      * Desktop message catalogs are in DT directory, so append desktop
      * search paths to current NLSPATH.
      */
      #define NLS_PATH_STRING  CDE_INSTALLATION_TOP "/nls/msg/%L/%N.cat:" \
                         CDE_INSTALLATION_TOP "/lib/nls/msg/%L/%N.cat:" \
                         CDE_INSTALLATION_TOP "/lib/nls/msg/%l/%t/%c/%N.cat:" \
                         CDE_INSTALLATION_TOP "/lib/nls/msg/%l/%c/%N.cat"

      curNlsPath = getenv("NLSPATH");
      if (curNlsPath && strlen(curNlsPath) == 0)
      {
        curNlsPath = NULL;
      }

     /*
      * 7 is "NLSPATH"
      * 1 is "="
      * <length of NLS_PATH_STRING>
      * 1 for null byte
      */
      newNlsPathLen = 7 + 1 + strlen(NLS_PATH_STRING) + 1;

      if (curNlsPath != NULL)
      {
       /*
        * 1 is ":"
        * <length of curNlsPath>
        */
        size_t length = strlen(curNlsPath);
        if (length > SIZE_MAX - newNlsPathLen - 1)
          newNlsPathLen = 0;
        else
          newNlsPathLen += length + 1;
      }

      newNlsPath = newNlsPathLen ? malloc(newNlsPathLen) : NULL;

      if (newNlsPath)
      {
        if (curNlsPath)
          snprintf(newNlsPath, newNlsPathLen, "NLSPATH=%s:%s", curNlsPath, NLS_PATH_STRING);
        else
          snprintf(newNlsPath, newNlsPathLen, "NLSPATH=%s", NLS_PATH_STRING);
        /* putenv retains storage only on success. Catalog fallback remains
         * usable even when extending the search path runs out of memory.
         */
        if (putenv(newNlsPath) != 0)
          free(newNlsPath);
      }

     /*
      * Open message catalog. Note, if invalid descriptor returned (ie
      * msg catalog could not be opened), subsequent call to catgets() using
      * that descriptor will return 'def_str'.
      */
      nl_fd = CATOPEN("dtlogin", NL_CAT_LOCALE);
    }

    return ((unsigned char *) CATGETS(nl_fd, set_num, msg_num, def_str));
}

void
printEnv( char **e )
{
	if (!e) return;
	while (*e)
		Debug ("    %s\n", *e++);
}

static int
EnvAllocationSize(size_t nameLength, size_t valueLength, size_t *size)
{
    if (nameLength > SIZE_MAX - 2 || valueLength > SIZE_MAX - nameLength - 2)
        return 0;
    *size = nameLength + valueLength + 2;
    return 1;
}

static char *
makeEnv( char *name, char *value )
{
	char	*result;
	size_t size;

	if (!name || !value || !EnvAllocationSize(strlen(name), strlen(value), &size))
	    return NULL;
	result = malloc (size);
	if (!result) {
		LogOutOfMem(
		  ReadCatalog(MC_LOG_SET,MC_LOG_MAKEENV,MC_DEF_LOG_MAKEENV));
		return 0;
	}

        if (*value) {
	           sprintf (result, "%s=%s", name, value);
        }
        else {
                sprintf (result, "%s", name);
        }

	return result;
}

char *
getEnv( char **e, char *name )
{
    size_t l;
    if (!e || !name) return NULL;
    l = strlen(name);

    while (*e) {
	if (strlen (*e) > l &&
	    !strncmp (*e, name, l) &&
	    (*e)[l] == '=')
	  return (*e) + l + 1;

	++e;
    }
    return 0;
}

char **
setEnv( char **e, char *name, char *value )
{
    char	**new, **old;
    char	*newe;
    size_t envsize, l;

    if (!name || !*name || !value || strchr(name, '='))
        return e;
    l = strlen (name);
    newe = makeEnv (name, value);
    if (!newe) {
	LogOutOfMem(ReadCatalog(MC_LOG_SET,MC_LOG_SETENV,MC_DEF_LOG_SETENV));
	return e;
    }
    if (e) {
	for (old = e; *old; old++)
	    if (strlen (*old) > l &&
		!strncmp (*old, name, l) &&
		(*old)[l] == '=')
	      break;

	if (*old) {
		free (*old);
		*old = newe;
		return e;
	}
	envsize = old - e;
	if (envsize > SIZE_MAX / sizeof(char *) - 2) {
	    free(newe);
	    return e;
	}
	new = (char **)
	      realloc((char *) e, (envsize + 2) * sizeof (char *));
    } else {
	envsize = 0;
	new = (char **) malloc (2 * sizeof (char *));
    }
    if (!new) {
	LogOutOfMem(ReadCatalog(MC_LOG_SET,MC_LOG_SETENV,MC_DEF_LOG_SETENV));
	free (newe);
	return e;
    }
    new[envsize] = newe;
    new[envsize+1] = 0;
    return new;
}

void
freeEnv (char **env)
{
    char    **e;

    if (env)
    {
        for (e = env; *e; e++)
            free (*e);
        free (env);
    }
}

# define isblank(c)	((c) == ' ' || c == '\t')

int
appendArg(char ***argv, const char *word)
{
    char **result;
    size_t count = 0;
    int savedErrno;
    if (!argv || !word) {
	errno = EINVAL;
	return 0;
    }
    while (*argv && (*argv)[count]) ++count;
    if (count > SIZE_MAX / sizeof(char *) - 2) {
	errno = EOVERFLOW;
	return 0;
    }
    result = malloc((count + 2) * sizeof(char *));
    if (!result) goto failed;
    result[count] = strdup(word);
    if (!result[count]) {
	savedErrno = errno;
	free(result);
	errno = savedErrno;
	goto failed;
    }
    if (count) memcpy(result, *argv, count * sizeof(char *));
    result[count + 1] = NULL;
    free(*argv);
    *argv = result;
    return 1;
failed:
    savedErrno = errno ? errno : ENOMEM;
    LogOutOfMem(ReadCatalog(MC_LOG_SET,MC_LOG_PARSEARGS,MC_DEF_LOG_PARSEARGS));
    errno = savedErrno;
    return 0;
}

int
parseArgs(char ***argv, const char *string)
{
    char **result;
    const char *word, *cursor;
    size_t original = 0, added = 0, used;
    int savedErrno;

    if (!argv || !string) {
	errno = EINVAL;
	return 0;
    }
    while (*argv && (*argv)[original])
	++original;
    cursor = string;
    while (*cursor) {
	while (isblank(*cursor)) ++cursor;
	if (!*cursor) break;
	++added;
	while (*cursor && !isblank(*cursor)) ++cursor;
    }
    if (original > SIZE_MAX / sizeof(char *) - 1 ||
	added > SIZE_MAX / sizeof(char *) - original - 1) {
	errno = EOVERFLOW;
	return 0;
    }
    result = malloc((original + added + 1) * sizeof(char *));
    if (!result) goto failed;
    if (original) memcpy(result, *argv, original * sizeof(char *));
    used = original;
    cursor = string;
    while (*cursor) {
	while (isblank(*cursor)) ++cursor;
	if (!*cursor) break;
	word = cursor;
	while (*cursor && !isblank(*cursor)) ++cursor;
	result[used] = malloc((size_t)(cursor - word) + 1);
	if (!result[used]) {
	    while (used > original) free(result[--used]);
	    free(result);
	    goto failed;
	}
	memcpy(result[used], word, cursor - word);
	result[used++][cursor - word] = '\0';
    }
    result[used] = NULL;
    free(*argv);
    *argv = result;
    return 1;

failed:
    savedErrno = errno ? errno : ENOMEM;
    LogOutOfMem(ReadCatalog(MC_LOG_SET,MC_LOG_PARSEARGS,MC_DEF_LOG_PARSEARGS));
    errno = savedErrno;
    return 0;
}

void
CleanUpChild( void )
{
/*
 *  On i386/i486 platforms setprrp() functions causes the mouse not
 *  to work.  Since in the daemon mode the parent daemon has already
 *  executed a setpgrp it is a process and session leader. Since it
 *  has also gotten rid of the controlling terminal there is no great
 *  harm in not making the sub-daemons as leaders.
 */
#if defined (SYSV) || defined (SVR4) || defined(__linux__)
	setpgrp ();
#else
	setpgrp (0, getpid ());
	sigsetmask (0);
#endif
#ifdef SIGCHLD
	(void) signal (SIGCHLD, SIG_DFL);
#endif
	(void) signal (SIGTERM, SIG_DFL);
	(void) signal (SIGPIPE, SIG_DFL);
	(void) signal (SIGALRM, SIG_DFL);
	(void) signal (SIGHUP, SIG_DFL);
	CloseOnFork ();
}

char * *
parseEnv( char **e, char *string )
{

    char *s1, *s2, *t1, *t2, *state;

    if (!string)
        return e;
    s1 = s2 = strdup(string);
    if (!s2)
        return e;

    while ((t1 = strtok_r(s1," \t", &state)) != NULL ) {
	if ( (t2 = strchr(t1,'=')) != NULL ) {
	    *t2++ = '\0';
	    e = setEnv(e, t1, t2);
	}

	s1 = NULL;
    }

    free(s2);
    return (e);
}

/*************************************<->*************************************
 *
 *  void SetHourGlassCursor
 *
 *
 *  Description:
 *  -----------
 *  sets the window cursor to an hourglass
 *
 *
 *  Inputs:
 *  ------
 *  dpy	= display
 *  w   = window
 *
 *  Outputs:
 *  -------
 *  None
 *
 *  Comments:
 *  --------
 *  None. (None doesn't count as a comment)
 *
 *************************************<->***********************************/

void
SetHourGlassCursor( Display *dpy, Window w )
{
    Cursor	cursor;

    XUndefineCursor(dpy, w);

    cursor = _DtGetHourGlassCursor(dpy);

    XDefineCursor(dpy, w, cursor);
    XFreeCursor(dpy, cursor);
    XFlush(dpy);
}

/***************************************************************************
 *
 *  BoundedCall
 *
 *  Call JOB (ARGUMENT) and wait at most SECONDS (0: as long as it takes) for
 *  it to return, for the calls of the X library that have no time limit of their
 *  own (opening a display, requests to a server that does not answer).  The
 *  call is made by a thread of its own, with every signal blocked, and the
 *  caller does not look at the signals the way the call would: nothing is
 *  jumped over.  Returns 1 and the result of the job in *RESULT when it
 *  returned in time, 0 when it did not and -1 when it could not be started.
 *  A job that did not return in time is left running: it owns ARGUMENT from
 *  then on, and what it returns is dropped.  The caller must not use what the
 *  job uses until the process ends.
 *
 ***************************************************************************/

struct bounded_call {
    pthread_mutex_t	lock;
    pthread_cond_t	finished;
    int			done;
    int			abandoned;
    void		*(*job)(void *);
    void		*argument;
    void		*result;
};

static void *
BoundedCallThread( void *data )
{
    struct bounded_call	*call = data;
    void		*result = call->job (call->argument);
    int			abandoned;

    pthread_mutex_lock (&call->lock);
    call->result = result;
    call->done = 1;
    abandoned = call->abandoned;
    if (!abandoned)
	pthread_cond_signal (&call->finished);
    pthread_mutex_unlock (&call->lock);
    if (abandoned)
	free (call);
    return NULL;
}

int
BoundedCall( unsigned seconds, void *(*job)(void *), void *argument,
	     void **result )
{
    struct bounded_call	*call;
    pthread_attr_t	attributes;
    pthread_condattr_t	condition;
    pthread_t		thread;
    sigset_t		all, previous;
    struct timespec	deadline;
    int			started, status = 0;

    call = calloc (1, sizeof (*call));
    if (call == NULL)
	return -1;
    call->job = job;
    call->argument = argument;
    pthread_mutex_init (&call->lock, NULL);
    pthread_condattr_init (&condition);
    pthread_condattr_setclock (&condition, CLOCK_MONOTONIC);
    pthread_cond_init (&call->finished, &condition);
    pthread_condattr_destroy (&condition);
    pthread_attr_init (&attributes);
    pthread_attr_setdetachstate (&attributes, PTHREAD_CREATE_DETACHED);

    /* the signals are for the caller: the thread starts with all of them blocked */
    sigfillset (&all);
    pthread_sigmask (SIG_SETMASK, &all, &previous);
    started = pthread_create (&thread, &attributes, BoundedCallThread, call);
    pthread_sigmask (SIG_SETMASK, &previous, NULL);
    pthread_attr_destroy (&attributes);
    if (started != 0)
    {
	free (call);
	return -1;
    }

    clock_gettime (CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += seconds;
    pthread_mutex_lock (&call->lock);
    while (!call->done && status == 0)
    {
	if (seconds)
	    status = pthread_cond_timedwait (&call->finished, &call->lock,
					     &deadline);
	else
	    pthread_cond_wait (&call->finished, &call->lock);
    }
    if (call->done)
    {
	*result = call->result;
	pthread_mutex_unlock (&call->lock);
	free (call);
	return 1;
    }
    call->abandoned = 1;
    pthread_mutex_unlock (&call->lock);
    return 0;
}

#if !defined (ENABLE_DYNAMIC_LANGLIST)
/***************************************************************************
 *
 *  MakeLangList
 *
 *  Generate the list of languages installed on the host.
 *  Result is stored the global array "languageList"
 *
 ***************************************************************************/

#define DELIM		" \t"   /* delimiters in language list		   */

/*
 * The scan of the NLS directory is given langListTimeout seconds.  The time is
 * looked at between the entries of the directory; there is no timer signal.
 */
static time_t	scanDeadline;		/* 0: no limit */
static int	scanTimedOut;

static int
ScanExpired( void )
{
    if (scanDeadline && !scanTimedOut && time (0) > scanDeadline)
	scanTimedOut = 1;
    return scanTimedOut;
}

void
MakeLangList( void )
{
    int i, j;

    char        *lang[500];             /* sort list for languages         */
    int         nlang;                  /* total number of languages       */
    char        *p, *s;
    char        *savelist;

    /*
     *  build language list from set of languages installed on the host...
     *  Wrap a timer around it so it doesn't hang things up too long.
     *  langListTimeout resource  by default is 30 seconds to scan NLS dir.
     */

    p = languageList;
    strcpy( p, "C");

    scanTimedOut = 0;
    scanDeadline = langListTimeout > 0 ? time (0) + langListTimeout : 0;
    ScanNLSDir(DEF_NLS_DIR);
    if (scanTimedOut) {
        LogError(ReadCatalog(MC_LOG_SET,MC_LOG_NO_SCAN,MC_DEF_LOG_NO_SCAN),
                   DEF_NLS_DIR, langListTimeout);
    }


    /*
     *  sort the list to eliminate duplicates and replace in global array...
     */

    p = savelist = strdup(languageList);
    nlang = 0;

    while ( (s = strtok(p, DELIM)) != NULL ) {

        /* the sort array holds 500 languages */
        if ( nlang >= (int)(sizeof(lang) / sizeof(lang[0])) - 1 )
            break;

        if ( nlang == 0 ) {
            lang[0] = s;
            lang[++nlang] = 0;
            p = NULL;
            continue;
        }

        for (i = nlang; i > 0 && strcmp(s,lang[i-1]) < 0; i--);

        if (i==0 || strcmp(s,lang[i-1]) != 0 ) {
            for (j = nlang; j > i; j--)
                lang[j] = lang[j-1];

            lang[i] = s;
            lang[++nlang] = 0;
        }

        p = NULL;
    }


    p = languageList;
    strcpy(p,"");

    for ( i = 0; i < nlang; i++) {
        strcat(p, lang[i]);
        strcat(p, " ");
    }

    free(savelist);

}


static int
MatchesFileSuffix(const char *filename, const char *suffix)
{
    int		retval = 0;
#if defined(_AIX) || defined(SVR4) || defined(__linux__) || defined(CSRG_BASED)
    int		different = 1;

    /*
     * The assumption here is that the use of strrstr is
     * to determine if "dp->d_name" ends in ".cat".
     */
    if (strlen(filename) >= strlen(suffix)) {
      different = strcmp(filename + (strlen(filename) - strlen (suffix)), suffix);
    }

    return (different == 0);
#else
    return (strrstr(filename, suffix) != NULL);
#endif
}

/***************************************************************************
 *
 *  ScanNLSDir
 *
 *  Scan a directory structure to see if it contains an installed language.
 *  If so, the name of the language is appended to a global list of languages.
 *
 *  Scan method and scan directory will vary by platform.
 *
 ***************************************************************************/


static void
ScanNLSDir(char *dirname)

#if defined(_AIX)
/*
 * Search installed locale names for AIX 3.2.5
 */
{
    DIR *dirp;
    struct dirent *dp;

    /*  Search valid locales which are locale database files in
     *  /usr/lib/nls/loc.
     *  File name is "??_??" which can be used as LANG variable.
     */
    if((dirp = opendir(dirname)) != NULL)
    {
        while((dp = readdir(dirp)) != NULL && !ScanExpired())
	{
            if(strlen(dp->d_name) == 5 && dp->d_name[2] == '_')
	    {
                if((int) strlen(languageList) + 7 < LANGLISTSIZE )
		{
                    strcat(languageList, " ");
                    strcat(languageList, dp->d_name);
                }
            }
        }
        closedir(dirp);
    }
}

#else /* !_AIX */
/*
 * Scan for installed locales on generic platform
 */
{
    DIR *dirp;
    struct dirent *dp;
    char* locale;
    char locale_path[MAXPATHLEN];
    struct stat locale_stat;
    int retval;

    /*
     * To determin the fully installed locale list, check several locations.
     */
    if(NULL != (dirp = opendir(dirname)))
    {
        while((dp = readdir(dirp)) != NULL && !ScanExpired())
	{
	    locale = dp->d_name;

            if ( (strcmp(dp->d_name, ".") == 0) ||
                 (strcmp(dp->d_name, "..") == 0) )
              continue;

	    if (locale[0] != '.' &&
                LANGLISTSIZE > (int) (strlen(languageList)+strlen(locale)+2))
	    {
		(void) snprintf(locale_path, MAXPATHLEN, "%s/%s",
                                dirname, locale);
	        retval = stat(locale_path, &locale_stat);

	    	if (0 == retval && S_ISDIR(locale_stat.st_mode))
		{
		    strcat(languageList, " ");
		    strcat(languageList, locale);
		}
            }
        }
        closedir(dirp);
    }
}

#endif

#endif /* ENABLE_DYNAMIC_LANGLIST */

#ifdef _AIX
#define ENVFILE "/etc/environment"

/* Refer to the LANG environment variable, first.
 * Or, search a line which includes "LANG=XX_XX" in /etc/environment.
 * If succeeded, set the value to d->language.
 */
void
SetDefaultLanguage(struct display *d)
{
    FILE *file;
    char lineBuf[160];
    int n;
    char *p;
    char *lang = NULL;

    if((lang = getenv( "LANG" )) == NULL ) {
	if((file = fopen(ENVFILE, "r")) != NULL) {
	    while(fgets(lineBuf, sizeof(lineBuf) - 1, file)) {
		n = strlen(lineBuf);
		if(n > 1 && lineBuf[0] != '#') {
		    if(lineBuf[n - 1] == '\n')
			lineBuf[n - 1] = '\0';
		    if((p = strstr(lineBuf, "LANG=")) != NULL) {
			p += 5;
			if(strlen(p) == 5 && p[2] == '_') {
			    lang = p;
			    break;
			}
		    }
		}
	    }
	    fclose(file);
	}
    }
    if(lang != NULL && strlen(lang) > 0) {
    /*
     * If LANG is set for hft, we need to change it for X.
     * Currently there are four hft LANG variables.
     */
        if(strcmp(lang, "En_JP") == 0)
            strcpy(d->language, "Ja_JP");
        else if(strcmp(lang, "en_JP") == 0)
            strcpy(d->language, "ja_JP");
        else if(strcmp(lang, "en_KR") == 0)
            strcpy(d->language, "ko_KR");
        else if(strcmp(lang, "en_TW") == 0)
            strcpy(d->language, "zh_TW");
        else
            *(strncpy(d->language, lang, LANGUAGESIZE) + LANGUAGESIZE) = '\0';
    }
}
#endif /* _AIX */


char **
setLang( struct display *d, char **env , char *langptr)
{
    char  langlist[LANGLISTSIZE];
    int   s = 0;
    char *element = NULL;
    int   set_def_lang = FALSE;


    if (NULL != langptr)
      Debug("setLang():  langlist = %s\n", langptr);
    else
      Debug("setLang():  langlist = NULL\n");

    if (langptr)
      snprintf(langlist, sizeof(langlist), "%s", langptr);
    else
      snprintf(langlist, sizeof(langlist), "%s", getEnv(env, "LANGLIST"));

    if (strlen(langlist) > 0) {
        element = strtok(langlist, DELIM);
        while(element) {
            set_def_lang = FALSE;
            if (strcmp(element,d->language) == 0){
                env = setEnv(env, "LANG",  d->language);
                break;
            }
            else
              set_def_lang = TRUE;

            s += strlen(element) +1;
            element = strtok(langlist+s, DELIM);
        }
    } else
      set_def_lang = TRUE;

    if (set_def_lang) {
        env = setEnv(env, "LANG", "C");
        strcpy(d->language, "C");
    }
    return env;
}

static char localHostbuf[256];
static int  gotLocalHostname;

char *
localHostname (void)
{
    if (!gotLocalHostname)
    {
        XmuGetHostname (localHostbuf, sizeof (localHostbuf) - 1);
        gotLocalHostname = 1;
    }
    return localHostbuf;
}
