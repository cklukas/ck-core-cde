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

## Session files and directories

- Saving a session archives the directory it writes, "current" or "home", to
  its `.old` version and moves an older `.old` aside under a name made with
  `mkdtemp` (if no such name can be made, the older archive is dropped so that
  the save can go on). If a write fails, or the save is cancelled, the archive
  is put back from where it was made: the directory that was written and the
  one it was archived to are the ones the save chose, not ones derived from the
  kind of session, so a Home session that saves into "current" at logout no
  longer loses its Home directory, and the first save of a session, which
  archives nothing, no longer takes the user's `sessionetc` script for an
  archive. A cancelled save puts the archive back also when there was no older
  one. Pruning goes on only if the session was moved aside first, and runs the
  discard commands with the oldest archive in place.
- An entry that cannot be removed no longer stops the removal of the rest of a
  directory (as with `rm -rf`), and a copy of a session that fails is removed
  instead of being restored half-copied at the next start.
- Sessions are archived, pruned, restored and copied without a shell and
  without looking programs up in the search path. Removing a session directory
  ran `system("rm -rf <path>")` and copying one ran `system("cp -r <from> <to>")`
  with the unquoted path of the home directory, so a home directory with a
  space in its name removed or copied the wrong directories, and one with shell
  syntax in it ran that syntax. Moving a directory started `mv` through the
  search path in a `vfork` child. These steps are now done in process
  (`SmRemoveTree`, `SmCopyTree` and `rename`): a symbolic link in a session
  directory is removed or copied as a link, never followed, and a move onto an
  existing directory fails instead of nesting one inside the other.
- The error log is rotated again. The name of `errorlog` was built one
  character short, so it was never moved to `errorlog.old`; with a long home
  directory the buffers were resized through the wrong pointer. The files are
  now renamed instead of linked and unlinked, so a file system without hard
  links, or an empty `errorlog.old`, no longer loses the log.
- The paths of the session files are put together in fixed buffers. A home
  directory so long that they would not fit is refused like a home directory
  that cannot be written, and `dtsession` starts with the system defaults
  instead of overflowing the buffers or, when the directories cannot be created
  at all, dereferencing a null path.

## Saved sessions and palettes

A saved session is read from files in the user's home directory, and the
palette the color server loads from the files and resources named in
`Dtstyle.paletteDirectories`. A damaged or hand-edited file used to crash
`dtsession` while it started, or corrupt its memory.

- The quoting parser for the commands of a CDE 1.0 client file indexed one
  element past its table of nesting levels when quotes were nested ten deep.
- The per-screen buffers for hints, commands and remote commands were allocated
  with room for 5,000 bytes and grown by 5,000 bytes, whatever the length of the
  line that had to fit, and the check for growing them was off by one byte; a
  longer command or hint overran the heap. They are now grown to fit.
- An option without a value (`-screen` at the end of a line, `-cmd` with an
  empty command) dereferenced a null pointer, and the host or command of one
  line carried over to the next, which then started a local command as a
  remote one. The lines of the client file are sorted into the buffers by
  `ParseClientFile`, which starts every line from nothing.
- The font path list of the saved settings wrote one pointer past its array
  at 50 entries and went on through the freed array when it grew. The key
  mapping divided by a number of keysyms per code of zero, and the modifier
  map wrote `8 * maxKeyPerMod` bytes into a 5,000 byte buffer for any value
  the file named; it is accepted up to the 255 the protocol allows.
- Directories of `Dtstyle.paletteDirectories` are separated by blanks, but the
  list was walked without skipping them: the second directory of a list sent the
  color server into an endless loop. An entry longer than 255 bytes overran the
  stack, and the copy of a directory that held the palette was allocated with
  the length of the system directory. A palette with more colors than the table
  (eight), a first line longer than the read buffer or without a newline, or too
  few colors for a low color display read or wrote outside its buffers.
- The unused `MigrateResources` converter, which was not part of the build, is
  removed.

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
that nothing of the shutdown runs inside it. `check-session-files.py` runs the
error-log rotation, the directory moves, copies and removals and the session
path set-up on real files, with names that contain spaces and shell syntax, a
file system without hard links, an unwritable and an over-long home directory.
`check-session-parsers.py` feeds crafted client files and settings to the
production parsers and `RestoreSettings`, in a UTF-8 and the C locale.
`check-palette-parser.py` does the same for the palette directory list and
palette files.
