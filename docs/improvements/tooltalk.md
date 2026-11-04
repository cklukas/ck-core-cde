---
title: ToolTalk
description: How ttsession decodes requests, authenticates clients and prepares the environment of the programs it starts.
---

# ToolTalk

ToolTalk is the message bus of the desktop. `ttsession` runs once for a session.
Programs connect to it over ONC RPC, register the messages they handle and send
messages that it routes; it starts the program of an installed ptype when a
message needs a handler that is not running. Everything a client sends reaches
`ttsession` as RPC arguments, and the daemon decodes them before it knows who
sent them.

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

## Checks

`make -C lib/tt/slib check` compiles the production decoding and request code
with the sanitizers next to a test program and runs each case in its own
process. The program builds its requests by hand, with lengths and null markers
that libtt never produces, and serves them through the dispatcher `ttsession`
uses. The cases cover each authentication level, the cookie comparison, the
environment policy and the start environment of a message.
