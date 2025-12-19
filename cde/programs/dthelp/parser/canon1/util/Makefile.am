MAINTAINERCLEANFILES = Makefile.in

noinst_LIBRARIES = libutil.a

AM_CPPFLAGS = -DCANON1 -I../../include/util

noinst_PROGRAMS = context fclndir

context_SOURCES = context.c

fclndir_SOURCES = context.c

libutil_a_SOURCES = allwhite.c inctest.c letter.c triedump.c		\
	triepdmp.c trieplk.c trierset.c upcmp8.c upstrcmp.c wchar.c	\
	conutil.c error.c exit.c itoa.c malloc.c openchk.c	\
	trie.c triecnt.c whitesp.c

context_LDADD = libutil.a
fclndir_LDADD = libutil.a
