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

## Screen lock

- The typed password is overwritten in memory once it has been checked, when
  the line is killed with Escape, when characters are backed over and when the
  dialog times out; it used to stay in the heap buffer.
- A typed password is compared with the hash of the account only when the
  account has a usable one. `crypt()` returns a null pointer for the hash of a
  locked account (`!`, `*`), for the `x` that stands in `/etc/passwd` on a
  shadowed system and for an empty field, and the C library of some systems
  returns a failure token instead. The lock used to compare whatever came back
  with `strcmp`, so a wrong password on such a system ended `dtsession` with a
  crash, and a hash that is its own failure token (`*0`) accepted any password.
  Such an account does not unlock the display now, and the comparison does not
  stop at the first different byte.
- On a system that has a shadow entry for the account, only the shadow hash is
  consulted. Before, an old password left in `/etc/passwd` unlocked the display
  as well.
- With PAM, the service name is the program name without its directory. A
  `dtsession` started through the search path (no `/` in `argv[0]`) failed every
  unlock attempt.

## Signals

- The shutdown holds `SIGTERM` while it runs and gives the signal mask back
  when it ends without exiting, which is what happens when a client cancels it;
  the next signal then starts a new shutdown. A signal that arrives during a
  shutdown is held until it ends, and a second logout request made while one is
  under way is dropped. The wait of the shutdown for the answers of the clients
  runs the event loop with `XtAppProcessEvent`: the answers, a client's cancel and
  the timeout arrive as callbacks, which `XtAppNextEvent` runs without returning
  until an X event comes in, so a cancelled shutdown of an idle display never
  ended. If the signal cannot be set up (the pipe, its flags or
  the handler), the failure is logged and `SIGTERM` gets its default action,
  even when the program was started with it ignored.
- `SIGTERM` (sent by the login manager when the session ends, or by `kill`)
  starts a normal shutdown. The handler used to run the whole shutdown: it sent
  ToolTalk notices, saved the session, allocated memory, ran an event loop and
  started the user's `sessionexit` script from inside the signal handler, which
  deadlocks or corrupts memory if the signal arrives while the program is in
  `malloc` or Xlib. The handler now writes one byte to a pipe and returns, and
  the event loop carries out the shutdown. Several signals cause one shutdown;
  a shutdown that a client cancels can be requested again. If the pipe cannot
  be created the signal keeps its default action and ends the process.

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
`SVR4`. `check-lock-authentication.py` runs `localAuthenticate()` of the PAM and
non-PAM builds against a password database and a `crypt()` that can fail.
`check-signals.py` raises `SIGTERM` against the production handler and checks
that nothing of the shutdown runs inside it.
