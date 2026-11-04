---
title: Subprocess control daemon
description: How dtspcd and the SPC protocol validate requests, authenticate users and give up privileges.
---

# Subprocess control daemon

`dtspcd` runs commands for a registered user on behalf of CDE clients, locally
or on another host. Clients talk to it with the SPC protocol in `libDtSvc`.
The daemon is started by `inetd` as `root` and becomes the user that
authenticates, so everything it accepts before that point comes from an
unauthenticated peer.

## Requests

Every request has a 20-byte header and a payload. Both the daemon and the
client library validate what they receive:

- The first 18 header bytes are hexadecimal digits in fixed fields: channel id,
  request type, payload length and sequence number. A header with any other
  character is a protocol error and ends the connection.
- The request type is one the protocol defines (`0` to `NREQS - 1`). A type that
  has no handler on the receiving side, such as a stdout or reply message sent
  to the daemon, is refused.
- The payload is at most `SPC_BUFSIZ` (4096) bytes. The request buffer holds a
  full-size payload and the NUL that the reader stores after it.
- A request that acts on a channel must name a channel of the connection. An
  unknown channel id is answered with an error reply.
- Channel ids come from a per-process counter. They no longer contain part of
  the channel's address.

The fields inside a payload are read within the payload's own limit:

- A counted-string group (`REGISTER`, `LOGFILE_REPLY`, `DEVICE_REPLY` and the
  spawn request) announces its string count in hexadecimal. The count must be
  valid, no larger than the strings present, and at least the number the request
  type requires. Fields that older protocol revisions leave out read as absent.
- A multi-packet request (`ENVIRON_RESET`, `APP_B00_SPAWN`) announces its string
  count in decimal and continues in packets of the same type. The count is
  limited, and the daemon stops after as many continuation packets as there are
  strings to place.
- The encoded termios string of `CHANNEL_TERMIOS` must fit the daemon's buffer.
  A `SERVER_DEBUG` file name is read from the request without copying.
- A numeric field that does not parse is an error, not an uninitialized value.

## Registration

A client proves who it is by creating a file that only that user can create.
The daemon names the file, with a random suffix, in the user's home directory
(or in the directory given with `-auth_dir`, or the system temporary directory
when the home directory is not usable). The file must be a regular file
with a single link and the setuid bit set, its owner must be the user named in
the request, and it must not be a symbolic link. A file with more than one name
is refused because a hard link to another user's setuid file would otherwise
pass for that user.

Before the daemon accepts commands it gives up its own identity:

1. it builds the user's `HOME` and `SHELL` entries and environment file path,
   and refuses the registration if they do not fit;
2. it sets the supplementary groups (when it is `root`), the group and the user;
3. it checks that the process now has exactly that identity and, for any user
   other than `root`, that it cannot become `root` again.

If any step does not take effect the client is told the registration failed and
the daemon exits. A daemon that is not `root` can only serve its own user.

## Tests

`programs/dtspcd/tests` holds request-level regression tests. The test program
includes the production `main.c` and links the built `libDtSvc`, then drives the
request handlers with hand-built requests. Each case runs in its own process
under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make -C programs/dtspcd check
```

The cases cover the header and payload limits, dispatch, the counted-string
readers, multi-packet requests, the spawn, termios and debug handlers, unknown
channels, registration refusals, the random file-name suffix and the identity
change. Run against the code from before these changes, the same test source
fails 16 of its 18 comparable cases; the other two check ordinary behavior.

## Limits

- The tests exercise the handlers directly. They do not run a complete
  authenticated session, which needs a ToolTalk session and file system
  conditions that the check suite does not provide.
- The termios string decoder in `libDtSvc` and the remainder of `libDtSvc` are
  outside this change.
- No fuzzing of the protocol readers has been done yet.
