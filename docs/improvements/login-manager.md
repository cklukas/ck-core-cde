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

## Tests

`programs/dtlogin/tests` holds regression tests for the XDMCP handlers,
authorization data and the signal and process handling. They cut the production
functions out of the sources, wrap them with stubs and failure injection and run
under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make -C programs/dtlogin check
```

Run against the code from before these changes, the same test files fail 26 of
their 28 scenarios; the other two check results that did not change.

## Limits

- The tests exercise the functions directly. A complete login through the
  greeter with PAM has not been run in the check suite.
- Session-side signal handling, the chooser socket and allocation-failure paths
  are not yet reviewed.
