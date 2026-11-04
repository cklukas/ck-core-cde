---
title: Session manager
description: How dtsession gives up its installed identity, authenticates the screen lock, handles signals and session files, parses saved sessions and serves session-management clients.
---

# Session manager

`dtsession` starts the user's desktop session, restores the clients saved in
it, locks the screen and saves the session again at logout. It reads the files
of a saved session, answers X clients and the clients of the X Session
Management Protocol (XSMP) over ICE sockets, and receives ToolTalk requests.
On systems without PAM it is installed setuid root so that it can read the
password database when the screen is unlocked; everywhere else it runs as the
user who started it.

## Installed identity

- Giving up the installed identity is the first thing `main()` does, before the
  locale is set, before the environment is read and before the toolkit or the
  display are opened. Earlier, the locale files were opened and parsed while the
  process still ran as root.
- Every step is checked. When `dtsession` is setuid and cannot return to the
  user (a user at a process limit makes `setreuid` fail with `EAGAIN`, a kernel
  or security module can refuse it), it stops with a message instead of running
  the whole session as root. After the change the real and effective user ids
  must be the user's, and unless the password database is needed later, the
  installed user id must not be recoverable.
- Where passwords are kept in a file only root can read (`/etc/shadow`), the
  installed user id stays available as the saved set-user-ID. It is taken
  for the short moment the password database is read, and a failure to give it up
  again stops the process. Every program `dtsession` starts for the user, and
  the helper processes it forks, give up the saved id and the installed group
  before they exec; a client can no longer start with the privileged group
  because the `setregid` call that was meant to drop it failed unnoticed.
- The code for the Tru64 security library (`SIA`) is removed. Nothing in the
  build defined it and it could not take the identity it asked for.

To check an installation, run the setuid `dtsession` under
`strace -f -u <user> -e trace=openat,setresuid`: the first `setresuid` must come
before the first open of a locale file, which it did not before.
`make -C programs/dtsession check` runs the same source guard.

## Tests

`programs/dtsession/tests` holds regression tests that cut the production code
out of the sources, wrap it with stubs and failure injection and run it under
AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make -C programs/dtsession check
```

`check-privileges.py` runs `SmPriv.c` against a fake kernel that models the
real, effective and saved ids and can refuse a call, report success without
changing anything or leave a way back to root, in the builds with and without
`SVR4`.
