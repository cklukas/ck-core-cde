---
title: DtSearch
description: How DtSearch databases store their integers, what a database made by an earlier build needs, and the checks that index and query a database.
---

# DtSearch

DtSearch is the full-text search engine of the desktop. `dtsrcreate`,
`dtsrload` and `dtsrindex` build a database from documents, `dtsrclean` and
`dtsrdelete` maintain it, and programs such as DtInfo query it through the
DtSrAPI (`Dt/Search.h`). A database is a set of files (`.d00`, `.d2x`, `.k0x`,
`.d99` and the data dictionary `.dbd`) written by the tools and read by the
engine.

## The widths of a database

The integer types of a database file have one width on every machine, in C and
in C++, and whatever feature macros the build sets:

| Type | Width | Where |
|---|---|---|
| `INT` | 2 bytes | counts and record numbers of the engine's pages |
| `DtSrINT32`, `DtSrUINT32` | 4 bytes | the fields of the DtSearch records |
| `LONG`, `ULONG`, `DB_ADDR`, `F_ADDR` | 8 bytes | database addresses, page and record pointers, timestamps, the entries of the `.d99` file |

`Dt/vista.h` and `Dt/Search.h` define these types with fixed-width integers and
assert their sizes when they are compiled; they no longer take them from
`LONG_BIT`, which glibc hides from C code built with the feature macros of the
build while the C++ compilers show it. Every address and timestamp in a file is
big-endian, so the files of a little-endian and a big-endian machine are the
same.

The `.d99` file starts with a header of 4104 bytes (the free space of the file)
and then holds the address list of each word: for every document that has the
word, 8 bytes with the record number of the document and a weight in the lowest
byte, followed by the free holes of the list, each 8 bytes of ones.

## Databases of an earlier build

A database made before this layout was fixed stored its addresses in 4 bytes.
Its data dictionary carries the version `V3.dt`, and the engine refuses it
("incompatible dictionary file") instead of reading it with the wrong stride.
Make the database again from its source documents with `dtsrcreate`, `dtsrload`
and `dtsrindex`; there is no converter.

## Maintenance tools

Every tool prints its usage and ends when it is run without arguments.
`dtsrclean` and `dtsrdelete` used to die of a signal at that point, and when
they printed any message, because they wrote to a stream that only the engine's
start-up sets. `dtsrdelete` keeps the entries of the documents it leaves in an
address list as they are in the file; it used to write them back in host
order, which left a little-endian machine with lists no query could read.

## Damaged indexes

A statistical query reads the address list of each word from the `.d99` file
into a buffer with room for the objects of the database and adds each entry to
a node of a table that has one node per object. A list that is longer than the
database has objects, a list that is cut off, or an entry that names an object
the database does not have used to be read and written outside those arrays.
The query now ends with the error `991 The index of the database is damaged.`
and the engine answers the next query.

`dtsrdelete` removes the addresses of the deleted documents from the list of
each word and now marks the positions that come free at the end of the list
with ones, like every other free position of the file; it left what it had
removed in them. The tools no longer define globals of the library's names:
`dtsrhan` had a `shutdown_now` of its own next to the library's, and `dtsrdbrec`
a second `print_dbrec`, so that a program and the library used two of them.

## Checks

`make -C lib/DtSearch check` builds a program as C and as C++ that asserts the
widths above, the constants that lay out a record slot and a page, and the
byte order of an address. `make -C programs/dtsr check` makes a database of 420
documents from a made-up vocabulary, in which a few words are in more than half
of the documents (their address lists run over several blocks of the `.d99`
file) and the others in few. It loads and indexes the documents in two batches,
reads the `.d99` file directly to see that every list holds the right record
numbers in 8-byte entries, and queries the database through the DtSrAPI: single
words, several words, `&`, `|` and `~`, nested expressions, in the exact and
the stemmed search and in the statistical search. The same queries run again
after `dtsrclean -v` and after `dtsrdelete` has removed a seventh of the
documents. The query program is built with the sanitizers when the compiler has
them.

The same test checks that no tool defines a global that `libDtSearch` defines
(`nm -D` of each tool and of the library), that the free positions of the `.d99`
file are all ones after `dtsrdelete`, and that a statistical query on a copy of
the database in which one address was changed to an object that does not exist
ends with the error above and is followed by a correct query.
