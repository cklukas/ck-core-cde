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
