MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtaction

dtaction_SOURCES = Main.c

dtaction_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB)

# The change of group and user ID is tested against a fake kernel.
EXTRA_DIST = tests/check-setgid-uid.py tests/check-setgid-uid.c

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-setgid-uid.py
