MAINTAINERCLEANFILES = Makefile.in

EXTRA_DIST = tests/check-auth-file.py tests/check-auth-file.c

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-auth-file.py

bin_PROGRAMS = dtpdmd

dtpdmd_CFLAGS = -DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\"

dtpdmd_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB)

dtpdmd_SOURCES = dispatch.c dtpdmd.c mailbox.c manager.c records.c	\
		 nlmsg.c setup.c util.c dtpdmdP.h nlmsg.h
