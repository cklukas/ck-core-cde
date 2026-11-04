---
title: Calendar server
description: How rpc.cmsd validates requests, stores calendars and gives up privileges.
---

# Calendar server

`rpc.cmsd` serves calendars to Calendar Manager and other CSA clients over RPC.
It is started by `inetd` or `xinetd` as `root`, switches to the calendar owner's
identity and handles requests from the network. The request path is validated
before data is used.

## Requests

- A lookup, archive or restore request carries an attribute count and an array
  of operators. The two counts must agree; a request whose counts differ is
  refused instead of being decoded into an array of the wrong size.
- Attribute numbers sent by a client are not used as indexes. The server finds
  the attribute by its name and keeps the index within the calendar's or the
  entry's attribute array. An entry has no value for an attribute that was added
  to the calendar after the entry was stored.
- A calendar name is copied without a fixed-size buffer. An empty name or a name
  containing `/` is refused, so a name cannot select a file outside the
  calendar directory.
- Creating a calendar that already exists neither adds an access list to it nor
  removes it, and creating one that fails does not remove a file it did not
  create. Deleting several appointments in one request walks its list of keys
  within the list.
- A date criterion that is not a date, a Classification or Type attribute
  without a value, an unterminated reference identifier and an offset beyond the
  range of a calendar date are handled instead of dereferenced.

## Calendar files

A calendar is stored as a log of records. The writers build every record, entry
table and attribute list in a buffer that grows with the data, and escape every
string with the rules the log already used for some fields: a newline,
a backslash and a double quote are written with a leading backslash. A string
without those characters is stored exactly as before. The reader's token buffer
is sized by the file, and an unterminated string at the end of a file ends the
token, so what the writers store can be read back.

## Privileges

`rpc.cmsd` changes its effective user and group to the daemon's at start-up and
stops if it cannot. When it changes the owner or mode of a calendar file it
switches to the file owner and back; if the chmod or chown fails, or the switch
back fails, the daemon stops instead of continuing as `root`. The signals that
start garbage collection and debugging are held back while a request is being
processed.

## Tests

`programs/dtcm/server/tests` holds request-level regression tests. The test
program includes the production `log.c` and `cmsfunc.c`, compiles the other
sources it exercises with AddressSanitizer and UndefinedBehaviorSanitizer and
links the built `rpc.cmsd` and `libcsa`. Each case runs in its own process:

```sh
make -C programs/dtcm/server check
```

The cases cover the request decoders, calendar names and attribute handling, the
log writers and reader with strings of all sizes, creating, updating and
deleting entries and calendars, and the privilege changes. Run against the code
from before these changes, the same test source fails 27 of its 29 cases; the
other two check results that did not change.

## Limits

- The tests call the request handlers directly; they do not run a live
  `rpc.cmsd` with a client.
- The parts of `rpc.cmsd` and `libcsa` outside the request path, such as garbage
  collection and the recurrence-rule code, have not been audited.
