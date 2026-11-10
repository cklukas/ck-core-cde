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

## Protocol limits

The XDR routines of the calendar protocols (versions 2 to 5 and the callback
protocol) read the length of a string or the number of elements from the
message and allocate that much before they read the contents. Every string,
byte string, array and list in them is therefore bounded, by the constants of
`lib/csa/cmlimits.h`, on both the request and the reply side:

| Constant | Limit | What it bounds |
|---|---:|---|
| `_DtCM_LIMIT_NAME` | 512 | calendar and user names (`user@host`) |
| `_DtCM_LIMIT_ATTR_NAME` | 256 | attribute names |
| `_DtCM_LIMIT_CHARSET`, `_DtCM_LIMIT_ENTRY_ID` | 64, 256 | character set names, entry identifiers |
| `_DtCM_LIMIT_DATE_TIME` | 128 | dates, date ranges, durations, lead times |
| `_DtCM_LIMIT_TEXT` | 65,536 | string attribute values, appointment text, opaque data |
| `_DtCM_LIMIT_ARCHIVE` | 64 MiB | the text of an archived calendar |
| `_DtCM_LIMIT_ATTRIBUTES` | 1,024 | attributes or attribute names of an entry, a calendar or a request; reminders of an appointment |
| `_DtCM_LIMIT_ENTRIES`, `_DtCM_LIMIT_KEYS` | 1,048,576, 65,536 | entries, appointments, reminders in a list; keys in a request |
| `_DtCM_LIMIT_CALENDARS` | 65,536 | calendar names in a list |
| `_DtCM_LIMIT_ACCESS_ENTRIES`, `_DtCM_LIMIT_ITEMS` | 65,536 | access list and date list entries; instances, ranges and exceptions of a request |
| `_DtCM_LIMIT_TAGS`, `_DtCM_LIMIT_REGISTRATIONS` | 16 | tags of an appointment, registrations in a request |

A message that exceeds a limit is refused (the server answers that it could not
decode the arguments) and a reply that would exceed one is not sent. The
values of enumerations of the older protocols and the type of an attribute
value are checked on decoding; an unknown value refuses the message. A string
that is not there is sent as the empty string.

Linked lists are coded in a loop with the bytes of the recursive routines that
rpcgen generates, so a list of a million nodes no longer nests a million calls
deep when it is read, written or freed. The XDR routines of versions 2 to 4
are kept in `rtable2_xdr.c`, `rtable3_xdr.c` and `rtable4_xdr.c` instead of
being generated, and the list coding is in `cmxdrlist.c`.

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

`lib/csa/tests` tests the XDR routines with hand-made messages: strings and
arrays at and beyond their limits, counts of two and four billion, lists of
300,000 nodes, enumeration values and attribute types that do not exist, messages
cut off in the middle of a list, and the bytes of lists whose `next` member is
first, in the middle and last. The cases run under AddressSanitizer and
UndefinedBehaviorSanitizer with leak detection and a limit on a single
allocation:

```sh
make -C lib/csa check
```

Run against the XDR routines from before these changes, 14 of the 17 cases
fail; the other three check the bytes of lists and the release of cut-off
messages, which did not change. A live `rpc.cmsd` that is sent one request with
a list of two million nodes crashes in the previous code and refuses the request
now.

## Limits

- The request-level tests call the handlers directly; the live checks of the
  daemon are kept outside the tree.
- The parts of `rpc.cmsd` and `libcsa` outside the request path, such as garbage
  collection and the recurrence-rule code, have not been audited.
