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
/* $XConsortium: CanvasOs.c /main/5 1996/05/09 03:40:38 drk $ */
/************************************<+>*************************************
 ****************************************************************************
 **
 **   File:	   UtilSDL.c
 **
 **   Project:     Cde Help System
 **
 **   Description: Utility functions for parsing an SDL volume.
 **
 **  (c) Copyright 1987, 1988, 1989, 1990, 1991, 1992 Hewlett-Packard Company
 **
 **  (c) Copyright 1993, 1994 Hewlett-Packard Company
 **  (c) Copyright 1993, 1994 International Business Machines Corp.
 **  (c) Copyright 1993, 1994 Sun Microsystems, Inc.
 **  (c) Copyright 1993, 1994 Novell, Inc.
 **
 ****************************************************************************
 ************************************<+>*************************************/

/*
 * system includes
 */
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Canvas Engine includes
 */
#include "CanvasP.h"

/*
 * private includes
 */
#include "bufioI.h"
#include "CanvasOsI.h"
#include "FormatUtilI.h"

#ifdef NLS16
#endif

/********    Private Function Declarations    ********/
/********    End Private Function Declarations    ********/

/******************************************************************************
 *
 * Private defines.
 *
 *****************************************************************************/
/******************************************************************************
 *
 * Private macros.
 *
 *****************************************************************************/
/******************************************************************************
 *
 * Private data.
 *
 *****************************************************************************/
/******************************************************************************
 *
 * Private Functions
 *
 *****************************************************************************/
/*
 * Create the file "<dir>/dtXXXXXX" with mkstemp(3), where <dir> is $TMPDIR or
 * the system default.  The file exists (mode 0600, owned by the caller) when
 * this returns, so no other user can plant a file or symbolic link at the
 * name.  Returns the open descriptor and the name in malloc'ed storage, or -1
 * with nothing created.
 */
static int
CreateTempFile (
    char	**ret_name)
{
    char	 path[PATH_MAX];
    char	*dir = getenv("TMPDIR");
    char	*name;
    int		 len;
    int		 fd;

    if (dir == nullptr || *dir == '\0')
	dir = P_tmpdir;

    len = snprintf(path, sizeof(path), "%s/dtXXXXXX", dir);
    if (len < 0 || (size_t) len >= sizeof(path))
	return -1;

    fd = mkstemp(path);
    if (fd == -1)
	return -1;

    name = strdup(path);
    if (name == nullptr)
      {
	close(fd);
	unlink(path);
	return -1;
      }

    *ret_name = name;
    return fd;
}

/******************************************************************************
 *
 * Semi Public Functions
 *
 *****************************************************************************/
/*********************************************************************
 * Function: _DtCvRunInterp
 *
 *    _DtCvRunInterp calls a script and maybe gets data.
 *
 *********************************************************************/
int
_DtCvRunInterp(
    int			(*filter_exec)(_DtCvPointer, const char *, char **),
    _DtCvPointer	  client_data,
    char		 *interp,
    char                 *data,
    char                **ret_data)
{
    int          result;
    int          myFd;
    FILE        *myFile;
    int          size;
    int          writeBufSize = 0;
    char        *writeBuf     = nullptr;
    char        *ptr;
    char        *fileName;
    char        *newData;
    char         readBuf[BUFSIZ];
    BufFilePtr   myBufFile;

    /*
     * ask for permission to run the interperator command.
     */
    newData = data;
    if (filter_exec != nullptr && (*filter_exec)(client_data,data,&newData) != 0)
	return -1;

    /*
     * create a temporary file to write the data to.
     */
    myFd = CreateTempFile(&fileName);
    if (myFd == -1)
      {
	if (newData != data)
	    free(newData);
	return -1;
      }

    /*
     * write the data to file.
     */
    result = write(myFd, newData, strlen(newData));

    if (result != -1)
      {
	/*
	 * change the access permissions so the file is read-only for
	 * the interpreter, which runs as this user.
	 */
	result = fchmod(myFd, S_IRUSR);
      }

    /*
     * close the file.
     */
    close(myFd);

    if (newData != data)
	free(newData);

    if (result == -1)
      {
	unlink(fileName);
	free(fileName);
	return -1;
      }

    /*
     * create the system command string with its parameters
     */
    ptr = (char *) malloc(strlen(interp) + strlen(fileName) + 2);
    if (!ptr)
      {
	unlink(fileName);
	free(fileName);
        return -1;
      }

    strcpy (ptr, interp);
    strcat (ptr, " ");
    strcat (ptr, fileName);

    myFile = popen(ptr, "r");

    /*
     * free the command
     */
    free (ptr);

    /*
     * check for problems
     */
    if (!myFile) /* couldn't create execString process */
      {
	unlink(fileName);
	free(fileName);
        return -1;
      }

    /*
     * create a file handler for the pipe.
     */
    myBufFile = _DtHelpCeCreatePipeBufFile(myFile);
    if (myBufFile == nullptr)
      {
        (void) pclose(myFile); /* don't check for error, it was popen'd */
	unlink(fileName);
	free(fileName);
        return -1;
      }

    /*
     * read the file the pipe writes to until the pipe finishes.
     * Create a string from the results
     */
    do {
        readBuf[0] = '\0';
        ptr        = readBuf;

        result = _DtHelpCeGetNxtBuf(myBufFile, readBuf, &ptr, BUFSIZ);

        if (result > 0)
          {
            char *newBuf;

            size = strlen(readBuf);
            if (writeBuf == nullptr)
                newBuf = (char *) malloc (size + 1);
            else
                newBuf = (char *) realloc (writeBuf, writeBufSize + size + 1);

            if (newBuf != nullptr)
              {
                writeBuf = newBuf;
                writeBuf[writeBufSize] = '\0';
                strcat(writeBuf, readBuf);
                writeBufSize += size;
              }
            else
                result = -1;	/* writeBuf, if any, is freed below */
          }
    } while (result != -1 && !feof(FileStream(myBufFile)));

    /*
     * close the pipe
     */
    _DtHelpCeBufFileClose (myBufFile, True);

    if (result == -1)
      {
	if (writeBuf != nullptr)
	    free(writeBuf);
	
	writeBuf = nullptr;
      }
    else
	result = 0;

    /*
     * unlink the temporary file and free the memory.
     */
    unlink(fileName);
    free(fileName);

    /*
     * return the data
     */
    *ret_data = writeBuf;

    return result;

}  /* End _DtCvRunInterp */
