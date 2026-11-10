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
