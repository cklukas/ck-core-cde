MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtexec

EXTRA_DIST = tests/check-process-discovery.py

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-process-discovery.py

dtexec_CFLAGS = -DMSGLOG_CLIENT_ONLY	\
	-I../../lib/DtSvc/include

dtexec_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB)

dtexec_SOURCES = Main.c
