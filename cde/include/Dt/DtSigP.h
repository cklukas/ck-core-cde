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
 * Installing a signal disposition.
 *
 * _DtSignal() replaces signal(2).  The disposition is installed with
 * sigaction(2), whose behaviour is specified by POSIX; signal(2) leaves open
 * whether the handler stays installed and whether interrupted system calls are
 * restarted.  Like the signal() of glibc and the BSD systems, the handler stays
 * installed and interrupted calls restart, and no other signal is blocked while
 * the handler runs.  The previous disposition is returned, or SIG_ERR.
 */
#ifndef _Dt_DtSigP_h
#define _Dt_DtSigP_h

#include <signal.h>

static inline void (*_DtSignal(int sig, void (*handler)(int)))(int)
{
    struct sigaction action = {};
    struct sigaction previous = {};

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(sig, &action, &previous) == -1)
        return SIG_ERR;
    return previous.sa_handler;
}

#endif /* _Dt_DtSigP_h */
