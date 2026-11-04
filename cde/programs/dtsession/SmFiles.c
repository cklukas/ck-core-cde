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
 **  File:        SmFiles.c
 **
 **  Project:     DT Session Manager (dtsession)
 **
 **  Description:
 **  -----------
 **  File and directory operations on the session directories.  See SmFiles.h.
 **
 *************************************<+>*************************************/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "SmFiles.h"

/* directories nested deeper than this are not walked */
#define MAX_DEPTH 64

char *
SmJoinPath(const char *first, ...)
{
    va_list args;
    const char *part;
    size_t length = 1;
    char *path, *end;
    int firstPart = 1;

    va_start(args, first);
    for (part = first; part != NULL; part = va_arg(args, const char *))
        length += strlen(part) + 1;
    va_end(args);

    path = malloc(length);
    if (path == NULL)
        return NULL;
    end = path;
    *end = '\0';

    va_start(args, first);
    for (part = first; part != NULL; part = va_arg(args, const char *)) {
        if (!firstPart)
            *end++ = '/';
        firstPart = 0;
        end = stpcpy(end, part);
    }
    va_end(args);

    return path;
}

/* the names in a directory, without "." and ".."; the descriptor is closed */
static int
ReadNames(int fd, char ***namesRet, size_t *countRet)
{
    DIR *dir = fdopendir(fd);
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0, size = 0;

    if (dir == NULL) {
        close(fd);
        return -1;
    }
    for (;;) {
        /* readdir() leaves errno alone at the end of the directory, so only
           the value set by this call may be looked at */
        errno = 0;
        entry = readdir(dir);
        if (entry == NULL)
            break;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (count == size) {
            char **grown = realloc(names, (size ? size * 2 : 16) * sizeof(char *));
            if (grown == NULL)
                goto failed;
            names = grown;
            size = size ? size * 2 : 16;
        }
        names[count] = strdup(entry->d_name);
        if (names[count] == NULL)
            goto failed;
        count++;
    }
    if (errno != 0)
        goto failed;
    closedir(dir);
    *namesRet = names;
    *countRet = count;
    return 0;

failed:
    {
        int saved = errno ? errno : ENOMEM;
        while (count > 0)
            free(names[--count]);
        free(names);
        closedir(dir);
        errno = saved;
        return -1;
    }
}

static void
FreeNames(char **names, size_t count)
{
    while (count > 0)
        free(names[--count]);
    free(names);
}

static int RemoveEntry(int parent, const char *name, int depth);

/*
 * Remove everything in a directory; the descriptor is closed.  An entry that
 * cannot be removed is skipped; the result is -1 with errno of the first one.
 */
static int
RemoveContents(int fd, int depth)
{
    char **names;
    size_t count, i;
    int result = 0, firstErrno = 0;
    int dirfd;

    dirfd = dup(fd);
    if (dirfd < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (ReadNames(fd, &names, &count) != 0) {
        int saved = errno;
        close(dirfd);
        errno = saved;
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (RemoveEntry(dirfd, names[i], depth) != 0 && result == 0) {
            result = -1;
            firstErrno = errno;
        }
    }
    FreeNames(names, count);
    close(dirfd);
    if (result != 0)
        errno = firstErrno;
    return result;
}

static int
RemoveEntry(int parent, const char *name, int depth)
{
    struct stat st;

    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        int fd;

        if (depth >= MAX_DEPTH) {
            errno = ELOOP;
            return -1;
        }
        fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            return -1;
        if (RemoveContents(fd, depth + 1) != 0)
            return -1;
        return unlinkat(parent, name, AT_REMOVEDIR);
    }
    return unlinkat(parent, name, 0);
}

int
SmRemoveTree(const char *path)
{
    struct stat st;
    int fd;

    if (path == NULL || path[0] == '\0' || !strcmp(path, "/")) {
        errno = EINVAL;
        return -1;
    }
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (RemoveContents(fd, 1) != 0)
        return -1;
    return rmdir(path);
}

static int
CopyFile(const char *from, const char *to, mode_t mode)
{
    char buffer[8192];
    int in, out, saved;
    ssize_t n;

    in = open(from, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0)
        return -1;
    out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) {
        saved = errno;
        close(in);
        errno = saved;
        return -1;
    }
    while ((n = read(in, buffer, sizeof(buffer))) != 0) {
        const char *at = buffer;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            goto failed;
        }
        while (n > 0) {
            ssize_t written = write(out, at, (size_t) n);

            if (written < 0) {
                if (errno == EINTR)
                    continue;
                goto failed;
            }
            at += written;
            n -= written;
        }
    }
    if (fchmod(out, mode & 0777) != 0)
        goto failed;
    close(in);
    return close(out);

failed:
    saved = errno;
    close(in);
    close(out);
    errno = saved;
    return -1;
}

static int
CopyEntry(const char *from, const char *to, int depth)
{
    struct stat st;

    if (lstat(from, &st) != 0)
        return -1;

    if (S_ISLNK(st.st_mode)) {
        char *target = malloc((size_t) st.st_size + 1);
        ssize_t length;
        int result;

        if (target == NULL)
            return -1;
        length = readlink(from, target, (size_t) st.st_size);
        if (length < 0) {
            int saved = errno;
            free(target);
            errno = saved;
            return -1;
        }
        target[length] = '\0';
        result = symlink(target, to);
        {
            int saved = errno;
            free(target);
            errno = saved;
        }
        return result;
    }

    if (S_ISREG(st.st_mode))
        return CopyFile(from, to, st.st_mode);

    if (S_ISDIR(st.st_mode)) {
        char **names;
        size_t count, i;
        int fd, result = 0;

        if (depth >= MAX_DEPTH) {
            errno = ELOOP;
            return -1;
        }
        fd = open(from, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 || ReadNames(fd, &names, &count) != 0)
            return -1;
        if (mkdir(to, 0700) != 0) {
            int saved = errno;
            FreeNames(names, count);
            errno = saved;
            return -1;
        }
        for (i = 0; i < count && result == 0; i++) {
            char *source = SmJoinPath(from, names[i], NULL);
            char *target = SmJoinPath(to, names[i], NULL);

            if (source == NULL || target == NULL)
                result = -1, errno = ENOMEM;
            else
                result = CopyEntry(source, target, depth + 1);
            free(source);
            free(target);
        }
        {
            int saved = errno;
            FreeNames(names, count);
            errno = saved;
        }
        if (result == 0)
            result = chmod(to, st.st_mode & 0777);
        return result;
    }

    /* sockets, devices and the like are not part of a session */
    return 0;
}

int
SmCopyTree(const char *from, const char *to)
{
    if (from == NULL || to == NULL || from[0] == '\0' || to[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    return CopyEntry(from, to, 0);
}

char *
SmMakeTempDirectory(const char *directory, const char *prefix)
{
    size_t length = strlen(prefix) + sizeof(".XXXXXX");
    char *leaf = malloc(length);
    char *path;

    if (leaf == NULL)
        return NULL;
    snprintf(leaf, length, "%s.XXXXXX", prefix);
    path = SmJoinPath(directory, leaf, NULL);
    free(leaf);
    if (path == NULL)
        return NULL;
    if (mkdtemp(path) == NULL) {
        int saved = errno;
        free(path);
        errno = saved;
        return NULL;
    }
    return path;
}
