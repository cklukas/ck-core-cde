/*
 * CDE - Common Desktop Environment
 *
 * These libraries and programs are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * These libraries and programs are distributed without any warranty; without
 * even the implied warranty of merchantability or fitness for a particular
 * purpose. See the GNU Lesser General Public License for more details.
 *
 * Shared decoding and feedback for dtfile worker pipes.
 */
#ifndef DTFILE_FILE_OP_PROTOCOL_H
#define DTFILE_FILE_OP_PROTOCOL_H

#include <stddef.h>
#include <X11/Intrinsic.h>

int PipeReadExact(int fd, void *value, size_t size);
int PipeReadStringField(int fd, char **value);
void FileOpStartError(Widget parent, int error);
void FileOpProtocolError(Widget parent);

#endif
