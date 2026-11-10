---
title: Data types and database files
description: How libDtSvc matches files to data types, expands database values and reads the database files.
---

# Data types and database files

`libDtSvc` decides what a file is by matching it against the data-type database
(the `.dt` files), expands the keywords and commands of the values it finds
there, and reads the action and data-type files that make up the database. File
Manager and every other client of `DtDtsFileToDataType()` and
`DtDtsDataTypeToAttributeValue()` run this code on file names and file contents
they do not control.

## Matching files

- The contents of a file are mapped, or read when mapping fails, and their length
  is the number of bytes that could be read: a file that is shorter than when it
  was looked at, or a read that fails, no longer leaves memory that was not
  written to be compared. A file or buffer that the length cannot describe has
  no contents to match.
- The offsets and lengths of a content test are checked against that length,
  counted from the start or, with a leading minus sign, from the end. The end of
  a buffer given by the caller counts as well. A buffer given with a size of
  zero or less has nothing to match.
- A link is followed to what it leads to. The target of a link is whole (its
  last character is no longer dropped), a relative target starts in the directory
  of the link, and a chain of more than 40 links, or one that leads back to
  itself, is an error instead of a loop that never ends.
- A character in a pattern or in a name that is not valid in the current locale
  is the byte it is when names are compared, so that the match never depends on
  memory that was not set.

## Values with keywords and commands

A value of the database can contain the keywords `%file%`, `%dir%`, `%name%`,
`%base%` and `%suffix%`, and commands in backquotes whose output replaces them.

- `%suffix%` and `%base%` are those of the name, not of the whole path: the
  suffix of `/data.d/README` is empty and its base is `README`. `%dir%` of a file
  in the root directory is `/`.
- A command is read from the text of the database, not from the text that
  replaced a keyword. The keywords in a command are quoted for the shell, each
  as one word, and the file name that replaces a keyword in the rest of the value
  is never searched for commands. A file named ``a`touch x`.txt`` is a file with a
  strange name.
- A backquote without a partner stays in the text, and the text before it is
  kept. A command prints at most 64 KiB into a value.

## The cache of the database

The cache file of the database is named after the host and the number of the
display (not the screen), and nothing of the display name but letters, digits and
the characters of host and display names becomes part of the name; a display name
without a colon no longer ends the client. A cache that is mapped must have a
header whose offsets and counts lie inside the file; another file is not used.

## Reading database files

The database files are read line by line, with these rules:

- A line ends with a newline, and only a newline is removed; the last line of a
  file need not have one. A line longer than the buffer is cut at the buffer and
  the rest of it is dropped.
- A message about a bad definition is as long as the names in it. The name of
  the definition and the path of the file used to be copied into a buffer of one
  path length.
- The variables set with `set NAME=value` are kept in a table that grows with
  their number, and a variable that refers to itself, directly or through others,
  is replaced a limited number of times instead of for ever. A line is searched
  for variables after a character of more than one byte, too, and an environment
  variable whose value contains an equals sign is found.

## Tests

`lib/DtSvc/tests` cuts the production functions out of `Dts.c`, `DtsMM.c` and
`DbReader.c` and compiles them under AddressSanitizer and UndefinedBehaviorSanitizer
with the file system, the allocator, `read` and `mmap` under the control of the
test:

```sh
make -C lib/DtSvc check
```

Run against the code from before these changes, 7 of the 14 scenarios for `Dts.c`
fail (the other seven check results that did not change), the cache name and
header checks fail, and all 6 scenarios of the database reader fail.
