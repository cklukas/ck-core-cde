MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtexec

EXTRA_DIST = tests/check-process-discovery.py \
	tests/check-panic-signal.py tests/check-panic-signal.c

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-process-discovery.py
	CC="$(CC)" python3 $(srcdir)/tests/check-panic-signal.py

dtexec_CFLAGS = -DMSGLOG_CLIENT_ONLY	\
	-I../../lib/DtSvc/include

dtexec_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB)

dtexec_SOURCES = Main.c

dtexec_CFLAGS += $(CK_HARDENING_PHASE2_FLAGS)
