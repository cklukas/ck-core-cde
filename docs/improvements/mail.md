---
title: Mailer
description: How dtmail and dtmailpr handle the mail group of a set-group-id installation and the data they take from messages, mail servers, mailboxes, other users and ToolTalk peers.
---

# Mailer

`dtmail` is the graphical mailer; `dtmailpr` prints a mailbox without a
display. Both are built on `libDtMail`, which reads and writes mailboxes in the
mail spool, parses messages and hands them to the transport. A mailer reads
data that anyone can send, and the program that is installed set-group-id for
the `mail` group can change every user's mailbox, so this page records what the
programs do with the group and with the data they are given.

## Installed group

Where the group exists, `make install` gives `dtmail` the `mail` group and
mode 2755, because the spool directory of many systems is writable only for
that group. The program needs
the group for one reason: to create, lock and rewrite files in the spool.

- `include/DtMail/MailGroup.hh` is the only code that changes a group id. The
  group is given up as the first thing `RoamApp::initialize()` (and `main()` of
  `dtmailpr`) does, before the signal handlers are installed, the locale is
  set, the environment is read or a file is opened. Earlier, that happened
  after `XtSetLanguageProc()` and `_DtEnvControl()` had already run with the
  mail group.
- Each change of the effective group id is verified with `getegid()`. A kernel
  or security module that refuses `setegid()`, or reports success without
  changing anything, makes the program stop with a message instead of going on
  with the mail group in a process that believes it has given it up; the
  results of `setgid()` were ignored before. Taking the group for a spool
  operation can fail without harm: the operation is then done with the user's
  own group, as it is when the installation is not set-group-id.
- The effective group is the mail group only between `enableGroupPrivileges()`
  and `disableGroupPrivileges()` around a single system call on a spool or lock
  file (the `PRIV_ENABLED` macros of `RFCMailBox.C`). The saved group id keeps
  the mail group so that the next such call can take it again; nothing else in
  the program changes a group id, and a test fails when something does.

To check an installation, copy `dtmailpr` somewhere, give the copy the `mail`
group and mode 2755, and run it as an ordinary user under
`sudo strace -f -u <user> -e trace=openat,setresgid`: the first `setresgid`
(the drop of the effective group) comes before the first open of a locale file.
`make -C programs/dtmail check` runs the fake-kernel scenarios and the source
guard for the order.

## Mailbox files and paths

A save writes the new mailbox to a temporary file next to the mailbox and
renames it over the old one. The mail group is held while that file is made,
and the directory is often one that other users can write to.

- The temporary file is made with `mkstemp()` under `<mailbox>.tmp.XXXXXX`. It
  was `<mailbox>.tmp.<time in hex>`, a name that can be worked out, opened with
  `O_CREAT|O_TRUNC` through whatever link was found under that name; the name
  is now random and `mkstemp()` never opens a file or link that exists.
- A mailbox that is a link is opened through `realpath()`, so that a save
  replaces the file the link names and leaves the link alone. The loop that
  followed links used a buffer size that was the size of a pointer and copied
  link targets into buffers of `MAXPATHLEN` with `strcpy()`; a link whose target
  was longer than seven bytes named the wrong file. A mailbox that does not
  exist yet is created under the name it was given.
- `Session::expandPath()` and `getRelativePath()` expand `$NAME` and `${NAME}`
  themselves instead of starting a shell with `echo <path>` through `popen()`.
  Command substitution, quotes, globs and other shell syntax in a mailbox or
  folder name are no longer interpreted. A path that does not fit the 2048
  bytes of the buffers is refused with an error instead of being copied past
  them, and every name made from a mailbox path (lock files, the temporary
  file) fits into `MAXPATHLEN`.
- The callbacks that a `Session` calls to take and give up the mail group, to
  report that it is busy and to ask for the time of the last interaction were
  never initialised, so a program that did not register them called through
  garbage. They start as null.

`make -C programs/dtmail/libDtMail check` compiles `Session.C` and
`RFCMailBox.C` with AddressSanitizer and UndefinedBehaviorSanitizer next to
`tests/mailbox-files.C`, links them with the built `libDtMail.a`, and opens,
changes and saves real mailbox files in an empty directory for each case.

## Text made from a received message

The reply commands, "Save As Text", the print commands and `dtmailpr` put
addresses, the date, the subject and character set names of the message into
headers of a reply, into the text they write and into the name of a print
file. Those strings come from the sender. The buffers that held them were sized
by a guess, and a message, or the server it came from, could write past them.

- `include/DtMail/TextBuffer.hh` holds text that grows as it is appended to.
  The address list of a reply (`ComposeFamily::valueToAddrString()`; every `,`
  of a header becomes `, ` and the buffer was sized for the header), the text
  of all headers that `Save As Text` writes (`DmxMsg::getHeaders()` doubled its
  buffer once) and the header line of `dtmailpr` (a buffer of 1024 bytes) are
  built in it.
- A reply to a message without a `Subject` header builds its subject in the
  same way (it used a buffer of 200 bytes for the date of the message), and the
  date stops at the first line break, so that it stays a single line of the
  subject.
- The character set name of a message (`X-Sun-Charset`) is copied with
  `copyBounded()` into a buffer that starts empty (it was copied with
  `strcpy()` into 64 bytes and read before it was set), and the default
  character set is named with `snprintf()`.
- The name of the print file that is made from the subject is cut to fit next
  to the home directory and the suffix `.ps`; it was written with `sprintf()`
  into a buffer of `MAXPATHLEN`. A program without `$HOME` uses the current
  directory.
- The function that reduces the sender to a name (`phrase()` of `dmx` and
  `dtmailpr`) allocated one byte too few for an address of 5120 bytes or more
  and did not free its buffer on every path.
- Printing or saving as text more than 2047 messages at once, and `dtmailpr` on
  a mailbox of more than 1023 messages, wrote pointers past the array of
  messages. The commands now use the first messages and say how many.
- The text of a mail server error (the answer of a POP3 or IMAP server, the
  command that failed) and the text of the lock dialog are formatted into the
  dialog buffer with `snprintf()`; they were written with `sprintf()` into 2048
  bytes, and a server chooses how long its answer is.
- The sound that is played when new mail arrives (`realsound` in the mailrc) is
  quoted as one argument for the shell by `quotedForShell()`; the command was
  formatted into a buffer of 1500 bytes without quoting.

`make -C programs/dtmail check` runs the production functions under
AddressSanitizer and UndefinedBehaviorSanitizer on headers of every length, and
guards the places that need a display.

## Input of the user who starts the mailer

The command line, the resources, the session file and the files named for
attachments belong to whoever starts the program. Where `dtmail` is installed
set-group-id, a caller who can overflow a buffer from them runs code in a
process whose saved group id is the mail group, so these buffers are bounded
as well.

- The font list that joins the user font and the glyph font (`-xrm
  'Dtmail*userFont: ...'`) is built in a `TextBuffer`; it was a 1024-byte
  buffer filled with `strcpy()` and `strcat()`. The name of the display that
  is exported with `putenv()` is allocated for its length (it was 256 bytes).
- A session file is read word by word with `readSessionWord()`, which keeps what
  fits and skips the rest of a longer word. The session reader used
  `fscanf("%s")` into buffers of 4097 and 256 bytes. The session file is made
  readable and writable for its owner only; it was shared with the group.
- The messages about an attachment file (`-c file`, an attachment added in the
  compose window or by a ToolTalk message) are built in buffers sized for the
  path they name, and the name of the file is found with `strrchr()`; the old
  loop read the byte before the path when it had no slash.
- The names of the widgets of a header line are built from the whole header
  name. They were built in 50 bytes, with the name of a header of a message
  loaded into the compose window written at an offset that depended on its
  length.

`make -C programs/dtmail check` runs `readSessionWord()` and the widget-name
function under the sanitizers on input of every length, and guards the places
that need a display.

## Mail servers

A mailer that fetches mail through a POP2, POP3, APOP or IMAP server reads what
the server answers, and a server, or whoever sits between it and the mailer,
chooses every byte of that. The clients in `libDtMail` (`DtMailServer.C`,
`POP2Server.C`, `POP3Server.C`, `APOPServer.C`, `IMAPServer.C` and the socket
functions in `IO.C`) therefore treat the answers as data to check.

- A command is formatted with `vsnprintf()` into its 512-byte buffer. A command
  that does not fit, or whose arguments contain a carriage return or a line
  feed (the user name, the password and the folder come from the mailrc), is
  not sent: the server would take the text after the line break for a second
  command, such as `DELE`. The end of a line read from a server, and of a
  command written to the protocol log, is removed by one helper that takes off
  a final line feed and a final carriage return from a line of any length,
  empty ones included. The old code read the byte before the buffer for an
  empty line, and the POP2 and POP3 clients wrote a carriage return back where
  they meant to end the line.
- The number of messages must be between 0 and 1,048,576. A reply to `STAT`
  without a number, or with a number the server made up, is a protocol
  violation, so no array is sized by it and no loop runs for it. The numbers
  in a `LIST` or `FETCH ... RFC822.SIZE` answer must name a message of the
  mailbox (the size lists were written at `sizes[number - 1]` for any number
  the server sent, or for an uninitialized one when the line did not parse;
  nothing sets the size limit that asks for these lists today),
  and a `UIDL` identifier is cut to the 128 bytes that hold it (it was read
  with an unbounded `%s`). A message length below zero is no length.
- The transaction tag of the IMAP client wraps at 9999 instead of running over
  its 6-byte buffer, a tagged answer without a space no longer sends the
  search for the end of the tag past the end of the line, and the status line
  of a POP3 server that has no text after `+OK` is no longer read past its end.
- Reading a message restores the `SIGINT` setting on every way out; it was left
  ignored after a failed transfer. The line that starts a stored message is
  built with `snprintf()` from the name of the server.
- The APOP client finishes the MD5 digest of the challenge and the password
  (the digest it sent was an array nothing had written), and it wipes the
  buffer that held the password.
- The name of a mailrc property (`<folder>_<key>`) is built in a bounded buffer
  and is empty, so that no property is found, when the folder name is too long.
  The default folder name is a copy that the destructor can release; it was a
  string literal passed to `free()`. A property that exists without a value no
  longer stops the program.
- A host name is resolved on 64-bit systems: `inet_addr()` failure was compared
  with `-1` as an `unsigned long`, so every name was taken for the address
  255.255.255.255. The text of a socket error is bounded, `SockPrintf()` sizes
  its buffer from the text, and the build no longer carries the second variant
  of the socket functions that cast descriptors to pointers.
- `SafeCtime()` ends its text inside the buffer it is given (the copy was not
  terminated when the text was longer than the buffer), `SafeLocaltime()`
  copes with a time that cannot be converted, and `GetGroupName()` is given the
  size of the caller's buffer (the caller has 100 bytes and a group name can
  be longer).

`make -C programs/dtmail/libDtMail check` starts a server of its own on the
loopback address (a forked process that answers what the case scripts) and runs
the production POP3 and IMAP clients against it under the sanitizers: ordinary
retrievals with their commands, counts and sizes a server made up, identifiers
and lines of every length, and passwords with line breaks.
`make -C programs/dtmail check` runs the line helpers, the command formatter,
the tag, the identifier format and `SafeCtime()` on input of every length.

## Requests from other applications

Another application in the user's ToolTalk session can ask `dtmail` to send a
message (`Mail`), to open a mailbox (`Display`) or to print one (`Print`), with
the data in a buffer or in a file it names.

- A `Mail` request without a buffer or a file no longer passes a message
  buffer that was never set to the library. The file a `Mail` request names is
  read only when it is a regular file of at most 64 MiB: an empty file, a
  directory, a device or a file that is too large is refused, and a FIFO is
  not waited for (opening it blocked the mailer until someone wrote to it).
  The file is closed after it is read (every request left its descriptor open)
  and its buffer is released with `delete[]`.
- A `Display` request with neither buffer nor file is refused. It passed a null
  mailbox name on, which stopped the program.
- A `Print` request that carried its data in a buffer asked about the mailbox
  by the name in `file`, which is null in that case. It uses the file that it
  made from the buffer.
- The file that is made from a buffer is created with `mkstemp()` in
  `~/.dt/tmp` with mode 0600 (it was `tempnam()` and `open(O_CREAT)` with mode
  0644, and fell back to the shared temporary directory when `$HOME` was
  unset). A request that cannot be given a private file is refused.

`make -C programs/dtmail check` runs the production functions that make the
data of a request against real files, FIFOs, devices and sparse files under the
sanitizers, and guards the handlers.
