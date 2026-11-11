---
title: ToolTalk
description: How ttsession and rpc.ttdbserverd validate requests, how sessions are authenticated, how the programs ttsession starts get their environment, which Desktop requests a joined program answers and what the client library decodes into.
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

## Notices with a disposition

A notice has no handler. Its disposition (`TT_QUEUE`, `TT_START`) asks for a
handler to be started or for the message to be kept for one, which needs the
ptype of a handler, so for a notice `ttsession` has nothing to do after the
observers have been offered it. It used to try, find that it could not, and
fail the message: the observers of a notice sent with `TT_QUEUE` got it as
`TT_SENT` and then a second time as `TT_FAILED`, and anyone who counted the
notices a process received counted every one twice. The notice is delivered to
its observers once.

## Ending a session

`ttsession` registers its session with the portmapper. When it was told to end
(`SIGTERM`, `SIGINT`) or the root process of a process-tree session exited, the
signal handler called `exit()` and the registration stayed: the portmapper listed
a program that nothing served, and a client that looked for the session got a
connection refused. The handler now only notes the signal and the main loop ends
the session: it stops the root process of a process-tree session, deletes the
session, which takes it out of the portmapper, and exits (with status 1 when it
was told to end, with the status of the root process otherwise).

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

## Environment of libtt in a privileged process

A program that runs with other privileges than the user who started it (a
set-user-ID or set-group-ID program, or one that has raised its effective ids)
acts for its owner, but its environment is the user's. libtt treats the
following settings as not set when the effective user or group id of the process
differs from the real one:

| Setting | What it would name |
|---|---|
| `TTSESSION_CMD`, `SUN_TTSESSION_CMD` | the command that starts `ttsession` |
| `TT_TRACE_SCRIPT` | a trace script, which can name a file to write |
| `LD_LIBRARY_PATH` | the directories libtt searches for the X libraries it loads |
| `TTAUTHORITY` | the authority file |
| `TTPATH`, `OPENWINHOME`, `DTMOUNTPOINT`, `DTAUTOMOUNTPOINT`, the variables that name a map file | the places libtt reads its type, host and partition files from |
| `HOME` | the home directory, which is then the one the password database gives the real user |

A client that finds no `ttsession` starts one. A process with other privileges
starts it with the real user and group ids of the process, given up for good
before anything is run, so the session never runs with the privileges of the
program that started it.

The authority file and the files next to it are made in the directory of the user
who started the process. The lock of the authority file (`file-c`, `file-l`) is
made with a create that fails if the name exists and does not follow a link, and
a lock file that is already there is a lock that is held; it used to be opened
and truncated, so a link in its place sent the truncation to the file it pointed
to. The file that receives the new entries (`file-n`) is made the same way after
the old one has been removed, so a link planted in between does not redirect
the write.

`TTSESSION_CMD` is a command, not a line for a shell: a program and its
arguments separated by white space, to which `-d` and the display name are
added. Quotes, redirections, variables and `;` in it are passed to the program as
they stand. A command that needs a shell goes into a script. The display name is
one argument of the command whatever it contains.

The tools `ttcp`, `ttmv`, `ttrm`, `tttar` and `tt_type_comp` run `cp`, `mv`, `rm`,
`tar` and the C preprocessor directly, with each name and option as one
argument. File names with blanks or characters a shell treats specially work and
are never interpreted; a name that contains `*` is that name and not a pattern
(the shell that starts the tool still expands what the user types). The options
that `tt_type_comp` passes to the preprocessor are no longer limited by the
length of a fixed command line.

## Desktop requests that act on a process

`ttdt_session_join()` joins a session as a "good desktop citizen": the library
registers patterns for the standard Desktop requests and answers them for the
program. The requests about windows (geometry, iconified and mapped state,
raise and lower, X information, and with a shell widget and no callback also
`Quit`, `Pause` and `Resume`) act on the `shell` widget that the caller passes;
a program that passes none gets them passed to its callback, or failed if it has
none. Until now the same call also
registered, for every program that joined, a handler for the requests that report
or change the state of the whole process:

| Request | What the library did to the program |
|---|---|
| `Set_Environment` | `putenv()` of any name and value: `HOME`, `PATH`, `TMPDIR` and `LD_*` reached every program it started afterwards and every `getenv()` caller in its address space |
| `Set_Situation` | `chdir()` to any directory |
| `Signal` | sent the program any signal number |
| `Set_Locale` | `setlocale()` for any category |
| `Get_Environment`, `Get_Situation`, `Get_Locale`, `Get_Sysinfo` | answered with the value of any environment variable, the working directory, the locale and the `uname()` fields |
| `Do_Command` | passed to the callback of the program, whether or not the program had a use for it |

Any process of the session could send these, and the session's authentication
level does not tell programs of one user apart (see above), so a program that had
only joined the session to receive its own messages could have its environment,
its directory and its signals set by any other program of the session.

The library now registers for none of them unless the program asks. The last
argument of `ttdt_session_join()` is a `Ttdt_session_requests`, an `unsigned int`
that is zero or the bitwise OR of the requests the program answers, one value for
each request:

| Value | Request | What the library does when asked for it |
|---|---|---|
| `TTDT_SESSION_GET_LOCALE` | `Get_Locale` | replies with the locale of the program for each category asked |
| `TTDT_SESSION_SET_LOCALE` | `Set_Locale` | calls `setlocale()` for each category and locale in the request |
| `TTDT_SESSION_GET_ENVIRONMENT` | `Get_Environment` | replies with the value of each variable named |
| `TTDT_SESSION_SET_ENVIRONMENT` | `Set_Environment` | sets or removes each variable named |
| `TTDT_SESSION_GET_SITUATION` | `Get_Situation` | replies with the working directory |
| `TTDT_SESSION_SET_SITUATION` | `Set_Situation` | changes the working directory to the path in the request, or to the file of the message |
| `TTDT_SESSION_SIGNAL` | `Signal` | replies, then sends the program the signal |
| `TTDT_SESSION_GET_SYSINFO` | `Get_Sysinfo` | replies with the `uname()` fields |
| `TTDT_SESSION_DO_COMMAND` | `Do_Command` | passes the request to the callback of the program, which must then be given |

The values stand for one request each, so a program that only answers questions
does not take on the requests that change it: `ttdt_sender_imprint_on()`, for
example, asks the sender of a message for `Get_Locale` and `Get_Situation`, and a
program that wants to be imprinted on asks for those two values only.

What is not asked for is not registered. A `Set_Environment` that no program of
the session registered for is failed by `ttsession` with `TT_ERR_NO_MATCH`, and
one that is addressed to a program that did not ask is failed by the library with
`TT_DESKTOP_ENOTSUP` (`tttk_Xt_input_handler()` fails what no callback took, as it
did before). Nothing in the program changes. `ttdt_session_join()` itself returns
`TT_ERR_NUM` for a value that is none of the above and `TT_ERR_POINTER` when
`TTDT_SESSION_DO_COMMAND` is asked for without a callback. `ttdt_message_accept()`
never registered for these requests and takes no argument for them.

There is no value that stands for all of them. A program that asks for
`TTDT_SESSION_SET_ENVIRONMENT`, `TTDT_SESSION_SET_SITUATION` or
`TTDT_SESSION_SIGNAL` lets every program of the session change its environment,
its working directory and its signals; the environment it sets is not limited
the way the start environment of a ptype is. In `dtksh`, `ttdt_session_join`
takes the requests as further arguments (`TTDT_GET_SYSINFO` and so on, no argument
for none), and the code that Application Builder generates asks for
`TTDT_SESSION_DO_COMMAND` only when the project connects an action to a
`Do_Command` message.

### What the clients of the desktop ask for

Every program of the desktop that calls `ttdt_session_join()` was looked at for
the requests it handles in its own code and for the programs that send these
requests. Apart from `ttsnoop`, which sends what its user composes, `dtpad` is
the only program of the desktop that sends one of them (`Get_Sysinfo`), and
`ttdt_sender_imprint_on()` is used by the `CoEd` demo only. Only these ask for
something:

| Program | Asks for | Why |
|---|---|---|
| `dtpad` (server mode) | `TTDT_SESSION_GET_SYSINFO` | a second server that finds the display already served asks the one that serves it for its host name with `Get_Sysinfo`, to say on which host the editor is |
| `ttsnoop` | `TTDT_SESSION_DO_COMMAND` | the callback of the session it joins shows every request to the person who runs the tool and lets that person fill in the answer; it never runs anything |
| programs made with Application Builder | `TTDT_SESSION_DO_COMMAND`, when the project connects an action to it | the generated callback runs that action; nothing is asked for otherwise |

All the others ask for nothing, because none of them sends or handles any of
these requests: `dtfile`, `dtterm` (including the terminal server), `dtcm`
(including the appointment editor), `dtmail`, `dtinfo`, `dtcreate`, `dticon`, the
template in `examples` and the `CoEd` demo. They keep the window requests that
their shell widget and callbacks decide, `Get_Status`, and the media requests
that their ptypes declare. A program that does not ask answers none of these
requests, and a request for one is failed as described above.

## Objects the client decodes

A client holds the messages it receives and the files it joins as the classes of
the client side of the library (`_Tt_c_message`, `_Tt_c_file`), which add the
operations only a client does. The library used to decode what it received into
the classes it shares with the server (the list of messages `ttsession`
delivers, the messages dequeued from the database of a file, the file object that
`find_file` made) and to call the operations of the client classes on those
objects. That is undefined behaviour, however much it looked like it worked
because the client classes have no members of their own, and UBSan reported it
for every message a program received. The client now decodes into its own
classes: `tt_c_xdr_next_message_args` is the counterpart of the xdr functions of
the server (`mp_s_xdr_functions`) for the delivery of a message list, the
messages dequeued from a file are a `_Tt_c_message_list`, and `find_file` makes
`_Tt_c_file` objects. The wire format is the one the server encodes.

A check that holds the library to this has to reach it: the session requests
test (see below) fails on any sanitizer report from the processes that use the
library as a client, whatever file it names. File-scoped messages need the
database of a mount point the caller owns, which a check run by a user cannot
make, so they are not part of `make check`; they were run against the sanitizer
build of `rpc.ttdbserverd` on a scratch file system, live and queued, once with
the library from before and once with this one.

## Database server

`rpc.ttdbserverd` keeps the files and objects ToolTalk knows about, with their
properties, queued messages and joined sessions, in NetISAM tables, and answers
ONC RPC requests for them. It is usually started by `inetd` as root. It decides
what a client may do by the user id in the unix credentials of the request, or
the secure RPC credentials in builds that have them, and refuses a request with
no credentials. Unix credentials are written by the client, so those user ids
are as trustworthy as the clients and the network between them.

The server has one interface, the requests of a partition based database
(`TT_GET_FILE_PARTITION`, `TT_CREATE_FILE`, `TT_SET_OBJ_PROP` and so on, numbered
from 101 in `lib/db/db_server.h`): a client names files, objects and
properties, and the server decides which tables they are in and what the
caller may do with them.

### The procedures of the old servers are gone

The servers before this one had a second set of procedures, numbered 1 to 24:
the NetISAM calls themselves (`isopen`, `isbuild`, `isread`, `iswrite`,
`isrewrec`, `isdelrec`, `isstart`, `iserase` and the others) on a table named by
its path, and calls for the access and the sessions of an oid. A client could
open any table of the server, including the one that holds who may do what, and
read and rewrite its records; the access check of those procedures depended on
the key the client sent and not on the record that was read or changed, and no
check was complete. They cannot be made safe by a patch, and nothing needs them:
libtt used them only to talk to a server that did not have the procedures above.
They are removed from
the server (the procedures, their XDR routines, the cache of oid access
information and the conversion of old tables to the current layout when a
partition is opened) and from libtt (the `_Tt_old_db` classes and the NetISAM
client over RPC, and the fallback to them that `_Tt_db_client` took on a
server that did not answer the call for the authentication level). A request for
one of the numbers 1 to 24 is answered as a request for a procedure that does
not exist, and its arguments are not decoded. The databases of the old servers
are not read; the options a server of that kind took are the same.

What was kept from the file that held them is the table of the NetISAM files the
server has open (`tt_isam_cache.h`): `cached_isopen`, `cached_isclose` and the
compression of the tables, which the current procedures use as before.

The numbers, lengths and pointers of a request are checked before they are used:

- Properties a client can set, such as cache levels and the information about a
  queued message, are validated before the server uses them to size or index
  anything.
- The cache level of a file or an object, which counts its changes, is a property
  that a client can set to any `int`. A change that finds the highest `int` fails
  instead of making the number wrap around, and a client that says it has the
  highest level is told that nothing is newer.
- A change of the properties of an object (`TT_SET_OBJ_PROPS`, `TT_SET_OBJ_PROP`,
  `TT_ADD_OBJ_PROP`, `TT_DELETE_OBJ_PROP`) comes with the cache level the caller
  read the properties at. If somebody changed them since, the server answers
  `TT_DB_ERR_UPDATE_CONFLICT` (`TT_ERR_DBUPDATE` in the API) with the properties
  as they are now and their cache level, and the change is not made, so that the
  caller can update its copy and try again. The three procedures that set and add
  properties made the change anyway and reported that it had worked, so the
  changes of two clients to one object were lost without notice; the delete
  reported success without deleting.
- A position inside a table is not taken from the wire.
- The answers of the procedures are kept from one request to the next, and the
  dispatcher frees what an answer holds once it is sent. It left the pointers
  where they were, and `TT_GET_OBJ_TYPE` and `TT_GET_OBJ_FILE` only reset theirs
  on the way that reaches a table: after any successful request, a request with
  an object id of no partition answered with the freed string and freed it a
  second time, which ends a daemon that runs as root. The dispatcher clears
  every pointer it has freed, and those two procedures start from none. It did not
  free the properties that the four procedures that change the properties of an
  object answer with (the delete answers with them every time, the others when
  the caller is out of date), so the daemon lost the memory of every such reply.
- An object id that has no key (no `|` in it, or characters above 127) decoded
  with a null pointer in `_tt_base64_decode`, and a character above 127 was a
  negative index of its table; both are digits that are not there now, and the
  key is all zero. A file name with no `/` or no characters made
  `_tt_is_file_a_directory` compute a pointer from a null one.
- The user, group and mode that come with a request are moved as 32 bit words
  of their own. The routine used to move each as a `long` through a pointer to
  the field, which on a 64-bit build is 8 bytes of a 4 byte field, and the mode,
  the last of the three, ran past the end of the access. The same routine is in
  libtt, which sends them.
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
on a 64-bit one. The number 16 was written where the length of a key was meant,
so on a 64-bit build the code looked for the name of a property, and for the
owner of an object, in the middle of the key: the session functions
(`TT_GET_ALL_SESSIONS`, `TT_DELETE_SESSION`) could not find their records in the
tables the server makes. The session functions, the key descriptors and the
bounds of the keys in the protocol use the length of the structure, the access
record is the one layout of the access table (a key and a long for each number),
and a key is zeroed before it is filled, so that its padding is the same in
every copy of it. `ttdbck` takes its smallest and largest key from the same
length.

### Who may reach what

- Every request that works for a user runs with the user and groups of its own
  credentials. Only the question about the server itself (the authentication
  level it requires) and the mount point and partition of a path are served
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
- The requests are checked against the access record of the file or the object
  (owner, group and mode) by one rule, the one of a file in a Unix file system
  and of `rpc.ttdbserverd(1M)`, which says that the protection of the file
  decides: the owner and the superuser may do anything; anybody else is in the
  class of the group if the group of the record is the group the request acts in
  or one of the groups its credentials list, and in the class of the others if
  not, and only the bits of that class count, a read needing the read bit and a
  write the write bit. A user who is a member of the group of a file by a
  supplementary group has the group's access; only the group the request acts in
  used to count. A user or group
  of -1 in a record stands for everybody, and a mode of -1 for no protection. The
  check used to let a write through for a user who only had the read bit of his
  class, and gave a member of the group the bits of the others when the group
  bits refused him. A property comes with a cache level that is read first, so
  writing a property needs the read bit as well. Moving an object to another file
  is a write of the object and of the file it is moved to; it was a read of the
  object and nothing else. The option `-n`, which turns the checking off, lets
  anybody read and write and still leaves what is the owner's alone (changing the
  access, removing) to the owner; it had no effect on these requests, only on
  the procedures that are gone.

- The tables of a partition are in its `TT_DB` directory, which the server makes
  if there is none, and are opened by their paths. The server works in the
  directory only if it is the server's own: a directory (never a link to one),
  owned by the user the server runs as, that neither the group nor the others
  can write to, in a partition that the group and the others cannot write to or
  that has the sticky bit (a mount point such as `/tmp`), and none of the files
  of the tables is a link. The directory is looked at through a descriptor
  opened without following a link. Any other `TT_DB` makes the requests on that
  partition fail with `TT_DB_ERR_DB_OPEN_FAILED`, and nothing is created through
  it. Before, a user could plant a `TT_DB` that was a link, or a directory of his
  own, in a partition he could write to, and have the daemon make its tables in
  places it chose or read tables it did not make.

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
| Netfile | 128 + `HOST_LENGTH` + 2 * `MAXPATHLEN` (9344) | the prefix and the host and two paths that `tt_file_netfile` puts in one name |
| Object id | 128 + `HOST_LENGTH` + `MAXPATHLEN` (5248) | key, file system type, host, partition and three colons |
| Type name (ptype, otype) | 1024 | identifiers |
| Property name | `MAX_PROP_LENGTH` (64) | the field of a record that holds a name; the server stores no more of one |
| Value of a property, session id, part of a queued message | 32767 less the key and the name (32671 on a 64-bit build, 32687 on a 32-bit one) | what a record of at most `ISMAXRECLEN` (32767) bytes holds after its key and the name of the property |
| Body of a message | 16 MiB and 64 KiB | libtt marshals the whole message into the body, which holds the arguments (up to 16 MiB) and what else the message has, and the server stores it in properties of the size above: 516 of them |
| Messages queued on a file | 65536 | one value of one property each, and a reply that lists them |
| Properties of a file or object | 65536 | one record each |
| Values of a property | 65536 | one per queued message or joined session |
| Ptypes of a message | 1024 | |
| Objects of a file | 65536 | one record each, and a reply that lists them |
| Messages in one reply | 65536 | |

The limits are chosen far above what libtt sends or the server answers with. A
request that exceeds one fails to decode, like any other malformed request, and
the client gets a decode error. A name of a property longer than 64 characters,
which the server used to cut off without saying so, is refused now. A value of a
property is one record of the table, so it holds no more than the record has room
for after its key and the name; the limit used to be 1 MiB, which no record can
hold, and a value above the room of a record failed inside the database with an
internal error. libtt now refuses a value above the limit before it sends
anything (`TT_ERR_PROPLEN` for a spec), and a request for more is refused by the
server in the same way. The test has
the allocator refuse any request for more than 64 MiB, replaces every length,
count and small integer of a request of each procedure with huge values, and
decodes each limit and the size above it.

The lists above are limits of what a reply can carry, and the tables could grow
past them at the request of clients (every client that joins a session adds a
value, and any client may add properties and objects), which left a file whose
properties or objects no reply could list any more. The server now refuses what
would be one too many with `TT_DB_ERR_DISK_FULL` (`TT_ERR_DBFULL` in the API): a
value of a property that has the limit of values, together with all the values of
the request that adds them; the first value of a property of a file or object that
has the limit of properties; and an object made in, or moved to, a file that has
the limit of objects. The records of the key are counted when a table has as many
records as the limit, so a table that is not that big costs nothing, and a count is
a pass over the records of one key. A request that adds only the values a property
lacks (`unique`) is counted by all its values. The limit of objects of a file is
65536, no longer 1048576, which no reply of a usable size could list. Listing the
properties of a key looked each record's property up in the list of the properties
so far, which took a pass over the list for every record: with the properties a
file may have now it takes the time of a few thousand requests. The records of a
key come in the order of the names, so a name that is not the one before it is a
new property.

A string that a reply has none of used to fail to encode: a missing property has
no name, an object that does not exist has no type and one without a forward
pointer has none, so the server answered with a system error instead of the
result, and a client could not send a request to remove an object that has no
forward pointer. A string that is not there is now sent as the empty string,
which the receiver takes for no string.

### Errors of the connection

When a request to the server fails (the connection went away, the call timed
out, what was sent did not fit the protocol), libtt takes the connection for
lost and makes a new one for the next request. `_Tt_db_object` and `_Tt_db_file`
put the result of making that connection in the place of the failure, so a
caller whose request had not been done was told that it had: `tt_spec_write` of
a property value that the client could not send returned `TT_OK` and stored
nothing. The caller is now told the failure. A request that could not be encoded
or whose reply could not be decoded is `TT_DB_ERR_RPC_FAILED`, which is told
apart from a connection that failed.

### Queued messages

A message that a sender addresses to a ptype and a file is kept on the file until
a process of the ptype takes it: its body is stored in as many parts as it takes
to fill records of the property table (`TT_DB_LIMIT_PROPERTY_VALUE` bytes each),
and a value of the property `_TT_QUEUED_MSGS` says which message it is, how
long, in how many parts, and for which ptypes. The parts were as long as a whole
record (`ISMAXRECLEN`), which leaves no room for the key and the name, so the
database refused any part that was full: a message longer than 32671 bytes did
not queue, and the sender was not told. The parts are now what a record holds,
messages up to 16 MiB are queued and come back whole, and a message that cannot
be queued (one that is too long, or that would be the 65537th queued on the
file) is reported to the sender, who gets `TT_ERR_OVERFLOW` (or
`TT_ERR_DBFULL`). Taking a message also did two things wrong: it compared only
the first ptype of a message with those that ask for it, so a message queued for
several ptypes was found only by the first, and it never wrote back the ptypes
that were left, so a message was delivered again to the ptype that had it
already. The parts of a message are removed once the last ptype has it.

### Fixes found by running the tools against the server

- The client stub for `TT_MOVE_FILE` asked the server for `TT_REMOVE_FILE`, so
  `tt_file_move` and `ttmv` removed the file and its objects from the database
  instead of moving them. The stub asks for the move. A test calls every stub of
  `db_server_clnt.C` with a client that only records the number of the procedure
  it was asked for.
- The server now moves a file inside a partition (see above); until the stub was
  fixed no client could reach that code.
- `ttdbck` read the records of the access table, which hold a `long` for each
  number (8 bytes on a 64-bit build), into a structure with 4 bytes for each, and
  wrote 12 bytes beyond it: it ended in a segmentation fault when it exited.
  The structure is as long as the record.
- `_Tt_api_spec_map_ref::getSpec` declared a second variable of the same name
  for the spec it looked up in the database and returned the first, which was
  null, so the first lookup of an object that this process had not created
  failed with `TT_ERR_OBJID`: `tttar` could not archive objects and `ttcp -p`
  could not copy them.
- The counter of a key started at the process id times 10000 in an `int`, which
  overflows for a process id above 214748; it is computed unsigned.
- `tt_spec_write` returned `TT_ERR_INTERNAL` for every failure but a few; it
  passes on `TT_ERR_PROPLEN`, `TT_ERR_ACCESS`, `TT_ERR_DBFULL`,
  `TT_ERR_DBCONSIST` and `TT_ERR_DBUPDATE` as well.

Reading the properties of a key that has none (`getProperties`) went on to read
the next record of the table after the failed positioning, which is an error
when the table has no position, so a file or object with no properties made
`TT_MOVE_FILE` to another partition, the one request that asks for them whole,
fail with `TT_DB_ERR_CORRUPT_DB`. A key with no properties has no records to
read. The move then dereferenced the null list that the server gives for none;
it moves a file with no properties and no objects as well.

`ttrm -r` destroyed the objects of nothing: its test of whether a path may be
destroyed (`remover::can_rm`) answered no for every path when the `-r` option was
given, so the objects of the whole tree stayed in the database after the files
were removed. It answers yes for the paths below the one named (a link never),
and `ttrmdir` asks for the objects of an empty directory only, as `rmdir(1)`
removes nothing else. `tt_file_destroy` of a file that the database knows
nothing of is `TT_OK`: it has no objects to destroy.

The file an object is in (`getFile` of the server database) was returned with
the NUL that ends the path in the record, which a file name that is looked up
does not have, so the fallback of the removal of an object to the owner of its
file never found the file, and `tt_spec_destroy` by a user who owned neither the
object nor the file was answered as no such file (`TT_ERR_INTERNAL` for the
caller) instead of as denied. The path is returned as the string it is: a user
who owns neither is told `TT_ERR_ACCESS`, and the owner of the file may remove
the objects in it, as the rule says. Moving an object to the file it is in is
told apart now (`TT_DB_ERR_SAME_OBJECT`) instead of rewriting its record.

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

Moving a file (`TT_MOVE_FILE`) looked for the mount point of the whole name
`host:path`, which is a relative path, and so refused every move; the partition
is now that of the path. A file moved inside its partition is renamed in the
tables, and the names of the files below it follow (the name of the file itself
had a `/` put after it, and those below it two).

## Checks

`make -C lib/tt/lib check` compiles the production sources that read the
environment and start programs next to a test program, with the sanitizers, and
simulates a process with other effective than real ids. It covers the settings
above in a plain and a privileged process, the arguments `ttsession` is started
with (a display name or a command that holds shell characters, a command of
several words), the running of programs and pipelines without a shell, and, when
the superuser starts the test with `TT_ENV_TEST_REAL_UID` and
`TT_ENV_TEST_REAL_GID`, a real set-user-ID process that starts `ttsession` and
must give up its privileges. `make -C lib/tt/bin check` runs the built `ttrm`,
`ttmv`, `ttcp`, `tttar` and `tt_type_comp` on names and options with blanks and
shell characters; `tttar` runs against a process-tree `ttsession` that the test
starts when the portmapper is available.

`make -C lib/tt/bin/ttsession check` starts a process-tree `ttsession` of its own
(when the portmapper is there to register it with), lets one client observe an
operation and another send a notice with `TT_QUEUE`, and checks that the observer
receives it once, as sent, and that a session that is told to end exits and is no
longer registered with the portmapper.

The same check runs the session requests test. One process joins a session of its
own asking for none of the requests of the process, for each of them alone and for
all of them (`session-requests-test.c`, built with the sanitizers when the build
does not carry them, with `ttdt_session_join()` and `tttk_Xt_input_handler()` as a
client uses them), and a second process sends every one of the nine requests, first
to nobody in particular and then addressed to the first. A request that was asked
for must be handled and do what it is for: the answers carry the locale, the value
of the variable, the working directory and the host name, and afterwards the
variable, the directory, the locale and the signal count of the process are the
ones the requests set and the callback has seen the `Do_Command`. Every other
request must fail (`TT_ERR_NO_MATCH` when sent to nobody in particular,
`TT_DESKTOP_ENOTSUP` when addressed) and leave the process as it was. The values
`ttdt_session_join()` refuses are checked too. Built against the library from
before the change (`TT_REQUESTS_OLD_JOIN=1`), the cases that ask for less than all
of the requests fail: the library answers every one of them.

The test also fails on a sanitizer report, from either of the two processes, from
the library they use or from `ttsession` (which is searched when it has ended),
wherever the report comes from: opening, joining, receiving, replying, sending and
closing are the paths of every client, and the check keeps no list of reports to
overlook. Against the library from before the objects the client decodes were
typed (see above), every check that receives a message fails.

`make -C lib/tt/slib check` compiles the production decoding and request code
with the sanitizers next to a test program and runs each case in its own
process. The program builds its requests by hand, with lengths and null markers
that libtt never produces, and serves them through the dispatcher `ttsession`
uses. The cases cover each authentication level, the cookie comparison, the
environment policy and the start environment of a message.

`make -C lib/tt/bin/ttdbserverd check` does the same for the database server: it
runs the real request dispatcher on requests it makes up, and a mutation of
those requests, under the sanitizers, with the leak detector on: what a request
allocates must be freed by the time the case ends. Every case runs on every build, including
the two that need the database of a partition: a build in which NetISAM and the
server disagree on the width of a long cannot make that database, and those
cases fail.

The request cases also decode every limit of the protocol and the size above it,
send a request of each procedure of the new interface with each of its lengths,
counts and small integers replaced by huge values while the allocator refuses
any request for more than 64 MiB, and send every reply that has a string
the server does not have.

They also serve the requests to users with other credentials: for the owner of a
file or object, a member of its group and everybody else, over every combination
of the group and other read and write bits, whether each of reading a property,
writing a property, changing the type, reading and changing the file of an
object, creating an object in a file and moving one into it is allowed, and that
changing an access is the owner's alone; the rule that decides on its own, for
all the permission bits, the kinds of caller and the records whose owner or
group is -1; the partitions of object ids; the credentials of the requests that
skip none; the sessions of a file in the property table the server made; and
that the procedures 1 to 24 of the old servers are answered as procedures that
do not exist and do nothing.

Two more cases call the mount table and path functions of libtt that the server
uses: a path that no mount point leads to, and the directory and base name of
paths with and without a `/`.

`make -C lib/tt/mini_isam check` builds the same test of the width of a long as
C and as C++ and runs both against `libisam`, so the C library and its C++
clients are held to one layout.
