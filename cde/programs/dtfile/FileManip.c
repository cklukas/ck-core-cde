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
/* $TOG: FileManip.c /main/10 1999/12/09 13:06:10 mgreess $ */
/************************************<+>*************************************
 ****************************************************************************
 *
 *   FILE:           FileManip.c
 *
 *   COMPONENT_NAME: Desktop File Manager (dtfile)
 *
 *   Description:    This module does the copy, move and rename commands.
 *                   Included are functions to control command execution,
 *                   move a directory, get the directory portion of a pathname,
 *                   get the file name portion of a pathname, and check if a
 *                   folder is to be moved within itself.
 *
 *   FUNCTIONS: Check
 *		CheckAccess
 *		CopyDir
 *		DName
 *		FileManip
 *		FileOperationError
 *		MoveDir
 *		defined
 *
 *   (c) Copyright 1993, 1994, 1995 Hewlett-Packard Company
 *   (c) Copyright 1993, 1994, 1995 International Business Machines Corp.
 *   (c) Copyright 1993, 1994, 1995 Sun Microsystems, Inc.
 *   (c) Copyright 1993, 1994, 1995 Novell, Inc.
 *
 ****************************************************************************
 ************************************<+>*************************************/

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <sys/types.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include <Xm/Xm.h>

#include <Xm/MwmUtil.h>

#include <Dt/DtP.h>                     /* required for DtDirPaths type */
#include <Dt/Connect.h>
#include <Dt/DtNlUtils.h>
#include <Dt/SharedProcs.h>

#include "Encaps.h"
#include "SharedProcs.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "Help.h"
#include "SharedMsgs.h"

#ifndef CDE_INSTALLATION_TOP
#define CDE_INSTALLATION_TOP "/usr/dt"
#endif

/*  Local Function Definitions  */
static char * MOVE_CMD = "/bin/mv";
static char * LINK_CMD = "/bin/ln";
static char * REMOVE_CMD = "/bin/rm";
static char * DTCOPY = CDE_INSTALLATION_TOP "/bin/dtfile_copy";

/* Operation workers own their directory helpers through completion. */
typedef struct {
    sigset_t mask;
    struct sigaction child_action;
} FileHelperSignals;

static int
FileHelperBegin(FileHelperSignals *signals)
{
    sigset_t blocked;
    struct sigaction normal;
    int saved;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &signals->mask) != 0) return -1;
    memset(&normal, 0, sizeof(normal));
    normal.sa_handler = SIG_DFL;
    sigemptyset(&normal.sa_mask);
    /* SIG_IGN/SA_NOCLDWAIT must not discard this exact helper's status. */
    if (sigaction(SIGCHLD, &normal, &signals->child_action) == 0) return 0;
    saved = errno;
    (void)sigprocmask(SIG_SETMASK, &signals->mask, NULL);
    errno = saved;
    return -1;
}

static int
FileHelperRestore(const FileHelperSignals *signals)
{
    int saved = 0;
    if (sigaction(SIGCHLD, &signals->child_action, NULL) != 0) saved = errno;
    if (sigprocmask(SIG_SETMASK, &signals->mask, NULL) != 0 && saved == 0) saved = errno;
    if (saved != 0) { errno = saved; return -1; }
    return 0;
}

static void
FileHelperChildSignals(void)
{
    sigset_t empty;
    struct sigaction normal;
    static const int reset[] = {SIGINT, SIGQUIT, SIGPIPE, SIGTERM, SIGCHLD};
    size_t i;
    memset(&normal, 0, sizeof(normal));
    normal.sa_handler = SIG_DFL;
    sigemptyset(&normal.sa_mask);
    for (i = 0; i < sizeof(reset) / sizeof(reset[0]); ++i)
        if (sigaction(reset[i], &normal, NULL) != 0) _exit(127);
    sigemptyset(&empty);
    if (sigprocmask(SIG_SETMASK, &empty, NULL) != 0) _exit(127);
}

static int
FileHelperWait(pid_t child, const FileHelperSignals *signals)
{
   int status = 0, saved = 0;
   pid_t result;
   if (child <= 0)
   {
      saved = EINVAL;
      if (FileHelperRestore(signals) != 0) saved = errno;
      errno = saved;
      return -1;
   }
   do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
   if (result < 0) saved = errno;
   if (FileHelperRestore(signals) != 0 && saved == 0) saved = errno;
   if (saved != 0) { errno = saved; return -1; }
   if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
   errno = WIFEXITED(status) && WEXITSTATUS(status) == 127 ? ENOEXEC :
           WIFEXITED(status) ? ECANCELED : EIO;
   return -1;
}

static char *
FilePathCopy(const char *value)
{
   size_t length;
   char *copy;
   if (value == NULL) { errno = EINVAL; return NULL; }
   length = strlen(value);
   copy = malloc(length + 1);
   if (copy != NULL) memcpy(copy, value, length + 1);
   return copy;
}

static char *
FilePathJoin(const char *directory, const char *leaf)
{
   size_t directory_len, leaf_len, separator, length;
   char *path;
   if (directory == NULL || leaf == NULL || leaf[0] == '\0')
   { errno = EINVAL; return NULL; }
   directory_len = strlen(directory);
   leaf_len = strlen(leaf);
   separator = directory_len != 0 && directory[directory_len - 1] == '/' ? 0 : 1;
   if (leaf_len > (size_t)-1 - separator - 1 ||
       directory_len > (size_t)-1 - leaf_len - separator - 1)
   { errno = ENAMETOOLONG; return NULL; }
   length = directory_len + separator + leaf_len;
   path = malloc(length + 1);
   if (path == NULL) return NULL;
   memcpy(path, directory, directory_len);
   if (separator != 0) path[directory_len++] = '/';
   memcpy(path + directory_len, leaf, leaf_len + 1);
   return path;
}

static char *
FileParentPath(const char *path)
{
   char *parent, *slash;
   size_t length;
   if (path == NULL || path[0] == '\0')
   { errno = EINVAL; return NULL; }
   length = strlen(path);
   parent = malloc(length + 1);
   if (parent == NULL) return NULL;
   memcpy(parent, path, length + 1);
   while (length > 1 && parent[length - 1] == '/') parent[--length] = '\0';
   slash = strrchr(parent, '/');
   if (slash == NULL)
      strcpy(parent, ".");
   else if (slash == parent)
      parent[1] = '\0';
   else
      *slash = '\0';
   return parent;
}

static void
FilePathsFree(char *first, char *second, char *third, char *fourth)
{
   free(first);
   free(second);
   free(third);
   free(fourth);
}

static char *
FileReadLink(const char *path)
{
   size_t capacity = 256;
   char *value;
   for (;;)
   {
      ssize_t length;
      value = malloc(capacity + 1);
      if (value == NULL) return NULL;
      length = readlink(path, value, capacity);
      if (length < 0) { int saved = errno; free(value); errno = saved; return NULL; }
      if ((size_t)length < capacity)
      {
         value[length] = '\0';
         return value;
      }
      free(value);
      if (capacity > (size_t)-1 / 2 || capacity >= 64u * 1024u * 1024u)
      { errno = ENAMETOOLONG; return NULL; }
      capacity *= 2;
   }
}


/************************************************************************
 *
 *  CheckAccess
 *
 ************************************************************************/
int
CheckAccess(
        const char *fname,
        int what)
{
    int access_priv;
    uid_t save_ruid;
    gid_t save_rgid;

#ifdef BLS
/*--------------------------------------------------------------------
 * access code for BLS
 *------------------------------------------------------------------*/

      setresuid(geteuid(),-1,-1);
      return access (fname, what);

#else /* the rest of the OS's */

   save_ruid = getuid();

#ifdef _AIX
   setreuid(geteuid(),-1);
#else
   setuid(geteuid());
#endif /* _AIX */

   save_rgid = getgid();

#ifdef _AIX
   setregid(getegid(),-1);
#else
   setgid(getegid());
#endif /* _AIX */

   access_priv = access (fname, what);

#ifdef _AIX
   setreuid(save_ruid,-1);
   setregid(save_rgid,-1);
#else
   setuid(save_ruid);
   setgid(save_rgid);
#endif /* _AIX */

   return access_priv;
#endif /* BLS */
}


/************************************************************************
 *
 *  FileOperationError
 *	Display an error message.
 *
 ************************************************************************/
void
FileOperationError(
        Widget w,
        char *message1,
        char *message2 )
{
   char *message_buf;
   char * title;
   char * tmpStr;

  if (message2 != NULL)
   {
     message_buf = XtMalloc(strlen(message1) + strlen(message2) + 1);
     (void) sprintf(message_buf,message1, message2);
   }
   else
   {
     message_buf = XtMalloc(strlen(message1) + 1);
     (void) sprintf(message_buf, "%s", message1);
   }

   /*  Display an error dialog  */
   tmpStr = GetSharedMessage(FILE_MANIPULATION_ERROR_TITLE);
   title = XtNewString(tmpStr);
   _DtMessage (w, title, message_buf, NULL, HelpRequestCB);
   XtFree(title);
   XtFree(message_buf);
}


/************************************************************************
 *
 *  DName
 *	Returns the file name of its argument.
 *      Keep looking thru the string until a "/" is found which still
 *      has some characters after it.
 *
 ************************************************************************/
char *
DName(
        char *name )
{
   char * p;
   char * q;

   p = q = name;

   while (1)
   {
      q = DtStrchr(q, '/');
      if ((q) && *(q+1))
         p = q + 1;

      if (q == NULL)
         break;

      q++;
   }

   return(p);
}


/************************************************************************
 *
 *  Check
 *
 ************************************************************************/
static int
FileDirectoryHasAncestor(const char *path, const struct stat *ancestor)
{
   struct stat current, parent;
   int fd = -1, next = -1, result = -1, saved, flags;
   size_t depth;
   if (path == NULL || path[0] == '\0' || ancestor == NULL || !S_ISDIR(ancestor->st_mode))
   { errno = EINVAL; return -1; }
#if defined(O_SEARCH)
   flags = O_SEARCH;
#elif defined(O_PATH)
   flags = O_PATH;
#else
   flags = O_RDONLY;
#endif
#ifdef O_DIRECTORY
   flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
   flags |= O_CLOEXEC;
#endif
   fd = open(path, flags);
   if (fd < 0) return -1;
   for (depth = 0; depth < 65536; ++depth)
   {
      if (fstat(fd, &current) != 0) goto finished;
      if (!S_ISDIR(current.st_mode)) { errno = ENOTDIR; goto finished; }
      if (current.st_dev == ancestor->st_dev && current.st_ino == ancestor->st_ino)
      { result = 1; goto finished; }
      next = openat(fd, "..", flags);
      if (next < 0 || fstat(next, &parent) != 0) goto finished;
      if (current.st_dev == parent.st_dev && current.st_ino == parent.st_ino)
      { result = 0; goto finished; }
      if (close(fd) != 0) { fd = -1; goto finished; }
      fd = next; next = -1;
   }
   errno = ELOOP;
finished:
   saved = errno;
   if (fd >= 0 && close(fd) != 0 && result >= 0) { saved = errno; result = -1; }
   if (next >= 0 && close(next) != 0 && result >= 0) { saved = errno; result = -1; }
   errno = saved;
   return result;
}

static int
Check(
        Widget w,
        const char *path,
        const struct stat *source,
        int mode,
        FileOperationErrorProc errorHandler)
{
   int result = FileDirectoryHasAncestor(path, source), saved = errno;
   if (result == 0) return 0;
   if (errorHandler)
   {
      if (result > 0)
         (*errorHandler)(w, mode == COPY_FILE
            ? GETMESSAGE(11,35, "Cannot copy a folder into itself.")
            : GETMESSAGE(11,16, "A folder cannot be moved into itself.\n%s"),
            mode == COPY_FILE ? NULL : (char *)path);
      else
         (*errorHandler)(w, GETMESSAGE(11,33, "Cannot open %s"), (char *)path);
   }
   errno = result > 0 ? EINVAL : saved;
   return 1; /* An incomplete ancestry check is never permission to proceed. */
}


/************************************************************************
 *
 *  MoveDir
 *
 ************************************************************************/

static Boolean
MoveDir(
        Widget w,
        char *source,
        char *target,
        Boolean isContainer,
        struct stat *sourceStatInfo,
        FileOperationErrorProc errorHandler,
        int type )
{
   static char *pname = "MoveDir";
   char *p;

   char *targetDir;             /* original target path for diagnostics */
   char *actualTarget = NULL;
   char *sourceParent = NULL;
   char *targetParent = NULL;
   struct stat  s1;             /* status of from file */
   struct stat  s2;             /* status of to file   */
   char * cptr;
   int len, val, val1;

   char * msg;
   char * tmpStr;
   pid_t child_pid;
   int rc;
   FileHelperSignals helper_signals;

   /* Copy target so we have it for an error dialog if we need it */
   targetDir = FilePathCopy(target);
   if (targetDir == NULL)
   {
      if (errorHandler) (*errorHandler)(w, GETMESSAGE(11,33, "Cannot open %s"), target);
      return False;
   }

   if ((val = stat (target, &s2)) < 0)
      val = lstat(target, &s2);

   if (val >= 0) /* target exists */
   {
      if (!isContainer)
      {
         if (errorHandler) (*errorHandler)(w, GetSharedMessage(CANT_OVERWRITE_ERROR), target);
         free(targetDir);
         return False;
      }
      if ((s2.st_mode & S_IFMT) != S_IFDIR) /* target not directory */
      {
         if (errorHandler)
         {
            char * tmpStr;
            tmpStr = GetSharedMessage(CANT_OVERWRITE_ERROR);
            msg = XtNewString(tmpStr);
            (*errorHandler) (w, msg, target);
            XtFree(msg);
         }
         free(targetDir);
         return (False);
      }

      actualTarget = FilePathJoin(target, DName(source));
      if (actualTarget == NULL)
      {
         if (errorHandler) (*errorHandler)(w, GETMESSAGE(11,33, "Cannot open %s"), target);
         free(targetDir);
         return False;
      }
      target = actualTarget;
      if (lstat(target, &s2) >= 0) /* resolved target exists */
      {
         if (errorHandler)
         {
            char * tmpStr;

            tmpStr = GetSharedMessage(CANT_OVERWRITE_ERROR);
            msg = XtNewString(tmpStr);
            (*errorHandler) (w, msg, target);
            XtFree(msg);
         }
         free(actualTarget);
         free(targetDir);
         return (False);
      }
   }
   else
   {
      actualTarget = FilePathCopy(target);
      if (actualTarget == NULL)
      {
         if (errorHandler) (*errorHandler)(w, GETMESSAGE(11,33, "Cannot open %s"), target);
         free(targetDir);
         return False;
      }
      target = actualTarget;
   }


   p = DName (source);

   /*  don't rename these  */
   DtLastChar(p, &cptr, &len);

   if (!strcmp (p, ".") || !strcmp (p, "..") ||
       !strcmp (p, "")  || ((len == 1) && (*cptr == '/')))
   {
      if (errorHandler)
      {
         char * tmpStr;

         tmpStr = (GETMESSAGE(11,32, "Cannot rename %s"));
         msg = XtNewString(tmpStr);
         (*errorHandler) (w, msg, p);
         XtFree(msg);
      }
      FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
      return (False);
   }

   sourceParent = FileParentPath(source);
   targetParent = FileParentPath(target);
   if (sourceParent == NULL || targetParent == NULL)
   {
      if (errorHandler) (*errorHandler)(w, GETMESSAGE(11, 14, "Cannot find the folders location."), NULL);
      FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
      return False;
   }

   /*  parent doesn't exist  */
   if((val = stat(sourceParent, &s1)) < 0)
      val = lstat(sourceParent, &s1);

   if((val1 = stat(targetParent, &s2)) < 0)
      val1 = lstat(targetParent, &s2);

   if (val < 0 || val1 < 0)
   {
      if (errorHandler)
      {
         tmpStr = GETMESSAGE(11, 14, "Cannot find the folders location.");
         msg = XtNewString(tmpStr);
         (*errorHandler) (w, msg, NULL);
         XtFree(msg);
      }
      FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
      return (False);
   }


   /*  check for target parent not writeable  */
   if (CheckAccess(targetParent, W_OK) == -1)
   {
      if (errorHandler)
      {
         char * tmpStr;

         tmpStr = GetSharedMessage(CANT_WRITE_ERROR);
         msg = XtNewString(tmpStr);
         (*errorHandler) (w, msg, targetDir);
         XtFree(msg);
      }
      FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
      return (False);
   }


   if (Check(w, targetParent, sourceStatInfo, MOVE_FILE, errorHandler))
   {
      FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
      return False;
   }

   /* check for source parent not writeable */
   if (CheckAccess(sourceParent, W_OK) == -1)
   {
      if (errorHandler)
      {
         char * tmpStr;

         tmpStr = GetSharedMessage(CANT_WRITE_ERROR);
         msg = XtNewString(tmpStr);
         (*errorHandler) (w, msg, source);
         XtFree(msg);
      }
      FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
      return (False);
   }
   /*
     if (((sourceStatInfo->st_mode & S_IFMT) == S_IFDIR) &&
     (CheckAccess(source, W_OK) != 0))
     {
     if (errorHandler)
     {
     char * tmpStr;

     tmpStr=GETMESSAGE(11, 57,"You do not have permission to move the folder\n%s\nWrite permission is required.");
     msg = XtMalloc(strlen(tmpStr) + strlen(source) + 2);
     sprintf(msg, tmpStr, source);
     (*errorHandler) (w, msg, source);
     XtFree(msg);
     }
     XtFree(targetDir);
     return (False);
     }
     */
   /*  if parents are not on the same device, do a copy & delete */
   if (s1.st_dev != s2.st_dev)
   {
      /* Determine correct Geometry Placement fo Move Dialog */
      /* @@@ ... to be added */

      if (FileHelperBegin(&helper_signals) != 0)
      {
         if (errorHandler) (*errorHandler)(w, GETMESSAGE(42, 30, "Cannot prepare the folder operation for %s."), source);
         FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
         return False;
      }
      child_pid = fork();
      if (child_pid == -1)
      {
         int saved = errno;
         (void)FileHelperRestore(&helper_signals);
         errno = saved;
         if (errorHandler)
         {
            tmpStr = GETMESSAGE(11, 39, "Cannot create child process.\nThe maximum number of processes for this system has been reached.\nStop some of the processes or programs that are currently\nrunning and then retry this function.");
            msg = XtNewString(tmpStr);
            (*errorHandler) (w, msg, NULL);
            XtFree(msg);
         }
         FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
         return False;
      }

      if (child_pid == 0)
      {
         FileHelperChildSignals();
	 DBGFORK(("%s:  child forked\n", pname));

         /* pass in geometry, and other command lines params when available */
	 if(type == TRASH_DIRECTORY)
           rc = execlp(DTCOPY, "dtfile_copy", "-move", "-confirmReplace",
		 "-confirmErrors", "-popDown","-checkPerms", source, target, NULL);
	 else
           rc = execlp(DTCOPY, "dtfile_copy", "-move", "-confirmReplace",
		 "-confirmErrors", "-popDown", source, target, NULL);

         /* call errorhandler */
         perror ("Could not exec child process \"dtfile_copy\"");

	 DBGFORK(("%s:  child exiting\n", pname));

         _exit (127);
      }

      DBGFORK(("%s:  forked child<%d>\n", pname, child_pid));


      rc = FileHelperWait(child_pid, &helper_signals);
      if (rc != 0 && errno != ECANCELED && errorHandler)
         (*errorHandler)(w, GETMESSAGE(42, 31, "The folder operation did not complete for %s."), source);
      FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
      return rc == 0;
   }

/*   This part of code was implemented with the idea that the links
     to be treated differently.  So, it has to be uncommented whenever
     links are handled differently (i.e., moving a link shall move the
     absolute object.

   if(strcmp(link_path, source) != 0)
   {
     if (RunFileCommand (MOVE_CMD, link_path, target, NULL) == 0)
     {
       FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
       return (True);
     }
   }
   else
*/
   {
     if (RunFileCommand (MOVE_CMD, source, target, NULL) == 0)
     {
       FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
       return (True);
     }
   }

   FilePathsFree(targetDir, actualTarget, sourceParent, targetParent);
   return (False);
}


/************************************************************************
 *
 *  CopyDir
 *
 ************************************************************************/
static Boolean
CopyDir(
        Widget w,
        int mode,
        char *from,
        char *to,
        Boolean  isContainer,
        struct stat *s1,
        FileOperationErrorProc errorHandler,
        Boolean checkForBusyDir,
        int type )
{
   static char *pname = "CopyDir";
   char *target = NULL;
   char *target_dir = NULL;
   struct stat  s2;             /* status of to file   */
   pid_t child_pid;
   int rc, target_rc;
   FileHelperSignals helper_signals;
   char *msg, *tmpStr;

   /* Check if source is readable */
   if (CheckAccess(from, R_OK) == -1)
   {
      if (errorHandler)
      {
         tmpStr = GetSharedMessage(CANT_READ_ERROR);
         msg = XtNewString(tmpStr);
         (*errorHandler) (w, msg, from);
         XtFree(msg);
      }
      return (False);
   }

   /* generate target name */
   /* "to" can be something to copy the source into (isContainer=TRUE)     */
   /*    or it can be the full path of the destination (isContainer=FALSE) */
   /* the former case is probably more common (e.g. a drag&drop copy)      */
   /*    whereas the second case occurs when, for example, the menu is     */
   /*    to copy directory /u/joe/a to /u/joe/b (not doable with d&d)      */
   target = isContainer ? FilePathJoin(to, DName(from)) : FilePathCopy(to);
   if (target == NULL || (target_dir = FileParentPath(target)) == NULL)
   {
      if (errorHandler) (*errorHandler)(w, GETMESSAGE(11, 14, "Cannot find the folders location."), NULL);
      FilePathsFree(target, target_dir, NULL, NULL);
      return False;
   }

   /* Check if target directory exists */
   if ((target_rc = stat (target, &s2)) < 0)
      target_rc = lstat(target, &s2);

   if (target_rc >= 0)
   {
      if (mode != MERGE_DIR)
      {
         if (errorHandler) (*errorHandler)(w, GetSharedMessage(CANT_OVERWRITE_ERROR), target);
         FilePathsFree(target, target_dir, NULL, NULL);
         return False;
      }
      /* target exists:
       * make sure it's a directory */

      if ((s2.st_mode & S_IFMT) != S_IFDIR) 	/* target not directory */
      {
         if (errorHandler)
         {
            tmpStr = GetSharedMessage(CANT_OVERWRITE_ERROR);
            msg = XtNewString(tmpStr);
            (*errorHandler) (w, msg, target);
            XtFree(msg);
         }
         FilePathsFree(target, target_dir, NULL, NULL);
         return (False);
      }
   }
   else
   {
     /* target does not exist:
      * make sure the "to" directory exists and is writable */
     if ((rc = stat (target_dir, &s2)) < 0)
        rc = lstat(target_dir, &s2);

     if (rc < 0 || (s2.st_mode & S_IFMT) != S_IFDIR)
     {
        if (errorHandler)
        {
           tmpStr = GETMESSAGE(11, 14, "Cannot find the folders location.");
           msg = XtNewString(tmpStr);
           (*errorHandler) (w, msg, NULL);
           XtFree(msg);
        }
        FilePathsFree(target, target_dir, NULL, NULL);
        return (False);
     }

     if (CheckAccess(target_dir, W_OK) == -1)
     {
        if (errorHandler)
        {
           tmpStr = GetSharedMessage(CANT_WRITE_ERROR);
           msg = XtNewString(tmpStr);
           (*errorHandler) (w, msg, to);
           XtFree(msg);
        }
        FilePathsFree(target, target_dir, NULL, NULL);
        return (False);
     }
   }

   /* Walk actual directory identities; root inode numbers are filesystem-specific. */
   if (Check(w, target_rc >= 0 ? target : target_dir, s1, COPY_FILE, errorHandler))
   {
      FilePathsFree(target, target_dir, NULL, NULL);
      return False;
   }

   /* Determine correct Geometry Placement fo Copy Dialog */
   /* @@@ ... to be added */

   /* If all the above checks have passed, then fork off the copy dialog */

   if (FileHelperBegin(&helper_signals) != 0)
   {
      if (errorHandler) (*errorHandler)(w, GETMESSAGE(42, 30, "Cannot prepare the folder operation for %s."), from);
      FilePathsFree(target, target_dir, NULL, NULL);
      return False;
   }
   child_pid = fork();
   if (child_pid == -1)
   {
      int saved = errno;
      (void)FileHelperRestore(&helper_signals);
      errno = saved;
      if (errorHandler)
      {
         tmpStr = GETMESSAGE(11, 39, "Cannot create child process.\nThe maximum number of processes for this system has been reached.\nStop some of the processes or programs that are currently\nrunning and then retry this function.");
         msg = XtNewString(tmpStr);
         (*errorHandler) (w, msg, NULL);
         XtFree(msg);
      }
      FilePathsFree(target, target_dir, NULL, NULL);
      return False;
   }

   if (child_pid == 0)
   {
      FileHelperChildSignals();
      DBGFORK(("%s:  child forked\n", pname));

      /* pass in geometry, and other command lines params when available */
      if (mode == MERGE_DIR)
        /* merge source & target directories */
        rc = execlp(DTCOPY, "dtfile_copy",
                     "-dontDelete", "-forceCopies", "-copyTop",
                     "-confirmReplace", "-confirmErrors", "-popDown",
                    from, target, (char *)NULL);
      else
         /* replace target dir */
         rc = execlp(DTCOPY, "dtfile_copy",
                     "-forceCopies", "-copyTop",
                     "-confirmReplace", "-confirmErrors", "-popDown",
                     from, target, (char *)NULL);

      /* call errorhandler */
      perror ("Could not exec child process \"dtfile_copy\"");

      DBGFORK(("%s:  child exiting\n", pname));

      _exit (127);
   }

   DBGFORK(("%s:  forked child<%d>\n", pname, child_pid));


   rc = FileHelperWait(child_pid, &helper_signals);
   if (rc != 0 && errno != ECANCELED && errorHandler)
      (*errorHandler)(w, GETMESSAGE(42, 31, "The folder operation did not complete for %s."), from);
   FilePathsFree(target, target_dir, NULL, NULL);
   return rc == 0;
}


/************************************************************************
 *
 *  FileManip
 *
 ************************************************************************/
Boolean
FileManip(
        Widget w,
        int mode,
        char *from,
        char *to,
        Boolean  isContainer,          /* described in function CopyDir */
        FileOperationErrorProc errorHandler,
        Boolean checkForBusyDir,
        int type )
{
   int fold;
   int fnew;
   ssize_t n;
   int transferError = 0;
   Boolean copy_dir_return;
   Boolean move_dir_return;
   void (*oldInt)(int);
   void (*oldQuit)(int);
   void (*oldPipe)(int);
   void (*oldTerm)(int);
   Boolean restricted = False;

   struct stat s1;      /* status of from file e.g. lstat info */
   struct stat s4;      /* status of from file e.g. stat info  */
   struct stat s2;      /* status of to file e.g.   stat info  */
   struct stat s3;      /* status of to file e.g.   lstat info */

   char buf [BLOCK_SIZE];		/* generic buffer */
   char *filename = NULL;
   char * msg;
   int link_result;
   char * tmpStr;


   /* Check the <from> part of the command:
    *
    * Report error if <from> doesn't exist.
    * Else error if <from> hasn't read access.
    */

   DPRINTF(("FileManip: mode %d type %d from \"%s\" to \"%s\"\n", mode, type, from, to));
   if (stat (from, &s1) < 0)
   {
      if (lstat (from, &s1) < 0)
      {
         if (errorHandler)
         {
            tmpStr = (GETMESSAGE(11,28, "%s cannot be found."));
            msg = XtNewString(tmpStr);
            (*errorHandler) (w, msg, from);
            XtFree(msg);
         }
         return (False);
      }
   }

   /* We will check if we need to initiate a copy of a directory */
   if ((s1.st_mode & S_IFMT) == S_IFDIR)	/* from is a directory */
   {
      if (mode == COPY_FILE  ||  mode == MERGE_DIR)
      {
         oldInt = signal (SIGINT, SIG_IGN);
         oldQuit = signal (SIGQUIT, SIG_IGN);
         oldPipe = signal (SIGPIPE, SIG_IGN);
         oldTerm = signal (SIGTERM, SIG_IGN);

         copy_dir_return = CopyDir(w, mode, from, to, isContainer, &s1,
                                   errorHandler, checkForBusyDir, type);

         (void) signal (SIGINT, oldInt);
         (void) signal (SIGQUIT, oldQuit);
         (void) signal (SIGPIPE, oldPipe);
         (void) signal (SIGTERM, oldTerm);

         return (copy_dir_return);
      }


      /*  If the directory has more than one open view or open views  */
      /*  of sub directories.  Then do not allow the move or rename.  */

      if (mode == MOVE_FILE)
      {
         if (checkForBusyDir && DirectoryBusy (from))
         {
            if (errorHandler)
            {
               char message_buf[512];
               char * tmpStr;

               message_buf[0] = '\0';
               tmpStr = (GETMESSAGE(11,30, "Cannot move or rename the folder %s.\nAll File Manager views displayed for a folder or its sub-folders\nmust be closed before a folder can be moved or renamed."));
               sprintf (message_buf, tmpStr, from);
               (*errorHandler) (w, message_buf, NULL);
            }
            return (False);
         }


         oldInt = signal (SIGINT, SIG_IGN);
         oldQuit = signal (SIGQUIT, SIG_IGN);
         oldPipe = signal (SIGPIPE, SIG_IGN);
         oldTerm = signal (SIGTERM, SIG_IGN);

         move_dir_return = MoveDir(w, from, to, isContainer, &s1, errorHandler, type);

         (void) signal (SIGINT, oldInt);
         (void) signal (SIGQUIT, oldQuit);
         (void) signal (SIGPIPE, oldPipe);
         (void) signal (SIGTERM, oldTerm);

         return (move_dir_return);
      }

      if (mode == LINK_FILE)
      {
         if(type == DESKTOP)
         {
             char *tmp, *ptr;

             tmp = XtNewString(to);
             /* get the workspace number first */
             ptr = strrchr(tmp, '/');
             if (ptr == NULL)
                restricted = True;
             else
             {
                *ptr = '\0';
                /* now get the Desktop */
                ptr = strrchr(tmp, '/');
                /* A destination already below Desktop is an exact target. */
                if (ptr == NULL || strcmp(ptr, "/Desktop") != 0)
                   restricted = True;
             }
             XtFree(tmp);
         }

         if ((strcmp(from, "/.") == 0 || strcmp(from, "/") == 0) &&
             !restricted && type != TRASH_DIRECTORY && isContainer)
         {
            size_t host_len = strlen(home_host_name), title_len = strlen(root_title);
            char *leaf;
            if (host_len > (size_t)-1 - title_len - 2)
               filename = NULL;
            else if ((leaf = malloc(host_len + title_len + 2)) != NULL)
            {
               memcpy(leaf, home_host_name, host_len);
               leaf[host_len] = ':';
               memcpy(leaf + host_len + 1, root_title, title_len + 1);
               filename = FilePathJoin(to, leaf);
               free(leaf);
            }
         }
         else if ( (!restricted && type != TRASH_DIRECTORY) && isContainer)
            filename = FilePathJoin(to, DName(from));
         else
            filename = FilePathCopy(to);

         if (filename == NULL)
         {
            if (errorHandler) (*errorHandler)(w, GetSharedMessage(CANT_CREATE_ERROR), to);
            return False;
         }
	 to = filename;

         if (((link_result = symlink(from, to)) != 0) && errorHandler)
         {
            if(type == NOT_DESKTOP || errno != EEXIST )
            {
               char * tmpStr;

               tmpStr = GetSharedMessage(CANT_CREATE_ERROR);
               msg = XtNewString(tmpStr);
               (*errorHandler) (w, msg, to);
               XtFree(msg);
            }
         }

         free(filename);
         return(link_result == 0 ? True : False);
      }
   }

   if (CheckAccess(from, R_OK) == -1)
   {
      /*
       * A move operation does not require read permission, but a copy does.
       */
      if (mode == COPY_FILE)
      {
         if (errorHandler)
         {
            char * tmpStr;

            tmpStr = GetSharedMessage(CANT_READ_ERROR);
            msg = XtNewString(tmpStr);
            (*errorHandler) (w, msg, from);
            XtFree(msg);
         }
         return (False);
      }
   }


   /* Here <from> is a file (not a directory).
    * Check the <to> part of the command:
    *
    * <To> can either be an existing file,
    *                    an existing directory,
    *                    or a new file in an existing directory.
    */

   if (lstat (to, &s2) >= 0) 			   /* <to> exists */
   {
      if (isContainer && (stat (to, &s3) >= 0) &&
#if defined(__linux__) || defined(CSRG_BASED)
           (((s3.st_mode & S_IFMT) == S_IFDIR)          /* if is a directory */
           || ((s3.st_mode & S_IFMT) == S_IFSOCK)) )    /* or a net special */
#else
#if defined(SVR4) || defined(_AIX)
           ((s3.st_mode & S_IFMT) == S_IFDIR) )         /* if is a directory */
#else
           (((s3.st_mode & S_IFMT) == S_IFDIR)          /* if is a directory */
           || ((s3.st_mode & S_IFMT) == S_IFNWK)) )     /* or a net special */
#endif
#endif
      {						   /* then get file name */
            filename = FilePathJoin(to, DName(from));
            if (filename == NULL)
            {
               if (errorHandler) (*errorHandler)(w, GetSharedMessage(CANT_CREATE_ERROR), to);
               return False;
            }
	    to = filename;
      }

      if (lstat (to, &s2) >= 0)		           /* reverify <to> exists */
      {
         if ((stat (to, &s3) >= 0) &&
             ((s3.st_mode & S_IFMT) == S_IFDIR))   /* if is a directory */
         {
            if (errorHandler)
            {
               char * tmpStr;

               tmpStr = GetSharedMessage(CANT_OVERWRITE_ERROR);
               msg = XtNewString(tmpStr);
               (*errorHandler) (w, msg, to);
               XtFree(msg);
            }
            free(filename);
            return (False);
         }


         /* Explicit replacement belongs to the operation worker. An object
          * appearing at an exact target must never be silently unlinked here. */
         if (errorHandler) (*errorHandler)(w, GetSharedMessage(CANT_OVERWRITE_ERROR), to);
         free(filename);
         return False;
      }
   }


   /* Here <from> is a file and <to> doesn't exist.
    *
    * If not a copy, link the files.  If the link succeeds, unlink
    *   <from> and return.
    * Copy <from> to <to>.
    * If the copy cmd is not specified, unlink <from>.
    */

   if (mode != COPY_FILE)
   {
      /* Try to maintain symbolic links, except when we're doing a link! */
      if (((s1.st_mode & S_IFMT) == S_IFLNK) && (mode != LINK_FILE))
      {
         char *link_path = FileReadLink(from);

         if (link_path != NULL)
         {
            link_result = symlink(link_path, to);
            free(link_path);
         }
         else
         {
            /* Fail-safe; do it the hard way */
            if (mode == MOVE_FILE)
            {
               if (RunFileCommand(MOVE_CMD, from, to, NULL) == 0)
               {
                  free(filename);
                  return(True);
               }
            }
            else
            {
               if (RunFileCommand(LINK_CMD, "-s", from, to) == 0)
               {
                  free(filename);
                  return(True);
               }
            }

            link_result = (-1);
         }
      }
      else
      {
         if (mode == LINK_FILE)
            link_result = symlink(from, to);
         else
         {
            char *link_path;

            link_path = _DtFollowLink(from);

            if (link_path == NULL)
                 link_result = -1;
            else if(strcmp(link_path, from) != 0)
                 link_result = symlink(link_path, to);
            else
                 link_result = link (from, to);
         }
      }

      /* If this was a link, then time to bail out */
      if (mode == LINK_FILE)
      {
         if ((link_result != 0) && errorHandler)
         {
            char * tmpStr;

            tmpStr = GetSharedMessage(CANT_CREATE_ERROR);
            msg = XtNewString(tmpStr);
            (*errorHandler) (w, msg, to);
            XtFree(msg);
         }

         free(filename);
         return(link_result == 0 ? True : False);
      }

      /* Unlink source only if this was a move request */
      if ((mode == MOVE_FILE) && (link_result >= 0))
      {
         if (unlink (from) < 0)
         {
            if (errorHandler)
            {
               char * tmpStr;

               tmpStr = GetSharedMessage(CANT_DELETE_ERROR);
               msg = XtNewString(tmpStr);
               (*errorHandler) (w, msg, from);
               XtFree(msg);
            }
            (void) unlink (to);
            free(filename);
            return (False);
         }

         free(filename);
         return (True);
      }
   }

   /*  unable to read <from>  */

   if ((fold = open (from, O_RDONLY)) < 0)
   {
      if (errorHandler)
      {
         char * tmpStr;

         tmpStr = GetSharedMessage(CANT_READ_ERROR);
         msg = XtNewString(tmpStr);
         (*errorHandler) (w, msg, from);
         XtFree(msg);
      }
      free(filename);
      return (False);
   }


   /*  unable create <to>  */


   /* Use the opened source's mode, not another lookup of its pathname. */
   if (fstat(fold, &s4) < 0)
   {
      if (errorHandler)
      {
         tmpStr = GetSharedMessage(CANT_READ_ERROR);
         msg = XtNewString(tmpStr);
         (*errorHandler)(w, msg, from);
         XtFree(msg);
      }
      (void) close(fold);
      free(filename);
      return (False);
   }

   /* The operation worker must resolve any replacement before this call.
    * Never truncate an occupant (including a link) which appears here.
    */
   if ((fnew = open(to, O_WRONLY | O_CREAT | O_EXCL, s4.st_mode)) < 0)
   {
      if (errorHandler)
      {
         char * tmpStr;

         tmpStr = GetSharedMessage(CANT_CREATE_ERROR);
         msg = XtNewString(tmpStr);
         (*errorHandler) (w, msg, to);
         XtFree(msg);
      }
      (void) close (fold);
      free(filename);
      return (False);
   }

   /*  do the copy  */


   for (;;)
   {
      size_t offset = 0;
      do {
         n = read(fold, buf, sizeof(buf));
      } while (n < 0 && errno == EINTR);
      if (n == 0)
         break;
      if (n < 0)
      {
         transferError = errno;
         break;
      }
      while (offset < (size_t)n)
      {
         ssize_t written = write(fnew, buf + offset, (size_t)n - offset);
         if (written < 0 && errno == EINTR)
            continue;
         if (written <= 0)
         {
            transferError = written == 0 ? EIO : errno;
            break;
         }
         offset += (size_t)written;
      }
      if (transferError)
         break;
   }

   /* A descriptor is closed once, including when close reports EINTR.
    * Preserve the first transfer error across cleanup and error reporting.
    * Never remove the source until both descriptors closed successfully.
    */
   if (close(fold) < 0 && !transferError)
      transferError = errno;
   if (close(fnew) < 0 && !transferError)
      transferError = errno;

   if (transferError)
   {
      if (errorHandler)
      {
         char * strerrormsg = NULL;
         char * catmsg = NULL;
         char * samsg = NULL;
         const char *errnoMsg;
         Boolean unknown = False;
         int bufLen;

         switch (transferError)
         {
#ifdef EDQUOT
             case EDQUOT:
             {
                 if(mode == COPY_FILE)
                   tmpStr = (GETMESSAGE(11,51, "Unable to copy the file/folder because\nthe disk quota will be exceeded on the disk\nyou are copying it to."));
                 else
                   tmpStr = (GETMESSAGE(11,52, "Unable to move the file/folder because\nthe disk quota will be exceeded on the disk\nyou are moving it to."));
                 catmsg = XtNewString(tmpStr);
                 break;
             }
#endif
             case ENOSPC:
                 {
                 if(mode == COPY_FILE)
                   tmpStr = (GETMESSAGE(11,42, "No space available on the\ndevice you are copying to.\n\n"));
                 else
                   tmpStr = (GETMESSAGE(11,43, "No space available on the\ndevice you are moving to.\n"));
                 catmsg = XtNewString(tmpStr);
                 break;
                 }
             case EAGAIN:
                 errnoMsg = "EAGAIN:  ";
                 strerrormsg = strerror(transferError);
                 break;
             case EBADF:
                 errnoMsg = "EBADF:  ";
                 strerrormsg = strerror(transferError);
                 break;
             case EDEADLK:
                 errnoMsg = "EDEADLK:  ";
                 strerrormsg = strerror(transferError);
                 break;
             case EINTR:
                 errnoMsg = "EINTR:  ";
                 strerrormsg = strerror(transferError);
                 break;
             case EIO:
                 errnoMsg = "EIO:   ";
                 strerrormsg = strerror(transferError);
                 break;
             case ENOLCK:
                 errnoMsg = "ENOLCK:  ";
                 strerrormsg = strerror(transferError);
                 break;
             case EPIPE:
                 errnoMsg = "EPIPE:  ";
                 strerrormsg = strerror(transferError);
                 break;
             default:
                 unknown = True;
                 errnoMsg = GETMESSAGE(11,56, "(Unknown):");
                 strerrormsg = strerror(transferError);
                 break;
         }

         /* If catmsg is NULL then one of the miscellanous error's occurred.
          * Set up a generic error message which will output the internal
          * error message.
          */
         if(catmsg == NULL)
         {
           if(mode == COPY_FILE)
             tmpStr = (GETMESSAGE(11,53, "The copy of the file/folder failed\ndue to some internal error. The internal\nerror given is:"));
           else
             tmpStr = (GETMESSAGE(11,54, "The move of the file/folder failed\ndue to some internal error. The internal\nerror given is:"));
           catmsg = XtNewString(tmpStr);
           tmpStr = (GETMESSAGE(11,55, "Please see your System Administrator"));
           samsg = XtNewString(tmpStr);
         }

         /* Build a concatination of the possible message parts */
         bufLen = (strerrormsg ? strlen(strerrormsg) +
                   strlen(errnoMsg) + strlen(samsg) : 0) +
                   strlen(catmsg) + 10;
         msg = XtMalloc(bufLen);
         strcpy (msg, catmsg);
         if (strerrormsg)
         {
           strcat (msg, "\n\n");
           strcat (msg, errnoMsg);
           if(unknown)
             strcat (msg, "  ");
           strcat (msg, strerrormsg);
           strcat (msg, "\n\n");
           strcat (msg, samsg);
         }

         (*errorHandler) (w, msg, to);
         XtFree(msg);
         XtFree(catmsg);
         XtFree(samsg);
      }
      (void) unlink(to);
      free(filename);
      return (False);
   }

   /*  unlink <from> if not copy  */

   if (mode == MOVE_FILE)
   {
      if (unlink (from) < 0)
      {
         int source_error = errno;
         if (errorHandler)
         {
            char *tmpStr, *ptr, *slash;

            slash = strrchr(from, '/');
            ptr = slash == NULL ? from : slash + 1;
            tmpStr = (GETMESSAGE(11,38, "You do not have permission to move %s\nHowever, you can copy the object.\nTo copy an object:\n  - press and hold the <Ctrl> key, and\n  - drag the object with your mouse.\nOr\n  - use 'Copy To' in the 'Selected' menu popup of the menu bar."));
            msg = XtNewString(tmpStr);
            (*errorHandler) (w, msg, ptr);
            XtFree(msg);
         }
         if (unlink(to) != 0 && errorHandler)
            (*errorHandler)(w, GetSharedMessage(CANT_DELETE_ERROR), to);
         errno = source_error;
         free(filename);
         return (False);
      }
   }


   /* Replacement, including the old target's ownership policy, belongs to
    * the worker. This function only creates an unoccupied destination. */
   (void) chown(to, getuid(), getgid());

   free(filename);
   return (True);
}
