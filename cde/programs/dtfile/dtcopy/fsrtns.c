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
/* $TOG: fsrtns.c /main/6 1998/10/26 12:41:20 mgreess $ */
/************************************<+>*************************************
 ****************************************************************************
 *
 *   FILE:           fsrtns.c
 *
 *
 *   DESCRIPTION:    Routines to manipulate files and directores
 *
 *   FUNCTIONS: CopyDir
 *		CopyFile
 *		CopyLink
 *		CopyObject
 *		EmptyDir
 *		EraseObject
 *		fsCopy
 *		fsCopyLink
 *		fsEmpty
 *		fsErase
 *		fsMove
 *		fsRename
 *
 *   (c) Copyright 1993, 1994, 1995 Hewlett-Packard Company
 *   (c) Copyright 1993, 1994, 1995 International Business Machines Corp.
 *   (c) Copyright 1993, 1994, 1995 Sun Microsystems, Inc.
 *   (c) Copyright 1993, 1994, 1995 Novell, Inc.
 *
 ****************************************************************************
 ************************************<+>*************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <utime.h>
#include <dirent.h>
#include <sys/file.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include "fsrtns.h"


/*--------------------------------------------------------------------
 * Callback functions
 *------------------------------------------------------------------*/

int (*progressCallback)(char *fname) = NULL;
int (*errorCallback)(char *fname, int errnum) = NULL;
int (*periodicCallback)(void) = NULL;

/*--------------------------------------------------------------------
 * Local subroutines
 *------------------------------------------------------------------*/

static int CopyObject(char *sourceP, char *targetP, int repl, int link, int *complete);
static int EraseObject(char *nameP, int force);

/* Allocate an exact child path; never truncate an operand of a file operation. */
static int ChildPath(const char *parent, const char *name, char **result)
{
  size_t base = strlen(parent), length = strlen(name), separator;
  char *path;
  *result = NULL;
  if (base == 0)
    return EINVAL;
  separator = parent[base - 1] != '/';
  if (base > SIZE_MAX - separator - 1 ||
      length > SIZE_MAX - base - separator - 1)
    return ENAMETOOLONG;
  path = malloc(base + separator + length + 1);
  if (!path)
    return ENOMEM;
  memcpy(path, parent, base);
  if (separator)
    path[base++] = '/';
  memcpy(path + base, name, length + 1);
  *result = path;
  return 0;
}


static int
CopyFile(char *sourceP, char *targetP, int repl, struct stat *statP)
/* copy a file; if repl non-zero, overwrite any existing file */
{
  int src, tgt;
  ssize_t nread, nwrite;
  char buffer[2048];
  struct utimbuf ut;
  int rc;

  /* open source file for read */
  src = open(sourceP, O_RDONLY, 0);
  if (src < 0)
    return errno;

  /* create target file */
  tgt = open(targetP, O_CREAT | O_EXCL | O_WRONLY, statP->st_mode & 0777);
  if (tgt < 0 && errno == EEXIST && repl) {
    rc = EraseObject(targetP, repl);
    if (rc) {
      close(src);
      return rc;
    }
    tgt = open(targetP, O_CREAT | O_EXCL | O_WRONLY, statP->st_mode & 0777);
  }
  if (tgt < 0) {
    rc = errno;
    close(src);
    return rc;
  }

  /* if we have root privileges, make sure file ownership is preserved */
  if (geteuid() == 0) {
    if (statP->st_uid != 0 || statP->st_gid != getegid()) {
      rc = fchown(tgt, statP->st_uid, statP->st_gid);
      if (rc) {
        rc = errno;
        close(src);
        close(tgt);
        return rc;
      }
    }
  }

  /* copy data */
  rc = 0;
  for (;;) {
    /* read data from source file */
    do {
      errno = 0;
      nread = read(src, buffer, sizeof(buffer));
    } while (nread < 0 && errno == EINTR);
    if (nread <= 0) {
      if (nread < 0)
        rc = errno;
      break;
    }

    /* A successful write may consume only part of the input block. */
    {
      size_t written = 0;
      while (written < (size_t)nread) {
        do {
          nwrite = write(tgt, buffer + written, (size_t)nread - written);
        } while (nwrite < 0 && errno == EINTR);
        if (nwrite <= 0) {
          rc = nwrite < 0 ? errno : EIO;
          break;
        }
        written += (size_t)nwrite;
      }
    }
    if (rc)
      break;

    if (periodicCallback)
      if (periodicCallback() != 0) {
        unlink(targetP);
        close(src);
        close(tgt);
        return -1;
      }
  }

  /* Preserve the first transfer error, including delayed close failures. */
  if (close(src) != 0 && rc == 0)
    rc = errno;
  if (close(tgt) != 0 && rc == 0)
    rc = errno;
  if (rc)
    return rc;

  /* set target file attributes */
  ut.actime = statP->st_atime;
  ut.modtime = statP->st_mtime;
  rc = utime(targetP, &ut);

  return (rc != 0)? errno: 0;
}


static int
CopyLink(char *sourceP, char *targetP, int repl, struct stat *statP)
/* copy a symbolic link */
{
  ssize_t length;
  int rc;
  size_t capacity = 256;
  char *buf = malloc(capacity + 1);
  (void)statP;
  if (!buf)
    return ENOMEM;
  for (;;) {
    char *grown;
    do {
      length = readlink(sourceP, buf, capacity);
    } while (length < 0 && errno == EINTR);
    if (length < 0) {
      rc = errno;
      goto done;
    }
    if ((size_t)length < capacity)
      break;
    /* A full buffer is not evidence that the entire link was read. */
    if (capacity > (size_t)SSIZE_MAX / 2) {
      rc = ENAMETOOLONG;
      goto done;
    }
    capacity *= 2;
    grown = realloc(buf, capacity + 1);
    if (!grown) {
      rc = ENOMEM;
      goto done;
    }
    buf = grown;
  }
  buf[length] = '\0';
  rc = symlink(buf, targetP) == 0 ? 0 : errno;
  if (rc == EEXIST && repl) {
    rc = EraseObject(targetP, repl);
    if (rc == 0)
      rc = symlink(buf, targetP) == 0 ? 0 : errno;
  }
done:
  free(buf);
  return rc;
}


static int
CopyDir(char *sourceP, char *targetP, int repl, int link, struct stat *statP,
        int *complete)
/* copy a directory and recursively all its subdirectories */
{
  DIR *dirP;                      /* open directory */
  struct dirent *entryP;          /* directory entry */
  char *srcname = NULL, *tgtname = NULL;
  int rc;
  int allCopied = 1;
  *complete = 0;

  /* open source directory */
  dirP = opendir(sourceP);
  if (dirP == NULL)
    return errno;

  /* create target directory */
  rc = mkdir(targetP, statP->st_mode & 0777);
  if (rc < 0 && errno == EEXIST && repl) {
    rc = EraseObject(targetP, repl);
    if (rc)
    {
        closedir(dirP);
        return rc;
    }
    rc = mkdir(targetP, statP->st_mode & 0777);
  }
  if (rc < 0) {
    rc = errno;
    closedir(dirP);
    return rc;
  }

  /* if we have root privileges, make sure directory ownership is preserved */
  if (geteuid() == 0) {
    if (statP->st_uid != 0 || statP->st_gid != getegid()) {
      rc = chown(targetP, statP->st_uid, statP->st_gid);
      if (rc) {
        rc = errno;
        closedir(dirP);
        return rc;
      }
    }
  }

  for (rc = 0; rc == 0; ) {
    do {
      errno = 0;
      entryP = readdir(dirP);
    } while (entryP == NULL && errno == EINTR);
    if (entryP == NULL) {
      rc = errno;
      break;
    }
    if (strcmp(entryP->d_name, ".") == 0 || strcmp(entryP->d_name, "..") == 0)
      continue;

    rc = ChildPath(sourceP, entryP->d_name, &srcname);
    if (rc == 0)
      rc = ChildPath(targetP, entryP->d_name, &tgtname);
    if (rc == 0) {
      int copied;
      rc = CopyObject(srcname, tgtname, repl, link, &copied);
      if (!copied)
        allCopied = 0;
    }
    free(srcname);
    free(tgtname);
    srcname = tgtname = NULL;
  }

  if (closedir(dirP) != 0 && rc == 0)
    rc = errno;
  *complete = rc == 0 && allCopied;
  return rc;
}


static int
CopyObject(char *sourceP, char *targetP, int repl, int link, int *complete)
/* copy a directory, file, or symbolic link */
{
  struct stat src_stat;
  int rc;
  int copied = 1;
  *complete = 0;

  if (progressCallback)
    if (progressCallback(sourceP) != 0)
      return -1;

  if (periodicCallback)
    if (periodicCallback() != 0)
      return -1;

  if (lstat(sourceP, &src_stat) < 0)
    rc = errno;

  else {

copy_switch:

    switch(src_stat.st_mode & S_IFMT) {

      case S_IFDIR:
        rc = CopyDir(sourceP, targetP, repl, link, &src_stat, &copied);
        break;

      case S_IFREG:
        rc = CopyFile(sourceP, targetP, repl, &src_stat);
        break;

      case S_IFLNK:
        if (link)
          rc = CopyLink(sourceP, targetP, repl, &src_stat);
        else if (stat(sourceP, &src_stat) < 0)
          rc = errno;
        else
          goto copy_switch;
        break;

      default:
        rc = EINVAL;
    }
  }

  /*
   * Return code zero means everything is ok;
   * return code -1 means the operation is aborted.
   * In either case, propagated the return code up.
   */
  if (rc == 0)
    *complete = copied;
  if (rc <= 0)
    return rc;

  /*
   * Return code > 0 means an error occurred in the last operation.
   * Call the the error callback function.  If the callback returns
   * zero, we return zero; this will cause the error to be ignored.
   * Otherwise we return -1 to signal that the operation is aborted.
   */
  if (!errorCallback)
    return rc;
  else if (errorCallback(sourceP, rc) == 0)
    return 0;
  else
    return -1;
}


int
EmptyDir(char *sourceP, int rm, int force)
{
  DIR *dirP;                      /* open directory */
  struct dirent *entryP;          /* directory entry */
  char *srcname;
  int rc;

  /* open source directory */
  dirP = opendir(sourceP);
  if (dirP == NULL)
    return errno;

  rc = 0;
  while (rc == 0) {
    do {
      errno = 0;
      entryP = readdir(dirP);
    } while (entryP == NULL && errno == EINTR);
    if (entryP == NULL) {
      rc = errno;
      break;
    }
    if (strcmp(entryP->d_name, ".") == 0 || strcmp(entryP->d_name, "..") == 0)
      continue;
    rc = ChildPath(sourceP, entryP->d_name, &srcname);
    if (rc == 0)
      rc = EraseObject(srcname, force);
    free(srcname);
  }

  if (closedir(dirP) != 0 && rc == 0)
    rc = errno;
  if (rc == 0 && rm) {
    rc = rmdir(sourceP);
    if (rc < 0)
      rc = errno;
  }
  return rc;
}


static int
EraseObject(char *nameP, int force)
{
  struct stat src_stat;
  int rc = 0;

  if (periodicCallback)
    if (periodicCallback() != 0)
      return -1;

  if (lstat(nameP, &src_stat) < 0)
    rc = errno;
  else if ((src_stat.st_mode & S_IFMT) == S_IFDIR) {
    if (access(nameP, X_OK|W_OK))
      return errno;
    rc = EmptyDir(nameP, 1, force);
  }
  else {
    if (!force && access(nameP, W_OK))
      return errno;

    if (unlink(nameP))
      rc = errno;
  }

  /*
   * Return code zero means everything is ok;
   * return code -1 means the operation is aborted.
   * In either case, propagated the return code up.
   */
  if (rc <= 0)
    return rc;

  /*
   * Return code > 0 means an error occurred in the last operation.
   * Call the the error callback function.  If the callback returns
   * zero, we return zero; this will cause the error to be ignored.
   * Otherwise we return -1 to signal that the operation is aborted.
   */
  if (!errorCallback)
    return rc;
  else if (errorCallback(nameP, rc) == 0)
    return 0;
  else
    return -1;
}

/*--------------------------------------------------------------------
 * Exported routines
 *------------------------------------------------------------------*/

void
fsRename(char *sourceP, char *targetP, int replace, int *rcP)
{
  int rc;
  struct stat buf;

  if (!replace) {
    /* return error if target file already exists */
    rc = lstat(targetP, &buf);
    if (rc == 0) {
      *rcP = EEXIST;
      return;
    } else if (errno != ENOENT) {
      *rcP = errno;
      return;
    }
  }
  *rcP = rename(sourceP, targetP);
  if (*rcP < 0)
    *rcP = errno;

  if (replace && *rcP == ENOTDIR || *rcP == EISDIR) {
    /* error reason: tried to replace file by directory or vice versa */
    *rcP = EraseObject(targetP, replace);
    if (*rcP < 0)
      return;
    *rcP = rename(sourceP, targetP);
    if (*rcP < 0)
      *rcP = errno;
  }
}


void
fsMove(char *sourceP, char *targetP, int replace, int *rcP)
{
  int complete;
  /* try to rename */
  fsRename(sourceP, targetP, replace, rcP);
  if (*rcP == 0 || *rcP != EXDEV)
    return;

  /* source and target on different file systems: do copy + erase */
  {
    /* first check if we have write permission in the source directory */
    char *dir, *p = strrchr(sourceP, '/');
    size_t length = p ? (p == sourceP ? 1 : (size_t)(p - sourceP)) : 1;
    dir = malloc(length + 1);
    if (!dir) {
      *rcP = ENOMEM;
      return;
    }
    memcpy(dir, p ? sourceP : ".", length);
    dir[length] = '\0';
    *rcP = access(dir, W_OK) == 0 ? 0 : errno;
    free(dir);
    if (*rcP)
      return;
  }

  *rcP = CopyObject(sourceP, targetP, replace, 1, &complete);
  if (*rcP != 0 || !complete)
    return;
  *rcP = EraseObject(sourceP, replace);
}


void
fsCopy(char *sourceP, char *targetP, int replace, int *rcP)
{
   int complete;
   *rcP = CopyObject(sourceP, targetP, replace, 0, &complete);
}


void
fsCopyLink(char *sourceP, char *targetP, int replace, int *rcP)
{
  int complete;
  *rcP = CopyObject(sourceP, targetP, replace, 1, &complete);
}


void
fsErase(char *nameP, int *rcP, int force)
{
  *rcP = EraseObject(nameP, force);
}


void
fsEmpty(char *nameP, int *rcP)
{
  *rcP = EmptyDir(nameP, 0, 0);
}

