MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtpdm

EXTRA_DIST = tests/check-notify-string.py tests/check-notify-string.cc

check-local:
	CXX="$(CXX)" CC="$(CC)" python3 $(srcdir)/tests/check-notify-string.py

dtpdm_CFLAGS = -DI18N_MSG -DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\"

dtpdm_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB)

if SOLARIS
dtpdm_LDADD += -lintl -lgen -lresolv
endif

dtpdm_SOURCES = JobBox.c Main.c MainWindow.c PdmMsgs.c PdmOid.c		\
		PdmXp.c PrinterBox.c JobBox.h PdmMsgs.h PdmOid.h	\
		PdmXp.h MainWindow.h PdmOidDefs.h PdmOidStrs.h		\
		PrinterBox.h
