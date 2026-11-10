---
title: Mailer
description: How dtmail and dtmailpr handle the mail group of a set-group-id installation and the data they take from messages, mailboxes, other users and ToolTalk peers.
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
by a guess, and a message could write past them.

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
