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

#ifndef _MAILGROUP_HH
#define _MAILGROUP_HH

#include <sys/types.h>
#include <unistd.h>

// The group a set-group-id installation of dtmail and dtmailpr runs with.
//
// A mailer that is installed set-group-id for the mail group needs that group
// for one reason: to create, lock and rewrite files in the mail spool.  It
// gives the group up as the first thing it does and takes it back only for
// the spool operations (the PRIV_ENABLED macros of RFCMailBox.C), one system
// call at a time.  Every change of the effective group id is verified, and a
// program that is left holding the group when it believes it has given it up
// stops instead of going on: anything it ran or started from then on (the
// transport, an action, a viewer) would run with write access to every
// user's mailbox.
//
// The saved group id keeps the installed group, which is what makes the
// switch back possible; no other part of the program changes a group id.
class MailGroup {
  public:
    MailGroup() : _installed(0), _user(0), _dropped(false) {}

    // Remember the installed and the real group id and give the installed one
    // up.  Stops the program when that does not work.  Calling it again does
    // nothing, so that the start of a derived application and of its base
    // class can both do it.
    void dropOrStop(void)
    {
        if (_dropped)
            return;
        _installed = getegid();
        _user = getgid();
        if (!setEffective(_user))
            stop("dtmail: cannot give up the group of the installed program\n");
        _dropped = true;
    }

    // Take the installed group for the next system call.  A refusal leaves
    // the program with the user's group, which cannot do more than it could
    // before, so the caller carries on and the operation fails or is repeated
    // without the group.
    bool enable(void) { return _dropped && setEffective(_installed); }

    // Give the installed group up again.  A refusal leaves the program with
    // the group held, so it stops.
    void disableOrStop(void)
    {
        if (_dropped && !setEffective(_user))
            stop("dtmail: cannot give up the group of the installed program\n");
    }

    gid_t installedGid(void) const { return _installed; }
    gid_t userGid(void) const { return _user; }

  private:
    static bool setEffective(gid_t gid)
    {
        if (getegid() == gid)
            return true;
        // do not take the call's word for it
        return setegid(gid) == 0 && getegid() == gid;
    }

    static void stop(const char *message)
    {
        const char *p = message;
        while (*p)
            p++;
        if (write(STDERR_FILENO, message, (size_t)(p - message)) < 0) {
            // nothing more can be done about it
        }
        _exit(1);
    }

    gid_t _installed;	// effective gid at startup: the mail group
    gid_t _user;	// real gid at startup
    bool  _dropped;
};

#endif
