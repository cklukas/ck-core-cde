---
title: Login manager
description: How dtlogin handles XDMCP requests, authorization data, signals and privileges.
---

# Login manager

`dtlogin` runs as `root`. It starts X servers, shows the login screen and starts
the user's session, and it can answer XDMCP requests from the network. It
validates what it receives, creates its files with care and gives up
privileges only when that really worked.

## XDMCP

- A `Request` that offers only loopback addresses, or addresses that are
  malformed, is declined. The selection of the connection type no longer returns
  an index past its arrays.
- Addresses shorter than the family requires are not read beyond their length.
- `XDM-AUTHORIZATION-1` is only accepted after its handshake; a request for it
  without one is declined. Authorization data is zeroed when a request is
  repeated, so it is neither used uninitialized nor leaked.
- The `Willing` status text is read from a pipe, not from a temporary file named
  by `tmpnam`, and a command that prints nothing leaves the status empty instead
  of a stale command line.
- A chooser connection that sends nothing is closed after a timeout instead of
  blocking the display manager.

## Configuration files

- The `Xaccess` file: a word longer than the 255 characters the reader has room
  for is cut off and reported, where it used to be written over the neighbouring
  data. A display that does not resolve costs its own entry only; before, the
  rest of the file was not read. The wildcard matcher of display patterns takes
  time in proportion to the lengths of the pattern and the name (a pattern of
  `*a*a*a…` used to take exponential time), and a pattern that ends in a
  backslash no longer reads beyond its end. An allocation failure stops the
  reader instead of dereferencing a null pointer, and an entry that offers a
  chooser is no longer read after it was freed.
- The address of the local host is looked up once and kept. It used to be looked
  up and leaked on every use, and a host name that does not resolve ended the
  login manager with a null pointer dereference; now it matches no address.
- The `Xservers` file: `local_uid` at the end of a line, a display that is
  listed again with a class after it was listed without, a very long host
  name for the `*` display and a failed allocation are handled instead of
  dereferencing a null pointer or writing past a buffer.
- The search for a configuration file looks for a regular file that can be
  read instead of opening it, so a pipe in the search path cannot block the
  manager, and an allocation failure ends the search.
- The language in a resource file name is expanded without reading past the end
  of a name that ends in `%`, and a failed allocation is passed on. The
  directories of the alternate desktops are read with a list that is checked
  while it grows, the number of alternate desktops the resource files can add
  is limited to 100, and the resource value that says whether a file adds one
  is no longer modified in the database while it is read.
- A failed allocation of the program name ends the login manager with its
  message instead of crashing, and a failed allocation of the argument list
  keeps the resources that are loaded.

## Displays

- A `Manage` request replaces the session of a display of the same name only
  when it comes from the same host. A display of the configuration file, or one
  that another host manages, is not ended by a request that names it; the
  request is answered with a failure.
- A `Manage` request that runs out of memory takes its display out of the
  display list before it is freed; the list kept a pointer to freed memory.
  Display records that failed to be created no longer keep the shared page
  they mapped, and a removed display releases the authorization names, search
  paths and name lengths it held.

## Chooser

The chooser lets a display that sent an indirect query pick the host that
manages it. The chooser program runs on the manager's host and reports the
choice back to the manager.

- The manager listens for the report on a Unix domain socket in the
  authorization directory (`dtlogin-chooser-<pid>`, mode `0600`) and passes its
  path to the chooser in `-xdmaddress`. It used to listen on a TCP port that was
  open on every interface, so any process and any host could answer for a
  chooser. The socket is not inherited by the sessions the manager starts and
  is removed when the manager stops.
- A report is accepted only for a client whose display was started with a
  chooser. Reports for other clients are dropped, and the host a client chose
  is kept for the 15 seconds in which its display sends the next query. Before,
  a report was dropped in every case, so the chosen host was never used.
- The list of clients that sent an indirect query is limited to 256 entries and
  an entry that is not followed up is dropped after ten minutes, so datagrams
  with forged source addresses cannot make the manager grow without limit.
- A chooser report that names a client address of any length no longer
  overflows the buffer of the debug log.

## Authorization data

- Authorization cookies come from `/dev/urandom` and nothing else. If the random
  source cannot be read, no cookie is generated.
- Authorization and backup files are created with `mkstemp` and `O_EXCL`, in
  sized buffers, and the lock files are removed only by the process that holds
  them. A missing `HOME` no longer unlocks a lock that was never taken.

## Sessions and signals

- The signal handlers for shutdown, rescan and child exit set flags; the main
  loop acts on them. A shutdown request waits for the X server that is starting
  (up to five seconds) before it is carried out.
- The resource loader runs the configured `xrdb` command without a shell. The
  display name comes from the network for XDMCP displays, so it is passed as an
  argument. The configured command is split into words as before; shell
  syntax in it is no longer interpreted.
- If the X server or session process cannot change its user, group or
  supplementary groups, that process stops instead of running with the display
  manager's privileges.
- The length of a message sent to the greeter is checked before it is copied.

## Authentication through PAM

On a system with PAM, `dtlogin` checks passwords with the helper of `libDtSvc`
(`DtUtil2/SvcPam.c`, which also starts the session and sets the credentials).

- The account is checked as well as the password. `pam_acct_mgmt` runs after a
  successful `pam_authenticate`, so an expired or locked account and a refusal
  by `pam_nologin` stop the login; the `account` entries of the `dtlogin`
  service were never consulted before. A password that has to be changed is
  reported as such (the greeter's expired-password dialog) and the login goes on.
- Every authentication starts from a new handle. A handle kept the user of an
  earlier attempt, so after a failed attempt for one user the next attempt, for
  another one, checked the password against the first; the helper also read an
  uninitialized status when it was given a handle without a service name. A
  refused login ends its handle.
- The conversation reads the messages as the array of pointers that PAM passes
  (it stepped through them as an array of structs), answers the hidden prompt
  with the password, and fails a visible question (a user name) or a prompt
  when it has no password, instead of answering with nothing. The password is on
  loan to the conversation for the one call; the pointer stayed in a static
  afterwards. What a failed conversation allocated is freed.

## Tests

`programs/dtlogin/tests` holds regression tests for the XDMCP handlers,
authorization data and the signal and process handling. They cut the production
functions out of the sources, wrap them with stubs and failure injection and run
under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make -C programs/dtlogin check
```

`tests/check-config.py` runs the production readers of the `Xaccess` and
`Xservers` files, the search path, the language expansion and the alternate
desktop loader with lines and allocation failures they must handle.

`tests/check-displays.py` runs the display list and the `Manage` handler with
failing allocations and rival peers (an XDMCP display of the same host, of
another host and of the configuration file).

`tests/check-chooser.py` runs the chooser socket, the choices and the client list
with a real Unix socket: the file and its mode, the argument that names it, a
report with and without a chooser, a 255 byte client address, the limit on the
list and the expiry.

`lib/DtSvc/tests/check-svc-pam.py` compiles the PAM helper against a stand-in for
the PAM library: a right and a wrong password, an account that may not log in, a
password that has to be changed, a second attempt for another user, the session
calls on the handle of the login, setup failures, a conversation with several
prompts, with a visible question and without a password, and failing
allocations.

Run against the code from before these changes, the same test files fail 26 of
their 28 scenarios; the other two check results that did not change.

## Greeter exit and the PAM stack

- The greeter no longer crashes with `SIGSEGV` when it exits after a login. It
  closed its message catalog whether or not one had been opened, and closing a
  null or failed handle ends the process in the C library; `dtlogin` logged
  "signal = 11" after every successful login. The catalog is closed only when
  it was opened and is not used afterwards.
- The shipped PAM stack (`config/dtlogin.src`) lists `pam_lastlog` as an
  optional session module with the `-` prefix, so a system that does not ship
  the module no longer logs an error for every login.

`tests/check-greeter-catalog.py` runs the catalog functions of the greeter with
stand-ins that abort on an invalid handle, in each state the greeter can be in
at exit; the code from before the change fails all four scenarios.

## Limits

- The tests exercise the functions directly. A complete login through the
  greeter with PAM is not part of the check suite; it was run by hand against
  real PAM (correct and wrong passwords, a second user after a failed attempt,
  expired and locked accounts, an expired password, empty passwords and a
  `pam_nologin` refusal).
- Session-side signal handling, the chooser socket and allocation-failure paths
  are not yet reviewed.
