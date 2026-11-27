MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtcalc

EXTRA_DIST = tests/check-rcfile-tempfile.py tests/check-rcfile-tempfile.c \
	tests/check-mp-errorlog.py tests/check-mp-errorlog.c

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-rcfile-tempfile.py
	CC="$(CC)" python3 $(srcdir)/tests/check-mp-errorlog.py

dtcalc_CPPFLAGS = -DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\"

dtcalc_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB)

if SOLARIS
dtcalc_LDADD += -lintl
endif


dtcalc_SOURCES = calctool.c ds_common.c ds_popup.c ds_widget.c		\
		 ds_xlib.c functions.c help.c motif.c mp.c text.c text.h	\
		 calctool.h ds_popup.h ds_xlib.h patchlevel.h		\
		 ds_common.h ds_widget.h motif.h

AM_CFLAGS = $(CK_HARDENING_PHASE2_FLAGS)
