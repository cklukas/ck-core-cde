---
title: Mailer
description: How dtmail and dtmailpr run without the mail group, how the small set-group-id helper dtmailspool serves the user's own mailbox in the mail spool, and what the mailer does with the data it takes from messages, mail servers, mailboxes, other users and ToolTalk peers.
---

# Mailer

`dtmail` is the graphical mailer; `dtmailpr` prints a mailbox without a
display. Both are built on `libDtMail`, which reads and writes mailboxes in the
mail spool, parses messages and hands them to the transport. A mailer reads
data that anyone can send, and a program that holds the `mail` group can change
every user's mailbox, so the mailers run with no group of their own and the one
small program that holds the group does nothing but serve the user's own
mailbox. This page records what the programs do with the group and with the
data they are given.

## The mail spool and its helper

The spool directory of many systems is writable only for the `mail` group, so
making the mailbox of a user, its lock files and the file that replaces it in a
save needs that group. `dtmail` used to be installed set-group-id for it. It
gave the group up when it started and took it back around single system calls
with `setegid()`, which works only while the saved group id keeps the mail
group, so a defect anywhere in the graphical program, the ToolTalk handlers or
the parsers of mail could have taken the group back and written to every user's
mailbox.

Now `dtmail` and `dtmailpr` are installed without any special mode and never
hold the group. `dtmailspool`, a program of three plain C files that uses
nothing but the C library, does the work that needs the group, for the user who
started it and for nobody else. `make install` gives it the `mail` group and
mode 2755 (the install rule does not fail where the group does not exist; the
helper then works with the group of the user, which is right for a spool
directory that every user can write to).

- **Started by the mailer, with nothing of the mailer's.** The first spool
  operation starts the helper with `posix_spawn()` and a connected socket as
  descriptor 3, an empty environment and the default signal handling. The
  helper reads no environment variable, option or argument, closes every other
  descriptor, makes sure that descriptors 0 to 2 are open, does not write
  core files and cannot be examined or traced by its user, and refuses to run
  when it is set-user-id or descriptor 3 is not a socket. It ends when the
  mailer closes the connection or ends, or on a `SIGTERM` between two
  requests, and removes the replacement file of a save that was not finished.
  The signals of a terminal (interrupt, quit, hangup) reach the whole process
  group of the mailer, which has locks to release through the helper when it
  ends on one of them, so the helper ignores them; it also ignores `SIGXFSZ`
  and `SIGXCPU` and lifts the limits on file size and time the mailer set for
  itself, so that it finishes what it has begun.
- **Only the user's own mailbox, and no path.** The spool directory is a
  constant of the build (`--with-mail-spool-dir`, default `/var/mail`, or
  `/var/spool/mail` where only that exists). The helper makes every name from
  the login name of its real user: `<name>`, `<name>.lock`, `<name>.dtmail`, a
  short-lived `<name>.lk.XXXXXX` for a lock and `<name>.tmp.XXXXXX` for the
  replacement of a save, all with random suffixes and created exclusively.
  A request names none of them. Every file is reached relative to the spool
  directory with `O_NOFOLLOW`, and a mailbox must be a regular file owned by
  the user: a link, a directory, a FIFO or a file of another user is refused,
  and a link under the name of the mailbox is not made through. A user called
  `<name>.lock` or `<name>.dtmail` owns the file that would be the lock file
  of `<name>`: the helper looks the two names up when it starts and serves no
  lock of that kind when a user of that name exists or the lookup fails.
- **The group is held for one operation.** The helper reads and checks a
  request with the group of the user as its effective group, takes the mail
  group with `setegid()` for the one operation, and gives it up again before it
  answers. Every change is verified with `getegid()`; a helper that cannot
  change the group or finds it changed stops. Nothing else in the mailer
  changes a user or group id, and a test fails when something does. The saved
  group id of the helper keeps the mail group, so the helper is the only
  program that could regain it.
- **A protocol that is checked at every byte.** A request is a frame of a magic
  number, an operation, a length of at most 256 bytes and the payload. Frames
  are read exactly, so a descriptor arrives with the first byte of the frame it
  belongs to; a frame that stops anywhere, is longer than the limit, has
  another magic number, a wrong length for its operation, an unknown operation
  or a descriptor ends the helper (a whole frame that is wrong is answered
  `BAD_REQUEST` first). The client checks replies the same way: any reply that
  is cut, oversize, of the wrong shape, or carries descriptors where none belong
  fails the call, closes what arrived and drops that helper; the next call
  starts another.
- **The mailer keeps its descriptors.** The helper opens the mailbox, or makes
  it with mode 0660 and the mail group, and hands the descriptor over; the
  mailer maps it, locks it with `lockf()` and writes through it as before, and
  none of the `mmap()` and parser code changed. The helper makes the file that
  replaces the mailbox in a save with the mode and group of the old mailbox,
  hands over that descriptor, and renames it over the mailbox when asked, if
  the name still holds a regular file of the user. The dot-locks (`.lock`, the
  text of the lock file is compared whole when it is checked or released) and
  the `.dtmail` lock are made, verified, released and broken by the helper; the
  age of a held lock is told by the file system's own clock, so the rule that
  breaks a lock older than five minutes still works on a remote spool.
- **A mailbox anywhere else needs no helper.** `MailboxFiles` is the one place
  that decides: the mailbox of the user in the spool directory, recognised by
  the directory's identity and the user's name, goes through the helper; any
  other mailbox is opened, locked and replaced with plain system calls and no
  second try with a group. The mailbox descriptor of both is close-on-exec.

A helper that is not installed, does not start or answers against the protocol
makes the call fail (the inbox cannot be opened: "could not be accessed"); the
mailer does not try the spool directory itself.

The spool directory has to be searchable by every user, as the usual modes 2775
and 1777 are: the mailer looks at the path of the mailbox (to resolve links and
to find out where it is) before it asks the helper, and the helper is not asked
to tell it anything but the descriptor.

What was removed with the group: `include/DtMail/MailGroup.hh`, the
`PRIV_ENABLED` macros and their retry without the group, the enable and disable
callbacks of `Session`, `Application` and `RoamApp`, `isSetMailGidNeeded()`,
`GetGroupName()` and `GetIdForGroupName()`, and the check at the start of
`dtmail` that stopped it when it was not running as the group `mail` (together
with the `__ignore_group_permissions` setting that switched the check off).
The messages that went with the check went too, in every catalog and help
volume: `DTME_BadRunGroup` and its text, the first message of the mailer
"Mailer has not been properly installed, and cannot run because the execution
group is incorrectly set", and the help sections *Mailer has not been properly
installed* and *Exit Mail?* ([Message catalogs](#message-catalogs)).

To check an installation, `ls -l $(bindir)/dtmailspool` shows owner `root`,
group `mail` and `-rwxr-sr-x`, and `dtmail` and `dtmailpr` have no `s` in their
mode. While `dtmail` has opened its inbox, `grep Gid /proc/<pid>/status` shows
the group of the user four times for `dtmail`, and the group of the user, the
group of the user, `mail` and the group of the user (real, effective, saved,
file system) for `dtmailspool`.

`make -C programs/dtmail check` compiles the production sources of the helper
with the sanitizers and drives them over a socket the way the mailer does:
every truncation of every request, lengths and magic numbers that are wrong,
requests with descriptors, frames sent a byte at a time or together; links,
directories, FIFOs and (with `DTMAIL_TEST_SUDO=1`, through `sudo -n`) files of
other users in the place of the mailbox; the locks (held, aged, broken,
foreign), the replacement of a save, a killed helper; a helper started with
descriptors, signals and standard streams of the mailer's; and a fake kernel
for the group ids that refuses or ignores a change and records every change.
Source checks keep the group calls in `main.c` of the helper, the helper free of
environment, arguments and path-based calls, and the group install rule with the
helper alone. `make -C programs/dtmail/libDtMail check` runs the client against
helpers that answer wrongly in every way listed above, the same scenarios of
mailbox, locks and replacement against both ways of handling files, and the
production `RFCMailBox` on the user's inbox in a spool directory of the test.

## Mailbox files and paths

A save writes the new mailbox to a temporary file next to the mailbox and
renames it over the old one. The directory is often one that other users can
write to. (For the user's own mailbox in the mail spool the helper makes the
file and does the rename; see above.)

- The temporary file is made with `mkstemp()` under `<mailbox>.tmp.XXXXXX`. It
  was `<mailbox>.tmp.<time in hex>`, a name that can be worked out, opened with
  `O_CREAT|O_TRUNC` through whatever link was found under that name; the name
  is now random and `mkstemp()` never opens a file or link that exists. (The
  helper makes its file the same way: random name, `O_EXCL`, `O_NOFOLLOW`.)
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
- The callbacks that a `Session` calls to report that it is busy and to ask for
  the time of the last interaction were never initialised, so a program that
  did not register them called through garbage. They start as null.

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
attachments belong to whoever starts the program. A caller who can overflow a
buffer from them runs code as the user, in the process that holds the user's
mail and mail server passwords, so these buffers are bounded.

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
  copes with a time that cannot be converted.

`make -C programs/dtmail/libDtMail check` starts a server of its own on the
loopback address (a forked process that answers what the case scripts) and runs
the production POP3 and IMAP clients against it under the sanitizers: ordinary
retrievals with their commands, counts and sizes a server made up, identifiers
and lines of every length, and passwords with line breaks.
`make -C programs/dtmail check` runs the line helpers, the command formatter,
the tag, the identifier format and `SafeCtime()` on input of every length.

## Message data and lock files

The structure of a message (the parameters of its `Content-Type`, the enriched
text of its body, the addresses in its headers) and the environment (the
character set of the locale, the length of a path) decide how much text the
library builds, so the places that built it in a buffer of a guessed size now
size it from the data.

- A message of the type `message/partial` is put together from its parts when
  the mailbox is read. Reading a mailbox that held two parts of one message
  stopped the mailer, because the headers of the carried message were read
  through an envelope that had no parent message and so no session. The parts are now
  matched by the parameters `id`, `number` and `total`, which are read as
  parameters (a name inside the quoted `id` is not a parameter) and as plain
  numbers (`atoi()` returned anything for text that was no number). A part
  needs a number between 1 and 1,000 that is not above its own total; a message
  has at most 1,000 parts and 256 MiB. The old code wrote the part at
  `parts[number - 1]` for any number, sized the text of the whole message from the
  `Content-Length` headers of the parts, and appended headers and data to it with
  `strcat()`, which stops at a zero byte and does not look at the size. The new
  text grows as needed, keeps every byte, and a first part without a blank line
  after the headers of the carried message is left alone. The list of parts is
  released with the mailbox.
- The converter for `text/enriched` and `text/richtext` keeps the name of a
  command in a 256-byte buffer: a longer one is cut. The counter that guarded
  the buffer was a `char` that wrapped after 127 characters, so a command name of
  a few hundred characters wrote past the buffer on the stack. The converter no
  longer reads the byte after a text that ends with `<`, and the buffer for the
  plain text is sized from the length of the text and checked.
- The MIME type from the data type database and the character set of the locale
  are copied into their 64-byte buffers with `snprintf()`; `strcpy()` was used.
- The `From ` line that is made up for a message without one is built from the
  first word of the sender, with the characters that would end the line left
  out, in a buffer sized for it (it was 100 bytes). A list of addresses is
  copied into a buffer with room for the closing quote of an unterminated
  quoted string, which the old size missed; an empty list gets its terminating
  byte; an empty address no longer forms a pointer before its string.
  The log file that receives a copy of a sent message gets a complete first
  line (its time was cut to 8 bytes and left without a terminator).
- The temporary file of a lock is named with six random characters read from
  `/dev/urandom` instead of `mktemp()` (`MailboxFiles`; the helper takes the
  names of its own files from the same source), and the names of lock files that
  do not fit `MAXPATHLEN` are refused. A lock file, `.lock` or `.dtmail`, is
  released only when it holds exactly the text this process wrote: at most 129
  bytes are read into a buffer and compared whole, so a file with no number,
  no line end or more bytes than any buffer is not ours and stays (the `.dtmail`
  lock was scanned unterminated, with the number uninitialized when it was not
  one). Strings that `strdup()` made are released with `free()`, and an array
  with `delete[]`.

`make -C programs/dtmail/libDtMail check` opens mailboxes with parts of one
message in every shape described above and a `text/enriched` message with a very
long command, and `make -C programs/dtmail check` runs the parameter reader, the
growing text, the enriched converter, the `From ` line, the lock name and the
address reader on input of every length. Both
fail on the sources before this change.

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

## Message catalogs

Two catalogs of the mailer are indexed by number. `libDtMail` looks the text of
an error up as message "enumerator + 1" of set 1 of `libDtMail.msg`, with the
English text in `Common/DtMailError.C` as the fall-back when no catalog is
installed, so the order of `DTMailError_t`, of those texts and of the messages
of the catalog is one order. `dtmail` names the messages of `DtMail.msg` by a
literal set and number in its `CATGETS` calls. Taking a message out of either
catalog gives every later message a new number, and it has to do so in the
English source, in the source of every locale and in every call that names one;
a gap, or one locale left behind, shows the text of a neighbouring message
under an error or a dialog, or a conversion of another type (`%s` where an
integer is passed) than the one the code passes.

`make -C programs/dtmail/libDtMail check` runs `tests/check-message-catalogs.py`
over these catalogs:

- the enumerators, the fall-back texts and the English catalog name the same
  errors in the same order, and the comment above each message of the catalog
  names the error that has its number;
- every locale numbers its messages like the English source: unique numbers
  without gaps, no more than the English source has, the same error named above
  a message and the same conversions in it; the locales that the build installs
  (`programs/localized/Makefile.am`) have exactly the English set of messages in
  every set;
- every `CATGETS` call on set 2 of `DtMail.msg` names the message whose text is
  its default;
- no removed message is left: the enumerator, the help sections or their
  identifiers, or the text of the message in a source file;
- gencat accepts every source, and the compiled catalog read back with
  `catgets()` holds the messages of the source under the numbers of the source;
- the production `DtMailEnv` (`Common/DtMailError.C`, compiled with the
  sanitizers) returns, for every error, the text of its message from the English
  catalog and from each installed locale, the fall-back text without a catalog,
  and "invalid error" for a value beyond the table.

The test fails on the sources before the removal of the message about the group
and on copies of the catalogs, the code and the help volumes with one defect each
(a locale or message left under its old number, two messages swapped, a message
missing or doubled, a changed conversion, a call that was not renumbered, an
enumerator or a table entry out of place, a help section that came back, a source
that gencat refuses).

`libDtMail.msg` of ko_KR, sv_SE and zh_TW is in the layout of an older
enumeration, has no comments that name the errors and is not part of the build
(only de_DE, es_ES, fr_FR, it_IT and ja_JP are installed); the test can check
its numbers and conversions but not what its messages say.
