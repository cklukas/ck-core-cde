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
#ifndef _BOUNDED_H
#define _BOUNDED_H

/*
 * Call JOB (ARGUMENT) and wait at most SECONDS (0: as long as it takes) for it.
 * Returns 1 and the result of the job in *RESULT when it returned in time, 0
 * when it did not and -1 when it could not be started.  A job that did not
 * return in time is left running and owns ARGUMENT from then on.
 */
extern int BoundedCall(unsigned seconds, void *(*job)(void *), void *argument, void **result);

#endif /* _BOUNDED_H */
