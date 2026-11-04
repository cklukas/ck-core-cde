---
title: ToolTalk
description: How ttsession and rpc.ttdbserverd validate requests, how sessions are authenticated and how the programs ttsession starts get their environment.
---

# ToolTalk

ToolTalk is the message bus of the desktop. `ttsession` runs once for a session.
Programs connect to it over ONC RPC, register the messages they handle and send
messages that it routes; it starts the program of an installed ptype when a
message needs a handler that is not running. Everything a client sends reaches
`ttsession` as RPC arguments, and the daemon decodes them before it knows who
sent them. `rpc.ttdbserverd` keeps the files and objects ToolTalk knows about
in NetISAM tables and answers requests for them in the same way.

## What a session protects

A session is protected by its authentication level (see below). At the default
level every request carries a random cookie that `ttsession` creates when the
session starts and keeps in the owner's authority file, readable by the owner
only. A client that has the cookie acts as the owner: it can send any message,
register any pattern and start any installed ptype. Two limits follow from the
design of the protocol, whatever the level:

- Programs of the same user are not isolated from each other. Whoever can read
  the authority file can use the session.
- The cookie travels in clear text in every request, and `ttsession` listens on
  all interfaces. Whoever can read the network traffic of a session can use it.
  Use a session across hosts only on a network you trust, or through an
  encrypted tunnel.

The user and group ids of a message are set by the client that sends it. A
handler that compares them with its own, as `dtterm` does for requests for a new
terminal, learns what the sender says. At the cookie level every sender holds the
owner's cookie anyway; the other levels are described below.

## Authentication levels

`ttsession -a LEVEL`, or the environment variable `TOOLTALK_AUTH_LEVEL` when the
option is not given, chooses the level.

| Level | A request must carry | Notes |
|---|---|---|
| `cookie` (default) | the session cookie | Unix credentials are refused. |
| `unix` | unix credentials with the user id of the session's owner | The client writes its own credentials, so this keeps out clients that behave, not a program that lies about its user id. |
| `des` | secure RPC credentials of the owner | Only in builds with secure RPC support (Solaris). `ttsession` refuses to start at this level without it. |
| `none` | nothing | Anyone who can reach the session can use it. |

A level accepts only what it asks for. At the `unix` and `des` levels a request
with no credentials, such as an `rpcinfo` call to the program of the session, is
refused (it used to be served), as it already was at the cookie level, and
credentials of a kind too weak for the level are refused as well. The cookie is
compared byte by byte without stopping at the first difference. At start,
`ttsession` writes a warning to the system log for the `unix` and `none` levels,
also when it runs with `-s`.

## Request decoding

- Strings, byte strings and lists of strings with a negative length or count, or
  a length beyond the data that arrived, are refused. This includes the session
  cookie, which is decoded before it is checked, and every string of a message.
- Session addresses are read within the size of their fields: a long host name no
  longer overruns the buffers that hold it.
- A null object where a procedure needs one, a truncated body and arguments that
  cannot be decoded are answered with an error. A failed decode does not go on
  into the procedure, and what it allocated is freed.
- Loading types into a running session leaves the live type table alone when the
  database cannot be read. Looking up the children of an object type finds the
  types it should, and a cycle of parent types ends instead of looping.
- The session cookie is drawn from `/dev/urandom` and nothing else. If the random
  source cannot be read, no session starts.

## Environment of started programs

A context of a message whose name begins with `$` becomes an environment
variable of the program `ttsession` starts for the message: `$MYAPP_PROFILE`
sets `MYAPP_PROFILE`. The sender of a message neither installed the ptype nor
owns the session, so `ttsession` limits what a message can set:

- The variables `ttsession` sets itself to tie the program to its session
  (`TT_SESSION`, `TT_TOKEN`, `TT_FILE`, `_TT_SID`, their `_SUN_TT_` spellings,
  and `DISPLAY` when the session has a display) win over the message, because
  the variables of the message are applied first.
- Names that change which code the program runs or loads, whom it believes it
  is, or where it finds its credentials, are not passed on:

  | Kind | Names |
  |---|---|
  | search paths and shell | `PATH`, `IFS`, `ENV`, `BASH_ENV`, `SHELL`, `SHELLOPTS`, `BASHOPTS`, `PS4`, `CDPATH`, `PROMPT_COMMAND`, `BASH_FUNC_*` |
  | loader and C library | `LD_*`, `DYLD_*`, `MALLOC_*`, `GCONV_PATH`, `LOCPATH`, `NLSPATH`, `GLIBC_TUNABLES`, `HOSTALIASES`, `RES_OPTIONS`, `RESOLV_HOST_CONF`, `LOCALDOMAIN` |
  | identity and credentials | `HOME`, `USER`, `LOGNAME`, `XAUTHORITY`, `ICEAUTHORITY` |
  | toolkit and desktop resources | `XENVIRONMENT`, `XFILESEARCHPATH`, `XUSERFILESEARCHPATH`, `XAPPLRESDIR`, `XBMLANGPATH`, `XLOCALEDIR`, `DTDATABASESEARCHPATH` |
  | runtimes | `PYTHON*`, `PERL*`, `RUBY*`, `CLASSPATH`, `JAVA_TOOL_OPTIONS`, `_JAVA_OPTIONS`, `JDK_JAVA_OPTIONS`, `NODE_OPTIONS`, `NODE_PATH`, `TCLLIBPATH` |
  | ToolTalk | `TT_*`, `_TT_*`, `_SUN_TT_*`, `TOOLTALK*`, `TTAUTHORITY`, `TTPATH`, `TTSESSION*` |

- A variable that cannot be set stops the start of the program.

The list cannot be complete, which is why the owner can restrict the names to a
list. Each dropped name is written to the system log (`not setting $NAME for the
start of a ptype`), also with `-s`, once for every start that asked for it.

The owner of the session sets `TOOLTALK_START_ENV` in the environment of the
process that starts `ttsession`, the same way as `TOOLTALK_AUTH_LEVEL`. It is a
comma separated list:

| Entry | Meaning |
|---|---|
| `NAME` | pass this variable, also if it is one of the names above |
| `PREFIX*` | pass the variables whose names begin with `PREFIX` |
| `strict` | pass only `DISPLAY`, `LANG`, `LANGUAGE`, `LC_*`, `TERM` and `TZ`, and what the other entries name |

`TOOLTALK_START_ENV=LD_LIBRARY_PATH,MYAPP_*` passes those two on top of the
default policy; `TOOLTALK_START_ENV=strict,MYAPP_*` passes only the built-in
names and `MYAPP_*`. An entry that is none of these makes the policy strict and
is reported in the system log when `ttsession` starts.

A program that used to receive one of the dropped variables from a message stops
getting it. Approve the name in `TOOLTALK_START_ENV`, or set the variable in the
start string of the ptype, which the person who installs the ptype controls.

## Database server

`rpc.ttdbserverd` keeps the files and objects ToolTalk knows about, with their
properties, queued messages and joined sessions, in NetISAM tables, and answers
ONC RPC requests for them. It is usually started by `inetd` as root. It decides
what a client may do by the user id in the unix credentials of the request, or
the secure RPC credentials in builds that have them, and refuses a request with
no credentials. Unix credentials are written by the client, so those user ids
are as trustworthy as the clients and the network between them.

The numbers, lengths and pointers of a request are checked before they are used:

- A file descriptor outside the table of open files is refused. NetISAM hands
  out more descriptors than the table has entries: one beyond the table is
  closed and the open fails, and a failed `isopen` or `isbuild` no longer leaves
  a descriptor behind that points at freed memory.
- Records a client sends are copied into zero-padded buffers, because NetISAM
  takes the record length from the buffer. Key descriptors, object keys, session
  ids and paths that are missing, too short or too long are refused.
- The transaction log is opened without following links, the length of each
  record is checked against the buffer, and a transaction with a stale
  descriptor no longer closes a descriptor of the server or removes the log of
  an earlier request. A list of records is decoded in a loop, so its length
  cannot exhaust the stack.
- Properties a client can set, such as cache levels and the information about a
  queued message, are validated before the server uses them to size or index
  anything.
- The cache level of a file or an object, which counts its changes, is a property
  that a client can set to any `int`. A change that finds the highest `int` fails
  instead of making the number wrap around, and a client that says it has the
  highest level is told that nothing is newer.
- A position inside a table is no longer taken from the wire.
- The record lengths of a table are kept when the table is opened, so record
  buffers have the size of the records, and overlapping entries of a b-tree are
  moved with `memmove`.

A NetISAM table stores its numbers (page and record numbers, the serial of a
duplicate key, the fields of the access table) in the width of a C `long`:
4 bytes on a 32-bit build and 8 bytes on a 64-bit build. The width comes from
`LONG_BIT`, and where `<limits.h>` does not define it the headers take it from
`ULONG_MAX`, so the C library and the C++ server and client library always agree.
A table cannot be moved between a 32-bit and a 64-bit `rpc.ttdbserverd`. On
64-bit Linux the C part stored 4 bytes where the C++ part expected 8 before this
was fixed, so the server could not complete the tables of a partition and there
is no working database of the old layout to convert.

### The length of a key

Every record of the tables starts with the key of the file or object it is
about: the structure `_Tt_key_data` (the version, padding, and the host id, time
and counter as longs), which is kept in the tables as it is in memory. Its size,
`OID_KEY_LENGTH` in `lib/tt_const.h`, is 16 bytes on a 32-bit build and 32 bytes
on a 64-bit one. The code of the old interface had the number 16 for it, so on a
64-bit build it looked for the name of a property, and the owner of an object,
in the middle of the key: the access check, the session procedures, `gettype`,
the modification date, the message queue locks and the session functions of the
new interface (`TT_GET_ALL_SESSIONS`, `TT_DELETE_SESSION`) could not find their
records in the tables the server makes. All of them, the key descriptors and the
bounds of the keys in the protocol now use the length of the structure, the
access record is the one layout of the access table (a key and a long for each
number; the 16 bit layout of the first servers is not read any more, as their
databases are not either), and a key is zeroed before it is filled, so that its
padding is the same in every copy of it. `ttdbck` takes its smallest and
largest key from the same length.

### Who may reach what

- Every request that works for a user runs with the user and groups of its own
  credentials. Only the questions about the server itself (the authentication
  level it requires, and the mount point and partition of a path) are served
  without. `TT_IS_FILE_IN_DB` and `TT_IS_OBJ_IN_DB` skipped the credentials: they
  ran with those of the request before, and answered a request that had none.
- The partition in an object id names the directory where the database of the
  object is, and the server makes a `TT_DB` directory with the tables in it if
  there is none yet. A partition is accepted if it is a mount point of the host,
  which is what `TT_GET_FILE_PARTITION` answers for a file in it, or a name of
  the partition map, spelled as the mount table does (no `..`, no link, no
  trailing `/`). Any other is refused with `TT_DB_ERR_ILLEGAL_OBJECT`, and no
  directory is made. Before, any client could have a `TT_DB` directory made in
  any directory that the daemon, which runs as root, can write to.
- A client of the old servers names a table `<partition>/TT_DB/<table>`, with
  `oid_prop`, `oid_access`, `oid_container` or `docoid_path` for the table, and
  `isbuild`, `isopen` and `iserase` serve that path and no other: `TT_DB` has
  to be the directory above the table (a name that has `TT_DB` in it, a `..` or
  `.` component, a relative path or an empty component is refused), the
  directory above `TT_DB` has to be a partition as above, and neither `TT_DB` nor
  a file of the table may be a link. `iserase` takes the name of a table of an
  old server, never the name of a table of the layout that replaced them. It
  and `isbuild` had no check at all, and `isopen` accepted any path with the
  letters `TT_DB` somewhere in it. A table that does not exist yet is made in
  the partition of the path.
- The tables of the old interface hold the access of an object (owner, group and
  mode) in the access table, and the check that a user may read or write the
  properties of an object never applied: it compared the name of the property
  table with the name the old servers gave it, while the daemon keeps the path
  of the table under its new name, so it allowed everything. The rule is now the
  one `rpc.ttdbserverd(1M)` documents for the option that turns it off: the
  protection of the file the object is in decides. The owner of the file or of
  the object may do anything; a member of the group of the file may do what the
  group bits of the mode of the file, or of the mode of the object, give to the
  group; everybody else what the other bits give. A request that reads and
  writes needs both permissions. The properties of the file itself, which has no
  file to look at, are everybody's. An object with no access record, or a
  request with no object key, is nobody's. The superuser, and any user when the
  daemon runs with `-n`, may do anything. All the records of a transaction are
  checked, not the first alone, and the session procedures and `gettype` check
  the object they work on. The access record is read in the layout of the access
  table the server makes: the key, and a long for each number (see the length of
  a key below). It is read each time and no longer cached by the oid alone.
- The procedures that read and write the access of an oid (`getoidaccess`,
  `setoiduser`, `setoidgroup`, `setoidmode`) refuse a descriptor that another
  user opened, as every other procedure on a descriptor does.
- `addsession` and `delsession` compared the session ids the wrong way round:
  adding stopped at a session that was different from the new one, and deleting
  removed the first session that was different. They also looked for the record
  with the bytes of a key descriptor, which finds nothing. They now look for the
  records of the property `_TT_SESSIONS` of the object, add a session that is
  not there, remove the one that is named (a session that is not there is
  `ENOREC`), and count up the modification date of the object.

### Limits of the protocol

The routines that decode a request allocate the storage for a string, a byte
array or a list as long as the length in the request says, before they read what
the length counts. A request of 16 bytes could therefore demand 4 GiB. Every
string, byte array and list of the protocol now has a limit, named in
`lib/db/db_server.h`, and the XDR routines refuse a longer one. They are shared
by the server and libtt, so the limits hold for a reply as well as for a request.

| What | Limit | Where it comes from |
| --- | --- | --- |
| Path, partition | `MAXPATHLEN` (4096 on Linux) | the longest path of the host |
| File name `host:path` | `HOST_LENGTH` + 1 + `MAXPATHLEN` (5121) | the form in which libtt names a file |
| Object id | 128 + `HOST_LENGTH` + `MAXPATHLEN` (5248) | key, file system type, host, partition and three colons |
| Type name (ptype, otype) | 1024 | identifiers |
| Property name | `MAX_PROP_LENGTH` (64) | the field of a record that holds a name; the server stores no more of one |
| Session id | 32767 less the key and name (32671 on a 64-bit build) | what a record of at most `ISMAXRECLEN` (32767) bytes holds after its key and name |
| Value of a property | 1 MiB | a record holds at most `ISMAXRECLEN` bytes |
| Body of a message | 16 MiB | libtt marshals the whole message into the body, and the server stores it in properties of `ISMAXRECLEN` bytes each: 513 of them |
| Properties of a file or object | 65536 | one record each |
| Values of a property | 65536 | one per queued message or joined session |
| Ptypes of a message | 1024 | |
| Objects of a file | 1048576 | one record each |
| Messages in one reply | 65536 | |
| Properties of a spec, old interface | 16 | the server answers with one |

The limits are chosen far above what libtt sends or the server answers with. A
request that exceeds one fails to decode, like any other malformed request, and
the client gets a decode error. A name of a property longer than 64 characters,
which the server used to cut off without saying so, is refused now. The strings
and records of the old interface that were bounded before (table paths of
`PATH_LENGTH` characters, records of `ISMAXRECLEN` bytes, object keys) keep
their bounds. The test has the allocator refuse any request for more than
64 MiB, replaces every length, count and small integer of a request of each
procedure with huge values, and decodes each limit and the size above it.

A string that a reply has none of used to fail to encode: a missing property has
no name, an object that does not exist has no type and one without a forward
pointer has none, so the server answered with a system error instead of the
result, and a client could not send a request to remove an object that has no
forward pointer. A string that is not there is now sent as the empty string,
which the receiver takes for no string.

### Mount points and paths

A path that no mount point leads to (the mount table is empty or cannot be read,
or the path is relative and the working directory is gone) is in no partition:
`_Tt_file_system::bestMatchToPath()` returns no entry, and its callers in libtt
and in the server treat the path as having no partition and no network path
instead of dereferencing the entry. `_Tt_dirname()` and `_Tt_basename()` stop at
the start of a path with no `/`, where they used to read backwards from the
beginning of the string (`_Tt_dirname()` now returns -1 for such a path, like it
does for one in the root directory, and `_Tt_basename()` returns 0). The unused
`set_real_path()` of the server, which copied paths into a buffer without
checking their length, is gone.

## Checks

`make -C lib/tt/slib check` compiles the production decoding and request code
with the sanitizers next to a test program and runs each case in its own
process. The program builds its requests by hand, with lengths and null markers
that libtt never produces, and serves them through the dispatcher `ttsession`
uses. The cases cover each authentication level, the cookie comparison, the
environment policy and the start environment of a message.

`make -C lib/tt/bin/ttdbserverd check` does the same for the database server: it
runs the real request dispatcher on requests it makes up, and a mutation of
those requests, under the sanitizers. Every case runs on every build, including
the two that need the database of a partition: a build in which NetISAM and the
server disagree on the width of a long cannot make that database, and those
cases fail.

The request cases also decode every limit of the protocol and the size above it,
send a request of each procedure of the new interface with each of its lengths,
counts and small integers replaced by huge values while the allocator refuses
any request for more than 64 MiB, and send every reply that has a string
the server does not have.

They also serve the old interface to users with other credentials: for the
owner of a file, a member of its group and everybody else, over the modes of the
file and of the object, whether each of reading, writing, rewriting and
test-and-set is allowed; the paths of `isbuild`, `isopen` and `iserase`, with
links and with partitions that are none; the partitions of object ids; the
credentials of the requests that skip none; the descriptors of the oid access
procedures; and adding and removing sessions.

Two more cases call the mount table and path functions of libtt that the server
uses: a path that no mount point leads to, and the directory and base name of
paths with and without a `/`.

`make -C lib/tt/mini_isam check` builds the same test of the width of a long as
C and as C++ and runs both against `libisam`, so the C library and its C++
clients are held to one layout.
