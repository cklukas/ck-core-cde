MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = util TermPrim Term TermView

lib_LTLIBRARIES = libDtTerm.la

EXTRA_DIST = tests/check-terminal-parser.py tests/check-suid-root.py \
	     tests/check-realloc-failures.py tests/check-realloc-failures.c \
	     tests/realloc-inject.h tests/check-utf8-font-fallback.py

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-terminal-parser.py
	CC="$(CC)" python3 $(srcdir)/tests/check-suid-root.py
	CC="$(CC)" python3 $(srcdir)/tests/check-realloc-failures.py
	CC="$(CC)" python3 $(srcdir)/tests/check-utf8-font-fallback.py

libDtTerm_la_SOURCES =

libDtTerm_la_LIBADD = Term/libTerm.la TermPrim/libTermPrim.la \
		       TermView/libTermView.la

if OPENBSD
libDtTerm_la_LIBADD += -lutil
endif

if FREEBSD
libDtTerm_la_LIBADD += -lutil
endif

if SOLARIS
libDtTerm_la_LIBADD += -lgen
endif

if HAS_UTEMPTER_LIBRARY
libDtTerm_la_LIBADD += -lutempter
endif

# in order to try to keep lib versions the same across platforms, (2.1.0)
if BSD
libDtTerm_la_LDFLAGS = -version-info 2:1:0
else
libDtTerm_la_LDFLAGS = -version-info 3:0:1
endif


AM_CFLAGS = $(CK_HARDENING_PHASE2_FLAGS)
