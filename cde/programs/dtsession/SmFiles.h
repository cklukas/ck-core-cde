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
/*************************************<+>*************************************
 **
 **  File:        SmFiles.h
 **
 **  Project:     DT Session Manager (dtsession)
 **
 **  Description:
 **  -----------
 **  File and directory operations on the session directories, done in
 **  process: no shell, no search path, no limit on the length of a name
 **  other than the one the system sets.
 **
 *************************************<+>*************************************/

#ifndef _SmFiles_h
#define _SmFiles_h

/*
 * Join the components with "/" into a newly allocated string (free() it).
 * The list ends with a null pointer.  Returns a null pointer if memory is
 * short.
 */
extern char *SmJoinPath(const char *first, ...);

/*
 * Remove a file or a directory with everything below it.  Symbolic links are
 * removed, not followed.  A path that does not exist is not an error.  An entry
 * that cannot be removed does not stop the others from being removed (as with
 * "rm -rf").  Returns 0 on success, -1 otherwise, with errno of the first
 * failure.
 */
extern int SmRemoveTree(const char *path);

/*
 * Copy a file or a directory tree to a path that does not exist yet.
 * Symbolic links are copied as links.  Returns 0 on success, -1 (errno set)
 * otherwise; a partial copy is left in place.
 */
extern int SmCopyTree(const char *from, const char *to);

/*
 * Create a new, empty directory "<directory>/<prefix>.XXXXXX" with a name no
 * other process can have chosen and return its path in a newly allocated
 * string (free() it); a null pointer (errno set) if it cannot be made.
 */
extern char *SmMakeTempDirectory(const char *directory, const char *prefix);

#endif /* _SmFiles_h */
